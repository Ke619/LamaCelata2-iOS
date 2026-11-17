// Process and time imports from libSystem.
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

#include "scimitar/imports.h"
#include "scimitar/runtime.h"

namespace scimitar::imports {

namespace {
struct AtExit {
    uint32_t fn, arg;
};
std::mutex g_atexit_lock;
std::vector<AtExit> g_atexit;
}  // namespace

// int usleep(useconds_t)
void imp_usleep_UNIX2003(Context& c) { shim_return(c, (uint32_t)::usleep(arg32(c, 0))); }

// int __cxa_atexit(void (*fn)(void*), void* arg, void* dso): static
// destructors; kept, run by run_atexit() when the port shuts down.
void imp___cxa_atexit(Context& c) {
    std::lock_guard<std::mutex> l(g_atexit_lock);
    g_atexit.push_back({arg32(c, 0), arg32(c, 1)});
    shim_return(c, 0);
}

void imp___stack_chk_fail(Context& c) {
    fatal("stack protector: the guest stack was overwritten (return address %08x)",
          c.mem->read<uint32_t>(c.esp));
}

// Apple System Log, used by the engine's logger: messages go to stderr (errors
// always, everything when SCIMITAR_LOG is set).
void imp_asl_open(Context& c) { shim_return(c, 0xa5100001); }
void imp_asl_close(Context& c) { shim_return(c, 0); }
void imp_asl_set_filter(Context& c) { shim_return(c, 0xff); }
// int asl_log(aslclient, aslmsg, int level, const char* format, ...)
void imp_asl_log(Context& c) {
    static const bool all = std::getenv("SCIMITAR_LOG") != nullptr;
    const int level = (int)arg32(c, 2);
    if (all || level <= 3) {
        const std::string s = guest_format(*c.mem, arg32(c, 3), c.esp + 4 + 16, false);
        std::fprintf(stderr, "[game log %d] %s%s", level, s.c_str(),
                     !s.empty() && s.back() == '\n' ? "" : "\n");
    }
    shim_return(c, 0);
}

// void exit(int): static destructors registered with __cxa_atexit are not run
// (the process is going away; they would touch subsystems not all shimmed yet).
[[noreturn]] void imp_exit(Context& c) {
    const int code = (int)arg32(c, 0);
    std::fflush(stdout);
    std::fprintf(stderr, "[game exit %d]\n", code);
    std::_Exit(code);
}
[[noreturn]] void imp__exit(Context& c) { std::_Exit((int)arg32(c, 0)); }

void imp_abort(Context& c) { fatal("the game called abort() (from %08x)", c.mem->read<uint32_t>(c.esp)); }

}  // namespace scimitar::imports

namespace scimitar {
namespace {
using namespace imports;
const ImportShim kMisc[] = {
    {"_usleep$UNIX2003", imp_usleep_UNIX2003},
    {"___cxa_atexit", imp___cxa_atexit},
    {"___stack_chk_fail", imp___stack_chk_fail},
    {"_abort", imp_abort},
    {"_exit", imp_exit},
    {"__exit", imp__exit},
    {"_asl_open", imp_asl_open},
    {"_asl_close", imp_asl_close},
    {"_asl_set_filter", imp_asl_set_filter},
    {"_asl_log", imp_asl_log},
};
SCIMITAR_REGISTER_SHIMS(kMisc);
}  // namespace
}  // namespace scimitar
