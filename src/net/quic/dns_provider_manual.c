/*
 * dns_provider_manual.c — operator-prompted DNS provider.
 *
 * Prints the requested TXT record to stderr via the conduit print
 * path, waits for the user to confirm via stdin, and proceeds.
 * Designed for dev / CI runs against Let's Encrypt staging where DNS
 * edits are done by hand.
 *
 * Reads from `/dev/tty` if stdin isn't a TTY (some operator
 * environments redirect stdin to a pipe); falls back to stdin
 * otherwise.
 */

#define N00B_USE_INTERNAL_API
#include <stdio.h>
#include <unistd.h>

#include "n00b.h"
#include "core/alloc.h"
#include "core/runtime.h"
#include "core/string.h"
#include "adt/result.h"
#include "conduit/print.h"
#include "net/quic/quic_types.h"
#include "net/quic/dns_provider.h"

static n00b_allocator_t *
mp_alloc(void)
{
    return (n00b_allocator_t *)&n00b_get_runtime()->conduit_pool;
}

/* Wait for the operator to press ENTER.  Reads from /dev/tty if
 * available so the caller can pipe stdin without breaking us. */
static void
wait_for_enter(void)
{
    FILE *tty = fopen("/dev/tty", "r");
    FILE *src = tty ? tty : stdin;
    int c;
    do {
        c = fgetc(src);
    } while (c != '\n' && c != EOF);
    if (tty) fclose(tty);
}

static int
manual_set_txt(n00b_quic_dns_provider_t *self,
               const char               *fqdn,
               const char               *value)
{
    (void)self;
    if (!fqdn || !value) {
        return N00B_QUIC_ERR_NULL_ARG;
    }
    n00b_printf("\n[n00b acme] Please add the following DNS TXT record:\n"
                "  Name:  «#»\n"
                "  Value: \"«#»\"\n"
                "\n"
                "Then press ENTER to continue, or Ctrl-C to abort.",
                n00b_string_from_cstr(fqdn),
                n00b_string_from_cstr(value),
                .fd = 2);
    wait_for_enter();
    return N00B_QUIC_OK;
}

static int
manual_remove_txt(n00b_quic_dns_provider_t *self,
                  const char               *fqdn,
                  const char               *value)
{
    (void)self;
    (void)value;
    if (!fqdn) {
        return N00B_QUIC_ERR_NULL_ARG;
    }
    n00b_printf("\n[n00b acme] You may now remove the TXT record at:\n"
                "  «#»\n"
                "(no need to press ENTER; cleanup is operator-driven.)",
                n00b_string_from_cstr(fqdn),
                .fd = 2);
    return N00B_QUIC_OK;
}

static void
manual_close(n00b_quic_dns_provider_t *self)
{
    (void)self;
    /* Nothing to release. */
}

n00b_quic_dns_provider_t *
n00b_quic_dns_provider_manual(void)
{
    n00b_quic_dns_provider_t *p = n00b_alloc_with_opts(
        n00b_quic_dns_provider_t,
        &(n00b_alloc_opts_t){.allocator = mp_alloc()});
    p->name       = "manual";
    p->set_txt    = manual_set_txt;
    p->remove_txt = manual_remove_txt;
    p->close      = manual_close;
    p->ctx        = nullptr;
    return p;
}
