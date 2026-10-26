// Phase 2 — per-function relocation tests.
//
// Every function in arm64_reloc.cpp exercised individually:
//   ClassifyInsn    — canonical + boundary instruction words
//   EncodeBranch    — range/sign/alignment
//   EmitRelocated   — all 9 kinds across cap boundaries

#include <jni.h>
#include <android/log.h>
#include <stdint.h>
#include <stdio.h>
#include <string>
#include "arm64_reloc.h"
#include "TestReport.h"

namespace {

    constexpr uint32_t kStpX29X30 = 0xA9BF7BFDu;
    constexpr uint32_t kLdpX29X30 = 0xA8C17BFDu;
    constexpr uint32_t kStpX16X17 = 0xA9BF47F0u;
    constexpr uint32_t kLdpX16X17 = 0xA8C147F0u;

    int64_t decode_adr_offset(uint32_t insn) {
        uint32_t immlo = (insn >> 29) & 3;
        uint32_t immhi = (insn >> 5) & 0x7FFFF;
        uint32_t imm21 = (immhi << 2) | immlo;
        if (imm21 & (1u << 20)) imm21 |= 0xFFE00000u;
        return (int64_t)(int32_t)imm21;
    }

    int64_t decode_branch_offset(uint32_t insn) {
        uint32_t imm26 = insn & 0x03FFFFFF;
        if (imm26 & (1u << 25)) imm26 |= 0xFC000000;
        return ((int64_t)(int32_t)imm26) << 2;
    }

    int64_t decode_bcond_offset(uint32_t insn) {
        uint32_t imm19 = (insn >> 5) & 0x7FFFF;
        if (imm19 & (1u << 18)) imm19 |= 0xFFF80000;
        return ((int64_t)(int32_t)imm19) << 2;
    }

    int64_t decode_tbz_offset(uint32_t insn) {
        uint32_t imm14 = (insn >> 5) & 0x3FFF;
        if (imm14 & (1u << 13)) imm14 |= 0xFFFFC000;
        return ((int64_t)(int32_t)imm14) << 2;
    }

    uint32_t make_adr(int rd, int64_t offset) {
        uint32_t imm21 = (uint32_t)(offset & 0x1FFFFF);
        uint32_t immlo = imm21 & 3;
        uint32_t immhi = (imm21 >> 2) & 0x7FFFF;
        return 0x10000000u | (immlo << 29) | (immhi << 5) | (uint32_t)rd;
    }

    // Precompute the expected "B <target>" from `from` to `to` for the
    // two-insn conditional relocation tests. Returns the raw insn word.
    uint32_t enc_b(uint64_t from, uint64_t to) {
        int64_t off = (int64_t)(to - from);
        int64_t imm = off >> 2;
        return 0x14000000u | (uint32_t)(imm & 0x03FFFFFFu);
    }

} // namespace

static int run_classify_tests() {
    int pass = 0;
    auto check = [&](const char* name, uint32_t insn, InsnKind want) {
        InsnInfo info = ClassifyInsn(insn);
        bool ok = (info.kind == want) && (info.raw == insn);
        char buf[96];
        snprintf(buf, sizeof buf, "insn=0x%08x got=%d want=%d raw_ok=%d",
                 insn, (int)info.kind, (int)want, info.raw == insn ? 1 : 0);
        testreport::Report(name, ok, buf);
        if (ok) pass++;
    };

    check("reloc_classify_nop",       0xD503201Fu, InsnKind::NORMAL);
    check("reloc_classify_add",       0x8B010000u, InsnKind::NORMAL);
    check("reloc_classify_mov",       0xAA0003E0u, InsnKind::NORMAL);
    check("reloc_classify_ret",       0xD65F03C0u, InsnKind::NORMAL);
    check("reloc_classify_b_pos",     0x14000000u, InsnKind::B);
    check("reloc_classify_b_neg",     0x17FFFFFFu, InsnKind::B);
    check("reloc_classify_bl_pos",    0x94000000u, InsnKind::BL);
    check("reloc_classify_bl_neg",    0x97FFFFFFu, InsnKind::BL);
    check("reloc_classify_bcond_eq",  0x54000000u, InsnKind::B_COND);
    check("reloc_classify_bcond_ne",  0x54000040u, InsnKind::B_COND);
    check("reloc_classify_cbz_w",     0x34000000u, InsnKind::CBZ_CBNZ);
    check("reloc_classify_cbnz_x",    0xB5000000u, InsnKind::CBZ_CBNZ);
    check("reloc_classify_tbz",       0x36000000u, InsnKind::TBZ_TBNZ);
    check("reloc_classify_tbnz",      0x37000000u, InsnKind::TBZ_TBNZ);
    check("reloc_classify_adr",       0x10000000u, InsnKind::ADR);
    check("reloc_classify_adrp",      0x90000000u, InsnKind::ADRP);
    check("reloc_classify_ldr_w",     0x18000040u, InsnKind::LDR_LITERAL);
    check("reloc_classify_ldr_x",     0x58000000u, InsnKind::LDR_LITERAL);
    check("reloc_classify_ldrsw",     0x98000000u, InsnKind::LDR_LITERAL);

    InsnInfo round = ClassifyInsn(0xD503201Fu);
    testreport::Report("reloc_classify_raw_roundtrip",
                       round.raw == 0xD503201Fu, "NOP raw preserved");
    if (round.raw == 0xD503201Fu) pass++;
    return pass;
}

static int run_encode_branch_tests() {
    int pass = 0;
    auto report = [&](const char* name, bool ok, const std::string& d) {
        testreport::Report(name, ok, d);
        if (ok) pass++;
    };

    {
        uint32_t out = 0;
        bool ok = EncodeBranch(&out, 0x1000, 0x1004, false);
        char d[64]; snprintf(d, sizeof d, "insn=0x%08x", out);
        report("reloc_encode_branch_fwd", ok && out == 0x14000001u, d);
    }
    {
        uint32_t out = 0;
        bool ok = EncodeBranch(&out, 0x1000, 0x0FFC, false);
        char d[64]; snprintf(d, sizeof d, "insn=0x%08x", out);
        report("reloc_encode_branch_bwd", ok && out == 0x17FFFFFFu, d);
    }
    {
        uint32_t out = 0;
        bool ok = EncodeBranch(&out, 0x1000, 0x1000, false);
        report("reloc_encode_branch_self", ok && out == 0x14000000u, "B +0");
    }
    {
        uint32_t out_b = 0, out_bl = 0;
        EncodeBranch(&out_b,  0x1000, 0x2000, false);
        EncodeBranch(&out_bl, 0x1000, 0x2000, true);
        report("reloc_encode_branch_link",
               (out_b & 0xFC000000u) == 0x14000000u &&
               (out_bl & 0xFC000000u) == 0x94000000u,
               "B/BL opcode distinct");
    }
    {
        uint32_t out = 0;
        bool ok = EncodeBranch(&out, 0x1000, 0x1002, false);
        report("reloc_encode_branch_unaligned", !ok, "0x1002 rejected");
    }
    {
        uint32_t out = 0;
        bool ok_plus  = EncodeBranch(&out, 0, (1LL << 27) - 4, false);
        bool ok_minus = EncodeBranch(&out, (1LL << 27), 0, false);
        report("reloc_encode_branch_edge_in", ok_plus && ok_minus,
               "max forward / max backward accepted");
    }
    {
        uint32_t out = 0;
        bool bad_plus  = EncodeBranch(&out, 0, (1LL << 27) + 4, false);
        bool bad_minus = EncodeBranch(&out, (1LL << 27) + 4, 0, false);
        report("reloc_encode_branch_edge_out", !bad_plus && !bad_minus,
               "+/-128MB+4 rejected");
    }
    {
        uint32_t out = 0;
        EncodeBranch(&out, 0x10000, 0x8000, false);
        int64_t decoded = decode_branch_offset(out);
        report("reloc_encode_branch_roundtrip",
               decoded == (int64_t)(0x8000 - 0x10000),
               "decode matches -0x8000");
    }
    return pass;
}

static int run_emit_tests() {
    int pass = 0;
    auto check = [&](const char* name, bool ok, const std::string& d) {
        testreport::Report(name, ok, d);
        if (ok) pass++;
    };

    // ── NORMAL ──────────────────────────────────────────────────────
    {
        uint32_t out[4] = {0};
        int n = EmitRelocated(0xD503201Fu, 0x1000, 0x2000, out, 4);
        char d[64]; snprintf(d, sizeof d, "n=%d out0=0x%08x", n, out[0]);
        check("reloc_emit_normal", n == 1 && out[0] == 0xD503201Fu, d);
    }
    {
        uint32_t out[1] = {0};
        int n = EmitRelocated(0xD503201Fu, 0x1000, 0x2000, out, 0);
        check("reloc_emit_normal_cap0", n == 0, "cap=0 rejected");
    }

    // ── B ───────────────────────────────────────────────────────────
    {
        uint32_t out[4] = {0};
        int n = EmitRelocated(0x14000004u, 0x1000, 0x2000, out, 4);
        int64_t off = decode_branch_offset(out[0]);
        char d[80]; snprintf(d, sizeof d, "n=%d off=%lld", n, (long long)off);
        check("reloc_emit_b", n == 1 && off == -0xFF0, d);
    }

    // ── BL ──────────────────────────────────────────────────────────
    {
        uint32_t out[4] = {0};
        int n = EmitRelocated(0x94000004u, 0x1000, 0x2000, out, 4);
        char d[96];
        snprintf(d, sizeof d, "n=%d [0]=0x%08x [1]=0x%08x [2]=0x%08x",
                 n, out[0], out[1], out[2]);
        check("reloc_emit_bl",
              n == 3 && out[0] == kStpX29X30 && out[2] == kLdpX29X30, d);
    }
    {
        uint32_t out[2] = {0};
        int n = EmitRelocated(0x94000004u, 0x1000, 0x2000, out, 2);
        check("reloc_emit_bl_cap2", n == 0, "cap=2 rejected");
    }

    // ── ADRP ────────────────────────────────────────────────────────
    {
        uint32_t out[4] = {0};
        uint32_t adrp = 0xB0000000u;
        int n = EmitRelocated(adrp, 0x10000, 0x20000, out, 4);
        int64_t off = decode_adr_offset(out[0]) << 12;
        char d[64]; snprintf(d, sizeof d, "n=%d page_off=0x%llx",
                             n, (unsigned long long)off);
        check("reloc_emit_adrp", n == 1 && off == -0xF000, d);
    }

    // ── ADR (short and long) ────────────────────────────────────────
    {
        uint32_t out[4] = {0};
        uint32_t adr = make_adr(1, 8);
        int n = EmitRelocated(adr, 0x1000, 0x1000, out, 4);
        int64_t off = decode_adr_offset(out[0]);
        char d[64]; snprintf(d, sizeof d, "n=%d off=%lld", n, (long long)off);
        check("reloc_emit_adr_short", n == 1 && off == 8, d);
    }
    {
        uint32_t out[4] = {0};
        uint32_t adr = make_adr(1, 0x80000);
        int n = EmitRelocated(adr, 0x0, 0x400000, out, 4);
        char d[80]; snprintf(d, sizeof d, "n=%d [0]=0x%08x [1]=0x%08x",
                             n, out[0], out[1]);
        bool op_adrp = (out[0] & 0x9F000000u) == 0x90000000u;
        bool op_add  = (out[1] & 0xFFC00000u) == 0x91000000u;
        check("reloc_emit_adr_long", n == 2 && op_adrp && op_add, d);
    }

    // ── B.cond — inverted skip + B target ───────────────────────────
    // Original: 0x54000000 = b.eq, imm19=0 (target = orig_pc = 0x1000).
    // Relocate from new_pc=0x2000.
    // out[0] = b.ne skip +8 = 0x54000041
    // out[1] = B from new_pc+4 (0x2004) to 0x1000 = 0x17FFFBFF
    {
        uint32_t out[4] = {0};
        int n = EmitRelocated(0x54000000u, 0x1000, 0x2000, out, 4);
        bool n_ok = (n == 2);
        bool skip_ok = (out[0] == 0x54000041u);
        bool b_ok    = (out[1] == 0x17FFFBFFu);
        bool decoded_ok = (decode_branch_offset(out[1]) == (int64_t)0x1000 - (int64_t)0x2004);
        char d[128];
        snprintf(d, sizeof d, "n=%d [0]=0x%08x skip=%d [1]=0x%08x b=%d dec=%d",
                 n, out[0], skip_ok ? 1 : 0, out[1], b_ok ? 1 : 0, decoded_ok ? 1 : 0);
        check("reloc_emit_bcond_two_insns", n_ok && skip_ok && b_ok && decoded_ok, d);
    }
    // Original = b.ne +0 (0x54000001). Inverted = b.eq skip +8.
    {
        uint32_t out[4] = {0};
        int n = EmitRelocated(0x54000001u, 0x1000, 0x2000, out, 4);
        bool n_ok = (n == 2);
        bool skip_ok = (out[0] == 0x54000040u);   // b.eq skip 8
        bool b_ok    = (out[1] == 0x17FFFBFFu);   // B 0x2004 -> 0x1000
        char d[128];
        snprintf(d, sizeof d, "n=%d [0]=0x%08x [1]=0x%08x",
                 n, out[0], out[1]);
        check("reloc_emit_bcond_ne_inverts_to_eq", n_ok && skip_ok && b_ok, d);
    }
    {
        uint32_t out[1] = {0};
        int n = EmitRelocated(0x54000000u, 0x1000, 0x2000, out, 1);
        check("reloc_emit_bcond_cap1", n == 0, "cap=1 rejected");
    }

    // ── CBZ / CBNZ ──────────────────────────────────────────────────
    // Original: 0x34000000 = cbz w0, imm19=0 (target = 0x1000).
    // out[0] = cbnz w0 skip +8 = 0x35000040
    {
        uint32_t out[4] = {0};
        int n = EmitRelocated(0x34000000u, 0x1000, 0x2000, out, 4);
        bool n_ok = (n == 2);
        bool skip_ok = (out[0] == 0x35000040u);
        bool b_ok = (out[1] == 0x17FFFBFFu);
        char d[128];
        snprintf(d, sizeof d, "n=%d [0]=0x%08x skip=%d [1]=0x%08x b=%d",
                 n, out[0], skip_ok ? 1 : 0, out[1], b_ok ? 1 : 0);
        check("reloc_emit_cbz_two_insns", n_ok && skip_ok && b_ok, d);
    }
    // CBNZ X0 (0xB5000000) inverts to CBZ X0
    {
        uint32_t out[4] = {0};
        int n = EmitRelocated(0xB5000000u, 0x1000, 0x2000, out, 4);
        bool skip_ok = (out[0] == 0xB4000040u);
        char d[64]; snprintf(d, sizeof d, "n=%d [0]=0x%08x", n, out[0]);
        check("reloc_emit_cbnz_inverts_to_cbz", n == 2 && skip_ok, d);
    }

    // ── TBZ / TBNZ ──────────────────────────────────────────────────
    // Original: 0x36000000 = tbz w0, #0, imm14=0 (target = 0x1000).
    // out[0] = tbnz w0, #0, skip +8 = 0x37000040
    {
        uint32_t out[4] = {0};
        int n = EmitRelocated(0x36000000u, 0x1000, 0x2000, out, 4);
        bool n_ok = (n == 2);
        bool skip_ok = (out[0] == 0x37000040u);
        bool b_ok = (out[1] == 0x17FFFBFFu);
        char d[128];
        snprintf(d, sizeof d, "n=%d [0]=0x%08x skip=%d [1]=0x%08x b=%d",
                 n, out[0], skip_ok ? 1 : 0, out[1], b_ok ? 1 : 0);
        check("reloc_emit_tbz_two_insns", n_ok && skip_ok && b_ok, d);
    }
    // TBNZ inverts to TBZ
    {
        uint32_t out[4] = {0};
        int n = EmitRelocated(0x37000000u, 0x1000, 0x2000, out, 4);
        bool skip_ok = (out[0] == 0x36000040u);
        char d[64]; snprintf(d, sizeof d, "n=%d [0]=0x%08x", n, out[0]);
        check("reloc_emit_tbnz_inverts_to_tbz", n == 2 && skip_ok, d);
    }

    // ── LDR literal ─────────────────────────────────────────────────
    // 0x18000000 = ldr w0, imm19=0, target = orig_pc = 0x1000.
    // Expected:
    //   out[0] = stp x16, x17, [sp, #-16]! = 0xA9BF47F0
    //   out[1] = adrp x16, page(0x1000) from PC 0x2004 = 0xF0FFFFF0
    //   out[2] = ldr w0, [x16, #0] = 0xB9400200
    //   out[3] = ldp x16, x17, [sp], #16 = 0xA8C147F0
    {
        uint32_t out[4] = {0};
        int n = EmitRelocated(0x18000000u, 0x1000, 0x2000, out, 4);
        char d[160];
        snprintf(d, sizeof d, "n=%d [0]=0x%08x [1]=0x%08x [2]=0x%08x [3]=0x%08x",
                 n, out[0], out[1], out[2], out[3]);
        bool ok = (n == 4)
                  && (out[0] == kStpX16X17)
                  && (out[1] == 0xF0FFFFF0u)
                  && (out[2] == 0xB9400200u)
                  && (out[3] == kLdpX16X17);
        check("reloc_emit_ldr_four_insns", ok, d);
    }
    // LDR X0 variant -> uses 64-bit LDR form (0xF9400000 base)
    {
        uint32_t out[4] = {0};
        int n = EmitRelocated(0x58000000u, 0x1000, 0x2000, out, 4);
        bool ldr_ok = (out[2] == 0xF9400200u);
        char d[80]; snprintf(d, sizeof d, "n=%d [2]=0x%08x", n, out[2]);
        check("reloc_emit_ldr_x_variant", n == 4 && ldr_ok, d);
    }
    // LDRSW variant
    {
        uint32_t out[4] = {0};
        int n = EmitRelocated(0x98000000u, 0x1000, 0x2000, out, 4);
        bool ldr_ok = (out[2] == 0xB9800200u);
        char d[80]; snprintf(d, sizeof d, "n=%d [2]=0x%08x", n, out[2]);
        check("reloc_emit_ldrsw_variant", n == 4 && ldr_ok, d);
    }
    // LDR W16 rejected (would clobber our scratch)
    {
        uint32_t out[4] = {0};
        int n = EmitRelocated(0x18000010u, 0x1000, 0x2000, out, 4);
        check("reloc_emit_ldr_rt16_rejected", n == 0, "LDR into x16 rejected");
    }
    // LDR W17 rejected
    {
        uint32_t out[4] = {0};
        int n = EmitRelocated(0x18000011u, 0x1000, 0x2000, out, 4);
        check("reloc_emit_ldr_rt17_rejected", n == 0, "LDR into x17 rejected");
    }
    // LDR W0 with cap < 4 rejected
    {
        uint32_t out[3] = {0};
        int n = EmitRelocated(0x18000000u, 0x1000, 0x2000, out, 3);
        check("reloc_emit_ldr_cap3", n == 0, "cap=3 rejected");
    }

    return pass;
}

extern "C" int RunRelocTests() {
    int before = (int)testreport::Snapshot().size();
    run_classify_tests();
    run_encode_branch_tests();
    run_emit_tests();
    return (int)testreport::Snapshot().size() - before;
}