// The smaller system DLLs: ADVAPI32 (crypto random, user name),
// SHELL32 (special folders), ole32/OLEAUT32, WININET, iphlpapi, WINMM,
// Winsock (unavailable: no online play) and uplay32. XInput is xinput.mm.
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <random>

#include "win32.h"

namespace scimitar::win32 {
// Without acr_misc.cpp there are no COM classes to create.
[[gnu::weak]] bool create_com_instance(uint32_t, uint32_t, uint32_t, uint32_t*) { return false; }
}  // namespace scimitar::win32

namespace scimitar::imports {
using namespace scimitar::win32;

// ---- ADVAPI32 ----
// (The registry is registry.cpp.)

W32(advapi32, GetUserNameA) {
    const uint32_t buf = arg(c, 0), size_ptr = arg(c, 1);
    const std::string name = "Player";
    if (rd(size_ptr) <= name.size()) {
        wr(size_ptr, (uint32_t)name.size() + 1);
        set_last_error(122);
        return ret(c, 0, 2);
    }
    put_str(buf, rd(size_ptr), name);
    wr(size_ptr, (uint32_t)name.size() + 1);
    ret(c, 1, 2);
}

W32(advapi32, CryptAcquireContextA) {
    wr(arg(c, 0), 0x10050);
    ret(c, 1, 5);
}
W32(advapi32, CryptReleaseContext) { ret(c, 1, 2); }
W32(advapi32, CryptGenRandom) {
    static std::random_device rd_dev;
    for (uint32_t k = 0; k < arg(c, 1); k++) gptr(arg(c, 2))[k] = (uint8_t)rd_dev();
    ret(c, 1, 3);
}

// ---- SHELL32 ----
// Special folders: the save and settings folders the game uses map to
// ~/Library/Application Support/scimitar-recomp/ac1/<folder>, created as asked.


W32(shell32, SHGetSpecialFolderPathA) {
    // (hwnd, path, csidl, create): every folder lives under one host folder,
    // seen by the game as drive D:.
    const uint32_t csidl = arg(c, 2) & 0xff;
    const char* name = csidl == 0x05 ? "My Documents" : csidl == 0x1a ? "Application Data"
                     : csidl == 0x1c ? "Local Settings" : csidl == 0x23 ? "Common Application Data"
                     : "Folder";
    const std::string host = host_path(std::string("D:\\") + name);
    for (size_t k = 1; k <= host.size(); k++)  // with its parents (the D: drive's folder)
        if (k == host.size() || host[k] == '/') ::mkdir(host.substr(0, k).c_str(), 0755);
    trace("SHGetSpecialFolderPathA(%#x) = %s", csidl, host.c_str());
    put_str(arg(c, 1), 260, std::string("D:\\") + name);
    ret(c, 1, 4);
}

W32(shell32, SHCreateDirectoryExA) {
    std::string p = host_path(gstr(arg(c, 1)));
    for (size_t k = 1; k <= p.size(); k++)
        if (k == p.size() || p[k] == '/') ::mkdir(p.substr(0, k).c_str(), 0755);
    ret(c, 0, 3);
}

// ---- ole32 / OLEAUT32 ----

W32(ole32, CoInitialize) { ret(c, 0, 1); }
W32(ole32, CoUninitialize) { ret(c, 0, 0); }
W32(ole32, CoCreateInstance) {
    // (clsid, outer, context, iid, out)
    if (uint32_t hr; create_com_instance(arg(c, 0), arg(c, 3), arg(c, 4), &hr)) return ret(c, hr, 5);
    trace("CoCreateInstance(%08x-%04x...): no such COM class", rd(arg(c, 0)), rd(arg(c, 0) + 4) & 0xffff);
    if (arg(c, 4)) wr(arg(c, 4), 0);
    ret(c, 0x80040154u, 5);  // REGDB_E_CLASSNOTREG
}

W32(oleaut32, SysAllocString) {
    const std::u16string s = gwstr(arg(c, 0));
    const uint32_t b = galloc(4 + 2 * (uint32_t)s.size() + 2);
    wr(b, 2 * (uint32_t)s.size());
    put_wstr(b + 4, (uint32_t)s.size() + 1, s);
    ret(c, b + 4, 1);
}

// ---- WININET, iphlpapi, WINMM ----

W32(wininet, InternetGetCookieW) { ret(c, 0, 4); }
W32(wininet, InternetSetCookieW) { ret(c, 0, 3); }
W32(iphlpapi, GetAdaptersInfo) { ret(c, 232, 2); }  // ERROR_NO_DATA

W32(winmm, waveOutGetDevCapsA) {
    // (device, caps, size): one stereo device.
    const uint32_t caps = arg(c, 1), size = arg(c, 2);
    std::memset(gptr(caps), 0, size);
    put_str(caps + 8, 32, "Speakers");
    if (size >= 52) wr16(caps + 48, 2);
    ret(c, 0, 3);
}

// ---- Winsock: no network ----

W32(ws2_32, WSAStartup) {
    const uint32_t data = arg(c, 1);
    std::memset(gptr(data), 0, 400);
    wr16(data, (uint16_t)arg(c, 0));
    wr16(data + 2, 0x0202);
    ret(c, 0, 2);
}
W32(ws2_32, WSACleanup) { ret(c, 0, 0); }
W32(ws2_32, WSAGetLastError) { ret(c, 10050, 0); }  // WSAENETDOWN
W32(ws2_32, WSASetLastError) { ret(c, 0, 1); }
W32(ws2_32, socket) { ret(c, 0xffffffffu, 3); }
W32(ws2_32, closesocket) { ret(c, 0, 1); }
W32(ws2_32, gethostname) { put_str(arg(c, 0), arg(c, 1), "scimitar"); ret(c, 0, 2); }
W32(ws2_32, gethostbyname) { ret(c, 0, 1); }
W32(ws2_32, gethostbyaddr) { ret(c, 0, 3); }
W32(ws2_32, inet_addr) { ret(c, 0xffffffffu, 1); }
W32(ws2_32, inet_ntoa) { static uint32_t s = guest_strdup("0.0.0.0"); ret(c, s, 1); }
W32(ws2_32, htons) { ret(c, __builtin_bswap16((uint16_t)arg(c, 0)), 1); }
W32(ws2_32, ntohs) { ret(c, __builtin_bswap16((uint16_t)arg(c, 0)), 1); }
W32(ws2_32, htonl) { ret(c, __builtin_bswap32(arg(c, 0)), 1); }
W32(ws2_32, ntohl) { ret(c, __builtin_bswap32(arg(c, 0)), 1); }
W32(ws2_32, bind) { ret(c, 0xffffffffu, 3); }
W32(ws2_32, connect) { ret(c, 0xffffffffu, 3); }
W32(ws2_32, listen) { ret(c, 0xffffffffu, 2); }
W32(ws2_32, accept) { ret(c, 0xffffffffu, 3); }
W32(ws2_32, send) { ret(c, 0xffffffffu, 4); }
W32(ws2_32, sendto) { ret(c, 0xffffffffu, 6); }
W32(ws2_32, recv) { ret(c, 0xffffffffu, 4); }
W32(ws2_32, recvfrom) { ret(c, 0xffffffffu, 6); }
W32(ws2_32, select) { ret(c, 0xffffffffu, 5); }
W32(ws2_32, setsockopt) { ret(c, 0xffffffffu, 5); }
W32(ws2_32, getsockopt) { ret(c, 0xffffffffu, 5); }
W32(ws2_32, ioctlsocket) { ret(c, 0xffffffffu, 3); }
W32(ws2_32, shutdown) { ret(c, 0xffffffffu, 2); }
W32(ws2_32, getsockname) { ret(c, 0xffffffffu, 3); }
W32(ws2_32, getpeername) { ret(c, 0xffffffffu, 3); }
W32(ws2_32, WSAAsyncSelect) { ret(c, 0xffffffffu, 4); }
W32(ws2_32, __WSAFDIsSet) { ret(c, 0, 2); }

// ---- uplay32: the Ubisoft Connect check is the DLL's loading; nothing calls it ----

W32(uplay32, Test) { ret(c, 0, 0); }

}  // namespace scimitar::imports
