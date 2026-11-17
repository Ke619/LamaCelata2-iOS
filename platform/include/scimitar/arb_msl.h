#pragma once
// ARB assembly programs (ARB_vertex_program / ARB_fragment_program, as cgc 2.2
// writes them for the game) translated to Metal Shading Language, for the Metal
// rendering backend (docs/METAL.md). Plain C++: no Metal here, so the
// translation is testable anywhere and cacheable by its text.
//
// What the generated functions expect (the backend's side of the contract):
//   vertex   [[stage_in]] attributes a<N> = vertex.attrib[N] ([[attribute(N)]]),
//            float4 P[] at [[buffer(0)]] = the program's constants (`params`).
//            Output: position (GL clip space converted to Metal's, see
//            Options::gl_clip) and the varyings written, colours clamped.
//   fragment varyings by name (user(tc0)...), P[] at [[buffer(0)]], FixedState
//            at [[buffer(1)]], texture and sampler N for texture unit N.
//            Function constant kFcAlphaFunc selects the alpha test.
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace scimitar::arb {

enum class Stage : uint8_t { Vertex, Fragment };
enum class TexTarget : uint8_t { None, T1D, T2D, T3D, Cube, Rect, Shadow2D };

// Varyings between the stages, as bits: texcoord[0..7], then these, then
// texcoord[8..15] (beyond ARB's eight; the D3D9 translator writes them for
// Direct3D's shader model 3, which has ten interpolators of any index).
enum : uint32_t { kVaryingColor0 = 8, kVaryingColor1 = 9, kVaryingFog = 10, kVaryingTexCoord8 = 11, kVaryingCount = 19 };

// One float4 of the constant buffer: a program environment or local
// parameter the backend copies in, or a literal (only in arrays that are
// addressed relatively; other literals are inlined in the code).
struct ParamRef {
    enum Kind : uint8_t { Env, Local, Literal } kind = Literal;
    uint16_t index = 0;
    float value[4] = {};
};

// Fragment [[buffer(1)]]: the fixed-function state the programs see.
struct FixedState {
    float alpha_ref = 0;      // glAlphaFunc reference, clamped to [0, 1]
    float lod_bias[16] = {};  // glTexEnv GL_TEXTURE_LOD_BIAS per texture unit
};

// Function constant (uint): the alpha test, glAlphaFunc's function minus
// GL_NEVER (0 never ... 7 always). Not set: always (no test, no discard).
constexpr int kFcAlphaFunc = 0;

struct Options {
    std::string entry = "arb_main";  // the entry point's name
    // Vertex: also output these varyings (bits as above) with (0, 0, 0, 1)
    // when the program does not write them, for a fragment program that
    // reads them (Metal requires every fragment input to be a vertex output).
    uint32_t extra_varyings = 0;
    // Vertex: attributes fetched as unsigned or signed integers (vertex
    // formats that are not normalized), converted to float in the shader.
    uint32_t uint_attribs = 0, int_attribs = 0;
    // Vertex: GL clip space to Metal's: y negated (images keep GL's bottom-up
    // row order, so textures and render targets need no flipping) and z from
    // [-w, w] to [0, w].
    bool gl_clip = true;
    // Vertex: colour outputs clamped to [0, 1], as GL (and Direct3D before
    // shader model 3) does; Direct3D's vs_3_0 leaves them as written
    // (Revelations passes depth through a colour).
    bool clamp_colors = true;
    // Fragment: fragment.position at integer pixel centres (Direct3D 9's
    // vPos: 0 for the first pixel) instead of GL's and Metal's half-integers.
    bool integer_pixel_centers = false;
    // Emit the shared prelude (includes, helpers, the function constant).
    // Off for several programs in one library: emit prelude() once.
    bool prelude = true;
};

struct Program {
    bool ok = false;
    std::string error;  // "line N: message" when !ok
    Stage stage = Stage::Fragment;
    std::string msl;
    std::vector<ParamRef> params;  // P[k] at buffer(0)
    uint32_t attribs = 0;          // vertex: generic attributes read (0-30)
    uint32_t varyings = 0;         // vertex: written (with extra); fragment: read
    uint32_t color_outputs = 0;    // fragment: result.color[N] written
    bool reads_position = false;   // fragment.position
    bool writes_depth = false;     // result.depth
    bool kills = false;            // KIL
    TexTarget textures[16] = {};   // fragment: target per texture unit
    int instructions = 0;
};

Program translate(std::string_view text, const Options& options = {});

// The definitions every translated program uses; translate() includes it
// unless Options::prelude is false.
const std::string& prelude();

}  // namespace scimitar::arb
