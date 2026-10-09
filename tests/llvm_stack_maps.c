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
check_location(void *context, const n00bb_llvm_stack_location_t *location)
{
    unsigned *count = context;
    (*count)++;
    bool named_probe = false;
    for (size_t i = 0; i + 5 <= location->function.length; i++)
        if (!memcmp(location->function.bytes + i, "probe", 5))
            named_probe = true;
    CHECK(named_probe);
    CHECK(location->id == 42
        || location->id == 43
        || location->id == N00BB_LLVM_FRAME_SENTINEL_ID);
    CHECK(location->size_bytes == sizeof(void *));
    CHECK(location->stack_bytes >= sizeof(void *));
    if (location->id == 43)
        CHECK(location->kind == N00BB_LLVM_STACKMAP_REGISTER);
    else CHECK(location->kind == N00BB_LLVM_STACKMAP_DIRECT
               && location->frame_offset < 0);
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
    LLVMTypeRef parameters[] = {
        LLVMInt64TypeInContext(context), LLVMInt32TypeInContext(context)
    };
    LLVMTypeRef map_type = LLVMFunctionType(LLVMVoidTypeInContext(context),
                                           parameters, 2, true);
    LLVMValueRef map = LLVMAddFunction(module, "llvm.experimental.stackmap",
                                       map_type);
    LLVMValueRef operands[] = {
        LLVMConstInt(parameters[0], 42, false),
        LLVMConstInt(parameters[1], 0, false), root,
    };
    LLVMBuildCall2(builder, map_type, map, operands, 3, "");
    LLVMValueRef descriptor = LLVMAddGlobal(module,
        LLVMInt8TypeInContext(context),
        "__n00bb_gc_frame.frame_map_probe");
    LLVMValueRef sentinel = LLVMBuildAlloca(builder, pointer, "sentinel");
    LLVMValueRef marker = LLVMBuildStore(builder, descriptor, sentinel);
    LLVMSetVolatile(marker, true);
    LLVMValueRef sentinel_operands[] = {
        LLVMConstInt(parameters[0], N00BB_LLVM_FRAME_SENTINEL_ID, false),
        LLVMConstInt(parameters[1], 0, false), sentinel,
    };
    LLVMBuildCall2(builder, map_type, map, sentinel_operands, 3, "");
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
    LLVMValueRef register_descriptor = LLVMAddGlobal(module,
        LLVMInt8TypeInContext(context),
        "__n00bb_gc_frame.frame_map_register_probe");
    LLVMValueRef register_sentinel = LLVMBuildAlloca(builder, pointer,
        "sentinel");
    LLVMValueRef register_marker = LLVMBuildStore(builder,
        register_descriptor, register_sentinel);
    LLVMSetVolatile(register_marker, true);
    LLVMValueRef register_sentinel_operands[] = {
        LLVMConstInt(parameters[0], N00BB_LLVM_FRAME_SENTINEL_ID, false),
        LLVMConstInt(parameters[1], 0, false), register_sentinel,
    };
    LLVMBuildCall2(builder, map_type, map, register_sentinel_operands, 3, "");
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
    CHECK(n00bb_llvm_stack_maps_visit(LLVMGetBufferStart(object),
        LLVMGetBufferSize(object), format, check_location, &count, &found));
    CHECK(found && count == 4);
    const char *profile = format == CORE_TARGET_OBJECT_MACH_O
        ? "arm64-macos" : format == CORE_TARGET_OBJECT_COFF
            ? "x86_64-windows" : "x86_64-linux";
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
        format == CORE_TARGET_OBJECT_MACH_O ? "__DATA_CONST" : NULL,
        format == CORE_TARGET_OBJECT_MACH_O ? "__n00b_frames"
            : format == CORE_TARGET_OBJECT_COFF ? ".n00b$F"
                                            : ".data.rel.ro.n00b_frames",
        &section));
    CHECK(core_le_load_u32(section.bytes) == 2);
    CHECK(core_le_load_u32(section.bytes + 4) == 1);
    CHECK(section.relocation_count == 2);
    CHECK(core_le_load_u32(section.bytes + 8 + 24 + 12) == 0);
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
