// Process, environment, errno, time, sorting, virtual memory, dynamic
// loading and network imports.
//
// Networking (the Quazal online layer, Ubisoft services) is not supported:
// sockets fail with ENETDOWN and name lookups fail, so the game's online code
// sees no network.
#include <libgen.h>
#include <mach-o/dyld.h>
#include <sys/sysctl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <mutex>
#include <numeric>
#include <string>
#include <vector>

#include "scimitar/guest_heap.h"
#include "scimitar/imports.h"
#include "scimitar/runtime.h"

namespace scimitar {
uint32_t guest_file(GuestMemory& m, FILE* f);  // stdio.cpp
}

namespace scimitar::imports {
void imp_fclose(Context& c);  // stdio.cpp


void write_tm(Context& c, uint32_t p, const std::tm& t);  // libc_conv.cpp

namespace {

const char* S(Context& c, uint32_t a) { return (const char*)c.mem->ptr(a); }

std::mutex g_lock;

uint32_t guest_copy(Context& c, const std::string& s) {
    const uint32_t a = heap::alloc(*c.mem, (uint32_t)s.size() + 1);
    std::memcpy(c.mem->ptr(a), s.c_str(), s.size() + 1);
    return a;
}

// A guest string per key, created once and kept (getenv, strerror...).
uint32_t kept_string(Context& c, const std::string& key, const std::string& value) {
    static std::map<std::string, uint32_t> kept;
    std::lock_guard<std::mutex> l(g_lock);
    auto it = kept.find(key + '\0' + value);
    if (it != kept.end()) return it->second;
    return kept[key + '\0' + value] = guest_copy(c, value);
}

// Per-thread guest storage (errno, struct tm for gmtime/localtime...).
uint32_t thread_block(Context& c, int which, uint32_t size) {
    thread_local std::map<int, uint32_t> blocks;
    uint32_t& a = blocks[which];
    if (!a) {
        a = heap::alloc(*c.mem, size);
        std::memset(c.mem->ptr(a), 0, size);
    }
    return a;
}

}  // namespace

// int* __error(void): this thread's errno, in guest memory. Shims that fail
// set it from the host errno.
void imp___error(Context& c) { shim_return(c, thread_block(c, 0, 16)); }

uint32_t errno_address(Context& c) { return thread_block(c, 0, 16); }

}  // namespace scimitar::imports

namespace scimitar {
void set_guest_errno(Context& c, int e) { c.mem->write<uint32_t>(imports::errno_address(c), (uint32_t)e); }
}  // namespace scimitar

namespace scimitar::imports {

// ---- environment and process ----
void imp_getenv(Context& c) {
    const std::string name = S(c, arg32(c, 0));
    const char* v = std::getenv(name.c_str());
    shim_return(c, v ? kept_string(c, "env:" + name, v) : 0);
}
void imp_getpid(Context& c) { shim_return(c, (uint32_t)::getpid()); }
// The guest believes in 4 KB pages (the original hardware's).
void imp_getpagesize(Context& c) { shim_return(c, 4096); }
// int _NSGetExecutablePath(char* buf, uint32_t* size)
void imp__NSGetExecutablePath(Context& c) {
    char path[4096];
    uint32_t n = sizeof path;
    _NSGetExecutablePath(path, &n);
    // The game's own path, not the port's: from the arguments the port set up.
    std::string exe = process_arguments().empty() ? path : process_arguments()[0];
    const uint32_t buf = arg32(c, 0), size = c.mem->read<uint32_t>(arg32(c, 1));
    if (exe.size() + 1 > size) {
        c.mem->write<uint32_t>(arg32(c, 1), (uint32_t)exe.size() + 1);
        return shim_return(c, (uint32_t)-1);
    }
    std::memcpy(c.mem->ptr(buf), exe.c_str(), exe.size() + 1);
    shim_return(c, 0);
}
void imp_strerror_UNIX2003(Context& c) {
    const int e = (int)arg32(c, 0);
    shim_return(c, kept_string(c, "strerror:" + std::to_string(e), std::strerror(e)));
}
void imp_perror(Context& c) {
    std::fprintf(stderr, "[game] %s: %s\n", arg32(c, 0) ? S(c, arg32(c, 0)) : "",
                 std::strerror((int)c.mem->read<uint32_t>(errno_address(c))));
    shim_return(c, 0);
}
// void __assert_rtn(const char* func, const char* file, int line, const char* expr)
[[noreturn]] void imp___assert_rtn(Context& c) {
    fatal("assertion failed in the game: %s (%s:%d, %s)", S(c, arg32(c, 3)), S(c, arg32(c, 1)),
          (int)arg32(c, 2), S(c, arg32(c, 0)));
}
// Signals: handlers are not installed (the port handles faults itself).
void imp_sigaction(Context& c) {
    if (arg32(c, 2)) std::memset(c.mem->ptr(arg32(c, 2)), 0, 12);  // i386 struct sigaction
    shim_return(c, 0);
}
void imp_alarm(Context& c) { shim_return(c, 0); }
// No shell: the game uses system()/popen() only for side tasks (opening URLs...).
void imp_system_UNIX2003(Context& c) { shim_return(c, (uint32_t)-1); }
void imp_popen_UNIX2003(Context& c) { shim_return(c, 0); }
// FILE* popen(const char* command, const char* mode): only Brotherhood's
// hardware detection, `system_profiler SPDisplaysDataType -xml`, gets an
// answer: this machine's GPU, named as the CPU (the game knows no Apple GPU)
// with 2 GB of video memory, which the game's table maps to its high
// settings profile (an unknown card gets the lowest settings and a warning).
namespace {
std::mutex g_popen_lock;
std::vector<uint32_t> g_popened;
std::string display_profile() {
    char model[128] = "Apple GPU";
    size_t n = sizeof model;
    sysctlbyname("machdep.cpu.brand_string", model, &n, nullptr, 0);
    return std::string("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<plist version=\"1.0\"><array><dict>"
                       "<key>_dataType</key><string>SPDisplaysDataType</string><key>_items</key><array><dict>"
                       "<key>sppci_model</key><string>") +
           model +
           "</string><key>spdisplays_vram</key><string>2048 MB</string>"
           "<key>spdisplays_vendor</key><string>Apple</string></dict></array></dict></array></plist>\n";
}
}  // namespace
void imp_popen(Context& c) {
    const std::string cmd = S(c, arg32(c, 0));
    if (cmd.rfind("system_profiler SPDisplaysDataType", 0) != 0) return shim_return(c, 0);
    static const std::string profile = display_profile();
    FILE* f = fmemopen((void*)profile.data(), profile.size(), "r");
    if (!f) return shim_return(c, 0);
    const uint32_t g = guest_file(*c.mem, f);
    std::lock_guard<std::mutex> l(g_popen_lock);
    g_popened.push_back(g);
    shim_return(c, g);
}
void imp_pclose(Context& c) {
    {
        std::lock_guard<std::mutex> l(g_popen_lock);
        auto it = std::find(g_popened.begin(), g_popened.end(), arg32(c, 0));
        if (it == g_popened.end()) return shim_return(c, (uint32_t)-1);
        g_popened.erase(it);
    }
    imp_fclose(c);  // same argument, and 0 when it closes
}
// int setjmp: returns 0 the first time; no import longjmps back (the game's
// Objective-C exceptions are not supported, see objc.cpp).
void imp__setjmp(Context& c) { shim_return(c, 0); }
// Dynamic loading: the game's only dlopen is the Orbit library, whose loader
// is overridden; anything else fails.
void imp_dlopen(Context& c) { shim_return(c, 0); }
void imp_dlsym(Context& c) { shim_return(c, 0); }
void imp_dlerror(Context& c) { shim_return(c, kept_string(c, "dlerror", "dynamic loading is not supported")); }
void imp_basename(Context& c) {
    std::string p = S(c, arg32(c, 0));
    std::vector<char> buf(p.begin(), p.end());
    buf.push_back(0);
    const std::string b = ::basename(buf.data());
    const uint32_t out = thread_block(c, 1, 1024);
    strlcpy((char*)c.mem->ptr(out), b.c_str(), 1024);
    shim_return(c, out);
}

// ---- random numbers ----
void imp_rand(Context& c) { shim_return(c, (uint32_t)std::rand()); }
void imp_srand(Context& c) {
    std::srand(arg32(c, 0));
    shim_return(c, 0);
}

// ---- time (i386 time_t is 32 bits; struct tm is 44 bytes) ----
namespace {
void tm_result(Context& c, const std::tm* t, uint32_t out) {
    if (!t) return shim_return(c, 0);
    write_tm(c, out, *t);
    shim_return(c, out);
}
}  // namespace
void imp_gmtime(Context& c) {
    const time_t t = (int32_t)c.mem->read<uint32_t>(arg32(c, 0));
    std::tm r;
    tm_result(c, gmtime_r(&t, &r), thread_block(c, 2, 48));
}
void imp_localtime(Context& c) {
    const time_t t = (int32_t)c.mem->read<uint32_t>(arg32(c, 0));
    std::tm r;
    tm_result(c, localtime_r(&t, &r), thread_block(c, 3, 48));
}
void imp_localtime_r(Context& c) {
    const time_t t = (int32_t)c.mem->read<uint32_t>(arg32(c, 0));
    std::tm r;
    tm_result(c, localtime_r(&t, &r), arg32(c, 1));
}
void imp_mktime_UNIX2003(Context& c) {
    std::tm t{};
    const uint32_t p = arg32(c, 0);
    int* f[9] = {&t.tm_sec, &t.tm_min, &t.tm_hour, &t.tm_mday, &t.tm_mon,
                 &t.tm_year, &t.tm_wday, &t.tm_yday, &t.tm_isdst};
    for (int k = 0; k < 9; k++) *f[k] = (int32_t)c.mem->read<uint32_t>(p + 4 * k);
    const time_t r = std::mktime(&t);
    write_tm(c, p, t);
    shim_return(c, (uint32_t)(int32_t)r);
}
void imp_asctime(Context& c) {
    std::tm t{};
    const uint32_t p = arg32(c, 0);
    int* f[9] = {&t.tm_sec, &t.tm_min, &t.tm_hour, &t.tm_mday, &t.tm_mon,
                 &t.tm_year, &t.tm_wday, &t.tm_yday, &t.tm_isdst};
    for (int k = 0; k < 9; k++) *f[k] = (int32_t)c.mem->read<uint32_t>(p + 4 * k);
    char buf[64];
    asctime_r(&t, buf);
    const uint32_t out = thread_block(c, 4, 64);
    std::memcpy(c.mem->ptr(out), buf, std::strlen(buf) + 1);
    shim_return(c, out);
}

// ---- sorting: the comparator is guest code ----
// void qsort(void* base, size_t n, size_t size, int (*compar)(const void*, const void*))
void imp_qsort(Context& c) {
    const uint32_t base = arg32(c, 0), n = arg32(c, 1), size = arg32(c, 2), cmp = arg32(c, 3);
    if (n < 2 || !size) return shim_return(c, 0);
    std::vector<uint32_t> order(n);
    std::iota(order.begin(), order.end(), 0u);
    std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
        return (int32_t)call_guest(c, cmp, {base + a * size, base + b * size}) < 0;
    });
    std::vector<uint8_t> copy(c.mem->ptr(base), c.mem->ptr(base) + (size_t)n * size);
    for (uint32_t k = 0; k < n; k++)
        std::memcpy(c.mem->ptr(base + k * size), copy.data() + (size_t)order[k] * size, size);
    shim_return(c, 0);
}

// ---- virtual memory: from the guest heap, page aligned, zeroed ----
// kern_return_t mach_vm_allocate(vm_map_t, mach_vm_address_t* addr, mach_vm_size_t size, int flags)
void imp_mach_vm_allocate(Context& c) {
    const uint32_t out = arg32(c, 1);
    const uint64_t size = arg64(c, 2);
    if (size >> 31) return shim_return(c, 3);  // KERN_NO_SPACE
    if (std::getenv("SCIMITAR_TRACE_BIG_ALLOCS") && size >= (8u << 20))
        std::fprintf(stderr, "mach_vm_allocate(%llu MB) from %08x\n", (unsigned long long)(size >> 20),
                     c.mem->read<uint32_t>(c.esp));
    heap::GuestSite site(c);
    const uint32_t a = heap::alloc(*c.mem, (uint32_t)size, 4096);
    if (!a) return shim_return(c, 3);
    std::memset(c.mem->ptr(a), 0, (size_t)size);
    c.mem->write<uint64_t>(out, a);
    shim_return(c, 0);
}
// kern_return_t mach_vm_deallocate(vm_map_t, mach_vm_address_t addr, mach_vm_size_t size)
void imp_mach_vm_deallocate(Context& c) {
    heap::release(*c.mem, (uint32_t)arg64(c, 1));
    shim_return(c, 0);
}
void imp_mach_error_string(Context& c) {
    shim_return(c, kept_string(c, "mach_error", "mach error " + std::to_string(arg32(c, 0))));
}

// ---- network: unavailable ----
namespace {
void net_fail(Context& c) {
    set_guest_errno(c, ENETDOWN);
    shim_return(c, (uint32_t)-1);
}
}  // namespace
void imp_socket(Context& c) { net_fail(c); }
void imp_bind(Context& c) { net_fail(c); }
void imp_connect(Context& c) { net_fail(c); }
void imp_listen(Context& c) { net_fail(c); }
void imp_accept(Context& c) { net_fail(c); }
void imp_send(Context& c) { net_fail(c); }
void imp_recv(Context& c) { net_fail(c); }
void imp_sendto(Context& c) { net_fail(c); }
void imp_recvfrom(Context& c) { net_fail(c); }
void imp_select(Context& c) { net_fail(c); }
void imp_poll(Context& c) { net_fail(c); }
void imp_setsockopt(Context& c) { net_fail(c); }
void imp_getsockopt(Context& c) { net_fail(c); }
void imp_getsockname(Context& c) { net_fail(c); }
void imp_ioctl(Context& c) { net_fail(c); }
// The $UNIX2003 variants generated code calls by their own names.
void imp_bind_UNIX2003(Context& c) { net_fail(c); }
void imp_connect_UNIX2003(Context& c) { net_fail(c); }
void imp_listen_UNIX2003(Context& c) { net_fail(c); }
void imp_accept_UNIX2003(Context& c) { net_fail(c); }
void imp_send_UNIX2003(Context& c) { net_fail(c); }
void imp_recv_UNIX2003(Context& c) { net_fail(c); }
void imp_sendto_UNIX2003(Context& c) { net_fail(c); }
void imp_recvfrom_UNIX2003(Context& c) { net_fail(c); }
void imp_select_UNIX2003(Context& c) { net_fail(c); }
void imp_poll_UNIX2003(Context& c) { net_fail(c); }
void imp_getsockname_UNIX2003(Context& c) { net_fail(c); }
// int getifaddrs(struct ifaddrs**): no interfaces to report (Brotherhood's
// network layer looks for its LAN address).
void imp_getifaddrs(Context& c) { net_fail(c); }
void imp_freeifaddrs(Context& c) { shim_return(c, 0); }
// int gethostname(char* name, size_t len)
void imp_gethostname(Context& c) {
    const uint32_t name = arg32(c, 0), len = arg32(c, 1);
    static const char kHost[] = "localhost";
    if (len < sizeof kHost) return net_fail(c);
    std::memcpy(c.mem->ptr(name), kHost, sizeof kHost);
    shim_return(c, 0);
}
// int getnameinfo(...): EAI_FAIL
void imp_getnameinfo(Context& c) { shim_return(c, 4); }
void imp_gai_strerror(Context& c) { shim_return(c, kept_string(c, "gai_strerror", "name lookup unavailable")); }
void imp_gethostbyname(Context& c) { shim_return(c, 0); }
void imp_inet_addr(Context& c) { shim_return(c, 0xffffffffu); }  // INADDR_NONE
void imp_inet_ntoa(Context& c) { shim_return(c, kept_string(c, "inet_ntoa", "0.0.0.0")); }

}  // namespace scimitar::imports

namespace scimitar {
namespace {
using namespace imports;
const ImportShim kProcess[] = {
    {"___error", imp___error},       {"_getenv", imp_getenv},       {"_getpid", imp_getpid},
    {"_getpagesize", imp_getpagesize}, {"__NSGetExecutablePath", imp__NSGetExecutablePath},
    {"_strerror$UNIX2003", imp_strerror_UNIX2003}, {"_perror", imp_perror},
    {"___assert_rtn", imp___assert_rtn}, {"_sigaction", imp_sigaction}, {"_alarm", imp_alarm},
    {"_system$UNIX2003", imp_system_UNIX2003}, {"_popen$UNIX2003", imp_popen_UNIX2003},
    {"_pclose", imp_pclose},         {"__setjmp", imp__setjmp},     {"_dlopen", imp_dlopen},
    {"_dlsym", imp_dlsym},           {"_dlerror", imp_dlerror},     {"_basename", imp_basename},
    {"_rand", imp_rand},             {"_srand", imp_srand},         {"_gmtime", imp_gmtime},
    {"_localtime", imp_localtime},   {"_localtime_r", imp_localtime_r},
    {"_mktime$UNIX2003", imp_mktime_UNIX2003}, {"_asctime", imp_asctime}, {"_qsort", imp_qsort},
    {"_mach_vm_allocate", imp_mach_vm_allocate}, {"_mach_vm_deallocate", imp_mach_vm_deallocate},
    {"_mach_error_string", imp_mach_error_string},
    {"_socket", imp_socket},         {"_bind", imp_bind},           {"_bind$UNIX2003", imp_bind},
    {"_connect", imp_connect},       {"_connect$UNIX2003", imp_connect}, {"_listen", imp_listen},
    {"_listen$UNIX2003", imp_listen}, {"_accept", imp_accept},      {"_accept$UNIX2003", imp_accept},
    {"_send", imp_send},             {"_send$UNIX2003", imp_send},  {"_recv", imp_recv},
    {"_recv$UNIX2003", imp_recv},    {"_sendto$UNIX2003", imp_sendto},
    {"_recvfrom$UNIX2003", imp_recvfrom}, {"_select", imp_select}, {"_select$UNIX2003", imp_select},
    {"_poll$UNIX2003", imp_poll},    {"_setsockopt", imp_setsockopt}, {"_getsockopt", imp_getsockopt},
    {"_getsockname$UNIX2003", imp_getsockname}, {"_ioctl", imp_ioctl},
    {"_gethostbyname", imp_gethostbyname}, {"_inet_addr", imp_inet_addr}, {"_inet_ntoa", imp_inet_ntoa},
    {"_getifaddrs", imp_getifaddrs}, {"_freeifaddrs", imp_freeifaddrs}, {"_gethostname", imp_gethostname},
    {"_getnameinfo", imp_getnameinfo}, {"_gai_strerror", imp_gai_strerror}, {"_popen", imp_popen},
};
SCIMITAR_REGISTER_SHIMS(kProcess);
}  // namespace
}  // namespace scimitar
