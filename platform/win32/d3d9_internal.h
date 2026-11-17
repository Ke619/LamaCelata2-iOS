#pragma once
// Direct3D 9's objects as d3d9.mm (the API) and d3d9_metal.mm (rendering on
// Metal) share them. Objective-C++ only.
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "com.h"
#include "win32.h"

namespace scimitar::win32::d3d9 {

using namespace scimitar::win32::com;

constexpr uint32_t D3D_OK = 0, D3DERR_NOTAVAILABLE = 0x8876086a, D3DERR_INVALIDCALL = 0x8876086c,
                   D3DERR_NOTFOUND = 0x88760866;
constexpr uint32_t FMT_A8R8G8B8 = 21, FMT_X8R8G8B8 = 22, FMT_R5G6B5 = 23, FMT_D24S8 = 75, FMT_D16 = 80,
                   FMT_D24X8 = 77, FMT_INDEX16 = 101, FMT_INDEX32 = 102;
// NULL: a render target with no memory (depth-only passes), the vendors' trick.
constexpr uint32_t FMT_NULL = 0x4c4c554e;
constexpr uint32_t FMT_DXT1 = 0x31545844, FMT_DXT2 = 0x32545844, FMT_DXT3 = 0x33545844, FMT_DXT4 = 0x34545844,
                   FMT_DXT5 = 0x35545844;

// Bytes per row of `w` texels (block-compressed formats: per row of blocks),
// and rows of blocks for `h` texel rows.
uint32_t format_pitch(uint32_t fmt, uint32_t w);
uint32_t format_rows(uint32_t fmt, uint32_t h);
bool is_depth_format(uint32_t fmt);

struct Surface : Native {
    static constexpr uint32_t kKind = 0xd3d90000u + 1;
    Surface() { kind = kKind; }
    uint32_t width = 0, height = 0, format = 0, usage = 0, pool = 0;
    uint32_t pitch = 0, data = 0;  // guest memory holding the texels (what LockRect hands out)
    uint32_t container = 0;        // the texture it is a level of, or 0
    uint32_t level = 0, face = 0;
    id<MTLTexture> tex = nil;      // standalone surfaces (render targets, depth): their own texture
    bool dirty = false;            // guest data newer than the texture
};

struct Texture : Native {
    static constexpr uint32_t kKind = 0xd3d90000u + 2;
    Texture() { kind = kKind; }
    uint32_t width = 0, height = 0, depth = 1, levels = 1, format = 0, usage = 0, pool = 0;
    bool cube = false, volume = false;
    std::vector<uint32_t> surfaces;  // guest Surface objects: level-major, faces within (cube)
    uint32_t lod = 0;
    id<MTLTexture> tex = nil;
    bool dirty = true;               // some level's guest data is newer than the texture
    bool used = false;               // handed to a draw since `tex` was made
    ~Texture() override {
        for (uint32_t s : surfaces) destroy(s);  // its levels go with it
    }
};

struct Buffer : Native {
    static constexpr uint32_t kKind = 0xd3d90000u + 3;
    Buffer() { kind = kKind; }
    uint32_t length = 0, usage = 0, format = 0, fvf = 0, pool = 0;
    uint32_t data = 0;               // guest address of the contents (in a Metal-visible block)
    id<MTLBuffer> mtl = nil;         // the block holding `data`
    uint32_t offset = 0;             // `data` inside it
    bool index = false;
    bool used = false;               // bound by a draw since its memory last changed
};

struct Shader : Native {
    static constexpr uint32_t kKind = 0xd3d90000u + 4;
    Shader() { kind = kKind; }
    bool vertex = false;
    std::vector<uint32_t> tokens;
    void* compiled = nullptr;        // the renderer's translation (d3d9_metal.mm)
};

struct Declaration : Native {
    static constexpr uint32_t kKind = 0xd3d90000u + 5;
    Declaration() { kind = kKind; }
    std::vector<uint8_t> elements;  // D3DVERTEXELEMENT9 array, 8 bytes each, with its end marker
};

struct Query : Native {
    static constexpr uint32_t kKind = 0xd3d90000u + 6;
    Query() { kind = kKind; }
    uint32_t type = 0;
};

// A Set* call recorded between BeginStateBlock and EndStateBlock: the setter,
// its arguments after `this`, and a copy of what its pointer argument (if
// any) points to.
struct Recorded {
    GuestFn fn = nullptr;
    std::vector<uint32_t> args;
    int ptr = -1;               // which argument is a pointer (1-based, as arg())
    std::vector<uint8_t> blob;  // the data it points to
    uint32_t guest = 0;         // that data in guest memory, made on first Apply
};

struct StateBlock : Native {
    static constexpr uint32_t kKind = 0xd3d90000u + 7;
    StateBlock() { kind = kKind; }
    std::vector<Recorded> calls;  // recorded blocks: replayed by Apply
};

struct Renderer;

struct Device : Native {
    static constexpr uint32_t kKind = 0xd3d90000u + 8;
    Device() { kind = kKind; }
    StateBlock* recording = nullptr;  // between BeginStateBlock and EndStateBlock
    uint32_t hwnd = 0, behavior = 0;
    uint32_t pp[14] = {};  // D3DPRESENT_PARAMETERS
    uint32_t d3d = 0;      // the IDirect3D9 that made it
    uint32_t backbuffer = 0, depth = 0;
    uint32_t render_targets[4] = {}, depth_stencil = 0;
    uint32_t render_state[256] = {};
    uint32_t sampler_state[20][14] = {};
    uint32_t stage_state[8][33] = {};
    uint32_t textures[20] = {};
    uint32_t streams[16][3] = {};  // buffer, offset, stride
    uint32_t stream_freq[16] = {};
    uint32_t indices = 0, decl = 0, fvf = 0, vs = 0, ps = 0;
    std::vector<float> vs_const = std::vector<float>(256 * 4), ps_const = std::vector<float>(224 * 4);
    int32_t vs_int[16 * 4] = {}, ps_int[16 * 4] = {};
    uint32_t vs_bool[16] = {}, ps_bool[16] = {};
    uint32_t viewport[6] = {};  // X, Y, Width, Height, MinZ, MaxZ
    uint32_t scissor[4] = {};
    float transforms[512][16] = {};
    uint64_t frames = 0, draws = 0;
    CAMetalLayer* layer = nil;
    id<MTLDevice> mtl = nil;
    id<MTLCommandQueue> queue = nil;
    Renderer* renderer = nullptr;
};

// References as Direct3D 9 counts them: a texture's level surfaces count
// their texture. The device holds one on everything bound to it (render
// targets, depth, textures, streams, indices, shaders, declaration), so a game
// may release its own while the object stays bound.
void add_ref(uint32_t obj);
void release(uint32_t obj);
void bind(uint32_t& slot, uint32_t obj);

// Rendering (d3d9_metal.mm).
bool frame_traced(const Device* d);  // SCIMITAR_D3D_TRACE_FRAME(S)
void r_init(Device* d);
void r_reset(Device* d);
void r_clear(Device* d, uint32_t flags, uint32_t color, float z, uint32_t stencil);
void r_draw(Device* d, uint32_t prim, uint32_t start_vertex, uint32_t prims);
void r_draw_indexed(Device* d, uint32_t prim, int32_t base_vertex, uint32_t start_index, uint32_t prims);
void r_draw_up(Device* d, uint32_t prim, uint32_t prims, uint32_t data, uint32_t stride);
void r_draw_indexed_up(Device* d, uint32_t prim, uint32_t vertices, uint32_t prims, uint32_t indices,
                       uint32_t index_format, uint32_t data, uint32_t stride);
void r_present(Device* d);
void r_stretch_rect(Device* d, uint32_t src, uint32_t src_rect, uint32_t dst, uint32_t dst_rect, uint32_t filter);
void r_read_back(Device* d, uint32_t src, uint32_t dst);  // GetRenderTargetData
// Guest memory Metal can read in place, for buffers: 16-byte aligned.
uint32_t r_buffer_memory(uint32_t length, __strong id<MTLBuffer>* buffer, uint32_t* offset);
void r_buffer_release(id<MTLBuffer> buffer, uint32_t offset, uint32_t length);

}  // namespace scimitar::win32::d3d9
