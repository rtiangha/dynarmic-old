/* This file is part of the dynarmic project.
 * Copyright (c) 2016 MerryMage
 * This software may be used and distributed according to the terms of the GNU
 * General Public License version 2 or any later version.
 */

#include <iterator>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <fmt/format.h>
#include <fmt/ostream.h>

#include <dynarmic/A32/coprocessor.h>

#include "backend/A64/a32_emit_a64.h"
#include "backend/A64/a32_jitstate.h"
#include "backend/A64/abi.h"
#include "backend/A64/block_of_code.h"
#include "backend/A64/devirtualize.h"
#include "backend/A64/emit_a64.h"
#include "backend/A64/emitter/a64_emitter.h"
#include "backend/A64/perf_map.h"
#include "common/assert.h"
#include "common/bit_util.h"
#include "common/common_types.h"
#include "common/scope_exit.h"
#include "common/variant_util.h"
#include "frontend/A32/location_descriptor.h"
#include "frontend/A32/types.h"
#include "frontend/ir/basic_block.h"
#include "frontend/ir/microinstruction.h"
#include "frontend/ir/opcodes.h"

// TODO: Have ARM flags in host flags and not have them use up GPR registers unless necessary.
// TODO: Actually implement that proper instruction selector you've always wanted to sweetheart.

namespace Dynarmic::BackendA64 {

// Note that unlike the x64 backend these only returns ONLY the offset to register and not the
// address!
static size_t MJitStateReg(A32::Reg reg) {
    return offsetof(A32JitState, Reg) + sizeof(u32) * static_cast<size_t>(reg);
}

static size_t MJitStateExtReg(A32::ExtReg reg) {
    if (A32::IsSingleExtReg(reg)) {
        size_t index = static_cast<size_t>(reg) - static_cast<size_t>(A32::ExtReg::S0);
        return offsetof(A32JitState, ExtReg) + sizeof(u32) * index;
    }
    if (A32::IsDoubleExtReg(reg)) {
        size_t index = static_cast<size_t>(reg) - static_cast<size_t>(A32::ExtReg::D0);
        return offsetof(A32JitState, ExtReg) + sizeof(u64) * index;
    }
    ASSERT_FALSE("Should never happen.");
}

A32EmitContext::A32EmitContext(RegAlloc& reg_alloc, IR::Block& block)
    : EmitContext(reg_alloc, block) {}

A32::LocationDescriptor A32EmitContext::Location() const {
    return A32::LocationDescriptor{block.Location()};
}

bool A32EmitContext::IsSingleStep() const {
    return A32::LocationDescriptor{block.Location()}.SingleStepping();
}

FP::RoundingMode A32EmitContext::FPSCR_RMode() const {
    return Location().FPSCR().RMode();
}

u32 A32EmitContext::FPCR() const {
    return Location().FPSCR().Value();
}

bool A32EmitContext::FPSCR_FTZ() const {
    return Location().FPSCR().FTZ();
}

bool A32EmitContext::FPSCR_DN() const {
    return Location().FPSCR().DN();
}

std::ptrdiff_t A32EmitContext::GetInstOffset(IR::Inst* inst) const {
    return std::distance(block.begin(), IR::Block::iterator(inst));
}

A32EmitA64::A32EmitA64(BlockOfCode& code, A32::UserConfig config, A32::Jit* jit_interface)
    : EmitA64(code), config(std::move(config)), jit_interface(jit_interface) {
    exception_handler.Register(code, [this](CodePtr PC) { FastmemCallback(PC); });
    GenMemoryAccessors();
    GenTerminalHandlers();
    code.PreludeComplete();
    ClearFastDispatchTable();
    fastmem_patch_info.clear();
}

A32EmitA64::~A32EmitA64() = default;

A32EmitA64::BlockDescriptor A32EmitA64::Emit(IR::Block& block) {
    code.EnableWriting();
    SCOPE_EXIT {
        code.DisableWriting();
    };

    RegAlloc reg_alloc{code, A32JitState::SpillCount, SpillToOpArg<A32JitState>};
    A32EmitContext ctx{reg_alloc, block};

    const u8* entrypoint = code.AlignCode16();

    // Start emitting.
    EmitCondPrelude(ctx);

    for (auto iter = block.begin(); iter != block.end(); ++iter) {
        IR::Inst* inst = &*iter;

        // Call the relevant Emit* member function.
        switch (inst->GetOpcode()) {

#define OPCODE(name, type, ...)                                                                    \
    case IR::Opcode::name:                                                                         \
        A32EmitA64::Emit##name(ctx, inst);                                                         \
        break;
#define A32OPC(name, type, ...)                                                                    \
    case IR::Opcode::A32##name:                                                                    \
        A32EmitA64::EmitA32##name(ctx, inst);                                                      \
        break;
#define A64OPC(...)
#include "backend/A64/opcodes.inc"
#undef OPCODE
#undef A32OPC
#undef A64OPC

        default:
            ASSERT_FALSE("Invalid opcode: {}", inst->GetOpcode());
            break;
        }

        reg_alloc.EndOfAllocScope();
    }

    reg_alloc.AssertNoMoreUses();

    EmitAddCycles(block.CycleCount());
    EmitA64::EmitTerminal(block.GetTerminal(), ctx.Location().SetSingleStepping(false),
                          ctx.IsSingleStep());
    code.BRK(0);
    code.PatchConstPool();
    code.FlushIcacheSection(entrypoint, code.GetCodePtr());

    const size_t size = static_cast<size_t>(code.GetCodePtr() - entrypoint);

    const A32::LocationDescriptor descriptor{block.Location()};
    const A32::LocationDescriptor end_location{block.EndLocation()};

    const auto range =
        boost::icl::discrete_interval<u32>::closed(descriptor.PC(), end_location.PC() - 1);
    block_ranges.AddRange(range, descriptor);

    return RegisterBlock(descriptor, entrypoint, size);
}

void A32EmitA64::ClearCache() {
    EmitA64::ClearCache();
    block_ranges.ClearCache();
    ClearFastDispatchTable();
    fastmem_patch_info.clear();
}

void A32EmitA64::InvalidateCacheRanges(const boost::icl::interval_set<u32>& ranges) {
    InvalidateBasicBlocks(block_ranges.InvalidateRanges(ranges));
}

void A32EmitA64::EmitCondPrelude(const A32EmitContext& ctx) {
    if (ctx.block.GetCondition() == IR::Cond::AL) {
        ASSERT(!ctx.block.HasConditionFailedLocation());
        return;
    }

    ASSERT(ctx.block.HasConditionFailedLocation());

    FixupBranch pass = EmitCond(ctx.block.GetCondition());
    EmitAddCycles(ctx.block.ConditionFailedCycleCount());
    EmitTerminal(IR::Term::LinkBlock{ctx.block.ConditionFailedLocation()}, ctx.block.Location(),
                 ctx.IsSingleStep());
    code.SetJumpTarget(pass);
}

void A32EmitA64::ClearFastDispatchTable() {
    if (config.enable_fast_dispatch) {
        fast_dispatch_table.fill({});
    }
}

void A32EmitA64::GenMemoryAccessors() {
    code.AlignCode16();
    read_memory_8 = code.GetCodePtr();
    // Push lr and fp onto the stack
    code.ABI_PushRegisters(0x60000000);
    code.ADD(X29, SP, 0);
    ABI_PushCallerSaveRegistersAndAdjustStackExcept(code, ABI_RETURN);
    Devirtualize<&A32::UserCallbacks::MemoryRead8>(config.callbacks).EmitCall(code);
    ABI_PopCallerSaveRegistersAndAdjustStackExcept(code, ABI_RETURN);
    code.ABI_PopRegisters(0x60000000);
    code.RET();
    PerfMapRegister(read_memory_8, code.GetCodePtr(), "a32_read_memory_8");

    code.AlignCode16();
    read_memory_16 = code.GetCodePtr();
    // Push lr and fp onto the stack
    code.ABI_PushRegisters(0x60000000);
    code.ADD(X29, SP, 0);
    ABI_PushCallerSaveRegistersAndAdjustStackExcept(code, ABI_RETURN);
    Devirtualize<&A32::UserCallbacks::MemoryRead16>(config.callbacks).EmitCall(code);
    ABI_PopCallerSaveRegistersAndAdjustStackExcept(code, ABI_RETURN);
    code.ABI_PopRegisters(0x60000000);
    code.RET();
    PerfMapRegister(read_memory_16, code.GetCodePtr(), "a32_read_memory_16");

    code.AlignCode16();
    read_memory_32 = code.GetCodePtr();
    // Push lr and fp onto the stack
    code.ABI_PushRegisters(0x60000000);
    code.ADD(X29, SP, 0);
    ABI_PushCallerSaveRegistersAndAdjustStackExcept(code, ABI_RETURN);
    Devirtualize<&A32::UserCallbacks::MemoryRead32>(config.callbacks).EmitCall(code);
    ABI_PopCallerSaveRegistersAndAdjustStackExcept(code, ABI_RETURN);
    code.ABI_PopRegisters(0x60000000);
    code.RET();
    PerfMapRegister(read_memory_32, code.GetCodePtr(), "a32_read_memory_32");

    code.AlignCode16();
    read_memory_64 = code.GetCodePtr();
    // Push lr and fp onto the stack
    code.ABI_PushRegisters(0x60000000);
    code.ADD(X29, SP, 0);
    ABI_PushCallerSaveRegistersAndAdjustStackExcept(code, ABI_RETURN);
    Devirtualize<&A32::UserCallbacks::MemoryRead64>(config.callbacks).EmitCall(code);
    ABI_PopCallerSaveRegistersAndAdjustStackExcept(code, ABI_RETURN);
    code.ABI_PopRegisters(0x60000000);
    code.RET();
    PerfMapRegister(read_memory_64, code.GetCodePtr(), "a32_read_memory_64");

    code.AlignCode16();
    write_memory_8 = code.GetCodePtr();
    // Push lr and fp onto the stack
    code.ABI_PushRegisters(0x60000000);
    code.ADD(X29, SP, 0);
    ABI_PushCallerSaveRegistersAndAdjustStackExcept(code, ABI_RETURN);
    Devirtualize<&A32::UserCallbacks::MemoryWrite8>(config.callbacks).EmitCall(code);
    ABI_PopCallerSaveRegistersAndAdjustStackExcept(code, ABI_RETURN);
    code.ABI_PopRegisters(0x60000000);
    code.RET();
    PerfMapRegister(write_memory_8, code.GetCodePtr(), "a32_write_memory_8");

    code.AlignCode16();
    write_memory_16 = code.GetCodePtr();
    // Push lr and fp onto the stack
    code.ABI_PushRegisters(0x60000000);
    code.ADD(X29, SP, 0);
    ABI_PushCallerSaveRegistersAndAdjustStackExcept(code, ABI_RETURN);
    Devirtualize<&A32::UserCallbacks::MemoryWrite16>(config.callbacks).EmitCall(code);
    ABI_PopCallerSaveRegistersAndAdjustStackExcept(code, ABI_RETURN);
    code.ABI_PopRegisters(0x60000000);
    code.RET();
    PerfMapRegister(write_memory_16, code.GetCodePtr(), "a32_write_memory_16");

    code.AlignCode16();
    write_memory_32 = code.GetCodePtr();
    // Push lr and fp onto the stack
    code.ABI_PushRegisters(0x60000000);
    code.ADD(X29, SP, 0);
    ABI_PushCallerSaveRegistersAndAdjustStackExcept(code, ABI_RETURN);
    Devirtualize<&A32::UserCallbacks::MemoryWrite32>(config.callbacks).EmitCall(code);
    ABI_PopCallerSaveRegistersAndAdjustStackExcept(code, ABI_RETURN);
    code.ABI_PopRegisters(0x60000000);
    code.RET();
    PerfMapRegister(write_memory_32, code.GetCodePtr(), "a32_write_memory_32");

    code.AlignCode16();
    write_memory_64 = code.GetCodePtr();
    // Push lr and fp onto the stack
    code.ABI_PushRegisters(0x60000000);
    code.ADD(X29, SP, 0);
    ABI_PushCallerSaveRegistersAndAdjustStackExcept(code, ABI_RETURN);
    Devirtualize<&A32::UserCallbacks::MemoryWrite64>(config.callbacks).EmitCall(code);
    ABI_PopCallerSaveRegistersAndAdjustStackExcept(code, ABI_RETURN);
    code.ABI_PopRegisters(0x60000000);
    code.RET();
    PerfMapRegister(write_memory_64, code.GetCodePtr(), "a32_write_memory_64");
}

void A32EmitA64::GenTerminalHandlers() {
    const ARM64Reg fast_dispatch_entry_reg = X19;
    const ARM64Reg location_descriptor_reg = X20;

    // PC ends up in fast_dispatch_entry_reg, location_descriptor ends up in
    // location_descriptor_reg.
    const auto calculate_location_descriptor = [this](ARM64Reg fast_dispatch_entry_reg_,
                                                      ARM64Reg location_descriptor_reg_) {
        // This calculation has to match up with IREmitter::PushRSB
        code.LDR(INDEX_UNSIGNED, DecodeReg(location_descriptor_reg_), X28,
                 offsetof(A32JitState, upper_location_descriptor));
        code.LDR(INDEX_UNSIGNED, DecodeReg(fast_dispatch_entry_reg_), X28,
                 MJitStateReg(A32::Reg::PC));
        code.ORR(location_descriptor_reg_, fast_dispatch_entry_reg_, location_descriptor_reg_,
                 ArithOption{location_descriptor_reg_, ST_LSL, 32});
    };

    FixupBranch fast_dispatch_cache_miss, rsb_cache_miss;

    code.AlignCode16();
    terminal_handler_pop_rsb_hint = code.GetCodePtr();
    calculate_location_descriptor(fast_dispatch_entry_reg, location_descriptor_reg);
    code.LDR(INDEX_UNSIGNED, DecodeReg(code.ABI_SCRATCH1), X28, offsetof(A32JitState, rsb_ptr));
    code.SUBI2R(code.ABI_SCRATCH1, DecodeReg(code.ABI_SCRATCH1), 1);
    code.ANDI2R(code.ABI_SCRATCH1, DecodeReg(code.ABI_SCRATCH1), u32(A32JitState::RSBPtrMask));
    code.STR(INDEX_UNSIGNED, DecodeReg(code.ABI_SCRATCH1), X28, offsetof(A32JitState, rsb_ptr));

    // cmp(location_descriptor_reg, qword[r15 + offsetof(A32JitState, rsb_location_descriptors) +
    // rsb_ptr * sizeof(u64)]);
    code.ADD(code.ABI_SCRATCH1, X28, code.ABI_SCRATCH1, ArithOption{code.ABI_SCRATCH1, ST_LSL, 3});
    code.LDR(INDEX_UNSIGNED, X8, code.ABI_SCRATCH1,
             offsetof(A32JitState, rsb_location_descriptors));
    code.CMP(location_descriptor_reg, X8);
    if (config.enable_fast_dispatch) {
        rsb_cache_miss = code.B(CC_NEQ);
    } else {
        code.B(CC_NEQ, code.GetReturnFromRunCodeAddress());
    }
    code.LDR(INDEX_UNSIGNED, code.ABI_SCRATCH1, code.ABI_SCRATCH1,
             offsetof(A32JitState, rsb_codeptrs));
    code.BR(code.ABI_SCRATCH1);
    PerfMapRegister(terminal_handler_pop_rsb_hint, code.GetCodePtr(),
                    "a32_terminal_handler_pop_rsb_hint");

    if (config.enable_fast_dispatch) {
        terminal_handler_fast_dispatch_hint = code.AlignCode16();
        calculate_location_descriptor(fast_dispatch_entry_reg, location_descriptor_reg);
        code.SetJumpTarget(rsb_cache_miss);
        code.MOVI2R(code.ABI_SCRATCH1, reinterpret_cast<u64>(fast_dispatch_table.data()));
        code.CRC32CW(DecodeReg(fast_dispatch_entry_reg), DecodeReg(fast_dispatch_entry_reg),
                     DecodeReg(code.ABI_SCRATCH1));
        code.ANDI2R(fast_dispatch_entry_reg, fast_dispatch_entry_reg, fast_dispatch_table_mask);
        code.ADD(fast_dispatch_entry_reg, fast_dispatch_entry_reg, code.ABI_SCRATCH1);

        code.LDR(INDEX_UNSIGNED, code.ABI_SCRATCH1, fast_dispatch_entry_reg,
                 offsetof(FastDispatchEntry, location_descriptor));
        code.CMP(location_descriptor_reg, code.ABI_SCRATCH1);
        fast_dispatch_cache_miss = code.B(CC_NEQ);
        code.LDR(INDEX_UNSIGNED, code.ABI_SCRATCH1, fast_dispatch_entry_reg,
                 offsetof(FastDispatchEntry, code_ptr));
        code.BR(code.ABI_SCRATCH1);

        code.SetJumpTarget(fast_dispatch_cache_miss);
        code.STR(INDEX_UNSIGNED, location_descriptor_reg, fast_dispatch_entry_reg,
                 offsetof(FastDispatchEntry, location_descriptor));
        code.LookupBlock();
        code.STR(INDEX_UNSIGNED, code.ABI_RETURN, fast_dispatch_entry_reg,
                 offsetof(FastDispatchEntry, code_ptr));
        code.BR(code.ABI_RETURN);
        PerfMapRegister(terminal_handler_fast_dispatch_hint, code.GetCodePtr(),
                        "a32_terminal_handler_fast_dispatch_hint");

        code.AlignCode16();
        fast_dispatch_table_lookup =
            reinterpret_cast<FastDispatchEntry& (*)(u64)>(code.GetWritableCodePtr());
        code.MOVI2R(code.ABI_PARAM2, reinterpret_cast<u64>(fast_dispatch_table.data()));
        code.CRC32CW(DecodeReg(code.ABI_PARAM1), DecodeReg(code.ABI_PARAM1),
                     DecodeReg(code.ABI_PARAM2));
        code.ANDI2R(DecodeReg(code.ABI_PARAM1), DecodeReg(code.ABI_PARAM1),
                    fast_dispatch_table_mask);
        code.ADD(code.ABI_RETURN, code.ABI_PARAM1, code.ABI_PARAM2);
        code.RET();
    }
}

void A32EmitA64::EmitA32GetRegister(A32EmitContext& ctx, IR::Inst* inst) {
    A32::Reg reg = inst->GetArg(0).GetA32RegRef();

    Arm64Gen::ARM64Reg result = DecodeReg(ctx.reg_alloc.ScratchGpr());
    code.LDR(INDEX_UNSIGNED, result, X28, MJitStateReg(reg));
    ctx.reg_alloc.DefineValue(inst, result);
}

void A32EmitA64::EmitA32GetExtendedRegister32(A32EmitContext& ctx, IR::Inst* inst) {
    A32::ExtReg reg = inst->GetArg(0).GetA32ExtRegRef();
    ASSERT(A32::IsSingleExtReg(reg));

    ARM64Reg result = ctx.reg_alloc.ScratchFpr();
    code.fp_emitter.LDR(32, INDEX_UNSIGNED, result, X28, MJitStateExtReg(reg));
    ctx.reg_alloc.DefineValue(inst, result);
}

void A32EmitA64::EmitA32GetExtendedRegister64(A32EmitContext& ctx, IR::Inst* inst) {
    A32::ExtReg reg = inst->GetArg(0).GetA32ExtRegRef();
    ASSERT(A32::IsDoubleExtReg(reg));

    ARM64Reg result = ctx.reg_alloc.ScratchFpr();
    code.fp_emitter.LDR(64, INDEX_UNSIGNED, result, X28, MJitStateExtReg(reg));
    ctx.reg_alloc.DefineValue(inst, result);
}

void A32EmitA64::EmitA32SetRegister(A32EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    A32::Reg reg = inst->GetArg(0).GetA32RegRef();
    if (args[1].IsInFpr()) {
        Arm64Gen::ARM64Reg to_store = ctx.reg_alloc.UseFpr(args[1]);
        code.fp_emitter.STR(32, INDEX_UNSIGNED, to_store, X28, MJitStateReg(reg));
    } else {
        Arm64Gen::ARM64Reg to_store = DecodeReg(ctx.reg_alloc.UseGpr(args[1]));
        code.STR(INDEX_UNSIGNED, to_store, X28, MJitStateReg(reg));
    }
}

void A32EmitA64::EmitA32SetExtendedRegister32(A32EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    A32::ExtReg reg = inst->GetArg(0).GetA32ExtRegRef();
    ASSERT(A32::IsSingleExtReg(reg));
    if (args[1].IsInFpr()) {
        ARM64Reg to_store = ctx.reg_alloc.UseFpr(args[1]);
        code.fp_emitter.STR(32, INDEX_UNSIGNED, to_store, X28, MJitStateExtReg(reg));
    } else {
        ARM64Reg to_store = DecodeReg(ctx.reg_alloc.UseGpr(args[1]));
        code.STR(INDEX_UNSIGNED, to_store, X28, MJitStateExtReg(reg));
    }
}

void A32EmitA64::EmitA32SetExtendedRegister64(A32EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    A32::ExtReg reg = inst->GetArg(0).GetA32ExtRegRef();
    ASSERT(A32::IsDoubleExtReg(reg));
    if (args[1].IsInFpr()) {
        ARM64Reg to_store = ctx.reg_alloc.UseFpr(args[1]);
        code.fp_emitter.STR(64, INDEX_UNSIGNED, to_store, X28, MJitStateExtReg(reg));
    } else {
        ARM64Reg to_store = ctx.reg_alloc.UseGpr(args[1]);
        code.STR(INDEX_UNSIGNED, to_store, X28, MJitStateExtReg(reg));
    }
}

static u32 GetCpsrImpl(A32JitState* jit_state) {
    return jit_state->Cpsr();
}

void A32EmitA64::EmitA32GetCpsr(A32EmitContext& ctx, IR::Inst* inst) {
    // TODO:Inline
    ctx.reg_alloc.HostCall(inst);
    code.MOV(code.ABI_PARAM1, X28);
    code.QuickCallFunction(&GetCpsrImpl);
}

static void SetCpsrImpl(u32 value, A32JitState* jit_state) {
    jit_state->SetCpsr(value);
}

void A32EmitA64::EmitA32SetCpsr(A32EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);

    // TODO:Inline
    ctx.reg_alloc.HostCall(nullptr, args[0]);

    // Use an unused HostCall register
    ARM64Reg host_fpsr = X9;

    if (config.always_little_endian) {
        code.ANDI2R(code.ABI_PARAM1, code.ABI_PARAM1, 0xFFFFFDFF, ctx.reg_alloc.ScratchGpr());
    }

    // Since this is one of the only places where the ~sticky~
    // guest's Q flag can be cleared it is also a great place to clear the host's Q flag
    code.MRS(host_fpsr, FIELD_FPSR);
    code.ANDI2R(host_fpsr, host_fpsr, ~(1 << 27));
    code._MSR(FIELD_FPSR, host_fpsr);

    code.MOV(code.ABI_PARAM2, X28);
    code.QuickCallFunction(&SetCpsrImpl);
}

void A32EmitA64::EmitA32SetCpsrNZCVRaw(A32EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ARM64Reg a = DecodeReg(ctx.reg_alloc.UseScratchGpr(args[0]));

    code.ANDI2R(a, a, 0xF0000000);
    code.STR(INDEX_UNSIGNED, a, X28, offsetof(A32JitState, cpsr_nzcv));
}

void A32EmitA64::EmitA32SetCpsrNZCV(A32EmitContext& ctx, IR::Inst* inst) {
    EmitA32SetCpsrNZCVRaw(ctx, inst);
}

void A32EmitA64::EmitA32SetCpsrNZCVQ(A32EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ARM64Reg host_fpsr = ctx.reg_alloc.ScratchGpr();
    if (args[0].IsImmediate()) {
        u32 imm = args[0].GetImmediateU32();
        ARM64Reg a = DecodeReg(ctx.reg_alloc.ScratchGpr());

        code.MOVI2R(a, u32(imm & 0xF0000000));
        code.STR(INDEX_UNSIGNED, a, X28, offsetof(A32JitState, cpsr_nzcv));
        code.MOVI2R(a, u8((imm & 0x08000000) != 0 ? 1 : 0));
        code.STR(INDEX_UNSIGNED, a, X28, offsetof(A32JitState, cpsr_q));
    } else {
        ARM64Reg a = DecodeReg(ctx.reg_alloc.UseScratchGpr(args[0]));
        ARM64Reg q = DecodeReg(ctx.reg_alloc.ScratchGpr());

        code.UBFX(q, a, 27, 1);
        code.STR(INDEX_UNSIGNED, q, X28, offsetof(A32JitState, cpsr_q));
        code.ANDI2R(a, a, 0xF0000000);
        code.STR(INDEX_UNSIGNED, a, X28, offsetof(A32JitState, cpsr_nzcv));
    }

    // Since this is one of the only places where the ~sticky~
    // guest's Q flag can be cleared it is also a great place to clear the host's Q flag.
    // TODO : possibly a better job at explaining.
    code.MRS(host_fpsr, FIELD_FPSR);
    code.ANDI2R(host_fpsr, host_fpsr, ~(1 << 27));
    code._MSR(FIELD_FPSR, host_fpsr);
}

void A32EmitA64::EmitA32GetNFlag(A32EmitContext& ctx, IR::Inst* inst) {
    Arm64Gen::ARM64Reg result = DecodeReg(ctx.reg_alloc.ScratchGpr());
    code.LDR(INDEX_UNSIGNED, result, X28, offsetof(A32JitState, cpsr_nzcv));
    code.UBFX(result, result, 31, 1);
    ctx.reg_alloc.DefineValue(inst, result);
}

void A32EmitA64::EmitA32SetNFlag(A32EmitContext& ctx, IR::Inst* inst) {
    constexpr size_t flag_bit = 31;
    constexpr u32 flag_mask = 1u << flag_bit;
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    Arm64Gen::ARM64Reg nzcv = DecodeReg(ctx.reg_alloc.ScratchGpr());

    code.LDR(INDEX_UNSIGNED, nzcv, X28, offsetof(A32JitState, cpsr_nzcv));
    if (args[0].IsImmediate()) {
        if (args[0].GetImmediateU1()) {
            code.ORRI2R(nzcv, nzcv, flag_mask);
        } else {
            code.ANDI2R(nzcv, nzcv, ~flag_mask);
        }
    } else {
        Arm64Gen::ARM64Reg to_store = DecodeReg(ctx.reg_alloc.UseGpr(args[0]));

        code.BFI(nzcv, to_store, flag_bit, 1);
    }
    code.STR(INDEX_UNSIGNED, nzcv, X28, offsetof(A32JitState, cpsr_nzcv));
}

void A32EmitA64::EmitA32GetZFlag(A32EmitContext& ctx, IR::Inst* inst) {
    Arm64Gen::ARM64Reg result = DecodeReg(ctx.reg_alloc.ScratchGpr());
    code.LDR(INDEX_UNSIGNED, result, X28, offsetof(A32JitState, cpsr_nzcv));
    code.UBFX(result, result, 30, 1);
    ctx.reg_alloc.DefineValue(inst, result);
}

void A32EmitA64::EmitA32SetZFlag(A32EmitContext& ctx, IR::Inst* inst) {
    constexpr size_t flag_bit = 30;
    constexpr u32 flag_mask = 1u << flag_bit;
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    Arm64Gen::ARM64Reg nzcv = DecodeReg(ctx.reg_alloc.ScratchGpr());

    code.LDR(INDEX_UNSIGNED, nzcv, X28, offsetof(A32JitState, cpsr_nzcv));
    if (args[0].IsImmediate()) {
        if (args[0].GetImmediateU1()) {
            code.ORRI2R(nzcv, nzcv, flag_mask);
        } else {
            code.ANDI2R(nzcv, nzcv, ~flag_mask);
        }
    } else {
        Arm64Gen::ARM64Reg to_store = DecodeReg(ctx.reg_alloc.UseScratchGpr(args[0]));

        code.BFI(nzcv, to_store, flag_bit, 1);
    }
    code.STR(INDEX_UNSIGNED, nzcv, X28, offsetof(A32JitState, cpsr_nzcv));
}

void A32EmitA64::EmitA32SetCheckBit(A32EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const ARM64Reg to_store = DecodeReg(ctx.reg_alloc.UseGpr(args[0]));
    code.STRB(INDEX_UNSIGNED, to_store, X28, offsetof(A32JitState, check_bit));
}

void A32EmitA64::EmitA32GetCFlag(A32EmitContext& ctx, IR::Inst* inst) {
    Arm64Gen::ARM64Reg result = DecodeReg(ctx.reg_alloc.ScratchGpr());
    code.LDR(INDEX_UNSIGNED, result, X28, offsetof(A32JitState, cpsr_nzcv));
    code.UBFX(result, result, 29, 1);
    ctx.reg_alloc.DefineValue(inst, result);
}

void A32EmitA64::EmitA32SetCFlag(A32EmitContext& ctx, IR::Inst* inst) {
    constexpr size_t flag_bit = 29;
    constexpr u32 flag_mask = 1u << flag_bit;
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    Arm64Gen::ARM64Reg nzcv = DecodeReg(ctx.reg_alloc.ScratchGpr());

    code.LDR(INDEX_UNSIGNED, nzcv, X28, offsetof(A32JitState, cpsr_nzcv));
    if (args[0].IsImmediate()) {
        if (args[0].GetImmediateU1()) {
            code.ORRI2R(nzcv, nzcv, flag_mask);
        } else {
            code.ANDI2R(nzcv, nzcv, ~flag_mask);
        }
    } else {
        Arm64Gen::ARM64Reg to_store = DecodeReg(ctx.reg_alloc.UseScratchGpr(args[0]));
        code.BFI(nzcv, to_store, flag_bit, 1);
    }
    code.STR(INDEX_UNSIGNED, nzcv, X28, offsetof(A32JitState, cpsr_nzcv));
}

void A32EmitA64::EmitA32GetVFlag(A32EmitContext& ctx, IR::Inst* inst) {
    Arm64Gen::ARM64Reg result = DecodeReg(ctx.reg_alloc.ScratchGpr());
    code.LDR(INDEX_UNSIGNED, result, X28, offsetof(A32JitState, cpsr_nzcv));
    code.UBFX(result, result, 28, 1);
    ctx.reg_alloc.DefineValue(inst, result);
}

void A32EmitA64::EmitA32SetVFlag(A32EmitContext& ctx, IR::Inst* inst) {
    constexpr size_t flag_bit = 28;
    constexpr u32 flag_mask = 1u << flag_bit;
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    Arm64Gen::ARM64Reg nzcv = DecodeReg(ctx.reg_alloc.ScratchGpr());

    code.LDR(INDEX_UNSIGNED, nzcv, X28, offsetof(A32JitState, cpsr_nzcv));
    if (args[0].IsImmediate()) {
        if (args[0].GetImmediateU1()) {
            code.ORRI2R(nzcv, nzcv, flag_mask);
        } else {
            code.ANDI2R(nzcv, nzcv, ~flag_mask);
        }
    } else {
        Arm64Gen::ARM64Reg to_store = DecodeReg(ctx.reg_alloc.UseGpr(args[0]));

        code.BFI(nzcv, to_store, flag_bit, 1);
    }
    code.STR(INDEX_UNSIGNED, nzcv, X28, offsetof(A32JitState, cpsr_nzcv));
}

void A32EmitA64::EmitA32OrQFlag(A32EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    if (args[0].IsImmediate()) {
        if (args[0].GetImmediateU1()) {
            ARM64Reg to_store = DecodeReg(ctx.reg_alloc.UseGpr(args[0]));
            code.STR(INDEX_UNSIGNED, to_store, X28, offsetof(A32JitState, cpsr_q));
        }
    } else {
        ARM64Reg to_store = ctx.reg_alloc.UseGpr(args[0]);
        ARM64Reg scratch = DecodeReg(ctx.reg_alloc.ScratchGpr());

        code.LDR(INDEX_UNSIGNED, scratch, X28, offsetof(A32JitState, cpsr_q));
        code.ORR(scratch, scratch, to_store);
        code.STR(INDEX_UNSIGNED, scratch, X28, offsetof(A32JitState, cpsr_q));
    }
}

void A32EmitA64::EmitA32GetGEFlags(A32EmitContext& ctx, IR::Inst* inst) {
    ARM64Reg result = EncodeRegToSingle(ctx.reg_alloc.ScratchFpr());
    code.LDR(INDEX_UNSIGNED, result, X28, offsetof(A32JitState, cpsr_ge));
    ctx.reg_alloc.DefineValue(inst, result);
}

void A32EmitA64::EmitA32SetGEFlags(A32EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ASSERT(!args[0].IsImmediate());
    ARM64Reg to_store = INVALID_REG;
    if (args[0].IsInFpr()) {
        to_store = EncodeRegToSingle(ctx.reg_alloc.UseFpr(args[0]));
    } else {
        to_store = DecodeReg(ctx.reg_alloc.UseGpr(args[0]));
    }
    code.STR(INDEX_UNSIGNED, to_store, X28, offsetof(A32JitState, cpsr_ge));
}

void A32EmitA64::EmitA32SetGEFlagsCompressed(A32EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    if (args[0].IsImmediate()) {
        ARM64Reg to_store = DecodeReg(ctx.reg_alloc.ScratchGpr());
        u32 imm = args[0].GetImmediateU32();
        u32 ge = 0;
        ge |= Common::Bit<19>(imm) ? 0xFF000000 : 0;
        ge |= Common::Bit<18>(imm) ? 0x00FF0000 : 0;
        ge |= Common::Bit<17>(imm) ? 0x0000FF00 : 0;
        ge |= Common::Bit<16>(imm) ? 0x000000FF : 0;

        code.MOVI2R(to_store, ge);
        code.STR(INDEX_UNSIGNED, to_store, X28, offsetof(A32JitState, cpsr_ge));
    } else {
        ARM64Reg a = DecodeReg(ctx.reg_alloc.UseScratchGpr(args[0]));
        ARM64Reg scratch = DecodeReg(ctx.reg_alloc.ScratchGpr());

        code.UBFX(a, a, 16, 4);
        code.MOVI2R(scratch, 0x00204081);
        code.MUL(a, a, scratch);
        code.ANDI2R(a, a, 0x01010101);
        code.ORR(a, a, a, ArithOption{a, ST_LSL, 1});
        code.STR(INDEX_UNSIGNED, a, X28, offsetof(A32JitState, cpsr_ge));
    }
}

void A32EmitA64::EmitA32BXWritePC(A32EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    auto& arg = args[0];

    const u32 upper_without_t =
        (ctx.Location().SetSingleStepping(false).UniqueHash() >> 32) & 0xFFFFFFFE;

    // Pseudocode:
    // if (new_pc & 1) {
    //    new_pc &= 0xFFFFFFFE;
    //    cpsr.T = true;
    // } else {
    //    new_pc &= 0xFFFFFFFC;
    //    cpsr.T = false;
    // }
    // We rely on the fact we disallow EFlag from changing within a block.

    if (arg.IsImmediate()) {
        const ARM64Reg scratch = DecodeReg(ctx.reg_alloc.ScratchGpr());
        u32 new_pc = arg.GetImmediateU32();
        const u32 mask = Common::Bit<0>(new_pc) ? 0xFFFFFFFE : 0xFFFFFFFC;
        const u32 new_upper = upper_without_t | (Common::Bit<0>(new_pc) ? 1 : 0);

        code.MOVI2R(scratch, new_pc & mask);
        code.STR(INDEX_UNSIGNED, scratch, X28, MJitStateReg(A32::Reg::PC));
        code.MOVI2R(scratch, new_upper);
        code.STR(INDEX_UNSIGNED, scratch, X28, offsetof(A32JitState, upper_location_descriptor));
    } else {
        const ARM64Reg new_pc = DecodeReg(ctx.reg_alloc.UseScratchGpr(arg));
        const ARM64Reg mask = DecodeReg(ctx.reg_alloc.ScratchGpr());
        const ARM64Reg new_upper = DecodeReg(ctx.reg_alloc.ScratchGpr());

        code.ANDI2R(mask, new_pc, 1);
        code.MOVI2R(new_upper, upper_without_t);
        code.ADD(new_upper, new_upper, mask);
        code.STR(INDEX_UNSIGNED, new_upper, X28, offsetof(A32JitState, upper_location_descriptor));
        code.LSL(mask, mask, 1);
        code.SUBI2R(mask, mask, 4); // mask = pc & 1 ? 0xFFFFFFFE : 0xFFFFFFFC
        code.AND(new_pc, new_pc, mask);
        code.STR(INDEX_UNSIGNED, new_pc, X28, MJitStateReg(A32::Reg::PC));
    }
}

void A32EmitA64::EmitA32CallSupervisor(A32EmitContext& ctx, IR::Inst* inst) {
    ctx.reg_alloc.HostCall(nullptr);

    code.SwitchFpscrOnExit();
    code.LDR(INDEX_UNSIGNED, code.ABI_PARAM2, X28, offsetof(A32JitState, cycles_to_run));
    code.SUB(code.ABI_PARAM2, code.ABI_PARAM2, X26);

    Devirtualize<&A32::UserCallbacks::AddTicks>(config.callbacks).EmitCall(code);
    ctx.reg_alloc.EndOfAllocScope();
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ctx.reg_alloc.HostCall(nullptr, {}, args[0]);
    Devirtualize<&A32::UserCallbacks::CallSVC>(config.callbacks).EmitCall(code);
    Devirtualize<&A32::UserCallbacks::GetTicksRemaining>(config.callbacks).EmitCall(code);
    code.STR(INDEX_UNSIGNED, code.ABI_RETURN, X28, offsetof(A32JitState, cycles_to_run));
    code.MOV(X26, code.ABI_RETURN);
    code.SwitchFpscrOnEntry();
}

void A32EmitA64::EmitA32ExceptionRaised(A32EmitContext& ctx, IR::Inst* inst) {
    ctx.reg_alloc.HostCall(nullptr);
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ASSERT(args[0].IsImmediate() && args[1].IsImmediate());
    u32 pc = args[0].GetImmediateU32();
    u64 exception = args[1].GetImmediateU64();
    Devirtualize<&A32::UserCallbacks::ExceptionRaised>(config.callbacks)
        .EmitCall(code, [&](RegList param) {
            code.MOVI2R(param[0], pc);
            code.MOVI2R(param[1], exception);
        });
}

static u32 GetFpscrImpl(A32JitState* jit_state) {
    return jit_state->Fpscr();
}

void A32EmitA64::EmitA32GetFpscr(A32EmitContext& ctx, IR::Inst* inst) {
    ctx.reg_alloc.HostCall(inst);
    // Use an unused HostCall register
    const ARM64Reg fpsr = X9;
    const ARM64Reg fpcr = X10;
    code.MOV(code.ABI_PARAM1, X28);

    code.MRS(fpsr, FIELD_FPSR);
    code.MRS(fpcr, FIELD_FPCR);
    code.STR(INDEX_UNSIGNED, fpsr, X28, offsetof(A32JitState, guest_fpsr));
    code.STR(INDEX_UNSIGNED, fpcr, X28, offsetof(A32JitState, guest_fpcr));
    code.QuickCallFunction(&GetFpscrImpl);
}

static void SetFpscrImpl(u32 value, A32JitState* jit_state) {
    jit_state->SetFpscr(value);
}

void A32EmitA64::EmitA32SetFpscr(A32EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ctx.reg_alloc.HostCall(nullptr, args[0]);
    // Use an unused HostCall register
    const ARM64Reg fpsr = X9;
    const ARM64Reg fpcr = X10;

    code.MOV(code.ABI_PARAM2, X28);

    code.QuickCallFunction(&SetFpscrImpl);

    code.LDR(INDEX_UNSIGNED, fpsr, X28, offsetof(A32JitState, guest_fpsr));
    code.LDR(INDEX_UNSIGNED, fpcr, X28, offsetof(A32JitState, guest_fpcr));
    code._MSR(FIELD_FPSR, fpsr);
    code._MSR(FIELD_FPCR, fpcr);
}

void A32EmitA64::EmitA32GetFpscrNZCV(A32EmitContext& ctx, IR::Inst* inst) {
    ARM64Reg result = DecodeReg(ctx.reg_alloc.ScratchGpr());
    code.LDR(INDEX_UNSIGNED, result, X28, offsetof(A32JitState, fpsr_nzcv));
    ctx.reg_alloc.DefineValue(inst, result);
}

void A32EmitA64::EmitA32SetFpscrNZCV(A32EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ARM64Reg value = DecodeReg(ctx.reg_alloc.UseScratchGpr(args[0]));

    code.ANDI2R(value, value, 0xF0000000);

    code.STR(INDEX_UNSIGNED, value, X28, offsetof(A32JitState, fpsr_nzcv));
}

void A32EmitA64::EmitA32ClearExclusive(A32EmitContext&, IR::Inst*) {
    code.STR(INDEX_UNSIGNED, WZR, X28, offsetof(A32JitState, exclusive_state));
}

void A32EmitA64::EmitA32SetExclusive(A32EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ASSERT(args[1].IsImmediate());
    Arm64Gen::ARM64Reg address = DecodeReg(ctx.reg_alloc.UseGpr(args[0]));
    Arm64Gen::ARM64Reg state = DecodeReg(ctx.reg_alloc.ScratchGpr());

    code.MOVI2R(state, u8(1));
    code.STR(INDEX_UNSIGNED, state, X28, offsetof(A32JitState, exclusive_state));
    code.STR(INDEX_UNSIGNED, address, X28, offsetof(A32JitState, exclusive_address));
}

A32EmitA64::DoNotFastmemMarker A32EmitA64::GenerateDoNotFastmemMarker(A32EmitContext& ctx,
                                                                      IR::Inst* inst) {
    return std::make_tuple(ctx.Location(), ctx.GetInstOffset(inst));
}

bool A32EmitA64::ShouldFastmem(const DoNotFastmemMarker& marker) const {
    return config.fastmem_pointer && exception_handler.SupportsFastmem() &&
           do_not_fastmem.count(marker) == 0;
}

void A32EmitA64::DoNotFastmem(const DoNotFastmemMarker& marker) {
    do_not_fastmem.emplace(marker);
    InvalidateBasicBlocks({std::get<0>(marker)});
}

template <typename T>
void A32EmitA64::ReadMemory(A32EmitContext& ctx, IR::Inst* inst, const CodePtr callback_fn) {
    constexpr size_t bit_size = Common::BitSize<T>();
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);

    ctx.reg_alloc.UseScratch(args[0], ABI_PARAM2);
    ctx.reg_alloc.ScratchGpr({ABI_RETURN});

    ARM64Reg result = ctx.reg_alloc.ScratchGpr();
    ARM64Reg vaddr = DecodeReg(code.ABI_PARAM2);
    ARM64Reg tmp = code.ABI_RETURN;

    const auto do_not_fastmem_marker = GenerateDoNotFastmemMarker(ctx, inst);

    const auto page_table_lookup = [this, result, vaddr, tmp, callback_fn](FixupBranch& end) {
        constexpr size_t bit_size = Common::BitSize<T>();

        code.MOVP2R(result, config.page_table);
        code.MOV(tmp, vaddr, ArithOption{vaddr, ST_LSR, 12});
        code.LDR(result, result, ArithOption{tmp, true});
        FixupBranch abort = code.CBZ(result);
        code.ANDI2R(vaddr, vaddr, 4095);
        switch (bit_size) {
        case 8:
            code.LDRB(DecodeReg(result), result, vaddr);
            break;
        case 16:
            code.LDRH(DecodeReg(result), result, vaddr);
            break;
        case 32:
            code.LDR(DecodeReg(result), result, vaddr);
            break;
        case 64:
            code.LDR(result, result, vaddr);
            break;
        default:
            ASSERT_FALSE("Invalid bit_size");
            break;
        }
        end = code.B();
        code.SetJumpTarget(abort);
        code.BL(callback_fn);
        code.MOV(result, code.ABI_RETURN);
    };

    if (ShouldFastmem(do_not_fastmem_marker)) {
        const CodePtr patch_location = code.GetCodePtr();
        switch (bit_size) {
        case 8:
            code.LDRB(DecodeReg(result), X27, vaddr);
            break;
        case 16:
            code.LDRH(DecodeReg(result), X27, vaddr);
            break;
        case 32:
            code.LDR(DecodeReg(result), X27, vaddr);
            break;
        case 64:
            code.LDR(result, X27, vaddr);
            break;
        default:
            ASSERT_FALSE("Invalid bit_size");
            break;
        }

        fastmem_patch_info.emplace(
            patch_location, FastmemPatchInfo{[this, patch_location, page_table_lookup, callback_fn,
                                              result, do_not_fastmem_marker] {
                CodePtr save_code_ptr = code.GetCodePtr();
                code.SetCodePtr(patch_location);
                FixupBranch thunk = code.B();
                u8* end_ptr = code.GetWritableCodePtr();
                code.FlushIcacheSection(reinterpret_cast<const u8*>(patch_location), end_ptr);
                code.SetCodePtr(save_code_ptr);
                code.SwitchToFarCode();
                code.SetJumpTarget(thunk);
                if (config.page_table) {
                    FixupBranch end{};
                    page_table_lookup(end);
                    code.SetJumpTarget(end, end_ptr);
                } else {
                    code.BL(callback_fn);
                    code.MOV(result, code.ABI_RETURN);
                }
                code.B(end_ptr);
                code.FlushIcache();
                code.SwitchToNearCode();

                DoNotFastmem(do_not_fastmem_marker);
            }});

        ctx.reg_alloc.DefineValue(inst, result);
        return;
    }

    if (!config.page_table) {
        code.BL(callback_fn);
        code.MOV(result, code.ABI_RETURN);
        ctx.reg_alloc.DefineValue(inst, result);
        return;
    }

    FixupBranch end{};
    page_table_lookup(end);
    code.SetJumpTarget(end);

    ctx.reg_alloc.DefineValue(inst, result);
}

template <typename T>
void A32EmitA64::WriteMemory(A32EmitContext& ctx, IR::Inst* inst, const CodePtr callback_fn) {
    constexpr size_t bit_size = Common::BitSize<T>();
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);

    ctx.reg_alloc.ScratchGpr({ABI_RETURN});
    ctx.reg_alloc.UseScratch(args[0], ABI_PARAM2);
    ctx.reg_alloc.UseScratch(args[1], ABI_PARAM3);

    ARM64Reg vaddr = DecodeReg(code.ABI_PARAM2);
    ARM64Reg value = code.ABI_PARAM3;
    ARM64Reg page_index = ctx.reg_alloc.ScratchGpr();
    ARM64Reg addr = ctx.reg_alloc.ScratchGpr();

    const auto do_not_fastmem_marker = GenerateDoNotFastmemMarker(ctx, inst);

    const auto page_table_lookup = [this, vaddr, value, page_index, addr,
                                    callback_fn](FixupBranch& end) {
        constexpr size_t bit_size = Common::BitSize<T>();

        code.MOVP2R(addr, config.page_table);
        code.MOV(DecodeReg(page_index), vaddr, ArithOption{vaddr, ST_LSR, 12});
        code.LDR(addr, addr, ArithOption{page_index, true});
        FixupBranch abort = code.CBZ(addr);
        code.ANDI2R(vaddr, vaddr, 4095);
        switch (bit_size) {
        case 8:
            code.STRB(DecodeReg(value), addr, vaddr);
            break;
        case 16:
            code.STRH(DecodeReg(value), addr, vaddr);
            break;
        case 32:
            code.STR(DecodeReg(value), addr, vaddr);
            ;
            break;
        case 64:
            code.STR(value, addr, vaddr);
            break;
        default:
            ASSERT_FALSE("Invalid bit_size");
            break;
        }
        end = code.B();
        code.SetJumpTarget(abort);
        code.BL(callback_fn);
    };

    if (ShouldFastmem(do_not_fastmem_marker)) {
        const CodePtr patch_location = code.GetCodePtr();
        switch (bit_size) {
        case 8:
            code.STRB(DecodeReg(value), X27, vaddr);
            break;
        case 16:
            code.STRH(DecodeReg(value), X27, vaddr);
            break;
        case 32:
            code.STR(DecodeReg(value), X27, vaddr);
            break;
        case 64:
            code.STR(value, X27, vaddr);
            break;
        default:
            ASSERT_FALSE("Invalid bit_size");
            break;
        }

        fastmem_patch_info.emplace(
            patch_location, FastmemPatchInfo{[this, patch_location, page_table_lookup, callback_fn,
                                              do_not_fastmem_marker] {
                CodePtr save_code_ptr = code.GetCodePtr();
                code.SetCodePtr(patch_location);
                FixupBranch thunk = code.B();
                u8* end_ptr = code.GetWritableCodePtr();
                code.FlushIcacheSection(reinterpret_cast<const u8*>(patch_location), end_ptr);
                code.SetCodePtr(save_code_ptr);
                code.SwitchToFarCode();
                code.SetJumpTarget(thunk);
                if (config.page_table) {
                    FixupBranch end{};
                    page_table_lookup(end);
                    code.SetJumpTarget(end, end_ptr);
                } else {
                    code.BL(callback_fn);
                }
                code.B(end_ptr);
                code.FlushIcache();
                code.SwitchToNearCode();

                DoNotFastmem(do_not_fastmem_marker);
            }});
        return;
    }

    if (!config.page_table) {
        code.BL(callback_fn);
        return;
    }

    FixupBranch end{};
    page_table_lookup(end);
    code.SetJumpTarget(end);
}

void A32EmitA64::EmitA32ReadMemory8(A32EmitContext& ctx, IR::Inst* inst) {
    ReadMemory<u8>(ctx, inst, read_memory_8);
}

void A32EmitA64::EmitA32ReadMemory16(A32EmitContext& ctx, IR::Inst* inst) {
    ReadMemory<u16>(ctx, inst, read_memory_16);
}

void A32EmitA64::EmitA32ReadMemory32(A32EmitContext& ctx, IR::Inst* inst) {
    ReadMemory<u32>(ctx, inst, read_memory_32);
}

void A32EmitA64::EmitA32ReadMemory64(A32EmitContext& ctx, IR::Inst* inst) {
    ReadMemory<u64>(ctx, inst, read_memory_64);
}

void A32EmitA64::EmitA32WriteMemory8(A32EmitContext& ctx, IR::Inst* inst) {
    WriteMemory<u8>(ctx, inst, write_memory_8);
}

void A32EmitA64::EmitA32WriteMemory16(A32EmitContext& ctx, IR::Inst* inst) {
    WriteMemory<u16>(ctx, inst, write_memory_16);
}

void A32EmitA64::EmitA32WriteMemory32(A32EmitContext& ctx, IR::Inst* inst) {
    WriteMemory<u32>(ctx, inst, write_memory_32);
}

void A32EmitA64::EmitA32WriteMemory64(A32EmitContext& ctx, IR::Inst* inst) {
    WriteMemory<u64>(ctx, inst, write_memory_64);
}

template <typename T, void (A32::UserCallbacks::*fn)(A32::VAddr, T)>
static void ExclusiveWrite(BlockOfCode& code, RegAlloc& reg_alloc, IR::Inst* inst,
                           const A32::UserConfig& config) {
    auto args = reg_alloc.GetArgumentInfo(inst);
    reg_alloc.HostCall(nullptr, {}, args[0], args[1]);

    // Use unused HostCall registers
    ARM64Reg passed = W9;
    ARM64Reg tmp = W10;

    std::vector<FixupBranch> end;

    code.MOVI2R(passed, u32(1));
    code.LDR(INDEX_UNSIGNED, tmp, X28, offsetof(A32JitState, exclusive_state));
    end.push_back(code.CBZ(tmp));
    code.LDR(INDEX_UNSIGNED, tmp, X28, offsetof(A32JitState, exclusive_address));
    code.EOR(tmp, code.ABI_PARAM2, tmp);
    code.TSTI2R(tmp, A32JitState::RESERVATION_GRANULE_MASK, reg_alloc.ScratchGpr());
    end.push_back(code.B(CC_NEQ));
    code.STR(INDEX_UNSIGNED, WZR, X28, offsetof(A32JitState, exclusive_state));

    Devirtualize<fn>(config.callbacks).EmitCall(code);
    code.MOVI2R(passed, 0);

    for (FixupBranch e : end) {
        code.SetJumpTarget(e);
    }

    reg_alloc.DefineValue(inst, passed);
}

void A32EmitA64::EmitA32ExclusiveWriteMemory8(A32EmitContext& ctx, IR::Inst* inst) {
    ExclusiveWrite<u8, &A32::UserCallbacks::MemoryWrite8>(code, ctx.reg_alloc, inst, config);
}

void A32EmitA64::EmitA32ExclusiveWriteMemory16(A32EmitContext& ctx, IR::Inst* inst) {
    ExclusiveWrite<u16, &A32::UserCallbacks::MemoryWrite16>(code, ctx.reg_alloc, inst, config);
}

void A32EmitA64::EmitA32ExclusiveWriteMemory32(A32EmitContext& ctx, IR::Inst* inst) {
    ExclusiveWrite<u32, &A32::UserCallbacks::MemoryWrite32>(code, ctx.reg_alloc, inst, config);
}

void A32EmitA64::EmitA32ExclusiveWriteMemory64(A32EmitContext& ctx, IR::Inst* inst) {
    ExclusiveWrite<u64, &A32::UserCallbacks::MemoryWrite64>(code, ctx.reg_alloc, inst, config);
}

static void EmitCoprocessorException() {
    ASSERT_FALSE("Should raise coproc exception here");
}

static void CallCoprocCallback(BlockOfCode& code, RegAlloc& reg_alloc, A32::Jit* jit_interface,
                               A32::Coprocessor::Callback callback, IR::Inst* inst = nullptr,
                               std::optional<Argument::copyable_reference> arg0 = {},
                               std::optional<Argument::copyable_reference> arg1 = {}) {
    reg_alloc.HostCall(inst, {}, {}, arg0, arg1);

    code.MOVP2R(code.ABI_PARAM1, jit_interface);
    if (callback.user_arg) {
        code.MOVP2R(code.ABI_PARAM2, *callback.user_arg);
    }

    code.QuickCallFunction(callback.function);
}

void A32EmitA64::EmitA32CoprocInternalOperation(A32EmitContext& ctx, IR::Inst* inst) {
    auto coproc_info = inst->GetArg(0).GetCoprocInfo();

    size_t coproc_num = coproc_info[0];
    bool two = coproc_info[1] != 0;
    unsigned opc1 = static_cast<unsigned>(coproc_info[2]);
    A32::CoprocReg CRd = static_cast<A32::CoprocReg>(coproc_info[3]);
    A32::CoprocReg CRn = static_cast<A32::CoprocReg>(coproc_info[4]);
    A32::CoprocReg CRm = static_cast<A32::CoprocReg>(coproc_info[5]);
    unsigned opc2 = static_cast<unsigned>(coproc_info[6]);

    std::shared_ptr<A32::Coprocessor> coproc = config.coprocessors[coproc_num];
    if (!coproc) {
        EmitCoprocessorException();
        return;
    }

    auto action = coproc->CompileInternalOperation(two, opc1, CRd, CRn, CRm, opc2);
    if (!action) {
        EmitCoprocessorException();
        return;
    }

    CallCoprocCallback(code, ctx.reg_alloc, jit_interface, *action);
}

void A32EmitA64::EmitA32CoprocSendOneWord(A32EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    auto coproc_info = inst->GetArg(0).GetCoprocInfo();

    size_t coproc_num = coproc_info[0];
    bool two = coproc_info[1] != 0;
    unsigned opc1 = static_cast<unsigned>(coproc_info[2]);
    A32::CoprocReg CRn = static_cast<A32::CoprocReg>(coproc_info[3]);
    A32::CoprocReg CRm = static_cast<A32::CoprocReg>(coproc_info[4]);
    unsigned opc2 = static_cast<unsigned>(coproc_info[5]);

    std::shared_ptr<A32::Coprocessor> coproc = config.coprocessors[coproc_num];
    if (!coproc) {
        EmitCoprocessorException();
        return;
    }

    auto action = coproc->CompileSendOneWord(two, opc1, CRn, CRm, opc2);
    switch (action.index()) {
    case 0:
        EmitCoprocessorException();
        return;
    case 1:
        CallCoprocCallback(code, ctx.reg_alloc, jit_interface,
                           std::get<A32::Coprocessor::Callback>(action), nullptr, args[1]);
        return;
    case 2: {
        u32* destination_ptr = std::get<u32*>(action);

        ARM64Reg reg_word = DecodeReg(ctx.reg_alloc.UseGpr(args[1]));
        ARM64Reg reg_destination_addr = ctx.reg_alloc.ScratchGpr();

        code.MOVP2R(reg_destination_addr, destination_ptr);
        code.STR(INDEX_UNSIGNED, reg_word, reg_destination_addr, 0);

        return;
    }
    default:
        UNREACHABLE();
    }
}

void A32EmitA64::EmitA32CoprocSendTwoWords(A32EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    auto coproc_info = inst->GetArg(0).GetCoprocInfo();

    size_t coproc_num = coproc_info[0];
    bool two = coproc_info[1] != 0;
    unsigned opc = static_cast<unsigned>(coproc_info[2]);
    A32::CoprocReg CRm = static_cast<A32::CoprocReg>(coproc_info[3]);

    std::shared_ptr<A32::Coprocessor> coproc = config.coprocessors[coproc_num];
    if (!coproc) {
        EmitCoprocessorException();
        return;
    }

    auto action = coproc->CompileSendTwoWords(two, opc, CRm);
    switch (action.index()) {
    case 0:
        EmitCoprocessorException();
        return;
    case 1:
        CallCoprocCallback(code, ctx.reg_alloc, jit_interface,
                           std::get<A32::Coprocessor::Callback>(action), nullptr, args[1], args[2]);
        return;
    case 2: {
        auto destination_ptrs = std::get<std::array<u32*, 2>>(action);

        ARM64Reg reg_word1 = DecodeReg(ctx.reg_alloc.UseGpr(args[1]));
        ARM64Reg reg_word2 = DecodeReg(ctx.reg_alloc.UseGpr(args[2]));
        ARM64Reg reg_destination_addr = ctx.reg_alloc.ScratchGpr();

        code.MOVP2R(reg_destination_addr, destination_ptrs[0]);
        code.STR(INDEX_UNSIGNED, reg_word1, reg_destination_addr, 0);
        code.MOVP2R(reg_destination_addr, destination_ptrs[1]);
        code.STR(INDEX_UNSIGNED, reg_word2, reg_destination_addr, 0);

        return;
    }
    default:
        UNREACHABLE();
    }
}

void A32EmitA64::EmitA32CoprocGetOneWord(A32EmitContext& ctx, IR::Inst* inst) {
    auto coproc_info = inst->GetArg(0).GetCoprocInfo();

    size_t coproc_num = coproc_info[0];
    bool two = coproc_info[1] != 0;
    unsigned opc1 = static_cast<unsigned>(coproc_info[2]);
    A32::CoprocReg CRn = static_cast<A32::CoprocReg>(coproc_info[3]);
    A32::CoprocReg CRm = static_cast<A32::CoprocReg>(coproc_info[4]);
    unsigned opc2 = static_cast<unsigned>(coproc_info[5]);

    std::shared_ptr<A32::Coprocessor> coproc = config.coprocessors[coproc_num];
    if (!coproc) {
        EmitCoprocessorException();
        return;
    }

    auto action = coproc->CompileGetOneWord(two, opc1, CRn, CRm, opc2);
    switch (action.index()) {
    case 0:
        EmitCoprocessorException();
        return;
    case 1:
        CallCoprocCallback(code, ctx.reg_alloc, jit_interface,
                           std::get<A32::Coprocessor::Callback>(action), inst);
        return;
    case 2: {
        u32* source_ptr = std::get<u32*>(action);

        ARM64Reg result = ctx.reg_alloc.ScratchGpr();

        code.MOVP2R(result, source_ptr);
        code.LDR(INDEX_UNSIGNED, DecodeReg(result), result, 0);

        ctx.reg_alloc.DefineValue(inst, result);

        return;
    }
    default:
        UNREACHABLE();
    }
}

void A32EmitA64::EmitA32CoprocGetTwoWords(A32EmitContext& ctx, IR::Inst* inst) {
    auto coproc_info = inst->GetArg(0).GetCoprocInfo();

    size_t coproc_num = coproc_info[0];
    bool two = coproc_info[1] != 0;
    unsigned opc = coproc_info[2];
    A32::CoprocReg CRm = static_cast<A32::CoprocReg>(coproc_info[3]);

    std::shared_ptr<A32::Coprocessor> coproc = config.coprocessors[coproc_num];
    if (!coproc) {
        EmitCoprocessorException();
        return;
    }

    auto action = coproc->CompileGetTwoWords(two, opc, CRm);
    switch (action.index()) {
    case 0:
        EmitCoprocessorException();
        return;
    case 1:
        CallCoprocCallback(code, ctx.reg_alloc, jit_interface,
                           std::get<A32::Coprocessor::Callback>(action), inst);
        return;
    case 2: {
        auto source_ptrs = std::get<std::array<u32*, 2>>(action);

        ARM64Reg reg_result = ctx.reg_alloc.ScratchGpr();
        ARM64Reg reg_tmp = ctx.reg_alloc.ScratchGpr();

        code.MOVP2R(reg_tmp, source_ptrs[1]);
        code.LDR(INDEX_UNSIGNED, DecodeReg(reg_result), reg_tmp, 0);
        code.MOVP2R(reg_tmp, source_ptrs[0]);
        code.LDR(INDEX_UNSIGNED, DecodeReg(reg_tmp), reg_tmp, 0);
        code.ORR(reg_result, reg_tmp, reg_result, ArithOption{reg_result, ST_LSL, 32});

        ctx.reg_alloc.DefineValue(inst, reg_result);

        return;
    }
    default:
        UNREACHABLE();
    }
}

void A32EmitA64::EmitA32CoprocLoadWords(A32EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    auto coproc_info = inst->GetArg(0).GetCoprocInfo();

    size_t coproc_num = coproc_info[0];
    bool two = coproc_info[1] != 0;
    bool long_transfer = coproc_info[2] != 0;
    A32::CoprocReg CRd = static_cast<A32::CoprocReg>(coproc_info[3]);
    bool has_option = coproc_info[4] != 0;
    std::optional<u8> option = std::nullopt;
    if (has_option) {
        option = coproc_info[5];
    }

    std::shared_ptr<A32::Coprocessor> coproc = config.coprocessors[coproc_num];
    if (!coproc) {
        EmitCoprocessorException();
        return;
    }

    auto action = coproc->CompileLoadWords(two, long_transfer, CRd, option);
    if (!action) {
        EmitCoprocessorException();
        return;
    }

    CallCoprocCallback(code, ctx.reg_alloc, jit_interface, *action, nullptr, args[1]);
}

void A32EmitA64::EmitA32CoprocStoreWords(A32EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    auto coproc_info = inst->GetArg(0).GetCoprocInfo();

    size_t coproc_num = coproc_info[0];
    bool two = coproc_info[1] != 0;
    bool long_transfer = coproc_info[2] != 0;
    A32::CoprocReg CRd = static_cast<A32::CoprocReg>(coproc_info[3]);
    bool has_option = coproc_info[4] != 0;
    std::optional<u8> option = std::nullopt;
    if (has_option) {
        option = coproc_info[5];
    }

    std::shared_ptr<A32::Coprocessor> coproc = config.coprocessors[coproc_num];
    if (!coproc) {
        EmitCoprocessorException();
        return;
    }

    auto action = coproc->CompileStoreWords(two, long_transfer, CRd, option);
    if (!action) {
        EmitCoprocessorException();
        return;
    }

    CallCoprocCallback(code, ctx.reg_alloc, jit_interface, *action, nullptr, args[1]);
}

std::string A32EmitA64::LocationDescriptorToFriendlyName(
    const IR::LocationDescriptor& ir_descriptor) const {
    const A32::LocationDescriptor descriptor{ir_descriptor};
    return fmt::format("a32_{}{:08X}_{}_fpcr{:08X}", descriptor.TFlag() ? "t" : "a",
                       descriptor.PC(), descriptor.EFlag() ? "be" : "le",
                       descriptor.FPSCR().Value());
}

void A32EmitA64::FastmemCallback(CodePtr PC) {
    const auto iter = fastmem_patch_info.find(PC);
    ASSERT(iter != fastmem_patch_info.end());
    iter->second.callback();
    fastmem_patch_info.erase(iter);
}

void A32EmitA64::EmitTerminalImpl(IR::Term::Interpret terminal,
                                  IR::LocationDescriptor initial_location, bool) {
    ASSERT_MSG(A32::LocationDescriptor{terminal.next}.TFlag() ==
                   A32::LocationDescriptor{initial_location}.TFlag(),
               "Unimplemented");
    ASSERT_MSG(A32::LocationDescriptor{terminal.next}.EFlag() ==
                   A32::LocationDescriptor{initial_location}.EFlag(),
               "Unimplemented");

    code.MOVI2R(DecodeReg(code.ABI_PARAM2), A32::LocationDescriptor{terminal.next}.PC());
    code.MOVI2R(DecodeReg(code.ABI_PARAM3), terminal.num_instructions);
    code.STR(INDEX_UNSIGNED, DecodeReg(code.ABI_PARAM2), X28, MJitStateReg(A32::Reg::PC));
    code.SwitchFpscrOnExit();
    Devirtualize<&A32::UserCallbacks::InterpreterFallback>(config.callbacks).EmitCall(code);
    code.ReturnFromRunCode(true); // TODO: Check cycles
}

void A32EmitA64::EmitTerminalImpl(IR::Term::ReturnToDispatch, IR::LocationDescriptor, bool) {
    code.ReturnFromRunCode();
}

void A32EmitA64::EmitSetUpperLocationDescriptor(IR::LocationDescriptor new_location,
                                                IR::LocationDescriptor old_location) {
    auto get_upper = [](const IR::LocationDescriptor& desc) -> u32 {
        return static_cast<u32>(
            A32::LocationDescriptor{desc}.SetSingleStepping(false).UniqueHash() >> 32);
    };

    const u32 old_upper = get_upper(old_location);
    const u32 new_upper = [&] {
        const u32 mask = ~u32(config.always_little_endian ? 0x2 : 0);
        return get_upper(new_location) & mask;
    }();

    if (old_upper != new_upper) {
        code.MOVI2R(DecodeReg(code.ABI_SCRATCH1), new_upper);
        code.STR(INDEX_UNSIGNED, DecodeReg(code.ABI_SCRATCH1), X28,
                 offsetof(A32JitState, upper_location_descriptor));
    }
}

void A32EmitA64::EmitTerminalImpl(IR::Term::LinkBlock terminal,
                                  IR::LocationDescriptor initial_location, bool is_single_step) {
    EmitSetUpperLocationDescriptor(terminal.next, initial_location);

    if (!config.enable_optimizations || is_single_step) {
        code.MOVI2R(DecodeReg(code.ABI_SCRATCH1), A32::LocationDescriptor{terminal.next}.PC());
        code.STR(INDEX_UNSIGNED, DecodeReg(code.ABI_SCRATCH1), X28, MJitStateReg(A32::Reg::PC));
        code.ReturnFromRunCode();
        return;
    }

    code.CMP(X26, ZR);

    patch_information[terminal.next].jg.emplace_back(code.GetCodePtr());
    if (auto next_bb = GetBasicBlock(terminal.next)) {
        EmitPatchJg(terminal.next, next_bb->entrypoint);
    } else {
        EmitPatchJg(terminal.next);
    }
    FixupBranch dest = code.B();

    code.SwitchToFarCode();
    code.AlignCode16();
    code.SetJumpTarget(dest);
    code.MOVI2R(DecodeReg(code.ABI_SCRATCH1), A32::LocationDescriptor{terminal.next}.PC());
    code.STR(INDEX_UNSIGNED, DecodeReg(code.ABI_SCRATCH1), X28, MJitStateReg(A32::Reg::PC));
    PushRSBHelper(X1, X2, terminal.next);
    code.ForceReturnFromRunCode();

    // Todo: find a better/generic place to FlushIcache when switching between
    //       far code and near code
    code.FlushIcache();
    code.SwitchToNearCode();
}

void A32EmitA64::EmitTerminalImpl(IR::Term::LinkBlockFast terminal,
                                  IR::LocationDescriptor initial_location, bool is_single_step) {
    EmitSetUpperLocationDescriptor(terminal.next, initial_location);

    if (!config.enable_optimizations || is_single_step) {
        code.MOVI2R(DecodeReg(code.ABI_SCRATCH1), A32::LocationDescriptor{terminal.next}.PC());
        code.STR(INDEX_UNSIGNED, DecodeReg(code.ABI_SCRATCH1), X28, MJitStateReg(A32::Reg::PC));
        code.ReturnFromRunCode();
        return;
    }

    patch_information[terminal.next].jmp.emplace_back(code.GetCodePtr());
    if (auto next_bb = GetBasicBlock(terminal.next)) {
        EmitPatchJmp(terminal.next, next_bb->entrypoint);
    } else {
        EmitPatchJmp(terminal.next);
    }
}

void A32EmitA64::EmitTerminalImpl(IR::Term::PopRSBHint, IR::LocationDescriptor,
                                  bool is_single_step) {
    if (!config.enable_optimizations || is_single_step) {
        code.ReturnFromRunCode();
        return;
    }
    code.B(terminal_handler_pop_rsb_hint);
}

void A32EmitA64::EmitTerminalImpl(IR::Term::FastDispatchHint, IR::LocationDescriptor,
                                  bool is_single_step) {
    if (config.enable_fast_dispatch && !is_single_step) {
        code.B(terminal_handler_fast_dispatch_hint);
    } else {
        code.ReturnFromRunCode();
    }
}

void A32EmitA64::EmitTerminalImpl(IR::Term::If terminal, IR::LocationDescriptor initial_location,
                                  bool is_single_step) {
    FixupBranch pass = EmitCond(terminal.if_);
    EmitTerminal(terminal.else_, initial_location, is_single_step);
    code.SetJumpTarget(pass);
    EmitTerminal(terminal.then_, initial_location, is_single_step);
}

void A32EmitA64::EmitTerminalImpl(IR::Term::CheckBit terminal,
                                  IR::LocationDescriptor initial_location, bool is_single_step) {
    FixupBranch fail;
    code.LDRB(INDEX_UNSIGNED, DecodeReg(code.ABI_SCRATCH1), X28, offsetof(A32JitState, check_bit));
    fail = code.CBZ(DecodeReg(code.ABI_SCRATCH1));
    EmitTerminal(terminal.then_, initial_location, is_single_step);
    code.SetJumpTarget(fail);
    EmitTerminal(terminal.else_, initial_location, is_single_step);
}

void A32EmitA64::EmitTerminalImpl(IR::Term::CheckHalt terminal,
                                  IR::LocationDescriptor initial_location, bool is_single_step) {
    code.LDRB(INDEX_UNSIGNED, DecodeReg(code.ABI_SCRATCH1), X28,
              offsetof(A32JitState, halt_requested));
    // Conditional branch only gives +/- 1MB of branch distance
    FixupBranch zero = code.CBZ(DecodeReg(code.ABI_SCRATCH1));
    code.B(code.GetForceReturnFromRunCodeAddress());
    code.SetJumpTarget(zero);
    EmitTerminal(terminal.else_, initial_location, is_single_step);
}

void A32EmitA64::EmitPatchJg(const IR::LocationDescriptor& target_desc, CodePtr target_code_ptr) {
    const CodePtr patch_location = code.GetCodePtr();

    auto long_branch_gt = [this](CodePtr ptr) {
        const s64 distance = reinterpret_cast<s64>(ptr) - reinterpret_cast<s64>(code.GetCodePtr());

        if ((distance >> 2) >= -0x40000 && (distance >> 2) <= 0x3FFFF) {
            code.B(CC_GT, ptr);
            return;
        }

        FixupBranch cc_le = code.B(CC_LE);
        code.B(ptr);
        code.SetJumpTarget(cc_le);
    };

    if (target_code_ptr) {
        long_branch_gt(target_code_ptr);
    } else {
        code.MOVI2R(DecodeReg(code.ABI_SCRATCH1), A32::LocationDescriptor{target_desc}.PC());
        code.STR(INDEX_UNSIGNED, DecodeReg(code.ABI_SCRATCH1), X28, MJitStateReg(A32::Reg::PC));
        long_branch_gt(code.GetReturnFromRunCodeAddress());
    }
    code.EnsurePatchLocationSize(patch_location, 24);
}

void A32EmitA64::EmitPatchJmp(const IR::LocationDescriptor& target_desc, CodePtr target_code_ptr) {
    const CodePtr patch_location = code.GetCodePtr();
    if (target_code_ptr) {
        code.B(target_code_ptr);
    } else {
        code.MOVI2R(DecodeReg(code.ABI_SCRATCH1), A32::LocationDescriptor{target_desc}.PC());
        code.STR(INDEX_UNSIGNED, DecodeReg(code.ABI_SCRATCH1), X28, MJitStateReg(A32::Reg::PC));
        code.B(code.GetReturnFromRunCodeAddress());
    }
    code.EnsurePatchLocationSize(patch_location, 20);
}

void A32EmitA64::EmitPatchMovX0(CodePtr target_code_ptr) {
    if (!target_code_ptr) {
        target_code_ptr = code.GetReturnFromRunCodeAddress();
    }
    const CodePtr patch_location = code.GetCodePtr();
    code.MOVP2R(X0, target_code_ptr);
    code.EnsurePatchLocationSize(patch_location, 16);
}

void A32EmitA64::Unpatch(const IR::LocationDescriptor& location) {
    EmitA64::Unpatch(location);
    if (config.enable_fast_dispatch) {
        code.DisableWriting();
        SCOPE_EXIT {
            code.EnableWriting();
        };

        (*fast_dispatch_table_lookup)(location.Value()) = {};
    }
}

} // namespace Dynarmic::BackendA64
