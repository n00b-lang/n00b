// Declarative C2Y Grammar Parser
//
// This parser implements the complete C2Y grammar (ISO/IEC 9899:202y N3783 Annex A)
// using a declarative style where production branches directly mirror the grammar.
//
// DECLARATIVE INTERFACE
//
// Production branches use these macros to match grammar elements:
//
//     required_nt(name)       - Match a required non-terminal
//     optional_nt(name)       - Match an optional non-terminal
//     required_op("x")        - Match a required operator/punctuation
//     optional_op("x")        - Match an optional operator
//     required_keyword(list)  - Match a required keyword from a list
//     optional_keyword(list)  - Match an optional keyword
//
// Branch bodies contain NO conditionals - they are pure sequences of these macros.
// All failure handling is encapsulated in the macro implementations.
//
// PRODUCTION STRUCTURE
//
// Grammar productions without recursion are declared with:
//
//     declare_nt(name, N)         - Declare production with N branches
//
// Individual branches of all productions are declared with:
//     nt_branch(name, 0) { ... }  - Define branch 0 of production `name`
//     nt_branch(name, 1) { ... }  - Define branch 1
//
// Branches are tried in order; first match wins. More specific
// branches need to come first, for correct parsing.
//
// RECURSION
//
// Recursive productions need special treatment. The top production
// where recursion can happen needs to maintain a loop, so instead of
// `declare_nt()`, we use:
//
//     declare_recursive(name, N)  - Declare left-recursive production
//
// For direct left-recursion, the left recursion should be noted with:
//     direct_self_call(name)      - Direct left recursion calls (A -> A x)
//
// For indirect left-recursion, it should be noted with:
//     indirect_self_call(name)    - Indirect left recursion calls
//
// SYMBOL TABLE REGISTRATION
//
// After matching, branches may register symbols:
//
//     register_declaration(ctx)           - Register variables, typedefs, tags
//     register_function_definition(ctx)   - Register function and parameters
//     register_label_def(ctx)             - Register label definition
//     register_label_ref(ctx)             - Register label reference (goto)
//
// TERMINALS
//
// Terminal matching functions (identifier, constant, string_literal,
// etc.) contain the token-level conditionals. Branches call these via
// required_nt/optional_nt.
//
// =============================================================================
// COMPILER EXTENSIONS
// =============================================================================
//
// This parser supports several common compiler extensions beyond standard C2Y.
// Extension branches are clearly marked with [EXTENSION] comments. These enable
// parsing real-world code that uses GCC/Clang features found in system headers.
//
// GCC EXTENSIONS:
//
//   __attribute__((...))  - Function/variable attributes
//       attribute_specifier branch 1
//       Parses: __attribute__((aligned(16))), __attribute__((deprecated)), etc.
//
//   __asm__("label")      - Assembly symbol names for linker
//       gcc_asm_label production
//       Parses: int foo __asm__("_foo");
//       Used in init_declarator branches 0 and 1
//
//   ({ ... })             - Statement expressions (compound literals with statements)
//       primary_expression branch 6
//       Parses: int x = ({ int a = 1; a + 1; });
//
//   __extension__         - Suppress warnings for extensions
//       Recognized as keyword, treated as no-op prefix
//
//   Alternative keywords  - GCC spelling variants (see parse_support.c):
//       __inline__, __inline       (function specifiers)
//       __volatile__, __volatile   (type qualifiers)
//       __const__, __const         (type qualifiers)
//       __restrict__, __restrict   (type qualifiers)
//       __signed__, __signed       (type specifiers)
//       __typeof__, __typeof       (typeof operators)
//       __asm__, __asm, asm        (assembly)
//       __builtin_va_list          (variadic arguments type)
//
// CLANG EXTENSIONS:
//
//   Nullability specifiers - Pointer nullability annotations
//       Recognized in kw_type_qualifier:
//       _Nullable, _Nonnull, _Null_unspecified
//       __nullable, __nonnull, __null_unspecified
//
// COMMON EXTENSIONS (GCC + Clang):
//
//   Trailing comma in enums - enum { A, B, C, } is accepted
//       enum_specifier branch 1
//       While technically C99+, explicitly handled for clarity
//
// NCC EXTENSIONS:
//
//   1. The 'typeid' transformation, to produce a unique C identifier
//      token for any given type. This does light normalization,
//      mainly just keyword ordering. Specifically, 'int' does NOT
//      normalize to int32_t, even though `int` is generally 32
//      bits. Nor do typedef names normalize to what they alias (I may
//      change this).
//
//   2. The 'once' function specifier, to support functions that only
//      execute a single time, even if multiple calls occur in
//      parallel. If the function has a return type, subsequent calls
//      return the cached value.
//
//   3. '_kargs' blocks after function signatures.
//
//   4. A postfix ! operator, which either retrieves the contents of a
//      Result type, or propogates by returning. This is just like the
//      Rust ? operator, but ? would be ambiguous in C given the
//      ternary operator, where ! is not. Both symbols seem
//      appropriate for the operation.
//
// Extension keywords are registered in src/lex.c (is_keyword
// function) and keyword lists are defined in src/parse_support.c
// (kw_* string lists).
//

#include "parse_internal.h"

declare_nt(identifier, 2);
declare_nt(synthetic_identifier, 2);
declare_nt(typeid_atom, 2);
declare_nt(typeid_continuation, 2);
declare_nt(synthetic_string_literal, 2);
declare_nt(primary_expression, 10);
declare_nt(generic_selection, 1);
declare_nt(generic_controlling_operand, 2);
declare_recursive(generic_assoc_list, 2);
declare_nt(generic_association, 2);
declare_recursive(postfix_expression, 11);
declare_recursive(argument_expression_list, 4);
declare_nt(keyword_argument, 1);
declare_nt(compound_literal, 1);
declare_recursive(storage_class_specifiers, 2);
declare_nt(unary_expression, 10);
declare_nt(unary_operator, 1);
declare_nt(cast_expression, 2);
declare_recursive(multiplicative_expression, 4);
declare_recursive(additive_expression, 3);
declare_recursive(shift_expression, 3);
declare_recursive(relational_expression, 5);
declare_recursive(equality_expression, 3);
declare_recursive(AND_expression, 2);
declare_recursive(exclusive_OR_expression, 2);
declare_recursive(inclusive_OR_expression, 2);
declare_recursive(logical_AND_expression, 2);
declare_recursive(logical_OR_expression, 2);
declare_nt(conditional_expression, 2);
declare_nt(assignment_expression, 2);
declare_nt(assignment_operator, 1);
declare_recursive(expression, 2);
declare_nt(constant_expression, 1);
declare_nt(constant_range_expression, 1);
declare_nt(type_name, 1);
declare_recursive(specifier_qualifier_list, 2);
declare_nt(abstract_declarator, 2);
declare_nt(type_specifier_qualifier, 3);
declare_nt(attribute_specifier_sequence, 1);
declare_nt(pointer, 4);
declare_recursive(direct_abstract_declarator, 3);
declare_nt(type_specifier, 7);
declare_nt(alignment_specifier, 2);
declare_nt(attribute_specifier, 2);
declare_nt(type_qualifier_list, 2);
declare_nt(array_abstract_declarator, 4);
declare_nt(function_abstract_declarator, 1);
declare_nt(atomic_type_specifier, 1);
declare_nt(struct_or_union_specifier, 3);
declare_nt(enum_specifier, 3);
declare_nt(typedef_name, 1);
declare_nt(typeof_specifier, 2);
declare_recursive(attribute_list, 2);
declare_nt(parameter_type_list, 6);
declare_recursive(member_declaration_list, 2);
declare_nt(enum_type_specifier, 1);
declare_recursive(enumerator_list, 2);
declare_nt(typeof_specifier_argument, 2);
declare_nt(attribute, 1);
declare_recursive(parameter_list, 2);
declare_nt(member_declaration, 3);
declare_nt(enumerator, 2);
declare_nt(attribute_token, 2);
declare_nt(attribute_argument_clause, 1);
declare_nt(parameter_declaration, 2);
declare_recursive(member_declarator_list, 2);
declare_nt(static_assert_declaration, 1);
declare_nt(enumeration_constant, 1);
declare_nt(standard_attribute, 1);
declare_nt(attribute_prefixed_token, 1);
declare_nt(attribute_prefix, 1);
declare_recursive(declaration_specifiers, 2);
declare_nt(declarator, 1);
declare_nt(member_declarator, 2);
declare_nt(declaration_specifier, 3);
declare_recursive(direct_declarator, 4);
declare_nt(storage_class_specifier, 1);
declare_nt(function_specifier, 2);
declare_nt(array_declarator, 4);
declare_nt(function_declarator, 1);
declare_nt(designator, 2);
declare_recursive(designator_list, 2);
declare_nt(designation, 1);
declare_nt(braced_initializer, 2);
declare_nt(initializer, 2);
declare_recursive(initializer_list, 2);
declare_nt(init_declarator, 2);
declare_recursive(init_declarator_list, 2);
declare_nt(gcc_asm_label, 1);
declare_nt(gcc_asm_statement, 1);
declare_nt(balanced_token, 4);
declare_recursive(balanced_token_sequence, 2);
declare_nt(expression_statement, 3);
declare_nt(jump_statement, 7);
declare_nt(keyword_clause, 2);
declare_nt(keyword_param, 2);
declare_recursive(keyword_param_list, 2);
declare_nt(label, 4);
declare_nt(labeled_statement, 1);
declare_nt(selection_header, 3);
declare_nt(simple_declaration, 1);
declare_nt(selection_statement, 3);
declare_nt(iteration_statement, 4);
declare_nt(primary_block, 3);
declare_nt(secondary_block, 1);
declare_nt(compound_statement, 1);
declare_nt(block_item, 3);
declare_recursive(block_item_list, 2);
declare_nt(unlabeled_statement, 4);
declare_nt(statement, 2);
declare_nt(declaration, 7);
declare_nt(attribute_declaration, 1);
declare_nt(function_body, 1);
declare_nt(function_definition, 3);
declare_nt(external_declaration, 4);
declare_recursive(translation_unit, 2);

nt_branch(identifier, 0)
{
    start_nt();
    required_nt(synthetic_identifier);
    end_nt();
}

nt_branch(identifier, 1)
{
    start_nt();
    required_nt(provided_identifier);
    end_nt();
}

// [EXTENSION: NCC] typeid(x) - produces identifier containing encoded type ID
// Takes same arguments as typeof: type name or expression
// Example: int x = typeid(int *); -> int x = __abcd1234...;
// Note: typeid is parsed as an identifier (not keyword) to allow it as a variable name
nt_branch(synthetic_identifier, 0)
{
    start_nt();
    required_named_identifier(kw_typeid);
    required_op("(");
    required_nt(typeid_atom);
    optional_nt(typeid_continuation);
    required_op(")");
    end_nt();
}

// [EXTENSION: NCC] constexpr_paste("prefix", expr) - produces identifier
// by concatenating a string prefix with an integer expression result.
// Example: constexpr_paste("item_", 3) -> item_3
nt_branch(synthetic_identifier, 1)
{
    start_nt();
    required_named_identifier(kw_constexpr_paste);
    required_op("(");
    required_nt(string_literal);
    required_op(",");
    required_nt(assignment_expression);
    required_op(")");
    end_nt();
}

nt_branch(typeid_atom, 0)
{
    start_nt();
    required_nt(string_literal);
    end_nt();
}

nt_branch(typeid_atom, 1)
{
    start_nt();
    required_nt(typeof_specifier_argument);
    end_nt();
}

nt_branch(typeid_continuation, 0)
{
    start_nt();
    required_op(",");
    required_nt(typeid_atom);
    optional_nt(typeid_continuation);
    end_nt();
}

nt_branch(typeid_continuation, 1)
{
    start_nt();
    required_op(",");
    end_nt();
}

// [EXTENSION: NCC] typestr(x) - produces string literal containing encoded type ID
nt_branch(synthetic_string_literal, 0)
{
    start_nt();
    required_named_identifier(kw_typestr);
    required_op("(");
    required_nt(typeid_atom);
    optional_nt(typeid_continuation);
    required_op(")");
    end_nt();
}

nt_branch(synthetic_string_literal, 1)
{
    start_nt();
    required_nt(string_literal);
    end_nt();
}

nt_branch(primary_expression, 0)
{
    start_nt();
    required_nt(constant);
    end_nt();
}

nt_branch(primary_expression, 1)
{
    start_nt();
    required_nt(synthetic_string_literal);
    end_nt();
}

nt_branch(primary_expression, 2)
{
    start_nt();
    required_op("(");
    required_nt(expression);
    required_op(")");
    end_nt();
}

nt_branch(primary_expression, 3)
{
    start_nt();
    required_nt(generic_selection);
    end_nt();
}

nt_branch(primary_expression, 4)
{
    start_nt();
    required_nt(static_assert_declaration);
    end_nt();
}

// [EXTENSION: GCC] Statement expression: ({ statements; expr; })
// Allows compound statements as expressions, evaluating to last expression value.
// Example: int x = ({ int a = 1; int b = 2; a + b; });
nt_branch(primary_expression, 5)
{
    start_nt();
    required_op("(");
    required_nt(compound_statement);
    required_op(")");
    end_nt();
}

// Plain identifier - must come after typeid branch (0) so typeid(...) matches first
nt_branch(primary_expression, 6)
{
    start_nt();
    required_nt(identifier);
    end_nt();
}

// [EXTENSION: GCC] __builtin_va_arg(ap, type) - extract value from va_list
// Takes an expression and a type name as arguments.
nt_branch(primary_expression, 7)
{
    start_nt();
    required_keyword(kw_builtin_va_arg);
    required_op("(");
    required_nt(assignment_expression);
    required_op(",");
    required_nt(type_name);
    required_op(")");
    end_nt();
}

// [EXTENSION: GCC] __builtin_types_compatible_p(type1, type2) - check type compatibility
// Takes two type names as arguments, returns 1 if compatible, 0 otherwise.
nt_branch(primary_expression, 8)
{
    start_nt();
    required_keyword(kw_builtin_types_compatible_p);
    required_op("(");
    required_nt(type_name);
    required_op(",");
    required_nt(type_name);
    required_op(")");
    end_nt();
}

// [EXTENSION: GCC] __extension__ expr - suppress warnings for GCC extensions
// Just passes through to the following expression.
nt_branch(primary_expression, 9)
{
    start_nt();
    required_keyword(kw_gcc_extension);
    required_nt(primary_expression);
    end_nt();
}

nt_branch(generic_selection, 0)
{
    start_nt();
    required_keyword(kw_generic);
    required_op("(");
    required_nt(generic_controlling_operand);
    required_op(",");
    required_nt(generic_assoc_list);
    required_op(")");
    end_nt();
}

nt_branch(generic_controlling_operand, 0)
{
    start_nt();
    required_nt(type_name);
    end_nt();
}

nt_branch(generic_controlling_operand, 1)
{
    start_nt();
    required_nt(assignment_expression);
    end_nt();
}

nt_branch(generic_assoc_list, 0)
{
    start_nt();
    required_nt(generic_association);
    end_nt();
}

nt_branch(generic_assoc_list, 1)
{
    start_nt();
    direct_self_call(generic_assoc_list);
    required_op(",");
    required_nt(generic_association);
    end_nt();
}

nt_branch(generic_association, 0)
{
    start_nt();
    required_keyword(kw_default);
    required_op(":");
    required_nt(assignment_expression);
    end_nt();
}

nt_branch(generic_association, 1)
{
    start_nt();
    required_nt(type_name);
    required_op(":");
    required_nt(assignment_expression);
    end_nt();
}

nt_branch(postfix_expression, 0)
{
    start_nt();
    direct_self_call(postfix_expression);
    required_op("[");
    required_nt(expression);
    required_op("]");
    end_nt();
}

nt_branch(postfix_expression, 1)
{
    start_nt();
    direct_self_call(postfix_expression);
    required_op("(");
    optional_nt(argument_expression_list);
    required_op(")");
    end_nt();
}

nt_branch(postfix_expression, 2)
{
    start_nt();
    direct_self_call(postfix_expression);
    required_op(".");
    required_nt(identifier);
    end_nt();
}

nt_branch(postfix_expression, 3)
{
    start_nt();
    direct_self_call(postfix_expression);
    required_op("->");
    required_nt(identifier);
    end_nt();
}

nt_branch(postfix_expression, 4)
{
    start_nt();
    direct_self_call(postfix_expression);
    required_op("++");
    end_nt();
}

nt_branch(postfix_expression, 5)
{
    start_nt();
    direct_self_call(postfix_expression);
    required_op("--");
    end_nt();
}

nt_branch(postfix_expression, 6)
{
    start_nt();
    base_case_only();
    required_nt(compound_literal);
    end_nt();
}

nt_branch(postfix_expression, 7)
{
    start_nt();
    base_case_only();
    required_op("(");
    required_nt(type_name);
    required_op(")");
    required_nt(braced_initializer);
    end_nt();
}

nt_branch(postfix_expression, 8)
{
    start_nt();
    base_case_only();
    required_op("(");
    required_nt(storage_class_specifiers);
    required_nt(type_name);
    required_op(")");
    required_nt(braced_initializer);
    end_nt();
}

nt_branch(postfix_expression, 9)
{
    start_nt();
    base_case_only();
    required_nt(primary_expression);
    end_nt();
}

// postfix_expression '!' for error propagation (like Rust's `?` operator).
// Transformed by xform_bang.c into a statement expression that checks .is_ok
// and either yields .ok or early-returns with the error.
nt_branch(postfix_expression, 10)
{
    start_nt();
    direct_self_call(postfix_expression);
    required_op("!");
    end_nt();
}

// keyword_argument: '.' identifier '=' assignment_expression
// Used for call sites like: foo(1, .verbose = true)
nt_branch(keyword_argument, 0)
{
    start_nt();
    required_op(".");
    required_nt(identifier);
    required_op("=");
    required_nt(assignment_expression);
    end_nt();
}

nt_branch(argument_expression_list, 0)
{
    start_nt();
    required_nt(keyword_argument);
    end_nt();
}

nt_branch(argument_expression_list, 1)
{
    start_nt();
    required_nt(assignment_expression);
    end_nt();
}

nt_branch(argument_expression_list, 2)
{
    start_nt();
    direct_self_call(argument_expression_list);
    required_op(",");
    required_nt(keyword_argument);
    end_nt();
}

nt_branch(argument_expression_list, 3)
{
    start_nt();
    direct_self_call(argument_expression_list);
    required_op(",");
    required_nt(assignment_expression);
    end_nt();
}

nt_branch(compound_literal, 0)
{
    start_nt();
    required_op("(");
    optional_nt(storage_class_specifiers);
    required_nt(type_name);
    required_op(")");
    required_nt(braced_initializer);
    end_nt();
}

nt_branch(storage_class_specifiers, 0)
{
    start_nt();
    direct_self_call(storage_class_specifiers);
    required_nt(storage_class_specifier);
    end_nt();
}

nt_branch(storage_class_specifiers, 1)
{
    start_nt();
    required_nt(storage_class_specifier);
    end_nt();
}

nt_branch(unary_expression, 0)
{
    start_nt();
    required_op("++");
    required_nt(unary_expression);
    end_nt();
}

nt_branch(unary_expression, 1)
{
    start_nt();
    required_op("--");
    required_nt(unary_expression);
    end_nt();
}

nt_branch(unary_expression, 2)
{
    start_nt();
    required_nt(unary_operator);
    required_nt(cast_expression);
    end_nt();
}

nt_branch(unary_expression, 3)
{
    start_nt();
    required_keyword(kw_sizeof);
    required_op("(");
    required_nt(type_name);
    required_op(")");
    end_nt();
}

nt_branch(unary_expression, 4)
{
    start_nt();
    required_keyword(kw_sizeof);
    required_nt(unary_expression);
    end_nt();
}

nt_branch(unary_expression, 5)
{
    start_nt();
    required_keyword(kw_alignof);
    required_op("(");
    required_nt(type_name);
    required_op(")");
    end_nt();
}

nt_branch(unary_expression, 6)
{
    start_nt();
    required_keyword(kw_countof);
    required_op("(");
    required_nt(type_name);
    required_op(")");
    end_nt();
}

nt_branch(unary_expression, 7)
{
    start_nt();
    required_keyword(kw_countof);
    required_nt(unary_expression);
    end_nt();
}

// [EXTENSION: GCC] Address of label - computed goto
// Syntax: &&label (label as value)
nt_branch(unary_expression, 8)
{
    start_nt();
    required_op("&&");
    required_nt(identifier);
    end_nt();
}

nt_branch(unary_expression, 9)
{
    start_nt();
    required_nt(postfix_expression);
    end_nt();
}

nt_branch(unary_operator, 0)
{
    start_nt();
    required_op_list(op_unary);
    end_nt();
}

nt_branch(cast_expression, 0)
{
    start_nt();
    required_op("(");
    required_nt(type_name);
    required_op(")");
    required_nt(cast_expression);
    end_nt();
}

nt_branch(cast_expression, 1)
{
    start_nt();
    required_nt(unary_expression);
    end_nt();
}

nt_branch(multiplicative_expression, 0)
{
    start_nt();
    direct_self_call(multiplicative_expression);
    required_op("*");
    required_nt(cast_expression);
    end_nt();
}

nt_branch(multiplicative_expression, 1)
{
    start_nt();
    direct_self_call(multiplicative_expression);
    required_op("/");
    required_nt(cast_expression);
    end_nt();
}

nt_branch(multiplicative_expression, 2)
{
    start_nt();
    direct_self_call(multiplicative_expression);
    required_op("%");
    required_nt(cast_expression);
    end_nt();
}

nt_branch(multiplicative_expression, 3)
{
    start_nt();
    base_case_only();
    required_nt(cast_expression);
    end_nt();
}

nt_branch(additive_expression, 0)
{
    start_nt();
    direct_self_call(additive_expression);
    required_op("+");
    required_nt(multiplicative_expression);
    end_nt();
}

nt_branch(additive_expression, 1)
{
    start_nt();
    direct_self_call(additive_expression);
    required_op("-");
    required_nt(multiplicative_expression);
    end_nt();
}

nt_branch(additive_expression, 2)
{
    start_nt();
    base_case_only();
    required_nt(multiplicative_expression);
    end_nt();
}

nt_branch(shift_expression, 0)
{
    start_nt();
    direct_self_call(shift_expression);
    required_op("<<");
    required_nt(additive_expression);
    end_nt();
}

nt_branch(shift_expression, 1)
{
    start_nt();
    direct_self_call(shift_expression);
    required_op(">>");
    required_nt(additive_expression);
    end_nt();
}

nt_branch(shift_expression, 2)
{
    start_nt();
    base_case_only();
    required_nt(additive_expression);
    end_nt();
}

nt_branch(relational_expression, 0)
{
    start_nt();
    direct_self_call(relational_expression);
    required_op("<");
    required_nt(shift_expression);
    end_nt();
}

nt_branch(relational_expression, 1)
{
    start_nt();
    direct_self_call(relational_expression);
    required_op(">");
    required_nt(shift_expression);
    end_nt();
}

nt_branch(relational_expression, 2)
{
    start_nt();
    direct_self_call(relational_expression);
    required_op("<=");
    required_nt(shift_expression);
    end_nt();
}

nt_branch(relational_expression, 3)
{
    start_nt();
    direct_self_call(relational_expression);
    required_op(">=");
    required_nt(shift_expression);
    end_nt();
}

nt_branch(relational_expression, 4)
{
    start_nt();
    base_case_only();
    required_nt(shift_expression);
    end_nt();
}

nt_branch(equality_expression, 0)
{
    start_nt();
    direct_self_call(equality_expression);
    required_op("==");
    required_nt(relational_expression);
    end_nt();
}

nt_branch(equality_expression, 1)
{
    start_nt();
    direct_self_call(equality_expression);
    required_op("!=");
    required_nt(relational_expression);
    end_nt();
}

nt_branch(equality_expression, 2)
{
    start_nt();
    base_case_only();
    required_nt(relational_expression);
    end_nt();
}

nt_branch(AND_expression, 0)
{
    start_nt();
    direct_self_call(AND_expression);
    required_op("&");
    required_nt(equality_expression);
    end_nt();
}

nt_branch(AND_expression, 1)
{
    start_nt();
    base_case_only();
    required_nt(equality_expression);
    end_nt();
}

nt_branch(exclusive_OR_expression, 0)
{
    start_nt();
    direct_self_call(exclusive_OR_expression);
    required_op("^");
    required_nt(AND_expression);
    end_nt();
}

nt_branch(exclusive_OR_expression, 1)
{
    start_nt();
    base_case_only();
    required_nt(AND_expression);
    end_nt();
}

nt_branch(inclusive_OR_expression, 0)
{
    start_nt();
    direct_self_call(inclusive_OR_expression);
    required_op("|");
    required_nt(exclusive_OR_expression);
    end_nt();
}

nt_branch(inclusive_OR_expression, 1)
{
    start_nt();
    base_case_only();
    required_nt(exclusive_OR_expression);
    end_nt();
}

nt_branch(logical_AND_expression, 0)
{
    start_nt();
    direct_self_call(logical_AND_expression);
    required_op("&&");
    required_nt(inclusive_OR_expression);
    end_nt();
}

nt_branch(logical_AND_expression, 1)
{
    start_nt();
    base_case_only();
    required_nt(inclusive_OR_expression);
    end_nt();
}

nt_branch(logical_OR_expression, 0)
{
    start_nt();
    direct_self_call(logical_OR_expression);
    required_op("||");
    required_nt(logical_AND_expression);
    end_nt();
}

nt_branch(logical_OR_expression, 1)
{
    start_nt();
    base_case_only();
    required_nt(logical_AND_expression);
    end_nt();
}

nt_branch(conditional_expression, 0)
{
    start_nt();
    required_nt(logical_OR_expression);
    required_op("?");
    required_nt(expression);
    required_op(":");
    required_nt(conditional_expression);
    end_nt();
}

nt_branch(conditional_expression, 1)
{
    start_nt();
    required_nt(logical_OR_expression);
    end_nt();
}

nt_branch(assignment_expression, 0)
{
    start_nt();
    required_nt(unary_expression);
    required_nt(assignment_operator);
    required_nt(assignment_expression);
    end_nt();
}

nt_branch(assignment_expression, 1)
{
    start_nt();
    required_nt(conditional_expression);
    end_nt();
}

nt_branch(assignment_operator, 0)
{
    start_nt();
    required_op_list(op_assign);
    end_nt();
}

nt_branch(expression, 0)
{
    start_nt();
    direct_self_call(expression);
    required_op(",");
    required_nt(assignment_expression);
    end_nt();
}

nt_branch(expression, 1)
{
    start_nt();
    base_case_only();
    required_nt(assignment_expression);
    end_nt();
}

nt_branch(constant_expression, 0)
{
    start_nt();
    required_nt(conditional_expression);
    end_nt();
}

nt_branch(constant_range_expression, 0)
{
    start_nt();
    required_nt(constant_expression);
    required_op("...");
    required_nt(constant_expression);
    end_nt();
}

nt_branch(type_name, 0)
{
    start_nt();
    required_nt(specifier_qualifier_list);
    optional_nt(abstract_declarator);
    end_nt();
}

nt_branch(specifier_qualifier_list, 0)
{
    start_nt();
    direct_self_call(specifier_qualifier_list);
    required_nt(type_specifier_qualifier);
    optional_nt(attribute_specifier_sequence);
    end_nt();
}

nt_branch(specifier_qualifier_list, 1)
{
    start_nt();
    required_nt(type_specifier_qualifier);
    optional_nt(attribute_specifier_sequence);
    end_nt();
}

nt_branch(abstract_declarator, 0)
{
    start_nt();
    optional_nt(pointer);
    required_nt(direct_abstract_declarator);
    end_nt();
}

nt_branch(abstract_declarator, 1)
{
    start_nt();
    required_nt(pointer);
    end_nt();
}

nt_branch(type_specifier_qualifier, 0)
{
    start_nt();
    required_nt(type_specifier);
    end_nt();
}

nt_branch(type_specifier_qualifier, 1)
{
    start_nt();
    required_keyword(kw_type_qualifier);
    end_nt();
}

nt_branch(type_specifier_qualifier, 2)
{
    start_nt();
    required_nt(alignment_specifier);
    end_nt();
}

nt_branch(attribute_specifier_sequence, 0)
{
    start_nt();
    required_nt(attribute_specifier);
    optional_nt(attribute_specifier_sequence);
    end_nt();
}

nt_branch(pointer, 0)
{
    start_nt();
    required_op("*");
    optional_nt(attribute_specifier_sequence);
    optional_nt(type_qualifier_list);
    required_nt(pointer);
    end_nt();
}

nt_branch(pointer, 1)
{
    start_nt();
    required_op("*");
    optional_nt(attribute_specifier_sequence);
    optional_nt(type_qualifier_list);
    end_nt();
}

// [EXTENSION: Apple] Block pointer (Objective-C blocks) with nested pointer
nt_branch(pointer, 2)
{
    start_nt();
    required_op("^");
    optional_nt(attribute_specifier_sequence);
    optional_nt(type_qualifier_list);
    required_nt(pointer);
    end_nt();
}

// [EXTENSION: Apple] Block pointer (Objective-C blocks)
nt_branch(pointer, 3)
{
    start_nt();
    required_op("^");
    optional_nt(attribute_specifier_sequence);
    optional_nt(type_qualifier_list);
    end_nt();
}

nt_branch(direct_abstract_declarator, 0)
{
    start_nt();
    required_nt(function_abstract_declarator);
    optional_nt(attribute_specifier_sequence);
    end_nt();
}

nt_branch(direct_abstract_declarator, 1)
{
    start_nt();
    required_nt(array_abstract_declarator);
    optional_nt(attribute_specifier_sequence);
    end_nt();
}

nt_branch(direct_abstract_declarator, 2)
{
    start_nt();
    base_case_only();
    required_op("(");
    required_nt(abstract_declarator);
    required_op(")");
    end_nt();
}

nt_branch(type_specifier, 0)
{
    start_nt();
    required_keyword(kw_type_specifier);
    end_nt();
}

nt_branch(type_specifier, 1)
{
    start_nt();
    required_keyword(kw_bitint);
    required_op("(");
    required_nt(constant_expression);
    required_op(")");
    end_nt();
}

nt_branch(type_specifier, 2)
{
    start_nt();
    required_nt(atomic_type_specifier);
    end_nt();
}

nt_branch(type_specifier, 3)
{
    start_nt();
    required_nt(struct_or_union_specifier);
    end_nt();
}

nt_branch(type_specifier, 4)
{
    start_nt();
    required_nt(enum_specifier);
    end_nt();
}

nt_branch(type_specifier, 5)
{
    start_nt();
    required_nt(typedef_name);
    end_nt();
}

nt_branch(type_specifier, 6)
{
    start_nt();
    required_nt(typeof_specifier);
    end_nt();
}

nt_branch(alignment_specifier, 0)
{
    start_nt();
    required_keyword(kw_type_alignment);
    required_op("(");
    required_nt(type_name);
    required_op(")");
    end_nt();
}

nt_branch(alignment_specifier, 1)
{
    start_nt();
    required_keyword(kw_type_alignment);
    required_op("(");
    required_nt(constant_expression);
    required_op(")");
    end_nt();
}

nt_branch(attribute_specifier, 0)
{
    start_nt();
    required_op("[");
    required_op("[");
    required_nt(attribute_list);
    required_op("]");
    required_op("]");
    end_nt();
}

// [EXTENSION: GCC] __attribute__((...)) syntax
// Allows attaching arbitrary attributes to declarations.
// Uses double parentheses: __attribute__((attr1, attr2(args)))
// Content is parsed as balanced_token_sequence (not semantically validated).
// Examples: __attribute__((aligned(16))), __attribute__((deprecated("msg")))
nt_branch(attribute_specifier, 1)
{
    start_nt();
    required_keyword(kw_gcc_attribute);
    required_op("(");
    required_op("(");
    optional_nt(balanced_token_sequence);
    required_op(")");
    required_op(")");
    end_nt();
}

nt_branch(type_qualifier_list, 0)
{
    start_nt();
    required_keyword(kw_type_qualifier);
    required_nt(type_qualifier_list);
    end_nt();
}

nt_branch(type_qualifier_list, 1)
{
    start_nt();
    required_keyword(kw_type_qualifier);
    end_nt();
}

nt_branch(array_abstract_declarator, 0)
{
    start_nt();
    indirect_self_opt(direct_abstract_declarator);
    required_op("[");
    optional_nt(type_qualifier_list);
    optional_nt(assignment_expression);
    required_op("]");
    end_nt();
}

nt_branch(array_abstract_declarator, 1)
{
    start_nt();
    indirect_self_opt(direct_abstract_declarator);
    required_op("[");
    optional_keyword(kw_static);
    optional_nt(type_qualifier_list);
    optional_nt(assignment_expression);
    required_op("]");
    end_nt();
}

nt_branch(array_abstract_declarator, 2)
{
    start_nt();
    indirect_self_opt(direct_abstract_declarator);
    required_op("[");
    required_nt(type_qualifier_list);
    required_keyword(kw_static);
    required_nt(assignment_expression);
    required_op("]");
    end_nt();
}

nt_branch(array_abstract_declarator, 3)
{
    start_nt();
    indirect_self_opt(direct_abstract_declarator);
    required_op("[");
    required_op("*");
    required_op("]");
    end_nt();
}

nt_branch(function_abstract_declarator, 0)
{
    start_nt();
    indirect_self_opt(direct_abstract_declarator);
    required_op("(");
    optional_nt(parameter_type_list);
    required_op(")");
    end_nt();
}

nt_branch(atomic_type_specifier, 0)
{
    start_nt();
    required_keyword(kw_atomic);
    required_op("(");
    required_nt(type_name);
    required_op(")");
    end_nt();
}

nt_branch(struct_or_union_specifier, 0)
{
    start_nt();
    required_keyword(kw_struct_or_union);
    optional_nt(attribute_specifier_sequence);
    optional_nt(identifier);
    required_op("{");
    required_nt(member_declaration_list);
    required_op("}");
    end_nt();
}

// [EXTENSION: GCC/Common] Empty struct body
// Allows: struct foo { } - struct with no members
// This is undefined behavior in C but supported by most compilers
nt_branch(struct_or_union_specifier, 1)
{
    start_nt();
    required_keyword(kw_struct_or_union);
    optional_nt(attribute_specifier_sequence);
    optional_nt(identifier);
    required_op("{");
    required_op("}");
    end_nt();
}

nt_branch(struct_or_union_specifier, 2)
{
    start_nt();
    required_keyword(kw_struct_or_union);
    optional_nt(attribute_specifier_sequence);
    required_nt(identifier);
    end_nt();
}

nt_branch(enum_specifier, 0)
{
    start_nt();
    required_keyword(kw_enum);
    optional_nt(attribute_specifier_sequence);
    optional_nt(identifier);
    optional_nt(enum_type_specifier);
    required_op("{");
    required_nt(enumerator_list);
    required_op("}");
    end_nt();
}

// [EXTENSION: C99+/Common] Trailing comma in enum list
// Allows: enum { A, B, C, } - note trailing comma before closing brace.
// Standard since C99, but explicitly handled here for real-world compatibility.
nt_branch(enum_specifier, 1)
{
    start_nt();
    required_keyword(kw_enum);
    optional_nt(attribute_specifier_sequence);
    optional_nt(identifier);
    optional_nt(enum_type_specifier);
    required_op("{");
    required_nt(enumerator_list);
    required_op(",");
    required_op("}");
    end_nt();
}

nt_branch(enum_specifier, 2)
{
    start_nt();
    required_keyword(kw_enum);
    required_nt(identifier);
    optional_nt(enum_type_specifier);
    end_nt();
}

nt_branch(typedef_name, 0)
{
    start_nt();
    required_nt(typedef_name_terminal);
    end_nt();
}

nt_branch(typeof_specifier, 0)
{
    start_nt();
    required_keyword(kw_typeof);
    required_op("(");
    required_nt(typeof_specifier_argument);
    required_op(")");
    end_nt();
}

nt_branch(typeof_specifier, 1)
{
    start_nt();
    required_keyword(kw_typeof_unqual);
    required_op("(");
    required_nt(typeof_specifier_argument);
    required_op(")");
    end_nt();
}

nt_branch(attribute_list, 0)
{
    start_nt();
    optional_nt(attribute);
    end_nt();
}

nt_branch(attribute_list, 1)
{
    start_nt();
    direct_self_call(attribute_list);
    required_op(",");
    optional_nt(attribute);
    end_nt();
}

// Typed n00b varargs with params: parameter_list ',' type_name '+'
// n00b varargs: parameter_list '+' (typed if last param_decl is bare type)
// Handles: int + (typed, 0 positional), int, int + (typed, 1 positional)
nt_branch(parameter_type_list, 0)
{
    start_nt();
    required_nt(parameter_list);
    required_op("+");
    end_nt();
}

// n00b varargs: parameter_list ',' '+' (untyped with params)
// Handles: int, + (untyped, 1 positional)
nt_branch(parameter_type_list, 1)
{
    start_nt();
    required_nt(parameter_list);
    required_op(",");
    required_op("+");
    end_nt();
}

// n00b varargs: '+' only (untyped, no params)
nt_branch(parameter_type_list, 2)
{
    start_nt();
    required_op("+");
    end_nt();
}

// C varargs with params: parameter_list ',' '...'
nt_branch(parameter_type_list, 3)
{
    start_nt();
    required_nt(parameter_list);
    required_op(",");
    required_op("...");
    end_nt();
}

// C varargs only: '...'
nt_branch(parameter_type_list, 4)
{
    start_nt();
    required_op("...");
    end_nt();
}

// No varargs: parameter_list only
nt_branch(parameter_type_list, 5)
{
    start_nt();
    required_nt(parameter_list);
    end_nt();
}

nt_branch(member_declaration_list, 0)
{
    start_nt();
    direct_self_call(member_declaration_list);
    required_nt(member_declaration);
    end_nt();
}

nt_branch(member_declaration_list, 1)
{
    start_nt();
    required_nt(member_declaration);
    end_nt();
}

nt_branch(enum_type_specifier, 0)
{
    start_nt();
    required_op(":");
    required_nt(specifier_qualifier_list);
    end_nt();
}

nt_branch(enumerator_list, 0)
{
    start_nt();
    required_nt(enumerator);
    end_nt();
}

nt_branch(enumerator_list, 1)
{
    start_nt();
    direct_self_call(enumerator_list);
    required_op(",");
    required_nt(enumerator);
    end_nt();
}

nt_branch(typeof_specifier_argument, 0)
{
    start_nt();
    required_nt(type_name);
    end_nt();
}

nt_branch(typeof_specifier_argument, 1)
{
    start_nt();
    required_nt(expression);
    end_nt();
}

nt_branch(attribute, 0)
{
    start_nt();
    required_nt(attribute_token);
    optional_nt(attribute_argument_clause);
    end_nt();
}

nt_branch(parameter_list, 0)
{
    start_nt();
    required_nt(parameter_declaration);
    end_nt();
}

nt_branch(parameter_list, 1)
{
    start_nt();
    direct_self_call(parameter_list);
    required_op(",");
    required_nt(parameter_declaration);
    end_nt();
}

nt_branch(member_declaration, 0)
{
    start_nt();
    optional_nt(attribute_specifier_sequence);
    required_nt(specifier_qualifier_list);
    optional_nt(member_declarator_list);
    required_op(";");
    end_nt();
}

nt_branch(member_declaration, 1)
{
    start_nt();
    required_nt(static_assert_declaration);
    end_nt();
}

// [EXTENSION: GCC] __extension__ before member declarations
nt_branch(member_declaration, 2)
{
    start_nt();
    required_keyword(kw_gcc_extension);
    required_nt(member_declaration);
    end_nt();
}

nt_branch(enumerator, 0)
{
    start_nt();
    required_nt(enumeration_constant);
    optional_nt(attribute_specifier_sequence);
    required_op("=");
    required_nt(constant_expression);
    end_nt();
}

nt_branch(enumerator, 1)
{
    start_nt();
    required_nt(enumeration_constant);
    optional_nt(attribute_specifier_sequence);
    end_nt();
}

nt_branch(attribute_token, 0)
{
    start_nt();
    required_nt(attribute_prefixed_token);
    end_nt();
}

nt_branch(attribute_token, 1)
{
    start_nt();
    required_nt(standard_attribute);
    end_nt();
}

nt_branch(attribute_argument_clause, 0)
{
    start_nt();
    required_op("(");
    optional_nt(balanced_token_sequence);
    required_op(")");
    end_nt();
}

nt_branch(parameter_declaration, 0)
{
    start_nt();
    optional_nt(attribute_specifier_sequence);
    required_nt(declaration_specifiers);
    required_nt(declarator);
    end_nt();
}

nt_branch(parameter_declaration, 1)
{
    start_nt();
    optional_nt(attribute_specifier_sequence);
    required_nt(declaration_specifiers);
    optional_nt(abstract_declarator);
    end_nt();
}

nt_branch(member_declarator_list, 0)
{
    start_nt();
    required_nt(member_declarator);
    end_nt();
}

nt_branch(member_declarator_list, 1)
{
    start_nt();
    direct_self_call(member_declarator_list);
    required_op(",");
    required_nt(member_declarator);
    end_nt();
}

nt_branch(static_assert_declaration, 0)
{
    start_nt();
    required_keyword(kw_static_assert);
    required_op("(");
    required_nt(constant_expression);
    optional_op(",");
    optional_nt(string_literal);
    required_op(")");
    required_op(";");
    end_nt();
}

nt_branch(enumeration_constant, 0)
{
    start_nt();
    required_nt(identifier);
    end_nt();
}

nt_branch(standard_attribute, 0)
{
    start_nt();
    required_nt(identifier);
    end_nt();
}

nt_branch(attribute_prefixed_token, 0)
{
    start_nt();
    required_nt(attribute_prefix);
    required_op("::");
    required_nt(identifier);
    end_nt();
}

nt_branch(attribute_prefix, 0)
{
    start_nt();
    required_nt(identifier);
    end_nt();
}

nt_branch(declaration_specifiers, 0)
{
    start_nt();
    base_case_only();
    required_nt(declaration_specifier);
    optional_nt(attribute_specifier_sequence);
    end_nt();
}

nt_branch(declaration_specifiers, 1)
{
    start_nt();
    direct_self_call(declaration_specifiers);
    required_nt(declaration_specifier);
    optional_nt(attribute_specifier_sequence);
    end_nt();
}

nt_branch(declarator, 0)
{
    start_nt();
    optional_nt(pointer);
    required_nt(direct_declarator);
    end_nt();
}

// Try bitfield first (more specific) before plain declarator
nt_branch(member_declarator, 0)
{
    start_nt();
    optional_nt(declarator);
    required_op(":");
    required_nt(constant_expression);
    end_nt();
}

nt_branch(member_declarator, 1)
{
    start_nt();
    required_nt(declarator);
    end_nt();
}

nt_branch(declaration_specifier, 0)
{
    start_nt();
    required_nt(storage_class_specifier);
    end_nt();
}

nt_branch(declaration_specifier, 1)
{
    start_nt();
    required_nt(type_specifier_qualifier);
    end_nt();
}

nt_branch(declaration_specifier, 2)
{
    start_nt();
    required_nt(function_specifier);
    end_nt();
}

nt_branch(direct_declarator, 0)
{
    start_nt();
    base_case_only();
    required_op("(");
    required_nt(declarator);
    required_op(")");
    end_nt();
}

nt_branch(direct_declarator, 1)
{
    start_nt();
    required_nt(array_declarator);
    optional_nt(attribute_specifier_sequence);
    end_nt();
}

nt_branch(direct_declarator, 2)
{
    start_nt();
    required_nt(function_declarator);
    optional_nt(attribute_specifier_sequence);
    end_nt();
}

nt_branch(direct_declarator, 3)
{
    start_nt();
    required_nt(identifier);
    optional_nt(attribute_specifier_sequence);
    end_nt();
}

nt_branch(storage_class_specifier, 0)
{
    start_nt();
    required_keyword(kw_storage_class);
    end_nt();
}

nt_branch(function_specifier, 0)
{
    start_nt();
    required_keyword(kw_fn_specifier);
    end_nt();
}

// Branch for 'once' as an identifier (not a keyword)
nt_branch(function_specifier, 1)
{
    start_nt();
    required_named_identifier(kw_once_id);
    end_nt();
}

nt_branch(array_declarator, 0)
{
    start_nt();
    indirect_self_call(direct_declarator);
    required_op("[");
    optional_nt(type_qualifier_list);
    optional_nt(assignment_expression);
    required_op("]");
    end_nt();
}

nt_branch(array_declarator, 1)
{
    start_nt();
    indirect_self_call(direct_declarator);
    required_op("[");
    required_keyword(kw_static);
    optional_nt(type_qualifier_list);
    required_nt(assignment_expression);
    required_op("]");
    end_nt();
}

nt_branch(array_declarator, 2)
{
    start_nt();
    indirect_self_call(direct_declarator);
    required_op("[");
    optional_keyword(kw_static);
    optional_nt(type_qualifier_list);
    optional_nt(assignment_expression);
    required_op("]");
    end_nt();
}

nt_branch(array_declarator, 3)
{
    start_nt();
    indirect_self_call(direct_declarator);
    required_op("[");
    required_nt(type_qualifier_list);
    required_keyword(kw_static);
    required_nt(assignment_expression);
    required_op("]");
    end_nt();
}

nt_branch(function_declarator, 0)
{
    start_nt();
    indirect_self_call(direct_declarator);
    required_op("(");
    optional_nt(parameter_type_list);
    required_op(")");
    end_nt();
}

nt_branch(designator, 0)
{
    start_nt();
    required_op("[");
    required_nt(constant_expression);
    required_op("]");
    end_nt();
}

nt_branch(designator, 1)
{
    start_nt();
    required_op(".");
    required_nt(identifier);
    end_nt();
}

nt_branch(designator_list, 0)
{
    start_nt();
    direct_self_call(designator_list);
    required_nt(designator);
    end_nt();
}

nt_branch(designator_list, 1)
{
    start_nt();
    required_nt(designator);
    end_nt();
}

nt_branch(designation, 0)
{
    start_nt();
    required_nt(designator_list);
    required_op("=");
    end_nt();
}

nt_branch(braced_initializer, 0)
{
    start_nt();
    required_op("{");
    required_nt(initializer_list);
    optional_op(",");
    required_op("}");
    end_nt();
}

nt_branch(braced_initializer, 1)
{
    start_nt();
    required_op("{");
    required_op("}");
    end_nt();
}

nt_branch(initializer, 0)
{
    start_nt();
    required_nt(braced_initializer);
    end_nt();
}

nt_branch(initializer, 1)
{
    start_nt();
    required_nt(assignment_expression);
    end_nt();
}

nt_branch(initializer_list, 0)
{
    start_nt();
    optional_nt(designation);
    required_nt(initializer);
    end_nt();
}

nt_branch(initializer_list, 1)
{
    start_nt();
    direct_self_call(initializer_list);
    required_op(",");
    optional_nt(designation);
    required_nt(initializer);
    end_nt();
}

// [EXTENSION: GCC] Assembly symbol name for linker
// Allows specifying the assembly/linker symbol name for a declaration.
// Accepts: __asm__("name"), __asm("name"), or asm("name")
// Example: int foo __asm__("_foo"); - symbol will be "_foo" instead of "foo"
// Commonly used in system headers for ABI compatibility.
nt_branch(gcc_asm_label, 0)
{
    start_nt();
    required_keyword(kw_gcc_asm);
    required_op("(");
    required_nt(string_literal);
    required_op(")");
    end_nt();
}

// [EXTENSION: GCC] Inline assembly statement
// Syntax: __asm__ [volatile] [goto] ( assembly : outputs : inputs : clobbers : labels );
// The contents are parsed as balanced tokens since full asm parsing is complex.
// Examples: __asm__ __volatile__("" ::: "memory");
//           __asm__("nop");
nt_branch(gcc_asm_statement, 0)
{
    start_nt();
    required_keyword(kw_gcc_asm);
    optional_keyword(kw_type_qualifier); // volatile, __volatile__
    optional_keyword(kw_goto);           // goto for asm goto
    required_op("(");
    optional_nt(balanced_token_sequence);
    required_op(")");
    required_op(";");
    end_nt();
}

// init_declarator with initializer
// [EXTENSION: GCC] Supports optional __asm__("label") after declarator
nt_branch(init_declarator, 0)
{
    start_nt();
    required_nt(declarator);
    optional_nt(gcc_asm_label);
    optional_nt(attribute_specifier_sequence);
    required_op("=");
    required_nt(initializer);
    end_nt();
}

// init_declarator without initializer
// [EXTENSION: GCC] Supports optional __asm__("label") after declarator
nt_branch(init_declarator, 1)
{
    start_nt();
    required_nt(declarator);
    optional_nt(gcc_asm_label);
    optional_nt(attribute_specifier_sequence);
    end_nt();
}

nt_branch(init_declarator_list, 0)
{
    start_nt();
    required_nt(init_declarator);
    end_nt();
}

nt_branch(init_declarator_list, 1)
{
    start_nt();
    direct_self_call(init_declarator_list);
    required_op(",");
    required_nt(init_declarator);
    end_nt();
}

nt_branch(balanced_token, 0)
{
    start_nt();
    required_op("(");
    optional_nt(balanced_token_sequence);
    required_op(")");
    end_nt();
}

nt_branch(balanced_token, 1)
{
    start_nt();
    required_op("[");
    optional_nt(balanced_token_sequence);
    required_op("]");
    end_nt();
}

nt_branch(balanced_token, 2)
{
    start_nt();
    required_op("{");
    optional_nt(balanced_token_sequence);
    required_op("}");
    end_nt();
}

nt_branch(balanced_token, 3)
{
    start_nt();
    required_nt(non_bracket_token);
    end_nt();
}

nt_branch(balanced_token_sequence, 0)
{
    start_nt();
    direct_self_call(balanced_token_sequence);
    required_nt(balanced_token);
    end_nt();
}

nt_branch(balanced_token_sequence, 1)
{
    start_nt();
    required_nt(balanced_token);
    end_nt();
}

nt_branch(expression_statement, 0)
{
    start_nt();
    required_nt(attribute_specifier_sequence);
    required_nt(expression);
    required_op(";");
    end_nt();
}

nt_branch(expression_statement, 1)
{
    start_nt();
    required_nt(expression);
    required_op(";");
    end_nt();
}

nt_branch(expression_statement, 2)
{
    start_nt();
    required_op(";");
    end_nt();
}

nt_branch(jump_statement, 0)
{
    start_nt();
    required_keyword(kw_goto);
    required_nt(identifier);
    required_op(";");
    register_label_ref(ctx);

    end_nt();
}

nt_branch(jump_statement, 1)
{
    start_nt();
    required_keyword(kw_continue);
    required_nt(identifier);
    required_op(";");
    end_nt();
}

nt_branch(jump_statement, 2)
{
    start_nt();
    required_keyword(kw_continue);
    required_op(";");
    end_nt();
}

nt_branch(jump_statement, 3)
{
    start_nt();
    required_keyword(kw_break);
    required_nt(identifier);
    required_op(";");
    end_nt();
}

nt_branch(jump_statement, 4)
{
    start_nt();
    required_keyword(kw_break);
    required_op(";");
    end_nt();
}

nt_branch(jump_statement, 5)
{
    start_nt();
    required_keyword(kw_return);
    optional_nt(expression);
    required_op(";");
    end_nt();
}

// [EXTENSION: GCC] Indirect goto - jump to computed label
// Syntax: goto *expr;
nt_branch(jump_statement, 6)
{
    start_nt();
    required_keyword(kw_goto);
    required_op("*");
    required_nt(expression);
    required_op(";");
    end_nt();
}

// =============================================================================
// KEYWORD ARGUMENTS EXTENSION
// =============================================================================
//
// Syntax:
//   void foo(int x) _kargs { bool opt = true; int timeout = 30; };
//   void foo(int x) _kargs { bool opt = true; } { ... function body ... }
//
// keyword_clause:
//     '_kargs' '{' keyword_param_list '}'
//
// keyword_param_list:
//     keyword_param_list keyword_param
//   | keyword_param
//
// keyword_param:
//     declaration_specifiers declarator '=' initializer ';'
//   | declaration_specifiers declarator ';'

nt_branch(keyword_clause, 0)
{
    start_nt();
    required_keyword(kw_keywords);
    required_op("{");
    required_nt(keyword_param_list);
    required_op("}");
    end_nt();
}

// [EXTENSION] Opaque keyword passthrough
// Syntax: void foo(int x, ...) _kargs: opaque;
// Function accepts keyword arguments to forward, but defines none of its own.
// Adds void *kargs parameter without generating a struct.
nt_branch(keyword_clause, 1)
{
    start_nt();
    required_keyword(kw_keywords);
    required_op(":");
    required_named_identifier(kw_opaque);
    end_nt();
}

nt_branch(keyword_param, 0)
{
    start_nt();
    required_nt(declaration_specifiers);
    required_nt(declarator);
    required_op("=");
    required_nt(initializer);
    required_op(";");
    end_nt();
}

nt_branch(keyword_param, 1)
{
    start_nt();
    required_nt(declaration_specifiers);
    required_nt(declarator);
    required_op(";");
    end_nt();
}

nt_branch(keyword_param_list, 0)
{
    start_nt();
    direct_self_call(keyword_param_list);
    required_nt(keyword_param);
    end_nt();
}

nt_branch(keyword_param_list, 1)
{
    start_nt();
    required_nt(keyword_param);
    end_nt();
}

nt_branch(label, 0)
{
    start_nt();
    optional_nt(attribute_specifier_sequence);
    required_nt(identifier);
    required_op(":");
    register_label_def(ctx);
    end_nt();
}

nt_branch(label, 1)
{
    start_nt();
    optional_nt(attribute_specifier_sequence);
    required_keyword(kw_case);
    required_nt(constant_range_expression);
    required_op(":");
    end_nt();
}

nt_branch(label, 2)
{
    start_nt();
    optional_nt(attribute_specifier_sequence);
    required_keyword(kw_case);
    required_nt(constant_expression);
    required_op(":");
    end_nt();
}

nt_branch(label, 3)
{
    start_nt();
    optional_nt(attribute_specifier_sequence);
    required_keyword(kw_default);
    required_op(":");
    end_nt();
}

nt_branch(labeled_statement, 0)
{
    start_nt();
    required_nt(label);
    required_nt(statement);
    end_nt();
}

nt_branch(selection_header, 0)
{
    start_nt();
    required_nt(simple_declaration);
    end_nt();
}

nt_branch(selection_header, 1)
{
    start_nt();
    required_nt(declaration);
    required_nt(expression);
    end_nt();
}

nt_branch(selection_header, 2)
{
    start_nt();
    required_nt(expression);
    end_nt();
}

nt_branch(simple_declaration, 0)
{
    start_nt();
    optional_nt(attribute_specifier_sequence);
    required_nt(declaration_specifiers);
    required_nt(declarator);
    required_op("=");
    required_nt(initializer);
    end_nt();
}

nt_branch(selection_statement, 0)
{
    start_nt();
    required_keyword(kw_if);
    required_op("(");
    required_nt(selection_header);
    required_op(")");
    required_nt(secondary_block);
    required_keyword(kw_else);
    required_nt(secondary_block);
    end_nt();
}

nt_branch(selection_statement, 1)
{
    start_nt();
    required_keyword(kw_if);
    required_op("(");
    required_nt(selection_header);
    required_op(")");
    required_nt(secondary_block);
    end_nt();
}

nt_branch(selection_statement, 2)
{
    start_nt();
    required_keyword(kw_switch);
    required_op("(");
    required_nt(selection_header);
    required_op(")");
    required_nt(secondary_block);
    end_nt();
}

nt_branch(iteration_statement, 0)
{
    start_nt();
    required_keyword(kw_while);
    required_op("(");
    required_nt(expression);
    required_op(")");
    required_nt(secondary_block);
    end_nt();
}

nt_branch(iteration_statement, 1)
{
    start_nt();
    required_keyword(kw_do);
    required_nt(secondary_block);
    required_keyword(kw_while);
    required_op("(");
    required_nt(expression);
    required_op(")");
    required_op(";");
    end_nt();
}

nt_branch(iteration_statement, 2)
{
    start_nt();
    required_keyword(kw_for);
    required_op("(");
    optional_nt(expression);
    required_op(";");
    optional_nt(expression);
    required_op(";");
    optional_nt(expression);
    required_op(")");
    required_nt(secondary_block);
    end_nt();
}

nt_branch(iteration_statement, 3)
{
    start_nt();
    required_keyword(kw_for);
    required_op("(");
    required_nt(declaration);
    optional_nt(expression);
    required_op(";");
    optional_nt(expression);
    required_op(")");
    required_nt(secondary_block);
    end_nt();
}

nt_branch(primary_block, 0)
{
    start_nt();
    required_nt(compound_statement);
    end_nt();
}

nt_branch(primary_block, 1)
{
    start_nt();
    required_nt(selection_statement);
    end_nt();
}

nt_branch(primary_block, 2)
{
    start_nt();
    required_nt(iteration_statement);
    end_nt();
}

nt_branch(secondary_block, 0)
{
    start_nt();
    required_nt(statement);
    end_nt();
}

nt_branch(compound_statement, 0)
{
    start_nt();
    required_op("{");
    enter_block_scope(ctx);
    optional_nt(block_item_list);
    required_op("}");
    end_block_scope();
}

nt_branch(block_item, 0)
{
    start_nt();
    required_nt(declaration);
    end_nt();
}

nt_branch(block_item, 1)
{
    start_nt();
    required_nt(label);
    end_nt();
}

nt_branch(block_item, 2)
{
    start_nt();
    required_nt(unlabeled_statement);
    end_nt();
}

nt_branch(block_item_list, 0)
{
    start_nt();
    direct_self_call(block_item_list);
    required_nt(block_item);
    end_nt();
}

nt_branch(block_item_list, 1)
{
    start_nt();
    required_nt(block_item);
    end_nt();
}

nt_branch(unlabeled_statement, 0)
{
    start_nt();
    required_nt(expression_statement);
    end_nt();
}

nt_branch(unlabeled_statement, 1)
{
    start_nt();
    optional_nt(attribute_specifier_sequence);
    required_nt(primary_block);
    end_nt();
}

nt_branch(unlabeled_statement, 2)
{
    start_nt();
    optional_nt(attribute_specifier_sequence);
    required_nt(jump_statement);
    end_nt();
}

// [EXTENSION: GCC] Inline assembly statement
nt_branch(unlabeled_statement, 3)
{
    start_nt();
    required_nt(gcc_asm_statement);
    end_nt();
}

nt_branch(statement, 0)
{
    start_nt();
    required_nt(labeled_statement);
    end_nt();
}

nt_branch(statement, 1)
{
    start_nt();
    required_nt(unlabeled_statement);
    end_nt();
}

nt_branch(declaration, 0)
{
    start_nt();
    required_nt(attribute_specifier_sequence);
    required_nt(declaration_specifiers);
    required_nt(init_declarator_list);
    required_op(";");
    register_declaration(ctx);
    end_nt();
}

nt_branch(declaration, 1)
{
    start_nt();
    required_nt(declaration_specifiers);
    optional_nt(init_declarator_list);
    required_op(";");
    register_declaration(ctx);
    end_nt();
}

nt_branch(declaration, 2)
{
    start_nt();
    required_nt(static_assert_declaration);
    end_nt();
}

nt_branch(declaration, 3)
{
    start_nt();
    required_nt(attribute_declaration);
    end_nt();
}

// [EXTENSION] Function declaration with keyword arguments
// Syntax: void foo(int x) _kargs { bool opt = true; };
nt_branch(declaration, 4)
{
    start_nt();
    required_nt(declaration_specifiers);
    required_nt(declarator);
    required_nt(keyword_clause);
    required_op(";");
    register_declaration(ctx);
    end_nt();
}

// [EXTENSION: GCC] Type declaration with trailing attribute
// Syntax: struct foo { ... } __attribute__((...));
// This allows attributes on type-only declarations (no variable declared)
nt_branch(declaration, 5)
{
    start_nt();
    required_nt(declaration_specifiers);
    required_nt(attribute_specifier_sequence);
    required_op(";");
    register_declaration(ctx);
    end_nt();
}

// [EXTENSION: GCC] __extension__ before declarations
// Suppresses GCC warnings on the following declaration.
nt_branch(declaration, 6)
{
    start_nt();
    required_keyword(kw_gcc_extension);
    required_nt(declaration);
    end_nt();
}

nt_branch(attribute_declaration, 0)
{
    start_nt();
    required_nt(attribute_specifier_sequence);
    required_op(";");
    end_nt();
}

nt_branch(function_body, 0)
{
    start_nt();
    required_nt(compound_statement);
    end_nt();
}

// [EXTENSION] Function definition with keyword arguments
// Syntax: void foo(int x) _kargs { bool opt = true; } { ... }
nt_branch(function_definition, 0)
{
    start_nt();
    optional_nt(attribute_specifier_sequence);
    required_nt(declaration_specifiers);
    required_nt(declarator);
    required_nt(keyword_clause);
    register_function_definition(ctx);
    required_nt(function_body);
    end_func_scope();
}

nt_branch(function_definition, 1)
{
    start_nt();
    optional_nt(attribute_specifier_sequence);
    required_nt(declaration_specifiers);
    required_nt(declarator);
    register_function_definition(ctx);
    required_nt(function_body);
    end_func_scope();
}

// [EXTENSION: GCC] __extension__ before function definitions
nt_branch(function_definition, 2)
{
    start_nt();
    required_keyword(kw_gcc_extension);
    required_nt(function_definition);
    end_nt();
}

nt_branch(external_declaration, 0)
{
    start_nt();
    required_nt(function_definition);
    end_nt();
}

nt_branch(external_declaration, 1)
{
    start_nt();
    required_nt(declaration);
    end_nt();
}

// [EXTENSION: Common] Empty declaration (stray semicolon at file scope)
// Many compilers accept this with a warning; system headers sometimes have them.
nt_branch(external_declaration, 2)
{
    start_nt();
    required_op(";");
    end_nt();
}

// [EXTENSION: NCC] Package declaration
// Syntax: package identifier;
nt_branch(external_declaration, 3)
{
    start_nt();
    required_named_identifier(kw_package);
    required_nt(identifier);
    required_op(";");
    register_package(ctx);
    end_nt();
}

nt_branch(translation_unit, 0)
{
    start_nt();
    direct_self_call(translation_unit);
    required_nt(external_declaration);
    end_nt();
}

nt_branch(translation_unit, 1)
{
    start_nt();
    required_nt(external_declaration);
    end_nt();
}

tnode_t *
parse_type_expression_st(lex_t *state, int *position, symtab_t *st)
{
    parser_t parser = {
        .input          = state->input,
        .lex            = state,
        .tokens         = state->toks,
        .num_tokens     = state->num_toks,
        .pos            = *position,
        .cur_node       = nullptr,
        .recursive_node = nullptr,
        .symtab         = st,
        .label_table    = nullptr,
    };

#if defined(PDEBUG)
    parser.debug_cur = nullptr;
    pdebug_push(&parser, "<<root>>");
    parser.debug_root = parser.debug_cur;
#endif
    tnode_t *result = type_name(&parser);

    *position = parser.pos;

    return result;
}

tnode_t *
parse_expression_st(lex_t *state, int *position, symtab_t *st)
{
    parser_t parser = {
        .input          = state->input,
        .lex            = state,
        .tokens         = state->toks,
        .num_tokens     = state->num_toks,
        .pos            = *position,
        .cur_node       = nullptr,
        .recursive_node = nullptr,
        .symtab         = st,
        .label_table    = nullptr,
    };

#if defined(PDEBUG)
    parser.debug_cur = nullptr;
    pdebug_push(&parser, "<<root>>");
    parser.debug_root = parser.debug_cur;
#endif
    tnode_t *result = expression(&parser);

    *position = parser.pos;

    return result;
}

tnode_t *
parse_statement_st(lex_t *state, int *position, symtab_t *st)
{
    parser_t parser = {
        .input          = state->input,
        .lex            = state,
        .tokens         = state->toks,
        .num_tokens     = state->num_toks,
        .pos            = *position,
        .cur_node       = nullptr,
        .recursive_node = nullptr,
        .symtab         = st,
        .label_table    = nullptr,
    };

#if defined(PDEBUG)
    parser.debug_cur = nullptr;
    pdebug_push(&parser, "<<root>>");
    parser.debug_root = parser.debug_cur;
#endif
    tnode_t *result = statement(&parser);

    *position = parser.pos;

    return result;
}

tnode_t *
parse_declaration_st(lex_t *state, int *position, symtab_t *st)
{
    parser_t parser = {
        .input          = state->input,
        .lex            = state,
        .tokens         = state->toks,
        .num_tokens     = state->num_toks,
        .pos            = *position,
        .cur_node       = nullptr,
        .recursive_node = nullptr,
        .symtab         = st,
        .label_table    = nullptr,
    };

#if defined(PDEBUG)
    parser.debug_cur = nullptr;
    pdebug_push(&parser, "<<root>>");
    parser.debug_root = parser.debug_cur;
#endif
    tnode_t *result = declaration(&parser);

    *position = parser.pos;

    return result;
}

tnode_t *
parse_translation_unit_st(lex_t *state, int *position, symtab_t *st)
{
    parser_t parser = {
        .input          = state->input,
        .lex            = state,
        .tokens         = state->toks,
        .num_tokens     = state->num_toks,
        .pos            = *position,
        .cur_node       = nullptr,
        .recursive_node = nullptr,
        .symtab         = st,
        .label_table    = nullptr,
        .memo           = nullptr,
        .memo_size      = 0,
    };

    memo_init(&parser);

    // Register per-NT error handlers (idempotent)
    nt_error_init();

#if defined(PDEBUG)
    parser.debug_cur = nullptr;
    pdebug_push(&parser, "<<root>>");
    parser.debug_root = parser.debug_cur;
#endif
    parse_prime(&parser);
    tnode_t *result = translation_unit(&parser);

    *position = parser.pos;

    memo_free(&parser);
    return result;
}
