// The Metal backend's core (docs/METAL.md): memory, objects, render passes,
// pipelines, draws, queries and presentation. GL entry points arrive from
// shims.mm with host arguments.
#include "backend.h"
#include "shader_cache.h"

#include <chrono>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "scimitar/guest_heap.h"
#include "scimitar/runtime.h"

namespace scimitar::metal {

namespace {

bool verbose() {
    static const bool v = std::getenv("SCIMITAR_VERBOSE") || std::getenv("SCIMITAR_TRACE_GL");
    return v;
}

// Logs a message once per distinct text.
void warn_once(const std::string& m) {
    static std::map<std::string, int> seen;
    if (seen[m]++ == 0) std::fprintf(stderr, "metal: %s\n", m.c_str());
}

// Whether the GPU samples BC (DXT) textures: every Mac does, iPhones and iPads
// from the A17 Pro and M1 on. Without them the backend decodes DXT on upload.
// SCIMITAR_NO_BC=1 decodes anyway (to test that path).
bool g_bc_textures = true;

std::string hex(uint32_t v) {
    char b[16];
    std::snprintf(b, sizeof b, "0x%x", v);
    return b;
}

MTLCompareFunction compare(GLenum f) {
    switch (f) {
    case GL_NEVER: return MTLCompareFunctionNever;
    case GL_LESS: return MTLCompareFunctionLess;
    case GL_EQUAL: return MTLCompareFunctionEqual;
    case GL_LEQUAL: return MTLCompareFunctionLessEqual;
    case GL_GREATER: return MTLCompareFunctionGreater;
    case GL_NOTEQUAL: return MTLCompareFunctionNotEqual;
    case GL_GEQUAL: return MTLCompareFunctionGreaterEqual;
    default: return MTLCompareFunctionAlways;
    }
}

MTLStencilOperation stencil_op(GLenum op) {
    switch (op) {
    case GL_ZERO: return MTLStencilOperationZero;
    case GL_REPLACE: return MTLStencilOperationReplace;
    case GL_INCR: return MTLStencilOperationIncrementClamp;
    case GL_DECR: return MTLStencilOperationDecrementClamp;
    case GL_INVERT: return MTLStencilOperationInvert;
    case GL_INCR_WRAP: return MTLStencilOperationIncrementWrap;
    case GL_DECR_WRAP: return MTLStencilOperationDecrementWrap;
    default: return MTLStencilOperationKeep;
    }
}

MTLBlendFactor blend_factor(GLenum f) {
    switch (f) {
    case GL_ZERO: return MTLBlendFactorZero;
    case GL_ONE: return MTLBlendFactorOne;
    case GL_SRC_COLOR: return MTLBlendFactorSourceColor;
    case GL_ONE_MINUS_SRC_COLOR: return MTLBlendFactorOneMinusSourceColor;
    case GL_SRC_ALPHA: return MTLBlendFactorSourceAlpha;
    case GL_ONE_MINUS_SRC_ALPHA: return MTLBlendFactorOneMinusSourceAlpha;
    case GL_DST_ALPHA: return MTLBlendFactorDestinationAlpha;
    case GL_ONE_MINUS_DST_ALPHA: return MTLBlendFactorOneMinusDestinationAlpha;
    case GL_DST_COLOR: return MTLBlendFactorDestinationColor;
    case GL_ONE_MINUS_DST_COLOR: return MTLBlendFactorOneMinusDestinationColor;
    case GL_SRC_ALPHA_SATURATE: return MTLBlendFactorSourceAlphaSaturated;
    case GL_CONSTANT_COLOR: return MTLBlendFactorBlendColor;
    case GL_ONE_MINUS_CONSTANT_COLOR: return MTLBlendFactorOneMinusBlendColor;
    case GL_CONSTANT_ALPHA: return MTLBlendFactorBlendAlpha;
    case GL_ONE_MINUS_CONSTANT_ALPHA: return MTLBlendFactorOneMinusBlendAlpha;
    default: return MTLBlendFactorOne;
    }
}

MTLBlendOperation blend_op(GLenum e) {
    switch (e) {
    case GL_FUNC_SUBTRACT: return MTLBlendOperationSubtract;
    case GL_FUNC_REVERSE_SUBTRACT: return MTLBlendOperationReverseSubtract;
    case GL_MIN: return MTLBlendOperationMin;
    case GL_MAX: return MTLBlendOperationMax;
    default: return MTLBlendOperationAdd;
    }
}

MTLSamplerAddressMode address(GLenum w) {
    switch (w) {
    case GL_CLAMP:
    case GL_CLAMP_TO_EDGE: return MTLSamplerAddressModeClampToEdge;
    case GL_CLAMP_TO_BORDER: return MTLSamplerAddressModeClampToBorderColor;
    case GL_MIRRORED_REPEAT: return MTLSamplerAddressModeMirrorRepeat;
    default: return MTLSamplerAddressModeRepeat;
    }
}

uint32_t type_size(GLenum type) {
    switch (type) {
    case GL_BYTE: case GL_UNSIGNED_BYTE: return 1;
    case GL_SHORT: case GL_UNSIGNED_SHORT: case GL_HALF_FLOAT_ARB: return 2;
    default: return 4;
    }
}

// The Metal vertex format for a GL attribute array; integer formats (not
// normalized) are read as integers by the shader.
MTLVertexFormat vertex_format(const VertexAttrib& a, bool* integer, bool* is_signed) {
    *integer = false, *is_signed = false;
    const uint32_t n = a.size == GL_BGRA ? 4 : std::clamp<uint32_t>(a.size, 1, 4);
    if (a.size == GL_BGRA) return MTLVertexFormatUChar4Normalized_BGRA;
#define PICK(F1, F2, F3, F4) return n == 1 ? F1 : n == 2 ? F2 : n == 3 ? F3 : F4
    switch (a.type) {
    case GL_FLOAT: PICK(MTLVertexFormatFloat, MTLVertexFormatFloat2, MTLVertexFormatFloat3, MTLVertexFormatFloat4);
    case GL_HALF_FLOAT_ARB: PICK(MTLVertexFormatHalf, MTLVertexFormatHalf2, MTLVertexFormatHalf3, MTLVertexFormatHalf4);
    case GL_UNSIGNED_BYTE:
        if (a.normalized)
            PICK(MTLVertexFormatUCharNormalized, MTLVertexFormatUChar2Normalized, MTLVertexFormatUChar3Normalized,
                 MTLVertexFormatUChar4Normalized);
        *integer = true;
        PICK(MTLVertexFormatUChar, MTLVertexFormatUChar2, MTLVertexFormatUChar3, MTLVertexFormatUChar4);
    case GL_BYTE:
        if (a.normalized)
            PICK(MTLVertexFormatCharNormalized, MTLVertexFormatChar2Normalized, MTLVertexFormatChar3Normalized,
                 MTLVertexFormatChar4Normalized);
        *integer = *is_signed = true;
        PICK(MTLVertexFormatChar, MTLVertexFormatChar2, MTLVertexFormatChar3, MTLVertexFormatChar4);
    case GL_UNSIGNED_SHORT:
        if (a.normalized)
            PICK(MTLVertexFormatUShortNormalized, MTLVertexFormatUShort2Normalized, MTLVertexFormatUShort3Normalized,
                 MTLVertexFormatUShort4Normalized);
        *integer = true;
        PICK(MTLVertexFormatUShort, MTLVertexFormatUShort2, MTLVertexFormatUShort3, MTLVertexFormatUShort4);
    case GL_SHORT:
        if (a.normalized)
            PICK(MTLVertexFormatShortNormalized, MTLVertexFormatShort2Normalized, MTLVertexFormatShort3Normalized,
                 MTLVertexFormatShort4Normalized);
        *integer = *is_signed = true;
        PICK(MTLVertexFormatShort, MTLVertexFormatShort2, MTLVertexFormatShort3, MTLVertexFormatShort4);
    case GL_UNSIGNED_INT:
        *integer = true;
        PICK(MTLVertexFormatUInt, MTLVertexFormatUInt2, MTLVertexFormatUInt3, MTLVertexFormatUInt4);
    case GL_INT:
        *integer = *is_signed = true;
        PICK(MTLVertexFormatInt, MTLVertexFormatInt2, MTLVertexFormatInt3, MTLVertexFormatInt4);
    }
#undef PICK
    warn_once("vertex type " + hex(a.type) + " read as float4");
    return MTLVertexFormatFloat4;
}

uint64_t hash(const void* p, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t k = 0; k < n; k++) h = (h ^ ((const uint8_t*)p)[k]) * 1099511628211ull;
    return h;
}

bool is_pow2(int v) { return v > 0 && (v & (v - 1)) == 0; }
int full_levels(int w, int h, int d) {
    int m = std::max({w, h, d, 1}), n = 1;
    while (m > 1) m >>= 1, n++;
    return n;
}

// Built-in shaders: a full-viewport triangle that copies a texture (present,
// blits; float4 rect u0 v0 u1 v1 maps NDC x -1..1 to u0..u1 and NDC y +1..-1
// to v0..v1), and one that clears (depth and colour from buffer 0).
const char* const kBuiltinMSL = R"(#include <metal_stdlib>
using namespace metal;
struct QOut { float4 pos [[position]]; float2 uv; };
vertex QOut quad_vs(uint vid [[vertex_id]], constant float4& r [[buffer(0)]]) {
    const float2 p = float2((vid << 1) & 2, vid & 2);
    QOut o;
    o.pos = float4(p * 2.0f - 1.0f, 0.0f, 1.0f);
    o.uv = float2(mix(r.x, r.z, p.x), mix(r.w, r.y, p.y));
    return o;
}
fragment float4 copy_fs(QOut in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {
    return t.sample(s, in.uv);
}
struct ClearIn { float4 color; float depth; };
vertex float4 clear_vs(uint vid [[vertex_id]], constant ClearIn& c [[buffer(0)]]) {
    const float2 p = float2((vid << 1) & 2, vid & 2);
    return float4(p * 2.0f - 1.0f, c.depth, 1.0f);
}
fragment float4 clear_fs(constant ClearIn& c [[buffer(0)]]) { return c.color; }
)";

struct ClearIn {
    float color[4];
    float depth;
    float pad[3];
};

}  // namespace

// The descriptor of a program pipeline: draws (Backend::draw) and the
// startup precompile (shader_cache.mm) build it from the same state.
MTLRenderPipelineDescriptor* pipeline_descriptor(id<MTLFunction> vfn, id<MTLFunction> ffn, const PipeDesc& d,
                                                 uint32_t color_outputs) {
    MTLRenderPipelineDescriptor* pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = vfn;
    pd.fragmentFunction = ffn;
    const bool blend = d.blend[0];
    const uint32_t src = d.blend[1], dst = d.blend[2], eq = d.blend[3], mask = d.blend[4];
    const MTLColorWriteMask wm = (mask & 1 ? MTLColorWriteMaskRed : 0) | (mask & 2 ? MTLColorWriteMaskGreen : 0) |
                                 (mask & 4 ? MTLColorWriteMaskBlue : 0) | (mask & 8 ? MTLColorWriteMaskAlpha : 0);
    // Colour 0, then glDrawBuffers' more targets: the same blending and mask
    // on every draw buffer the program writes (result.color[k]); the others
    // keep their contents.
    for (int k = 0; k < 1 + kMoreColors; k++) {
        const MTLPixelFormat f = (MTLPixelFormat)d.formats[k == 0 ? 0 : 2 + k];
        MTLRenderPipelineColorAttachmentDescriptor* ca = pd.colorAttachments[k];
        ca.pixelFormat = f;
        if (f == MTLPixelFormatInvalid) continue;
        const bool written = k == 0 || (color_outputs >> k & 1);
        ca.blendingEnabled = written && blend;
        ca.sourceRGBBlendFactor = ca.sourceAlphaBlendFactor = blend_factor(src);
        ca.destinationRGBBlendFactor = ca.destinationAlphaBlendFactor = blend_factor(dst);
        ca.rgbBlendOperation = ca.alphaBlendOperation = blend_op(eq);
        ca.writeMask = written ? wm : MTLColorWriteMaskNone;
    }
    pd.depthAttachmentPixelFormat = (MTLPixelFormat)d.formats[1];
    pd.stencilAttachmentPixelFormat = (MTLPixelFormat)d.formats[2];
    MTLVertexDescriptor* vd = [MTLVertexDescriptor new];
    for (uint32_t k = 0; k < d.nu; k++) {
        const uint32_t* u = d.uses[k];
        vd.attributes[u[0]].format = (MTLVertexFormat)u[1];
        vd.attributes[u[0]].offset = u[3];
        vd.attributes[u[0]].bufferIndex = kVertexBufferBase + u[2];
    }
    for (uint32_t k = 0; k < d.nb; k++) {
        MTLVertexBufferLayoutDescriptor* l = vd.layouts[kVertexBufferBase + k];
        l.stride = d.binds[k][0];
        if (d.binds[k][1]) l.stepFunction = MTLVertexStepFunctionConstant, l.stepRate = 0;
    }
    pd.vertexDescriptor = vd;
    return pd;
}

// ---- memory ----

namespace {
// Class k holds 256 * 2^(k/4) bytes, rounded up to 256.
uint32_t class_size(int k) {
    static const auto sizes = [] {
        std::array<uint32_t, 64> s{};
        for (int i = 0; i < 64; i++) s[i] = ((uint32_t)std::ceil(256.0 * std::exp2(i / 4.0)) + 255) & ~255u;
        return s;
    }();
    return sizes[k];
}
}  // namespace

Block Arena::alloc(uint32_t size) {
    size = std::max<uint32_t>(size, 1);
    if (size > class_size(kClasses - 1)) {
        const uint32_t n = (size + 16383) & ~16383u;
        const uint32_t g = heap::alloc(*mem_, n, 16384);
        if (!g) fatal("metal: guest heap exhausted (%u bytes for a buffer)", n);
        id<MTLBuffer> b = [dev_ newBufferWithBytesNoCopy:mem_->ptr(g) length:n options:MTLResourceStorageModeShared
                                             deallocator:nil];
        bytes_ += n;
        return Block{b, 0, g, n, -1};
    }
    int cls = 0;
    while (class_size(cls) < size) cls++;
    if (!free_[cls].empty()) {
        Block b = free_[cls].back();
        free_[cls].pop_back();
        return b;
    }
    return carve(class_size(cls), cls);
}

Block Arena::carve(uint32_t size, int cls) {
    constexpr uint32_t kChunk = 8u << 20;
    if (chunks_.empty() || chunks_.back().used + size > chunks_.back().size) {
        const uint32_t g = heap::alloc(*mem_, kChunk, 16384);
        if (!g) fatal("metal: guest heap exhausted (a %u-byte buffer arena chunk)", kChunk);
        id<MTLBuffer> b = [dev_ newBufferWithBytesNoCopy:mem_->ptr(g) length:kChunk options:MTLResourceStorageModeShared
                                             deallocator:nil];
        if (!b) fatal("metal: cannot wrap guest memory as a buffer");
        chunks_.push_back({g, kChunk, 0, b});
        bytes_ += kChunk;
    }
    Chunk& c = chunks_.back();
    Block b{c.buffer, c.used, c.guest + c.used, size, cls};
    c.used += size;
    return b;
}

void Arena::release(const Block& b) {
    if (!b) return;
    if (b.cls >= 0) {
        free_[b.cls].push_back(b);
    } else {
        bytes_ -= b.size;
        heap::release(*mem_, b.guest);
    }
}

Slice Ring::alloc(size_t n, size_t align) {
    for (;;) {
        if (cur_ == bufs_.size())
            bufs_.push_back([dev_ newBufferWithLength:std::max(kChunk, n)
                                              options:MTLResourceStorageModeShared | MTLResourceCPUCacheModeWriteCombined]);
        const size_t at = (used_ + align - 1) & ~(align - 1);
        if (at + n <= bufs_[cur_].length) {
            used_ = at + n;
            return {bufs_[cur_], (uint32_t)at, (uint8_t*)bufs_[cur_].contents + at};
        }
        cur_++, used_ = 0;
    }
}

int PassTargets::width() const {
    id<MTLTexture> t = color ?: depth ?: stencil;
    return t ? std::max<int>(1, (int)t.width >> (color ? color_level : 0)) : 0;
}
int PassTargets::height() const {
    id<MTLTexture> t = color ?: depth ?: stencil;
    return t ? std::max<int>(1, (int)t.height >> (color ? color_level : 0)) : 0;
}

// ---- setup ----

Backend& backend() {
    static Backend b;
    return b;
}

bool Backend::init(GuestMemory* m) {
    if (dev) return true;
    mem = m;
    dev = MTLCreateSystemDefaultDevice();
    if (!dev) return false;
    g_bc_textures = dev.supportsBCTextureCompression && !std::getenv("SCIMITAR_NO_BC");
    if (!g_bc_textures) std::fprintf(stderr, "metal: no BC textures on %s: DXT is decoded on upload\n", dev.name.UTF8String);
    queue = [dev newCommandQueue];
    arena_.init(mem, dev);
    for (FrameSlot& s : slots_) s.ring.init(dev);
    in_flight_ = dispatch_semaphore_create(kMaxInFlight);
    NSError* err = nil;
    builtin_ = [dev newLibraryWithSource:@(kBuiltinMSL) options:nil error:&err];
    if (!builtin_) fatal("metal: built-in shaders: %s", err.localizedDescription.UTF8String);

    // Stand-ins for unbound textures (GL samples (0, 0, 0, 1)), and the lit
    // shadow map for shadow units without a depth texture.
    const uint8_t black[4] = {0, 0, 0, 255};
    auto dummy = [&](MTLTextureType type) {
        MTLTextureDescriptor* td = [MTLTextureDescriptor new];
        td.textureType = type;
        td.pixelFormat = MTLPixelFormatRGBA8Unorm;
        td.width = td.height = td.depth = 1;
        td.storageMode = MTLStorageModeShared;
        id<MTLTexture> t = [dev newTextureWithDescriptor:td];
        const int slices = type == MTLTextureTypeCube ? 6 : 1;
        for (int s = 0; s < slices; s++)
            [t replaceRegion:MTLRegionMake3D(0, 0, 0, 1, 1, 1) mipmapLevel:0 slice:s withBytes:black bytesPerRow:4
                bytesPerImage:4];
        return t;
    };
    dummy_2d_ = dummy(MTLTextureType2D), dummy_cube_ = dummy(MTLTextureTypeCube);
    dummy_3d_ = dummy(MTLTextureType3D), dummy_1d_ = dummy(MTLTextureType1D);
    {
        MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                                                                      width:1 height:1 mipmapped:NO];
        td.storageMode = MTLStorageModePrivate;
        td.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
        lit_shadow_ = [dev newTextureWithDescriptor:td];
        MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.depthAttachment.texture = lit_shadow_;
        rp.depthAttachment.loadAction = MTLLoadActionClear;
        rp.depthAttachment.clearDepth = 1.0;
        rp.depthAttachment.storeAction = MTLStoreActionStore;
        id<MTLCommandBuffer> cb = [queue commandBuffer];
        [[cb renderCommandEncoderWithDescriptor:rp] endEncoding];
        [cb commit];
    }
    MTLSamplerDescriptor* sd = [MTLSamplerDescriptor new];
    dummy_sampler_ = [dev newSamplerStateWithDescriptor:sd];
    sd.compareFunction = MTLCompareFunctionLessEqual;
    lit_sampler_ = [dev newSamplerStateWithDescriptor:sd];
    // GL_QUADS as triangles: 16-bit indices for up to 16k quads.
    {
        quad_index_count_ = 16384 * 6;
        std::vector<uint16_t> idx(quad_index_count_);
        for (uint32_t q = 0; q < 16384; q++) {
            const uint16_t b = (uint16_t)(q * 4);
            const uint16_t v[6] = {b, (uint16_t)(b + 1), (uint16_t)(b + 2), b, (uint16_t)(b + 2), (uint16_t)(b + 3)};
            std::memcpy(&idx[q * 6], v, sizeof v);
        }
        quad_indices_ = [dev newBufferWithBytes:idx.data() length:idx.size() * 2 options:MTLResourceStorageModeShared];
    }
    cb_ = [queue commandBuffer];
    begin_frame();
    if (verbose()) std::fprintf(stderr, "metal: %s\n", dev.name.UTF8String);
    return true;
}

void Backend::attach_layer(CAMetalLayer* layer, int w, int h) {
    layer_ = layer;
    layer_.device = dev;
    layer_.pixelFormat = MTLPixelFormatBGRA8Unorm;
    layer_.framebufferOnly = YES;
    layer_.maximumDrawableCount = 3;
    set_default_size(w, h);
}

void Backend::draw_progress(double done) {
    @autoreleasepool {
        if (!layer_) return;
        CGSize size = layer_.drawableSize;
        if (size.width < 16 || size.height < 16) {
            const CGFloat sc = layer_.contentsScale > 0 ? layer_.contentsScale : 1;
            size = CGSizeMake(layer_.bounds.size.width * sc, layer_.bounds.size.height * sc);
            if (size.width < 16 || size.height < 16) return;
            layer_.drawableSize = size;
        }
        if (!progress_tex_[0]) {
            const uint8_t px[2][4] = {{60, 56, 52, 255}, {214, 206, 196, 255}};  // BGRA: track, fill
            for (int k = 0; k < 2; k++) {
                MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                              width:1 height:1 mipmapped:NO];
                progress_tex_[k] = [dev newTextureWithDescriptor:td];
                [progress_tex_[k] replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:px[k] bytesPerRow:4];
            }
        }
        id<CAMetalDrawable> drawable = [layer_ nextDrawable];
        if (!drawable) return;
        static bool told;
        if (!told) std::fprintf(stderr, "shaders: progress bar in %.0fx%.0f\n", size.width, size.height), told = true;
        MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = drawable.texture;
        rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].clearColor = MTLClearColorMake(0.02, 0.02, 0.025, 1);
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLCommandBuffer> cb = [queue commandBuffer];
        id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
        [e setRenderPipelineState:builtin_pipeline("quad_vs", "copy_fs", MTLPixelFormatBGRA8Unorm, MTLPixelFormatInvalid,
                                                   MTLPixelFormatInvalid, 0xf)];
        const float r[4] = {0, 1, 1, 0};
        [e setVertexBytes:r length:sizeof r atIndex:0];
        [e setFragmentSamplerState:dummy_sampler_ atIndex:0];
        const NSUInteger W = (NSUInteger)size.width, H = (NSUInteger)size.height;
        const NSUInteger bw = W * 2 / 5, bh = std::max<NSUInteger>(4, H / 90), x = (W - bw) / 2, y = H * 3 / 4;
        for (int k = 0; k < 2; k++) {
            const NSUInteger w = k ? (NSUInteger)(bw * std::clamp(done, 0.0, 1.0)) : bw;
            if (!w) continue;
            [e setScissorRect:(MTLScissorRect){x, y, w, bh}];
            [e setFragmentTexture:progress_tex_[k] atIndex:0];
            [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        }
        [e endEncoding];
        [cb presentDrawable:drawable];
        [cb commit];
    }
}

void Backend::set_default_size(int w, int h) {
    if (w <= 0 || h <= 0 || (w == default_w_ && h == default_h_)) return;
    if (enc_ && (pass_.color == default_color_ || pass_.depth == default_ds_)) end_pass();
    if (pending_clear_.targets.color == default_color_) pending_clear_ = {};
    default_w_ = w, default_h_ = h;
    MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                  width:w height:h mipmapped:NO];
    td.storageMode = MTLStorageModeShared;
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    default_color_ = [dev newTextureWithDescriptor:td];
    td.pixelFormat = MTLPixelFormatDepth32Float_Stencil8;
    td.storageMode = MTLStorageModePrivate;
    default_ds_ = [dev newTextureWithDescriptor:td];
    if (verbose()) std::fprintf(stderr, "metal: default framebuffer %dx%d\n", w, h);
}

// ---- frames and command buffers ----

void Backend::begin_frame() {
    dispatch_semaphore_wait(in_flight_, DISPATCH_TIME_FOREVER);
    frame_++;
    retire();
    FrameSlot& s = slot();
    s.ring.reset();
    for (id<MTLBuffer> v : s.visibility) std::memset(v.contents, 0, v.length);
    s.vis_cur = 0, s.vis_used = 0;
    shader_cache().new_frame();
    // SCIMITAR_METAL_DUMP_FRAME=N dumps frame N (scripted runs); F7 the next one.
    static const uint64_t dump_frame = std::getenv("SCIMITAR_METAL_DUMP_FRAME") ? std::strtoull(std::getenv("SCIMITAR_METAL_DUMP_FRAME"), nullptr, 0) : 0;
    if (dump_requested.exchange(false) || (dump_frame && frame_ == dump_frame)) dump_begin_frame();
}

// Frees what completed command buffers no longer need and reads finished queries.
void Backend::retire() {
    const uint64_t done = completed_seg_.load();
    while (!deferred_.empty() && deferred_.front().first <= done) {
        arena_.release(deferred_.front().second);
        deferred_.pop_front();
    }
    while (!committed_.empty() && committed_.front().first <= done) committed_.pop_front();
    std::vector<GLuint> still;
    for (GLuint id : pending_queries_) {
        auto it = queries.find(id);
        if (it == queries.end() || it->second.resolved) continue;
        if (!resolve(it->second)) still.push_back(id);
    }
    pending_queries_.swap(still);
}

// Reads a query's result once its command buffer has completed.
bool Backend::resolve(Query& q) {
    if (q.resolved) return true;
    if (q.seg > completed_seg_.load()) return false;
    q.result = 0;
    for (const auto& s : q.slots) q.result += *(const uint64_t*)((const uint8_t*)s.buffer.contents + s.offset);
    q.resolved = true;
    return true;
}

void Backend::submit(bool final_of_frame, id<CAMetalDrawable> drawable) {
    if (pending_clear_.targets.color || pending_clear_.targets.depth || pending_clear_.targets.stencil) {
        const PassTargets t = pending_clear_.targets;
        begin_pass(t);
    }
    end_pass();
    commit(final_of_frame, drawable);
}

// Commits the command buffer (no pass open) and starts the next one.
void Backend::commit(bool final_of_frame, id<CAMetalDrawable> drawable) {
    draws_since_commit_ = 0;
    if (drawable) {
#if TARGET_OS_IPHONE
        // The frame-rate cap: the display shows this frame only a set time after the last one (the
        // render loop then waits for drawables, so the game runs at that rate).
        const double min_duration = frame_min_duration.load();
        if (min_duration > 0) [cb_ presentDrawable:drawable afterMinimumDuration:min_duration];
        else
#endif
            [cb_ presentDrawable:drawable];
    }
    const uint64_t seg = seg_;
    std::atomic<uint64_t>* done = &completed_seg_;
    std::atomic<double>* gpu = &gpu_ms;
    std::atomic<uint64_t>* gpu_frames_done = &gpu_frames;
    dispatch_semaphore_t sem = final_of_frame ? in_flight_ : nullptr;
    [cb_ addCompletedHandler:^(id<MTLCommandBuffer> cb) {
      if (cb.error) std::fprintf(stderr, "metal: command buffer failed: %s\n", cb.error.localizedDescription.UTF8String);
      double ms = gpu->load();
      while (!gpu->compare_exchange_weak(ms, ms + (cb.GPUEndTime - cb.GPUStartTime) * 1000.0)) {}
      if (sem) (*gpu_frames_done)++;
      uint64_t c = done->load();
      while (c < seg && !done->compare_exchange_weak(c, seg)) {}
      if (sem) dispatch_semaphore_signal(sem);
    }];
    [cb_ commit];
    committed_.push_back({seg, cb_});
    seg_++;
    cb_ = [queue commandBuffer];
    if (!final_of_frame) n_splits++;
}

void Backend::wait_seg(uint64_t seg) {
    if (seg >= seg_) submit(false);
    for (auto& [s, cb] : committed_)
        if (s >= seg) {
            [cb waitUntilCompleted];
            break;
        }
    // The handler may run a moment after the wait returns.
    while (completed_seg_.load() < seg) std::this_thread::yield();
    retire();
}

void Backend::flush_and_wait() { wait_seg(seg_); }

// ---- buffers ----

Buffer* Backend::bound_buffer(GLenum target) {
    GLuint name = 0;
    switch (target) {
    case GL_ARRAY_BUFFER: name = st.array_buffer; break;
    case GL_ELEMENT_ARRAY_BUFFER: name = st.element_buffer; break;
    case GL_PIXEL_PACK_BUFFER: name = st.pack_buffer; break;
    case GL_PIXEL_UNPACK_BUFFER: name = st.unpack_buffer; break;
    }
    if (!name) return nullptr;
    return &buffers[name];
}

// New storage for a buffer the GPU may still read; the old storage is freed
// when the command buffers that used it complete.
void Backend::rename(Buffer& b, bool keep_contents) {
    const Block old = b.block;
    b.block = arena_.alloc(std::max<uint32_t>(b.size, 1));
    if (keep_contents && old) std::memcpy(mem->ptr(b.block.guest), mem->ptr(old.guest), std::min(old.size, b.size));
    if (old) deferred_.push_back({b.last_use, old});
    b.last_use = 0;
    n_renames++;
}

void Backend::buffer_data(GLenum target, uint32_t size, const void* data) {
    Buffer* b = bound_buffer(target);
    if (!b) return;
    if (!b->block || b->block.size < size || b->block.size > 2 * std::max<uint32_t>(size, 256) || in_flight(b->last_use)) {
        b->size = size;
        if (b->block && !in_flight(b->last_use) && b->block.size >= size && b->block.size <= 2 * std::max<uint32_t>(size, 256)) {
        } else if (b->block) {
            rename(*b, false);
        } else {
            b->block = arena_.alloc(std::max<uint32_t>(size, 1));
        }
    }
    b->size = size;
    if (data) std::memcpy(mem->ptr(b->block.guest), data, size);
}

void Backend::buffer_sub_data(GLenum target, uint32_t offset, uint32_t size, const void* data) {
    Buffer* b = bound_buffer(target);
    if (!b || !b->block || !data || offset + size > b->size) return;
    if (in_flight(b->last_use)) rename(*b, true);
    std::memcpy(mem->ptr(b->block.guest + offset), data, size);
}

// The storage itself is mapped: its guest address. A map of storage the GPU may
// still read gets new storage with the old contents: GL keeps the bytes a
// write-only map does not write (only glBufferData(NULL) discards them).
uint32_t Backend::map_buffer(GLenum target, GLenum access) {
    Buffer* b = bound_buffer(target);
    if (!b) return 0;
    if (!b->block) b->block = arena_.alloc(std::max<uint32_t>(b->size, 1));
    if (access != GL_READ_ONLY && in_flight(b->last_use)) rename(*b, true);
    else if (access == GL_READ_ONLY && in_flight(b->last_use)) wait_seg(b->last_use);
    b->mapped = true;
    return b->block.guest;
}

bool Backend::unmap_buffer(GLenum target) {
    Buffer* b = bound_buffer(target);
    if (b) b->mapped = false;
    return true;
}

void Backend::delete_buffers(int n, const GLuint* ids) {
    for (int k = 0; k < n; k++) {
        auto it = buffers.find(ids[k]);
        if (it == buffers.end()) continue;
        if (it->second.block) deferred_.push_back({std::max(it->second.last_use, seg_), it->second.block});
        for (GLuint* slot : {&st.array_buffer, &st.element_buffer, &st.pack_buffer, &st.unpack_buffer})
            if (*slot == ids[k]) *slot = 0;
        for (VertexAttrib& a : st.attribs)
            if (a.buffer == ids[k]) a.buffer = 0;
        buffers.erase(it);
    }
}

// ---- textures ----

Texture* Backend::bound_texture(GLenum target) {
    TexUnit& u = st.units[st.unit];
    GLuint name = 0;
    GLenum kind = target;
    switch (target) {
    case GL_TEXTURE_1D: name = u.t1d; break;
    case GL_TEXTURE_2D: name = u.t2d; break;
    case GL_TEXTURE_3D: name = u.t3d; break;
    case GL_TEXTURE_RECTANGLE_ARB: name = u.rect; break;
    default:
        if (target == GL_TEXTURE_CUBE_MAP || (target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && target <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z))
            name = u.cube, kind = GL_TEXTURE_CUBE_MAP;
        break;
    }
    if (!name) return nullptr;  // the default texture objects are not used by the game
    Texture& t = textures[name];
    if (!t.target) t.target = kind;
    return &t;
}

void Backend::create_storage(Texture& t, int w, int h, int d, int levels) {
    MTLTextureDescriptor* td = [MTLTextureDescriptor new];
    int slices = 1;
    switch (t.target) {
    case GL_TEXTURE_1D: td.textureType = MTLTextureType1D, levels = 1; break;
    case GL_TEXTURE_3D: td.textureType = MTLTextureType3D; break;
    case GL_TEXTURE_CUBE_MAP: td.textureType = MTLTextureTypeCube, slices = 6; break;
    default: td.textureType = MTLTextureType2D; break;
    }
    td.pixelFormat = t.fmt.pixel;
    td.width = std::max(w, 1), td.height = std::max(h, 1), td.depth = std::max(d, 1);
    td.mipmapLevelCount = std::max(levels, 1);
    const bool ds = t.fmt.depth || t.fmt.stencil;
    td.storageMode = ds ? MTLStorageModePrivate : MTLStorageModeShared;
    td.usage = MTLTextureUsageShaderRead | (t.fmt.compressed || td.textureType == MTLTextureType1D ? 0 : MTLTextureUsageRenderTarget) |
               (t.fmt.swizzled || ds ? MTLTextureUsagePixelFormatView : 0);
    t.tex = [dev newTextureWithDescriptor:td];
    if (!t.tex) fatal("metal: cannot create a %dx%dx%d texture (format %d)", w, h, d, (int)t.fmt.pixel);
    t.w = w, t.h = h, t.d = d, t.levels = (int)td.mipmapLevelCount;
    t.sample_view = nil;
    if (t.fmt.swizzled || ds) {
        MTLTextureSwizzleChannels sw = t.fmt.swizzle;
        if (ds) sw = {MTLTextureSwizzleRed, MTLTextureSwizzleRed, MTLTextureSwizzleRed, MTLTextureSwizzleOne};
        // The view keeps the texture's format: Metal cannot view the depth plane
        // of a depth-stencil texture as Depth32Float (an invalid view reads
        // garbage, e.g. the depth of field's depth). Sampled, it reads depth.
        t.sample_view = [t.tex newTextureViewWithPixelFormat:t.fmt.pixel textureType:td.textureType
                                                      levels:NSMakeRange(0, t.levels) slices:NSMakeRange(0, slices)
                                                     swizzle:sw];
    }
    t.last_use = 0;
}

// Copies every level and slice of `from` into `to` (same format, at least as many levels).
static void copy_texture(id<MTLCommandQueue> queue, id<MTLTexture> from, id<MTLTexture> to, bool wait) {
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
    const NSUInteger levels = std::min(from.mipmapLevelCount, to.mipmapLevelCount);
    const NSUInteger slices = from.textureType == MTLTextureTypeCube ? 6 : 1;
    for (NSUInteger s = 0; s < slices; s++)
        for (NSUInteger l = 0; l < levels; l++) {
            const MTLSize size = MTLSizeMake(std::max<NSUInteger>(1, from.width >> l), std::max<NSUInteger>(1, from.height >> l),
                                             std::max<NSUInteger>(1, from.depth >> l));
            [bl copyFromTexture:from sourceSlice:s sourceLevel:l sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:size
                      toTexture:to destinationSlice:s destinationLevel:l destinationOrigin:MTLOriginMake(0, 0, 0)];
        }
    [bl endEncoding];
    [cb commit];
    if (wait) [cb waitUntilCompleted];
}

void Backend::rename(Texture& t, bool keep_contents) {
    id<MTLTexture> old = t.tex;
    create_storage(t, t.w, t.h, t.d, t.levels);
    if (keep_contents && old) {
        if (old.storageMode == MTLStorageModeShared) {
            // The GPU only reads the old storage: copy on the CPU.
            const NSUInteger slices = old.textureType == MTLTextureTypeCube ? 6 : 1;
            std::vector<uint8_t> tmp;
            for (NSUInteger s = 0; s < slices; s++)
                for (int l = 0; l < t.levels; l++) {
                    const int w = std::max(1, t.w >> l), h = std::max(1, t.h >> l), d = std::max(1, t.d >> l);
                    const size_t row = t.fmt.compressed ? (size_t)((w + 3) / 4) * t.fmt.bpp : (size_t)w * t.fmt.bpp;
                    const size_t rows = t.fmt.compressed ? (size_t)(h + 3) / 4 : (size_t)h;
                    tmp.resize(row * rows * d);
                    const MTLRegion r = MTLRegionMake3D(0, 0, 0, w, h, d);
                    [old getBytes:tmp.data() bytesPerRow:row bytesPerImage:row * rows fromRegion:r mipmapLevel:l slice:s];
                    [t.tex replaceRegion:r mipmapLevel:l slice:s withBytes:tmp.data() bytesPerRow:row bytesPerImage:row * rows];
                }
        } else {
            copy_texture(queue, old, t.tex, true);
        }
    }
    n_renames++;
}

// Formats: the storage for an internal format, preferring the layout of the
// first upload so that it needs no conversion.
const TexFormat* tex_format(GLenum internal, GLenum format, GLenum type) {
    static std::map<uint64_t, TexFormat> cache;
    const uint64_t key = (uint64_t)internal << 32 | (uint64_t)(format & 0xffff) << 16 | (type & 0xffff);
    auto it = cache.find(key);
    if (it != cache.end()) return &it->second;
    TexFormat f;
    const MTLTextureSwizzle R = MTLTextureSwizzleRed, G = MTLTextureSwizzleGreen, Z = MTLTextureSwizzleZero,
                            O = MTLTextureSwizzleOne;
    auto sw = [&](MTLTextureSwizzle r, MTLTextureSwizzle g, MTLTextureSwizzle b, MTLTextureSwizzle a) {
        f.swizzle = {r, g, b, a};
        f.swizzled = true;
    };
    switch (internal) {
    case GL_RGBA8: case GL_RGBA: case 4: case GL_RGB8: case GL_RGB: case 3: case GL_RGB5_A1: case GL_RGBA4: case GL_RGB5:
        f.pixel = format == GL_BGRA || format == GL_BGR ? MTLPixelFormatBGRA8Unorm : MTLPixelFormatRGBA8Unorm;
        f.bpp = 4;
        break;
    case GL_SRGB8_ALPHA8: case GL_SRGB8: f.pixel = MTLPixelFormatRGBA8Unorm_sRGB, f.bpp = 4; break;
    case GL_ALPHA8: case GL_ALPHA: f.pixel = MTLPixelFormatR8Unorm, f.bpp = 1, sw(Z, Z, Z, R); break;
    case GL_LUMINANCE8: case GL_LUMINANCE: case 1: f.pixel = MTLPixelFormatR8Unorm, f.bpp = 1, sw(R, R, R, O); break;
    case GL_INTENSITY8: case GL_INTENSITY: f.pixel = MTLPixelFormatR8Unorm, f.bpp = 1, sw(R, R, R, R); break;
    case GL_LUMINANCE8_ALPHA8: case GL_LUMINANCE_ALPHA: case 2: f.pixel = MTLPixelFormatRG8Unorm, f.bpp = 2, sw(R, R, R, G); break;
    case GL_RGBA16F_ARB: case GL_RGB16F_ARB: f.pixel = MTLPixelFormatRGBA16Float, f.bpp = 8; break;
    case GL_RGBA32F_ARB: case GL_RGB32F_ARB: f.pixel = MTLPixelFormatRGBA32Float, f.bpp = 16; break;
    case GL_LUMINANCE16F_ARB: f.pixel = MTLPixelFormatR16Float, f.bpp = 2, sw(R, R, R, O); break;
    case GL_LUMINANCE32F_ARB: f.pixel = MTLPixelFormatR32Float, f.bpp = 4, sw(R, R, R, O); break;
    case GL_ALPHA16F_ARB: f.pixel = MTLPixelFormatR16Float, f.bpp = 2, sw(Z, Z, Z, R); break;
    case GL_ALPHA32F_ARB: f.pixel = MTLPixelFormatR32Float, f.bpp = 4, sw(Z, Z, Z, R); break;
    case GL_LUMINANCE_ALPHA16F_ARB: f.pixel = MTLPixelFormatRG16Float, f.bpp = 4, sw(R, R, R, G); break;
    case GL_LUMINANCE_ALPHA32F_ARB: f.pixel = MTLPixelFormatRG32Float, f.bpp = 8, sw(R, R, R, G); break;
    case GL_RGB10_A2: f.pixel = MTLPixelFormatRGB10A2Unorm, f.bpp = 4; break;
    case GL_RGBA16: f.pixel = MTLPixelFormatRGBA16Unorm, f.bpp = 8; break;
    case GL_DEPTH_COMPONENT: case GL_DEPTH_COMPONENT16: case GL_DEPTH_COMPONENT24: case GL_DEPTH_COMPONENT32:
        f.pixel = MTLPixelFormatDepth32Float, f.bpp = 4, f.depth = true;
        break;
    case GL_DEPTH24_STENCIL8_EXT: case GL_DEPTH_STENCIL_EXT:
        f.pixel = MTLPixelFormatDepth32Float_Stencil8, f.bpp = 8, f.depth = f.stencil = true;
        break;
    case GL_COMPRESSED_RGB_S3TC_DXT1_EXT:
        f.pixel = MTLPixelFormatBC1_RGBA, f.bpp = 8, f.compressed = true;
        sw(R, G, MTLTextureSwizzleBlue, O);  // RGB DXT1: the 3-colour block's black is opaque
        break;
    case GL_COMPRESSED_RGBA_S3TC_DXT1_EXT: f.pixel = MTLPixelFormatBC1_RGBA, f.bpp = 8, f.compressed = true; break;
    case GL_COMPRESSED_RGBA_S3TC_DXT3_EXT: f.pixel = MTLPixelFormatBC2_RGBA, f.bpp = 16, f.compressed = true; break;
    case GL_COMPRESSED_RGBA_S3TC_DXT5_EXT: f.pixel = MTLPixelFormatBC3_RGBA, f.bpp = 16, f.compressed = true; break;
    default:
        warn_once("texture internal format " + hex(internal) + " stored as RGBA8");
        f.pixel = MTLPixelFormatRGBA8Unorm, f.bpp = 4;
        break;
    }
    if (f.compressed && !g_bc_textures) {
        f.bc = f.pixel == MTLPixelFormatBC1_RGBA ? 1 : f.pixel == MTLPixelFormatBC2_RGBA ? 2 : 3;
        f.pixel = MTLPixelFormatRGBA8Unorm, f.bpp = 4, f.compressed = false;
    }
    return &(cache[key] = f);
}

MTLPixelFormat renderbuffer_format(GLenum internal, TexFormat* out) {
    *out = *tex_format(internal, GL_BGRA, GL_UNSIGNED_BYTE);
    return out->pixel;
}

namespace {

// Source pixels of (format, type) as RGBA floats.
uint32_t components(GLenum format) {
    switch (format) {
    case GL_RGBA: case GL_BGRA: return 4;
    case GL_RGB: case GL_BGR: return 3;
    case GL_LUMINANCE_ALPHA: return 2;
    default: return 1;
    }
}
uint32_t source_bpp(GLenum format, GLenum type) {
    switch (type) {
    case GL_UNSIGNED_INT_8_8_8_8_REV: case GL_UNSIGNED_INT_8_8_8_8: case GL_UNSIGNED_INT_2_10_10_10_REV: return 4;
    case GL_UNSIGNED_SHORT_5_6_5: case GL_UNSIGNED_SHORT_1_5_5_5_REV: case GL_UNSIGNED_SHORT_4_4_4_4_REV: return 2;
    }
    return components(format) * type_size(type);
}
float half_to_float(uint16_t h) {
    const uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
    float v = e == 0 ? std::ldexp((float)m, -24) : e == 31 ? (m ? NAN : INFINITY) : std::ldexp((float)(m | 1024), (int)e - 25);
    return s ? -v : v;
}
uint16_t float_to_half(float f) {
    __fp16 h = (__fp16)f;
    uint16_t r;
    std::memcpy(&r, &h, 2);
    return r;
}
void decode(const uint8_t* p, GLenum format, GLenum type, float out[4]) {
    float c[4] = {0, 0, 0, 1};
    const uint32_t n = components(format);
    if (type == GL_UNSIGNED_INT_8_8_8_8_REV) type = GL_UNSIGNED_BYTE;
    for (uint32_t k = 0; k < n; k++) {
        switch (type) {
        case GL_UNSIGNED_BYTE: c[k] = p[k] / 255.0f; break;
        case GL_BYTE: c[k] = std::max(-1.0f, (int8_t)p[k] / 127.0f); break;
        case GL_UNSIGNED_SHORT: c[k] = ((const uint16_t*)p)[k] / 65535.0f; break;
        case GL_HALF_FLOAT_ARB: c[k] = half_to_float(((const uint16_t*)p)[k]); break;
        case GL_FLOAT: c[k] = ((const float*)p)[k]; break;
        default: break;
        }
    }
    switch (format) {
    case GL_BGRA: out[0] = c[2], out[1] = c[1], out[2] = c[0], out[3] = c[3]; return;
    case GL_BGR: out[0] = c[2], out[1] = c[1], out[2] = c[0], out[3] = 1; return;
    case GL_RGB: out[0] = c[0], out[1] = c[1], out[2] = c[2], out[3] = 1; return;
    case GL_LUMINANCE_ALPHA: out[0] = out[1] = out[2] = c[0], out[3] = c[1]; return;
    case GL_ALPHA: out[0] = out[1] = out[2] = 0, out[3] = c[0]; return;
    case GL_LUMINANCE: case GL_RED: case GL_INTENSITY: out[0] = out[1] = out[2] = c[0], out[3] = 1; return;
    default: std::memcpy(out, c, 16); return;
    }
}
// RGBA floats into the storage format (which a swizzle may present differently).
void encode(const float c[4], const TexFormat& f, GLenum format, uint8_t* p) {
    auto u8 = [](float v) { return (uint8_t)std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f); };
    // Single- and two-channel storage keeps the channel the swizzle reads.
    const float lum = format == GL_ALPHA ? c[3] : c[0];
    switch (f.pixel) {
    case MTLPixelFormatRGBA8Unorm: case MTLPixelFormatRGBA8Unorm_sRGB:
        p[0] = u8(c[0]), p[1] = u8(c[1]), p[2] = u8(c[2]), p[3] = u8(c[3]);
        break;
    case MTLPixelFormatBGRA8Unorm: p[0] = u8(c[2]), p[1] = u8(c[1]), p[2] = u8(c[0]), p[3] = u8(c[3]); break;
    case MTLPixelFormatR8Unorm: p[0] = u8(lum); break;
    case MTLPixelFormatRG8Unorm: p[0] = u8(c[0]), p[1] = u8(c[3]); break;
    case MTLPixelFormatRGBA16Float:
        for (int k = 0; k < 4; k++) ((uint16_t*)p)[k] = float_to_half(c[k]);
        break;
    case MTLPixelFormatRGBA32Float: std::memcpy(p, c, 16); break;
    case MTLPixelFormatR16Float: ((uint16_t*)p)[0] = float_to_half(lum); break;
    case MTLPixelFormatR32Float: std::memcpy(p, &lum, 4); break;
    case MTLPixelFormatRG16Float: ((uint16_t*)p)[0] = float_to_half(c[0]), ((uint16_t*)p)[1] = float_to_half(c[3]); break;
    case MTLPixelFormatRG32Float: std::memcpy(p, &c[0], 4), std::memcpy(p + 4, &c[3], 4); break;
    case MTLPixelFormatRGBA16Unorm:
        for (int k = 0; k < 4; k++) ((uint16_t*)p)[k] = (uint16_t)std::lround(std::clamp(c[k], 0.0f, 1.0f) * 65535.0f);
        break;
    default: std::memset(p, 0, 4); break;
    }
}
// Whether the source rows are already in the storage's layout.
bool direct(const TexFormat& f, GLenum format, GLenum type) {
    const bool u8 = type == GL_UNSIGNED_BYTE || type == GL_UNSIGNED_INT_8_8_8_8_REV;
    switch (f.pixel) {
    case MTLPixelFormatBGRA8Unorm: return format == GL_BGRA && u8;
    case MTLPixelFormatRGBA8Unorm: case MTLPixelFormatRGBA8Unorm_sRGB: return format == GL_RGBA && u8;
    case MTLPixelFormatR8Unorm: return components(format) == 1 && type == GL_UNSIGNED_BYTE;
    case MTLPixelFormatRG8Unorm: return format == GL_LUMINANCE_ALPHA && type == GL_UNSIGNED_BYTE;
    case MTLPixelFormatRGBA16Float: return format == GL_RGBA && type == GL_HALF_FLOAT_ARB;
    case MTLPixelFormatRGBA32Float: return format == GL_RGBA && type == GL_FLOAT;
    case MTLPixelFormatR16Float: return components(format) == 1 && type == GL_HALF_FLOAT_ARB;
    case MTLPixelFormatR32Float: return components(format) == 1 && type == GL_FLOAT;
    case MTLPixelFormatRGB10A2Unorm: return format == GL_RGBA && type == GL_UNSIGNED_INT_2_10_10_10_REV;
    default: return false;
    }
}

// BC1-BC3 (DXT1, DXT3, DXT5) to RGBA8: `blocks` holds ceil(w/4) x ceil(h/4)
// blocks, `out` gets w x h pixels.
void rgb565(uint16_t c, int* rgb) {
    rgb[0] = ((c >> 11) & 31) * 255 / 31, rgb[1] = ((c >> 5) & 63) * 255 / 63, rgb[2] = (c & 31) * 255 / 31;
}
void decode_bc(int kind, const uint8_t* blocks, int w, int h, uint8_t* out) {
    const int bw = (w + 3) / 4, bh = (h + 3) / 4, size = kind == 1 ? 8 : 16;
    for (int by = 0; by < bh; by++)
        for (int bx = 0; bx < bw; bx++) {
            const uint8_t* b = blocks + ((size_t)by * bw + bx) * size;
            uint8_t alpha[16];
            std::fill(alpha, alpha + 16, 255);
            if (kind == 2) {  // explicit 4-bit alpha
                for (int i = 0; i < 16; i++) alpha[i] = ((b[i / 2] >> (4 * (i & 1))) & 15) * 17;
            } else if (kind == 3) {  // interpolated alpha: 2 endpoints, 3-bit indices
                int a[8] = {b[0], b[1]};
                if (a[0] > a[1])
                    for (int i = 1; i < 7; i++) a[i + 1] = ((7 - i) * a[0] + i * a[1]) / 7;
                else {
                    for (int i = 1; i < 5; i++) a[i + 1] = ((5 - i) * a[0] + i * a[1]) / 5;
                    a[6] = 0, a[7] = 255;
                }
                uint64_t bits = 0;
                for (int i = 0; i < 6; i++) bits |= (uint64_t)b[2 + i] << (8 * i);
                for (int i = 0; i < 16; i++) alpha[i] = (uint8_t)a[(bits >> (3 * i)) & 7];
            }
            const uint8_t* cb = kind == 1 ? b : b + 8;
            const uint16_t c0 = cb[0] | cb[1] << 8, c1 = cb[2] | cb[3] << 8;
            int col[4][4];
            rgb565(c0, col[0]), rgb565(c1, col[1]);
            col[0][3] = col[1][3] = col[2][3] = col[3][3] = 255;
            if (c0 > c1 || kind != 1) {  // DXT3/5 colour blocks always use four colours
                for (int k = 0; k < 3; k++) {
                    col[2][k] = (2 * col[0][k] + col[1][k]) / 3;
                    col[3][k] = (col[0][k] + 2 * col[1][k]) / 3;
                }
            } else {
                for (int k = 0; k < 3; k++) col[2][k] = (col[0][k] + col[1][k]) / 2, col[3][k] = 0;
                col[3][3] = 0;  // transparent black
            }
            const uint32_t idx = cb[4] | cb[5] << 8 | cb[6] << 16 | (uint32_t)cb[7] << 24;
            for (int i = 0; i < 16; i++) {
                const int px = bx * 4 + (i & 3), py = by * 4 + i / 4;
                if (px >= w || py >= h) continue;
                const int* c = col[(idx >> (2 * i)) & 3];
                uint8_t* o = out + ((size_t)py * w + px) * 4;
                o[0] = (uint8_t)c[0], o[1] = (uint8_t)c[1], o[2] = (uint8_t)c[2];
                o[3] = kind == 1 ? (uint8_t)c[3] : alpha[i];
            }
        }
}

}  // namespace

void decode_bc_image(int kind, const uint8_t* blocks, int w, int h, uint8_t* rgba) { decode_bc(kind, blocks, w, h, rgba); }

void Backend::upload(Texture& t, int face, int level, int x, int y, int z, int w, int h, int d, GLenum format,
                     GLenum type, const void* pixels, bool compressed) {
    if (!pixels || !t.tex || w <= 0 || h <= 0 || d <= 0) return;
    if (level >= t.levels) return;
    const MTLRegion region = MTLRegionMake3D(x, y, z, w, h, d);
    if (t.fmt.depth || t.fmt.stencil) {
        warn_once("depth texture upload ignored");
        return;
    }
    if (t.fmt.bc) {
        const size_t block_image = (size_t)((w + 3) / 4) * ((h + 3) / 4) * (t.fmt.bc == 1 ? 8 : 16);
        std::vector<uint8_t> tmp((size_t)w * h * 4);
        for (int zz = 0; zz < d; zz++) {
            decode_bc(t.fmt.bc, (const uint8_t*)pixels + zz * block_image, w, h, tmp.data());
            [t.tex replaceRegion:MTLRegionMake3D(x, y, z + zz, w, h, 1) mipmapLevel:level slice:face withBytes:tmp.data()
                     bytesPerRow:(size_t)w * 4 bytesPerImage:(size_t)w * h * 4];
        }
        return;
    }
    if (compressed || t.fmt.compressed) {
        const size_t row = (size_t)((w + 3) / 4) * t.fmt.bpp, rows = (size_t)(h + 3) / 4;
        [t.tex replaceRegion:region mipmapLevel:level slice:face withBytes:pixels bytesPerRow:row bytesPerImage:row * rows];
        return;
    }
    const uint32_t spp = source_bpp(format, type);
    const int row_pixels = st.unpack_row_length > 0 ? st.unpack_row_length : w;
    const size_t align = (size_t)std::max(1, st.unpack_alignment);
    const size_t src_row = ((size_t)row_pixels * spp + align - 1) / align * align;
    if (direct(t.fmt, format, type)) {
        [t.tex replaceRegion:region mipmapLevel:level slice:face withBytes:pixels bytesPerRow:src_row
               bytesPerImage:src_row * h];
        return;
    }
    const size_t dst_row = (size_t)w * t.fmt.bpp;
    std::vector<uint8_t> tmp(dst_row * h * d);
    float c[4];
    for (int zz = 0; zz < d; zz++)
        for (int yy = 0; yy < h; yy++) {
            const uint8_t* s = (const uint8_t*)pixels + (zz * (size_t)h + yy) * src_row;
            uint8_t* o = tmp.data() + (zz * (size_t)h + yy) * dst_row;
            for (int xx = 0; xx < w; xx++) {
                decode(s + (size_t)xx * spp, format, type, c);
                encode(c, t.fmt, format, o + (size_t)xx * t.fmt.bpp);
            }
        }
    [t.tex replaceRegion:region mipmapLevel:level slice:face withBytes:tmp.data() bytesPerRow:dst_row bytesPerImage:dst_row * h];
}

void Backend::tex_image(GLenum target, int level, GLenum internal, int w, int h, int d, GLenum format, GLenum type,
                        const void* pixels, bool compressed, uint32_t) {
    static const bool trace = std::getenv("SCIMITAR_TRACE_NAMES") != nullptr;
    Texture* t = bound_texture(target);
    if (trace) {
        const TexUnit& u = st.units[st.unit];
        std::fprintf(stderr, "metal: teximage unit %u target %x (2d %u cube %u 3d %u) level %d %dx%d internal %x -> %s\n", st.unit,
                     target, u.t2d, u.cube, u.t3d, level, w, h, internal, t ? "ok" : "no texture bound");
    }
    if (!t) return;
    const int face = t->target == GL_TEXTURE_CUBE_MAP ? (int)(target - GL_TEXTURE_CUBE_MAP_POSITIVE_X) : 0;
    const TexFormat& f = *tex_format(internal, format, type);
    // The size of level 0 this level implies (when there is no storage yet).
    const bool is3d = t->target == GL_TEXTURE_3D;
    const int w0 = w << level, h0 = h << level, d0 = is3d ? d << level : 1;
    const bool same = t->tex && t->fmt.pixel == f.pixel && t->internal == internal && std::max(1, t->w >> level) == w &&
                      std::max(1, t->h >> level) == h && (!is3d || std::max(1, t->d >> level) == std::max(d, 1));
    if (!same) {
        if (level != 0 && t->tex) {
            warn_once("texture level " + std::to_string(level) + " of a different size or format");
            return;
        }
        t->fmt = f, t->internal = internal;
        const int levels = is_pow2(w0) && is_pow2(h0) && (d0 == 1 || is_pow2(d0)) ? full_levels(w0, h0, d0) : 1;
        create_storage(*t, w0, h0, d0, levels);
    } else if (level >= t->levels) {
        // A level beyond the chain: storage with the full chain, levels kept.
        id<MTLTexture> old = t->tex;
        create_storage(*t, t->w, t->h, t->d, full_levels(t->w, t->h, t->d));
        copy_texture(queue, old, t->tex, true);
    } else if (in_flight(t->last_use)) {
        rename(*t, !(level == 0 && t->levels == 1 && t->target != GL_TEXTURE_CUBE_MAP));
    }
    upload(*t, face, level, 0, 0, 0, w, h, std::max(d, 1), format, type, pixels, compressed);
}

void Backend::tex_sub_image(GLenum target, int level, int x, int y, int z, int w, int h, int d, GLenum format,
                            GLenum type, const void* pixels, bool compressed, uint32_t) {
    Texture* t = bound_texture(target);
    if (!t || !t->tex) return;
    const int face = t->target == GL_TEXTURE_CUBE_MAP ? (int)(target - GL_TEXTURE_CUBE_MAP_POSITIVE_X) : 0;
    if (in_flight(t->last_use)) {
        const bool whole = level == 0 && t->levels == 1 && x == 0 && y == 0 && z == 0 && w == t->w && h == t->h &&
                           std::max(d, 1) == std::max(t->d, 1) && t->target != GL_TEXTURE_CUBE_MAP;
        rename(*t, !whole);
    }
    upload(*t, face, level, x, y, z, w, h, std::max(d, 1), format, type, pixels, compressed);
}

void Backend::tex_parameter(GLenum target, GLenum pname, float v) {
    Texture* t = bound_texture(target);
    if (!t) return;
    const GLenum e = (GLenum)v;
    switch (pname) {
    case GL_TEXTURE_MIN_FILTER: t->min_filter = e; break;
    case GL_TEXTURE_MAG_FILTER: t->mag_filter = e; break;
    case GL_TEXTURE_WRAP_S: t->wrap_s = e; break;
    case GL_TEXTURE_WRAP_T: t->wrap_t = e; break;
    case GL_TEXTURE_WRAP_R: t->wrap_r = e; break;
    case GL_TEXTURE_BASE_LEVEL: t->base_level = (int)v; break;
    case GL_TEXTURE_MAX_LEVEL: t->max_level = (int)v; break;
    case GL_TEXTURE_MIN_LOD: t->min_lod = v; break;
    case GL_TEXTURE_MAX_LOD: t->max_lod = v; break;
    case GL_TEXTURE_MAX_ANISOTROPY_EXT:
        t->max_aniso = v;
        if (v > 1) {
            static bool told;
            if (!told) std::fprintf(stderr, "metal: the game asks for %gx anisotropic filtering\n", (double)v);
            told = true;
        }
        break;
    case GL_TEXTURE_COMPARE_MODE: t->compare_mode = e; break;
    case GL_TEXTURE_COMPARE_FUNC: t->compare_func = e; break;
    case GL_DEPTH_TEXTURE_MODE: case GL_GENERATE_MIPMAP: break;
    default: warn_once("texture parameter " + hex(pname)); return;
    }
    t->sampler = nil, t->shadow_sampler = nil;
}

id<MTLSamplerState> Backend::sampler_for(Texture& t, bool shadow) {
    // forced_aniso (SCIMITAR_ANISO=N at start, then the F4 menu): at least
    // N-times anisotropic filtering for the linearly filtered, mipmapped
    // textures (the games ask for none: their OGLState starts at 1 and no option
    // raises it). SCIMITAR_ANISO_FROM=F (for comparisons) applies it from frame F on.
    static const uint64_t forced_from = [] {
        const char* v = std::getenv("SCIMITAR_ANISO_FROM");
        return v ? std::strtoull(v, nullptr, 10) : 0ull;
    }();
    const long forced_aniso = frame_ >= forced_from ? forced_aniso_level() : 1;
    __strong id<MTLSamplerState>& cached = shadow ? t.shadow_sampler : t.sampler;
    if (t.sampler_forced_aniso != forced_aniso) t.sampler = nil, t.shadow_sampler = nil, t.sampler_forced_aniso = forced_aniso;
    if (cached) return cached;
    const bool mip = t.levels > 1;
    uint32_t minf = 0, mipf = 0;  // min: 0 nearest 1 linear; mip: 0 none 1 nearest 2 linear
    switch (t.min_filter) {
    case GL_NEAREST: minf = 0; break;
    case GL_LINEAR: minf = 1; break;
    case GL_NEAREST_MIPMAP_NEAREST: minf = 0, mipf = 1; break;
    case GL_LINEAR_MIPMAP_NEAREST: minf = 1, mipf = 1; break;
    case GL_NEAREST_MIPMAP_LINEAR: minf = 0, mipf = 2; break;
    default: minf = 1, mipf = 2; break;
    }
    if (!mip) mipf = 0;
    const uint32_t mag = t.mag_filter == GL_NEAREST ? 0 : 1;
    long wanted = std::lround(t.max_aniso);
    if (minf == 1 && mipf != 0 && !shadow) wanted = std::max(wanted, forced_aniso);
    const uint32_t aniso = (uint32_t)std::clamp(wanted, 1L, 16L);
    const uint32_t lo = (uint32_t)std::clamp(t.base_level, 0, 31), hi = (uint32_t)std::clamp(t.max_level, 0, 31);
    const uint32_t k[9] = {minf, mipf, mag, t.wrap_s, t.wrap_t, t.wrap_r, aniso, lo | hi << 8,
                           shadow ? t.compare_func : 0};
    const uint64_t key = hash(k, sizeof k) & ~(1ull << 63);
    auto it = samplers_.find(key);
    if (it == samplers_.end()) {
        MTLSamplerDescriptor* sd = [MTLSamplerDescriptor new];
        sd.minFilter = minf ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
        sd.magFilter = mag ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
        sd.mipFilter = mipf == 0 ? MTLSamplerMipFilterNotMipmapped : mipf == 1 ? MTLSamplerMipFilterNearest : MTLSamplerMipFilterLinear;
        sd.sAddressMode = address(t.wrap_s), sd.tAddressMode = address(t.wrap_t), sd.rAddressMode = address(t.wrap_r);
        sd.maxAnisotropy = mipf ? aniso : 1;
        sd.lodMinClamp = (float)lo;
        sd.lodMaxClamp = (float)std::max(lo, hi);
        if (shadow) sd.compareFunction = compare(t.compare_func);
        it = samplers_.emplace(key, [dev newSamplerStateWithDescriptor:sd]).first;
    }
    return cached = it->second;
}

id<MTLTexture> Backend::sample_texture(Texture& t, bool shadow) {
    if (shadow) return t.tex;
    return t.sample_view ?: t.tex;
}

// Bindings keep the deleted name, as Apple's GL behaves: Brotherhood deletes a
// bound render target, generates the same name again and specifies it without
// binding it (glDeleteTextures, glGenTextures, glTexImage2D), which on Apple's
// GL lands in the new texture.
void Backend::delete_textures(int n, const GLuint* ids) {
    for (int k = 0; k < n; k++) {
        textures.erase(ids[k]);  // Metal keeps it alive while command buffers use it
    }
}

// ---- framebuffers and render passes ----

void Backend::renderbuffer_storage(GLenum internal, int w, int h, int samples) {
    if (!st.renderbuffer) return;
    Renderbuffer& r = renderbuffers[st.renderbuffer];
    renderbuffer_format(internal, &r.fmt);
    if (samples > 1) warn_once("multisampled renderbuffers are single-sampled");
    MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:r.fmt.pixel width:std::max(w, 1)
                                                                                 height:std::max(h, 1) mipmapped:NO];
    const bool ds = r.fmt.depth || r.fmt.stencil;
    td.storageMode = ds ? MTLStorageModePrivate : MTLStorageModeShared;
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    r.tex = [dev newTextureWithDescriptor:td];
    r.w = w, r.h = h, r.samples = samples;
}

void Backend::framebuffer_attach(GLenum attachment, uint8_t kind, GLuint name, GLenum textarget, int level) {
    if (!st.draw_fb) return;
    Framebuffer& fb = framebuffers[st.draw_fb];
    Attachment a;
    if (name) {
        a.kind = kind, a.name = name, a.level = level;
        if (textarget >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && textarget <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z)
            a.slice = (int)(textarget - GL_TEXTURE_CUBE_MAP_POSITIVE_X);
    }
    if (attachment >= GL_COLOR_ATTACHMENT0_EXT && attachment <= GL_COLOR_ATTACHMENT3_EXT) fb.color[attachment - GL_COLOR_ATTACHMENT0_EXT] = a;
    else if (attachment == GL_DEPTH_ATTACHMENT_EXT) fb.depth = a;
    else if (attachment == GL_STENCIL_ATTACHMENT_EXT) fb.stencil = a;
    else if (attachment == 0x821A) fb.depth = fb.stencil = a;  // GL_DEPTH_STENCIL_ATTACHMENT
}

GLenum Backend::check_framebuffer() { return GL_FRAMEBUFFER_COMPLETE_EXT; }

id<MTLTexture> Backend::attachment_texture(const Attachment& a) {
    if (a.kind == 1) {
        auto it = textures.find(a.name);
        return it == textures.end() ? nil : it->second.tex;
    }
    if (a.kind == 2) {
        auto it = renderbuffers.find(a.name);
        return it == renderbuffers.end() ? nil : it->second.tex;
    }
    return nil;
}

// The attachments a framebuffer draws to. Only colour attachment 0: without
// glDrawBuffers (the game does not import it) GL draws to it alone.
PassTargets Backend::targets(GLuint fb) {
    PassTargets t;
    if (!fb) {
        t.color = default_color_, t.depth = default_ds_, t.stencil = default_ds_;
        return t;
    }
    auto it = framebuffers.find(fb);
    if (it == framebuffers.end()) return t;
    const Framebuffer& f = it->second;
    t.color = attachment_texture(f.color[0]);
    t.color_level = f.color[0].level, t.color_slice = f.color[0].slice;
    // glDrawBuffers: colour attachments 1.. as well, when they match in size.
    for (int k = 0; k + 1 < st.draw_buffers && k < kMoreColors; k++) {
        id<MTLTexture> m = attachment_texture(f.color[1 + k]);
        if (!m || !t.color) continue;
        const int mw = std::max<int>(1, (int)m.width >> f.color[1 + k].level);
        const int mh = std::max<int>(1, (int)m.height >> f.color[1 + k].level);
        if (mw != t.width() || mh != t.height()) {
            warn_once("draw buffers of different sizes: the smaller dropped");
            continue;
        }
        t.more[k] = m, t.more_level[k] = f.color[1 + k].level, t.more_slice[k] = f.color[1 + k].slice;
    }
    id<MTLTexture> d = attachment_texture(f.depth), s = attachment_texture(f.stencil);
    if (d && (d.pixelFormat == MTLPixelFormatDepth32Float || d.pixelFormat == MTLPixelFormatDepth32Float_Stencil8)) t.depth = d;
    if (s && s.pixelFormat == MTLPixelFormatDepth32Float_Stencil8) t.stencil = s;
    // Attachments must match in size (EXT framebuffers are incomplete otherwise).
    if (t.color && t.depth && ((int)t.depth.width != t.width() || (int)t.depth.height != t.height())) {
        warn_once("framebuffer attachments of different sizes: depth dropped");
        t.depth = nil, t.stencil = nil;
    }
    return t;
}

void Backend::end_pass() {
    if (!enc_) return;
    [enc_ endEncoding];
    enc_ = nil;
    if (dumping_) dump_pass_ending();
    pass_ = {};
}

void Backend::begin_pass(const PassTargets& t) {
    end_pass();
    // Hands the GPU what is encoded so far once it is a fair amount of work,
    // at a pass boundary (no extra loads and stores): it is busy while the
    // rest is encoded, so a wait on a query or the frame finishes sooner.
    static const uint32_t commit_draws = [] {
        const char* e = std::getenv("SCIMITAR_COMMIT_DRAWS");
        return e ? (uint32_t)std::atoi(e) : 300u;
    }();
    if (commit_draws && draws_since_commit_ >= commit_draws) commit(false, nil);
    MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
    const bool clearing = pending_clear_.targets == t;
    if (t.color) {
        rp.colorAttachments[0].texture = t.color;
        rp.colorAttachments[0].level = t.color_level;
        if (t.color.textureType == MTLTextureType3D) rp.colorAttachments[0].depthPlane = t.color_slice;
        else rp.colorAttachments[0].slice = t.color_slice;
        rp.colorAttachments[0].loadAction = clearing && pending_clear_.color ? MTLLoadActionClear : MTLLoadActionLoad;
        rp.colorAttachments[0].clearColor = pending_clear_.color_value;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    }
    for (int k = 0; k < kMoreColors; k++) {  // glDrawBuffers: GL clears every draw buffer
        if (!t.more[k]) continue;
        MTLRenderPassColorAttachmentDescriptor* ca = rp.colorAttachments[1 + k];
        ca.texture = t.more[k];
        ca.level = t.more_level[k];
        if (t.more[k].textureType == MTLTextureType3D) ca.depthPlane = t.more_slice[k];
        else ca.slice = t.more_slice[k];
        ca.loadAction = clearing && pending_clear_.color ? MTLLoadActionClear : MTLLoadActionLoad;
        ca.clearColor = pending_clear_.color_value;
        ca.storeAction = MTLStoreActionStore;
    }
    if (t.depth) {
        rp.depthAttachment.texture = t.depth;
        rp.depthAttachment.loadAction = clearing && pending_clear_.depth ? MTLLoadActionClear : MTLLoadActionLoad;
        rp.depthAttachment.clearDepth = pending_clear_.depth_value;
        rp.depthAttachment.storeAction = MTLStoreActionStore;
    }
    if (t.stencil) {
        rp.stencilAttachment.texture = t.stencil;
        rp.stencilAttachment.loadAction = clearing && pending_clear_.stencil ? MTLLoadActionClear : MTLLoadActionLoad;
        rp.stencilAttachment.clearStencil = pending_clear_.stencil_value;
        rp.stencilAttachment.storeAction = MTLStoreActionStore;
    }
    if (clearing) pending_clear_ = {};
    FrameSlot& s = slot();
    if (s.visibility.empty()) s.visibility.push_back([dev newBufferWithLength:65536 options:MTLResourceStorageModeShared]);
    pass_vis_ = s.visibility[s.vis_cur];
    rp.visibilityResultBuffer = pass_vis_;
    enc_ = [cb_ renderCommandEncoderWithDescriptor:rp];
    if (dumping_) dump_pass_begun(t, rp);
    pass_ = t;
    ec_ = EncoderCache{};
    pass_drawn_ = false;
    n_passes++;
    if (active_query) bind_visibility_slot();
}

bool Backend::ensure_pass() {
    const PassTargets t = targets(st.draw_fb);
    if (!t.color && !t.depth && !t.stencil) return false;
    if (enc_ && pass_ == t) return true;
    const bool pending = pending_clear_.targets.color || pending_clear_.targets.depth || pending_clear_.targets.stencil;
    if (pending && !(pending_clear_.targets == t)) {
        const PassTargets p = pending_clear_.targets;
        begin_pass(p);  // the clear of an earlier framebuffer still happens
    }
    begin_pass(t);
    return true;
}

void Backend::bind_visibility_slot() {
    FrameSlot& s = slot();
    if (s.vis_used + 8 > 65536) {
        // This pass's visibility buffer is full: the next one, in a new pass.
        s.vis_cur++, s.vis_used = 0;
        if (s.vis_cur == s.visibility.size())
            s.visibility.push_back([dev newBufferWithLength:65536 options:MTLResourceStorageModeShared]);
        const PassTargets t = pass_;
        begin_pass(t);  // calls back here with room
        return;
    }
    Query& q = queries[active_query];
    q.slots.push_back({pass_vis_, (uint32_t)s.vis_used});
    [enc_ setVisibilityResultMode:MTLVisibilityResultModeCounting offset:s.vis_used];
    s.vis_used += 8;
}

// ---- clears ----

void Backend::clear(GLbitfield mask) {
    n_clears++;
    const PassTargets t = targets(st.draw_fb);
    if (!t.color) mask &= ~GL_COLOR_BUFFER_BIT;
    if (!t.depth) mask &= ~GL_DEPTH_BUFFER_BIT;
    if (!t.stencil) mask &= ~GL_STENCIL_BUFFER_BIT;
    if (!mask) return;
    const bool full_scissor = !st.scissor_test || (st.scissor[0] <= 0 && st.scissor[1] <= 0 &&
                                                   st.scissor[0] + st.scissor[2] >= t.width() &&
                                                   st.scissor[1] + st.scissor[3] >= t.height());
    const bool as_load = full_scissor && (!(mask & GL_COLOR_BUFFER_BIT) || st.color_mask == 0xf) &&
                         (!(mask & GL_DEPTH_BUFFER_BIT) || st.depth_mask) &&
                         (!(mask & GL_STENCIL_BUFFER_BIT) || (st.stencil_write_mask & 0xff) == 0xff);
    if (!as_load || (enc_ && pass_ == t && pass_drawn_)) {
        clear_with_draw(mask);
        return;
    }
    // Before any draw in the pass: a load action of the pass to come.
    if (enc_ && pass_ == t) end_pass();
    const bool pending = pending_clear_.targets.color || pending_clear_.targets.depth || pending_clear_.targets.stencil;
    if (pending && !(pending_clear_.targets == t)) {
        const PassTargets p = pending_clear_.targets;
        begin_pass(p);
        end_pass();
    }
    PendingClear& c = pending_clear_;
    c.targets = t;
    if (mask & GL_COLOR_BUFFER_BIT) {
        c.color = true;
        c.color_value = MTLClearColorMake(st.clear_color[0], st.clear_color[1], st.clear_color[2], st.clear_color[3]);
    }
    if (mask & GL_DEPTH_BUFFER_BIT) c.depth = true, c.depth_value = std::clamp(st.clear_depth, 0.0, 1.0);
    if (mask & GL_STENCIL_BUFFER_BIT) c.stencil = true, c.stencil_value = (uint32_t)st.clear_stencil & 0xff;
}

void Backend::clear_with_draw(GLbitfield mask) {
    if (!ensure_pass()) return;
    const PassTargets& t = pass_;
    const uint8_t cmask = (mask & GL_COLOR_BUFFER_BIT) ? st.color_mask : 0;
    id<MTLRenderPipelineState> ps =
        builtin_pipeline("clear_vs", "clear_fs", t.color ? t.color.pixelFormat : MTLPixelFormatInvalid,
                         t.depth ? t.depth.pixelFormat : MTLPixelFormatInvalid,
                         t.stencil ? t.stencil.pixelFormat : MTLPixelFormatInvalid, cmask, true, &t);
    [enc_ setRenderPipelineState:ps];
    MTLDepthStencilDescriptor* dd = [MTLDepthStencilDescriptor new];
    dd.depthCompareFunction = MTLCompareFunctionAlways;
    dd.depthWriteEnabled = (mask & GL_DEPTH_BUFFER_BIT) && st.depth_mask;
    if (mask & GL_STENCIL_BUFFER_BIT) {
        MTLStencilDescriptor* sd = [MTLStencilDescriptor new];
        sd.stencilCompareFunction = MTLCompareFunctionAlways;
        sd.depthStencilPassOperation = MTLStencilOperationReplace;
        sd.writeMask = st.stencil_write_mask & 0xff;
        dd.frontFaceStencil = sd, dd.backFaceStencil = sd;
    }
    const uint32_t k[3] = {(uint32_t)dd.depthWriteEnabled, (mask & GL_STENCIL_BUFFER_BIT) != 0, st.stencil_write_mask & 0xff};
    const uint64_t key = hash(k, sizeof k) | 1ull << 63;
    auto it = depth_stencil_.find(key);
    if (it == depth_stencil_.end()) it = depth_stencil_.emplace(key, [dev newDepthStencilStateWithDescriptor:dd]).first;
    [enc_ setDepthStencilState:it->second];
    [enc_ setStencilReferenceValue:(uint32_t)st.clear_stencil & 0xff];
    const int w = t.width(), h = t.height();
    [enc_ setViewport:(MTLViewport){0, 0, (double)w, (double)h, 0, 1}];
    MTLScissorRect sc = {0, 0, (NSUInteger)w, (NSUInteger)h};
    if (st.scissor_test) {
        const int x0 = std::clamp(st.scissor[0], 0, w), y0 = std::clamp(st.scissor[1], 0, h);
        const int x1 = std::clamp(st.scissor[0] + st.scissor[2], 0, w), y1 = std::clamp(st.scissor[1] + st.scissor[3], 0, h);
        if (x1 <= x0 || y1 <= y0) {
            ec_ = EncoderCache{};
            return;
        }
        sc = {(NSUInteger)x0, (NSUInteger)y0, (NSUInteger)(x1 - x0), (NSUInteger)(y1 - y0)};
    }
    [enc_ setScissorRect:sc];
    [enc_ setCullMode:MTLCullModeNone];
    [enc_ setTriangleFillMode:MTLTriangleFillModeFill];
    [enc_ setDepthBias:0 slopeScale:0 clamp:0];
    ClearIn c = {{st.clear_color[0], st.clear_color[1], st.clear_color[2], st.clear_color[3]},
                 (float)std::clamp(st.clear_depth, 0.0, 1.0), {}};
    [enc_ setVertexBytes:&c length:sizeof c atIndex:0];
    [enc_ setFragmentBytes:&c length:sizeof c atIndex:0];
    [enc_ drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    pass_drawn_ = true;
    ec_ = EncoderCache{};  // the encoder's state no longer matches the cache
}

id<MTLRenderPipelineState> Backend::builtin_pipeline(const char* vs, const char* fs, MTLPixelFormat color,
                                                     MTLPixelFormat depth, MTLPixelFormat stencil, uint8_t mask,
                                                     bool, const PassTargets* more) {
    // The pass's other colour attachments (glDrawBuffers), left unwritten:
    // a pipeline must name every attachment of the pass it draws in.
    MTLPixelFormat mf[kMoreColors] = {};
    for (int k = 0; k < kMoreColors; k++) mf[k] = more ? more->more_format(k) : MTLPixelFormatInvalid;
    char key[160];
    std::snprintf(key, sizeof key, "%s/%s/%d/%d/%d/%d/%d/%d/%d", vs, fs, (int)color, (int)depth, (int)stencil, mask,
                  (int)mf[0], (int)mf[1], (int)mf[2]);
    auto it = builtin_pipelines_.find(key);
    if (it != builtin_pipelines_.end()) return it->second;
    MTLRenderPipelineDescriptor* pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = [builtin_ newFunctionWithName:@(vs)];
    pd.fragmentFunction = [builtin_ newFunctionWithName:@(fs)];
    pd.colorAttachments[0].pixelFormat = color;
    pd.colorAttachments[0].writeMask = (mask & 1 ? MTLColorWriteMaskRed : 0) | (mask & 2 ? MTLColorWriteMaskGreen : 0) |
                                       (mask & 4 ? MTLColorWriteMaskBlue : 0) | (mask & 8 ? MTLColorWriteMaskAlpha : 0);
    for (int k = 0; k < kMoreColors; k++) {
        pd.colorAttachments[1 + k].pixelFormat = mf[k];
        pd.colorAttachments[1 + k].writeMask = MTLColorWriteMaskNone;
    }
    pd.depthAttachmentPixelFormat = depth;
    pd.stencilAttachmentPixelFormat = stencil;
    NSError* err = nil;
    id<MTLRenderPipelineState> ps = [dev newRenderPipelineStateWithDescriptor:pd error:&err];
    if (!ps) fatal("metal: built-in pipeline %s: %s", key, err.localizedDescription.UTF8String);
    builtin_pipelines_[key] = ps;
    return ps;
}

// ---- programs ----

void Backend::program_string(GLenum target, const std::string& text) {
    const GLuint name = target == GL_VERTEX_PROGRAM_ARB ? st.vp : st.fp;
    if (!name) return;
    Program& p = programs[name];
    p.target = target;
    p.base = arb::translate(text);
    p.ok = p.base.ok;
    p.vertex_fns.clear();
    for (auto& f : p.fragment_fns) f = nil;
    p.local_gen++;
    p.text = text;
    // A label for frame dumps: the shader's source file, else its program name.
    p.label = "program " + std::to_string(name);
    for (const char* key : {"#program ", "source file: "}) {
        const size_t at = text.find(key);
        if (at == std::string::npos) continue;
        std::string v = text.substr(at + std::strlen(key), text.find('\n', at) - at - std::strlen(key));
        const size_t slash = v.find_last_of("\\/");
        if (slash != std::string::npos) v = v.substr(slash + 1);
        while (!v.empty() && (v.back() == '\r' || v.back() == ' ')) v.pop_back();
        p.label = v + " (" + std::to_string(name) + ")";
    }
    if (!p.ok) std::fprintf(stderr, "metal: program %u not translated: %s\n", name, p.base.error.c_str());
}

void Backend::program_env(GLenum target, uint32_t index, uint32_t count, const float* v) {
    const int s = target == GL_VERTEX_PROGRAM_ARB ? 0 : 1;
    if (!v || index >= kEnv) return;
    count = std::min<uint32_t>(count, kEnv - index);
    std::memcpy(env[s][index], v, 16 * count);
    env_gen[s]++;
}

void Backend::program_local(GLenum target, uint32_t index, uint32_t count, const float* v) {
    const GLuint name = target == GL_VERTEX_PROGRAM_ARB ? st.vp : st.fp;
    if (!name || !v || index >= kLocals) return;
    Program& p = programs[name];
    count = std::min<uint32_t>(count, kLocals - index);
    if (p.locals.size() < index + count) p.locals.resize(index + count, {0, 0, 0, 0});
    std::memcpy(p.locals[index].data(), v, 16 * count);
    p.local_gen++;
}

id<MTLLibrary> Backend::library(const std::string& msl) {
    auto it = libraries_.find(msl);
    if (it != libraries_.end()) return it->second;
    MTLCompileOptions* o = [MTLCompileOptions new];
    o.preserveInvariance = YES;
    NSError* err = nil;
    const auto t0 = std::chrono::steady_clock::now();
    id<MTLLibrary> lib = [dev newLibraryWithSource:@(msl.c_str()) options:o error:&err];
    compile_ms = compile_ms + std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (!lib) std::fprintf(stderr, "metal: MSL does not compile: %s\n%s\n", err.localizedDescription.UTF8String, msl.c_str());
    libraries_[msl] = lib;
    return lib;
}

const arb::Program* Backend::vertex_variant(Program& p, uint32_t extra, uint32_t uint_mask, uint32_t int_mask,
                                            id<MTLFunction>* fn) {
    const std::array<uint32_t, 3> key = {extra, uint_mask, int_mask};
    auto it = p.vertex_fns.find(key);
    if (it == p.vertex_fns.end()) {
        arb::Options o;
        o.extra_varyings = extra, o.uint_attribs = uint_mask, o.int_attribs = int_mask;
        arb::Program v = arb::translate(p.text, o);
        id<MTLFunction> f = v.ok ? function(v.msl, -1) : nil;
        it = p.vertex_fns.emplace(key, std::make_pair(f, std::move(v))).first;
    } else if (!it->second.first && it->second.second.ok) {
        it->second.first = function(it->second.second.msl, -1);  // was still compiling
    }
    *fn = it->second.first;
    return &it->second.second;
}

id<MTLFunction> Backend::function(const std::string& msl, int alpha) {
    std::string key = msl;
    key += '\0';
    key += (char)alpha;
    auto it = functions_.find(key);
    if (it != functions_.end()) return it->second;
    // Compiled on another thread (shader_cache.mm): nil while it compiles
    // past this frame's wait budget, and the draw is skipped.
    bool pending = false;
    double waited = 0;
    id<MTLFunction> f = shader_cache().function(dev, msl, alpha, &pending, &waited);
    compile_ms = compile_ms + waited;
    if (pending) return nil;
    functions_[key] = f;
    return f;
}

id<MTLFunction> Backend::fragment_function(Program& p, int alpha) {
    __strong id<MTLFunction>& f = p.fragment_fns[alpha];
    if (!f) f = function(p.base.msl, alpha);
    return f;
}

id<MTLDepthStencilState> Backend::depth_stencil_state(bool has_depth, bool has_stencil) {
    const bool dt = has_depth && st.depth_test, stt = has_stencil && st.stencil_test;
    const uint32_t k[10] = {dt, dt ? st.depth_func : 0, dt && st.depth_mask, stt, stt ? st.stencil_func : 0,
                            stt ? st.stencil_value_mask & 0xff : 0, stt ? st.stencil_write_mask & 0xff : 0,
                            stt ? st.stencil_fail : 0, stt ? st.stencil_zfail : 0, stt ? st.stencil_zpass : 0};
    const uint64_t key = hash(k, sizeof k) & ~(1ull << 63);  // built-in states use the top bit
    auto it = depth_stencil_.find(key);
    if (it != depth_stencil_.end()) return it->second;
    MTLDepthStencilDescriptor* dd = [MTLDepthStencilDescriptor new];
    dd.depthCompareFunction = dt ? compare(st.depth_func) : MTLCompareFunctionAlways;
    dd.depthWriteEnabled = dt && st.depth_mask;
    if (stt) {
        MTLStencilDescriptor* sd = [MTLStencilDescriptor new];
        sd.stencilCompareFunction = compare(st.stencil_func);
        sd.stencilFailureOperation = stencil_op(st.stencil_fail);
        sd.depthFailureOperation = stencil_op(st.stencil_zfail);
        sd.depthStencilPassOperation = stencil_op(st.stencil_zpass);
        sd.readMask = st.stencil_value_mask & 0xff;
        sd.writeMask = st.stencil_write_mask & 0xff;
        dd.frontFaceStencil = sd, dd.backFaceStencil = sd;
    }
    return depth_stencil_[key] = [dev newDepthStencilStateWithDescriptor:dd];
}

// ---- draws ----

namespace {

// Vertex fetch for one draw: Metal bindings for the attributes the program reads.
struct Binding {
    id<MTLBuffer> buffer = nil;
    NSUInteger offset = 0;
    uint32_t stride = 0;
    bool constant = false;
    GLuint gl_buffer = 0;
    uint32_t base = 0;  // pointer of the binding's first byte (GL side)
};
struct AttribUse {
    int index;
    MTLVertexFormat format;
    int binding;
    uint32_t offset;
};

}  // namespace

void Backend::set_dynamic_state(int tw, int th) {
    MTLCullMode cull = MTLCullModeNone;
    if (st.cull) cull = st.cull_face == GL_FRONT ? MTLCullModeFront : MTLCullModeBack;
    if ((int)cull != ec_.cull) [enc_ setCullMode:cull], ec_.cull = (int)cull;
    // Rows are kept in GL's order (y negated in the vertex programs), which
    // mirrors the winding.
    const int winding = st.front_face == GL_CCW ? (int)MTLWindingClockwise : (int)MTLWindingCounterClockwise;
    if (winding != ec_.winding) [enc_ setFrontFacingWinding:(MTLWinding)winding], ec_.winding = winding;
    const int fill = st.polygon_mode == GL_LINE ? (int)MTLTriangleFillModeLines : (int)MTLTriangleFillModeFill;
    if (fill != ec_.fill) [enc_ setTriangleFillMode:(MTLTriangleFillMode)fill], ec_.fill = fill;
    const float bias[3] = {st.polygon_offset ? st.offset_units : 0, st.polygon_offset ? st.offset_factor : 0, 0};
    if (std::memcmp(bias, ec_.bias, sizeof bias) != 0) {
        [enc_ setDepthBias:bias[0] slopeScale:bias[1] clamp:0];
        std::memcpy(ec_.bias, bias, sizeof bias);
    }
    // GL's viewport and scissor rectangles count rows from the bottom, which
    // is row 0 of our images: they are used as they are.
    const MTLViewport vp = {(double)(st.viewport[0] + shift_x), (double)(st.viewport[1] + shift_y), (double)st.viewport[2],
                            (double)st.viewport[3], std::clamp(st.depth_near, 0.0, 1.0), std::clamp(st.depth_far, 0.0, 1.0)};
    if (std::memcmp(&vp, &ec_.viewport, sizeof vp) != 0) [enc_ setViewport:vp], ec_.viewport = vp;
    MTLScissorRect sc = {0, 0, (NSUInteger)tw, (NSUInteger)th};
    if (st.scissor_test) {
        const int sx = st.scissor[0] + shift_x, sy = st.scissor[1] + shift_y;
        const int x0 = std::clamp(sx, 0, tw), y0 = std::clamp(sy, 0, th);
        const int x1 = std::clamp(sx + st.scissor[2], 0, tw), y1 = std::clamp(sy + st.scissor[3], 0, th);
        sc = {(NSUInteger)x0, (NSUInteger)y0, (NSUInteger)std::max(0, x1 - x0), (NSUInteger)std::max(0, y1 - y0)};
    }
    if (std::memcmp(&sc, &ec_.scissor, sizeof sc) != 0) [enc_ setScissorRect:sc], ec_.scissor = sc;
    const uint32_t ref = (uint32_t)st.stencil_ref & 0xff;
    if (ref != ec_.stencil_ref) [enc_ setStencilReferenceValue:ref], ec_.stencil_ref = ref;
    if (std::memcmp(st.blend_color, ec_.blend_color, 16) != 0) {
        [enc_ setBlendColorRed:st.blend_color[0] green:st.blend_color[1] blue:st.blend_color[2] alpha:st.blend_color[3]];
        std::memcpy(ec_.blend_color, st.blend_color, 16);
    }
}

void Backend::bind_constants(const arb::Program& p, Program& prog, int stage) {
    const Program*& cached = stage == 0 ? ec_.vconst_prog : ec_.fconst_prog;
    uint64_t& gen = stage == 0 ? ec_.vconst_gen : ec_.fconst_gen;
    uint64_t& local = stage == 0 ? ec_.vconst_local : ec_.fconst_local;
    if (p.params.empty()) return;
    if (cached == &prog && gen == env_gen[stage] && local == prog.local_gen) return;
    const Slice s = ring(16 * p.params.size());
    float* out = (float*)s.ptr;
    for (size_t k = 0; k < p.params.size(); k++) {
        const arb::ParamRef& r = p.params[k];
        const float* src = r.value;
        static const float zero[4] = {0, 0, 0, 0};
        if (r.kind == arb::ParamRef::Env) src = r.index < kEnv ? env[stage][r.index] : zero;
        else if (r.kind == arb::ParamRef::Local) src = r.index < prog.locals.size() ? prog.locals[r.index].data() : zero;
        std::memcpy(out + 4 * k, src, 16);
    }
    // The same ring buffer as the last draw's: only the offset changes, which
    // spares Metal re-adding the buffer to the encoder's resource list.
    __strong id<MTLBuffer>& bound = ec_.const_buf[stage];
    if (stage == 0) {
        if (bound == s.buffer) [enc_ setVertexBufferOffset:s.offset atIndex:0];
        else [enc_ setVertexBuffer:s.buffer offset:s.offset atIndex:0];
    } else {
        if (bound == s.buffer) [enc_ setFragmentBufferOffset:s.offset atIndex:0];
        else [enc_ setFragmentBuffer:s.buffer offset:s.offset atIndex:0];
    }
    bound = s.buffer;
    cached = &prog, gen = env_gen[stage], local = prog.local_gen;
}

void Backend::draw(GLenum mode, int first, int count, GLenum index_type, uint32_t indices, bool indexed) {
    if (count <= 0) return;
    auto vit = programs.find(st.vp), fit = programs.find(st.fp);
    if (!st.vp_enabled || !st.fp_enabled || vit == programs.end() || fit == programs.end() || !vit->second.ok ||
        !fit->second.ok) {
        n_skipped++;
        warn_once("draw without vertex and fragment programs skipped");
        return;
    }
    Program& vprog = vit->second;
    Program& fprog = fit->second;
    if (st.cull && st.cull_face == GL_FRONT_AND_BACK && mode >= GL_TRIANGLES) return;
    if (!ensure_pass()) return;
    const int tw = pass_.width(), th = pass_.height();
    if (st.scissor_test && (st.scissor[2] <= 0 || st.scissor[3] <= 0)) return;

    // Indices: GL's, or generated for quads, fans and loops.
    MTLPrimitiveType prim = MTLPrimitiveTypeTriangle;
    bool convert = false;
    switch (mode) {
    case GL_POINTS: prim = MTLPrimitiveTypePoint; break;
    case GL_LINES: prim = MTLPrimitiveTypeLine; break;
    case GL_LINE_STRIP: prim = MTLPrimitiveTypeLineStrip; break;
    case GL_TRIANGLES: prim = MTLPrimitiveTypeTriangle; break;
    case GL_TRIANGLE_STRIP: case GL_QUAD_STRIP: prim = MTLPrimitiveTypeTriangleStrip; break;
    case GL_LINE_LOOP: prim = MTLPrimitiveTypeLine, convert = true; break;
    default: prim = MTLPrimitiveTypeTriangle, convert = true; break;  // quads, fans, polygons
    }
    // The GL index data (CPU-readable: buffers live in guest memory).
    const uint8_t* index_src = nullptr;
    Buffer* ibo = nullptr;
    if (indexed) {
        if (st.element_buffer) {
            ibo = &buffers[st.element_buffer];
            if (!ibo->block) return;
            index_src = mem->ptr(ibo->block.guest + indices);
        } else {
            index_src = mem->ptr(indices);
        }
    }
    auto index_at = [&](int k) -> uint32_t {
        if (!indexed) return (uint32_t)(first + k);
        switch (index_type) {
        case GL_UNSIGNED_BYTE: return index_src[k];
        case GL_UNSIGNED_SHORT: return ((const uint16_t*)index_src)[k];
        default: return ((const uint32_t*)index_src)[k];
        }
    };
    // The vertex range, needed for client arrays.
    uint32_t vmax = 0;
    bool client = false;
    const arb::Program& vbase = vprog.base;
    for (int a = 0; a < kAttribs; a++)
        if ((vbase.attribs >> a & 1) && st.attribs[a].enabled && !st.attribs[a].buffer) client = true;
    if (client) {
        if (indexed)
            for (int k = 0; k < count; k++) vmax = std::max(vmax, index_at(k));
        else
            vmax = (uint32_t)(first + count - 1);
    }

    // Vertex layout and bindings.
    Binding bindings[kAttribs];
    int nb = 0;
    AttribUse uses[kAttribs];
    int nu = 0;
    uint32_t uint_mask = 0, int_mask = 0;
    int const_binding = -1;
    std::vector<float> consts;
    for (int a = 0; a < kAttribs; a++) {
        if (!(vbase.attribs >> a & 1)) continue;
        const VertexAttrib& va = st.attribs[a];
        if (!va.enabled) {
            if (const_binding < 0) const_binding = nb++, bindings[const_binding].constant = true;
            uses[nu++] = {a, MTLVertexFormatFloat4, const_binding, (uint32_t)consts.size() * 4};
            consts.insert(consts.end(), va.current, va.current + 4);
            continue;
        }
        bool integer, is_signed;
        const MTLVertexFormat f = vertex_format(va, &integer, &is_signed);
        if (integer) (is_signed ? int_mask : uint_mask) |= 1u << a;
        const uint32_t n = va.size == GL_BGRA ? 4 : va.size;
        const uint32_t stride = va.stride ? va.stride : n * type_size(va.type);
        int b = -1;
        for (int k = 0; k < nb; k++)
            if (!bindings[k].constant && bindings[k].gl_buffer == va.buffer && bindings[k].stride == stride &&
                va.pointer >= bindings[k].base && va.pointer - bindings[k].base < stride)
                b = k;
        if (b < 0) {
            b = nb++;
            bindings[b].gl_buffer = va.buffer, bindings[b].stride = stride, bindings[b].base = va.pointer;
        } else if (va.pointer < bindings[b].base) {
            bindings[b].base = va.pointer;  // not reached: pointers are compared from the base up
        }
        uses[nu++] = {a, f, b, va.pointer};
    }
    for (int k = 0; k < nu; k++)
        if (!bindings[uses[k].binding].constant) uses[k].offset -= bindings[uses[k].binding].base;
    for (int k = 0; k < nb; k++) {
        Binding& b = bindings[k];
        if (b.constant) {
            const Slice s = ring(consts.size() * 4, 16);
            std::memcpy(s.ptr, consts.data(), consts.size() * 4);
            b.buffer = s.buffer, b.offset = s.offset, b.stride = (uint32_t)consts.size() * 4;
        } else if (b.gl_buffer) {
            Buffer& gb = buffers[b.gl_buffer];
            if (!gb.block) return;
            touch(gb);
            b.buffer = gb.block.buffer, b.offset = gb.block.offset + b.base;
        } else {
            // A client array: copy the vertices this draw reads.
            uint32_t span = 0;
            for (int u = 0; u < nu; u++)
                if (uses[u].binding == k) span = std::max(span, uses[u].offset + 16);
            const size_t n = (size_t)vmax * b.stride + span;
            const Slice s = ring(n, 16);
            std::memcpy(s.ptr, mem->ptr(b.base), n);
            b.buffer = s.buffer, b.offset = s.offset;
        }
    }

    // Programs and pipeline.
    const arb::Program& fbase = fprog.base;
    id<MTLFunction> vfn = nil;
    const arb::Program* vvar = vertex_variant(vprog, fbase.varyings & ~vbase.varyings, uint_mask, int_mask, &vfn);
    int alpha = 8;
    if (st.alpha_test && st.alpha_func != GL_ALWAYS) alpha = (int)(st.alpha_func - GL_NEVER) & 7;
    id<MTLFunction> ffn = fragment_function(fprog, alpha);
    if (!vfn || !ffn) {
        n_skipped++;
        return;
    }
    const MTLPixelFormat cf = pass_.color ? pass_.color.pixelFormat : MTLPixelFormatInvalid;
    const MTLPixelFormat df = pass_.depth ? pass_.depth.pixelFormat : MTLPixelFormatInvalid;
    const MTLPixelFormat sf = pass_.stencil ? pass_.stencil.pixelFormat : MTLPixelFormatInvalid;
    PipeDesc pdesc;
    std::memset(&pdesc, 0, sizeof pdesc);
    pdesc.formats[0] = (uint32_t)cf, pdesc.formats[1] = (uint32_t)df, pdesc.formats[2] = (uint32_t)sf;
    for (int k = 0; k < kMoreColors; k++) pdesc.formats[3 + k] = (uint32_t)pass_.more_format(k);
    if (st.blend) pdesc.blend[0] = 1, pdesc.blend[1] = st.blend_src, pdesc.blend[2] = st.blend_dst, pdesc.blend[3] = st.blend_eq;
    pdesc.blend[4] = st.color_mask;
    pdesc.nu = nu, pdesc.nb = nb;
    for (int k = 0; k < nu; k++)
        pdesc.uses[k][0] = (uint32_t)uses[k].index, pdesc.uses[k][1] = (uint32_t)uses[k].format,
        pdesc.uses[k][2] = (uint32_t)uses[k].binding, pdesc.uses[k][3] = uses[k].offset;
    for (int k = 0; k < nb; k++) pdesc.binds[k][0] = bindings[k].stride, pdesc.binds[k][1] = bindings[k].constant;
    std::string key;
    // Only the attributes and bindings in use (the whole PipeDesc is ~600
    // bytes, mostly zeros, and the key is hashed at every draw).
    const void* fns[2] = {(__bridge const void*)vfn, (__bridge const void*)ffn};
    const size_t head = offsetof(PipeDesc, uses);
    key.reserve(sizeof fns + head + 16 * nu + 8 * nb);
    key.append((const char*)fns, sizeof fns);
    key.append((const char*)&pdesc, head);
    key.append((const char*)pdesc.uses, 16 * (size_t)nu);
    key.append((const char*)pdesc.binds, 8 * (size_t)nb);
    id<MTLRenderPipelineState> ps;
    auto pit = pipelines_.find(key);
    if (pit != pipelines_.end()) {
        ps = pit->second;
    } else {
        MTLRenderPipelineDescriptor* pd = pipeline_descriptor(vfn, ffn, pdesc, fbase.color_outputs);
        if (id<MTLBinaryArchive> a = shader_cache().archive()) pd.binaryArchives = @[a];
        // Built on another thread (shader_cache.mm); the draw is skipped
        // while it compiles past this frame's wait budget.
        bool pending = false;
        double waited = 0;
        ps = shader_cache().pipeline(dev, key, pd, &pending, &waited);
        compile_ms = compile_ms + waited;
        if (pending) {
            n_skipped++;
            return;
        }
        pipelines_[key] = ps;
        n_pipelines++;
        const uint32_t vopts[3] = {fbase.varyings & ~vbase.varyings, uint_mask, int_mask};
        if (ps) shader_cache().record(vprog.text, fprog.text, vopts, alpha, pdesc);
    }
    if (!ps) {
        n_skipped++;
        return;
    }
    if (ps != ec_.pipeline) [enc_ setRenderPipelineState:ps], ec_.pipeline = ps;
    id<MTLDepthStencilState> ds = depth_stencil_state(pass_.depth != nil, pass_.stencil != nil);
    if (ds != ec_.depth_stencil) [enc_ setDepthStencilState:ds], ec_.depth_stencil = ds;
    set_dynamic_state(tw, th);

    for (int k = 0; k < nb; k++) {
        const int idx = kVertexBufferBase + k;
        if (bindings[k].buffer == ec_.vbuf[idx] && bindings[k].offset == ec_.voff[idx]) continue;
        if (bindings[k].buffer == ec_.vbuf[idx]) [enc_ setVertexBufferOffset:bindings[k].offset atIndex:idx];
        else [enc_ setVertexBuffer:bindings[k].buffer offset:bindings[k].offset atIndex:idx];
        ec_.vbuf[idx] = bindings[k].buffer, ec_.voff[idx] = bindings[k].offset;
    }
    bind_constants(*vvar, vprog, 0);
    bind_constants(fbase, fprog, 1);

    // Textures.
    id<MTLTexture> bound_tex[kUnits] = {};
    arb::FixedState fixed;
    fixed.alpha_ref = std::clamp(st.alpha_ref, 0.0f, 1.0f);
    for (int u = 0; u < kUnits; u++) {
        const arb::TexTarget tt = fbase.textures[u];
        if (tt == arb::TexTarget::None) continue;
        fixed.lod_bias[u] = st.units[u].lod_bias;
        const TexUnit& tu = st.units[u];
        GLuint name = 0;
        GLenum want = GL_TEXTURE_2D;
        id<MTLTexture> fallback = dummy_2d_;
        switch (tt) {
        case arb::TexTarget::T1D: name = tu.t1d, want = GL_TEXTURE_1D, fallback = dummy_1d_; break;
        case arb::TexTarget::T3D: name = tu.t3d, want = GL_TEXTURE_3D, fallback = dummy_3d_; break;
        case arb::TexTarget::Cube: name = tu.cube, want = GL_TEXTURE_CUBE_MAP, fallback = dummy_cube_; break;
        case arb::TexTarget::Rect: name = tu.rect, want = GL_TEXTURE_RECTANGLE_ARB; break;
        default: name = tu.t2d; break;
        }
        Texture* t = nullptr;
        if (name) {
            auto it = textures.find(name);
            if (it != textures.end() && it->second.tex && it->second.target == want) t = &it->second;
        }
        id<MTLTexture> tex;
        id<MTLSamplerState> smp;
        if (tt == arb::TexTarget::Shadow2D) {
            // A shadow unit without a depth texture reads as lit (see opengl.cpp).
            if (t && t->fmt.depth) tex = t->tex, smp = sampler_for(*t, true);
            else tex = lit_shadow_, smp = lit_sampler_;
        } else if (t) {
            tex = sample_texture(*t, false), smp = sampler_for(*t, false);
        } else {
            tex = fallback, smp = dummy_sampler_;
        }
        if (t) t->last_use = seg_;
        bound_tex[u] = tex;
        if (tex != ec_.tex[u]) [enc_ setFragmentTexture:tex atIndex:u], ec_.tex[u] = tex;
        if (smp != ec_.smp[u]) [enc_ setFragmentSamplerState:smp atIndex:u], ec_.smp[u] = smp;
    }
    if (!ec_.fixed_set || std::memcmp(&fixed, &ec_.fixed, sizeof fixed) != 0) {
        [enc_ setFragmentBytes:&fixed length:sizeof fixed atIndex:1];
        ec_.fixed = fixed, ec_.fixed_set = true;
    }

    // The draw.
    if (convert) {
        // Quads, fans, polygons and loops as lists.
        std::vector<uint32_t> out;
        if (mode == GL_QUADS) {
            for (int q = 0; q + 3 < count; q += 4)
                for (int k : {0, 1, 2, 0, 2, 3}) out.push_back(index_at(q + k));
        } else if (mode == GL_LINE_LOOP) {
            for (int k = 0; k < count; k++) out.push_back(index_at(k)), out.push_back(index_at((k + 1) % count));
        } else {  // fan, polygon
            for (int k = 1; k + 1 < count; k++) out.push_back(index_at(0)), out.push_back(index_at(k)), out.push_back(index_at(k + 1));
        }
        if (out.empty()) return;
        const Slice s = ring(out.size() * 4, 4);
        std::memcpy(s.ptr, out.data(), out.size() * 4);
        [enc_ drawIndexedPrimitives:prim indexCount:out.size() indexType:MTLIndexTypeUInt32 indexBuffer:s.buffer
                  indexBufferOffset:s.offset];
    } else if (indexed) {
        const uint32_t isz = index_type == GL_UNSIGNED_INT ? 4 : 2;
        id<MTLBuffer> ib;
        NSUInteger ioff;
        if (ibo && index_type != GL_UNSIGNED_BYTE && (ibo->block.offset + indices) % 4 == 0) {
            touch(*ibo);
            ib = ibo->block.buffer, ioff = ibo->block.offset + indices;
        } else {
            const Slice s = ring((size_t)count * isz, 4);
            if (index_type == GL_UNSIGNED_BYTE)
                for (int k = 0; k < count; k++) ((uint16_t*)s.ptr)[k] = index_src[k];
            else
                std::memcpy(s.ptr, index_src, (size_t)count * isz);
            ib = s.buffer, ioff = s.offset;
        }
        [enc_ drawIndexedPrimitives:prim indexCount:count indexType:isz == 4 ? MTLIndexTypeUInt32 : MTLIndexTypeUInt16
                        indexBuffer:ib indexBufferOffset:ioff];
    } else {
        [enc_ drawPrimitives:prim vertexStart:first vertexCount:count];
    }
    pass_drawn_ = true;
    n_draws++;
    draws_since_commit_++;
    if (dumping_) dump_draw(mode, count, indexed, vprog, fprog, fbase, bound_tex);
}

// ---- blits and read-back ----

void Backend::blit_framebuffer(int sx0, int sy0, int sx1, int sy1, int dx0, int dy0, int dx1, int dy1,
                               GLbitfield mask, GLenum filter) {
    const PassTargets src = targets(st.read_fb), dst = targets(st.draw_fb);
    if ((mask & GL_COLOR_BUFFER_BIT) && src.color && dst.color) {
        // A draw: scales, flips and converts formats.
        if (!ensure_pass()) return;
        id<MTLRenderPipelineState> ps =
            builtin_pipeline("quad_vs", "copy_fs", pass_.color.pixelFormat,
                             pass_.depth ? pass_.depth.pixelFormat : MTLPixelFormatInvalid,
                             pass_.stencil ? pass_.stencil.pixelFormat : MTLPixelFormatInvalid, 0xf, true, &pass_);
        [enc_ setRenderPipelineState:ps];
        MTLDepthStencilDescriptor* dd = [MTLDepthStencilDescriptor new];
        [enc_ setDepthStencilState:depth_stencil_.count(~0ull) ? depth_stencil_[~0ull]
                                                                 : (depth_stencil_[~0ull] = [dev newDepthStencilStateWithDescriptor:dd])];
        const int x = std::min(dx0, dx1), y = std::min(dy0, dy1), w = std::abs(dx1 - dx0), h = std::abs(dy1 - dy0);
        [enc_ setViewport:(MTLViewport){(double)x, (double)y, (double)w, (double)h, 0, 1}];
        const int pw = pass_.width(), ph = pass_.height();
        const int cx0 = std::clamp(x, 0, pw), cy0 = std::clamp(y, 0, ph), cx1 = std::clamp(x + w, 0, pw), cy1 = std::clamp(y + h, 0, ph);
        if (cx1 > cx0 && cy1 > cy0) {
            [enc_ setScissorRect:(MTLScissorRect){(NSUInteger)cx0, (NSUInteger)cy0, (NSUInteger)(cx1 - cx0), (NSUInteger)(cy1 - cy0)}];
            [enc_ setCullMode:MTLCullModeNone];
            [enc_ setTriangleFillMode:MTLTriangleFillModeFill];
            const float sw = (float)src.width(), sh = (float)src.height();
            // NDC x -1 is the destination's left edge, NDC y +1 its row y (GL's bottom).
            const float u0 = (dx0 <= dx1 ? sx0 : sx1) / sw, u1 = (dx0 <= dx1 ? sx1 : sx0) / sw;
            const float v0 = (dy0 <= dy1 ? sy0 : sy1) / sh, v1 = (dy0 <= dy1 ? sy1 : sy0) / sh;
            const float r[4] = {u0, v0, u1, v1};
            [enc_ setVertexBytes:r length:sizeof r atIndex:0];
            [enc_ setFragmentTexture:src.color atIndex:0];
            MTLSamplerDescriptor* sd = [MTLSamplerDescriptor new];
            sd.minFilter = sd.magFilter = filter == GL_LINEAR ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
            const uint64_t skey = filter == GL_LINEAR ? ~1ull : ~2ull;
            if (!samplers_.count(skey)) samplers_[skey] = [dev newSamplerStateWithDescriptor:sd];
            [enc_ setFragmentSamplerState:samplers_[skey] atIndex:0];
            [enc_ drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            pass_drawn_ = true;
        }
        ec_ = EncoderCache{};
    }
    if ((mask & (GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT)) && src.depth && dst.depth && src.depth != dst.depth &&
        src.depth.pixelFormat == dst.depth.pixelFormat && src.depth.width == dst.depth.width &&
        src.depth.height == dst.depth.height) {
        end_pass();
        id<MTLBlitCommandEncoder> bl = [cb_ blitCommandEncoder];
        [bl copyFromTexture:src.depth toTexture:dst.depth];
        [bl endEncoding];
    } else if (mask & (GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT)) {
        warn_once("depth/stencil blit not done");
    }
}

void Backend::read_pixels(int x, int y, int w, int h, GLenum format, GLenum type, void* out) {
    const PassTargets src = targets(st.read_fb);
    if (!src.color || w <= 0 || h <= 0 || !out) return;
    flush_and_wait();
    id<MTLTexture> t = src.color;
    const size_t row4 = (size_t)w * 4;
    std::vector<uint8_t> px(row4 * h);
    if (t.storageMode == MTLStorageModePrivate || t.pixelFormat != MTLPixelFormatBGRA8Unorm && t.pixelFormat != MTLPixelFormatRGBA8Unorm) {
        warn_once("glReadPixels from this format is not supported");
        return;
    }
    [t getBytes:px.data() bytesPerRow:row4 fromRegion:MTLRegionMake2D(x, y, w, h) mipmapLevel:src.color_level];
    const bool bgra_src = t.pixelFormat == MTLPixelFormatBGRA8Unorm;
    const uint32_t n = components(format);
    const size_t align = (size_t)std::max(1, st.pack_alignment);
    const size_t row = ((size_t)w * n + align - 1) / align * align;
    for (int yy = 0; yy < h; yy++)
        for (int xx = 0; xx < w; xx++) {
            const uint8_t* s = &px[yy * row4 + xx * 4];
            const uint8_t rgba[4] = {bgra_src ? s[2] : s[0], s[1], bgra_src ? s[0] : s[2], s[3]};
            uint8_t* o = (uint8_t*)out + yy * row + xx * n;
            if (format == GL_BGRA) o[0] = rgba[2], o[1] = rgba[1], o[2] = rgba[0], o[3] = rgba[3];
            else for (uint32_t k = 0; k < n; k++) o[k] = rgba[k];
        }
    (void)type;
}

bool Backend::read_default(int* w, int* h, std::vector<uint8_t>* rgb) {
    if (!default_color_) return false;
    flush_and_wait();
    *w = default_w_, *h = default_h_;
    std::vector<uint8_t> px((size_t)default_w_ * default_h_ * 4);
    [default_color_ getBytes:px.data() bytesPerRow:(size_t)default_w_ * 4 fromRegion:MTLRegionMake2D(0, 0, default_w_, default_h_)
                 mipmapLevel:0];
    rgb->resize((size_t)default_w_ * default_h_ * 3);
    for (size_t k = 0; k < (size_t)default_w_ * default_h_; k++)
        (*rgb)[3 * k] = px[4 * k + 2], (*rgb)[3 * k + 1] = px[4 * k + 1], (*rgb)[3 * k + 2] = px[4 * k];
    return true;
}

// ---- queries ----

void Backend::begin_query(GLuint id) {
    Query& q = queries[id];
    q = Query{};
    active_query = id;
    if (enc_) bind_visibility_slot();
}

void Backend::end_query() {
    if (!active_query) return;
    if (enc_) [enc_ setVisibilityResultMode:MTLVisibilityResultModeDisabled offset:0];
    Query& q = queries[active_query];
    q.seg = seg_, q.ended = true;
    pending_queries_.push_back(active_query);
    active_query = 0;
}

bool Backend::query_available(GLuint id) {
    auto it = queries.find(id);
    if (it == queries.end() || !it->second.ended) return false;
    Query& q = it->second;
    if (resolve(q)) return true;
    // Polled again while still unsubmitted: the caller waits for it, so submit.
    if (q.seg >= seg_) {
        if (q.asked == frame_) submit(false);
        q.asked = frame_;
    }
    return false;
}

uint64_t Backend::query_result(GLuint id) {
    auto it = queries.find(id);
    if (it == queries.end() || !it->second.ended) return 0;
    Query& q = it->second;
    if (!resolve(q)) {
        wait_seg(q.seg);
        resolve(q);
    }
    return q.result;
}

// ---- presentation ----

void Backend::present() {
    // At the game's first present (the window is laid out and on screen by
    // then; at attach_layer it is not): compile what earlier runs and the
    // game's archives hold (shader_cache.mm), with a progress bar.
    static bool precompiled;
    if (!precompiled && layer_) {
        precompiled = true;
        shader_cache().startup(
            dev, [](size_t done, size_t total, void* b) { static_cast<Backend*>(b)->draw_progress(total ? (double)done / total : 1); },
            this);
    }
    @autoreleasepool {
        if (pending_clear_.targets.color || pending_clear_.targets.depth || pending_clear_.targets.stencil) {
            const PassTargets t = pending_clear_.targets;
            begin_pass(t);
        }
        end_pass();
        id<CAMetalDrawable> drawable = nil;
        if (layer_) {
            const int dw = drawable_w.load(), dh = drawable_h.load();
            if (dw > 0 && dh > 0 && (layer_.drawableSize.width != dw || layer_.drawableSize.height != dh))
                layer_.drawableSize = CGSizeMake(dw, dh);
#if !TARGET_OS_IPHONE  // on iOS, presentation always follows the display
            // SCIMITAR_VSYNC=0: never wait for the display (benchmarks).
            static const bool no_sync = std::getenv("SCIMITAR_VSYNC") && std::getenv("SCIMITAR_VSYNC")[0] == '0';
            layer_.displaySyncEnabled = vsync.load() && !no_sync;
#endif
            drawable = [layer_ nextDrawable];
        }
        if (drawable && default_color_) {
            MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture = drawable.texture;
            rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            id<MTLRenderCommandEncoder> e = [cb_ renderCommandEncoderWithDescriptor:rp];
            [e setRenderPipelineState:builtin_pipeline("quad_vs", "copy_fs", MTLPixelFormatBGRA8Unorm, MTLPixelFormatInvalid,
                                                       MTLPixelFormatInvalid, 0xf)];
            // The image's rows are bottom-up: the screen's top shows the last row.
            const float r[4] = {0, 1, 1, 0};
            [e setVertexBytes:r length:sizeof r atIndex:0];
            [e setFragmentTexture:default_color_ atIndex:0];
            const uint64_t skey = ~1ull;
            if (!samplers_.count(skey)) {
                MTLSamplerDescriptor* sd = [MTLSamplerDescriptor new];
                sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear;
                samplers_[skey] = [dev newSamplerStateWithDescriptor:sd];
            }
            [e setFragmentSamplerState:samplers_[skey] atIndex:0];
            [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            [e endEncoding];
        }
        submit(true, drawable);
        if (dumping_) dump_end_frame();
        begin_frame();
    }
}

}  // namespace scimitar::metal
