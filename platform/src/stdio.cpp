// stdio imports. A guest FILE* is a small guest block standing for a host
// FILE*; stdin, stdout and stderr get theirs when the data imports are bound
// (___stdinp & co.). Paths are host paths: the guest sees the same file system.
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

#include "scimitar/guest_heap.h"
#include "scimitar/imports.h"
#include "scimitar/runtime.h"

namespace scimitar {

namespace {

std::mutex g_lock;
std::map<uint32_t, FILE*> g_files;  // guest FILE* -> host FILE*

const char* S(Context& c, uint32_t a) { return (const char*)c.mem->ptr(a); }

}  // namespace

uint32_t guest_file(GuestMemory& m, FILE* f) {
    const uint32_t a = heap::alloc(m, 16);
    std::memset(m.ptr(a), 0, 16);
    std::lock_guard<std::mutex> l(g_lock);
    g_files[a] = f;
    return a;
}

FILE* host_file(uint32_t g) {
    std::lock_guard<std::mutex> l(g_lock);
    auto it = g_files.find(g);
    if (it == g_files.end()) fatal("not a FILE* from fopen: %08x", g);
    return it->second;
}

namespace imports {

namespace {
uint32_t opened(Context& c, FILE* f) {
    if (!f) {
        set_guest_errno(c, errno);
        return 0;
    }
    return guest_file(*c.mem, f);
}
}  // namespace

// With SCIMITAR_TRACE_FILES, opens are printed with their result.
void imp_fopen(Context& c) {
    static const bool trace = std::getenv("SCIMITAR_TRACE_FILES") != nullptr;
    FILE* f = std::fopen(host_path(S(c, arg32(c, 0))).c_str(), S(c, arg32(c, 1)));
    if (trace) std::fprintf(stderr, "fopen(%s, %s) = %s\n", S(c, arg32(c, 0)), S(c, arg32(c, 1)), f ? "ok" : "failed");
    shim_return(c, opened(c, f));
}
void imp_fopen_UNIX2003(Context& c) { imp_fopen(c); }
void imp_fdopen(Context& c) { shim_return(c, opened(c, ::fdopen((int)arg32(c, 0), S(c, arg32(c, 1))))); }
void imp_fdopen_UNIX2003(Context& c) { imp_fdopen(c); }
void imp_fclose(Context& c) {
    const uint32_t g = arg32(c, 0);
    FILE* f = host_file(g);
    {
        std::lock_guard<std::mutex> l(g_lock);
        g_files.erase(g);
    }
    const int r = std::fclose(f);
    heap::release(*c.mem, g);
    shim_return(c, (uint32_t)r);
}
// size_t fread(void* p, size_t size, size_t n, FILE*)
void imp_fread(Context& c) {
    const uint32_t p = arg32(c, 0), size = arg32(c, 1), n = arg32(c, 2);
    if ((uint64_t)size * n + p > GuestMemory::size()) fatal("fread: buffer outside guest memory");
    shim_return(c, (uint32_t)std::fread(c.mem->ptr(p), size, n, host_file(arg32(c, 3))));
}
void imp_fwrite_UNIX2003(Context& c) {
    const uint32_t p = arg32(c, 0), size = arg32(c, 1), n = arg32(c, 2);
    shim_return(c, (uint32_t)std::fwrite(c.mem->ptr(p), size, n, host_file(arg32(c, 3))));
}
void imp_fflush(Context& c) {
    shim_return(c, (uint32_t)std::fflush(arg32(c, 0) ? host_file(arg32(c, 0)) : nullptr));
}
// int fseek(FILE*, long offset, int whence): long is 32 bits on i386
void imp_fseek(Context& c) {
    shim_return(c, (uint32_t)std::fseek(host_file(arg32(c, 0)), (int32_t)arg32(c, 1), (int)arg32(c, 2)));
}
void imp_ftell(Context& c) { shim_return(c, (uint32_t)(int32_t)std::ftell(host_file(arg32(c, 0)))); }
// int fsetpos(FILE*, const fpos_t*): fpos_t is a 64-bit offset
void imp_fsetpos(Context& c) {
    shim_return(c, (uint32_t)fseeko(host_file(arg32(c, 0)), (off_t)c.mem->read<uint64_t>(arg32(c, 1)), SEEK_SET));
}
void imp_fgets(Context& c) {
    const uint32_t buf = arg32(c, 0), n = arg32(c, 1);
    const char* r = std::fgets((char*)c.mem->ptr(buf), (int)n, host_file(arg32(c, 2)));
    shim_return(c, r ? buf : 0);
}
void imp_fputs_UNIX2003(Context& c) { shim_return(c, (uint32_t)std::fputs(S(c, arg32(c, 0)), host_file(arg32(c, 1)))); }
void imp_fputc(Context& c) { shim_return(c, (uint32_t)std::fputc((int)arg32(c, 0), host_file(arg32(c, 1)))); }
void imp_putc(Context& c) { imp_fputc(c); }
void imp_getc(Context& c) { shim_return(c, (uint32_t)std::getc(host_file(arg32(c, 0)))); }
void imp_ungetc(Context& c) { shim_return(c, (uint32_t)std::ungetc((int)arg32(c, 0), host_file(arg32(c, 1)))); }
// Wide character I/O in the C locale: one byte per character.
void imp_getwc(Context& c) {
    const int ch = std::getc(host_file(arg32(c, 0)));
    shim_return(c, ch == EOF ? 0xffffffffu : (uint32_t)ch);
}
void imp_putwc(Context& c) {
    const uint32_t w = arg32(c, 0);
    shim_return(c, std::fputc(w < 0x100 ? (int)w : '?', host_file(arg32(c, 1))) == EOF ? 0xffffffffu : w);
}
void imp_ungetwc(Context& c) {
    const uint32_t w = arg32(c, 0);
    shim_return(c, std::ungetc(w < 0x100 ? (int)w : '?', host_file(arg32(c, 1))) == EOF ? 0xffffffffu : w);
}
void imp_ferror(Context& c) { shim_return(c, (uint32_t)std::ferror(host_file(arg32(c, 0)))); }
void imp_fileno(Context& c) { shim_return(c, (uint32_t)::fileno(host_file(arg32(c, 0)))); }
// int setvbuf(FILE*, char* buf, int mode, size_t size): guest buffers are not
// used (the host FILE buffers itself), the mode is.
void imp_setvbuf(Context& c) {
    shim_return(c, (uint32_t)std::setvbuf(host_file(arg32(c, 0)), nullptr, (int)arg32(c, 2), arg32(c, 3)));
}
void imp_flockfile(Context& c) {
    ::flockfile(host_file(arg32(c, 0)));
    shim_return(c, 0);
}
void imp_funlockfile(Context& c) {
    ::funlockfile(host_file(arg32(c, 0)));
    shim_return(c, 0);
}
void imp_puts(Context& c) { shim_return(c, (uint32_t)std::puts(S(c, arg32(c, 0)))); }
// int fprintf(FILE*, const char* fmt, ...)
void imp_fprintf(Context& c) {
    const std::string s = guest_format(*c.mem, arg32(c, 1), c.esp + 12, false);
    shim_return(c, (uint32_t)std::fwrite(s.data(), 1, s.size(), host_file(arg32(c, 0))));
}

}  // namespace imports

namespace {
using namespace imports;
const ImportShim kStdio[] = {
    {"_fopen", imp_fopen},           {"_fopen$UNIX2003", imp_fopen_UNIX2003},
    {"_fdopen", imp_fdopen},         {"_fdopen$UNIX2003", imp_fdopen_UNIX2003},
    {"_fclose", imp_fclose},         {"_fread", imp_fread},           {"_fwrite$UNIX2003", imp_fwrite_UNIX2003},
    {"_fflush", imp_fflush},         {"_fseek", imp_fseek},           {"_ftell", imp_ftell},
    {"_fsetpos", imp_fsetpos},       {"_fgets", imp_fgets},           {"_fputs$UNIX2003", imp_fputs_UNIX2003},
    {"_fputc", imp_fputc},           {"_putc", imp_putc},             {"_getc", imp_getc},
    {"_ungetc", imp_ungetc},         {"_getwc", imp_getwc},           {"_putwc", imp_putwc},
    {"_ungetwc", imp_ungetwc},       {"_ferror", imp_ferror},         {"_fileno", imp_fileno},
    {"_setvbuf", imp_setvbuf},       {"_flockfile", imp_flockfile},   {"_funlockfile", imp_funlockfile},
    {"_puts", imp_puts},             {"_fprintf", imp_fprintf},
};
SCIMITAR_REGISTER_SHIMS(kStdio);
}  // namespace

}  // namespace scimitar
