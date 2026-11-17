// The Metal backend's internals (docs/METAL.md): the GL state the game sets,
// its objects, and what they become in Metal. One Backend, driven by whichever
// thread holds the game's one GL context (calls are serialized).
#pragma once
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include "gl_enums.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "scimitar/arb_msl.h"
#include "scimitar/guest_memory.h"

namespace scimitar::metal {

constexpr int kUnits = 16;      // texture units
constexpr int kAttribs = 16;    // generic vertex attributes
constexpr int kEnv = 256;       // program environment registers per stage
constexpr int kLocals = 256;    // program local registers
constexpr int kFrameSlots = 3;  // per-frame resources (rings, visibility buffers)
constexpr int kMaxInFlight = 2;
constexpr int kVertexBufferBase = 1;  // vertex buffers 1..30; constants at 0

// ---- memory ----

// GPU-visible storage inside guest memory: guest heap blocks wrapped once as
// MTLBuffers (no copies). Buffers up to 1 MB are sub-allocated from 8 MB chunks
// in classes four to an octave (a block wastes at most a fifth of its size);
// larger ones get a heap block of their own, rounded to 16 KB pages.
struct Block {
    id<MTLBuffer> buffer = nil;
    uint32_t offset = 0;  // in `buffer`
    uint32_t guest = 0;   // guest address of the first byte
    uint32_t size = 0;    // class size (>= requested)
    int cls = -1;         // -1: a dedicated chunk
    explicit operator bool() const { return buffer != nil; }
};

class Arena {
public:
    void init(GuestMemory* mem, id<MTLDevice> dev) { mem_ = mem, dev_ = dev; }
    Block alloc(uint32_t size);
    void release(const Block& b);
    uint64_t bytes() const { return bytes_; }

private:
    struct Chunk {
        uint32_t guest, size, used;
        id<MTLBuffer> buffer;
    };
    GuestMemory* mem_ = nullptr;
    id<MTLDevice> dev_ = nil;
    std::vector<Chunk> chunks_;  // the last one is carved
    static constexpr int kClasses = 4 * 12 + 1;  // 256 bytes to 1 MB
    std::vector<Block> free_[kClasses];
    uint64_t bytes_ = 0;
    Block carve(uint32_t size, int cls);
};

// Per-frame transient memory (constants, client arrays, generated indices).
struct Slice {
    id<MTLBuffer> buffer = nil;
    uint32_t offset = 0;
    uint8_t* ptr = nullptr;
};
class Ring {
public:
    void init(id<MTLDevice> dev) { dev_ = dev; }
    Slice alloc(size_t n, size_t align = 256);
    void reset() { cur_ = 0, used_ = 0; }

private:
    static constexpr size_t kChunk = 4 << 20;
    id<MTLDevice> dev_ = nil;
    std::vector<id<MTLBuffer>> bufs_;
    size_t cur_ = 0, used_ = 0;
};

// ---- GL objects ----

struct Buffer {
    Block block;
    uint32_t size = 0;
    uint64_t last_use = 0;  // command buffer sequence number
    bool mapped = false;
};

struct TexFormat {
    MTLPixelFormat pixel = MTLPixelFormatInvalid;
    uint32_t bpp = 0;          // bytes per pixel, or per 4x4 block when compressed
    bool compressed = false;
    uint8_t bc = 0;            // 1-3: BC1-BC3 (DXT) blocks decoded to RGBA8 on upload (no BC on the GPU)
    bool depth = false, stencil = false;
    MTLTextureSwizzleChannels swizzle = {MTLTextureSwizzleRed, MTLTextureSwizzleGreen, MTLTextureSwizzleBlue,
                                         MTLTextureSwizzleAlpha};
    bool swizzled = false;
};

struct Texture {
    GLenum target = 0;  // GL_TEXTURE_2D, _CUBE_MAP, _3D, _1D, _RECTANGLE_ARB
    GLenum internal = 0;
    TexFormat fmt;
    id<MTLTexture> tex = nil;
    id<MTLTexture> sample_view = nil;  // swizzled view, or depth read as (d, d, d, 1)
    int w = 0, h = 0, d = 0, levels = 0;
    // Parameters (GL defaults).
    GLenum min_filter = GL_NEAREST_MIPMAP_LINEAR, mag_filter = GL_LINEAR;
    GLenum wrap_s = GL_REPEAT, wrap_t = GL_REPEAT, wrap_r = GL_REPEAT;
    int base_level = 0, max_level = 1000;
    float max_aniso = 1, min_lod = -1000, max_lod = 1000;
    GLenum compare_mode = GL_NONE, compare_func = GL_LEQUAL;
    id<MTLSamplerState> sampler = nil, shadow_sampler = nil;
    long sampler_forced_aniso = 1;  // the forced anisotropy when the samplers were made
    uint64_t last_use = 0;
};

struct Renderbuffer {
    TexFormat fmt;
    id<MTLTexture> tex = nil;
    int w = 0, h = 0, samples = 0;
};

struct Attachment {
    uint8_t kind = 0;  // 0 none, 1 texture, 2 renderbuffer
    GLuint name = 0;
    int level = 0, slice = 0;
    bool operator==(const Attachment& o) const {
        return kind == o.kind && name == o.name && level == o.level && slice == o.slice;
    }
};
struct Framebuffer {
    Attachment color[4], depth, stencil;
};

struct Program {
    GLenum target = 0;
    std::string text;
    std::string label;  // its shader source file or program name, for frame dumps
    arb::Program base;          // the translation with default options
    bool ok = false;
    std::vector<std::array<float, 4>> locals;  // grown on demand
    uint64_t local_gen = 1;
    // Compiled functions: vertex variants by (extra varyings, uint mask, int
    // mask); fragment by alpha function (8: none).
    std::map<std::array<uint32_t, 3>, std::pair<id<MTLFunction>, arb::Program>> vertex_fns;
    id<MTLFunction> fragment_fns[9] = {};
};

struct Query {
    struct SlotRef {
        id<MTLBuffer> buffer;
        uint32_t offset;
    };
    std::vector<SlotRef> slots;
    uint64_t seg = 0;    // the command buffer it ended in
    uint64_t asked = 0;  // the frame of the last availability poll
    bool ended = false, resolved = false;
    uint64_t result = 0;
};

// ---- GL state ----

struct VertexAttrib {
    bool enabled = false;
    uint32_t size = 4;
    GLenum type = GL_FLOAT;
    bool normalized = false;
    uint32_t stride = 0;
    uint32_t pointer = 0;  // guest address or buffer offset
    GLuint buffer = 0;
    float current[4] = {0, 0, 0, 1};
};

struct TexUnit {
    GLuint t1d = 0, t2d = 0, t3d = 0, cube = 0, rect = 0;
    float lod_bias = 0;
};

struct State {
    bool blend = false, depth_test = false, stencil_test = false, cull = false, alpha_test = false;
    bool polygon_offset = false, scissor_test = false, vp_enabled = false, fp_enabled = false;
    GLenum blend_src = GL_ONE, blend_dst = GL_ZERO, blend_eq = GL_FUNC_ADD;
    float blend_color[4] = {};
    GLenum depth_func = GL_LESS;
    bool depth_mask = true;
    double depth_near = 0, depth_far = 1;
    GLenum stencil_func = GL_ALWAYS;
    int stencil_ref = 0;
    uint32_t stencil_value_mask = ~0u, stencil_write_mask = ~0u;
    GLenum stencil_fail = GL_KEEP, stencil_zfail = GL_KEEP, stencil_zpass = GL_KEEP;
    GLenum cull_face = GL_BACK, front_face = GL_CCW, polygon_mode = GL_FILL;
    float offset_factor = 0, offset_units = 0;
    uint8_t color_mask = 0xf;
    GLenum alpha_func = GL_ALWAYS;
    float alpha_ref = 0;
    int viewport[4] = {0, 0, 0, 0};
    int scissor[4] = {0, 0, 0, 0};
    float clear_color[4] = {};
    double clear_depth = 1;
    int clear_stencil = 0;
    GLuint array_buffer = 0, element_buffer = 0, pack_buffer = 0, unpack_buffer = 0;
    GLuint draw_fb = 0, read_fb = 0, renderbuffer = 0;
    GLuint vp = 0, fp = 0;
    uint32_t unit = 0;  // active texture unit
    TexUnit units[kUnits];
    VertexAttrib attribs[kAttribs];
    int unpack_alignment = 4, pack_alignment = 4, unpack_row_length = 0;
    GLenum matrix_mode = GL_MODELVIEW;
    int draw_buffers = 1;  // glDrawBuffers: colour attachments 0..n-1 are drawn
};

// ---- what the encoder has ----

struct EncoderCache {
    id<MTLRenderPipelineState> pipeline = nil;
    id<MTLDepthStencilState> depth_stencil = nil;
    int cull = -1, winding = -1, fill = -1;
    float bias[3] = {-1, -1, -1};
    MTLViewport viewport = {-1, -1, -1, -1, -1, -1};
    MTLScissorRect scissor = {~0ul, ~0ul, ~0ul, ~0ul};
    uint32_t stencil_ref = ~0u;
    float blend_color[4] = {-1, -1, -1, -1};
    id<MTLBuffer> vbuf[31] = {};
    NSUInteger voff[31] = {};
    id<MTLTexture> tex[kUnits] = {};
    id<MTLSamplerState> smp[kUnits] = {};
    arb::FixedState fixed;
    bool fixed_set = false;
    const Program* vconst_prog = nullptr;  // the constants bound: program and generations
    uint64_t vconst_gen = 0, vconst_local = 0;
    const Program* fconst_prog = nullptr;
    uint64_t fconst_gen = 0, fconst_local = 0;
    id<MTLBuffer> const_buf[2] = {};  // bound at buffer index 0, per stage
};

// The attachments of a render pass, as Metal textures. `more` are colour
// attachments 1-3, drawn with glDrawBuffers (Brotherhood's G-buffer).
constexpr int kMoreColors = 3;
struct PassTargets {
    id<MTLTexture> color = nil, depth = nil, stencil = nil;
    int color_level = 0, color_slice = 0;
    id<MTLTexture> more[kMoreColors] = {};
    int more_level[kMoreColors] = {}, more_slice[kMoreColors] = {};
    bool operator==(const PassTargets& o) const {
        for (int k = 0; k < kMoreColors; k++)
            if (more[k] != o.more[k] || more_level[k] != o.more_level[k] || more_slice[k] != o.more_slice[k]) return false;
        return color == o.color && depth == o.depth && stencil == o.stencil && color_level == o.color_level &&
               color_slice == o.color_slice;
    }
    MTLPixelFormat more_format(int k) const { return more[k] ? more[k].pixelFormat : MTLPixelFormatInvalid; }
    int width() const;
    int height() const;
};

struct FrameSlot {
    Ring ring;
    std::vector<id<MTLBuffer>> visibility;  // 64 KB each, zeroed for reuse
    size_t vis_cur = 0, vis_used = 0;
};

class Backend {
public:
    bool init(GuestMemory* mem);
    void attach_layer(CAMetalLayer* layer, int w, int h);

    GuestMemory* mem = nullptr;
    id<MTLDevice> dev = nil;
    id<MTLCommandQueue> queue = nil;
    State st;

    // Objects (name -> object). Names are made here, as GL would.
    std::unordered_map<GLuint, Buffer> buffers;
    std::unordered_map<GLuint, Texture> textures;
    std::unordered_map<GLuint, Renderbuffer> renderbuffers;
    std::unordered_map<GLuint, Framebuffer> framebuffers;
    std::unordered_map<GLuint, Program> programs;
    std::unordered_map<GLuint, Query> queries;
    // Object names, allocated as GL does: per kind, the lowest unused first.
    // The engine relies on it: it deletes and regenerates textures and keeps
    // using the old number, which Apple's GL hands out again.
    struct Names {
        GLuint next = 1;          // names from here up were never handed out
        std::set<GLuint> free;    // freed names below `next`
        GLuint get() {
            if (free.empty()) return next++;
            const GLuint n = *free.begin();
            free.erase(free.begin());
            return n;
        }
        void put(GLuint n) {
            if (n && n < next) free.insert(n);
        }
        // A name the game uses without generating it (allowed in GL).
        void use(GLuint n) {
            if (!n) return;
            if (n < next) free.erase(n);
            else {
                for (GLuint k = next; k < n; k++) free.insert(k);
                next = n + 1;
            }
        }
    };
    enum NameKind { kBufferNames, kTextureNames, kFramebufferNames, kRenderbufferNames, kProgramNames, kQueryNames, kNameKinds };
    Names names[kNameKinds];
    float env[2][kEnv][4] = {};  // [0] vertex, [1] fragment
    uint64_t env_gen[2] = {1, 1};
    GLuint active_query = 0;
    GLenum error = GL_NO_ERROR;

    // GL entry points with host arguments.
    void buffer_data(GLenum target, uint32_t size, const void* data);
    void buffer_sub_data(GLenum target, uint32_t offset, uint32_t size, const void* data);
    uint32_t map_buffer(GLenum target, GLenum access);  // guest address
    bool unmap_buffer(GLenum target);
    void delete_buffers(int n, const GLuint* ids);
    void tex_image(GLenum target, int level, GLenum internal, int w, int h, int d, GLenum format, GLenum type,
                   const void* pixels, bool compressed, uint32_t image_size);
    void tex_sub_image(GLenum target, int level, int x, int y, int z, int w, int h, int d, GLenum format, GLenum type,
                       const void* pixels, bool compressed, uint32_t image_size);
    void tex_parameter(GLenum target, GLenum pname, float value);
    void delete_textures(int n, const GLuint* ids);
    void renderbuffer_storage(GLenum internal, int w, int h, int samples);
    void framebuffer_attach(GLenum attachment, uint8_t kind, GLuint name, GLenum textarget, int level);
    GLenum check_framebuffer();
    void program_string(GLenum target, const std::string& text);
    void program_env(GLenum target, uint32_t index, uint32_t count, const float* v);
    void program_local(GLenum target, uint32_t index, uint32_t count, const float* v);
    void clear(GLbitfield mask);
    void draw(GLenum mode, int first, int count, GLenum index_type, uint32_t indices, bool indexed);
    void blit_framebuffer(int sx0, int sy0, int sx1, int sy1, int dx0, int dy0, int dx1, int dy1, GLbitfield mask,
                          GLenum filter);
    void read_pixels(int x, int y, int w, int h, GLenum format, GLenum type, void* out);
    void begin_query(GLuint id);
    void end_query();
    bool query_available(GLuint id);
    uint64_t query_result(GLuint id);
    void flush_and_wait();  // commits the work so far and waits for it

    // Presentation.
    void set_default_size(int w, int h);
    void present();
    bool read_default(int* w, int* h, std::vector<uint8_t>* rgb);
    std::atomic<int> drawable_w{0}, drawable_h{0};
    std::atomic<bool> vsync{true};
    std::atomic<double> frame_min_duration{0};  // iOS: the least time between presents (the frame-rate cap), 0: none
    // Anisotropic filtering forced on the linearly filtered, mipmapped textures
    // (1: none; set_forced_anisotropy, SCIMITAR_ANISO at start).
    std::atomic<int> forced_aniso{[] {
        const char* v = std::getenv("SCIMITAR_ANISO");
        return v ? (int)std::clamp(std::strtol(v, nullptr, 10), 1L, 16L) : 1;
    }()};
    long forced_aniso_level() const { return forced_aniso.load(std::memory_order_relaxed); }
    int shift_x = 0, shift_y = 0;  // set_draw_shift: GL's x, and y counted up from the bottom

    // Frame dumps (F13, dump.mm): the next frame's passes as images, its draws as text.
    std::atomic<bool> dump_requested{false};

    // Statistics for SCIMITAR_TRACE_GL.
    uint64_t n_draws = 0, n_passes = 0, n_pipelines = 0, n_clears = 0, n_renames = 0, n_splits = 0;
    uint64_t n_skipped = 0;
    std::atomic<double> compile_ms{0};  // MSL compiles, function specialization, pipeline builds
    std::atomic<double> gpu_ms{0};      // GPU time of the command buffers completed
    std::atomic<uint64_t> gpu_frames{0};  // frames whose last command buffer completed

private:
    // Frame dumps (dump.mm).
    struct DumpImage {
        std::string name;
        id<MTLTexture> color;
        id<MTLBuffer> depth;
        int w = 0, h = 0, dw = 0, dh = 0;
    };
    bool dumping_ = false;
    int dump_pass_ = 0, dump_draw_ = 0;
    std::string dump_dir_;
    FILE* dump_log_ = nullptr;
    std::vector<DumpImage> dump_images_;
    std::map<GLuint, id<MTLTexture>> dump_textures_;  // sampled this frame, by GL name
    void dump_begin_frame();
    void dump_pass_begun(const PassTargets& t, MTLRenderPassDescriptor* rp);
    void dump_pass_ending();
    void dump_draw(GLenum mode, int count, bool indexed, const Program& vp, const Program& fp,
                   const arb::Program& fbase, const id<MTLTexture>* bound);
    void dump_end_frame();
    void dump_textures();

    CAMetalLayer* layer_ = nil;
    Arena arena_;
    FrameSlot slots_[kFrameSlots];
    uint64_t frame_ = 0;  // frames begun
    dispatch_semaphore_t in_flight_ = nullptr;
    id<MTLCommandBuffer> cb_ = nil;
    uint64_t seg_ = 1;  // sequence number of cb_
    std::atomic<uint64_t> completed_seg_{0};
    std::deque<std::pair<uint64_t, id<MTLCommandBuffer>>> committed_;
    std::deque<std::pair<uint64_t, Block>> deferred_;  // freed once that sequence number completes
    std::vector<GLuint> pending_queries_;

    // The default framebuffer.
    id<MTLTexture> default_color_ = nil, default_ds_ = nil;
    int default_w_ = 0, default_h_ = 0;

    // The open render pass.
    id<MTLRenderCommandEncoder> enc_ = nil;
    PassTargets pass_;
    id<MTLBuffer> pass_vis_ = nil;
    EncoderCache ec_;
    bool pass_drawn_ = false;
    // Clears waiting for the next pass's load actions.
    struct PendingClear {
        bool color = false, depth = false, stencil = false;
        MTLClearColor color_value;
        double depth_value = 1;
        uint32_t stencil_value = 0;
        PassTargets targets;
    } pending_clear_;

    // Caches.
    std::unordered_map<std::string, id<MTLLibrary>> libraries_;
    // Functions by MSL and alpha function (-1 for vertex), kept like the
    // libraries: pipelines_ keys on function addresses, so a function freed
    // when its program is deleted or reloaded must never have its address
    // reused by another shader (a stale pipeline after a world switch).
    std::unordered_map<std::string, id<MTLFunction>> functions_;
    std::unordered_map<std::string, id<MTLRenderPipelineState>> pipelines_;
    std::unordered_map<uint64_t, id<MTLDepthStencilState>> depth_stencil_;
    std::unordered_map<uint64_t, id<MTLSamplerState>> samplers_;
    id<MTLLibrary> builtin_ = nil;
    std::unordered_map<std::string, id<MTLRenderPipelineState>> builtin_pipelines_;
    id<MTLTexture> dummy_2d_ = nil, dummy_cube_ = nil, dummy_3d_ = nil, dummy_1d_ = nil, lit_shadow_ = nil;
    id<MTLSamplerState> dummy_sampler_ = nil, lit_sampler_ = nil;
    id<MTLBuffer> quad_indices_ = nil;  // 0 1 2 0 2 3 ... for GL_QUADS (16-bit)
    uint32_t quad_index_count_ = 0;

    // Frames and command buffers.
    void begin_frame();
    void submit(bool final_of_frame, id<CAMetalDrawable> drawable = nil);
    void commit(bool final_of_frame, id<CAMetalDrawable> drawable);
    uint32_t draws_since_commit_ = 0;
    void wait_seg(uint64_t seg);
    void retire();
    bool resolve(Query& q);
    FrameSlot& slot() { return slots_[frame_ % kFrameSlots]; }
    Slice ring(size_t n, size_t align = 256) { return slot().ring.alloc(n, align); }
    void end_pass();
    void touch(Buffer& b) { b.last_use = seg_; }
    bool in_flight(uint64_t last_use) const { return last_use > completed_seg_.load(); }

    // Buffers.
    Buffer* bound_buffer(GLenum target);
    void rename(Buffer& b, bool keep_contents);

    // Textures.
    Texture* bound_texture(GLenum target);
    void create_storage(Texture& t, int w, int h, int d, int levels);
    void rename(Texture& t, bool keep_contents);
    id<MTLSamplerState> sampler_for(Texture& t, bool shadow);
    id<MTLTexture> sample_texture(Texture& t, bool shadow);
    void upload(Texture& t, int face, int level, int x, int y, int z, int w, int h, int d, GLenum format,
                GLenum type, const void* pixels, bool compressed);

    // Framebuffers and passes.
    PassTargets targets(GLuint fb);
    id<MTLTexture> attachment_texture(const Attachment& a);
    bool ensure_pass();
    void begin_pass(const PassTargets& t);
    void bind_visibility_slot();

    // Programs and pipelines.
    id<MTLLibrary> library(const std::string& msl);
    id<MTLFunction> function(const std::string& msl, int alpha);
    const arb::Program* vertex_variant(Program& p, uint32_t extra, uint32_t uint_mask, uint32_t int_mask,
                                       id<MTLFunction>* fn);
    id<MTLFunction> fragment_function(Program& p, int alpha);
    id<MTLDepthStencilState> depth_stencil_state(bool has_depth, bool has_stencil);
    id<MTLRenderPipelineState> builtin_pipeline(const char* vs, const char* fs, MTLPixelFormat color,
                                                MTLPixelFormat depth, MTLPixelFormat stencil, uint8_t mask,
                                                bool blend_off = true, const PassTargets* more = nullptr);
    void set_dynamic_state(int target_w, int target_h);
    void bind_constants(const arb::Program& p, Program& prog, int stage);
    void clear_with_draw(GLbitfield mask);
    // The startup shader precompile's progress bar, straight to the layer (shader_cache.mm).
    void draw_progress(double done);
    id<MTLTexture> progress_tex_[2] = {};
};

Backend& backend();
const TexFormat* tex_format(GLenum internal, GLenum format, GLenum type);
// BC1-BC3 (DXT1, DXT3, DXT5) blocks of a w x h image to RGBA8 (frame dumps too).
void decode_bc_image(int kind, const uint8_t* blocks, int w, int h, uint8_t* rgba);
MTLPixelFormat renderbuffer_format(GLenum internal, TexFormat* out);

}  // namespace scimitar::metal
