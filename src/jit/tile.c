#include "moar.h"
#include "dasm_proto.h"
#include <math.h>
#if MVM_JIT_ARCH == MVM_JIT_ARCH_X64
#include "x64/tile_decl.h"
#include "x64/tile_tables.h"
#endif

struct TileState {
    /* state is junction of applicable tiles */
    MVMint32 state;
    /* rule is number of best applicable tile */
    MVMint32 rule;
    /* template is template of assigned tile */
    const MVMJitTileTemplate *template;
};

struct TileTree {
    MVM_DYNAR_DECL(struct TileState, states);
    MVMSpeshGraph *sg;
    MVMJitTileList *list;
    MVMint32 order_nr;
};

/* Postorder collection of tile states (rulesets) */
static void tile_node(MVMThreadContext *tc, MVMJitTreeTraverser *traverser,
                      MVMJitExprTree *tree, MVMint32 node) {
    struct TileTree *tiles       = traverser->data;
    MVMJitExprNode op            = tree->nodes[node];
    const MVMJitExprOpInfo *info = tree->info[node].op_info;
    MVMint32 first_child = node+1;
    MVMint32 nchild      = info->nchild < 0 ? tree->nodes[first_child++] : info->nchild;
    MVMint32 *state_info = NULL;

    switch (op) {
    case MVM_JIT_ALL:
    case MVM_JIT_ANY:
    case MVM_JIT_ARGLIST:
        {
            /* Unary variadic nodes are exactly the same... */
            MVMint32 i;
            for (i = 0; i < nchild; i++) {
                MVMint32 child = tree->nodes[first_child+i];
                state_info = MVM_jit_tile_state_lookup(tc, op, tiles->states[child].state, -1);
                if (state_info == NULL) {
                    MVM_oops(tc, "OOPS, %s can't be tiled with a %s child at position %d",
                             info->name, tree->info[child].op_info->name, i);
                }
            }
            tiles->states[node].state    = state_info[3];
            tiles->states[node].rule     = state_info[4];
        }
        break;
    case MVM_JIT_DO:
        {
            MVMint32 last_child = first_child+nchild-1;
            MVMint32 left_state = tiles->states[tree->nodes[first_child]].state;
            MVMint32 right_state = tiles->states[tree->nodes[last_child]].state;
            state_info = MVM_jit_tile_state_lookup(tc, op, left_state, right_state);
            if (state_info == NULL) {
                MVM_oops(tc, "Can't tile this DO node");
            }
            tiles->states[node].state = state_info[3];
            tiles->states[node].rule  = state_info[4];
        }
        break;
    case MVM_JIT_IF:
    case MVM_JIT_EITHER:
        {
            MVMint32 cond = tree->nodes[node+1],
                left = tree->nodes[node+2],
                right = tree->nodes[node+3];
            MVMint32 *left_state  = MVM_jit_tile_state_lookup(tc, op, tiles->states[cond].state,
                                                              tiles->states[left].state);
            MVMint32 *right_state = MVM_jit_tile_state_lookup(tc, op, tiles->states[cond].state,
                                                              tiles->states[right].state);
            if (left_state == NULL || right_state == NULL ||
                left_state[3] != right_state[3] ||
                left_state[4] != right_state[4]) {
                MVM_oops(tc, "Inconsistent %s tile state", info->name);
            }
            tiles->states[node].state = left_state[3];
            tiles->states[node].rule  = left_state[4];
        }
        break;
    default:
        {
            if (nchild == 0) {
                state_info = MVM_jit_tile_state_lookup(tc, op, -1, -1);
            } else if (nchild == 1) {
                MVMint32 left = tree->nodes[first_child];
                MVMint32 lstate = tiles->states[left].state;
                state_info = MVM_jit_tile_state_lookup(tc, op, lstate, -1);
            } else if (nchild == 2) {
                MVMint32 left  = tree->nodes[first_child];
                MVMint32 lstate = tiles->states[left].state;
                MVMint32 right = tree->nodes[first_child+1];
                MVMint32 rstate = tiles->states[right].state;
                state_info = MVM_jit_tile_state_lookup(tc, op, lstate, rstate);
            } else {
                MVM_oops(tc, "Can't deal with %d children of node %s\n", nchild, info->name);
            }
            if (state_info == NULL)
                MVM_oops(tc, "Tiler table could not find next state for %s\n",
                         info->name);
            tiles->states[node].state = state_info[3];
            tiles->states[node].rule  = state_info[4];
        }
    }
}


/* It may happen that a nodes which is used multiple times is tiled in
 * differrent ways, because it is the parent tile which determines which
 * 'symbol' the child node gets to implement, and hence different parents might
 * decide differently. That may mean the same value will be computed more than
 * once, which could be suboptimal. Still, it is necessary to resolve such
 * conflicts. We do so by generating a new node for the parent node to refer to,
 * leaving the old node as it was. That may cause the tree to grow, which is
 * implemented by realloc. As a result, it is unsafe to take references to tree
 * elements while it is being modified. */

static MVMint32 assign_tile(MVMThreadContext *tc, MVMJitExprTree *tree,
                            MVMJitTreeTraverser *traverser,
                            MVMJitExprNode node, MVMint32 rule_nr) {
    const MVMJitTileTemplate *template = &MVM_jit_tile_rules[rule_nr];
    struct TileTree *tiles = traverser->data;

    if (rule_nr > (sizeof(MVM_jit_tile_rules)/sizeof(MVM_jit_tile_rules[0])))
        MVM_oops(tc, "Attempt to assign invalid tile rule %d\n", rule_nr);

    if (tiles->states[node].template == NULL || tiles->states[node].template == template ||
        memcmp(template, tiles->states[node].template, sizeof(MVMJitTileTemplate)) == 0) {
        /* happy case, no conflict */
        tiles->states[node].rule     = rule_nr;
        tiles->states[node].template = template;
        return node;
    } else {
        /* resolve conflict by copying this node */
        const MVMJitExprOpInfo *info = tree->info[node].op_info;
        MVMint32 space = (info->nchild < 0 ?
                          2 + tree->nodes[node+1] + info->nargs :
                          1 + info->nchild + info->nargs);
        MVMint32 num   = tree->nodes_num;

        /* NB - we should have an append_during_traversal function
         * because the following is quite a common pattern */

        /* Internal copy; hence no realloc may happen during append, ensure the
         * space is available before the copy */
        MVM_DYNAR_ENSURE_SPACE(tree->nodes, space);
        MVM_DYNAR_APPEND(tree->nodes, tree->nodes + node, space);

        /* Copy the information node as well */
        MVM_DYNAR_ENSURE_SIZE(tree->info, num);
        memcpy(tree->info + num, tree->info + node, sizeof(MVMJitExprNodeInfo));

        /* Also ensure the visits and tiles array are of correct size */
        MVM_DYNAR_ENSURE_SIZE(tiles->states, num);
        MVM_DYNAR_ENSURE_SIZE(traverser->visits, num);

        /* Assign the new tile */
        tiles->states[num].rule     = rule_nr;
        tiles->states[num].template = template;

        /* Return reference to new node */
        return num;
    }
}



/* Preorder propagation of rules downward */
static void select_tiles(MVMThreadContext *tc, MVMJitTreeTraverser *traverser,
                         MVMJitExprTree *tree, MVMint32 node) {

    MVMJitExprNode op    = tree->nodes[node];
    MVMint32 first_child = node+1;
    MVMint32 nchild      = (tree->info[node].op_info->nchild < 0 ?
                            tree->nodes[first_child++] :
                            tree->info[node].op_info->nchild);
    struct TileTree *tiles = traverser->data;

    const MVMJitTileTemplate *tile = tiles->states[node].template;
    MVMint32 left_sym = tile->left_sym, right_sym = tile->right_sym;

/* Tile assignment is somewhat precarious due to (among other things), possible
 * reallocation. So let's provide a single macro to do it correctly. */
#define DO_ASSIGN_CHILD(NUM, SYM) do { \
        MVMint32 child     = tree->nodes[first_child+(NUM)]; \
        MVMint32 state     = tiles->states[child].state; \
        MVMint32 rule      = MVM_jit_tile_select_lookup(tc, state, (SYM)); \
        MVMint32 assigned  = assign_tile(tc, tree, traverser, child, rule); \
        tree->nodes[first_child+(NUM)] = assigned; \
    } while(0)

    switch (op) {
    case MVM_JIT_ALL:
    case MVM_JIT_ANY:
    case MVM_JIT_ARGLIST:
        {
            MVMint32 i;
            for (i = 0; i < nchild; i++) {
                DO_ASSIGN_CHILD(i, left_sym);
            }
        }
        break;
    case MVM_JIT_DO:
        {
            MVMint32 i, last_child, last_rule;
            for (i = 0; i < nchild - 1; i++) {
                DO_ASSIGN_CHILD(i, left_sym);
            }
            DO_ASSIGN_CHILD(i, right_sym);
        }
        break;
    case MVM_JIT_IF:
    case MVM_JIT_EITHER:
        {
            DO_ASSIGN_CHILD(0, left_sym);
            DO_ASSIGN_CHILD(1, right_sym);
            DO_ASSIGN_CHILD(2, right_sym);
        }
        break;
    default:
        {
            if (nchild > 0) {
                DO_ASSIGN_CHILD(0, left_sym);
            }
            if (nchild > 1) {
                DO_ASSIGN_CHILD(1, right_sym);
            }
            if (nchild > 2) {
                MVM_oops(tc, "Can't tile %d children of %s", nchild, tree->info[node].op_info->name);
            }
        }
    }

#undef DO_ASSIGN_CHILD
}

static void arglist_get_values(MVMThreadContext *tc, MVMJitExprTree *tree, MVMint32 node, MVMJitExprValue **values) {
    MVMint32 i, nchild = tree->nodes[node+1];
    for (i = 0; i < nchild; i++) {
        MVMint32 carg = tree->nodes[node+2+i];
        MVMint32 val  = tree->nodes[carg+1];
        *values++     = &tree->info[val].value;
    }
}



static void select_values(MVMThreadContext *tc, MVMJitTreeTraverser *traverser,
                          MVMJitExprTree *tree, MVMint32 node) {

    MVMJitExprValue *cur_value = &tree->info[node].value;
    MVMJitExprNode args[8];
    struct TileTree *tiles = traverser->data;
    const MVMJitTileTemplate *template = tiles->states[node].template;
    MVMJitTile *tile;
    MVMJitTileList *list = tiles->list;

    MVMint32 i, num_values;
    /* pre-increment order nr  */
    tiles->order_nr++;

    /* create tile object */
    tile            = MVM_spesh_alloc(tc, tiles->sg, sizeof(MVMJitTile));
    tile->emit      = template->emit;
    tile->num_vals  = template->num_vals;
    tile->node      = node;
    tile->order_nr  = tiles->order_nr;
    tile->values[0] = cur_value;
    /* insert into tile list */
    if (list->last != NULL) {
        list->last->next = tile;
    }
    if (list->first == NULL) {
        list->first = tile;
    }
    list->last = tile;
    /* Log tile for debugging */
    if (template->expr)
        MVM_jit_log(tc, "%04d/%04d: %s\n", tiles->order_nr, node, template->expr);

    switch (tree->nodes[node]) {
    case MVM_JIT_IF:
        num_values = 0;
        cur_value->first_created = tiles->order_nr;
        break;
    case MVM_JIT_ARGLIST:
        arglist_get_values(tc, tree, node, tile->values + 1);
        num_values = tree->nodes[node+1];
        break;
    case MVM_JIT_DO:
        {
            MVMint32 nchild = tree->nodes[node+1];
            MVMint32 last_child = tree->nodes[node+1+nchild];
            tile->values[1] = &tree->info[last_child].value;
            num_values = 1;
        }
        break;
    default:
        if (template->path == NULL)
            return;
        cur_value->first_created = tiles->order_nr;
        MVM_jit_tile_get_values(tc, tree, node, template->path, template->regs,
                                tile->values + 1, tile->args);
        num_values = template->num_vals;
        break;
    }

    /* not sure if this is still the right place for these calculations! */
    cur_value->type    = template->vtype;
    cur_value->num_use = 0;
    /* update use information */
    for (i = 0; i < num_values; i++) {
        /* use +1 offset because values[0] now contains the nodes value */
        tile->values[i+1]->last_use  = MAX(tile->values[i+1]->last_use, tiles->order_nr);
        tile->values[i+1]->num_use++;
    }
}


MVMJitTileList * MVM_jit_tile_expr_tree(MVMThreadContext *tc, MVMJitExprTree *tree) {
    MVMJitTreeTraverser traverser;
    MVMint32 i;
    struct TileTree tiles;

    MVM_DYNAR_INIT(tiles.states, tree->nodes_num);
    traverser.policy = MVM_JIT_TRAVERSER_ONCE;
    traverser.inorder = NULL;
    traverser.preorder = NULL;
    traverser.postorder = &tile_node;
    traverser.data = &tiles;

    MVM_jit_expr_tree_traverse(tc, tree, &traverser);
    /* 'pushdown' of tiles to roots */
    for (i = 0; i < tree->roots_num; i++) {
        MVMint32 node = tree->roots[i];
        assign_tile(tc, tree, &traverser, tree->roots[i], tiles.states[node].rule);
    }

    /* NB - we can add actual code generation during the postorder step here */
    tiles.sg            = tree->graph->sg;
    tiles.list          = MVM_spesh_alloc(tc, tiles.sg, sizeof(MVMJitTileList));
    tiles.order_nr      = 0;
    traverser.preorder  = &select_tiles;
    traverser.postorder = &select_values;
    MVM_jit_expr_tree_traverse(tc, tree, &traverser);

    MVM_free(tiles.states);
    return tiles.list;
}

#define FIRST_CHILD(t,x) (t->info[x].op_info->nchild < 0 ? x + 2 : x + 1)
/* Get input for a tile rule, write into values and args */
void MVM_jit_tile_get_values(MVMThreadContext *tc, MVMJitExprTree *tree, MVMint32 node,
                             const MVMint8 *path, MVMint32 regs,
                             MVMJitExprValue **values, MVMJitExprNode *args) {
    while (*path) {
        MVMJitExprNode cur_node = node;
        do {
            MVMint32 first_child = FIRST_CHILD(tree, cur_node) - 1;
            MVMint32 child_nr    = *path++ - '0';
            cur_node = tree->nodes[first_child+child_nr];
        } while (*path != '.');
        /* regs nodes go to values, others to args */
        if (regs & 1) {
            *values++ = &tree->info[cur_node].value;
        } else {
            *args++ = cur_node;
        }
        regs >>= 1;
        path++;
    }
}

#undef FIRST_CHILD