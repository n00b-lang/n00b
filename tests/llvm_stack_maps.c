#include "llvm_stack_maps.h"
#include "native_frame_maps.h"

#include <llvm-c/Core.h>
#include <llvm-c/Error.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>
#include <llvm-c/Transforms/PassBuilder.h>
#include <stdlib.h>
#include <string.h>

#include "lib/alloc.h"
#include "lib/byteorder.h"
#include "lib/object_section.h"

#define CHECK(condition) do { if (!(condition)) abort(); } while (0)

static bool
check_function(void *context, const n00bb_llvm_stack_function_t *function)
{
    (void)context;
    bool named_probe = false;
    for (size_t i = 0; i + 5 <= function->name.length; i++)
        if (!memcmp(function->name.bytes + i, "probe", 5))
            named_probe = true;
    CHECK(named_probe);
    CHECK(function->code_bytes > 0);
    CHECK(function->stack_bytes >= sizeof(void *));
    return true;
}

static bool
check_location(void *context, const n00bb_llvm_stack_location_t *location)
{
    unsigned *count = context;
    (*count)++;
    CHECK(location->id == 42 || location->id == 43);
    if (location->id == 43)
        CHECK(location->kind == N00BB_LLVM_STACKMAP_REGISTER);
    else CHECK(location->kind == N00BB_LLVM_STACKMAP_DIRECT);
    return true;
}

static void
check_target(const char *triple,
             core_target_object_format_t format)
{
    LLVMContextRef context = LLVMContextCreate();
    LLVMModuleRef module = LLVMModuleCreateWithNameInContext("stack-map-test",
                                                             context);
    LLVMTypeRef pointer = LLVMPointerTypeInContext(context, 0);
    LLVMTypeRef signature = LLVMFunctionType(LLVMVoidTypeInContext(context),
                                            &pointer, 1, false);
    LLVMValueRef function = LLVMAddFunction(module, "frame_map_probe",
                                           signature);
    LLVMAttributeRef frame_pointer = LLVMCreateStringAttribute(context,
        "frame-pointer", sizeof("frame-pointer") - 1,
        "all", sizeof("all") - 1);
    LLVMAddAttributeAtIndex(function, LLVMAttributeFunctionIndex,
                            frame_pointer);
    LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(context, function,
                                                             "entry");
    LLVMBuilderRef builder = LLVMCreateBuilderInContext(context);
    LLVMPositionBuilderAtEnd(builder, entry);
    LLVMValueRef root = LLVMBuildAlloca(builder, pointer, "root");
    LLVMValueRef store = LLVMBuildStore(builder, LLVMGetParam(function, 0),
                                       root);
    LLVMSetVolatile(store, true);
    LLVMValueRef spill = LLVMBuildAlloca(builder,
        LLVMArrayType2(pointer, 64), "spill");
    LLVMValueRef spilled = LLVMBuildStore(builder, LLVMGetParam(function, 0),
                                         spill);
    LLVMSetVolatile(spilled, true);
    LLVMTypeRef parameters[] = {
        LLVMInt64TypeInContext(context), LLVMInt32TypeInContext(context)
    };
    LLVMTypeRef map_type = LLVMFunctionType(LLVMVoidTypeInContext(context),
                                           parameters, 2, true);
    LLVMValueRef map = LLVMAddFunction(module, "llvm.experimental.stackmap",
                                       map_type);
    LLVMValueRef operands[] = {
        LLVMConstInt(parameters[0], 42, false),
        LLVMConstInt(parameters[1], 0, false), root, spill,
    };
    LLVMBuildCall2(builder, map_type, map, operands, 4, "");
    LLVMBuildRetVoid(builder);
    LLVMValueRef register_probe = LLVMAddFunction(module,
        "frame_map_register_probe", signature);
    LLVMAddAttributeAtIndex(register_probe, LLVMAttributeFunctionIndex,
                            frame_pointer);
    LLVMBasicBlockRef register_entry = LLVMAppendBasicBlockInContext(context,
        register_probe, "entry");
    LLVMPositionBuilderAtEnd(builder, register_entry);
    LLVMValueRef register_operands[] = {
        LLVMConstInt(parameters[0], 43, false),
        LLVMConstInt(parameters[1], 0, false),
        LLVMGetParam(register_probe, 0),
    };
    LLVMBuildCall2(builder, map_type, map, register_operands, 3, "");
    LLVMBuildRetVoid(builder);
    LLVMDisposeBuilder(builder);

    LLVMTargetRef target = nullptr;
    char *message = nullptr;
    CHECK(!LLVMGetTargetFromTriple(triple, &target, &message));
    LLVMTargetMachineRef machine = LLVMCreateTargetMachine(target, triple,
        "generic", "", LLVMCodeGenLevelDefault, LLVMRelocDefault,
        LLVMCodeModelDefault);
    LLVMSetTarget(module, triple);
    LLVMTargetDataRef layout = LLVMCreateTargetDataLayout(machine);
    char *layout_text = LLVMCopyStringRepOfTargetData(layout);
    LLVMSetDataLayout(module, layout_text);
    LLVMDisposeMessage(layout_text);
    LLVMDisposeTargetData(layout);
    LLVMPassBuilderOptionsRef options = LLVMCreatePassBuilderOptions();
    LLVMErrorRef error = LLVMRunPasses(module, "default<O2>", machine,
                                      options);
    CHECK(!error);
    LLVMDisposePassBuilderOptions(options);
    LLVMMemoryBufferRef object = nullptr;
    CHECK(!LLVMTargetMachineEmitToMemoryBuffer(machine, module,
        LLVMObjectFile, &message, &object));
    unsigned count = 0;
    bool found = false;
    n00bb_llvm_stack_visitor_t visitor = {
        .function = check_function, .location = check_location,
        .context = &count,
    };
    CHECK(n00bb_llvm_stack_maps_visit(LLVMGetBufferStart(object),
        LLVMGetBufferSize(object), format, &visitor, &found));
    CHECK(found && count == 3);
    core_string_t profile = format == CORE_TARGET_OBJECT_MACH_O
        ? CORE_STRING_STATIC("arm64-macos")
        : format == CORE_TARGET_OBJECT_COFF
            ? CORE_STRING_STATIC("x86_64-windows")
            : CORE_STRING_STATIC("x86_64-linux");
    core_target_desc_t target_desc;
    CHECK(core_target_profile(profile, 0, &target_desc));
    n00bb_native_frames_t *frames = n00bb_native_frames_new(format);
    CHECK(frames && n00bb_native_frames_add(frames,
        LLVMGetBufferStart(object), LLVMGetBufferSize(object)));
    uint8_t *companion = NULL;
    size_t companion_bytes = 0;
    CHECK(n00bb_native_frames_emit(frames, &target_desc,
                                   &companion, &companion_bytes));
    core_object_section_t section;
    CHECK(core_object_section_find(companion, companion_bytes, format,
        format == CORE_TARGET_OBJECT_MACH_O
            ? CORE_STRING_STATIC("__DATA_CONST") : (core_string_t){},
        format == CORE_TARGET_OBJECT_MACH_O
            ? CORE_STRING_STATIC("__n00b_frames")
            : format == CORE_TARGET_OBJECT_COFF
                ? CORE_STRING_STATIC(".n00b$F")
                : CORE_STRING_STATIC(".data.rel.ro.n00b_frames"),
        &section));
    uint32_t rows = core_le_load_u32(section.bytes);
    bool windows = format == CORE_TARGET_OBJECT_COFF;
    CHECK(section.relocation_count == rows);
    CHECK(rows == (windows ? 2u : 1u));
    uint32_t precise = 0;
    bool spread = false;
    for (uint32_t row = 0; row < rows; row++) {
        const uint8_t *frame = section.bytes + 8 + row * 32;
        int32_t record = (int32_t)core_le_load_u32(frame + 20);
        uint32_t flags = core_le_load_u32(frame + 24);
        CHECK(core_le_load_u32(frame + 8) > 0);
        CHECK(windows ? record >= 0 && record % 8 == 0 : record == 0);
        CHECK(flags <= 1 && core_le_load_u32(frame + 28) == 0);
        if (flags) {
            CHECK(windows && core_le_load_u32(frame + 12) == 0);
            continue;
        }
        precise++;
        CHECK(!windows || record > 0);
        CHECK(core_le_load_u32(frame + 12) >= 1);
        bool rooted = false;
        for (uint32_t i = 0; i < core_le_load_u32(frame + 12); i++) {
            const uint8_t *root = section.bytes + 8 + rows * 32
                + (core_le_load_u32(frame + 16) + i) * 8;
            int32_t offset = (int32_t)core_le_load_u32(root);
            CHECK(core_le_load_u32(root + 4) == 8);
            CHECK(offset + 8 <= record || offset >= record + 16);
            rooted = rooted || offset < record;
            spread = spread || offset > 0;
        }
        CHECK(rooted);
    }
    CHECK(precise == 1 && spread == windows);
    core_free(companion);
    n00bb_native_frames_free(frames);
    LLVMDisposeMemoryBuffer(object);
    LLVMDisposeTargetMachine(machine);
    LLVMDisposeModule(module);
    LLVMContextDispose(context);
}

int
main(void)
{
    LLVMInitializeAllTargetInfos();
    LLVMInitializeAllTargets();
    LLVMInitializeAllTargetMCs();
    LLVMInitializeAllAsmPrinters();
    check_target("aarch64-apple-macosx14.0.0", CORE_TARGET_OBJECT_MACH_O);
    check_target("x86_64-pc-linux-gnu", CORE_TARGET_OBJECT_ELF);
    check_target("x86_64-pc-windows-msvc", CORE_TARGET_OBJECT_COFF);
    return 0;
}
