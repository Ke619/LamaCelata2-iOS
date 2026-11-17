#pragma once
// Runtime support for generated code: the function registry used by indirect
// calls, the i386 cdecl helpers used by import shims, and fatal errors.
#include <cstdint>
#include <cstring>
#include <initializer_list>

#include "scimitar/context.h"
#include "scimitar/guest_ops.h"
#include "scimitar/sse.h"

namespace scimitar {

using GuestFn = void (*)(Context&);

struct FuncEntry {
    uint32_t addr;
    GuestFn fn;
    const char* name;  // mangled symbol, for diagnostics
};

// Generated translation units register their functions at startup.
void register_functions(const FuncEntry* entries, size_t count);
const FuncEntry* find_function(uint32_t addr);

struct FuncRegistrar {
    FuncRegistrar(const FuncEntry* entries, size_t count) { register_functions(entries, count); }
};

// PE images: generated code lists every import with the function that
// implements it (the platform's shim, else a weak stub that stops the run), so
// the loader can fill the import address table with host functions.
struct ImportEntry {
    const char* name;  // "kernel32!CreateFileA"
    GuestFn fn;
};
void register_imports(const ImportEntry* entries, size_t count);
const ImportEntry* find_import(const char* name);
struct ImportRegistrar {
    ImportRegistrar(const ImportEntry* entries, size_t count) { register_imports(entries, count); }
};

// `call` through a register or memory: the return address is already pushed.
void call_indirect(Context& c, uint32_t target);

// Calls guest function `fn` (cdecl) from host code that runs on guest thread
// `c` (an import shim or host function): pushes `args` and a return address
// below the current stack, runs the function, and returns its eax. Registers
// other than the callee-saved ones are clobbered, as for any cdecl call.
uint32_t call_guest(Context& c, uint32_t fn, std::initializer_list<uint32_t> args);
// A virtual call: `object`'s vtable slot `slot`, with `object` as the first argument.
uint32_t call_virtual(Context& c, uint32_t object, int slot, std::initializer_list<uint32_t> args);

// Native functions the game can call through pointers (vtables, callbacks).
// Each gets a guest address in [kHostFnBase, kHostFnBase + kHostFnSpan) and is
// registered like a translated function; it must follow the shim convention
// (finish with shim_return). Registration order decides the addresses.
constexpr uint32_t kHostFnBase = 0xf0000000;
constexpr uint32_t kHostFnSpan = 0x00100000;
constexpr uint32_t kHostFnStride = 16;
uint32_t register_host_function(GuestFn fn, const char* name);
uint32_t host_function_count();

// rdtsc: a 1 GHz time-stamp counter (host monotonic time in nanoseconds).
uint64_t rdtsc();
// cpuid: a fixed SSE3-class Intel CPU (leaf in eax, results in eax..edx).
void cpuid(Context& c);

[[noreturn]] void fatal(const char* fmt, ...);

// The guest thread state of the calling host thread, if any (host main and the
// threads the platform creates set it). `fatal` uses it to print a guest
// backtrace by walking the frame-pointer chain (the game keeps frame pointers).
void set_thread_context(Context* c);
Context* thread_context();
// Prints the guest backtrace of `c` to stderr ([esp] first, for shims).
void print_guest_backtrace(Context& c);
// The translated or host function containing guest address `addr` (the one
// with the greatest start address not above it), or nullptr.
const FuncEntry* find_function_containing(uint32_t addr);

// Called by `fatal` with the formatted message before it aborts. Test harnesses
// install one that does not return (e.g. siglongjmp out of the failed run).
using FatalHook = void (*)(const char* message);
void set_fatal_hook(FatalHook hook);

// ---- cdecl helpers for import shims ----
// On entry to a shim the return address is at [esp]; argument i (32-bit slots)
// is at [esp + 4 + 4*i]. A shim finishes with `shim_return`, which pops the
// return address like `ret` would.
inline uint32_t arg32(const Context& c, int i) {
    return ops::rd32(c.mem->base(), c.esp + 4 + 4 * i);
}
inline uint64_t arg64(const Context& c, int i) {
    return arg32(c, i) | static_cast<uint64_t>(arg32(c, i + 1)) << 32;
}
inline void shim_return(Context& c, uint32_t eax) {
    c.eax = eax;
    c.esp += 4;
}
inline void shim_return64(Context& c, uint64_t v) {
    c.eax = static_cast<uint32_t>(v);
    c.edx = static_cast<uint32_t>(v >> 32);
    c.esp += 4;
}
// float/double arguments (a float takes one slot, a double two) and results:
// i386 returns floating-point values in x87 ST0.
inline float arg_f32(const Context& c, int i) {
    const uint32_t b = arg32(c, i);
    float f;
    std::memcpy(&f, &b, 4);
    return f;
}
inline double arg_f64(const Context& c, int i) {
    const uint64_t b = arg64(c, i);
    double d;
    std::memcpy(&d, &b, 8);
    return d;
}
inline void shim_return_f64(Context& c, double v) {
    uint64_t b;
    std::memcpy(&b, &v, 8);
    x87::push(c.fpu, x87::from_f64(b));
    c.esp += 4;
}
inline void shim_return_f32(Context& c, float v) {
    uint32_t b;
    std::memcpy(&b, &v, 4);
    x87::push(c.fpu, x87::from_f32(b));
    c.esp += 4;
}

}  // namespace scimitar
