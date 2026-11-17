// Direct3D 9 rendering on Metal (d3d9_internal.h's r_* functions).
//
// Shaders: the game's D3D9 bytecode goes through the PC-data work's
// translator to ARB assembly (platform/src/d3d9_arb.cpp) and on through the
// Metal backend's ARB -> MSL translator (platform/src/arb_msl.cpp), without
// the Mac conventions: D3D clip space is Metal's, and matrices are read as the
// constant table says. Vertex inputs are ARB generic attributes by usage,
// float constants buffer(0), the fragment program's fixed state buffer(1),
// vertex streams buffers 1-16, texture and sampler N for sampler N.
//
// Resources: buffers live in guest memory that Metal reads in place (wrapped
// blocks); textures and standalone surfaces have Metal textures, filled from
// the guest copy the game locks when it is dirty. Render targets and depth
// are Metal-only (the guest copy is filled by GetRenderTargetData).
//
// The game thread records each frame as compact commands (everything that
// reads device state or guest memory happens there: pipelines, state
// objects, texture uploads, constants, DrawPrimitiveUP data); a render thread
// encodes them into one Metal command buffer per frame and commits it, while
// the game builds the next frame, as a Windows driver does. One frame is in
// flight: at Present the game waits for the frame before, so draws never race
// the game's writes to the buffers they read.
//
// SCIMITAR_SCREENSHOT=dir saves the back buffer as PPM every
// SCIMITAR_SCREENSHOT_EVERY frames (default 300), from frame
// SCIMITAR_SCREENSHOT_FROM on, at most SCIMITAR_SCREENSHOT_COUNT of them.
#include <sys/stat.h>

#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <functional>
#include <chrono>
#include <map>
#include <tuple>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "d3d9_internal.h"
#include "win32_window.h"
#include "scimitar/arb_msl.h"
#include "scimitar/d3d9_arb.h"
#include "scimitar/guest_heap.h"

namespace scimitar::win32::d3d9 {

namespace {

id<MTLDevice> metal_device() {
    static id<MTLDevice> d = MTLCreateSystemDefaultDevice();
    return d;
}

// ---- Metal-visible guest memory for buffers ----

struct Chunk {
    uint32_t guest = 0, size = 0, used = 0;
    id<MTLBuffer> buffer = nil;
};
std::mutex g_mem_lock;
std::vector<Chunk> g_chunks;
std::map<uint32_t, std::vector<std::pair<id<MTLBuffer>, uint32_t>>> g_free;  // size class -> blocks

uint32_t size_class(uint32_t n) {
    uint32_t c = 256;
    while (c < n) c <<= 1;
    return c;
}

}  // namespace

uint32_t r_buffer_memory(uint32_t length, __strong id<MTLBuffer>* buffer, uint32_t* offset) {
    const uint32_t n = size_class(std::max<uint32_t>(length, 16));
    std::lock_guard<std::mutex> l(g_mem_lock);
    auto& fl = g_free[n];
    if (!fl.empty()) {
        *buffer = fl.back().first;
        *offset = fl.back().second;
        fl.pop_back();
    } else {
        const uint32_t kChunk = std::max<uint32_t>(64u << 20, n);
        if (g_chunks.empty() || g_chunks.back().used + n > g_chunks.back().size) {
            Chunk ch;
            ch.size = kChunk;
            ch.guest = heap::alloc(mem(), kChunk, 16384);
            if (!ch.guest) fatal("d3d9: guest heap exhausted (buffer memory)");
            ch.buffer = [metal_device() newBufferWithBytesNoCopy:gptr(ch.guest) length:kChunk
                                                         options:MTLResourceStorageModeShared deallocator:nil];
            if (!ch.buffer) fatal("d3d9: cannot wrap guest memory for Metal");
            g_chunks.push_back(ch);
        }
        Chunk& ch = g_chunks.back();
        *buffer = ch.buffer;
        *offset = ch.used;
        ch.used += n;
    }
    // The guest address of the block: find its chunk.
    for (const Chunk& ch : g_chunks)
        if (ch.buffer == *buffer) return ch.guest + *offset;
    fatal("d3d9: buffer block without a chunk");
}

void r_buffer_release(id<MTLBuffer> buffer, uint32_t offset, uint32_t length) {
    if (!buffer) return;
    std::lock_guard<std::mutex> l(g_mem_lock);
    // Released blocks are reused from the next frame on (draws recorded this
    // frame may still read them): kept aside until Present.
    extern std::vector<std::tuple<uint32_t, id<MTLBuffer>, uint32_t>> g_retired;
    g_retired.emplace_back(size_class(std::max<uint32_t>(length, 16)), buffer, offset);
}
std::vector<std::tuple<uint32_t, id<MTLBuffer>, uint32_t>> g_retired;

namespace {

// Blocks retired during a frame become free once the GPU has finished that
// frame: one frame later, as one frame may be in flight.
std::vector<std::tuple<uint32_t, id<MTLBuffer>, uint32_t>> g_retired_prev;
void recycle_retired() {
    std::lock_guard<std::mutex> l(g_mem_lock);
    for (auto& [n, b, off] : g_retired_prev) g_free[n].push_back({b, off});
    g_retired_prev = std::move(g_retired);
    g_retired.clear();
}

// ---- formats ----

enum class Convert { None, R5G6B5, A1R5G5B5, X1R5G5B5, A4R4G4B4, A8L8 };

struct Format {
    MTLPixelFormat pf = MTLPixelFormatInvalid;
    Convert convert = Convert::None;  // to BGRA8 when Metal has no matching format
    MTLTextureSwizzleChannels swizzle = {MTLTextureSwizzleRed, MTLTextureSwizzleGreen, MTLTextureSwizzleBlue,
                                         MTLTextureSwizzleAlpha};
    bool block = false;
    uint32_t bytes = 4;  // per texel after conversion (per block for block formats)
};

Format format_of(uint32_t d3d) {
    Format f;
    auto sw = [](MTLTextureSwizzle r, MTLTextureSwizzle g, MTLTextureSwizzle b, MTLTextureSwizzle a) {
        return MTLTextureSwizzleChannels{r, g, b, a};
    };
    // Direct3D 9 reads the channels a format lacks as 1 (R32F: r, 1, 1, 1).
    const MTLTextureSwizzleChannels r111 = sw(MTLTextureSwizzleRed, MTLTextureSwizzleOne, MTLTextureSwizzleOne,
                                              MTLTextureSwizzleOne);
    const MTLTextureSwizzleChannels rg11 = sw(MTLTextureSwizzleRed, MTLTextureSwizzleGreen, MTLTextureSwizzleOne,
                                              MTLTextureSwizzleOne);
    switch (d3d) {
    case 21: f.pf = MTLPixelFormatBGRA8Unorm; break;                                   // A8R8G8B8
    case 22: f.pf = MTLPixelFormatBGRA8Unorm;                                          // X8R8G8B8
        f.swizzle = sw(MTLTextureSwizzleRed, MTLTextureSwizzleGreen, MTLTextureSwizzleBlue, MTLTextureSwizzleOne); break;
    case 23: f.pf = MTLPixelFormatBGRA8Unorm, f.convert = Convert::R5G6B5; break;
    case 24: f.pf = MTLPixelFormatBGRA8Unorm, f.convert = Convert::X1R5G5B5; break;
    case 25: f.pf = MTLPixelFormatBGRA8Unorm, f.convert = Convert::A1R5G5B5; break;
    case 26: f.pf = MTLPixelFormatBGRA8Unorm, f.convert = Convert::A4R4G4B4; break;
    case 28: f.pf = MTLPixelFormatA8Unorm, f.bytes = 1; break;                         // A8
    case 50: f.pf = MTLPixelFormatR8Unorm, f.bytes = 1;                                // L8
        f.swizzle = sw(MTLTextureSwizzleRed, MTLTextureSwizzleRed, MTLTextureSwizzleRed, MTLTextureSwizzleOne); break;
    case 51: f.pf = MTLPixelFormatRG8Unorm, f.bytes = 2;                               // A8L8
        f.swizzle = sw(MTLTextureSwizzleRed, MTLTextureSwizzleRed, MTLTextureSwizzleRed, MTLTextureSwizzleGreen); break;
    case 60: f.pf = MTLPixelFormatRG8Snorm, f.bytes = 2; break;                        // V8U8
    case 63: f.pf = MTLPixelFormatRGBA8Snorm; break;                                   // Q8W8V8U8
    case 34: f.pf = MTLPixelFormatRG16Unorm, f.swizzle = rg11; break;                  // G16R16
    case 35: f.pf = MTLPixelFormatBGR10A2Unorm; break;                                 // A2R10G10B10
    case 31: f.pf = MTLPixelFormatRGB10A2Unorm; break;                                 // A2B10G10R10
    case 36: f.pf = MTLPixelFormatRGBA16Unorm, f.bytes = 8; break;                     // A16B16G16R16
    case 111: f.pf = MTLPixelFormatR16Float, f.bytes = 2, f.swizzle = r111; break;
    case 112: f.pf = MTLPixelFormatRG16Float, f.swizzle = rg11; break;
    case 113: f.pf = MTLPixelFormatRGBA16Float, f.bytes = 8; break;
    case 114: f.pf = MTLPixelFormatR32Float, f.swizzle = r111; break;
    case 115: f.pf = MTLPixelFormatRG32Float, f.bytes = 8, f.swizzle = rg11; break;
    case 116: f.pf = MTLPixelFormatRGBA32Float, f.bytes = 16; break;
    case FMT_DXT1: f.pf = MTLPixelFormatBC1_RGBA, f.block = true, f.bytes = 8; break;
    case FMT_DXT2: case FMT_DXT3: f.pf = MTLPixelFormatBC2_RGBA, f.block = true, f.bytes = 16; break;
    case FMT_DXT4: case FMT_DXT5: f.pf = MTLPixelFormatBC3_RGBA, f.block = true, f.bytes = 16; break;
    case 75: case 77: case 71: case 0x5a544e49: f.pf = MTLPixelFormatDepth32Float_Stencil8; break;
    case 80: case 70: case 79: case 82: f.pf = MTLPixelFormatDepth32Float; break;
    case FMT_NULL: f.pf = MTLPixelFormatInvalid; break;  // no texture: draws write no colour
    default: f.pf = MTLPixelFormatBGRA8Unorm; break;
    }
    return f;
}

// One row of texels from the guest layout to the Metal one.
void convert_row(Convert k, const uint8_t* src, uint8_t* dst, uint32_t w) {
    for (uint32_t x = 0; x < w; x++) {
        const uint16_t v = (uint16_t)(src[2 * x] | src[2 * x + 1] << 8);
        uint8_t b = 0, g = 0, r = 0, a = 255;
        switch (k) {
        case Convert::R5G6B5: r = (v >> 11 & 31) * 255 / 31, g = (v >> 5 & 63) * 255 / 63, b = (v & 31) * 255 / 31; break;
        case Convert::A1R5G5B5: a = v >> 15 ? 255 : 0; [[fallthrough]];
        case Convert::X1R5G5B5: r = (v >> 10 & 31) * 255 / 31, g = (v >> 5 & 31) * 255 / 31, b = (v & 31) * 255 / 31; break;
        case Convert::A4R4G4B4: a = (v >> 12) * 17, r = (v >> 8 & 15) * 17, g = (v >> 4 & 15) * 17, b = (v & 15) * 17; break;
        default: break;
        }
        dst[4 * x] = b, dst[4 * x + 1] = g, dst[4 * x + 2] = r, dst[4 * x + 3] = a;
    }
}

// ---- textures ----

id<MTLTexture> make_texture(MTLTextureType type, const Format& f, uint32_t w, uint32_t h, uint32_t d,
                            uint32_t levels, bool render_target) {
    MTLTextureDescriptor* td = [[MTLTextureDescriptor alloc] init];
    td.textureType = type;
    td.pixelFormat = f.pf;
    td.width = w, td.height = h, td.depth = type == MTLTextureType3D ? d : 1;
    td.mipmapLevelCount = levels;
    td.usage = MTLTextureUsageShaderRead | (render_target ? MTLTextureUsageRenderTarget : 0);
    td.storageMode = render_target || f.pf == MTLPixelFormatDepth32Float_Stencil8 || f.pf == MTLPixelFormatDepth32Float
                         ? MTLStorageModePrivate : MTLStorageModeShared;
    if (!render_target && f.pf != MTLPixelFormatDepth32Float_Stencil8) td.swizzle = f.swizzle;
    id<MTLTexture> t = [metal_device() newTextureWithDescriptor:td];
    if (!t) fatal("d3d9: cannot create a %ux%u texture (format %lu)", w, h, (unsigned long)f.pf);
    if (render_target) {
        // Render targets start black (Metal leaves new textures undefined;
        // drivers hand out zeroed memory, and the game reads targets it has
        // not fully drawn, at their edges).
        static id<MTLCommandQueue> q = [metal_device() newCommandQueue];
        id<MTLCommandBuffer> cb = [q commandBuffer];
        const bool depth = f.pf == MTLPixelFormatDepth32Float_Stencil8 || f.pf == MTLPixelFormatDepth32Float;
        for (uint32_t level = 0; level < levels; level++)
            for (uint32_t slice = 0; slice < (type == MTLTextureTypeCube ? 6u : 1u); slice++) {
                MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
                if (depth) {
                    rp.depthAttachment.texture = t, rp.depthAttachment.level = level, rp.depthAttachment.slice = slice;
                    rp.depthAttachment.loadAction = MTLLoadActionClear, rp.depthAttachment.clearDepth = 1;
                    rp.depthAttachment.storeAction = MTLStoreActionStore;
                    if (f.pf == MTLPixelFormatDepth32Float_Stencil8) {
                        rp.stencilAttachment.texture = t, rp.stencilAttachment.level = level, rp.stencilAttachment.slice = slice;
                        rp.stencilAttachment.loadAction = MTLLoadActionClear;
                        rp.stencilAttachment.storeAction = MTLStoreActionStore;
                    }
                } else {
                    rp.colorAttachments[0].texture = t, rp.colorAttachments[0].level = level, rp.colorAttachments[0].slice = slice;
                    rp.colorAttachments[0].loadAction = MTLLoadActionClear;
                    rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
                    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
                }
                [[cb renderCommandEncoderWithDescriptor:rp] endEncoding];
            }
        [cb commit];
        [cb waitUntilCompleted];  // before any use on the device's own queue
    }
    return t;
}

void upload(id<MTLTexture> tex, const Surface* s, uint32_t level, uint32_t slice, uint32_t depth_slices) {
    const Format f = format_of(s->format);
    if (const char* dir = std::getenv("SCIMITAR_DUMP_TEXTURES"); dir && level == 0 && s->width >= 256 &&
                                                                   (s->format == 21 || s->format == 22)) {
        static int n = 0;
        ::mkdir(dir, 0755);
        char path[1024];
        std::snprintf(path, sizeof path, "%s/tex_%04d_%ux%u.ppm", dir, n++, s->width, s->height);
        if (FILE* fp = std::fopen(path, "wb")) {
            std::fprintf(fp, "P6\n%u %u\n255\n", s->width, s->height);
            for (uint32_t y = 0; y < s->height; y++)
                for (uint32_t x = 0; x < s->width; x++) {
                    const uint8_t* p = gptr(s->data + y * s->pitch + 4 * x);
                    const uint8_t rgb[3] = {p[2], p[1], p[0]};
                    std::fwrite(rgb, 1, 3, fp);
                }
            std::fclose(fp);
        }
    }
    // SCIMITAR_DUMP_RAW_TEXTURES=dir: every level-0 upload of a block-compressed texture, as the guest has it (debugging).
    if (const char* dir = std::getenv("SCIMITAR_DUMP_RAW_TEXTURES"); dir && level == 0 && f.block) {
        static int n = 0;
        ::mkdir(dir, 0755);
        char path[1024];
        std::snprintf(path, sizeof path, "%s/tex_%04d_%ux%u_p%u_f%08x.raw", dir, n++, s->width, s->height, s->pitch, s->format);
        if (FILE* fp = std::fopen(path, "wb")) {
            std::fwrite(gptr(s->data), 1, (size_t)format_rows(s->format, s->height) * s->pitch, fp);
            std::fclose(fp);
        }
    }
    if (tex.storageMode == MTLStorageModePrivate) return;
    const uint32_t w = s->width, h = depth_slices > 1 ? s->height / depth_slices : s->height;
    const uint32_t rows = format_rows(s->format, h);
    if (f.convert == Convert::None) {
        const uint32_t row_bytes = f.block ? ((w + 3) / 4) * f.bytes : w * f.bytes;
        const uint32_t row = std::min(row_bytes, s->pitch);
        for (uint32_t z = 0; z < depth_slices; z++)
            [tex replaceRegion:MTLRegionMake3D(0, 0, z, w, h, 1) mipmapLevel:level slice:slice
                     withBytes:gptr(s->data + z * rows * s->pitch) bytesPerRow:row
                 bytesPerImage:tex.textureType == MTLTextureType3D ? row * rows : 0];
        return;
    }
    std::vector<uint8_t> tmp(4 * w * h);
    for (uint32_t y = 0; y < h; y++) convert_row(f.convert, gptr(s->data + y * s->pitch), &tmp[4 * w * y], w);
    [tex replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:level slice:slice withBytes:tmp.data()
           bytesPerRow:4 * w bytesPerImage:0];
}

bool is_render_target(uint32_t usage) { return usage & 3; }  // RENDERTARGET | DEPTHSTENCIL

id<MTLTexture> texture_of(Texture* t) {
    if (t->format == FMT_NULL) return nil;
    // A texture that draws already use, changed again: a new Metal texture
    // filled from every level's guest data, so the draws recorded before (and
    // the frame still on the GPU) keep the contents they were recorded with,
    // as Direct3D keeps them by waiting. Revelations rewrites a 16x256 lookup
    // texture every frame.
    if (t->tex && t->dirty && t->used && !is_render_target(t->usage)) {
        t->tex = nil;
        for (uint32_t s : t->surfaces)
            if (Surface* x = as<Surface>(s)) x->dirty = true;
    }
    if (!t->tex) {
        const Format f = format_of(t->format);
        const MTLTextureType type = t->cube ? MTLTextureTypeCube : t->volume ? MTLTextureType3D : MTLTextureType2D;
        t->tex = make_texture(type, f, t->width, t->height, t->depth, t->levels, is_render_target(t->usage));
        t->dirty = true;
        t->used = false;
    }
    if (t->dirty && !is_render_target(t->usage)) {
        const uint32_t faces = t->cube ? 6 : 1;
        for (size_t k = 0; k < t->surfaces.size(); k++) {
            Surface* s = as<Surface>(t->surfaces[k]);
            if (!s || !s->dirty) continue;
            upload(t->tex, s, s->level, t->cube ? (uint32_t)(k % faces) : 0,
                   t->volume ? std::max(1u, t->depth >> s->level) : 1);
            s->dirty = false;
        }
        t->dirty = false;
    }
    t->used = true;
    return t->tex;
}

// A render-target texture as shaders sample it: render targets are made
// without a swizzle (L8, A8L8, X8R8G8B8 need one), so they get a view.
std::map<Texture*, id<MTLTexture>> g_views;
id<MTLTexture> sampled_view(Texture* t) {
    id<MTLTexture> tex = texture_of(t);
    if (!tex || !is_render_target(t->usage)) return tex;
    const Format f = format_of(t->format);
    const MTLTextureSwizzleChannels id_sw = {MTLTextureSwizzleRed, MTLTextureSwizzleGreen, MTLTextureSwizzleBlue,
                                             MTLTextureSwizzleAlpha};
    if (std::memcmp(&f.swizzle, &id_sw, sizeof id_sw) == 0) return tex;
    auto it = g_views.find(t);
    if (it != g_views.end() && it->second.parentTexture == tex) return it->second;
    id<MTLTexture> v = [tex newTextureViewWithPixelFormat:tex.pixelFormat textureType:tex.textureType
                                                   levels:NSMakeRange(0, tex.mipmapLevelCount)
                                                   slices:NSMakeRange(0, tex.textureType == MTLTextureTypeCube ? 6 : 1)
                                                  swizzle:f.swizzle];
    g_views[t] = v;
    return v;
}

// The Metal texture a surface draws into or reads from, with its level and slice.
struct View {
    id<MTLTexture> tex = nil;
    uint32_t level = 0, slice = 0;
    bool no_alpha = false;  // X8R8G8B8: D3D reads its alpha as 1
    bool operator==(const View& o) const { return tex == o.tex && level == o.level && slice == o.slice; }
};

View view_of(uint32_t surface) {
    Surface* s = as<Surface>(surface);
    if (!s) return {};
    if (s->container) {
        Texture* t = as<Texture>(s->container);
        if (!t) return {};
        return {texture_of(t), s->level, s->face, t->format == FMT_X8R8G8B8};
    }
    if (s->format == FMT_NULL) return {};
    if (!s->tex) s->tex = make_texture(MTLTextureType2D, format_of(s->format), s->width, s->height, 1, 1, true);
    return {s->tex, 0, 0, s->format == FMT_X8R8G8B8};
}

// ---- shaders ----

struct Compiled {
    bool ok = false;
    int id = 0;  // creation order (SCIMITAR_DUMP_SHADERS file number)
    int major = 0;  // shader model
    std::string arb;                          // the ARB program
    arb::Program fragment;                    // fragment programs: the MSL
    // Vertex programs: by extra varyings and the integer attributes (unsigned, signed).
    std::map<std::tuple<uint32_t, uint32_t, uint32_t>, arb::Program> vertex;
    std::map<int, int> bools;                 // bool register -> float register used for it
    std::vector<int8_t> bool_of;              // float register -> its bool register, or -1 (built on first draw)
};

std::unordered_map<std::string, id<MTLLibrary>> g_libraries;

id<MTLFunction> function_of(const std::string& msl, const char* entry) {
    auto it = g_libraries.find(msl);
    if (it == g_libraries.end()) {
        NSError* err = nil;
        MTLCompileOptions* o = [[MTLCompileOptions alloc] init];
        o.fastMathEnabled = NO;
        id<MTLLibrary> lib = [metal_device() newLibraryWithSource:@(msl.c_str()) options:o error:&err];
        if (!lib) {
            std::fprintf(stderr, "d3d9: MSL does not compile: %s\n", err.localizedDescription.UTF8String);
            if (std::getenv("SCIMITAR_VERBOSE")) std::fprintf(stderr, "%s\n", msl.c_str());
        }
        it = g_libraries.emplace(msl, lib).first;
    }
    return it->second ? [it->second newFunctionWithName:@(entry)] : nil;
}

Compiled* compile(Shader* s) {
    if (s->compiled) return static_cast<Compiled*>(s->compiled);
    auto* c = new Compiled;
    s->compiled = c;
    static int next_id = 0;
    c->id = next_id++;
    ::scimitar::d3d9::Program p = ::scimitar::d3d9::parse(s->tokens.data(), s->tokens.size());
    c->major = p.major;
    ::scimitar::d3d9::ArbOptions o;
    o.transpose_column_matrices = false;  // the engine uploads them as its HLSL reads them
    o.vertex_cmp = true;                  // MSL selects: no NaN from branches not taken
    o.texcoords = 16;                     // shader model 3's interpolators (MSL has room)
    o.derivatives = true;                 // dsx, dsy as DDX, DDY
    o.separate_attributes = true;         // Metal has 31 attributes: no aliasing
    // Booleans become float registers the draw fills, just past the float
    // constants Direct3D has (256 vertex, 224 pixel): the top float registers
    // are in use too (Revelations' bone palettes reach c255).
    const int first = s->vertex ? 256 : 224;
    for (int b = 0; b < 16; b++) o.bool_registers[b] = first + b;
    c->bools = o.bool_registers;
    std::string err;
    if (!p.error.empty()) err = p.error;
    else c->arb = ::scimitar::d3d9::to_arb(p, o, &err);
    if (c->arb.empty()) {
        trace("shader not translated (%s): fallback", err.c_str());
        c->arb = ::scimitar::d3d9::fallback_arb(p);
    }
    if (const char* dir = std::getenv("SCIMITAR_DUMP_SHADERS")) {
        ::mkdir(dir, 0755);
        char path[1024];
        std::snprintf(path, sizeof path, "%s/%04d_%s.arb", dir, c->id, s->vertex ? "vs" : "ps");
        if (FILE* f = std::fopen(path, "w")) {
            std::fprintf(f, "# %s\n%s", err.c_str(), c->arb.c_str());
            std::fclose(f);
        }
    }
    if (!s->vertex) {
        arb::Options ao;
        ao.entry = "ps_main";
        ao.gl_clip = false;
        ao.integer_pixel_centers = true;  // vPos
        c->fragment = arb::translate(c->arb, ao);
        if (!c->fragment.ok) trace("fragment ARB -> MSL failed: %s", c->fragment.error.c_str());
    }
    c->ok = true;
    return c;
}

const arb::Program& vertex_program(Compiled* c, uint32_t extra, uint32_t uint_attribs, uint32_t int_attribs) {
    const auto key = std::make_tuple(extra, uint_attribs, int_attribs);
    auto it = c->vertex.find(key);
    if (it != c->vertex.end()) return it->second;
    arb::Options ao;
    ao.entry = "vs_main";
    ao.gl_clip = false;
    ao.clamp_colors = c->major < 3;  // vs_3_0 colour outputs are not clamped
    ao.extra_varyings = extra;
    ao.uint_attribs = uint_attribs;
    ao.int_attribs = int_attribs;
    arb::Program p = arb::translate(c->arb, ao);
    if (!p.ok) trace("vertex ARB -> MSL failed: %s", p.error.c_str());
    // D3D9 puts pixel centres on integer coordinates, Metal half a pixel
    // further (D3D9's pixel k is Metal's k + 0.5): positions move half a
    // pixel right and down (buffer 29: 1/viewport size), as Wine does, by a
    // little less than half so that the edges of a full-screen quad do not
    // fall exactly on pixel centres. Moved the other way, full-screen passes
    // left their last column and row undrawn (Revelations' bloom chain spread
    // that into a bright frame along the right and bottom edges).
    const std::string sig = "vertex vs_main_out vs_main(";
    const size_t at = p.msl.find(sig);
    const size_t pos = p.msl.find("    out.pos = o_pos;\n");
    if (p.ok && at != std::string::npos && pos != std::string::npos) {
        p.msl.replace(pos, std::strlen("    out.pos = o_pos;\n"),
                      "    out.pos = float4(o_pos.x + 0.984375f * d3d_hp.x * o_pos.w, o_pos.y - 0.984375f * d3d_hp.y * o_pos.w, o_pos.z, o_pos.w);\n");
        const size_t args = at + sig.size();
        const bool none = p.msl[args] == ')';
        p.msl.insert(args, std::string("constant float2& d3d_hp [[buffer(29)]]") + (none ? "" : ",\n        "));
    }
    return c->vertex.emplace(key, std::move(p)).first->second;
}

// D3D vertex usage (and index) -> generic attribute, as d3d9_arb binds them
// with separate attributes (no aliasing of tangent and binormal).
int attribute_of(uint32_t usage, uint32_t index) {
    return ::scimitar::d3d9::vertex_attribute((int)usage, (int)index, true);
}

struct VertexFormat {
    MTLVertexFormat format;
    int kind;  // 0 float, 1 uint, 2 int
};

VertexFormat vertex_format(uint32_t type) {
    switch (type) {
    case 0: return {MTLVertexFormatFloat, 0};
    case 1: return {MTLVertexFormatFloat2, 0};
    case 2: return {MTLVertexFormatFloat3, 0};
    case 3: return {MTLVertexFormatFloat4, 0};
    case 4: return {MTLVertexFormatUChar4Normalized_BGRA, 0};  // D3DCOLOR
    case 5: return {MTLVertexFormatUChar4, 1};                 // UBYTE4
    case 6: return {MTLVertexFormatShort2, 2};
    case 7: return {MTLVertexFormatShort4, 2};
    case 8: return {MTLVertexFormatUChar4Normalized, 0};
    case 9: return {MTLVertexFormatShort2Normalized, 0};
    case 10: return {MTLVertexFormatShort4Normalized, 0};
    case 11: return {MTLVertexFormatUShort2Normalized, 0};
    case 12: return {MTLVertexFormatUShort4Normalized, 0};
    case 13: return {MTLVertexFormatUInt1010102Normalized, 0};  // UDEC3 (approximate)
    case 14: return {MTLVertexFormatInt1010102Normalized, 0};   // DEC3N
    case 15: return {MTLVertexFormatHalf2, 0};
    case 16: return {MTLVertexFormatHalf4, 0};
    default: return {MTLVertexFormatInvalid, 0};
    }
}

// ---- state translation ----

MTLBlendFactor blend_factor(uint32_t b, bool alpha) {
    switch (b) {
    case 1: return MTLBlendFactorZero;
    case 2: return MTLBlendFactorOne;
    case 3: return alpha ? MTLBlendFactorSourceAlpha : MTLBlendFactorSourceColor;
    case 4: return alpha ? MTLBlendFactorOneMinusSourceAlpha : MTLBlendFactorOneMinusSourceColor;
    case 5: return MTLBlendFactorSourceAlpha;
    case 6: return MTLBlendFactorOneMinusSourceAlpha;
    case 7: return MTLBlendFactorDestinationAlpha;
    case 8: return MTLBlendFactorOneMinusDestinationAlpha;
    case 9: return alpha ? MTLBlendFactorDestinationAlpha : MTLBlendFactorDestinationColor;
    case 10: return alpha ? MTLBlendFactorOneMinusDestinationAlpha : MTLBlendFactorOneMinusDestinationColor;
    case 11: return MTLBlendFactorSourceAlphaSaturated;
    case 14: return alpha ? MTLBlendFactorBlendAlpha : MTLBlendFactorBlendColor;
    case 15: return alpha ? MTLBlendFactorOneMinusBlendAlpha : MTLBlendFactorOneMinusBlendColor;
    default: return MTLBlendFactorOne;
    }
}

MTLBlendOperation blend_op(uint32_t o) {
    switch (o) {
    case 2: return MTLBlendOperationSubtract;
    case 3: return MTLBlendOperationReverseSubtract;
    case 4: return MTLBlendOperationMin;
    case 5: return MTLBlendOperationMax;
    default: return MTLBlendOperationAdd;
    }
}

MTLCompareFunction compare(uint32_t f) {
    switch (f) {
    case 1: return MTLCompareFunctionNever;
    case 2: return MTLCompareFunctionLess;
    case 3: return MTLCompareFunctionEqual;
    case 4: return MTLCompareFunctionLessEqual;
    case 5: return MTLCompareFunctionGreater;
    case 6: return MTLCompareFunctionNotEqual;
    case 7: return MTLCompareFunctionGreaterEqual;
    default: return MTLCompareFunctionAlways;
    }
}

MTLStencilOperation stencil_op(uint32_t o) {
    switch (o) {
    case 2: return MTLStencilOperationZero;
    case 3: return MTLStencilOperationReplace;
    case 4: return MTLStencilOperationIncrementClamp;
    case 5: return MTLStencilOperationDecrementClamp;
    case 6: return MTLStencilOperationInvert;
    case 7: return MTLStencilOperationIncrementWrap;
    case 8: return MTLStencilOperationDecrementWrap;
    default: return MTLStencilOperationKeep;
    }
}

MTLSamplerAddressMode address(uint32_t a) {
    switch (a) {
    case 2: return MTLSamplerAddressModeMirrorRepeat;
    case 3: return MTLSamplerAddressModeClampToEdge;
    case 4: return MTLSamplerAddressModeClampToBorderColor;
    case 5: return MTLSamplerAddressModeMirrorClampToEdge;
    default: return MTLSamplerAddressModeRepeat;
    }
}

}  // namespace

// ---- the renderer ----

// What a pipeline state depends on (prepare() fills it).
struct PipelineKey {
    uint64_t w[11];
    bool operator==(const PipelineKey& o) const { return std::memcmp(w, o.w, sizeof w) == 0; }
};
struct PipelineKeyHash {
    size_t operator()(const PipelineKey& k) const {
        uint64_t h = 1469598103934665603ull;
        for (uint64_t v : k.w) h = (h ^ v) * 1099511628211ull, h ^= h >> 29;
        return (size_t)h;
    }
};

// ---- recorded commands ----

enum class Op : uint8_t {
    BeginPass, EndPass, Pipeline, DepthStencil, StencilRef, Cull, Fill, DepthBias, BlendColor, Viewport, Scissor,
    VertexBytes, FragmentBytes, VertexBuffer, FragmentTexture, FragmentSampler, Draw, DrawIndexed, Run,
};

struct Cmd {
    Op op;
    uint8_t index = 0;
    id obj = nil;  // pass descriptor, pipeline, depth state, buffer, texture or sampler
    uint64_t u[6] = {};
};

// Commands from the game thread to the render thread: a frame, or the part
// of one before a read-back.
struct Batch {
    std::vector<Cmd> cmds;
    std::vector<uint8_t> bytes;  // setVertexBytes/setFragmentBytes data
    std::vector<std::function<void(id<MTLCommandBuffer>)>> runs;  // copies, present, read-back
    bool commit = false;
    uint64_t seq = 0;
    void clear() {
        cmds.clear();
        bytes.clear();
        runs.clear();
        commit = false;
    }
};

// What the current pass has set, so repeated settings are not recorded.
struct PassState {
    const void* obj[80] = {};
    uint64_t val[80] = {};
    bool set[80] = {};
};
enum Slot { kPipeline, kDepthState, kStencilRef, kCull, kFill, kBias, kBlend, kViewport, kScissor, kVertexBuffer = 10,
            kTexture = 41, kSampler = 57 };

struct Renderer {
    // Game thread: the frame being recorded.
    Device* dev = nullptr;
    Batch* batch = nullptr;
    bool in_pass = false;
    PassState ps;
    uint64_t seq = 0, prev_frame_seq = 0;
    // The render thread's side and the hand-over.
    std::mutex lock;
    std::condition_variable cv;
    std::deque<Batch*> queue, pool;
    uint64_t completed = 0;  // the last batch whose command buffer the GPU finished
    id<MTLCommandBuffer> rt_cmd = nil;
    View colors[4], depth;
    bool has_depth = false, has_stencil = false;
    MTLPixelFormat color_formats[4] = {}, depth_format = MTLPixelFormatInvalid;
    // A clear asked for before the pass that it applies to starts.
    bool pending_clear = false;
    uint32_t clear_flags = 0, clear_color = 0, clear_stencil = 0;
    float clear_z = 1;
    std::unordered_map<PipelineKey, id<MTLRenderPipelineState>, PipelineKeyHash> pipelines;
    std::unordered_map<uint64_t, id<MTLDepthStencilState>> depth_states;
    std::unordered_map<uint64_t, id<MTLSamplerState>> samplers;
    id<MTLBuffer> zeros = nil;         // attributes the declaration lacks
    id<MTLTexture> white2d = nil, whitecube = nil, white3d = nil;
    id<MTLBuffer> upload = nil;        // DrawPrimitiveUP data, this frame's of uploads[2]
    id<MTLBuffer> uploads[2] = {};
    uint32_t upload_used = 0;
    id<MTLTexture> backbuffer_tex = nil;
    uint64_t skipped = 0;
    // Textures the next draw samples while drawing into them: copies to read.
    std::map<Texture*, id<MTLTexture>> feedback;
    std::map<std::pair<NSUInteger, NSUInteger>, std::vector<id<MTLTexture>>> scratch;
    // Scaled copies (StretchRect, Present): by destination format; samplers point, linear.
    std::map<MTLPixelFormat, id<MTLRenderPipelineState>> blit;
    id<MTLSamplerState> blit_samplers[2] = {};
};

namespace {

constexpr uint32_t kUploadSize = 32u << 20;

// ---- recording (game thread) ----

Cmd& record(Renderer* r, Op op, uint8_t index = 0) {
    Cmd& c = r->batch->cmds.emplace_back();
    c.op = op, c.index = index;
    return c;
}

// A setting the pass may already have: recorded only when it changes.
bool changed(Renderer* r, int slot, const void* obj, uint64_t val) {
    PassState& p = r->ps;
    if (p.set[slot] && p.obj[slot] == obj && p.val[slot] == val) return false;
    p.set[slot] = true, p.obj[slot] = obj, p.val[slot] = val;
    return true;
}

uint64_t bits_of(const void* p, size_t n) {  // a short value's bytes as a key (n <= 8) or their hash
    uint64_t h = 0;
    if (n <= 8) return std::memcpy(&h, p, n), h;
    h = 1469598103934665603ull;
    for (size_t k = 0; k < n; k++) h = (h ^ ((const uint8_t*)p)[k]) * 1099511628211ull;
    return h;
}

void record_bytes(Renderer* r, Op op, uint8_t index, const void* data, size_t n) {
    std::vector<uint8_t>& b = r->batch->bytes;
    const size_t off = (b.size() + 15) & ~(size_t)15;
    b.resize(off + n);
    std::memcpy(b.data() + off, data, n);
    Cmd& c = record(r, op, index);
    c.u[0] = off, c.u[1] = n;
}

// Work done on the render thread's command buffer, outside a pass.
void record_run(Renderer* r, std::function<void(id<MTLCommandBuffer>)> f) {
    r->batch->runs.push_back(std::move(f));
    record(r, Op::Run).u[0] = r->batch->runs.size() - 1;
}

void dump_pass(Device* d);

void end_pass(Renderer* r) {
    static bool dumping = false;
    static const bool dump = std::getenv("SCIMITAR_D3D_DUMP_PASSES") != nullptr;
    if (dump && r->in_pass && r->dev && !dumping) {
        dumping = true;  // the dump's own copies end the pass
        dump_pass(r->dev);
        dumping = false;
    }
    if (r->in_pass) record(r, Op::EndPass);
    r->in_pass = false;
}

// Hands the recorded commands to the render thread; with `commit`, its
// command buffer is committed after them. Returns the batch's number.
uint64_t submit(Renderer* r, bool commit) {
    end_pass(r);
    Batch* b = r->batch;
    b->commit = commit;
    b->seq = ++r->seq;
    std::unique_lock<std::mutex> l(r->lock);
    r->queue.push_back(b);
    if (!r->pool.empty()) {
        r->batch = r->pool.front();
        r->pool.pop_front();
    } else {
        r->batch = new Batch;
    }
    r->cv.notify_all();
    return b->seq;
}

// Waits until the GPU has finished the command buffer committed with batch `seq`.
void wait_completed(Renderer* r, uint64_t seq) {
    std::unique_lock<std::mutex> l(r->lock);
    r->cv.wait(l, [&] { return r->completed >= seq; });
}

// ---- encoding (render thread) ----

void encode(Renderer* r, id<MTLCommandQueue> queue, Batch& b) {
    if (!r->rt_cmd) r->rt_cmd = [queue commandBuffer];
    id<MTLCommandBuffer> cb = r->rt_cmd;
    id<MTLRenderCommandEncoder> enc = nil;
    auto f = [](const Cmd& c, int k) { float v; uint32_t w = (uint32_t)c.u[k]; std::memcpy(&v, &w, 4); return v; };
    auto dbl = [](const Cmd& c, int k) { double v; std::memcpy(&v, &c.u[k], 8); return v; };
    for (const Cmd& c : b.cmds) {
        switch (c.op) {
        case Op::BeginPass: enc = [cb renderCommandEncoderWithDescriptor:(MTLRenderPassDescriptor*)c.obj]; break;
        case Op::EndPass: [enc endEncoding], enc = nil; break;
        case Op::Pipeline: [enc setRenderPipelineState:(id<MTLRenderPipelineState>)c.obj]; break;
        case Op::DepthStencil: [enc setDepthStencilState:(id<MTLDepthStencilState>)c.obj]; break;
        case Op::StencilRef: [enc setStencilReferenceValue:(uint32_t)c.u[0]]; break;
        case Op::Cull:
            [enc setFrontFacingWinding:MTLWindingClockwise];
            [enc setCullMode:(MTLCullMode)c.u[0]];
            break;
        case Op::Fill: [enc setTriangleFillMode:(MTLTriangleFillMode)c.u[0]]; break;
        case Op::DepthBias: [enc setDepthBias:f(c, 0) slopeScale:f(c, 1) clamp:0]; break;
        case Op::BlendColor: [enc setBlendColorRed:f(c, 0) green:f(c, 1) blue:f(c, 2) alpha:f(c, 3)]; break;
        case Op::Viewport:
            [enc setViewport:(MTLViewport){dbl(c, 0), dbl(c, 1), dbl(c, 2), dbl(c, 3), dbl(c, 4), dbl(c, 5)}];
            break;
        case Op::Scissor: [enc setScissorRect:(MTLScissorRect){c.u[0], c.u[1], c.u[2], c.u[3]}]; break;
        case Op::VertexBytes: [enc setVertexBytes:b.bytes.data() + c.u[0] length:c.u[1] atIndex:c.index]; break;
        case Op::FragmentBytes: [enc setFragmentBytes:b.bytes.data() + c.u[0] length:c.u[1] atIndex:c.index]; break;
        case Op::VertexBuffer: [enc setVertexBuffer:(id<MTLBuffer>)c.obj offset:c.u[0] atIndex:c.index]; break;
        case Op::FragmentTexture: [enc setFragmentTexture:(id<MTLTexture>)c.obj atIndex:c.index]; break;
        case Op::FragmentSampler: [enc setFragmentSamplerState:(id<MTLSamplerState>)c.obj atIndex:c.index]; break;
        case Op::Draw:
            [enc drawPrimitives:(MTLPrimitiveType)c.u[0] vertexStart:c.u[1] vertexCount:c.u[2] instanceCount:c.u[3]];
            break;
        case Op::DrawIndexed:
            [enc drawIndexedPrimitives:(MTLPrimitiveType)(c.u[0] & 0xff) indexCount:c.u[1]
                             indexType:(MTLIndexType)(c.u[0] >> 8 & 0xff) indexBuffer:(id<MTLBuffer>)c.obj
                     indexBufferOffset:c.u[2] instanceCount:c.u[3] baseVertex:(NSInteger)(int64_t)c.u[4] baseInstance:0];
            break;
        case Op::Run: b.runs[c.u[0]](cb); break;
        }
    }
    if (enc) [enc endEncoding];
    if (b.commit) {
        const uint64_t seq = b.seq;
        [cb addCompletedHandler:^(id<MTLCommandBuffer>) {
            std::lock_guard<std::mutex> l(r->lock);
            r->completed = std::max(r->completed, seq);
            r->cv.notify_all();
        }];
        [cb commit];
        r->rt_cmd = nil;
    }
}

void render_thread(Renderer* r, id<MTLCommandQueue> queue) {
    pthread_setname_np("d3d9 render");
    for (;;) {
        Batch* b;
        {
            std::unique_lock<std::mutex> l(r->lock);
            r->cv.wait(l, [&] { return !r->queue.empty(); });
            b = r->queue.front();
            r->queue.pop_front();
        }
        @autoreleasepool {
            encode(r, queue, *b);
        }
        b->clear();
        std::lock_guard<std::mutex> l(r->lock);
        r->pool.push_back(b);
    }
}

// The render targets the device has set now.
void current_targets(Device* d, View colors[4], View* depth) {
    for (int k = 0; k < 4; k++) colors[k] = d->render_targets[k] ? view_of(d->render_targets[k]) : View{};
    *depth = d->depth_stencil ? view_of(d->depth_stencil) : View{};
}

// Starts (or keeps) the pass for the current targets, with a pending clear
// applied as its load action.
bool pass(Device* d) {
    Renderer* r = d->renderer;
    View colors[4], depth;
    current_targets(d, colors, &depth);
    // The depth surface must be at least as large as the colour target (as
    // in Direct3D, whose games pair a screen-sized depth buffer with targets a
    // few rows shorter: Prince of Persia at 1728x1117 draws its scene into a
    // 1728x1112 target). Metal takes a larger one and draws in the common area.
    if (depth.tex && colors[0].tex &&
        (depth.tex.width < std::max<NSUInteger>(1, colors[0].tex.width >> colors[0].level) ||
         depth.tex.height < std::max<NSUInteger>(1, colors[0].tex.height >> colors[0].level))) {
        if (frame_traced(d))
            std::fprintf(stderr, "pass: depth %lux%lu dropped for colour %lux%lu\n", (unsigned long)depth.tex.width,
                         (unsigned long)depth.tex.height, (unsigned long)(colors[0].tex.width >> colors[0].level),
                         (unsigned long)(colors[0].tex.height >> colors[0].level));
        depth = View{};
    }
    bool same = r->in_pass && depth == r->depth;
    for (int k = 0; k < 4; k++) same = same && colors[k] == r->colors[k];
    if (same && !r->pending_clear) return true;
    end_pass(r);
    MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
    for (int k = 0; k < 4; k++) {
        r->colors[k] = colors[k];
        r->color_formats[k] = colors[k].tex ? colors[k].tex.pixelFormat : MTLPixelFormatInvalid;
        if (!colors[k].tex) continue;
        auto* a = rp.colorAttachments[k];
        a.texture = colors[k].tex;
        a.level = colors[k].level;
        a.slice = colors[k].slice;
        a.storeAction = MTLStoreActionStore;
        if (r->pending_clear && (r->clear_flags & 1)) {
            a.loadAction = MTLLoadActionClear;
            const uint32_t c = r->clear_color;
            a.clearColor = MTLClearColorMake((c >> 16 & 0xff) / 255.0, (c >> 8 & 0xff) / 255.0, (c & 0xff) / 255.0,
                                             colors[k].no_alpha ? 1.0 : (c >> 24) / 255.0);
        } else {
            a.loadAction = MTLLoadActionLoad;
        }
    }
    r->depth = depth;
    r->depth_format = depth.tex ? depth.tex.pixelFormat : MTLPixelFormatInvalid;
    r->has_depth = depth.tex != nil;
    r->has_stencil = depth.tex && depth.tex.pixelFormat == MTLPixelFormatDepth32Float_Stencil8;
    if (depth.tex) {
        rp.depthAttachment.texture = depth.tex;
        rp.depthAttachment.storeAction = MTLStoreActionStore;
        rp.depthAttachment.loadAction = r->pending_clear && (r->clear_flags & 2) ? MTLLoadActionClear : MTLLoadActionLoad;
        rp.depthAttachment.clearDepth = r->clear_z;
        if (r->has_stencil) {
            rp.stencilAttachment.texture = depth.tex;
            rp.stencilAttachment.storeAction = MTLStoreActionStore;
            rp.stencilAttachment.loadAction =
                r->pending_clear && (r->clear_flags & 4) ? MTLLoadActionClear : MTLLoadActionLoad;
            rp.stencilAttachment.clearStencil = r->clear_stencil;
        }
    }
    if (!colors[0].tex && !depth.tex) return false;
    if (!colors[0].tex) {
        rp.renderTargetWidth = depth.tex.width;
        rp.renderTargetHeight = depth.tex.height;
    }
    r->pending_clear = false;
    record(r, Op::BeginPass).obj = rp;
    r->in_pass = true;
    r->ps = PassState{};
    return true;
}

// A scaled, format-converting copy as a draw: texels (sx, sy, sw, sh) of
// level `a.level` of a 2D texture into the rectangle (dx, dy, dw, dh) of a
// render-target view. False when the copy cannot be drawn.
const char* const kBlitMsl = R"(
#include <metal_stdlib>
using namespace metal;
struct V { float4 pos [[position]]; float2 uv; };
vertex V blit_vs(uint vid [[vertex_id]], constant float4& src [[buffer(0)]]) {
    const float2 t = float2(vid & 1, vid >> 1);
    V o;
    o.pos = float4(t.x * 2 - 1, 1 - t.y * 2, 0, 1);
    o.uv = src.xy + t * src.zw;
    return o;
}
fragment float4 blit_fs(V in [[stage_in]], texture2d<float> tex [[texture(0)]], sampler s [[sampler(0)]],
                        constant float2& opt [[buffer(0)]]) {
    float4 c = tex.sample(s, in.uv, level(opt.x));
    if (opt.y != 0) c.a = 1;
    return c;
}
)";

// Records the copy (the pipeline and sampler are made here, on the game
// thread; the render thread encodes the pass). `dst` nil: the drawable of
// the frame being presented, in the layer's pixel format.
bool draw_scaled(Device* d, const View& a, NSUInteger sx, NSUInteger sy, NSUInteger sw, NSUInteger sh,
                 id<MTLTexture> dst, MTLPixelFormat dst_format, uint32_t dst_level, uint32_t dst_slice, NSUInteger dx,
                 NSUInteger dy, NSUInteger dw, NSUInteger dh, bool linear,
                 std::function<id<MTLTexture>()> target = nullptr) {
    Renderer* r = d->renderer;
    if (a.tex.textureType != MTLTextureType2D || (dst && !(dst.usage & MTLTextureUsageRenderTarget))) return false;
    const MTLPixelFormat pf = dst ? dst.pixelFormat : dst_format;
    if (pf == MTLPixelFormatDepth32Float || pf == MTLPixelFormatDepth32Float_Stencil8 ||
        a.tex.pixelFormat == MTLPixelFormatDepth32Float || a.tex.pixelFormat == MTLPixelFormatDepth32Float_Stencil8)
        return false;
    __strong id<MTLRenderPipelineState>& ps = r->blit[pf];
    if (!ps) {
        MTLRenderPipelineDescriptor* pd = [[MTLRenderPipelineDescriptor alloc] init];
        pd.vertexFunction = function_of(kBlitMsl, "blit_vs");
        pd.fragmentFunction = function_of(kBlitMsl, "blit_fs");
        pd.colorAttachments[0].pixelFormat = pf;
        NSError* err = nil;
        ps = [d->mtl newRenderPipelineStateWithDescriptor:pd error:&err];
        if (!ps) fatal("d3d9: the copy pipeline does not build: %s", err.localizedDescription.UTF8String);
    }
    __strong id<MTLSamplerState>& smp = r->blit_samplers[linear];
    if (!smp) {
        MTLSamplerDescriptor* sd = [[MTLSamplerDescriptor alloc] init];
        sd.minFilter = sd.magFilter = linear ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
        sd.sAddressMode = sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
        smp = [d->mtl newSamplerStateWithDescriptor:sd];
    }
    const double W = std::max<NSUInteger>(1, a.tex.width >> a.level), H = std::max<NSUInteger>(1, a.tex.height >> a.level);
    const std::array<float, 4> src = {(float)(sx / W), (float)(sy / H), (float)(sw / W), (float)(sh / H)};
    const std::array<float, 2> opt = {(float)a.level, a.no_alpha ? 1.0f : 0.0f};
    id<MTLRenderPipelineState> pso = ps;
    id<MTLSamplerState> sampler = smp;
    id<MTLTexture> from = a.tex;
    end_pass(r);
    record_run(r, [=](id<MTLCommandBuffer> cb) {
        id<MTLTexture> to = dst ? dst : target();
        if (!to) return;
        MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = to;
        rp.colorAttachments[0].level = dst_level;
        rp.colorAttachments[0].slice = dst_slice;
        rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        const NSUInteger w = dst ? dw : to.width, h = dst ? dh : to.height;
        id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
        [enc setRenderPipelineState:pso];
        [enc setViewport:(MTLViewport){(double)dx, (double)dy, (double)w, (double)h, 0, 1}];
        [enc setVertexBytes:src.data() length:sizeof(float) * 4 atIndex:0];
        [enc setFragmentBytes:opt.data() length:sizeof(float) * 2 atIndex:0];
        [enc setFragmentTexture:from atIndex:0];
        [enc setFragmentSamplerState:sampler atIndex:0];
        [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
        [enc endEncoding];
    });
    return true;
}

uint32_t vertex_count(uint32_t prim, uint32_t prims) {
    switch (prim) {
    case 1: return prims;          // POINTLIST
    case 2: return 2 * prims;      // LINELIST
    case 3: return prims + 1;      // LINESTRIP
    case 4: return 3 * prims;      // TRIANGLELIST
    case 5: case 6: return prims + 2;  // TRIANGLESTRIP, TRIANGLEFAN
    default: return 0;
    }
}

MTLPrimitiveType primitive(uint32_t prim) {
    switch (prim) {
    case 1: return MTLPrimitiveTypePoint;
    case 2: return MTLPrimitiveTypeLine;
    case 3: return MTLPrimitiveTypeLineStrip;
    case 5: return MTLPrimitiveTypeTriangleStrip;
    default: return MTLPrimitiveTypeTriangle;
    }
}

uint32_t upload_bytes(Device* d, const void* data, uint32_t n) {
    Renderer* r = d->renderer;
    const uint32_t off = (r->upload_used + 255) & ~255u;
    if (off + n > kUploadSize) fatal("d3d9: more than %u bytes of DrawPrimitiveUP data in a frame", kUploadSize);
    std::memcpy((uint8_t*)r->upload.contents + off, data, n);
    r->upload_used = off + n;
    return off;
}

struct Streams {
    // Per stream: the buffer and offset to bind (nil: none).
    id<MTLBuffer> buffer[16] = {};
    uint32_t offset[16] = {}, stride[16] = {};
};

id<MTLSamplerState> sampler_of(Renderer* r, const uint32_t* ss) {
    const uint64_t key = (uint64_t)ss[1] | (uint64_t)ss[2] << 3 | (uint64_t)ss[3] << 6 | (uint64_t)ss[5] << 9 |
                         (uint64_t)ss[6] << 12 | (uint64_t)ss[7] << 15 | (uint64_t)std::min<uint32_t>(ss[10], 16) << 18 |
                         (uint64_t)ss[9] << 24 | (uint64_t)(ss[4] != 0) << 40;
    auto it = r->samplers.find(key);
    if (it != r->samplers.end()) return it->second;
    MTLSamplerDescriptor* sd = [[MTLSamplerDescriptor alloc] init];
    sd.sAddressMode = address(ss[1]);
    sd.tAddressMode = address(ss[2]);
    sd.rAddressMode = address(ss[3]);
    auto filter = [](uint32_t f) { return f >= 2 ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest; };
    sd.magFilter = filter(ss[5]);
    sd.minFilter = filter(ss[6]);
    sd.mipFilter = ss[7] == 0 ? MTLSamplerMipFilterNotMipmapped : ss[7] == 1 ? MTLSamplerMipFilterNearest
                                                                             : MTLSamplerMipFilterLinear;
    if ((ss[5] == 3 || ss[6] == 3) && ss[10] > 1) sd.maxAnisotropy = std::min<uint32_t>(ss[10], 16);
    // SCIMITAR_ANISO=N (2..16): at least N-times anisotropic filtering for the
    // linearly filtered, mipmapped textures.
    static const uint32_t forced_aniso = [] {
        const char* v = std::getenv("SCIMITAR_ANISO");
        return v ? (uint32_t)std::clamp(std::strtol(v, nullptr, 10), 1L, 16L) : 1u;
    }();
    if (forced_aniso > 1 && ss[6] >= 2 && ss[7] != 0 && sd.maxAnisotropy < forced_aniso) sd.maxAnisotropy = forced_aniso;
    sd.lodMinClamp = (float)ss[9];  // D3DSAMP_MAXMIPLEVEL: the largest level used
    sd.borderColor = ss[4] >> 24 ? MTLSamplerBorderColorOpaqueWhite : MTLSamplerBorderColorTransparentBlack;
    if (ss[4] && (ss[4] & 0xffffff) == 0 && (ss[4] >> 24)) sd.borderColor = MTLSamplerBorderColorOpaqueBlack;
    id<MTLSamplerState> s = [metal_device() newSamplerStateWithDescriptor:sd];
    r->samplers[key] = s;
    return s;
}

id<MTLDepthStencilState> depth_state_of(Device* d) {
    Renderer* r = d->renderer;
    const uint32_t* rs = d->render_state;
    const bool z = rs[7] && r->has_depth, zwrite = z && rs[14], stencil = rs[52] && r->has_stencil;
    const uint64_t key = (uint64_t)z | (uint64_t)zwrite << 1 | (uint64_t)(z ? rs[23] : 8) << 2 |
                         (uint64_t)stencil << 6 |
                         (stencil ? ((uint64_t)rs[53] << 7 | (uint64_t)rs[54] << 11 | (uint64_t)rs[55] << 15 |
                                     (uint64_t)rs[56] << 19 | (uint64_t)(rs[58] & 0xff) << 23 |
                                     (uint64_t)(rs[59] & 0xff) << 31 | (uint64_t)rs[185] << 39 |
                                     (uint64_t)rs[186] << 40 | (uint64_t)rs[187] << 44 | (uint64_t)rs[188] << 48 |
                                     (uint64_t)rs[189] << 52)
                                  : 0);
    auto it = r->depth_states.find(key);
    if (it != r->depth_states.end()) return it->second;
    MTLDepthStencilDescriptor* dd = [[MTLDepthStencilDescriptor alloc] init];
    dd.depthCompareFunction = z ? compare(rs[23]) : MTLCompareFunctionAlways;
    dd.depthWriteEnabled = zwrite;
    if (stencil) {
        MTLStencilDescriptor* front = [[MTLStencilDescriptor alloc] init];
        front.stencilFailureOperation = stencil_op(rs[53]);
        front.depthFailureOperation = stencil_op(rs[54]);
        front.depthStencilPassOperation = stencil_op(rs[55]);
        front.stencilCompareFunction = compare(rs[56]);
        front.readMask = rs[58] & 0xff;
        front.writeMask = rs[59] & 0xff;
        MTLStencilDescriptor* back = front;
        if (rs[185]) {  // TWOSIDEDSTENCILMODE
            back = [[MTLStencilDescriptor alloc] init];
            back.stencilFailureOperation = stencil_op(rs[186]);
            back.depthFailureOperation = stencil_op(rs[187]);
            back.depthStencilPassOperation = stencil_op(rs[188]);
            back.stencilCompareFunction = compare(rs[189]);
            back.readMask = rs[58] & 0xff;
            back.writeMask = rs[59] & 0xff;
        }
        dd.frontFaceStencil = front;
        dd.backFaceStencil = back;
    }
    id<MTLDepthStencilState> s = [metal_device() newDepthStencilStateWithDescriptor:dd];
    r->depth_states[key] = s;
    return s;
}

// Records everything a draw needs set on the encoder; false when it cannot be drawn.
bool prepare(Device* d, const Streams& st) {
    Renderer* r = d->renderer;
    Shader* vs = as<Shader>(d->vs);
    Shader* ps = as<Shader>(d->ps);
    Declaration* decl = as<Declaration>(d->decl);
    if (!vs || !ps || !decl) {
        r->skipped++;
        return false;  // fixed-function and FVF draws: not drawn
    }
    Compiled* cv = compile(vs);
    Compiled* cp = compile(ps);
    if (!cp->fragment.ok) return false;
    // SCIMITAR_D3D_SKIP_PS=id,...: draws with these pixel shaders are left out (debugging).
    static const std::vector<int> skip_ps = [] {
        std::vector<int> v;
        for (const char* e = std::getenv("SCIMITAR_D3D_SKIP_PS"); e && *e;) {
            char* end = nullptr;
            v.push_back((int)std::strtol(e, &end, 10));
            e = *end == ',' ? end + 1 : end;
            if (end == e && *e) break;
        }
        return v;
    }();
    for (int id : skip_ps)
        if (id == cp->id) return false;
    // SCIMITAR_D3D_SKIP_TEX=WxH: draws sampling a texture of that size on unit 0 are left out (debugging).
    static const char* skip_tex = std::getenv("SCIMITAR_D3D_SKIP_TEX");
    if (skip_tex)
        if (Texture* t0 = as<Texture>(d->textures[0])) {
            char sz[32];
            std::snprintf(sz, sizeof sz, "%ux%u", t0->width, t0->height);
            if (std::strstr(skip_tex, sz)) return false;
        }
    // Vertex attributes from the declaration: what the program reads, by
    // stream (the descriptor itself is built only for a new pipeline).
    struct Attr {
        int a;
        uint32_t stream, offset;
        VertexFormat vf;
    };
    Attr attrs[32];
    int nattrs = 0;
    uint32_t declared = 0, uint_attribs = 0, int_attribs = 0;
    for (size_t k = 0; k + 8 <= decl->elements.size(); k += 8) {
        const uint8_t* e = &decl->elements[k];
        const uint32_t stream = e[0] | e[1] << 8, offset = e[2] | e[3] << 8, type = e[4], usage = e[6], index = e[7];
        if (stream == 0xff || stream >= 16) break;
        const int a = attribute_of(usage, index);
        const VertexFormat vf = vertex_format(type);
        if (a < 0 || vf.format == MTLVertexFormatInvalid || (declared >> a & 1) || nattrs == 32) continue;
        declared |= 1u << a;
        if (vf.kind == 1) uint_attribs |= 1u << a;
        if (vf.kind == 2) int_attribs |= 1u << a;
        attrs[nattrs++] = {a, stream, offset, vf};
    }
    const arb::Program& pv = vertex_program(cv, cp->fragment.varyings, uint_attribs, int_attribs);
    if (!pv.ok) return false;
    const uint32_t zeros = pv.attribs & ~declared;  // read but not declared: zeros
    uint32_t used_streams = 0, instanced = 0;
    for (int k = 0; k < nattrs; k++)
        if (pv.attribs >> attrs[k].a & 1) used_streams |= 1u << attrs[k].stream;
    // The pipeline's key: everything the descriptor below depends on.
    const uint32_t* rs = d->render_state;
    PipelineKey key{};
    key.w[0] = (uint64_t)(uintptr_t)cv;
    key.w[1] = (uint64_t)(uintptr_t)cp;
    key.w[2] = (uint64_t)(uintptr_t)decl;
    for (uint32_t s = 0; s < 16; s++) {
        if (!(used_streams >> s & 1)) continue;
        const uint64_t stride = st.buffer[s] ? st.stride[s] : 0;
        key.w[3 + s / 4] |= (stride & 0xffff) << (16 * (s % 4));
        if ((d->stream_freq[s] >> 30) == 2) instanced |= 1u << s;
    }
    key.w[7] = (uint64_t)used_streams | (uint64_t)instanced << 16 | (uint64_t)pv.varyings << 32 |
               (uint64_t)(r->colors[0].no_alpha | r->colors[1].no_alpha << 1 | r->colors[2].no_alpha << 2 |
                          r->colors[3].no_alpha << 3) << 48;
    key.w[8] = (uint64_t)r->color_formats[0] | (uint64_t)r->color_formats[1] << 16 |
               (uint64_t)r->color_formats[2] << 32 | (uint64_t)r->color_formats[3] << 48;
    key.w[9] = (uint64_t)r->depth_format | (uint64_t)r->has_stencil << 16 | (uint64_t)(rs[15] ? rs[25] : 8) << 20 |
               (uint64_t)(rs[168] & 15) << 24 | (uint64_t)(rs[190] & 15) << 28 | (uint64_t)(rs[191] & 15) << 32 |
               (uint64_t)(rs[192] & 15) << 36;
    if (rs[27])
        key.w[10] = 1 | (uint64_t)(rs[19] & 31) << 1 | (uint64_t)(rs[20] & 31) << 6 | (uint64_t)(rs[171] & 7) << 11 |
                    (uint64_t)(rs[206] != 0) << 14 |
                    (rs[206] ? ((uint64_t)(rs[207] & 31) << 15 | (uint64_t)(rs[208] & 31) << 20 |
                                (uint64_t)(rs[209] & 7) << 25)
                             : 0);
    auto it = r->pipelines.find(key);
    id<MTLRenderPipelineState> pso;
    if (it != r->pipelines.end()) {
        pso = it->second;
    } else {
        MTLVertexDescriptor* vd = [MTLVertexDescriptor vertexDescriptor];
        for (int k = 0; k < nattrs; k++) {
            const Attr& at = attrs[k];
            if (!(pv.attribs >> at.a & 1)) continue;
            vd.attributes[at.a].format = at.vf.format;
            vd.attributes[at.a].offset = at.offset;
            vd.attributes[at.a].bufferIndex = 1 + at.stream;
        }
        for (int a = 0; a < 31; a++)
            if (zeros >> a & 1) {
                vd.attributes[a].format = MTLVertexFormatFloat4;
                vd.attributes[a].offset = 0;
                vd.attributes[a].bufferIndex = 30;
            }
        for (uint32_t s = 0; s < 16; s++) {
            if (!(used_streams >> s & 1)) continue;
            if (!st.buffer[s] || !st.stride[s]) {
                // A stream with stride 0 is a constant for every vertex.
                vd.layouts[1 + s].stepFunction = MTLVertexStepFunctionConstant;
                vd.layouts[1 + s].stepRate = 0;
                vd.layouts[1 + s].stride = 16;
            } else {
                vd.layouts[1 + s].stride = st.stride[s];
                vd.layouts[1 + s].stepFunction =
                    (instanced >> s & 1) ? MTLVertexStepFunctionPerInstance : MTLVertexStepFunctionPerVertex;
                vd.layouts[1 + s].stepRate = 1;
            }
        }
        if (zeros) {
            vd.layouts[30].stride = 16;
            vd.layouts[30].stepFunction = MTLVertexStepFunctionConstant;
            vd.layouts[30].stepRate = 0;
        }
        MTLRenderPipelineDescriptor* pd = [[MTLRenderPipelineDescriptor alloc] init];
        pd.vertexFunction = function_of(pv.msl, "vs_main");
        MTLFunctionConstantValues* fc = [[MTLFunctionConstantValues alloc] init];
        // ALPHATESTENABLE -> the alpha function minus D3DCMP_NEVER (GL's order is the same).
        uint32_t alpha = rs[15] ? (rs[25] >= 1 && rs[25] <= 8 ? rs[25] - 1 : 7) : 7;
        [fc setConstantValue:&alpha type:MTLDataTypeUInt atIndex:0];
        NSError* err = nil;
        {
            auto lib = g_libraries.find(cp->fragment.msl);
            if (lib == g_libraries.end()) function_of(cp->fragment.msl, "ps_main"), lib = g_libraries.find(cp->fragment.msl);
            pd.fragmentFunction = lib->second ? [lib->second newFunctionWithName:@"ps_main" constantValues:fc error:&err] : nil;
        }
        pd.vertexDescriptor = vd;
        const uint32_t masks[4] = {rs[168], rs[190], rs[191], rs[192]};
        for (int k = 0; k < 4; k++) {
            if (r->color_formats[k] == MTLPixelFormatInvalid) continue;
            auto* ca = pd.colorAttachments[k];
            ca.pixelFormat = r->color_formats[k];
            // X8R8G8B8 targets keep alpha 1 (blending with destination alpha reads 1).
            const uint32_t m = masks[k] & (r->colors[k].no_alpha ? 7u : 15u);
            // D3DCOLORWRITEENABLE red 1, green 2, blue 4, alpha 8.
            ca.writeMask = (m & 1 ? MTLColorWriteMaskRed : 0) | (m & 2 ? MTLColorWriteMaskGreen : 0) |
                           (m & 4 ? MTLColorWriteMaskBlue : 0) | (m & 8 ? MTLColorWriteMaskAlpha : 0);
            if (rs[27]) {
                ca.blendingEnabled = YES;
                ca.sourceRGBBlendFactor = blend_factor(rs[19], false);
                ca.destinationRGBBlendFactor = blend_factor(rs[20], false);
                ca.rgbBlendOperation = blend_op(rs[171]);
                if (rs[206]) {
                    ca.sourceAlphaBlendFactor = blend_factor(rs[207], true);
                    ca.destinationAlphaBlendFactor = blend_factor(rs[208], true);
                    ca.alphaBlendOperation = blend_op(rs[209]);
                } else {
                    ca.sourceAlphaBlendFactor = blend_factor(rs[19], true);
                    ca.destinationAlphaBlendFactor = blend_factor(rs[20], true);
                    ca.alphaBlendOperation = blend_op(rs[171]);
                }
            }
        }
        if (r->depth_format != MTLPixelFormatInvalid) pd.depthAttachmentPixelFormat = r->depth_format;
        if (r->has_stencil) pd.stencilAttachmentPixelFormat = r->depth_format;
        if (!pd.vertexFunction || !pd.fragmentFunction) {
            pso = nil;
        } else {
            pso = [metal_device() newRenderPipelineStateWithDescriptor:pd error:&err];
            if (!pso) std::fprintf(stderr, "d3d9: pipeline: %s\n", err.localizedDescription.UTF8String);
        }
        r->pipelines[key] = pso;
    }
    if (!pso) return false;
    if (changed(r, kPipeline, (__bridge const void*)pso, 0)) record(r, Op::Pipeline).obj = pso;
    id<MTLDepthStencilState> dss = depth_state_of(d);
    if (changed(r, kDepthState, (__bridge const void*)dss, 0)) record(r, Op::DepthStencil).obj = dss;
    if (changed(r, kStencilRef, nullptr, rs[57] & 0xff)) record(r, Op::StencilRef).u[0] = rs[57] & 0xff;
    const uint64_t cull = rs[22] == 2 ? MTLCullModeFront : rs[22] == 3 ? MTLCullModeBack : MTLCullModeNone;
    if (changed(r, kCull, nullptr, cull)) record(r, Op::Cull).u[0] = cull;
    const uint64_t fillmode = rs[8] == 2 ? MTLTriangleFillModeLines : MTLTriangleFillModeFill;
    if (changed(r, kFill, nullptr, fillmode)) record(r, Op::Fill).u[0] = fillmode;
    float bias, slope;
    std::memcpy(&bias, &rs[195], 4);
    std::memcpy(&slope, &rs[175], 4);
    bias *= 16777216.0f;
    uint32_t bw, sw;
    std::memcpy(&bw, &bias, 4), std::memcpy(&sw, &slope, 4);
    if (changed(r, kBias, nullptr, (uint64_t)bw << 32 | sw)) {
        Cmd& c = record(r, Op::DepthBias);
        c.u[0] = bw, c.u[1] = sw;
    }
    const uint32_t bf = rs[193];
    if (changed(r, kBlend, nullptr, bf)) {
        const float rgba[4] = {(bf >> 16 & 0xff) / 255.0f, (bf >> 8 & 0xff) / 255.0f, (bf & 0xff) / 255.0f,
                               (bf >> 24) / 255.0f};
        Cmd& c = record(r, Op::BlendColor);
        for (int k = 0; k < 4; k++) std::memcpy(&c.u[k], &rgba[k], 4);
    }
    // Viewport and scissor, within the target.
    const NSUInteger tw = r->colors[0].tex ? std::max<NSUInteger>(1, r->colors[0].tex.width >> r->colors[0].level)
                                           : r->depth.tex.width;
    const NSUInteger th = r->colors[0].tex ? std::max<NSUInteger>(1, r->colors[0].tex.height >> r->colors[0].level)
                                           : r->depth.tex.height;
    float minz, maxz;
    std::memcpy(&minz, &d->viewport[4], 4);
    std::memcpy(&maxz, &d->viewport[5], 4);
    const uint32_t vx = std::min<uint32_t>(d->viewport[0], (uint32_t)tw - 1), vy = std::min<uint32_t>(d->viewport[1], (uint32_t)th - 1);
    const uint32_t vw = std::min<uint32_t>(d->viewport[2], (uint32_t)tw - vx), vh = std::min<uint32_t>(d->viewport[3], (uint32_t)th - vy);
    {
        const double vp[6] = {(double)vx, (double)vy, (double)std::max(vw, 1u), (double)std::max(vh, 1u), minz, maxz};
        if (changed(r, kViewport, nullptr, bits_of(vp, sizeof vp))) std::memcpy(record(r, Op::Viewport).u, vp, sizeof vp);
    }
    MTLScissorRect sc = {0, 0, tw, th};
    if (rs[174]) {
        const int32_t x0 = std::max<int32_t>(0, (int32_t)d->scissor[0]), y0 = std::max<int32_t>(0, (int32_t)d->scissor[1]);
        const int32_t x1 = std::min<int32_t>((int32_t)tw, (int32_t)d->scissor[2]), y1 = std::min<int32_t>((int32_t)th, (int32_t)d->scissor[3]);
        sc = {(NSUInteger)std::min<int32_t>(x0, (int32_t)tw - 1), (NSUInteger)std::min<int32_t>(y0, (int32_t)th - 1),
              (NSUInteger)std::max(1, x1 - x0), (NSUInteger)std::max(1, y1 - y0)};
        sc.width = std::min<NSUInteger>(sc.width, tw - sc.x);
        sc.height = std::min<NSUInteger>(sc.height, th - sc.y);
    }
    {
        const uint64_t scv[4] = {sc.x, sc.y, sc.width, sc.height};
        if (changed(r, kScissor, nullptr, bits_of(scv, sizeof scv))) std::memcpy(record(r, Op::Scissor).u, scv, sizeof scv);
    }
    // Constants.
    // A program's parameters from the constants (bool registers read as 0 or
    // 1 from the float registers that stand for them).
    auto fill = [&](const arb::Program& p, const std::vector<float>& consts, const uint32_t* bools, Compiled* cc,
                    std::vector<float>* out) {
        if (cc->bool_of.empty()) {
            int top = 0;
            for (const auto& [b, f] : cc->bools) top = std::max(top, f + 1);
            cc->bool_of.assign(top + 1, -1);
            for (const auto& [b, f] : cc->bools) cc->bool_of[f] = (int8_t)b;
        }
        const std::vector<int8_t>& bool_of = cc->bool_of;
        out->resize(4 * std::max<size_t>(1, p.params.size()));
        for (size_t k = 0; k < p.params.size(); k++) {
            const arb::ParamRef& ref = p.params[k];
            float* v = &(*out)[4 * k];
            if (ref.kind == arb::ParamRef::Literal) {
                std::memcpy(v, ref.value, 16);
                continue;
            }
            const int reg = ref.index;
            if ((size_t)reg < bool_of.size() && bool_of[reg] >= 0) v[0] = v[1] = v[2] = v[3] = bools[bool_of[reg]] ? 1.0f : 0.0f;
            else std::memcpy(v, (size_t)(4 * reg + 4) <= consts.size() ? &consts[4 * reg] : consts.data(), 16);
        }
    };
    // SCIMITAR_D3D_TRACE_FRAME=N: every draw of frame N, with its shaders,
    // textures and the fragment program's constants.
    if (frame_traced(d)) {
        std::fprintf(stderr, "draw: vs %d ps %d rt %lux%lu fmt %d blend %u (%u,%u) z %u ztest %u", cv->id, cp->id,
                     (unsigned long)tw, (unsigned long)th, (int)r->color_formats[0], rs[27], rs[19], rs[20], rs[14], rs[7]);
        const uint32_t rt0 = d->render_targets[0];
        if (Surface* s0 = as<Surface>(rt0))
            std::fprintf(stderr, " rt0 %08x f%x%s", rt0, s0->format, rt0 == d->backbuffer ? " (back buffer)" : "");
        else
            std::fprintf(stderr, " rt0 %08x %s", rt0, native(rt0) ? "(not a surface)" : "(released)");
        for (int u = 0; u < 16; u++)
            if (Texture* tx = as<Texture>(d->textures[u]))
                std::fprintf(stderr, " t%d %ux%u f%x u%x%s", u, tx->width, tx->height, tx->format, tx->usage,
                             tx->cube ? " cube" : "");
        std::fprintf(stderr, "\n");
        // SCIMITAR_D3D_TRACE_PARAMS=N: that many of each program's constants
        // (default 12 fragment, no vertex ones).
        static const size_t nparams =
            std::getenv("SCIMITAR_D3D_TRACE_PARAMS") ? std::strtoul(std::getenv("SCIMITAR_D3D_TRACE_PARAMS"), nullptr, 10) : 0;
        for (size_t k = 0; k < pv.params.size() && k < nparams; k++) {
            const arb::ParamRef& ref = pv.params[k];
            const float* v = ref.kind == arb::ParamRef::Literal ? ref.value : &d->vs_const[4 * std::min<int>(ref.index, 255)];
            std::fprintf(stderr, "   V%zu %c%d = %g %g %g %g\n", k, ref.kind == arb::ParamRef::Literal ? 'L' : 'c',
                         ref.index, v[0], v[1], v[2], v[3]);
        }
        for (size_t k = 0; k < cp->fragment.params.size() && k < (nparams ? nparams : 12); k++) {
            const arb::ParamRef& ref = cp->fragment.params[k];
            const float* v = ref.kind == arb::ParamRef::Literal ? ref.value : &d->ps_const[4 * std::min<int>(ref.index, 223)];
            std::fprintf(stderr, "   P%zu %c%d = %g %g %g %g\n", k, ref.kind == arb::ParamRef::Literal ? 'L' : 'c',
                         ref.index, v[0], v[1], v[2], v[3]);
        }
    }
    const float hp[2] = {1.0f / std::max(vw, 1u), 1.0f / std::max(vh, 1u)};
    record_bytes(r, Op::VertexBytes, 29, hp, sizeof hp);
    static thread_local std::vector<float> P;
    fill(pv, d->vs_const, d->vs_bool, cv, &P);
    if (!pv.params.empty()) record_bytes(r, Op::VertexBytes, 0, P.data(), P.size() * 4);
    fill(cp->fragment, d->ps_const, d->ps_bool, cp, &P);
    if (!cp->fragment.params.empty()) record_bytes(r, Op::FragmentBytes, 0, P.data(), P.size() * 4);
    struct {
        float alpha_ref;
        float lod_bias[16];
    } fx{};
    fx.alpha_ref = (rs[24] & 0xff) / 255.0f;
    for (int u = 0; u < 16; u++) std::memcpy(&fx.lod_bias[u], &d->sampler_state[u][8], 4);
    record_bytes(r, Op::FragmentBytes, 1, &fx, sizeof fx);
    // Streams.
    auto vertex_buffer = [&](int index, id<MTLBuffer> buf, uint64_t offset) {
        if (!changed(r, kVertexBuffer + index, (__bridge const void*)buf, offset)) return;
        Cmd& c = record(r, Op::VertexBuffer, (uint8_t)index);
        c.obj = buf, c.u[0] = offset;
    };
    for (uint32_t s = 0; s < 16; s++)
        if (used_streams >> s & 1) {
            if (st.buffer[s]) vertex_buffer(1 + s, st.buffer[s], st.offset[s]);
            else vertex_buffer(1 + s, r->zeros, 0);
        }
    vertex_buffer(30, r->zeros, 0);
    // Textures.
    for (int u = 0; u < 16; u++) {
        const arb::TexTarget t = cp->fragment.textures[u];
        if (t == arb::TexTarget::None) continue;
        id<MTLTexture> tex = nil;
        if (Texture* tx = as<Texture>(d->textures[u])) {
            auto fb = r->feedback.find(tx);
            tex = fb != r->feedback.end() ? fb->second : sampled_view(tx);
        }
        else if (Surface* sf = as<Surface>(d->textures[u])) tex = view_of(d->textures[u]).tex, (void)sf;
        const bool want_cube = t == arb::TexTarget::Cube, want_3d = t == arb::TexTarget::T3D;
        static const bool no_textures = std::getenv("SCIMITAR_D3D_NO_TEXTURES") != nullptr;  // debugging
        if (no_textures) tex = nil;
        if (!tex || (tex.textureType == MTLTextureTypeCube) != want_cube || (tex.textureType == MTLTextureType3D) != want_3d)
            tex = want_cube ? r->whitecube : want_3d ? r->white3d : r->white2d;
        if (changed(r, kTexture + u, (__bridge const void*)tex, 0)) record(r, Op::FragmentTexture, (uint8_t)u).obj = tex;
        id<MTLSamplerState> smp = sampler_of(r, d->sampler_state[u]);
        if (changed(r, kSampler + u, (__bridge const void*)smp, 0)) record(r, Op::FragmentSampler, (uint8_t)u).obj = smp;
    }
    return true;
}

Streams streams_of(Device* d) {
    Streams st;
    for (int s = 0; s < 16; s++) {
        Buffer* b = as<Buffer>(d->streams[s][0]);
        if (!b) continue;
        b->used = true;
        st.buffer[s] = b->mtl;
        st.offset[s] = b->offset + d->streams[s][1];
        st.stride[s] = d->streams[s][2];
    }
    return st;
}

uint32_t instances(Device* d) {
    const uint32_t f = d->stream_freq[0];
    return (f >> 30) == 1 ? std::max(1u, f & 0x3fffffff) : 1;
}

void record_draw(Renderer* r, MTLPrimitiveType type, uint64_t start, uint64_t count, uint64_t instances) {
    Cmd& c = record(r, Op::Draw);
    c.u[0] = type, c.u[1] = start, c.u[2] = count, c.u[3] = instances;
}
void record_draw_indexed(Renderer* r, MTLPrimitiveType type, uint64_t count, MTLIndexType itype, id<MTLBuffer> ib,
                         uint64_t offset, uint64_t instances, int64_t base) {
    Cmd& c = record(r, Op::DrawIndexed);
    c.obj = ib;
    c.u[0] = (uint64_t)type | (uint64_t)itype << 8, c.u[1] = count, c.u[2] = offset, c.u[3] = instances;
    c.u[4] = (uint64_t)base;
}

// Fans become lists (Metal has none): indices into the fan's vertices.
std::vector<uint32_t> fan_indices(uint32_t prims, const std::function<uint32_t(uint32_t)>& index) {
    std::vector<uint32_t> v;
    v.reserve(3 * prims);
    for (uint32_t k = 0; k < prims; k++) v.push_back(index(0)), v.push_back(index(k + 1)), v.push_back(index(k + 2));
    return v;
}

void write_ppm(const std::string& path, id<MTLBuffer> out, NSUInteger w, NSUInteger h);

void save_screenshot(Device* d) {
    static const char* dir = std::getenv("SCIMITAR_SCREENSHOT");
    if (!dir) return;
    static const uint64_t every = std::getenv("SCIMITAR_SCREENSHOT_EVERY") ? std::strtoull(std::getenv("SCIMITAR_SCREENSHOT_EVERY"), nullptr, 10) : 300;
    if (!every || d->frames % every) return;
    // SCIMITAR_SCREENSHOT_FROM / _COUNT: only frames from N on, at most M of them.
    static const uint64_t from = std::getenv("SCIMITAR_SCREENSHOT_FROM") ? std::strtoull(std::getenv("SCIMITAR_SCREENSHOT_FROM"), nullptr, 10) : 0;
    static uint64_t left = std::getenv("SCIMITAR_SCREENSHOT_COUNT") ? std::strtoull(std::getenv("SCIMITAR_SCREENSHOT_COUNT"), nullptr, 10) : UINT64_MAX;
    if (d->frames < from || !left) return;
    left--;
    Renderer* r = d->renderer;
    id<MTLTexture> src = r->backbuffer_tex;
    if (!src) return;
    const NSUInteger w = src.width, h = src.height;
    id<MTLBuffer> out = [d->mtl newBufferWithLength:w * h * 4 options:MTLResourceStorageModeShared];
    ::mkdir(dir, 0755);
    char name[1024];
    std::snprintf(name, sizeof name, "%s/frame_%06llu.ppm", dir, (unsigned long long)d->frames);
    const std::string path = name;
    // Copied at the end of the frame; written once the GPU has finished it.
    end_pass(r);
    record_run(r, [=](id<MTLCommandBuffer> cb) {
        id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
        [bl copyFromTexture:src sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                 sourceSize:MTLSizeMake(w, h, 1) toBuffer:out destinationOffset:0 destinationBytesPerRow:w * 4
          destinationBytesPerImage:w * h * 4];
        [bl endEncoding];
        [cb addCompletedHandler:^(id<MTLCommandBuffer>) { write_ppm(path, out, w, h); }];
    });
}

void write_ppm(const std::string& path, id<MTLBuffer> out, NSUInteger w, NSUInteger h) {
    // At most 1280 pixels wide (every k-th pixel): screenshots are for checking.
    const NSUInteger k = (w + 1279) / 1280, ow = w / k, oh = h / k;
    if (FILE* f = std::fopen(path.c_str(), "wb")) {
        std::fprintf(f, "P6\n%lu %lu\n255\n", (unsigned long)ow, (unsigned long)oh);
        const uint8_t* p = (const uint8_t*)out.contents;
        std::vector<uint8_t> row(ow * 3);
        for (NSUInteger y = 0; y < oh; y++) {
            for (NSUInteger x = 0; x < ow; x++) {
                const uint8_t* px = p + 4 * (y * k * w + x * k);
                row[3 * x] = px[2], row[3 * x + 1] = px[1], row[3 * x + 2] = px[0];
            }
            std::fwrite(row.data(), 1, row.size(), f);
        }
        std::fclose(f);
    }
}

// SCIMITAR_D3D_DUMP_PASSES=dir (with SCIMITAR_D3D_TRACE_FRAME=N): each
// pass's colour targets of frame N as they are when the pass ends, converted
// to 8 bits (debugging).
void dump_pass(Device* d) {
    static const char* dir = std::getenv("SCIMITAR_D3D_DUMP_PASSES");
    if (!dir || !frame_traced(d)) return;
    Renderer* r = d->renderer;
    static int n = 0;
    ::mkdir(dir, 0755);
    for (int k = 0; k < 4; k++) {
        const View v = r->colors[k];
        if (!v.tex || v.tex.textureType != MTLTextureType2D) continue;
        const NSUInteger w = std::max<NSUInteger>(1, v.tex.width >> v.level), h = std::max<NSUInteger>(1, v.tex.height >> v.level);
        if (v.tex.pixelFormat == MTLPixelFormatR32Float) {  // raw floats
            id<MTLBuffer> out = [d->mtl newBufferWithLength:w * h * 4 options:MTLResourceStorageModeShared];
            char name[1024];
            std::snprintf(name, sizeof name, "%s/f%llu_pass_%04d_rt%d_%lux%lu.f32", dir, (unsigned long long)d->frames, n, k, (unsigned long)w, (unsigned long)h);
            const std::string path = name;
            id<MTLTexture> src = v.tex;
            const uint32_t level = v.level;
            end_pass(r);
            record_run(r, [=](id<MTLCommandBuffer> cb) {
                id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
                [bl copyFromTexture:src sourceSlice:0 sourceLevel:level sourceOrigin:MTLOriginMake(0, 0, 0)
                         sourceSize:MTLSizeMake(w, h, 1) toBuffer:out destinationOffset:0 destinationBytesPerRow:w * 4
                  destinationBytesPerImage:w * h * 4];
                [bl endEncoding];
                [cb addCompletedHandler:^(id<MTLCommandBuffer>) {
                    if (FILE* f = std::fopen(path.c_str(), "wb")) std::fwrite(out.contents, 4, w * h, f), std::fclose(f);
                }];
            });
            continue;
        }
        MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                     width:w height:h mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        td.storageMode = MTLStorageModePrivate;
        id<MTLTexture> tmp = [d->mtl newTextureWithDescriptor:td];
        View src = v;
        src.no_alpha = false;
        if (!draw_scaled(d, src, 0, 0, w, h, tmp, MTLPixelFormatBGRA8Unorm, 0, 0, 0, 0, w, h, false)) continue;
        id<MTLBuffer> out = [d->mtl newBufferWithLength:w * h * 4 options:MTLResourceStorageModeShared];
        char name[1024];
        std::snprintf(name, sizeof name, "%s/f%llu_pass_%04d_rt%d_%lux%lu_pf%d.ppm", dir, (unsigned long long)d->frames, n, k, (unsigned long)w,
                      (unsigned long)h, (int)v.tex.pixelFormat);
        const std::string path = name;
        record_run(r, [=](id<MTLCommandBuffer> cb) {
            id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
            [bl copyFromTexture:tmp sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                     sourceSize:MTLSizeMake(w, h, 1) toBuffer:out destinationOffset:0 destinationBytesPerRow:w * 4
              destinationBytesPerImage:w * h * 4];
            [bl endEncoding];
            [cb addCompletedHandler:^(id<MTLCommandBuffer>) { write_ppm(path, out, w, h); }];
        });
    }
    std::fprintf(stderr, "dump: frame %llu pass %d ends\n", (unsigned long long)d->frames, n);
    n++;
}

}  // namespace

void r_init(Device* d) {
    auto* r = new Renderer;
    r->dev = d;
    d->renderer = r;
    d->mtl = metal_device();
    if (!d->queue) d->queue = [d->mtl newCommandQueue];
    r->zeros = [d->mtl newBufferWithLength:256 options:MTLResourceStorageModeShared];
    std::memset(r->zeros.contents, 0, 256);
    for (auto& u : r->uploads) u = [d->mtl newBufferWithLength:kUploadSize options:MTLResourceStorageModeShared];
    r->upload = r->uploads[0];
    const uint32_t white[6] = {0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff};
    Format f;
    f.pf = MTLPixelFormatBGRA8Unorm;
    r->white2d = make_texture(MTLTextureType2D, f, 1, 1, 1, 1, false);
    [r->white2d replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:white bytesPerRow:4];
    r->whitecube = make_texture(MTLTextureTypeCube, f, 1, 1, 1, 1, false);
    for (int k = 0; k < 6; k++)
        [r->whitecube replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 slice:k withBytes:white bytesPerRow:4 bytesPerImage:0];
    r->white3d = make_texture(MTLTextureType3D, f, 1, 1, 1, 1, false);
    [r->white3d replaceRegion:MTLRegionMake3D(0, 0, 0, 1, 1, 1) mipmapLevel:0 slice:0 withBytes:white bytesPerRow:4 bytesPerImage:4];
    r->backbuffer_tex = view_of(d->backbuffer).tex;
    if (d->layer) d->layer.framebufferOnly = NO;
    r->batch = new Batch;
    id<MTLCommandQueue> queue = d->queue;
    std::thread([r, queue] { render_thread(r, queue); }).detach();
}

void r_reset(Device* d) {
    Renderer* r = d->renderer;
    wait_completed(r, submit(r, true));
    r->backbuffer_tex = view_of(d->backbuffer).tex;
    if (d->layer) d->layer.drawableSize = CGSizeMake(d->pp[0], d->pp[1]);
}

void r_clear(Device* d, uint32_t flags, uint32_t color, float z, uint32_t stencil) {
    Renderer* r = d->renderer;
    if (frame_traced(d))
        std::fprintf(stderr, "clear: flags %x color %08x z %g, viewport %u,%u %ux%u\n", flags, color, z, d->viewport[0],
                     d->viewport[1], d->viewport[2], d->viewport[3]);
    @autoreleasepool {
        end_pass(r);
        r->pending_clear = true;
        r->clear_flags = flags;
        r->clear_color = color;
        r->clear_z = z;
        r->clear_stencil = stencil;
        pass(d);  // the clear happens even if nothing is drawn
    }
}

namespace {
// A texture bound for sampling that is also a render target of the pass
// (post-processing that reads the image it writes): Metal gives no defined
// result, so the draw reads a copy taken before it.
void resolve_feedback(Device* d) {
    Renderer* r = d->renderer;
    r->feedback.clear();
    View colors[4], depth;
    current_targets(d, colors, &depth);
    for (int u = 0; u < 16; u++) {
        Texture* tx = as<Texture>(d->textures[u]);
        if (!tx || !tx->tex || !is_render_target(tx->usage)) continue;
        bool hit = false;
        for (const View& v : colors) hit = hit || v.tex == tx->tex;
        if (!hit || r->feedback.count(tx)) continue;
        end_pass(r);
        id<MTLTexture> src = tx->tex;
        MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:src.pixelFormat
                                                                                     width:src.width height:src.height
                                                                                 mipmapped:NO];
        td.usage = MTLTextureUsageShaderRead;
        td.storageMode = MTLStorageModePrivate;
        const Format f = format_of(tx->format);
        if (src.pixelFormat != MTLPixelFormatDepth32Float_Stencil8) td.swizzle = f.swizzle;
        id<MTLTexture> copy = [d->mtl newTextureWithDescriptor:td];
        record_run(r, [src, copy](id<MTLCommandBuffer> cb) {
            id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
            [bl copyFromTexture:src sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                     sourceSize:MTLSizeMake(src.width, src.height, 1) toTexture:copy destinationSlice:0
               destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
            [bl endEncoding];
        });
        r->feedback[tx] = copy;
        static bool said = false;
        if (!said) trace("d3d9: a draw samples its own render target: it reads a copy"), said = true;
    }
}
}  // namespace

void r_draw(Device* d, uint32_t prim, uint32_t start, uint32_t prims) {
    @autoreleasepool {
        resolve_feedback(d);
        Renderer* r = d->renderer;
        if (!pass(d) || !prepare(d, streams_of(d))) return;
        const uint32_t n = vertex_count(prim, prims);
        if (prim == 6) {
            auto idx = fan_indices(prims, [&](uint32_t k) { return start + k; });
            const uint32_t off = upload_bytes(d, idx.data(), (uint32_t)idx.size() * 4);
            record_draw_indexed(r, MTLPrimitiveTypeTriangle, idx.size(), MTLIndexTypeUInt32, r->upload, off, 1, 0);
            return;
        }
        record_draw(r, primitive(prim), start, n, instances(d));
    }
}

void r_draw_indexed(Device* d, uint32_t prim, int32_t base, uint32_t start, uint32_t prims) {
    @autoreleasepool {
        Buffer* ib = as<Buffer>(d->indices);
        if (ib) ib->used = true;
        if (!ib) return;
        resolve_feedback(d);
        Renderer* r = d->renderer;
        if (!pass(d) || !prepare(d, streams_of(d))) return;
        const bool wide = ib->format == FMT_INDEX32;
        const uint32_t isize = wide ? 4 : 2;
        if (prim == 6) {
            const uint8_t* src = gptr(ib->data + start * isize);
            auto idx = fan_indices(prims, [&](uint32_t k) {
                return (wide ? ((const uint32_t*)src)[k] : ((const uint16_t*)src)[k]) + (uint32_t)base;
            });
            const uint32_t off = upload_bytes(d, idx.data(), (uint32_t)idx.size() * 4);
            record_draw_indexed(r, MTLPrimitiveTypeTriangle, idx.size(), MTLIndexTypeUInt32, r->upload, off, 1, 0);
            return;
        }
        record_draw_indexed(r, primitive(prim), vertex_count(prim, prims), wide ? MTLIndexTypeUInt32 : MTLIndexTypeUInt16,
                            ib->mtl, ib->offset + start * isize, instances(d), base);
    }
}

void r_draw_up(Device* d, uint32_t prim, uint32_t prims, uint32_t data, uint32_t stride) {
    @autoreleasepool {
        const uint32_t n = vertex_count(prim, prims);
        resolve_feedback(d);
        Renderer* r = d->renderer;
        if (pass(d)) {
            Streams st;
            st.buffer[0] = r->upload;
            st.offset[0] = upload_bytes(d, gptr(data), n * stride);
            st.stride[0] = stride;
            if (prepare(d, st)) {
                if (prim == 6) {
                    auto idx = fan_indices(prims, [](uint32_t k) { return k; });
                    const uint32_t off = upload_bytes(d, idx.data(), (uint32_t)idx.size() * 4);
                    record_draw_indexed(r, MTLPrimitiveTypeTriangle, idx.size(), MTLIndexTypeUInt32, r->upload, off, 1, 0);
                } else {
                    record_draw(r, primitive(prim), 0, n, 1);
                }
            }
        }
        // D3D9 unsets stream 0 after a UP draw.
        bind(d->streams[0][0], 0);
        d->streams[0][1] = d->streams[0][2] = 0;
    }
}

void r_draw_indexed_up(Device* d, uint32_t prim, uint32_t vertices, uint32_t prims, uint32_t indices,
                       uint32_t index_format, uint32_t data, uint32_t stride) {
    @autoreleasepool {
        resolve_feedback(d);
        Renderer* r = d->renderer;
        if (pass(d)) {
            Streams st;
            st.buffer[0] = r->upload;
            st.offset[0] = upload_bytes(d, gptr(data), vertices * stride);
            st.stride[0] = stride;
            if (prepare(d, st)) {
                const bool wide = index_format == FMT_INDEX32;
                const uint32_t n = vertex_count(prim, prims);
                if (prim == 6) {
                    const uint8_t* src = gptr(indices);
                    auto idx = fan_indices(prims, [&](uint32_t k) {
                        return wide ? ((const uint32_t*)src)[k] : (uint32_t)((const uint16_t*)src)[k];
                    });
                    const uint32_t off = upload_bytes(d, idx.data(), (uint32_t)idx.size() * 4);
                    record_draw_indexed(r, MTLPrimitiveTypeTriangle, idx.size(), MTLIndexTypeUInt32, r->upload, off, 1, 0);
                } else {
                    const uint32_t off = upload_bytes(d, gptr(indices), n * (wide ? 4 : 2));
                    record_draw_indexed(r, primitive(prim), n, wide ? MTLIndexTypeUInt32 : MTLIndexTypeUInt16, r->upload,
                                        off, 1, 0);
                }
            }
        }
        bind(d->streams[0][0], 0);
        d->streams[0][1] = d->streams[0][2] = 0;
        bind(d->indices, 0);
    }
}

void r_present(Device* d) {
    Renderer* r = d->renderer;
    @autoreleasepool {
        end_pass(r);
        id<MTLTexture> bb = r->backbuffer_tex;
        CAMetalLayer* layer = d->layer;
        if (layer && bb) {
            // The drawable is taken on the render thread, when the frame is encoded.
            __block id<CAMetalDrawable> drawable = nil;
            auto next = [layer]() -> id<CAMetalDrawable> { return [layer nextDrawable]; };
            const CGSize ds = layer.drawableSize;
            if ((NSUInteger)ds.width == bb.width && (NSUInteger)ds.height == bb.height &&
                layer.pixelFormat == bb.pixelFormat) {
                record_run(r, [bb, next](id<MTLCommandBuffer> cb) {
                    id<CAMetalDrawable> drawable = next();
                    if (!drawable) return;
                    id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
                    [bl copyFromTexture:bb sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                             sourceSize:MTLSizeMake(bb.width, bb.height, 1) toTexture:drawable.texture destinationSlice:0
                       destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
                    [bl endEncoding];
                    [cb presentDrawable:drawable];
                });
            } else {
                // The back buffer is not the drawable's size (the game picked
                // its own mode): scaled to fit.
                auto slot = std::make_shared<id<CAMetalDrawable>>();
                View v;
                v.tex = bb;
                draw_scaled(d, v, 0, 0, bb.width, bb.height, nil, layer.pixelFormat, 0, 0, 0, 0, 0, 0, true,
                            [slot, next]() -> id<MTLTexture> {
                                *slot = next();
                                return (*slot).texture;
                            });
                record_run(r, [slot](id<MTLCommandBuffer> cb) {
                    if (*slot) [cb presentDrawable:*slot];
                    *slot = nil;
                });
            }
            (void)drawable;
        }
        save_screenshot(d);
        // D3DPRESENT_INTERVAL_ONE waits for the display's refresh. A layer
        // that is hidden or not composited hands out drawables at once, so
        // presents are paced to 60 Hz here as well (Prince of Persia with
        // vertical sync on has no frame limiter of its own: its loading
        // screen ran at 1,400 fps and never finished).
        if (d->pp[13] == 1) {
            using sclock = std::chrono::steady_clock;
            static sclock::time_point next;
            const auto period = std::chrono::microseconds(16667);
            const auto now = sclock::now();
            if (next > now) std::this_thread::sleep_until(next);
            next = std::max(next, now) + period;
        }
        const uint64_t seq = submit(r, true);
        // One frame in flight: the render thread encodes this frame and the
        // GPU runs it while the game builds the next; the frame before must
        // be done before its memory is reused.
        wait_completed(r, r->prev_frame_seq);
        r->prev_frame_seq = seq;
        r->upload = r->uploads[d->frames & 1];
        r->upload_used = 0;
        recycle_retired();
        input_frame(d->frames);
        pad_frame(d->frames);
        if (tracing() && (d->frames < 5 || d->frames % 300 == 0))
            trace("frame %llu: %llu draws skipped (fixed function or no declaration), %zu pipelines",
                  (unsigned long long)d->frames, (unsigned long long)r->skipped, r->pipelines.size());
    }
}

void r_stretch_rect(Device* d, uint32_t src, uint32_t src_rect, uint32_t dst, uint32_t dst_rect, uint32_t filter) {
    Renderer* r = d->renderer;
    @autoreleasepool {
        View a = view_of(src), b = view_of(dst);
        auto size = [](const View& v) {
            return std::make_pair(std::max<NSUInteger>(1, v.tex.width >> v.level),
                                  std::max<NSUInteger>(1, v.tex.height >> v.level));
        };
        if (!a.tex || !b.tex) {
            trace("StretchRect(%08x -> %08x): a side has no texture", src, dst);
            return;
        }
        const auto [aw, ah] = size(a);
        const auto [bw, bh] = size(b);
        NSUInteger sx = 0, sy = 0, sw = aw, sh = ah, dx = 0, dy = 0, dw = bw, dh = bh;
        if (src_rect) sx = rd(src_rect), sy = rd(src_rect + 4), sw = rd(src_rect + 8) - sx, sh = rd(src_rect + 12) - sy;
        if (dst_rect) dx = rd(dst_rect), dy = rd(dst_rect + 4), dw = rd(dst_rect + 8) - dx, dh = rd(dst_rect + 12) - dy;
        if (sx >= aw || sy >= ah || dx >= bw || dy >= bh) return;
        sw = std::min(sw, aw - sx), sh = std::min(sh, ah - sy), dw = std::min(dw, bw - dx), dh = std::min(dh, bh - dy);
        if (!sw || !sh || !dw || !dh) return;
        end_pass(r);
        if (frame_traced(d))
            std::fprintf(stderr, "stretch: %lux%lu fmt %lu (%lu,%lu %lux%lu) -> %lux%lu fmt %lu (%lu,%lu %lux%lu)%s\n",
                         (unsigned long)aw, (unsigned long)ah, (unsigned long)a.tex.pixelFormat, (unsigned long)sx,
                         (unsigned long)sy, (unsigned long)sw, (unsigned long)sh, (unsigned long)bw, (unsigned long)bh,
                         (unsigned long)b.tex.pixelFormat, (unsigned long)dx, (unsigned long)dy, (unsigned long)dw,
                         (unsigned long)dh, dst == d->backbuffer ? " (back buffer)" : "");
        // The same size and format: a copy, exact (depth and stencil too).
        if (sw == dw && sh == dh && a.tex.pixelFormat == b.tex.pixelFormat && !(a == b)) {
            record_run(r, [a, b, sx, sy, sw, sh, dx, dy](id<MTLCommandBuffer> cb) {
                id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
                [bl copyFromTexture:a.tex sourceSlice:a.slice sourceLevel:a.level sourceOrigin:MTLOriginMake(sx, sy, 0)
                         sourceSize:MTLSizeMake(sw, sh, 1) toTexture:b.tex destinationSlice:b.slice
                   destinationLevel:b.level destinationOrigin:MTLOriginMake(dx, dy, 0)];
                [bl endEncoding];
            });
            return;
        }
        // Scaled or converted: drawn (D3DTEXF_LINEAR and above filter).
        if (a.tex == b.tex && a.level == b.level && a.slice == b.slice) return;
        if (!draw_scaled(d, a, sx, sy, sw, sh, b.tex, b.tex.pixelFormat, b.level, b.slice, dx, dy, dw, dh, filter >= 2)) {
            static int said = 0;
            if (said++ < 8)
                trace("StretchRect: %lu -> %lu (%lux%lu -> %lux%lu) not done", (unsigned long)a.tex.pixelFormat,
                      (unsigned long)b.tex.pixelFormat, (unsigned long)sw, (unsigned long)sh, (unsigned long)dw,
                      (unsigned long)dh);
        }
    }
}

void r_read_back(Device* d, uint32_t src, uint32_t dst) {
    Renderer* r = d->renderer;
    @autoreleasepool {
        View a = view_of(src);
        Surface* b = as<Surface>(dst);
        if (!a.tex || !b) return;
        end_pass(r);
        const NSUInteger w = std::min<NSUInteger>(b->width, a.tex.width), h = std::min<NSUInteger>(b->height, a.tex.height);
        id<MTLBuffer> out = [d->mtl newBufferWithLength:b->pitch * h options:MTLResourceStorageModeShared];
        const uint32_t pitch = b->pitch;
        record_run(r, [a, out, w, h, pitch](id<MTLCommandBuffer> cb) {
            id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
            [bl copyFromTexture:a.tex sourceSlice:a.slice sourceLevel:a.level sourceOrigin:MTLOriginMake(0, 0, 0)
                     sourceSize:MTLSizeMake(w, h, 1) toBuffer:out destinationOffset:0 destinationBytesPerRow:pitch
              destinationBytesPerImage:pitch * h];
            [bl endEncoding];
        });
        // Everything recorded so far, this copy last, done on the GPU.
        wait_completed(r, submit(r, true));
        std::memcpy(gptr(b->data), out.contents, b->pitch * h);
    }
}

}  // namespace scimitar::win32::d3d9
