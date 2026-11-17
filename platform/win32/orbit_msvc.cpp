// The Orbit C++ client as Revelations imports it from
// ubiorbitapi_r2_loader.dll: the same 22 mg::orbitclient methods Brotherhood
// imports on the Mac (platform/src/orbit.cpp implements those), compiled by
// MSVC. Three differences, bridged here, so the saves and the offline session
// are the Mac ports' own:
//
// - Methods are thiscall: `this` in ecx, arguments on the stack, popped by the
//   callee. Each method here lays out the GCC frame the Mac method expects
//   ([return] this args...) below the caller's arguments and calls it.
// - Listeners are MSVC objects too: their callback (vtable slot 0) is
//   thiscall. The Mac methods call listener->vtable[0](listener, args...) the
//   GCC way, so each request passes a proxy listener whose slot 0 is a host
//   function that forwards to the game's listener with `this` in ecx.
// - wchar_t is 16 bits (UTF-16) where the Mac's is 32: save names are
//   converted both ways. The other strings that cross are empty or ASCII keys
//   the game only passes through.
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

#include "scimitar/guest_heap.h"
#include "scimitar/runtime.h"

namespace scimitar::imports {
// platform/src/orbit.cpp: Brotherhood's client methods (GCC frames).
void imp__ZN2mg11orbitclient11OrbitClientC1Ev(Context& c);
void imp__ZN2mg11orbitclient11OrbitClientD1Ev(Context& c);
void imp__ZN2mg11orbitclient11OrbitClient6UpdateEv(Context& c);
void imp__ZN2mg11orbitclient11OrbitClient18GetRequestUniqueIdEv(Context& c);
void imp__ZN2mg11orbitclient11OrbitClient13StartLauncherEjjPKcS3_(Context& c);
void imp__ZN2mg11orbitclient11OrbitClient15GetLoginDetailsEjPNS0_24IGetLoginDetailsListenerE(Context& c);
void imp__ZN2mg11orbitclient11OrbitClient23GetInstallationErrorNumEv(Context& c);
void imp__ZN2mg11orbitclient11OrbitClient26GetInstallationErrorStringEPKc(Context& c);
void imp__ZN2mg11orbitclient11OrbitClient17GetSavegameReaderEjPNS0_26IGetSavegameReaderListenerEjj(Context& c);
void imp__ZN2mg11orbitclient11OrbitClient17GetSavegameWriterEjPNS0_26IGetSavegameWriterListenerEjjb(Context& c);
void imp__ZN2mg11orbitclient11OrbitClient15GetSavegameListEjPNS0_24IGetSavegameListListenerEj(Context& c);
void imp__ZN2mg11orbitclient11OrbitClient14RemoveSavegameEjPNS0_23IRemoveSavegameListenerEjj(Context& c);
void imp__ZN2mg11orbitclient14SavegameReader4ReadEjPNS0_21ISavegameReadListenerEjPvj(Context& c);
void imp__ZN2mg11orbitclient14SavegameReader5CloseEv(Context& c);
void imp__ZN2mg11orbitclient14SavegameWriter7SetNameEPw(Context& c);
void imp__ZN2mg11orbitclient14SavegameWriter5WriteEjPNS0_22ISavegameWriteListenerEPvj(Context& c);
void imp__ZN2mg11orbitclient14SavegameWriter5CloseEb(Context& c);
void imp__ZN2mg11orbitclient12SavegameInfo7GetNameEv(Context& c);
void imp__ZN2mg11orbitclient12SavegameInfo13GetSavegameIdEv(Context& c);
void imp__ZN2mg11orbitclient12SavegameInfo7GetSizeEv(Context& c);
}  // namespace scimitar::imports

namespace scimitar::orbit_msvc {
namespace {

using Method = void (*)(Context&);

// The return address call_guest pushes (never executed: the callee returns to us).
constexpr uint32_t kReturnToHost = 0xfffffff0;

// Calls the Mac method `m` as if the game had called it with `this` and
// `args` (GCC: all on the stack, the caller pops), and returns eax. The frame
// goes below the current stack pointer, so the thiscall frame above survives.
uint32_t call_mac(Context& c, Method m, uint32_t self, std::initializer_list<uint32_t> args) {
    const uint32_t esp = c.esp;
    c.esp = (c.esp - 4 * (uint32_t)(args.size() + 2)) & ~15u;
    uint32_t p = c.esp;
    c.mem->write<uint32_t>(p, kReturnToHost), p += 4;
    c.mem->write<uint32_t>(p, self), p += 4;
    for (uint32_t a : args) c.mem->write<uint32_t>(p, a), p += 4;
    m(c);  // a shim: returns with its return address popped
    c.esp = esp;
    return c.eax;
}

// thiscall return: eax, then the return address and `nargs` words popped.
void ret(Context& c, uint32_t eax, int nargs) {
    c.eax = eax;
    c.esp += 4 + 4u * (uint32_t)nargs;
}

// ---- proxy listeners ----

// A proxy is a guest object { vtable, game listener, argument count, kind };
// its vtable's slot 0 is `forward`. Proxies are reused per (listener, count).
// The save list's kind converts the list: the Mac client hands the listener
// an array of pointers to SavegameInfo objects, Revelations' client an array
// of the 4-byte objects themselves (the game takes element k's address as
// the object), each holding the entry's index.
enum Kind : uint32_t { kPlain = 0, kSaveList = 1 };
uint32_t g_proxy_vtable = 0;
std::mutex g_mutex;
std::map<std::pair<uint32_t, uint32_t>, uint32_t> g_proxies;

// GCC frame: [return] proxy args...; the game's callback takes args with
// `this` in ecx and pops them.
void forward(Context& c) {
    const uint32_t proxy = arg32(c, 0);
    const uint32_t listener = c.mem->read<uint32_t>(proxy + 4), n = c.mem->read<uint32_t>(proxy + 8);
    std::vector<uint32_t> args(n);
    for (uint32_t k = 0; k < n; k++) args[k] = arg32(c, 1 + (int)k);
    if (c.mem->read<uint32_t>(proxy + 12) == kSaveList && n == 3) {  // (req, list, count)
        const uint32_t count = args[2], cells = heap::alloc(*c.mem, 4 * count + 4);
        if (!cells) fatal("orbit: guest heap exhausted");
        for (uint32_t k = 0; k < count; k++)
            c.mem->write<uint32_t>(cells + 4 * k, c.mem->read<uint32_t>(c.mem->read<uint32_t>(args[1] + 4 * k)));
        args[1] = cells;
    }
    const uint32_t ecx = c.ecx;
    c.ecx = listener;
    const uint32_t fn = c.mem->read<uint32_t>(c.mem->read<uint32_t>(listener));
    const uint32_t esp = c.esp;
    c.esp -= 4 * n;
    for (uint32_t k = 0; k < n; k++) c.mem->write<uint32_t>(c.esp + 4 * k, args[k]);
    c.esp -= 4;
    c.mem->write<uint32_t>(c.esp, kReturnToHost);
    const FuncEntry* f = find_function(fn);
    if (!f) fatal("orbit: listener callback %08x is not a recompiled function", fn);
    f->fn(c);
    c.esp = esp;
    c.ecx = ecx;
    shim_return(c, c.eax);
}

uint32_t proxy(Context& c, uint32_t listener, uint32_t nargs, uint32_t kind = kPlain) {
    std::lock_guard<std::mutex> l(g_mutex);
    if (!g_proxy_vtable) {
        g_proxy_vtable = heap::alloc(*c.mem, 4);
        c.mem->write<uint32_t>(g_proxy_vtable, register_host_function(forward, "orbit_msvc::forward"));
    }
    uint32_t& p = g_proxies[{listener, nargs | kind << 16}];
    if (!p) {
        p = heap::alloc(*c.mem, 16);
        if (!p) fatal("orbit: guest heap exhausted");
        c.mem->write<uint32_t>(p, g_proxy_vtable);
        c.mem->write<uint32_t>(p + 4, listener);
        c.mem->write<uint32_t>(p + 8, nargs);
        c.mem->write<uint32_t>(p + 12, kind);
    }
    return p;
}

// ---- strings ----

// UTF-16 (the game's) to a temporary UTF-32 copy below the stack.
uint32_t utf32_on_stack(Context& c, uint32_t s) {
    std::vector<uint32_t> out;
    for (uint32_t a = s; a && out.size() < 1024; a += 2) {
        uint32_t ch = c.mem->read<uint16_t>(a);
        if (!ch) break;
        if (ch >= 0xd800 && ch < 0xdc00) {
            const uint32_t lo = c.mem->read<uint16_t>(a + 2);
            if (lo >= 0xdc00 && lo < 0xe000) ch = 0x10000 + ((ch - 0xd800) << 10) + (lo - 0xdc00), a += 2;
        }
        out.push_back(ch);
    }
    out.push_back(0);
    c.esp = (c.esp - 4 * (uint32_t)out.size() - 64) & ~15u;  // released by the caller restoring esp
    for (size_t k = 0; k < out.size(); k++) c.mem->write<uint32_t>(c.esp + 4 * (uint32_t)k, out[k]);
    return c.esp;
}

// A UTF-32 string (the Mac layer's) as a UTF-16 copy on the guest heap, kept
// per source address: the game holds name pointers while it lists saves.
std::map<uint32_t, uint32_t> g_utf16;
uint32_t utf16_copy(Context& c, uint32_t s) {
    if (!s) return 0;
    std::lock_guard<std::mutex> l(g_mutex);
    uint32_t& out = g_utf16[s];
    if (out) return out;
    std::vector<uint16_t> u;
    for (uint32_t a = s; u.size() < 2048; a += 4) {
        uint32_t ch = c.mem->read<uint32_t>(a);
        if (!ch) break;
        if (ch >= 0x10000) {
            ch -= 0x10000;
            u.push_back((uint16_t)(0xd800 + (ch >> 10)));
            u.push_back((uint16_t)(0xdc00 + (ch & 0x3ff)));
        } else {
            u.push_back((uint16_t)ch);
        }
    }
    u.push_back(0);
    out = heap::alloc(*c.mem, 2 * (uint32_t)u.size());
    if (!out) fatal("orbit: guest heap exhausted");
    std::memcpy(c.mem->ptr(out), u.data(), 2 * u.size());
    return out;
}

uint32_t empty_utf16(Context& c) {
    static uint32_t s = [&] {
        const uint32_t a = heap::alloc(*c.mem, 4);
        c.mem->write<uint32_t>(a, 0);
        return a;
    }();
    return s;
}

uint32_t a(const Context& c, int i) { return arg32(c, i); }  // thiscall: arguments after the return address

}  // namespace
}  // namespace scimitar::orbit_msvc

// The imports (recomp's import_symbol of "ubiorbitapi_r2_loader!<MSVC name>").
namespace scimitar::imports {
using namespace orbit_msvc;

// OrbitClient::OrbitClient(): the client object holds the session.
void imp_ubiorbitapi_r2_loader___0OrbitClient_orbitclient_mg__QAE_XZ(Context& c) {
    call_mac(c, imp__ZN2mg11orbitclient11OrbitClientC1Ev, c.ecx, {});
    ret(c, c.ecx, 0);
}
void imp_ubiorbitapi_r2_loader___1OrbitClient_orbitclient_mg__QAE_XZ(Context& c) { ret(c, 0, 0); }
void imp_ubiorbitapi_r2_loader__Update_OrbitClient_orbitclient_mg__QAEXXZ(Context& c) { ret(c, 0, 0); }
void imp_ubiorbitapi_r2_loader__GetRequestUniqueId_OrbitClient_orbitclient_mg__QAEIXZ(Context& c) {
    ret(c, call_mac(c, imp__ZN2mg11orbitclient11OrbitClient18GetRequestUniqueIdEv, c.ecx, {}), 0);
}
// No launcher: false means "keep running" (true would ask the game to quit).
void imp_ubiorbitapi_r2_loader__StartLauncher_OrbitClient_orbitclient_mg__QAE_NIIPBD0_Z(Context& c) { ret(c, 0, 4); }
void imp_ubiorbitapi_r2_loader__StartProcess_OrbitClient_orbitclient_mg__QAEXPAG00_Z(Context& c) { ret(c, 0, 3); }
void imp_ubiorbitapi_r2_loader__GetInstallationErrorNum_OrbitClient_orbitclient_mg__QAEIXZ(Context& c) {
    ret(c, 0, 0);  // the launcher library "loaded"
}
void imp_ubiorbitapi_r2_loader__GetInstallationErrorString_OrbitClient_orbitclient_mg__QAEPAGPBD_Z(Context& c) {
    ret(c, empty_utf16(c), 1);
}
void imp_ubiorbitapi_r2_loader__GetLocText_OrbitClient_orbitclient_mg__QAEPBGPBGPBD_Z(Context& c) {
    ret(c, a(c, 0), 2);  // the key itself
}
// Callback (req, char* user, wchar_t* password, char* ticket): the password is
// empty, which reads the same in UTF-16 and UTF-32.
void imp_ubiorbitapi_r2_loader__GetLoginDetails_OrbitClient_orbitclient_mg__QAEXIPAVIGetLoginDetailsListener_23__Z(
    Context& c) {
    call_mac(c, imp__ZN2mg11orbitclient11OrbitClient15GetLoginDetailsEjPNS0_24IGetLoginDetailsListenerE, c.ecx,
             {a(c, 0), proxy(c, a(c, 1), 4)});
    ret(c, 0, 2);
}
// Callback (req, status, reader).
void imp_ubiorbitapi_r2_loader__GetSavegameReader_OrbitClient_orbitclient_mg__QAEXIPAVIGetSavegameReaderListener_23_II_Z(
    Context& c) {
    call_mac(c, imp__ZN2mg11orbitclient11OrbitClient17GetSavegameReaderEjPNS0_26IGetSavegameReaderListenerEjj,
             c.ecx, {a(c, 0), proxy(c, a(c, 1), 3), a(c, 2), a(c, 3)});
    ret(c, 0, 4);
}
// Callback (req, status, writer).
void imp_ubiorbitapi_r2_loader__GetSavegameWriter_OrbitClient_orbitclient_mg__QAEXIPAVIGetSavegameWriterListener_23_II_N_Z(
    Context& c) {
    call_mac(c, imp__ZN2mg11orbitclient11OrbitClient17GetSavegameWriterEjPNS0_26IGetSavegameWriterListenerEjjb,
             c.ecx, {a(c, 0), proxy(c, a(c, 1), 3), a(c, 2), a(c, 3), a(c, 4)});
    ret(c, 0, 5);
}
// Callback (req, SavegameInfo list[], count): see Kind.
void imp_ubiorbitapi_r2_loader__GetSavegameList_OrbitClient_orbitclient_mg__QAEXIPAVIGetSavegameListListener_23_I_Z(
    Context& c) {
    call_mac(c, imp__ZN2mg11orbitclient11OrbitClient15GetSavegameListEjPNS0_24IGetSavegameListListenerEj, c.ecx,
             {a(c, 0), proxy(c, a(c, 1), 3, kSaveList), a(c, 2)});
    ret(c, 0, 3);
}
// Callback (req, status).
void imp_ubiorbitapi_r2_loader__RemoveSavegame_OrbitClient_orbitclient_mg__QAEXIPAVIRemoveSavegameListener_23_II_Z(
    Context& c) {
    call_mac(c, imp__ZN2mg11orbitclient11OrbitClient14RemoveSavegameEjPNS0_23IRemoveSavegameListenerEjj, c.ecx,
             {a(c, 0), proxy(c, a(c, 1), 2), a(c, 2), a(c, 3)});
    ret(c, 0, 4);
}
// Callback (req, bytes read).
void imp_ubiorbitapi_r2_loader__Read_SavegameReader_orbitclient_mg__QAEXIPAVISavegameReadListener_23_IPAXI_Z(Context& c) {
    call_mac(c, imp__ZN2mg11orbitclient14SavegameReader4ReadEjPNS0_21ISavegameReadListenerEjPvj, c.ecx,
             {a(c, 0), proxy(c, a(c, 1), 2), a(c, 2), a(c, 3), a(c, 4)});
    ret(c, 0, 5);
}
void imp_ubiorbitapi_r2_loader__Close_SavegameReader_orbitclient_mg__QAEXXZ(Context& c) {
    call_mac(c, imp__ZN2mg11orbitclient14SavegameReader5CloseEv, c.ecx, {});
    ret(c, 0, 0);
}
void imp_ubiorbitapi_r2_loader__SetName_SavegameWriter_orbitclient_mg__QAE_NPAG_Z(Context& c) {
    const uint32_t esp = c.esp, self = c.ecx, name = utf32_on_stack(c, a(c, 0));
    const uint32_t ok = call_mac(c, imp__ZN2mg11orbitclient14SavegameWriter7SetNameEPw, self, {name});
    c.esp = esp;
    ret(c, ok & 0xff, 1);
}
// Callback (req, bytes written).
void imp_ubiorbitapi_r2_loader__Write_SavegameWriter_orbitclient_mg__QAEXIPAVISavegameWriteListener_23_PAXI_Z(Context& c) {
    call_mac(c, imp__ZN2mg11orbitclient14SavegameWriter5WriteEjPNS0_22ISavegameWriteListenerEPvj, c.ecx,
             {a(c, 0), proxy(c, a(c, 1), 2), a(c, 2), a(c, 3)});
    ret(c, 0, 4);
}
void imp_ubiorbitapi_r2_loader__Close_SavegameWriter_orbitclient_mg__QAEX_N_Z(Context& c) {
    call_mac(c, imp__ZN2mg11orbitclient14SavegameWriter5CloseEb, c.ecx, {a(c, 0) & 0xff});
    ret(c, 0, 1);
}
void imp_ubiorbitapi_r2_loader__GetName_SavegameInfo_orbitclient_mg__QAEPBGXZ(Context& c) {
    ret(c, utf16_copy(c, call_mac(c, imp__ZN2mg11orbitclient12SavegameInfo7GetNameEv, c.ecx, {})), 0);
}
void imp_ubiorbitapi_r2_loader__GetSavegameId_SavegameInfo_orbitclient_mg__QAEIXZ(Context& c) {
    ret(c, call_mac(c, imp__ZN2mg11orbitclient12SavegameInfo13GetSavegameIdEv, c.ecx, {}), 0);
}
void imp_ubiorbitapi_r2_loader__GetSize_SavegameInfo_orbitclient_mg__QAEIXZ(Context& c) {
    ret(c, call_mac(c, imp__ZN2mg11orbitclient12SavegameInfo7GetSizeEv, c.ecx, {}), 0);
}

}  // namespace scimitar::imports
