#pragma once
// The Windows platform layer: native implementations of the Win32 and DirectX
// functions a recompiled Windows executable imports (docs/AC1_WINDOWS.md).
//
// Shims are named after the import, "kernel32!CreateFileA" ->
// scimitar::imports::imp_kernel32_CreateFileA (recomp's import_symbol), and
// follow the import's calling convention on the guest stack: Win32 and COM
// methods are stdcall (the callee pops its arguments: finish with `ret(c, value,
// nargs)`), the C runtime's are cdecl. The generated code lists every import
// with its implementation (or a weak stub that stops the run), and the loader
// fills the import address table with host functions for them.
//
// COM objects (Direct3D, DirectInput, DirectSound) live in guest memory: a
// vtable pointer then the object's guest-visible fields; the vtable holds host
// function addresses (register_host_function), so the game's virtual calls
// reach native methods. The native state hangs off a table keyed by the
// object's guest address.
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "scimitar/runtime.h"

namespace recomp {
class Image;
}

namespace scimitar::win32 {

// The process's guest memory (one per process).
GuestMemory& mem();
inline uint8_t* gptr(uint32_t a) { return mem().ptr(a); }
inline uint32_t rd(uint32_t a) { return mem().read<uint32_t>(a); }
inline void wr(uint32_t a, uint32_t v) { mem().write<uint32_t>(a, v); }
inline uint16_t rd16(uint32_t a) { return mem().read<uint16_t>(a); }
inline void wr16(uint32_t a, uint16_t v) { mem().write<uint16_t>(a, v); }

// stdcall: returns `eax` and pops the return address and `nargs` argument words.
inline void ret(Context& c, uint32_t eax, int nargs) {
    c.eax = eax;
    c.esp += 4 + 4u * (uint32_t)nargs;
}
inline void ret64(Context& c, uint64_t v, int nargs) {
    c.edx = (uint32_t)(v >> 32);
    ret(c, (uint32_t)v, nargs);
}
inline uint32_t arg(const Context& c, int i) { return arg32(c, i); }

// Guest strings.
std::string gstr(uint32_t a);                       // char*, "" for null
std::u16string gwstr(uint32_t a);                   // wchar_t* (UTF-16)
std::string utf8(const std::u16string& s);
std::u16string utf16(const std::string& s);
// Copies `s` with its terminator into a buffer of `cap` chars; returns the
// length copied (without the terminator), truncating to cap - 1.
uint32_t put_str(uint32_t dst, uint32_t cap, const std::string& s);
uint32_t put_wstr(uint32_t dst, uint32_t cap, const std::u16string& s);
uint32_t guest_strdup(const std::string& s);  // on the guest heap

// Guest heap (the platform's, scimitar/guest_heap.h).
uint32_t galloc(uint32_t size, uint32_t align = 16);
void gfree(uint32_t p);

// GetLastError/SetLastError: the thread's error, in its TEB as on Windows.
void set_last_error(uint32_t e);
uint32_t last_error();

// Kernel objects behind HANDLEs.
struct Object {
    virtual ~Object() = default;
};
uint32_t add_handle(std::shared_ptr<Object> o);
std::shared_ptr<Object> get_handle(uint32_t h);
template <typename T> std::shared_ptr<T> handle_as(uint32_t h) {
    return std::dynamic_pointer_cast<T>(get_handle(h));
}
bool close_handle(uint32_t h);
constexpr uint32_t kCurrentProcess = 0xffffffffu, kCurrentThread = 0xfffffffeu;

// Waitable objects: events, semaphores, mutexes, threads (WaitFor*Object*).
struct Waitable : Object {
    // Takes the object if it is signalled (resets auto-reset events, counts a
    // semaphore down, takes a mutex); true if it was.
    virtual bool try_acquire() = 0;
    virtual bool signalled() const = 0;
};
// Sets an event (CreateEventA's), as SetEvent does.
void signal_event(uint32_t handle);
// An overlapped read or write on `file` finished (ReadFile and WriteFile do
// them at once): queues its completion packet if the file belongs to an I/O
// completion port (kernel32_acr.cpp).
void overlapped_done(uint32_t file, uint32_t overlapped, uint32_t bytes);

// COM classes beyond system.cpp's (acr_misc.cpp): if `clsid` is one, creates
// it, stores the object at `out`, sets *hr and returns true.
bool create_com_instance(uint32_t clsid, uint32_t iid, uint32_t out, uint32_t* hr);

// Wakes every waiter so they look again (call after signalling something).
void notify_waiters();
// Waits until `pred` holds or `ms` milliseconds pass (0xffffffff: forever).
bool wait_until(uint32_t ms, const std::function<bool()>& pred);

// Threads. Every guest thread has a TEB at fs:0: the SEH chain at +0, stack
// bounds, its own address at +0x18, ids, the static-TLS array at +0x2c, the
// PEB at +0x30, last error at +0x34, TlsAlloc slots at +0xe10.
uint32_t create_teb(uint32_t stack_base, uint32_t stack_limit);
// Sets up the calling host thread as a guest thread: TEB, static TLS, context.
void attach_thread(Context& c, uint32_t stack_base, uint32_t stack_limit);
uint32_t current_thread_id();

// The executable as loaded: its image, base and path.
struct Process {
    const recomp::Image* image = nullptr;
    std::string game_id;      // "ac1", "acr": names the port's folders and log lines
    std::string game_dir;     // the executable's folder as the game sees it, "C:\AC1"
    std::string exe_path;     // Windows-style, "C:\AC1\AssassinsCreed_Dx9.exe"
    std::string host_dir;     // the game's folder on this Mac
    std::string command_line; // as GetCommandLineA returns it
    uint32_t peb = 0;
};
Process& process();
// Fills the import address table and the static-TLS index; call once.
void load_process(GuestMemory& m, const recomp::Image& image, const std::string& host_exe,
                  const std::vector<std::string>& args, const std::string& game_id = "ac1");

// Windows paths as host paths: '\' becomes '/', the game's drive letter maps
// to the game folder's volume ("C:\..." to "/..."), relative paths stay
// relative (the host working directory is the game folder).
std::string host_path(const std::string& win);
// A REG_DWORD the game reads when it has not written one itself (a port's
// defaults). `key` is a full path, "HKCU\\Software\\...".
void registry_default_dword(const std::string& key, const std::string& name, uint32_t value);
// The port's folder for the game's own files outside its install folder:
// ~/Library/Application Support/scimitar-recomp/<game id>.
std::string support_dir();

// Shims reached only through GetProcAddress (not imported by the executable).
struct DynamicShim {
    const char* name;  // "kernel32!IsProcessorFeaturePresent"
    GuestFn fn;
};
std::vector<DynamicShim>& dynamic_shims();
struct DynamicRegistrar {
    DynamicRegistrar(const DynamicShim* shims, size_t n);
};

// Debugging: SCIMITAR_TRACE_WIN32 logs every shim call that uses trace().
bool tracing();
[[gnu::format(printf, 1, 2)]] void trace(const char* fmt, ...);

}  // namespace scimitar::win32

// Defines a shim: W32(kernel32, CreateFileA) { ... }.
#define W32(dll, fn) void imp_##dll##_##fn(::scimitar::Context& c)
