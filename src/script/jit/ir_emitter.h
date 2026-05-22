/*
 * ir_emitter.h -- Lua 5.4 bytecode to LLVM IR translation.
 *
 * The IREmitter walks a Lua Proto's instruction array and emits
 * corresponding LLVM IR using IRBuilder. Each Lua register becomes
 * an alloca'd JITValue. The LLVM mem2reg pass promotes these to SSA.
 */
#ifndef TDB_JIT_IR_EMITTER_H
#define TDB_JIT_IR_EMITTER_H

#include "tdb/jit.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Function.h>
#pragma GCC diagnostic pop

// Lua internal headers (we vendor Lua 5.4 so this is safe)
extern "C" {
#include "lobject.h"
#include "lopcodes.h"
}

#include <string>
#include <unordered_set>
#include <vector>

namespace tdb::script::jit {

class IREmitter {
public:
    IREmitter(llvm::LLVMContext &ctx, llvm::Module &mod);

    // Emit LLVM IR for a Lua Proto. Returns the function, or nullptr
    // if the Proto contains unsupported opcodes.
    llvm::Function *emit(const Proto *proto, const std::string &func_name);

    // Error message if emit() returned nullptr.
    const std::string &error() const { return error_; }

    // Set of opcodes that were unsupported (for diagnostics).
    const std::unordered_set<int> &unsupported_opcodes() const {
        return unsupported_;
    }

private:
    llvm::LLVMContext &ctx_;
    llvm::Module &mod_;
    std::string error_;
    std::unordered_set<int> unsupported_;

    // LLVM types for JITValue struct: { i32 type, i64 ival, double fval, ptr sval }
    llvm::StructType *jit_value_ty_ = nullptr;

    // Lazily-created type
    llvm::StructType *get_jit_value_type();

    // Helpers for emitting JITValue operations
    llvm::Value *emit_load_type(llvm::IRBuilder<> &B, llvm::Value *reg);
    llvm::Value *emit_load_ival(llvm::IRBuilder<> &B, llvm::Value *reg);
    llvm::Value *emit_load_fval(llvm::IRBuilder<> &B, llvm::Value *reg);
    llvm::Value *emit_load_sval(llvm::IRBuilder<> &B, llvm::Value *reg);

    void emit_store_nil(llvm::IRBuilder<> &B, llvm::Value *reg);
    void emit_store_bool(llvm::IRBuilder<> &B, llvm::Value *reg, bool val);
    void emit_store_int(llvm::IRBuilder<> &B, llvm::Value *reg, llvm::Value *ival);
    void emit_store_float(llvm::IRBuilder<> &B, llvm::Value *reg, llvm::Value *fval);
    void emit_store_copy(llvm::IRBuilder<> &B, llvm::Value *dst, llvm::Value *src);

    // Emit arithmetic operation with type dispatch
    void emit_arith(llvm::IRBuilder<> &B, llvm::Value *dst,
                    llvm::Value *lhs, llvm::Value *rhs,
                    llvm::Instruction::BinaryOps int_op,
                    llvm::Instruction::BinaryOps float_op,
                    llvm::Function *parent);

    // Scan instructions for jump targets to create basic blocks
    std::vector<llvm::BasicBlock *> create_basic_blocks(
        llvm::Function *func, const Instruction *code, int code_size);
};

} // namespace tdb::script::jit

#endif // TDB_JIT_IR_EMITTER_H
