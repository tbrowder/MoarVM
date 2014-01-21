/* Ensures that if a generation 2 object comes to hold a reference to a
 * nursery object, then the generation 2 object becomes an inter-generational
 * root. */
#define MVM_WB(tc, update_root, referenced) \
    { \
        MVMCollectable *u = (MVMCollectable *)update_root; \
        MVMCollectable *r = (MVMCollectable *)referenced; \
        if (((u->flags & MVM_CF_SECOND_GEN) && r && !(r->flags & MVM_CF_SECOND_GEN))) \
            MVM_gc_write_barrier_hit(tc, u); \
    }

/* Does an assignment, but makes sure the write barrier MVM_WB is applied
 * first. Takes the root object, the address within it we're writing to, and
 * the thing we're writing. Note that update_addr is not involved in the
 * write barrier. */
#define MVM_ASSIGN_REF(tc, update_root, update_addr, referenced) \
    { \
        void *_r = referenced; \
        MVM_WB(tc, update_root, _r); \
        update_addr = _r; \
    }

/* Function for if the write barrier is hit. */
MVM_PUBLIC void MVM_gc_write_barrier_hit(MVMThreadContext *tc, MVMCollectable *update_root);

/* Write barrier for binding lexicals in frames. */
#define MVM_FRAME_LEX_WB(tc, bound_frame) \
    { \
        MVMFrame *wb_f = bound_frame; \
        if (wb_f->gen2 && !wb_f->in_gen2_roots) \
            MVM_gc_root_gen2_frame_add(tc, wb_f); \
    }



