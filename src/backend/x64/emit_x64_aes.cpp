/* This file is part of the dynarmic project.
 * Copyright (c) 2018 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include "backend/x64/abi.h"
#include "backend/x64/block_of_code.h"
#include "backend/x64/emit_x64.h"
#include "common/common_types.h"
#include "common/crypto/aes.h"
#include "frontend/ir/microinstruction.h"

namespace Dynarmic::Backend::X64 {

using namespace Xbyak::util;
namespace AES = Common::Crypto::AES;

using AESFn = void(AES::State&, const AES::State&);

static void EmitAESFunction(RegAlloc::ArgumentInfo args, EmitContext& ctx, BlockOfCode& code,
                            IR::Inst* inst, AESFn fn) {
    constexpr u32 stack_space = static_cast<u32>(sizeof(AES::State)) * 2;
    const Xbyak::Xmm input = ctx.reg_alloc.UseXmm(args[0]);
    const Xbyak::Xmm result = ctx.reg_alloc.ScratchXmm();
    ctx.reg_alloc.EndOfAllocScope();

    ctx.reg_alloc.HostCall(nullptr);
    code.sub(rsp, stack_space + ABI_SHADOW_SPACE);
    code.lea(code.ABI_PARAM1, ptr[rsp + ABI_SHADOW_SPACE]);
    code.lea(code.ABI_PARAM2, ptr[rsp + ABI_SHADOW_SPACE + sizeof(AES::State)]);

    code.movaps(xword[code.ABI_PARAM2], input);

    code.CallFunction(fn);

    code.movaps(result, xword[rsp + ABI_SHADOW_SPACE]);

    // Free memory
    code.add(rsp, stack_space + ABI_SHADOW_SPACE);

    ctx.reg_alloc.DefineValue(inst, result);
}

void EmitX64::EmitAESDecryptSingleRound(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);

    EmitAESFunction(args, ctx, code, inst, AES::DecryptSingleRound);
}

void EmitX64::EmitAESEncryptSingleRound(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);

    EmitAESFunction(args, ctx, code, inst, AES::EncryptSingleRound);
}

void EmitX64::EmitAESInverseMixColumns(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);

    if (code.HasAESNI()) {
        const Xbyak::Xmm data = ctx.reg_alloc.UseScratchXmm(args[0]);

        code.aesimc(data, data);

        ctx.reg_alloc.DefineValue(inst, data);
    } else {
        EmitAESFunction(args, ctx, code, inst, AES::InverseMixColumns);
    }
}

void EmitX64::EmitAESMixColumns(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    EmitAESFunction(args, ctx, code, inst, AES::MixColumns);
}

} // namespace Dynarmic::Backend::X64
