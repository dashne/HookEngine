//
// Created by ibrahim on 10/22/2026.
//

#ifndef HOOKENGINE_ARM64_RELOC_H
#define HOOKENGINE_ARM64_RELOC_H

#endif //HOOKENGINE_ARM64_RELOC_H

#pragma once
#include <cstdint>
#include <cstddef>

enum class InsnKind {
    NORMAL, B, BL, B_COND, CBZ_CBNZ, TBZ_TBNZ, ADR, ADRP, LDR_LITERAL,
};

struct InsnInfo { InsnKind kind; uint32_t raw; };

InsnInfo ClassifyInsn(uint32_t insn);

// Relocate `insn` from orig_pc to new_pc.
// Returns number of 32-bit slots written to `out` (0 = cannot relocate).
int EmitRelocated(uint32_t insn, uint64_t orig_pc, uint64_t new_pc,
                  uint32_t* out, size_t out_capacity);

// Encode a direct B/BL. Returns false if out of ±128MB range.
bool EncodeBranch(uint32_t* out, uint64_t from, uint64_t to, bool link);
