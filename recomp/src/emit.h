#pragma once
// C++ code generation for decoded i386 functions.
#include <cstdint>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "flow.h"

namespace recomp {

class Image;

// Thrown when a function cannot be translated (unsupported instruction etc.).
struct EmitError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct EmitResult {
    std::string code;                  // the function definition
    std::set<uint32_t> callees;        // functions it calls or tail-calls directly
    std::set<std::string> imports;     // external imports it calls (dyld names)
    int flag_writes = 0, flag_writes_kept = 0;
};

std::string func_symbol(uint32_t addr);                  // "f_00182660"
std::string import_symbol(const std::string& dyld_name); // "_read$UNIX2003" -> "imp_read_UNIX2003"
// The hand-written replacement of an overridden function, by its symbol:
// "_MacCreateWindow" -> "ov_MacCreateWindow" (folded as import_symbol does).
std::string override_symbol(const std::string& symbol);
std::string demangle(const std::string& mangled);

// Translates `fn` into `void cname(scimitar::Context& c)`. `image` resolves calls
// and import stubs; it may be null for self-contained code. Throws EmitError
// for code it cannot translate.
EmitResult emit_function(DecodedFunction& fn, const std::string& cname, const Image* image,
                         const std::string& comment);

}  // namespace recomp
