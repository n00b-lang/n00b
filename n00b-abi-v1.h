#ifndef N00B_ABI_V1_H
#define N00B_ABI_V1_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Review draft. All serialized integer fields are little-endian. On the
 * three initial little-endian targets, validated records may be exposed as
 * these C views; readers must validate bytes before returning a view. */

typedef union {
    struct {
        uint8_t bytes[15];
        uint8_t selector; /* length bits 0-3, encoding 4-5, tag 6-7 */
    } short_value;
    struct {
        uint8_t *data;
        uint64_t selector; /* length bits 0-59, encoding 60-61, tag 62-63 */
    } long_value, span_value;
    struct {
        uint8_t *body;
        uint64_t selector; /* reserved bits 0-59, encoding 60-61, tag 62-63 */
    } rich_value;
    uint64_t words[2];
} n00b_abi_string_v1;
#define N00B_ABI_STRING_TAG_SHIFT 62u
#define N00B_ABI_STRING_ENCODING_SHIFT 60u
#define N00B_ABI_STRING_SHORT_LENGTH_MASK UINT8_C(0x0f)
#define N00B_ABI_STRING_LONG_LENGTH_MASK ((UINT64_C(1) << 60) - 1)
typedef enum {
    N00B_ABI_STRING_SHORT = 0,
    N00B_ABI_STRING_LONG = 1,
    N00B_ABI_STRING_SPAN = 2,
    N00B_ABI_STRING_RICH = 3,
} n00b_abi_string_tag_v1;
typedef enum {
    N00B_ABI_STRING_BINARY = 0,
    N00B_ABI_STRING_ASCII = 1,
    N00B_ABI_STRING_UTF8 = 2,
} n00b_abi_string_encoding_v1;
/* Runtime value-layout views, not metadata table records. */
typedef struct {
    void *base;
    uint32_t type_index;
    uint16_t start_bit;
    uint16_t end_bit;
} n00b_abi_bitptr_v1;

typedef struct {
    void *data;
    const void *descriptor;
} n00b_abi_array_value_v1;

typedef struct {
    uint64_t total_count;
    uint32_t rank;
    uint32_t flat_rank;
    uint32_t flags;
    uint32_t reserved_zero;
    /* `rank` uint64_t extents follow this header. */
} n00b_abi_array_descriptor_header_v1;

typedef struct {
    uint64_t total_count;
    uint64_t origin_bit;
    uint32_t rank;
    uint32_t flags;
    /* `rank` uint64_t extents, then `rank` int64_t bit strides follow. */
} n00b_abi_view_descriptor_header_v1;

typedef struct {
    uint32_t handler_function_index;
    uint32_t callback_type_index;
    void *environment;
} n00b_abi_callback_value_v1;

typedef union {
    uint64_t inline_bits;
    void *out_of_line;
} n00b_abi_box_payload_v1;

typedef struct {
    uint16_t array_extent;
    uint16_t cached_byte_stride;
    uint32_t type_field;
    n00b_abi_box_payload_v1 payload;
} n00b_abi_box_v1;

/* `first` is an element ordinal in the global U32/U64 stream, or a byte
 * offset in the byte stream. The containing field selects the stream. */
typedef struct { uint32_t first, count; } n00b_abi_range_v1;

#define N00B_ABI_V1_CONTAINER_VERSION 1u
#define N00B_ABI_V1_TABLE_SCHEMA_VERSION 1u
#define N00B_ABI_V1_PROFILE_ID 1u
#define N00B_ABI_V1_MAGIC "N00BMETA"
#define N00B_ABI_V1_NO_TYPE_INDEX UINT32_MAX
#define N00B_ABI_CAP_WRAPPING (UINT64_C(1) << 0)
#define N00B_ABI_CAP_DEBUG (UINT64_C(1) << 1)
#define N00B_ABI_CAP_OPTIMIZED (UINT64_C(1) << 2)
#define N00B_ABI_CAP_SANITIZED (UINT64_C(1) << 3)
#define N00B_ABI_CPU_NATIVE_ATOMIC128 (UINT64_C(1) << 0)
#define N00B_ABI_DATA_LAYOUT_PROFILE_V1 UINT64_C(1)
#define N00B_ABI_FRAME_SENTINEL UINT32_C(0x4230304e)

typedef enum {
    N00B_ABI_TARGET_X86_64_LINUX = 1,
    N00B_ABI_TARGET_ARM64_MACOS = 2,
    N00B_ABI_TARGET_X86_64_WINDOWS = 3,
} n00b_abi_target_v1;

typedef enum {
    N00B_ABI_TABLE_PACKAGE = 1,
    N00B_ABI_TABLE_MODULE,
    N00B_ABI_TABLE_SCC,
    N00B_ABI_TABLE_TYPE,
    N00B_ABI_TABLE_LAYOUT,
    N00B_ABI_TABLE_FIELD,
    N00B_ABI_TABLE_ARM,
    N00B_ABI_TABLE_PACKED_RUN,
    N00B_ABI_TABLE_PHYSICAL_RUN,
    N00B_ABI_TABLE_TRACE,
    N00B_ABI_TABLE_TRACE_CHILD,
    N00B_ABI_TABLE_TRACE_ARM,
    N00B_ABI_TABLE_TAG_EXTRACTOR,
    N00B_ABI_TABLE_FUNCTION,
    N00B_ABI_TABLE_PARAMETER,
    N00B_ABI_TABLE_CODE_RANGE,
    N00B_ABI_TABLE_SCOPE,
    N00B_ABI_TABLE_SYMBOL,
    N00B_ABI_TABLE_NATIVE_BINDING,
    N00B_ABI_TABLE_TRAIT_METHOD,
    N00B_ABI_TABLE_DISPATCH,
    N00B_ABI_TABLE_DISPATCH_ENTRY,
    N00B_ABI_TABLE_FRAME_LAYOUT,
    N00B_ABI_TABLE_SOURCE_FILE,
    N00B_ABI_TABLE_SOURCE_LOCATION,
    N00B_ABI_TABLE_STORAGE,
    N00B_ABI_TABLE_TLS_RELOCATION,
    N00B_ABI_TABLE_STATIC_STRING,
    N00B_ABI_TABLE_NAME_SLOT,
    N00B_ABI_TABLE_REQUIREMENT,
    N00B_ABI_TABLE_INDEX_PATCH,
    N00B_ABI_TABLE_U32_STREAM,
    N00B_ABI_TABLE_BYTE_STREAM,
    N00B_ABI_TABLE_U64_STREAM,
    N00B_ABI_TABLE_CONSTRAINT,
    N00B_ABI_TABLE_SHAPE,
    N00B_ABI_TABLE_ENUM_ITEM,
    N00B_ABI_TABLE_RUNTIME_CONFIG,
    N00B_ABI_TABLE_CALL_SIGNATURE,
    N00B_ABI_TABLE_SPECIALIZATION_LINK,
    N00B_ABI_TABLE_TYPE_ARGUMENT,
    N00B_ABI_TABLE_CALL_SHAPE,
} n00b_abi_table_kind_v1;

typedef enum {
    N00B_ABI_TYPE_NIL = 0,
    N00B_ABI_TYPE_PRIMITIVE = 1,
    N00B_ABI_TYPE_RECORD,
    N00B_ABI_TYPE_VARIANT,
    N00B_ABI_TYPE_ENUM,
    N00B_ABI_TYPE_FLAGSET,
    N00B_ABI_TYPE_REFERENCE,
    N00B_ABI_TYPE_OPTION,
    N00B_ABI_TYPE_ARRAY,
    N00B_ABI_TYPE_VIEW,
    N00B_ABI_TYPE_STRING,
    N00B_ABI_TYPE_BOX,
    N00B_ABI_TYPE_CALLBACK,
    N00B_ABI_TYPE_FUNCTION,
    N00B_ABI_TYPE_TRAIT,
    N00B_ABI_TYPE_WRAPPER,
    N00B_ABI_TYPE_UNION,
    N00B_ABI_TYPE_ATOMIC,
    N00B_ABI_TYPE_VARIABLE,
    N00B_ABI_TYPE_NAMED,
    N00B_ABI_TYPE_TUPLE,
} n00b_abi_type_kind_v1;

typedef enum {
    N00B_ABI_TYPE_ARG_TYPE = 1,
    N00B_ABI_TYPE_ARG_UINT,
    N00B_ABI_TYPE_ARG_SINT,
    N00B_ABI_TYPE_ARG_STRING,
} n00b_abi_type_arg_kind_v1;

typedef enum {
    N00B_ABI_TYPE_FLAG_SIGNED = 1u << 0,
    N00B_ABI_TYPE_FLAG_OPAQUE = 1u << 1,
    N00B_ABI_TYPE_FLAG_PACKABLE = 1u << 2,
    N00B_ABI_TYPE_FLAG_NON_CONCRETE = 1u << 3,
    N00B_ABI_TYPE_FLAG_BITS = 1u << 4,
} n00b_abi_type_flag_v1;

typedef enum {
    N00B_ABI_LAYOUT_DEFAULT = 0,
    N00B_ABI_LAYOUT_ORDERED = 1,
    N00B_ABI_LAYOUT_C = 2,
} n00b_abi_layout_policy_v1;

typedef enum {
    N00B_ABI_ENUM_ITEM_FLAG_PRIMITIVE = 1u << 0,
} n00b_abi_enum_item_flag_v1;

typedef enum {
    N00B_ABI_VISIBILITY_PRIVATE = 1,
    N00B_ABI_VISIBILITY_PUBLIC = 2,
    N00B_ABI_VISIBILITY_PACKAGE = 3,
} n00b_abi_visibility_v1;

typedef enum {
    N00B_ABI_SYMBOL_FLAG_EXPORTED = 1u << 0,
} n00b_abi_symbol_flag_v1;

typedef enum {
    N00B_ABI_FUNCTION_FLAG_WRAPPABLE = 1u << 0,
} n00b_abi_function_flag_v1;

typedef enum {
    N00B_ABI_SYMBOL_VARIABLE = 1,
    N00B_ABI_SYMBOL_FUNCTION = 2,
    N00B_ABI_SYMBOL_TYPE = 3,
    N00B_ABI_SYMBOL_VARIANT_ARM = 4,
    N00B_ABI_SYMBOL_ENUM_ITEM = 5,
    N00B_ABI_SYMBOL_LABEL = 6,
    N00B_ABI_SYMBOL_PARAMETER = 7,
    N00B_ABI_SYMBOL_MODULE = 8,
    N00B_ABI_SYMBOL_FIELD = 9,
} n00b_abi_symbol_kind_v1;

typedef enum {
    N00B_ABI_NATIVE_SECTION_OFFSET = 1,
    N00B_ABI_NATIVE_EXTERNAL_NAME = 2,
} n00b_abi_native_binding_kind_v1;

typedef enum {
    N00B_ABI_CONSTRAINT_UNIFIES = 1,
    N00B_ABI_CONSTRAINT_ONE_OF = 2,
    N00B_ABI_CONSTRAINT_IMPLEMENTS = 3,
    N00B_ABI_CONSTRAINT_HAS_FIELD = 4,
    N00B_ABI_CONSTRAINT_HAS_PARAM = 5,
    N00B_ABI_CONSTRAINT_PROMOTES = 6,
    N00B_ABI_CONSTRAINT_EXCLUDES = 7,
    N00B_ABI_CONSTRAINT_METHOD = 8,
    N00B_ABI_CONSTRAINT_TYPE_FAMILY = 9,
} n00b_abi_constraint_kind_v1;

typedef enum {
    N00B_ABI_CONSTRAINT_FLAG_HAS_SCOPE = 1u << 0,
} n00b_abi_constraint_flag_v1;

typedef enum {
    N00B_ABI_FAMILY_NONE = 0,
    N00B_ABI_FAMILY_NAMED = 1,
    N00B_ABI_FAMILY_BITINT = 2,
    N00B_ABI_FAMILY_INDIRECTION = 3,
    N00B_ABI_FAMILY_SEQUENCE = 4,
    N00B_ABI_FAMILY_RECORD = 5,
    N00B_ABI_FAMILY_FUNCTION = 6,
    N00B_ABI_FAMILY_BINDER = 7,
    N00B_ABI_FAMILY_BOUND = 8,
    N00B_ABI_FAMILY_SUM = 9,
    N00B_ABI_FAMILY_VAR = 10,
    N00B_ABI_FAMILY_TRAIT = 11,
} n00b_abi_type_family_v1;

typedef enum {
    N00B_ABI_BINDING_NOT_APPLICABLE = 0,
    N00B_ABI_BINDING_VAR = 1,
    N00B_ABI_BINDING_LET = 2,
    N00B_ABI_BINDING_CONST = 3,
} n00b_abi_binding_kind_v1;

typedef enum {
    N00B_ABI_PHASE_COMPTIME = 1u << 0,
    N00B_ABI_PHASE_RUNTIME = 1u << 1,
} n00b_abi_phase_v1;

typedef enum {
    N00B_ABI_DURATION_UNSPECIFIED = 0,
    N00B_ABI_DURATION_COMPTIME = 1,
    N00B_ABI_DURATION_AUTOMATIC = 2,
    N00B_ABI_DURATION_MODULE = 3,
    N00B_ABI_DURATION_THREAD = 4,
    N00B_ABI_DURATION_DYNAMIC = 5,
} n00b_abi_storage_duration_v1;

typedef enum {
    N00B_ABI_PASS_IN = 0,
    N00B_ABI_PASS_OUT = 1,
    N00B_ABI_PASS_MODIFY = 2,
    N00B_ABI_PASS_CONSUME = 3,
} n00b_abi_parameter_mode_v1;

typedef enum {
    N00B_ABI_PARAMETER_FLAG_VARIADIC = 1u << 0,
} n00b_abi_parameter_flag_v1;

typedef enum {
    N00B_ABI_PATCH_INDEX = 1,
    N00B_ABI_PATCH_BOX_TYPE = 2,
} n00b_abi_patch_kind_v1;

typedef enum {
    N00B_ABI_STORAGE_READ_ONLY = 1,
    N00B_ABI_STORAGE_WRITABLE = 2,
    N00B_ABI_STORAGE_TLS = 3,
} n00b_abi_storage_kind_v1;

typedef struct {
    uint8_t magic[8];
    uint32_t container_version;
    uint32_t header_bytes;
    uint32_t target_id;
    uint32_t abi_profile_id;
    uint64_t feature_bits;
    uint64_t artifact_capabilities;
    uint64_t cpu_profile;
    uint64_t data_layout_profile;
    uint32_t required_alignment;
    uint32_t directory_count;
    uint64_t directory_offset;
    uint64_t total_extent;
    uint64_t payload_checksum;
} n00b_abi_header_v1;

typedef struct {
    uint32_t table_kind;
    uint32_t schema_version;
    uint32_t record_bytes;
    uint32_t record_count;
    uint64_t file_offset;
    uint64_t byte_length;
} n00b_abi_directory_v1;

typedef struct {
    n00b_abi_string_v1 name;
    uint32_t parent_package_index;
    uint32_t visibility;
    uint32_t generation;
    uint32_t reserved_zero;
    n00b_abi_range_v1 modules;
} n00b_abi_package_v1;

typedef struct {
    n00b_abi_string_v1 full_path;
    uint32_t package_index;
    uint32_t scc_index;
    uint32_t comptime_initializer;
    uint32_t runtime_initializer;
    n00b_abi_range_v1 imports;
    n00b_abi_range_v1 static_storage;
    n00b_abi_range_v1 tls_storage;
    uint32_t runtime_config_index;
    uint32_t source_file_index;
} n00b_abi_module_v1;

typedef struct {
    uint64_t interface_identity;
    uint64_t freshness_identity;
    n00b_abi_range_v1 members;
    n00b_abi_range_v1 lexical_order;
} n00b_abi_scc_v1;

typedef struct {
    uint64_t typehash;
    uint64_t size_bits;
    n00b_abi_string_v1 canonical_name;
    uint32_t kind;
    uint32_t flags;
    uint32_t alignment_bytes;
    uint32_t layout_index;
    uint32_t owner_module_index;
    uint32_t tracing_index;
    n00b_abi_range_v1 components;
    n00b_abi_range_v1 fields_or_arms;
    n00b_abi_range_v1 constraints;
    uint32_t semantic_width_bits;
    uint32_t unpacked_storage_bits;
    uint32_t call_signature_index;
    uint32_t typevar_owner_scope_index;
    uint32_t typevar_ordinal;
    uint32_t specialization_start_ix;
    uint32_t specialization_end_ix;
    /* Link-only: identifies the imported function requirement that owns this
     * variable. Zero in a consolidated image and on locally owned types. */
    uint32_t typevar_import_requirement_index;
    n00b_abi_string_v1 named_constructor;
    n00b_abi_range_v1 named_arguments; /* Direct TYPE_ARGUMENT row span. */
} n00b_abi_type_v1;

typedef struct {
    uint32_t owner_type_index;
    uint32_t ordinal;
    uint32_t kind; /* n00b_abi_type_arg_kind_v1 */
    uint32_t type_index; /* Only for TYPE; nil is index zero. */
    uint64_t scalar_bits; /* UINT or two's-complement SINT. */
    n00b_abi_string_v1 string_value;
} n00b_abi_type_argument_v1;

/* Fixed-width table row; zero is the reserved row, never a parent type. */
typedef uint32_t n00b_abi_specialization_link_v1;

/* Two's-complement 128-bit bit pattern, low word first. The parent type
 * supplies signedness and semantic width. Composite flag names add no bits. */
typedef struct {
    n00b_abi_string_v1 name;
    uint32_t parent_type_index;
    uint32_t declaration_ordinal;
    uint64_t value_low;
    uint64_t value_high;
    uint32_t flags;
    uint32_t symbol_index;
} n00b_abi_enum_item_v1;

typedef struct {
    n00b_abi_string_v1 name;
    n00b_abi_string_v1 scope_module_name;
    n00b_abi_string_v1 scope_package_name;
    uint32_t kind;
    uint32_t flags;
    uint32_t variable_ordinal;
    uint32_t target_type_index;
    uint32_t result_type_index;
    uint32_t call_shape_index;
    uint32_t type_family;
    uint32_t reserved_zero;
    uint64_t parameter_index;
    n00b_abi_range_v1 arguments;
} n00b_abi_constraint_v1;

typedef struct {
    uint64_t total_count;
    uint32_t rank;
    uint32_t flat_rank;
    n00b_abi_range_v1 extents;
} n00b_abi_shape_v1;

typedef struct {
    uint32_t type_index;
    uint32_t policy; /* n00b_abi_layout_policy_v1 */
    /* U32-stream indexes into PHYSICAL_RUN. Occupied runs union all arms. */
    n00b_abi_range_v1 occupied_runs;
    n00b_abi_range_v1 reserved_runs;
    uint32_t tag_extractor_index;
    uint32_t reserved_zero;
} n00b_abi_layout_v1;

typedef struct {
    uint64_t unit_byte_offset;
    uint32_t unit_width_bits;
    uint32_t lsb_position;
    uint32_t width_bits;
    uint32_t reserved_zero;
} n00b_abi_packed_run_v1;

typedef struct {
    uint64_t byte_offset;
    uint32_t bit_offset;
    uint32_t width_bits;
} n00b_abi_physical_run_v1;

typedef struct {
    n00b_abi_string_v1 name;
    uint32_t parent_type_index;
    uint32_t child_type_index;
    uint32_t declaration_ordinal;
    uint32_t visibility;
    uint32_t symbol_index;
    uint32_t reserved_zero;
    uint64_t logical_bit_offset;
    n00b_abi_range_v1 packed_runs;
    n00b_abi_range_v1 physical_runs;
} n00b_abi_field_v1;

typedef struct {
    n00b_abi_field_v1 field;
    uint32_t tag_value;
    uint32_t reserved_zero;
} n00b_abi_arm_v1;

typedef enum {
    N00B_ABI_TAG_DIRECT_BITS = 1,
    N00B_ABI_TAG_POINTER_NULL = 2,
    N00B_ABI_TAG_BITPTR_DEPTH = 3,
} n00b_abi_tag_kind_v1;

typedef struct {
    uint64_t unit_byte_offset;
    uint64_t mask;
    uint32_t unit_width_bits;
    uint32_t lsb_position;
    uint32_t tag_width_bits;
    uint32_t kind; /* n00b_abi_tag_kind_v1 */
} n00b_abi_tag_extractor_v1;

typedef struct {
    uint32_t type_index;
    uint32_t reserved_zero;
    uint32_t child_trace_index;
    uint32_t tag_extractor_index;
    n00b_abi_range_v1 fixed_children;
    n00b_abi_range_v1 arms;
    uint64_t option_bit_offset;
} n00b_abi_trace_v1;

typedef struct {
    uint64_t byte_offset;
    uint32_t child_type_index;
    uint32_t reserved_zero;
} n00b_abi_trace_child_v1;

typedef struct {
    uint32_t tag_value;
    uint32_t child_trace_index;
    n00b_abi_range_v1 children;
} n00b_abi_trace_arm_v1;

typedef struct {
    n00b_abi_string_v1 name;
    uint32_t module_index;
    uint32_t scope_index;
    uint32_t symbol_index;
    uint32_t success_type_index;
    uint32_t failure_type_index;
    /* U32-stream indexes into the parameter table. `keywords` is a subset
     * in declaration order; its ordinal is the omission bitmap bit. */
    n00b_abi_range_v1 parameters;
    n00b_abi_range_v1 keywords;
    n00b_abi_range_v1 constraints;
    n00b_abi_range_v1 code_ranges;
    n00b_abi_range_v1 frame_layouts;
    uint32_t flags;
    uint32_t variadic_type_index;
    uint32_t reserved_zero;
    uint64_t valid_keyword_bits;
} n00b_abi_function_v1;

typedef struct {
    uint32_t type_index;
    uint32_t success_type_index;
    uint32_t failure_type_index;
    uint32_t rest_parameter_index;
    n00b_abi_range_v1 positional_parameters;
    n00b_abi_range_v1 keyword_parameters;
} n00b_abi_call_signature_v1;

/* A retained METHOD obligation's call pattern, not a callable TYPE.
 * NO_TYPE_INDEX means its failure channel remains unconstrained. */
typedef struct {
    uint32_t success_type_index;
    uint32_t failure_type_index;
    uint32_t rest_parameter_index;
    uint32_t reserved_zero;
    n00b_abi_range_v1 positional_parameters;
    n00b_abi_range_v1 keyword_parameters;
} n00b_abi_call_shape_v1;

typedef struct {
    n00b_abi_string_v1 name;
    uint32_t type_index;
    uint32_t passing_mode;
    uint32_t declaration_ordinal;
    uint32_t flags;
    uint64_t keyword_block_bit_offset;
} n00b_abi_parameter_v1;

typedef struct {
    uint32_t function_index;
    uint32_t generation;
    uint32_t phase_mask;
    uint32_t reserved_zero;
    uint64_t byte_length;
    const void *start; /* Native relocation; never a serialized process VA. */
} n00b_abi_code_range_v1;

typedef struct {
    n00b_abi_string_v1 kind_name;
    uint32_t module_index;
    uint32_t function_index;
    uint32_t parent_scope_index;
    uint32_t source_location_index;
    uint32_t flags;
    uint32_t reserved_zero;
    n00b_abi_range_v1 children;
    n00b_abi_range_v1 symbols;
} n00b_abi_scope_v1;

typedef struct {
    n00b_abi_string_v1 name;
    uint32_t kind;
    uint32_t type_index;
    uint32_t module_index;
    uint32_t scope_index;
    uint32_t visibility;
    uint32_t flags;
    uint32_t binding_kind;
    uint32_t phase_mask;
    uint32_t mutable_phase_mask;
    uint32_t initializer_phase_mask;
    uint32_t storage_duration;
    uint32_t reserved_zero;
    n00b_abi_string_v1 storage_class_name;
    uint32_t declaration_location_index;
    uint32_t native_binding_index;
    n00b_abi_range_v1 definitions;
    n00b_abi_range_v1 uses;
} n00b_abi_symbol_v1;

typedef struct {
    n00b_abi_string_v1 native_name;
    uint32_t kind;
    uint32_t section_index;
    int64_t offset;
    uint32_t flags;
    uint32_t reserved_zero;
} n00b_abi_native_binding_v1;

typedef struct {
    uint32_t trait_type_index;
    uint32_t declaration_ordinal;
    uint32_t signature_type_index;
    uint32_t symbol_index;
} n00b_abi_trait_method_v1;

typedef struct {
    uint32_t trait_type_index;
    uint32_t method_ordinal;
    uint32_t generation;
    uint32_t reserved_zero;
    n00b_abi_range_v1 entries;
} n00b_abi_dispatch_v1;

typedef struct {
    uint32_t receiver_type_index;
    uint32_t function_index;
} n00b_abi_dispatch_entry_v1;

typedef struct {
    uint32_t function_index;
    uint32_t shadow_frame_type_index;
    n00b_abi_range_v1 pointer_bitmap_bytes;
    uint64_t frame_byte_extent; /* FP-relative managed region below FP. */
} n00b_abi_frame_layout_v1;

typedef struct {
    n00b_abi_string_v1 path;
    uint64_t source_identity;
} n00b_abi_source_file_v1;

typedef struct {
    uint32_t file_index;
    uint32_t start_line;
    uint32_t start_column;
    uint32_t end_line;
    uint32_t end_column;
    uint32_t reserved_zero;
    uint64_t start_byte_offset;
    uint64_t end_byte_offset; /* Exclusive. */
} n00b_abi_source_location_v1;

typedef struct {
    uint32_t module_index;
    uint32_t symbol_index;
    uint32_t type_index;
    uint32_t segment_kind;
    uint32_t segment_index;
    uint32_t instance_shape_index;
    uint64_t byte_offset;
    uint64_t byte_length;
    n00b_abi_range_v1 tls_relocations;
} n00b_abi_storage_v1;

typedef struct {
    uint32_t template_storage_index;
    uint32_t referenced_storage_index;
    uint64_t write_byte_offset;
    int64_t addend_bytes;
    uint32_t target_kind;
    uint32_t reserved_zero;
} n00b_abi_tls_relocation_v1;

typedef struct {
    n00b_abi_string_v1 value;
} n00b_abi_static_string_v1;

typedef struct {
    n00b_abi_string_v1 memory_manager_name;
} n00b_abi_runtime_config_v1;

typedef struct {
    uint64_t name_hash;
    uint32_t symbol_index;
    uint32_t ambiguous; /* One when the path names multiple functions. */
} n00b_abi_name_slot_v1;

/* Link-only. Neither record appears in a fully resolved final image. */
typedef struct {
    n00b_abi_string_v1 full_path;
    uint32_t symbol_kind;
    uint32_t contract_type_index;
    uint32_t required_layout_index;
    uint32_t required_phase_mask;
    uint32_t requesting_module_index;
    uint32_t reserved_zero;
    uint64_t required_capabilities;
} n00b_abi_requirement_v1;

typedef struct {
    uint32_t target_section_index;
    uint32_t referenced_table_kind;
    uint64_t target_byte_offset;
    uint32_t local_index;
    uint32_t patch_kind;
    uint32_t reserved_zero;
    uint32_t reserved_zero_2;
} n00b_abi_index_patch_v1;

/* A final image exposes one of these to its loader. It is relocated before
 * registration. The compiler/linker validates a main image's file checksum;
 * the n00b loader validates a dynamic image's file before native loading. */
typedef struct {
    uint32_t descriptor_version;
    uint32_t reserved_zero;
    const n00b_abi_header_v1 *metadata;
    uint64_t metadata_bytes;
} n00b_abi_image_descriptor_v1;

/* Immutable merged view. Its indexed records contain snapshot-global indexes
 * even when copied from an image with different local indexes. Old views
 * and their record pointers remain valid for the process lifetime; dynamic
 * load publishes a new generation. A
 * query returning int32_t returns 1 when found, 0 when absent, and -1 for
 * invalid arguments or metadata. Query functions never print diagnostics. */
typedef struct n00b_abi_snapshot_v1 n00b_abi_snapshot_v1;

typedef enum {
    N00B_ABI_OK = 0,
    N00B_ABI_BAD_ARGUMENT = 1,
    N00B_ABI_BAD_METADATA = 2,
    N00B_ABI_INCOMPATIBLE = 3,
    N00B_ABI_UNRESOLVED = 4,
    N00B_ABI_AMBIGUOUS = 5,
} n00b_abi_status_v1;

typedef struct {
    uint32_t status;
    uint32_t table_kind;
    uint32_t record_index;
    uint32_t reserved_zero;
    uint64_t byte_offset;
} n00b_abi_error_v1;

/* Registration is transactional and does not write stdout/stderr. The
 * loader reports a nonzero status through its own diagnostics. */
uint32_t n00b_abi_v1_register_image(
    const n00b_abi_image_descriptor_v1 *image,
    uint64_t *published_generation, n00b_abi_error_v1 *error);

const n00b_abi_snapshot_v1 *n00b_abi_v1_current_snapshot(void);
uint64_t n00b_abi_v1_generation(const n00b_abi_snapshot_v1 *snapshot);
uint32_t n00b_abi_v1_table_count(const n00b_abi_snapshot_v1 *snapshot,
                                 uint32_t table_kind);
const void *n00b_abi_v1_record(const n00b_abi_snapshot_v1 *snapshot,
                               uint32_t table_kind, uint32_t index,
                               uint32_t *record_bytes);
int32_t n00b_abi_v1_index_at(const n00b_abi_snapshot_v1 *snapshot,
                             n00b_abi_range_v1 range, uint32_t ordinal,
                             uint32_t *index_out);
int32_t n00b_abi_v1_u64_at(const n00b_abi_snapshot_v1 *snapshot,
                           n00b_abi_range_v1 range, uint32_t ordinal,
                           uint64_t *value_out);
int32_t n00b_abi_v1_bytes(const n00b_abi_snapshot_v1 *snapshot,
                          n00b_abi_range_v1 range,
                          const uint8_t **data_out);
/* For an inline string the returned bytes may alias `value`; retain that
 * record or value for as long as the returned pointer is used. */
int32_t n00b_abi_v1_string_bytes(const n00b_abi_snapshot_v1 *snapshot,
                                 const n00b_abi_string_v1 *value,
                                 const uint8_t **data_out,
                                 uint64_t *byte_count_out);

/* One unformatted write of at most INT32_MAX bytes. Returns bytes written,
 * including a possible short write, or -1 on failure. No libc is required. */
int32_t n00b_abi_v1_raw_fd_write(int32_t fd, n00b_abi_string_v1 text);

void *n00b_allocate(uint32_t allocator, uint32_t type_index,
                    uint64_t byte_size, uint32_t alignment,
                    const void *instance_descriptor);

#if defined(_WIN32)
/* Register a live Windows handle as a nonstandard n00b descriptor. The
 * caller retains ownership and must unregister before closing the handle. */
int32_t n00b_abi_v1_fd_register_handle(void *handle);
/* Returns 1 if removed, 0 if absent, -1 for a standard/invalid descriptor. */
int32_t n00b_abi_v1_fd_unregister_handle(int32_t fd);
#endif

const n00b_abi_type_v1 *n00b_abi_v1_type(
    const n00b_abi_snapshot_v1 *snapshot, uint32_t type_index);
const n00b_abi_type_argument_v1 *n00b_abi_v1_type_argument(
    const n00b_abi_snapshot_v1 *snapshot, uint32_t argument_index);
/* Follows transitive specialization links. Returns 1, 0, or -1. */
int32_t n00b_abi_v1_type_specializes(
    const n00b_abi_snapshot_v1 *snapshot, uint32_t type_index,
    uint32_t general_type_index);
const n00b_abi_package_v1 *n00b_abi_v1_package(
    const n00b_abi_snapshot_v1 *snapshot, uint32_t package_index);
const n00b_abi_module_v1 *n00b_abi_v1_module(
    const n00b_abi_snapshot_v1 *snapshot, uint32_t module_index);
const n00b_abi_runtime_config_v1 *n00b_abi_v1_runtime_config(
    const n00b_abi_snapshot_v1 *snapshot, uint32_t config_index);
const n00b_abi_layout_v1 *n00b_abi_v1_layout(
    const n00b_abi_snapshot_v1 *snapshot, uint32_t layout_index);
const n00b_abi_field_v1 *n00b_abi_v1_field(
    const n00b_abi_snapshot_v1 *snapshot, uint32_t field_index);
const n00b_abi_function_v1 *n00b_abi_v1_function(
    const n00b_abi_snapshot_v1 *snapshot, uint32_t function_index);
const n00b_abi_call_signature_v1 *n00b_abi_v1_call_signature(
    const n00b_abi_snapshot_v1 *snapshot, uint32_t signature_index);
const n00b_abi_call_shape_v1 *n00b_abi_v1_call_shape(
    const n00b_abi_snapshot_v1 *snapshot, uint32_t shape_index);
const n00b_abi_symbol_v1 *n00b_abi_v1_symbol(
    const n00b_abi_snapshot_v1 *snapshot, uint32_t symbol_index);
const n00b_abi_scope_v1 *n00b_abi_v1_scope(
    const n00b_abi_snapshot_v1 *snapshot, uint32_t scope_index);
const n00b_abi_source_location_v1 *n00b_abi_v1_source_location(
    const n00b_abi_snapshot_v1 *snapshot, uint32_t location_index);
const n00b_abi_code_range_v1 *n00b_abi_v1_code_range(
    const n00b_abi_snapshot_v1 *snapshot, uint32_t code_range_index);
const n00b_abi_frame_layout_v1 *n00b_abi_v1_frame_layout(
    const n00b_abi_snapshot_v1 *snapshot, uint32_t frame_index);
const n00b_abi_trace_v1 *n00b_abi_v1_trace(
    const n00b_abi_snapshot_v1 *snapshot, uint32_t trace_index);
const n00b_abi_storage_v1 *n00b_abi_v1_storage(
    const n00b_abi_snapshot_v1 *snapshot, uint32_t storage_index);

/* Returns only concrete type rows; non-concrete shape hashes are not unique. */
int32_t n00b_abi_v1_find_typehash(const n00b_abi_snapshot_v1 *snapshot,
                                  uint64_t typehash, uint32_t *type_index);
int32_t n00b_abi_v1_find_module(const n00b_abi_snapshot_v1 *snapshot,
                                n00b_abi_string_v1 full_path,
                                uint32_t *module_index);
/* Returns -1 for an ambiguous name or invalid input, 0 for no match. */
int32_t n00b_abi_v1_find_symbol(const n00b_abi_snapshot_v1 *snapshot,
                                n00b_abi_string_v1 full_path,
                                uint32_t *symbol_index);
int32_t n00b_abi_v1_resolve_trait_method(
    const n00b_abi_snapshot_v1 *snapshot, uint32_t trait_type_index,
    uint32_t method_ordinal, uint32_t receiver_type_index,
    uint32_t *function_index);

_Static_assert(sizeof(n00b_abi_string_v1) == 16, "string ABI width");
_Static_assert(_Alignof(n00b_abi_string_v1) == 8, "string ABI alignment");
_Static_assert(offsetof(n00b_abi_string_v1, short_value.bytes) == 0,
               "short string bytes offset");
_Static_assert(offsetof(n00b_abi_string_v1, short_value.selector) == 15,
               "short string selector offset");
_Static_assert(offsetof(n00b_abi_string_v1, long_value.data) == 0,
               "long string pointer offset");
_Static_assert(offsetof(n00b_abi_string_v1, long_value.selector) == 8,
               "long string selector offset");
_Static_assert(offsetof(n00b_abi_string_v1, span_value.data) == 0,
               "span string pointer offset");
_Static_assert(offsetof(n00b_abi_string_v1, rich_value.body) == 0,
               "rich string pointer offset");
_Static_assert(offsetof(n00b_abi_string_v1, words[1]) == 8,
               "string selector offset");
_Static_assert(sizeof(n00b_abi_bitptr_v1) == 16, "bitptr width");
_Static_assert(_Alignof(n00b_abi_bitptr_v1) == 8, "bitptr alignment");
_Static_assert(offsetof(n00b_abi_bitptr_v1, type_index) == 8,
               "bitptr type offset");
_Static_assert(sizeof(n00b_abi_array_value_v1) == 16, "array value width");
_Static_assert(_Alignof(n00b_abi_array_value_v1) == 8,
               "array value alignment");
_Static_assert(sizeof(n00b_abi_array_descriptor_header_v1) == 24,
               "array descriptor header width");
_Static_assert(offsetof(n00b_abi_array_descriptor_header_v1, flat_rank) == 12,
               "array flat-rank offset");
_Static_assert(sizeof(n00b_abi_view_descriptor_header_v1) == 24,
               "view descriptor header width");
_Static_assert(offsetof(n00b_abi_view_descriptor_header_v1, origin_bit) == 8,
               "view origin offset");
_Static_assert(sizeof(n00b_abi_callback_value_v1) == 16, "callback width");
_Static_assert(offsetof(n00b_abi_callback_value_v1, environment) == 8,
               "callback environment offset");
_Static_assert(sizeof(n00b_abi_box_v1) == 16, "box width");
_Static_assert(_Alignof(n00b_abi_box_v1) == 8, "box alignment");
_Static_assert(offsetof(n00b_abi_box_v1, type_field) == 4,
               "box type offset");
_Static_assert(offsetof(n00b_abi_box_v1, payload) == 8,
               "box payload offset");
_Static_assert(sizeof(n00b_abi_range_v1) == 8, "range width");
_Static_assert(sizeof(n00b_abi_header_v1) == 88, "header width");
_Static_assert(sizeof(n00b_abi_directory_v1) == 32, "directory width");
_Static_assert(sizeof(n00b_abi_package_v1) == 40, "package width");
_Static_assert(sizeof(n00b_abi_module_v1) == 64, "module width");
_Static_assert(offsetof(n00b_abi_module_v1, source_file_index) == 60,
               "module source-file offset");
_Static_assert(sizeof(n00b_abi_scc_v1) == 32, "SCC width");
_Static_assert(sizeof(n00b_abi_type_v1) == 136, "type width");
_Static_assert(sizeof(n00b_abi_type_argument_v1) == 40,
               "type argument width");
_Static_assert(sizeof(n00b_abi_specialization_link_v1) == 4,
               "specialization link width");
_Static_assert(offsetof(n00b_abi_type_v1, call_signature_index) == 88,
               "type signature offset");
_Static_assert(offsetof(n00b_abi_type_v1, typevar_owner_scope_index) == 92,
               "type-variable owner offset");
_Static_assert(offsetof(n00b_abi_type_v1, specialization_start_ix) == 100,
               "type specialization range offset");
_Static_assert(offsetof(n00b_abi_type_v1, specialization_end_ix) == 104,
               "type specialization end offset");
_Static_assert(sizeof(n00b_abi_call_signature_v1) == 32,
               "call signature width");
_Static_assert(offsetof(n00b_abi_call_signature_v1,
                        positional_parameters) == 16,
               "call signature parameter offset");
_Static_assert(sizeof(n00b_abi_call_shape_v1) == 32,
               "call shape width");
_Static_assert(sizeof(n00b_abi_enum_item_v1) == 48, "enum item width");
_Static_assert(sizeof(n00b_abi_constraint_v1) == 96, "constraint width");
_Static_assert(sizeof(n00b_abi_shape_v1) == 24, "shape width");
_Static_assert(sizeof(n00b_abi_layout_v1) == 32, "layout width");
_Static_assert(sizeof(n00b_abi_packed_run_v1) == 24, "packed run width");
_Static_assert(offsetof(n00b_abi_packed_run_v1, width_bits) == 16,
               "packed run width offset");
_Static_assert(sizeof(n00b_abi_physical_run_v1) == 16, "physical run width");
_Static_assert(sizeof(n00b_abi_field_v1) == 64, "field width");
_Static_assert(sizeof(n00b_abi_arm_v1) == 72, "arm width");
_Static_assert(sizeof(n00b_abi_tag_extractor_v1) == 32, "tag width");
_Static_assert(offsetof(n00b_abi_tag_extractor_v1, kind) == 28,
               "tag kind offset");
_Static_assert(sizeof(n00b_abi_trace_v1) == 40, "trace width");
_Static_assert(sizeof(n00b_abi_trace_child_v1) == 16, "trace child width");
_Static_assert(sizeof(n00b_abi_trace_arm_v1) == 16, "trace arm width");
_Static_assert(sizeof(n00b_abi_function_v1) == 96, "function width");
_Static_assert(sizeof(n00b_abi_parameter_v1) == 40, "parameter width");
_Static_assert(sizeof(n00b_abi_code_range_v1) == 32, "code range width");
_Static_assert(sizeof(n00b_abi_scope_v1) == 56, "scope width");
_Static_assert(sizeof(n00b_abi_symbol_v1) == 104, "symbol width");
_Static_assert(sizeof(n00b_abi_native_binding_v1) == 40, "binding width");
_Static_assert(sizeof(n00b_abi_trait_method_v1) == 16, "method width");
_Static_assert(sizeof(n00b_abi_dispatch_v1) == 24, "dispatch width");
_Static_assert(sizeof(n00b_abi_dispatch_entry_v1) == 8, "dispatch entry width");
_Static_assert(sizeof(n00b_abi_frame_layout_v1) == 24, "frame width");
_Static_assert(sizeof(n00b_abi_source_file_v1) == 24, "source file width");
_Static_assert(sizeof(n00b_abi_source_location_v1) == 40, "source location width");
_Static_assert(offsetof(n00b_abi_source_location_v1, start_byte_offset) == 24,
               "source start byte offset");
_Static_assert(offsetof(n00b_abi_source_location_v1, end_byte_offset) == 32,
               "source end byte offset");
_Static_assert(sizeof(n00b_abi_storage_v1) == 48, "storage width");
_Static_assert(sizeof(n00b_abi_tls_relocation_v1) == 32, "TLS relocation width");
_Static_assert(sizeof(n00b_abi_static_string_v1) == 16, "static string width");
_Static_assert(sizeof(n00b_abi_runtime_config_v1) == 16, "runtime config width");
_Static_assert(sizeof(n00b_abi_name_slot_v1) == 16, "name slot width");
_Static_assert(sizeof(n00b_abi_requirement_v1) == 48, "requirement width");
_Static_assert(sizeof(n00b_abi_index_patch_v1) == 32, "patch width");
_Static_assert(sizeof(n00b_abi_image_descriptor_v1) == 24, "image descriptor width");
_Static_assert(sizeof(n00b_abi_error_v1) == 24, "error width");
_Static_assert(offsetof(n00b_abi_header_v1, artifact_capabilities) == 32,
               "artifact capability offset");
_Static_assert(offsetof(n00b_abi_header_v1, payload_checksum) == 80,
               "header checksum offset");
_Static_assert(offsetof(n00b_abi_requirement_v1, required_capabilities) == 40,
               "requirement capability offset");
_Static_assert(offsetof(n00b_abi_requirement_v1, required_phase_mask) == 28,
               "requirement phase offset");
_Static_assert(offsetof(n00b_abi_requirement_v1, requesting_module_index) == 32,
               "requirement requester offset");
_Static_assert(offsetof(n00b_abi_index_patch_v1, target_byte_offset) == 8,
               "patch target offset");
_Static_assert(offsetof(n00b_abi_field_v1, symbol_index) == 32,
               "field symbol offset");
_Static_assert(offsetof(n00b_abi_function_v1, symbol_index) == 24,
               "function symbol offset");
_Static_assert(offsetof(n00b_abi_function_v1, valid_keyword_bits) == 88,
               "function keyword offset");
_Static_assert(offsetof(n00b_abi_code_range_v1, phase_mask) == 8,
               "code phase offset");
_Static_assert(offsetof(n00b_abi_symbol_v1, phase_mask) == 44,
               "symbol phase offset");
_Static_assert(offsetof(n00b_abi_symbol_v1, storage_class_name) == 64,
               "symbol storage class offset");
_Static_assert(offsetof(n00b_abi_constraint_v1, kind) == 48,
               "constraint kind offset");
_Static_assert(offsetof(n00b_abi_constraint_v1, parameter_index) == 80,
               "constraint parameter offset");
_Static_assert(offsetof(n00b_abi_scope_v1, children) == 40,
               "scope children offset");
_Static_assert(offsetof(n00b_abi_module_v1, runtime_config_index) == 56,
               "module runtime config offset");
_Static_assert(offsetof(n00b_abi_frame_layout_v1, frame_byte_extent) == 16,
               "frame extent offset");
_Static_assert(offsetof(n00b_abi_enum_item_v1, symbol_index) == 44,
               "enum item symbol offset");

#ifdef __cplusplus
}
#endif

#endif
