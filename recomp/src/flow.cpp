#include "flow.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "macho.h"

namespace recomp {

bool is_jcc(ZydisMnemonic m) {
    switch (m) {
    case ZYDIS_MNEMONIC_JO: case ZYDIS_MNEMONIC_JNO: case ZYDIS_MNEMONIC_JB:
    case ZYDIS_MNEMONIC_JNB: case ZYDIS_MNEMONIC_JZ: case ZYDIS_MNEMONIC_JNZ:
    case ZYDIS_MNEMONIC_JBE: case ZYDIS_MNEMONIC_JNBE: case ZYDIS_MNEMONIC_JS:
    case ZYDIS_MNEMONIC_JNS: case ZYDIS_MNEMONIC_JP: case ZYDIS_MNEMONIC_JNP:
    case ZYDIS_MNEMONIC_JL: case ZYDIS_MNEMONIC_JNL: case ZYDIS_MNEMONIC_JLE:
    case ZYDIS_MNEMONIC_JNLE:
        return true;
    default:
        return false;
    }
}

bool is_loop(ZydisMnemonic m) {
    return m == ZYDIS_MNEMONIC_LOOP || m == ZYDIS_MNEMONIC_LOOPE || m == ZYDIS_MNEMONIC_LOOPNE ||
           m == ZYDIS_MNEMONIC_JECXZ;
}

bool is_x87(ZydisMnemonic m) {
    switch (m) {
    case ZYDIS_MNEMONIC_FLD: case ZYDIS_MNEMONIC_FST: case ZYDIS_MNEMONIC_FSTP:
    case ZYDIS_MNEMONIC_FILD: case ZYDIS_MNEMONIC_FIST: case ZYDIS_MNEMONIC_FISTP:
    case ZYDIS_MNEMONIC_FISTTP: case ZYDIS_MNEMONIC_FLDZ: case ZYDIS_MNEMONIC_FLD1:
    case ZYDIS_MNEMONIC_FADD: case ZYDIS_MNEMONIC_FADDP: case ZYDIS_MNEMONIC_FIADD:
    case ZYDIS_MNEMONIC_FSUB: case ZYDIS_MNEMONIC_FSUBP: case ZYDIS_MNEMONIC_FISUB:
    case ZYDIS_MNEMONIC_FSUBR: case ZYDIS_MNEMONIC_FSUBRP: case ZYDIS_MNEMONIC_FISUBR:
    case ZYDIS_MNEMONIC_FMUL: case ZYDIS_MNEMONIC_FMULP: case ZYDIS_MNEMONIC_FIMUL:
    case ZYDIS_MNEMONIC_FDIV: case ZYDIS_MNEMONIC_FDIVP: case ZYDIS_MNEMONIC_FIDIV:
    case ZYDIS_MNEMONIC_FDIVR: case ZYDIS_MNEMONIC_FDIVRP: case ZYDIS_MNEMONIC_FIDIVR:
    case ZYDIS_MNEMONIC_FXCH: case ZYDIS_MNEMONIC_FCHS: case ZYDIS_MNEMONIC_FABS:
    case ZYDIS_MNEMONIC_FLDCW: case ZYDIS_MNEMONIC_FNSTCW: case ZYDIS_MNEMONIC_FWAIT:
    case ZYDIS_MNEMONIC_FNOP:
    case ZYDIS_MNEMONIC_FCMOVB: case ZYDIS_MNEMONIC_FCMOVBE: case ZYDIS_MNEMONIC_FCMOVE:
    case ZYDIS_MNEMONIC_FCMOVNB: case ZYDIS_MNEMONIC_FCMOVNBE: case ZYDIS_MNEMONIC_FCMOVNE:
    case ZYDIS_MNEMONIC_FCMOVNU: case ZYDIS_MNEMONIC_FCMOVU:
    // Compares, status word, functions and control (MSVC code and its CRT).
    case ZYDIS_MNEMONIC_FCOM: case ZYDIS_MNEMONIC_FCOMP: case ZYDIS_MNEMONIC_FCOMPP:
    case ZYDIS_MNEMONIC_FUCOM: case ZYDIS_MNEMONIC_FUCOMP: case ZYDIS_MNEMONIC_FUCOMPP:
    case ZYDIS_MNEMONIC_FCOMI: case ZYDIS_MNEMONIC_FCOMIP: case ZYDIS_MNEMONIC_FUCOMI:
    case ZYDIS_MNEMONIC_FUCOMIP: case ZYDIS_MNEMONIC_FICOM: case ZYDIS_MNEMONIC_FICOMP:
    case ZYDIS_MNEMONIC_FTST: case ZYDIS_MNEMONIC_FXAM: case ZYDIS_MNEMONIC_FNSTSW:
    case ZYDIS_MNEMONIC_FSQRT: case ZYDIS_MNEMONIC_FRNDINT: case ZYDIS_MNEMONIC_FSCALE:
    case ZYDIS_MNEMONIC_FPREM: case ZYDIS_MNEMONIC_FPREM1: case ZYDIS_MNEMONIC_F2XM1:
    case ZYDIS_MNEMONIC_FYL2X: case ZYDIS_MNEMONIC_FPATAN: case ZYDIS_MNEMONIC_FPTAN:
    case ZYDIS_MNEMONIC_FSIN: case ZYDIS_MNEMONIC_FCOS: case ZYDIS_MNEMONIC_FSINCOS:
    case ZYDIS_MNEMONIC_FLDPI: case ZYDIS_MNEMONIC_FLDL2E: case ZYDIS_MNEMONIC_FLDL2T:
    case ZYDIS_MNEMONIC_FLDLG2: case ZYDIS_MNEMONIC_FLDLN2: case ZYDIS_MNEMONIC_FNCLEX:
    case ZYDIS_MNEMONIC_FNINIT: case ZYDIS_MNEMONIC_FFREE: case ZYDIS_MNEMONIC_FFREEP:
    case ZYDIS_MNEMONIC_FNSTENV: case ZYDIS_MNEMONIC_FLDENV: case ZYDIS_MNEMONIC_FNSAVE:
    case ZYDIS_MNEMONIC_FRSTOR: case ZYDIS_MNEMONIC_FDECSTP: case ZYDIS_MNEMONIC_FINCSTP:
        return true;
    default:
        return false;
    }
}

bool is_sse(ZydisMnemonic m) {
    switch (m) {
#define S(X) case ZYDIS_MNEMONIC_##X:
    S(ADDSS) S(SUBSS) S(MULSS) S(DIVSS) S(MINSS) S(MAXSS) S(SQRTSS) S(RCPSS) S(RSQRTSS)
    S(ADDSD) S(SUBSD) S(MULSD) S(DIVSD) S(MINSD) S(MAXSD) S(SQRTSD)
    S(ADDPS) S(SUBPS) S(MULPS) S(DIVPS) S(MINPS) S(MAXPS) S(SQRTPS) S(RCPPS) S(RSQRTPS)
    S(ADDPD) S(SUBPD) S(MULPD) S(DIVPD) S(MINPD) S(MAXPD) S(SQRTPD) S(HADDPS) S(HADDPD)
    S(ANDPS) S(ANDNPS) S(ORPS) S(XORPS) S(ANDPD) S(ANDNPD) S(ORPD) S(XORPD)
    S(UNPCKLPS) S(UNPCKHPS) S(UNPCKLPD) S(UNPCKHPD) S(MOVHLPS) S(MOVLHPS)
    S(SHUFPS) S(SHUFPD) S(PSHUFD) S(CMPPS) S(CMPPD) S(CMPSS) S(CMPSD)
    S(CVTSS2SD) S(CVTSD2SS) S(CVTDQ2PS) S(CVTDQ2PD) S(CVTTPS2DQ) S(CVTPS2DQ) S(CVTTPD2DQ)
    S(CVTPS2PD) S(CVTPD2PS) S(CVTSI2SS) S(CVTSI2SD) S(CVTTSS2SI) S(CVTSS2SI) S(CVTTSD2SI)
    S(CVTSD2SI)
    S(UCOMISS) S(COMISS) S(UCOMISD) S(COMISD)
    S(MOVAPS) S(MOVUPS) S(MOVAPD) S(MOVUPD) S(MOVDQA) S(MOVDQU) S(MOVNTPS) S(MOVNTDQ)
    S(MOVSS) S(MOVSD) S(MOVD) S(MOVQ) S(MOVHPS) S(MOVHPD) S(MOVLPS) S(MOVLPD)
    S(MOVMSKPS) S(MOVMSKPD)
    S(PADDD) S(PSUBD) S(PADDQ) S(PSUBQ) S(PADDW) S(PSUBW) S(PAND) S(PANDN) S(POR) S(PXOR)
    S(PCMPEQD) S(PCMPGTD) S(PCMPGTW)
    S(PSLLD) S(PSRLD) S(PSRAD) S(PSLLQ) S(PSRLQ) S(PSLLW) S(PSRLW) S(PSRAW) S(PSLLDQ) S(PSRLDQ)
    S(PUNPCKLBW) S(PUNPCKHBW) S(PUNPCKLWD) S(PUNPCKHWD) S(PUNPCKLDQ) S(PUNPCKHDQ)
    S(PUNPCKLQDQ) S(PUNPCKHQDQ) S(PACKSSDW) S(PACKSSWB) S(PACKUSWB)
    S(LDMXCSR) S(STMXCSR) S(CVTPI2PS)
    S(PADDSW) S(PSUBSW) S(PADDUSW) S(PSUBUSW) S(PADDB) S(PSUBB) S(PMULLW) S(PMULHW) S(PMULHUW)
    S(PMADDWD) S(PMULUDQ) S(PCMPEQW) S(PCMPEQB) S(PMINSW) S(PMAXSW) S(PSHUFLW) S(PSHUFHW)
    S(PMOVMSKB) S(PEXTRW) S(PINSRW) S(PSHUFW) S(CVTTPS2PI) S(CVTPS2PI)
#undef S
        return true;
    default:
        return false;
    }
}

bool supported_mnemonic(ZydisMnemonic m) {
    if (is_jcc(m) || is_loop(m) || is_x87(m) || is_sse(m)) return true;
    if (m >= ZYDIS_MNEMONIC_CMOVB && m <= ZYDIS_MNEMONIC_CMOVZ) return true;
    if (m == ZYDIS_MNEMONIC_MUL || m == ZYDIS_MNEMONIC_IMUL) return true;
    if (m == ZYDIS_MNEMONIC_DIV || m == ZYDIS_MNEMONIC_IDIV) return true;
    switch (m) {
    case ZYDIS_MNEMONIC_SETO: case ZYDIS_MNEMONIC_SETNO: case ZYDIS_MNEMONIC_SETB:
    case ZYDIS_MNEMONIC_SETNB: case ZYDIS_MNEMONIC_SETZ: case ZYDIS_MNEMONIC_SETNZ:
    case ZYDIS_MNEMONIC_SETBE: case ZYDIS_MNEMONIC_SETNBE: case ZYDIS_MNEMONIC_SETS:
    case ZYDIS_MNEMONIC_SETNS: case ZYDIS_MNEMONIC_SETP: case ZYDIS_MNEMONIC_SETNP:
    case ZYDIS_MNEMONIC_SETL: case ZYDIS_MNEMONIC_SETNL: case ZYDIS_MNEMONIC_SETLE:
    case ZYDIS_MNEMONIC_SETNLE:
    case ZYDIS_MNEMONIC_NOP: case ZYDIS_MNEMONIC_MOV: case ZYDIS_MNEMONIC_MOVZX:
    case ZYDIS_MNEMONIC_MOVSX: case ZYDIS_MNEMONIC_LEA: case ZYDIS_MNEMONIC_PUSH:
    case ZYDIS_MNEMONIC_POP: case ZYDIS_MNEMONIC_LEAVE: case ZYDIS_MNEMONIC_RET:
    case ZYDIS_MNEMONIC_JMP: case ZYDIS_MNEMONIC_CALL: case ZYDIS_MNEMONIC_NOT:
    case ZYDIS_MNEMONIC_ADD: case ZYDIS_MNEMONIC_ADC: case ZYDIS_MNEMONIC_SUB:
    case ZYDIS_MNEMONIC_SBB: case ZYDIS_MNEMONIC_CMP: case ZYDIS_MNEMONIC_AND:
    case ZYDIS_MNEMONIC_OR: case ZYDIS_MNEMONIC_XOR: case ZYDIS_MNEMONIC_TEST:
    case ZYDIS_MNEMONIC_INC: case ZYDIS_MNEMONIC_DEC: case ZYDIS_MNEMONIC_NEG:
    case ZYDIS_MNEMONIC_SHL: case ZYDIS_MNEMONIC_SHR: case ZYDIS_MNEMONIC_SAR:
    case ZYDIS_MNEMONIC_ROL: case ZYDIS_MNEMONIC_ROR: case ZYDIS_MNEMONIC_SHLD:
    case ZYDIS_MNEMONIC_SHRD: case ZYDIS_MNEMONIC_CWDE: case ZYDIS_MNEMONIC_CBW:
    case ZYDIS_MNEMONIC_CDQ: case ZYDIS_MNEMONIC_CWD: case ZYDIS_MNEMONIC_XCHG:
    case ZYDIS_MNEMONIC_XADD: case ZYDIS_MNEMONIC_BSWAP: case ZYDIS_MNEMONIC_BSF:
    case ZYDIS_MNEMONIC_BSR: case ZYDIS_MNEMONIC_RDTSC: case ZYDIS_MNEMONIC_CPUID:
    case ZYDIS_MNEMONIC_INT3: case ZYDIS_MNEMONIC_HLT: case ZYDIS_MNEMONIC_UD2:
    case ZYDIS_MNEMONIC_EMMS: case ZYDIS_MNEMONIC_CLD: case ZYDIS_MNEMONIC_STD:
    case ZYDIS_MNEMONIC_MOVSB: case ZYDIS_MNEMONIC_MOVSW: case ZYDIS_MNEMONIC_STOSB:
    case ZYDIS_MNEMONIC_STOSW: case ZYDIS_MNEMONIC_STOSD: case ZYDIS_MNEMONIC_LODSB:
    case ZYDIS_MNEMONIC_LODSW: case ZYDIS_MNEMONIC_LODSD: case ZYDIS_MNEMONIC_CMPSB:
    case ZYDIS_MNEMONIC_CMPSW: case ZYDIS_MNEMONIC_SCASB: case ZYDIS_MNEMONIC_SCASW:
    case ZYDIS_MNEMONIC_SCASD:
    case ZYDIS_MNEMONIC_LAHF: case ZYDIS_MNEMONIC_SAHF: case ZYDIS_MNEMONIC_PUSHFD:
    case ZYDIS_MNEMONIC_POPFD: case ZYDIS_MNEMONIC_PUSHAD: case ZYDIS_MNEMONIC_POPAD:
    case ZYDIS_MNEMONIC_CLC: case ZYDIS_MNEMONIC_STC: case ZYDIS_MNEMONIC_CMC:
    case ZYDIS_MNEMONIC_PREFETCHNTA: case ZYDIS_MNEMONIC_PREFETCHT0:
    case ZYDIS_MNEMONIC_PREFETCHT1: case ZYDIS_MNEMONIC_PREFETCHT2:
    case ZYDIS_MNEMONIC_LFENCE: case ZYDIS_MNEMONIC_SFENCE: case ZYDIS_MNEMONIC_MFENCE:
    case ZYDIS_MNEMONIC_PAUSE: case ZYDIS_MNEMONIC_BT: case ZYDIS_MNEMONIC_BTS:
    case ZYDIS_MNEMONIC_BTR: case ZYDIS_MNEMONIC_BTC: case ZYDIS_MNEMONIC_CMPXCHG:
    case ZYDIS_MNEMONIC_RCL: case ZYDIS_MNEMONIC_RCR: case ZYDIS_MNEMONIC_XLAT:
        return true;
    default:
        return false;
    }
}

namespace {

std::string fmt_err(const char* f, uint32_t a, const char* s = "") {
    char buf[256];
    std::snprintf(buf, sizeof buf, f, a, s);
    return buf;
}

// The instruction that ends right where `a` starts, if decoded.
const Insn* previous(const DecodedFunction& fn, uint32_t a) {
    auto it = fn.insns.lower_bound(a);
    if (it == fn.insns.begin()) return nullptr;
    --it;
    return it->second.addr + it->second.in.length == a ? &it->second : nullptr;
}

// The instruction control reaches `a` from on a straight path: the one before
// it when that falls through, else the only branch to `a`; nullptr when there
// are several (or none).
const Insn* predecessor(const DecodedFunction& fn, uint32_t a) {
    const Insn* p = previous(fn, a);
    if (p && p->falls_through && !fn.labels.count(a)) return p;
    const Insn* only = p && p->falls_through ? p : nullptr;
    int n = only ? 1 : 0;
    if (fn.labels.count(a))
        for (const auto& [ja, j] : fn.insns)
            if ((j.xfer == Xfer::CondJump || j.xfer == Xfer::Jump) && j.target == a) only = &j, n++;
    return n == 1 ? only : nullptr;
}

ZydisRegister reg32(ZydisRegister r) { return ZydisRegisterGetLargestEnclosing(kMode, r); }

bool is_reg(const ZydisDecodedOperand& op, ZydisRegister r) {
    return op.type == ZYDIS_OPERAND_TYPE_REGISTER && reg32(op.reg.value) == r;
}

// Value a register holds as a PIC base: `call next; pop reg` sets it to the
// address of the pop. 0 if the function has no such pair for `r` (or several).
uint32_t pic_base(const DecodedFunction& fn, ZydisRegister r) {
    uint32_t v = 0;
    for (const auto& [a, i] : fn.insns) {
        if (i.xfer != Xfer::CallNext) continue;
        auto it = fn.insns.find(i.target);
        if (it == fn.insns.end() || it->second.in.mnemonic != ZYDIS_MNEMONIC_POP ||
            !is_reg(it->second.ops[0], r))
            continue;
        if (v && v != i.target) return 0;
        v = i.target;
    }
    return v;
}

// Number of table entries implied by the bounds check that guards the jump:
// `cmp x, imm` then `ja default` (imm + 1 entries; or `jae`, imm) on the path
// that falls through to the table, or `jbe table` (`jb`) on the path that
// branches to it. The check may sit in another block that jumps to the
// table's block, so predecessors through branches are searched as well as the
// straight-line ones. 0 if none is found within a few instructions.
uint32_t table_bound(const DecodedFunction& fn, uint32_t jmp_addr,
                     ZydisRegister index = ZYDIS_REGISTER_NONE) {
    struct Item { uint32_t addr; int depth; };
    std::vector<Item> work{{jmp_addr, 0}};
    std::set<uint32_t> seen;
    auto check = [&](const Insn& jcc, bool taken) -> uint32_t {
        const auto m = jcc.in.mnemonic;
        const bool falls_ok = !taken && (m == ZYDIS_MNEMONIC_JNBE || m == ZYDIS_MNEMONIC_JNB);
        const bool taken_ok = taken && (m == ZYDIS_MNEMONIC_JBE || m == ZYDIS_MNEMONIC_JB);
        if (!falls_ok && !taken_ok) return 0;
        const Insn* c = previous(fn, jcc.addr);
        // MSVC schedules moves and pushes between the compare and the branch
        // (SSE and x87 moves too: Prince of Persia's 0x87d68a).
        for (int k = 0; index != ZYDIS_REGISTER_NONE && k < 6 && c && c->falls_through; k++) {
            const auto cm = c->in.mnemonic;
            if (cm != ZYDIS_MNEMONIC_MOV && cm != ZYDIS_MNEMONIC_PUSH && cm != ZYDIS_MNEMONIC_LEA &&
                cm != ZYDIS_MNEMONIC_POP && cm != ZYDIS_MNEMONIC_MOVZX && cm != ZYDIS_MNEMONIC_MOVSX &&
                cm != ZYDIS_MNEMONIC_MOVSS && cm != ZYDIS_MNEMONIC_MOVSD && cm != ZYDIS_MNEMONIC_MOVAPS &&
                cm != ZYDIS_MNEMONIC_MOVUPS && cm != ZYDIS_MNEMONIC_FLD && cm != ZYDIS_MNEMONIC_FSTP &&
                cm != ZYDIS_MNEMONIC_FST)
                break;
            if (c->in.operand_count_visible >= 1 && is_reg(c->ops[0], index) && cm != ZYDIS_MNEMONIC_PUSH) return 0;
            c = previous(fn, c->addr);
        }
        if (!c || c->in.mnemonic != ZYDIS_MNEMONIC_CMP ||
            c->ops[1].type != ZYDIS_OPERAND_TYPE_IMMEDIATE)
            return 0;
        // When the index register is known, the check must be on it.
        if (index != ZYDIS_REGISTER_NONE && c->ops[0].type == ZYDIS_OPERAND_TYPE_REGISTER &&
            reg32(c->ops[0].reg.value) != index)
            return 0;
        const uint64_t imm = c->ops[1].imm.value.u & 0xffffffffu;
        if (imm > 4096) return 0;
        const bool inclusive = m == ZYDIS_MNEMONIC_JNBE || m == ZYDIS_MNEMONIC_JBE;
        return (uint32_t)imm + (inclusive ? 1 : 0);
    };
    while (!work.empty()) {
        const Item it = work.back();
        work.pop_back();
        if (it.depth > 10 || !seen.insert(it.addr).second) continue;
        // Straight-line predecessor.
        if (const Insn* p = previous(fn, it.addr); p && p->falls_through) {
            if (p->xfer == Xfer::CondJump) {
                if (uint32_t n = check(*p, false)) return n;
            }
            // `and $mask, %index` (MSVC's memcpy): mask + 1 entries.
            if (index != ZYDIS_REGISTER_NONE && p->in.mnemonic == ZYDIS_MNEMONIC_AND &&
                is_reg(p->ops[0], index) && p->ops[1].type == ZYDIS_OPERAND_TYPE_IMMEDIATE &&
                (p->ops[1].imm.value.u & 0xffffffffu) < 4096)
                return (uint32_t)(p->ops[1].imm.value.u & 0xffffffffu) + 1;
            if (index != ZYDIS_REGISTER_NONE && p->in.operand_count_visible >= 1 &&
                is_reg(p->ops[0], index) && p->in.mnemonic != ZYDIS_MNEMONIC_CMP &&
                p->in.mnemonic != ZYDIS_MNEMONIC_TEST && p->in.mnemonic != ZYDIS_MNEMONIC_PUSH &&
                p->in.mnemonic != ZYDIS_MNEMONIC_MOVZX)
                continue;  // the index is computed here: a check before says nothing
            work.push_back({p->addr, it.depth + 1});
        }
        // Branches to this address.
        if (fn.labels.count(it.addr))
            for (const auto& [a, j] : fn.insns)
                if ((j.xfer == Xfer::CondJump || j.xfer == Xfer::Jump) && j.target == it.addr) {
                    if (j.xfer == Xfer::CondJump)
                        if (uint32_t n = check(j, true)) return n;
                    work.push_back({a, it.depth + 1});
                }
    }
    return 0;
}

// The value register `r` holds just before `at`, when it comes from a short
// chain the compiler uses for jump table addresses: `lea K(%pic), r` (pic +
// K), `mov $imm, r`, or `mov slot, r` where every store to `slot` (an
// esp/ebp-relative stack slot) in the function stores the same such value.
bool register_value(const DecodedFunction& fn, ZydisRegister r, uint32_t at, ZydisRegister pic,
                    uint32_t pic_value, uint32_t* out, int depth = 0) {
    if (depth > 3) return false;
    const Insn* i = predecessor(fn, at);
    for (int k = 0; i && k < 256; k++, i = predecessor(fn, i->addr)) {
        const auto m = i->in.mnemonic;
        if (!(i->in.operand_count_visible >= 1 && is_reg(i->ops[0], r))) {
            if (i->xfer == Xfer::Call || i->xfer == Xfer::CallImport || i->xfer == Xfer::CallIndirect) {
                if (r == ZYDIS_REGISTER_EAX || r == ZYDIS_REGISTER_ECX || r == ZYDIS_REGISTER_EDX)
                    return false;  // clobbered by the call
            }
            continue;
        }
        const auto& src = i->ops[1];
        if (m == ZYDIS_MNEMONIC_LEA && reg32(src.mem.base) == pic && src.mem.index == ZYDIS_REGISTER_NONE) {
            *out = pic_value + static_cast<uint32_t>(src.mem.disp.value);
            return true;
        }
        if (m == ZYDIS_MNEMONIC_MOV && src.type == ZYDIS_OPERAND_TYPE_IMMEDIATE) {
            *out = static_cast<uint32_t>(src.imm.value.u);
            return true;
        }
        if (m == ZYDIS_MNEMONIC_MOV && src.type == ZYDIS_OPERAND_TYPE_MEMORY &&
            src.mem.index == ZYDIS_REGISTER_NONE &&
            (reg32(src.mem.base) == ZYDIS_REGISTER_ESP || reg32(src.mem.base) == ZYDIS_REGISTER_EBP)) {
            // Every store to this stack slot must give the same value.
            int stores = 0;
            uint32_t value = 0;
            for (const auto& [a, s] : fn.insns)
                if (s.in.mnemonic == ZYDIS_MNEMONIC_MOV && s.ops[0].type == ZYDIS_OPERAND_TYPE_MEMORY &&
                    reg32(s.ops[0].mem.base) == reg32(src.mem.base) && s.ops[0].mem.index == ZYDIS_REGISTER_NONE &&
                    s.ops[0].mem.disp.value == src.mem.disp.value) {
                    uint32_t v;
                    if (s.ops[1].type != ZYDIS_OPERAND_TYPE_REGISTER ||
                        !register_value(fn, reg32(s.ops[1].reg.value), s.addr, pic, pic_value, &v, depth + 1) ||
                        (stores && v != value))
                        return false;
                    value = v;
                    stores++;
                }
            if (!stores) return false;
            *out = value;
            return true;
        }
        return false;  // some other definition
    }
    return false;
}

// register_value over every path to `at`: walks back through all predecessors
// (fall-through and branches; loops are cut) to the first definition of `r`
// on each, which must all be the same `lea K(%pic), r`. For a table address
// kept in a callee-saved register across a loop of virtual calls (Havok).
bool register_value_paths(const DecodedFunction& fn, ZydisRegister r, uint32_t at, ZydisRegister pic,
                          uint32_t pic_value, uint32_t* out) {
    std::vector<uint32_t> work{at};
    std::set<uint32_t> seen;
    bool found = false;
    uint32_t value = 0;
    while (!work.empty()) {
        const uint32_t a = work.back();
        work.pop_back();
        if (!seen.insert(a).second) continue;
        if (seen.size() > 4096) return false;
        std::vector<const Insn*> preds;
        if (const Insn* p = previous(fn, a); p && p->falls_through) preds.push_back(p);
        if (fn.labels.count(a))
            for (const auto& [ja, j] : fn.insns)
                if (((j.xfer == Xfer::CondJump || j.xfer == Xfer::Jump) && j.target == a) ||
                    (j.xfer == Xfer::JumpTable && std::find(j.cases.begin(), j.cases.end(), a) != j.cases.end()))
                    preds.push_back(&j);
        if (preds.empty() && a != at) return false;  // the entry: r is undefined there
        for (const Insn* i : preds) {
            if (i->in.operand_count_visible >= 1 && is_reg(i->ops[0], r)) {
                const auto& src = i->ops[1];
                if (i->in.mnemonic != ZYDIS_MNEMONIC_LEA || reg32(src.mem.base) != pic ||
                    src.mem.index != ZYDIS_REGISTER_NONE)
                    return false;
                const uint32_t v = pic_value + static_cast<uint32_t>(src.mem.disp.value);
                if (found && v != value) return false;
                found = true, value = v;
                continue;
            }
            if ((i->xfer == Xfer::Call || i->xfer == Xfer::CallImport || i->xfer == Xfer::CallIndirect) &&
                (r == ZYDIS_REGISTER_EAX || r == ZYDIS_REGISTER_ECX || r == ZYDIS_REGISTER_EDX))
                return false;
            if (i->in.mnemonic == ZYDIS_MNEMONIC_POP && is_reg(i->ops[0], r)) return false;
            work.push_back(i->addr);
        }
    }
    if (!found) return false;
    *out = value;
    return true;
}

// Recognises the compiler's two jump-table forms and lists their targets:
//   jmp *table(,%idx,4)                         absolute entries
//   lea (,%idx,4), %r; jmp *table(%r)           absolute entries, index pre-scaled
//   mov off(%pic,%idx,4), %r; add %pic, %r; jmp *%r   entries relative to pic
//   mov off(%t,%idx,4), %r; add %pic, %r; jmp *%r     the same, with %t = pic + K
//                                                     loaded earlier (register_value)
//   mov (%s,%t), %r; add %pic, %r; jmp *%r            the same, unoptimized: %t = pic + K,
//                                                     %s = 4 * index (either way round)
// Returns false (leaving `cases` empty) for other indirect jumps.
bool resolve_table(const DecodedFunction& fn, const Insn& j, const Image& image,
                   std::vector<uint32_t>* cases, std::string* err) {
    const auto& op = j.ops[0];
    uint32_t table = 0, rel = 0;
    if (op.type == ZYDIS_OPERAND_TYPE_MEMORY && op.mem.base == ZYDIS_REGISTER_NONE &&
        op.mem.index != ZYDIS_REGISTER_NONE && op.mem.scale == 4) {
        table = static_cast<uint32_t>(op.mem.disp.value);
    } else if (op.type == ZYDIS_OPERAND_TYPE_MEMORY && op.mem.base != ZYDIS_REGISTER_NONE &&
               op.mem.index == ZYDIS_REGISTER_NONE) {
        const Insn* lea = previous(fn, j.addr);
        if (!lea || lea->in.mnemonic != ZYDIS_MNEMONIC_LEA ||
            !is_reg(lea->ops[0], reg32(op.mem.base)) ||
            lea->ops[1].mem.base != ZYDIS_REGISTER_NONE || lea->ops[1].mem.scale != 4 ||
            lea->ops[1].mem.disp.value != 0)
            return false;
        table = static_cast<uint32_t>(op.mem.disp.value);
    } else if (op.type == ZYDIS_OPERAND_TYPE_REGISTER && image.is_pe() && previous(fn, j.addr) &&
               previous(fn, j.addr)->in.mnemonic == ZYDIS_MNEMONIC_MOV &&
               is_reg(previous(fn, j.addr)->ops[0], reg32(op.reg.value)) &&
               previous(fn, j.addr)->ops[1].type == ZYDIS_OPERAND_TYPE_MEMORY &&
               previous(fn, j.addr)->ops[1].mem.base == ZYDIS_REGISTER_NONE &&
               previous(fn, j.addr)->ops[1].mem.index != ZYDIS_REGISTER_NONE &&
               previous(fn, j.addr)->ops[1].mem.scale == 4) {
        // MSVC (hand-written assembly): `mov T(,%idx,4), %r; jmp *%r`.
        const Insn* mv = previous(fn, j.addr);
        table = static_cast<uint32_t>(mv->ops[1].mem.disp.value);
        const ZydisRegister idx = reg32(mv->ops[1].mem.index);
        const uint32_t bounded = table_bound(fn, mv->addr, idx);
        const uint32_t n = bounded ? bounded : 64;  // no check: the table ends at its first foreign entry
        std::set<uint32_t> seen;
        for (uint32_t k = 0; k < n; k++) {
            const uint8_t* p = image.at(table + 4 * k, 4);
            if (!p) break;
            uint32_t e;
            std::memcpy(&e, p, 4);
            // Entries that discovery made functions (data pointers) run as tail calls.
            if (e < fn.lo || e >= fn.end || (!bounded && image.function_at(e))) break;
            if (seen.insert(e).second) cases->push_back(e);
        }
        if (cases->empty()) return false;
        return true;
    } else if (op.type == ZYDIS_OPERAND_TYPE_REGISTER) {
        const ZydisRegister r = reg32(op.reg.value);
        const Insn* add = previous(fn, j.addr);
        if (!add || add->in.mnemonic != ZYDIS_MNEMONIC_ADD || !is_reg(add->ops[0], r) ||
            add->ops[1].type != ZYDIS_OPERAND_TYPE_REGISTER)
            return false;
        const ZydisRegister pic = reg32(add->ops[1].reg.value);
        const Insn* mov = previous(fn, add->addr);
        if (!mov || mov->in.mnemonic != ZYDIS_MNEMONIC_MOV || !is_reg(mov->ops[0], r))
            return false;
        const auto& m = mov->ops[1];
        if (m.type != ZYDIS_OPERAND_TYPE_MEMORY || m.mem.index == ZYDIS_REGISTER_NONE) return false;
        rel = pic_base(fn, pic);
        if (!rel) return false;
        if (m.mem.scale == 1 && m.mem.base != ZYDIS_REGISTER_NONE) {
            // Unoptimized code: `lea (,%idx,4), %s; lea K(%pic), %t; mov (%s,%t), %r`,
            // the table's address in one register, the scaled index in the other.
            uint32_t base = 0;
            if (!register_value(fn, reg32(m.mem.base), mov->addr, pic, rel, &base) &&
                !register_value(fn, reg32(m.mem.index), mov->addr, pic, rel, &base))
                return false;
            table = base + static_cast<uint32_t>(m.mem.disp.value);
        } else if (m.mem.scale != 4) {
            return false;
        } else if (reg32(m.mem.base) == pic) {
            table = rel + static_cast<uint32_t>(m.mem.disp.value);
        } else {
            // The table's address is in another register, computed earlier
            // (often spilled to the stack): lea K(%pic) -> table = pic + K.
            uint32_t base = 0;
            if (!register_value(fn, reg32(m.mem.base), mov->addr, pic, rel, &base) &&
                !register_value_paths(fn, reg32(m.mem.base), mov->addr, pic, rel, &base))
                return false;
            table = base + static_cast<uint32_t>(m.mem.disp.value);
        }
    } else {
        return false;
    }
    // PE (MSVC): the bounds check must test the index register.
    const ZydisRegister index = image.is_pe() && op.type == ZYDIS_OPERAND_TYPE_MEMORY && rel == 0
                                    ? reg32(op.mem.index) : ZYDIS_REGISTER_NONE;
    // MSVC's two-level switch: `movzx %idx, byte T2(%r)` picks the entry from a
    // byte table, which the bounds check (on %r) covers; the entries are
    // 0..max(T2). Other instructions (pushes) may sit between it and the jump.
    const Insn* mz = previous(fn, j.addr);
    for (int k = 0; k < 4 && mz && mz->falls_through && mz->in.mnemonic != ZYDIS_MNEMONIC_MOVZX &&
                    !(mz->in.operand_count_visible >= 1 && index != ZYDIS_REGISTER_NONE &&
                      is_reg(mz->ops[0], index));
         k++)
        mz = previous(fn, mz->addr);
    uint32_t bound = 0;
    if (index != ZYDIS_REGISTER_NONE && mz && mz->in.mnemonic == ZYDIS_MNEMONIC_MOVZX &&
        is_reg(mz->ops[0], index) && mz->ops[1].type == ZYDIS_OPERAND_TYPE_MEMORY &&
        mz->ops[1].size == 8 &&
        (mz->ops[1].mem.index == ZYDIS_REGISTER_NONE || mz->ops[1].mem.base == ZYDIS_REGISTER_NONE)) {
        const auto& src = mz->ops[1].mem;
        const ZydisRegister byte_index = reg32(src.index != ZYDIS_REGISTER_NONE ? src.index : src.base);
        const uint32_t t2 = static_cast<uint32_t>(src.disp.value);
        const uint32_t bytes = table_bound(fn, mz->addr, byte_index);
        uint32_t n = 0;
        for (uint32_t k = 0; k < bytes; k++) {
            const uint8_t* b = image.at(t2 + k, 1);
            if (!b) return false;
            n = std::max<uint32_t>(n, *b + 1u);
        }
        // No bound found (a compare with a register): MSVC puts the byte
        // table right after the word table, which ends where it starts.
        if (!n && t2 > table && (t2 - table) % 4 == 0 && t2 - table <= 4096) n = (t2 - table) / 4;
        if (!n) return false;
        bound = n;
    } else {
        bound = table_bound(fn, j.addr, index);
    }
    // MSVC's memcpy and memmove: `cmp $n, %idx; jb T` ... `T: neg %idx; jmp
    // *B(,%idx,4)`: idx in [-(n-1), 0], the table is the n words up to B.
    if (const Insn* ng = previous(fn, j.addr); index != ZYDIS_REGISTER_NONE && ng && ng->falls_through &&
                                               ng->in.mnemonic == ZYDIS_MNEMONIC_NEG && is_reg(ng->ops[0], index)) {
        const uint32_t n = table_bound(fn, ng->addr, index);
        if (!n) return false;
        table -= 4 * (n - 1);
        bound = n;
    }
    // MSVC's memcpy: `sub $k, %idx; jb T` reaches `jmp *B(,%idx,4)` with idx in
    // [-k, -1]: the table is the k words below B.
    if (const Insn* pv = previous(fn, j.addr); index != ZYDIS_REGISTER_NONE && (!pv || !pv->falls_through)) {
        const Insn* jb = predecessor(fn, j.addr);
        const Insn* sub = jb ? previous(fn, jb->addr) : nullptr;
        if (jb && jb->in.mnemonic == ZYDIS_MNEMONIC_JB && jb->target == j.addr && sub &&
            sub->in.mnemonic == ZYDIS_MNEMONIC_SUB && is_reg(sub->ops[0], index) &&
            sub->ops[1].type == ZYDIS_OPERAND_TYPE_IMMEDIATE && sub->ops[1].imm.value.u <= 16) {
            const uint32_t k = (uint32_t)sub->ops[1].imm.value.u;
            table -= 4 * k;
            bound = k;
        }
    }
    const uint32_t limit = bound ? bound : 1024;
    std::set<uint32_t> seen;
    for (uint32_t k = 0; k < limit; k++) {
        const uint8_t* p = image.at(table + 4 * k, 4);
        if (!p) break;
        uint32_t e;
        std::memcpy(&e, p, 4);
        const uint32_t t = rel + e;
        if (t < fn.lo || t >= fn.end) {
            // MSVC's memcpy indexes some tables from 1: entry 0 is the code
            // before them. With a known bound, such entries are skipped.
            if (bound && image.is_pe()) continue;
            if (bound) {
                *err = fmt_err("jump table entry outside the function at %08x", j.addr);
                return false;
            }
            break;  // no bound known: the table ends at the first foreign entry
        }
        if (seen.insert(t).second) cases->push_back(t);
    }
    if (cases->empty()) {
        *err = fmt_err("empty jump table at %08x", j.addr);
        return false;
    }
    return true;
}

}  // namespace

// The word at `slot` when it lies in the read-only __TEXT segment and points
// into [start, end), else 0.
static uint32_t constant_code_slot(const Image& image, uint32_t slot, uint32_t start, uint32_t end) {
    for (const Section& s : image.sections())
        if (s.segname == "__TEXT" && slot >= s.addr && slot + 4 <= s.addr + s.size) {
            const uint8_t* p = image.at(slot, 4);
            if (!p) return 0;
            uint32_t v;
            std::memcpy(&v, p, 4);
            return v >= start && v < end ? v : 0;
        }
    return 0;
}

DecodedFunction decode_image_function(const Image& image, const Function& f) {
    if (image.is_pe()) {
        const Section* s = image.code_section(f.addr);
        const uint32_t lo = s ? s->addr : f.addr, hi = s ? s->addr + s->size : f.end;
        const uint8_t* code = image.at(lo, hi - lo);
        if (!code) code = image.at(f.addr, 1);
        return decode_function(code ? code + (f.addr - lo) : nullptr, f.addr, hi, &image, lo);
    }
    return decode_function(image.at(f.addr, f.end - f.addr), f.addr, f.end, &image);
}

DecodedFunction decode_function(const uint8_t* code, uint32_t start, uint32_t end,
                                const Image* image, uint32_t lo) {
    DecodedFunction fn;
    if (!lo || lo > start) lo = start;
    fn.start = start;
    fn.lo = lo;
    fn.end = end;
    fn.labels.insert(start);
    const bool pe = image && image->is_pe();
    // PE: another function's start ends this one (a branch there is a tail call).
    auto other_function = [&](uint32_t a) { return pe && a != start && image->function_at(a); };
    // PE: `call/jmp [slot]` through the import address table.
    auto iat_slot = [&](const Insn& i) -> uint32_t {
        if (!image || i.in.operand_count_visible < 1) return 0;
        const auto& op = i.ops[0];
        if (op.type != ZYDIS_OPERAND_TYPE_MEMORY || op.mem.base != ZYDIS_REGISTER_NONE ||
            op.mem.index != ZYDIS_REGISTER_NONE || op.mem.segment == ZYDIS_REGISTER_FS)
            return 0;
        const uint32_t slot = static_cast<uint32_t>(op.mem.disp.value);
        return image->stubs().count(slot) && pe ? slot : 0;
    };

    ZydisDecoder dec;
    ZydisDecoderInit(&dec, kMode, ZYDIS_STACK_WIDTH_32);
    ZydisFormatter att;
    ZydisFormatterInit(&att, ZYDIS_FORMATTER_STYLE_ATT);

    std::vector<uint32_t> work{start};
    std::set<uint32_t> tables_done;
    for (;;) {
        while (!work.empty()) {
            uint32_t a = work.back();
            work.pop_back();
            bool after_call = false;
            while (!fn.insns.count(a)) {
                if (other_function(a)) {
                    fn.falls_into.insert(a);
                    break;
                }
                if (a < lo || a >= end) {
                    // The compiler emits nothing after a call that never returns
                    // (abort, __cxa_throw...), so the next function follows directly.
                    if (!after_call)
                        fn.errors.push_back(fmt_err("falls out of the function at %08x", a));
                    break;
                }
                Insn i;
                i.addr = a;
                if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&dec, code + ((int64_t)a - start), end - a, &i.in,
                                                         i.ops))) {
                    fn.errors.push_back(fmt_err("cannot decode at %08x", a));
                    break;
                }
                char text[160];
                ZydisFormatterFormatInstruction(&att, &i.in, i.ops, i.in.operand_count_visible, text,
                                                sizeof text, a, nullptr);
                i.text = text;
                const auto m = i.in.mnemonic;
                const uint32_t next = a + i.in.length;
                const bool direct = i.in.operand_count_visible > 0 &&
                                    i.ops[0].type == ZYDIS_OPERAND_TYPE_IMMEDIATE &&
                                    i.ops[0].imm.is_relative;
                uint32_t t = 0;
                if (direct) {
                    ZyanU64 abs;
                    ZydisCalcAbsoluteAddress(&i.in, &i.ops[0], a, &abs);
                    t = static_cast<uint32_t>(abs);
                }
                const bool inside = t >= lo && t < end && !other_function(t);
                const uint32_t slot = (m == ZYDIS_MNEMONIC_CALL || m == ZYDIS_MNEMONIC_JMP) ? iat_slot(i) : 0;
                const bool is_stub = image && image->stubs().count(t);
                const bool is_func = image && image->function_at(t);

                if (m == ZYDIS_MNEMONIC_RET) {
                    i.xfer = Xfer::Ret;
                    i.falls_through = false;
                } else if (m == ZYDIS_MNEMONIC_HLT || m == ZYDIS_MNEMONIC_UD2 ||
                           (pe && m == ZYDIS_MNEMONIC_INT3)) {
                    // Traps: execution never continues here. MSVC pads with int3.
                    i.falls_through = false;
                } else if (m == ZYDIS_MNEMONIC_CALL && slot) {
                    i.xfer = Xfer::CallImport;
                    i.target = slot;
                } else if (m == ZYDIS_MNEMONIC_JMP && slot) {
                    i.xfer = Xfer::TailImport;
                    i.target = slot;
                    i.falls_through = false;
                } else if (m == ZYDIS_MNEMONIC_CALL) {
                    if (!direct) i.xfer = Xfer::CallIndirect;
                    else if (t == next) i.xfer = Xfer::CallNext;
                    else if (is_stub) i.xfer = Xfer::CallImport;
                    else if (is_func) i.xfer = Xfer::Call;
                    else fn.errors.push_back(fmt_err("call to a non-function address at %08x", a));
                    i.target = t;
                } else if (m == ZYDIS_MNEMONIC_JMP || is_jcc(m) || is_loop(m)) {
                    const bool cond = m != ZYDIS_MNEMONIC_JMP;
                    i.target = t;
                    i.falls_through = cond;
                    uint32_t slot_target = 0;
                    if (!direct && image && i.ops[0].type == ZYDIS_OPERAND_TYPE_MEMORY &&
                        i.ops[0].mem.base == ZYDIS_REGISTER_NONE &&
                        i.ops[0].mem.index == ZYDIS_REGISTER_NONE)
                        slot_target = constant_code_slot(*image, (uint32_t)i.ops[0].mem.disp.value,
                                                         lo, end);
                    if (slot_target) {
                        // A switch whose index the compiler folded to a constant:
                        // `jmp *TABLE` through read-only data, a plain jump.
                        i.xfer = Xfer::Jump;
                        i.target = slot_target;
                        fn.labels.insert(slot_target);
                        work.push_back(slot_target);
                    } else if (!direct) {
                        i.xfer = Xfer::JumpIndirect;
                    } else if (inside) {
                        i.xfer = cond ? Xfer::CondJump : Xfer::Jump;
                        fn.labels.insert(t);
                        work.push_back(t);
                    } else if (!cond && is_stub) {
                        i.xfer = Xfer::TailImport;
                    } else if (cond && pe && is_func) {
                        i.xfer = Xfer::CondTailCall;  // MSVC's CRT assembly does this
                    } else if (!cond && is_func) {
                        i.xfer = Xfer::TailCall;
                    } else {
                        fn.errors.push_back(fmt_err("branch leaves the function at %08x", a));
                    }
                }
                for (unsigned k = 0; k < i.in.operand_count_visible; k++) {
                    const auto& op = i.ops[k];
                    if (op.type == ZYDIS_OPERAND_TYPE_IMMEDIATE && !op.imm.is_relative &&
                        op.imm.value.u >= 0x1000 && op.imm.value.u <= 0xffffffffu)
                        fn.refs.insert(static_cast<uint32_t>(op.imm.value.u));
                    if (op.type == ZYDIS_OPERAND_TYPE_MEMORY && op.mem.disp.value >= 0x1000)
                        fn.refs.insert(static_cast<uint32_t>(op.mem.disp.value));
                }
                if (!supported_mnemonic(m))
                    fn.errors.push_back(fmt_err("unsupported instruction at %08x: %s", a, text));
                const bool ft = i.falls_through;
                // Padding (nops) after a call that never returns may also run
                // to the next function.
                after_call = (m == ZYDIS_MNEMONIC_CALL && i.xfer != Xfer::CallNext) ||
                             (after_call && m == ZYDIS_MNEMONIC_NOP);
                fn.insns[a] = i;
                if (!ft) break;
                a = next;
            }
        }
        // Jump tables are resolved once the code before them is decoded (their
        // bounds check and the PIC base come from it); their targets may lead to
        // more code, and more tables.
        bool more = false;
        if (image)
            for (auto& [a, i] : fn.insns) {
                if (i.xfer != Xfer::JumpIndirect || !tables_done.insert(a).second) continue;
                std::vector<uint32_t> cases;
                std::string err;
                if (resolve_table(fn, i, *image, &cases, &err)) {
                    i.xfer = Xfer::JumpTable;
                    i.cases = cases;
                    for (uint32_t t : cases) {
                        fn.labels.insert(t);
                        work.push_back(t);
                    }
                    more = true;
                } else if (!err.empty()) {
                    fn.errors.push_back(err);
                }
            }
        if (!more) break;
    }
    uint32_t prev_end = 0;
    for (auto& [a, i] : fn.insns) {
        if (a < prev_end) fn.errors.push_back(fmt_err("overlapping instructions at %08x", a));
        prev_end = a + i.in.length;
    }
    // Position-independent code (the bundled libstdc++) addresses data and
    // functions relative to a PIC base register: count those as references too.
    std::map<ZydisRegister, uint32_t> pic;
    for (const auto& [a, i] : fn.insns)
        if (i.xfer == Xfer::CallNext) {
            auto it = fn.insns.find(i.target);
            if (it != fn.insns.end() && it->second.in.mnemonic == ZYDIS_MNEMONIC_POP &&
                it->second.ops[0].type == ZYDIS_OPERAND_TYPE_REGISTER) {
                const ZydisRegister r = reg32(it->second.ops[0].reg.value);
                if (!pic.count(r)) pic[r] = pic_base(fn, r);
            }
        }
    if (!pic.empty())
        for (const auto& [a, i] : fn.insns)
            for (unsigned k = 0; k < i.in.operand_count_visible; k++) {
                const auto& op = i.ops[k];
                if (op.type != ZYDIS_OPERAND_TYPE_MEMORY || op.mem.index != ZYDIS_REGISTER_NONE)
                    continue;
                auto it = pic.find(reg32(op.mem.base));
                if (it != pic.end() && it->second)
                    fn.refs.insert(it->second + static_cast<uint32_t>(op.mem.disp.value));
            }
    return fn;
}

namespace {

// "scimitar::Foo" from MSVC's decorated type name ".?AVFoo@scimitar@@" (nested
// templates are left as they are).
std::string rtti_class(const std::string& t) {
    if (t.size() < 6 || t.compare(0, 4, ".?AV") != 0 && t.compare(0, 4, ".?AU") != 0) return t;
    std::string body = t.substr(4);
    if (body.size() >= 2 && body.compare(body.size() - 2, 2, "@@") == 0) body.resize(body.size() - 2);
    if (body.find('?') != std::string::npos) return body;
    std::vector<std::string> parts;
    for (size_t p = 0; p <= body.size();) {
        const size_t q = body.find('@', p);
        parts.push_back(body.substr(p, q == std::string::npos ? std::string::npos : q - p));
        if (q == std::string::npos) break;
        p = q + 1;
    }
    std::string out;
    for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
        if (it->empty()) continue;
        if (!out.empty()) out += "::";
        out += *it;
    }
    return out;
}

size_t discover_pe_functions(Image& image) {
    // Strong starts: the entry point, exports, TLS callbacks and direct call
    // targets in code reached from them. Weak ones: code addresses found as
    // data (vtables, tables, immediates), which may be constants that happen
    // to fall in the code range; they are checked once everything is decoded.
    std::map<uint32_t, std::string> starts;
    std::set<uint32_t> weak;
    // Targets of direct calls from several functions: entries, whatever else
    // reaches them (a garbage start's call targets are random, so one caller
    // is not enough).
    std::map<uint32_t, std::set<uint32_t>> callers;
    auto byte_at = [&](uint32_t a) -> int {
        const uint8_t* p = image.at(a, 1);
        return p ? *p : -1;
    };
    // MSVC aligns functions to 16 bytes with int3 padding: take code
    // addresses seen as data that look like a start.
    // Small CRT helpers follow the previous function's ret directly.
    // So do the linker's jump thunks (`jmp rel32`, 5 bytes each, packed).
    auto thunk = [&](uint32_t a) {
        const uint8_t* p = image.at(a, 5);
        if (!p || p[0] != 0xe9) return false;
        int32_t rel;
        std::memcpy(&rel, p + 1, 4);
        return image.code_section(a + 5 + (uint32_t)rel) != nullptr;
    };
    // With base relocations (Revelations), every code address in data or in
    // an instruction's immediate is relocated: those are certain, and other
    // words are not addresses at all.
    const auto& relocs = image.relocations();
    std::set<uint32_t> reloc_targets;
    for (const auto& [site, target] : relocs)
        if (image.code_section(target)) reloc_targets.insert(target);
    auto plausible = [&](uint32_t a) {
        if (!relocs.empty()) return reloc_targets.count(a) != 0;
        // Packed code (no padding) follows a `ret`, `ret n` or a `jmp rel32`
        // thunk (Prince of Persia's 0xc0ebe5, after a run of thunks).
        return image.code_section(a) && (a % 16 == 0 || byte_at(a - 1) == 0xcc || byte_at(a - 1) == 0xc3 ||
                                         byte_at(a - 3) == 0xc2 || thunk(a) || (a >= 5 && thunk(a - 5)));
    };
    auto add = [&](uint32_t a, const std::string& name, bool strong) {
        if (!image.code_section(a)) return false;
        auto [it, fresh] = starts.emplace(a, name);
        if (fresh && !strong) weak.insert(a);
        if (!fresh && strong && weak.erase(a)) return true;  // confirmed: decode again
        return fresh;
    };
    char buf[32];
    auto sub = [&](uint32_t a) {
        std::snprintf(buf, sizeof buf, "sub_%08x", a);
        return std::string(buf);
    };
    add(image.entry(), "entry", true);
    for (const Export& e : image.exports()) add(e.addr, e.name, true);
    if (const uint32_t cb = image.tls().callbacks)
        for (uint32_t p = cb;; p += 4) {
            const uint8_t* q = image.at(p, 4);
            uint32_t v = 0;
            if (q) std::memcpy(&v, q, 4);
            if (!v) break;
            add(v, "tls_callback", true);
        }
    for (const Vtable& v : image.vtables())
        for (size_t k = 0; k < v.slots.size(); k++)
            add(v.slots[k], rtti_class(v.type) + "::vf" + std::to_string(k), false);
    // Words in data: with relocations, the relocated ones that hold a code
    // address (at any alignment: the C runtime's x87 dispatch tables are
    // packed); switch tables inside code are the functions' own.
    if (!relocs.empty())
        for (const auto& [site, target] : relocs)
            if (!image.code_section(site) && image.code_section(target)) add(target, sub(target), false);
    // Without: code addresses that look like a start, and any code
    // address in a run of them (tables of code addresses, such as the C
    // runtime's x87 dispatch tables, whose entries are labels).
    static const std::vector<Section> kNone;
    for (const Section& sec : relocs.empty() ? image.sections() : kNone) {
        if (sec.flags & 0x20000000u) continue;  // code
        auto word = [&](uint32_t a, uint32_t* v) {
            if (a < sec.addr || a + 4 > sec.addr + sec.size) return false;
            const uint8_t* p = image.at(a, 4);
            if (!p) return false;
            std::memcpy(v, p, 4);
            return true;
        };
        for (uint32_t a = sec.addr; a + 4 <= sec.addr + sec.size; a += 4) {
            uint32_t v, w;
            if (!word(a, &v)) break;
            if (!image.code_section(v)) continue;
            const bool in_run = (word(a - 4, &w) && image.code_section(w)) || (word(a + 4, &w) && image.code_section(w));
            if (plausible(v) || in_run) add(v, sub(v), false);
        }
    }
    // Decoding reveals more: calls (strong if the caller is), and addresses
    // of code taken in code (weak).
    auto explore = [&](std::vector<uint32_t> pending) {
        while (!pending.empty()) {
            image.set_function_starts(starts);
            std::vector<std::pair<uint32_t, bool>> found;
            for (uint32_t a : pending) {
                const Function* f = image.function_at(a);
                if (!f) continue;
                const bool strong = !weak.count(a);
                const DecodedFunction fn = decode_image_function(image, *f);
                for (const auto& [ia, i] : fn.insns) {
                    if (i.in.mnemonic == ZYDIS_MNEMONIC_CALL && i.target && i.xfer != Xfer::CallImport &&
                        i.xfer != Xfer::CallNext && image.code_section(i.target)) {
                        found.push_back({i.target, strong});
                        callers[i.target].insert(a);
                    }
                    for (unsigned k = 0; k < i.in.operand_count_visible; k++) {
                        const auto& op = i.ops[k];
                        if (op.type == ZYDIS_OPERAND_TYPE_IMMEDIATE && !op.imm.is_relative &&
                            plausible(static_cast<uint32_t>(op.imm.value.u)))
                            found.push_back({static_cast<uint32_t>(op.imm.value.u), false});
                    }
                }
            }
            pending.clear();
            for (auto [t, strong] : found)
                if (add(t, sub(t), strong)) pending.push_back(t);
        }
    };
    {
        std::vector<uint32_t> all;
        for (const auto& [a, n] : starts) all.push_back(a);
        explore(all);
    }
    // Weak starts that are not code: those inside another function's
    // instruction, targets of a conditional jump elsewhere (MSVC never
    // branches conditionally to another function), or whose own code does not
    // decode as a function. Dropping one changes how its neighbours decode, so
    // repeat until none is left.
    for (int round = 0; round < 8; round++) {
        image.set_function_starts(starts);
        std::vector<std::pair<uint32_t, uint8_t>> insns;  // start, length (of functions kept)
        std::set<uint32_t> jcc_targets, falls_into, bad;
        for (const Function& f : image.functions()) {
            const DecodedFunction fn = decode_image_function(image, f);
            // Only strong functions are evidence against weak starts: a weak
            // one may be garbage itself (weak starts that disagree with each
            // other are both kept; splitting code is safe, flags are passed).
            const bool trusted = !weak.count(f.addr);
            for (const auto& [ia, i] : fn.insns) {
                if (!trusted) continue;
                insns.push_back({ia, i.in.length});
                // MSVC functions never run into the next one (but after calls
                // that do not return): a weak start this code runs into is
                // inside it.
                const uint32_t next = ia + i.in.length;
                if (i.falls_through && fn.falls_into.count(next) && i.in.mnemonic != ZYDIS_MNEMONIC_CALL &&
                    i.in.mnemonic != ZYDIS_MNEMONIC_NOP)
                    falls_into.insert(next);
                if ((i.xfer == Xfer::None || i.xfer == Xfer::CondTailCall) && is_jcc(i.in.mnemonic) &&
                    i.target != f.addr)
                    jcc_targets.insert(i.target);
            }
            // Instructions a game never runs (I/O, BCD, far pointers, system):
            // a weak start decoding to them is data.
            if (weak.count(f.addr))
                for (const auto& [ia, i] : fn.insns)
                    switch (i.in.mnemonic) {
                    case ZYDIS_MNEMONIC_IN: case ZYDIS_MNEMONIC_OUT: case ZYDIS_MNEMONIC_INSB:
                    case ZYDIS_MNEMONIC_INSD: case ZYDIS_MNEMONIC_INSW: case ZYDIS_MNEMONIC_OUTSB:
                    case ZYDIS_MNEMONIC_OUTSD: case ZYDIS_MNEMONIC_OUTSW: case ZYDIS_MNEMONIC_LES:
                    case ZYDIS_MNEMONIC_LDS: case ZYDIS_MNEMONIC_LSS: case ZYDIS_MNEMONIC_LFS:
                    case ZYDIS_MNEMONIC_LGS: case ZYDIS_MNEMONIC_ENTER: case ZYDIS_MNEMONIC_INT:
                    case ZYDIS_MNEMONIC_INT1: case ZYDIS_MNEMONIC_INTO: case ZYDIS_MNEMONIC_BOUND:
                    case ZYDIS_MNEMONIC_AAA: case ZYDIS_MNEMONIC_AAS: case ZYDIS_MNEMONIC_AAM:
                    case ZYDIS_MNEMONIC_AAD: case ZYDIS_MNEMONIC_DAA: case ZYDIS_MNEMONIC_DAS:
                    case ZYDIS_MNEMONIC_ARPL: case ZYDIS_MNEMONIC_IRETD: case ZYDIS_MNEMONIC_IRET:
                    case ZYDIS_MNEMONIC_CLI: case ZYDIS_MNEMONIC_STI: case ZYDIS_MNEMONIC_SALC:
                    case ZYDIS_MNEMONIC_HLT:
                        bad.insert(f.addr);
                        break;
                    default:
                        if (i.in.operand_count_visible > 0 && i.ops[0].type == ZYDIS_OPERAND_TYPE_POINTER)
                            bad.insert(f.addr);  // far call/jmp
                        for (unsigned k = 0; k < i.in.operand_count_visible; k++)
                            if (i.ops[k].type == ZYDIS_OPERAND_TYPE_REGISTER &&
                                ZydisRegisterGetClass(i.ops[k].reg.value) == ZYDIS_REGCLASS_SEGMENT &&
                                i.in.mnemonic != ZYDIS_MNEMONIC_MOV && i.in.mnemonic != ZYDIS_MNEMONIC_PUSH)
                                bad.insert(f.addr);  // pop es & co.
                        break;
                    }
            if (weak.count(f.addr))
                for (const std::string& e : fn.errors)
                    if (e.rfind("unsupported instruction", 0) != 0) {
                        bad.insert(f.addr);
                        if (const char* dbg = std::getenv("SCIMITAR_DISCOVERY_DEBUG"))
                            if (std::strtoul(dbg, nullptr, 16) == f.addr)
                                std::fprintf(stderr, "discovery: %08x: %s\n", f.addr, e.c_str());
                    }
        }
        std::sort(insns.begin(), insns.end());
        for (uint32_t w : weak) {
            const auto cl = callers.find(w);
            if (jcc_targets.count(w) && (cl == callers.end() || cl->second.size() < 2)) bad.insert(w);
            // Code that runs into a direct call's target is more often dead
            // code after a call that does not return (MSVC leaves stray bytes
            // after `throw`, Prince of Persia's 0x4bc70f) than a false start.
            if (falls_into.count(w) && cl == callers.end()) bad.insert(w);
            auto it = std::lower_bound(insns.begin(), insns.end(), std::make_pair(w - 15, (uint8_t)0));
            for (; it != insns.end() && it->first < w; ++it)
                if (it->first + it->second > w) bad.insert(w);
        }
        if (const char* dbg = std::getenv("SCIMITAR_DISCOVERY_DEBUG")) {
            const uint32_t w = (uint32_t)std::strtoul(dbg, nullptr, 16);
            if (bad.count(w))
                std::fprintf(stderr, "discovery: %08x dropped in round %d (jcc target %d, run into %d)\n", w,
                             round, (int)jcc_targets.count(w), (int)falls_into.count(w));
        }
        if (bad.empty()) break;
        for (uint32_t a : bad) {
            starts.erase(a);
            weak.erase(a);
        }
    }
    image.set_function_starts(starts);
    return starts.size();
}

}  // namespace

size_t discover_functions(Image& image) {
    if (image.is_pe()) return discover_pe_functions(image);
    size_t added = 0;
    const Section* text = image.text();
    std::vector<uint32_t> todo;
    for (const Function& f : image.functions()) todo.push_back(f.addr);
    auto prologue_at = [&](uint32_t r) {
        if (!text || r < text->addr || (uint64_t)r + 3 > (uint64_t)text->addr + text->size ||
            image.function_at(r))
            return false;
        const uint8_t* b = image.at(r, 3);
        return b && b[0] == 0x55 && b[1] == 0x89 && b[2] == 0xe5;
    };
    // Code pointers in data (static initializer tables, callback tables).
    std::vector<uint32_t> from_data;
    for (const Section& sec : image.sections()) {
        if (sec.segname != "__DATA" || sec.type() == 1 /* zerofill */) continue;
        for (uint32_t a = sec.addr; a + 4 <= sec.addr + sec.size; a += 4) {
            const uint8_t* p = image.at(a, 4);
            if (!p) break;
            uint32_t v;
            std::memcpy(&v, p, 4);
            if (prologue_at(v)) from_data.push_back(v | 0x80000000u);
        }
    }
    bool first = true;
    while (!todo.empty()) {
        std::vector<uint32_t> targets;
        if (first) targets = from_data, first = false;
        std::map<uint32_t, DecodedFunction> decoded;
        for (uint32_t a : todo) {
            const Function* f = image.function_at(a);
            if (!f) continue;
            DecodedFunction fn = decode_function(image.at(f->addr, f->end - f->addr), f->addr,
                                                 f->end, &image);
            // Direct calls into another function's extent, and jumps that
            // leave the function for one (tail calls to code without a symbol).
            for (const auto& [ia, i] : fn.insns)
                if ((i.in.mnemonic == ZYDIS_MNEMONIC_CALL || i.in.mnemonic == ZYDIS_MNEMONIC_JMP) &&
                    i.xfer == Xfer::None && i.target && !image.stubs().count(i.target))
                    targets.push_back(i.target);
            // An address taken in code (absolute or PIC-relative) that lies in
            // __text, outside the code its container reaches, and starts with
            // the prologue every function of the game has (push %ebp; mov
            // %esp, %ebp): a function without a symbol that is only called
            // through a pointer.
            for (uint32_t r : fn.refs)
                if (prologue_at(r)) targets.push_back(r | 0x80000000u);  // marked: check reach
        }
        todo.clear();
        for (uint32_t t : targets) {
            const bool by_ref = t & 0x80000000u;
            t &= 0x7fffffffu;
            const Function* g = image.function_containing(t);
            if (!g || g->addr == t) continue;
            if (by_ref) {
                DecodedFunction owner = decode_function(image.at(g->addr, g->end - g->addr),
                                                        g->addr, g->end, &image);
                bool covered = false;
                for (const auto& [ia, i] : owner.insns)
                    if (t >= ia && t < ia + i.in.length) covered = true;
                if (covered) continue;
            }
            const uint32_t owner = g->addr;
            if (image.split_function(t)) {
                added++;
                todo.push_back(t);
                todo.push_back(owner);  // its extent shrank
            }
        }
    }
    return added;
}

}  // namespace recomp
