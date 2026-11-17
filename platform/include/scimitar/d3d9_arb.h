#pragma once
// Direct3D 9 shader bytecode (vs_3_0, ps_3_0) to ARB assembly
// (ARB_vertex_program, ARB_fragment_program), so the Mac engine can run the
// Windows release's shaders (docs/PC_DATA.md). Plain C++.
//
// The output follows the conventions of the Mac build's own ARB programs, so
// the engine binds them as it does those: float constants are program.env[n]
// at the D3D register n (fragment registers from 128 on, the material's, are
// program.local[n]); vertex inputs are generic attributes by usage (position
// 0, blend weight 1, normal 2, colour 3 and 4, blend indices 7, texture
// coordinate n 8 + n, tangent 14, binormal 15); vertex outputs and fragment
// inputs pair by usage index (texcoord n, color); sampler n is texture unit n.
//
// ARB programs have no flow control. Loops with constant counts (rep) are
// unrolled; if/else blocks are flattened: both sides run and every write
// inside one is merged into its register under the block's condition, as Cg
// compiled the same HLSL for the Mac. Instructions ARB lacks are expanded
// (lrp, cmp and sincos in vertex programs, nrm, dp2add, mova's rounding).
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace scimitar::d3d9 {

enum class RegisterSet { Bool = 0, Int4 = 1, Float4 = 2, Sampler = 3 };

// An entry of the bytecode's constant table (the CTAB comment).
struct Constant {
    std::string name;
    RegisterSet set = RegisterSet::Float4;
    int reg = 0, count = 0;  // first register and registers used
    int elements = 1;        // array elements (1 for a single value)
    bool is_struct = false;
    int rows = 0, columns = 0;
    bool column_major = false;  // a matrix stored a column per register
};

struct Source {
    int type = 0, num = 0;
    uint8_t swizzle[4] = {0, 1, 2, 3};
    int modifier = 0;               // D3DSPSM_*: 1 negate, 11 abs, 12 -abs
    bool relative = false;          // indexed by an address register
    int rel_type = 0, rel_num = 0;  // that register (a0 or aL)
    int rel_component = 0;
};

struct Dest {
    int type = 0, num = 0;
    int mask = 0xf;
    bool saturate = false;
    int shift = 0;
};

struct Instruction {
    int op = 0;
    int control = 0;  // comparison of ifc/breakc/setp, texld variant
    bool predicated = false;
    bool has_dest = false;
    Dest dst;
    std::vector<Source> src;
    // dcl
    int usage = 0, usage_index = 0, texture_type = 0;
    // def, defi, defb
    float fvalue[4] = {};
    int ivalue[4] = {};
};

struct Program {
    bool vertex = false;
    int major = 0, minor = 0;
    std::vector<Instruction> code;
    std::vector<Constant> constants;
    std::string error;  // parse error, empty when the bytecode was understood
};

// Parses `count` 32-bit tokens (version token first).
Program parse(const uint32_t* tokens, size_t count);

struct ArbOptions {
    // Bool register -> float register holding it (the Mac build's shaders
    // take some booleans as float constants; the engine sets those).
    std::map<int, int> bool_registers;
    // Sampler units that are sampled with depth comparison (SHADOW2D).
    uint32_t shadow_samplers = 0;
    // The engine uploads matrices a row per register, as the Mac build's
    // programs read them; HLSL compiled for D3D keeps them a column per
    // register (the constant table says so). Read such matrices transposed.
    bool transpose_column_matrices = true;
    // Merge writes inside if blocks of vertex programs with CMP, as fragment
    // programs do, instead of x + mask * (new - x), which turns an infinity
    // or NaN the branch not taken computed into NaN. ARB vertex programs have
    // no CMP: only for the ARB -> MSL translator, which accepts it there.
    bool vertex_cmp = false;
    // Texture coordinate sets between the stages: ARB has eight, shader
    // model 3 indices go to 15 (Prince of Persia uses 8 and 9). The ARB ->
    // MSL translator takes all 16.
    int texcoords = 8;
    // dsx and dsy as NV_fragment_program2's DDX and DDY, which the ARB -> MSL
    // translator takes (ARB fragment programs have no derivatives).
    bool derivatives = false;
    // Vertex inputs without NVIDIA's aliasing, where tangent and binormal
    // share generic attributes 14 and 15 with texcoord 6 and 7: tangent 16,
    // binormal 17, texcoord 8-15 at 18-25 (Prince of Persia reads tangent
    // and texcoord 6). The ARB -> MSL translator takes attributes to 31.
    bool separate_attributes = false;
};

// The generic vertex attribute a D3D9 input usage and index read: NVIDIA's
// aliasing, or with `separate` the layout ArbOptions::separate_attributes
// describes. -1 for none.
int vertex_attribute(int usage, int index, bool separate);

// The ARB program, or "" with *error saying why not.
std::string to_arb(const Program& program, const ArbOptions& options, std::string* error);

// A program of the same kind that draws nothing (vertex: every vertex at the
// origin) or magenta (fragment), for bytecode to_arb refuses.
std::string fallback_arb(const Program& program);

}  // namespace scimitar::d3d9
