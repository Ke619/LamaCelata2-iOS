#include "emit.h"

#include <cxxabi.h>

#include <cctype>
#include <cinttypes>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <map>

#include "macho.h"

namespace recomp {

namespace {

// Flag bits tracked for liveness. AF is not modelled.
enum : uint8_t { CF = 1, PF = 2, ZF = 4, SF = 8, OF = 16, ALL = 31 };

[[noreturn]] void die(const char* f, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    throw EmitError(buf);
}

std::string fmt(const char* f, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    return buf;
}

// ---- conditions ----

struct Cond {
    const char* expr;
    uint8_t use;
};

bool condition(ZydisMnemonic m, Cond* out) {
    switch (m) {
#define C(J, S, E, U) \
    case ZYDIS_MNEMONIC_##J: \
    case ZYDIS_MNEMONIC_##S: *out = {E, U}; return true;
        C(JO, SETO, "of", OF)
        C(JNO, SETNO, "!of", OF)
        C(JB, SETB, "cf", CF)
        C(JNB, SETNB, "!cf", CF)
        C(JZ, SETZ, "zf", ZF)
        C(JNZ, SETNZ, "!zf", ZF)
        C(JBE, SETBE, "(cf || zf)", CF | ZF)
        C(JNBE, SETNBE, "(!cf && !zf)", CF | ZF)
        C(JS, SETS, "sf", SF)
        C(JNS, SETNS, "!sf", SF)
        C(JP, SETP, "pf", PF)
        C(JNP, SETNP, "!pf", PF)
        C(JL, SETL, "(sf != of)", SF | OF)
        C(JNL, SETNL, "(sf == of)", SF | OF)
        C(JLE, SETLE, "(zf || sf != of)", ZF | SF | OF)
        C(JNLE, SETNLE, "(!zf && sf == of)", ZF | SF | OF)
#undef C
#define CM(CMOV, J) \
    case ZYDIS_MNEMONIC_##CMOV: return condition(ZYDIS_MNEMONIC_##J, out);
        CM(CMOVO, JO) CM(CMOVNO, JNO) CM(CMOVB, JB) CM(CMOVNB, JNB)
        CM(CMOVZ, JZ) CM(CMOVNZ, JNZ) CM(CMOVBE, JBE) CM(CMOVNBE, JNBE)
        CM(CMOVS, JS) CM(CMOVNS, JNS) CM(CMOVP, JP) CM(CMOVNP, JNP)
        CM(CMOVL, JL) CM(CMOVNL, JNL) CM(CMOVLE, JLE) CM(CMOVNLE, JNLE)
#undef CM
    default:
        return false;
    }
}

bool is_cmov(ZydisMnemonic m) { return m >= ZYDIS_MNEMONIC_CMOVB && m <= ZYDIS_MNEMONIC_CMOVZ; }

// ---- operands ----

const char* reg_field(ZydisRegister r) {
    switch (ZydisRegisterGetLargestEnclosing(kMode, r)) {
    case ZYDIS_REGISTER_EAX: return "c.eax";
    case ZYDIS_REGISTER_ECX: return "c.ecx";
    case ZYDIS_REGISTER_EDX: return "c.edx";
    case ZYDIS_REGISTER_EBX: return "c.ebx";
    case ZYDIS_REGISTER_ESP: return "c.esp";
    case ZYDIS_REGISTER_EBP: return "c.ebp";
    case ZYDIS_REGISTER_ESI: return "c.esi";
    case ZYDIS_REGISTER_EDI: return "c.edi";
    default: die("unsupported register %s", ZydisRegisterGetString(r));
    }
}

bool is_high8(ZydisRegister r) {
    return r == ZYDIS_REGISTER_AH || r == ZYDIS_REGISTER_CH || r == ZYDIS_REGISTER_DH ||
           r == ZYDIS_REGISTER_BH;
}

const char* ctype(unsigned bits) {
    switch (bits) {
    case 8: return "uint8_t";
    case 16: return "uint16_t";
    case 32: return "uint32_t";
    default: die("unsupported operand width %u", bits);
    }
}

std::string ea(const ZydisDecodedOperand& op) {
    const auto& m = op.mem;
    if (m.segment != ZYDIS_REGISTER_DS && m.segment != ZYDIS_REGISTER_SS &&
        m.segment != ZYDIS_REGISTER_ES && m.segment != ZYDIS_REGISTER_NONE &&
        m.segment != ZYDIS_REGISTER_FS)
        die("unsupported segment %s", ZydisRegisterGetString(m.segment));
    std::string e;
    if (m.segment == ZYDIS_REGISTER_FS) e = "c.fs_base";  // Windows: the thread's TEB
    if (m.base != ZYDIS_REGISTER_NONE) e += (e.empty() ? "" : " + ") + std::string(reg_field(m.base));
    if (m.index != ZYDIS_REGISTER_NONE) {
        if (!e.empty()) e += " + ";
        e += reg_field(m.index);
        if (m.scale > 1) e += fmt(" * %u", m.scale);
    }
    const uint32_t disp = static_cast<uint32_t>(m.disp.value);
    if (disp || e.empty()) {
        if (!e.empty()) e += " + ";
        e += fmt("0x%" PRIx32 "u", disp);
    }
    return "(uint32_t)(" + e + ")";
}

// `as_width` sizes immediates by the instruction's operand width: an encoded
// imm8 is sign-extended (Zydis already did so in value.u) and must not be cut
// back to 8 bits.
// Segment selectors as 32-bit Windows sets them (flat code and data, fs the TEB).
uint32_t segment_selector(ZydisRegister r) {
    switch (r) {
    case ZYDIS_REGISTER_CS: return 0x1b;
    case ZYDIS_REGISTER_FS: return 0x3b;
    case ZYDIS_REGISTER_GS: return 0;
    default: return 0x23;
    }
}

std::string read(const ZydisDecodedOperand& op, unsigned as_width = 0) {
    const unsigned w =
        op.type == ZYDIS_OPERAND_TYPE_IMMEDIATE && as_width ? as_width : op.size;
    switch (op.type) {
    case ZYDIS_OPERAND_TYPE_REGISTER:
        if (ZydisRegisterGetClass(op.reg.value) == ZYDIS_REGCLASS_SEGMENT)
            return fmt("(%s)0x%xu", ctype(w ? w : 16), segment_selector(op.reg.value));
        if (is_high8(op.reg.value)) return fmt("(uint8_t)(%s >> 8)", reg_field(op.reg.value));
        if (w == 32) return reg_field(op.reg.value);
        return fmt("(%s)%s", ctype(w), reg_field(op.reg.value));
    case ZYDIS_OPERAND_TYPE_MEMORY:
        return fmt("rd%u(M, %s)", w, ea(op).c_str());
    case ZYDIS_OPERAND_TYPE_IMMEDIATE: {
        const uint64_t mask = w == 32 ? 0xffffffffull : (1ull << w) - 1;
        return fmt("(%s)0x%" PRIx64 "u", ctype(w), op.imm.value.u & mask);
    }
    default:
        die("unsupported operand type %d", op.type);
    }
}

std::string write(const ZydisDecodedOperand& op, const std::string& v) {
    const unsigned w = op.size;
    switch (op.type) {
    case ZYDIS_OPERAND_TYPE_REGISTER: {
        const char* r = reg_field(op.reg.value);
        if (is_high8(op.reg.value))
            return fmt("%s = (%s & ~0xff00u) | ((uint32_t)(uint8_t)(%s) << 8);", r, r,
                       v.c_str());
        if (w == 8) return fmt("%s = (%s & ~0xffu) | (uint8_t)(%s);", r, r, v.c_str());
        if (w == 16) return fmt("%s = (%s & ~0xffffu) | (uint16_t)(%s);", r, r, v.c_str());
        return fmt("%s = %s;", r, v.c_str());
    }
    case ZYDIS_OPERAND_TYPE_MEMORY:
        return fmt("wr%u(M, %s, %s);", w, ea(op).c_str(), v.c_str());
    default:
        die("cannot write to operand type %d", op.type);
    }
}

// ---- flag effects ----

// fcmovcc conditions, in terms of the integer flags.
const char* fcmov_condition(ZydisMnemonic m, uint8_t* use) {
    switch (m) {
    case ZYDIS_MNEMONIC_FCMOVB: *use = CF; return "cf";
    case ZYDIS_MNEMONIC_FCMOVNB: *use = CF; return "!cf";
    case ZYDIS_MNEMONIC_FCMOVE: *use = ZF; return "zf";
    case ZYDIS_MNEMONIC_FCMOVNE: *use = ZF; return "!zf";
    case ZYDIS_MNEMONIC_FCMOVBE: *use = CF | ZF; return "(cf || zf)";
    case ZYDIS_MNEMONIC_FCMOVNBE: *use = CF | ZF; return "(!cf && !zf)";
    case ZYDIS_MNEMONIC_FCMOVU: *use = PF; return "pf";
    case ZYDIS_MNEMONIC_FCMOVNU: *use = PF; return "!pf";
    default: return nullptr;
    }
}

void flag_effects(Insn& i) {
    const auto m = i.in.mnemonic;
    Cond c;
    if (condition(m, &c)) {
        i.use = c.use;
        return;
    }
    if (fcmov_condition(m, &i.use)) return;
    switch (i.xfer) {
    case Xfer::Call:
    case Xfer::CallImport:
    case Xfer::CallIndirect:
        // Flags are not preserved across calls; nothing may read them after.
        i.def = ALL;
        return;
    default:
        break;
    }
    switch (m) {
    case ZYDIS_MNEMONIC_ADC:
    case ZYDIS_MNEMONIC_SBB:
        i.use = CF;
        i.def = ALL;
        return;
    case ZYDIS_MNEMONIC_ADD:
    case ZYDIS_MNEMONIC_SUB:
    case ZYDIS_MNEMONIC_CMP:
    case ZYDIS_MNEMONIC_AND:
    case ZYDIS_MNEMONIC_OR:
    case ZYDIS_MNEMONIC_XOR:
    case ZYDIS_MNEMONIC_TEST:
    case ZYDIS_MNEMONIC_NEG:
    case ZYDIS_MNEMONIC_UCOMISS:  // set ZF/PF/CF, clear OF/SF
    case ZYDIS_MNEMONIC_COMISS:
    case ZYDIS_MNEMONIC_UCOMISD:
    case ZYDIS_MNEMONIC_COMISD:
    case ZYDIS_MNEMONIC_DIV:   // all flags undefined after division
    case ZYDIS_MNEMONIC_IDIV:
    case ZYDIS_MNEMONIC_MUL:   // SF/ZF/PF are architecturally undefined after
    case ZYDIS_MNEMONIC_IMUL:  // multiplies; compilers never read them
        i.def = ALL;
        return;
    case ZYDIS_MNEMONIC_INC:
    case ZYDIS_MNEMONIC_DEC:
        i.def = ALL & ~CF;  // CF is preserved
        return;
    case ZYDIS_MNEMONIC_FCOMI: case ZYDIS_MNEMONIC_FCOMIP: case ZYDIS_MNEMONIC_FUCOMI:
    case ZYDIS_MNEMONIC_FUCOMIP: case ZYDIS_MNEMONIC_CMPXCHG: case ZYDIS_MNEMONIC_POPFD:
    case ZYDIS_MNEMONIC_BT: case ZYDIS_MNEMONIC_BTS: case ZYDIS_MNEMONIC_BTR:
    case ZYDIS_MNEMONIC_BTC:
        i.def = ALL;
        return;
    case ZYDIS_MNEMONIC_SAHF:
        i.def = CF | PF | ZF | SF;
        return;
    case ZYDIS_MNEMONIC_LAHF:
        i.use = CF | PF | ZF | SF;
        return;
    case ZYDIS_MNEMONIC_PUSHFD:
        i.use = ALL;
        return;
    case ZYDIS_MNEMONIC_CLC:
    case ZYDIS_MNEMONIC_STC:
        i.def = CF;
        return;
    case ZYDIS_MNEMONIC_CMC:
        i.use = CF;
        i.def = CF;
        return;
    case ZYDIS_MNEMONIC_RCL:
    case ZYDIS_MNEMONIC_RCR:
        i.use = CF;
        if (i.ops[1].type == ZYDIS_OPERAND_TYPE_IMMEDIATE && (i.ops[1].imm.value.u & 31)) i.def = CF | OF;
        else i.may_def = CF | OF;
        return;
    case ZYDIS_MNEMONIC_LOOPE:
    case ZYDIS_MNEMONIC_LOOPNE:
        i.use = ZF;
        return;
    case ZYDIS_MNEMONIC_XADD:
    case ZYDIS_MNEMONIC_BSF:  // only ZF is defined; the rest are undefined
    case ZYDIS_MNEMONIC_BSR:
        i.def = ALL;
        return;
    case ZYDIS_MNEMONIC_CMPSB: case ZYDIS_MNEMONIC_CMPSW: case ZYDIS_MNEMONIC_SCASB:
    case ZYDIS_MNEMONIC_SCASW: case ZYDIS_MNEMONIC_SCASD:
        // With a rep prefix and ecx == 0 nothing is compared.
        if (i.in.attributes & (ZYDIS_ATTRIB_HAS_REPE | ZYDIS_ATTRIB_HAS_REPNE)) i.may_def = ALL;
        else i.def = ALL;
        return;
    case ZYDIS_MNEMONIC_CMPSD:  // the string form (the SSE form leaves flags alone)
        if (i.in.operand_count_visible == 0 ||
            i.ops[0].type != ZYDIS_OPERAND_TYPE_REGISTER ||
            ZydisRegisterGetClass(i.ops[0].reg.value) != ZYDIS_REGCLASS_XMM) {
            if (i.in.attributes & (ZYDIS_ATTRIB_HAS_REPE | ZYDIS_ATTRIB_HAS_REPNE)) i.may_def = ALL;
            else i.def = ALL;
        }
        return;
    case ZYDIS_MNEMONIC_ROL:
    case ZYDIS_MNEMONIC_ROR:
        // CF and OF only; a zero count changes nothing.
        if (i.ops[1].type == ZYDIS_OPERAND_TYPE_IMMEDIATE) {
            if (i.ops[1].imm.value.u & 31) i.def = CF | OF;
        } else {
            i.may_def = CF | OF;
        }
        return;
    case ZYDIS_MNEMONIC_SHLD:
    case ZYDIS_MNEMONIC_SHRD:
        if (i.ops[2].type == ZYDIS_OPERAND_TYPE_IMMEDIATE) {
            if (i.ops[2].imm.value.u & 31) i.def = ALL;
        } else {
            i.may_def = ALL;
        }
        return;
    case ZYDIS_MNEMONIC_SHL:
    case ZYDIS_MNEMONIC_SHR:
    case ZYDIS_MNEMONIC_SAR:
        // A zero count leaves every flag unchanged, so only a constant non-zero
        // count kills earlier flag values; a register count only may write them.
        if (i.ops[1].type == ZYDIS_OPERAND_TYPE_IMMEDIATE) {
            if (i.ops[1].imm.value.u & 31) i.def = ALL;
        } else {
            i.may_def = ALL;
        }
        return;
    default:
        return;
    }
}

// ---- one instruction ----

class Emitter {
public:
    Emitter(std::string* out, const Image* image, EmitResult* res, const DecodedFunction* fn)
        : out_(out), image_(image), res_(res), fn_(fn) {}

    void line(const std::string& s) { *out_ += "        " + s + "\n"; }

    void szp(unsigned w, uint8_t want, const char* indent = "") {
        if (want & ZF) line(fmt("%szf = r == 0;", indent));
        if (want & SF) line(fmt("%ssf = (r >> %u) & 1;", indent, w - 1));
        if (want & PF) line(fmt("%spf = parity(r);", indent));
    }

    // PE: functions may be entered with live flags (code split at a start
    // discovery found inside it, conditional tail calls), so transfers to
    // other functions other than calls pass them in the Context's eflags.
    bool passes_flags() const { return image_ && image_->is_pe(); }
    void store_flags() {
        if (passes_flags())
            line("c.eflags = (c.eflags & ~0x8d5u) | (uint32_t)cf | (uint32_t)pf << 2 | (uint32_t)zf << 6 | "
                 "(uint32_t)sf << 7 | (uint32_t)of << 11;");
    }

    void push_return(const Insn& i) {
        line("c.esp -= 4;");
        line(fmt("wr32(M, c.esp, 0x%08xu);", i.addr + i.in.length));
    }

    // C++ call expression for a direct call/jump target (function or stub).
    std::string callee(uint32_t target, bool via_stub) {
        if (via_stub) {
            if (!image_) die("import call without an image");
            const Import& imp = image_->stubs().at(target);
            if (!imp.internal) {
                res_->imports.insert(imp.name);
                return import_symbol(imp.name) + "(c);";
            }
            target = imp.internal;
        }
        res_->callees.insert(target);
        return func_symbol(target) + "(c);";
    }

    // ST(i) index of an x87 register operand.
    static int sti(const ZydisDecodedOperand& op) {
        return op.reg.value - ZYDIS_REGISTER_ST0;
    }

    // Value of an x87 memory source operand as an F80 expression.
    std::string fp_source(const ZydisDecodedOperand& op, bool integer) {
        const std::string a = ea(op);
        if (integer) {
            switch (op.size) {
            case 16: return fmt("x87::from_int((int16_t)rd16(M, %s))", a.c_str());
            case 32: return fmt("x87::from_int((int32_t)rd32(M, %s))", a.c_str());
            case 64: return fmt("x87::from_int((int64_t)rd64(M, %s))", a.c_str());
            default: break;
            }
        } else {
            switch (op.size) {
            case 32: return fmt("x87::from_f32(rd32(M, %s))", a.c_str());
            case 64: return fmt("x87::from_f64(rd64(M, %s))", a.c_str());
            case 80: return fmt("rd80(M, %s)", a.c_str());
            default: break;
            }
        }
        die("unsupported x87 operand size %u", op.size);
    }

    void x87(const Insn& i) {
        const auto m = i.in.mnemonic;
        const auto* o = i.ops;
        const unsigned n = i.in.operand_count_visible;
        const bool mem = n >= 1 && o[0].type == ZYDIS_OPERAND_TYPE_MEMORY;
        const char* F = "c.fpu";
        switch (m) {
        case ZYDIS_MNEMONIC_FWAIT:
        case ZYDIS_MNEMONIC_FNOP:
            return;
        case ZYDIS_MNEMONIC_FLD:
            if (mem) line(fmt("x87::push(%s, %s);", F, fp_source(o[0], false).c_str()));
            else line(fmt("{ F80 v = x87::get(%s, %d); x87::push(%s, v); }", F, sti(o[0]), F));
            return;
        case ZYDIS_MNEMONIC_FILD:
            line(fmt("x87::push(%s, %s);", F, fp_source(o[0], true).c_str()));
            return;
        case ZYDIS_MNEMONIC_FLDZ:
            line(fmt("x87::push(%s, x87::kZero);", F));
            return;
        case ZYDIS_MNEMONIC_FLD1:
            line(fmt("x87::push(%s, x87::kOne);", F));
            return;
        case ZYDIS_MNEMONIC_FST:
        case ZYDIS_MNEMONIC_FSTP: {
            if (mem) {
                const std::string a = ea(o[0]);
                const std::string v = fmt("x87::get(%s, 0)", F);
                if (o[0].size == 32) line(fmt("wr32(M, %s, x87::to_f32(%s, %s));", a.c_str(), F, v.c_str()));
                else if (o[0].size == 64) line(fmt("wr64(M, %s, x87::to_f64(%s, %s));", a.c_str(), F, v.c_str()));
                else if (o[0].size == 80) line(fmt("wr80(M, %s, %s);", a.c_str(), v.c_str()));
                else die("unsupported fst size %u at %08x", o[0].size, i.addr);
            } else {
                line(fmt("x87::set(%s, %d, x87::get(%s, 0));", F, sti(o[0]), F));
            }
            if (m == ZYDIS_MNEMONIC_FSTP) line(fmt("x87::pop(%s);", F));
            return;
        }
        case ZYDIS_MNEMONIC_FIST:
        case ZYDIS_MNEMONIC_FISTP:
        case ZYDIS_MNEMONIC_FISTTP: {
            const unsigned w = o[0].size;
            if (w != 16 && w != 32 && w != 64) die("unsupported fist size %u at %08x", w, i.addr);
            line(fmt("int64_t v = x87::to_int(%s, x87::get(%s, 0), %u, %s);", F, F, w,
                     m == ZYDIS_MNEMONIC_FISTTP ? "true" : "false"));
            line(fmt("wr%u(M, %s, (uint%u_t)v);", w, ea(o[0]).c_str(), w));
            if (m != ZYDIS_MNEMONIC_FIST) line(fmt("x87::pop(%s);", F));
            return;
        }
        case ZYDIS_MNEMONIC_FXCH: {
            const int k = n ? sti(o[0]) : 1;
            line(fmt("F80 a = x87::get(%s, 0), b = x87::get(%s, %d);", F, F, k));
            line(fmt("x87::set(%s, 0, b); x87::set(%s, %d, a);", F, F, k));
            return;
        }
        case ZYDIS_MNEMONIC_FCHS:
            line(fmt("x87::set(%s, 0, x87::chs(x87::get(%s, 0)));", F, F));
            return;
        case ZYDIS_MNEMONIC_FABS:
            line(fmt("x87::set(%s, 0, x87::abs(x87::get(%s, 0)));", F, F));
            return;
        case ZYDIS_MNEMONIC_FLDCW:
            line(fmt("%s.cw = rd16(M, %s);", F, ea(o[0]).c_str()));
            return;
        case ZYDIS_MNEMONIC_FNSTCW:
            line(fmt("wr16(M, %s, %s.cw);", ea(o[0]).c_str(), F));
            return;
        case ZYDIS_MNEMONIC_FNSTSW:
            if (mem) line(fmt("wr16(M, %s, x87::status(%s));", ea(o[0]).c_str(), F));
            else line(fmt("c.eax = (c.eax & ~0xffffu) | x87::status(%s);", F));
            return;
        case ZYDIS_MNEMONIC_FCOM: case ZYDIS_MNEMONIC_FCOMP: case ZYDIS_MNEMONIC_FCOMPP:
        case ZYDIS_MNEMONIC_FUCOM: case ZYDIS_MNEMONIC_FUCOMP: case ZYDIS_MNEMONIC_FUCOMPP:
        case ZYDIS_MNEMONIC_FICOM: case ZYDIS_MNEMONIC_FICOMP: case ZYDIS_MNEMONIC_FTST: {
            // ST0 against a memory operand, ST(i) (ST1 by default) or zero.
            const bool integer = m == ZYDIS_MNEMONIC_FICOM || m == ZYDIS_MNEMONIC_FICOMP;
            std::string b;
            if (m == ZYDIS_MNEMONIC_FTST) b = "x87::kZero";
            else if (mem) b = fp_source(o[0], integer);
            else {
                // Zydis lists st0 first when it shows both registers.
                int k = 1;
                for (unsigned q = 0; q < n; q++)
                    if (o[q].type == ZYDIS_OPERAND_TYPE_REGISTER && sti(o[q]) != 0) k = sti(o[q]);
                b = fmt("x87::get(%s, %d)", F, k);
            }
            line(fmt("x87::set_compare(%s, x87::compare(x87::get(%s, 0), %s));", F, F, b.c_str()));
            const int pops = (m == ZYDIS_MNEMONIC_FCOMPP || m == ZYDIS_MNEMONIC_FUCOMPP) ? 2
                             : (m == ZYDIS_MNEMONIC_FCOMP || m == ZYDIS_MNEMONIC_FUCOMP ||
                                m == ZYDIS_MNEMONIC_FICOMP) ? 1 : 0;
            for (int k = 0; k < pops; k++) line(fmt("x87::pop(%s);", F));
            return;
        }
        case ZYDIS_MNEMONIC_FCOMI: case ZYDIS_MNEMONIC_FCOMIP:
        case ZYDIS_MNEMONIC_FUCOMI: case ZYDIS_MNEMONIC_FUCOMIP: {
            int k = 1;
            for (unsigned q = 0; q < n; q++)
                if (o[q].type == ZYDIS_OPERAND_TYPE_REGISTER && sti(o[q]) != 0) k = sti(o[q]);
            if (want_ & ALL) {
                line(fmt("const int r = x87::compare(x87::get(%s, 0), x87::get(%s, %d));", F, F, k));
                if (want_ & ZF) line("zf = r == 0 || r == 2;");
                if (want_ & PF) line("pf = r == 2;");
                if (want_ & CF) line("cf = r == -1 || r == 2;");
                if (want_ & OF) line("of = false;");
                if (want_ & SF) line("sf = false;");
            }
            if (m == ZYDIS_MNEMONIC_FCOMIP || m == ZYDIS_MNEMONIC_FUCOMIP) line(fmt("x87::pop(%s);", F));
            return;
        }
        case ZYDIS_MNEMONIC_FXAM:
            line(fmt("x87::examine(%s);", F));
            return;
        case ZYDIS_MNEMONIC_FSQRT:
            line(fmt("x87::set(%s, 0, x87::sqrt(%s, x87::get(%s, 0)));", F, F, F));
            return;
        case ZYDIS_MNEMONIC_FRNDINT:
            line(fmt("x87::set(%s, 0, x87::rndint(%s, x87::get(%s, 0)));", F, F, F));
            return;
        case ZYDIS_MNEMONIC_FSCALE:
            line(fmt("x87::set(%s, 0, x87::scale(x87::get(%s, 0), x87::get(%s, 1)));", F, F, F));
            return;
        case ZYDIS_MNEMONIC_FPREM:
        case ZYDIS_MNEMONIC_FPREM1:
            line(fmt("x87::set(%s, 0, x87::prem(%s, x87::get(%s, 0), x87::get(%s, 1), %s));", F, F, F, F,
                     m == ZYDIS_MNEMONIC_FPREM1 ? "true" : "false"));
            return;
        case ZYDIS_MNEMONIC_F2XM1:
            line(fmt("x87::set(%s, 0, x87::f2xm1(x87::get(%s, 0)));", F, F));
            return;
        case ZYDIS_MNEMONIC_FSIN:
        case ZYDIS_MNEMONIC_FCOS:
            line(fmt("x87::set(%s, 0, x87::%s(x87::get(%s, 0)));", F,
                     m == ZYDIS_MNEMONIC_FSIN ? "sin" : "cos", F));
            line(fmt("%s.sw &= ~0x400u;", F));  // C2 = 0: the operand was in range
            return;
        case ZYDIS_MNEMONIC_FSINCOS:
            line(fmt("{ F80 v = x87::get(%s, 0); x87::set(%s, 0, x87::sin(v)); x87::push(%s, x87::cos(v)); }",
                     F, F, F));
            line(fmt("%s.sw &= ~0x400u;", F));
            return;
        case ZYDIS_MNEMONIC_FPTAN:
            line(fmt("x87::set(%s, 0, x87::tan(x87::get(%s, 0)));", F, F));
            line(fmt("x87::push(%s, x87::kOne);", F));
            line(fmt("%s.sw &= ~0x400u;", F));
            return;
        case ZYDIS_MNEMONIC_FYL2X:
        case ZYDIS_MNEMONIC_FPATAN:
            // ST1 = f(ST1, ST0), then pop.
            line(fmt("x87::set(%s, 1, x87::%s(x87::get(%s, 1), x87::get(%s, 0)));", F,
                     m == ZYDIS_MNEMONIC_FYL2X ? "yl2x" : "patan", F, F));
            line(fmt("x87::pop(%s);", F));
            return;
        case ZYDIS_MNEMONIC_FLDPI: line(fmt("x87::push(%s, x87::kPi);", F)); return;
        case ZYDIS_MNEMONIC_FLDL2E: line(fmt("x87::push(%s, x87::kL2e);", F)); return;
        case ZYDIS_MNEMONIC_FLDL2T: line(fmt("x87::push(%s, x87::kL2t);", F)); return;
        case ZYDIS_MNEMONIC_FLDLG2: line(fmt("x87::push(%s, x87::kLg2);", F)); return;
        case ZYDIS_MNEMONIC_FLDLN2: line(fmt("x87::push(%s, x87::kLn2);", F)); return;
        case ZYDIS_MNEMONIC_FNCLEX:
            return;  // exceptions are masked and never recorded
        case ZYDIS_MNEMONIC_FNINIT:
            line(fmt("x87::init(%s);", F));
            return;
        case ZYDIS_MNEMONIC_FFREE:
        case ZYDIS_MNEMONIC_FFREEP:
            line(fmt("%s.valid &= ~(1u << ((%s.top + %d) & 7));", F, F, sti(o[0])));
            if (m == ZYDIS_MNEMONIC_FFREEP) line(fmt("%s.top = (%s.top + 1) & 7;", F, F));
            return;
        case ZYDIS_MNEMONIC_FDECSTP: line(fmt("%s.top = (%s.top - 1) & 7;", F, F)); return;
        case ZYDIS_MNEMONIC_FINCSTP: line(fmt("%s.top = (%s.top + 1) & 7;", F, F)); return;
        case ZYDIS_MNEMONIC_FNSTENV: line(fmt("x87::store_env(%s, M + %s);", F, ea(o[0]).c_str())); return;
        case ZYDIS_MNEMONIC_FLDENV: line(fmt("x87::load_env(%s, M + %s);", F, ea(o[0]).c_str())); return;
        case ZYDIS_MNEMONIC_FNSAVE: line(fmt("x87::save(%s, M + %s);", F, ea(o[0]).c_str())); return;
        case ZYDIS_MNEMONIC_FRSTOR: line(fmt("x87::restore(%s, M + %s);", F, ea(o[0]).c_str())); return;
        default:
            break;
        }
        uint8_t use;
        if (const char* cond = fcmov_condition(m, &use)) {
            const std::string e = cond[0] == '(' ? cond : fmt("(%s)", cond);
            line(fmt("if %s x87::set(%s, 0, x87::get(%s, %d));", e.c_str(), F, F, sti(o[1])));
            return;
        }
        // Arithmetic: fop mem (ST0 op= mem), fop st(d), st(s), and the popping forms.
        const char* op = nullptr;
        bool reverse = false, popping = false, integer = false;
        switch (m) {
        case ZYDIS_MNEMONIC_FADDP: popping = true; [[fallthrough]];
        case ZYDIS_MNEMONIC_FADD: op = "add"; break;
        case ZYDIS_MNEMONIC_FIADD: op = "add"; integer = true; break;
        case ZYDIS_MNEMONIC_FSUBP: popping = true; [[fallthrough]];
        case ZYDIS_MNEMONIC_FSUB: op = "sub"; break;
        case ZYDIS_MNEMONIC_FISUB: op = "sub"; integer = true; break;
        case ZYDIS_MNEMONIC_FSUBRP: popping = true; [[fallthrough]];
        case ZYDIS_MNEMONIC_FSUBR: op = "sub"; reverse = true; break;
        case ZYDIS_MNEMONIC_FISUBR: op = "sub"; reverse = true; integer = true; break;
        case ZYDIS_MNEMONIC_FMULP: popping = true; [[fallthrough]];
        case ZYDIS_MNEMONIC_FMUL: op = "mul"; break;
        case ZYDIS_MNEMONIC_FIMUL: op = "mul"; integer = true; break;
        case ZYDIS_MNEMONIC_FDIVP: popping = true; [[fallthrough]];
        case ZYDIS_MNEMONIC_FDIV: op = "div"; break;
        case ZYDIS_MNEMONIC_FIDIV: op = "div"; integer = true; break;
        case ZYDIS_MNEMONIC_FDIVRP: popping = true; [[fallthrough]];
        case ZYDIS_MNEMONIC_FDIVR: op = "div"; reverse = true; break;
        case ZYDIS_MNEMONIC_FIDIVR: op = "div"; reverse = true; integer = true; break;
        default: die("unsupported x87 instruction at %08x: %s", i.addr, i.text.c_str());
        }
        int dst = 0;
        std::string a, b;
        if (mem) {
            a = fmt("x87::get(%s, 0)", F);
            b = fp_source(o[0], integer);
        } else {
            int s = 0;
            if (n >= 2) dst = sti(o[0]), s = sti(o[1]);
            else if (popping) dst = 1, s = 0;  // faddp etc. without operands: st(1), st
            else die("unsupported x87 form at %08x: %s", i.addr, i.text.c_str());
            a = fmt("x87::get(%s, %d)", F, dst);
            b = fmt("x87::get(%s, %d)", F, s);
        }
        if (reverse) std::swap(a, b);
        line(fmt("x87::set(%s, %d, x87::%s(%s, %s, %s));", F, dst, op, F, a.c_str(), b.c_str()));
        if (popping) line(fmt("x87::pop(%s);", F));
    }



    // ---- string instructions ----
    // Element size from the mnemonic's last letter (b, w, d).
    static unsigned string_size(ZydisMnemonic m) {
        switch (m) {
        case ZYDIS_MNEMONIC_MOVSB: case ZYDIS_MNEMONIC_STOSB: case ZYDIS_MNEMONIC_LODSB:
        case ZYDIS_MNEMONIC_CMPSB: case ZYDIS_MNEMONIC_SCASB:
            return 8;
        case ZYDIS_MNEMONIC_MOVSW: case ZYDIS_MNEMONIC_STOSW: case ZYDIS_MNEMONIC_LODSW:
        case ZYDIS_MNEMONIC_CMPSW: case ZYDIS_MNEMONIC_SCASW:
            return 16;
        default:
            return 32;
        }
    }

    void string_op(const Insn& i, uint8_t want) {
        const auto m = i.in.mnemonic;
        const unsigned w = string_size(m);
        const char* T = ctype(w);
        const bool rep = i.in.attributes &
                         (ZYDIS_ATTRIB_HAS_REP | ZYDIS_ATTRIB_HAS_REPE | ZYDIS_ATTRIB_HAS_REPNE);
        const bool repe = i.in.attributes & ZYDIS_ATTRIB_HAS_REPE;
        const bool repne = i.in.attributes & ZYDIS_ATTRIB_HAS_REPNE;
        const unsigned bytes = w / 8;
        line(fmt("const uint32_t step = c.df ? 0u - %uu : %uu;", bytes, bytes));
        std::string body;
        bool compares = false;
        switch (m) {
        case ZYDIS_MNEMONIC_MOVSB: case ZYDIS_MNEMONIC_MOVSW: case ZYDIS_MNEMONIC_MOVSD:
            body = fmt("wr%u(M, c.edi, rd%u(M, c.esi)); c.esi += step; c.edi += step;", w, w);
            break;
        case ZYDIS_MNEMONIC_STOSB: case ZYDIS_MNEMONIC_STOSW: case ZYDIS_MNEMONIC_STOSD:
            body = fmt("wr%u(M, c.edi, (%s)c.eax); c.edi += step;", w, T);
            break;
        case ZYDIS_MNEMONIC_LODSB: case ZYDIS_MNEMONIC_LODSW: case ZYDIS_MNEMONIC_LODSD:
            body = w == 32 ? "c.eax = rd32(M, c.esi); c.esi += step;"
                 : fmt("c.eax = (c.eax & ~0x%xu) | rd%u(M, c.esi); c.esi += step;",
                       w == 8 ? 0xff : 0xffff, w);
            break;
        case ZYDIS_MNEMONIC_CMPSB: case ZYDIS_MNEMONIC_CMPSW: case ZYDIS_MNEMONIC_CMPSD:
            body = fmt("a = rd%u(M, c.esi); b = rd%u(M, c.edi); c.esi += step; c.edi += step;", w, w);
            compares = true;
            break;
        default:  // scas
            body = fmt("a = (%s)c.eax; b = rd%u(M, c.edi); c.edi += step;", T, w);
            compares = true;
            break;
        }
        if (compares) {
            line(fmt("%s a = 0, b = 0, r = 0;", T));
            body += fmt(" r = (%s)(a - b);", T);
        }
        if (!rep) {
            line(body);
        } else {
            // With ecx == 0 nothing runs and the flags keep their values.
            if (compares) line("bool ran = false;");
            line("while (c.ecx) {");
            line("    " + body);
            line("    c.ecx--;");
            if (compares) line("    ran = true;");
            if (repe && compares) line("    if (r != 0) break;");
            if (repne && compares) line("    if (r == 0) break;");
            line("}");
        }
        if (compares && (want & ALL)) {
            const char* in = rep ? "    " : "";
            if (rep) line("if (ran) {");
            if (want & CF) line(fmt("%scf = a < b;", in));
            if (want & OF) line(fmt("%sof = (((a ^ b) & (a ^ r)) >> %u) & 1;", in, w - 1));
            szp(w, want, in);
            if (rep) line("}");
        }
    }

    // ---- SSE ----

    static bool is_xmm(const ZydisDecodedOperand& op) {
        return op.type == ZYDIS_OPERAND_TYPE_REGISTER &&
               ZydisRegisterGetClass(op.reg.value) == ZYDIS_REGCLASS_XMM;
    }
    static std::string xmm(const ZydisDecodedOperand& op) {
        return fmt("c.xmm[%d]", op.reg.value - ZYDIS_REGISTER_XMM0);
    }

    // An xmm or memory source as an Xmm value; memory reads `bits` bits into
    // the low lane(s) (the rest zero; helpers only use the lanes they need).
    std::string xsrc(const ZydisDecodedOperand& op, unsigned bits) {
        if (is_xmm(op)) return xmm(op);
        if (op.type != ZYDIS_OPERAND_TYPE_MEMORY) die("unexpected SSE source operand");
        const std::string a = ea(op);
        switch (bits) {
        case 32: return fmt("sse::from32(rd32(M, %s))", a.c_str());
        case 64: return fmt("sse::from64(rd64(M, %s))", a.c_str());
        case 128: return fmt("sse::ld128(M, %s)", a.c_str());
        default: die("unsupported SSE source width %u", bits);
        }
    }


    static bool is_mmx(const ZydisDecodedOperand& op) {
        return op.type == ZYDIS_OPERAND_TYPE_REGISTER &&
               ZydisRegisterGetClass(op.reg.value) == ZYDIS_REGCLASS_MMX;
    }
    static int mmn(const ZydisDecodedOperand& op) { return op.reg.value - ZYDIS_REGISTER_MM0; }

    // A 64-bit MMX source: register or m64.
    std::string msrc(const ZydisDecodedOperand& op) {
        if (is_mmx(op)) return fmt("x87::mm(c.fpu, %d)", mmn(op));
        if (op.type == ZYDIS_OPERAND_TYPE_MEMORY) return fmt("rd64(M, %s)", ea(op).c_str());
        die("unexpected MMX source operand");
    }

    // MMX instructions (the game has a few, in Wwise's sample converters).
    void mmx(const Insn& i) {
        const auto m = i.in.mnemonic;
        const auto* o = i.ops;
        auto set = [&](const std::string& v) {
            line(fmt("x87::set_mm(c.fpu, %d, %s);", mmn(o[0]), v.c_str()));
        };
        auto bin = [&](const char* fn) {
            set(fmt("sse::mmx(sse::%s, x87::mm(c.fpu, %d), %s)", fn, mmn(o[0]), msrc(o[1]).c_str()));
        };
        auto shift = [&](const char* fn) {
            const std::string n = o[1].type == ZYDIS_OPERAND_TYPE_IMMEDIATE
                                      ? fmt("%uu", (unsigned)(o[1].imm.value.u & 0xff))
                                      : msrc(o[1]);
            set(fmt("sse::mmx_shift(sse::%s, x87::mm(c.fpu, %d), %s)", fn, mmn(o[0]), n.c_str()));
        };
        auto unpack = [&](const char* kind, const char* low) {
            set(fmt("sse::mmx_%s(sse::%s, x87::mm(c.fpu, %d), %s)", kind, low, mmn(o[0]),
                    msrc(o[1]).c_str()));
        };
        switch (m) {
        case ZYDIS_MNEMONIC_MOVQ:
            if (is_mmx(o[0])) set(msrc(o[1]));
            else line(fmt("wr64(M, %s, x87::mm(c.fpu, %d));", ea(o[0]).c_str(), mmn(o[1])));
            return;
        case ZYDIS_MNEMONIC_MOVD:
            if (is_mmx(o[0])) set(fmt("(uint64_t)%s", read(o[1], 32).c_str()));
            else line(write(o[0], fmt("(uint32_t)x87::mm(c.fpu, %d)", mmn(o[1]))));
            return;
        case ZYDIS_MNEMONIC_CVTPI2PS:
            line(fmt("%s = sse::cvtpi2ps(%s, %s);", xmm(o[0]).c_str(), xmm(o[0]).c_str(),
                     msrc(o[1]).c_str()));
            return;
        case ZYDIS_MNEMONIC_PADDSW: bin("paddsw"); return;
        case ZYDIS_MNEMONIC_PSUBSW: bin("psubsw"); return;
        case ZYDIS_MNEMONIC_PADDUSW: bin("paddusw"); return;
        case ZYDIS_MNEMONIC_PSUBUSW: bin("psubusw"); return;
        case ZYDIS_MNEMONIC_PSUBW: bin("psubw"); return;
        case ZYDIS_MNEMONIC_PSUBD: bin("psubd"); return;
        case ZYDIS_MNEMONIC_PADDB: bin("paddb"); return;
        case ZYDIS_MNEMONIC_PSUBB: bin("psubb"); return;
        case ZYDIS_MNEMONIC_PMULLW: bin("pmullw"); return;
        case ZYDIS_MNEMONIC_PMULHW: bin("pmulhw"); return;
        case ZYDIS_MNEMONIC_PMADDWD: bin("pmaddwd"); return;
        case ZYDIS_MNEMONIC_PMULUDQ: bin("pmuludq"); return;
        case ZYDIS_MNEMONIC_PCMPEQW: bin("pcmpeqw"); return;
        case ZYDIS_MNEMONIC_PCMPEQB: bin("pcmpeqb"); return;
        case ZYDIS_MNEMONIC_PCMPEQD: bin("pcmpeqd"); return;
        case ZYDIS_MNEMONIC_PMINSW: bin("pminsw"); return;
        case ZYDIS_MNEMONIC_PMAXSW: bin("pmaxsw"); return;
        case ZYDIS_MNEMONIC_PACKSSDW:
        case ZYDIS_MNEMONIC_PACKSSWB:
        case ZYDIS_MNEMONIC_PACKUSWB:
            set(fmt("sse::mmx_pack(sse::%s, x87::mm(c.fpu, %d), %s)",
                    m == ZYDIS_MNEMONIC_PACKSSDW ? "packssdw" : m == ZYDIS_MNEMONIC_PACKSSWB ? "packsswb" : "packuswb",
                    mmn(o[0]), msrc(o[1]).c_str()));
            return;
        case ZYDIS_MNEMONIC_PSHUFW:
            set(fmt("sse::pshufw(%s, %d)", msrc(o[1]).c_str(), (int)(o[2].imm.value.u & 0xff)));
            return;
        case ZYDIS_MNEMONIC_PEXTRW:
            line(write(o[0], fmt("(uint32_t)(x87::mm(c.fpu, %d) >> %d & 0xffff)", mmn(o[1]),
                                 16 * (int)(o[2].imm.value.u & 3))));
            return;
        case ZYDIS_MNEMONIC_PINSRW: {
            const int sh = 16 * (int)(o[2].imm.value.u & 3);
            set(fmt("(x87::mm(c.fpu, %d) & ~(0xffffull << %d)) | (uint64_t)(uint16_t)%s << %d", mmn(o[0]), sh,
                    read(o[1], 16).c_str(), sh));
            return;
        }
        case ZYDIS_MNEMONIC_PMOVMSKB:
            line(write(o[0], fmt("sse::pmovmskb(sse::from64(x87::mm(c.fpu, %d)), 8)", mmn(o[1]))));
            return;
        case ZYDIS_MNEMONIC_CVTTPS2PI:
        case ZYDIS_MNEMONIC_CVTPS2PI:
            set(fmt("sse::%s(%s)", m == ZYDIS_MNEMONIC_CVTTPS2PI ? "cvttps2pi" : "cvtps2pi",
                    xsrc(o[1], 64).c_str()));
            return;
        case ZYDIS_MNEMONIC_PAND: bin("pand"); return;
        case ZYDIS_MNEMONIC_PANDN: bin("pandn"); return;
        case ZYDIS_MNEMONIC_POR: bin("por"); return;
        case ZYDIS_MNEMONIC_PXOR: bin("pxor"); return;
        case ZYDIS_MNEMONIC_PADDD: bin("paddd"); return;
        case ZYDIS_MNEMONIC_PADDW: bin("paddw"); return;
        case ZYDIS_MNEMONIC_PCMPGTW: bin("pcmpgtw"); return;
        case ZYDIS_MNEMONIC_PCMPGTD: bin("pcmpgtd"); return;
        case ZYDIS_MNEMONIC_PSLLD: shift("pslld"); return;
        case ZYDIS_MNEMONIC_PSRLD: shift("psrld"); return;
        case ZYDIS_MNEMONIC_PSRAD: shift("psrad"); return;
        case ZYDIS_MNEMONIC_PSLLW: shift("psllw"); return;
        case ZYDIS_MNEMONIC_PSRLW: shift("psrlw"); return;
        case ZYDIS_MNEMONIC_PSRAW: shift("psraw"); return;
        case ZYDIS_MNEMONIC_PSLLQ: shift("psllq"); return;
        case ZYDIS_MNEMONIC_PSRLQ: shift("psrlq"); return;
        case ZYDIS_MNEMONIC_PUNPCKLBW: unpack("punpckl", "punpcklbw"); return;
        case ZYDIS_MNEMONIC_PUNPCKLWD: unpack("punpckl", "punpcklwd"); return;
        case ZYDIS_MNEMONIC_PUNPCKLDQ: unpack("punpckl", "punpckldq"); return;
        case ZYDIS_MNEMONIC_PUNPCKHBW: unpack("punpckh", "punpcklbw"); return;
        case ZYDIS_MNEMONIC_PUNPCKHWD: unpack("punpckh", "punpcklwd"); return;
        case ZYDIS_MNEMONIC_PUNPCKHDQ: unpack("punpckh", "punpckldq"); return;
        default:
            die("unsupported MMX instruction at %08x: %s", i.addr, i.text.c_str());
        }
    }

    // Returns false if this is not an SSE form (string movsd/cmpsd).
    bool sse(const Insn& i, uint8_t want) {
        const auto m = i.in.mnemonic;
        const auto* o = i.ops;
        const unsigned n = i.in.operand_count_visible;
        if (m == ZYDIS_MNEMONIC_LDMXCSR) {
            line(fmt("sse::ldmxcsr(c, rd32(M, %s));", ea(o[0]).c_str()));
            return true;
        }
        if (m == ZYDIS_MNEMONIC_STMXCSR) {
            line(fmt("wr32(M, %s, c.mxcsr);", ea(o[0]).c_str()));
            return true;
        }
        bool any_xmm = false;
        for (unsigned k = 0; k < n; k++) any_xmm |= is_xmm(o[k]) || is_mmx(o[k]);
        if (!any_xmm && (m == ZYDIS_MNEMONIC_MOVSD || m == ZYDIS_MNEMONIC_CMPSD))
            return false;  // the string instructions of the same name
        for (unsigned k = 0; k < n; k++)
            if (is_mmx(o[k])) return mmx(i), true;
        auto binary = [&](const char* fn, unsigned bits) {
            line(fmt("%s = sse::%s(%s, %s);", xmm(o[0]).c_str(), fn, xmm(o[0]).c_str(),
                     xsrc(o[1], bits).c_str()));
        };
        auto with_imm = [&](const char* fn, unsigned bits) {
            line(fmt("%s = sse::%s(%s, %s, %d);", xmm(o[0]).c_str(), fn, xmm(o[0]).c_str(),
                     xsrc(o[1], bits).c_str(), (int)(o[2].imm.value.u & 0xff)));
        };
        auto shift = [&](const char* fn) {
            if (o[1].type == ZYDIS_OPERAND_TYPE_IMMEDIATE)
                line(fmt("%s = sse::%s(%s, %u);", xmm(o[0]).c_str(), fn, xmm(o[0]).c_str(),
                         (unsigned)(o[1].imm.value.u & 0xff)));
            else
                line(fmt("%s = sse::%s(%s, sse::u64(%s, 0));", xmm(o[0]).c_str(), fn,
                         xmm(o[0]).c_str(), xsrc(o[1], 128).c_str()));
        };
        switch (m) {
#define B(MN, FN, BITS) case ZYDIS_MNEMONIC_##MN: binary(FN, BITS); return true;
        B(ADDSS, "addss", 32) B(SUBSS, "subss", 32) B(MULSS, "mulss", 32) B(DIVSS, "divss", 32)
        B(MINSS, "minss", 32) B(MAXSS, "maxss", 32) B(SQRTSS, "sqrtss", 32)
        B(RCPSS, "rcpss", 32) B(RSQRTSS, "rsqrtss", 32) B(CVTSS2SD, "cvtss2sd", 32)
        B(ADDSD, "addsd", 64) B(SUBSD, "subsd", 64) B(MULSD, "mulsd", 64) B(DIVSD, "divsd", 64)
        B(MINSD, "minsd", 64) B(MAXSD, "maxsd", 64) B(SQRTSD, "sqrtsd", 64)
        B(CVTSD2SS, "cvtsd2ss", 64)
        B(ADDPS, "addps", 128) B(SUBPS, "subps", 128) B(MULPS, "mulps", 128)
        B(DIVPS, "divps", 128) B(MINPS, "minps", 128) B(MAXPS, "maxps", 128)
        B(SQRTPS, "sqrtps", 128) B(RCPPS, "rcpps", 128) B(RSQRTPS, "rsqrtps", 128)
        B(ADDPD, "addpd", 128) B(SUBPD, "subpd", 128) B(MULPD, "mulpd", 128)
        B(DIVPD, "divpd", 128) B(MINPD, "minpd", 128) B(MAXPD, "maxpd", 128)
        B(SQRTPD, "sqrtpd", 128) B(HADDPS, "haddps", 128) B(HADDPD, "haddpd", 128)
        B(ANDPS, "andps", 128) B(ANDNPS, "andnps", 128) B(ORPS, "orps", 128)
        B(XORPS, "xorps", 128) B(ANDPD, "andps", 128) B(ANDNPD, "andnps", 128)
        B(ORPD, "orps", 128) B(XORPD, "xorps", 128)
        B(UNPCKLPS, "unpcklps", 128) B(UNPCKHPS, "unpckhps", 128)
        B(UNPCKLPD, "unpcklpd", 128) B(UNPCKHPD, "unpckhpd", 128)
        B(MOVHLPS, "movhlps", 128) B(MOVLHPS, "movlhps", 128)
        B(CVTDQ2PS, "cvtdq2ps", 128) B(CVTDQ2PD, "cvtdq2pd", 64)
        B(CVTTPS2DQ, "cvttps2dq", 128) B(CVTPS2DQ, "cvtps2dq", 128)
        B(CVTTPD2DQ, "cvttpd2dq", 128) B(CVTPS2PD, "cvtps2pd", 64) B(CVTPD2PS, "cvtpd2ps", 128)
        B(PADDD, "paddd", 128) B(PSUBD, "psubd", 128) B(PADDQ, "paddq", 128)
        B(PSUBQ, "psubq", 128) B(PADDW, "paddw", 128) B(PSUBW, "psubw", 128)
        B(PAND, "pand", 128) B(PANDN, "pandn", 128) B(POR, "por", 128) B(PXOR, "pxor", 128)
        B(PCMPEQD, "pcmpeqd", 128) B(PCMPGTD, "pcmpgtd", 128) B(PCMPGTW, "pcmpgtw", 128)
        B(PUNPCKLBW, "punpcklbw", 128) B(PUNPCKHBW, "punpckhbw", 128)
        B(PUNPCKLWD, "punpcklwd", 128) B(PUNPCKHWD, "punpckhwd", 128)
        B(PUNPCKLDQ, "punpckldq", 128) B(PUNPCKHDQ, "punpckhdq", 128)
        B(PUNPCKLQDQ, "punpcklqdq", 128) B(PUNPCKHQDQ, "punpckhqdq", 128)
        B(PACKSSDW, "packssdw", 128) B(PACKSSWB, "packsswb", 128) B(PACKUSWB, "packuswb", 128)
        B(PADDSW, "paddsw", 128) B(PSUBSW, "psubsw", 128) B(PADDUSW, "paddusw", 128)
        B(PSUBUSW, "psubusw", 128) B(PADDB, "paddb", 128) B(PSUBB, "psubb", 128)
        B(PMULLW, "pmullw", 128) B(PMULHW, "pmulhw", 128) B(PMULHUW, "pmulhuw", 128)
        B(PMADDWD, "pmaddwd", 128) B(PMULUDQ, "pmuludq", 128) B(PCMPEQW, "pcmpeqw", 128)
        B(PCMPEQB, "pcmpeqb", 128) B(PMINSW, "pminsw", 128) B(PMAXSW, "pmaxsw", 128)
#undef B
        case ZYDIS_MNEMONIC_PSHUFLW: with_imm("pshuflw", 128); return true;
        case ZYDIS_MNEMONIC_PSHUFHW: with_imm("pshufhw", 128); return true;
        case ZYDIS_MNEMONIC_PMOVMSKB:
            line(write(o[0], fmt("sse::pmovmskb(%s, 16)", xmm(o[1]).c_str())));
            return true;
        case ZYDIS_MNEMONIC_PEXTRW:
            line(write(o[0], fmt("(uint32_t)sse::u16(%s, %d)", xmm(o[1]).c_str(), (int)(o[2].imm.value.u & 7))));
            return true;
        case ZYDIS_MNEMONIC_PINSRW:
            line(fmt("sse::set16(%s, %d, (uint16_t)%s);", xmm(o[0]).c_str(), (int)(o[2].imm.value.u & 7),
                     read(o[1], 16).c_str()));
            return true;
        case ZYDIS_MNEMONIC_SHUFPS: with_imm("shufps", 128); return true;
        case ZYDIS_MNEMONIC_SHUFPD: with_imm("shufpd", 128); return true;
        case ZYDIS_MNEMONIC_PSHUFD: with_imm("pshufd", 128); return true;
        case ZYDIS_MNEMONIC_CMPPS: with_imm("cmpps", 128); return true;
        case ZYDIS_MNEMONIC_CMPPD: with_imm("cmppd", 128); return true;
        case ZYDIS_MNEMONIC_CMPSS: with_imm("cmpss", 32); return true;
        case ZYDIS_MNEMONIC_CMPSD: with_imm("cmpsd", 64); return true;
        case ZYDIS_MNEMONIC_PSLLD: shift("pslld"); return true;
        case ZYDIS_MNEMONIC_PSRLD: shift("psrld"); return true;
        case ZYDIS_MNEMONIC_PSRAD: shift("psrad"); return true;
        case ZYDIS_MNEMONIC_PSLLQ: shift("psllq"); return true;
        case ZYDIS_MNEMONIC_PSRLQ: shift("psrlq"); return true;
        case ZYDIS_MNEMONIC_PSLLW: shift("psllw"); return true;
        case ZYDIS_MNEMONIC_PSRLW: shift("psrlw"); return true;
        case ZYDIS_MNEMONIC_PSRAW: shift("psraw"); return true;
        case ZYDIS_MNEMONIC_PSLLDQ:
            line(fmt("%s = sse::pslldq(%s, %u);", xmm(o[0]).c_str(), xmm(o[0]).c_str(),
                     (unsigned)(o[1].imm.value.u & 0xff)));
            return true;
        case ZYDIS_MNEMONIC_PSRLDQ:
            line(fmt("%s = sse::psrldq(%s, %u);", xmm(o[0]).c_str(), xmm(o[0]).c_str(),
                     (unsigned)(o[1].imm.value.u & 0xff)));
            return true;
        case ZYDIS_MNEMONIC_UCOMISS:
        case ZYDIS_MNEMONIC_COMISS:
        case ZYDIS_MNEMONIC_UCOMISD:
        case ZYDIS_MNEMONIC_COMISD: {
            const bool dbl = m == ZYDIS_MNEMONIC_UCOMISD || m == ZYDIS_MNEMONIC_COMISD;
            if (!(want & ALL)) return true;
            line(fmt("sse::Flags f = sse::ucomi(sse::%s(%s, 0), sse::%s(%s, 0));",
                     dbl ? "f64" : "f32", xmm(o[0]).c_str(), dbl ? "f64" : "f32",
                     xsrc(o[1], dbl ? 64 : 32).c_str()));
            if (want & ZF) line("zf = f.zf;");
            if (want & PF) line("pf = f.pf;");
            if (want & CF) line("cf = f.cf;");
            if (want & OF) line("of = false;");
            if (want & SF) line("sf = false;");
            return true;
        }
        case ZYDIS_MNEMONIC_CVTSI2SS:
        case ZYDIS_MNEMONIC_CVTSI2SD:
            line(fmt("%s = sse::%s(%s, (int32_t)%s);", xmm(o[0]).c_str(),
                     m == ZYDIS_MNEMONIC_CVTSI2SS ? "cvtsi2ss" : "cvtsi2sd", xmm(o[0]).c_str(),
                     read(o[1], 32).c_str()));
            return true;
        case ZYDIS_MNEMONIC_CVTTSS2SI:
        case ZYDIS_MNEMONIC_CVTSS2SI:
        case ZYDIS_MNEMONIC_CVTTSD2SI:
        case ZYDIS_MNEMONIC_CVTSD2SI: {
            const bool dbl = m == ZYDIS_MNEMONIC_CVTTSD2SI || m == ZYDIS_MNEMONIC_CVTSD2SI;
            const bool trunc = m == ZYDIS_MNEMONIC_CVTTSS2SI || m == ZYDIS_MNEMONIC_CVTTSD2SI;
            line(write(o[0], fmt("(uint32_t)sse::%s(sse::%s(%s, 0))", trunc ? "cvtt_i32" : "cvt_i32",
                                 dbl ? "f64" : "f32", xsrc(o[1], dbl ? 64 : 32).c_str())));
            return true;
        }
        case ZYDIS_MNEMONIC_MOVMSKPS:
        case ZYDIS_MNEMONIC_MOVMSKPD:
            line(write(o[0], fmt("sse::%s(%s)", m == ZYDIS_MNEMONIC_MOVMSKPS ? "movmskps" : "movmskpd",
                                 xmm(o[1]).c_str())));
            return true;
        case ZYDIS_MNEMONIC_MOVAPS: case ZYDIS_MNEMONIC_MOVUPS: case ZYDIS_MNEMONIC_MOVAPD:
        case ZYDIS_MNEMONIC_MOVUPD: case ZYDIS_MNEMONIC_MOVDQA: case ZYDIS_MNEMONIC_MOVDQU:
        case ZYDIS_MNEMONIC_MOVNTPS: case ZYDIS_MNEMONIC_MOVNTDQ:
            if (is_xmm(o[0])) line(fmt("%s = %s;", xmm(o[0]).c_str(), xsrc(o[1], 128).c_str()));
            else line(fmt("sse::st128(M, %s, %s);", ea(o[0]).c_str(), xmm(o[1]).c_str()));
            return true;
        case ZYDIS_MNEMONIC_MOVSS:
        case ZYDIS_MNEMONIC_MOVSD: {
            const bool dbl = m == ZYDIS_MNEMONIC_MOVSD;
            const char* lane = dbl ? "64" : "32";
            if (is_xmm(o[0]) && is_xmm(o[1]))  // register form merges the low lane
                line(fmt("sse::set%s(%s, 0, sse::u%s(%s, 0));", lane, xmm(o[0]).c_str(), lane,
                         xmm(o[1]).c_str()));
            else if (is_xmm(o[0]))  // load zeroes the rest
                line(fmt("%s = %s;", xmm(o[0]).c_str(), xsrc(o[1], dbl ? 64 : 32).c_str()));
            else
                line(fmt("wr%s(M, %s, sse::u%s(%s, 0));", lane, ea(o[0]).c_str(), lane,
                         xmm(o[1]).c_str()));
            return true;
        }
        case ZYDIS_MNEMONIC_MOVD:
            if (is_xmm(o[0])) line(fmt("%s = sse::from32(%s);", xmm(o[0]).c_str(), read(o[1], 32).c_str()));
            else line(write(o[0], fmt("sse::u32(%s, 0)", xmm(o[1]).c_str())));
            return true;
        case ZYDIS_MNEMONIC_MOVQ:
            if (is_xmm(o[0]))
                line(fmt("%s = sse::from64(sse::u64(%s, 0));", xmm(o[0]).c_str(),
                         xsrc(o[1], 64).c_str()));
            else line(fmt("wr64(M, %s, sse::u64(%s, 0));", ea(o[0]).c_str(), xmm(o[1]).c_str()));
            return true;
        case ZYDIS_MNEMONIC_MOVHPS:
        case ZYDIS_MNEMONIC_MOVHPD:
            if (is_xmm(o[0])) line(fmt("sse::set64(%s, 1, rd64(M, %s));", xmm(o[0]).c_str(), ea(o[1]).c_str()));
            else line(fmt("wr64(M, %s, sse::u64(%s, 1));", ea(o[0]).c_str(), xmm(o[1]).c_str()));
            return true;
        case ZYDIS_MNEMONIC_MOVLPS:
        case ZYDIS_MNEMONIC_MOVLPD:
            if (is_xmm(o[0])) line(fmt("sse::set64(%s, 0, rd64(M, %s));", xmm(o[0]).c_str(), ea(o[1]).c_str()));
            else line(fmt("wr64(M, %s, sse::u64(%s, 0));", ea(o[0]).c_str(), xmm(o[1]).c_str()));
            return true;
        default:
            die("unsupported SSE instruction at %08x: %s", i.addr, i.text.c_str());
        }
    }

    void insn(const Insn& i) {
        want_ = (i.def | i.may_def) & i.live_out;
        if (is_x87(i.in.mnemonic)) return x87(i);
        if (is_sse(i.in.mnemonic) && sse(i, (i.def | i.may_def) & i.live_out)) return;
        switch (i.in.mnemonic) {
        case ZYDIS_MNEMONIC_MOVSB: case ZYDIS_MNEMONIC_MOVSW: case ZYDIS_MNEMONIC_MOVSD:
        case ZYDIS_MNEMONIC_STOSB: case ZYDIS_MNEMONIC_STOSW: case ZYDIS_MNEMONIC_STOSD:
        case ZYDIS_MNEMONIC_LODSB: case ZYDIS_MNEMONIC_LODSW: case ZYDIS_MNEMONIC_LODSD:
        case ZYDIS_MNEMONIC_CMPSB: case ZYDIS_MNEMONIC_CMPSW: case ZYDIS_MNEMONIC_CMPSD:
        case ZYDIS_MNEMONIC_SCASB: case ZYDIS_MNEMONIC_SCASW: case ZYDIS_MNEMONIC_SCASD:
            return string_op(i, (i.def | i.may_def) & i.live_out);
        default:
            break;
        }
        const auto m = i.in.mnemonic;
        const auto* o = i.ops;
        const unsigned n = i.in.operand_count_visible;
        const uint8_t want = (i.def | i.may_def) & i.live_out;
        const unsigned w = n ? o[0].size : 32;
        const char* T = n && o[0].size ? ctype(o[0].size) : "uint32_t";
        Cond cond;

        switch (i.xfer) {
        case Xfer::CallNext:
            push_return(i);
            return;
        case Xfer::Call:
        case Xfer::CallImport:
            push_return(i);
            line(callee(i.target, i.xfer == Xfer::CallImport));
            return;
        case Xfer::CallIndirect:
            line(fmt("uint32_t t = %s;", read(o[0], 32).c_str()));
            push_return(i);
            line("call_indirect(c, t);");
            return;
        case Xfer::TailCall:
        case Xfer::TailImport:
            if (i.xfer == Xfer::TailCall) store_flags();
            line(callee(i.target, i.xfer == Xfer::TailImport));
            line("return;");
            return;
        case Xfer::JumpTable: {
            // The target is computed as the original code does, then matched
            // against the table's labels.
            line(fmt("uint32_t t = %s;", read(o[0], 32).c_str()));
            line("switch (t) {");
            for (uint32_t t : i.cases) {
                if (fn_->insns.count(t)) {
                    line(fmt("case 0x%08xu: goto L_%08x;", t, t));
                } else if (image_ && image_->is_pe() && image_->function_at(t)) {
                    // PE: an entry at another function's start runs it.
                    line(fmt("case 0x%08xu:", t));
                    store_flags();
                    line(callee(t, false));
                    line("return;");
                }
            }
            line(fmt("default: fatal(\"jump table at %08x: target %%08x is not in the table\", t);",
                     i.addr));
            line("}");
            return;
        }
        case Xfer::JumpIndirect:
            // Treated as a tail call through a pointer (e.g. a virtual call as the
            // last action). A target that is not a known function, such as a
            // switch-table label, stops in call_indirect with its address.
            line(fmt("uint32_t t = %s;", read(o[0], 32).c_str()));
            line("call_indirect(c, t);");
            line("return;");
            return;
        default:
            break;
        }

        switch (m) {
        case ZYDIS_MNEMONIC_NOP:
            return;
        case ZYDIS_MNEMONIC_MOV:
            line(write(o[0], read(o[1], w)));
            return;
        case ZYDIS_MNEMONIC_MOVZX:
            line(write(o[0], fmt("(%s)%s", T, read(o[1]).c_str())));
            return;
        case ZYDIS_MNEMONIC_MOVSX: {
            const char* S = o[1].size == 8 ? "int8_t" : "int16_t";
            line(write(o[0], fmt("(%s)(int32_t)(%s)%s", T, S, read(o[1]).c_str())));
            return;
        }
        case ZYDIS_MNEMONIC_LEA:
            line(write(o[0], ea(o[1])));
            return;
        case ZYDIS_MNEMONIC_PUSH:
            line(fmt("uint32_t v = %s;", read(o[0], 32).c_str()));
            line("c.esp -= 4;");
            line("wr32(M, c.esp, v);");
            return;
        case ZYDIS_MNEMONIC_POP:
            if (o[0].type == ZYDIS_OPERAND_TYPE_REGISTER &&
                ZydisRegisterGetClass(o[0].reg.value) == ZYDIS_REGCLASS_SEGMENT)
                die("pop to a segment register at %08x", i.addr);
            // A memory destination is addressed with esp already incremented.
            line("uint32_t v = rd32(M, c.esp);");
            line("c.esp += 4;");
            line(write(o[0], "v"));
            return;
        case ZYDIS_MNEMONIC_XLAT:
            line("c.eax = (c.eax & ~0xffu) | rd8(M, c.ebx + (c.eax & 0xffu));");
            return;
        case ZYDIS_MNEMONIC_LEAVE:
            line("c.esp = c.ebp;");
            line("c.ebp = rd32(M, c.esp);");
            line("c.esp += 4;");
            return;
        case ZYDIS_MNEMONIC_RET:
            line(fmt("c.esp += %u;", 4 + (n ? (unsigned)o[0].imm.value.u : 0)));
            line("return;");
            return;
        case ZYDIS_MNEMONIC_JMP:
            line(fmt("goto L_%08x;", i.target));
            return;
        case ZYDIS_MNEMONIC_NOT:
            line(write(o[0], fmt("(%s)~%s", T, read(o[0]).c_str())));
            return;
        case ZYDIS_MNEMONIC_ADD:
        case ZYDIS_MNEMONIC_ADC:
        case ZYDIS_MNEMONIC_SUB:
        case ZYDIS_MNEMONIC_SBB:
        case ZYDIS_MNEMONIC_CMP: {
            const bool add = m == ZYDIS_MNEMONIC_ADD || m == ZYDIS_MNEMONIC_ADC;
            const bool carry_in = m == ZYDIS_MNEMONIC_ADC || m == ZYDIS_MNEMONIC_SBB;
            const bool locked = i.in.attributes & ZYDIS_ATTRIB_HAS_LOCK;
            if (locked && (carry_in || o[0].type != ZYDIS_OPERAND_TYPE_MEMORY))
                die("unsupported locked form at %08x: %s", i.addr, i.text.c_str());
            line(fmt("%s b = %s;", T, read(o[1], w).c_str()));
            if (locked)
                line(fmt("%s a = atomic_rmw<%s>(M, %s, b, %s);", T, T, ea(o[0]).c_str(),
                         add ? "kAtomicAdd" : "kAtomicSub"));
            else
                line(fmt("%s a = %s;", T, read(o[0]).c_str()));
            line(fmt("uint32_t ci = %s;", carry_in ? "cf" : "0"));
            line(fmt("%s r = (%s)(a %c b %c ci);", T, T, add ? '+' : '-', add ? '+' : '-'));
            if (m != ZYDIS_MNEMONIC_CMP && !locked) line(write(o[0], "r"));
            if (want & CF)
                line(add ? fmt("cf = (((uint64_t)a + b + ci) >> %u) & 1;", w)
                         : "cf = (uint64_t)a < (uint64_t)b + ci;");
            if (want & OF)
                line(add ? fmt("of = (((a ^ r) & (b ^ r)) >> %u) & 1;", w - 1)
                         : fmt("of = (((a ^ b) & (a ^ r)) >> %u) & 1;", w - 1));
            szp(w, want);
            return;
        }
        case ZYDIS_MNEMONIC_AND:
        case ZYDIS_MNEMONIC_OR:
        case ZYDIS_MNEMONIC_XOR:
        case ZYDIS_MNEMONIC_TEST: {
            const char op = m == ZYDIS_MNEMONIC_OR ? '|' : m == ZYDIS_MNEMONIC_XOR ? '^' : '&';
            if (i.in.attributes & ZYDIS_ATTRIB_HAS_LOCK) {
                if (o[0].type != ZYDIS_OPERAND_TYPE_MEMORY) die("locked form at %08x", i.addr);
                line(fmt("%s b = %s;", T, read(o[1], w).c_str()));
                line(fmt("%s a = atomic_rmw<%s>(M, %s, b, %s);", T, T, ea(o[0]).c_str(),
                         op == '|' ? "kAtomicOr" : op == '^' ? "kAtomicXor" : "kAtomicAnd"));
                line(fmt("%s r = (%s)(a %c b);", T, T, op));
            } else {
                line(fmt("%s r = (%s)(%s %c %s);", T, T, read(o[0]).c_str(), op,
                         read(o[1], w).c_str()));
                if (m != ZYDIS_MNEMONIC_TEST) line(write(o[0], "r"));
            }
            if (want & CF) line("cf = false;");
            if (want & OF) line("of = false;");
            szp(w, want);
            return;
        }
        case ZYDIS_MNEMONIC_INC:
        case ZYDIS_MNEMONIC_DEC: {
            const bool inc = m == ZYDIS_MNEMONIC_INC;
            if (i.in.attributes & ZYDIS_ATTRIB_HAS_LOCK) {
                if (o[0].type != ZYDIS_OPERAND_TYPE_MEMORY) die("locked form at %08x", i.addr);
                line(fmt("%s a = atomic_rmw<%s>(M, %s, (%s)1, %s);", T, T, ea(o[0]).c_str(), T,
                         inc ? "kAtomicAdd" : "kAtomicSub"));
                line(fmt("%s r = (%s)(a %c 1);", T, T, inc ? '+' : '-'));
            } else {
                line(fmt("%s a = %s;", T, read(o[0]).c_str()));
                line(fmt("%s r = (%s)(a %c 1);", T, T, inc ? '+' : '-'));
                line(write(o[0], "r"));
            }
            if (want & OF)
                line(fmt("of = %s == (%s)0x%xu;", inc ? "r" : "a", T, 1u << (w - 1)));
            szp(w, want);
            return;
        }
        case ZYDIS_MNEMONIC_NEG:
            line(fmt("%s a = %s;", T, read(o[0]).c_str()));
            line(fmt("%s r = (%s)(0u - a);", T, T));
            line(write(o[0], "r"));
            if (want & CF) line("cf = a != 0;");
            if (want & OF) line(fmt("of = a == (%s)0x%xu;", T, 1u << (w - 1)));
            szp(w, want);
            return;
        case ZYDIS_MNEMONIC_MUL:
        case ZYDIS_MNEMONIC_IMUL: {
            const bool sgn = m == ZYDIS_MNEMONIC_IMUL;
            if (n == 1 && w != 32) {
                // ax = al * src8, or dx:ax = ax * src16.
                const char* U = w == 8 ? "uint8_t" : "uint16_t";
                const char* S = w == 8 ? "int8_t" : "int16_t";
                if (sgn) line(fmt("int32_t p = (int32_t)(%s)c.eax * (int32_t)(%s)%s;", S, S, read(o[0]).c_str()));
                else line(fmt("int32_t p = (int32_t)((uint32_t)(%s)c.eax * (uint32_t)%s);", U, read(o[0]).c_str()));
                if (w == 8) {
                    line("c.eax = (c.eax & ~0xffffu) | ((uint32_t)p & 0xffffu);");
                    if (want & (CF | OF))
                        line(sgn ? "cf = of = p != (int32_t)(int8_t)p;" : "cf = of = ((uint32_t)p >> 8 & 0xff) != 0;");
                } else {
                    line("c.eax = (c.eax & ~0xffffu) | ((uint32_t)p & 0xffffu);");
                    line("c.edx = (c.edx & ~0xffffu) | ((uint32_t)p >> 16 & 0xffffu);");
                    if (want & (CF | OF))
                        line(sgn ? "cf = of = p != (int32_t)(int16_t)p;" : "cf = of = ((uint32_t)p >> 16) != 0;");
                }
                if (want & (ZF | SF | PF)) {
                    line(fmt("%s r = (%s)p;", U, U));
                    szp(w, want);
                }
                return;
            }
            if (n == 1) {
                // edx:eax = eax * src.
                if (sgn) line(fmt("int64_t p = (int64_t)(int32_t)c.eax * (int32_t)%s;",
                                  read(o[0]).c_str()));
                else line(fmt("uint64_t p = (uint64_t)c.eax * %s;", read(o[0]).c_str()));
                line("c.eax = (uint32_t)p;");
                line("c.edx = (uint32_t)((uint64_t)p >> 32);");
                if (want & (CF | OF))
                    line(sgn ? "cf = of = p != (int64_t)(int32_t)p;" : "cf = of = c.edx != 0;");
                if (want & (ZF | SF | PF)) {
                    line("uint32_t r = c.eax;");
                    szp(32, want);
                }
                return;
            }
            if (!sgn) die("multi-operand mul at %08x", i.addr);
            // imul dst, src  /  imul dst, src, imm: truncated signed product.
            const char* S = w == 16 ? "int16_t" : "int32_t";
            const std::string x = n == 2 ? read(o[0]) : read(o[1]);
            const std::string y = n == 2 ? read(o[1], w) : read(o[2], w);
            line(fmt("int64_t p = (int64_t)(%s)%s * (int64_t)(%s)%s;", S, x.c_str(), S, y.c_str()));
            line(fmt("%s r = (%s)p;", T, T));
            line(write(o[0], "r"));
            if (want & (CF | OF)) line(fmt("cf = of = p != (int64_t)(%s)r;", S));
            szp(w, want);
            return;
        }
        case ZYDIS_MNEMONIC_DIV:
        case ZYDIS_MNEMONIC_IDIV: {
            // edx:eax / src -> quotient in eax, remainder in edx (ax / src8 ->
            // al, ah; dx:ax / src16 -> ax, dx). x86 raises #DE on a zero divisor
            // or an overflowing quotient.
            if (w != 32) {
                const bool sg = m == ZYDIS_MNEMONIC_IDIV;
                line(fmt("int32_t d = (int32_t)(%s)%s;", sg ? (w == 8 ? "int8_t" : "int16_t")
                                                             : (w == 8 ? "uint8_t" : "uint16_t"),
                         read(o[0]).c_str()));
                line(fmt("if (d == 0) fatal(\"division by zero at %08x\");", i.addr));
                if (w == 8)
                    line(sg ? "int32_t n = (int16_t)c.eax;" : "int32_t n = (int32_t)(c.eax & 0xffffu);");
                else
                    line(sg ? "int32_t n = (int32_t)((c.edx & 0xffffu) << 16 | (c.eax & 0xffffu));"
                            : "int64_t n = (int64_t)((c.edx & 0xffffu) << 16 | (c.eax & 0xffffu));");
                line("int64_t q = (int64_t)n / d, rm = (int64_t)n % d;");
                const long lo = sg ? -(1L << (w - 1)) : 0, hi = sg ? (1L << (w - 1)) - 1 : (1L << w) - 1;
                line(fmt("if (q < %ld || q > %ld) fatal(\"division overflow at %08x\");", lo, hi, i.addr));
                if (w == 8) line("c.eax = (c.eax & ~0xffffu) | ((uint32_t)q & 0xffu) | ((uint32_t)rm & 0xffu) << 8;");
                else {
                    line("c.eax = (c.eax & ~0xffffu) | ((uint32_t)q & 0xffffu);");
                    line("c.edx = (c.edx & ~0xffffu) | ((uint32_t)rm & 0xffffu);");
                }
                return;
            }
            line(fmt("uint32_t d = %s;", read(o[0]).c_str()));
            line(fmt("if (d == 0) fatal(\"division by zero at %08x\");", i.addr));
            line("uint64_t n = (uint64_t)c.edx << 32 | c.eax;");
            if (m == ZYDIS_MNEMONIC_DIV) {
                line("uint64_t q = n / d;");
                line(fmt("if (q >> 32) fatal(\"division overflow at %08x\");", i.addr));
                line("c.eax = (uint32_t)q;");
                line("c.edx = (uint32_t)(n % d);");
            } else {
                line("int64_t sn = (int64_t)n, sd = (int32_t)d;");
                line("if (sn == INT64_MIN && sd == -1) fatal(\"division overflow\");");
                line("int64_t q = sn / sd;");
                line(fmt("if (q != (int32_t)q) fatal(\"division overflow at %08x\");", i.addr));
                line("c.eax = (uint32_t)q;");
                line("c.edx = (uint32_t)(sn % sd);");
            }
            return;
        }
        case ZYDIS_MNEMONIC_SHL:
        case ZYDIS_MNEMONIC_SHR:
        case ZYDIS_MNEMONIC_SAR: {
            line(fmt("%s a = %s;", T, read(o[0]).c_str()));
            line(fmt("uint32_t n = (uint32_t)%s & 31;", read(o[1]).c_str()));
            line("if (n) {");
            if (m == ZYDIS_MNEMONIC_SHL) {
                line(fmt("    %s r = (%s)((uint32_t)a << n);", T, T));
                if (want & (CF | OF))
                    line(fmt("    cf = n <= %u ? ((uint32_t)a >> (%u - n)) & 1 : 0;", w, w));
                if (want & OF) line(fmt("    of = ((r >> %u) & 1) ^ cf;", w - 1));
            } else if (m == ZYDIS_MNEMONIC_SHR) {
                line(fmt("    %s r = (%s)((uint32_t)a >> n);", T, T));
                if (want & CF) line("    cf = ((uint32_t)a >> (n - 1)) & 1;");
                if (want & OF) line(fmt("    of = (a >> %u) & 1;", w - 1));
            } else {
                const char* S = w == 8 ? "int8_t" : w == 16 ? "int16_t" : "int32_t";
                line(fmt("    uint32_t k = n < %u ? n : %u;", w, w - 1));
                line(fmt("    %s r = (%s)((int32_t)(%s)a >> k);", T, T, S));
                if (want & CF)
                    line(fmt("    cf = ((int32_t)(%s)a >> (n - 1 < 31 ? n - 1 : 31)) & 1;", S));
                if (want & OF) line("    of = false;");
            }
            line("    " + write(o[0], "r"));
            szp(w, want, "    ");
            line("}");
            return;
        }
        case ZYDIS_MNEMONIC_CWDE:
            line("c.eax = (uint32_t)(int32_t)(int16_t)c.eax;");
            return;
        case ZYDIS_MNEMONIC_CBW:
            line("c.eax = (c.eax & ~0xffffu) | (uint16_t)(int16_t)(int8_t)c.eax;");
            return;
        case ZYDIS_MNEMONIC_CDQ:
            line("c.edx = (uint32_t)((int32_t)c.eax >> 31);");
            return;
        case ZYDIS_MNEMONIC_CWD:
            line("c.edx = (c.edx & ~0xffffu) | ((int16_t)c.eax < 0 ? 0xffffu : 0u);");
            return;
        case ZYDIS_MNEMONIC_ROL:
        case ZYDIS_MNEMONIC_ROR: {
            const bool left = m == ZYDIS_MNEMONIC_ROL;
            line(fmt("%s a = %s;", T, read(o[0]).c_str()));
            line(fmt("uint32_t n = (uint32_t)%s & 31;", read(o[1]).c_str()));
            line("if (n) {");
            line(fmt("    uint32_t k = n %% %u;", w));
            line(fmt("    %s r = k ? (%s)(%s) : a;", T, T,
                     left ? fmt("(uint32_t)a << k | (uint32_t)a >> (%u - k)", w).c_str()
                          : fmt("(uint32_t)a >> k | (uint32_t)a << (%u - k)", w).c_str()));
            line("    " + write(o[0], "r"));
            if (left) {
                if (want & (CF | OF)) line("    cf = r & 1;");
                if (want & OF) line(fmt("    of = ((r >> %u) & 1) ^ cf;", w - 1));
            } else {
                if (want & CF) line(fmt("    cf = (r >> %u) & 1;", w - 1));
                if (want & OF) line(fmt("    of = ((r >> %u) ^ (r >> %u)) & 1;", w - 1, w - 2));
            }
            line("}");
            return;
        }
        case ZYDIS_MNEMONIC_SHLD:
        case ZYDIS_MNEMONIC_SHRD: {
            if (w != 32) die("16-bit %s at %08x", m == ZYDIS_MNEMONIC_SHLD ? "shld" : "shrd", i.addr);
            const bool left = m == ZYDIS_MNEMONIC_SHLD;
            line(fmt("uint32_t a = %s, s = %s;", read(o[0]).c_str(), read(o[1]).c_str()));
            line(fmt("uint32_t n = (uint32_t)%s & 31;", read(o[2]).c_str()));
            line("if (n) {");
            line(left ? "    uint32_t r = a << n | s >> (32 - n);" : "    uint32_t r = a >> n | s << (32 - n);");
            line("    " + write(o[0], "r"));
            if (want & CF) line(left ? "    cf = (a >> (32 - n)) & 1;" : "    cf = (a >> (n - 1)) & 1;");
            if (want & OF) line("    of = ((r ^ a) >> 31) & 1;");
            szp(32, want, "    ");
            line("}");
            return;
        }
        case ZYDIS_MNEMONIC_XCHG:
            if (o[0].type == ZYDIS_OPERAND_TYPE_MEMORY || o[1].type == ZYDIS_OPERAND_TYPE_MEMORY) {
                // xchg with memory is always atomic on x86.
                const auto& mo = o[0].type == ZYDIS_OPERAND_TYPE_MEMORY ? o[0] : o[1];
                const auto& ro = o[0].type == ZYDIS_OPERAND_TYPE_MEMORY ? o[1] : o[0];
                line(fmt("%s old = atomic_rmw<%s>(M, %s, %s, kAtomicXchg);", T, T, ea(mo).c_str(),
                         read(ro).c_str()));
                line(write(ro, "old"));
            } else {
                line(fmt("%s a = %s, b = %s;", T, read(o[0]).c_str(), read(o[1]).c_str()));
                line(write(o[0], "b"));
                line(write(o[1], "a"));
            }
            return;
        case ZYDIS_MNEMONIC_XADD: {
            const bool locked = i.in.attributes & ZYDIS_ATTRIB_HAS_LOCK;
            line(fmt("%s b = %s;", T, read(o[1]).c_str()));
            if (locked) {
                if (o[0].type != ZYDIS_OPERAND_TYPE_MEMORY) die("locked xadd at %08x", i.addr);
                line(fmt("%s a = atomic_rmw<%s>(M, %s, b, kAtomicAdd);", T, T, ea(o[0]).c_str()));
            } else {
                line(fmt("%s a = %s;", T, read(o[0]).c_str()));
            }
            line(fmt("%s r = (%s)(a + b);", T, T));
            line(write(o[1], "a"));
            if (!locked) line(write(o[0], "r"));
            if (want & CF) line(fmt("cf = (((uint64_t)a + b) >> %u) & 1;", w));
            if (want & OF) line(fmt("of = (((a ^ r) & (b ^ r)) >> %u) & 1;", w - 1));
            szp(w, want);
            return;
        }
        case ZYDIS_MNEMONIC_BSWAP:
            line(write(o[0], fmt("__builtin_bswap32(%s)", read(o[0]).c_str())));
            return;
        case ZYDIS_MNEMONIC_BSF:
        case ZYDIS_MNEMONIC_BSR:
            // A zero source sets ZF and leaves the destination unchanged.
            line(fmt("%s v = %s;", T, read(o[1]).c_str()));
            line(fmt("if (v) { %s }", write(o[0], m == ZYDIS_MNEMONIC_BSF
                                                     ? "(uint32_t)__builtin_ctz(v)"
                                                     : fmt("%uu - (uint32_t)__builtin_clz((uint32_t)v)",
                                                           31).c_str()).c_str()));
            if (want & ZF) line("zf = v == 0;");
            if (want & (CF | OF | SF | PF)) line("cf = of = sf = pf = false;");
            return;
        case ZYDIS_MNEMONIC_RDTSC:
            line("uint64_t t = rdtsc();");
            line("c.eax = (uint32_t)t;");
            line("c.edx = (uint32_t)(t >> 32);");
            return;
        case ZYDIS_MNEMONIC_CPUID:
            line("cpuid(c);");
            return;
        case ZYDIS_MNEMONIC_LAHF:
            line("c.eax = (c.eax & ~0xff00u) | (uint32_t)(sf << 7 | zf << 6 | pf << 2 | 2 | cf) << 8;");
            return;
        case ZYDIS_MNEMONIC_SAHF:
            line("{ const uint32_t h = c.eax >> 8; cf = h & 1; pf = h >> 2 & 1; zf = h >> 6 & 1; sf = h >> 7 & 1; }");
            return;
        case ZYDIS_MNEMONIC_PUSHFD:
            line("uint32_t v = (c.eflags & ~0xcd5u) | cf | pf << 2 | zf << 6 | sf << 7 | (uint32_t)c.df << 10 | of << 11;");
            line("c.esp -= 4;");
            line("wr32(M, c.esp, v);");
            return;
        case ZYDIS_MNEMONIC_POPFD:
            line("uint32_t v = rd32(M, c.esp);");
            line("c.esp += 4;");
            line("cf = v & 1; pf = v >> 2 & 1; zf = v >> 6 & 1; sf = v >> 7 & 1; of = v >> 11 & 1;");
            line("c.df = v >> 10 & 1;");
            line("c.eflags = (v & ~0xcd5u) | 2u;");
            return;
        case ZYDIS_MNEMONIC_PUSHAD:
            line("uint32_t sp = c.esp;");
            line("const uint32_t r[8] = {c.eax, c.ecx, c.edx, c.ebx, sp, c.ebp, c.esi, c.edi};");
            line("for (uint32_t v : r) { c.esp -= 4; wr32(M, c.esp, v); }");
            return;
        case ZYDIS_MNEMONIC_POPAD:
            line("c.edi = rd32(M, c.esp); c.esi = rd32(M, c.esp + 4); c.ebp = rd32(M, c.esp + 8);");
            line("c.ebx = rd32(M, c.esp + 16); c.edx = rd32(M, c.esp + 20); c.ecx = rd32(M, c.esp + 24);");
            line("c.eax = rd32(M, c.esp + 28); c.esp += 32;");
            return;
        case ZYDIS_MNEMONIC_CLC: line("cf = false;"); return;
        case ZYDIS_MNEMONIC_STC: line("cf = true;"); return;
        case ZYDIS_MNEMONIC_CMC: line("cf = !cf;"); return;
        case ZYDIS_MNEMONIC_PREFETCHNTA: case ZYDIS_MNEMONIC_PREFETCHT0:
        case ZYDIS_MNEMONIC_PREFETCHT1: case ZYDIS_MNEMONIC_PREFETCHT2:
        case ZYDIS_MNEMONIC_LFENCE: case ZYDIS_MNEMONIC_SFENCE: case ZYDIS_MNEMONIC_PAUSE:
            return;
        case ZYDIS_MNEMONIC_MFENCE:
            line("__atomic_thread_fence(__ATOMIC_SEQ_CST);");
            return;
        case ZYDIS_MNEMONIC_BT: case ZYDIS_MNEMONIC_BTS: case ZYDIS_MNEMONIC_BTR:
        case ZYDIS_MNEMONIC_BTC: {
            // A register bit offset into memory addresses a bit string.
            const bool reg_offset = o[1].type == ZYDIS_OPERAND_TYPE_REGISTER;
            line(fmt("uint32_t n = %s;", read(o[1], w).c_str()));
            std::string dst;
            if (o[0].type == ZYDIS_OPERAND_TYPE_MEMORY && reg_offset) {
                line(fmt("uint32_t a = %s + (uint32_t)((int32_t)n >> %u) * %uu;", ea(o[0]).c_str(),
                         w == 16 ? 4 : 5, w / 8));
                line(fmt("%s v = rd%u(M, a);", T, w));
            } else {
                line(fmt("%s v = %s;", T, read(o[0]).c_str()));
            }
            line(fmt("n &= %uu;", w - 1));
            line("cf = (v >> n) & 1;");
            if (want & ~CF & ALL) line("of = sf = pf = zf = false;");
            if (m != ZYDIS_MNEMONIC_BT) {
                const char* op = m == ZYDIS_MNEMONIC_BTS ? "v | (%s)(1u << n)" : m == ZYDIS_MNEMONIC_BTR
                                     ? "v & (%s)~(1u << n)" : "v ^ (%s)(1u << n)";
                const std::string nv = fmt(op, T);
                if (o[0].type == ZYDIS_OPERAND_TYPE_MEMORY && reg_offset)
                    line(fmt("wr%u(M, a, (%s)(%s));", w, T, nv.c_str()));
                else
                    line(write(o[0], fmt("(%s)(%s)", T, nv.c_str())));
            }
            return;
        }
        case ZYDIS_MNEMONIC_CMPXCHG: {
            const char* A = w == 8 ? "(uint8_t)c.eax" : w == 16 ? "(uint16_t)c.eax" : "c.eax";
            line(fmt("%s a = %s, b;", T, A));
            if (i.in.attributes & ZYDIS_ATTRIB_HAS_LOCK) {
                if (o[0].type != ZYDIS_OPERAND_TYPE_MEMORY) die("locked form at %08x", i.addr);
                line(fmt("b = atomic_cas<%s>(M, %s, a, %s);", T, ea(o[0]).c_str(), read(o[1]).c_str()));
            } else {
                line(fmt("b = %s;", read(o[0]).c_str()));
                line(fmt("if (a == b) { %s }", write(o[0], read(o[1])).c_str()));
            }
            // Flags as `cmp eax, dest`; eax gets the old value when they differ.
            line(fmt("%s r = (%s)(a - b);", T, T));
            ZydisDecodedOperand acc{};
            acc.type = ZYDIS_OPERAND_TYPE_REGISTER;
            acc.size = (ZyanU16)w;
            acc.reg.value = w == 8 ? ZYDIS_REGISTER_AL : w == 16 ? ZYDIS_REGISTER_AX : ZYDIS_REGISTER_EAX;
            line(fmt("if (a != b) { %s }", write(acc, "b").c_str()));
            if (want & CF) line("cf = a < b;");
            if (want & OF) line(fmt("of = (((a ^ b) & (a ^ r)) >> %u) & 1;", w - 1));
            szp(w, want);
            return;
        }
        case ZYDIS_MNEMONIC_RCL:
        case ZYDIS_MNEMONIC_RCR: {
            // Rotate through carry, one bit at a time (rare: 64-bit shifts in the CRT).
            const bool left = m == ZYDIS_MNEMONIC_RCL;
            line(fmt("%s a = %s;", T, read(o[0]).c_str()));
            line(fmt("uint32_t n = ((uint32_t)%s & 31) %% %u;", read(o[1]).c_str(), w + 1));
            line("if (n) {");
            line("    bool c0 = cf;");
            line("    for (uint32_t k = 0; k < n; k++) {");
            if (left) {
                line(fmt("        const bool out = (a >> %u) & 1;", w - 1));
                line(fmt("        a = (%s)((uint32_t)a << 1 | (uint32_t)c0);", T));
            } else {
                line("        const bool out = a & 1;");
                line(fmt("        a = (%s)((uint32_t)a >> 1 | (uint32_t)c0 << %u);", T, w - 1));
            }
            line("        c0 = out;");
            line("    }");
            line("    cf = c0;");
            line("    " + write(o[0], "a"));
            if (want & OF)
                line(left ? fmt("    of = ((a >> %u) & 1) ^ cf;", w - 1)
                          : fmt("    of = ((a >> %u) ^ (a >> %u)) & 1;", w - 1, w - 2));
            line("}");
            return;
        }
        case ZYDIS_MNEMONIC_CLD:
            line("c.df = 0;");
            return;
        case ZYDIS_MNEMONIC_STD:
            line("c.df = 1;");
            return;
        case ZYDIS_MNEMONIC_EMMS:
            line("x87::emms(c.fpu);");
            return;
        case ZYDIS_MNEMONIC_INT3:
            line(fmt("fatal(\"int3 (breakpoint) at %08x\");", i.addr));
            return;
        case ZYDIS_MNEMONIC_HLT:
        case ZYDIS_MNEMONIC_UD2:
            line(fmt("fatal(\"%s at %08x\");", m == ZYDIS_MNEMONIC_HLT ? "hlt" : "ud2", i.addr));
            return;
        default:
            break;
        }
        if (is_cmov(m) && condition(m, &cond)) {
            // The source is read even when the condition is false, as on x86.
            line(fmt("%s v = %s;", T, read(o[1]).c_str()));
            line(fmt("if %s { %s }",
                     cond.expr[0] == '(' ? cond.expr : fmt("(%s)", cond.expr).c_str(),
                     write(o[0], "v").c_str()));
            return;
        }
        if (is_loop(m)) {
            if (i.xfer != Xfer::CondJump) die("loop out of the function at %08x", i.addr);
            if (m == ZYDIS_MNEMONIC_JECXZ) {
                line(fmt("if (c.ecx == 0) goto L_%08x;", i.target));
            } else {
                line("c.ecx--;");
                const char* extra = m == ZYDIS_MNEMONIC_LOOPE ? " && zf" : m == ZYDIS_MNEMONIC_LOOPNE ? " && !zf" : "";
                line(fmt("if (c.ecx != 0%s) goto L_%08x;", extra, i.target));
            }
            return;
        }
        if (condition(m, &cond)) {
            if (i.xfer == Xfer::CondTailCall) {
                const std::string e = cond.expr[0] == '(' ? cond.expr : fmt("(%s)", cond.expr);
                line(fmt("if %s {", e.c_str()));
                store_flags();
                line(callee(i.target, false));
                line("return;");
                line("}");
            } else if (is_jcc(m)) {
                const std::string e = cond.expr[0] == '(' ? cond.expr : fmt("(%s)", cond.expr);
                line(fmt("if %s goto L_%08x;", e.c_str(), i.target));
            } else {
                line(write(o[0], fmt("%s ? 1 : 0", cond.expr)));
            }
            return;
        }
        die("unsupported instruction at %08x: %s", i.addr, i.text.c_str());
    }

private:
    std::string* out_;
    const Image* image_;
    EmitResult* res_;
    uint8_t want_ = 0;  // flags the current instruction must produce
    const DecodedFunction* fn_;
};

bool is_call(Xfer x) {
    return x == Xfer::Call || x == Xfer::CallImport || x == Xfer::CallIndirect;
}

}  // namespace

std::string func_symbol(uint32_t addr) { return fmt("f_%08x", addr); }

std::string import_symbol(const std::string& dyld_name) {
    // Drop the one underscore Mach-O prefixes to C names (so `_exit` and
    // `__exit` stay distinct), keep underscores, fold anything else ('$', '.')
    // to '_'.
    std::string s = "imp_";
    size_t k = dyld_name.size() > 1 && dyld_name[0] == '_' ? 1 : 0;
    for (; k < dyld_name.size(); k++) {
        const char ch = dyld_name[k];
        s += std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' ? ch : '_';
    }
    return s;
}

std::string override_symbol(const std::string& symbol) { return "ov_" + import_symbol(symbol).substr(4); }

std::string demangle(const std::string& name) {
    const char* n = name.c_str();
    if (n[0] == '_') n++;  // Mach-O adds a leading underscore
    int status = 0;
    char* d = abi::__cxa_demangle(n, nullptr, nullptr, &status);
    std::string out = status == 0 && d ? d : n;
    std::free(d);
    return out;
}


// Register caching. The guest registers a function uses live in local variables
// rather than in the Context: guest memory is accessed through a uint8_t
// pointer, which may alias anything, so with registers in the Context the C++
// compiler has to reload them after every guest store. The locals are written
// back to the Context before anything else can see it (a call to another
// function, an indirect call, an import, cpuid, fatal, a return) and read again
// after a call. Works on the finished body: c.eax... become eax..., c.xmm[N]
// becomes xmmN. Returns the declarations to put at the top of the function.
std::string cache_registers(std::string& body) {
    static const char* kGpr[] = {"eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"};
    auto replace_all = [](std::string& s, const std::string& from, const std::string& to) {
        for (size_t p = s.find(from); p != std::string::npos; p = s.find(from, p + to.size()))
            s.replace(p, from.size(), to);
    };
    std::vector<std::pair<std::string, std::string>> used;  // local, Context field
    for (const char* r : kGpr) {
        const std::string field = std::string("c.") + r;
        if (body.find(field) == std::string::npos) continue;
        replace_all(body, field, r);
        used.emplace_back(r, field);
    }
    for (int k = 0; k < 8; k++) {
        const std::string field = fmt("c.xmm[%d]", k), local = fmt("xmm%d", k);
        if (body.find(field) == std::string::npos) continue;
        replace_all(body, field, local);
        used.emplace_back(local, field);
    }
    if (used.empty()) return "";
    std::string out_sync, in_sync, decl;
    for (const auto& [local, field] : used) {
        out_sync += field + " = " + local + "; ";
        in_sync += local + " = " + field + "; ";
        decl += fmt("    auto %s = %s;\n", local.c_str(), field.c_str());
    }
    // Rewrite line by line: calls get a sync on both sides, returns and fatal
    // calls one before.
    std::string out;
    out.reserve(body.size() + body.size() / 4);
    size_t pos = 0;
    while (pos < body.size()) {
        size_t end = body.find('\n', pos);
        if (end == std::string::npos) end = body.size();
        const std::string line = body.substr(pos, end - pos);
        pos = end + 1;
        const size_t ind = line.find_first_not_of(' ');
        const std::string code = ind == std::string::npos ? "" : line.substr(ind);
        const std::string pad = ind == std::string::npos ? "" : line.substr(0, ind);
        const bool is_call = (code.rfind("f_", 0) == 0 && code.size() > 6 && code.compare(code.size() - 4, 4, "(c);") == 0) ||
                             (code.rfind("imp_", 0) == 0 && code.compare(code.size() - 4, 4, "(c);") == 0) ||
                             code == "call_indirect(c, t);" || code == "cpuid(c);";
        if (is_call) {
            out += pad + out_sync + "\n" + line + "\n" + pad + in_sync + "\n";
        } else if (code == "return;") {
            out += pad + out_sync + "\n" + line + "\n";
        } else if (code.rfind("default: ", 0) == 0 && code.find("fatal(") != std::string::npos) {
            out += pad + "default: " + out_sync + code.substr(9) + "\n";  // inside the switch
        } else if (code.find("fatal(") != std::string::npos) {
            out += pad + out_sync + "\n" + line + "\n";
        } else {
            out += line + "\n";
        }
    }
    body = std::move(out);
    return decl;
}

EmitResult emit_function(DecodedFunction& fn, const std::string& cname, const Image* image,
                         const std::string& comment) {
    if (!fn.errors.empty()) die("%s: %s", cname.c_str(), fn.errors.front().c_str());
    const bool pe = image && image->is_pe();
    for (auto& [a, i] : fn.insns) flag_effects(i);

    // Backward flag liveness to a fixed point. Flags are dead after ret, tail
    // calls, and calls (callers never read flags a callee left behind).
    auto live_in = [&](uint32_t s) -> uint8_t {
        auto it = fn.insns.find(s);
        if (it == fn.insns.end()) return 0;
        return it->second.use | (it->second.live_out & ~it->second.def);
    };
    for (bool changed = true; changed;) {
        changed = false;
        for (auto it = fn.insns.rbegin(); it != fn.insns.rend(); ++it) {
            Insn& i = it->second;
            uint8_t live = 0;
            if (i.falls_through) live |= live_in(i.addr + i.in.length);
            // PE: flags go along to other functions this code transfers to.
            if (pe && ((i.falls_through && fn.falls_into.count(i.addr + i.in.length)) || i.xfer == Xfer::TailCall ||
                       i.xfer == Xfer::CondTailCall || i.xfer == Xfer::JumpTable))
                live |= ALL;
            if (i.xfer == Xfer::Jump || i.xfer == Xfer::CondJump) live |= live_in(i.target);
            for (uint32_t t : i.cases) live |= live_in(t);
            if (live != i.live_out) {
                i.live_out = live;
                changed = true;
            }
        }
    }
    const Insn& entry = fn.insns.at(fn.start);
    const uint8_t live_at_entry = entry.use | (entry.live_out & ~entry.def);

    EmitResult res;
    std::string body;
    Emitter em(&body, image, &res, &fn);
    std::set<uint32_t> jumped_to;
    for (auto& [a, i] : fn.insns)
        if (i.xfer == Xfer::Jump || i.xfer == Xfer::CondJump) jumped_to.insert(i.target);
        else jumped_to.insert(i.cases.begin(), i.cases.end());
    // PE functions may hold code below their entry (reached by jumps back):
    // the body is in address order, so execution starts with a jump.
    if (fn.insns.begin()->first != fn.start) {
        jumped_to.insert(fn.start);
        body += fmt("    goto L_%08x;\n", fn.start);
    }
    for (auto& [a, i] : fn.insns) {
        if (jumped_to.count(a)) body += fmt("L_%08x:\n", a);
        body += fmt("    // %08x: %s\n    {\n", a, i.text.c_str());
        em.insn(i);
        if (i.falls_through && fn.falls_into.count(a + i.in.length)) {
            // PE: the code runs on into the next function, as if tail-calling it.
            em.store_flags();
            em.line(func_symbol(a + i.in.length) + "(c);");
            em.line("return;");
            res.callees.insert(a + i.in.length);
        } else if (i.falls_through && !fn.insns.count(a + i.in.length))
            em.line(is_call(i.xfer)
                        ? fmt("fatal(\"returned from a noreturn call at %08x\");", a)
                        : fmt("fatal(\"ran past the end of the function at %08x\");", a));
        body += "    }\n";
        for (uint8_t b = 1; b <= OF; b <<= 1) {
            if (i.def & b && !is_call(i.xfer)) res.flag_writes++;
            if (i.def & b & i.live_out && !is_call(i.xfer)) res.flag_writes_kept++;
        }
    }

    const std::string regs = cache_registers(body);

    std::string& out = res.code;
    if (!comment.empty()) out += "// " + comment + "\n";
    // Huge x87 functions (AC1's has 29,000 instructions, 17,000 of them x87)
    // take clang's optimizer more than half an hour: they are left unoptimized.
    size_t x87_insns = 0;
    for (const auto& [a, i] : fn.insns) x87_insns += is_x87(i.in.mnemonic);
    if (x87_insns > 4000) out += "[[clang::optnone]] ";
    out += fmt("void %s(scimitar::Context& c) {\n", cname.c_str());
    out += "    using namespace scimitar;\n    using namespace scimitar::ops;\n";
    out += "    uint8_t* const M = c.mem->base();\n";
    out += "    [[maybe_unused]] bool cf = false, pf = false, zf = false, sf = false, of = false;\n";
    if (live_at_entry && pe)
        out += "    // Flags read before being set: passed in by the code that came here.\n"
               "    cf = c.eflags & 1; pf = c.eflags >> 2 & 1; zf = c.eflags >> 6 & 1; sf = c.eflags >> 7 & 1; "
               "of = c.eflags >> 11 & 1;\n";
    else if (live_at_entry) out += "    // NOTE: flags read before being set; they start as false.\n";
    out += regs;
    out += body;
    out += "}\n";
    return res;
}

}  // namespace recomp
