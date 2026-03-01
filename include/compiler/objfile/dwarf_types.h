/**
 * @file n00b_dwarf_types.h
 * @brief DWARF debug information constants.
 *
 * Defines all DWARF constants needed for parsing and generating debug
 * information: tags (N00B_DW_TAG), attributes (N00B_DW_AT), forms (N00B_DW_FORM),
 * base type encodings (N00B_DW_ATE), unit types (N00B_DW_UT), and line number
 * program opcodes (N00B_DW_LNS, N00B_DW_LNE).
 *
 * Covers DWARF versions 2 through 5, plus common GNU extensions.
 */
#pragma once

#include <stdint.h>

// ============================================================================
// N00B_DW_TAG — Debug Information Entry tags
// ============================================================================

#define N00B_DW_TAG_array_type             0x01
#define N00B_DW_TAG_class_type             0x02
#define N00B_DW_TAG_entry_point            0x03
#define N00B_DW_TAG_enumeration_type       0x04
#define N00B_DW_TAG_formal_parameter       0x05
#define N00B_DW_TAG_imported_declaration   0x08
#define N00B_DW_TAG_label                  0x0a
#define N00B_DW_TAG_lexical_block          0x0b
#define N00B_DW_TAG_member                 0x0d
#define N00B_DW_TAG_pointer_type           0x0f
#define N00B_DW_TAG_reference_type         0x10
#define N00B_DW_TAG_compile_unit           0x11
#define N00B_DW_TAG_string_type            0x12
#define N00B_DW_TAG_structure_type         0x13
#define N00B_DW_TAG_subroutine_type        0x15
#define N00B_DW_TAG_typedef                0x16
#define N00B_DW_TAG_union_type             0x17
#define N00B_DW_TAG_unspecified_parameters 0x18
#define N00B_DW_TAG_variant                0x19
#define N00B_DW_TAG_common_block           0x1a
#define N00B_DW_TAG_common_inclusion       0x1b
#define N00B_DW_TAG_inheritance            0x1c
#define N00B_DW_TAG_inlined_subroutine     0x1d
#define N00B_DW_TAG_module                 0x1e
#define N00B_DW_TAG_ptr_to_member_type     0x1f
#define N00B_DW_TAG_set_type               0x20
#define N00B_DW_TAG_subrange_type          0x21
#define N00B_DW_TAG_with_stmt              0x22
#define N00B_DW_TAG_access_declaration     0x23
#define N00B_DW_TAG_base_type              0x24
#define N00B_DW_TAG_catch_block            0x25
#define N00B_DW_TAG_const_type             0x26
#define N00B_DW_TAG_constant               0x27
#define N00B_DW_TAG_enumerator             0x28
#define N00B_DW_TAG_file_type              0x29
#define N00B_DW_TAG_friend                 0x2a
#define N00B_DW_TAG_namelist               0x2b
#define N00B_DW_TAG_namelist_item          0x2c
#define N00B_DW_TAG_packed_type            0x2d
#define N00B_DW_TAG_subprogram             0x2e
#define N00B_DW_TAG_template_type_parameter  0x2f
#define N00B_DW_TAG_template_value_parameter 0x30
#define N00B_DW_TAG_thrown_type             0x31
#define N00B_DW_TAG_try_block              0x32
#define N00B_DW_TAG_variant_part           0x33
#define N00B_DW_TAG_variable               0x34
#define N00B_DW_TAG_volatile_type          0x35
#define N00B_DW_TAG_dwarf_procedure        0x36
#define N00B_DW_TAG_restrict_type          0x37
#define N00B_DW_TAG_interface_type         0x38
#define N00B_DW_TAG_namespace              0x39
#define N00B_DW_TAG_imported_module        0x3a
#define N00B_DW_TAG_unspecified_type       0x3b
#define N00B_DW_TAG_partial_unit           0x3c
#define N00B_DW_TAG_imported_unit          0x3d
#define N00B_DW_TAG_condition              0x3f
#define N00B_DW_TAG_shared_type            0x40
#define N00B_DW_TAG_type_unit              0x41
#define N00B_DW_TAG_rvalue_reference_type  0x42
#define N00B_DW_TAG_template_alias         0x43
#define N00B_DW_TAG_coarray_type           0x44
#define N00B_DW_TAG_generic_subrange       0x45
#define N00B_DW_TAG_dynamic_type           0x46
#define N00B_DW_TAG_atomic_type            0x47
#define N00B_DW_TAG_call_site              0x48
#define N00B_DW_TAG_call_site_parameter    0x49
#define N00B_DW_TAG_skeleton_unit          0x4a
#define N00B_DW_TAG_immutable_type         0x4b

// ============================================================================
// N00B_DW_CHILDREN
// ============================================================================

#define N00B_DW_CHILDREN_no  0x00
#define N00B_DW_CHILDREN_yes 0x01

// ============================================================================
// N00B_DW_AT — Attribute names
// ============================================================================

#define N00B_DW_AT_sibling               0x01
#define N00B_DW_AT_location              0x02
#define N00B_DW_AT_name                  0x03
#define N00B_DW_AT_ordering              0x09
#define N00B_DW_AT_byte_size             0x0b
#define N00B_DW_AT_bit_offset            0x0c
#define N00B_DW_AT_bit_size              0x0d
#define N00B_DW_AT_stmt_list             0x10
#define N00B_DW_AT_low_pc                0x11
#define N00B_DW_AT_high_pc               0x12
#define N00B_DW_AT_language              0x13
#define N00B_DW_AT_discr                 0x15
#define N00B_DW_AT_discr_value           0x16
#define N00B_DW_AT_visibility            0x17
#define N00B_DW_AT_import                0x18
#define N00B_DW_AT_string_length         0x19
#define N00B_DW_AT_common_reference      0x1a
#define N00B_DW_AT_comp_dir              0x1b
#define N00B_DW_AT_const_value           0x1c
#define N00B_DW_AT_containing_type       0x1d
#define N00B_DW_AT_default_value         0x1e
#define N00B_DW_AT_inline                0x20
#define N00B_DW_AT_is_optional           0x21
#define N00B_DW_AT_lower_bound           0x22
#define N00B_DW_AT_producer              0x25
#define N00B_DW_AT_prototyped            0x27
#define N00B_DW_AT_return_addr           0x2a
#define N00B_DW_AT_start_scope           0x2c
#define N00B_DW_AT_bit_stride            0x2e
#define N00B_DW_AT_upper_bound           0x2f
#define N00B_DW_AT_abstract_origin       0x31
#define N00B_DW_AT_accessibility         0x32
#define N00B_DW_AT_address_class         0x33
#define N00B_DW_AT_artificial            0x34
#define N00B_DW_AT_base_types            0x35
#define N00B_DW_AT_calling_convention    0x36
#define N00B_DW_AT_count                 0x37
#define N00B_DW_AT_data_member_location  0x38
#define N00B_DW_AT_decl_column           0x39
#define N00B_DW_AT_decl_file             0x3a
#define N00B_DW_AT_decl_line             0x3b
#define N00B_DW_AT_declaration           0x3c
#define N00B_DW_AT_discr_list            0x3d
#define N00B_DW_AT_encoding              0x3e
#define N00B_DW_AT_external              0x3f
#define N00B_DW_AT_frame_base            0x40
#define N00B_DW_AT_friend                0x41
#define N00B_DW_AT_identifier_case       0x42
#define N00B_DW_AT_macro_info            0x43
#define N00B_DW_AT_namelist_item         0x44
#define N00B_DW_AT_priority              0x45
#define N00B_DW_AT_segment               0x46
#define N00B_DW_AT_specification         0x47
#define N00B_DW_AT_static_link           0x48
#define N00B_DW_AT_type                  0x49
#define N00B_DW_AT_use_location          0x4a
#define N00B_DW_AT_variable_parameter    0x4b
#define N00B_DW_AT_virtuality            0x4c
#define N00B_DW_AT_vtable_elem_location  0x4d
#define N00B_DW_AT_allocated             0x4e
#define N00B_DW_AT_associated            0x4f
#define N00B_DW_AT_data_location         0x50
#define N00B_DW_AT_byte_stride           0x51
#define N00B_DW_AT_entry_pc              0x52
#define N00B_DW_AT_use_UTF8              0x53
#define N00B_DW_AT_extension             0x54
#define N00B_DW_AT_ranges                0x55
#define N00B_DW_AT_trampoline            0x56
#define N00B_DW_AT_call_column           0x57
#define N00B_DW_AT_call_file             0x58
#define N00B_DW_AT_call_line             0x59
#define N00B_DW_AT_description           0x5a
#define N00B_DW_AT_binary_scale          0x5b
#define N00B_DW_AT_decimal_scale         0x5c
#define N00B_DW_AT_small                 0x5d
#define N00B_DW_AT_decimal_sign          0x5e
#define N00B_DW_AT_digit_count           0x5f
#define N00B_DW_AT_picture_string        0x60
#define N00B_DW_AT_mutable               0x61
#define N00B_DW_AT_threads_scaled        0x62
#define N00B_DW_AT_explicit              0x63
#define N00B_DW_AT_object_pointer        0x64
#define N00B_DW_AT_endianity             0x65
#define N00B_DW_AT_elemental             0x66
#define N00B_DW_AT_pure                  0x67
#define N00B_DW_AT_recursive             0x68
#define N00B_DW_AT_signature             0x69
#define N00B_DW_AT_main_subprogram       0x6a
#define N00B_DW_AT_data_bit_offset       0x6b
#define N00B_DW_AT_const_expr            0x6c
#define N00B_DW_AT_enum_class            0x6d
#define N00B_DW_AT_linkage_name          0x6e

// DWARF 5
#define N00B_DW_AT_string_length_bit_size  0x6f
#define N00B_DW_AT_string_length_byte_size 0x70
#define N00B_DW_AT_rank                    0x71
#define N00B_DW_AT_str_offsets_base        0x72
#define N00B_DW_AT_addr_base               0x73
#define N00B_DW_AT_rnglists_base           0x74
#define N00B_DW_AT_dwo_name                0x76
#define N00B_DW_AT_reference               0x77
#define N00B_DW_AT_rvalue_reference        0x78
#define N00B_DW_AT_macros                  0x79
#define N00B_DW_AT_call_all_calls          0x7a
#define N00B_DW_AT_call_all_source_calls   0x7b
#define N00B_DW_AT_call_all_tail_calls     0x7c
#define N00B_DW_AT_call_return_pc          0x7d
#define N00B_DW_AT_call_value              0x7e
#define N00B_DW_AT_call_origin             0x7f
#define N00B_DW_AT_call_parameter          0x80
#define N00B_DW_AT_call_pc                 0x81
#define N00B_DW_AT_call_tail_call          0x82
#define N00B_DW_AT_call_target             0x83
#define N00B_DW_AT_call_target_clobbered   0x84
#define N00B_DW_AT_call_data_location      0x85
#define N00B_DW_AT_call_data_value         0x86
#define N00B_DW_AT_noreturn                0x87
#define N00B_DW_AT_alignment               0x88
#define N00B_DW_AT_export_symbols          0x89
#define N00B_DW_AT_deleted                 0x8a
#define N00B_DW_AT_defaulted               0x8b
#define N00B_DW_AT_loclists_base           0x8c

// GNU extensions
#define N00B_DW_AT_GNU_vector              0x2107
#define N00B_DW_AT_GNU_template_name       0x2110
#define N00B_DW_AT_GNU_all_tail_call_sites 0x2116
#define N00B_DW_AT_GNU_all_call_sites      0x2117
#define N00B_DW_AT_GNU_pubnames            0x2134
#define N00B_DW_AT_GNU_pubtypes            0x2135

// ============================================================================
// N00B_DW_FORM — Attribute form encodings
// ============================================================================

#define N00B_DW_FORM_addr            0x01
#define N00B_DW_FORM_block2          0x03
#define N00B_DW_FORM_block4          0x04
#define N00B_DW_FORM_data2           0x05
#define N00B_DW_FORM_data4           0x06
#define N00B_DW_FORM_data8           0x07
#define N00B_DW_FORM_string          0x08
#define N00B_DW_FORM_block           0x09
#define N00B_DW_FORM_block1          0x0a
#define N00B_DW_FORM_data1           0x0b
#define N00B_DW_FORM_flag            0x0c
#define N00B_DW_FORM_sdata           0x0d
#define N00B_DW_FORM_strp            0x0e
#define N00B_DW_FORM_udata           0x0f
#define N00B_DW_FORM_ref_addr        0x10
#define N00B_DW_FORM_ref1            0x11
#define N00B_DW_FORM_ref2            0x12
#define N00B_DW_FORM_ref4            0x13
#define N00B_DW_FORM_ref8            0x14
#define N00B_DW_FORM_ref_udata       0x15
#define N00B_DW_FORM_indirect        0x16
#define N00B_DW_FORM_sec_offset      0x17
#define N00B_DW_FORM_exprloc         0x18
#define N00B_DW_FORM_flag_present    0x19
#define N00B_DW_FORM_strx            0x1a
#define N00B_DW_FORM_addrx           0x1b
#define N00B_DW_FORM_ref_sup4        0x1c
#define N00B_DW_FORM_strp_sup        0x1d
#define N00B_DW_FORM_data16          0x1e
#define N00B_DW_FORM_line_strp       0x1f
#define N00B_DW_FORM_ref_sig8        0x20
#define N00B_DW_FORM_implicit_const  0x21
#define N00B_DW_FORM_loclistx        0x22
#define N00B_DW_FORM_rnglistx        0x23
#define N00B_DW_FORM_ref_sup8        0x24
#define N00B_DW_FORM_strx1           0x25
#define N00B_DW_FORM_strx2           0x26
#define N00B_DW_FORM_strx3           0x27
#define N00B_DW_FORM_strx4           0x28
#define N00B_DW_FORM_addrx1          0x29
#define N00B_DW_FORM_addrx2          0x2a
#define N00B_DW_FORM_addrx3          0x2b
#define N00B_DW_FORM_addrx4          0x2c

// ============================================================================
// N00B_DW_ATE — Base type encodings
// ============================================================================

#define N00B_DW_ATE_address        0x01
#define N00B_DW_ATE_boolean        0x02
#define N00B_DW_ATE_complex_float  0x03
#define N00B_DW_ATE_float          0x04
#define N00B_DW_ATE_signed         0x05
#define N00B_DW_ATE_signed_char    0x06
#define N00B_DW_ATE_unsigned       0x07
#define N00B_DW_ATE_unsigned_char  0x08
#define N00B_DW_ATE_imaginary_float 0x09
#define N00B_DW_ATE_packed_decimal 0x0a
#define N00B_DW_ATE_numeric_string 0x0b
#define N00B_DW_ATE_edited         0x0c
#define N00B_DW_ATE_signed_fixed   0x0d
#define N00B_DW_ATE_unsigned_fixed 0x0e
#define N00B_DW_ATE_decimal_float  0x0f
#define N00B_DW_ATE_UTF            0x10

// ============================================================================
// N00B_DW_UT — Unit types (DWARF 5)
// ============================================================================

#define N00B_DW_UT_compile       0x01
#define N00B_DW_UT_type          0x02
#define N00B_DW_UT_partial       0x03
#define N00B_DW_UT_skeleton      0x04
#define N00B_DW_UT_split_compile 0x05
#define N00B_DW_UT_split_type    0x06

// ============================================================================
// N00B_DW_LNS — Line number standard opcodes
// ============================================================================

#define N00B_DW_LNS_copy               0x01
#define N00B_DW_LNS_advance_pc         0x02
#define N00B_DW_LNS_advance_line       0x03
#define N00B_DW_LNS_set_file           0x04
#define N00B_DW_LNS_set_column         0x05
#define N00B_DW_LNS_negate_stmt        0x06
#define N00B_DW_LNS_set_basic_block    0x07
#define N00B_DW_LNS_const_add_pc       0x08
#define N00B_DW_LNS_fixed_advance_pc   0x09
#define N00B_DW_LNS_set_prologue_end   0x0a
#define N00B_DW_LNS_set_epilogue_begin 0x0b
#define N00B_DW_LNS_set_isa            0x0c

// ============================================================================
// N00B_DW_LNE — Line number extended opcodes
// ============================================================================

#define N00B_DW_LNE_end_sequence     0x01
#define N00B_DW_LNE_set_address      0x02
#define N00B_DW_LNE_define_file      0x03
#define N00B_DW_LNE_set_discriminator 0x04

// ============================================================================
// N00B_DW_LNCT — Line number content types (DWARF 5)
// ============================================================================

#define N00B_DW_LNCT_path            0x01
#define N00B_DW_LNCT_directory_index 0x02
#define N00B_DW_LNCT_timestamp       0x03
#define N00B_DW_LNCT_size            0x04
#define N00B_DW_LNCT_MD5             0x05

// ============================================================================
// N00B_DW_OP — Selected DWARF expression opcodes (used for member locations)
// ============================================================================

#define N00B_DW_OP_plus_uconst 0x23
