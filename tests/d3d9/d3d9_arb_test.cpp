// D3D9 bytecode -> ARB (scimitar/d3d9_arb.h) on synthetic shaders: each pair
// is assembled here token by token, translated, compiled by this Mac's
// OpenGL and by the ARB -> MSL translator, and drawn as a full-screen quad
// whose pixels are checked. Covers what the translator has to get right for
// the Windows release's shaders: column-major matrices read transposed, bool
// if/else flattened (with an infinity in the branch not taken), rep loops
// unrolled, ifc, cmp, lrp and the constant table.
#define GL_SILENCE_DEPRECATION
#define GL_GLEXT_PROTOTYPES
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "scimitar/arb_msl.h"
#include "scimitar/d3d9_arb.h"

namespace {

// Register types and opcodes (d3d9types.h).
enum { TEMP = 0, INPUT = 1, CONST = 2, OUTPUT = 6, CONSTINT = 7, COLOROUT = 8, CONSTBOOL = 14 };
enum { MOV = 1, ADD = 2, RCP = 6, DP4 = 9, LRP = 18, DCL = 31, REP = 38, ENDREP = 39, IF = 40, IFC = 41,
       ELSE = 42, ENDIF = 43, DEFI = 48, DEF = 81, CMP = 88 };
constexpr uint32_t kIdentity = 0xe4, kXXXX = 0x00, kYYYY = 0x55, kZZZZ = 0xaa, kWWWW = 0xff;

uint32_t reg(int type, int num) { return 0x80000000u | (uint32_t)(type & 7) << 28 | (uint32_t)(type >> 3 & 3) << 11 | (uint32_t)num; }
uint32_t dst(int type, int num, int mask = 0xf) { return reg(type, num) | (uint32_t)mask << 16; }
uint32_t src(int type, int num, uint32_t swizzle = kIdentity, int modifier = 0) {
    return reg(type, num) | swizzle << 16 | (uint32_t)modifier << 24;
}
uint32_t f(float v) {
    uint32_t u;
    std::memcpy(&u, &v, 4);
    return u;
}

struct Asm {
    std::vector<uint32_t> t;
    explicit Asm(uint32_t version) { t.push_back(version); }
    Asm& op(int code, std::initializer_list<uint32_t> params, int control = 0) {
        t.push_back((uint32_t)code | (uint32_t)control << 16 | (uint32_t)params.size() << 24);
        t.insert(t.end(), params);
        return *this;
    }
    Asm& dcl(int usage, int index, uint32_t d) { return op(DCL, {0x80000000u | (uint32_t)usage | (uint32_t)index << 16, d}); }
    // A constant table: (name, register set, register, count, class, rows, columns).
    struct C {
        const char* name;
        int set, reg, count, cls, rows, columns;
    };
    Asm& ctab(std::initializer_list<C> cs) {
        std::vector<uint8_t> b(28 + 20 * cs.size() + 16 * cs.size());
        auto put16 = [&](size_t at, uint32_t v) { b[at] = (uint8_t)v, b[at + 1] = (uint8_t)(v >> 8); };
        auto put32 = [&](size_t at, uint32_t v) { put16(at, v & 0xffff), put16(at + 2, v >> 16); };
        put32(0, 28), put32(12, (uint32_t)cs.size()), put32(16, 28);
        size_t k = 0;
        for (const C& c : cs) {
            const size_t info = 28 + 20 * k, type = 28 + 20 * cs.size() + 16 * k;
            put32(info, (uint32_t)b.size());
            b.insert(b.end(), c.name, c.name + std::strlen(c.name) + 1);
            put16(info + 4, (uint32_t)c.set), put16(info + 6, (uint32_t)c.reg), put16(info + 8, (uint32_t)c.count);
            put32(info + 12, (uint32_t)type);
            put16(type, (uint32_t)c.cls), put16(type + 4, (uint32_t)c.rows), put16(type + 6, (uint32_t)c.columns), put16(type + 8, 1);
            k++;
        }
        while (b.size() % 4) b.push_back(0);
        t.push_back(0xfffe | (uint32_t)(1 + b.size() / 4) << 16);
        t.push_back(0x42415443);  // CTAB
        for (size_t i = 0; i < b.size(); i += 4) t.push_back(b[i] | b[i + 1] << 8 | b[i + 2] << 16 | (uint32_t)b[i + 3] << 24);
        return *this;
    }
    std::vector<uint32_t> end() {
        t.push_back(0x0000ffff);
        return t;
    }
};

GLuint compile(GLenum target, const std::string& text, bool* ok) {
    GLuint id = 0;
    glGenProgramsARB(1, &id);
    glBindProgramARB(target, id);
    glProgramStringARB(target, GL_PROGRAM_FORMAT_ASCII_ARB, (GLsizei)text.size(), text.data());
    GLint pos = -1;
    glGetIntegerv(GL_PROGRAM_ERROR_POSITION_ARB, &pos);
    if (pos != -1) {
        std::printf("  GL rejected at %d: %s\n%s\n", pos, (const char*)glGetString(GL_PROGRAM_ERROR_STRING_ARB), text.c_str());
        *ok = false;
    }
    return id;
}

// Translates both, draws, compares pixel (8, 8) with `want` (0..255).
bool run(const char* name, const std::vector<uint32_t>& vs, const std::vector<uint32_t>& ps,
         const scimitar::d3d9::ArbOptions& options, const std::vector<std::pair<int, std::array<float, 4>>>& env,
         const int want[4]) {
    std::string error;
    const std::string vtext = scimitar::d3d9::to_arb(scimitar::d3d9::parse(vs.data(), vs.size()), options, &error);
    if (vtext.empty()) return std::printf("%s: vertex shader refused: %s\n", name, error.c_str()), false;
    const std::string ftext = scimitar::d3d9::to_arb(scimitar::d3d9::parse(ps.data(), ps.size()), options, &error);
    if (ftext.empty()) return std::printf("%s: pixel shader refused: %s\n", name, error.c_str()), false;
    for (const std::string* t : {&vtext, &ftext}) {
        const auto msl = scimitar::arb::translate(*t);
        if (!msl.ok) return std::printf("%s: ARB -> MSL failed: %s\n%s\n", name, msl.error.c_str(), t->c_str()), false;
    }
    bool ok = true;
    const GLuint vp = compile(GL_VERTEX_PROGRAM_ARB, vtext, &ok), fp = compile(GL_FRAGMENT_PROGRAM_ARB, ftext, &ok);
    if (!ok) return std::printf("%s: not compiled\n", name), false;
    glBindProgramARB(GL_VERTEX_PROGRAM_ARB, vp);
    glBindProgramARB(GL_FRAGMENT_PROGRAM_ARB, fp);
    for (const auto& [r, v] : env) {
        glProgramEnvParameter4fvARB(GL_VERTEX_PROGRAM_ARB, (GLuint)r, v.data());
        glProgramEnvParameter4fvARB(GL_FRAGMENT_PROGRAM_ARB, (GLuint)r, v.data());
    }
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    unsigned char px[4];
    glReadPixels(8, 8, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    bool good = true;
    for (int c = 0; c < 4; c++) good &= std::abs(px[c] - want[c]) <= 2;
    std::printf("%s: %s (%d %d %d %d, expected %d %d %d %d)\n", name, good ? "OK" : "WRONG", px[0], px[1], px[2], px[3],
                want[0], want[1], want[2], want[3]);
    if (!good) std::printf("%s\n%s\n", vtext.c_str(), ftext.c_str());
    glDeleteProgramsARB(1, &vp);
    glDeleteProgramsARB(1, &fp);
    return good;
}

// ps_3_0: oC0 = texcoord0.
std::vector<uint32_t> copy_texcoord_ps() {
    return Asm(0xffff0300).dcl(5, 0, dst(INPUT, 0)).op(MOV, {dst(COLOROUT, 0), src(INPUT, 0)}).end();
}
// vs_3_0 prologue: v0 position -> o0.
Asm passthrough_vs() {
    Asm a(0xfffe0300);
    a.dcl(0, 0, dst(INPUT, 0)).dcl(0, 0, dst(OUTPUT, 0)).dcl(5, 0, dst(OUTPUT, 1));
    a.op(MOV, {dst(OUTPUT, 0), src(INPUT, 0)});
    return a;
}

}  // namespace

int main() {
    const CGLPixelFormatAttribute attrs[] = {kCGLPFAOpenGLProfile, CGLPixelFormatAttribute(kCGLOGLPVersion_Legacy),
                                             kCGLPFAAccelerated, CGLPixelFormatAttribute(0)};
    CGLPixelFormatObj pf = nullptr;
    GLint npf = 0;
    CGLContextObj ctx = nullptr;
    if (CGLChoosePixelFormat(attrs, &pf, &npf) != kCGLNoError || !pf || CGLCreateContext(pf, nullptr, &ctx) != kCGLNoError) {
        std::printf("no OpenGL context\n");
        return 1;
    }
    CGLSetCurrentContext(ctx);
    GLuint tex = 0, fbo = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 16, 16, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glGenFramebuffersEXT(1, &fbo);
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fbo);
    glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_TEXTURE_2D, tex, 0);
    glViewport(0, 0, 16, 16);
    const GLfloat quad[] = {-1, -1, 0, 1, 1, -1, 0, 1, -1, 1, 0, 1, 1, 1, 0, 1};
    GLuint vbo = 0;
    glGenBuffersARB(1, &vbo);
    glBindBufferARB(GL_ARRAY_BUFFER_ARB, vbo);
    glBufferDataARB(GL_ARRAY_BUFFER_ARB, sizeof quad, quad, GL_STATIC_DRAW_ARB);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, nullptr);
    glEnableVertexAttribArray(0);
    glEnable(GL_VERTEX_PROGRAM_ARB);
    glEnable(GL_FRAGMENT_PROGRAM_ARB);

    bool ok = true;
    {
        // A column-major float4x4 at c0..c3 (the engine uploads rows): with
        // def c10 = (0, 1, 0, 0), dp4(c10, c0) is the matrix's [1][0] (0.25),
        // not [0][1] (0.75). if b0 (float c20.x = 1): o1.y = 0.5; the else
        // side takes rcp(c11.x = 0), which must not reach o1.z (0.75).
        Asm vs = passthrough_vs();
        vs.ctab({{"g_M", 2, 0, 4, 3, 4, 4}, {"g_B", 0, 0, 1, 0, 1, 1}});
        vs.op(DEF, {dst(CONST, 10), f(0), f(1), f(0), f(0)}).op(DEF, {dst(CONST, 12), f(0.125f), f(0.5f), f(0.75f), f(1)});
        vs.op(MOV, {dst(OUTPUT, 1), src(CONST, 12)});
        vs.op(DP4, {dst(OUTPUT, 1, 1), src(CONST, 10), src(CONST, 0)});
        vs.op(IF, {src(CONSTBOOL, 0)});
        vs.op(MOV, {dst(OUTPUT, 1, 2), src(CONST, 12, kYYYY)});
        vs.op(ELSE, {});
        vs.op(MOV, {dst(OUTPUT, 1, 2), src(CONST, 12, kXXXX)});
        vs.op(RCP, {dst(TEMP, 0, 1), src(CONST, 11, kXXXX)});
        vs.op(MOV, {dst(OUTPUT, 1, 4), src(TEMP, 0, kXXXX)});
        vs.op(ENDIF, {});
        scimitar::d3d9::ArbOptions o;
        o.bool_registers[0] = 20;
        const int want[4] = {64, 128, 191, 255};
        ok &= run("matrix + bool if", vs.end(), copy_texcoord_ps(), o,
                  {{0, {1, 0.75f, 0, 0}}, {1, {0.25f, 1, 0, 0}}, {2, {0, 0, 1, 0}}, {3, {0, 0, 0, 1}},
                   {11, {0, 0, 0, 0}}, {20, {1, 1, 1, 1}}},
                  want);
    }
    {
        // ps: rep 3 { r0 += c0 } (r0.x = 0.3); ifc c0.x < c0.y: r1.x = c0.w
        // (0.4) else r1.x = rcp(0); r1.y = cmp(-c0.x, c0.z, c0.y) = 0.2;
        // r1.z = lrp(0.5, 1, 0) = 0.5.
        Asm ps(0xffff0300);
        ps.op(DEF, {dst(CONST, 0), f(0.1f), f(0.2f), f(0.3f), f(0.4f)});
        ps.op(DEF, {dst(CONST, 1), f(0), f(0.5f), f(1), f(1)});
        ps.op(DEFI, {dst(CONSTINT, 0), 3, 0, 1, 0});
        ps.op(MOV, {dst(TEMP, 0), src(CONST, 1, kXXXX)});
        ps.op(REP, {src(CONSTINT, 0)});
        ps.op(ADD, {dst(TEMP, 0), src(TEMP, 0), src(CONST, 0)});
        ps.op(ENDREP, {});
        ps.op(IFC, {src(CONST, 0, kXXXX), src(CONST, 0, kYYYY)}, 4);  // <
        ps.op(MOV, {dst(TEMP, 1, 1), src(CONST, 0, kWWWW)});
        ps.op(ELSE, {});
        ps.op(RCP, {dst(TEMP, 1, 1), src(CONST, 1, kXXXX)});
        ps.op(ENDIF, {});
        ps.op(CMP, {dst(TEMP, 1, 2), src(CONST, 0, kXXXX, 1), src(CONST, 0, kZZZZ), src(CONST, 0, kYYYY)});
        ps.op(LRP, {dst(TEMP, 1, 4), src(CONST, 1, kYYYY), src(CONST, 1, kZZZZ), src(CONST, 1, kXXXX)});
        ps.op(MOV, {dst(TEMP, 0, 2), src(TEMP, 1, kXXXX)});
        ps.op(MOV, {dst(TEMP, 0, 4), src(TEMP, 1, kYYYY)});
        ps.op(MOV, {dst(TEMP, 0, 8), src(TEMP, 1, kZZZZ)});
        ps.op(MOV, {dst(COLOROUT, 0), src(TEMP, 0)});
        Asm vs = passthrough_vs();
        vs.op(MOV, {dst(OUTPUT, 1), src(INPUT, 0)});
        const int want[4] = {77, 102, 51, 128};
        ok &= run("rep + ifc + cmp + lrp", vs.end(), ps.end(), {}, {}, want);
    }
    std::printf("d3d9_arb: %s\n", ok ? "OK" : "FAILED");
    return ok ? 0 : 1;
}
