// The ARB -> MSL translator (platform/src/arb_msl.cpp) against Metal.
//
//   arb_msl_test                synthetic programs: each is translated, compiled
//                               and run on the GPU (a 1x1 or 1x2 float target), and
//                               the pixels are compared with values worked out from
//                               the ARB specs; malformed programs must be refused.
//   arb_msl_test [--pipelines] FILE...
//                               translates every "!!ARBvp1.0"/"!!ARBfp1.0" program
//                               in FILE (e.g. build/forge_programs.txt or the
//                               game's scimitar_sf) and compiles the distinct
//                               results with Metal; --pipelines also builds a
//                               render pipeline for each (GPU code generation).
//
// Exit status is 0 only if everything translated, compiled and matched.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "scimitar/arb_msl.h"

namespace {

using scimitar::arb::Program;
using scimitar::arb::Stage;
using scimitar::arb::TexTarget;

id<MTLDevice> g_device;
id<MTLCommandQueue> g_queue;

MTLCompileOptions* compile_options() {
    MTLCompileOptions* o = [MTLCompileOptions new];
    o.preserveInvariance = YES;
    return o;
}

id<MTLLibrary> compile(const std::string& src, std::string* error) {
    NSError* err = nil;
    id<MTLLibrary> lib = [g_device newLibraryWithSource:[NSString stringWithUTF8String:src.c_str()]
                                                options:compile_options()
                                                  error:&err];
    if (!lib && error) *error = err ? err.localizedDescription.UTF8String : "unknown error";
    return lib;
}

// ---- the synthetic cases ----

using V4 = std::array<float, 4>;

// Inputs every case sees. Vertex attributes (constant over the triangle except
// attribute 0, the position) feed the harness vertex program's varyings.
constexpr V4 kTex0 = {0.5f, -2, 3, 0.25f}, kTex1 = {1, 2, 3, 4}, kTex2 = {-1, 0.5f, 0, 2},
             kTex3 = {0.75f, 0.25f, 0.5f, 2};
constexpr V4 kCol0 = {0.25f, 0.5f, 0.75f, 1}, kCol1 = {1, 0, 0.5f, 0.25f};
constexpr V4 kAttr1 = {1.7f, 10, -0.5f, 1}, kAttr2 = {1, 2, 3, 4};
float g_env[16][4] = {
    {2, 3, 4, 5}, {-1, -2, -3, -4}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {10, 20, 30, 40},
    {0.75f, 0.25f, 0.5f, 2},   // 6: 2D coordinates (texel 1,0), w for TXP
    {-1, 0.2f, 0.1f, 1},       // 7: cube direction, -X face
    {0.75f, 0.25f, 0.75f, 1},  // 8: 3D coordinates (voxel 1,0,1)
    {0.75f, 0.25f, 0.5f, 1},   // 9: shadow: texel (1,0) depth 0.75, r 0.5 -> lit
    {0.25f, 0.25f, 0.5f, 1},   // 10: shadow: texel (0,0) depth 0.25, r 0.5 -> shadowed
    {1.5f, 0.5f, 1.0f, 2},     // 11: shadow, projective: (0.75, 0.25, 0.5)
};
float g_local[4][4] = {{0.5f, 0.25f, 0.125f, 2}};

struct Case {
    const char* name;
    std::string vp, fp;  // empty: the harness's own
    V4 expect;           // colour 0 (row 0)
    bool killed = false;            // expect the clear colour instead
    uint32_t alpha_func = 7;        // function constant
    float alpha_ref = 0;
    int height = 1;                 // 2: row 1 must stay clear
    bool check_c1 = false;
    V4 expect_c1 = {};
    bool check_depth = false;
    float expect_depth = 0;
    uint32_t uint_attribs = 0;
    float tolerance = 1e-5f;
};

const char* kHarnessVP =
    "!!ARBvp1.0\n"
    "MOV result.position, vertex.attrib[0];\n"
    "MOV result.texcoord[0], vertex.attrib[8];\n"
    "MOV result.texcoord[1], vertex.attrib[9];\n"
    "MOV result.texcoord[2], vertex.attrib[10];\n"
    "MOV result.texcoord[3], vertex.attrib[11];\n"
    "MOV result.color, vertex.attrib[3];\n"
    "MOV result.color.secondary, vertex.attrib[4];\n"
    "END\n";

std::string fp(const std::string& body) { return "!!ARBfp1.0\n" + body + "\nEND\n"; }
std::string vp(const std::string& body) {
    return "!!ARBvp1.0\nMOV result.position, vertex.attrib[0];\n" + body + "\nEND\n";
}
std::string read_fp(const char* input) { return fp(std::string("MOV result.color, ") + input + ";"); }

std::vector<Case> cases() {
    std::vector<Case> c;
    auto add = [&](const char* name, const std::string& f, V4 e) {
        Case k;
        k.name = name, k.fp = f, k.expect = e;
        c.push_back(k);
        return &c.back();
    };
    auto addv = [&](const char* name, const std::string& v, const char* input, V4 e) {
        Case k;
        k.name = name, k.vp = v, k.fp = read_fp(input), k.expect = e;
        c.push_back(k);
        return &c.back();
    };
    // Fragment programs.
    add("MOV", fp("MOV result.color, fragment.texcoord[1];"), {1, 2, 3, 4});
    add("ADD", fp("ADD result.color, fragment.texcoord[1], program.env[0];"), {3, 5, 7, 9});
    add("SUB", fp("SUB result.color, fragment.texcoord[1], program.env[0];"), {-1, -1, -1, -1});
    add("MUL scalar swizzle", fp("MUL result.color, fragment.texcoord[1], program.env[0].y;"), {3, 6, 9, 12});
    add("MAD negate local", fp("MAD result.color, -fragment.texcoord[1], program.env[0], program.local[0];"),
        {-1.5f, -5.75f, -11.875f, -18});
    add("swizzle", fp("MOV result.color, fragment.texcoord[1].wzyx;"), {4, 3, 2, 1});
    add("write mask", fp("TEMP R0;\nMOV R0, program.env[0];\nMOV R0.yw, fragment.texcoord[1];\nMOV result.color, R0;"),
        {2, 2, 4, 4});
    add("DP3", fp("DP3 result.color, fragment.texcoord[1], program.env[0];"), {20, 20, 20, 20});
    add("DP4", fp("DP4 result.color, fragment.texcoord[1], program.env[0];"), {40, 40, 40, 40});
    add("DPH", fp("DPH result.color, fragment.texcoord[1], program.env[0];"), {25, 25, 25, 25});
    add("DST", fp("DST result.color, fragment.texcoord[1], program.env[0];"), {1, 6, 3, 5});
    add("XPD", fp("TEMP R0;\nMOV R0, program.env[5];\nXPD R0.xyz, fragment.texcoord[1], program.env[0];\nMOV result.color, R0;"),
        {-1, 2, -1, 40});
    add("ABS", fp("ABS result.color, program.env[1];"), {1, 2, 3, 4});
    add("MAX MIN", fp("TEMP R0;\nMAX R0.xy, fragment.texcoord[2], fragment.texcoord[0];\n"
                      "MIN R0.zw, fragment.texcoord[2], fragment.texcoord[0];\nMOV result.color, R0;"),
        {0.5f, 0.5f, 0, 0.25f});
    add("FLR FRC", fp("TEMP R0;\nFLR R0.xy, fragment.texcoord[0];\nFRC R0.zw, {0, 0, -1.25, 2.75};\nMOV result.color, R0;"),
        {0, -2, 0.75f, 0.75f});
    add("SGE SLT", fp("TEMP R0;\nSGE R0.xy, fragment.texcoord[2], fragment.texcoord[0];\n"
                      "SLT R0.zw, fragment.texcoord[2], fragment.texcoord[0];\nMOV result.color, R0;"),
        {0, 1, 1, 0});
    add("CMP", fp("CMP result.color, fragment.texcoord[2], program.env[0], program.env[1];"), {2, -2, -3, -4});
    add("LRP", fp("LRP result.color, program.local[0], fragment.texcoord[1], program.env[0];"), {1.5f, 2.75f, 3.875f, 3});
    add("RCP", fp("RCP result.color, fragment.texcoord[0].w;"), {4, 4, 4, 4});
    add("RSQ of |x|", fp("RSQ result.color, -fragment.texcoord[1].w;"), {0.5f, 0.5f, 0.5f, 0.5f});
    add("EX2 LG2", fp("TEMP R0;\nEX2 R0.x, fragment.texcoord[1].z;\nLG2 R0.y, fragment.texcoord[1].w;\n"
                      "LG2 R0.z, -fragment.texcoord[1].y;\nMOV R0.w, fragment.texcoord[0].x;\nMOV result.color, R0;"),
        {8, 2, 1, 0.5f})->tolerance = 1e-4f;
    add("POW", fp("POW result.color, fragment.texcoord[1].y, fragment.texcoord[1].z;"), {8, 8, 8, 8})->tolerance = 1e-4f;
    add("SIN COS SCS", fp("TEMP R0, R1;\nSIN R0.x, fragment.texcoord[2].z;\nCOS R0.y, fragment.texcoord[2].z;\n"
                          "SCS R1.xy, fragment.texcoord[0].x;\nMOV R0.zw, R1.xxxy;\nMOV result.color, R0;"),
        {0, 1, 0.87758256f, 0.47942554f})->tolerance = 1e-3f;
    add("LIT", fp("LIT result.color, {0.5, 0.25, 0, 2};"), {1, 0.5f, 0.0625f, 1})->tolerance = 1e-4f;
    add("LIT unlit", fp("LIT result.color, {-0.5, 0.25, 0, 2};"), {1, 0, 0, 1});
    add("_SAT", fp("MOV_SAT result.color, {-1, 0.5, 2, 1};"), {0, 0.5f, 1, 1});
    add("SWZ", fp("SWZ result.color, fragment.texcoord[1], -w, 0, 1, y;"), {-4, 0, 1, 2});
    add("PARAM array", fp("PARAM c[3] = { {7, 8}, program.env[0..1] };\nMAD result.color, c[1], c[0].y, c[2];"),
        {15, 22, 29, 36});
    add("PARAM array defaults", fp("PARAM c[2] = { {7}, 5 };\nADD result.color, c[0], c[1];"), {12, 5, 5, 6});
    add("PARAM scalar", fp("PARAM k = 3;\nMUL result.color, fragment.texcoord[1], k;"), {3, 6, 9, 12});
    add("ALIAS", fp("PARAM k = program.env[0];\nALIAS j = k;\nMOV result.color, j;"), {2, 3, 4, 5});
    add("colours", fp("ADD result.color, fragment.color.primary, fragment.color.secondary;"), {1.25f, 0.5f, 1.25f, 1.25f});
    add("fragment.position", fp("MOV result.color, fragment.position;"), {0.5f, 0.5f, 0.5f, 1});
    add("KIL kills", fp("KIL fragment.texcoord[2];\nMOV result.color, {1};"), {})->killed = true;
    add("KIL passes", fp("KIL fragment.texcoord[1];\nMOV result.color, {1, 2, 3, 4};"), {1, 2, 3, 4});
    add("TEX 2D", fp("TEX result.color, program.env[6], texture[0], 2D;"), {0, 1, 0, 1});
    add("TXP 2D", fp("TXP result.color, program.env[6], texture[0], 2D;"), {1, 0, 0, 1});
    add("TXB 2D", fp("TXB result.color, program.env[6], texture[0], 2D;"), {0, 1, 0, 1});
    add("TEX_SAT", fp("TEX_SAT result.color, program.env[6], texture[5], 2D;"), {0, 1, 0.5f, 1});
    add("TEX CUBE", fp("TEX result.color, program.env[7], texture[1], CUBE;"), {1, 10, 0, 1});
    add("TEX 3D", fp("TEX result.color, program.env[8], texture[2], 3D;"), {1, 0, 1, 1});
    add("TEX RECT", fp("TEX result.color, {1.5, 0.5}, texture[3], RECT;"), {0, 1, 0, 1});
    add("TEX 1D", fp("TEX result.color, {0.75}, texture[4], 1D;"), {6, 0, 0, 1});
    add("SHADOW2D lit", fp("OPTION ARB_fragment_program_shadow;\nTXP result.color, program.env[9], texture[9], SHADOW2D;"),
        {1, 1, 1, 1});
    add("SHADOW2D shadowed",
        fp("OPTION ARB_fragment_program_shadow;\nTXP result.color, program.env[10], texture[10], SHADOW2D;"), {0, 0, 0, 1});
    add("SHADOW2D projective",
        fp("OPTION ARB_fragment_program_shadow;\nTXP result.color, program.env[11], texture[9], SHADOW2D;"), {1, 1, 1, 1});
    {
        Case* k = add("alpha test fails", fp("MOV result.color, {1, 1, 1, 0.25};"), {});
        k->killed = true, k->alpha_func = 4, k->alpha_ref = 0.5f;  // GL_GREATER
        k = add("alpha test passes", fp("MOV result.color, {1, 1, 1, 0.25};"), {1, 1, 1, 0.25f});
        k->alpha_func = 4, k->alpha_ref = 0.2f;
    }
    {
        Case* k = add("draw buffers", fp("OPTION ARB_draw_buffers;\nMOV result.color[0], {1};\nMOV result.color[1], {2, 3, 4, 5};"),
                      {1, 0, 0, 1});
        k->check_c1 = true, k->expect_c1 = {2, 3, 4, 5};
    }
    {
        Case* k = add("result.depth", fp("MOV result.color, {1};\nMOV result.depth.z, {0.25}.x;"), {1, 0, 0, 1});
        k->check_depth = true, k->expect_depth = 0.25f;
    }
    // Vertex programs (read back through a fragment program).
    addv("vertex MOV", vp("MOV result.texcoord[1], vertex.attrib[2];"), "fragment.texcoord[1]", {1, 2, 3, 4});
    addv("ARL relative", vp("PARAM c[4] = { {1, 2, 3, 4}, program.env[0..1], {9, 9, 9, 9} };\nADDRESS A0;\n"
                            "ARL A0.x, vertex.attrib[1].x;\nMOV result.texcoord[0], c[A0.x + 1];"),
         "fragment.texcoord[0]", {-1, -2, -3, -4});
    addv("ARL clamped", vp("PARAM c[4] = { {1, 2, 3, 4}, program.env[0..1], {9, 9, 9, 9} };\nADDRESS A0;\n"
                           "ARL A0.x, vertex.attrib[1].y;\nMOV result.texcoord[0], c[A0.x];"),
         "fragment.texcoord[0]", {9, 9, 9, 9});
    addv("ARL negative offset", vp("PARAM c[4] = { {1, 2, 3, 4}, program.env[0..1], {9, 9, 9, 9} };\nADDRESS A0;\n"
                                   "ARL A0.x, vertex.attrib[1].x;\nMOV result.texcoord[0], c[A0.x - 1].wzyx;"),
         "fragment.texcoord[0]", {4, 3, 2, 1});
    addv("ARL floors", vp("PARAM c[4] = { {1, 2, 3, 4}, program.env[0..1], {9, 9, 9, 9} };\nADDRESS A0;\n"
                          "ARL A0.x, vertex.attrib[1].z;\nMOV result.texcoord[0], c[A0.x + 2];"),
         "fragment.texcoord[0]", {2, 3, 4, 5});
    addv("colour clamped", vp("MOV result.color, {2, -1, 0.5, 1};"), "fragment.color", {1, 0, 0.5f, 1});
    addv("secondary colour", vp("MOV result.color.secondary, {0.1, 0.2, 0.3, 0.4};"), "fragment.color.secondary",
         {0.1f, 0.2f, 0.3f, 0.4f});
    addv("EXP", vp("TEMP R0;\nEXP R0, vertex.attrib[1].z;\nMOV result.texcoord[0], R0;"), "fragment.texcoord[0]",
         {0.5f, 0.5f, 0.70710678f, 1})->tolerance = 1e-4f;
    addv("LOG", vp("TEMP R0;\nLOG R0, vertex.attrib[1].y;\nMOV result.texcoord[0], R0;"), "fragment.texcoord[0]",
         {3, 1.25f, 3.32192809f, 1})->tolerance = 1e-4f;
    addv("output defaults", vp("MOV result.texcoord[2].xy, vertex.attrib[2];"), "fragment.texcoord[2]", {1, 2, 0, 1});
    addv("fog coordinate", vp("MOV result.fogcoord.x, vertex.attrib[2].z;"), "fragment.fogcoord", {3, 0, 0, 1});
    addv("conventional attributes", vp("MOV result.texcoord[0], vertex.texcoord[1];"), "fragment.texcoord[0]",
         {1, 2, 3, 4});
    addv("varying not written", vp(""), "fragment.texcoord[5]", {0, 0, 0, 1});
    addv("integer attribute", vp("MOV result.texcoord[0], vertex.attrib[6];"), "fragment.texcoord[0]", {3, 7, 255, 1})
        ->uint_attribs = 1u << 6;
    {
        // Only GL's bottom row (window y 0) is covered: it must be row 0 in memory.
        Case* k = addv("rows bottom-up",
                       "!!ARBvp1.0\nMAD result.position, vertex.attrib[0], {1, 0.25, 1, 1}, {0, -0.75, 0, 0};\nEND\n",
                       "{1, 2, 3, 4}", {1, 2, 3, 4});
        k->height = 2;
    }
    addv("clip z to depth", vp("MOV result.texcoord[0], vertex.attrib[2];"), "fragment.position", {0.5f, 0.5f, 0.5f, 1});
    return c;
}

// Programs the translator must refuse.
const char* const kBad[] = {
    "!!ARBfp1.0\nFOO result.color, fragment.texcoord[0];\nEND\n",
    "!!ARBfp1.0\nMOV result.color, state.material.diffuse;\nEND\n",
    "!!ARBfp1.0\nMOV result.color, R0;\nEND\n",
    "!!ARBfp1.0\nMOV result.color, fragment.texcoord[0];\n",
    "!!ARBvp1.0\nMOV result.position, fragment.texcoord[0];\nEND\n",
    "!!ARBfp1.0\nTEX result.color, fragment.texcoord[0], texture[0], SHADOW2D;\nEND\n",
    "!!ARBfp1.0\nTEX result.color, fragment.texcoord[0], texture[0], 2D;\nTEX result.color, fragment.texcoord[0], "
    "texture[0], CUBE;\nEND\n",
    "!!ARBvp1.0\nMOV_SAT result.position, vertex.attrib[0];\nEND\n",
    "!!ARBvp1.0\nOPTION ARB_position_invariant;\nEND\n",
    "!!ARBfp1.0\nPARAM c[2] = { program.env[0..2] };\nEND\n",
    "!!ARBfp1.0\nTEMP R0;\nMOV R0.yx, fragment.texcoord[0];\nEND\n",
    "!ARBfp1.0\nEND\n",
};

// ---- GPU resources for the synthetic cases ----

id<MTLTexture> make_texture(MTLTextureType type, MTLPixelFormat format, int w, int h, int d, const void* data,
                            size_t row_bytes) {
    MTLTextureDescriptor* td = [MTLTextureDescriptor new];
    td.textureType = type;
    td.pixelFormat = format;
    td.width = w, td.height = h, td.depth = d;
    td.storageMode = MTLStorageModeShared;
    td.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> t = [g_device newTextureWithDescriptor:td];
    if (type == MTLTextureTypeCube) {
        for (int f = 0; f < 6; f++)
            [t replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0 slice:f
                   withBytes:(const char*)data + f * row_bytes * h bytesPerRow:row_bytes bytesPerImage:0];
    } else if (data) {
        [t replaceRegion:MTLRegionMake3D(0, 0, 0, w, h, d) mipmapLevel:0 slice:0 withBytes:data
             bytesPerRow:row_bytes bytesPerImage:row_bytes * h];
    }
    return t;
}

struct Textures {
    id<MTLTexture> t2d, t2d_bright, cube, t3d, t1d, depth;
    id<MTLSamplerState> nearest, shadow;
};

Textures make_textures() {
    Textures t;
    // 2x2, rows bottom-up as GL uploads them: (0,0) red, (1,0) green, (0,1) blue, (1,1) white.
    const float px[16] = {1, 0, 0, 1, 0, 1, 0, 1, 0, 0, 1, 1, 1, 1, 1, 0.5f};
    t.t2d = make_texture(MTLTextureType2D, MTLPixelFormatRGBA32Float, 2, 2, 1, px, 32);
    const float bright[16] = {2, 0, 0, 1, -1, 3, 0.5f, 1, 0, 0, 1, 1, 1, 1, 1, 1};
    t.t2d_bright = make_texture(MTLTextureType2D, MTLPixelFormatRGBA32Float, 2, 2, 1, bright, 32);
    float faces[6][4];
    for (int f = 0; f < 6; f++) faces[f][0] = (float)f, faces[f][1] = 10, faces[f][2] = 0, faces[f][3] = 1;
    t.cube = make_texture(MTLTextureTypeCube, MTLPixelFormatRGBA32Float, 1, 1, 1, faces, 16);
    float vox[2][2][2][4];
    for (int z = 0; z < 2; z++)
        for (int y = 0; y < 2; y++)
            for (int x = 0; x < 2; x++) vox[z][y][x][0] = (float)x, vox[z][y][x][1] = (float)y, vox[z][y][x][2] = (float)z, vox[z][y][x][3] = 1;
    t.t3d = make_texture(MTLTextureType3D, MTLPixelFormatRGBA32Float, 2, 2, 2, vox, 32);
    const float line[8] = {5, 0, 0, 1, 6, 0, 0, 1};
    t.t1d = make_texture(MTLTextureType1D, MTLPixelFormatRGBA32Float, 2, 1, 1, line, 32);
    // Depth 2x2: (0,0) 0.25, (1,0) 0.75, (0,1) 0.5, (1,1) 1. Filled by a blit
    // from a buffer (depth textures cannot be written from the CPU).
    {
        MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                                                                      width:2 height:2 mipmapped:NO];
        td.storageMode = MTLStorageModePrivate;
        td.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
        t.depth = [g_device newTextureWithDescriptor:td];
        const float depths[4] = {0.25f, 0.75f, 0.5f, 1};
        id<MTLBuffer> b = [g_device newBufferWithBytes:depths length:sizeof depths options:MTLResourceStorageModeShared];
        id<MTLCommandBuffer> cb = [g_queue commandBuffer];
        id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
        [bl copyFromBuffer:b sourceOffset:0 sourceBytesPerRow:8 sourceBytesPerImage:16 sourceSize:MTLSizeMake(2, 2, 1)
                 toTexture:t.depth destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
        [bl endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
    }
    MTLSamplerDescriptor* sd = [MTLSamplerDescriptor new];
    sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterNearest;
    t.nearest = [g_device newSamplerStateWithDescriptor:sd];
    sd.compareFunction = MTLCompareFunctionLessEqual;  // GL_LEQUAL: r <= texel
    t.shadow = [g_device newSamplerStateWithDescriptor:sd];
    return t;
}

// Runs one case; returns an empty string or what went wrong.
std::string run(const Case& k, const Textures& tex) {
    const Program f = scimitar::arb::translate(k.fp, {});
    if (!f.ok) return "fragment program: " + f.error;
    scimitar::arb::Options vo;
    vo.uint_attribs = k.uint_attribs;
    const Program probe = scimitar::arb::translate(k.vp.empty() ? kHarnessVP : k.vp, vo);
    if (!probe.ok) return "vertex program: " + probe.error;
    vo.extra_varyings = f.varyings & ~probe.varyings;
    const Program v = scimitar::arb::translate(k.vp.empty() ? kHarnessVP : k.vp, vo);
    std::string err;
    id<MTLLibrary> vl = compile(v.msl, &err);
    if (!vl) return "vertex MSL does not compile: " + err + "\n" + v.msl;
    id<MTLLibrary> fl = compile(f.msl, &err);
    if (!fl) return "fragment MSL does not compile: " + err + "\n" + f.msl;

    MTLFunctionConstantValues* fc = [MTLFunctionConstantValues new];
    if (k.alpha_func != 7) {
        const uint32_t a = k.alpha_func;
        [fc setConstantValue:&a type:MTLDataTypeUInt atIndex:scimitar::arb::kFcAlphaFunc];
    }
    NSError* nerr = nil;
    id<MTLFunction> ffn = [fl newFunctionWithName:@"arb_main" constantValues:fc error:&nerr];
    if (!ffn) return std::string("fragment function: ") + nerr.localizedDescription.UTF8String;

    MTLRenderPipelineDescriptor* pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = [vl newFunctionWithName:@"arb_main"];
    pd.fragmentFunction = ffn;
    pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA32Float;
    if (k.check_c1) pd.colorAttachments[1].pixelFormat = MTLPixelFormatRGBA32Float;
    if (k.check_depth) pd.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
    // One interleaved buffer: 16 float4 attributes per vertex.
    MTLVertexDescriptor* vd = [MTLVertexDescriptor new];
    for (int a = 0; a < 16; a++) {
        if (!(v.attribs >> a & 1)) continue;
        vd.attributes[a].format = (k.uint_attribs >> a & 1) ? MTLVertexFormatUChar4 : MTLVertexFormatFloat4;
        vd.attributes[a].offset = 16 * a;
        vd.attributes[a].bufferIndex = 30;
    }
    vd.layouts[30].stride = 256;
    pd.vertexDescriptor = vd;
    id<MTLRenderPipelineState> ps = [g_device newRenderPipelineStateWithDescriptor:pd error:&nerr];
    if (!ps) return std::string("pipeline: ") + nerr.localizedDescription.UTF8String;

    float verts[3][16][4] = {};
    const float pos[3][4] = {{-1, -1, 0, 1}, {3, -1, 0, 1}, {-1, 3, 0, 1}};
    for (int i = 0; i < 3; i++) {
        std::memcpy(verts[i][0], pos[i], 16);
        std::memcpy(verts[i][1], kAttr1.data(), 16);
        std::memcpy(verts[i][2], kAttr2.data(), 16);
        std::memcpy(verts[i][3], kCol0.data(), 16);
        std::memcpy(verts[i][4], kCol1.data(), 16);
        const uint8_t bytes[4] = {3, 7, 255, 1};
        std::memcpy(verts[i][6], bytes, 4);
        std::memcpy(verts[i][8], kTex0.data(), 16);
        std::memcpy(verts[i][9], kTex1.data(), 16);
        std::memcpy(verts[i][10], kTex2.data(), 16);
        std::memcpy(verts[i][11], kTex3.data(), 16);
    }
    auto params = [&](const Program& p) {
        std::vector<float> out(4 * std::max<size_t>(1, p.params.size()));
        for (size_t s = 0; s < p.params.size(); s++) {
            const auto& r = p.params[s];
            const float* src = r.kind == scimitar::arb::ParamRef::Env ? g_env[r.index]
                               : r.kind == scimitar::arb::ParamRef::Local ? g_local[r.index] : r.value;
            std::memcpy(&out[4 * s], src, 16);
        }
        return out;
    };
    const std::vector<float> vparams = params(v), fparams = params(f);
    scimitar::arb::FixedState fixed;
    fixed.alpha_ref = k.alpha_ref;

    const V4 clear = {-7, -7, -7, -7};
    auto target = [&](MTLPixelFormat fmt) {
        MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fmt width:1 height:k.height mipmapped:NO];
        td.storageMode = fmt == MTLPixelFormatDepth32Float ? MTLStorageModePrivate : MTLStorageModeShared;
        td.usage = MTLTextureUsageRenderTarget;
        return [g_device newTextureWithDescriptor:td];
    };
    id<MTLTexture> rt0 = target(MTLPixelFormatRGBA32Float), rt1 = k.check_c1 ? target(MTLPixelFormatRGBA32Float) : nil;
    id<MTLTexture> depth = k.check_depth ? target(MTLPixelFormatDepth32Float) : nil;
    MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = rt0;
    rp.colorAttachments[0].loadAction = MTLLoadActionClear;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(clear[0], clear[1], clear[2], clear[3]);
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    if (rt1) {
        rp.colorAttachments[1].texture = rt1;
        rp.colorAttachments[1].loadAction = MTLLoadActionClear;
        rp.colorAttachments[1].storeAction = MTLStoreActionStore;
    }
    if (depth) {
        rp.depthAttachment.texture = depth;
        rp.depthAttachment.loadAction = MTLLoadActionClear;
        rp.depthAttachment.clearDepth = 1;
        rp.depthAttachment.storeAction = MTLStoreActionStore;
    }
    id<MTLCommandBuffer> cb = [g_queue commandBuffer];
    id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
    [enc setRenderPipelineState:ps];
    [enc setVertexBytes:verts length:sizeof verts atIndex:30];
    [enc setVertexBytes:vparams.data() length:vparams.size() * 4 atIndex:0];
    [enc setFragmentBytes:fparams.data() length:fparams.size() * 4 atIndex:0];
    [enc setFragmentBytes:&fixed length:sizeof fixed atIndex:1];
    for (int u = 0; u < 16; u++) {
        id<MTLTexture> t = nil;
        switch (f.textures[u]) {
        case TexTarget::T2D: case TexTarget::Rect: t = u == 5 ? tex.t2d_bright : tex.t2d; break;
        case TexTarget::Cube: t = tex.cube; break;
        case TexTarget::T3D: t = tex.t3d; break;
        case TexTarget::T1D: t = tex.t1d; break;
        case TexTarget::Shadow2D: t = tex.depth; break;
        case TexTarget::None: continue;
        }
        [enc setFragmentTexture:t atIndex:u];
        [enc setFragmentSamplerState:f.textures[u] == TexTarget::Shadow2D ? tex.shadow : tex.nearest atIndex:u];
    }
    if (depth) {
        id<MTLDepthStencilState> ds;
        MTLDepthStencilDescriptor* dd = [MTLDepthStencilDescriptor new];
        dd.depthCompareFunction = MTLCompareFunctionAlways;
        dd.depthWriteEnabled = YES;
        ds = [g_device newDepthStencilStateWithDescriptor:dd];
        [enc setDepthStencilState:ds];
    }
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [enc endEncoding];
    id<MTLBuffer> depth_out;
    if (depth) {
        depth_out = [g_device newBufferWithLength:16 options:MTLResourceStorageModeShared];
        id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
        [bl copyFromTexture:depth sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                 sourceSize:MTLSizeMake(1, 1, 1) toBuffer:depth_out destinationOffset:0 destinationBytesPerRow:4
          destinationBytesPerImage:4];
        [bl endEncoding];
    }
    [cb commit];
    [cb waitUntilCompleted];
    if (cb.error) return std::string("command buffer: ") + cb.error.localizedDescription.UTF8String;

    char msg[512];
    auto check = [&](id<MTLTexture> t, int row, const V4& want, const char* what) -> std::string {
        float got[4];
        [t getBytes:got bytesPerRow:16 fromRegion:MTLRegionMake2D(0, row, 1, 1) mipmapLevel:0];
        for (int i = 0; i < 4; i++)
            if (!(std::fabs(got[i] - want[i]) <= k.tolerance * std::max(1.0f, std::fabs(want[i])))) {
                std::snprintf(msg, sizeof msg, "%s row %d: got (%g, %g, %g, %g), expected (%g, %g, %g, %g)", what, row,
                              got[0], got[1], got[2], got[3], want[0], want[1], want[2], want[3]);
                return msg;
            }
        return "";
    };
    std::string r = check(rt0, 0, k.killed ? clear : k.expect, "colour 0");
    if (r.empty() && k.height == 2) r = check(rt0, 1, clear, "colour 0");
    if (r.empty() && rt1) r = check(rt1, 0, k.expect_c1, "colour 1");
    if (r.empty() && depth) {
        const float d = *(const float*)depth_out.contents;
        if (std::fabs(d - k.expect_depth) > 1e-6f) {
            std::snprintf(msg, sizeof msg, "depth: got %g, expected %g", d, k.expect_depth);
            r = msg;
        }
    }
    if (!r.empty()) r += "\n--- vertex MSL ---\n" + v.msl + "--- fragment MSL ---\n" + f.msl;
    return r;
}

int synthetic() {
    const Textures tex = make_textures();
    int failed = 0, n = 0;
    for (const Case& k : cases()) {
        n++;
        const std::string r = run(k, tex);
        if (!r.empty()) {
            failed++;
            std::printf("FAIL %s: %s\n", k.name, r.c_str());
        }
    }
    int refused = 0;
    for (const char* bad : kBad) {
        const Program p = scimitar::arb::translate(bad);
        if (p.ok) std::printf("FAIL accepted a malformed program:\n%s\n", bad);
        else refused++;
    }
    const int nbad = (int)(sizeof kBad / sizeof *kBad);
    std::printf("arb_msl: %d/%d programs ran as expected on %s, %d/%d malformed programs refused\n", n - failed, n,
                g_device.name.UTF8String, refused, nbad);
    return failed == 0 && refused == nbad ? 0 : 1;
}

// ---- the corpus ----

// Every "!!ARBvp1.0"/"!!ARBfp1.0" program in the data, up to an END line.
void extract(const std::string& d, std::vector<std::string>* out) {
    for (size_t p = d.find("!!ARB"); p != std::string::npos; p = d.find("!!ARB", p + 1)) {
        if (d.compare(p, 10, "!!ARBvp1.0") != 0 && d.compare(p, 10, "!!ARBfp1.0") != 0) continue;
        size_t e = p;
        for (;;) {
            e = d.find("END", e + 1);
            if (e == std::string::npos) break;
            const bool line_start = d[e - 1] == '\n' || d[e - 1] == '\r';
            const bool line_end = e + 3 >= d.size() || d[e + 3] == '\n' || d[e + 3] == '\r' || d[e + 3] == '\0' || d[e + 3] == ' ';
            if (line_start && line_end) break;
        }
        if (e == std::string::npos) continue;
        out->push_back(d.substr(p, e + 3 - p) + "\n");
    }
}

std::string with_entry(std::string msl, const std::string& name) {
    for (size_t p = msl.find("arb_main"); p != std::string::npos; p = msl.find("arb_main", p + name.size()))
        msl.replace(p, 8, name);
    return msl;
}

int corpus(const std::vector<std::string>& files, bool pipelines) {
    std::vector<std::string> programs;
    for (const std::string& f : files) {
        std::ifstream in(f, std::ios::binary);
        if (!in) {
            std::printf("cannot read %s\n", f.c_str());
            return 1;
        }
        const std::string d((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        extract(d, &programs);
    }
    const auto t0 = std::chrono::steady_clock::now();
    // Translate everything; programs that differ only in comments translate
    // to the same MSL and are compiled once.
    scimitar::arb::Options o;
    o.prelude = false;
    std::map<std::string, Program> distinct;  // MSL -> translation
    std::map<std::string, int> errors;        // message -> count
    std::string first_error_program;
    size_t vps = 0, fps = 0, failed = 0, max_params = 0, max_insts = 0, kills = 0, shadow = 0;
    for (const std::string& text : programs) {
        Program p = scimitar::arb::translate(text, o);
        if (!p.ok) {
            failed++;
            errors[p.error.substr(p.error.find(':') + 2)]++;
            if (first_error_program.empty()) first_error_program = text;
            continue;
        }
        (p.stage == Stage::Vertex ? vps : fps)++;
        max_params = std::max(max_params, p.params.size());
        max_insts = std::max(max_insts, (size_t)p.instructions);
        kills += p.kills;
        for (TexTarget t : p.textures) shadow += t == TexTarget::Shadow2D;
        std::string key = p.msl;
        distinct.emplace(std::move(key), std::move(p));
    }
    const double translate_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("arb_msl: %zu programs (%zu vertex, %zu fragment), %zu not translated, %zu distinct translations; "
                "%.0f ms to translate (%.1f us each); at most %zu constants and %zu instructions\n",
                programs.size(), vps, fps, failed, distinct.size(), translate_ms, 1000 * translate_ms / std::max<size_t>(1, programs.size()),
                max_params, max_insts);
    for (const auto& [m, n] : errors) std::printf("  not translated (%d): %s\n", n, m.c_str());
    if (!first_error_program.empty()) std::printf("first untranslated program:\n%s\n", first_error_program.c_str());

    // Compile in libraries of 64 programs, in parallel.
    std::vector<const Program*> list;
    for (const auto& [m, p] : distinct) list.push_back(&p);
    const size_t batch = 64, batches = (list.size() + batch - 1) / batch;
    // (Blocks capture by copy: these are reached through pointers.)
    std::vector<id<MTLLibrary>> libs_v(batches);
    std::atomic<size_t> bad_v{0};
    std::mutex print_lock_v;
    auto* libs = &libs_v;
    auto* bad = &bad_v;
    auto* print_lock = &print_lock_v;
    const auto t1 = std::chrono::steady_clock::now();
    dispatch_apply(batches, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^(size_t b) {
      std::string src = scimitar::arb::prelude();
      for (size_t i = b * batch; i < std::min(list.size(), (b + 1) * batch); i++)
          src += with_entry(list[i]->msl, "p" + std::to_string(i));
      std::string err;
      (*libs)[b] = compile(src, &err);
      if ((*libs)[b]) return;
      // Find the programs at fault.
      for (size_t i = b * batch; i < std::min(list.size(), (b + 1) * batch); i++) {
          std::string e1;
          if (compile(scimitar::arb::prelude() + list[i]->msl, &e1)) continue;
          (*bad)++;
          std::lock_guard<std::mutex> l(*print_lock);
          if (*bad <= 5) std::printf("MSL does not compile: %s\n%s\n", e1.c_str(), list[i]->msl.c_str());
      }
    });
    const double compile_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count();
    std::printf("arb_msl: %zu distinct translations compiled in %.1f s, %zu failed\n", list.size(), compile_s, bad->load());

    std::atomic<size_t> bad_pipelines_v{0};
    auto* bad_pipelines = &bad_pipelines_v;
    if (pipelines && *bad == 0) {
        // Each program in a render pipeline with a minimal partner stage.
        const auto t2 = std::chrono::steady_clock::now();
        Program partner_fp = scimitar::arb::translate("!!ARBfp1.0\nMOV result.color, {1};\nEND\n");
        id<MTLLibrary> pf = compile(partner_fp.msl, nullptr);
        dispatch_apply(list.size(), dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^(size_t i) {
          @autoreleasepool {
              const Program& p = *list[i];
              const std::string name = "p" + std::to_string(i);
              MTLRenderPipelineDescriptor* pd = [MTLRenderPipelineDescriptor new];
              // Fragment functions read the alpha test's function constant, so
              // they are always specialized (here with it unset: no test).
              id<MTLFunction> fn = [(*libs)[i / batch] newFunctionWithName:[NSString stringWithUTF8String:name.c_str()]
                                                            constantValues:[MTLFunctionConstantValues new]
                                                                     error:nil];
              uint32_t attribs = 0;
              if (p.stage == Stage::Vertex) {
                  pd.vertexFunction = fn;
                  pd.fragmentFunction = [pf newFunctionWithName:@"arb_main" constantValues:[MTLFunctionConstantValues new] error:nil];
                  attribs = p.attribs;
                  pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
              } else {
                  scimitar::arb::Options vo;
                  vo.extra_varyings = p.varyings;
                  const Program pv = scimitar::arb::translate("!!ARBvp1.0\nMOV result.position, vertex.attrib[0];\nEND\n", vo);
                  pd.vertexFunction = [compile(pv.msl, nullptr) newFunctionWithName:@"arb_main"];
                  pd.fragmentFunction = fn;
                  attribs = 1;
                  for (int c = 0; c < 4; c++)
                      if ((p.color_outputs | 1) >> c & 1) pd.colorAttachments[c].pixelFormat = MTLPixelFormatBGRA8Unorm;
                  if (p.writes_depth) pd.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
              }
              MTLVertexDescriptor* vd = [MTLVertexDescriptor new];
              for (int a = 0; a < 16; a++)
                  if (attribs >> a & 1) {
                      vd.attributes[a].format = MTLVertexFormatFloat4;
                      vd.attributes[a].offset = 16 * a;
                      vd.attributes[a].bufferIndex = 30;
                  }
              vd.layouts[30].stride = 256;
              pd.vertexDescriptor = vd;
              NSError* err = nil;
              if (![g_device newRenderPipelineStateWithDescriptor:pd error:&err]) {
                  (*bad_pipelines)++;
                  std::lock_guard<std::mutex> l(*print_lock);
                  if (*bad_pipelines <= 5)
                      std::printf("pipeline fails: %s\n%s\n", err.localizedDescription.UTF8String, p.msl.c_str());
              }
          }
        });
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t2).count();
        std::printf("arb_msl: %zu render pipelines built in %.1f s, %zu failed\n", list.size(), s, bad_pipelines->load());
    }
    return failed == 0 && *bad == 0 && *bad_pipelines == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    @autoreleasepool {
        g_device = MTLCreateSystemDefaultDevice();
        if (!g_device) {
            std::printf("arb_msl: no Metal device\n");
            return 1;
        }
        g_queue = [g_device newCommandQueue];
        std::vector<std::string> files;
        bool pipelines = false;
        for (int i = 1; i < argc; i++) {
            if (std::strcmp(argv[i], "--pipelines") == 0) pipelines = true;
            else files.push_back(argv[i]);
        }
        return files.empty() ? synthetic() : corpus(files, pipelines);
    }
}
