// Frame dumps for debugging the Metal backend: F13 (or
// scimitar::metal::request_dump) makes the next frame write, to
// /tmp/ac2dump/<time>/, every render pass's colour and depth as it ends
// (pass_NN_color.ppm, pass_NN_depth.pgm, scaled to the depth range found) and
// frame.txt: each pass's attachments and each draw's programs (by shader source
// file or program name), textures, constants and state, with value ranges of
// float targets (exposure and luminance passes).
#include "backend.h"

#include <sys/stat.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "scimitar/metal_gl.h"

namespace scimitar::metal {

namespace {

std::mutex g_dump_lock;
std::string g_dump_result;  // set by the render thread, taken by the shell

std::string fmt_name(MTLPixelFormat f) {
    switch (f) {
    case MTLPixelFormatBGRA8Unorm: return "BGRA8";
    case MTLPixelFormatRGBA8Unorm: return "RGBA8";
    case MTLPixelFormatRGBA8Unorm_sRGB: return "RGBA8_sRGB";
    case MTLPixelFormatRGBA16Float: return "RGBA16F";
    case MTLPixelFormatRGBA32Float: return "RGBA32F";
    case MTLPixelFormatR16Float: return "R16F";
    case MTLPixelFormatR32Float: return "R32F";
    case MTLPixelFormatRG16Float: return "RG16F";
    case MTLPixelFormatRG32Float: return "RG32F";
    case MTLPixelFormatR8Unorm: return "R8";
    case MTLPixelFormatRG8Unorm: return "RG8";
    case MTLPixelFormatRGB10A2Unorm: return "RGB10A2";
    case MTLPixelFormatRGBA16Unorm: return "RGBA16";
    case MTLPixelFormatDepth32Float: return "D32F";
    case MTLPixelFormatDepth32Float_Stencil8: return "D32F_S8";
    case MTLPixelFormatBC1_RGBA: return "BC1";
    case MTLPixelFormatBC2_RGBA: return "BC2";
    case MTLPixelFormatBC3_RGBA: return "BC3";
    default: return "fmt" + std::to_string((int)f);
    }
}

std::string tex_desc(id<MTLTexture> t) {
    if (!t) return "-";
    char b[160];
    std::snprintf(b, sizeof b, "%p %s %lux%lu%s%s", (__bridge void*)t, fmt_name(t.pixelFormat).c_str(), (unsigned long)t.width,
                  (unsigned long)t.height, t.textureType == MTLTextureTypeCube ? " cube" : t.textureType == MTLTextureType3D ? " 3d" : "",
                  t.parentTexture ? " (view)" : "");
    return b;
}

// RGBA floats of one texel of a colour format.
bool texel(MTLPixelFormat f, const uint8_t* p, float out[4]) {
    switch (f) {
    case MTLPixelFormatBGRA8Unorm: out[0] = p[2] / 255.f, out[1] = p[1] / 255.f, out[2] = p[0] / 255.f, out[3] = p[3] / 255.f; return true;
    case MTLPixelFormatRGBA8Unorm: case MTLPixelFormatRGBA8Unorm_sRGB:
        for (int k = 0; k < 4; k++) out[k] = p[k] / 255.f;
        return true;
    case MTLPixelFormatRGBA16Float:
        for (int k = 0; k < 4; k++) out[k] = (float)((const __fp16*)p)[k];
        return true;
    case MTLPixelFormatRGBA32Float: std::memcpy(out, p, 16); return true;
    case MTLPixelFormatR16Float: out[0] = out[1] = out[2] = (float)*(const __fp16*)p, out[3] = 1; return true;
    case MTLPixelFormatR32Float: out[0] = out[1] = out[2] = *(const float*)p, out[3] = 1; return true;
    case MTLPixelFormatRG16Float: out[0] = (float)((const __fp16*)p)[0], out[1] = (float)((const __fp16*)p)[1], out[2] = 0, out[3] = 1; return true;
    case MTLPixelFormatR8Unorm: out[0] = out[1] = out[2] = p[0] / 255.f, out[3] = 1; return true;
    case MTLPixelFormatRGB10A2Unorm: {
        const uint32_t v = *(const uint32_t*)p;
        out[0] = (v & 1023) / 1023.f, out[1] = (v >> 10 & 1023) / 1023.f, out[2] = (v >> 20 & 1023) / 1023.f, out[3] = (v >> 30) / 3.f;
        return true;
    }
    default: return false;
    }
}

uint32_t bytes_per_pixel(MTLPixelFormat f) {
    switch (f) {
    case MTLPixelFormatRGBA16Float: case MTLPixelFormatRG32Float: case MTLPixelFormatRGBA16Unorm: return 8;
    case MTLPixelFormatRGBA32Float: return 16;
    case MTLPixelFormatR16Float: case MTLPixelFormatRG8Unorm: return 2;
    case MTLPixelFormatR8Unorm: return 1;
    default: return 4;
    }
}

}  // namespace

void Backend::dump_begin_frame() {
    char stamp[32];
    const std::time_t now = std::time(nullptr);
    std::strftime(stamp, sizeof stamp, "%H%M%S", std::localtime(&now));
#if TARGET_OS_IPHONE  // no /tmp: the app's temporary folder (devicectl copies it from the container)
    const std::string base = std::string(NSTemporaryDirectory().UTF8String) + "ac2dump";
#else
    const std::string base = "/tmp/ac2dump";
#endif
    mkdir(base.c_str(), 0755);
    dump_dir_ = base + "/" + stamp;
    mkdir(dump_dir_.c_str(), 0755);
    dump_log_ = std::fopen((dump_dir_ + "/frame.txt").c_str(), "w");
    if (!dump_log_) return;
    dumping_ = true;
    dump_pass_ = 0, dump_draw_ = 0;
    dump_images_.clear();
    std::fprintf(stderr, "metal: dumping frame %llu to %s\n", (unsigned long long)frame_, dump_dir_.c_str());
    std::fprintf(dump_log_, "frame %llu, default framebuffer %dx%d\n", (unsigned long long)frame_, default_w_, default_h_);
}

void Backend::dump_pass_begun(const PassTargets& t, MTLRenderPassDescriptor* rp) {
    static const char* const kLoad[] = {"dontcare", "load", "clear"};
    std::fprintf(dump_log_, "\npass %02d: color %s level %d slice %d (%s), depth %s (%s, clear %.3f), stencil %s\n", dump_pass_,
                 tex_desc(t.color).c_str(), t.color_level, t.color_slice, t.color ? kLoad[rp.colorAttachments[0].loadAction] : "-",
                 tex_desc(t.depth).c_str(), t.depth ? kLoad[rp.depthAttachment.loadAction] : "-", rp.depthAttachment.clearDepth,
                 tex_desc(t.stencil).c_str());
    if (t.color == default_color_) std::fprintf(dump_log_, "  (the default framebuffer)\n");
    // The framebuffer's attachments as the game set them.
    auto fb = framebuffers.find(st.draw_fb);
    if (st.draw_fb && fb != framebuffers.end()) {
        auto describe = [&](const char* what, const Attachment& a) {
            if (!a.kind) return;
            std::string state = "missing";
            if (a.kind == 1) {
                auto it = textures.find(a.name);
                if (it != textures.end()) state = it->second.tex ? tex_desc(it->second.tex) : "no storage";
            } else {
                auto it = renderbuffers.find(a.name);
                if (it != renderbuffers.end()) state = it->second.tex ? tex_desc(it->second.tex) : "no storage";
            }
            std::fprintf(dump_log_, "  fb %u %s: %s %u level %d slice %d -> %s\n", st.draw_fb, what,
                         a.kind == 1 ? "texture" : "renderbuffer", a.name, a.level, a.slice, state.c_str());
        };
        const Framebuffer& f = fb->second;
        for (int k = 0; k < 4; k++) describe(("color" + std::to_string(k)).c_str(), f.color[k]);
        describe("depth", f.depth);
        describe("stencil", f.stencil);
    }
}

// The pass's attachments as they are now, copied for writing after the frame.
void Backend::dump_pass_ending() {
    id<MTLBlitCommandEncoder> bl = [cb_ blitCommandEncoder];
    DumpImage img;
    char name[32];
    std::snprintf(name, sizeof name, "pass_%02d", dump_pass_);
    img.name = name;
    if (id<MTLTexture> c = pass_.color) {
        const int w = std::max(1, (int)c.width >> pass_.color_level), h = std::max(1, (int)c.height >> pass_.color_level);
        MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:c.pixelFormat width:w height:h mipmapped:NO];
        td.storageMode = MTLStorageModeShared;
        img.color = [dev newTextureWithDescriptor:td];
        const NSUInteger slice = c.textureType == MTLTextureType3D ? 0 : pass_.color_slice;
        [bl copyFromTexture:c sourceSlice:slice sourceLevel:pass_.color_level sourceOrigin:MTLOriginMake(0, 0, 0)
                 sourceSize:MTLSizeMake(w, h, 1) toTexture:img.color destinationSlice:0 destinationLevel:0
          destinationOrigin:MTLOriginMake(0, 0, 0)];
        img.w = w, img.h = h;
    }
    if (id<MTLTexture> d = pass_.depth) {
        const int w = (int)d.width, h = (int)d.height;
        img.depth = [dev newBufferWithLength:(size_t)w * h * 4 options:MTLResourceStorageModeShared];
        img.dw = w, img.dh = h;
        [bl copyFromTexture:d sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, 1)
                   toBuffer:img.depth destinationOffset:0 destinationBytesPerRow:(size_t)w * 4 destinationBytesPerImage:(size_t)w * h * 4
                    options:d.pixelFormat == MTLPixelFormatDepth32Float_Stencil8 ? MTLBlitOptionDepthFromDepthStencil : MTLBlitOptionNone];
    }
    [bl endEncoding];
    dump_images_.push_back(img);
    dump_pass_++;
}

void Backend::dump_draw(GLenum mode, int count, bool indexed, const Program& vp, const Program& fp, const arb::Program& fbase,
                        const id<MTLTexture>* bound) {
    // Each program's text once, as programs/<label>.txt.
    static std::set<std::string> written;
    if (dump_draw_ == 0) written.clear();
    for (const Program* p : {&vp, &fp})
        if (written.insert(p->label).second) {
            mkdir((dump_dir_ + "/programs").c_str(), 0755);
            if (FILE* fo = std::fopen((dump_dir_ + "/programs/" + p->label + ".txt").c_str(), "w")) {
                std::fwrite(p->text.data(), 1, p->text.size(), fo);
                std::fclose(fo);
            }
        }
    std::fprintf(dump_log_, "  draw %d: %s %d%s  vp %s  fp %s\n", dump_draw_++, mode == GL_TRIANGLES ? "tri" : "mode", count,
                 indexed ? " indexed" : "", vp.label.c_str(), fp.label.c_str());
    std::fprintf(dump_log_, "    viewport %d %d %d %d  blend %s %x %x eq %x  depth %s %x write %d  stencil %d  alpha %s %x %.3f  mask %x  cull %d\n",
                 st.viewport[0], st.viewport[1], st.viewport[2], st.viewport[3], st.blend ? "on" : "off", st.blend_src,
                 st.blend_dst, st.blend_eq, st.depth_test ? "on" : "off", st.depth_func, st.depth_mask, st.stencil_test,
                 st.alpha_test ? "on" : "off", st.alpha_func, st.alpha_ref, st.color_mask, st.cull);
    for (int u = 0; u < kUnits; u++) {
        if (fbase.textures[u] == arb::TexTarget::None) continue;
        // The GL texture bound for the program's target, and what it holds.
        const TexUnit& tu = st.units[u];
        GLuint name = tu.t2d;
        GLenum want = GL_TEXTURE_2D;
        switch (fbase.textures[u]) {
        case arb::TexTarget::Cube: name = tu.cube, want = GL_TEXTURE_CUBE_MAP; break;
        case arb::TexTarget::T3D: name = tu.t3d, want = GL_TEXTURE_3D; break;
        case arb::TexTarget::T1D: name = tu.t1d, want = GL_TEXTURE_1D; break;
        case arb::TexTarget::Rect: name = tu.rect, want = GL_TEXTURE_RECTANGLE_ARB; break;
        default: break;
        }
        std::string gl = "GL texture " + std::to_string(name);
        auto it = textures.find(name);
        if (!name) gl += " (none bound)";
        else if (it == textures.end()) gl += " (unknown name)";
        else if (!it->second.tex) gl += " (no storage)";
        else if (it->second.target != want) gl += " (target " + std::to_string(it->second.target) + ", wanted " + std::to_string(want) + ")";
        std::fprintf(dump_log_, "    unit %d: %s  [%s]\n", u, tex_desc(bound[u]).c_str(), gl.c_str());
        if (it != textures.end() && it->second.tex) dump_textures_[name] = it->second.tex;
    }
    // The fragment program's constants, as gathered.
    for (size_t k = 0; k < fbase.params.size() && k < 12; k++) {
        const arb::ParamRef& r = fbase.params[k];
        const float* v = r.value;
        if (r.kind == arb::ParamRef::Env) v = env[1][r.index];
        else if (r.kind == arb::ParamRef::Local) v = r.index < fp.locals.size() ? fp.locals[r.index].data() : r.value;
        std::fprintf(dump_log_, "    P[%zu] %s[%u] = %g %g %g %g\n", k, r.kind == arb::ParamRef::Env ? "env" : r.kind == arb::ParamRef::Local ? "local" : "lit",
                     r.index, v[0], v[1], v[2], v[3]);
    }
}

// Every texture the frame sampled, small: textures/tex_<GL name>_<w>x<h>_<format>.ppm,
// the mip level nearest 128 pixels, decoded to RGB (BC included).
void Backend::dump_textures() {
    mkdir((dump_dir_ + "/textures").c_str(), 0755);
    for (const auto& [name, t] : dump_textures_) {
        if (t.storageMode == MTLStorageModePrivate || t.textureType != MTLTextureType2D) continue;
        NSUInteger level = 0;
        while (level + 1 < t.mipmapLevelCount && std::max(t.width >> level, t.height >> level) > 128) level++;
        const int w = (int)std::max<NSUInteger>(1, t.width >> level), h = (int)std::max<NSUInteger>(1, t.height >> level);
        const MTLPixelFormat f = t.pixelFormat;
        std::vector<uint8_t> rgba((size_t)w * h * 4);
        const int bc = f == MTLPixelFormatBC1_RGBA ? 1 : f == MTLPixelFormatBC2_RGBA ? 2 : f == MTLPixelFormatBC3_RGBA ? 3 : 0;
        if (bc) {
            const size_t row = (size_t)((w + 3) / 4) * (bc == 1 ? 8 : 16), rows = (size_t)(h + 3) / 4;
            std::vector<uint8_t> blocks(row * rows);
            [t getBytes:blocks.data() bytesPerRow:row fromRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:level];
            decode_bc_image(bc, blocks.data(), w, h, rgba.data());
        } else if (f == MTLPixelFormatRGBA8Unorm || f == MTLPixelFormatBGRA8Unorm || f == MTLPixelFormatRGBA8Unorm_sRGB) {
            [t getBytes:rgba.data() bytesPerRow:(size_t)w * 4 fromRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:level];
            if (f == MTLPixelFormatBGRA8Unorm)
                for (size_t k = 0; k < rgba.size(); k += 4) std::swap(rgba[k], rgba[k + 2]);
        } else {
            continue;
        }
        char path[512];
        std::snprintf(path, sizeof path, "%s/textures/tex_%u_%dx%d_%s.ppm", dump_dir_.c_str(), name, (int)t.width,
                      (int)t.height, fmt_name(f).c_str());
        // Larger without mip levels (render targets): every step-th texel.
        const int step = std::max(1, (std::max(w, h) + 127) / 128), ow = (w + step - 1) / step, oh = (h + step - 1) / step;
        if (FILE* fo = std::fopen(path, "wb")) {
            std::fprintf(fo, "P6\n%d %d\n255\n", ow, oh);
            for (int y = 0; y < oh; y++)
                for (int x = 0; x < ow; x++) std::fwrite(&rgba[((size_t)y * step * w + (size_t)x * step) * 4], 1, 3, fo);
            std::fclose(fo);
        }
    }
    dump_textures_.clear();
}

void Backend::dump_end_frame() {
    dumping_ = false;
    flush_and_wait();
    for (const DumpImage& img : dump_images_) {
        if (img.color) {
            const MTLPixelFormat f = img.color.pixelFormat;
            const uint32_t bpp = bytes_per_pixel(f);
            std::vector<uint8_t> px((size_t)img.w * img.h * bpp);
            [img.color getBytes:px.data() bytesPerRow:(size_t)img.w * bpp fromRegion:MTLRegionMake2D(0, 0, img.w, img.h) mipmapLevel:0];
            std::vector<uint8_t> rgb((size_t)img.w * img.h * 3);
            float lo[4] = {INFINITY, INFINITY, INFINITY, INFINITY}, hi[4] = {-INFINITY, -INFINITY, -INFINITY, -INFINITY}, sum[4] = {};
            bool ok = true;
            // Rows bottom-up (GL's order): written top row first.
            for (int y = 0; y < img.h && ok; y++)
                for (int x = 0; x < img.w; x++) {
                    float c[4];
                    if (!(ok = texel(f, &px[((size_t)y * img.w + x) * bpp], c))) break;
                    for (int k = 0; k < 4; k++) lo[k] = std::min(lo[k], c[k]), hi[k] = std::max(hi[k], c[k]), sum[k] += c[k];
                    uint8_t* o = &rgb[((size_t)(img.h - 1 - y) * img.w + x) * 3];
                    for (int k = 0; k < 3; k++) o[k] = (uint8_t)std::lround(std::clamp(c[k], 0.0f, 1.0f) * 255);
                }
            if (ok) {
                if (FILE* fo = std::fopen((dump_dir_ + "/" + img.name + "_color.ppm").c_str(), "wb")) {
                    std::fprintf(fo, "P6\n%d %d\n255\n", img.w, img.h);
                    std::fwrite(rgb.data(), 1, rgb.size(), fo);
                    std::fclose(fo);
                }
                const double n = (double)img.w * img.h;
                std::fprintf(dump_log_, "\n%s color %dx%d %s: min %g %g %g %g  max %g %g %g %g  mean %g %g %g %g\n", img.name.c_str(), img.w,
                             img.h, fmt_name(f).c_str(), lo[0], lo[1], lo[2], lo[3], hi[0], hi[1], hi[2], hi[3], sum[0] / n,
                             sum[1] / n, sum[2] / n, sum[3] / n);
            } else {
                std::fprintf(dump_log_, "\n%s color: format %s not written\n", img.name.c_str(), fmt_name(f).c_str());
            }
        }
        if (img.depth) {
            const float* d = (const float*)img.depth.contents;
            const int dw = img.dw, dh = img.dh;
            const size_t n = (size_t)dw * dh;
            float lo = INFINITY, hi = -INFINITY;
            for (size_t k = 0; k < n; k++) lo = std::min(lo, d[k]), hi = std::max(hi, d[k]);
            std::vector<uint8_t> g(n);
            for (int y = 0; y < dh; y++)
                for (int x = 0; x < dw; x++)
                    g[(size_t)(dh - 1 - y) * dw + x] = (uint8_t)std::lround(hi > lo ? (d[(size_t)y * dw + x] - lo) / (hi - lo) * 255 : 0);
            if (FILE* fo = std::fopen((dump_dir_ + "/" + img.name + "_depth.pgm").c_str(), "wb")) {
                std::fprintf(fo, "P5\n%d %d\n255\n", dw, dh);
                std::fwrite(g.data(), 1, g.size(), fo);
                std::fclose(fo);
            }
            std::fprintf(dump_log_, "%s depth %dx%d: min %g max %g\n", img.name.c_str(), dw, dh, lo, hi);
        }
    }
    std::fclose(dump_log_);
    dump_log_ = nullptr;
    dump_images_.clear();
    dump_textures();
    std::fprintf(stderr, "metal: frame dumped to %s\n", dump_dir_.c_str());
    std::lock_guard<std::mutex> l(g_dump_lock);
    g_dump_result = dump_dir_;
}

void request_dump() { backend().dump_requested = true; }

std::string take_dump_result() {
    std::lock_guard<std::mutex> l(g_dump_lock);
    std::string r;
    r.swap(g_dump_result);
    return r;
}

}  // namespace scimitar::metal
