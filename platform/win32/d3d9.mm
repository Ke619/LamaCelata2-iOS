// Direct3D 9 (d3d9!Direct3DCreate9) for the recompiled game, on Metal.
//
// IDirect3D9 describes one adapter (an NVIDIA GeForce 8800-class card with
// shader model 3, the game's target) with this Mac's display modes.
// IDirect3DDevice9 keeps the device state the game sets and owns its
// resources: textures, surfaces, vertex and index buffers live in guest
// memory, so locks hand the game pointers into them.
//
// Status: device, resources, state and presentation (Clear and Present show
// in the window's CAMetalLayer); draws are counted, not yet rendered (the
// shaders' D3D9 bytecode -> MSL path is the next piece, docs/AC1_WINDOWS.md).
#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <vector>

#include "d3d9_internal.h"
#include "win32_window.h"

namespace scimitar::win32::d3d9 {

uint32_t format_pitch(uint32_t fmt, uint32_t w) {
    switch (fmt) {
    case FMT_DXT1: return std::max(1u, (w + 3) / 4) * 8;
    case FMT_DXT2: case FMT_DXT3: case FMT_DXT4: case FMT_DXT5: return std::max(1u, (w + 3) / 4) * 16;
    case 28: case 50: return w;                             // A8, L8
    case 23: case 25: case 26: case 51: case 60: case 80: case 111: return 2 * w;
    case 113: case 115: return 8 * w;                       // A16B16G16R16F, Q16W16V16U16
    case 116: return 16 * w;                                // A32B32G32R32F
    default: return 4 * w;
    }
}
uint32_t format_rows(uint32_t fmt, uint32_t h) {
    const bool block = fmt == FMT_DXT1 || fmt == FMT_DXT2 || fmt == FMT_DXT3 || fmt == FMT_DXT4 || fmt == FMT_DXT5;
    return block ? std::max(1u, (h + 3) / 4) : h;
}
bool is_depth_format(uint32_t fmt) {
    return fmt == 70 || fmt == 71 || fmt == 73 || fmt == 75 || fmt == 77 || fmt == 79 || fmt == 80 || fmt == 82 ||
           fmt == 0x5a544e49 /* INTZ */;
}

uint32_t g_vt_surface, g_vt_texture, g_vt_cube, g_vt_volume, g_vt_vb, g_vt_ib, g_vt_vs, g_vt_ps, g_vt_decl,
    g_vt_query, g_vt_stateblock, g_vt_swapchain;

uint32_t new_surface(uint32_t w, uint32_t h, uint32_t fmt, uint32_t usage, uint32_t pool, uint32_t container,
                     uint32_t level) {
    auto* s = new Surface;
    s->width = std::max(1u, w), s->height = std::max(1u, h), s->format = fmt, s->usage = usage, s->pool = pool;
    s->pitch = format_pitch(fmt, s->width);
    s->data = galloc(s->pitch * format_rows(fmt, s->height) + 16, 16);
    s->container = container, s->level = level;
    const uint32_t obj = create(g_vt_surface, s);
    if (container) s->refs = 0;  // owned by the texture
    return obj;
}

// Reset keeps the device's own surfaces (the back buffer, the automatic depth
// surface) as the same objects, as Direct3D 9 does: games keep the pointers
// GetBackBuffer/GetRenderTarget gave them before. Their size and format change.
uint32_t reset_surface(uint32_t obj, uint32_t w, uint32_t h, uint32_t fmt, uint32_t usage) {
    Surface* s = as<Surface>(obj);
    if (!s) return new_surface(w, h, fmt, usage, 0, 0, 0);
    w = std::max(1u, w), h = std::max(1u, h);
    if (s->width != w || s->height != h || s->format != fmt) {
        gfree(s->data);
        s->width = w, s->height = h, s->format = fmt;
        s->pitch = format_pitch(fmt, w);
        s->data = galloc(s->pitch * format_rows(fmt, h) + 16, 16);
        s->tex = nil;
    }
    return obj;
}

// ---- the device ----

uint32_t g_vt_device = 0;
uint32_t g_device_object = 0;

uint32_t owner_of(uint32_t obj) {
    const Surface* s = as<Surface>(obj);
    return s && s->container ? s->container : obj;
}
void add_ref(uint32_t obj) {
    if (Native* n = native(owner_of(obj))) n->refs++;
}
void release(uint32_t obj) {
    const uint32_t o = owner_of(obj);
    if (Native* n = native(o))
        if (!n->refs || !--n->refs) destroy(o);
}
void bind(uint32_t& slot, uint32_t obj) {
    if (slot == obj) return;
    add_ref(obj);
    const uint32_t old = slot;
    slot = obj;
    release(old);
}

// SCIMITAR_D3D_TRACE_FRAME=N: the frame whose calls are logged
// (SCIMITAR_D3D_TRACE_FRAMES=K: K frames from N on).
bool frame_traced(const Device* d) {
    static const uint64_t n =
        std::getenv("SCIMITAR_D3D_TRACE_FRAME") ? std::strtoull(std::getenv("SCIMITAR_D3D_TRACE_FRAME"), nullptr, 10) : ~0ull;
    static const uint64_t k =
        std::getenv("SCIMITAR_D3D_TRACE_FRAMES") ? std::strtoull(std::getenv("SCIMITAR_D3D_TRACE_FRAMES"), nullptr, 10) : 1;
    return d->frames >= n && d->frames - n < k;
}

Device* dev(Context& c) {
    Device* d = as<Device>(arg(c, 0));
    if (!d) {
        const com::Native* n = com::native(arg(c, 0));
        fatal("IDirect3DDevice9 method on %08x, which is no device (%s)", arg(c, 0), n ? typeid(*n).name() : "not a COM object");
    }
    return d;
}

float f32(uint32_t bits) {
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

// ---- IDirect3D9 ----

struct D3D : Native {};

// The adapter's modes, smallest first: common ones up to the main display's
// size in pixels, the desktop's size in points (GetAdapterDisplayMode's), and
// on a Retina display its pixels, the panel's own resolution (the last).
const std::vector<std::pair<uint32_t, uint32_t>>& modes() {
    static const std::vector<std::pair<uint32_t, uint32_t>> list = [] {
        const CGDirectDisplayID d = CGMainDisplayID();
        const std::pair<uint32_t, uint32_t> points{(uint32_t)CGDisplayPixelsWide(d), (uint32_t)CGDisplayPixelsHigh(d)};
        std::pair<uint32_t, uint32_t> pixels = points;
        if (CGDisplayModeRef m = CGDisplayCopyDisplayMode(d)) {
            const uint32_t w = (uint32_t)CGDisplayModeGetPixelWidth(m), h = (uint32_t)CGDisplayModeGetPixelHeight(m);
            if (w > pixels.first && h > pixels.second) pixels = {w, h};
            CGDisplayModeRelease(m);
        }
        if (!pixels.first || !pixels.second) pixels = {1920, 1080};
        static const uint32_t kCommon[][2] = {{800, 600},   {1024, 768},  {1152, 864},  {1280, 720},
                                              {1280, 800},  {1280, 1024}, {1440, 900},  {1600, 900},
                                              {1680, 1050}, {1920, 1080}, {1920, 1200}, {2560, 1440},
                                              {2560, 1600}, {3840, 2160}};
        std::vector<std::pair<uint32_t, uint32_t>> out;
        for (const auto& m : kCommon)
            if (m[0] <= pixels.first && m[1] <= pixels.second) out.emplace_back(m[0], m[1]);
        for (const auto& m : {points, pixels})
            if (m.first && m.second && std::find(out.begin(), out.end(), m) == out.end()) out.push_back(m);
        std::stable_sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
            return (uint64_t)a.first * a.second < (uint64_t)b.first * b.second;
        });
        return out;
    }();
    return list;
}

void d3d_GetAdapterCount(Context& c) { ret(c, 1, 1); }

void d3d_GetAdapterIdentifier(Context& c) {
    const uint32_t id = arg(c, 3);
    std::memset(gptr(id), 0, 1100);
    put_str(id, 512, "nv4_disp.dll");
    put_str(id + 512, 512, "NVIDIA GeForce 8800 GTX");
    put_str(id + 1024, 32, "\\\\.\\DISPLAY1");
    wr(id + 1056, 0x000f0b40), wr(id + 1060, 0x00060010);  // driver 6.14.11.6976
    wr(id + 1064, 0x10de), wr(id + 1068, 0x0191), wr(id + 1072, 0), wr(id + 1076, 0xa2);
    wr(id + 1080, 0xd7b71e3e), wr(id + 1084, 0x41d24251), wr(id + 1088, 0x6b0f0e9e), wr(id + 1092, 0x35c4c21a);
    wr(id + 1096, 1);
    ret(c, D3D_OK, 4);
}

void d3d_GetAdapterModeCount(Context& c) { ret(c, arg(c, 2) == FMT_X8R8G8B8 ? (uint32_t)modes().size() : 0, 3); }

void d3d_EnumAdapterModes(Context& c) {
    const uint32_t fmt = arg(c, 2), k = arg(c, 3), out = arg(c, 4);
    if (fmt != FMT_X8R8G8B8 || k >= modes().size()) return ret(c, D3DERR_INVALIDCALL, 5);
    wr(out, modes()[k].first), wr(out + 4, modes()[k].second), wr(out + 8, 60), wr(out + 12, FMT_X8R8G8B8);
    ret(c, D3D_OK, 5);
}

void d3d_GetAdapterDisplayMode(Context& c) {
    const uint32_t out = arg(c, 2);
    int w = 1920, h = 1080;
    @autoreleasepool {
        if (NSScreen* s = NSScreen.mainScreen) w = (int)s.frame.size.width, h = (int)s.frame.size.height;
    }
    wr(out, (uint32_t)w), wr(out + 4, (uint32_t)h), wr(out + 8, 60), wr(out + 12, FMT_X8R8G8B8);
    ret(c, D3D_OK, 3);
}

void d3d_CheckDeviceType(Context& c) { ret(c, D3D_OK, 6); }

void d3d_CheckDeviceFormat(Context& c) {
    // (adapter, type, adapterFmt, usage, rtype, fmt): the common formats and
    // NULL render targets (games that see an NVIDIA adapter count on them for
    // depth-only passes); not the other vendor tricks (INTZ/RAWZ depth
    // textures, ATI's formats), so the game takes its standard paths.
    const uint32_t fmt = arg(c, 6);
    if (fmt == FMT_NULL && (arg(c, 4) & 1)) return ret(c, D3D_OK, 7);  // D3DUSAGE_RENDERTARGET
    static const uint32_t kOk[] = {21, 22, 23, 25, 26, 28, 50, 51, 60, 63, 34, 35, 31, 75, 77, 80, 71,
                                   111, 112, 113, 114, 115, 116, 0x31545844, 0x33545844, 0x35545844};
    for (uint32_t f : kOk)
        if (f == fmt) return ret(c, D3D_OK, 7);
    trace("CheckDeviceFormat(%08x, usage %08x): not available", fmt, arg(c, 4));
    ret(c, D3DERR_NOTAVAILABLE, 7);
}

void d3d_CheckDeviceMultiSampleType(Context& c) {
    const uint32_t type = arg(c, 5), quality = arg(c, 6);
    if (quality) wr(quality, 1);
    ret(c, type <= 1 || type == 2 || type == 4 ? D3D_OK : D3DERR_NOTAVAILABLE, 7);
}

void d3d_CheckDepthStencilMatch(Context& c) { ret(c, D3D_OK, 6); }
void d3d_CheckDeviceFormatConversion(Context& c) { ret(c, D3D_OK, 5); }

void fill_caps(uint32_t p) {
    std::memset(gptr(p), 0, 304);
    const uint32_t v[] = {
        1, 0, 0x20000, 0xe0020000, 0x3a0, 0x8000000f, 1, 0x1bbef0, 0x2fcef2, 0x7732191, 0xff, 0x3fff, 0x3fff,
        0xff, 0x84208, 0x1ec05, 0x3030700, 0x3030700, 0x3030700, 0x3f, 0x3f, 0x1f, 8192, 8192, 2048, 8192,
        8192, 16};
    for (size_t k = 0; k < sizeof v / sizeof *v; k++) wr(p + 4 * (uint32_t)k, v[k]);
    auto f = [&](uint32_t off, float x) {
        uint32_t b;
        std::memcpy(&b, &x, 4);
        wr(p + off, b);
    };
    f(112, 1e10f), f(116, -1e8f), f(120, -1e8f), f(124, 1e8f), f(128, 1e8f), f(132, 0);
    wr(p + 136, 0x1ff), wr(p + 140, 0x180008), wr(p + 144, 0x3feffff), wr(p + 148, 8), wr(p + 152, 8);
    wr(p + 156, 0x17b), wr(p + 160, 8), wr(p + 164, 8), wr(p + 168, 4), wr(p + 172, 8);
    f(176, 8192), wr(p + 180, 0xffffff), wr(p + 184, 0xffffff), wr(p + 188, 16), wr(p + 192, 255);
    wr(p + 196, 0xfffe0300), wr(p + 200, 256), wr(p + 204, 0xffff0300), f(208, 65504);
    wr(p + 212, 0x351), wr(p + 232, 1), wr(p + 236, 0x30f), wr(p + 240, 4), wr(p + 244, 0x3000300);
    wr(p + 248, 1), wr(p + 252, 24), wr(p + 256, 32), wr(p + 260, 4);
    wr(p + 264, 0x1f), wr(p + 268, 24), wr(p + 272, 32), wr(p + 276, 4), wr(p + 280, 512);
    wr(p + 284, 0x3000300), wr(p + 288, 0xffff), wr(p + 292, 0xffff), wr(p + 296, 4096), wr(p + 300, 4096);
}

void d3d_GetDeviceCaps(Context& c) {
    fill_caps(arg(c, 3));
    ret(c, D3D_OK, 4);
}

void d3d_GetAdapterMonitor(Context& c) { ret(c, 0x10030, 2); }

void create_device_vtables();
void set_default_states(Device* d);

// The states a new (or reset) device starts with.
void set_default_states(Device* d) {
    uint32_t* rs = d->render_state;
    std::memset(rs, 0, sizeof d->render_state);
    rs[7] = d->pp[9] ? 1 : 0;  // ZENABLE: with an automatic depth buffer
    rs[8] = 3;                 // FILLMODE solid
    rs[9] = 2;                 // SHADEMODE gouraud
    rs[14] = 1;                // ZWRITEENABLE
    rs[16] = 1;                // LASTPIXEL
    rs[19] = 2, rs[20] = 1;    // SRCBLEND one, DESTBLEND zero
    rs[22] = 3;                // CULLMODE ccw
    rs[23] = 4;                // ZFUNC lessequal
    rs[25] = 8;                // ALPHAFUNC always
    rs[56] = 8;                // STENCILFUNC always
    rs[53] = rs[54] = rs[55] = 1;
    rs[58] = rs[59] = 0xffffffffu;
    rs[168] = rs[190] = rs[191] = rs[192] = 0xf;  // COLORWRITEENABLE(1-3)
    rs[171] = 1;               // BLENDOP add
    rs[186] = rs[187] = rs[188] = 1;  // CCW stencil ops keep
    rs[189] = 8;               // CCW_STENCILFUNC always
    rs[193] = 0xffffffffu;     // BLENDFACTOR
    rs[207] = 2, rs[208] = 1, rs[209] = 1;  // SRCBLENDALPHA one, DESTBLENDALPHA zero, BLENDOPALPHA add
    for (auto& ss : d->sampler_state) {
        std::memset(ss, 0, sizeof ss);
        ss[1] = ss[2] = ss[3] = 1;  // ADDRESSU/V/W wrap
        ss[5] = ss[6] = 1;          // MAG/MINFILTER point
        ss[7] = 0;                  // MIPFILTER none
        ss[10] = 1;                 // MAXANISOTROPY
    }
}

void d3d_CreateDevice(Context& c) {
    const uint32_t hwnd = arg(c, 3), behavior = arg(c, 4), pp = arg(c, 5), out = arg(c, 6);
    create_device_vtables();
    auto* d = new Device;
    d->hwnd = hwnd ? hwnd : rd(pp + 28);
    d->behavior = behavior;
    d->d3d = arg(c, 0);
    int w, h;
    window_size(d->hwnd, &w, &h);
    if (!rd(pp)) wr(pp, (uint32_t)w);
    if (!rd(pp + 4)) wr(pp + 4, (uint32_t)h);
    if (!rd(pp + 8)) wr(pp + 8, FMT_X8R8G8B8);
    for (int k = 0; k < 14; k++) d->pp[k] = rd(pp + 4 * k);
    trace("CreateDevice(hwnd %x, behavior %08x): %ux%u fmt %u, windowed %u, depth %u fmt %u, interval %08x",
          d->hwnd, behavior, d->pp[0], d->pp[1], d->pp[2], d->pp[8], d->pp[9], d->pp[10], d->pp[13]);
    // Direct3D drops the x87 to single precision unless asked not to.
    if (!(behavior & 2)) c.fpu.cw &= (uint16_t)~0x0300u;  // D3DCREATE_FPU_PRESERVE
    d->backbuffer = new_surface(d->pp[0], d->pp[1], d->pp[2], 1, 0, 0, 0);  // its reference: the swap chain's
    bind(d->render_targets[0], d->backbuffer);
    if (d->pp[9]) d->depth = new_surface(d->pp[0], d->pp[1], d->pp[10], 2, 0, 0, 0), bind(d->depth_stencil, d->depth);
    d->viewport[2] = d->pp[0], d->viewport[3] = d->pp[1];
    float one = 1.0f;
    std::memcpy(&d->viewport[5], &one, 4);
    d->scissor[2] = d->pp[0], d->scissor[3] = d->pp[1];
    @autoreleasepool {
        d->mtl = MTLCreateSystemDefaultDevice();
        d->queue = [d->mtl newCommandQueue];
        NSView* view = window_view(d->hwnd);
        if (view && [view.layer isKindOfClass:CAMetalLayer.class]) {
            d->layer = (CAMetalLayer*)view.layer;
            d->layer.device = d->mtl;
            d->layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
            d->layer.drawableSize = CGSizeMake(d->pp[0], d->pp[1]);
        }
    }
    set_default_states(d);
    r_init(d);
    const uint32_t obj = create(g_vt_device, d, 64);
    g_device_object = obj;
    wr(out, obj);
    // Full screen: Windows sizes the focus window to the back buffer (the game
    // may size its own targets from the window).
    if (!d->pp[8]) window_resized(c, d->hwnd, (int)d->pp[0], (int)d->pp[1]);
    ret(c, D3D_OK, 7);
}

}  // namespace scimitar::win32::d3d9

namespace scimitar::win32::d3d9 {

// ---- IDirect3DDevice9 methods ----

void dv_TestCooperativeLevel(Context& c) { ret(c, D3D_OK, 1); }
void dv_GetAvailableTextureMem(Context& c) { ret(c, 768u << 20, 1); }
void dv_EvictManagedResources(Context& c) { ret(c, D3D_OK, 1); }
void dv_GetDirect3D(Context& c) {
    add_ref(dev(c)->d3d);
    wr(arg(c, 1), dev(c)->d3d);
    ret(c, D3D_OK, 2);
}
void dv_GetDeviceCaps(Context& c) {
    fill_caps(arg(c, 1));
    ret(c, D3D_OK, 2);
}
void dv_GetDisplayMode(Context& c) {
    Device* d = dev(c);
    const uint32_t out = arg(c, 2);
    wr(out, d->pp[0]), wr(out + 4, d->pp[1]), wr(out + 8, 60), wr(out + 12, FMT_X8R8G8B8);
    ret(c, D3D_OK, 3);
}
void dv_GetCreationParameters(Context& c) {
    Device* d = dev(c);
    const uint32_t out = arg(c, 1);
    wr(out, 0), wr(out + 4, 1), wr(out + 8, d->hwnd), wr(out + 12, d->behavior);
    ret(c, D3D_OK, 2);
}
void dv_SetCursorProperties(Context& c) { ret(c, D3D_OK, 4); }
void dv_SetCursorPosition(Context& c) { ret(c, 0, 4); }
void dv_ShowCursor(Context& c) { ret(c, 0, 2); }
void dv_GetNumberOfSwapChains(Context& c) { ret(c, 1, 1); }

void dv_Reset(Context& c) {
    Device* d = dev(c);
    const uint32_t pp = arg(c, 1);
    int w, h;
    window_size(d->hwnd, &w, &h);
    if (!rd(pp)) wr(pp, (uint32_t)w);
    if (!rd(pp + 4)) wr(pp + 4, (uint32_t)h);
    if (!rd(pp + 8)) wr(pp + 8, FMT_X8R8G8B8);
    for (int k = 0; k < 14; k++) d->pp[k] = rd(pp + 4 * k);
    trace("Reset: %ux%u, back buffer %08x", d->pp[0], d->pp[1], d->backbuffer);
    d->backbuffer = reset_surface(d->backbuffer, d->pp[0], d->pp[1], d->pp[2], 1);
    bind(d->render_targets[0], d->backbuffer);
    for (int k = 1; k < 4; k++) bind(d->render_targets[k], 0);
    if (d->pp[9]) d->depth = reset_surface(d->depth, d->pp[0], d->pp[1], d->pp[10], 2);
    bind(d->depth_stencil, d->pp[9] ? d->depth : 0);
    d->viewport[0] = d->viewport[1] = 0, d->viewport[2] = d->pp[0], d->viewport[3] = d->pp[1];
    d->scissor[0] = d->scissor[1] = 0, d->scissor[2] = d->pp[0], d->scissor[3] = d->pp[1];
    set_default_states(d);
    r_reset(d);
    if (!d->pp[8]) window_resized(c, d->hwnd, (int)d->pp[0], (int)d->pp[1]);  // as in CreateDevice
    ret(c, D3D_OK, 2);
}

void dv_Present(Context& c) {
    Device* d = dev(c);
    if (frame_traced(d))
        std::fprintf(stderr, "Present(%08x, %08x, hwnd %x, %08x) from %08x\n", arg(c, 1), arg(c, 2), arg(c, 3),
                     arg(c, 4), rd(c.esp));
    d->frames++;
    if (tracing() && (d->frames < 5 || d->frames % 300 == 0))
        trace("Present: frame %llu, %llu draws so far", (unsigned long long)d->frames, (unsigned long long)d->draws);
    r_present(d);
    ret(c, D3D_OK, 5);
}

void dv_GetBackBuffer(Context& c) {
    Device* d = dev(c);
    add_ref(d->backbuffer);
    wr(arg(c, 4), d->backbuffer);
    trace("GetBackBuffer: %08x", d->backbuffer);
    ret(c, D3D_OK, 5);
}
void dv_GetRasterStatus(Context& c) {
    wr(arg(c, 2), 1), wr(arg(c, 2) + 4, 0);
    ret(c, D3D_OK, 3);
}
void dv_SetDialogBoxMode(Context& c) { ret(c, D3D_OK, 2); }
void dv_SetGammaRamp(Context& c) { ret(c, 0, 4); }
void dv_GetGammaRamp(Context& c) {
    const uint32_t r = arg(c, 2);
    for (uint32_t k = 0; k < 256; k++)
        for (uint32_t ch = 0; ch < 3; ch++) wr16(r + 512 * ch + 2 * k, (uint16_t)(k * 257));
    ret(c, 0, 3);
}

uint32_t make_texture(uint32_t vt, uint32_t w, uint32_t h, uint32_t depth, uint32_t levels, uint32_t usage,
                      uint32_t fmt, uint32_t pool, bool cube, bool volume) {
    auto* t = new Texture;
    trace("CreateTexture %ux%ux%u levels %u usage %x format %x pool %u%s", w, h, depth, levels, usage, fmt, pool,
          cube ? " cube" : volume ? " volume" : "");
    t->width = w, t->height = h, t->depth = depth, t->usage = usage, t->format = fmt, t->pool = pool;
    t->cube = cube, t->volume = volume;
    uint32_t full = 1;
    for (uint32_t m = std::max({w, h, depth}); m > 1; m >>= 1) full++;
    t->levels = levels == 0 || levels > full ? full : levels;
    if (usage & 0x400) t->levels = full;  // D3DUSAGE_AUTOGENMIPMAP: the API shows one level, keep the chain
    const uint32_t obj = create(vt, t);
    trace("  texture %08x", obj);
    const uint32_t faces = cube ? 6 : 1;
    for (uint32_t l = 0; l < t->levels; l++)
        for (uint32_t f = 0; f < faces; f++) {
            const uint32_t lw = std::max(1u, w >> l), lh = std::max(1u, h >> l), ld = std::max(1u, depth >> l);
            const uint32_t sf = new_surface(lw, volume ? lh * ld : lh, fmt, usage, pool, obj, l);
            as<Surface>(sf)->face = f;
            t->surfaces.push_back(sf);
        }
    return obj;
}

void dv_CreateTexture(Context& c) {
    // (w, h, levels, usage, fmt, pool, out, shared)
    const uint32_t obj = make_texture(g_vt_texture, arg(c, 1), arg(c, 2), 1, arg(c, 3), arg(c, 4), arg(c, 5),
                                      arg(c, 6), false, false);
    wr(arg(c, 7), obj);
    ret(c, D3D_OK, 9);
}

void dv_CreateVolumeTexture(Context& c) {
    // (w, h, d, levels, usage, fmt, pool, out, shared)
    const uint32_t obj = make_texture(g_vt_volume, arg(c, 1), arg(c, 2), arg(c, 3), arg(c, 4), arg(c, 5),
                                      arg(c, 6), arg(c, 7), false, true);
    wr(arg(c, 8), obj);
    ret(c, D3D_OK, 10);
}

void dv_CreateCubeTexture(Context& c) {
    // (edge, levels, usage, fmt, pool, out, shared)
    const uint32_t obj = make_texture(g_vt_cube, arg(c, 1), arg(c, 1), 1, arg(c, 2), arg(c, 3), arg(c, 4),
                                      arg(c, 5), true, false);
    wr(arg(c, 6), obj);
    ret(c, D3D_OK, 8);
}

uint32_t make_buffer(bool index, uint32_t length, uint32_t usage, uint32_t fmt_or_fvf, uint32_t pool) {
    auto* b = new Buffer;
    b->index = index, b->length = length, b->usage = usage, b->pool = pool;
    (index ? b->format : b->fvf) = fmt_or_fvf;
    b->data = r_buffer_memory(length, &b->mtl, &b->offset);
    std::memset(gptr(b->data), 0, length);
    return create(index ? g_vt_ib : g_vt_vb, b);
}

void dv_CreateVertexBuffer(Context& c) {
    // (length, usage, fvf, pool, out, shared)
    wr(arg(c, 5), make_buffer(false, arg(c, 1), arg(c, 2), arg(c, 3), arg(c, 4)));
    ret(c, D3D_OK, 7);
}

void dv_CreateIndexBuffer(Context& c) {
    wr(arg(c, 5), make_buffer(true, arg(c, 1), arg(c, 2), arg(c, 3), arg(c, 4)));
    ret(c, D3D_OK, 7);
}

void dv_CreateRenderTarget(Context& c) {
    // (w, h, fmt, ms, msq, lockable, out, shared)
    wr(arg(c, 7), new_surface(arg(c, 1), arg(c, 2), arg(c, 3), 1, 0, 0, 0));
    trace("CreateRenderTarget %ux%u format %x: %08x", arg(c, 1), arg(c, 2), arg(c, 3), rd(arg(c, 7)));
    ret(c, D3D_OK, 9);
}

void dv_CreateDepthStencilSurface(Context& c) {
    // (w, h, fmt, ms, msq, discard, out, shared)
    wr(arg(c, 7), new_surface(arg(c, 1), arg(c, 2), arg(c, 3), 2, 0, 0, 0));
    trace("CreateDepthStencilSurface %ux%u format %x: %08x", arg(c, 1), arg(c, 2), arg(c, 3), rd(arg(c, 7)));
    ret(c, D3D_OK, 9);
}

void dv_CreateOffscreenPlainSurface(Context& c) {
    // (w, h, fmt, pool, out, shared)
    wr(arg(c, 5), new_surface(arg(c, 1), arg(c, 2), arg(c, 3), 0, arg(c, 4), 0, 0));
    trace("CreateOffscreenPlainSurface %ux%u format %x: %08x", arg(c, 1), arg(c, 2), arg(c, 3), rd(arg(c, 5)));
    ret(c, D3D_OK, 7);
}

void dv_UpdateSurface(Context& c) {
    // (src, src rect, dst, dst point): a system-memory surface to a texture's level.
    Surface* src = as<Surface>(arg(c, 1));
    Surface* dst = as<Surface>(arg(c, 3));
    if (src && dst && src->format == dst->format && !arg(c, 2) && !arg(c, 4)) {
        const uint32_t rows = format_rows(src->format, std::min(src->height, dst->height));
        for (uint32_t y = 0; y < rows; y++)
            std::memcpy(gptr(dst->data + y * dst->pitch), gptr(src->data + y * src->pitch), std::min(src->pitch, dst->pitch));
        dst->dirty = true;
        if (Texture* t = as<Texture>(dst->container)) t->dirty = true;
    }
    ret(c, D3D_OK, 5);
}
void dv_UpdateTexture(Context& c) {
    // (src, dst): every level of a system-memory texture to a default-pool one.
    Texture* src = as<Texture>(arg(c, 1));
    Texture* dst = as<Texture>(arg(c, 2));
    if (src && dst && src->surfaces.size() == dst->surfaces.size()) {
        for (size_t k = 0; k < src->surfaces.size(); k++) {
            Surface* a = as<Surface>(src->surfaces[k]);
            Surface* b = as<Surface>(dst->surfaces[k]);
            std::memcpy(gptr(b->data), gptr(a->data), std::min(a->pitch * format_rows(a->format, a->height),
                                                               b->pitch * format_rows(b->format, b->height)));
            b->dirty = true;
        }
        dst->dirty = true;
    }
    ret(c, D3D_OK, 3);
}
void dv_GetRenderTargetData(Context& c) {
    r_read_back(dev(c), arg(c, 1), arg(c, 2));
    ret(c, D3D_OK, 3);
}
void dv_GetFrontBufferData(Context& c) { ret(c, D3D_OK, 3); }
void dv_StretchRect(Context& c) {
    // (src, src rect, dst, dst rect, filter)
    if (frame_traced(dev(c))) {
        auto rect = [](uint32_t r) {
            char b[64];
            if (r) std::snprintf(b, sizeof b, "(%d,%d)-(%d,%d)", (int)rd(r), (int)rd(r + 4), (int)rd(r + 8), (int)rd(r + 12));
            else std::snprintf(b, sizeof b, "all");
            return std::string(b);
        };
        auto desc = [](uint32_t o) {
            char b[96];
            if (const Surface* s = as<Surface>(o))
                std::snprintf(b, sizeof b, "%ux%u f%x usage %x container %08x", s->width, s->height, s->format, s->usage,
                              s->container);
            else
                std::snprintf(b, sizeof b, "%s", native(o) ? "not a surface" : "released");
            return std::string(b);
        };
        std::fprintf(stderr, "StretchRect(%08x %s [%s] -> %08x %s [%s], filter %u) from %08x\n", arg(c, 1),
                     rect(arg(c, 2)).c_str(), desc(arg(c, 1)).c_str(), arg(c, 3), rect(arg(c, 4)).c_str(),
                     desc(arg(c, 3)).c_str(), arg(c, 5), rd(c.esp));
    }
    r_stretch_rect(dev(c), arg(c, 1), arg(c, 2), arg(c, 3), arg(c, 4), arg(c, 5));
    ret(c, D3D_OK, 6);
}
void dv_ColorFill(Context& c) {
    if (frame_traced(dev(c))) std::fprintf(stderr, "ColorFill(%08x) not done\n", arg(c, 1));
    ret(c, D3D_OK, 4);
}

void dv_SetRenderTarget(Context& c) {
    Device* d = dev(c);
    const uint32_t k = arg(c, 1);
    if (k >= 4 || (k == 0 && !arg(c, 2))) return ret(c, D3DERR_INVALIDCALL, 3);  // target 0 is never unset
    bind(d->render_targets[k], arg(c, 2));
    if (frame_traced(d)) {
        const Surface* s = as<Surface>(arg(c, 2));
        std::fprintf(stderr, "SetRenderTarget(%u, %08x) %ux%u container %08x level %u, from %08x\n", k, arg(c, 2),
                     s ? s->width : 0, s ? s->height : 0, s ? s->container : 0, s ? s->level : 0, rd(c.esp));
    }
    if (k == 0 && arg(c, 2)) {
        if (Surface* s = as<Surface>(arg(c, 2))) {
            d->viewport[0] = d->viewport[1] = 0, d->viewport[2] = s->width, d->viewport[3] = s->height;
            d->scissor[0] = d->scissor[1] = 0, d->scissor[2] = s->width, d->scissor[3] = s->height;
        }
    }
    ret(c, D3D_OK, 3);
}

void dv_GetRenderTarget(Context& c) {
    Device* d = dev(c);
    const uint32_t k = arg(c, 1), s = k < 4 ? d->render_targets[k] : 0;
    add_ref(s);
    wr(arg(c, 2), s);
    ret(c, s ? D3D_OK : D3DERR_NOTFOUND, 3);
}

void dv_SetDepthStencilSurface(Context& c) {
    bind(dev(c)->depth_stencil, arg(c, 1));
    ret(c, D3D_OK, 2);
}

void dv_GetDepthStencilSurface(Context& c) {
    Device* d = dev(c);
    add_ref(d->depth_stencil);
    wr(arg(c, 1), d->depth_stencil);
    ret(c, d->depth_stencil ? D3D_OK : D3DERR_NOTFOUND, 2);
}

void dv_BeginScene(Context& c) { ret(c, D3D_OK, 1); }
void dv_EndScene(Context& c) { ret(c, D3D_OK, 1); }

void dv_Clear(Context& c) {
    // (count, rects, flags, color, z, stencil)
    r_clear(dev(c), arg(c, 3), arg(c, 4), f32(arg(c, 5)), arg(c, 6));
    ret(c, D3D_OK, 7);
}

void dv_SetTransform(Context& c) {
    const uint32_t s = arg(c, 1);
    if (s < 512) std::memcpy(dev(c)->transforms[s], gptr(arg(c, 2)), 64);
    ret(c, D3D_OK, 3);
}
void dv_GetTransform(Context& c) {
    const uint32_t s = arg(c, 1);
    if (s < 512) std::memcpy(gptr(arg(c, 2)), dev(c)->transforms[s], 64);
    ret(c, D3D_OK, 3);
}
void dv_SetViewport(Context& c) {
    std::memcpy(dev(c)->viewport, gptr(arg(c, 1)), 24);
    ret(c, D3D_OK, 2);
}
void dv_GetViewport(Context& c) {
    std::memcpy(gptr(arg(c, 1)), dev(c)->viewport, 24);
    ret(c, D3D_OK, 2);
}
void dv_SetMaterial(Context& c) { ret(c, D3D_OK, 2); }
void dv_SetLight(Context& c) { ret(c, D3D_OK, 3); }
void dv_LightEnable(Context& c) { ret(c, D3D_OK, 3); }
void dv_SetClipPlane(Context& c) { ret(c, D3D_OK, 3); }

void dv_SetRenderState(Context& c) {
    const uint32_t s = arg(c, 1);
    if (s < 256) dev(c)->render_state[s] = arg(c, 2);
    ret(c, D3D_OK, 3);
}
void dv_GetRenderState(Context& c) {
    const uint32_t s = arg(c, 1);
    wr(arg(c, 2), s < 256 ? dev(c)->render_state[s] : 0);
    ret(c, D3D_OK, 3);
}

void dv_CreateStateBlock(Context& c) {
    wr(arg(c, 2), create(g_vt_stateblock, new StateBlock));
    ret(c, D3D_OK, 3);
}
// Recorded state blocks: between BeginStateBlock and EndStateBlock the Set*
// calls a block can hold are recorded instead of changing the device, and the
// block's Apply replays them. Revelations records two at startup and applies
// them thousands of times a minute to reset its states between passes.
void dv_BeginStateBlock(Context& c) {
    Device* d = dev(c);
    if (d->recording) return ret(c, D3DERR_INVALIDCALL, 1);
    d->recording = new StateBlock;
    ret(c, D3D_OK, 1);
}
void dv_EndStateBlock(Context& c) {
    Device* d = dev(c);
    if (!d->recording) return ret(c, D3DERR_INVALIDCALL, 2);
    trace("EndStateBlock: %zu calls recorded", d->recording->calls.size());
    wr(arg(c, 1), create(g_vt_stateblock, d->recording));
    d->recording = nullptr;
    ret(c, D3D_OK, 2);
}

// The setter F (stdcall, N arguments with `this`), recorded while a block is
// being recorded. Obj: an argument naming an object the block keeps a
// reference to; Ptr: a pointer argument whose data is copied, Fixed bytes or
// Per bytes times argument Count.
template <GuestFn F, int N, int Obj = -1, int Ptr = -1, int Fixed = 0, int Count = -1, int Per = 0>
void recordable(Context& c) {
    Device* d = dev(c);
    if (!d->recording) return F(c);
    Recorded r;
    r.fn = F;
    for (int k = 1; k < N; k++) r.args.push_back(arg(c, k));
    if (Obj > 0) add_ref(arg(c, Obj));
    if (Ptr > 0 && arg(c, Ptr)) {
        const uint32_t bytes = Fixed ? (uint32_t)Fixed : (uint32_t)Per * arg(c, Count);
        r.ptr = Ptr;
        r.blob.assign(gptr(arg(c, Ptr)), gptr(arg(c, Ptr)) + bytes);
    }
    d->recording->calls.push_back(std::move(r));
    ret(c, D3D_OK, N);
}

uint32_t sampler_index(uint32_t s) { return s >= 257 ? s - 257 + 16 : s; }  // D3DVERTEXTEXTURESAMPLER0 = 257

void dv_GetTexture(Context& c) {
    const uint32_t k = sampler_index(arg(c, 1));
    const uint32_t t = k < 20 ? dev(c)->textures[k] : 0;
    add_ref(t);
    wr(arg(c, 2), t);
    ret(c, D3D_OK, 3);
}
void dv_SetTexture(Context& c) {
    const uint32_t k = sampler_index(arg(c, 1));
    if (k < 20) bind(dev(c)->textures[k], arg(c, 2));
    ret(c, D3D_OK, 3);
}
void dv_GetTextureStageState(Context& c) {
    const uint32_t s = arg(c, 1), t = arg(c, 2);
    wr(arg(c, 3), s < 8 && t < 33 ? dev(c)->stage_state[s][t] : 0);
    ret(c, D3D_OK, 4);
}
void dv_SetTextureStageState(Context& c) {
    const uint32_t s = arg(c, 1), t = arg(c, 2);
    if (s < 8 && t < 33) dev(c)->stage_state[s][t] = arg(c, 3);
    ret(c, D3D_OK, 4);
}
void dv_GetSamplerState(Context& c) {
    const uint32_t s = sampler_index(arg(c, 1)), t = arg(c, 2);
    wr(arg(c, 3), s < 20 && t < 14 ? dev(c)->sampler_state[s][t] : 0);
    ret(c, D3D_OK, 4);
}
void dv_SetSamplerState(Context& c) {
    const uint32_t s = sampler_index(arg(c, 1)), t = arg(c, 2);
    if (s < 20 && t < 14) dev(c)->sampler_state[s][t] = arg(c, 3);
    ret(c, D3D_OK, 4);
}
void dv_ValidateDevice(Context& c) {
    wr(arg(c, 1), 1);
    ret(c, D3D_OK, 2);
}
void dv_SetScissorRect(Context& c) {
    std::memcpy(dev(c)->scissor, gptr(arg(c, 1)), 16);
    ret(c, D3D_OK, 2);
}
void dv_GetScissorRect(Context& c) {
    std::memcpy(gptr(arg(c, 1)), dev(c)->scissor, 16);
    ret(c, D3D_OK, 2);
}
void dv_SetSoftwareVertexProcessing(Context& c) { ret(c, D3D_OK, 2); }
void dv_GetSoftwareVertexProcessing(Context& c) { ret(c, 0, 1); }
void dv_SetNPatchMode(Context& c) { ret(c, D3D_OK, 2); }

void dv_DrawPrimitive(Context& c) {
    // (type, start vertex, primitives)
    Device* d = dev(c);
    d->draws++;
    r_draw(d, arg(c, 1), arg(c, 2), arg(c, 3));
    ret(c, D3D_OK, 4);
}
void dv_DrawIndexedPrimitive(Context& c) {
    // (type, base vertex, min index, vertices, start index, primitives)
    Device* d = dev(c);
    d->draws++;
    r_draw_indexed(d, arg(c, 1), (int32_t)arg(c, 2), arg(c, 5), arg(c, 6));
    ret(c, D3D_OK, 7);
}
void dv_DrawPrimitiveUP(Context& c) {
    // (type, primitives, data, stride)
    Device* d = dev(c);
    d->draws++;
    r_draw_up(d, arg(c, 1), arg(c, 2), arg(c, 3), arg(c, 4));
    ret(c, D3D_OK, 5);
}
void dv_DrawIndexedPrimitiveUP(Context& c) {
    // (type, min index, vertices, primitives, indices, index format, data, stride)
    Device* d = dev(c);
    d->draws++;
    r_draw_indexed_up(d, arg(c, 1), arg(c, 3), arg(c, 4), arg(c, 5), arg(c, 6), arg(c, 7), arg(c, 8));
    ret(c, D3D_OK, 9);
}

void dv_CreateVertexDeclaration(Context& c) {
    auto* d = new Declaration;
    for (uint32_t p = arg(c, 1);; p += 8) {
        const uint8_t* e = gptr(p);
        d->elements.insert(d->elements.end(), e, e + 8);
        if (rd16(p) == 0xff) break;  // D3DDECL_END: stream 0xff
        if (d->elements.size() > 8 * 64) break;
    }
    wr(arg(c, 2), create(g_vt_decl, d));
    ret(c, D3D_OK, 3);
}
void dv_SetVertexDeclaration(Context& c) {
    bind(dev(c)->decl, arg(c, 1));
    ret(c, D3D_OK, 2);
}
void dv_GetVertexDeclaration(Context& c) {
    const uint32_t v = dev(c)->decl;
    add_ref(v);
    wr(arg(c, 1), v);
    ret(c, D3D_OK, 2);
}
void dv_SetFVF(Context& c) {
    dev(c)->fvf = arg(c, 1);
    ret(c, D3D_OK, 2);
}
void dv_GetFVF(Context& c) {
    wr(arg(c, 1), dev(c)->fvf);
    ret(c, D3D_OK, 2);
}

uint32_t make_shader(bool vertex, uint32_t tokens) {
    auto* s = new Shader;
    s->vertex = vertex;
    for (uint32_t p = tokens;; p += 4) {
        const uint32_t t = rd(p);
        s->tokens.push_back(t);
        if (t == 0x0000ffff || s->tokens.size() > (1u << 20)) break;  // D3DSIO_END
        if ((t & 0xffff) == 0xfffe) {  // comment: skip its body
            const uint32_t n = t >> 16 & 0x7fff;
            for (uint32_t k = 0; k < n; k++) s->tokens.push_back(rd(p + 4 + 4 * k));
            p += 4 * n;
        }
    }
    return create(vertex ? g_vt_vs : g_vt_ps, s);
}

void dv_CreateVertexShader(Context& c) {
    wr(arg(c, 2), make_shader(true, arg(c, 1)));
    ret(c, D3D_OK, 3);
}
void dv_SetVertexShader(Context& c) {
    bind(dev(c)->vs, arg(c, 1));
    ret(c, D3D_OK, 2);
}
void dv_GetVertexShader(Context& c) {
    const uint32_t v = dev(c)->vs;
    add_ref(v);
    wr(arg(c, 1), v);
    ret(c, D3D_OK, 2);
}
void dv_CreatePixelShader(Context& c) {
    wr(arg(c, 2), make_shader(false, arg(c, 1)));
    ret(c, D3D_OK, 3);
}
void dv_SetPixelShader(Context& c) {
    bind(dev(c)->ps, arg(c, 1));
    ret(c, D3D_OK, 2);
}
void dv_GetPixelShader(Context& c) {
    const uint32_t v = dev(c)->ps;
    add_ref(v);
    wr(arg(c, 1), v);
    ret(c, D3D_OK, 2);
}

template <bool Vertex> void set_const_f(Context& c) {
    Device* d = dev(c);
    std::vector<float>& v = Vertex ? d->vs_const : d->ps_const;
    const uint32_t start = arg(c, 1), data = arg(c, 2), n = arg(c, 3);
    if ((start + n) * 4 > v.size()) return ret(c, D3DERR_INVALIDCALL, 4);
    if (frame_traced(d)) {
        const float* f = (const float*)gptr(data);
        std::fprintf(stderr, "Set%cShaderConstantF(%u, %08x, %u) = %g %g %g %g from %08x\n", Vertex ? 'V' : 'P', start,
                     data, n, f[0], f[1], f[2], f[3], rd(c.esp));
    }
    std::memcpy(&v[start * 4], gptr(data), 16 * n);
    ret(c, D3D_OK, 4);
}
template <bool Vertex> void get_const_f(Context& c) {
    Device* d = dev(c);
    std::vector<float>& v = Vertex ? d->vs_const : d->ps_const;
    const uint32_t start = arg(c, 1), data = arg(c, 2), n = arg(c, 3);
    if ((start + n) * 4 > v.size()) return ret(c, D3DERR_INVALIDCALL, 4);
    std::memcpy(gptr(data), &v[start * 4], 16 * n);
    ret(c, D3D_OK, 4);
}
template <bool Vertex> void set_const_i(Context& c) {
    Device* d = dev(c);
    const uint32_t start = arg(c, 1), n = arg(c, 3);
    if (start + n > 16) return ret(c, D3DERR_INVALIDCALL, 4);
    std::memcpy((Vertex ? d->vs_int : d->ps_int) + 4 * start, gptr(arg(c, 2)), 16 * n);
    ret(c, D3D_OK, 4);
}
template <bool Vertex> void set_const_b(Context& c) {
    Device* d = dev(c);
    const uint32_t start = arg(c, 1), n = arg(c, 3);
    if (start + n > 16) return ret(c, D3DERR_INVALIDCALL, 4);
    std::memcpy((Vertex ? d->vs_bool : d->ps_bool) + start, gptr(arg(c, 2)), 4 * n);
    ret(c, D3D_OK, 4);
}

void dv_SetStreamSource(Context& c) {
    const uint32_t k = arg(c, 1);
    if (k >= 16) return ret(c, D3DERR_INVALIDCALL, 5);
    Device* d = dev(c);
    bind(d->streams[k][0], arg(c, 2));
    d->streams[k][1] = arg(c, 3), d->streams[k][2] = arg(c, 4);
    ret(c, D3D_OK, 5);
}
void dv_GetStreamSource(Context& c) {
    const uint32_t k = arg(c, 1);
    Device* d = dev(c);
    add_ref(d->streams[k][0]);
    wr(arg(c, 2), d->streams[k][0]), wr(arg(c, 3), d->streams[k][1]), wr(arg(c, 4), d->streams[k][2]);
    ret(c, D3D_OK, 5);
}
void dv_SetStreamSourceFreq(Context& c) {
    if (arg(c, 1) < 16) dev(c)->stream_freq[arg(c, 1)] = arg(c, 2);
    ret(c, D3D_OK, 3);
}
void dv_SetIndices(Context& c) {
    bind(dev(c)->indices, arg(c, 1));
    ret(c, D3D_OK, 2);
}
void dv_GetIndices(Context& c) {
    const uint32_t v = dev(c)->indices;
    add_ref(v);
    wr(arg(c, 1), v);
    ret(c, D3D_OK, 2);
}
void dv_CreateQuery(Context& c) {
    // (type, out): out null asks whether the type is supported.
    const uint32_t type = arg(c, 1), out = arg(c, 2);
    if (!out) return ret(c, D3D_OK, 3);
    auto* q = new Query;
    q->type = type;
    wr(out, create(g_vt_query, q));
    ret(c, D3D_OK, 3);
}

// ---- resources' methods ----

// IDirect3DResource9: GetDevice, SetPrivateData, GetPrivateData, FreePrivateData,
// SetPriority, GetPriority, PreLoad, GetType (after IUnknown).
// GetDevice and GetDirect3D add a reference, as Direct3D's do (the caller
// releases it).
void rs_GetDevice(Context& c) {
    add_ref(g_device_object);
    wr(arg(c, 1), g_device_object);
    ret(c, D3D_OK, 2);
}
void rs_SetPrivateData(Context& c) { ret(c, D3D_OK, 5); }
void rs_GetPrivateData(Context& c) { ret(c, D3DERR_NOTFOUND, 4); }
void rs_FreePrivateData(Context& c) { ret(c, D3D_OK, 2); }
void rs_SetPriority(Context& c) { ret(c, 0, 2); }
void rs_GetPriority(Context& c) { ret(c, 0, 1); }
void rs_PreLoad(Context& c) { ret(c, 0, 1); }
template <uint32_t Type> void rs_GetType(Context& c) { ret(c, Type, 1); }

// Surfaces: GetContainer, GetDesc, LockRect, UnlockRect, GetDC, ReleaseDC.
void sf_GetContainer(Context& c) {
    Surface* s = as<Surface>(arg(c, 0));
    if (!s || !s->container) return ret(c, E_NOINTERFACE, 3);
    wr(arg(c, 2), s->container);
    ret(c, D3D_OK, 3);
}
void put_surface_desc(uint32_t out, const Surface* s) {
    // Format, Type (1 surface), Usage, Pool, MultiSampleType, Quality, Width, Height
    wr(out, s->format), wr(out + 4, 1), wr(out + 8, s->usage), wr(out + 12, s->pool);
    wr(out + 16, 0), wr(out + 20, 0), wr(out + 24, s->width), wr(out + 28, s->height);
}
// A texture's level surfaces share the texture's reference count, as in
// Direct3D: their AddRef and Release count the container.
void sf_AddRef(Context& c) {
    Surface* s = as<Surface>(arg(c, 0));
    Native* n = s && s->container ? native(s->container) : native(arg(c, 0));
    ret(c, n ? ++n->refs : 1, 1);
}
void sf_Release(Context& c) {
    Surface* s = as<Surface>(arg(c, 0));
    const uint32_t owner = s && s->container ? s->container : arg(c, 0);
    Native* n = native(owner);
    if (!n) return ret(c, 0, 1);
    const uint32_t r = n->refs ? --n->refs : 0;
    if (!r) destroy(owner);
    ret(c, r, 1);
}
void sf_GetDesc(Context& c) {
    Surface* s = as<Surface>(arg(c, 0));
    put_surface_desc(arg(c, 1), s);
    ret(c, D3D_OK, 2);
}
uint32_t lock_surface(Surface* s, uint32_t out, uint32_t rect) {
    uint32_t off = 0;
    if (rect) {
        const uint32_t x = rd(rect), y = rd(rect + 4);
        off = (y ? format_rows(s->format, y) : 0) * s->pitch + (x ? format_pitch(s->format, x) : 0);
    }
    wr(out, s->pitch), wr(out + 4, s->data + off);
    return D3D_OK;
}
void sf_LockRect(Context& c) {
    Surface* s = as<Surface>(arg(c, 0));
    ret(c, lock_surface(s, arg(c, 1), arg(c, 2)), 4);
}
void sf_UnlockRect(Context& c) {
    Surface* s = as<Surface>(arg(c, 0));
    s->dirty = true;
    if (s->container)
        if (Texture* t = as<Texture>(s->container)) t->dirty = true;
    ret(c, D3D_OK, 1);
}
void sf_GetDC(Context& c) { ret(c, D3DERR_INVALIDCALL, 2); }
void sf_ReleaseDC(Context& c) { ret(c, D3D_OK, 2); }

// Base textures: SetLOD, GetLOD, GetLevelCount, SetAutoGenFilterType,
// GetAutoGenFilterType, GenerateMipSubLevels; then the texture's own methods.
void tx_SetLOD(Context& c) {
    Texture* t = as<Texture>(arg(c, 0));
    const uint32_t old = t->lod;
    t->lod = arg(c, 1);
    ret(c, old, 2);
}
void tx_GetLOD(Context& c) { ret(c, as<Texture>(arg(c, 0))->lod, 1); }
void tx_GetLevelCount(Context& c) {
    Texture* t = as<Texture>(arg(c, 0));
    ret(c, t->usage & 0x400 ? 1 : t->levels, 1);
}
void tx_SetAutoGenFilterType(Context& c) { ret(c, D3D_OK, 2); }
void tx_GetAutoGenFilterType(Context& c) { ret(c, 2, 1); }
void tx_GenerateMipSubLevels(Context& c) { ret(c, 0, 1); }
uint32_t level_surface(Texture* t, uint32_t level, uint32_t face) {
    const uint32_t faces = t->cube ? 6 : 1;
    if (level >= t->levels || face >= faces) return 0;
    return t->surfaces[level * faces + face];
}
void tx_GetLevelDesc(Context& c) {
    Texture* t = as<Texture>(arg(c, 0));
    const uint32_t s = level_surface(t, arg(c, 1), 0);
    if (!s) return ret(c, D3DERR_INVALIDCALL, 3);
    put_surface_desc(arg(c, 2), as<Surface>(s));
    ret(c, D3D_OK, 3);
}
void tx_GetSurfaceLevel(Context& c) {
    Texture* t = as<Texture>(arg(c, 0));
    const uint32_t s = level_surface(t, arg(c, 1), 0);
    if (!s) return ret(c, D3DERR_INVALIDCALL, 3);
    t->refs++;  // a level's references are its texture's (sf_AddRef, sf_Release)
    wr(arg(c, 2), s);
    ret(c, D3D_OK, 3);
}
void tx_LockRect(Context& c) {
    // (level, out, rect, flags)
    Texture* t = as<Texture>(arg(c, 0));
    const uint32_t s = level_surface(t, arg(c, 1), 0);
    if (!s) return ret(c, D3DERR_INVALIDCALL, 5);
    ret(c, lock_surface(as<Surface>(s), arg(c, 2), arg(c, 3)), 5);
}
void tx_UnlockRect(Context& c) {
    Texture* t = as<Texture>(arg(c, 0));
    t->dirty = true;
    if (const uint32_t s = level_surface(t, arg(c, 1), 0)) as<Surface>(s)->dirty = true;
    ret(c, D3D_OK, 2);
}
void tx_AddDirtyRect(Context& c) { ret(c, D3D_OK, 2); }
// Cube textures: GetLevelDesc, GetCubeMapSurface(face, level, out), LockRect(face, level, out, rect, flags)...
void cb_GetCubeMapSurface(Context& c) {
    Texture* t = as<Texture>(arg(c, 0));
    const uint32_t s = level_surface(t, arg(c, 2), arg(c, 1));
    if (!s) return ret(c, D3DERR_INVALIDCALL, 4);
    t->refs++;
    wr(arg(c, 3), s);
    ret(c, D3D_OK, 4);
}
void cb_LockRect(Context& c) {
    Texture* t = as<Texture>(arg(c, 0));
    const uint32_t s = level_surface(t, arg(c, 2), arg(c, 1));
    if (!s) return ret(c, D3DERR_INVALIDCALL, 6);
    ret(c, lock_surface(as<Surface>(s), arg(c, 3), arg(c, 4)), 6);
}
void cb_UnlockRect(Context& c) {
    Texture* t = as<Texture>(arg(c, 0));
    t->dirty = true;
    if (const uint32_t s = level_surface(t, arg(c, 2), arg(c, 1))) as<Surface>(s)->dirty = true;
    ret(c, D3D_OK, 3);
}
void cb_AddDirtyRect(Context& c) { ret(c, D3D_OK, 3); }
// Volume textures: LockBox(level, out D3DLOCKED_BOX{RowPitch, SlicePitch, pBits}, box, flags).
void vl_LockBox(Context& c) {
    Texture* t = as<Texture>(arg(c, 0));
    const uint32_t s = level_surface(t, arg(c, 1), 0);
    if (!s) return ret(c, D3DERR_INVALIDCALL, 5);
    Surface* sf = as<Surface>(s);
    const uint32_t out = arg(c, 2);
    const uint32_t lh = std::max(1u, t->height >> arg(c, 1));
    wr(out, sf->pitch), wr(out + 4, sf->pitch * format_rows(sf->format, lh)), wr(out + 8, sf->data);
    ret(c, D3D_OK, 5);
}
void vl_GetLevelDesc(Context& c) {
    // D3DVOLUME_DESC: Format, Type (4 volume), Usage, Pool, Width, Height, Depth
    Texture* t = as<Texture>(arg(c, 0));
    const uint32_t l = arg(c, 1), out = arg(c, 2);
    wr(out, t->format), wr(out + 4, 4), wr(out + 8, t->usage), wr(out + 12, t->pool);
    wr(out + 16, std::max(1u, t->width >> l)), wr(out + 20, std::max(1u, t->height >> l)),
        wr(out + 24, std::max(1u, t->depth >> l));
    ret(c, D3D_OK, 3);
}

// Vertex and index buffers: Lock(offset, size, out, flags), Unlock, GetDesc.
void bf_Lock(Context& c) {
    // (offset, size, out, flags): D3DLOCK_DISCARD on a dynamic buffer gets
    // fresh memory, as the driver renames it: draws already recorded keep
    // reading the old contents. So does any other lock of a buffer a recorded
    // draw uses (Direct3D waits for those draws instead), keeping its
    // contents; NOOVERWRITE and READONLY promise not to touch what draws read.
    Buffer* b = as<Buffer>(arg(c, 0));
    const uint32_t flags = arg(c, 4);
    if ((flags & 0x2000) && (b->usage & 0x200)) {
        r_buffer_release(b->mtl, b->offset, b->length);
        b->data = r_buffer_memory(b->length, &b->mtl, &b->offset);
        b->used = false;
    } else if (b->used && !(flags & 0x1010)) {  // D3DLOCK_NOOVERWRITE | D3DLOCK_READONLY
        const uint32_t old = b->data;
        id<MTLBuffer> old_mtl = b->mtl;
        const uint32_t old_offset = b->offset;
        b->data = r_buffer_memory(b->length, &b->mtl, &b->offset);
        std::memcpy(gptr(b->data), gptr(old), b->length);
        r_buffer_release(old_mtl, old_offset, b->length);
        b->used = false;
    }
    wr(arg(c, 3), b->data + arg(c, 1));
    ret(c, D3D_OK, 5);
}
void bf_Unlock(Context& c) { ret(c, D3D_OK, 1); }
void bf_GetDesc(Context& c) {
    Buffer* b = as<Buffer>(arg(c, 0));
    const uint32_t out = arg(c, 1);
    // D3DVERTEXBUFFER_DESC: Format, Type (2 vb, 3 ib), Usage, Pool, Size, FVF
    wr(out, b->index ? b->format : 100), wr(out + 4, b->index ? 3 : 2), wr(out + 8, b->usage);
    wr(out + 12, b->pool), wr(out + 16, b->length);
    if (!b->index) wr(out + 20, b->fvf);
    ret(c, D3D_OK, 2);
}

// Shaders: GetDevice, GetFunction(data, size*).
void sh_GetFunction(Context& c) {
    Shader* s = as<Shader>(arg(c, 0));
    const uint32_t data = arg(c, 1), size = arg(c, 2);
    if (data) std::memcpy(gptr(data), s->tokens.data(), 4 * s->tokens.size());
    wr(size, 4 * (uint32_t)s->tokens.size());
    ret(c, D3D_OK, 3);
}

// Declarations: GetDevice, GetDeclaration(elements, count*).
void dc_GetDeclaration(Context& c) {
    Declaration* d = as<Declaration>(arg(c, 0));
    if (arg(c, 1)) std::memcpy(gptr(arg(c, 1)), d->elements.data(), d->elements.size());
    wr(arg(c, 2), (uint32_t)d->elements.size() / 8);
    ret(c, D3D_OK, 3);
}

// Queries: GetDevice, GetType, GetDataSize, Issue, GetData(data, size, flags).
void qr_GetType(Context& c) { ret(c, as<Query>(arg(c, 0))->type, 1); }
uint32_t query_size(uint32_t type) { return type == 9 ? 4 : type == 8 ? 4 : type == 2 ? 4 : 8; }
void qr_GetDataSize(Context& c) { ret(c, query_size(as<Query>(arg(c, 0))->type), 1); }
void qr_Issue(Context& c) { ret(c, D3D_OK, 2); }
void qr_GetData(Context& c) {
    // Every query is done at once: events signalled, occlusion "all visible".
    Query* q = as<Query>(arg(c, 0));
    const uint32_t data = arg(c, 1), size = arg(c, 2);
    if (data && size >= 4) wr(data, q->type == 9 ? 100000 : 1);
    if (data && size >= 8) wr(data + 4, 0);
    ret(c, D3D_OK, 4);
}

// State blocks: GetDevice, Capture, Apply.
void sb_Capture(Context& c) { ret(c, D3D_OK, 1); }
void sb_Apply(Context& c) {
    // Replays a recorded block's calls on the device, each with a frame of
    // its own below the stack pointer ([return] device args...).
    if (StateBlock* sb = as<StateBlock>(arg(c, 0)))
        for (Recorded& r : sb->calls) {
            if (r.ptr > 0 && !r.guest) {
                r.guest = galloc((uint32_t)r.blob.size() + 16);
                std::memcpy(gptr(r.guest), r.blob.data(), r.blob.size());
            }
            const uint32_t esp = c.esp;
            c.esp = (c.esp - 4 * (uint32_t)r.args.size() - 64) & ~15u;
            wr(c.esp + 4, g_device_object);
            for (size_t k = 0; k < r.args.size(); k++)
                wr(c.esp + 8 + 4 * (uint32_t)k, (int)k + 1 == r.ptr ? r.guest : r.args[k]);
            r.fn(c);
            c.esp = esp;
        }
    ret(c, D3D_OK, 1);
}

#define M(n, f) {n, f}
#define U(n) {n, nullptr}

void create_device_vtables() {
    static bool done = false;
    if (done) return;
    done = true;
    const std::vector<Method> iunknown = {M("QueryInterface", QueryInterface), M("AddRef", AddRef),
                                          M("Release", Release)};
    auto with = [&](std::vector<Method> rest) {
        std::vector<Method> v = iunknown;
        v.insert(v.end(), rest.begin(), rest.end());
        return v;
    };
    const std::vector<Method> resource = {
        M("GetDevice", rs_GetDevice), M("SetPrivateData", rs_SetPrivateData),
        M("GetPrivateData", rs_GetPrivateData), M("FreePrivateData", rs_FreePrivateData),
        M("SetPriority", rs_SetPriority), M("GetPriority", rs_GetPriority), M("PreLoad", rs_PreLoad)};
    auto res = [&](GuestFn type, std::vector<Method> rest) {
        std::vector<Method> v = with(resource);
        v.push_back(M("GetType", type));
        v.insert(v.end(), rest.begin(), rest.end());
        return v;
    };
    const std::vector<Method> basetex = {
        M("SetLOD", tx_SetLOD), M("GetLOD", tx_GetLOD), M("GetLevelCount", tx_GetLevelCount),
        M("SetAutoGenFilterType", tx_SetAutoGenFilterType), M("GetAutoGenFilterType", tx_GetAutoGenFilterType),
        M("GenerateMipSubLevels", tx_GenerateMipSubLevels)};
    auto tex = [&](GuestFn type, std::vector<Method> rest) {
        std::vector<Method> v = res(type, basetex);
        v.insert(v.end(), rest.begin(), rest.end());
        return v;
    };
    {
        std::vector<Method> v = res(rs_GetType<1>, {M("GetContainer", sf_GetContainer), M("GetDesc", sf_GetDesc),
                                                    M("LockRect", sf_LockRect), M("UnlockRect", sf_UnlockRect),
                                                    M("GetDC", sf_GetDC), M("ReleaseDC", sf_ReleaseDC)});
        v[1] = M("AddRef", sf_AddRef), v[2] = M("Release", sf_Release);
        g_vt_surface = make_vtable("IDirect3DSurface9", v);
    }
    g_vt_texture = make_vtable("IDirect3DTexture9",
                               tex(rs_GetType<3>, {M("GetLevelDesc", tx_GetLevelDesc),
                                                   M("GetSurfaceLevel", tx_GetSurfaceLevel),
                                                   M("LockRect", tx_LockRect), M("UnlockRect", tx_UnlockRect),
                                                   M("AddDirtyRect", tx_AddDirtyRect)}));
    g_vt_volume = make_vtable("IDirect3DVolumeTexture9",
                              tex(rs_GetType<4>, {M("GetLevelDesc", vl_GetLevelDesc), U("GetVolumeLevel"),
                                                  M("LockBox", vl_LockBox), M("UnlockBox", tx_UnlockRect),
                                                  M("AddDirtyBox", tx_AddDirtyRect)}));
    g_vt_cube = make_vtable("IDirect3DCubeTexture9",
                            tex(rs_GetType<5>, {M("GetLevelDesc", tx_GetLevelDesc),
                                                M("GetCubeMapSurface", cb_GetCubeMapSurface),
                                                M("LockRect", cb_LockRect), M("UnlockRect", cb_UnlockRect),
                                                M("AddDirtyRect", cb_AddDirtyRect)}));
    g_vt_vb = make_vtable("IDirect3DVertexBuffer9", res(rs_GetType<6>, {M("Lock", bf_Lock), M("Unlock", bf_Unlock),
                                                                         M("GetDesc", bf_GetDesc)}));
    g_vt_ib = make_vtable("IDirect3DIndexBuffer9", res(rs_GetType<7>, {M("Lock", bf_Lock), M("Unlock", bf_Unlock),
                                                                        M("GetDesc", bf_GetDesc)}));
    g_vt_vs = make_vtable("IDirect3DVertexShader9", with({M("GetDevice", rs_GetDevice), M("GetFunction", sh_GetFunction)}));
    g_vt_ps = make_vtable("IDirect3DPixelShader9", with({M("GetDevice", rs_GetDevice), M("GetFunction", sh_GetFunction)}));
    g_vt_decl = make_vtable("IDirect3DVertexDeclaration9",
                            with({M("GetDevice", rs_GetDevice), M("GetDeclaration", dc_GetDeclaration)}));
    g_vt_query = make_vtable("IDirect3DQuery9", with({M("GetDevice", rs_GetDevice), M("GetType", qr_GetType),
                                                      M("GetDataSize", qr_GetDataSize), M("Issue", qr_Issue),
                                                      M("GetData", qr_GetData)}));
    g_vt_stateblock = make_vtable("IDirect3DStateBlock9", with({M("GetDevice", rs_GetDevice),
                                                                M("Capture", sb_Capture), M("Apply", sb_Apply)}));
    g_vt_device = make_vtable("IDirect3DDevice9", with({
        M("TestCooperativeLevel", dv_TestCooperativeLevel), M("GetAvailableTextureMem", dv_GetAvailableTextureMem),
        M("EvictManagedResources", dv_EvictManagedResources), M("GetDirect3D", dv_GetDirect3D),
        M("GetDeviceCaps", dv_GetDeviceCaps), M("GetDisplayMode", dv_GetDisplayMode),
        M("GetCreationParameters", dv_GetCreationParameters), M("SetCursorProperties", dv_SetCursorProperties),
        M("SetCursorPosition", dv_SetCursorPosition), M("ShowCursor", dv_ShowCursor),
        U("CreateAdditionalSwapChain"), U("GetSwapChain"), M("GetNumberOfSwapChains", dv_GetNumberOfSwapChains),
        M("Reset", dv_Reset), M("Present", dv_Present), M("GetBackBuffer", dv_GetBackBuffer),
        M("GetRasterStatus", dv_GetRasterStatus), M("SetDialogBoxMode", dv_SetDialogBoxMode),
        M("SetGammaRamp", dv_SetGammaRamp), M("GetGammaRamp", dv_GetGammaRamp),
        M("CreateTexture", dv_CreateTexture), M("CreateVolumeTexture", dv_CreateVolumeTexture),
        M("CreateCubeTexture", dv_CreateCubeTexture), M("CreateVertexBuffer", dv_CreateVertexBuffer),
        M("CreateIndexBuffer", dv_CreateIndexBuffer), M("CreateRenderTarget", dv_CreateRenderTarget),
        M("CreateDepthStencilSurface", dv_CreateDepthStencilSurface), M("UpdateSurface", dv_UpdateSurface),
        M("UpdateTexture", dv_UpdateTexture), M("GetRenderTargetData", dv_GetRenderTargetData),
        M("GetFrontBufferData", dv_GetFrontBufferData), M("StretchRect", dv_StretchRect),
        M("ColorFill", dv_ColorFill), M("CreateOffscreenPlainSurface", dv_CreateOffscreenPlainSurface),
        M("SetRenderTarget", dv_SetRenderTarget), M("GetRenderTarget", dv_GetRenderTarget),
        M("SetDepthStencilSurface", dv_SetDepthStencilSurface),
        M("GetDepthStencilSurface", dv_GetDepthStencilSurface), M("BeginScene", dv_BeginScene),
        M("EndScene", dv_EndScene), M("Clear", dv_Clear), M("SetTransform", (recordable<dv_SetTransform, 3, -1, 2, 64>)),
        M("GetTransform", dv_GetTransform), U("MultiplyTransform"), M("SetViewport", (recordable<dv_SetViewport, 2, -1, 1, 24>)),
        M("GetViewport", dv_GetViewport), M("SetMaterial", (recordable<dv_SetMaterial, 2, -1, 1, 68>)), U("GetMaterial"),
        M("SetLight", (recordable<dv_SetLight, 3, -1, 2, 104>)), U("GetLight"), M("LightEnable", (recordable<dv_LightEnable, 3>)), U("GetLightEnable"),
        M("SetClipPlane", (recordable<dv_SetClipPlane, 3, -1, 2, 16>)), U("GetClipPlane"), M("SetRenderState", (recordable<dv_SetRenderState, 3>)),
        M("GetRenderState", dv_GetRenderState), M("CreateStateBlock", dv_CreateStateBlock),
        M("BeginStateBlock", dv_BeginStateBlock), M("EndStateBlock", dv_EndStateBlock), U("SetClipStatus"),
        U("GetClipStatus"), M("GetTexture", dv_GetTexture), M("SetTexture", (recordable<dv_SetTexture, 3, 2>)),
        M("GetTextureStageState", dv_GetTextureStageState), M("SetTextureStageState", (recordable<dv_SetTextureStageState, 4>)),
        M("GetSamplerState", dv_GetSamplerState), M("SetSamplerState", (recordable<dv_SetSamplerState, 4>)),
        M("ValidateDevice", dv_ValidateDevice), U("SetPaletteEntries"), U("GetPaletteEntries"),
        U("SetCurrentTexturePalette"), U("GetCurrentTexturePalette"), M("SetScissorRect", (recordable<dv_SetScissorRect, 2, -1, 1, 16>)),
        M("GetScissorRect", dv_GetScissorRect), M("SetSoftwareVertexProcessing", dv_SetSoftwareVertexProcessing),
        M("GetSoftwareVertexProcessing", dv_GetSoftwareVertexProcessing), M("SetNPatchMode", (recordable<dv_SetNPatchMode, 2>)),
        U("GetNPatchMode"), M("DrawPrimitive", dv_DrawPrimitive), M("DrawIndexedPrimitive", dv_DrawIndexedPrimitive),
        M("DrawPrimitiveUP", dv_DrawPrimitiveUP), M("DrawIndexedPrimitiveUP", dv_DrawIndexedPrimitiveUP),
        U("ProcessVertices"), M("CreateVertexDeclaration", dv_CreateVertexDeclaration),
        M("SetVertexDeclaration", (recordable<dv_SetVertexDeclaration, 2, 1>)), M("GetVertexDeclaration", dv_GetVertexDeclaration),
        M("SetFVF", (recordable<dv_SetFVF, 2>)), M("GetFVF", dv_GetFVF), M("CreateVertexShader", dv_CreateVertexShader),
        M("SetVertexShader", (recordable<dv_SetVertexShader, 2, 1>)), M("GetVertexShader", dv_GetVertexShader),
        M("SetVertexShaderConstantF", (recordable<set_const_f<true>, 4, -1, 2, 0, 3, 16>)), M("GetVertexShaderConstantF", get_const_f<true>),
        M("SetVertexShaderConstantI", (recordable<set_const_i<true>, 4, -1, 2, 0, 3, 16>)), U("GetVertexShaderConstantI"),
        M("SetVertexShaderConstantB", (recordable<set_const_b<true>, 4, -1, 2, 0, 3, 4>)), U("GetVertexShaderConstantB"),
        M("SetStreamSource", (recordable<dv_SetStreamSource, 5, 2>)), M("GetStreamSource", dv_GetStreamSource),
        M("SetStreamSourceFreq", (recordable<dv_SetStreamSourceFreq, 3>)), U("GetStreamSourceFreq"),
        M("SetIndices", (recordable<dv_SetIndices, 2, 1>)), M("GetIndices", dv_GetIndices),
        M("CreatePixelShader", dv_CreatePixelShader), M("SetPixelShader", (recordable<dv_SetPixelShader, 2, 1>)),
        M("GetPixelShader", dv_GetPixelShader), M("SetPixelShaderConstantF", (recordable<set_const_f<false>, 4, -1, 2, 0, 3, 16>)),
        M("GetPixelShaderConstantF", get_const_f<false>), M("SetPixelShaderConstantI", (recordable<set_const_i<false>, 4, -1, 2, 0, 3, 16>)),
        U("GetPixelShaderConstantI"), M("SetPixelShaderConstantB", (recordable<set_const_b<false>, 4, -1, 2, 0, 3, 4>)),
        U("GetPixelShaderConstantB"), U("DrawRectPatch"), U("DrawTriPatch"), U("DeletePatch"),
        M("CreateQuery", dv_CreateQuery)}));
}

}  // namespace scimitar::win32::d3d9

namespace scimitar::win32::d3d9 {

// ---- D3DX: ID3DXConstantTable from a shader's CTAB comment ----

struct ConstantInfo {
    std::string name;
    uint32_t set = 2, reg = 0, count = 0, cls = 0, type = 3, rows = 1, cols = 4, elements = 1, members = 0;
    uint32_t bytes = 0, default_value = 0;  // guest address of the default, or 0
};

struct ConstantTable : Native {
    bool vertex = false;
    uint32_t blob = 0, blob_size = 0, creator = 0, version = 0;
    std::vector<ConstantInfo> constants;  // the table's, then array elements made on demand
    std::map<uint32_t, size_t> handles;   // handle -> index
    std::vector<uint32_t> handle_of;      // index -> handle
    uint32_t handle(size_t k) {
        while (handle_of.size() <= k) {
            const uint32_t h = galloc(4);
            handles[h] = handle_of.size();
            handle_of.push_back(h);
        }
        return handle_of[k];
    }
    ConstantInfo* find(uint32_t h) {
        if (!h) return nullptr;
        auto it = handles.find(h);
        if (it != handles.end()) return &constants[it->second];
        // A name string passed as a handle.
        const std::string n = gstr(h);
        for (size_t k = 0; k < constants.size(); k++)
            if (constants[k].name == n) return &constants[k];
        return nullptr;
    }
};

uint32_t g_vt_ctab = 0;

ConstantTable* table(Context& c) { return as<ConstantTable>(arg(c, 0)); }

void ct_GetBufferPointer(Context& c) { ret(c, table(c)->blob, 1); }
void ct_GetBufferSize(Context& c) { ret(c, table(c)->blob_size, 1); }

void ct_GetDesc(Context& c) {
    ConstantTable* t = table(c);
    const uint32_t out = arg(c, 1);
    uint32_t n = 0;
    for (const auto& k : t->constants)
        if (k.name.find('[') == std::string::npos) n++;
    wr(out, t->creator), wr(out + 4, t->version), wr(out + 8, n);
    ret(c, D3D_OK, 2);
}

void put_constant_desc(ConstantTable* t, const ConstantInfo& k, uint32_t out) {
    static std::map<std::string, uint32_t> names;
    auto it = names.find(k.name);
    if (it == names.end()) it = names.emplace(k.name, guest_strdup(k.name)).first;
    (void)t;
    const uint32_t v[12] = {it->second, k.set, k.reg, k.count, k.cls, k.type, k.rows, k.cols, k.elements,
                            k.members, k.bytes, k.default_value};
    for (int i = 0; i < 12; i++) wr(out + 4 * i, v[i]);
}

void ct_GetConstantDesc(Context& c) {
    // (handle, desc*, count*)
    ConstantTable* t = table(c);
    ConstantInfo* k = t->find(arg(c, 1));
    if (!k) return ret(c, D3DERR_INVALIDCALL, 4);
    if (arg(c, 2)) put_constant_desc(t, *k, arg(c, 2));
    if (arg(c, 3)) wr(arg(c, 3), 1);
    ret(c, D3D_OK, 4);
}

void ct_GetSamplerIndex(Context& c) {
    ConstantInfo* k = table(c)->find(arg(c, 1));
    ret(c, k && k->set == 3 ? k->reg : 0xffffffffu, 2);
}

void ct_GetConstant(Context& c) {
    // (parent, index): top-level constants only.
    ConstantTable* t = table(c);
    if (arg(c, 1)) return ret(c, 0, 3);
    uint32_t n = 0;
    for (size_t k = 0; k < t->constants.size(); k++) {
        if (t->constants[k].name.find('[') != std::string::npos) continue;
        if (n++ == arg(c, 2)) return ret(c, t->handle(k), 3);
    }
    ret(c, 0, 3);
}

void ct_GetConstantByName(Context& c) {
    ConstantTable* t = table(c);
    const std::string name = gstr(arg(c, 2));
    for (size_t k = 0; k < t->constants.size(); k++)
        if (t->constants[k].name == name) return ret(c, t->handle(k), 3);
    // "name[3]": an element of an array.
    const size_t br = name.find('[');
    if (br != std::string::npos) {
        const std::string base = name.substr(0, br);
        const uint32_t idx = (uint32_t)std::strtoul(name.c_str() + br + 1, nullptr, 10);
        for (size_t k = 0; k < t->constants.size(); k++)
            if (t->constants[k].name == base && idx < t->constants[k].elements) {
                ConstantInfo e = t->constants[k];
                const uint32_t per = std::max(1u, e.count / e.elements);
                e.name = name, e.reg += per * idx, e.count = per, e.elements = 1, e.bytes /= std::max(1u, t->constants[k].elements);
                t->constants.push_back(e);
                return ret(c, t->handle(t->constants.size() - 1), 3);
            }
    }
    ret(c, 0, 3);
}

void ct_GetConstantElement(Context& c) {
    ConstantTable* t = table(c);
    ConstantInfo* k = t->find(arg(c, 1));
    if (!k) return ret(c, 0, 3);
    const std::string name = k->name + "[" + std::to_string(arg(c, 2)) + "]";
    for (size_t j = 0; j < t->constants.size(); j++)
        if (t->constants[j].name == name) return ret(c, t->handle(j), 3);
    if (arg(c, 2) >= k->elements) return ret(c, 0, 3);
    ConstantInfo e = *k;
    const uint32_t per = std::max(1u, e.count / e.elements);
    e.name = name, e.reg += per * arg(c, 2), e.count = per, e.elements = 1;
    t->constants.push_back(e);
    ret(c, t->handle(t->constants.size() - 1), 3);
}

// Writes `n` float4 registers to the device for constant `k`, clamped to its count.
void write_regs(ConstantTable* t, uint32_t device, const ConstantInfo& k, const float* v, uint32_t n) {
    Device* d = as<Device>(device);
    if (!d) return;
    n = std::min(n, k.count);
    if (k.set == 2) {
        std::vector<float>& dst = t->vertex ? d->vs_const : d->ps_const;
        if ((k.reg + n) * 4 <= dst.size()) std::memcpy(&dst[k.reg * 4], v, 16 * n);
    } else if (k.set == 1) {
        int32_t* dst = t->vertex ? d->vs_int : d->ps_int;
        for (uint32_t i = 0; i < n * 4 && k.reg * 4 + i < 64; i++) dst[k.reg * 4 + i] = (int32_t)v[i];
    } else if (k.set == 0) {
        uint32_t* dst = t->vertex ? d->vs_bool : d->ps_bool;
        for (uint32_t i = 0; i < n && k.reg + i < 16; i++) dst[k.reg + i] = v[4 * i] != 0;
    }
}

// Scalars and vectors packed `cols` to a register; `count` values in all.
void set_values(Context& c, int nargs, const float* vals, uint32_t count) {
    ConstantTable* t = table(c);
    ConstantInfo* k = t->find(arg(c, 2));
    if (!k) return ret(c, D3DERR_INVALIDCALL, nargs);
    const uint32_t cols = k->cls == 1 || k->cls == 0 ? std::max(1u, k->cols) : 4;
    std::vector<float> regs(4 * std::max(1u, k->count), 0.0f);
    for (uint32_t i = 0; i < count && (i / cols) < k->count; i++) regs[4 * (i / cols) + i % cols] = vals[i];
    write_regs(t, arg(c, 1), *k, regs.data(), (count + cols - 1) / cols);
    ret(c, D3D_OK, nargs);
}

std::vector<float> floats(uint32_t p, uint32_t n) {
    std::vector<float> v(n);
    if (n) std::memcpy(v.data(), gptr(p), 4 * n);
    return v;
}

void ct_SetFloat(Context& c) { const float f = f32(arg(c, 3)); set_values(c, 4, &f, 1); }
void ct_SetFloatArray(Context& c) { auto v = floats(arg(c, 3), arg(c, 4)); set_values(c, 5, v.data(), arg(c, 4)); }
void ct_SetInt(Context& c) { const float f = (float)(int32_t)arg(c, 3); set_values(c, 4, &f, 1); }
void ct_SetIntArray(Context& c) {
    std::vector<float> v(arg(c, 4));
    for (uint32_t i = 0; i < v.size(); i++) v[i] = (float)(int32_t)rd(arg(c, 3) + 4 * i);
    set_values(c, 5, v.data(), (uint32_t)v.size());
}
void ct_SetBool(Context& c) { const float f = arg(c, 3) ? 1.0f : 0.0f; set_values(c, 4, &f, 1); }
void ct_SetBoolArray(Context& c) {
    std::vector<float> v(arg(c, 4));
    for (uint32_t i = 0; i < v.size(); i++) v[i] = rd(arg(c, 3) + 4 * i) ? 1.0f : 0.0f;
    set_values(c, 5, v.data(), (uint32_t)v.size());
}

void ct_SetVector(Context& c) {
    ConstantTable* t = table(c);
    ConstantInfo* k = t->find(arg(c, 2));
    if (!k) return ret(c, D3DERR_INVALIDCALL, 4);
    auto v = floats(arg(c, 3), 4);
    write_regs(t, arg(c, 1), *k, v.data(), 1);
    ret(c, D3D_OK, 4);
}

void ct_SetVectorArray(Context& c) {
    ConstantTable* t = table(c);
    ConstantInfo* k = t->find(arg(c, 2));
    if (!k) return ret(c, D3DERR_INVALIDCALL, 5);
    auto v = floats(arg(c, 3), 4 * arg(c, 4));
    write_regs(t, arg(c, 1), *k, v.data(), arg(c, 4));
    ret(c, D3D_OK, 5);
}

// Matrices: D3DXMATRIX is row-major; a MATRIX_ROWS constant takes a row per
// register, MATRIX_COLUMNS a column per register (rows/cols of the constant).
void put_matrix(const ConstantInfo& k, const float* m, bool transpose, float* regs) {
    const bool by_column = (k.cls == 3) != transpose;
    const uint32_t nreg = by_column ? k.cols : k.rows;
    const uint32_t per = by_column ? k.rows : k.cols;
    for (uint32_t r = 0; r < nreg && r < 4; r++)
        for (uint32_t i = 0; i < per && i < 4; i++) regs[4 * r + i] = by_column ? m[4 * i + r] : m[4 * r + i];
}

void set_matrices(Context& c, int nargs, uint32_t count, bool transpose, bool pointers) {
    ConstantTable* t = table(c);
    ConstantInfo* k = t->find(arg(c, 2));
    if (!k) return ret(c, D3DERR_INVALIDCALL, nargs);
    const uint32_t nreg = (k->cls == 3) != transpose ? k->cols : k->rows;
    std::vector<float> regs(4 * std::max(1u, nreg) * count, 0.0f);
    for (uint32_t e = 0; e < count; e++) {
        const uint32_t src = pointers ? rd(arg(c, 3) + 4 * e) : arg(c, 3) + 64 * e;
        auto m = floats(src, 16);
        put_matrix(*k, m.data(), transpose, &regs[4 * nreg * e]);
    }
    write_regs(t, arg(c, 1), *k, regs.data(), nreg * count);
    ret(c, D3D_OK, nargs);
}

void ct_SetMatrix(Context& c) { set_matrices(c, 4, 1, false, false); }
void ct_SetMatrixArray(Context& c) { set_matrices(c, 5, arg(c, 4), false, false); }
void ct_SetMatrixPointerArray(Context& c) { set_matrices(c, 5, arg(c, 4), false, true); }
void ct_SetMatrixTranspose(Context& c) { set_matrices(c, 4, 1, true, false); }
void ct_SetMatrixTransposeArray(Context& c) { set_matrices(c, 5, arg(c, 4), true, false); }
void ct_SetMatrixTransposePointerArray(Context& c) { set_matrices(c, 5, arg(c, 4), true, true); }

void ct_SetValue(Context& c) {
    // (device, handle, data, bytes): floats as the constant's registers hold them.
    ConstantTable* t = table(c);
    ConstantInfo* k = t->find(arg(c, 2));
    if (!k) return ret(c, D3DERR_INVALIDCALL, 5);
    auto v = floats(arg(c, 3), arg(c, 4) / 4);
    if (k->cls == 2 || k->cls == 3) {
        std::vector<float> regs(4 * 4, 0.0f);
        if (v.size() >= 16) put_matrix(*k, v.data(), false, regs.data());
        write_regs(t, arg(c, 1), *k, regs.data(), k->cls == 3 ? k->cols : k->rows);
        return ret(c, D3D_OK, 5);
    }
    set_values(c, 5, v.data(), (uint32_t)v.size());
}

void ct_SetDefaults(Context& c) {
    ConstantTable* t = table(c);
    for (const ConstantInfo& k : t->constants)
        if (k.default_value && k.name.find('[') == std::string::npos) {
            auto v = floats(k.default_value, 4 * k.count);
            write_regs(t, arg(c, 1), k, v.data(), k.count);
        }
    ret(c, D3D_OK, 2);
}

uint32_t make_constant_table(uint32_t function) {
    // Finds the CTAB comment in the bytecode.
    uint32_t p = function + 4;
    for (int guard = 0; guard < 4096; guard++) {
        const uint32_t tok = rd(p);
        if (tok == 0x0000ffff) return 0;
        if ((tok & 0xffff) == 0xfffe) {
            const uint32_t n = tok >> 16 & 0x7fff;
            if (n >= 1 && rd(p + 4) == 0x42415443) {  // "CTAB"
                auto* t = new ConstantTable;
                t->vertex = (rd(function) & 0xffff0000u) == 0xfffe0000u;
                t->blob_size = 4 * (n - 1);
                t->blob = galloc(t->blob_size + 4);
                std::memcpy(gptr(t->blob), gptr(p + 8), t->blob_size);
                const uint32_t b = t->blob;
                t->creator = rd(b + 4) ? b + rd(b + 4) : 0;
                t->version = rd(b + 8);
                const uint32_t count = rd(b + 12), info = rd(b + 16);
                for (uint32_t k = 0; k < count; k++) {
                    const uint32_t e = b + info + 20 * k;
                    ConstantInfo ci;
                    ci.name = gstr(b + rd(e));
                    ci.set = rd16(e + 4), ci.reg = rd16(e + 6), ci.count = rd16(e + 8);
                    const uint32_t ty = b + rd(e + 12);
                    ci.cls = rd16(ty), ci.type = rd16(ty + 2), ci.rows = rd16(ty + 4), ci.cols = rd16(ty + 6);
                    ci.elements = std::max<uint32_t>(1, rd16(ty + 8)), ci.members = rd16(ty + 10);
                    ci.bytes = ci.set == 3 ? 4 : 4 * ci.rows * ci.cols * ci.elements;
                    ci.default_value = rd(e + 16) ? b + rd(e + 16) : 0;
                    t->constants.push_back(ci);
                }
                return create(g_vt_ctab, t);
            }
            p += 4 * (n + 1);
            continue;
        }
        return 0;  // instructions: no table before them
    }
    return 0;
}

}  // namespace scimitar::win32::d3d9

namespace scimitar::imports {
using namespace scimitar::win32;
using namespace scimitar::win32::d3d9;

W32(d3dx9_36, D3DXCheckTextureRequirements) { ret(c, D3D_OK, 7); }

W32(d3dx9_36, D3DXGetShaderConstantTable) {
    // (function, out)
    if (!g_vt_ctab)
        g_vt_ctab = make_vtable("ID3DXConstantTable", {
            M("QueryInterface", com::QueryInterface), M("AddRef", com::AddRef), M("Release", com::Release),
            M("GetBufferPointer", ct_GetBufferPointer), M("GetBufferSize", ct_GetBufferSize),
            M("GetDesc", ct_GetDesc), M("GetConstantDesc", ct_GetConstantDesc),
            M("GetSamplerIndex", ct_GetSamplerIndex), M("GetConstant", ct_GetConstant),
            M("GetConstantByName", ct_GetConstantByName), M("GetConstantElement", ct_GetConstantElement),
            M("SetDefaults", ct_SetDefaults), M("SetValue", ct_SetValue), M("SetBool", ct_SetBool),
            M("SetBoolArray", ct_SetBoolArray), M("SetInt", ct_SetInt), M("SetIntArray", ct_SetIntArray),
            M("SetFloat", ct_SetFloat), M("SetFloatArray", ct_SetFloatArray), M("SetVector", ct_SetVector),
            M("SetVectorArray", ct_SetVectorArray), M("SetMatrix", ct_SetMatrix),
            M("SetMatrixArray", ct_SetMatrixArray), M("SetMatrixPointerArray", ct_SetMatrixPointerArray),
            M("SetMatrixTranspose", ct_SetMatrixTranspose),
            M("SetMatrixTransposeArray", ct_SetMatrixTransposeArray),
            M("SetMatrixTransposePointerArray", ct_SetMatrixTransposePointerArray)});
    const uint32_t t = make_constant_table(arg(c, 0));
    wr(arg(c, 1), t);
    ret(c, t ? D3D_OK : D3DERR_INVALIDCALL, 2);
}

W32(d3d9, Direct3DCreate9) {
    trace("Direct3DCreate9(%u)", arg(c, 0));
    static uint32_t vt = make_vtable("IDirect3D9", {
        M("QueryInterface", com::QueryInterface), M("AddRef", com::AddRef), M("Release", com::Release),
        U("RegisterSoftwareDevice"), M("GetAdapterCount", d3d_GetAdapterCount),
        M("GetAdapterIdentifier", d3d_GetAdapterIdentifier), M("GetAdapterModeCount", d3d_GetAdapterModeCount),
        M("EnumAdapterModes", d3d_EnumAdapterModes), M("GetAdapterDisplayMode", d3d_GetAdapterDisplayMode),
        M("CheckDeviceType", d3d_CheckDeviceType), M("CheckDeviceFormat", d3d_CheckDeviceFormat),
        M("CheckDeviceMultiSampleType", d3d_CheckDeviceMultiSampleType),
        M("CheckDepthStencilMatch", d3d_CheckDepthStencilMatch),
        M("CheckDeviceFormatConversion", d3d_CheckDeviceFormatConversion), M("GetDeviceCaps", d3d_GetDeviceCaps),
        M("GetAdapterMonitor", d3d_GetAdapterMonitor), M("CreateDevice", d3d_CreateDevice)});
    ret(c, create(vt, new D3D), 1);
}

}  // namespace scimitar::imports
