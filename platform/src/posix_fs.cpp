// File and directory imports beyond read/lseek (posix_io.cpp). Guest file
// descriptors are host descriptors; paths are host paths. Structures are laid
// out as on i386 Darwin: the legacy struct stat (32-bit inode) for _stat,
// _lstat and _fstat, struct stat64 for the $INODE64 variants and _stat64, and
// the two struct dirent layouts for readdir and readdir$INODE64.
#include <copyfile.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>
#include <utime.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <TargetConditionals.h>
#include <vector>

#include "scimitar/guest_heap.h"
#include "scimitar/imports.h"
#include "scimitar/runtime.h"

namespace scimitar::imports {

namespace {

// A guest path argument, as the host opens it (host_path).
std::string S(Context& c, uint32_t a) { return host_path((const char*)c.mem->ptr(a)); }

uint32_t result(Context& c, long r) {
    if (r < 0) set_guest_errno(c, errno);
    return (uint32_t)r;
}

void put_ts(Context& c, uint32_t p, const timespec& t) {
    c.mem->write<uint32_t>(p, (uint32_t)t.tv_sec);
    c.mem->write<uint32_t>(p + 4, (uint32_t)t.tv_nsec);
}

// struct stat (legacy, 96 bytes): dev 0, ino(32) 4, mode 8, nlink 10, uid 12,
// gid 16, rdev 20, atime 24, mtime 32, ctime 40, size 48, blocks 56,
// blksize 64, flags 68, gen 72.
void put_stat(Context& c, uint32_t p, const struct stat& s) {
    std::memset(c.mem->ptr(p), 0, 96);
    c.mem->write<uint32_t>(p, (uint32_t)s.st_dev);
    c.mem->write<uint32_t>(p + 4, (uint32_t)s.st_ino);
    c.mem->write<uint16_t>(p + 8, s.st_mode);
    c.mem->write<uint16_t>(p + 10, s.st_nlink);
    c.mem->write<uint32_t>(p + 12, s.st_uid);
    c.mem->write<uint32_t>(p + 16, s.st_gid);
    c.mem->write<uint32_t>(p + 20, (uint32_t)s.st_rdev);
    put_ts(c, p + 24, s.st_atimespec);
    put_ts(c, p + 32, s.st_mtimespec);
    put_ts(c, p + 40, s.st_ctimespec);
    c.mem->write<uint64_t>(p + 48, (uint64_t)s.st_size);
    c.mem->write<uint64_t>(p + 56, (uint64_t)s.st_blocks);
    c.mem->write<uint32_t>(p + 64, (uint32_t)s.st_blksize);
    c.mem->write<uint32_t>(p + 68, s.st_flags);
    c.mem->write<uint32_t>(p + 72, s.st_gen);
}

// struct stat64 (108 bytes): dev 0, mode 4, nlink 6, ino(64) 8, uid 16, gid 20,
// rdev 24, atime 28, mtime 36, ctime 44, birthtime 52, size 60, blocks 68,
// blksize 76, flags 80, gen 84.
void put_stat64(Context& c, uint32_t p, const struct stat& s) {
    std::memset(c.mem->ptr(p), 0, 108);
    c.mem->write<uint32_t>(p, (uint32_t)s.st_dev);
    c.mem->write<uint16_t>(p + 4, s.st_mode);
    c.mem->write<uint16_t>(p + 6, s.st_nlink);
    c.mem->write<uint64_t>(p + 8, s.st_ino);
    c.mem->write<uint32_t>(p + 16, s.st_uid);
    c.mem->write<uint32_t>(p + 20, s.st_gid);
    c.mem->write<uint32_t>(p + 24, (uint32_t)s.st_rdev);
    put_ts(c, p + 28, s.st_atimespec);
    put_ts(c, p + 36, s.st_mtimespec);
    put_ts(c, p + 44, s.st_ctimespec);
    put_ts(c, p + 52, s.st_birthtimespec);
    c.mem->write<uint64_t>(p + 60, (uint64_t)s.st_size);
    c.mem->write<uint64_t>(p + 68, (uint64_t)s.st_blocks);
    c.mem->write<uint32_t>(p + 76, (uint32_t)s.st_blksize);
    c.mem->write<uint32_t>(p + 80, s.st_flags);
    c.mem->write<uint32_t>(p + 84, s.st_gen);
}

template <bool Wide, int (*Fn)(const char*, struct stat*)>
void stat_path(Context& c) {
    struct stat s;
    const int r = Fn(S(c, arg32(c, 0)).c_str(), &s);
    if (r == 0) Wide ? put_stat64(c, arg32(c, 1), s) : put_stat(c, arg32(c, 1), s);
    shim_return(c, result(c, r));
}

template <bool Wide>
void stat_fd(Context& c) {
    struct stat s;
    const int r = ::fstat((int)arg32(c, 0), &s);
    if (r == 0) Wide ? put_stat64(c, arg32(c, 1), s) : put_stat(c, arg32(c, 1), s);
    shim_return(c, result(c, r));
}

int host_stat(const char* p, struct stat* s) { return ::stat(p, s); }
int host_lstat(const char* p, struct stat* s) { return ::lstat(p, s); }

// Directory streams: a guest DIR* stands for a host DIR*, with a guest buffer
// for the entry readdir returns.
struct Dir {
    DIR* host;
    uint32_t entry;
};
std::mutex g_lock;
std::map<uint32_t, Dir> g_dirs;

}  // namespace

// With SCIMITAR_TRACE_FILES, opens are printed with their result.
void imp_open(Context& c) {
    static const bool trace = std::getenv("SCIMITAR_TRACE_FILES") != nullptr;
    const int fd = ::open(S(c, arg32(c, 0)).c_str(), (int)arg32(c, 1), (int)arg32(c, 2));
    if (trace) std::fprintf(stderr, "open(%s, %#x) = %d\n", S(c, arg32(c, 0)).c_str(), arg32(c, 1), fd);
    shim_return(c, result(c, fd));
}
void imp_open_UNIX2003(Context& c) { imp_open(c); }
void imp_close(Context& c) { shim_return(c, result(c, ::close((int)arg32(c, 0)))); }
void imp_close_UNIX2003(Context& c) { imp_close(c); }
void imp_write(Context& c) {
    const uint32_t buf = arg32(c, 1), n = arg32(c, 2);
    shim_return(c, result(c, ::write((int)arg32(c, 0), c.mem->ptr(buf), n)));
}
void imp_write_UNIX2003(Context& c) { imp_write(c); }
void imp_read(Context& c) {
    const uint32_t buf = arg32(c, 1), n = arg32(c, 2);
    shim_return(c, result(c, ::read((int)arg32(c, 0), c.mem->ptr(buf), n)));
}
// ssize_t writev(int fd, const struct iovec* iov, int n): i386 iovec {ptr, len}
void imp_writev_UNIX2003(Context& c) {
    const uint32_t iov = arg32(c, 1);
    const int n = (int)arg32(c, 2);
    std::vector<iovec> v(n > 0 ? n : 0);
    for (int k = 0; k < n; k++) {
        v[k].iov_base = c.mem->ptr(c.mem->read<uint32_t>(iov + 8 * k));
        v[k].iov_len = c.mem->read<uint32_t>(iov + 8 * k + 4);
    }
    shim_return(c, result(c, ::writev((int)arg32(c, 0), v.data(), n)));
}
void imp_stat(Context& c) { stat_path<false, host_stat>(c); }
void imp_lstat(Context& c) { stat_path<false, host_lstat>(c); }
void imp_fstat(Context& c) { stat_fd<false>(c); }
void imp_stat_INODE64(Context& c) { stat_path<true, host_stat>(c); }
void imp_stat64(Context& c) { stat_path<true, host_stat>(c); }
void imp_fstat_INODE64(Context& c) { stat_fd<true>(c); }
void imp_mkdir(Context& c) { shim_return(c, result(c, ::mkdir(S(c, arg32(c, 0)).c_str(), (mode_t)arg32(c, 1)))); }
void imp_rmdir(Context& c) { shim_return(c, result(c, ::rmdir(S(c, arg32(c, 0)).c_str()))); }
void imp_unlink(Context& c) { shim_return(c, result(c, ::unlink(S(c, arg32(c, 0)).c_str()))); }
void imp_chmod_UNIX2003(Context& c) { shim_return(c, result(c, ::chmod(S(c, arg32(c, 0)).c_str(), (mode_t)arg32(c, 1)))); }
void imp_fsync(Context& c) { shim_return(c, result(c, ::fsync((int)arg32(c, 0)))); }
void imp_fsync_UNIX2003(Context& c) { imp_fsync(c); }
// int ftruncate(int fd, off_t length): off_t is 64 bits
void imp_ftruncate(Context& c) { shim_return(c, result(c, ::ftruncate((int)arg32(c, 0), (off_t)arg64(c, 1)))); }
void imp_flock(Context& c) { shim_return(c, result(c, ::flock((int)arg32(c, 0), (int)arg32(c, 1)))); }
// int fcntl(int fd, int cmd, ...): integer-argument commands only.
void imp_fcntl_UNIX2003(Context& c) {
    shim_return(c, result(c, ::fcntl((int)arg32(c, 0), (int)arg32(c, 1), (int)arg32(c, 2))));
}
void imp_getcwd(Context& c) {
    const uint32_t buf = arg32(c, 0), size = arg32(c, 1);
    if (!buf) fatal("getcwd(NULL, ...) is not supported");
    shim_return(c, ::getcwd((char*)c.mem->ptr(buf), size) ? buf : result(c, -1) & 0);
}
// int utime(const char*, const struct utimbuf*): i386 utimbuf is two 32-bit times
void imp_utime(Context& c) {
    const uint32_t t = arg32(c, 1);
    if (!t) return shim_return(c, result(c, ::utime(S(c, arg32(c, 0)).c_str(), nullptr)));
    utimbuf u{(time_t)(int32_t)c.mem->read<uint32_t>(t), (time_t)(int32_t)c.mem->read<uint32_t>(t + 4)};
    shim_return(c, result(c, ::utime(S(c, arg32(c, 0)).c_str(), &u)));
}
// int copyfile(const char* from, const char* to, copyfile_state_t, copyfile_flags_t)
void imp_copyfile(Context& c) {
    shim_return(c, result(c, ::copyfile(S(c, arg32(c, 0)).c_str(), S(c, arg32(c, 1)).c_str(), nullptr, arg32(c, 3))));
}

void imp_opendir(Context& c) {
    DIR* d = ::opendir(S(c, arg32(c, 0)).c_str());
    if (!d) return shim_return(c, result(c, -1) & 0);
    const uint32_t g = heap::alloc(*c.mem, 16), entry = heap::alloc(*c.mem, 1056);
    std::lock_guard<std::mutex> l(g_lock);
    g_dirs[g] = {d, entry};
    shim_return(c, g);
}
void imp_opendir_INODE64_UNIX2003(Context& c) { imp_opendir(c); }
void imp_opendir_UNIX2003(Context& c) { imp_opendir(c); }

namespace {
// Reads the next entry into the guest buffer, in the legacy layout ({u32 ino,
// u16 reclen, u8 type, u8 namlen, char name[256]}) or the 64-bit-inode one
// ({u64 ino, u64 seekoff, u16 reclen, u16 namlen, u8 type, char name[1024]}).
void read_dir(Context& c, bool wide) {
    Dir d;
    {
        std::lock_guard<std::mutex> l(g_lock);
        auto it = g_dirs.find(arg32(c, 0));
        if (it == g_dirs.end()) fatal("readdir: not a DIR* from opendir");
        d = it->second;
    }
    dirent* e = ::readdir(d.host);
    if (!e) return shim_return(c, 0);
    const uint32_t p = d.entry;
    std::memset(c.mem->ptr(p), 0, 1056);
    const uint32_t n = std::min<uint32_t>(e->d_namlen, wide ? 1023 : 255);
    if (wide) {
        c.mem->write<uint64_t>(p, e->d_ino);
        c.mem->write<uint64_t>(p + 8, e->d_seekoff);
        c.mem->write<uint16_t>(p + 16, (uint16_t)(21 + n + 1));
        c.mem->write<uint16_t>(p + 18, (uint16_t)n);
        c.mem->write<uint8_t>(p + 20, e->d_type);
        std::memcpy(c.mem->ptr(p + 21), e->d_name, n);
    } else {
        c.mem->write<uint32_t>(p, (uint32_t)e->d_ino);
        c.mem->write<uint16_t>(p + 4, (uint16_t)(8 + n + 1));
        c.mem->write<uint8_t>(p + 6, e->d_type);
        c.mem->write<uint8_t>(p + 7, (uint8_t)n);
        std::memcpy(c.mem->ptr(p + 8), e->d_name, n);
    }
    shim_return(c, p);
}
}  // namespace

void imp_readdir(Context& c) { read_dir(c, false); }
void imp_readdir_INODE64(Context& c) { read_dir(c, true); }
void imp_closedir(Context& c) {
    Dir d;
    {
        std::lock_guard<std::mutex> l(g_lock);
        auto it = g_dirs.find(arg32(c, 0));
        if (it == g_dirs.end()) return shim_return(c, (uint32_t)-1);
        d = it->second;
        g_dirs.erase(it);
    }
    ::closedir(d.host);
    heap::release(*c.mem, d.entry);
    heap::release(*c.mem, arg32(c, 0));
    shim_return(c, 0);
}
void imp_closedir_UNIX2003(Context& c) { imp_closedir(c); }

}  // namespace scimitar::imports

namespace scimitar {

std::string host_path(const char* path) {
#if TARGET_OS_IPHONE
    if (!path || !*path || ::access(path, F_OK) == 0) return path ? path : "";
    static std::mutex lock;
    static std::map<std::string, std::string> resolved;  // directory listings are slow: once per path
    std::lock_guard<std::mutex> l(lock);
    auto it = resolved.find(path);
    if (it != resolved.end()) return it->second;
    std::string out = path[0] == '/' ? "/" : "";
    const char* p = path;
    bool missing = false;  // a component was not found: the rest stays as written
    while (*p) {
        while (*p == '/') p++;
        const char* e = p;
        while (*e && *e != '/') e++;
        if (e == p) break;
        std::string comp(p, e);
        if (!missing && comp != "." && comp != "..") {
            const std::string as_is = out + comp;
            if (::access(as_is.c_str(), F_OK) != 0) {
                missing = true;
                if (DIR* d = ::opendir(out.empty() ? "." : out.c_str())) {
                    while (dirent* de = ::readdir(d))
                        if (strcasecmp(de->d_name, comp.c_str()) == 0) {
                            comp = de->d_name, missing = false;
                            break;
                        }
                    ::closedir(d);
                }
            }
        }
        out += comp;
        if (*e) out += '/';
        p = e;
    }
    return resolved[path] = out;
#else
    return path ? path : "";
#endif
}

}  // namespace scimitar

namespace scimitar {
namespace {
using namespace imports;
const ImportShim kFs[] = {
    {"_open", imp_open},             {"_open$UNIX2003", imp_open_UNIX2003},
    {"_close", imp_close},           {"_close$UNIX2003", imp_close_UNIX2003},
    {"_write", imp_write},           {"_write$UNIX2003", imp_write_UNIX2003},
    {"_read", imp_read},             {"_writev$UNIX2003", imp_writev_UNIX2003},
    {"_stat", imp_stat},             {"_lstat", imp_lstat},           {"_fstat", imp_fstat},
    {"_stat$INODE64", imp_stat_INODE64}, {"_stat64", imp_stat64}, {"_fstat$INODE64", imp_fstat_INODE64},
    {"_mkdir", imp_mkdir},           {"_rmdir", imp_rmdir},           {"_unlink", imp_unlink},
    {"_chmod$UNIX2003", imp_chmod_UNIX2003}, {"_fsync", imp_fsync}, {"_fsync$UNIX2003", imp_fsync_UNIX2003}, {"_ftruncate", imp_ftruncate},
    {"_flock", imp_flock},           {"_fcntl$UNIX2003", imp_fcntl_UNIX2003},
    {"_getcwd", imp_getcwd},         {"_utime", imp_utime},           {"_copyfile", imp_copyfile},
    {"_opendir", imp_opendir},       {"_opendir$UNIX2003", imp_opendir_UNIX2003},       {"_opendir$INODE64$UNIX2003", imp_opendir_INODE64_UNIX2003},
    {"_readdir", imp_readdir},       {"_readdir$INODE64", imp_readdir_INODE64},
    {"_closedir", imp_closedir},     {"_closedir$UNIX2003", imp_closedir_UNIX2003},
};
SCIMITAR_REGISTER_SHIMS(kFs);
}  // namespace
}  // namespace scimitar
