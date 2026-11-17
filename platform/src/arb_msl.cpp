// ARB vertex and fragment programs to Metal Shading Language (see
// scimitar/arb_msl.h and docs/METAL.md). A program is tokenized, parsed into
// instructions over resolved operands, then written out as one MSL function:
// each ARB instruction becomes a block that reads its operands into locals,
// computes a float4 and stores the written components, which keeps ARB's
// semantics (operands read before the destination changes, write masks,
// replicated scalar results) without analysis; Metal's compiler folds the rest.
//
// Semantics follow the ARB specs as NVIDIA's drivers ran cgc's output:
// uninitialised temporaries read 0 and outputs start at (0, 0, 0, 1); RSQ and
// LG2 use |x|; ARL floors; vertex colours are clamped to [0, 1]; relative
// addressing is clamped to the array.
#include "scimitar/arb_msl.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace scimitar::arb {

namespace {

struct Fail {
    int line;
    std::string message;
};

// ---- tokens ----

struct Tok {
    enum Kind { Ident, Number, Punct, End } kind = End;
    std::string text;
    double value = 0;
    int line = 0;
};

std::vector<Tok> tokenize(std::string_view s, int line) {
    std::vector<Tok> out;
    size_t i = 0;
    auto alnum = [&](size_t k) { return k < s.size() && (std::isalnum((unsigned char)s[k]) || s[k] == '_'); };
    auto digit = [&](size_t k) { return k < s.size() && std::isdigit((unsigned char)s[k]); };
    while (i < s.size()) {
        const char ch = s[i];
        if (ch == '\n') {
            line++, i++;
            continue;
        }
        if (std::isspace((unsigned char)ch)) {
            i++;
            continue;
        }
        if (ch == '#') {
            while (i < s.size() && s[i] != '\n') i++;
            continue;
        }
        Tok t;
        t.line = line;
        const bool prev_is_operand =
            !out.empty() && (out.back().kind == Tok::Ident || out.back().text == "]" || out.back().kind == Tok::Number);
        if (std::isalpha((unsigned char)ch) || ch == '_' || ch == '$') {
            size_t e = i + 1;
            while (alnum(e) || (e < s.size() && s[e] == '$')) e++;
            t.kind = Tok::Ident, t.text = std::string(s.substr(i, e - i));
            i = e;
        } else if (digit(i) && i + 1 < s.size() && s[i + 1] == 'D' && !alnum(i + 2)) {
            t.kind = Tok::Ident, t.text = std::string(s.substr(i, 2));  // 1D, 2D, 3D
            i += 2;
        } else if (digit(i) || (ch == '.' && digit(i + 1) && !prev_is_operand)) {
            size_t e = i;
            while (digit(e)) e++;
            if (e < s.size() && s[e] == '.' && !(e + 1 < s.size() && s[e + 1] == '.')) {
                e++;
                while (digit(e)) e++;
            }
            if (e < s.size() && (s[e] == 'e' || s[e] == 'E')) {
                size_t x = e + 1;
                if (x < s.size() && (s[x] == '+' || s[x] == '-')) x++;
                if (digit(x)) {
                    e = x;
                    while (digit(e)) e++;
                }
            }
            t.kind = Tok::Number, t.text = std::string(s.substr(i, e - i));
            t.value = std::strtod(t.text.c_str(), nullptr);
            i = e;
        } else if (ch == '.' && i + 1 < s.size() && s[i + 1] == '.') {
            t.kind = Tok::Punct, t.text = "..";
            i += 2;
        } else {
            t.kind = Tok::Punct, t.text = std::string(1, ch);
            i++;
        }
        out.push_back(std::move(t));
    }
    Tok end;
    end.line = line;
    out.push_back(end);
    return out;
}

// ---- operands ----

// Fragment inputs: the varyings, then the position.
constexpr int kInPosition = 31;
// Vertex outputs: the varyings, then the position. Fragment outputs: colours
// 0-3, then the depth.
constexpr int kOutPosition = 31;
// texcoord[n] as a varying index: 0-7, then 8-15 after the colours and fog.
int texcoord_varying(int n) { return n < 8 ? n : (int)kVaryingTexCoord8 + n - 8; }
constexpr int kOutDepth = 4;

enum class SrcKind : uint8_t { Temp, Attrib, FragIn, Param, ParamRel };

struct Source {
    SrcKind kind = SrcKind::Temp;
    int index = 0;      // temp, attribute, input; array for ParamRel
    int offset = 0;     // ParamRel: constant offset
    int addr = 0;       // ParamRel: address register
    int addr_comp = 0;  // ParamRel: its component
    ParamRef ref;       // Param
    uint8_t swz[4] = {0, 1, 2, 3};  // 4 = 0.0, 5 = 1.0 (SWZ)
    uint8_t neg = 0;                // negated components
};

struct Dest {
    enum Kind : uint8_t { Temp, Out, Addr } kind = Temp;
    int index = 0;
    uint8_t mask = 0xf;
};

enum class OpKind : uint8_t { Vector, Scalar, Pow, Tex, Kil, Arl, Swz };

struct OpInfo {
    const char* name;
    int nsrc;
    OpKind kind;
    bool vp, fp;
};

const OpInfo kOps[] = {
    {"ABS", 1, OpKind::Vector, true, true},  {"ADD", 2, OpKind::Vector, true, true},
    {"ARL", 1, OpKind::Arl, true, false},    {"CMP", 3, OpKind::Vector, true, true},
    {"COS", 1, OpKind::Scalar, false, true}, {"DDX", 1, OpKind::Vector, false, true},
    {"DDY", 1, OpKind::Vector, false, true},  {"DP3", 2, OpKind::Vector, true, true},
    {"DP4", 2, OpKind::Vector, true, true},  {"DPH", 2, OpKind::Vector, true, true},
    {"DST", 2, OpKind::Vector, true, true},  {"EX2", 1, OpKind::Scalar, true, true},
    {"EXP", 1, OpKind::Scalar, true, false}, {"FLR", 1, OpKind::Vector, true, true},
    {"FRC", 1, OpKind::Vector, true, true},  {"KIL", 1, OpKind::Kil, false, true},
    {"LG2", 1, OpKind::Scalar, true, true},  {"LIT", 1, OpKind::Vector, true, true},
    {"LOG", 1, OpKind::Scalar, true, false}, {"LRP", 3, OpKind::Vector, false, true},
    {"MAD", 3, OpKind::Vector, true, true},  {"MAX", 2, OpKind::Vector, true, true},
    {"MIN", 2, OpKind::Vector, true, true},  {"MOV", 1, OpKind::Vector, true, true},
    {"MUL", 2, OpKind::Vector, true, true},  {"POW", 2, OpKind::Pow, true, true},
    {"RCP", 1, OpKind::Scalar, true, true},  {"RSQ", 1, OpKind::Scalar, true, true},
    {"SCS", 1, OpKind::Scalar, false, true}, {"SGE", 2, OpKind::Vector, true, true},
    {"SIN", 1, OpKind::Scalar, false, true}, {"SLT", 2, OpKind::Vector, true, true},
    {"SUB", 2, OpKind::Vector, true, true},  {"SWZ", 1, OpKind::Swz, true, true},
    {"TEX", 1, OpKind::Tex, false, true},    {"TXB", 1, OpKind::Tex, false, true},
    {"TXP", 1, OpKind::Tex, false, true},    {"XPD", 2, OpKind::Vector, true, true},
};

struct Inst {
    const OpInfo* op = nullptr;
    bool sat = false;
    Dest dst;
    Source src[3];
    int unit = 0;
    TexTarget target = TexTarget::None;
    int line = 0;
};

struct Symbol {
    enum Kind : uint8_t { Temp, Address, Param, Array, Attrib, Output } kind = Temp;
    int index = 0;  // temp, address register, array
    ParamRef ref;   // Param
    Source src;     // Attrib (an ATTRIB alias)
    Dest dst;       // Output (an OUTPUT alias)
};

// ---- parser ----

class Parser {
public:
    Parser(std::vector<Tok> toks, Stage stage) : t_(std::move(toks)), stage_(stage) {}

    void parse() {
        for (;;) {
            if (peek().kind == Tok::End) fail("missing END");
            if (peek().kind == Tok::Ident && peek().text == "END") return;
            statement();
        }
    }

    std::vector<Inst> insts;
    std::vector<std::vector<ParamRef>> arrays;
    int temps = 0, addresses = 0;
    bool shadow_option = false, draw_buffers_option = false;

private:
    std::vector<Tok> t_;
    size_t p_ = 0;
    Stage stage_;
    std::map<std::string, Symbol> syms_;

    [[noreturn]] void fail(const std::string& m) const { throw Fail{peek().line, m}; }
    const Tok& peek(size_t k = 0) const { return t_[std::min(p_ + k, t_.size() - 1)]; }
    const Tok& next() {
        const Tok& t = peek();
        if (p_ < t_.size() - 1) p_++;
        return t;
    }
    bool is(const char* punct, size_t k = 0) const { return peek(k).kind == Tok::Punct && peek(k).text == punct; }
    bool accept(const char* punct) {
        if (!is(punct)) return false;
        next();
        return true;
    }
    void expect(const char* punct) {
        if (!accept(punct)) fail(std::string("expected '") + punct + "', found '" + peek().text + "'");
    }
    std::string ident() {
        if (peek().kind != Tok::Ident) fail("expected a name, found '" + peek().text + "'");
        return next().text;
    }
    bool accept_ident(const char* name) {
        if (peek().kind != Tok::Ident || peek().text != name) return false;
        next();
        return true;
    }
    int integer() {
        if (peek().kind != Tok::Number) fail("expected a number, found '" + peek().text + "'");
        const double v = next().value;
        if (v != std::floor(v) || v < 0 || v > 65535) fail("bad index");
        return (int)v;
    }
    int bracket_index() {
        expect("[");
        const int n = integer();
        expect("]");
        return n;
    }
    float signed_number() {
        float sign = 1;
        if (accept("-")) sign = -1;
        else accept("+");
        if (peek().kind != Tok::Number) fail("expected a number, found '" + peek().text + "'");
        return sign * (float)next().value;
    }
    void define(const std::string& name, const Symbol& s) {
        if (!syms_.emplace(name, s).second) fail("'" + name + "' declared twice");
    }

    void statement() {
        const std::string word = ident();
        if (word == "OPTION") {
            const std::string o = ident();
            if (o == "ARB_fragment_program_shadow") shadow_option = true;
            else if (o == "ARB_draw_buffers") draw_buffers_option = true;
            else if (o == "ARB_position_invariant") fail("ARB_position_invariant is not supported");
            else if (o != "ARB_precision_hint_fastest" && o != "ARB_precision_hint_nicest") fail("unknown option " + o);
        } else if (word == "TEMP" || word == "ADDRESS") {
            do {
                Symbol s;
                s.kind = word == "TEMP" ? Symbol::Temp : Symbol::Address;
                s.index = word == "TEMP" ? temps++ : addresses++;
                define(ident(), s);
            } while (accept(","));
        } else if (word == "PARAM") {
            param();
        } else if (word == "ATTRIB") {
            const std::string name = ident();
            expect("=");
            Symbol s;
            s.kind = Symbol::Attrib;
            s.src = input_binding();
            define(name, s);
        } else if (word == "OUTPUT") {
            const std::string name = ident();
            expect("=");
            Symbol s;
            s.kind = Symbol::Output;
            s.dst = output_binding();
            define(name, s);
        } else if (word == "ALIAS") {
            const std::string name = ident();
            expect("=");
            const std::string target = ident();
            const auto it = syms_.find(target);
            if (it == syms_.end()) fail("unknown name '" + target + "'");
            define(name, it->second);
        } else {
            instruction(word);
        }
        expect(";");
    }

    // PARAM name = item; or PARAM name[n] = { items };
    void param() {
        const std::string name = ident();
        if (accept("[")) {
            int size = -1;
            if (!is("]")) size = integer();
            expect("]");
            expect("=");
            expect("{");
            std::vector<ParamRef> items;
            do param_items(&items, true);
            while (accept(","));
            expect("}");
            if (size >= 0 && (size_t)size != items.size()) fail("array '" + name + "' has the wrong number of items");
            Symbol s;
            s.kind = Symbol::Array;
            s.index = (int)arrays.size();
            arrays.push_back(std::move(items));
            define(name, s);
        } else {
            expect("=");
            std::vector<ParamRef> items;
            param_items(&items, false);
            Symbol s;
            s.kind = Symbol::Param;
            s.ref = items[0];
            define(name, s);
        }
    }

    // One initializer: a constant, or program.env/local[n] (or [a..b] in arrays).
    void param_items(std::vector<ParamRef>* out, bool in_array) {
        if (is("{")) {
            out->push_back(vector_constant());
            return;
        }
        if (peek().kind == Tok::Number || is("-") || is("+")) {
            ParamRef r;
            r.value[0] = r.value[1] = r.value[2] = r.value[3] = signed_number();
            out->push_back(r);
            return;
        }
        const std::string w = ident();
        if (w == "state") fail("state bindings are not supported");
        if (w != "program") fail("unknown parameter binding '" + w + "'");
        expect(".");
        const std::string f = ident();
        if (f != "env" && f != "local") fail("unknown parameter binding 'program." + f + "'");
        expect("[");
        const int a = integer();
        int b = a;
        if (accept("..")) {
            if (!in_array) fail("a range outside an array");
            b = integer();
        }
        expect("]");
        if (b < a) fail("bad range");
        for (int k = a; k <= b; k++) {
            ParamRef r;
            r.kind = f == "env" ? ParamRef::Env : ParamRef::Local;
            r.index = (uint16_t)k;
            out->push_back(r);
        }
    }

    // { x }, { x, y }... : missing y and z are 0, a missing w is 1.
    ParamRef vector_constant() {
        expect("{");
        ParamRef r;
        r.value[3] = 1;
        int n = 0;
        do {
            if (n == 4) fail("too many components");
            r.value[n++] = signed_number();
        } while (accept(","));
        expect("}");
        return r;
    }

    // The attribute or input a source names (after "vertex." or "fragment.").
    Source input_binding() {
        Source s;
        const std::string w = ident();
        expect(".");
        const std::string m = ident();
        if (w == "vertex") {
            if (stage_ != Stage::Vertex) fail("vertex inputs in a fragment program");
            // NVIDIA's aliasing of the conventional attributes onto generic ones.
            s.kind = SrcKind::Attrib;
            if (m == "attrib") {
                s.index = bracket_index();
                // cgc 2.2 writes "vertex.attrib[N].secondary" for COLOR1
                // inputs; the suffix means nothing.
                if (is(".") && peek(1).kind == Tok::Ident && peek(1).text == "secondary") next(), next();
            } else if (m == "position") s.index = 0;
            else if (m == "weight") s.index = 1;
            else if (m == "normal") s.index = 2;
            else if (m == "fogcoord") s.index = 5;
            else if (m == "texcoord") s.index = 8 + (is("[") ? bracket_index() : 0);
            else if (m == "color") {
                s.index = 3;
                if (is(".") && peek(1).kind == Tok::Ident && (peek(1).text == "primary" || peek(1).text == "secondary")) {
                    next();
                    if (next().text == "secondary") s.index = 4;
                }
            } else fail("unknown vertex input '" + m + "'");
            if (s.index >= 31) fail("attribute out of range");
        } else if (w == "fragment") {
            if (stage_ != Stage::Fragment) fail("fragment inputs in a vertex program");
            s.kind = SrcKind::FragIn;
            if (m == "texcoord") s.index = is("[") ? bracket_index() : 0;
            else if (m == "position") s.index = kInPosition;
            else if (m == "fogcoord") s.index = kVaryingFog;
            else if (m == "color") {
                s.index = kVaryingColor0;
                if (is(".") && peek(1).kind == Tok::Ident && (peek(1).text == "primary" || peek(1).text == "secondary")) {
                    next();
                    if (next().text == "secondary") s.index = kVaryingColor1;
                }
            } else fail("unknown fragment input '" + m + "'");
            if (m == "texcoord" && s.index >= 16) fail("texture coordinate out of range");
            if (m == "texcoord") s.index = texcoord_varying(s.index);
        } else {
            fail("unknown input '" + w + "'");
        }
        return s;
    }

    // result.* (after OUTPUT name = or as a destination), without the mask.
    Dest output_binding() {
        if (ident() != "result") fail("expected result");
        expect(".");
        const std::string m = ident();
        Dest d;
        d.kind = Dest::Out;
        if (stage_ == Stage::Vertex) {
            if (m == "position") d.index = kOutPosition;
            else if (m == "fogcoord") d.index = kVaryingFog;
            else if (m == "texcoord") d.index = is("[") ? bracket_index() : 0;
            else if (m == "color") {
                d.index = kVaryingColor0;
                for (;;) {
                    if (!(is(".") && peek(1).kind == Tok::Ident)) break;
                    const std::string& q = peek(1).text;
                    if (q == "front" || q == "primary") next(), next();
                    else if (q == "secondary") next(), next(), d.index = kVaryingColor1;
                    else if (q == "back") fail("back-face colours are not supported");
                    else break;
                }
            } else fail("unsupported vertex output '" + m + "'");
            if (m == "texcoord" && d.index >= 16) fail("texture coordinate out of range");
            if (m == "texcoord") d.index = texcoord_varying(d.index);
        } else {
            if (m == "color") {
                d.index = 0;
                if (is("[")) {
                    d.index = bracket_index();
                    if (!draw_buffers_option && d.index) fail("result.color[n] without ARB_draw_buffers");
                    if (d.index >= 4) fail("colour output out of range");
                }
            } else if (m == "depth") d.index = kOutDepth;
            else fail("unknown fragment output '" + m + "'");
        }
        return d;
    }

    // ".xyzw"-style suffix: 1 or 4 components (one is replicated).
    void swizzle(Source* s) {
        if (!(is(".") && peek(1).kind == Tok::Ident)) return;
        const std::string w = peek(1).text;
        uint8_t c[4];
        if (w.size() != 1 && w.size() != 4) return;
        for (size_t k = 0; k < w.size(); k++) {
            const int v = component(w[k]);
            if (v < 0) return;
            c[k] = (uint8_t)v;
        }
        next(), next();
        uint8_t composed[4];
        for (int k = 0; k < 4; k++) composed[k] = s->swz[c[w.size() == 1 ? 0 : k]];
        std::memcpy(s->swz, composed, 4);
    }
    static int component(char ch) {
        switch (ch) {
        case 'x': case 'r': return 0;
        case 'y': case 'g': return 1;
        case 'z': case 'b': return 2;
        case 'w': case 'a': return 3;
        }
        return -1;
    }

    Source source() {
        bool negate = false;
        if (accept("-")) negate = true;
        else accept("+");
        Source s;
        if (is("{")) {
            s.kind = SrcKind::Param;
            s.ref = vector_constant();
        } else if (peek().kind == Tok::Number) {
            s.kind = SrcKind::Param;
            const float v = (float)next().value;
            for (float& x : s.ref.value) x = v;
        } else {
            const std::string w = peek().text;
            if (w == "vertex" || w == "fragment") {
                s = input_binding();
            } else if (w == "program") {
                std::vector<ParamRef> one;
                param_items(&one, false);
                s.kind = SrcKind::Param;
                s.ref = one[0];
            } else if (w == "state") {
                fail("state bindings are not supported");
            } else {
                const std::string name = ident();
                const auto it = syms_.find(name);
                if (it == syms_.end()) fail("unknown name '" + name + "'");
                const Symbol& sym = it->second;
                switch (sym.kind) {
                case Symbol::Temp: s.kind = SrcKind::Temp, s.index = sym.index; break;
                case Symbol::Param: s.kind = SrcKind::Param, s.ref = sym.ref; break;
                case Symbol::Attrib: s = sym.src; break;
                case Symbol::Array: s = array_element(sym.index); break;
                default: fail("'" + name + "' cannot be read");
                }
            }
        }
        swizzle(&s);
        if (negate) s.neg = 0xf;
        return s;
    }

    // name[n] or name[A0.x + n]
    Source array_element(int array) {
        const std::vector<ParamRef>& items = arrays[array];
        Source s;
        expect("[");
        if (peek().kind == Tok::Number) {
            const int n = integer();
            if ((size_t)n >= items.size()) fail("array index out of range");
            s.kind = SrcKind::Param;
            s.ref = items[n];
        } else {
            if (stage_ != Stage::Vertex) fail("relative addressing in a fragment program");
            const std::string a = ident();
            const auto it = syms_.find(a);
            if (it == syms_.end() || it->second.kind != Symbol::Address) fail("'" + a + "' is not an address register");
            s.kind = SrcKind::ParamRel;
            s.index = array;
            s.addr = it->second.index;
            expect(".");
            const std::string c = ident();
            if (c.size() != 1 || component(c[0]) != 0) fail("address component must be x");
            s.addr_comp = 0;
            if (accept("+")) s.offset = integer();
            else if (accept("-")) s.offset = -integer();
        }
        expect("]");
        return s;
    }

    Dest dest() {
        Dest d;
        if (peek().kind == Tok::Ident && peek().text == "result") {
            d = output_binding();
        } else {
            const std::string name = ident();
            const auto it = syms_.find(name);
            if (it == syms_.end()) fail("unknown name '" + name + "'");
            switch (it->second.kind) {
            case Symbol::Temp: d.kind = Dest::Temp, d.index = it->second.index; break;
            case Symbol::Address: d.kind = Dest::Addr, d.index = it->second.index; break;
            case Symbol::Output: d = it->second.dst; break;
            default: fail("'" + name + "' cannot be written");
            }
        }
        if (is(".") && peek(1).kind == Tok::Ident) {
            next();
            const std::string m = next().text;
            uint8_t mask = 0;
            int last = -1;
            for (char ch : m) {
                const int c = component(ch);
                if (c <= last) fail("bad write mask '" + m + "'");
                mask |= (uint8_t)(1u << c), last = c;
            }
            d.mask = mask;
        }
        return d;
    }

    void instruction(const std::string& word) {
        Inst in;
        in.line = t_[p_ - 1].line;
        std::string name = word;
        if (name.size() > 4 && name.compare(name.size() - 4, 4, "_SAT") == 0) {
            if (stage_ != Stage::Fragment) fail("_SAT in a vertex program");
            in.sat = true;
            name.resize(name.size() - 4);
        }
        for (const OpInfo& o : kOps)
            if (name == o.name) in.op = &o;
        if (!in.op) fail("unknown instruction '" + word + "'");
        if (!(stage_ == Stage::Vertex ? in.op->vp : in.op->fp)) fail(name + " is not allowed in this program type");
        if (in.op->kind == OpKind::Kil) {
            in.src[0] = source();
            insts.push_back(in);
            return;
        }
        in.dst = dest();
        if (in.op->kind == OpKind::Arl) {
            if (in.dst.kind != Dest::Addr) fail("ARL writes an address register");
        } else if (in.dst.kind == Dest::Addr) {
            fail("only ARL writes address registers");
        }
        for (int k = 0; k < in.op->nsrc; k++) {
            expect(",");
            in.src[k] = source();
        }
        if (in.op->kind == OpKind::Swz) {
            // SWZ dst, src, c0, c1, c2, c3 with each [-]x|y|z|w|0|1
            Source& s = in.src[0];
            const uint8_t base[4] = {s.swz[0], s.swz[1], s.swz[2], s.swz[3]};
            uint8_t neg = s.neg;
            for (int k = 0; k < 4; k++) {
                expect(",");
                bool n = false;
                if (accept("-")) n = true;
                else accept("+");
                if (peek().kind == Tok::Number) {
                    const double v = next().value;
                    if (v != 0 && v != 1) fail("SWZ components are x, y, z, w, 0 or 1");
                    s.swz[k] = v == 0 ? 4 : 5;
                } else {
                    const std::string c = ident();
                    if (c.size() != 1 || component(c[0]) < 0) fail("bad SWZ component '" + c + "'");
                    s.swz[k] = base[component(c[0])];
                }
                if (n) neg ^= (uint8_t)(1u << k);
            }
            s.neg = neg;
        }
        if (in.op->kind == OpKind::Tex) {
            expect(",");
            if (ident() != "texture") fail("expected texture[n]");
            in.unit = is("[") ? bracket_index() : 0;
            if (in.unit >= 16) fail("texture unit out of range");
            expect(",");
            const std::string tg = ident();
            if (tg == "1D") in.target = TexTarget::T1D;
            else if (tg == "2D") in.target = TexTarget::T2D;
            else if (tg == "3D") in.target = TexTarget::T3D;
            else if (tg == "CUBE") in.target = TexTarget::Cube;
            else if (tg == "RECT") in.target = TexTarget::Rect;
            else if (tg == "SHADOW2D") {
                if (!shadow_option) fail("SHADOW2D without ARB_fragment_program_shadow");
                in.target = TexTarget::Shadow2D;
            } else fail("unsupported texture target '" + tg + "'");
        }
        insts.push_back(in);
    }
};

// ---- MSL ----

std::string flt(float v) {
    if (!std::isfinite(v)) {
        uint32_t bits;
        std::memcpy(&bits, &v, 4);
        char b[48];
        std::snprintf(b, sizeof b, "as_type<float>(0x%08xu)", bits);
        return b;
    }
    char b[32];
    std::snprintf(b, sizeof b, "%.9g", (double)v);
    std::string s = b;
    if (s.find_first_of(".e") == std::string::npos) s += ".0";
    return s + "f";
}

const char* const kComp = "xyzw";
const char* const kVaryingName[kVaryingCount] = {"tc0",  "tc1",  "tc2",  "tc3",  "tc4", "tc5", "tc6",
                                                 "tc7",  "col0", "col1", "fog",  "tc8", "tc9", "tc10",
                                                 "tc11", "tc12", "tc13", "tc14", "tc15"};

class Emitter {
public:
    Emitter(const Parser& p, Stage stage, const Options& o, Program* out) : p_(p), stage_(stage), o_(o), out_(out) {}

    void run() {
        for (const Inst& in : p_.insts) instruction(in);
        assemble();
    }

private:
    const Parser& p_;
    Stage stage_;
    const Options& o_;
    Program* out_;
    std::string body_;
    std::map<std::pair<int, int>, int> slots_;  // (kind, index) -> P slot
    std::map<int, int> array_base_;             // relatively addressed array -> first slot
    uint32_t outputs_ = 0;                      // vertex: varyings written (+ bit 31 position)

    int slot(const ParamRef& r) {
        const auto key = std::make_pair((int)r.kind, (int)r.index);
        const auto it = slots_.find(key);
        if (it != slots_.end()) return it->second;
        const int s = (int)out_->params.size();
        out_->params.push_back(r);
        slots_[key] = s;
        return s;
    }

    static std::string literal(const float v[4]) {
        return "float4(" + flt(v[0]) + ", " + flt(v[1]) + ", " + flt(v[2]) + ", " + flt(v[3]) + ")";
    }

    // The source as a float4 expression, swizzled and negated.
    std::string src(const Source& s) {
        if (s.kind == SrcKind::Param && s.ref.kind == ParamRef::Literal) {
            float v[4];
            for (int k = 0; k < 4; k++) {
                v[k] = s.swz[k] == 4 ? 0.0f : s.swz[k] == 5 ? 1.0f : s.ref.value[s.swz[k]];
                if (s.neg >> k & 1) v[k] = -v[k];
            }
            return literal(v);
        }
        std::string base;
        switch (s.kind) {
        case SrcKind::Temp: base = "R" + std::to_string(s.index); break;
        case SrcKind::Attrib: {
            out_->attribs |= 1u << s.index;
            const bool integer = ((o_.uint_attribs | o_.int_attribs) >> s.index) & 1;
            base = integer ? "float4(in.a" + std::to_string(s.index) + ")" : "in.a" + std::to_string(s.index);
            break;
        }
        case SrcKind::FragIn:
            if (s.index == kInPosition) {
                out_->reads_position = true;
                base = o_.integer_pixel_centers ? "float4(in.pos.xy - 0.5f, in.pos.zw)" : "in.pos";
            } else {
                out_->varyings |= 1u << s.index;
                base = std::string("in.") + kVaryingName[s.index];
            }
            break;
        case SrcKind::Param: base = "P[" + std::to_string(slot(s.ref)) + "]"; break;
        case SrcKind::ParamRel: {
            const std::vector<ParamRef>& items = p_.arrays[s.index];
            auto it = array_base_.find(s.index);
            if (it == array_base_.end()) {
                it = array_base_.emplace(s.index, (int)out_->params.size()).first;
                for (const ParamRef& r : items) out_->params.push_back(r);
            }
            base = "P[" + std::to_string(it->second) + " + clamp(A" + std::to_string(s.addr) + "." +
                   kComp[s.addr_comp] + (s.offset ? (s.offset > 0 ? " + " : " - ") + std::to_string(std::abs(s.offset)) : "") +
                   ", 0, " + std::to_string(items.size() - 1) + ")]";
            break;
        }
        }
        const bool identity = s.swz[0] == 0 && s.swz[1] == 1 && s.swz[2] == 2 && s.swz[3] == 3;
        const bool extended = s.swz[0] > 3 || s.swz[1] > 3 || s.swz[2] > 3 || s.swz[3] > 3;
        if (extended || (s.neg && s.neg != 0xf)) {
            std::string e = "float4(";
            for (int k = 0; k < 4; k++) {
                std::string c = s.swz[k] == 4 ? "0.0f" : s.swz[k] == 5 ? "1.0f" : base + "." + kComp[s.swz[k]];
                if (s.neg >> k & 1) c = "-" + c;
                e += (k ? ", " : "") + c;
            }
            return e + ")";
        }
        std::string e = base;
        if (!identity) {
            e += ".";
            for (int k = 0; k < 4; k++) e += kComp[s.swz[k]];
        }
        return s.neg ? "(-" + e + ")" : e;
    }

    std::string dst_name(const Dest& d) {
        if (d.kind == Dest::Temp) return "R" + std::to_string(d.index);
        if (d.kind == Dest::Addr) return "A" + std::to_string(d.index);
        if (stage_ == Stage::Vertex) {
            outputs_ |= 1u << d.index;
            if (d.index == kOutPosition) return "o_pos";
            return std::string("o_") + kVaryingName[d.index];
        }
        if (d.index == kOutDepth) {
            out_->writes_depth = true;
            return "o_depth";
        }
        out_->color_outputs |= 1u << d.index;
        return "o_c" + std::to_string(d.index);
    }

    static std::string mask_suffix(uint8_t m) {
        std::string s;
        for (int k = 0; k < 4; k++)
            if (m >> k & 1) s += kComp[k];
        return s;
    }

    std::string sample(const Inst& in) {
        const std::string u = std::to_string(in.unit);
        const std::string t = "tex" + u, s = "smp" + u;
        const std::string op = in.op->name;
        const bool proj = op == "TXP";
        const std::string bias = op == "TXB" ? "bias(fx.lod_bias[" + u + "] + a.w)" : "bias(fx.lod_bias[" + u + "])";
        auto coord = [&](const char* c) { return proj ? std::string("a.") + c + " / a.w" : std::string("a.") + c; };
        switch (in.target) {
        case TexTarget::T1D: return t + ".sample(" + s + ", " + coord("x") + ")";
        case TexTarget::T2D: return t + ".sample(" + s + ", " + coord("xy") + ", " + bias + ")";
        case TexTarget::T3D:
        case TexTarget::Cube: return t + ".sample(" + s + ", " + coord("xyz") + ", " + bias + ")";
        case TexTarget::Rect:
            return t + ".sample(" + s + ", (" + coord("xy") + ") / float2(" + t + ".get_width(), " + t +
                   ".get_height()), level(0))";
        case TexTarget::Shadow2D:
            return "float4(float3(" + t + ".sample_compare(" + s + ", " + coord("xy") + ", " + coord("z") + ")), 1.0f)";
        case TexTarget::None: break;
        }
        return "float4(0.0f)";
    }

    void instruction(const Inst& in) {
        out_->instructions++;
        const std::string op = in.op->name;
        std::string code = "    {\n";
        static const char* const kArg[3] = {"a", "b", "c"};
        for (int k = 0; k < in.op->nsrc; k++) code += "        const float4 " + std::string(kArg[k]) + " = " + src(in.src[k]) + ";\n";
        if (in.op->kind == OpKind::Kil) {
            out_->kills = true;
            body_ += code + "        if (any(a < 0.0f)) discard_fragment();\n    }\n";
            return;
        }
        if (in.op->kind == OpKind::Arl) {
            body_ += code + "        " + dst_name(in.dst) + "." + mask_suffix(in.dst.mask) + " = int(floor(a.x));\n    }\n";
            return;
        }
        std::string r;
        if (op == "ABS") r = "abs(a)";
        // NV_fragment_program2's derivatives (the D3D9 translator's dsx and
        // dsy): screen x, and y up as GL's window coordinates run, or down as
        // Metal's and Direct3D's do (Options::gl_clip off).
        else if (op == "DDX") r = "dfdx(a)";
        else if (op == "DDY") r = o_.gl_clip ? "-dfdy(a)" : "dfdy(a)";
        else if (op == "ADD") r = "a + b";
        else if (op == "SUB") r = "a - b";
        else if (op == "MUL") r = "a * b";
        else if (op == "MAD") r = "a * b + c";
        else if (op == "MAX") r = "max(a, b)";
        else if (op == "MIN") r = "min(a, b)";
        else if (op == "MOV" || op == "SWZ") r = "a";
        else if (op == "FLR") r = "floor(a)";
        else if (op == "FRC") r = "fract(a)";
        else if (op == "SGE") r = "select(float4(0.0f), float4(1.0f), a >= b)";
        else if (op == "SLT") r = "select(float4(0.0f), float4(1.0f), a < b)";
        else if (op == "CMP") r = "select(c, b, a < 0.0f)";
        else if (op == "LRP") r = "a * b + (1.0f - a) * c";
        else if (op == "DP3") r = "float4(dot(a.xyz, b.xyz))";
        else if (op == "DP4") r = "float4(dot(a, b))";
        else if (op == "DPH") r = "float4(dot(a.xyz, b.xyz) + b.w)";
        else if (op == "DST") r = "float4(1.0f, a.y * b.y, a.z, b.w)";
        else if (op == "XPD") r = "float4(cross(a.xyz, b.xyz), 0.0f)";
        else if (op == "LIT") r = "arb_lit(a)";
        else if (op == "RCP") r = "float4(1.0f / a.x)";
        else if (op == "RSQ") r = "float4(rsqrt(abs(a.x)))";
        else if (op == "EX2") r = "float4(exp2(a.x))";
        else if (op == "LG2") r = "float4(log2(abs(a.x)))";
        else if (op == "POW") r = "float4(pow(abs(a.x), b.x))";
        else if (op == "SIN") r = "float4(sin(a.x))";
        else if (op == "COS") r = "float4(cos(a.x))";
        else if (op == "SCS") r = "float4(cos(a.x), sin(a.x), 0.0f, 0.0f)";
        else if (op == "EXP") r = "arb_exp(a.x)";
        else if (op == "LOG") r = "arb_log(a.x)";
        else if (in.op->kind == OpKind::Tex) {
            TexTarget& t = out_->textures[in.unit];
            if (t != TexTarget::None && t != in.target)
                throw Fail{in.line, "texture unit " + std::to_string(in.unit) + " used with two targets"};
            t = in.target;
            r = sample(in);
        }
        if (in.sat) r = "saturate(" + r + ")";
        const std::string name = dst_name(in.dst);
        if (in.dst.mask == 0xf) code += "        " + name + " = " + r + ";\n";
        else {
            const std::string m = mask_suffix(in.dst.mask);
            code += "        " + name + "." + m + " = (" + r + ")." + m + ";\n";
        }
        body_ += code + "    }\n";
    }

    void assemble() {
        const std::string& e = o_.entry;
        std::string s;
        if (o_.prelude) s += prelude();
        std::string locals;
        for (int k = 0; k < p_.temps; k++) locals += "    float4 R" + std::to_string(k) + " = float4(0.0f);\n";
        for (int k = 0; k < p_.addresses; k++) locals += "    int4 A" + std::to_string(k) + " = int4(0);\n";
        std::string args;
        auto arg = [&](const std::string& a) { args += (args.empty() ? "" : ",\n        ") + a; };
        if (stage_ == Stage::Vertex) {
            const uint32_t written = outputs_ & ((1u << kVaryingCount) - 1);
            out_->varyings = written | o_.extra_varyings;
            if (out_->attribs) {
                s += "struct " + e + "_in {\n";
                for (int k = 0; k < 31; k++) {
                    if (!(out_->attribs >> k & 1)) continue;
                    const char* type = (o_.uint_attribs >> k & 1) ? "uint4" : (o_.int_attribs >> k & 1) ? "int4" : "float4";
                    s += "    " + std::string(type) + " a" + std::to_string(k) + " [[attribute(" + std::to_string(k) + ")]];\n";
                }
                s += "};\n";
            }
            s += "struct " + e + "_out {\n    float4 pos [[position, invariant]];\n";
            for (int k = 0; k < (int)kVaryingCount; k++)
                if (out_->varyings >> k & 1) s += "    float4 " + std::string(kVaryingName[k]) + " [[user(" + kVaryingName[k] + ")]];\n";
            s += "};\n";
            if (out_->attribs) arg(e + "_in in [[stage_in]]");
            if (!out_->params.empty()) arg("constant float4* P [[buffer(0)]]");
            s += "vertex " + e + "_out " + e + "(" + args + ") {\n" + locals;
            s += "    float4 o_pos = float4(0.0f, 0.0f, 0.0f, 1.0f);\n";
            for (int k = 0; k < (int)kVaryingCount; k++)
                if (out_->varyings >> k & 1) s += "    float4 o_" + std::string(kVaryingName[k]) + " = float4(0.0f, 0.0f, 0.0f, 1.0f);\n";
            s += body_;
            s += "    " + e + "_out out;\n";
            s += o_.gl_clip ? "    out.pos = float4(o_pos.x, -o_pos.y, (o_pos.z + o_pos.w) * 0.5f, o_pos.w);\n"
                            : "    out.pos = o_pos;\n";
            for (int k = 0; k < (int)kVaryingCount; k++) {
                if (!(out_->varyings >> k & 1)) continue;
                const std::string n = kVaryingName[k];
                const bool colour = o_.clamp_colors && (k == (int)kVaryingColor0 || k == (int)kVaryingColor1);
                s += "    out." + n + " = " + (colour ? "saturate(o_" + n + ")" : "o_" + n) + ";\n";
            }
            s += "    return out;\n}\n";
        } else {
            const bool has_in = out_->varyings || out_->reads_position;
            if (has_in) {
                s += "struct " + e + "_in {\n";
                if (out_->reads_position) s += "    float4 pos [[position]];\n";
                for (int k = 0; k < (int)kVaryingCount; k++)
                    if (out_->varyings >> k & 1) s += "    float4 " + std::string(kVaryingName[k]) + " [[user(" + kVaryingName[k] + ")]];\n";
                s += "};\n";
            }
            const uint32_t colours = out_->color_outputs | 1;
            s += "struct " + e + "_out {\n";
            for (int k = 0; k < 4; k++)
                if (colours >> k & 1) s += "    float4 c" + std::to_string(k) + " [[color(" + std::to_string(k) + ")]];\n";
            if (out_->writes_depth) s += "    float depth [[depth(any)]];\n";
            s += "};\n";
            if (has_in) arg(e + "_in in [[stage_in]]");
            if (!out_->params.empty()) arg("constant float4* P [[buffer(0)]]");
            arg("constant arb_fixed& fx [[buffer(1)]]");
            for (int u = 0; u < 16; u++) {
                const char* type = nullptr;
                switch (out_->textures[u]) {
                case TexTarget::T1D: type = "texture1d<float>"; break;
                case TexTarget::T2D: case TexTarget::Rect: type = "texture2d<float>"; break;
                case TexTarget::T3D: type = "texture3d<float>"; break;
                case TexTarget::Cube: type = "texturecube<float>"; break;
                case TexTarget::Shadow2D: type = "depth2d<float>"; break;
                case TexTarget::None: break;
                }
                if (!type) continue;
                const std::string n = std::to_string(u);
                arg(std::string(type) + " tex" + n + " [[texture(" + n + ")]], sampler smp" + n + " [[sampler(" + n + ")]]");
            }
            s += "fragment " + e + "_out " + e + "(" + args + ") {\n" + locals;
            for (int k = 0; k < 4; k++)
                if (colours >> k & 1) s += "    float4 o_c" + std::to_string(k) + " = float4(0.0f);\n";
            if (out_->writes_depth) s += "    float4 o_depth = float4(0.0f);\n";
            s += body_;
            s += "    if (arb_alpha_func != 7u && !arb_alpha_pass(saturate(o_c0.w), fx.alpha_ref)) discard_fragment();\n";
            s += "    " + e + "_out out;\n";
            for (int k = 0; k < 4; k++)
                if (colours >> k & 1) s += "    out.c" + std::to_string(k) + " = o_c" + std::to_string(k) + ";\n";
            if (out_->writes_depth) s += "    out.depth = o_depth.z;\n";
            s += "    return out;\n}\n";
        }
        out_->msl = std::move(s);
    }
};

}  // namespace

const std::string& prelude() {
    static const std::string s = R"(#include <metal_stdlib>
using namespace metal;
constant uint arb_alpha_func_fc [[function_constant(0)]];
constant uint arb_alpha_func = is_function_constant_defined(arb_alpha_func_fc) ? arb_alpha_func_fc : 7u;
struct arb_fixed {
    float alpha_ref;
    float lod_bias[16];
};
static inline bool arb_alpha_pass(float a, float ref) {
    switch (arb_alpha_func) {
    case 0: return false;
    case 1: return a < ref;
    case 2: return a == ref;
    case 3: return a <= ref;
    case 4: return a > ref;
    case 5: return a != ref;
    case 6: return a >= ref;
    default: return true;
    }
}
static inline float4 arb_lit(float4 a) {
    const float x = max(a.x, 0.0f), y = max(a.y, 0.0f), w = clamp(a.w, -128.0f, 128.0f);
    const float spec = y > 0.0f ? exp2(w * log2(y)) : (w == 0.0f ? 1.0f : 0.0f);
    return float4(1.0f, x, a.x > 0.0f ? spec : 0.0f, 1.0f);
}
static inline float4 arb_exp(float x) {
    const float f = floor(x);
    return float4(exp2(f), x - f, exp2(x), 1.0f);
}
static inline float4 arb_log(float x) {
    const float ax = abs(x), e = floor(log2(ax));
    return float4(e, ax / exp2(e), log2(ax), 1.0f);
}
)";
    return s;
}

Program translate(std::string_view text, const Options& options) {
    Program out;
    size_t start = 0;
    while (start < text.size() && std::isspace((unsigned char)text[start])) start++;
    const std::string_view head = text.substr(start, 10);
    if (head == "!!ARBvp1.0") out.stage = Stage::Vertex;
    else if (head == "!!ARBfp1.0") out.stage = Stage::Fragment;
    else {
        out.error = "line 1: not an ARB vertex or fragment program";
        return out;
    }
    int line = 1;
    for (size_t k = 0; k < start; k++) line += text[k] == '\n';
    try {
        Parser p(tokenize(text.substr(start + 10), line), out.stage);
        p.parse();
        Emitter(p, out.stage, options, &out).run();
        out.ok = true;
    } catch (const Fail& f) {
        const Stage stage = out.stage;
        out = Program{};
        out.stage = stage;
        out.error = "line " + std::to_string(f.line) + ": " + f.message;
    }
    return out;
}

}  // namespace scimitar::arb
