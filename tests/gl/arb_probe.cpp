// Risk check: does this Mac's legacy OpenGL accept and run ARB assembly programs
// (ARB_vertex_program / ARB_fragment_program), which is all the game's shaders are?
//
//   gl_arb_probe                 creates a legacy CGL context, prints the renderer,
//                                extensions and program limits, then renders with
//                                synthetic programs and checks the pixels.
//   gl_arb_probe FILE...         also compiles every "!!ARBvp1.0"/"!!ARBfp1.0" program
//                                embedded in FILE (e.g. the game's scimitar_sf).
//
// Exit status is 0 only if everything compiled and rendered as expected.

#define GL_SILENCE_DEPRECATION
#define GL_GLEXT_PROTOTYPES
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace {

bool has_extension(const char* name) {
    const char* all = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
    if (!all) return false;
    std::string_view list(all);
    size_t n = std::strlen(name);
    for (size_t pos = list.find(name); pos != std::string_view::npos;
         pos = list.find(name, pos + 1)) {
        bool start = pos == 0 || list[pos - 1] == ' ';
        bool end = pos + n == list.size() || list[pos + n] == ' ';
        if (start && end) return true;
    }
    return false;
}

struct CompileResult {
    bool ok = false;
    GLenum gl_error = GL_NO_ERROR;
    bool native = false;
    GLint error_pos = -1;
    std::string error;
    GLint instructions = 0, native_instructions = 0, temporaries = 0, parameters = 0;
};

CompileResult compile(GLenum target, std::string_view text, GLuint id) {
    while (glGetError() != GL_NO_ERROR) {}
    glBindProgramARB(target, id);
    glProgramStringARB(target, GL_PROGRAM_FORMAT_ASCII_ARB, GLsizei(text.size()),
                       text.data());
    CompileResult r;
    r.gl_error = glGetError();
    r.ok = r.gl_error == GL_NO_ERROR;
    glGetIntegerv(GL_PROGRAM_ERROR_POSITION_ARB, &r.error_pos);
    if (auto* s = glGetString(GL_PROGRAM_ERROR_STRING_ARB)) r.error = (const char*)s;
    if (r.ok) {
        GLint native = 0;
        glGetProgramivARB(target, GL_PROGRAM_UNDER_NATIVE_LIMITS_ARB, &native);
        r.native = native != 0;
        glGetProgramivARB(target, GL_PROGRAM_INSTRUCTIONS_ARB, &r.instructions);
        glGetProgramivARB(target, GL_PROGRAM_NATIVE_INSTRUCTIONS_ARB,
                          &r.native_instructions);
        glGetProgramivARB(target, GL_PROGRAM_TEMPORARIES_ARB, &r.temporaries);
        glGetProgramivARB(target, GL_PROGRAM_PARAMETERS_ARB, &r.parameters);
    }
    return r;
}

void print_limits(GLenum target, const char* label) {
    struct Limit { GLenum pname; const char* name; };
    static const Limit common[] = {
        {GL_MAX_PROGRAM_INSTRUCTIONS_ARB, "instructions"},
        {GL_MAX_PROGRAM_NATIVE_INSTRUCTIONS_ARB, "native instructions"},
        {GL_MAX_PROGRAM_TEMPORARIES_ARB, "temporaries"},
        {GL_MAX_PROGRAM_PARAMETERS_ARB, "parameters"},
        {GL_MAX_PROGRAM_ATTRIBS_ARB, "attribs"},
        {GL_MAX_PROGRAM_ENV_PARAMETERS_ARB, "env parameters"},
        {GL_MAX_PROGRAM_LOCAL_PARAMETERS_ARB, "local parameters"},
    };
    static const Limit fragment[] = {
        {GL_MAX_PROGRAM_TEX_INSTRUCTIONS_ARB, "tex instructions"},
        {GL_MAX_PROGRAM_TEX_INDIRECTIONS_ARB, "tex indirections"},
        {GL_MAX_TEXTURE_IMAGE_UNITS_ARB, "texture image units"},
        {GL_MAX_TEXTURE_COORDS_ARB, "texture coords"},
    };
    std::printf("%s limits:\n", label);
    for (const Limit& l : common) {
        GLint v = 0;
        glGetProgramivARB(target, l.pname, &v);
        std::printf("  %-22s %d\n", l.name, v);
    }
    if (target == GL_FRAGMENT_PROGRAM_ARB) {
        for (const Limit& l : fragment) {
            GLint v = 0;
            if (l.pname == GL_MAX_PROGRAM_TEX_INSTRUCTIONS_ARB ||
                l.pname == GL_MAX_PROGRAM_TEX_INDIRECTIONS_ARB)
                glGetProgramivARB(target, l.pname, &v);
            else
                glGetIntegerv(l.pname, &v);
            std::printf("  %-22s %d\n", l.name, v);
        }
    }
}

// Synthetic programs written the way cgc emits them: generic attributes, env
// parameters set with glProgramEnvParameters4fvEXT, a high local parameter index
// (the game builds with MaxLocalParams=256), TEX and KIL.
const char kVertexProgram[] =
    "!!ARBvp1.0\n"
    "PARAM c[1] = { program.env[0] };\n"
    "MUL result.position, vertex.attrib[0], c[0];\n"
    "MOV result.texcoord[0], vertex.attrib[1];\n"
    "END\n"
    "# trailing comment, as cgc writes it\n";

const char kFragmentProgram[] =
    "!!ARBfp1.0\n"
    "OPTION ARB_precision_hint_fastest;\n"
    "PARAM tint = program.env[0];\n"
    "PARAM bias = program.local[255];\n"
    "TEMP t, k;\n"
    "SUB k, fragment.texcoord[0], {0.5, 0.5, 0.5, 0.5};\n"
    "KIL k.xxxx;\n"
    "TEX t, fragment.texcoord[0], texture[0], 2D;\n"
    "MAD result.color, t, tint, bias;\n"
    "END\n";

bool render_test() {
    constexpr int W = 8, H = 4;

    GLuint target_tex = 0, fbo = 0;
    glGenTextures(1, &target_tex);
    glBindTexture(GL_TEXTURE_2D, target_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 nullptr);
    glGenFramebuffersEXT(1, &fbo);
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fbo);
    glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT,
                              GL_TEXTURE_2D, target_tex, 0);
    if (glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT) != GL_FRAMEBUFFER_COMPLETE_EXT) {
        std::printf("render: framebuffer incomplete\n");
        return false;
    }

    // 1x1 source texture (128, 64, 255, 255).
    const unsigned char texel[4] = {128, 64, 255, 255};
    GLuint src_tex = 0;
    glActiveTexture(GL_TEXTURE0);
    glGenTextures(1, &src_tex);
    glBindTexture(GL_TEXTURE_2D, src_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, texel);

    GLuint programs[2];
    glGenProgramsARB(2, programs);
    CompileResult vp = compile(GL_VERTEX_PROGRAM_ARB, kVertexProgram, programs[0]);
    CompileResult fp = compile(GL_FRAGMENT_PROGRAM_ARB, kFragmentProgram, programs[1]);
    std::printf("synthetic vertex program:   %s\n", vp.ok ? "compiled" : "REJECTED");
    if (!vp.ok) std::printf("  at %d: %s\n", vp.error_pos, vp.error.c_str());
    std::printf("synthetic fragment program: %s\n", fp.ok ? "compiled" : "REJECTED");
    if (!fp.ok) std::printf("  at %d: %s\n", fp.error_pos, fp.error.c_str());
    if (!vp.ok || !fp.ok) return false;

    glEnable(GL_VERTEX_PROGRAM_ARB);
    glEnable(GL_FRAGMENT_PROGRAM_ARB);
    // Unset env parameters are zero, so a wrong env upload collapses the quad.
    const GLfloat vp_env[4] = {1, 1, 1, 1};
    const GLfloat fp_env[4] = {1, 2, 0.5f, 1};
    const GLfloat fp_local[4] = {0.25f, 0, 0, 0};
    glProgramEnvParameters4fvEXT(GL_VERTEX_PROGRAM_ARB, 0, 1, vp_env);
    glProgramEnvParameters4fvEXT(GL_FRAGMENT_PROGRAM_ARB, 0, 1, fp_env);
    glProgramLocalParameters4fvEXT(GL_FRAGMENT_PROGRAM_ARB, 255, 1, fp_local);

    // Full-screen quad as a VBO with generic attributes 0 (position) and 1 (uv).
    const GLfloat quad[] = {
        -1, -1, 0, 1, 0, 0,  1, -1, 0, 1, 1, 0,
        -1,  1, 0, 1, 0, 1,  1,  1, 0, 1, 1, 1,
    };
    GLuint vbo = 0;
    glGenBuffersARB(1, &vbo);
    glBindBufferARB(GL_ARRAY_BUFFER_ARB, vbo);
    glBufferDataARB(GL_ARRAY_BUFFER_ARB, sizeof(quad), quad, GL_STATIC_DRAW_ARB);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 6 * sizeof(GLfloat), nullptr);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 6 * sizeof(GLfloat),
                          reinterpret_cast<void*>(4 * sizeof(GLfloat)));
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);

    glViewport(0, 0, W, H);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    unsigned char pixels[W * H * 4];
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    GLenum err = glGetError();

    glDisable(GL_VERTEX_PROGRAM_ARB);
    glDisable(GL_FRAGMENT_PROGRAM_ARB);
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, 0);

    // Left half killed (clear colour); right half = texel * tint + bias.
    const int kept[4] = {192, 128, 128, 255};
    int bad = 0;
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            const unsigned char* p = pixels + (y * W + x) * 4;
            for (int c = 0; c < 4; ++c) {
                int want = x < W / 2 ? 0 : kept[c];
                if (std::abs(int(p[c]) - want) > 2) {
                    if (bad++ < 4)
                        std::printf("  pixel (%d,%d) = %d %d %d %d, expected %s\n", x, y,
                                    p[0], p[1], p[2], p[3],
                                    x < W / 2 ? "0 0 0 0" : "192 128 128 255");
                    break;
                }
            }
        }
    }
    std::printf("render with programs:       %s (GL error 0x%x)\n",
                bad == 0 && err == GL_NO_ERROR ? "OK" : "WRONG PIXELS", err);
    return bad == 0 && err == GL_NO_ERROR;
}

// Every "!!ARBvp1.0"/"!!ARBfp1.0" string in a file, up to its NUL terminator.
struct Embedded {
    GLenum target;
    size_t offset;
    std::string text;
};

std::vector<Embedded> find_programs(const std::string& data) {
    std::vector<Embedded> out;
    for (size_t pos = data.find("!!ARB"); pos != std::string::npos;
         pos = data.find("!!ARB", pos + 1)) {
        std::string_view head(data.data() + pos, std::min<size_t>(10, data.size() - pos));
        GLenum target = head == "!!ARBvp1.0" ? GL_VERTEX_PROGRAM_ARB
                        : head == "!!ARBfp1.0" ? GL_FRAGMENT_PROGRAM_ARB
                                               : 0;
        if (!target) continue;
        size_t end = data.find('\0', pos);
        if (end == std::string::npos) end = data.size();
        out.push_back({target, pos, data.substr(pos, end - pos)});
    }
    return out;
}

// Some of the game's vertex programs (cgc 2.2 output for a COLOR1 input bound
// to a generic attribute) read "vertex.attrib[N].secondary", which the
// drivers of the time accepted and Apple's current parser rejects. The suffix
// is meaningless on a generic attribute (the game feeds attribute N itself), so
// the port's GL layer drops it; the probe checks that this is enough.
std::string fix_secondary_attrib(std::string text) {
    for (size_t p = text.find("vertex.attrib["); p != std::string::npos;
         p = text.find("vertex.attrib[", p + 1)) {
        const size_t close = text.find(']', p);
        if (close != std::string::npos && text.compare(close + 1, 10, ".secondary") == 0)
            text.erase(close + 1, 10);
    }
    return text;
}

// cgc writes "# source file: ...\shaders\NAME.hlsl" and "#program ENTRY".
std::string describe(const std::string& text) {
    std::string name, entry;
    if (size_t p = text.find("# source file: "); p != std::string::npos) {
        size_t e = text.find('\n', p);
        std::string path = text.substr(p + 15, e - p - 15);
        size_t slash = path.find_last_of("\\/");
        name = slash == std::string::npos ? path : path.substr(slash + 1);
    }
    if (size_t p = text.find("#program "); p != std::string::npos)
        entry = text.substr(p + 9, text.find('\n', p) - p - 9);
    if (name.empty() && entry.empty()) return "?";
    return name + ":" + entry;
}

bool probe_file(const char* path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::printf("%s: cannot open\n", path);
        return false;
    }
    std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<Embedded> found = find_programs(data);

    int counts[2] = {}, accepted[2] = {}, native[2] = {}, fixed_count[2] = {};
    GLint max_instr[2] = {}, max_temps[2] = {}, max_params[2] = {};
    // A program id is tied to the target it was first bound to, so use one per target.
    GLuint ids[2] = {};
    glGenProgramsARB(2, ids);
    for (const Embedded& e : found) {
        int k = e.target == GL_FRAGMENT_PROGRAM_ARB;
        ++counts[k];
        CompileResult r = compile(e.target, e.text, ids[k]);
        if (!r.ok) {
            const std::string fixed = fix_secondary_attrib(e.text);
            if (fixed != e.text) {
                CompileResult r2 = compile(e.target, fixed, ids[k]);
                if (r2.ok) {
                    ++fixed_count[k];
                    r = r2;
                }
            }
        }
        if (r.ok) {
            ++accepted[k];
            native[k] += r.native;
            max_instr[k] = std::max(max_instr[k], r.instructions);
            max_temps[k] = std::max(max_temps[k], r.temporaries);
            max_params[k] = std::max(max_params[k], r.parameters);
            if (!r.native)
                std::printf("  over native limits: %s @0x%zx (%d instructions)\n",
                            describe(e.text).c_str(), e.offset, r.instructions);
        } else {
            // Show the line containing the error position.
            std::string line;
            if (r.error_pos >= 0 && size_t(r.error_pos) <= e.text.size()) {
                size_t b = e.text.rfind('\n', r.error_pos ? r.error_pos - 1 : 0);
                b = b == std::string::npos ? 0 : b + 1;
                line = e.text.substr(b, e.text.find('\n', r.error_pos) - b);
            }
            std::printf("  REJECTED %s @0x%zx (GL error 0x%x) at %d: %s\n    > %s\n",
                        describe(e.text).c_str(), e.offset, r.gl_error, r.error_pos,
                        r.error.c_str(), line.c_str());
        }
    }
    glDeleteProgramsARB(2, ids);

    std::printf("%s:\n", path);
    const char* kinds[2] = {"vertex", "fragment"};
    for (int k = 0; k < 2; ++k)
        std::printf("  %-8s programs: %3d found, %3d compiled (%d after dropping \".secondary\" "
                    "from a generic attribute), %3d under native limits"
                    " (max %d instructions, %d temps, %d params)\n",
                    kinds[k], counts[k], accepted[k], fixed_count[k], native[k], max_instr[k],
                    max_temps[k], max_params[k]);
    return accepted[0] == counts[0] && accepted[1] == counts[1] &&
           native[0] == counts[0] && native[1] == counts[1];
}

}  // namespace

int main(int argc, char** argv) {
    const CGLPixelFormatAttribute attrs[] = {
        kCGLPFAOpenGLProfile, CGLPixelFormatAttribute(kCGLOGLPVersion_Legacy),
        kCGLPFAAccelerated,
        kCGLPFAColorSize, CGLPixelFormatAttribute(24),
        kCGLPFAAlphaSize, CGLPixelFormatAttribute(8),
        kCGLPFADepthSize, CGLPixelFormatAttribute(24),
        CGLPixelFormatAttribute(0),
    };
    CGLPixelFormatObj pf = nullptr;
    GLint npf = 0;
    if (CGLChoosePixelFormat(attrs, &pf, &npf) != kCGLNoError || !pf) {
        std::printf("CGLChoosePixelFormat failed (no accelerated legacy renderer)\n");
        return 1;
    }
    CGLContextObj ctx = nullptr;
    if (CGLCreateContext(pf, nullptr, &ctx) != kCGLNoError || !ctx) {
        std::printf("CGLCreateContext failed\n");
        return 1;
    }
    CGLReleasePixelFormat(pf);
    CGLSetCurrentContext(ctx);

    std::printf("GL_RENDERER: %s\nGL_VERSION:  %s\n", glGetString(GL_RENDERER),
                glGetString(GL_VERSION));

    // Extensions behind the game's imported GL entry points.
    static const char* const needed[] = {
        "GL_ARB_vertex_program", "GL_ARB_fragment_program",
        "GL_EXT_gpu_program_parameters", "GL_ARB_vertex_buffer_object",
        "GL_EXT_framebuffer_object", "GL_EXT_framebuffer_blit",
        "GL_EXT_framebuffer_multisample", "GL_ARB_draw_buffers",
        "GL_ARB_occlusion_query", "GL_ARB_texture_compression",
        "GL_EXT_texture_compression_s3tc",
    };
    bool ok = true;
    for (const char* ext : needed) {
        bool present = has_extension(ext);
        std::printf("  %-34s %s\n", ext, present ? "yes" : "MISSING");
        ok &= present;
    }
    print_limits(GL_VERTEX_PROGRAM_ARB, "vertex program");
    print_limits(GL_FRAGMENT_PROGRAM_ARB, "fragment program");

    ok &= render_test();
    for (int i = 1; i < argc; ++i) ok &= probe_file(argv[i]);

    CGLSetCurrentContext(nullptr);
    CGLReleaseContext(ctx);
    std::printf("gl_arb_probe: %s\n", ok ? "OK" : "FAILED");
    return ok ? 0 : 1;
}
