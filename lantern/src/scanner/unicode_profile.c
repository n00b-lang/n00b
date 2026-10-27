#include "private/scanner/unicode_profile.h"
#include "private/unicode/ucd.h"
#include "lib/alloc.h"
#include "lib/buffer.h"
#include "unicode/encoding.h"

#include <stdio.h>
#include <string.h>

struct scan_unicode_profile_t {
    scan_lex_identifier_profile_t spec;
    bool restrict_scripts;
    bool allowed_scripts[256];
};

core_string_t
scan_unicode_profile_version(void)
{
    return ucd_version();
}

core_string_t
scan_lex_unicode_version(void)
{
    return scan_unicode_profile_version();
}

__attribute__((format(printf, 2, 3))) static void
set_error(core_string_t *error, const char *fmt, ...)
{
    if (!error) {
        return;
    }

    core_buffer_t *buf = core_buffer_empty();
    va_list        ap;

    va_start(ap, fmt);
    core_buffer_vprintf(buf, fmt, ap);
    va_end(ap);
    core_free(error->data);
    *error = core_buffer_take_string(buf);
}

static bool
version_matches(core_string_t requested)
{
    core_string_t actual = scan_unicode_profile_version();
    if (!requested.data || !requested.u8_bytes) {
        return true;
    }
    if (requested.u8_bytes > actual.u8_bytes) {
        return false;
    }
    return !memcmp(requested.data, actual.data, requested.u8_bytes)
        && (requested.u8_bytes == actual.u8_bytes
            || actual.data[requested.u8_bytes] == '.');
}

scan_unicode_profile_t *
scan_unicode_profile_compile(const scan_lex_identifier_profile_t *spec,
                             core_string_t *error)
{
    if (!spec || !spec->name.data) {
        return nullptr;
    }
    if (!version_matches(spec->unicode_version)) {
        core_string_t actual = scan_unicode_profile_version();
        set_error(error,
                 "Unicode profile '%.*s' requests version '%.*s'; build provides '%.*s'",
                 (int)spec->name.u8_bytes, spec->name.data,
                 (int)spec->unicode_version.u8_bytes,
                 spec->unicode_version.data,
                 (int)actual.u8_bytes, actual.data);
        return nullptr;
    }

    scan_unicode_profile_t *profile = core_alloc(scan_unicode_profile_t);
    profile->spec = *spec;

    for (const core_string_t *name = spec->scripts;
         name && name->data; name++) {
        uint8_t script = 0;
        if (!ucd_script_named(*name, &script)) {
            set_error(error,
                     "Unicode profile '%.*s' names unknown script '%.*s'",
                     (int)spec->name.u8_bytes, spec->name.data,
                     (int)name->u8_bytes, name->data);
            scan_unicode_profile_free(profile);
            return nullptr;
        }
        profile->restrict_scripts = true;
        profile->allowed_scripts[script] = true;
    }
    return profile;
}

void
scan_unicode_profile_free(scan_unicode_profile_t *profile)
{
    core_free(profile);
}

bool
scan_unicode_profile_is_start(const scan_unicode_profile_t *profile,
                              core_codepoint_t cp)
{
    return ucd_is_xid_start(cp)
        || cp == '_'
        || (profile->spec.emoji != SCAN_LEX_EMOJI_REJECT
            && ucd_is_extended_pictographic(cp));
}

bool
scan_unicode_profile_is_continue(const scan_unicode_profile_t *profile,
                                 core_codepoint_t cp)
{
    return ucd_is_xid_continue(cp)
        || (profile->spec.emoji != SCAN_LEX_EMOJI_REJECT
            && ucd_is_extended_pictographic(cp));
}

static bool
script_allowed(const scan_unicode_profile_t *profile, uint8_t script)
{
    return !profile->restrict_scripts || script == UCD_SCRIPT_COMMON
        || script == UCD_SCRIPT_INHERITED || profile->allowed_scripts[script];
}

static bool
diagnose(scan_unicode_identifier_result_t *result, scan_problem_t problem,
         size_t start, size_t end)
{
    result->diagnosed = true;
    result->problem = problem;
    result->diagnostic_start = start;
    result->diagnostic_end = end;
    return true;
}

static bool
check_normalized(const scan_unicode_profile_t *profile, core_string_t spelling,
                 scan_unicode_identifier_result_t *result)
{
    if (profile->spec.normalization == SCAN_LEX_NORMALIZE_NONE) {
        return false;
    }
    bool compat = profile->spec.normalization == SCAN_LEX_NORMALIZE_NFKC;
    core_string_t normalized = ucd_normalize(spelling,
                                             compat ? UCD_NFKC : UCD_NFC);
    if (!normalized.data || core_string_eq(normalized, spelling)) {
        core_free(normalized.data);
        return false;
    }
    size_t start = 0;
    while (start < spelling.u8_bytes && start < normalized.u8_bytes
           && spelling.data[start] == normalized.data[start]) {
        start++;
    }
    while (start > 0 && ((unsigned char)spelling.data[start] & 0xC0) == 0x80) {
        start--;
    }
    result->diagnosed = true;
    result->problem = compat
        ? SCAN_PROBLEM_WITH(IDENTIFIER_NOT_NFKC, normalized)
                             : SCAN_PROBLEM_WITH(IDENTIFIER_NOT_NFC,
                                 normalized);
    result->diagnostic_start = start;
    result->diagnostic_end = spelling.u8_bytes;
    return true;
}

bool
scan_unicode_profile_process(const scan_unicode_profile_t *profile,
                             core_string_t spelling,
                             scan_unicode_identifier_result_t *result)
{
    if (!profile || !result) {
        return false;
    }
    *result = (scan_unicode_identifier_result_t){0};
    uint8_t seen = UCD_SCRIPT_UNKNOWN;
    bool have_seen = false;
    uint32_t at = 0;
    while (at < spelling.u8_bytes) {
        size_t start = at;
        int32_t cp = unicode_utf8_decode(spelling.data,
                                         (uint32_t)spelling.u8_bytes, &at);
        if (cp < 0) {
            return diagnose(result,
                            SCAN_PROBLEM(IDENTIFIER_INVALID_UTF8),
                            start, start + 1);
        }
        uint8_t script = ucd_script((core_codepoint_t)cp);
        if (!script_allowed(profile, script)) {
            return diagnose(result, SCAN_PROBLEM(IDENTIFIER_DISALLOWED_SCRIPT),
                            start, at);
        }
        if (profile->spec.emoji == SCAN_LEX_EMOJI_DIAGNOSE
            && ucd_is_extended_pictographic((core_codepoint_t)cp)) {
            return diagnose(result, SCAN_PROBLEM(IDENTIFIER_EMOJI),
                            start, at);
        }
        if (script != UCD_SCRIPT_COMMON && script != UCD_SCRIPT_INHERITED) {
            if (profile->spec.diagnose_mixed_scripts && have_seen
                && seen != script) {
                return diagnose(result, SCAN_PROBLEM(IDENTIFIER_MIXED_SCRIPTS),
                                start, at);
            }
            seen = script;
            have_seen = true;
        }
    }

    if (check_normalized(profile, spelling, result)) {
        return true;
    }

    if (profile->spec.diagnose_confusables) {
        core_string_t skeleton = ucd_skeleton(spelling);
        bool changed = skeleton.data && !core_string_eq(skeleton, spelling);
        core_free(skeleton.data);
        if (changed) {
            return diagnose(result, SCAN_PROBLEM(IDENTIFIER_CONFUSABLE),
                            0, spelling.u8_bytes);
        }
    }
    return true;
}
