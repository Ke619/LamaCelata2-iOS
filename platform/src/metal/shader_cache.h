#pragma once
// The shader cache (shader_cache.mm, docs/METAL.md "Shader precompile"): what
// removes first-time compile stutter.
//
// - Recording: every pipeline the game draws with is appended to a per-game
//   disk cache as its two ARB program texts, the vertex variant, the alpha
//   function and the fixed-function state (PipeDesc).
// - Precompile: at startup, before the game's first frame, every recorded
//   pipeline and every ARB program found in the game's .forge archives that
//   was not compiled yet for this translator version is compiled on all cores
//   (MSL into the system's shader cache, pipelines into an MTLBinaryArchive),
//   with a progress bar in the window. Later launches only do what is new.
// - Background fallback: a function or pipeline still missing at a draw is
//   compiled on another thread; the draw waits at most a small budget per
//   frame (SCIMITAR_SHADER_WAIT_MS) and is skipped while it compiles.
#import <Metal/Metal.h>

#include <cstdint>
#include <string>

#include "backend.h"

namespace scimitar::metal {

// A pipeline's fixed-function state (zero-filled, hashed and stored as bytes).
struct PipeDesc {
    uint32_t formats[3 + kMoreColors];  // colour 0, depth, stencil, more colours
    uint32_t blend[5];                  // enabled, src, dst, equation, colour mask
    uint32_t nu, nb;
    uint32_t uses[kAttribs][4];  // attribute index, MTLVertexFormat, binding, offset
    uint32_t binds[kAttribs][2];  // stride, constant
};

// The pipeline descriptor for a fragment program writing `color_outputs` (backend.mm).
MTLRenderPipelineDescriptor* pipeline_descriptor(id<MTLFunction> vfn, id<MTLFunction> ffn, const PipeDesc& d,
                                                 uint32_t color_outputs);

uint64_t text_hash(const void* p, size_t n);

class ShaderCache {
public:
    // Precompiles what is not compiled yet, reporting progress through
    // `progress` (items done, of total) on the calling thread. Loads the
    // binary archive. `resources`: the game's Resources folder (else found
    // from the process arguments).
    void startup(id<MTLDevice> dev, void (*progress)(size_t done, size_t total, void* ctx), void* ctx,
                 const char* resources = nullptr);
    id<MTLBinaryArchive> archive() const { return archive_; }

    // A pipeline drawn with for the first time in this process.
    void record(const std::string& vtext, const std::string& ftext, const uint32_t vopts[3], int alpha,
                const PipeDesc& d);

    // Background compiles. Return the object, or nil with *pending set while
    // it compiles; *waited: the time this call blocked (ms).
    id<MTLFunction> function(id<MTLDevice> dev, const std::string& msl, int alpha, bool* pending, double* waited);
    id<MTLRenderPipelineState> pipeline(id<MTLDevice> dev, const std::string& key, MTLRenderPipelineDescriptor* pd,
                                        bool* pending, double* waited);
    void new_frame();

    uint64_t n_background = 0, n_skipped_frames = 0;

private:
    id<MTLBinaryArchive> archive_ = nil;
};

ShaderCache& shader_cache();

}  // namespace scimitar::metal
