#include <assert.h>
#include <string.h>

#include <llvm-c/BitReader.h>
#include <llvm-c/Core.h>

#include "ir/ir.h"
#include "lib/buffer.h"
#include "lib/diagnostic.h"
#include "llvm_backend.h"

int
main(void)
{
    ir_ctx_t *context = ir_ctx_new();
    const ir_profile_t profile = {.name = "llvm-probe"};
    const char *target = "x86_64-unknown-linux-gnu";
    ir_module_t *comptime = ir_module_new(context, "probe$comptime",
                                         &profile, target);
    ir_module_t *runtime = ir_module_new(context, "probe", &profile, target);
    core_diagnostics_t *diagnostics = core_diagnostics_new();
    n00bb_llvm_backend_t *backend = n00bb_llvm_backend_prepare(
        comptime, runtime, true, diagnostics);
    assert(backend);
    assert(core_diagnostics_count(diagnostics) == 0);

    for (int phase = 0; phase < 2; phase++) {
        core_buffer_t bytes = {0};
        bool is_comptime = phase == 0;
        assert(n00bb_llvm_backend_probe_bitcode(
            backend, is_comptime, &bytes, diagnostics));
        assert(bytes.byte_len > 0);
        LLVMMemoryBufferRef source = LLVMCreateMemoryBufferWithMemoryRangeCopy(
            bytes.data, bytes.byte_len, "probe");
        assert(source);
        LLVMContextRef reader = LLVMContextCreate();
        assert(reader);
        LLVMModuleRef parsed = nullptr;
        assert(!LLVMParseBitcodeInContext2(reader, source, &parsed));
        assert(parsed);
        assert(strcmp(LLVMGetTarget(parsed), target) == 0);
        LLVMDisposeModule(parsed);
        LLVMContextDispose(reader);
        LLVMDisposeMemoryBuffer(source);
        core_buffer_release(&bytes);
    }

    assert(core_diagnostics_count(diagnostics) == 0);
    n00bb_llvm_backend_free(backend);
    core_diagnostics_free(diagnostics);
    ir_ctx_free(context);
    return 0;
}
