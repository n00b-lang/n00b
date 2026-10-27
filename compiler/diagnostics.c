#include "diagnostics.h"

#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "compiler_cli.h"
#include "n00bb_version.h"

#include "lib/alloc.h"
#include "lib/buffer.h"
#include "unicode/encoding.h"

static size_t
fold_codepoints(core_string_t text, uint32_t *out)
{
    size_t count = 0;
    uint32_t at = 0;
    while (at < text.u8_bytes) {
        int32_t cp = unicode_utf8_decode(text.data, (uint32_t)text.u8_bytes,
                                         &at);
        if (cp < 0) {
            at++;
            continue;
        }
        if (cp >= 'A' && cp <= 'Z') cp += 'a' - 'A';
        if (cp == '-') cp = '_';
        out[count++] = (uint32_t)cp;
    }
    return count;
}

static size_t
edit_distance(const uint32_t *a, size_t a_len, const uint32_t *b,
              size_t b_len)
{
    size_t width = b_len + 1;
    size_t *rows = core_alloc_array(size_t, 3 * width);
    size_t *before = rows;
    size_t *previous = rows + width;
    size_t *current = rows + 2 * width;
    for (size_t j = 0; j <= b_len; j++) previous[j] = j;
    for (size_t i = 1; i <= a_len; i++) {
        current[0] = i;
        for (size_t j = 1; j <= b_len; j++) {
            size_t cost = a[i - 1] == b[j - 1] ? 0 : 1;
            size_t best = previous[j] + 1;
            if (current[j - 1] + 1 < best) best = current[j - 1] + 1;
            if (previous[j - 1] + cost < best) best = previous[j - 1] + cost;
            if (i > 1 && j > 1 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1]
                && before[j - 2] + 1 < best)
                best = before[j - 2] + 1;
            current[j] = best;
        }
        size_t *spare = before;
        before = previous;
        previous = current;
        current = spare;
    }
    size_t result = previous[b_len];
    core_free(rows);
    return result;
}

size_t
n00bb_closest_names(core_string_t name, const core_string_t *candidates,
                    size_t count, size_t *best, size_t best_capacity)
{
    if (!name.u8_bytes || !candidates || !best || !best_capacity) return 0;
    uint32_t *wanted = core_alloc_array(uint32_t, name.u8_bytes);
    size_t wanted_len = fold_codepoints(name, wanted);
    size_t limit = wanted_len < 3 ? 1 : (wanted_len + 2) / 3;
    size_t *distances = core_alloc_array(size_t, best_capacity);
    size_t found = 0;
    for (size_t i = 0; i < count; i++) {
        core_string_t candidate = candidates[i];
        if (!candidate.u8_bytes || core_string_eq(candidate, name)) continue;
        bool duplicate = false;
        for (size_t k = 0; k < found && !duplicate; k++)
            duplicate = core_string_eq(candidates[best[k]], candidate);
        if (duplicate) continue;
        uint32_t *other = core_alloc_array(uint32_t, candidate.u8_bytes);
        size_t other_len = fold_codepoints(candidate, other);
        size_t distance = edit_distance(wanted, wanted_len, other, other_len);
        core_free(other);
        if (distance > limit || distance >= wanted_len) continue;
        size_t slot = found;
        while (slot > 0 && distances[slot - 1] > distance) slot--;
        if (slot >= best_capacity) continue;
        size_t last = found < best_capacity ? found : best_capacity - 1;
        for (size_t k = last; k > slot; k--) {
            best[k] = best[k - 1];
            distances[k] = distances[k - 1];
        }
        best[slot] = i;
        distances[slot] = distance;
        if (found < best_capacity) found++;
    }
    core_free(distances);
    core_free(wanted);
    return found;
}

bool
n00bb_diag_did_you_mean(core_diagnostics_t *d, core_string_t name,
                        const core_string_t *candidates, size_t count)
{
    size_t best[3];
    size_t found = n00bb_closest_names(name, candidates, count, best, 3);
    if (!found) return false;
    core_string_t choices[3];
    for (size_t i = 0; i < found; i++) choices[i] = candidates[best[i]];
    n00bb_report_help(d, N00B_TEXT_DID_YOU_MEAN,
                      N00BB_ARGS(ARG_CHOICES(choices, found)));
    return true;
}

int
n00bb_explain_command(core_string_t code)
{
    n00bb_diag_t id;
    if (!n00bb_diag_lookup(code, &id)) {
        core_string_t codes[N00B_DIAG_COUNT];
        for (size_t i = 0; i < N00B_DIAG_COUNT; i++)
            codes[i] = n00bb_diag_code((n00bb_diag_t)i);
        core_diagnostics_t *d = core_diagnostics_new();
        n00bb_report(d, nullptr, N00B_DIAG_UNKNOWN_DIAGNOSTIC_CODE,
                     (core_diag_location_t){}, N00B_TEXT_UNKNOWN_CODE,
                     N00BB_ARGS(ARG_TEXT(code)));
        n00bb_diag_did_you_mean(d, code, codes, N00B_DIAG_COUNT);
        n00bb_print_diagnostics(d);
        core_diagnostics_free(d);
        return 2;
    }
    core_string_t title = n00bb_diag_title(id);
    core_string_t text = n00bb_diag_explanation(id);
    core_string_t name = n00bb_diag_code(id);
    printf("%.*s: %.*s\n\n%.*s\n", (int)name.u8_bytes, name.data,
           (int)title.u8_bytes, title.data, (int)text.u8_bytes, text.data);
    return 0;
}

#ifndef _WIN32
static const struct {
    int signal;
    const char *name;
} fatal_signals[] = {
    {SIGSEGV, "SIGSEGV"}, {SIGBUS, "SIGBUS"}, {SIGILL, "SIGILL"},
    {SIGFPE, "SIGFPE"}, {SIGABRT, "SIGABRT"},
};
static core_string_t crash_reports[sizeof(fatal_signals)
                                   / sizeof(fatal_signals[0])];

static void
report_crash(int signal)
{
    for (size_t i = 0; i < sizeof(fatal_signals) / sizeof(fatal_signals[0]);
         i++)
        if (fatal_signals[i].signal == signal && crash_reports[i].data)
            (void)!write(STDERR_FILENO, crash_reports[i].data,
                         crash_reports[i].u8_bytes);
    _exit(3);
}

void
n00bb_install_crash_handler(void)
{
    stack_t alternate = {.ss_size = 1u << 16};
    alternate.ss_sp = malloc(alternate.ss_size);
    if (alternate.ss_sp) sigaltstack(&alternate, nullptr);
    for (size_t i = 0; i < sizeof(fatal_signals) / sizeof(fatal_signals[0]);
         i++) {
        core_diagnostics_t *d = core_diagnostics_new();
        n00bb_internal_error(d, nullptr, (core_diag_location_t){0}, nullptr,
                             "the compiler crashed with %s",
                             fatal_signals[i].name);
        crash_reports[i] = core_diagnostics_render(d,
                                                   n00bb_diag_render_options());
        core_diagnostics_free(d);
        struct sigaction action = {.sa_handler = report_crash,
                                   .sa_flags = SA_ONSTACK | SA_RESETHAND};
        sigemptyset(&action.sa_mask);
        sigaction(fatal_signals[i].signal, &action, nullptr);
    }
}
#else
void
n00bb_install_crash_handler(void)
{
}
#endif
