#include <stdlib.h>

#include "../core/model/model.h"
#include "tu.h"


typedef struct {
    writ_session_t              *session;
    const writ_import_request_t *request;
    writ_tu_t                    tu;
    writ_map_t                  *record_names;
    writ_map_t                  *record_sources;
    writ_map_t                  *declared;
    writ_emit_t                  types;
    writ_emit_t                  values;
    writ_emit_t                  fns;
} importer_t;

static writ_sym_t
sym_of(importer_t *im, writ_str_t text)
{
    return writ_intern_str(im->session, text);
}

static writ_str_t
spelling(importer_t *im, CXCursor cursor)
{
    return writ_tu_text(im->session, clang_getCursorSpelling(cursor));
}

static writ_sym_t
usr(importer_t *im, CXCursor cursor)
{
    return sym_of(im, writ_tu_text(im->session, clang_getCursorUSR(cursor)));
}

static bool
claim(importer_t *im, writ_str_t name)
{
    writ_sym_t key = sym_of(im, name);
    if (!name.length || writ_map_get(im->declared, key, nullptr)) {
        return false;
    }
    writ_map_put(im->declared, key, key);
    return true;
}

static bool
selected(const importer_t *im, writ_str_t name)
{
    for (size_t i = 0; i < im->request->only_count; i++) {
        if (writ_glob_match(im->request->only[i], name)) {
            return true;
        }
    }
    return !im->request->only_count;
}

static void emit_typedef(importer_t *im, CXCursor cursor);
static void emit_record(importer_t *im, CXCursor decl);
static void emit_enum(importer_t *im, CXCursor decl, writ_str_t name);

static const struct {
    const char *c;
    const char *writ;
} typedef_names[] = {
    {"size_t", "usize"},     {"ssize_t", "isize"},    {"ptrdiff_t", "isize"},  {"intptr_t", "isize"},
    {"uintptr_t", "uptr"},   {"int8_t", "i8"},        {"int16_t", "i16"},      {"int32_t", "i32"},
    {"int64_t", "i64"},      {"uint8_t", "u8"},       {"uint16_t", "u16"},     {"uint32_t", "u32"},
    {"uint64_t", "u64"},     {"va_list", "va_list"},  {"__builtin_va_list", "va_list"},
    {"__darwin_va_list", "va_list"}, {"__gnuc_va_list", "va_list"},
};

static const struct {
    enum CXTypeKind kind;
    const char     *writ;
} builtins[] = {
    {CXType_Void, "void"},          {CXType_Bool, "bool"},           {CXType_Char_S, "c_char"},
    {CXType_Char_U, "c_char"},      {CXType_SChar, "c_schar"},       {CXType_UChar, "c_uchar"},
    {CXType_Short, "c_short"},      {CXType_UShort, "c_ushort"},     {CXType_Int, "c_int"},
    {CXType_UInt, "c_uint"},        {CXType_Long, "c_long"},         {CXType_ULong, "c_ulong"},
    {CXType_LongLong, "c_longlong"}, {CXType_ULongLong, "c_ulonglong"}, {CXType_Float, "f32"},
    {CXType_Double, "f64"},
};

static bool
is_function_pointer(CXType type)
{
    CXType canonical = clang_getCanonicalType(type);
    if (canonical.kind != CXType_Pointer) {
        return false;
    }
    enum CXTypeKind pointee = clang_getPointeeType(canonical).kind;
    return pointee == CXType_FunctionProto || pointee == CXType_FunctionNoProto;
}

static writ_str_t record_name(importer_t *im, CXCursor decl);

static writ_str_t
type_text(importer_t *im, CXType type)
{
    writ_session_t *session = im->session;
    switch (type.kind) {
    case CXType_Typedef: {
        writ_str_t name = writ_tu_text(session, clang_getTypedefName(type));
        for (size_t i = 0; i < sizeof(typedef_names) / sizeof(typedef_names[0]); i++) {
            if (writ_str_eq(name, writ_cstr(typedef_names[i].c))) {
                return writ_cstr(typedef_names[i].writ);
            }
        }
        CXCursor decl = clang_getTypeDeclaration(type);
        if (writ_tu_in_headers(&im->tu, decl)) {
            emit_typedef(im, decl);
            CXType underlying = clang_getTypedefDeclUnderlyingType(decl);
            CXType named = underlying.kind == CXType_Elaborated ? clang_Type_getNamedType(underlying) : underlying;
            if (named.kind == CXType_Record) {
                return record_name(im, clang_getTypeDeclaration(named));
            }
            return name;
        }
        return type_text(im, clang_getTypedefDeclUnderlyingType(decl));
    }
    case CXType_Elaborated:
        return type_text(im, clang_Type_getNamedType(type));
    case CXType_Attributed:
        return type_text(im, clang_Type_getModifiedType(type));
    case CXType_Record: {
        CXCursor decl = clang_getTypeDeclaration(type);
        writ_str_t name = record_name(im, decl);
        CXCursor definition = clang_getCursorDefinition(decl);
        if (!name.length) {
            return name;
        }
        if (!clang_isInvalid(clang_getCursorKind(definition))) {
            emit_record(im, definition);
        } else if (claim(im, name)) {
            writ_emit_line(&im->types, "opaque %.*s source \"struct %.*s\"", S(name), S(spelling(im, decl)));
        }
        return name;
    }
    case CXType_Enum: {
        CXCursor decl = clang_getTypeDeclaration(type);
        writ_str_t name = spelling(im, decl);
        if (writ_tu_in_headers(&im->tu, decl) && name.length && !clang_Cursor_isAnonymous(decl)) {
            emit_enum(im, decl, name);
            return name;
        }
        return type_text(im, clang_getEnumDeclIntegerType(decl));
    }
    case CXType_Pointer: {
        CXType pointee = clang_getPointeeType(type);
        enum CXTypeKind kind = clang_getCanonicalType(pointee).kind;
        if (kind == CXType_FunctionProto || kind == CXType_FunctionNoProto) {
            return writ_cstr("?void");
        }
        writ_str_t inner = type_text(im, pointee);
        if (!inner.length) {
            return inner;
        }
        bool nonnull = clang_Type_getNullability(type) == CXTypeNullability_NonNull;
        return writ_str_format(session, "%s%.*s", nonnull ? "*" : "?", S(inner));
    }
    case CXType_ConstantArray: {
        writ_str_t element = type_text(im, clang_getArrayElementType(type));
        return element.length ? writ_str_format(session, "[%.*s; %lld]", S(element), clang_getArraySize(type))
                              : element;
    }
    case CXType_IncompleteArray: {
        writ_str_t element = type_text(im, clang_getArrayElementType(type));
        return element.length ? writ_str_format(session, "?%.*s", S(element)) : element;
    }
    default:
        for (size_t i = 0; i < sizeof(builtins) / sizeof(builtins[0]); i++) {
            if (builtins[i].kind == type.kind) {
                return writ_cstr(builtins[i].writ);
            }
        }
        return writ_cstr("");
    }
}

static writ_str_t
param_type(importer_t *im, CXType type)
{
    CXType canonical = clang_getCanonicalType(type);
    if (canonical.kind == CXType_ConstantArray || canonical.kind == CXType_IncompleteArray) {
        writ_str_t element = type_text(im, clang_getArrayElementType(canonical));
        return element.length ? writ_str_format(im->session, "?%.*s", S(element)) : element;
    }
    return type_text(im, type);
}

static bool
is_va_list(CXType type)
{
    while (type.kind == CXType_Typedef || type.kind == CXType_Elaborated) {
        CXString spelled = clang_getTypedefName(type);
        const char *name = clang_getCString(spelled);
        bool found = name && strstr(name, "va_list");
        clang_disposeString(spelled);
        if (found) {
            return true;
        }
        type = type.kind == CXType_Elaborated ? clang_Type_getNamedType(type)
                                              : clang_getTypedefDeclUnderlyingType(clang_getTypeDeclaration(type));
    }
    return false;
}

static const char *
mode_for(CXType type)
{
    CXType canonical = clang_getCanonicalType(type);
    if (canonical.kind != CXType_Pointer || is_function_pointer(type) || is_va_list(type)) {
        return "";
    }
    return clang_isConstQualifiedType(clang_getPointeeType(canonical)) ? "" : "unknown ";
}

static writ_str_t
record_name(importer_t *im, CXCursor decl)
{
    uint64_t name = 0;
    if (writ_map_get(im->record_names, usr(im, decl), &name)) {
        return writ_sym_str(im->session, (writ_sym_t)name);
    }
    return clang_Cursor_isAnonymous(decl) ? writ_cstr("") : spelling(im, decl);
}

static writ_str_t
param_list(importer_t *im, CXType fn_type, CXCursor fn)
{
    writ_emit_t emit;
    writ_emit_init(&emit);
    int count = clang_getNumArgTypes(fn_type);
    for (int i = 0; i < count; i++) {
        CXType type = clang_getArgType(fn_type, (unsigned)i);
        writ_str_t text = param_type(im, type);
        if (!text.length) {
            writ_emit_discard(&emit);
            return writ_cstr("");
        }
        writ_str_t name = clang_Cursor_isNull(fn) ? writ_cstr("") : spelling(im, clang_Cursor_getArgument(fn, (unsigned)i));
        if (!name.length) {
            name = writ_str_format(im->session, "p%d", i);
        }
        writ_emit_text(&emit, "%s%.*s: %s%.*s", i ? ", " : "", S(name), clang_Cursor_isNull(fn) ? "" : mode_for(type),
                       S(text));
    }
    if (clang_isFunctionTypeVariadic(fn_type)) {
        writ_emit_text(&emit, "%s...", count ? ", " : "");
    }
    writ_str_t text = writ_emit_finish(&emit, im->session);
    return text.length ? text : writ_cstr(" ");
}

static writ_str_t
result_text(importer_t *im, CXType fn_type)
{
    CXType result = clang_getResultType(fn_type);
    if (clang_getCanonicalType(result).kind == CXType_Void) {
        return writ_cstr("");
    }
    writ_str_t text = type_text(im, result);
    if (!text.length) {
        return writ_cstr("?");
    }
    bool pointer = clang_getCanonicalType(result).kind == CXType_Pointer;
    return writ_str_format(im->session, " -> %.*s%s", S(text), pointer ? " unknown" : "");
}

static void
emit_function(importer_t *im, CXCursor cursor)
{
    writ_str_t name = spelling(im, cursor);
    CXType type = clang_getCursorType(cursor);
    writ_str_t params = param_list(im, type, cursor);
    writ_str_t result = result_text(im, type);
    if (!params.length || writ_str_eq(result, writ_cstr("?")) || !claim(im, name)) {
        return;
    }
    if (writ_str_eq(params, writ_cstr(" "))) {
        params = writ_cstr("");
    }
    writ_emit_line(&im->fns, "fn %.*s(%.*s)%.*s", S(name), S(params), S(result));
}

static void
emit_callback(importer_t *im, writ_str_t name, CXType pointer)
{
    if (!claim(im, name)) {
        return;
    }
    CXType fn = clang_getPointeeType(clang_getCanonicalType(pointer));
    writ_str_t params = param_list(im, fn, clang_getNullCursor());
    writ_str_t result = result_text(im, fn);
    if (!params.length || writ_str_eq(result, writ_cstr("?"))) {
        return;
    }
    if (writ_str_eq(params, writ_cstr(" "))) {
        params = writ_cstr("");
    }
    writ_emit_line(&im->types, "callback %.*s = fn(%.*s)%.*s", S(name), S(params), S(result));
}

typedef struct {
    importer_t *im;
    writ_emit_t fields;
    bool        ok;
    CXType      record;
    writ_str_t  name;
    writ_str_t  spelled;
} record_visit_t;

static void
name_anonymous_union(record_visit_t *visit, CXType type, writ_str_t field)
{
    importer_t *im = visit->im;
    CXType named = type.kind == CXType_Elaborated ? clang_Type_getNamedType(type) : type;
    CXCursor decl = clang_getTypeDeclaration(named);
    if (named.kind != CXType_Record || clang_getCursorKind(decl) != CXCursor_UnionDecl
        || !clang_Cursor_isAnonymous(decl) || !field.length) {
        return;
    }
    writ_sym_t key = usr(im, decl);
    writ_map_put(im->record_names, key, sym_of(im, writ_str_format(im->session, "%.*s_%.*s", S(visit->name), S(field))));
    writ_map_put(im->record_sources, key, sym_of(im, writ_str_format(im->session, "__typeof__(((%.*s *)0)->%.*s)",
                                                                      S(visit->spelled), S(field))));
}

static enum CXChildVisitResult
visit_field(CXCursor cursor, CXCursor parent, CXClientData data)
{
    (void)parent;
    record_visit_t *visit = data;
    if (clang_getCursorKind(cursor) != CXCursor_FieldDecl) {
        return CXChildVisit_Continue;
    }
    writ_str_t name = spelling(visit->im, cursor);
    name_anonymous_union(visit, clang_getCursorType(cursor), name);
    writ_str_t type = type_text(visit->im, clang_getCursorType(cursor));
    if (clang_Cursor_isBitField(cursor) || !name.length || !type.length) {
        visit->ok = false;
        return CXChildVisit_Break;
    }
    long long offset = clang_Type_getOffsetOf(visit->record, writ_str_copy(visit->im->session, name.data, name.length).data);
    writ_emit_line(&visit->fields, "    %.*s: %.*s @%lld", S(name), S(type), offset / 8);
    return CXChildVisit_Continue;
}

static void
emit_record(importer_t *im, CXCursor decl)
{
    writ_str_t name = record_name(im, decl);
    if (!name.length || !claim(im, name)) {
        return;
    }
    const char *keyword = clang_getCursorKind(decl) == CXCursor_UnionDecl ? "union" : "struct";
    uint64_t sourced = 0;
    bool typedefed = writ_map_get(im->record_names, usr(im, decl), nullptr);
    writ_str_t spelled = writ_map_get(im->record_sources, usr(im, decl), &sourced)
                           ? writ_sym_str(im->session, (writ_sym_t)sourced)
                       : typedefed ? name
                                   : writ_str_format(im->session, "%s %.*s", keyword, S(spelling(im, decl)));
    writ_str_t source = writ_str_eq(spelled, name) ? writ_cstr("")
                                                   : writ_str_format(im->session, " source \"%.*s\"", S(spelled));
    CXType type = clang_getCursorType(decl);
    long long size = clang_Type_getSizeOf(type);
    record_visit_t visit = {.im = im, .ok = size > 0, .record = type, .name = name, .spelled = spelled};
    writ_emit_init(&visit.fields);
    if (visit.ok) {
        clang_visitChildren(decl, visit_field, &visit);
    }
    writ_str_t fields = writ_emit_finish(&visit.fields, im->session);
    if (!visit.ok) {
        writ_emit_line(&im->types, "opaque %.*s%.*s", S(name), S(source));
        return;
    }
    writ_emit_line(&im->types, "%s %.*s%.*s @size(%lld) @align(%lld) {", keyword, S(name), S(source), size,
                   clang_Type_getAlignOf(type));
    writ_emit_text(&im->types, "%.*s", S(fields));
    writ_emit_line(&im->types, "}");
}

static enum CXChildVisitResult
visit_enum_item(CXCursor cursor, CXCursor parent, CXClientData data)
{
    (void)parent;
    void **context = data;
    importer_t *im = context[0];
    writ_emit_t *emit = context[1];
    bool *anonymous = context[2];
    writ_str_t name = spelling(im, cursor);
    long long value = clang_getEnumConstantDeclValue(cursor);
    if (*anonymous) {
        if (claim(im, name)) {
            writ_emit_line(&im->values, "const %.*s: c_int = %lld", S(name), value);
        }
    } else if (claim(im, name)) {
        writ_emit_line(emit, "    %.*s = %lld,", S(name), value);
    }
    return CXChildVisit_Continue;
}

static void
emit_enum(importer_t *im, CXCursor decl, writ_str_t name)
{
    bool anonymous = !name.length;
    if (!anonymous && !claim(im, name)) {
        return;
    }
    writ_emit_t items;
    writ_emit_init(&items);
    void *context[] = {im, &items, &anonymous};
    clang_visitChildren(decl, visit_enum_item, context);
    writ_str_t body = writ_emit_finish(&items, im->session);
    if (anonymous) {
        return;
    }
    writ_str_t base = type_text(im, clang_getEnumDeclIntegerType(decl));
    writ_emit_line(&im->types, "enum %.*s : %.*s {", S(name), S(base));
    writ_emit_text(&im->types, "%.*s", S(body));
    writ_emit_line(&im->types, "}");
}

static void
emit_typedef(importer_t *im, CXCursor cursor)
{
    writ_str_t name = spelling(im, cursor);
    CXType underlying = clang_getTypedefDeclUnderlyingType(cursor);
    CXType named = underlying.kind == CXType_Elaborated ? clang_Type_getNamedType(underlying) : underlying;
    if (named.kind == CXType_Record || named.kind == CXType_Enum) {
        CXCursor decl = clang_getTypeDeclaration(named);
        if (named.kind == CXType_Enum) {
            emit_enum(im, decl, name);
        } else if (writ_str_eq(record_name(im, decl), name)) {
            type_text(im, named);
        }
        return;
    }
    if (is_function_pointer(underlying)) {
        emit_callback(im, name, underlying);
        return;
    }
    for (size_t i = 0; i < sizeof(typedef_names) / sizeof(typedef_names[0]); i++) {
        if (writ_str_eq(name, writ_cstr(typedef_names[i].c))) {
            return;
        }
    }
    if (!claim(im, name)) {
        return;
    }
    writ_str_t text = type_text(im, underlying);
    if (text.length) {
        writ_emit_line(&im->types, "type %.*s = %.*s", S(name), S(text));
    }
}

typedef struct {
    writ_str_t *texts;
    CXTokenKind *kinds;
    unsigned    count;
} tokens_view_t;

static tokens_view_t
macro_tokens(importer_t *im, CXCursor cursor)
{
    CXToken *raw = nullptr;
    unsigned count = 0;
    clang_tokenize(im->tu.unit, clang_getCursorExtent(cursor), &raw, &count);
    tokens_view_t tokens = {writ_alloc(im->session, sizeof(writ_str_t) * (count + 1)),
                       writ_alloc(im->session, sizeof(CXTokenKind) * (count + 1)), count};
    for (unsigned i = 0; i < count; i++) {
        tokens.texts[i] = writ_tu_text(im->session, clang_getTokenSpelling(im->tu.unit, raw[i]));
        tokens.kinds[i] = clang_getTokenKind(raw[i]);
    }
    clang_disposeTokens(im->tu.unit, raw, count);
    return tokens;
}

static bool
integer_literal(writ_str_t text, long long *value, bool *is_unsigned)
{
    char digits[64];
    size_t length = 0;
    *is_unsigned = false;
    for (size_t i = 0; i < text.length && length + 1 < sizeof(digits); i++) {
        char c = text.data[i];
        if (c == 'u' || c == 'U') {
            *is_unsigned = true;
        } else if (c != 'l' && c != 'L') {
            digits[length++] = c;
        }
    }
    digits[length] = 0;
    char *end = nullptr;
    *value = (long long)strtoull(digits, &end, 0);
    return length && end && !*end;
}

static void
emit_object_macro(importer_t *im, writ_str_t name, const tokens_view_t *tokens)
{
    unsigned first = 1;
    unsigned last = tokens->count;
    while (last > first + 1 && writ_str_eq(tokens->texts[first], writ_cstr("("))
           && writ_str_eq(tokens->texts[last - 1], writ_cstr(")"))) {
        first++;
        last--;
    }
    bool negative = last > first && writ_str_eq(tokens->texts[first], writ_cstr("-"));
    first += negative;
    if (last != first + 1) {
        return;
    }
    writ_str_t token = tokens->texts[first];
    long long value = 0;
    bool is_unsigned = false;
    if (tokens->kinds[first] == CXToken_Literal && token.length > 1 && token.data[0] == '"' && !negative) {
        if (claim(im, name)) {
            writ_emit_line(&im->values, "const %.*s: *cstr = %.*s", S(name), S(token));
        }
        return;
    }
    if (tokens->kinds[first] != CXToken_Literal || !integer_literal(token, &value, &is_unsigned) || !claim(im, name)) {
        return;
    }
    value = negative ? -value : value;
    const char *type = value >= INT32_MIN && value <= INT32_MAX ? (is_unsigned ? "c_uint" : "c_int")
                     : is_unsigned                              ? "c_ulonglong"
                                                                : "c_longlong";
    writ_emit_line(&im->values, "const %.*s: %s = %lld", S(name), type, value);
}

static unsigned
skip_group(const tokens_view_t *tokens, unsigned at)
{
    int depth = 0;
    for (unsigned i = at; i < tokens->count; i++) {
        depth += writ_str_eq(tokens->texts[i], writ_cstr("(")) ? 1 : writ_str_eq(tokens->texts[i], writ_cstr(")")) ? -1 : 0;
        if (!depth) {
            return i + 1;
        }
    }
    return tokens->count;
}

static writ_str_t
simple_argument(importer_t *im, const tokens_view_t *tokens, unsigned first, unsigned last)
{
    while (last > first + 1 && writ_str_eq(tokens->texts[first], writ_cstr("(")) && skip_group(tokens, first) == last) {
        first++;
        last--;
    }
    if (last > first + 2 && writ_str_eq(tokens->texts[first], writ_cstr("(")) && skip_group(tokens, first) < last
        && tokens->kinds[first + 1] == CXToken_Keyword) {
        return simple_argument(im, tokens, skip_group(tokens, first), last);
    }
    if (last == first + 1 && (tokens->kinds[first] == CXToken_Identifier || tokens->kinds[first] == CXToken_Literal)) {
        return tokens->texts[first];
    }
    if (last == first + 4 && writ_str_eq(tokens->texts[first], writ_cstr("sizeof"))
        && tokens->kinds[first + 2] == CXToken_Identifier) {
        return writ_str_format(im->session, "sizeof(%.*s)", S(tokens->texts[first + 2]));
    }
    return writ_cstr("");
}

typedef struct {
    importer_t *im;
    writ_sym_t  target;
    CXCursor    found;
} target_search_t;

static enum CXChildVisitResult
find_target(CXCursor cursor, CXCursor parent, CXClientData data)
{
    (void)parent;
    target_search_t *search = data;
    if (clang_getCursorKind(cursor) == CXCursor_FunctionDecl
        && sym_of(search->im, spelling(search->im, cursor)) == search->target) {
        search->found = cursor;
        return CXChildVisit_Break;
    }
    return CXChildVisit_Continue;
}

static void
emit_expanding_macro(importer_t *im, writ_str_t name, const tokens_view_t *tokens)
{
    unsigned params_end = skip_group(tokens, 1);
    unsigned body = params_end;
    if (body + 3 > tokens->count || tokens->kinds[body] != CXToken_Identifier
        || !writ_str_eq(tokens->texts[body + 1], writ_cstr("(")) || skip_group(tokens, body + 1) != tokens->count) {
        return;
    }
    target_search_t search = {im, sym_of(im, tokens->texts[body]), clang_getNullCursor()};
    clang_visitChildren(clang_getTranslationUnitCursor(im->tu.unit), find_target, &search);
    if (clang_Cursor_isNull(search.found)) {
        return;
    }
    CXType target_type = clang_getCursorType(search.found);
    writ_emit_t args;
    writ_emit_init(&args);
    writ_emit_t params;
    writ_emit_init(&params);
    unsigned arg = 0;
    unsigned start = body + 2;
    bool have_params = false;
    for (unsigned i = start; i < tokens->count; i++) {
        bool at_end = i == tokens->count - 1;
        if (!at_end && !writ_str_eq(tokens->texts[i], writ_cstr(","))) {
            if (writ_str_eq(tokens->texts[i], writ_cstr("("))) {
                i = skip_group(tokens, i) - 1;
            }
            continue;
        }
        writ_str_t text = simple_argument(im, tokens, start, i);
        if (!text.length || arg >= (unsigned)clang_getNumArgTypes(target_type)) {
            writ_emit_discard(&args);
            writ_emit_discard(&params);
            return;
        }
        for (unsigned p = 2; p + 1 < params_end; p++) {
            if (writ_str_eq(tokens->texts[p], text)) {
                CXType type = clang_getArgType(target_type, arg);
                writ_emit_text(&params, "%s%.*s: %s%.*s", have_params ? ", " : "", S(text), mode_for(type),
                               S(param_type(im, type)));
                have_params = true;
            }
        }
        writ_emit_text(&args, "%s%.*s", arg ? ", " : "", S(text));
        arg++;
        start = i + 1;
    }
    writ_str_t param_text = writ_emit_finish(&params, im->session);
    writ_str_t arg_text = writ_emit_finish(&args, im->session);
    if (claim(im, name)) {
        writ_emit_line(&im->fns, "fn %.*s(%.*s)%.*s", S(name), S(param_text), S(result_text(im, target_type)));
        writ_emit_line(&im->fns, "  expands %.*s(%.*s)", S(tokens->texts[body]), S(arg_text));
    }
}

static void
emit_macro(importer_t *im, CXCursor cursor, bool function_like)
{
    writ_str_t name = spelling(im, cursor);
    if (clang_Cursor_isMacroBuiltin(cursor) || (name.length && name.data[0] == '_')) {
        return;
    }
    tokens_view_t tokens = macro_tokens(im, cursor);
    if (tokens.count < 2) {
        return;
    }
    if (function_like != clang_Cursor_isMacroFunctionLike(cursor)) {
        return;
    }
    if (function_like) {
        emit_expanding_macro(im, name, &tokens);
    } else {
        emit_object_macro(im, name, &tokens);
    }
}

typedef struct {
    importer_t *im;
    int         pass;
} walk_t;

static enum CXChildVisitResult
walk(CXCursor cursor, CXCursor parent, CXClientData data)
{
    (void)parent;
    walk_t *walk_state = data;
    importer_t *im = walk_state->im;
    if (!writ_tu_in_headers(&im->tu, cursor)) {
        return CXChildVisit_Continue;
    }
    enum CXCursorKind kind = clang_getCursorKind(cursor);
    switch (walk_state->pass) {
    case 0:
        if (kind == CXCursor_TypedefDecl) {
            CXType underlying = clang_getTypedefDeclUnderlyingType(cursor);
            CXType named = underlying.kind == CXType_Elaborated ? clang_Type_getNamedType(underlying) : underlying;
            CXCursor decl = clang_getTypeDeclaration(named);
            if (named.kind == CXType_Record && !writ_map_get(im->record_names, usr(im, decl), nullptr)) {
                writ_map_put(im->record_names, usr(im, decl), sym_of(im, spelling(im, cursor)));
            }
        }
        break;
    case 1: {
        bool eager = !im->request->only_count;
        writ_str_t name = spelling(im, cursor);
        if (kind == CXCursor_TypedefDecl && eager) {
            emit_typedef(im, cursor);
        } else if ((kind == CXCursor_StructDecl || kind == CXCursor_UnionDecl) && clang_isCursorDefinition(cursor)
                   && eager) {
            emit_record(im, cursor);
        } else if (kind == CXCursor_EnumDecl && (eager || clang_Cursor_isAnonymous(cursor))) {
            emit_enum(im, cursor, clang_Cursor_isAnonymous(cursor) ? writ_cstr("") : name);
        } else if (kind == CXCursor_FunctionDecl && selected(im, name)) {
            emit_function(im, cursor);
        } else if (kind == CXCursor_VarDecl && selected(im, name)) {
            writ_str_t type = type_text(im, clang_getCursorType(cursor));
            if (type.length && claim(im, name)) {
                writ_emit_line(&im->values, "var %.*s: %.*s", S(name), S(type));
            }
        } else if (kind == CXCursor_MacroDefinition) {
            emit_macro(im, cursor, false);
        }
        break;
    }
    default:
        if (kind == CXCursor_MacroDefinition && selected(im, spelling(im, cursor))) {
            emit_macro(im, cursor, true);
        }
        break;
    }
    return CXChildVisit_Continue;
}

writ_status_t
writ_clang_import(writ_session_t *session, const writ_platform_t *platform, const writ_import_request_t *request,
                  const writ_output_t *output)
{
    if (!session || !platform || !request || !output || !request->headers_count || !request->library.length) {
        return WRIT_INVALID_ARGUMENT;
    }
    importer_t im = {
        .session      = session,
        .request      = request,
        .record_names   = writ_map_new(session),
        .record_sources = writ_map_new(session),
        .declared       = writ_map_new(session),
    };
    writ_str_t error = {0};
    writ_status_t status = writ_tu_open(session, &im.tu, writ_platform_triple(platform), request, true, &error);
    if (status != WRIT_OK) {
        writ_report(session, WRIT_D_TOOLCHAIN, WRIT_NO_LOC, "%.*s", S(error));
        return status;
    }
    writ_emit_init(&im.types);
    writ_emit_init(&im.values);
    writ_emit_init(&im.fns);
    for (int pass = 0; pass < 3; pass++) {
        walk_t state = {&im, pass};
        clang_visitChildren(clang_getTranslationUnitCursor(im.tu.unit), walk, &state);
    }
    writ_tu_close(&im.tu);
    writ_emit_t out;
    writ_emit_init(&out);
    writ_emit_line(&out, "library %.*s", S(request->library));
    for (size_t i = 0; i < request->links_count; i++) {
        writ_emit_line(&out, "link \"%.*s\"", S(request->links[i]));
    }
    for (size_t i = 0; i < request->headers_count; i++) {
        writ_emit_line(&out, "header <%.*s>", S(request->headers[i]));
    }
    for (size_t i = 0; i < request->defines_count; i++) {
        writ_emit_line(&out, "define %.*s", S(request->defines[i]));
    }
    writ_str_t sections[] = {writ_emit_finish(&im.types, session), writ_emit_finish(&im.values, session),
                             writ_emit_finish(&im.fns, session)};
    for (size_t i = 0; i < 3; i++) {
        if (sections[i].length) {
            writ_emit_line(&out, "");
            writ_emit_text(&out, "%.*s", S(sections[i]));
        }
    }
    return output->write_file(output->context, writ_str_format(session, "%.*s.writ", S(request->library)),
                              writ_emit_finish(&out, session));
}
