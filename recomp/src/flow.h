#pragma once
// Decoding one function by following its control flow.
#include <Zydis/Zydis.h>

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace recomp {

class Image;

constexpr ZydisMachineMode kMode = ZYDIS_MACHINE_MODE_LEGACY_32;

// How an instruction transfers control.
enum class Xfer {
    None,          // falls through
    Ret,
    Jump,          // direct jmp inside the function
    CondJump,      // jcc inside the function
    TailCall,      // jmp to the start of another function
    CondTailCall,  // jcc to the start of another function (PE)
    TailImport,    // jmp to an import stub
    Call,          // call to the start of a function
    CallImport,    // call to an import stub
    CallNext,      // call to the next instruction (PIC base: pushes eip)
    CallIndirect,  // call through a register or memory
    JumpIndirect,  // jmp through a register or memory (treated as a tail call)
    JumpTable,     // switch: jmp through a jump table to `cases` inside the function
};

struct Insn {
    uint32_t addr = 0;
    ZydisDecodedInstruction in{};
    ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT]{};
    std::string text;
    Xfer xfer = Xfer::None;
    uint32_t target = 0;  // branch/call target, or stub address for imports
    std::vector<uint32_t> cases;  // JumpTable: distinct targets, in table order
    bool falls_through = true;
    // Flag liveness (filled by the translator).
    uint8_t use = 0, def = 0, may_def = 0, live_out = 0;
};

struct DecodedFunction {
    uint32_t start = 0, end = 0, lo = 0;  // entry; code lies in [lo, end)
    std::map<uint32_t, Insn> insns;
    std::set<uint32_t> labels;  // entry and internal branch targets
    std::set<uint32_t> refs;    // absolute addresses in immediates and displacements
    std::set<uint32_t> falls_into;  // PE: other functions' starts the code runs into
    std::vector<std::string> errors;
};

// Decodes the code at [start, end) from `code` (bytes of that range), following
// control flow from `start`. `image` (optional) classifies calls and jumps that
// leave the range; without it, leaving the range is an error.
// `lo` (default: `start`) widens the range below the entry: branches to
// [lo, end) stay inside. For PE images (no symbols, so no extents), a function
// is the code reachable from its entry within its section without passing
// another function's start: a branch to one is a tail call, falling into one
// ends the decoding.
DecodedFunction decode_function(const uint8_t* code, uint32_t start, uint32_t end,
                                const Image* image, uint32_t lo = 0);
// decode_function for a function of `image`, over the range its format gives.
struct Function;
DecodedFunction decode_image_function(const Image& image, const Function& f);

// Mach-O: splits functions at direct call targets that land inside another symbol's
// extent (code without a symbol of its own, e.g. stripped static helpers), and
// at code addresses taken in code or stored in data (static initializer and
// callback tables) that start with the game's prologue where their container
// never runs, until no new ones appear. Decodes the whole image; returns the
// number added.
// PE: finds every function start by recursive descent from the entry point,
// the exports, TLS callbacks, RTTI vtables and code addresses in data (aligned
// or after padding), adding the direct call targets and code addresses taken
// in code that each decoded function reveals; installs them in the image.
size_t discover_functions(Image& image);

// Mnemonics the code generator can translate.
bool supported_mnemonic(ZydisMnemonic m);

bool is_jcc(ZydisMnemonic m);
bool is_loop(ZydisMnemonic m);  // loop, loope, loopne, jecxz: branches on ecx
bool is_x87(ZydisMnemonic m);
bool is_sse(ZydisMnemonic m);  // SSE-family mnemonics (MOVSD/CMPSD also have string forms)

}  // namespace recomp
