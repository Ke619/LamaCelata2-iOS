// d3d9_arb_check FILE...: translates D3D9 shader bytecode files (one shader
// each, e.g. extracted from the Windows release's archives) to ARB with
// scimitar/d3d9_arb.h, then compiles each program with this Mac's OpenGL
// (ARB_vertex_program / ARB_fragment_program) and with the Metal backend's
// ARB -> MSL translator, and reports what failed, grouped by message.
//
//   -v     print every failure (file and message)
//   -d DIR write each translated program to DIR/<file>.arb
//   -m     also compile the MSL with Metal (slow: ~10 ms a program)
//
// The options follow games/ac2/pc_data.cpp (bool and shadow registers by
// constant name).
#import <Metal/Metal.h>
#define GL_SILENCE_DEPRECATION
#define GL_GLEXT_PROTOTYPES
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include "scimitar/arb_msl.h"
#include "scimitar/d3d9_arb.h"

int main(int argc, char** argv) {
    bool verbose = false, metal = false;
    std::string dump;
    std::vector<std::string> files;
    for (int k = 1; k < argc; k++) {
        if (!std::strcmp(argv[k], "-v")) verbose = true;
        else if (!std::strcmp(argv[k], "-m")) metal = true;
        else if (!std::strcmp(argv[k], "-d") && k + 1 < argc) dump = argv[++k];
        else files.push_back(argv[k]);
    }
    CGLPixelFormatAttribute attrs[] = {kCGLPFAAccelerated, kCGLPFAOpenGLProfile,
                                       (CGLPixelFormatAttribute)kCGLOGLPVersion_Legacy, (CGLPixelFormatAttribute)0};
    CGLPixelFormatObj pf = nullptr;
    GLint npf = 0;
    CGLContextObj ctx = nullptr;
    if (CGLChoosePixelFormat(attrs, &pf, &npf) != kCGLNoError || !pf || CGLCreateContext(pf, nullptr, &ctx) != kCGLNoError) {
        std::fprintf(stderr, "no OpenGL context\n");
        return 1;
    }
    CGLSetCurrentContext(ctx);
    id<MTLDevice> device = metal ? MTLCreateSystemDefaultDevice() : nil;

    std::map<std::string, int> refused, gl_errors, msl_errors, metal_errors;
    int ok = 0, vs = 0, ps = 0;
    GLuint names[2] = {};  // a program name is tied to its first target
    glGenProgramsARB(2, names);
    for (const std::string& f : files) {
        std::ifstream in(f, std::ios::binary);
        std::string bytes((std::istreambuf_iterator<char>(in)), {});
        std::vector<uint32_t> words(bytes.size() / 4);
        std::memcpy(words.data(), bytes.data(), words.size() * 4);
        const auto prog = scimitar::d3d9::parse(words.data(), words.size());
        (prog.vertex ? vs : ps)++;
        scimitar::d3d9::ArbOptions options;
        for (const auto& c : prog.constants) {
            if (c.set == scimitar::d3d9::RegisterSet::Bool && c.name == "g_DeconstructionEnabled") options.bool_registers[c.reg] = 79;
            if (c.set == scimitar::d3d9::RegisterSet::Bool && c.name == "g_HasSun") options.bool_registers[c.reg] = 77;
            if (c.set == scimitar::d3d9::RegisterSet::Sampler && c.name.rfind("g_ProjectorCookies", 0) == 0)
                for (int u = 0; u < c.count; u++) options.shadow_samplers |= 1u << (c.reg + u);
        }
        std::string error;
        const std::string arb = scimitar::d3d9::to_arb(prog, options, &error);
        if (!dump.empty() && !arb.empty()) {
            const std::string base = f.substr(f.find_last_of('/') + 1);
            std::ofstream(dump + "/" + base + ".arb") << arb;
        }
        if (arb.empty()) {
            refused[error]++;
            if (verbose) std::printf("%s: refused: %s\n", f.c_str(), error.c_str());
            continue;
        }
        const GLenum target = prog.vertex ? GL_VERTEX_PROGRAM_ARB : GL_FRAGMENT_PROGRAM_ARB;
        while (glGetError() != GL_NO_ERROR) {}
        glBindProgramARB(target, names[prog.vertex]);
        glProgramStringARB(target, GL_PROGRAM_FORMAT_ASCII_ARB, (GLsizei)arb.size(), arb.data());
        bool good = true;
        if (glGetError() != GL_NO_ERROR) {
            GLint pos = -1;
            glGetIntegerv(GL_PROGRAM_ERROR_POSITION_ARB, &pos);
            std::string msg = (const char*)glGetString(GL_PROGRAM_ERROR_STRING_ARB);
            gl_errors[msg.substr(0, msg.find('\n'))]++;
            if (verbose) std::printf("%s: GL at %d: %s\n", f.c_str(), pos, msg.c_str());
            good = false;
        }
        const auto msl = scimitar::arb::translate(arb);
        if (!msl.ok) {
            msl_errors[msl.error.substr(msl.error.find(':') + 1)]++;
            if (verbose) std::printf("%s: MSL: %s\n", f.c_str(), msl.error.c_str());
            good = false;
        } else if (device) {
            NSError* err = nil;
            id<MTLLibrary> lib = [device newLibraryWithSource:[NSString stringWithUTF8String:msl.msl.c_str()] options:nil error:&err];
            if (!lib) {
                std::string m = err.localizedDescription.UTF8String;
                metal_errors[m.substr(0, 120)]++;
                if (verbose) std::printf("%s: Metal: %s\n", f.c_str(), m.c_str());
                good = false;
            }
        }
        ok += good;
    }
    std::printf("%zu shaders (%d vertex, %d pixel): %d accepted everywhere\n", files.size(), vs, ps, ok);
    auto show = [](const char* what, const std::map<std::string, int>& m) {
        int total = 0;
        for (const auto& [k, n] : m) total += n;
        if (!total) return;
        std::printf("%s: %d\n", what, total);
        for (const auto& [k, n] : m) std::printf("  %5d  %s\n", n, k.c_str());
    };
    show("refused by the translator", refused);
    show("rejected by OpenGL", gl_errors);
    show("rejected by the ARB -> MSL translator", msl_errors);
    show("rejected by Metal", metal_errors);
    return ok == (int)files.size() ? 0 : 1;
}
