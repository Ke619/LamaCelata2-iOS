// KERNEL32, part 1: the process, modules, environment, errors, system
// information, code pages, locale and console (the statically linked C
// runtime's needs). Files are in kernel32_file.cpp, threads, synchronisation,
// memory and time in kernel32_sync.cpp.
#include <sys/sysctl.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>

#include "macho.h"
#include "win32.h"

namespace scimitar::win32 {

// Modules: the executable is its image base; the system DLLs the game names
// are handles of nothing, whose functions GetProcAddress finds among the
// imports the generated code lists and the shims only reached that way.
namespace {
std::mutex g_modules_lock;
std::map<std::string, uint32_t> g_modules;  // lower-case base name -> handle

std::string dll_base(const std::string& name) {
    std::string s = name;
    const size_t slash = s.find_last_of("\\/");
    if (slash != std::string::npos) s = s.substr(slash + 1);
    for (char& ch : s) ch = (char)std::tolower((unsigned char)ch);
    if (s.size() > 4 && s.compare(s.size() - 4, 4, ".dll") == 0) s.resize(s.size() - 4);
    return s;
}

std::string module_name(uint32_t h) {
    std::lock_guard<std::mutex> l(g_modules_lock);
    for (const auto& [n, v] : g_modules)
        if (v == h) return n;
    return "";
}

uint32_t module_handle(const std::string& base) {
    std::lock_guard<std::mutex> l(g_modules_lock);
    auto it = g_modules.find(base);
    if (it != g_modules.end()) return it->second;
    const uint32_t h = 0x60000000u + 0x10000u * (uint32_t)(g_modules.size() + 1);
    g_modules[base] = h;
    return h;
}

// Libraries the port provides (imported or loaded by name); others fail to load.
bool known_dll(const std::string& base) {
    static const char* kKnown[] = {"kernel32", "user32", "advapi32", "shell32", "ole32", "oleaut32",
                                   "ws2_32", "eax", "d3d9", "d3dx9_36", "dinput8", "dsound",
                                   "xinput1_3", "winmm", "binkw32", "wininet", "iphlpapi", "ntdll"};
    for (const char* k : kKnown)
        if (base == k) return true;
    return false;
}
}  // namespace

std::vector<DynamicShim>& dynamic_shims() {
    static std::vector<DynamicShim> v;
    return v;
}

DynamicRegistrar::DynamicRegistrar(const DynamicShim* shims, size_t n) {
    dynamic_shims().insert(dynamic_shims().end(), shims, shims + n);
}

}  // namespace scimitar::win32

namespace scimitar::imports {
using namespace scimitar::win32;

// ---- modules ----

W32(kernel32, GetModuleHandleA) {
    const uint32_t name = arg(c, 0);
    if (!name) return ret(c, process().image->image_base(), 1);
    const std::string base = dll_base(gstr(name));
    trace("GetModuleHandleA(%s)", base.c_str());
    if (base == "assassinscreed_dx9" || base == "assassinscreed_dx10")
        return ret(c, process().image->image_base(), 1);
    if (!known_dll(base)) {
        set_last_error(126);  // ERROR_MOD_NOT_FOUND
        return ret(c, 0, 1);
    }
    ret(c, module_handle(base), 1);
}

W32(kernel32, LoadLibraryA) {
    const std::string base = dll_base(gstr(arg(c, 0)));
    trace("LoadLibraryA(%s)", gstr(arg(c, 0)).c_str());
    if (!known_dll(base)) {
        set_last_error(126);
        return ret(c, 0, 1);
    }
    ret(c, module_handle(base), 1);
}

W32(kernel32, FreeLibrary) { ret(c, 1, 1); }

W32(kernel32, GetProcAddress) {
    const uint32_t module = arg(c, 0), name = arg(c, 1);
    std::string base = module == process().image->image_base() ? "" : module_name(module);
    const std::string fn = name < 0x10000 ? "ord" + std::to_string(name) : gstr(name);
    const std::string full = base + "!" + fn;
    uint32_t addr = 0;
    static std::mutex lock;
    static std::map<std::string, uint32_t> resolved;
    std::lock_guard<std::mutex> l(lock);
    if (auto it = resolved.find(full); it != resolved.end()) {
        addr = it->second;
    } else {
        GuestFn f = nullptr;
        for (const DynamicShim& d : dynamic_shims())
            if (full == d.name) f = d.fn;
        if (!f)
            if (const ImportEntry* e = find_import(full.c_str())) f = e->fn;
        if (f) addr = register_host_function(f, strdup(full.c_str()));
        resolved[full] = addr;
    }
    trace("GetProcAddress(%s) = %08x", full.c_str(), addr);
    if (!addr) set_last_error(127);  // ERROR_PROC_NOT_FOUND
    ret(c, addr, 2);
}

W32(kernel32, GetModuleFileNameA) {
    const uint32_t module = arg(c, 0), buf = arg(c, 1), size = arg(c, 2);
    std::string path = process().exe_path;
    if (module && module != process().image->image_base())
        path = "C:\\WINDOWS\\system32\\" + module_name(module) + ".dll";
    const uint32_t n = put_str(buf, size, path);
    ret(c, n, 3);
}

// ---- the process ----

W32(kernel32, GetCommandLineA) {
    static uint32_t line = guest_strdup(process().command_line);
    ret(c, line, 0);
}

W32(kernel32, GetStartupInfoA) {
    const uint32_t si = arg(c, 0);
    std::memset(gptr(si), 0, 68);
    wr(si, 68);
    ret(c, 0, 1);
}

W32(kernel32, GetCurrentProcess) { ret(c, kCurrentProcess, 0); }
W32(kernel32, GetCurrentProcessId) { ret(c, 0x1000, 0); }

W32(kernel32, ExitProcess) {
    trace("ExitProcess(%u)", arg(c, 0));
    std::fflush(nullptr);
    std::fprintf(stderr, "%s: ExitProcess(%u)\n", process().game_id.c_str(), arg(c, 0));
    _exit((int)arg(c, 0));
}

W32(kernel32, TerminateProcess) {
    std::fflush(nullptr);
    std::fprintf(stderr, "%s: TerminateProcess(%u)\n", process().game_id.c_str(), arg(c, 1));
    _exit((int)arg(c, 1));
}

W32(kernel32, FatalAppExitA) { fatal("FatalAppExitA: %s", gstr(arg(c, 1)).c_str()); }

// ---- environment ----

namespace {
std::string environment_block() {
    // A plain Windows environment (the host's is irrelevant to the game).
    static const char* kEnv[] = {"ALLUSERSPROFILE=C:\\Documents and Settings\\All Users",
                                 "APPDATA=C:\\Documents and Settings\\Player\\Application Data",
                                 "COMPUTERNAME=SCIMITAR", "HOMEDRIVE=C:", "NUMBER_OF_PROCESSORS=4",
                                 "OS=Windows_NT", "PATH=C:\\WINDOWS\\system32;C:\\WINDOWS",
                                 "PROCESSOR_ARCHITECTURE=x86", "SystemRoot=C:\\WINDOWS",
                                 "TEMP=C:\\TEMP", "TMP=C:\\TEMP", "USERNAME=Player",
                                 "USERPROFILE=C:\\Documents and Settings\\Player", "windir=C:\\WINDOWS"};
    std::string s;
    for (const char* e : kEnv) s += std::string(e) + '\0';
    s += '\0';
    return s;
}
}  // namespace

W32(kernel32, GetEnvironmentStrings) {
    const std::string b = environment_block();
    const uint32_t a = galloc((uint32_t)b.size());
    std::memcpy(gptr(a), b.data(), b.size());
    ret(c, a, 0);
}

W32(kernel32, GetEnvironmentStringsW) {
    const std::string b = environment_block();
    const uint32_t a = galloc(2 * (uint32_t)b.size());
    for (size_t k = 0; k < b.size(); k++) wr16(a + 2 * (uint32_t)k, (uint8_t)b[k]);
    ret(c, a, 0);
}

W32(kernel32, FreeEnvironmentStringsA) { gfree(arg(c, 0)); ret(c, 1, 1); }
W32(kernel32, FreeEnvironmentStringsW) { gfree(arg(c, 0)); ret(c, 1, 1); }
W32(kernel32, SetEnvironmentVariableA) { ret(c, 1, 2); }

// ---- errors and debugging ----

W32(kernel32, GetLastError) { ret(c, last_error(), 0); }
W32(kernel32, SetLastError) { set_last_error(arg(c, 0)); ret(c, 0, 1); }
W32(kernel32, SetErrorMode) { ret(c, 0, 1); }
W32(kernel32, IsDebuggerPresent) { ret(c, 0, 0); }
W32(kernel32, DebugBreak) { fatal("DebugBreak"); }
W32(kernel32, SetUnhandledExceptionFilter) { ret(c, 0, 1); }
W32(kernel32, UnhandledExceptionFilter) { ret(c, 1, 1); }  // EXCEPTION_EXECUTE_HANDLER

W32(kernel32, OutputDebugStringA) {
    if (std::getenv("SCIMITAR_LOG")) std::fprintf(stderr, "debug: %s", gstr(arg(c, 0)).c_str());
    ret(c, 0, 1);
}

W32(kernel32, RaiseException) {
    const uint32_t code = arg(c, 0), nargs = arg(c, 2), args = arg(c, 3);
    if (code == 0x406d1388) return ret(c, 0, 4);  // MSVC's "set thread name" for debuggers
    // 0xe06d7363: a C++ throw (args: magic, object, ThrowInfo).
    if (code == 0xe06d7363 && nargs >= 3) {
        const uint32_t info = rd(args + 8);
        const uint32_t types = info ? rd(info + 12) : 0;
        const uint32_t type = types && rd(types) ? rd(types + 4) : 0;
        const uint32_t td = type ? rd(type + 4) : 0;
        fatal("C++ exception thrown (%s)", td ? gstr(td + 8).c_str() : "?");
    }
    fatal("RaiseException(%08x)", code);
}

W32(kernel32, FormatMessageA) {
    // Used for error text only.
    const uint32_t flags = arg(c, 0), id = arg(c, 2), buf = arg(c, 4), size = arg(c, 5);
    const std::string text = "Error " + std::to_string(id) + "\r\n";
    if (flags & 0x100) {  // FORMAT_MESSAGE_ALLOCATE_BUFFER
        wr(buf, guest_strdup(text));
        return ret(c, (uint32_t)text.size(), 7);
    }
    ret(c, put_str(buf, size, text), 7);
}

W32(kernel32, LocalFree) { gfree(arg(c, 0)); ret(c, 0, 1); }

// ---- system ----

W32(kernel32, GetVersion) { ret(c, 0x0a280105u, 0); }  // Windows XP (5.1.2600)

W32(kernel32, GetVersionExA) {
    const uint32_t v = arg(c, 0);
    const uint32_t size = rd(v);
    std::memset(gptr(v + 4), 0, size - 4);
    wr(v + 4, 5);
    wr(v + 8, 1);
    wr(v + 12, 2600);
    wr(v + 16, 2);  // VER_PLATFORM_WIN32_NT
    put_str(v + 20, 128, "Service Pack 3");
    if (size >= 156) wr16(v + 148, 3);  // OSVERSIONINFOEX: service pack 3.0
    ret(c, 1, 1);
}

W32(kernel32, GetSystemInfo) {
    const uint32_t si = arg(c, 0);
    std::memset(gptr(si), 0, 36);
    wr(si + 4, 4096);
    wr(si + 8, 0x10000);
    wr(si + 12, 0x7ffeffff);
    wr(si + 16, 0xf);
    wr(si + 20, std::getenv("SCIMITAR_WIN32_CPUS") ? (uint32_t)std::atoi(std::getenv("SCIMITAR_WIN32_CPUS")) : 4);  // processors
    wr(si + 24, 586);
    wr(si + 28, 0x10000);
    wr16(si + 32, 6);
    wr16(si + 34, 0x0f0b);
    ret(c, 0, 1);
}

W32(kernel32, GetComputerNameExA) {
    const uint32_t buf = arg(c, 1), size_ptr = arg(c, 2);
    const std::string name = "SCIMITAR";
    if (rd(size_ptr) <= name.size()) {
        wr(size_ptr, (uint32_t)name.size() + 1);
        set_last_error(234);  // ERROR_MORE_DATA
        return ret(c, 0, 3);
    }
    wr(size_ptr, put_str(buf, rd(size_ptr), name));
    ret(c, 1, 3);
}

W32(kernel32, GetLogicalDriveStringsA) {
    const uint32_t size = arg(c, 0), buf = arg(c, 1);
    const char drives[] = "C:\\\0";
    if (size >= sizeof drives) std::memcpy(gptr(buf), drives, sizeof drives);
    ret(c, sizeof drives - 1, 2);
}

W32(kernel32, GetDriveTypeA) { ret(c, 3, 1); }  // DRIVE_FIXED

// ---- code pages ----

W32(kernel32, GetACP) { ret(c, 1252, 0); }
W32(kernel32, GetOEMCP) { ret(c, 437, 0); }
W32(kernel32, IsValidCodePage) { ret(c, 1, 1); }

W32(kernel32, GetCPInfo) {
    const uint32_t info = arg(c, 1);
    std::memset(gptr(info), 0, 20);
    wr(info, 1);              // single-byte
    gptr(info)[4] = '?';      // default char
    ret(c, 1, 2);
}

// Code page 1252 is Latin-1 apart from 0x80-0x9f; the game's text is ASCII.
W32(kernel32, MultiByteToWideChar) {
    const uint32_t src = arg(c, 2), dst = arg(c, 4), cap = arg(c, 5);
    int32_t n = (int32_t)arg(c, 3);
    std::string s;
    if (n < 0) s = gstr(src), s.push_back('\0');
    else s.assign(reinterpret_cast<const char*>(gptr(src)), (size_t)n);
    const uint32_t cp = arg(c, 0);
    std::u16string w = cp == 65001 ? utf16(s) : std::u16string(s.begin(), s.end());
    if (cp != 65001)
        for (size_t k = 0; k < s.size(); k++) w[k] = (char16_t)(uint8_t)s[k];
    if (!cap) return ret(c, (uint32_t)w.size(), 6);
    if (w.size() > cap) {
        set_last_error(122);  // ERROR_INSUFFICIENT_BUFFER
        return ret(c, 0, 6);
    }
    for (size_t k = 0; k < w.size(); k++) wr16(dst + 2 * (uint32_t)k, w[k]);
    ret(c, (uint32_t)w.size(), 6);
}

W32(kernel32, WideCharToMultiByte) {
    const uint32_t cp = arg(c, 0), src = arg(c, 2), dst = arg(c, 4), cap = arg(c, 5);
    int32_t n = (int32_t)arg(c, 3);
    std::u16string w;
    if (n < 0) w = gwstr(src), w.push_back(0);
    else
        for (int32_t k = 0; k < n; k++) w.push_back((char16_t)rd16(src + 2 * k));
    std::string s;
    if (cp == 65001) s = utf8(w);
    else
        for (char16_t ch : w) s += ch < 256 ? (char)ch : '?';
    if (!cap) return ret(c, (uint32_t)s.size(), 8);
    if (s.size() > cap) {
        set_last_error(122);
        return ret(c, 0, 8);
    }
    std::memcpy(gptr(dst), s.data(), s.size());
    ret(c, (uint32_t)s.size(), 8);
}

// ---- locale (the C runtime's "C" locale needs little) ----

W32(kernel32, GetUserDefaultLCID) { ret(c, 0x409, 0); }
W32(kernel32, IsValidLocale) { ret(c, 1, 2); }
W32(kernel32, EnumSystemLocalesA) { ret(c, 1, 2); }

namespace {
std::string locale_info(uint32_t type) {
    switch (type & 0xffff) {
    case 0x1004: return "1252";      // LOCALE_IDEFAULTANSICODEPAGE
    case 0xb: return "437";          // LOCALE_IDEFAULTCODEPAGE
    case 0x1001: return "English";   // LOCALE_SENGLANGUAGE
    case 0x1002: return "United States";
    case 0x3: return "ENU";          // LOCALE_SABBREVLANGNAME
    case 0x7: return "USA";          // LOCALE_SABBREVCTRYNAME
    case 0x59: return "en";          // LOCALE_SISO639LANGNAME
    case 0x5a: return "US";          // LOCALE_SISO3166CTRYNAME
    case 0xe: return ".";            // LOCALE_SDECIMAL
    case 0xf: return ",";            // LOCALE_STHOUSAND
    case 0x10: return "3;0";         // LOCALE_SGROUPING
    case 0x14: return "$";
    case 0x1e: return ":";           // LOCALE_STIME
    case 0x1d: return "/";           // LOCALE_SDATE
    default: return "";
    }
}
}  // namespace

W32(kernel32, GetLocaleInfoA) {
    const std::string v = locale_info(arg(c, 1));
    const uint32_t buf = arg(c, 2), cap = arg(c, 3);
    if (!cap) return ret(c, (uint32_t)v.size() + 1, 4);
    ret(c, put_str(buf, cap, v) + 1, 4);
}

W32(kernel32, GetLocaleInfoW) {
    const std::string v = locale_info(arg(c, 1));
    const uint32_t buf = arg(c, 2), cap = arg(c, 3);
    if (!cap) return ret(c, (uint32_t)v.size() + 1, 4);
    ret(c, put_wstr(buf, cap, utf16(v)) + 1, 4);
}

W32(kernel32, CompareStringA) {
    std::string a(reinterpret_cast<const char*>(gptr(arg(c, 2))),
                  (int32_t)arg(c, 3) < 0 ? std::strlen((const char*)gptr(arg(c, 2))) : arg(c, 3));
    std::string b(reinterpret_cast<const char*>(gptr(arg(c, 4))),
                  (int32_t)arg(c, 5) < 0 ? std::strlen((const char*)gptr(arg(c, 4))) : arg(c, 5));
    if (arg(c, 1) & 1)  // NORM_IGNORECASE
        for (auto* s : {&a, &b})
            for (char& ch : *s) ch = (char)std::tolower((unsigned char)ch);
    const int r = a.compare(b);
    ret(c, r < 0 ? 1 : r == 0 ? 2 : 3, 6);
}

W32(kernel32, CompareStringW) {
    auto read = [&](uint32_t p, int32_t n) {
        std::u16string s;
        if (n < 0) return gwstr(p);
        for (int32_t k = 0; k < n; k++) s.push_back((char16_t)rd16(p + 2 * k));
        return s;
    };
    std::u16string a = read(arg(c, 2), (int32_t)arg(c, 3)), b = read(arg(c, 4), (int32_t)arg(c, 5));
    if (arg(c, 1) & 1)
        for (auto* s : {&a, &b})
            for (char16_t& ch : *s)
                if (ch < 128) ch = (char16_t)std::tolower(ch);
    const int r = a.compare(b);
    ret(c, r < 0 ? 1 : r == 0 ? 2 : 3, 6);
}

// LCMapStringA/W: LCMAP_LOWERCASE (0x100) and LCMAP_UPPERCASE (0x200) of ASCII.
W32(kernel32, LCMapStringA) {
    const uint32_t flags = arg(c, 1), src = arg(c, 2), dst = arg(c, 4), cap = arg(c, 5);
    int32_t n = (int32_t)arg(c, 3);
    std::string s = n < 0 ? gstr(src) + '\0' : std::string((const char*)gptr(src), (size_t)n);
    for (char& ch : s)
        ch = flags & 0x100 ? (char)std::tolower((unsigned char)ch)
                           : flags & 0x200 ? (char)std::toupper((unsigned char)ch) : ch;
    if (!cap) return ret(c, (uint32_t)s.size(), 6);
    if (s.size() > cap) return ret(c, 0, 6);
    std::memcpy(gptr(dst), s.data(), s.size());
    ret(c, (uint32_t)s.size(), 6);
}

W32(kernel32, LCMapStringW) {
    const uint32_t flags = arg(c, 1), src = arg(c, 2), dst = arg(c, 4), cap = arg(c, 5);
    int32_t n = (int32_t)arg(c, 3);
    std::u16string s;
    if (n < 0) s = gwstr(src), s.push_back(0);
    else
        for (int32_t k = 0; k < n; k++) s.push_back((char16_t)rd16(src + 2 * k));
    for (char16_t& ch : s)
        if (ch < 128)
            ch = flags & 0x100 ? (char16_t)std::tolower(ch) : flags & 0x200 ? (char16_t)std::toupper(ch) : ch;
    if (!cap) return ret(c, (uint32_t)s.size(), 6);
    if (s.size() > cap) return ret(c, 0, 6);
    for (size_t k = 0; k < s.size(); k++) wr16(dst + 2 * (uint32_t)k, s[k]);
    ret(c, (uint32_t)s.size(), 6);
}

namespace {
uint16_t ctype_of(uint32_t ch) {
    // C1_UPPER 1, LOWER 2, DIGIT 4, SPACE 8, PUNCT 0x10, CNTRL 0x20, BLANK 0x40, XDIGIT 0x80, ALPHA 0x100
    uint16_t t = 0;
    if (ch < 128) {
        if (std::isupper((int)ch)) t |= 1;
        if (std::islower((int)ch)) t |= 2;
        if (std::isdigit((int)ch)) t |= 4;
        if (std::isspace((int)ch)) t |= 8;
        if (std::ispunct((int)ch)) t |= 0x10;
        if (std::iscntrl((int)ch)) t |= 0x20;
        if (ch == ' ' || ch == '\t') t |= 0x40;
        if (std::isxdigit((int)ch)) t |= 0x80;
        if (std::isalpha((int)ch)) t |= 0x100;
    } else if (ch >= 0xc0 && ch < 0x100 && ch != 0xd7 && ch != 0xf7) {
        t = (ch < 0xdf ? 1 : 2) | 0x100;
    } else if (ch >= 0xa0 && ch < 0xc0) {
        t = 0x10;
    }
    return t;
}
}  // namespace

W32(kernel32, GetStringTypeA) {
    // (LCID, type, src, n, out)
    const uint32_t src = arg(c, 2), out = arg(c, 4);
    int32_t n = (int32_t)arg(c, 3);
    if (n < 0) n = (int32_t)std::strlen((const char*)gptr(src)) + 1;
    for (int32_t k = 0; k < n; k++) wr16(out + 2 * k, ctype_of(gptr(src)[k]));
    ret(c, 1, 5);
}

W32(kernel32, GetStringTypeW) {
    // (type, src, n, out)
    const uint32_t src = arg(c, 1), out = arg(c, 3);
    int32_t n = (int32_t)arg(c, 2);
    if (n < 0) n = (int32_t)gwstr(src).size() + 1;
    for (int32_t k = 0; k < n; k++) wr16(out + 2 * k, ctype_of(rd16(src + 2 * k)));
    ret(c, 1, 4);
}

W32(kernel32, GetDateFormatA) {
    const uint32_t buf = arg(c, 4), cap = arg(c, 5);
    const std::string s = "01/01/2008";
    if (!cap) return ret(c, (uint32_t)s.size() + 1, 6);
    ret(c, put_str(buf, cap, s) + 1, 6);
}

W32(kernel32, GetTimeFormatA) {
    const uint32_t buf = arg(c, 4), cap = arg(c, 5);
    const std::string s = "12:00:00";
    if (!cap) return ret(c, (uint32_t)s.size() + 1, 6);
    ret(c, put_str(buf, cap, s) + 1, 6);
}

// ---- console (no console: the game is a GUI program) ----

W32(kernel32, GetConsoleCP) { ret(c, 437, 0); }
W32(kernel32, GetConsoleOutputCP) { ret(c, 437, 0); }
W32(kernel32, GetConsoleMode) { ret(c, 0, 2); }
W32(kernel32, SetConsoleCtrlHandler) { ret(c, 1, 2); }

W32(kernel32, WriteConsoleA) {
    std::fwrite(gptr(arg(c, 1)), 1, arg(c, 2), stderr);
    if (arg(c, 3)) wr(arg(c, 3), arg(c, 2));
    ret(c, 1, 5);
}

W32(kernel32, WriteConsoleW) {
    std::u16string s;
    for (uint32_t k = 0; k < arg(c, 2); k++) s.push_back((char16_t)rd16(arg(c, 1) + 2 * k));
    std::fputs(utf8(s).c_str(), stderr);
    if (arg(c, 3)) wr(arg(c, 3), arg(c, 2));
    ret(c, 1, 5);
}

}  // namespace scimitar::imports

// Functions the C runtime looks up with GetProcAddress.
namespace {
using namespace scimitar;
using namespace scimitar::win32;
void is_processor_feature_present(Context& c) {
    // PF_XMMI_INSTRUCTIONS_AVAILABLE 6, PF_XMMI64 10, PF_SSE3 13, PF_COMPARE_EXCHANGE_DOUBLE 2
    const uint32_t f = arg(c, 0);
    ret(c, f == 2 || f == 3 || f == 6 || f == 10 || f == 13, 1);
}
const DynamicShim kDynamic[] = {
    {"kernel32!IsProcessorFeaturePresent", is_processor_feature_present},
};
const DynamicRegistrar kDynamicRegistrar(kDynamic, sizeof kDynamic / sizeof *kDynamic);
}  // namespace
