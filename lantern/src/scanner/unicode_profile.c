#include "private/scanner/unicode_profile.h"
#include "lib/alloc.h"

#include <stdio.h>
#include <string.h>

#ifdef LANTERN_HAVE_ICU
#include <unicode/uchar.h>
#include <unicode/unorm2.h>
#include <unicode/uscript.h>
#include <unicode/uspoof.h>
#include <unicode/ustring.h>
#include <unicode/utf8.h>
#endif

struct scan_unicode_profile_t {
    scan_lex_identifier_profile_t spec;
#ifdef LANTERN_HAVE_ICU
    UScriptCode *scripts;
    size_t       script_count;
    const UNormalizer2 *normalizer;
    USpoofChecker      *spoof;
#endif
};

core_string_t
scan_unicode_profile_version(void)
{
#ifdef LANTERN_HAVE_ICU
    static const char text[] = U_UNICODE_VERSION;
    return core_string_borrow(text, sizeof(text) - 1);
#else
    return (core_string_t){0};
#endif
}

core_string_t
scan_lex_unicode_version(void)
{
    return scan_unicode_profile_version();
}

#ifdef LANTERN_HAVE_ICU
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
#endif

scan_unicode_profile_t *
scan_unicode_profile_compile(const scan_lex_identifier_profile_t *spec,
                             char *error, size_t error_size)
{
    if (!spec || !spec->name.data) {
        return nullptr;
    }
#ifndef LANTERN_HAVE_ICU
    snprintf(error, error_size,
             "Unicode identifier profiles require an ICU-enabled build");
    return nullptr;
#else
    if (!version_matches(spec->unicode_version)) {
        core_string_t actual = scan_unicode_profile_version();
        snprintf(error, error_size,
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
        char *text = core_strndup(name->data, name->u8_bytes);
        UErrorCode status = U_ZERO_ERROR;
        UScriptCode codes[8];
        int32_t count = uscript_getCode(text, codes, 8, &status);
        core_free(text);
        if (U_FAILURE(status) || count < 1) {
            snprintf(error, error_size,
                     "Unicode profile '%.*s' names unknown script '%.*s'",
                     (int)spec->name.u8_bytes, spec->name.data,
                     (int)name->u8_bytes, name->data);
            scan_unicode_profile_free(profile);
            return nullptr;
        }
        for (int32_t i = 0; i < count; i++) {
            bool duplicate = false;
            for (size_t j = 0; j < profile->script_count; j++) {
                duplicate = duplicate || profile->scripts[j] == codes[i];
            }
            if (!duplicate) {
                profile->scripts = core_realloc(
                    profile->scripts,
                    (profile->script_count + 1)
                        * sizeof(*profile->scripts));
                profile->scripts[profile->script_count++] = codes[i];
            }
        }
    }

    UErrorCode status = U_ZERO_ERROR;
    if (spec->normalization == SCAN_LEX_NORMALIZE_NFC) {
        profile->normalizer = unorm2_getNFCInstance(&status);
    }
    else if (spec->normalization == SCAN_LEX_NORMALIZE_NFKC) {
        profile->normalizer = unorm2_getNFKCInstance(&status);
    }
    if (U_FAILURE(status)) {
        snprintf(error, error_size, "cannot initialize Unicode normalizer");
        scan_unicode_profile_free(profile);
        return nullptr;
    }

    if (spec->diagnose_confusables) {
        status = U_ZERO_ERROR;
        profile->spoof = uspoof_open(&status);
        if (U_FAILURE(status)) {
            snprintf(error, error_size, "cannot initialize confusable checks");
            scan_unicode_profile_free(profile);
            return nullptr;
        }
    }
    return profile;
#endif
}

void
scan_unicode_profile_free(scan_unicode_profile_t *profile)
{
    if (!profile) {
        return;
    }
#ifdef LANTERN_HAVE_ICU
    if (profile->spoof) {
        uspoof_close(profile->spoof);
    }
    core_free(profile->scripts);
#endif
    core_free(profile);
}

bool
scan_unicode_profile_is_start(const scan_unicode_profile_t *profile,
                              core_codepoint_t cp)
{
#ifdef LANTERN_HAVE_ICU
    return u_hasBinaryProperty(cp, UCHAR_XID_START)
        || cp == '_'
        || (profile->spec.emoji != SCAN_LEX_EMOJI_REJECT
            && u_hasBinaryProperty(cp, UCHAR_EXTENDED_PICTOGRAPHIC));
#else
    (void)profile; (void)cp;
    return false;
#endif
}

bool
scan_unicode_profile_is_continue(const scan_unicode_profile_t *profile,
                                 core_codepoint_t cp)
{
#ifdef LANTERN_HAVE_ICU
    return u_hasBinaryProperty(cp, UCHAR_XID_CONTINUE)
        || (profile->spec.emoji != SCAN_LEX_EMOJI_REJECT
            && u_hasBinaryProperty(cp, UCHAR_EXTENDED_PICTOGRAPHIC));
#else
    (void)profile; (void)cp;
    return false;
#endif
}

#ifdef LANTERN_HAVE_ICU
static bool
script_allowed(const scan_unicode_profile_t *profile, UScriptCode script)
{
    if (!profile->script_count || script == USCRIPT_COMMON
        || script == USCRIPT_INHERITED) {
        return true;
    }
    for (size_t i = 0; i < profile->script_count; i++) {
        if (profile->scripts[i] == script) {
            return true;
        }
    }
    return false;
}

static core_string_t
normalize(const scan_unicode_profile_t *profile, core_string_t input)
{
    if (!profile->normalizer) {
        return (core_string_t){0};
    }
    UErrorCode status = U_ZERO_ERROR;
    int32_t u16_len = 0;
    u_strFromUTF8(nullptr, 0, &u16_len, input.data, (int32_t)input.u8_bytes,
                  &status);
    if (status != U_BUFFER_OVERFLOW_ERROR && U_FAILURE(status)) {
        return (core_string_t){0};
    }
    status = U_ZERO_ERROR;
    UChar *u16 = core_alloc_array(UChar, (size_t)u16_len + 1);
    u_strFromUTF8(u16, u16_len + 1, nullptr, input.data,
                  (int32_t)input.u8_bytes, &status);
    if (U_FAILURE(status)) {
        core_free(u16);
        return (core_string_t){0};
    }
    status = U_ZERO_ERROR;
    int32_t normalized_len = unorm2_normalize(profile->normalizer, u16, u16_len,
                                               nullptr, 0, &status);
    if (status != U_BUFFER_OVERFLOW_ERROR && U_FAILURE(status)) {
        core_free(u16);
        return (core_string_t){0};
    }
    status = U_ZERO_ERROR;
    UChar *normalized = core_alloc_array(UChar, (size_t)normalized_len + 1);
    unorm2_normalize(profile->normalizer, u16, u16_len, normalized,
                     normalized_len + 1, &status);
    core_free(u16);
    if (U_FAILURE(status)) {
        core_free(normalized);
        return (core_string_t){0};
    }
    int32_t utf8_len = 0;
    status = U_ZERO_ERROR;
    u_strToUTF8(nullptr, 0, &utf8_len, normalized, normalized_len, &status);
    if (status != U_BUFFER_OVERFLOW_ERROR && U_FAILURE(status)) {
        core_free(normalized);
        return (core_string_t){0};
    }
    status = U_ZERO_ERROR;
    char *utf8 = core_alloc_size(1, (size_t)utf8_len + 1);
    u_strToUTF8(utf8, utf8_len + 1, nullptr, normalized, normalized_len,
                &status);
    core_free(normalized);
    if (U_FAILURE(status)) {
        core_free(utf8);
        return (core_string_t){0};
    }
    return (core_string_t){.data = utf8, .u8_bytes = (size_t)utf8_len};
}
#endif

bool
scan_unicode_profile_process(const scan_unicode_profile_t *profile,
                             core_string_t spelling,
                             scan_unicode_identifier_result_t *result)
{
    if (!profile || !result) {
        return false;
    }
    *result = (scan_unicode_identifier_result_t){0};
#ifndef LANTERN_HAVE_ICU
    (void)spelling;
    return false;
#else
    UScriptCode seen = USCRIPT_INVALID_CODE;
    int32_t at = 0;
    while (at < (int32_t)spelling.u8_bytes) {
        int32_t start = at;
        UChar32 cp;
        U8_NEXT(spelling.data, at, (int32_t)spelling.u8_bytes, cp);
        if (cp < 0) {
            result->diagnostic = CORE_STRING_STATIC("invalid UTF-8 in identifier");
            result->diagnostic_start = (size_t)start;
            result->diagnostic_end = (size_t)at;
            return true;
        }
        UErrorCode status = U_ZERO_ERROR;
        UScriptCode script = uscript_getScript(cp, &status);
        if (U_FAILURE(status) || !script_allowed(profile, script)) {
            result->diagnostic = CORE_STRING_STATIC("identifier uses a disallowed script");
            result->diagnostic_start = (size_t)start;
            result->diagnostic_end = (size_t)at;
            return true;
        }
        if (profile->spec.emoji == SCAN_LEX_EMOJI_DIAGNOSE
            && u_hasBinaryProperty(cp, UCHAR_EXTENDED_PICTOGRAPHIC)) {
            result->diagnostic = CORE_STRING_STATIC("emoji is discouraged in identifiers");
            result->diagnostic_start = (size_t)start;
            result->diagnostic_end = (size_t)at;
            return true;
        }
        if (script != USCRIPT_COMMON && script != USCRIPT_INHERITED) {
            if (profile->spec.diagnose_mixed_scripts
                && seen != USCRIPT_INVALID_CODE && seen != script) {
                result->diagnostic = CORE_STRING_STATIC("identifier mixes scripts");
                result->diagnostic_start = (size_t)start;
                result->diagnostic_end = (size_t)at;
                return true;
            }
            seen = script;
        }
    }

    if (profile->spoof) {
        UErrorCode status = U_ZERO_ERROR;
        int32_t needed = uspoof_getSkeletonUTF8(
            profile->spoof, 0, spelling.data, (int32_t)spelling.u8_bytes,
            nullptr, 0, &status);
        status = U_ZERO_ERROR;
        char *skeleton = core_alloc_size(1, (size_t)needed + 1);
        uspoof_getSkeletonUTF8(profile->spoof, 0, spelling.data,
                              (int32_t)spelling.u8_bytes, skeleton,
                              needed + 1, &status);
        bool changed = U_SUCCESS(status)
                    && ((size_t)needed != spelling.u8_bytes
                        || memcmp(skeleton, spelling.data, (size_t)needed));
        core_free(skeleton);
        if (changed) {
            result->diagnostic = CORE_STRING_STATIC("identifier has a confusable spelling");
            result->diagnostic_end = spelling.u8_bytes;
            return true;
        }
    }

    result->normalized = normalize(profile, spelling);
    return true;
#endif
}
