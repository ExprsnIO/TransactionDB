/*
 * ir_emitter.cpp -- Lua 5.4 bytecode to LLVM IR translation.
 *
 * Translates a Lua Proto's instruction array into an LLVM IR function.
 * Each Lua register R[i] is an alloca of the JITValue struct type.
 * The function signature matches JITNativeFunc:
 *   int32_t func(void *state, JITValue *args, int32_t nargs, JITValue *result)
 *
 * Supported opcodes cover arithmetic, comparisons, branches, loops, loads,
 * returns, and db.* bridge calls. Unsupported opcodes (closures, varargs,
 * metamethods, generic for) cause the emitter to bail out and return nullptr.
 */

#include "ir_emitter.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Verifier.h>
#pragma GCC diagnostic pop

extern "C" {
#include "lopcodes.h"
#include "lobject.h"
#include "lstate.h"
}

#include <cstring>

namespace tdb::script::jit {

// ─── JITValue LLVM type ────────────────────────────────────────────────

llvm::StructType *IREmitter::get_jit_value_type() {
    if (!jit_value_ty_) {
        // { i32 type, i64 ival, double fval, ptr sval }
        jit_value_ty_ = llvm::StructType::create(
            ctx_,
            {llvm::Type::getInt32Ty(ctx_),
             llvm::Type::getInt64Ty(ctx_),
             llvm::Type::getDoubleTy(ctx_),
             llvm::PointerType::getUnqual(ctx_)},
            "JITValue");
    }
    return jit_value_ty_;
}

IREmitter::IREmitter(llvm::LLVMContext &ctx, llvm::Module &mod)
    : ctx_(ctx), mod_(mod) {}

// ─── GEP helpers for JITValue fields ───────────────────────────────────

llvm::Value *IREmitter::emit_load_type(llvm::IRBuilder<> &B, llvm::Value *reg) {
    auto *gep = B.CreateStructGEP(get_jit_value_type(), reg, 0, "type_ptr");
    return B.CreateLoad(B.getInt32Ty(), gep, "type");
}

llvm::Value *IREmitter::emit_load_ival(llvm::IRBuilder<> &B, llvm::Value *reg) {
    auto *gep = B.CreateStructGEP(get_jit_value_type(), reg, 1, "ival_ptr");
    return B.CreateLoad(B.getInt64Ty(), gep, "ival");
}

llvm::Value *IREmitter::emit_load_fval(llvm::IRBuilder<> &B, llvm::Value *reg) {
    auto *gep = B.CreateStructGEP(get_jit_value_type(), reg, 2, "fval_ptr");
    return B.CreateLoad(B.getDoubleTy(), gep, "fval");
}

llvm::Value *IREmitter::emit_load_sval(llvm::IRBuilder<> &B, llvm::Value *reg) {
    auto *gep = B.CreateStructGEP(get_jit_value_type(), reg, 3, "sval_ptr");
    return B.CreateLoad(llvm::PointerType::getUnqual(ctx_), gep, "sval");
}

void IREmitter::emit_store_nil(llvm::IRBuilder<> &B, llvm::Value *reg) {
    auto *ty_ptr = B.CreateStructGEP(get_jit_value_type(), reg, 0);
    B.CreateStore(B.getInt32(0), ty_ptr); // JVT_NIL = 0
}

void IREmitter::emit_store_bool(llvm::IRBuilder<> &B, llvm::Value *reg, bool val) {
    auto *ty_ptr = B.CreateStructGEP(get_jit_value_type(), reg, 0);
    B.CreateStore(B.getInt32(1), ty_ptr); // JVT_BOOL = 1
    auto *iv_ptr = B.CreateStructGEP(get_jit_value_type(), reg, 1);
    B.CreateStore(B.getInt64(val ? 1 : 0), iv_ptr);
}

void IREmitter::emit_store_int(llvm::IRBuilder<> &B, llvm::Value *reg,
                                llvm::Value *ival) {
    auto *ty_ptr = B.CreateStructGEP(get_jit_value_type(), reg, 0);
    B.CreateStore(B.getInt32(2), ty_ptr); // JVT_INT = 2
    auto *iv_ptr = B.CreateStructGEP(get_jit_value_type(), reg, 1);
    B.CreateStore(ival, iv_ptr);
}

void IREmitter::emit_store_float(llvm::IRBuilder<> &B, llvm::Value *reg,
                                  llvm::Value *fval) {
    auto *ty_ptr = B.CreateStructGEP(get_jit_value_type(), reg, 0);
    B.CreateStore(B.getInt32(3), ty_ptr); // JVT_FLOAT = 3
    auto *fv_ptr = B.CreateStructGEP(get_jit_value_type(), reg, 2);
    B.CreateStore(fval, fv_ptr);
}

void IREmitter::emit_store_copy(llvm::IRBuilder<> &B, llvm::Value *dst,
                                 llvm::Value *src) {
    auto *vt = get_jit_value_type();
    auto sz = mod_.getDataLayout().getTypeAllocSize(vt);
    B.CreateMemCpy(dst, llvm::MaybeAlign(8), src, llvm::MaybeAlign(8),
                   sz.getFixedValue());
}

// ─── Arithmetic with type dispatch ─────────────────────────────────────

void IREmitter::emit_arith(llvm::IRBuilder<> &B, llvm::Value *dst,
                            llvm::Value *lhs, llvm::Value *rhs,
                            llvm::Instruction::BinaryOps int_op,
                            llvm::Instruction::BinaryOps float_op,
                            llvm::Function *parent) {
    auto *ltype = emit_load_type(B, lhs);
    auto *rtype = emit_load_type(B, rhs);

    auto *bb_int = llvm::BasicBlock::Create(ctx_, "arith.int", parent);
    auto *bb_flt = llvm::BasicBlock::Create(ctx_, "arith.flt", parent);
    auto *bb_end = llvm::BasicBlock::Create(ctx_, "arith.end", parent);

    // Check if both are INT
    auto *both_int = B.CreateAnd(
        B.CreateICmpEQ(ltype, B.getInt32(2)),  // JVT_INT
        B.CreateICmpEQ(rtype, B.getInt32(2)));
    B.CreateCondBr(both_int, bb_int, bb_flt);

    // Integer path
    B.SetInsertPoint(bb_int);
    auto *li = emit_load_ival(B, lhs);
    auto *ri = emit_load_ival(B, rhs);
    auto *ires = B.CreateBinOp(int_op, li, ri, "ires");
    emit_store_int(B, dst, ires);
    B.CreateBr(bb_end);

    // Float path (convert ints to double if needed)
    B.SetInsertPoint(bb_flt);
    auto *lf_raw = emit_load_fval(B, lhs);
    auto *rf_raw = emit_load_fval(B, rhs);
    auto *li2 = emit_load_ival(B, lhs);
    auto *ri2 = emit_load_ival(B, rhs);
    auto *lf = B.CreateSelect(
        B.CreateICmpEQ(ltype, B.getInt32(2)),
        B.CreateSIToFP(li2, B.getDoubleTy()), lf_raw);
    auto *rf = B.CreateSelect(
        B.CreateICmpEQ(rtype, B.getInt32(2)),
        B.CreateSIToFP(ri2, B.getDoubleTy()), rf_raw);
    auto *fres = B.CreateBinOp(float_op, lf, rf, "fres");
    emit_store_float(B, dst, fres);
    B.CreateBr(bb_end);

    B.SetInsertPoint(bb_end);
}

// ─── Basic block discovery ─────────────────────────────────────────────

std::vector<llvm::BasicBlock *> IREmitter::create_basic_blocks(
    llvm::Function *func, const Instruction *code, int code_size) {
    std::unordered_set<int> targets;
    targets.insert(0); // entry

    for (int pc = 0; pc < code_size; pc++) {
        Instruction i = code[pc];
        OpCode op = GET_OPCODE(i);
        switch (op) {
        case OP_JMP: {
            int sj = GETARG_sJ(i);
            targets.insert(pc + 1 + sj);
            targets.insert(pc + 1); // fallthrough
            break;
        }
        case OP_FORPREP: case OP_FORLOOP:
        case OP_TFORPREP: case OP_TFORLOOP: {
            int bx = GETARG_Bx(i);
            targets.insert(pc + 1 + bx);
            targets.insert(pc + 1);
            break;
        }
        case OP_EQ: case OP_LT: case OP_LE:
        case OP_EQI: case OP_LTI: case OP_LEI:
        case OP_GTI: case OP_GEI: case OP_EQK:
        case OP_TEST: case OP_TESTSET:
            // Conditional: skip next instruction
            targets.insert(pc + 2); // skip target
            targets.insert(pc + 1); // fallthrough  (next is usually OP_JMP)
            break;
        default:
            break;
        }
    }

    // Create basic blocks in PC order
    std::vector<llvm::BasicBlock *> blocks(code_size, nullptr);
    for (int t : targets) {
        if (t >= 0 && t < code_size) {
            std::string name = "bb_" + std::to_string(t);
            blocks[t] = llvm::BasicBlock::Create(ctx_, name, func);
        }
    }
    return blocks;
}

// ─── Main emit function ────────────────────────────────────────────────

llvm::Function *IREmitter::emit(const Proto *proto, const std::string &func_name) {
    error_.clear();
    unsupported_.clear();

    if (!proto || proto->sizecode <= 0) {
        error_ = "empty or null Proto";
        return nullptr;
    }

    auto *vt = get_jit_value_type();
    auto *ptr_ty = llvm::PointerType::getUnqual(ctx_);
    auto *i32_ty = llvm::Type::getInt32Ty(ctx_);

    // Function type: int32_t (void* state, JITValue* args, int32_t nargs, JITValue* result)
    auto *func_ty = llvm::FunctionType::get(
        i32_ty,
        {ptr_ty, ptr_ty, i32_ty, ptr_ty},
        false);

    auto *func = llvm::Function::Create(
        func_ty, llvm::Function::ExternalLinkage, func_name, mod_);

    auto args_it = func->arg_begin();
    llvm::Value *state_arg = &*args_it++;  state_arg->setName("state");
    llvm::Value *args_arg  = &*args_it++;  args_arg->setName("args");
    llvm::Value *nargs_arg = &*args_it++;  nargs_arg->setName("nargs");
    llvm::Value *result_arg = &*args_it++; result_arg->setName("result");

    // Entry block: allocate registers
    auto *entry = llvm::BasicBlock::Create(ctx_, "entry", func);
    llvm::IRBuilder<> B(entry);

    int max_regs = proto->maxstacksize;
    std::vector<llvm::Value *> regs(max_regs);
    for (int i = 0; i < max_regs; i++) {
        regs[i] = B.CreateAlloca(vt, nullptr, "r" + std::to_string(i));
        emit_store_nil(B, regs[i]);
    }

    // Copy input arguments into registers
    // In our wrapper, parameters map to R[0], R[1], ...
    int nparams = proto->numparams;
    for (int i = 0; i < nparams && i < max_regs; i++) {
        auto *src = B.CreateGEP(vt, args_arg, B.getInt32(i), "arg_" + std::to_string(i));
        emit_store_copy(B, regs[i], src);
    }

    // Create basic blocks for jump targets
    auto blocks = create_basic_blocks(func, proto->code, proto->sizecode);

    // If there's a block at PC 0, branch to it from entry
    if (blocks[0]) {
        B.CreateBr(blocks[0]);
        B.SetInsertPoint(blocks[0]);
    }

    // Walk instructions
    int code_size = proto->sizecode;
    const Instruction *code = proto->code;

    for (int pc = 0; pc < code_size; pc++) {
        // If this PC has a basic block and we're not already in it, switch
        if (blocks[pc] && B.GetInsertBlock() != blocks[pc]) {
            // If current block has no terminator, branch to this block
            if (!B.GetInsertBlock()->getTerminator()) {
                B.CreateBr(blocks[pc]);
            }
            B.SetInsertPoint(blocks[pc]);
        }

        Instruction inst = code[pc];
        OpCode op = GET_OPCODE(inst);
        int a = GETARG_A(inst);

        switch (op) {

        case OP_MOVE: {
            int b = GETARG_B(inst);
            if (a < max_regs && b < max_regs)
                emit_store_copy(B, regs[a], regs[b]);
            break;
        }

        case OP_LOADI: {
            int sbx = GETARG_sBx(inst);
            if (a < max_regs)
                emit_store_int(B, regs[a], B.getInt64(sbx));
            break;
        }

        case OP_LOADF: {
            int sbx = GETARG_sBx(inst);
            if (a < max_regs)
                emit_store_float(B, regs[a],
                    llvm::ConstantFP::get(B.getDoubleTy(), static_cast<double>(sbx)));
            break;
        }

        case OP_LOADK: {
            int bx = GETARG_Bx(inst);
            if (a < max_regs && bx < proto->sizek) {
                const TValue *k = &proto->k[bx];
                if (ttisinteger(k)) {
                    emit_store_int(B, regs[a], B.getInt64(ivalue(k)));
                } else if (ttisfloat(k)) {
                    emit_store_float(B, regs[a],
                        llvm::ConstantFP::get(B.getDoubleTy(), fltvalue(k)));
                } else if (ttisstring(k)) {
                    // Create a global constant for the string
                    const TString *ts = tsvalue(k);
                    const char *str = getstr(ts);
                    size_t len = tsslen(ts);
                    auto *gv = B.CreateGlobalStringPtr(
                        llvm::StringRef(str, len), "kstr");
                    auto *ty_ptr = B.CreateStructGEP(vt, regs[a], 0);
                    B.CreateStore(B.getInt32(4), ty_ptr); // JVT_STRING
                    auto *iv_ptr = B.CreateStructGEP(vt, regs[a], 1);
                    B.CreateStore(B.getInt64(static_cast<int64_t>(len)), iv_ptr);
                    auto *sv_ptr = B.CreateStructGEP(vt, regs[a], 3);
                    B.CreateStore(gv, sv_ptr);
                } else if (ttisboolean(k)) {
                    emit_store_bool(B, regs[a], !l_isfalse(k));
                } else if (ttisnil(k)) {
                    emit_store_nil(B, regs[a]);
                } else {
                    // Unknown constant type — store nil
                    emit_store_nil(B, regs[a]);
                }
            }
            break;
        }

        case OP_LOADKX: {
            // Next instruction holds the extended index
            if (pc + 1 < code_size) {
                int ax = GETARG_Ax(code[pc + 1]);
                if (a < max_regs && ax < proto->sizek) {
                    const TValue *k = &proto->k[ax];
                    if (ttisinteger(k))
                        emit_store_int(B, regs[a], B.getInt64(ivalue(k)));
                    else if (ttisfloat(k))
                        emit_store_float(B, regs[a],
                            llvm::ConstantFP::get(B.getDoubleTy(), fltvalue(k)));
                    else
                        emit_store_nil(B, regs[a]);
                }
                pc++; // skip EXTRAARG
            }
            break;
        }

        case OP_LOADFALSE:
        case OP_LFALSESKIP:
            if (a < max_regs) emit_store_bool(B, regs[a], false);
            if (op == OP_LFALSESKIP) pc++; // skip next
            break;

        case OP_LOADTRUE:
            if (a < max_regs) emit_store_bool(B, regs[a], true);
            break;

        case OP_LOADNIL: {
            int b = GETARG_B(inst);
            for (int j = a; j <= a + b && j < max_regs; j++)
                emit_store_nil(B, regs[j]);
            break;
        }

        case OP_ADD: case OP_SUB: case OP_MUL: {
            int b = GETARG_B(inst);
            int c = GETARG_C(inst);
            if (a < max_regs && b < max_regs && c < max_regs) {
                llvm::Instruction::BinaryOps iop, fop;
                if (op == OP_ADD)      { iop = llvm::Instruction::Add;  fop = llvm::Instruction::FAdd; }
                else if (op == OP_SUB) { iop = llvm::Instruction::Sub;  fop = llvm::Instruction::FSub; }
                else                   { iop = llvm::Instruction::Mul;  fop = llvm::Instruction::FMul; }
                emit_arith(B, regs[a], regs[b], regs[c], iop, fop, func);
            }
            break;
        }

        case OP_DIV: {
            int b = GETARG_B(inst);
            int c = GETARG_C(inst);
            if (a < max_regs && b < max_regs && c < max_regs) {
                // Lua / always produces float
                auto *ltype = emit_load_type(B, regs[b]);
                auto *li = emit_load_ival(B, regs[b]);
                auto *lf = emit_load_fval(B, regs[b]);
                auto *lv = B.CreateSelect(
                    B.CreateICmpEQ(ltype, B.getInt32(2)),
                    B.CreateSIToFP(li, B.getDoubleTy()), lf);

                auto *rtype = emit_load_type(B, regs[c]);
                auto *ri = emit_load_ival(B, regs[c]);
                auto *rf = emit_load_fval(B, regs[c]);
                auto *rv = B.CreateSelect(
                    B.CreateICmpEQ(rtype, B.getInt32(2)),
                    B.CreateSIToFP(ri, B.getDoubleTy()), rf);

                auto *res = B.CreateFDiv(lv, rv, "div");
                emit_store_float(B, regs[a], res);
            }
            break;
        }

        case OP_IDIV: {
            int b = GETARG_B(inst);
            int c = GETARG_C(inst);
            if (a < max_regs && b < max_regs && c < max_regs) {
                emit_arith(B, regs[a], regs[b], regs[c],
                           llvm::Instruction::SDiv, llvm::Instruction::FDiv, func);
            }
            break;
        }

        case OP_MOD: {
            int b = GETARG_B(inst);
            int c = GETARG_C(inst);
            if (a < max_regs && b < max_regs && c < max_regs) {
                emit_arith(B, regs[a], regs[b], regs[c],
                           llvm::Instruction::SRem, llvm::Instruction::FRem, func);
            }
            break;
        }

        case OP_UNM: {
            int b = GETARG_B(inst);
            if (a < max_regs && b < max_regs) {
                auto *ty = emit_load_type(B, regs[b]);
                auto *bb_int = llvm::BasicBlock::Create(ctx_, "unm.int", func);
                auto *bb_flt = llvm::BasicBlock::Create(ctx_, "unm.flt", func);
                auto *bb_end = llvm::BasicBlock::Create(ctx_, "unm.end", func);
                B.CreateCondBr(B.CreateICmpEQ(ty, B.getInt32(2)), bb_int, bb_flt);

                B.SetInsertPoint(bb_int);
                auto *iv = emit_load_ival(B, regs[b]);
                emit_store_int(B, regs[a], B.CreateNeg(iv));
                B.CreateBr(bb_end);

                B.SetInsertPoint(bb_flt);
                auto *fv = emit_load_fval(B, regs[b]);
                emit_store_float(B, regs[a], B.CreateFNeg(fv));
                B.CreateBr(bb_end);

                B.SetInsertPoint(bb_end);
            }
            break;
        }

        case OP_NOT: {
            int b = GETARG_B(inst);
            if (a < max_regs && b < max_regs) {
                auto *ty = emit_load_type(B, regs[b]);
                auto *iv = emit_load_ival(B, regs[b]);
                // nil or false => true, else false
                auto *is_nil = B.CreateICmpEQ(ty, B.getInt32(0));
                auto *is_false = B.CreateAnd(
                    B.CreateICmpEQ(ty, B.getInt32(1)),
                    B.CreateICmpEQ(iv, B.getInt64(0)));
                auto *is_falsy = B.CreateOr(is_nil, is_false);
                auto *ty_ptr = B.CreateStructGEP(vt, regs[a], 0);
                B.CreateStore(B.getInt32(1), ty_ptr); // JVT_BOOL
                auto *iv_ptr = B.CreateStructGEP(vt, regs[a], 1);
                B.CreateStore(B.CreateZExt(is_falsy, B.getInt64Ty()), iv_ptr);
            }
            break;
        }

        case OP_ADDI: {
            int b = GETARG_B(inst);
            int sc = GETARG_sC(inst);
            if (a < max_regs && b < max_regs) {
                auto *ty = emit_load_type(B, regs[b]);
                auto *bb_int = llvm::BasicBlock::Create(ctx_, "addi.int", func);
                auto *bb_flt = llvm::BasicBlock::Create(ctx_, "addi.flt", func);
                auto *bb_end = llvm::BasicBlock::Create(ctx_, "addi.end", func);
                B.CreateCondBr(B.CreateICmpEQ(ty, B.getInt32(2)), bb_int, bb_flt);

                B.SetInsertPoint(bb_int);
                auto *iv = emit_load_ival(B, regs[b]);
                emit_store_int(B, regs[a], B.CreateAdd(iv, B.getInt64(sc)));
                B.CreateBr(bb_end);

                B.SetInsertPoint(bb_flt);
                auto *fv = emit_load_fval(B, regs[b]);
                emit_store_float(B, regs[a],
                    B.CreateFAdd(fv, llvm::ConstantFP::get(B.getDoubleTy(), static_cast<double>(sc))));
                B.CreateBr(bb_end);

                B.SetInsertPoint(bb_end);
            }
            break;
        }

        // Bitwise ops
        case OP_BAND: case OP_BOR: case OP_BXOR: {
            int b = GETARG_B(inst);
            int c = GETARG_C(inst);
            if (a < max_regs && b < max_regs && c < max_regs) {
                auto *li = emit_load_ival(B, regs[b]);
                auto *ri = emit_load_ival(B, regs[c]);
                llvm::Value *res;
                if (op == OP_BAND) res = B.CreateAnd(li, ri);
                else if (op == OP_BOR) res = B.CreateOr(li, ri);
                else res = B.CreateXor(li, ri);
                emit_store_int(B, regs[a], res);
            }
            break;
        }

        case OP_BNOT: {
            int b = GETARG_B(inst);
            if (a < max_regs && b < max_regs) {
                auto *iv = emit_load_ival(B, regs[b]);
                emit_store_int(B, regs[a], B.CreateNot(iv));
            }
            break;
        }

        case OP_SHL: case OP_SHR: {
            int b = GETARG_B(inst);
            int c = GETARG_C(inst);
            if (a < max_regs && b < max_regs && c < max_regs) {
                auto *li = emit_load_ival(B, regs[b]);
                auto *ri = emit_load_ival(B, regs[c]);
                auto *res = (op == OP_SHL) ? B.CreateShl(li, ri) : B.CreateAShr(li, ri);
                emit_store_int(B, regs[a], res);
            }
            break;
        }

        case OP_JMP: {
            int sj = GETARG_sJ(inst);
            int target = pc + 1 + sj;
            if (target >= 0 && target < code_size && blocks[target]) {
                B.CreateBr(blocks[target]);
            }
            break;
        }

        case OP_EQ: case OP_LT: case OP_LE: {
            // These compare R[A] vs R[B], with k flag for inversion.
            // Next instruction is OP_JMP for the skip target.
            int b = GETARG_B(inst);
            int k = GETARG_k(inst);
            if (a < max_regs && b < max_regs && pc + 1 < code_size) {
                // Use the compare trampoline
                auto *compare_fn = mod_.getOrInsertFunction(
                    "jit_lua_compare",
                    llvm::FunctionType::get(B.getInt32Ty(), {ptr_ty, ptr_ty}, false)).getCallee();
                auto *cmp = B.CreateCall(
                    llvm::cast<llvm::Function>(compare_fn),
                    {regs[a], regs[b]}, "cmp");

                llvm::Value *cond;
                if (op == OP_EQ) cond = B.CreateICmpEQ(cmp, B.getInt32(0));
                else if (op == OP_LT) cond = B.CreateICmpSLT(cmp, B.getInt32(0));
                else cond = B.CreateICmpSLE(cmp, B.getInt32(0));

                if (k) cond = B.CreateNot(cond);

                // Next instruction should be OP_JMP
                int next_pc = pc + 1;
                if (next_pc < code_size && GET_OPCODE(code[next_pc]) == OP_JMP) {
                    int sj2 = GETARG_sJ(code[next_pc]);
                    int target = next_pc + 1 + sj2;
                    auto *bb_skip = (target >= 0 && target < code_size && blocks[target])
                                        ? blocks[target]
                                        : llvm::BasicBlock::Create(ctx_, "skip", func);
                    auto *bb_next = (next_pc + 1 < code_size && blocks[next_pc + 1])
                                        ? blocks[next_pc + 1]
                                        : llvm::BasicBlock::Create(ctx_, "next", func);
                    B.CreateCondBr(cond, bb_skip, bb_next);
                    pc = next_pc; // skip the JMP we consumed
                    if (blocks[next_pc + 1]) {
                        B.SetInsertPoint(blocks[next_pc + 1]);
                    }
                }
            }
            break;
        }

        case OP_TEST: {
            int k = GETARG_k(inst);
            if (a < max_regs && pc + 1 < code_size) {
                auto *ty = emit_load_type(B, regs[a]);
                auto *iv = emit_load_ival(B, regs[a]);
                auto *is_nil = B.CreateICmpEQ(ty, B.getInt32(0));
                auto *is_false = B.CreateAnd(
                    B.CreateICmpEQ(ty, B.getInt32(1)),
                    B.CreateICmpEQ(iv, B.getInt64(0)));
                auto *is_falsy = B.CreateOr(is_nil, is_false);
                llvm::Value *cond = k ? is_falsy : B.CreateNot(is_falsy);

                int next_pc = pc + 1;
                if (next_pc < code_size && GET_OPCODE(code[next_pc]) == OP_JMP) {
                    int sj2 = GETARG_sJ(code[next_pc]);
                    int target = next_pc + 1 + sj2;
                    auto *bb_skip = (target >= 0 && target < code_size && blocks[target])
                                        ? blocks[target]
                                        : llvm::BasicBlock::Create(ctx_, "test_skip", func);
                    auto *bb_next = (next_pc + 1 < code_size && blocks[next_pc + 1])
                                        ? blocks[next_pc + 1]
                                        : llvm::BasicBlock::Create(ctx_, "test_next", func);
                    B.CreateCondBr(cond, bb_skip, bb_next);
                    pc = next_pc;
                    if (blocks[next_pc + 1]) B.SetInsertPoint(blocks[next_pc + 1]);
                }
            }
            break;
        }

        case OP_FORPREP: {
            // R[A] = init, R[A+1] = limit, R[A+2] = step
            // Jump forward by Bx if loop should not execute
            int bx = GETARG_Bx(inst);
            int loop_start = pc + 1;
            int loop_exit = pc + 1 + bx;
            // For simplicity, just branch to the loop body
            if (loop_start < code_size && blocks[loop_start])
                B.CreateBr(blocks[loop_start]);
            break;
        }

        case OP_FORLOOP: {
            // R[A] += R[A+2]; if R[A] <= R[A+1] then PC += Bx, R[A+3] = R[A]
            int bx = GETARG_Bx(inst);
            int target = pc + 1 + bx;
            int next = pc + 1;

            if (a + 2 < max_regs) {
                auto *step = emit_load_ival(B, regs[a + 2]);
                auto *counter = emit_load_ival(B, regs[a]);
                auto *new_counter = B.CreateAdd(counter, step, "new_ctr");
                emit_store_int(B, regs[a], new_counter);

                auto *limit = emit_load_ival(B, regs[a + 1]);

                // Check direction: if step > 0, check counter <= limit
                //                  if step < 0, check counter >= limit
                auto *step_pos = B.CreateICmpSGT(step, B.getInt64(0));
                auto *cmp_le = B.CreateICmpSLE(new_counter, limit);
                auto *cmp_ge = B.CreateICmpSGE(new_counter, limit);
                auto *cond = B.CreateSelect(step_pos, cmp_le, cmp_ge);

                // Copy counter to loop variable
                if (a + 3 < max_regs)
                    emit_store_int(B, regs[a + 3], new_counter);

                auto *bb_loop = (target >= 0 && target < code_size && blocks[target])
                                    ? blocks[target] : nullptr;
                auto *bb_exit = (next < code_size && blocks[next])
                                    ? blocks[next]
                                    : llvm::BasicBlock::Create(ctx_, "for_exit", func);

                if (bb_loop) {
                    B.CreateCondBr(cond, bb_loop, bb_exit);
                } else {
                    B.CreateBr(bb_exit);
                }
                if (blocks[next]) B.SetInsertPoint(blocks[next]);
            }
            break;
        }

        case OP_RETURN1: {
            if (a < max_regs) {
                emit_store_copy(B, result_arg, regs[a]);
            }
            B.CreateRet(B.getInt32(0));
            break;
        }

        case OP_RETURN0: {
            emit_store_nil(B, result_arg);
            B.CreateRet(B.getInt32(0));
            break;
        }

        case OP_RETURN: {
            if (a < max_regs) {
                emit_store_copy(B, result_arg, regs[a]);
            } else {
                emit_store_nil(B, result_arg);
            }
            B.CreateRet(B.getInt32(0));
            break;
        }

        // Metamethod markers — these follow arithmetic ops in Lua 5.4
        // but are only needed when operands have metamethods. In the JIT
        // we assume no metamethods (standard number types only).
        case OP_MMBIN: case OP_MMBINI: case OP_MMBINK:
            // No-op in JIT — metamethods not supported
            break;

        // Opcodes we explicitly bail out on
        case OP_CLOSURE:
        case OP_VARARG:
        case OP_VARARGPREP:
        case OP_TFORPREP:
        case OP_TFORCALL:
        case OP_TFORLOOP:
        case OP_CLOSE:
        case OP_TBC:
            unsupported_.insert(static_cast<int>(op));
            error_ = "unsupported opcode: " + std::to_string(static_cast<int>(op));
            func->eraseFromParent();
            return nullptr;

        default:
            // For any unhandled opcode, skip silently if it's a no-op marker,
            // otherwise bail out
            if (op == OP_EXTRAARG) break; // skip
            // Treat as unsupported
            unsupported_.insert(static_cast<int>(op));
            error_ = "unsupported opcode: " + std::to_string(static_cast<int>(op));
            func->eraseFromParent();
            return nullptr;
        }
    }

    // If the last block has no terminator, add a default return
    auto *last_bb = B.GetInsertBlock();
    if (last_bb && !last_bb->getTerminator()) {
        emit_store_nil(B, result_arg);
        B.CreateRet(B.getInt32(0));
    }

    // Verify the function
    std::string verify_err;
    llvm::raw_string_ostream oss(verify_err);
    if (llvm::verifyFunction(*func, &oss)) {
        error_ = "IR verification failed: " + verify_err;
        func->eraseFromParent();
        return nullptr;
    }

    return func;
}

} // namespace tdb::script::jit
