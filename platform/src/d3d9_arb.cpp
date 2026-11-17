// Direct3D 9 shader bytecode to ARB assembly: see scimitar/d3d9_arb.h.
//
// Token formats are the D3D9 driver interface's (d3d9types.h): an
// instruction token (opcode in bits 0-15, controls 16-23, length 24-27,
// predicated bit 28), then its parameters: destination (register number 0-10,
// type 28-30 and 11-12, relative bit 13, write mask 16-19, result modifiers
// 20-23, shift 24-27) and sources (swizzle 16-23, modifier 24-27), a source
// with the relative bit followed by its address register's token.
#include "scimitar/d3d9_arb.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>

namespace scimitar::d3d9 {

namespace {

enum Opcode {
    kNop = 0, kMov = 1, kAdd = 2, kSub = 3, kMad = 4, kMul = 5, kRcp = 6, kRsq = 7, kDp3 = 8, kDp4 = 9,
    kMin = 10, kMax = 11, kSlt = 12, kSge = 13, kExp = 14, kLog = 15, kLit = 16, kDst = 17, kLrp = 18,
    kFrc = 19, kM4x4 = 20, kM4x3 = 21, kM3x4 = 22, kM3x3 = 23, kM3x2 = 24, kCall = 25, kCallNz = 26,
    kLoop = 27, kRet = 28, kEndLoop = 29, kLabel = 30, kDcl = 31, kPow = 32, kCrs = 33, kSgn = 34,
    kAbs = 35, kNrm = 36, kSinCos = 37, kRep = 38, kEndRep = 39, kIf = 40, kIfc = 41, kElse = 42,
    kEndIf = 43, kBreak = 44, kBreakC = 45, kMova = 46, kDefB = 47, kDefI = 48, kTexKill = 65,
    kTex = 66, kDef = 81, kCmp = 88, kDp2Add = 90, kDsx = 91, kDsy = 92, kTexLdd = 93, kSetP = 94,
    kTexLdl = 95, kBreakP = 96,
};

enum RegType {
    kTemp = 0, kInput = 1, kConst = 2, kAddr = 3, kRastOut = 4, kAttrOut = 5, kOutput = 6,
    kConstInt = 7, kColorOut = 8, kDepthOut = 9, kSampler = 10, kConstBool = 14, kLoopReg = 15,
    kMisc = 17, kPredicate = 19,
};

enum Usage {
    kPosition = 0, kBlendWeight = 1, kBlendIndices = 2, kNormal = 3, kPSize = 4, kTexCoord = 5,
    kTangent = 6, kBinormal = 7, kPositionT = 9, kColor = 10, kFog = 11,
};

int reg_type(uint32_t t) { return (int)(((t >> 28) & 7) | ((t >> 8) & 0x18)); }

bool has_no_dest(int op) {
    switch (op) {
    case kNop: case kIf: case kIfc: case kElse: case kEndIf: case kRep: case kEndRep: case kLoop:
    case kEndLoop: case kBreak: case kBreakC: case kBreakP: case kCall: case kCallNz: case kRet:
    case kLabel:
        return true;
    }
    return false;
}

Dest dest_token(uint32_t t) {
    Dest d;
    d.type = reg_type(t);
    d.num = (int)(t & 0x7ff);
    d.mask = (int)((t >> 16) & 0xf);
    d.saturate = (t >> 20) & 1;
    d.shift = (int)((t >> 24) & 0xf);
    return d;
}

Source source_token(uint32_t t) {
    Source s;
    s.type = reg_type(t);
    s.num = (int)(t & 0x7ff);
    for (int k = 0; k < 4; k++) s.swizzle[k] = (uint8_t)((t >> (16 + 2 * k)) & 3);
    s.modifier = (int)((t >> 24) & 0xf);
    s.relative = (t >> 13) & 1;
    return s;
}

// The CTAB comment (after its FourCC): offsets are from its start.
void parse_ctab(const uint8_t* d, size_t size, Program& p) {
    auto u16 = [&](size_t at) -> uint32_t { return at + 2 <= size ? (uint32_t)(d[at] | d[at + 1] << 8) : 0; };
    auto u32 = [&](size_t at) -> uint32_t { return at + 4 <= size ? u16(at) | u16(at + 2) << 16 : 0; };
    if (size < 28) return;
    const uint32_t count = u32(12), info = u32(16);
    for (uint32_t k = 0; k < count && info + 20 * (k + 1) <= size; k++) {
        const size_t at = info + 20 * k;
        Constant c;
        const uint32_t name = u32(at);
        for (size_t n = name; n < size && d[n]; n++) c.name += (char)d[n];
        c.set = (RegisterSet)u16(at + 4);
        c.reg = (int)u16(at + 6);
        c.count = (int)u16(at + 8);
        const uint32_t type = u32(at + 12);
        if (type + 16 <= size) {
            c.is_struct = u16(type) == 5;      // D3DXPC_STRUCT
            c.column_major = u16(type) == 3;   // D3DXPC_MATRIX_COLUMNS
            c.rows = (int)u16(type + 4);
            c.columns = (int)u16(type + 6);
            c.elements = std::max(1, (int)u16(type + 8));
        }
        p.constants.push_back(c);
    }
}

std::string fmt_float(float v) {
    if (std::isnan(v)) v = 0;
    if (std::isinf(v)) v = v > 0 ? 3.4e38f : -3.4e38f;
    char b[32];
    std::snprintf(b, sizeof b, "%.9g", v);
    return b;
}

// ---- the translator ----

struct Operand {
    std::string text;  // as written in an instruction: "-c[4].xyxy"
    int cls = 0;       // 0 temp, 1 program parameter, 2 attribute
    std::string key;   // the register, for the one-parameter rule
    bool scalar = false;
};

class Translator {
public:
    Translator(const Program& p, const ArbOptions& o) : p_(p), o_(o), vs_(p.vertex) {}

    std::string run(std::string* error) {
        if (!p_.error.empty()) return fail_with(error, p_.error);
        if (p_.major != 3) return fail_with(error, "shader model " + std::to_string(p_.major) + " is not supported");
        declarations();
        emit_range(0, p_.code.size());
        if (!cond_.empty()) fail("unbalanced if");
        if (!err_.empty()) return fail_with(error, err_);
        return assemble();
    }

private:
    const Program& p_;
    const ArbOptions& o_;
    const bool vs_;
    std::string err_;
    std::vector<std::string> body_;
    std::set<std::string> temps_;
    int scratch_ = 0;
    std::vector<std::string> cond_;  // execution masks of the open if blocks, innermost last
    struct Block {
        std::string reg;    // its condition temp: .x mask, .y raw condition
        std::string outer;  // the enclosing mask, or ""
    };
    std::vector<Block> blocks_;
    // Declarations.
    std::map<int, std::string> inputs_;   // v# -> binding
    std::map<int, std::string> outputs_;  // o# -> binding (vertex)
    std::map<int, std::string> targets_;  // s# -> texture target
    std::map<int, std::array<float, 4>> fdef_;
    std::map<int, std::array<int, 4>> idef_;
    std::map<int, bool> bdef_;
    std::set<int> consts_;  // float registers read
    // Column-major matrices read transposed: register -> (first register, rows, column).
    struct Column {
        int base, rows, column;
    };
    std::map<int, Column> columns_;
    std::vector<const Constant*> column_arrays_;  // arrays of them, read through a0
    std::set<int> transposed_;  // matrices (first registers) whose temporaries are filled
    std::vector<std::string> prologue_;
    std::set<int> colors_;  // oC# written
    bool depth_out_ = false, relative_ = false, shadow_ = false;
    bool sincos_consts_ = false;

    void fail(const std::string& m) {
        if (err_.empty()) err_ = m;
    }
    static std::string fail_with(std::string* error, const std::string& m) {
        if (error) *error = m;
        return "";
    }
    void emit(const std::string& line) { body_.push_back(line + ";"); }
    std::string scratch() {
        const std::string t = "t" + std::to_string(scratch_++);
        temps_.insert(t);
        return t;
    }

    void declarations() {
        if (o_.transpose_column_matrices)
            for (const Constant& k : p_.constants)
                if (k.set == RegisterSet::Float4 && k.column_major && k.rows <= 4) {
                    if (k.elements == 1)
                        for (int j = 0; j < k.count; j++) columns_[k.reg + j] = {k.reg, k.rows, j};
                    else if (k.columns > 0)
                        column_arrays_.push_back(&k);
                }
        for (const Instruction& in : p_.code) {
            if (in.op == kDcl) {
                const Dest& d = in.dst;
                if (d.type == kInput) {
                    std::string b = input_binding(in.usage, in.usage_index);
                    if (b.empty()) fail("input usage " + std::to_string(in.usage));
                    inputs_[d.num] = b;
                } else if (d.type == kOutput && vs_) {
                    std::string b = output_binding(in.usage, in.usage_index);
                    if (b.empty()) fail("output usage " + std::to_string(in.usage));
                    outputs_[d.num] = b;
                } else if (d.type == kSampler) {
                    const char* t = in.texture_type == 2 ? "2D" : in.texture_type == 3 ? "CUBE" : in.texture_type == 4 ? "3D" : nullptr;
                    if (!t) fail("sampler type " + std::to_string(in.texture_type));
                    else if ((o_.shadow_samplers >> d.num) & 1 && in.texture_type == 2) t = "SHADOW2D", shadow_ = true;
                    targets_[d.num] = t ? t : "2D";
                } else if (d.type == kMisc && d.num > 1) {
                    fail("misc register " + std::to_string(d.num));
                }
            } else if (in.op == kDef) {
                std::array<float, 4> v;
                std::memcpy(v.data(), in.fvalue, sizeof in.fvalue);
                fdef_[in.dst.num] = v;
            } else if (in.op == kDefI) {
                std::array<int, 4> v;
                std::memcpy(v.data(), in.ivalue, sizeof in.ivalue);
                idef_[in.dst.num] = v;
            } else if (in.op == kDefB) {
                bdef_[in.dst.num] = in.ivalue[0] != 0;
            }
        }
    }

    std::string input_binding(int usage, int index) const {
        if (vs_) {
            const int a = vertex_attribute(usage, index, o_.separate_attributes);
            return a < 0 ? "" : "vertex.attrib[" + std::to_string(a) + "]";
        }
        switch (usage) {
        case kTexCoord: return index < o_.texcoords ? "fragment.texcoord[" + std::to_string(index) + "]" : "";
        case kColor: return index == 0 ? "fragment.color" : index == 1 ? "fragment.color.secondary" : "";
        case kFog: return "fragment.fogcoord";
        }
        return "";
    }
    std::string output_binding(int usage, int index) const {
        switch (usage) {
        case kPosition: return index ? "" : "result.position";
        case kTexCoord: return index < o_.texcoords ? "result.texcoord[" + std::to_string(index) + "]" : "";
        case kColor: return index == 0 ? "result.color" : index == 1 ? "result.color.secondary" : "";
        case kFog: return "result.fogcoord";
        case kPSize: return "result.pointsize";
        }
        return "";
    }

    static std::string swizzle_text(const uint8_t s[4]) {
        static const char c[] = "xyzw";
        if (s[0] == 0 && s[1] == 1 && s[2] == 2 && s[3] == 3) return "";
        if (s[0] == s[1] && s[1] == s[2] && s[2] == s[3]) return std::string(".") + c[s[0]];
        return std::string(".") + c[s[0]] + c[s[1]] + c[s[2]] + c[s[3]];
    }
    static std::string mask_text(int mask) {
        if (mask == 0xf || mask == 0) return "";
        std::string s = ".";
        for (int k = 0; k < 4; k++)
            if (mask >> k & 1) s += "xyzw"[k];
        return s;
    }
    static Operand temp(const std::string& text) { return Operand{text, 0, text}; }
    static Operand param(const std::string& text, const std::string& key) { return Operand{text, 1, key}; }

    // A source operand; `scalar` takes one component (the last selected,
    // which is the replicated one of a replicate swizzle).
    Operand src(const Source& s, bool scalar = false) {
        Operand o;
        std::string base;
        uint8_t swizzle[4];
        std::memcpy(swizzle, s.swizzle, 4);
        switch (s.type) {
        case kTemp:
            base = "r" + std::to_string(s.num);
            temps_.insert(base);
            o.cls = 0;
            break;
        case kConst:
            o.cls = 1;
            if (s.relative) {
                if (!vs_) fail("relative addressing in a fragment program");
                if (s.rel_type != kAddr) fail("relative addressing through aL");
                relative_ = true;
                temps_.insert("ar");
                const std::string a = std::string("ar.") + "xyzw"[s.rel_component];
                const std::string t = scratch();
                const Constant* array = nullptr;
                for (const Constant* k : column_arrays_)
                    if (s.num >= k->reg && s.num < k->reg + k->count) array = k;
                if (array) {
                    // An element of an array of column-major matrices: its
                    // column j gathered from the rows the engine uploaded.
                    const int j = (s.num - array->reg) % array->columns;
                    const std::string g = scratch();
                    for (int i = 0; i < array->rows; i++) {
                        emit("ADD " + t + ".x, " + a + ", " + fmt_float((float)(s.num - j + i)));
                        emit("ARL A0.x, " + t + ".x");
                        emit(std::string("MOV ") + g + "." + "xyzw"[i] + ", c[A0.x]." + "xyzw"[j]);
                    }
                    base = g;
                    o.cls = 0;
                    break;
                }
                // ARB offsets stop at 63: the register number goes into the address.
                emit("ADD " + t + ".x, " + a + ", " + fmt_float((float)s.num));
                emit("ARL A0.x, " + t + ".x");
                base = "c[A0.x]";
            } else if (columns_.count(s.num) && !fdef_.count(s.num)) {
                base = column(s.num);
                o.cls = 0;
            } else {
                consts_.insert(s.num);
                base = "c[" + std::to_string(s.num) + "]";
            }
            break;
        case kInput: {
            auto it = inputs_.find(s.num);
            if (it == inputs_.end()) fail("undeclared input v" + std::to_string(s.num));
            else base = it->second;
            o.cls = 2;
            break;
        }
        case kMisc:
            if (vs_ || s.num > 1) fail("misc register");
            if (s.num == 0) {
                base = "fragment.position";
                o.cls = 2;
            } else {
                // vFace: ARB fragment programs cannot tell the faces apart;
                // everything is front facing (as in the Mac build's programs).
                base = "k";
                std::memset(swizzle, 1, 4);
                o.cls = 1;
            }
            break;
        default:
            fail("source register type " + std::to_string(s.type));
            base = "r0";
        }
        o.key = base;
        bool negate = false;
        switch (s.modifier) {
        case 0: break;
        case 1: negate = true; break;
        case 11: case 12: {
            const std::string t = scratch();
            emit("ABS " + t + ", " + base);
            base = t, o.cls = 0, o.key = t;
            negate = s.modifier == 12;
            break;
        }
        default: fail("source modifier " + std::to_string(s.modifier));
        }
        if (scalar) {
            base += std::string(".") + "xyzw"[swizzle[3]];
            o.scalar = true;
        } else {
            base += swizzle_text(swizzle);
        }
        o.text = (negate ? "-" : "") + base;
        return o;
    }
    // Column `reg` of a column-major matrix, gathered from the rows the
    // engine uploaded (once, at the program's start).
    std::string column(int reg) {
        const Column& col = columns_.at(reg);
        if (transposed_.insert(col.base).second) {
            for (const auto& [r, c] : columns_) {
                if (c.base != col.base) continue;
                const std::string t = "m" + std::to_string(r);
                temps_.insert(t);
                for (int i = 0; i < c.rows; i++) {
                    consts_.insert(c.base + i);
                    prologue_.push_back(std::string("MOV ") + t + "." + "xyzw"[i] + ", c[" + std::to_string(c.base + i) +
                                        "]." + "xyzw"[c.column] + ";");
                }
            }
        }
        return "m" + std::to_string(reg);
    }

    // One component of a source, replicated.
    Operand component(const Source& s, int k) {
        Source c = s;
        std::fill(c.swizzle, c.swizzle + 4, s.swizzle[k]);
        return src(c, true);
    }
    static Operand negated(Operand o) {
        o.text = o.text[0] == '-' ? o.text.substr(1) : "-" + o.text;
        return o;
    }

    // ARB lets an instruction read one program parameter and one attribute
    // (each possibly more than once): others go through temporaries.
    void limit(std::vector<Operand>& ops) {
        std::string par, att;
        for (Operand& o : ops) {
            std::string* seen = o.cls == 1 ? &par : o.cls == 2 ? &att : nullptr;
            if (!seen) continue;
            if (seen->empty()) {
                *seen = o.key;
            } else if (*seen != o.key) {
                const std::string t = scratch();
                const bool scalar = o.scalar;
                emit("MOV " + t + ", " + o.text);
                o = temp(scalar ? t + ".x" : t);
                o.scalar = scalar;
            }
        }
    }
    static std::string join(const std::vector<Operand>& ops) {
        std::string s;
        for (const Operand& o : ops) s += ", " + o.text;
        return s;
    }
    // A plain instruction (no condition, no saturation): scratch results.
    void ins(const std::string& op, const std::string& dst, std::vector<Operand> ops, const std::string& suffix = "") {
        limit(ops);
        emit(op + " " + dst + join(ops) + suffix);
    }

    std::string dest_name(const Dest& d) {
        switch (d.type) {
        case kTemp: {
            const std::string n = "r" + std::to_string(d.num);
            temps_.insert(n);
            return n;
        }
        case kOutput:
            if (vs_) {
                if (!outputs_.count(d.num)) fail("undeclared output o" + std::to_string(d.num));
                const std::string n = "o" + std::to_string(d.num);
                temps_.insert(n);
                return n;
            }
            break;
        case kColorOut:
            if (!vs_ && d.num < 4) {
                colors_.insert(d.num);
                const std::string n = "oc" + std::to_string(d.num);
                temps_.insert(n);
                return n;
            }
            break;
        case kDepthOut:
            if (!vs_) {
                depth_out_ = true;
                temps_.insert("odepth");
                return "odepth";
            }
            break;
        case kAddr:
            if (vs_) {
                temps_.insert("ar");
                return "ar";
            }
            break;
        }
        fail("destination register type " + std::to_string(d.type));
        return "r0";
    }

    void clamp(const std::string& name, const std::string& mask) {
        emit("MAX " + name + mask + ", " + name + ", k.x");
        emit("MIN " + name + mask + ", " + name + ", k.y");
    }
    // The instruction's result into its destination: saturated, shifted,
    // and inside if blocks merged under the block's mask.
    void write(const std::string& op, const Dest& d, std::vector<Operand> ops, const std::string& suffix = "") {
        if (d.shift) fail("result shift");
        limit(ops);
        const std::string name = dest_name(d), mask = mask_text(d.mask);
        const std::string target = cond_.empty() ? name : scratch();
        if (d.saturate && !vs_) {
            emit(op + "_SAT " + target + mask + join(ops) + suffix);
        } else {
            emit(op + " " + target + mask + join(ops) + suffix);
            if (d.saturate) clamp(target, mask);
        }
        if (cond_.empty()) return;
        const std::string& m = cond_.back();
        if (!vs_ || o_.vertex_cmp) {
            emit("CMP " + name + mask + ", -" + m + ", " + target + ", " + name);
        } else {
            const std::string u = scratch();
            emit("SUB " + u + ", " + target + ", " + name);
            emit("MAD " + name + mask + ", " + u + ", " + m + ", " + name);
        }
    }

    void open_block(const std::string& reg) {
        const std::string outer = cond_.empty() ? "" : cond_.back();
        if (outer.empty()) emit("MOV " + reg + ".x, " + reg + ".y");
        else emit("MUL " + reg + ".x, " + reg + ".y, " + outer);
        blocks_.push_back({reg, outer});
        cond_.push_back(reg + ".x");
    }
    std::string block_reg() {
        const std::string r = "p" + std::to_string(blocks_.size());
        temps_.insert(r);
        return r;
    }

    // Instructions [begin, end): rep bodies are emitted once per iteration.
    void emit_range(size_t begin, size_t end) {
        for (size_t i = begin; i < end && err_.empty(); i++) {
            const Instruction& in = p_.code[i];
            if (in.op == kRep) {
                size_t depth = 0, j = i + 1;
                for (; j < end; j++) {
                    if (p_.code[j].op == kRep) depth++;
                    else if (p_.code[j].op == kEndRep && depth-- == 0) break;
                }
                if (j >= end) return fail("rep without endrep");
                if (in.src.empty() || in.src[0].type != kConstInt || !idef_.count(in.src[0].num))
                    return fail("rep count not a defined integer constant");
                const int n = idef_[in.src[0].num][0];
                if (n < 0 || n > 255) return fail("rep count " + std::to_string(n));
                for (int k = 0; k < n; k++) emit_range(i + 1, j);
                i = j;
                continue;
            }
            scratch_ = 0;
            instruction(in);
        }
    }

    void instruction(const Instruction& in) {
        if (in.predicated) return fail("predicated instruction");
        const Dest& d = in.dst;
        auto s = [&](size_t k) -> const Source& {
            static const Source none;
            if (k >= in.src.size()) {
                fail("missing source");
                return none;
            }
            return in.src[k];
        };
        switch (in.op) {
        case kNop: case kDcl: case kDef: case kDefI: case kDefB:
            return;
        case kMov: case kAbs: case kFrc:
            return write(in.op == kMov ? "MOV" : in.op == kAbs ? "ABS" : "FRC", d, {src(s(0))});
        case kAdd: case kSub: case kMul: case kDp3: case kDp4: case kMin: case kMax: case kSlt: case kSge: case kDst: {
            static const std::map<int, const char*> names = {{kAdd, "ADD"}, {kSub, "SUB"}, {kMul, "MUL"},
                {kDp3, "DP3"}, {kDp4, "DP4"}, {kMin, "MIN"}, {kMax, "MAX"}, {kSlt, "SLT"}, {kSge, "SGE"}, {kDst, "DST"}};
            return write(names.at(in.op), d, {src(s(0)), src(s(1))});
        }
        case kMad:
            return write("MAD", d, {src(s(0)), src(s(1)), src(s(2))});
        case kLit:
            return write("LIT", d, {src(s(0))});
        case kRcp: return write("RCP", d, {guarded(src(s(0), true), true)});
        case kRsq: return write("RSQ", d, {guarded(src(s(0), true), true)});
        case kExp: return write("EX2", d, {guarded(src(s(0), true), false)});
        case kLog: return write("LG2", d, {guarded(src(absolute(s(0)), true), true)});
        case kPow: return write("POW", d, {guarded(src(absolute(s(0)), true), true), guarded(src(s(1), true), false)});
        case kCrs: return write("XPD", d, {src(s(0)), src(s(1))});
        case kSgn: {
            const Operand a = src(s(0));
            const std::string p = scratch(), n = scratch();
            ins("SLT", p, {param("k.x", "k"), a});
            ins("SLT", n, {a, param("k.x", "k")});
            return write("SUB", d, {temp(p), temp(n)});
        }
        case kLrp:
            if (!vs_) return write("LRP", d, {src(s(0)), src(s(1)), src(s(2))});
            else {
                const Operand a = src(s(0)), b = src(s(1)), c = src(s(2));
                const std::string t = scratch();
                ins("SUB", t, {b, c});
                return write("MAD", d, {a, temp(t), c});
            }
        case kCmp:
            if (!vs_) return write("CMP", d, {src(s(0)), src(s(2)), src(s(1))});
            else {
                const Operand a = src(s(0)), b = src(s(1)), c = src(s(2));
                const std::string ge = scratch(), diff = scratch();
                ins("SGE", ge, {a, param("k.x", "k")});
                ins("SUB", diff, {b, c});
                return write("MAD", d, {temp(ge), temp(diff), c});
            }
        case kNrm: {
            const Operand a = src(s(0));
            const std::string t = scratch();
            ins("DP3", t + ".x", {a, a});
            emit("RSQ " + t + ".x, " + guarded(temp(t + ".x"), true).text);
            return write("MUL", d, {a, temp(t + ".x")});
        }
        case kDp2Add: {
            const std::string t = scratch();
            ins("MUL", t + ".x", {component(s(0), 0), component(s(1), 0)});
            ins("MAD", t + ".x", {component(s(0), 1), component(s(1), 1), temp(t + ".x")});
            return write("ADD", d, {temp(t + ".x"), src(s(2), true)});
        }
        case kSinCos:
            return sincos(d, src(s(0), true));
        case kDsx: case kDsy:
            if (vs_ || !o_.derivatives) return fail("opcode " + std::to_string(in.op));
            return write(in.op == kDsx ? "DDX" : "DDY", d, {src(s(0))});
        case kM4x4: case kM4x3: case kM3x4: case kM3x3: case kM3x2: {
            const int rows = in.op == kM4x4 || in.op == kM3x4 ? 4 : in.op == kM3x2 ? 2 : 3;
            const char* dp = in.op == kM4x4 || in.op == kM4x3 ? "DP4" : "DP3";
            const Operand a = src(s(0));
            const std::string t = scratch();
            for (int r = 0; r < rows; r++) {
                Source row = s(1);
                row.num += r;
                ins(dp, t + "." + "xyzw"[r], {a, src(row)});
            }
            Dest m = d;
            m.mask &= (1 << rows) - 1;
            return write("MOV", m, {temp(t)});
        }
        case kMova:
            if (!vs_) return fail("mova in a fragment program");
            // ARL truncates towards minus infinity; mova rounds to nearest.
            return write("ADD", d, {src(s(0)), param("k.z", "k")});
        case kTexKill: {
            if (vs_) return fail("texkill in a vertex program");
            Source r;
            r.type = d.type, r.num = d.num;
            int last = 0;
            for (int k = 0, n = 0; k < 4; k++)
                if (d.mask >> k & 1) r.swizzle[n++] = (uint8_t)k, last = k;
            for (int k = 0, n = __builtin_popcount(d.mask); k < 4; k++)
                if (k >= n) r.swizzle[k] = (uint8_t)last;
            Operand x = src(r);
            if (!cond_.empty()) {
                const std::string t = scratch();
                ins("CMP", t, {temp("-" + cond_.back()), x, param("k.y", "k")});
                x = temp(t);
            }
            return emit("KIL " + x.text);
        }
        case kTex: case kTexLdl: {
            if (vs_) return fail("texture fetch in a vertex program");
            if (in.src.size() < 2 || in.src[1].type != kSampler) return fail("texture source");
            const int unit = in.src[1].num;
            auto it = targets_.find(unit);
            if (it == targets_.end()) return fail("undeclared sampler s" + std::to_string(unit));
            // texldl's level of detail is not expressible in ARB: implicit LOD.
            const char* op = in.op == kTex && in.control == 1 ? "TXP" : in.op == kTex && in.control == 2 ? "TXB" : "TEX";
            const std::string suffix = ", texture[" + std::to_string(unit) + "], " + it->second;
            const std::string swz = swizzle_text(in.src[1].swizzle);
            if (swz.empty()) return write(op, d, {src(s(0))}, suffix);
            const std::string t = scratch();
            ins(op, t, {src(s(0))}, suffix);
            return write("MOV", d, {temp(t + swz)});
        }
        case kIf: {
            const Source& b = s(0);
            if (b.type != kConstBool) return fail("if on a non-boolean register");
            const std::string reg = block_reg();
            if (bdef_.count(b.num)) {
                emit("MOV " + reg + ".y, " + (bdef_[b.num] ? "k.y" : "k.x"));
            } else {
                auto it = o_.bool_registers.find(b.num);
                if (it == o_.bool_registers.end()) return fail("bool register b" + std::to_string(b.num) + " has no float register");
                consts_.insert(it->second);
                emit("ABS " + reg + ".y, c[" + std::to_string(it->second) + "].x");
                emit("SLT " + reg + ".y, -" + reg + ".y, k.x");
            }
            return open_block(reg);
        }
        case kIfc: {
            const Operand a = component(s(0), 0), b = component(s(1), 0);
            const std::string reg = block_reg(), y = reg + ".y", z = reg + ".z";
            switch (in.control) {
            case 1: ins("SLT", y, {b, a}); break;  // >
            case 3: ins("SGE", y, {a, b}); break;  // >=
            case 4: ins("SLT", y, {a, b}); break;  // <
            case 6: ins("SGE", y, {b, a}); break;  // <=
            case 2: case 5:                        // ==, !=
                ins("SGE", y, {a, b});
                ins("SGE", z, {b, a});
                emit("MUL " + y + ", " + y + ", " + z);
                if (in.control == 5) emit("SUB " + y + ", k.y, " + y);
                break;
            default: return fail("ifc comparison " + std::to_string(in.control));
            }
            return open_block(reg);
        }
        case kElse: {
            if (blocks_.empty()) return fail("else without if");
            const Block& b = blocks_.back();
            emit("SUB " + b.reg + ".x, k.y, " + b.reg + ".y");
            if (!b.outer.empty()) emit("MUL " + b.reg + ".x, " + b.reg + ".x, " + b.outer);
            return;
        }
        case kEndIf:
            if (blocks_.empty()) return fail("endif without if");
            blocks_.pop_back();
            cond_.pop_back();
            return;
        default:
            return fail("opcode " + std::to_string(in.op));
        }
    }

    // Inside an if block of a vertex program, an operand of RCP, RSQ, LG2,
    // EX2 or POW as the instruction may see it: where the block's mask is 0,
    // 1 (`one`) or 0 instead, so a branch not taken yields no infinity, which
    // the merge (x + mask * (new - x)) would turn into NaN. Fragment
    // programs merge with CMP, which needs no such care.
    Operand guarded(const Operand& a, bool one) {
        if (!vs_ || o_.vertex_cmp || cond_.empty()) return a;
        const std::string& m = cond_.back();
        const std::string t = scratch();
        if (one) {
            emit("SUB " + t + ".y, k.y, " + m);
            ins("MAD", t + ".x", {a, temp(m), temp(t + ".y")});
        } else {
            ins("MUL", t + ".x", {a, temp(m)});
        }
        Operand o = temp(t + ".x");
        o.scalar = true;
        return o;
    }

    static Source absolute(Source s) {
        if (s.modifier == 0 || s.modifier == 1) s.modifier = 11;
        else if (s.modifier == 12) s.modifier = 11;
        return s;
    }

    void sincos(const Dest& d, const Operand& a) {
        const std::string t = scratch();
        if (!vs_) {
            ins("SCS", t + ".xy", {a});
            return write("MOV", d, {temp(t)});
        }
        // Vertex programs have no SCS: series for half the angle (|x| <=
        // pi/2 for the |x| <= pi sincos allows), then the double-angle formulas.
        sincos_consts_ = true;
        const std::string h = scratch(), q = scratch(), w = scratch(), sn = scratch(), cs = scratch();
        ins("MUL", h + ".x", {a, param("k.z", "k")});
        emit("MUL " + q + ".x, " + h + ".x, " + h + ".x");
        // sin h = h (1 - q/6 (1 - q/20 (1 - q/42 (1 - q/72))))
        ins("MAD", w + ".x", {temp("-" + q + ".x"), param("ksin.w", "ksin"), param("k.y", "k")});
        const char* sin_terms[] = {"ksin.z", "ksin.y", "ksin.x"};
        for (const char* c : sin_terms) {
            emit("MUL " + w + ".x, " + w + ".x, " + q + ".x");
            ins("MAD", w + ".x", {temp("-" + w + ".x"), param(c, "ksin"), param("k.y", "k")});
        }
        emit("MUL " + sn + ".x, " + w + ".x, " + h + ".x");
        // cos h = 1 - q/2 (1 - q/12 (1 - q/30 (1 - q/56)))
        ins("MAD", w + ".x", {temp("-" + q + ".x"), param("kcos.w", "kcos"), param("k.y", "k")});
        const char* cos_terms[] = {"kcos.z", "kcos.y", "kcos.x"};
        for (const char* c : cos_terms) {
            emit("MUL " + w + ".x, " + w + ".x, " + q + ".x");
            ins("MAD", w + ".x", {temp("-" + w + ".x"), param(c, "kcos"), param("k.y", "k")});
        }
        emit("MOV " + cs + ".x, " + w + ".x");
        // sin x = 2 sin h cos h, cos x = 1 - 2 sin^2 h
        emit("MUL " + t + ".y, " + sn + ".x, " + cs + ".x");
        emit("ADD " + t + ".y, " + t + ".y, " + t + ".y");
        emit("MUL " + t + ".x, " + sn + ".x, " + sn + ".x");
        ins("MAD", t + ".x", {temp("-" + t + ".x"), param("k.w", "k"), param("k.y", "k")});
        return write("MOV", d, {temp(t)});
    }

    std::string assemble() {
        std::string out = vs_ ? "!!ARBvp1.0\n" : "!!ARBfp1.0\n";
        if (shadow_) out += "OPTION ARB_fragment_program_shadow;\n";
        bool mrt = false;
        for (int c : colors_) mrt |= c > 0;
        if (mrt) out += "OPTION ARB_draw_buffers;\n";
        out += std::string("# translated from D3D9 ") + (vs_ ? "vs" : "ps") + "_" + std::to_string(p_.major) + "_" +
               std::to_string(p_.minor) + " (scimitar d3d9_arb)\n";

        // c[]: every float register read, literals where the bytecode defines them.
        int n = consts_.empty() ? 0 : *consts_.rbegin() + 1;
        if (relative_)
            for (const Constant& k : p_.constants)
                if (k.set == RegisterSet::Float4) n = std::max(n, k.reg + k.count);
        if (n > 0) {
            std::vector<std::string> items;
            int run_start = -1;
            std::string run_kind;
            auto flush = [&](int end) {
                if (run_start < 0) return;
                items.push_back("program." + run_kind + "[" + std::to_string(run_start) +
                                (end - 1 > run_start ? ".." + std::to_string(end - 1) : "") + "]");
                run_start = -1;
            };
            for (int r = 0; r < n; r++) {
                auto it = fdef_.find(r);
                if (it != fdef_.end()) {
                    flush(r);
                    const auto& v = it->second;
                    items.push_back("{ " + fmt_float(v[0]) + ", " + fmt_float(v[1]) + ", " + fmt_float(v[2]) + ", " +
                                    fmt_float(v[3]) + " }");
                    continue;
                }
                const std::string kind = !vs_ && r >= 128 ? "local" : "env";
                if (run_start >= 0 && kind != run_kind) flush(r);
                if (run_start < 0) run_start = r, run_kind = kind;
            }
            flush(n);
            out += "PARAM c[" + std::to_string(n) + "] = { ";
            for (size_t k = 0; k < items.size(); k++) out += (k ? ",\n    " : "") + items[k];
            out += " };\n";
        }
        out += "PARAM k = { 0, 1, 0.5, 2 };\n";
        if (sincos_consts_) {
            out += "PARAM ksin = { " + fmt_float(1.0f / 6) + ", " + fmt_float(1.0f / 20) + ", " + fmt_float(1.0f / 42) +
                   ", " + fmt_float(1.0f / 72) + " };\n";
            out += "PARAM kcos = { " + fmt_float(1.0f / 2) + ", " + fmt_float(1.0f / 12) + ", " + fmt_float(1.0f / 30) +
                   ", " + fmt_float(1.0f / 56) + " };\n";
        }
        for (const std::string& t : temps_) out += "TEMP " + t + ";\n";
        if (relative_) out += "ADDRESS A0;\n";
        for (const std::string& l : prologue_) out += l + "\n";
        for (const std::string& l : body_) out += l + "\n";
        if (vs_) {
            for (const auto& [num, binding] : outputs_) {
                const std::string o = "o" + std::to_string(num);
                if (!temps_.count(o)) continue;
                if (binding == "result.fogcoord" || binding == "result.pointsize")
                    out += "MOV " + binding + ".x, " + o + ".x;\n";
                else
                    out += "MOV " + binding + ", " + o + ";\n";
            }
        } else {
            for (int c : colors_)
                out += "MOV result.color" + (mrt ? "[" + std::to_string(c) + "]" : std::string()) + ", oc" +
                       std::to_string(c) + ";\n";
            if (depth_out_) out += "MOV result.depth.z, odepth.x;\n";
        }
        out += "END\n";
        return out;
    }
};

}  // namespace

Program parse(const uint32_t* t, size_t count) {
    Program p;
    if (count < 2) {
        p.error = "no bytecode";
        return p;
    }
    const uint32_t kind = t[0] >> 16;
    if (kind != 0xfffe && kind != 0xffff) {
        p.error = "not a shader";
        return p;
    }
    p.vertex = kind == 0xfffe;
    p.major = (int)(t[0] >> 8 & 0xff);
    p.minor = (int)(t[0] & 0xff);
    size_t i = 1;
    while (i < count) {
        const uint32_t tok = t[i];
        if (tok == 0x0000ffff) return p;
        const int op = (int)(tok & 0xffff);
        if (op == 0xfffe) {
            const size_t n = (tok >> 16) & 0x7fff;
            if (i + 1 + n > count) break;
            if (n >= 1 && t[i + 1] == 0x42415443)  // "CTAB"
                parse_ctab(reinterpret_cast<const uint8_t*>(t + i + 2), (n - 1) * 4, p);
            i += 1 + n;
            continue;
        }
        const size_t len = (tok >> 24) & 0xf;
        if (i + 1 + len > count) break;
        const uint32_t* a = t + i + 1;
        Instruction in;
        in.op = op;
        in.control = (int)((tok >> 16) & 0xff);
        in.predicated = (tok >> 28) & 1;
        if (op == kDcl && len >= 2) {
            in.usage = (int)(a[0] & 0x1f);
            in.usage_index = (int)((a[0] >> 16) & 0xf);
            in.texture_type = (int)((a[0] >> 27) & 0xf);
            in.dst = dest_token(a[1]);
            in.has_dest = true;
        } else if ((op == kDef || op == kDefI) && len >= 5) {
            in.dst = dest_token(a[0]);
            in.has_dest = true;
            std::memcpy(op == kDef ? (void*)in.fvalue : (void*)in.ivalue, a + 1, 16);
        } else if (op == kDefB && len >= 2) {
            in.dst = dest_token(a[0]);
            in.has_dest = true;
            in.ivalue[0] = (int)a[1];
        } else {
            size_t k = 0;
            if (!has_no_dest(op) && len > 0) {
                in.dst = dest_token(a[k++]);
                in.has_dest = true;
                if (a[0] >> 13 & 1) {
                    p.error = "relative destination";
                    k++;
                }
            }
            while (k < len) {
                Source s = source_token(a[k++]);
                if (s.relative && k < len) {
                    s.rel_type = reg_type(a[k]);
                    s.rel_num = (int)(a[k] & 0x7ff);
                    s.rel_component = (int)((a[k] >> 16) & 3);
                    k++;
                }
                in.src.push_back(s);
            }
        }
        p.code.push_back(in);
        i += 1 + len;
    }
    if (p.error.empty()) p.error = "bytecode ends without its end token";
    return p;
}

int vertex_attribute(int usage, int index, bool separate) {
    switch (usage) {
    case kPosition: case kPositionT: return index ? -1 : 0;
    case kBlendWeight: return 1;
    case kBlendIndices: return 7;
    case kNormal: return 2;
    case kPSize: return 6;
    case kTexCoord:
        if (index < 8) return 8 + index;
        return separate && index < 16 ? 18 + index - 8 : -1;
    case kTangent: return separate ? 16 : 14;
    case kBinormal: return separate ? 17 : 15;
    case kColor: return index < 2 ? 3 + index : -1;
    case kFog: return 5;
    }
    return -1;
}

std::string to_arb(const Program& program, const ArbOptions& options, std::string* error) {
    Translator t(program, options);
    return t.run(error);
}

std::string fallback_arb(const Program& program) {
    if (program.vertex)
        return "!!ARBvp1.0\n# untranslated D3D9 vertex shader\nMOV result.position, { 0, 0, 0, 1 };\nEND\n";
    return "!!ARBfp1.0\n# untranslated D3D9 pixel shader\nMOV result.color, { 1, 0, 1, 1 };\nEND\n";
}

}  // namespace scimitar::d3d9
