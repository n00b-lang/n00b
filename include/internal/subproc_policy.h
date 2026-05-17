#pragma once

#include "conduit/subproc.h"

static inline bool
n00b_subproc_subs_requested(n00b_array_t(n00b_subproc_buf_inbox_t *) *subs)
{
    return subs && subs->len > 0;
}

static inline bool
n00b_subproc_xforms_requested(n00b_array_t(void *) *xforms)
{
    return xforms && xforms->len > 0;
}

static inline n00b_conduit_inbox_t(n00b_buffer_t *) *
n00b_subproc_wire_capture(n00b_conduit_t *c,
                          n00b_conduit_topic_t(n00b_buffer_t *) *source,
                          n00b_buffer_t **accum,
                          n00b_conduit_sub_handle_t *out_sub)
{
    if (!source || !accum) {
        return nullptr;
    }

    if (!*accum || (*accum)->alloc_len == 0) {
        *accum = n00b_buffer_empty();
    }

    n00b_conduit_inbox_t(n00b_buffer_t *) *inbox =
        n00b_alloc_with_opts(n00b_conduit_inbox_t(n00b_buffer_t *),
            &(n00b_alloc_opts_t){.allocator = c->allocator});
    n00b_conduit_inbox_init(n00b_buffer_t *, inbox, c,
                            N00B_CONDUIT_BP_UNBOUNDED, 0);

    *out_sub = n00b_conduit_subscribe(n00b_buffer_t *, source, inbox,
                                      .operations = N00B_CONDUIT_OP_ALL);
    return inbox;
}

static inline void
n00b_subproc_drain_capture(n00b_conduit_inbox_t(n00b_buffer_t *) *inbox,
                           n00b_buffer_t *accum)
{
    if (!inbox || !accum) {
        return;
    }

    n00b_conduit_message_t(n00b_buffer_t *) *msg;
    while ((msg = n00b_conduit_inbox_pop_msg(n00b_buffer_t *, inbox)) != nullptr) {
        n00b_buffer_t *buf = msg->payload;
        if (buf && buf->byte_len > 0) {
            n00b_buffer_concat(accum, buf);
        }
    }
}

static inline n00b_conduit_topic_t(n00b_buffer_t *) *
n00b_subproc_apply_xform_chain(n00b_conduit_t *c,
                               n00b_conduit_topic_t(n00b_buffer_t *) *source,
                               n00b_array_t(void *) *specs)
{
    if (!source) {
        return nullptr;
    }
    if (!n00b_subproc_xforms_requested(specs)) {
        return source;
    }

    n00b_conduit_topic_base_t *out =
        n00b_conduit_chain_from_specs(
            c,
            (n00b_conduit_topic_base_t *)source,
            (const n00b_conduit_xform_spec_base_t **)specs->data,
            specs->len);

    return out
        ? (n00b_conduit_topic_t(n00b_buffer_t *) *)out
        : source;
}

static inline void
n00b_subproc_wire_user_subs(n00b_conduit_t *c,
                            n00b_conduit_topic_t(n00b_buffer_t *) *source,
                            n00b_array_t(n00b_subproc_buf_inbox_t *) *subs)
{
    (void)c;

    if (!source || !subs) {
        return;
    }

    for (size_t i = 0; i < subs->len; i++) {
        n00b_subproc_buf_inbox_t *inbox = subs->data[i];
        if (!inbox) {
            continue;
        }
        n00b_conduit_subscribe(n00b_buffer_t *, source, inbox,
                               .operations = N00B_CONDUIT_OP_ALL);
    }
}

static inline uint32_t
n00b_subproc_completion_mask(bool wait_stdout,
                             bool wait_stderr,
                             bool wait_stdin)
{
    uint32_t mask = N00B_SUBPROC_DONE_F_PROC_EXIT;
    if (wait_stdout) {
        mask |= N00B_SUBPROC_DONE_F_STDOUT_EOF;
    }
    if (wait_stderr) {
        mask |= N00B_SUBPROC_DONE_F_STDERR_EOF;
    }
    if (wait_stdin) {
        mask |= N00B_SUBPROC_DONE_F_STDIN_DONE;
    }
    return mask;
}

static inline void
n00b_subproc_note_proc_done(n00b_subproc_t *sp)
{
    sp->done_flags |= N00B_SUBPROC_DONE_F_PROC_EXIT;
}

static inline void
n00b_subproc_note_stdout_done(n00b_subproc_t *sp)
{
    sp->done_flags |= N00B_SUBPROC_DONE_F_STDOUT_EOF
                    | N00B_SUBPROC_DONE_F_STDOUT_DRAIN;
}

static inline void
n00b_subproc_note_stderr_done(n00b_subproc_t *sp)
{
    sp->done_flags |= N00B_SUBPROC_DONE_F_STDERR_EOF
                    | N00B_SUBPROC_DONE_F_STDERR_DRAIN;
}

static inline bool
n00b_subproc_done_condition_met(n00b_subproc_t *sp)
{
    switch (sp->done_condition) {
    case N00B_SUBPROC_DONE_IO_DRAINED:
        return (sp->done_flags & sp->required_mask) == sp->required_mask;
    case N00B_SUBPROC_DONE_PROC_EXIT:
        return (sp->done_flags & N00B_SUBPROC_DONE_F_PROC_EXIT) != 0;
    case N00B_SUBPROC_DONE_STDOUT_EOF:
        return (sp->done_flags & N00B_SUBPROC_DONE_F_STDOUT_EOF) != 0;
    case N00B_SUBPROC_DONE_CUSTOM:
        return sp->done_fn && sp->done_fn(sp, sp->done_fn_ctx);
    }
    return false;
}
