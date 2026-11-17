// The smaller imports Revelations has and AC1 does not, mostly over AC1's
// shims: USER32 (display devices, key state, the foreground window, icons,
// wide message boxes and wsprintfW), SHELL32 (SHGetFolderPathA and the wide
// folder functions), ole32 (COM in multithreaded mode), the registry's wide
// query, dbghelp's stack walk (no symbols: crash reports stay empty), Winsock's
// WSAIoctl, D3DX's texture requirements and constant tables (over d3d9.mm's),
// and DirectSound 8 as a COM class.
#include <cstdio>

#include "win32.h"

namespace scimitar::imports {
#define SHIM(dll, fn) void imp_##dll##_##fn(Context& c);
SHIM(user32, GetKeyboardState) SHIM(user32, IsWindow) SHIM(user32, MessageBoxA)
SHIM(shell32, SHGetSpecialFolderPathA) SHIM(shell32, SHCreateDirectoryExA) SHIM(ole32, CoInitialize)
SHIM(d3dx9_36, D3DXGetShaderConstantTable) SHIM(dsound, DirectSoundCreate8)
#undef SHIM
}  // namespace scimitar::imports

namespace scimitar::win32 {
namespace {

uint32_t call_shim(Context& c, void (*fn)(Context&), std::initializer_list<uint32_t> args) {
    const uint32_t esp = c.esp;
    c.esp = (c.esp - 4 * (uint32_t)args.size() - 4) & ~15u;
    uint32_t p = c.esp + 4;
    for (uint32_t a : args) wr(p, a), p += 4;
    fn(c);
    c.esp = esp;
    return c.eax;
}

// Scratch bytes below the stack pointer while a shim runs.
uint32_t scratch(Context& c, uint32_t n) {
    c.esp = (c.esp - n - 16) & ~15u;
    return c.esp;
}

constexpr uint32_t kFirstWindow = 0x20000;  // user32.mm's first window handle

}  // namespace
}  // namespace scimitar::win32

namespace scimitar::imports {
using namespace scimitar::win32;

// ---- USER32 ----

// EnumDisplayDevices{A,W}(device, index, DISPLAY_DEVICE*, flags): one display.
static void enum_display_devices(Context& c, bool wide) {
    const uint32_t device = arg(c, 0), index = arg(c, 1), dd = arg(c, 2);
    if (index > 0 || !dd) return ret(c, 0, 4);
    // DISPLAY_DEVICE: cb, DeviceName[32], DeviceString[128], StateFlags,
    // DeviceID[128], DeviceKey[128] (chars of the A or W flavour).
    const uint32_t ch = wide ? 2 : 1;
    std::memset(gptr(dd + 4), 0, (32 + 128) * ch + 4 + 256 * ch);
    auto put = [&](uint32_t at, uint32_t cap, const std::string& s) {
        if (wide) put_wstr(at, cap, utf16(s));
        else put_str(at, cap, s);
    };
    const bool monitor = device != 0;  // the display's monitor when a device is named
    put(dd + 4, 32, monitor ? "\\\\.\\DISPLAY1\\Monitor0" : "\\\\.\\DISPLAY1");
    put(dd + 4 + 32 * ch, 128, monitor ? "Generic PnP Monitor" : "Apple Metal");
    wr(dd + 4 + 160 * ch, monitor ? 3 : 5);  // ATTACHED_TO_DESKTOP | (PRIMARY_DEVICE or MULTI_DRIVER)
    put(dd + 8 + 160 * ch, 128, monitor ? "MONITOR\\Default_Monitor" : "PCI\\VEN_106B&DEV_0000");
    ret(c, 1, 4);
}
W32(user32, EnumDisplayDevicesA) { enum_display_devices(c, false); }
W32(user32, EnumDisplayDevicesW) { enum_display_devices(c, true); }

// The key's state from GetKeyboardState's table (bit 15: down now).
W32(user32, GetAsyncKeyState) {
    const uint32_t vk = arg(c, 0) & 0xff, esp = c.esp;
    const uint32_t keys = scratch(c, 256);
    call_shim(c, imp_user32_GetKeyboardState, {keys});
    const uint8_t state = *gptr(keys + vk);
    c.esp = esp;
    ret(c, state & 0x80 ? 0x8000u : 0, 1);
}
// The game's window is the foreground one (the port has no other).
W32(user32, GetForegroundWindow) {
    ret(c, call_shim(c, imp_user32_IsWindow, {kFirstWindow}) ? kFirstWindow : 0, 0);
}
W32(user32, LoadIconA) { ret(c, 0x10030, 2); }
W32(user32, LoadImageA) { ret(c, 0x10040, 6); }
W32(user32, MessageBoxW) {
    const std::string text = utf8(gwstr(arg(c, 1))), title = utf8(gwstr(arg(c, 2)));
    std::fprintf(stderr, "acr: message box \"%s\": %s\n", title.c_str(), text.c_str());
    const uint32_t type = arg(c, 3) & 0xf;
    ret(c, type == 4 || type == 3 ? 6 : 1, 4);  // IDYES for yes/no boxes, else IDOK
}

// wsprintfW(buffer, format, ...): cdecl. %s, %S, %d, %i, %u, %x, %X, %c,
// %%, with flags, width and precision; at most 1024 characters, as Windows.
W32(user32, wsprintfW) {
    const uint32_t out = arg(c, 0);
    const std::u16string fmt = gwstr(arg(c, 1));
    std::u16string r;
    int next = 2;
    for (size_t k = 0; k < fmt.size(); k++) {
        if (fmt[k] != u'%' || k + 1 == fmt.size()) {
            r += fmt[k];
            continue;
        }
        std::string spec = "%";
        size_t j = k + 1;
        while (j < fmt.size() && std::strchr("-+ #0", (char)fmt[j])) spec += (char)fmt[j++];
        while (j < fmt.size() && fmt[j] >= u'0' && fmt[j] <= u'9') spec += (char)fmt[j++];
        if (j < fmt.size() && fmt[j] == u'.') {
            spec += '.', j++;
            while (j < fmt.size() && fmt[j] >= u'0' && fmt[j] <= u'9') spec += (char)fmt[j++];
        }
        bool wide_arg = true;  // %s is wide in wsprintfW; %hs and %S narrow
        if (j < fmt.size() && (fmt[j] == u'h' || fmt[j] == u'l')) wide_arg = fmt[j++] == u'l';
        if (j >= fmt.size()) break;
        const char16_t conv = fmt[j];
        k = j;
        char buf[256];
        switch (conv) {
        case u'%': r += u'%'; break;
        case u'c': r += (char16_t)arg(c, next++); break;
        case u's':
        case u'S': {
            const uint32_t s = arg(c, next++);
            const bool wide = conv == u's' ? wide_arg : !wide_arg;
            std::u16string v = !s ? u"(null)" : wide ? gwstr(s) : utf16(gstr(s));
            spec += 's';
            std::snprintf(buf, sizeof buf, spec.c_str(), utf8(v).c_str());
            r += utf16(buf);
            break;
        }
        case u'd': case u'i': case u'u': case u'x': case u'X':
            spec += (char)conv;
            std::snprintf(buf, sizeof buf, spec.c_str(), arg(c, next++));
            r += utf16(buf);
            break;
        default: r += u'%', r += conv; break;
        }
    }
    if (r.size() > 1023) r.resize(1023);
    put_wstr(out, 1024, r);
    shim_return(c, (uint32_t)r.size());
}

// ---- SHELL32 ----

// SHGetFolderPathA(hwnd, csidl, token, flags, path): the same folders as
// SHGetSpecialFolderPathA.
W32(shell32, SHGetFolderPathA) {
    const uint32_t r = call_shim(c, imp_shell32_SHGetSpecialFolderPathA, {arg(c, 0), arg(c, 4), arg(c, 1), 1});
    ret(c, r ? 0 : 0x80004005u, 5);  // S_OK / E_FAIL
}
// (scratch() moves esp: the arguments are read first, esp put back before ret.)
W32(shell32, SHGetSpecialFolderPathW) {
    const uint32_t hwnd = arg(c, 0), out = arg(c, 1), csidl = arg(c, 2), create = arg(c, 3);
    const uint32_t esp = c.esp, path = scratch(c, 520);
    const uint32_t r = call_shim(c, imp_shell32_SHGetSpecialFolderPathA, {hwnd, path, csidl, create});
    if (r) put_wstr(out, 260, utf16(gstr(path)));
    c.esp = esp;
    ret(c, r, 4);
}
W32(shell32, SHCreateDirectoryExW) {
    const uint32_t hwnd = arg(c, 0), sa = arg(c, 2), esp = c.esp;
    const std::string p = utf8(gwstr(arg(c, 1)));
    const uint32_t a = scratch(c, (uint32_t)p.size() + 1);
    put_str(a, (uint32_t)p.size() + 1, p);
    const uint32_t r = call_shim(c, imp_shell32_SHCreateDirectoryExA, {hwnd, a, sa});
    c.esp = esp;
    ret(c, r, 3);
}
// Opening a web page or the manual: nothing is started.
W32(shell32, ShellExecuteA) {
    std::fprintf(stderr, "acr: ShellExecuteA(%s %s) not started\n", gstr(arg(c, 1)).c_str(), gstr(arg(c, 2)).c_str());
    ret(c, 2, 6);  // ERROR_FILE_NOT_FOUND (a value <= 32 means failure)
}

// ---- ole32 ----

W32(ole32, CoInitializeEx) { ret(c, call_shim(c, imp_ole32_CoInitialize, {arg(c, 0)}), 2); }
W32(ole32, CoSetProxyBlanket) { ret(c, 0, 8); }

// ---- ADVAPI32: the registry is empty ----

W32(advapi32, RegQueryValueExW) { ret(c, 2, 6); }  // ERROR_FILE_NOT_FOUND

// ---- dbghelp: no symbols, no stack walk ----

W32(dbghelp, StackWalk64) { ret(c, 0, 9); }
W32(dbghelp, SymFunctionTableAccess64) { ret(c, 0, 3); }  // (process, 64-bit address)
W32(dbghelp, SymGetModuleInfo64) { ret(c, 0, 4); }       // (process, 64-bit address, info)

// ---- Winsock ----

W32(ws2_32, WSAIoctl) { ret(c, 0xffffffffu, 9); }  // SOCKET_ERROR: no network

// ---- D3DX ----

// D3DXCheckTextureRequirements(device, &w, &h, &levels, usage, &format, pool):
// every size and format the game asks for is fine as it is.
W32(d3dx9_43, D3DXCheckTextureRequirements) { ret(c, 0, 7); }
// D3DXGetShaderConstantTableEx(code, flags, ID3DXConstantTable**): the flags
// (D3DXCONSTTABLE_LARGEADDRESSAWARE) change nothing here.
W32(d3dx9_43, D3DXGetShaderConstantTableEx) {
    const uint32_t code = arg(c, 0), flags = arg(c, 1), out = arg(c, 2);
    const uint32_t hr = call_shim(c, imp_d3dx9_36_D3DXGetShaderConstantTable, {code, out});
    if (hr) trace("D3DXGetShaderConstantTableEx(%08x, version %08x, flags %x) failed: %08x", code, rd(code), flags, hr);
    ret(c, hr, 3);
}

}  // namespace scimitar::imports

namespace scimitar::win32 {

// COM classes (CoCreateInstance, system.cpp): DirectSound 8, which
// Revelations' Wwise creates this way (CLSID_DirectSound8
// 3901cc3f-84b5-4fa4-ba35-aa8172b8a09b) and then initializes, over
// dsound.cpp's DirectSoundCreate8.
bool create_com_instance(uint32_t clsid, uint32_t iid, uint32_t out, uint32_t* hr) {
    (void)iid;
    if (!(rd(clsid) == 0x3901cc3fu && rd(clsid + 4) == 0x4fa484b5u && rd(clsid + 8) == 0x81aa35bau &&
          rd(clsid + 12) == 0x9ba0b872u))
        return false;
    Context& c = *thread_context();
    *hr = call_shim(c, imports::imp_dsound_DirectSoundCreate8, {0, out, 0});
    trace("CoCreateInstance: DirectSound8 = %08x", *hr);
    return true;
}

}  // namespace scimitar::win32
