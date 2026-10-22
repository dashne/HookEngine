#include "arm64_reloc.h"

static inline int64_t sext(uint64_t v, int bits) {
    uint64_t m = 1ULL << (bits - 1);
    return (int64_t)((v ^ m) - m);
}

InsnInfo ClassifyInsn(uint32_t insn) {
    if ((insn & 0xFC000000u) == 0x14000000u) return {InsnKind::B, insn};
    if ((insn & 0xFC000000u) == 0x94000000u) return {InsnKind::BL, insn};
    if ((insn & 0xFF000010u) == 0x54000000u) return {InsnKind::B_COND, insn};
    if ((insn & 0x7E000000u) == 0x34000000u) return {InsnKind::CBZ_CBNZ, insn};
    if ((insn & 0x7E000000u) == 0x36000000u) return {InsnKind::TBZ_TBNZ, insn};
    if ((insn & 0x9F000000u) == 0x10000000u) return {InsnKind::ADR, insn};
    if ((insn & 0x9F000000u) == 0x90000000u) return {InsnKind::ADRP, insn};
    uint32_t m = insn & 0x3B000000u;
    if (m == 0x18000000u || m == 0x58000000u || m == 0x98000000u)
        return {InsnKind::LDR_LITERAL, insn};
    return {InsnKind::NORMAL, insn};
}

bool EncodeBranch(uint32_t* out, uint64_t from, uint64_t to, bool link) {
    int64_t off = (int64_t)(to - from);
    if ((off & 3) != 0) return false;
    int64_t imm = off >> 2;
    if (imm < -(1LL << 25) || imm >= (1LL << 25)) return false;
    *out = (link ? 0x94000000u : 0x14000000u) | (uint32_t)(imm & 0x03FFFFFFu);
    return true;
}

static bool enc_adrp(uint32_t* out, uint64_t from, uint64_t page, int rd) {
    int64_t d = (int64_t)(page - (from & ~0xFFFULL));
    if ((d & 0xFFF) != 0) return false;
    int64_t imm = d >> 12;
    if (imm < -(1LL << 20) || imm >= (1LL << 20)) return false;
    *out = 0x90000000u
           | ((uint32_t)(imm & 3) << 29)
           | ((uint32_t)((imm >> 2) & 0x7FFFF) << 5)
           | (uint32_t)rd;
    return true;
}

static bool enc_adr(uint32_t* out, uint64_t from, uint64_t to, int rd) {
    int64_t off = (int64_t)(to - from);
    if (off < -(1LL << 20) || off >= (1LL << 20)) return false;
    *out = 0x10000000u
           | ((uint32_t)(off & 3) << 29)
           | ((uint32_t)((off >> 2) & 0x7FFFF) << 5)
           | (uint32_t)rd;
    return true;
}

static bool enc_adrp_add(uint32_t* out, uint64_t from, uint64_t addr, int rd) {
    if (!enc_adrp(&out[0], from, addr & ~0xFFFULL, rd)) return false;
    out[1] = 0x91000000u
             | ((uint32_t)(addr & 0xFFF) << 10)
             | ((uint32_t)rd << 5)
             | (uint32_t)rd;
    return true;
}

int EmitRelocated(uint32_t insn, uint64_t orig_pc, uint64_t new_pc,
                  uint32_t* out, size_t cap) {
    InsnInfo info = ClassifyInsn(insn);
    switch (info.kind) {
        case InsnKind::NORMAL:
            if (cap < 1) return 0;
            out[0] = insn;
            return 1;

        case InsnKind::B: {
            if (cap < 1) return 0;
            int64_t off = sext(insn & 0x03FFFFFFu, 26) << 2;
            uint64_t dest = orig_pc + (uint64_t)off;
            return EncodeBranch(&out[0], new_pc, dest, false) ? 1 : 0;
        }

        case InsnKind::BL: {
            // BL clobbers LR. Standard frame save/restore around the call:
            //   0: stp x29, x30, [sp, #-16]!   save fp+lr, decrement sp
            //   1: BL helper                    PC = new_pc + 4
            //   2: ldp x29, x30, [sp], #16      restore fp+lr, increment sp
            // Caller appends `b target+4` after.
            if (cap < 3) return 0;
            int64_t off = sext(insn & 0x03FFFFFFu, 26) << 2;
            uint64_t dest = orig_pc + (uint64_t)off;
            uint32_t bl_insn;
            if (!EncodeBranch(&bl_insn, new_pc + 4, dest, true)) return 0;
            out[0] = 0xA9BF7BFDu;   // stp x29, x30, [sp, #-16]!
            out[1] = bl_insn;
            out[2] = 0xA8C17BFDu;   // ldp x29, x30, [sp], #16
            return 3;
        }

        case InsnKind::ADRP: {
            if (cap < 1) return 0;
            uint32_t lo = (insn >> 29) & 3;
            uint32_t hi = (insn >> 5) & 0x7FFFFu;
            int64_t imm = sext(((int64_t)hi << 2) | lo, 21);
            uint64_t page = (orig_pc & ~0xFFFULL) + (uint64_t)(imm << 12);
            return enc_adrp(&out[0], new_pc, page, insn & 0x1F) ? 1 : 0;
        }

        case InsnKind::ADR: {
            if (cap < 2) return 0;
            uint32_t lo = (insn >> 29) & 3;
            uint32_t hi = (insn >> 5) & 0x7FFFFu;
            int64_t imm = sext(((int64_t)hi << 2) | lo, 21);
            uint64_t target = orig_pc + (uint64_t)imm;
            int rd = insn & 0x1F;
            if (enc_adr(&out[0], new_pc, target, rd)) return 1;
            if (enc_adrp_add(out, new_pc, target, rd)) return 2;
            return 0;
        }

        case InsnKind::B_COND:
        case InsnKind::CBZ_CBNZ:
        case InsnKind::TBZ_TBNZ:
        case InsnKind::LDR_LITERAL:
            return 0;
    }
    return 0;
}