// Copyright (c) 2015-2016, tandasat. All rights reserved.
// Use of this source code is governed by a MIT-style license that can be
// found in the LICENSE file.

/// @file
/// Implements mem_trace functions.

#include "mem_trace.h"
#include <intrin.h>
#include "../HyperPlatform/HyperPlatform/common.h"
#include "../HyperPlatform/HyperPlatform/log.h"
#include "../HyperPlatform/HyperPlatform/util.h"
#include <capstone.h>

extern "C" {
////////////////////////////////////////////////////////////////////////////////
//
// macro utilities
//

////////////////////////////////////////////////////////////////////////////////
//
// constants and macros
//

////////////////////////////////////////////////////////////////////////////////
//
// types
//

////////////////////////////////////////////////////////////////////////////////
//
// prototypes
//

_Success_(return ) static bool RwepGetMmIoValue(_In_ void* address,
                                                _In_ GpRegisters* gp_regs,
                                                _In_ bool is_write,
                                                _Out_ ULONG64* value,
                                                _Out_ SIZE_T* size);

_Success_(return ) static bool RwepGetValueFromRegister(
    _Inout_ GpRegisters* gp_regs, _In_ x86_reg reg, _Out_ ULONG64* value);

////////////////////////////////////////////////////////////////////////////////
//
// variables
//

////////////////////////////////////////////////////////////////////////////////
//
// implementations
//

/*_Use_decl_annotations_*/ bool MemTraceIsEnabled() {
#if defined(MEMORYMON_ENABLE_MMIO_TRACE)
  return true;
#else
  return false;
#endif
}

_Use_decl_annotations_ bool MemTraceIsTargetAddress(ULONG64 pa) {
  if (!MemTraceIsEnabled()) {
    return false;
  }

  //if (UtilIsInBounds(pa, 0xfd5fa000ull, 0xfd5fafffull)) {
  //  return true;
  //}
  if (UtilIsInBounds(pa, 0xb0700000ull, 0xb0701fffull)) {
    return true;
  }
  return false;
}

_Use_decl_annotations_ bool MemTraceHandleReadWrite(void* guest_ip,
                                                    GpRegisters* gp_regs,
                                                    bool is_write) {
  if (!MemTraceIsEnabled()) {
    return false;
  }

  ULONG64 value = 0;
  SIZE_T size = 0;
  if (RwepGetMmIoValue(guest_ip, gp_regs, is_write, &value, &size)) {
    // clang-format off
    switch (size) {
      case 1: HYPERPLATFORM_LOG_INFO_SAFE("Value= %02x", value & UINT8_MAX); break;
      case 2: HYPERPLATFORM_LOG_INFO_SAFE("Value= %04x", value & UINT16_MAX); break;
      case 4: HYPERPLATFORM_LOG_INFO_SAFE("Value= %08x", value & UINT32_MAX); break;
      case 8: HYPERPLATFORM_LOG_INFO_SAFE("Value= %016llx", value); break;
      default: HYPERPLATFORM_COMMON_DBG_BREAK(); break;
    }
    // clang-format on
  } else {
    // Failed to get value. Most likely due to unsupported instruction
    HYPERPLATFORM_COMMON_DBG_BREAK();
    return false;
  }

  return true;
}

_Use_decl_annotations_ static bool RwepGetMmIoValue(void* address,
                                                    GpRegisters* gp_regs,
                                                    bool is_write,
                                                    ULONG64* value,
                                                    SIZE_T* size) {
  bool result = false;

  // NT_ASSERT(KeGetCurrentIrql() <= DISPATCH_LEVEL);

  csh handle = {};
  if (cs_open(CS_ARCH_X86, (sizeof(void*) == 4) ? CS_MODE_32 : CS_MODE_64,
              &handle) != CS_ERR_OK) {
    return result;
  }

  if (cs_option(handle, CS_OPT_DETAIL, CS_OPT_ON) != CS_ERR_OK) {
    cs_close(&handle);
    return result;
  }

  const auto current_cr3 = __readcr3();
  const auto guest_cr3 = UtilVmRead(VmcsField::kGuestCr3);
  __writecr3(guest_cr3);

  cs_insn* instructions = {};
  auto count = cs_disasm(handle, (uint8_t*)address, 15, (uint64_t)address, 0,
                         &instructions);

  __writecr3(current_cr3);
  if (count == 0) {
    cs_close(&handle);
    return result;
  }

  const auto& inst = instructions[0];
  HYPERPLATFORM_LOG_DEBUG_SAFE("%s %s", inst.mnemonic, inst.op_str);

  if (is_write) {
    switch (inst.id) {
      case X86_INS_MOV:
      case X86_INS_STOSB:
      case X86_INS_STOSD:
      case X86_INS_STOSQ:
      case X86_INS_STOSW:
        break;
      default:
        goto exit;
    }

    if (inst.detail->x86.op_count != 2) {
      goto exit;
    }

    const auto& second_operand = inst.detail->x86.operands[1];
    if (second_operand.type == X86_OP_REG &&
        RwepGetValueFromRegister(gp_regs, second_operand.reg, value)) {
      *size = second_operand.size;
    } else if (second_operand.type == X86_OP_IMM) {
      *value = second_operand.imm;
      *size = second_operand.size;
    } else {
      goto exit;
    }
  } else {
    switch (inst.id) {
      case X86_INS_MOV:
        break;
      default:
        goto exit;
    }

    if (inst.detail->x86.op_count != 2) {
      goto exit;
    }

    const auto& first_operand = inst.detail->x86.operands[0];
    if (first_operand.type == X86_OP_REG &&
        RwepGetValueFromRegister(gp_regs, first_operand.reg, value)) {
      *size = first_operand.size;
    } else {
      goto exit;
    }
  }

  result = true;

exit:;
  cs_free(instructions, count);
  cs_close(&handle);
  return result;
}

_Use_decl_annotations_ static bool RwepGetValueFromRegister(
    GpRegisters* gp_regs, x86_reg reg, ULONG64* value) {
  // Covers registers that are likely to be used. It is not comprehensive and
  // does not includes all possible registers.
  ULONG64 val = 0;
  switch (reg) {
    // clang-format off
    case X86_REG_AL:  val = gp_regs->ax & UINT8_MAX; break;
    case X86_REG_AH:  val = (gp_regs->ax >> 8) & UINT8_MAX; break;
    case X86_REG_AX:  val = gp_regs->ax & UINT16_MAX; break;
    case X86_REG_EAX: val = gp_regs->ax & UINT32_MAX; break;
    case X86_REG_RAX: val = gp_regs->ax; break;

    case X86_REG_BL:  val = gp_regs->bx & UINT8_MAX; break;
    case X86_REG_BH:  val = (gp_regs->bx >> 8) & UINT8_MAX; break;
    case X86_REG_BX:  val = gp_regs->bx & UINT16_MAX; break;
    case X86_REG_EBX: val = gp_regs->bx & UINT32_MAX; break;
    case X86_REG_RBX: val = gp_regs->bx; break;

    case X86_REG_CL:  val = gp_regs->cx & UINT8_MAX; break;
    case X86_REG_CH:  val = (gp_regs->cx >> 8) & UINT8_MAX; break;
    case X86_REG_CX:  val = gp_regs->cx & UINT16_MAX; break;
    case X86_REG_ECX: val = gp_regs->cx & UINT32_MAX; break;
    case X86_REG_RCX: val = gp_regs->cx; break;

    case X86_REG_DL:  val = gp_regs->dx & UINT8_MAX; break;
    case X86_REG_DH:  val = (gp_regs->dx >> 8) & UINT8_MAX; break;
    case X86_REG_DX:  val = gp_regs->dx & UINT16_MAX; break;
    case X86_REG_EDX: val = gp_regs->dx & UINT32_MAX; break;
    case X86_REG_RDX: val = gp_regs->dx; break;

    case X86_REG_DIL: val = gp_regs->di & UINT8_MAX; break;
    case X86_REG_DI:  val = gp_regs->di & UINT16_MAX; break;
    case X86_REG_EDI: val = gp_regs->di & UINT32_MAX; break;
    case X86_REG_RDI: val = gp_regs->di; break;

    case X86_REG_SIL: val = gp_regs->si & UINT8_MAX; break;
    case X86_REG_SI:  val = gp_regs->si & UINT16_MAX; break;
    case X86_REG_ESI: val = gp_regs->si & UINT32_MAX; break;
    case X86_REG_RSI: val = gp_regs->si; break;

    case X86_REG_BPL: val = gp_regs->r15 & UINT8_MAX; break;
    case X86_REG_BP:  val = gp_regs->r15 & UINT16_MAX; break;
    case X86_REG_EBP: val = gp_regs->r15 & UINT32_MAX; break;
    case X86_REG_RBP: val = gp_regs->r15; break;

    case X86_REG_R8B: val = gp_regs->r8 & UINT8_MAX; break;
    case X86_REG_R8W: val = gp_regs->r8 & UINT16_MAX; break;
    case X86_REG_R8D: val = gp_regs->r8 & UINT32_MAX; break;
    case X86_REG_R8:  val = gp_regs->r8; break;

    case X86_REG_R9B: val = gp_regs->r9 & UINT8_MAX; break;
    case X86_REG_R9W: val = gp_regs->r9 & UINT16_MAX; break;
    case X86_REG_R9D: val = gp_regs->r9 & UINT32_MAX; break;
    case X86_REG_R9:  val = gp_regs->r9; break;

    case X86_REG_R10B: val = gp_regs->r10 & UINT8_MAX; break;
    case X86_REG_R10W: val = gp_regs->r10 & UINT16_MAX; break;
    case X86_REG_R10D: val = gp_regs->r10 & UINT32_MAX; break;
    case X86_REG_R10:  val = gp_regs->r10; break;

    case X86_REG_R11B: val = gp_regs->r11 & UINT8_MAX; break;
    case X86_REG_R11W: val = gp_regs->r11 & UINT16_MAX; break;
    case X86_REG_R11D: val = gp_regs->r11 & UINT32_MAX; break;
    case X86_REG_R11:  val = gp_regs->r11; break;

    case X86_REG_R12B: val = gp_regs->r12 & UINT8_MAX; break;
    case X86_REG_R12W: val = gp_regs->r12 & UINT16_MAX; break;
    case X86_REG_R12D: val = gp_regs->r12 & UINT32_MAX; break;
    case X86_REG_R12:  val = gp_regs->r12; break;

    case X86_REG_R13B: val = gp_regs->r13 & UINT8_MAX; break;
    case X86_REG_R13W: val = gp_regs->r13 & UINT16_MAX; break;
    case X86_REG_R13D: val = gp_regs->r13 & UINT32_MAX; break;
    case X86_REG_R13:  val = gp_regs->r13; break;

    case X86_REG_R14B: val = gp_regs->r14 & UINT8_MAX; break;
    case X86_REG_R14W: val = gp_regs->r14 & UINT16_MAX; break;
    case X86_REG_R14D: val = gp_regs->r14 & UINT32_MAX; break;
    case X86_REG_R14:  val = gp_regs->r14; break;

    case X86_REG_R15B: val = gp_regs->r15 & UINT8_MAX; break;
    case X86_REG_R15W: val = gp_regs->r15 & UINT16_MAX; break;
    case X86_REG_R15D: val = gp_regs->r15 & UINT32_MAX; break;
    case X86_REG_R15:  val = gp_regs->r15; break;
    // clang-format on

    default:
      return false;
  }

  *value = val;
  return true;
}

}  // extern "C"