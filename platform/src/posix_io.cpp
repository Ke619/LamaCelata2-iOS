// File-descriptor I/O imports. Guest file descriptors are host descriptors.
#include <unistd.h>

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

#include "scimitar/imports.h"

namespace scimitar {

namespace {

bool g_trace = false;

void trace(const char* fmt, ...) {
    if (!g_trace) return;
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    import_trace().emplace_back(buf);
}

const ImportShim kPosixIo[] = {
    {"_read$UNIX2003", imports::imp_read_UNIX2003},
    {"_lseek", imports::imp_lseek},
    {"_aio_read", imports::imp_aio_read},
    {"_aio_error", imports::imp_aio_error},
    {"_aio_return", imports::imp_aio_return},
};
SCIMITAR_REGISTER_SHIMS(kPosixIo);

std::map<std::string, const ImportShim*>& registry() {
    static std::map<std::string, const ImportShim*> r;
    return r;
}

}  // namespace

ShimRegistrar::ShimRegistrar(const ImportShim* shims, size_t count) {
    for (size_t k = 0; k < count; k++) registry()[shims[k].dyld_name] = &shims[k];
}

void set_import_trace(bool on) { g_trace = on; }
bool import_trace_enabled() { return g_trace; }

std::vector<std::string>& import_trace() {
    static std::vector<std::string> t;
    return t;
}

const ImportShim* find_import_shim(const std::string& dyld_name) {
    auto it = registry().find(dyld_name);
    return it == registry().end() ? nullptr : it->second;
}

namespace imports {

// ssize_t read(int fd, void* buf, size_t n)
void imp_read_UNIX2003(Context& c) {
    const int fd = static_cast<int>(arg32(c, 0));
    const uint32_t buf = arg32(c, 1), n = arg32(c, 2);
    if (static_cast<uint64_t>(buf) + n > GuestMemory::size())
        fatal("read: buffer %08x+%u outside guest memory", buf, n);
    const ssize_t r = ::read(fd, c.mem->ptr(buf), n);
    trace("read(%d, %08x, %u) = %zd", fd, buf, n, r);
    shim_return(c, static_cast<uint32_t>(r));
}

// off_t lseek(int fd, off_t offset, int whence) -- off_t is 64-bit, returned in edx:eax
void imp_lseek(Context& c) {
    const int fd = static_cast<int>(arg32(c, 0));
    const int64_t off = static_cast<int64_t>(arg64(c, 1));
    const int whence = static_cast<int>(arg32(c, 3));
    if (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END)
        fatal("lseek: unexpected whence %d", whence);
    const off_t r = ::lseek(fd, off, whence);
    trace("lseek(%d, %lld, %d) = %lld", fd, (long long)off, whence, (long long)r);
    shim_return64(c, static_cast<uint64_t>(r));
}

// POSIX asynchronous reads (Brotherhood's streaming), done at once: aio_read
// reads, aio_error then reports the request finished and aio_return its
// result. The i386 struct aiocb: +0 fd, +4 offset (64-bit), +12 buffer, +16
// byte count.
namespace {
struct AioResult {
    int64_t result;
    int error;
};
std::mutex g_aio_lock;
std::map<uint32_t, AioResult> g_aio;  // by the guest aiocb's address
}  // namespace

// int aio_read(struct aiocb*)
void imp_aio_read(Context& c) {
    const uint32_t cb = arg32(c, 0);
    const int fd = (int)c.mem->read<uint32_t>(cb);
    const int64_t offset = (int64_t)(c.mem->read<uint32_t>(cb + 4) | (uint64_t)c.mem->read<uint32_t>(cb + 8) << 32);
    const uint32_t buf = c.mem->read<uint32_t>(cb + 12), n = c.mem->read<uint32_t>(cb + 16);
    if ((uint64_t)buf + n > GuestMemory::size()) fatal("aio_read: buffer %08x+%u outside guest memory", buf, n);
    const ssize_t r = ::pread(fd, c.mem->ptr(buf), n, offset);
    const int e = r < 0 ? errno : 0;
    trace("aio_read(%d, %08x, %u, %lld) = %zd", fd, buf, n, (long long)offset, r);
    std::lock_guard<std::mutex> l(g_aio_lock);
    g_aio[cb] = {r, e};
    shim_return(c, 0);
}

// int aio_error(const struct aiocb*): 0 once done (always, here), or the error
void imp_aio_error(Context& c) {
    std::lock_guard<std::mutex> l(g_aio_lock);
    auto it = g_aio.find(arg32(c, 0));
    shim_return(c, it == g_aio.end() ? (uint32_t)EINVAL : (uint32_t)it->second.error);
}

// ssize_t aio_return(struct aiocb*): the read's result, once
void imp_aio_return(Context& c) {
    std::lock_guard<std::mutex> l(g_aio_lock);
    auto it = g_aio.find(arg32(c, 0));
    if (it == g_aio.end()) {
        set_guest_errno(c, EINVAL);
        return shim_return(c, (uint32_t)-1);
    }
    const AioResult r = it->second;
    g_aio.erase(it);
    if (r.result < 0) set_guest_errno(c, r.error);
    shim_return(c, (uint32_t)r.result);
}

}  // namespace imports

}  // namespace scimitar
