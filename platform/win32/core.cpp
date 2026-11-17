// The Windows layer's core: guest memory and strings, handles, waiting,
// threads' TEBs and static TLS, and loading the process (win32.h).
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <map>
#include <mutex>

#include "macho.h"
#include "scimitar/guest_heap.h"
#include "win32.h"

namespace scimitar::win32 {

namespace {
GuestMemory* g_mem = nullptr;
}

GuestMemory& mem() { return *g_mem; }

// ---- strings ----

std::string gstr(uint32_t a) {
    if (!a) return "";
    return std::string(reinterpret_cast<const char*>(gptr(a)));
}

std::u16string gwstr(uint32_t a) {
    std::u16string s;
    if (!a) return s;
    for (uint16_t ch; (ch = rd16(a)) != 0; a += 2) s += (char16_t)ch;
    return s;
}

std::string utf8(const std::u16string& s) {
    std::string out;
    for (size_t k = 0; k < s.size(); k++) {
        uint32_t cp = s[k];
        if (cp >= 0xd800 && cp < 0xdc00 && k + 1 < s.size()) cp = 0x10000 + ((cp - 0xd800) << 10) + (s[++k] - 0xdc00);
        if (cp < 0x80) out += (char)cp;
        else if (cp < 0x800) out += (char)(0xc0 | cp >> 6), out += (char)(0x80 | (cp & 0x3f));
        else if (cp < 0x10000)
            out += (char)(0xe0 | cp >> 12), out += (char)(0x80 | (cp >> 6 & 0x3f)), out += (char)(0x80 | (cp & 0x3f));
        else
            out += (char)(0xf0 | cp >> 18), out += (char)(0x80 | (cp >> 12 & 0x3f)),
                out += (char)(0x80 | (cp >> 6 & 0x3f)), out += (char)(0x80 | (cp & 0x3f));
    }
    return out;
}

std::u16string utf16(const std::string& s) {
    std::u16string out;
    for (size_t k = 0; k < s.size();) {
        const uint8_t b = (uint8_t)s[k];
        uint32_t cp = b;
        int n = 0;
        if (b >= 0xf0) cp = b & 7, n = 3;
        else if (b >= 0xe0) cp = b & 15, n = 2;
        else if (b >= 0xc0) cp = b & 31, n = 1;
        k++;
        for (; n > 0 && k < s.size(); n--, k++) cp = cp << 6 | ((uint8_t)s[k] & 0x3f);
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out += (char16_t)(0xd800 + (cp >> 10));
            out += (char16_t)(0xdc00 + (cp & 0x3ff));
        } else {
            out += (char16_t)cp;
        }
    }
    return out;
}

uint32_t put_str(uint32_t dst, uint32_t cap, const std::string& s) {
    if (!dst || !cap) return 0;
    const uint32_t n = std::min<uint32_t>((uint32_t)s.size(), cap - 1);
    std::memcpy(gptr(dst), s.data(), n);
    gptr(dst)[n] = 0;
    return n;
}

uint32_t put_wstr(uint32_t dst, uint32_t cap, const std::u16string& s) {
    if (!dst || !cap) return 0;
    const uint32_t n = std::min<uint32_t>((uint32_t)s.size(), cap - 1);
    for (uint32_t k = 0; k < n; k++) wr16(dst + 2 * k, s[k]);
    wr16(dst + 2 * n, 0);
    return n;
}

uint32_t galloc(uint32_t size, uint32_t align) {
    const uint32_t p = heap::alloc(mem(), size ? size : 1, align);
    if (!p) fatal("guest heap exhausted (%u bytes)", size);
    return p;
}

void gfree(uint32_t p) { heap::release(mem(), p); }

uint32_t guest_strdup(const std::string& s) {
    const uint32_t a = galloc((uint32_t)s.size() + 1);
    std::memcpy(gptr(a), s.c_str(), s.size() + 1);
    return a;
}

// ---- handles ----

namespace {
std::mutex g_handles_lock;
std::map<uint32_t, std::shared_ptr<Object>> g_handles;
uint32_t g_next_handle = 0x100;
}  // namespace

uint32_t add_handle(std::shared_ptr<Object> o) {
    std::lock_guard<std::mutex> l(g_handles_lock);
    const uint32_t h = g_next_handle;
    g_next_handle += 4;
    g_handles[h] = std::move(o);
    return h;
}

std::shared_ptr<Object> get_handle(uint32_t h) {
    std::lock_guard<std::mutex> l(g_handles_lock);
    auto it = g_handles.find(h);
    return it == g_handles.end() ? nullptr : it->second;
}

bool close_handle(uint32_t h) {
    std::shared_ptr<Object> keep;  // destroyed outside the lock
    std::lock_guard<std::mutex> l(g_handles_lock);
    auto it = g_handles.find(h);
    if (it == g_handles.end()) return false;
    keep = std::move(it->second);
    g_handles.erase(it);
    return true;
}

// ---- waiting ----
// One condition variable for every waitable object: waits are few and short
// enough that waking all waiters on any change costs nothing measurable.

namespace {
std::mutex g_wait_lock;
std::condition_variable g_wait_cv;
uint64_t g_wait_epoch = 0;
}  // namespace

void notify_waiters() {
    {
        std::lock_guard<std::mutex> l(g_wait_lock);
        g_wait_epoch++;
    }
    g_wait_cv.notify_all();
}

bool wait_until(uint32_t ms, const std::function<bool()>& pred) {
    using clock = std::chrono::steady_clock;
    const auto deadline = clock::now() + std::chrono::milliseconds(ms);
    std::unique_lock<std::mutex> l(g_wait_lock);
    for (;;) {
        const uint64_t epoch = g_wait_epoch;
        l.unlock();
        if (pred()) return true;
        l.lock();
        if (ms == 0) return false;
        if (g_wait_epoch != epoch) continue;
        if (ms == 0xffffffffu) {
            g_wait_cv.wait_for(l, std::chrono::milliseconds(50));
        } else {
            if (clock::now() >= deadline) return false;
            g_wait_cv.wait_until(l, std::min(deadline, clock::now() + std::chrono::milliseconds(50)));
        }
    }
}

// ---- threads ----

namespace {
std::atomic<uint32_t> g_next_tid{0x1000};
thread_local uint32_t t_tid = 0;
constexpr uint32_t kTebSize = 0x1000;
}  // namespace

uint32_t current_thread_id() {
    if (!t_tid) t_tid = g_next_tid += 4;
    return t_tid;
}

uint32_t create_teb(uint32_t stack_base, uint32_t stack_limit) {
    const uint32_t teb = galloc(kTebSize, 4096);
    std::memset(gptr(teb), 0, kTebSize);
    wr(teb + 0x00, 0xffffffffu);  // no SEH frame
    wr(teb + 0x04, stack_base);
    wr(teb + 0x08, stack_limit);
    wr(teb + 0x18, teb);
    wr(teb + 0x20, 0x1000);  // process id
    wr(teb + 0x24, current_thread_id());
    wr(teb + 0x30, process().peb);
    return teb;
}

void attach_thread(Context& c, uint32_t stack_base, uint32_t stack_limit) {
    c.mem = &mem();
    c.fs_base = create_teb(stack_base, stack_limit);
    // Static TLS: the template, then zeros; the array's slot `index` (0).
    const recomp::TlsDirectory& tls = process().image->tls();
    if (tls.start) {
        const uint32_t size = tls.end - tls.start + tls.zero_fill;
        const uint32_t block = galloc(size + 16);
        std::memset(gptr(block), 0, size + 16);
        std::memcpy(gptr(block), gptr(tls.start), tls.end - tls.start);
        const uint32_t array = galloc(64 * 4);
        std::memset(gptr(array), 0, 64 * 4);
        wr(array + 4 * rd(tls.index_addr), block);
        wr(c.fs_base + 0x2c, array);
    }
    set_thread_context(&c);
}

void set_last_error(uint32_t e) {
    if (Context* c = thread_context()) wr(c->fs_base + 0x34, e);
}

uint32_t last_error() {
    Context* c = thread_context();
    return c ? rd(c->fs_base + 0x34) : 0;
}

// ---- the process ----

Process& process() {
    static Process p;
    return p;
}

namespace {
// Imports with no implementation at all (not even a generated stub) stop the
// run with their name: one host function per IAT slot, looked up by address.
std::map<uint32_t, std::string> g_missing;
void missing_import(Context& c) {
    const uint32_t ret_addr = c.mem->read<uint32_t>(c.esp);
    fatal("an import with no implementation was called (from %08x)", ret_addr);
}
}  // namespace

void load_process(GuestMemory& m, const recomp::Image& image, const std::string& host_exe,
                  const std::vector<std::string>& args, const std::string& game_id) {
    g_mem = &m;
    Process& p = process();
    p.image = &image;
    p.game_id = game_id;
    p.game_dir = "C:\\";
    for (char ch : game_id) p.game_dir += (char)std::toupper((unsigned char)ch);
    const size_t slash = host_exe.rfind('/');
    p.host_dir = slash == std::string::npos ? "." : host_exe.substr(0, slash);
    const std::string name = slash == std::string::npos ? host_exe : host_exe.substr(slash + 1);
    p.exe_path = p.game_dir + "\\" + name;
    p.command_line = "\"" + p.exe_path + "\"";
    for (size_t k = 1; k < args.size(); k++) p.command_line += " " + args[k];

    // A process environment block: the image base, not being debugged.
    p.peb = galloc(0x1000, 4096);
    std::memset(gptr(p.peb), 0, 0x1000);
    wr(p.peb + 0x08, image.image_base());

    // The import address table: each slot gets a host function for the
    // import's implementation.
    size_t bound = 0, unbound = 0;
    for (const auto& [slot, imp] : image.stubs()) {
        const ImportEntry* e = find_import(imp.name.c_str());
        uint32_t addr;
        if (e) {
            addr = register_host_function(e->fn, e->name);
            bound++;
        } else {
            addr = register_host_function(missing_import, "missing import");
            g_missing[slot] = imp.name;
            unbound++;
        }
        m.write<uint32_t>(slot, addr);
    }
    if (tracing()) std::fprintf(stderr, "win32: %zu imports bound, %zu without a stub\n", bound, unbound);
}

std::string support_dir() {
    static const std::string dir = [] {
        const char* home = std::getenv("HOME");
        std::string d = std::string(home ? home : "/tmp") + "/Library/Application Support/scimitar-recomp";
        ::mkdir(d.c_str(), 0755);
        d += "/" + process().game_id;
        ::mkdir(d.c_str(), 0755);
        return d;
    }();
    return dir;
}

std::string host_path(const std::string& win) {
    std::string s = win;
    for (char& ch : s)
        if (ch == '\\') ch = '/';
    // Drives: "C:/AC1" (game_dir: the executable's folder as the game sees it) is the
    // game folder; "D:" (special folders: saves, settings) and the rest of
    // C: live in the port's own folder, never elsewhere on this Mac.
    if (s.size() >= 2 && s[1] == ':') {
        const char drive = (char)std::toupper((unsigned char)s[0]);
        std::string rest = s.substr(2);
        if (!rest.empty() && rest[0] != '/') rest = "/" + rest;
        const std::string own = "/" + process().game_dir.substr(3);
        if (drive == 'C' && rest.size() >= own.size() && strncasecmp(rest.c_str(), own.c_str(), own.size()) == 0 &&
            (rest.size() == own.size() || rest[own.size()] == '/'))
            return process().host_dir + rest.substr(own.size());
        const std::string root = support_dir() + "/" + std::string(1, (char)std::tolower(drive));
        ::mkdir(root.c_str(), 0755);
        return root + rest;
    }
    return s;
}

bool tracing() {
    static const bool on = std::getenv("SCIMITAR_TRACE_WIN32") != nullptr;
    return on;
}

void trace(const char* fmt, ...) {
    if (!tracing()) return;
    char line[2048];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    std::fprintf(stderr, "win32: %s\n", line);
}

}  // namespace scimitar::win32
