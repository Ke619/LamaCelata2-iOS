// KERNEL32 functions Revelations imports and AC1 does not: wide-character
// twins of the ANSI functions (which they call with their strings converted),
// alertable I/O (ReadFileEx/WriteFileEx completion routines, run by SleepEx
// and WaitForMultipleObjectsEx), I/O completion ports, file mappings (a copy
// of the file), and process/module snapshots (empty).
#include <sys/stat.h>

#include <atomic>
#include <condition_variable>
#include <chrono>
#include <deque>
#include <map>
#include <mutex>

#include "scimitar/guest_heap.h"
#include "win32.h"

namespace scimitar::imports {
// The ANSI shims (kernel32*.cpp).
#define ANSI(fn) void imp_kernel32_##fn(Context& c);
ANSI(CreateFileA) ANSI(CreateDirectoryA) ANSI(CreateEventA) ANSI(DeleteFileA) ANSI(GetFileAttributesA)
ANSI(GetModuleHandleA) ANSI(LoadLibraryA) ANSI(GetCurrentDirectoryA) ANSI(GetFullPathNameA) ANSI(GetTempPathA)
ANSI(GetPrivateProfileIntA) ANSI(GetPrivateProfileStringA) ANSI(GetPrivateProfileSectionNamesA)
ANSI(WritePrivateProfileStringA) ANSI(ReadFile) ANSI(WriteFile) ANSI(GetFileSize) ANSI(GetFileTime) ANSI(Sleep)
ANSI(WaitForMultipleObjects)
#undef ANSI
}  // namespace scimitar::imports

namespace scimitar::win32 {
namespace {

using Shim = void (*)(Context&);

// Calls the shim `fn` (stdcall, `nargs` arguments) with `args`, below the
// current frame, and returns eax. The caller's frame is left as it was.
uint32_t call_shim(Context& c, Shim fn, std::initializer_list<uint32_t> args) {
    const uint32_t esp = c.esp;
    c.esp = (c.esp - 4 * (uint32_t)args.size() - 4) & ~15u;
    uint32_t p = c.esp + 4;
    for (uint32_t a : args) wr(p, a), p += 4;
    fn(c);
    c.esp = esp;
    return c.eax;
}

// Guest scratch space below the stack pointer while a shim runs (call_shim's
// frames go below it). It moves esp, so the shim's arguments are read from
// the snapshot it takes (arg), and it must be gone before the shim returns
// (its destructor puts esp back): shims use one in an inner scope.
class Scratch {
public:
    explicit Scratch(Context& c) : c_(c), saved_(c.esp) {
        for (int k = 0; k < 12; k++) args_[k] = arg32(c, k);
    }
    ~Scratch() { c_.esp = saved_; }
    uint32_t arg(int k) const { return args_[k]; }
    uint32_t alloc(uint32_t n) {
        c_.esp = (c_.esp - n - 16) & ~15u;
        return c_.esp;
    }
    // A UTF-8 copy of the guest UTF-16 string at `w` (0 stays 0).
    uint32_t ansi(uint32_t w) {
        if (!w) return 0;
        const std::string s = utf8(gwstr(w));
        const uint32_t a = alloc((uint32_t)s.size() + 1);
        std::memcpy(gptr(a), s.c_str(), s.size() + 1);
        return a;
    }

private:
    Context& c_;
    uint32_t saved_;
    uint32_t args_[12];
};

// ---- alertable I/O: completion routines queued per thread ----

struct Apc {
    uint32_t routine, error, bytes, overlapped;
};
thread_local std::deque<Apc> t_apcs;

// Runs the thread's queued completion routines; true if there were any.
bool run_apcs(Context& c) {
    bool any = false;
    while (!t_apcs.empty()) {
        const Apc a = t_apcs.front();
        t_apcs.pop_front();
        call_guest(c, a.routine, {a.error, a.bytes, a.overlapped});
        any = true;
    }
    return any;
}
constexpr uint32_t kWaitIoCompletion = 0xc0;

// ---- I/O completion ports ----

struct Port : Object {
    struct Packet {
        uint32_t bytes, key, overlapped;
    };
    std::mutex m;
    std::condition_variable cv;
    std::deque<Packet> queue;
};
std::mutex g_assoc_mutex;
std::map<uint32_t, std::pair<uint32_t, uint32_t>> g_assoc;  // file handle -> port, key

// ---- file mappings: the file's bytes copied on MapViewOfFile ----

struct Mapping : Object {
    uint32_t file = 0, size_lo = 0;
};
std::mutex g_views_mutex;
std::map<uint32_t, uint32_t> g_views;  // view address -> size

void put_filetime(uint32_t a, uint64_t t) { wr(a, (uint32_t)t), wr(a + 4, (uint32_t)(t >> 32)); }
uint64_t filetime_of(const struct timespec& t) {
    return (uint64_t)t.tv_sec * 10000000ull + (uint64_t)t.tv_nsec / 100 + 116444736000000000ull;
}

}  // namespace

void overlapped_done(uint32_t file, uint32_t overlapped, uint32_t bytes) {
    std::pair<uint32_t, uint32_t> pk;
    {
        std::lock_guard<std::mutex> l(g_assoc_mutex);
        auto it = g_assoc.find(file);
        if (it == g_assoc.end()) return;
        pk = it->second;
    }
    auto port = handle_as<Port>(pk.first);
    if (!port) return;
    {
        std::lock_guard<std::mutex> l(port->m);
        port->queue.push_back({bytes, pk.second, overlapped});
    }
    port->cv.notify_one();
}

}  // namespace scimitar::win32

namespace scimitar::imports {
using namespace scimitar::win32;

// ---- wide-character twins ----
// Each computes its result with a Scratch in an inner scope, then returns.

W32(kernel32, AreFileApisANSI) { ret(c, 1, 0); }

W32(kernel32, CreateFileW) {
    uint32_t h;
    {
        Scratch s(c);
        h = call_shim(c, imp_kernel32_CreateFileA,
                      {s.ansi(s.arg(0)), s.arg(1), s.arg(2), s.arg(3), s.arg(4), s.arg(5), s.arg(6)});
    }
    ret(c, h, 7);
}
W32(kernel32, CreateDirectoryW) {
    uint32_t r;
    {
        Scratch s(c);
        r = call_shim(c, imp_kernel32_CreateDirectoryA, {s.ansi(s.arg(0)), s.arg(1)});
    }
    ret(c, r, 2);
}
W32(kernel32, DeleteFileW) {
    uint32_t r;
    {
        Scratch s(c);
        r = call_shim(c, imp_kernel32_DeleteFileA, {s.ansi(s.arg(0))});
    }
    ret(c, r, 1);
}
W32(kernel32, GetFileAttributesW) {
    uint32_t r;
    {
        Scratch s(c);
        r = call_shim(c, imp_kernel32_GetFileAttributesA, {s.ansi(s.arg(0))});
    }
    ret(c, r, 1);
}
W32(kernel32, CreateEventW) {
    // Named events are not shared between processes here: the name is dropped.
    const uint32_t r = call_shim(c, imp_kernel32_CreateEventA, {arg(c, 0), arg(c, 1), arg(c, 2), 0});
    ret(c, r, 4);
}
W32(kernel32, GetModuleHandleW) {
    uint32_t r;
    {
        Scratch s(c);
        r = call_shim(c, imp_kernel32_GetModuleHandleA, {s.ansi(s.arg(0))});
    }
    ret(c, r, 1);
}
W32(kernel32, LoadLibraryW) {
    uint32_t r;
    {
        Scratch s(c);
        r = call_shim(c, imp_kernel32_LoadLibraryA, {s.ansi(s.arg(0))});
    }
    ret(c, r, 1);
}

// GetFileAttributesExW(name, GetFileExInfoStandard, WIN32_FILE_ATTRIBUTE_DATA*)
W32(kernel32, GetFileAttributesExW) {
    const std::string name = utf8(gwstr(arg(c, 0)));
    const uint32_t data = arg(c, 2);
    struct stat st;
    if (::stat(host_path(name).c_str(), &st) != 0) {
        set_last_error(2);  // ERROR_FILE_NOT_FOUND
        trace("GetFileAttributesExW(%s): none", name.c_str());
        return ret(c, 0, 3);
    }
    wr(data, S_ISDIR(st.st_mode) ? 0x10 : 0x80);  // DIRECTORY or NORMAL
    put_filetime(data + 4, filetime_of(st.st_birthtimespec));
    put_filetime(data + 12, filetime_of(st.st_atimespec));
    put_filetime(data + 20, filetime_of(st.st_mtimespec));
    wr(data + 28, (uint32_t)((uint64_t)st.st_size >> 32));
    wr(data + 32, (uint32_t)st.st_size);
    ret(c, 1, 3);
}

// Functions that fill a char buffer: called with a scratch buffer, the result
// widened. They return the length, or the size needed when it does not fit.
W32(kernel32, GetCurrentDirectoryW) {
    const uint32_t size = arg(c, 0), out = arg(c, 1);
    std::string dir;
    {
        Scratch s(c);
        const uint32_t buf = s.alloc(1024);
        if (call_shim(c, imp_kernel32_GetCurrentDirectoryA, {1024, buf}) < 1024) dir = gstr(buf);
    }
    const std::u16string w = utf16(dir);
    if (w.size() + 1 > size) return ret(c, (uint32_t)w.size() + 1, 2);
    ret(c, put_wstr(out, size, w), 2);
}
W32(kernel32, GetTempPathW) {
    const uint32_t size = arg(c, 0), out = arg(c, 1);
    std::string dir;
    {
        Scratch s(c);
        const uint32_t buf = s.alloc(1024);
        if (call_shim(c, imp_kernel32_GetTempPathA, {1024, buf}) < 1024) dir = gstr(buf);
    }
    const std::u16string w = utf16(dir);
    if (w.size() + 1 > size) return ret(c, (uint32_t)w.size() + 1, 2);
    ret(c, put_wstr(out, size, w), 2);
}
W32(kernel32, GetFullPathNameW) {
    const uint32_t size = arg(c, 1), out = arg(c, 2), file_part = arg(c, 3);
    std::string full8;
    {
        Scratch s(c);
        const uint32_t name = s.ansi(s.arg(0)), buf = s.alloc(2048);
        if (call_shim(c, imp_kernel32_GetFullPathNameA, {name, 2048, buf, 0}) < 2048) full8 = gstr(buf);
    }
    const std::u16string full = utf16(full8);
    if (full.size() + 1 > size) return ret(c, (uint32_t)full.size() + 1, 4);
    const uint32_t w = put_wstr(out, size, full);
    if (file_part) {
        const size_t k = full.rfind(u'\\');
        wr(file_part, k == std::u16string::npos ? out : out + 2 * (uint32_t)(k + 1));
    }
    ret(c, w, 4);
}

// ---- INI files ----

W32(kernel32, GetPrivateProfileIntW) {
    uint32_t r;
    {
        Scratch s(c);
        r = call_shim(c, imp_kernel32_GetPrivateProfileIntA,
                      {s.ansi(s.arg(0)), s.ansi(s.arg(1)), s.arg(2), s.ansi(s.arg(3))});
    }
    ret(c, r, 4);
}
W32(kernel32, WritePrivateProfileStringW) {
    uint32_t r;
    {
        Scratch s(c);
        r = call_shim(c, imp_kernel32_WritePrivateProfileStringA,
                      {s.ansi(s.arg(0)), s.ansi(s.arg(1)), s.ansi(s.arg(2)), s.ansi(s.arg(3))});
    }
    ret(c, r, 4);
}
// The ANSI result (n chars, possibly a list with inner nulls) widened into a
// buffer of `size` wide chars, null-terminated.
static void widen_result(uint32_t dst, uint32_t size, const std::vector<uint8_t>& a) {
    if (!size) return;
    const uint32_t n = std::min<uint32_t>((uint32_t)a.size(), size - 1);
    for (uint32_t k = 0; k < n; k++) wr16(dst + 2 * k, a[k]);
    wr16(dst + 2 * n, 0);
}
W32(kernel32, GetPrivateProfileStringW) {
    const uint32_t out = arg(c, 3), size = arg(c, 4), cap = std::min<uint32_t>(size ? size : 1, 32768);
    uint32_t n;
    std::vector<uint8_t> a;
    {
        Scratch s(c);
        const uint32_t buf = s.alloc(cap + 2);
        n = call_shim(c, imp_kernel32_GetPrivateProfileStringA,
                      {s.ansi(s.arg(0)), s.ansi(s.arg(1)), s.ansi(s.arg(2)), buf, cap, s.ansi(s.arg(5))});
        a.assign(gptr(buf), gptr(buf) + std::min(n + 1, cap));  // a list's inner nulls too
    }
    widen_result(out, size, a);
    ret(c, n, 6);
}
W32(kernel32, GetPrivateProfileSectionNamesW) {
    const uint32_t out = arg(c, 0), size = arg(c, 1), cap = std::min<uint32_t>(size ? size : 1, 32768);
    uint32_t n;
    std::vector<uint8_t> a;
    {
        Scratch s(c);
        const uint32_t buf = s.alloc(cap + 2);
        n = call_shim(c, imp_kernel32_GetPrivateProfileSectionNamesA, {buf, cap, s.ansi(s.arg(2))});
        a.assign(gptr(buf), gptr(buf) + std::min(n + 1, cap));
    }
    widen_result(out, size, a);
    ret(c, n, 3);
}

// ---- alertable I/O ----

// ReadFileEx(h, buf, n, OVERLAPPED*, routine): read at once (ReadFile with
// the OVERLAPPED), the routine queued for the thread's next alertable wait.
W32(kernel32, ReadFileEx) {
    const uint32_t h = arg(c, 0), buf = arg(c, 1), n = arg(c, 2), ov = arg(c, 3), routine = arg(c, 4);
    const uint32_t ok = call_shim(c, imp_kernel32_ReadFile, {h, buf, n, 0, ov});
    const uint32_t error = ok ? 0 : last_error();
    if (!ok && error != 38) return ret(c, 0, 5);  // failed to start; EOF completes with an error
    t_apcs.push_back({routine, error, rd(ov + 4), ov});
    set_last_error(0);
    ret(c, 1, 5);
}
W32(kernel32, WriteFileEx) {
    const uint32_t h = arg(c, 0), buf = arg(c, 1), n = arg(c, 2), ov = arg(c, 3), routine = arg(c, 4);
    if (!call_shim(c, imp_kernel32_WriteFile, {h, buf, n, 0, ov})) return ret(c, 0, 5);
    t_apcs.push_back({routine, 0, rd(ov + 4), ov});
    ret(c, 1, 5);
}
W32(kernel32, CancelIo) { ret(c, 1, 1); }  // nothing is ever pending

W32(kernel32, SleepEx) {
    const uint32_t ms = arg(c, 0), alertable = arg(c, 1);
    if (alertable && run_apcs(c)) return ret(c, kWaitIoCompletion, 2);
    call_shim(c, imp_kernel32_Sleep, {ms});
    ret(c, 0, 2);
}
W32(kernel32, WaitForMultipleObjectsEx) {
    const uint32_t n = arg(c, 0), handles = arg(c, 1), all = arg(c, 2), ms = arg(c, 3), alertable = arg(c, 4);
    if (alertable && run_apcs(c)) return ret(c, kWaitIoCompletion, 5);
    ret(c, call_shim(c, imp_kernel32_WaitForMultipleObjects, {n, handles, all, ms}), 5);
}

// ---- I/O completion ports ----
// Requests on associated files complete at once (ReadFile/WriteFile do the
// work synchronously) and queue their packet (overlapped_done).
W32(kernel32, CreateIoCompletionPort) {
    const uint32_t file = arg(c, 0), existing = arg(c, 1), key = arg(c, 2);
    uint32_t port = existing;
    if (!port) port = add_handle(std::make_shared<Port>());
    if (file != 0xffffffffu) {
        std::lock_guard<std::mutex> l(g_assoc_mutex);
        g_assoc[file] = {port, key};
    }
    trace("CreateIoCompletionPort(%08x, %08x, %08x) = %08x", file, existing, key, port);
    ret(c, port, 4);
}
// GetQueuedCompletionStatus(port, DWORD* bytes, ULONG_PTR* key, OVERLAPPED**, ms)
W32(kernel32, GetQueuedCompletionStatus) {
    auto port = handle_as<Port>(arg(c, 0));
    const uint32_t bytes = arg(c, 1), key = arg(c, 2), ov = arg(c, 3), ms = arg(c, 4);
    if (!port) {
        set_last_error(6);
        return ret(c, 0, 5);
    }
    std::unique_lock<std::mutex> l(port->m);
    auto ready = [&] { return !port->queue.empty(); };
    if (ms == 0xffffffffu) port->cv.wait(l, ready);
    else port->cv.wait_for(l, std::chrono::milliseconds(ms), ready);
    if (port->queue.empty()) {
        if (ov) wr(ov, 0);
        set_last_error(258);  // WAIT_TIMEOUT
        return ret(c, 0, 5);
    }
    const Port::Packet p = port->queue.front();
    port->queue.pop_front();
    if (bytes) wr(bytes, p.bytes);
    if (key) wr(key, p.key);
    if (ov) wr(ov, p.overlapped);
    ret(c, 1, 5);
}

// ---- file mappings ----

// CreateFileMappingA(file, sa, protect, size_hi, size_lo, name)
W32(kernel32, CreateFileMappingA) {
    auto m = std::make_shared<Mapping>();
    m->file = arg(c, 0);
    m->size_lo = arg(c, 4);
    if (m->file == 0xffffffffu && !m->size_lo) return ret(c, 0, 6);
    trace("CreateFileMappingA(%08x, %u)", m->file, m->size_lo);
    ret(c, add_handle(m), 6);
}
// MapViewOfFile(mapping, access, offset_hi, offset_lo, bytes): a copy of the
// file's bytes on the guest heap (pagefile mappings: zeros). Writes to a view
// do not reach the file.
W32(kernel32, MapViewOfFile) {
    auto m = handle_as<Mapping>(arg(c, 0));
    if (!m) return ret(c, 0, 5);
    const uint32_t off = arg(c, 3);
    uint32_t n = arg(c, 4);
    if (!n) {
        const uint32_t size = m->file == 0xffffffffu ? m->size_lo : call_shim(c, imp_kernel32_GetFileSize, {m->file, 0});
        n = (m->size_lo ? m->size_lo : size) - off;
    }
    const uint32_t view = galloc(n ? n : 1, 4096);
    if (!view) return ret(c, 0, 5);
    std::memset(gptr(view), 0, n);
    if (m->file != 0xffffffffu) {
        Scratch s(c);  // released before returning
        const uint32_t ov = s.alloc(20);
        std::memset(gptr(ov), 0, 20);
        wr(ov + 8, off);
        call_shim(c, imp_kernel32_ReadFile, {m->file, view, n, 0, ov});
    }
    {
        std::lock_guard<std::mutex> l(g_views_mutex);
        g_views[view] = n;
    }
    trace("MapViewOfFile(%08x, +%u, %u) = %08x", arg(c, 0), off, n, view);
    ret(c, view, 5);
}
W32(kernel32, UnmapViewOfFile) {
    std::lock_guard<std::mutex> l(g_views_mutex);
    if (g_views.erase(arg(c, 0))) gfree(arg(c, 0));
    ret(c, 1, 1);
}

// GetFileInformationByHandle(h, BY_HANDLE_FILE_INFORMATION*)
W32(kernel32, GetFileInformationByHandle) {
    const uint32_t h = arg(c, 0), info = arg(c, 1);
    uint32_t lo, hi_v;
    uint8_t times_v[24];
    bool have_times;
    {
        Scratch s(c);
        const uint32_t hi = s.alloc(4), times = s.alloc(24);
        wr(hi, 0);
        lo = call_shim(c, imp_kernel32_GetFileSize, {h, hi});
        hi_v = rd(hi);
        have_times = call_shim(c, imp_kernel32_GetFileTime, {h, times, times + 8, times + 16}) != 0;
        std::memcpy(times_v, gptr(times), 24);
    }
    if (lo == 0xffffffffu && last_error()) return ret(c, 0, 2);
    std::memset(gptr(info), 0, 52);
    wr(info, 0x80);  // FILE_ATTRIBUTE_NORMAL
    if (have_times) std::memcpy(gptr(info + 4), times_v, 24);
    wr(info + 28, 0x5c1a5c1a);  // volume serial
    wr(info + 32, hi_v);
    wr(info + 36, lo);
    wr(info + 40, 1);  // links
    wr(info + 48, h);  // file index
    ret(c, 1, 2);
}

W32(kernel32, UnlockFileEx) { ret(c, 1, 5); }
W32(kernel32, SetPriorityClass) { ret(c, 1, 2); }
W32(kernel32, PeekNamedPipe) { ret(c, 0, 6); }

// GetDiskFreeSpace{A,W}(root, sectors/cluster, bytes/sector, free clusters, clusters)
static void disk_free_space(Context& c) {
    const uint32_t v[4] = {8, 512, 0x00f00000, 0x01000000};  // 4 KB clusters: 60 of 64 GB free
    for (int k = 0; k < 4; k++)
        if (arg(c, 1 + k)) wr(arg(c, 1 + k), v[k]);
    ret(c, 1, 5);
}
W32(kernel32, GetDiskFreeSpaceA) { disk_free_space(c); }
W32(kernel32, GetDiskFreeSpaceW) { disk_free_space(c); }

W32(kernel32, InterlockedCompareExchange) {
    auto* p = reinterpret_cast<std::atomic<uint32_t>*>(gptr(arg(c, 0)));
    uint32_t expected = arg(c, 2);
    p->compare_exchange_strong(expected, arg(c, 1));
    ret(c, expected, 3);
}

// FormatMessageW: no message tables.
W32(kernel32, FormatMessageW) {
    set_last_error(317);  // ERROR_MR_MID_NOT_FOUND
    ret(c, 0, 7);
}

// ---- snapshots: no other processes or modules to see ----
W32(kernel32, CreateToolhelp32Snapshot) {
    set_last_error(5);
    ret(c, 0xffffffffu, 2);
}
W32(kernel32, Process32First) { set_last_error(18), ret(c, 0, 2); }  // ERROR_NO_MORE_FILES
W32(kernel32, Process32Next) { set_last_error(18), ret(c, 0, 2); }
W32(kernel32, Module32First) { set_last_error(18), ret(c, 0, 2); }
W32(kernel32, Module32Next) { set_last_error(18), ret(c, 0, 2); }

}  // namespace scimitar::imports
