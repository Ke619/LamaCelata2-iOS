// Local replacement for ubiorbitapi_r2.dylib (see scimitar/orbit.h).
//
// Prototypes, as the game calls them (cdecl; S session, L listener, R reader,
// W writer, I info; callbacks are listener->vtable[0](listener, reqId, ...)):
//   S    GetSession()                          S GetFakeSession(ip, port)
//   void Update(S)                             unsigned GetRequestUniqueId(S)
//   bool StartLauncher(S, pid, patch, lang, extra)    -> false: keep running
//   void GetLoginDetails(S, req, L)            cb (char* user, wchar_t* pw, char*)
//   void GetOrbitServer(S, req, L, pid, sub)   never completed (no launcher pipe)
//   void GetSavegameReader(S, req, L, pid, id) cb (int status, R)
//   void GetSavegameWriter(S, req, L, pid, id, bool)  cb (int status, W)
//   void GetSavegameList(S, req, L, pid)       cb (I* cells, unsigned count)
//   void RemoveSavegame(S, req, L, pid, id)    cb (int status)
//   void GetNetworkTraffic(S, req, L)          cb (4 x unsigned)
//   const wchar_t* GetLocText(S, key, lang)    -> key
//   void ReaderRead(R, req, L, offset, dst, len)      cb (unsigned bytesRead)
//   void ReaderClose(R)
//   bool WriterSetName(W, wchar_t* name)
//   void WriterWrite(W, req, L, src, len)      appends; cb (unsigned bytesWritten)
//   void WriterClose(W, bool commit)
//   const wchar_t* InfoGetName(I)  unsigned InfoGetProductId/SavegameId/Size(I)
// Status 0 is success (the only value the game accepts).

#include "scimitar/orbit.h"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "scimitar/guest_heap.h"
#include "scimitar/imports.h"
#include "scimitar/runtime.h"

namespace scimitar::orbit {

const char* const kNames[kFunctionCount] = {
    "MgOrbitdllGetSession", "MgOrbitdllGetFakeSession", "MgOrbitdllUpdate",
    "MgOrbitdllGetRequestUniqueId", "MgOrbitdllStartLauncher", "MgOrbitdllGetLoginDetails",
    "MgOrbitdllGetOrbitServer", "MgOrbitdllGetSavegameReader", "MgOrbitdllGetSavegameWriter",
    "MgOrbitdllGetSavegameList", "MgOrbitdllRemoveSavegame", "MgOrbitdllGetNetworkTraffic",
    "MgOrbitdllGetLocText", "MgOrbitdllSaveGameReaderRead", "MgOrbitdllSaveGameReaderClose",
    "MgOrbitdllSaveGameWriterSetName", "MgOrbitdllSaveGameWriterWrite",
    "MgOrbitdllSaveGameWriterClose", "MgOrbitdllSaveGameInfoGetName",
    "MgOrbitdllSaveGameInfoGetProductId", "MgOrbitdllSaveGameInfoGetSavegameId",
    "MgOrbitdllSaveGameInfoGetSize",
};

namespace {

// ---- storage ----

using WString = std::vector<uint32_t>;  // the game's wchar_t is 32 bits

struct Save {
    uint32_t pid = 0, id = 0;
    WString name;
    std::vector<uint8_t> data;
};

constexpr char kMagic[4] = {'A', 'C', 'S', 'V'};
constexpr uint32_t kVersion = 1;

std::mutex g_mutex;
std::string g_dir;

std::string default_dir() {
    if (const char* e = std::getenv("SCIMITAR_SAVE_DIR")) return e;
    const char* home = std::getenv("HOME");
    return std::string(home ? home : ".") + "/Library/Application Support/scimitar-recomp/saves";
}

void make_dirs(const std::string& path) {
    for (size_t p = 1; p != std::string::npos; p = path.find('/', p + 1))
        mkdir(path.substr(0, p).c_str(), 0755);
    mkdir(path.c_str(), 0755);
}

std::string path_of(uint32_t pid, uint32_t id) {
    char name[64];
    std::snprintf(name, sizeof name, "/p%u-%u.sav", pid, id);
    return save_directory() + name;
}

bool load(const std::string& path, Save* s) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    char magic[4];
    uint32_t hdr[4], n = 0;  // version, pid, id, name length
    bool ok = std::fread(magic, 1, 4, f) == 4 && std::memcmp(magic, kMagic, 4) == 0 &&
              std::fread(hdr, 4, 4, f) == 4 && hdr[0] == kVersion && hdr[3] <= 4096;
    if (ok) {
        s->pid = hdr[1], s->id = hdr[2];
        s->name.resize(hdr[3]);
        ok = std::fread(s->name.data(), 4, hdr[3], f) == hdr[3] && std::fread(&n, 4, 1, f) == 1;
    }
    if (ok) {
        s->data.resize(n);
        ok = std::fread(s->data.data(), 1, n, f) == n;
    }
    std::fclose(f);
    return ok;
}

bool store(const Save& s) {
    make_dirs(save_directory());
    const std::string path = path_of(s.pid, s.id), tmp = path + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return false;
    const uint32_t hdr[4] = {kVersion, s.pid, s.id, (uint32_t)s.name.size()};
    const uint32_t n = (uint32_t)s.data.size();
    bool ok = std::fwrite(kMagic, 1, 4, f) == 4 && std::fwrite(hdr, 4, 4, f) == 4 &&
              std::fwrite(s.name.data(), 4, s.name.size(), f) == s.name.size() &&
              std::fwrite(&n, 4, 1, f) == 1 && std::fwrite(s.data.data(), 1, n, f) == n;
    ok = std::fflush(f) == 0 && fsync(fileno(f)) == 0 && ok;
    std::fclose(f);
    if (!ok || std::rename(tmp.c_str(), path.c_str()) != 0) {
        std::remove(tmp.c_str());
        return false;
    }
    return true;
}

std::vector<Save> list(uint32_t pid) {
    std::vector<Save> out;
    DIR* d = opendir(save_directory().c_str());
    if (!d) return out;
    while (dirent* e = readdir(d)) {
        unsigned p, id;
        char tail[8] = {};
        if (std::sscanf(e->d_name, "p%u-%u.%7s", &p, &id, tail) != 3 || p != pid ||
            std::strcmp(tail, "sav") != 0)
            continue;
        Save s;
        if (load(save_directory() + "/" + e->d_name, &s) && s.pid == pid && s.id == id)
            out.push_back(std::move(s));
    }
    closedir(d);
    std::sort(out.begin(), out.end(), [](const Save& a, const Save& b) { return a.id < b.id; });
    return out;
}

// ---- open readers/writers and the current listing ----

struct Open {
    Save save;
};
std::map<uint32_t, Open> g_readers, g_writers;
uint32_t g_next_handle = 0x0a5e0001;
uint32_t g_next_request = 0;

// Entries of the listing being delivered: info cell value -> entry.
struct Listed {
    uint32_t pid, id, size, name_addr;
};
std::vector<Listed> g_listing;

// ---- guest helpers ----

WString read_wstring(Context& c, uint32_t a, size_t max = 1024) {
    WString s;
    for (size_t k = 0; a && k < max; k++) {
        const uint32_t ch = c.mem->read<uint32_t>(a + 4 * (uint32_t)k);
        if (!ch) break;
        s.push_back(ch);
    }
    return s;
}

// Guest stack space below the caller's frame, valid while a callback runs.
// Must be released (destroyed) before the host function returns to the guest.
class StackSpace {
public:
    explicit StackSpace(Context& c) : c_(c), saved_(c.esp), top_(c.esp) {}
    ~StackSpace() { c_.esp = saved_; }
    uint32_t alloc(uint32_t bytes) {
        top_ = (top_ - bytes) & ~15u;
        c_.esp = top_;
        return top_;
    }
    uint32_t put(const std::string& s) {
        const uint32_t a = alloc((uint32_t)s.size() + 1);
        std::memcpy(c_.mem->ptr(a), s.c_str(), s.size() + 1);
        return a;
    }
    uint32_t put(const WString& s) {
        const uint32_t a = alloc(4 * (uint32_t)s.size() + 4);
        for (size_t k = 0; k < s.size(); k++) c_.mem->write<uint32_t>(a + 4 * (uint32_t)k, s[k]);
        c_.mem->write<uint32_t>(a + 4 * (uint32_t)s.size(), 0);
        return a;
    }

private:
    Context& c_;
    uint32_t saved_, top_;
};

// ---- the 22 functions ----

void GetSession(Context& c) { shim_return(c, kSession); }
void GetFakeSession(Context& c) { shim_return(c, kSession); }
void Update(Context& c) { shim_return(c, 0); }

void GetRequestUniqueId(Context& c) {
    std::lock_guard<std::mutex> l(g_mutex);
    if (++g_next_request == 0xffffffffu) g_next_request = 1;  // the wrappers' "no session"
    shim_return(c, g_next_request);
}

void StartLauncher(Context& c) { shim_return(c, 0); }  // true would mean "quit, the launcher restarts us"

void GetLoginDetails(Context& c) {
    const uint32_t req = arg32(c, 1), listener = arg32(c, 2);
    {
        StackSpace sp(c);
        const uint32_t user = sp.put(std::string("Player")), pw = sp.put(WString{}),
                       ticket = sp.put(std::string());
        call_virtual(c, listener, 0, {req, user, pw, ticket});
    }
    shim_return(c, 0);
}

// Never completed: a server id would make the game open a pipe to the launcher.
void GetOrbitServer(Context& c) { shim_return(c, 0); }

// A client object (Brotherhood's mg::orbitclient SavegameReader, Writer, Info):
// a 4-byte guest heap object holding the library's handle, as the client's own
// `new` around the library's object.
uint32_t client_object(Context& c, uint32_t handle) {
    const uint32_t o = heap::alloc(*c.mem, 4);
    if (!o) fatal("orbit: guest heap exhausted");
    c.mem->write<uint32_t>(o, handle);
    return o;
}

void get_reader(Context& c, bool client) {
    const uint32_t req = arg32(c, 1), listener = arg32(c, 2), pid = arg32(c, 3), id = arg32(c, 4);
    uint32_t handle = 0;
    {
        std::lock_guard<std::mutex> l(g_mutex);
        Save s;
        if (load(path_of(pid, id), &s)) {
            handle = g_next_handle++;
            g_readers[handle] = {std::move(s)};
        }
    }
    call_virtual(c, listener, 0, {req, handle ? 0u : 1u, client ? client_object(c, handle) : handle});
    shim_return(c, 0);
}
void GetSavegameReader(Context& c) { get_reader(c, false); }

void get_writer(Context& c, bool client) {
    const uint32_t req = arg32(c, 1), listener = arg32(c, 2), pid = arg32(c, 3), id = arg32(c, 4);
    uint32_t handle;
    {
        std::lock_guard<std::mutex> l(g_mutex);
        handle = g_next_handle++;
        Save s;
        s.pid = pid, s.id = id;
        g_writers[handle] = {std::move(s)};
    }
    call_virtual(c, listener, 0, {req, 0u, client ? client_object(c, handle) : handle});
    shim_return(c, 0);
}
void GetSavegameWriter(Context& c) { get_writer(c, false); }

// The library hands the listener an array of 4-byte info cells (cell k holds
// k), valid during the callback; the client, an array (new[], its count in
// front) of client info objects holding the cells. Here the client's objects
// hold k themselves, and the names stay in guest memory after the callback.
void get_list(Context& c, bool client) {
    const uint32_t req = arg32(c, 1), listener = arg32(c, 2), pid = arg32(c, 3);
    std::vector<Save> saves;
    {
        std::lock_guard<std::mutex> l(g_mutex);
        saves = list(pid);
    }
    {
        StackSpace sp(c);
        const uint32_t n = (uint32_t)saves.size();
        g_listing.clear();
        for (const Save& s : saves) {
            uint32_t name;
            if (client) {
                name = heap::alloc(*c.mem, 4 * (uint32_t)s.name.size() + 4);
                if (!name) fatal("orbit: guest heap exhausted");
                for (size_t k = 0; k < s.name.size(); k++) c.mem->write<uint32_t>(name + 4 * (uint32_t)k, s.name[k]);
                c.mem->write<uint32_t>(name + 4 * (uint32_t)s.name.size(), 0);
            } else {
                name = sp.put(s.name);
            }
            g_listing.push_back({s.pid, s.id, (uint32_t)s.data.size(), name});
        }
        uint32_t list;
        if (client) {
            const uint32_t block = heap::alloc(*c.mem, 4 * n + 4);
            if (!block) fatal("orbit: guest heap exhausted");
            c.mem->write<uint32_t>(block, n);
            list = block + 4;
            for (uint32_t k = 0; k < n; k++) c.mem->write<uint32_t>(list + 4 * k, client_object(c, k));
        } else {
            // The game takes &cells[i] as the info handle of entry i (4-byte cells).
            list = sp.alloc(4 * n + 4);
            for (uint32_t k = 0; k < n; k++) c.mem->write<uint32_t>(list + 4 * k, k);
        }
        call_virtual(c, listener, 0, {req, list, n});
    }
    shim_return(c, 0);
}
void GetSavegameList(Context& c) { get_list(c, false); }

void RemoveSavegame(Context& c) {
    const uint32_t req = arg32(c, 1), listener = arg32(c, 2), pid = arg32(c, 3), id = arg32(c, 4);
    {
        std::lock_guard<std::mutex> l(g_mutex);
        std::remove(path_of(pid, id).c_str());
    }
    call_virtual(c, listener, 0, {req, 0u});
    shim_return(c, 0);
}

void GetNetworkTraffic(Context& c) {
    const uint32_t req = arg32(c, 1), listener = arg32(c, 2);
    call_virtual(c, listener, 0, {req, 0u, 0u, 0u, 0u});
    shim_return(c, 0);
}

void GetLocText(Context& c) { shim_return(c, arg32(c, 1)); }

void ReaderRead(Context& c) {
    const uint32_t handle = arg32(c, 0), req = arg32(c, 1), listener = arg32(c, 2);
    const uint32_t offset = arg32(c, 3), dst = arg32(c, 4), len = arg32(c, 5);
    uint32_t n = 0;
    {
        std::lock_guard<std::mutex> l(g_mutex);
        auto it = g_readers.find(handle);
        if (it == g_readers.end()) fatal("orbit: read from unknown reader %08x", handle);
        const auto& data = it->second.save.data;
        if (offset < data.size()) n = std::min<uint32_t>(len, (uint32_t)data.size() - offset);
        if ((uint64_t)dst + n > GuestMemory::size()) fatal("orbit: read destination out of range");
        std::memcpy(c.mem->ptr(dst), data.data() + offset, n);
    }
    call_virtual(c, listener, 0, {req, n});
    shim_return(c, 0);
}

void ReaderClose(Context& c) {
    std::lock_guard<std::mutex> l(g_mutex);
    g_readers.erase(arg32(c, 0));
    shim_return(c, 0);
}

void WriterSetName(Context& c) {
    const uint32_t handle = arg32(c, 0);
    WString name = read_wstring(c, arg32(c, 1));
    std::lock_guard<std::mutex> l(g_mutex);
    auto it = g_writers.find(handle);
    if (it == g_writers.end()) return shim_return(c, 0);
    it->second.save.name = std::move(name);
    shim_return(c, 1);
}

void WriterWrite(Context& c) {
    const uint32_t handle = arg32(c, 0), req = arg32(c, 1), listener = arg32(c, 2);
    const uint32_t src = arg32(c, 3), len = arg32(c, 4);
    {
        std::lock_guard<std::mutex> l(g_mutex);
        auto it = g_writers.find(handle);
        if (it == g_writers.end()) fatal("orbit: write to unknown writer %08x", handle);
        if ((uint64_t)src + len > GuestMemory::size()) fatal("orbit: write source out of range");
        auto& data = it->second.save.data;
        data.insert(data.end(), c.mem->ptr(src), c.mem->ptr(src) + len);
    }
    call_virtual(c, listener, 0, {req, len});
    shim_return(c, 0);
}

void WriterClose(Context& c) {
    const uint32_t handle = arg32(c, 0);
    const bool commit = arg32(c, 1) & 0xff;
    std::lock_guard<std::mutex> l(g_mutex);
    auto it = g_writers.find(handle);
    if (it != g_writers.end()) {
        if (commit && !store(it->second.save))
            std::fprintf(stderr, "orbit: could not write save %u to %s\n", it->second.save.id,
                         save_directory().c_str());
        g_writers.erase(it);
    }
    shim_return(c, 0);
}

const Listed* listed(Context& c, uint32_t info) {
    const uint32_t k = info ? c.mem->read<uint32_t>(info) : ~0u;
    return k < g_listing.size() ? &g_listing[k] : nullptr;
}
void InfoGetName(Context& c) { const Listed* e = listed(c, arg32(c, 0)); shim_return(c, e ? e->name_addr : 0); }
void InfoGetProductId(Context& c) { const Listed* e = listed(c, arg32(c, 0)); shim_return(c, e ? e->pid : ~0u); }
void InfoGetSavegameId(Context& c) { const Listed* e = listed(c, arg32(c, 0)); shim_return(c, e ? e->id : ~0u); }
void InfoGetSize(Context& c) { const Listed* e = listed(c, arg32(c, 0)); shim_return(c, e ? e->size : 0); }

const GuestFn kImpl[kFunctionCount] = {
    GetSession, GetFakeSession, Update, GetRequestUniqueId, StartLauncher, GetLoginDetails,
    GetOrbitServer, GetSavegameReader, GetSavegameWriter, GetSavegameList, RemoveSavegame,
    GetNetworkTraffic, GetLocText, ReaderRead, ReaderClose, WriterSetName, WriterWrite,
    WriterClose, InfoGetName, InfoGetProductId, InfoGetSavegameId, InfoGetSize,
};

// ---- the C++ client (Brotherhood) ----
//
// Brotherhood links the C++ Orbit client that AC2 builds in statically,
// mg::orbitclient, from libubiorbitapi_r2_loader.dylib: its methods are the
// game's imports. That client is a thin layer over the 22 functions: an
// OrbitClient holds a pimpl around the session, requests pass their listener
// through and complete with the same callbacks, except that readers, writers
// and listed infos are client objects (client_object). These methods are that
// layer, natively; `this` of an OrbitClient holds the session. The C++
// arguments line up with the functions' (this in place of the session), and a
// client object's `this` is replaced by its handle where a function takes one.

void replace_this_by_handle(Context& c) { c.mem->write<uint32_t>(c.esp + 4, c.mem->read<uint32_t>(arg32(c, 0))); }

void ClientConstruct(Context& c) {
    c.mem->write<uint32_t>(arg32(c, 0), kSession);
    shim_return(c, 0);
}
void ClientDestruct(Context& c) { shim_return(c, 0); }
void ClientGetInstallationErrorNum(Context& c) { shim_return(c, 0); }  // the launcher library "loaded"
void ClientGetInstallationErrorString(Context& c) {
    static uint32_t empty = 0;  // L""
    if (!empty) empty = heap::alloc(*c.mem, 4), c.mem->write<uint32_t>(empty, 0);
    shim_return(c, empty);
}
void ClientGetSavegameReader(Context& c) { get_reader(c, true); }
void ClientGetSavegameWriter(Context& c) { get_writer(c, true); }
void ClientGetSavegameList(Context& c) { get_list(c, true); }
void ClientReaderRead(Context& c) { replace_this_by_handle(c), ReaderRead(c); }
void ClientReaderClose(Context& c) { replace_this_by_handle(c), ReaderClose(c); }
void ClientWriterSetName(Context& c) { replace_this_by_handle(c), WriterSetName(c); }
void ClientWriterWrite(Context& c) { replace_this_by_handle(c), WriterWrite(c); }
void ClientWriterClose(Context& c) { replace_this_by_handle(c), WriterClose(c); }

}  // namespace

const uint32_t* functions() {
    static const auto addrs = [] {
        std::vector<uint32_t> a;
        for (int k = 0; k < kFunctionCount; k++) a.push_back(register_host_function(kImpl[k], kNames[k]));
        return a;
    }();
    return addrs.data();
}

void set_save_directory(const std::string& dir) {
    std::lock_guard<std::mutex> l(g_mutex);
    g_dir = dir;
}

std::string save_directory() { return g_dir.empty() ? default_dir() : g_dir; }

}  // namespace scimitar::orbit

// The client's methods as the game's imports (generated code calls imp_*).
namespace scimitar::imports {
void imp__ZN2mg11orbitclient11OrbitClientC1Ev(Context& c) { orbit::ClientConstruct(c); }
void imp__ZN2mg11orbitclient11OrbitClientD1Ev(Context& c) { orbit::ClientDestruct(c); }
void imp__ZN2mg11orbitclient11OrbitClient6UpdateEv(Context& c) { orbit::Update(c); }
void imp__ZN2mg11orbitclient11OrbitClient18GetRequestUniqueIdEv(Context& c) { orbit::GetRequestUniqueId(c); }
void imp__ZN2mg11orbitclient11OrbitClient13StartLauncherEjjPKcS3_(Context& c) { orbit::StartLauncher(c); }
void imp__ZN2mg11orbitclient11OrbitClient15GetLoginDetailsEjPNS0_24IGetLoginDetailsListenerE(Context& c) { orbit::GetLoginDetails(c); }
void imp__ZN2mg11orbitclient11OrbitClient10GetLocTextEPKwPKc(Context& c) { orbit::GetLocText(c); }
void imp__ZN2mg11orbitclient11OrbitClient23GetInstallationErrorNumEv(Context& c) { orbit::ClientGetInstallationErrorNum(c); }
void imp__ZN2mg11orbitclient11OrbitClient26GetInstallationErrorStringEPKc(Context& c) { orbit::ClientGetInstallationErrorString(c); }
void imp__ZN2mg11orbitclient11OrbitClient17GetSavegameReaderEjPNS0_26IGetSavegameReaderListenerEjj(Context& c) { orbit::ClientGetSavegameReader(c); }
void imp__ZN2mg11orbitclient11OrbitClient17GetSavegameWriterEjPNS0_26IGetSavegameWriterListenerEjjb(Context& c) { orbit::ClientGetSavegameWriter(c); }
void imp__ZN2mg11orbitclient11OrbitClient15GetSavegameListEjPNS0_24IGetSavegameListListenerEj(Context& c) { orbit::ClientGetSavegameList(c); }
void imp__ZN2mg11orbitclient11OrbitClient14RemoveSavegameEjPNS0_23IRemoveSavegameListenerEjj(Context& c) { orbit::RemoveSavegame(c); }
void imp__ZN2mg11orbitclient14SavegameReader4ReadEjPNS0_21ISavegameReadListenerEjPvj(Context& c) { orbit::ClientReaderRead(c); }
void imp__ZN2mg11orbitclient14SavegameReader5CloseEv(Context& c) { orbit::ClientReaderClose(c); }
void imp__ZN2mg11orbitclient14SavegameWriter7SetNameEPw(Context& c) { orbit::ClientWriterSetName(c); }
void imp__ZN2mg11orbitclient14SavegameWriter5WriteEjPNS0_22ISavegameWriteListenerEPvj(Context& c) { orbit::ClientWriterWrite(c); }
void imp__ZN2mg11orbitclient14SavegameWriter5CloseEb(Context& c) { orbit::ClientWriterClose(c); }
void imp__ZN2mg11orbitclient12SavegameInfo7GetNameEv(Context& c) { orbit::InfoGetName(c); }
void imp__ZN2mg11orbitclient12SavegameInfo13GetSavegameIdEv(Context& c) { orbit::InfoGetSavegameId(c); }
void imp__ZN2mg11orbitclient12SavegameInfo7GetSizeEv(Context& c) { orbit::InfoGetSize(c); }
}  // namespace scimitar::imports

namespace scimitar {
namespace {
const ImportShim kOrbitClient[] = {
    {"__ZN2mg11orbitclient11OrbitClientC1Ev", imports::imp__ZN2mg11orbitclient11OrbitClientC1Ev},
    {"__ZN2mg11orbitclient11OrbitClientD1Ev", imports::imp__ZN2mg11orbitclient11OrbitClientD1Ev},
    {"__ZN2mg11orbitclient11OrbitClient6UpdateEv", imports::imp__ZN2mg11orbitclient11OrbitClient6UpdateEv},
    {"__ZN2mg11orbitclient11OrbitClient18GetRequestUniqueIdEv", imports::imp__ZN2mg11orbitclient11OrbitClient18GetRequestUniqueIdEv},
    {"__ZN2mg11orbitclient11OrbitClient13StartLauncherEjjPKcS3_", imports::imp__ZN2mg11orbitclient11OrbitClient13StartLauncherEjjPKcS3_},
    {"__ZN2mg11orbitclient11OrbitClient15GetLoginDetailsEjPNS0_24IGetLoginDetailsListenerE", imports::imp__ZN2mg11orbitclient11OrbitClient15GetLoginDetailsEjPNS0_24IGetLoginDetailsListenerE},
    {"__ZN2mg11orbitclient11OrbitClient10GetLocTextEPKwPKc", imports::imp__ZN2mg11orbitclient11OrbitClient10GetLocTextEPKwPKc},
    {"__ZN2mg11orbitclient11OrbitClient23GetInstallationErrorNumEv", imports::imp__ZN2mg11orbitclient11OrbitClient23GetInstallationErrorNumEv},
    {"__ZN2mg11orbitclient11OrbitClient26GetInstallationErrorStringEPKc", imports::imp__ZN2mg11orbitclient11OrbitClient26GetInstallationErrorStringEPKc},
    {"__ZN2mg11orbitclient11OrbitClient17GetSavegameReaderEjPNS0_26IGetSavegameReaderListenerEjj", imports::imp__ZN2mg11orbitclient11OrbitClient17GetSavegameReaderEjPNS0_26IGetSavegameReaderListenerEjj},
    {"__ZN2mg11orbitclient11OrbitClient17GetSavegameWriterEjPNS0_26IGetSavegameWriterListenerEjjb", imports::imp__ZN2mg11orbitclient11OrbitClient17GetSavegameWriterEjPNS0_26IGetSavegameWriterListenerEjjb},
    {"__ZN2mg11orbitclient11OrbitClient15GetSavegameListEjPNS0_24IGetSavegameListListenerEj", imports::imp__ZN2mg11orbitclient11OrbitClient15GetSavegameListEjPNS0_24IGetSavegameListListenerEj},
    {"__ZN2mg11orbitclient11OrbitClient14RemoveSavegameEjPNS0_23IRemoveSavegameListenerEjj", imports::imp__ZN2mg11orbitclient11OrbitClient14RemoveSavegameEjPNS0_23IRemoveSavegameListenerEjj},
    {"__ZN2mg11orbitclient14SavegameReader4ReadEjPNS0_21ISavegameReadListenerEjPvj", imports::imp__ZN2mg11orbitclient14SavegameReader4ReadEjPNS0_21ISavegameReadListenerEjPvj},
    {"__ZN2mg11orbitclient14SavegameReader5CloseEv", imports::imp__ZN2mg11orbitclient14SavegameReader5CloseEv},
    {"__ZN2mg11orbitclient14SavegameWriter7SetNameEPw", imports::imp__ZN2mg11orbitclient14SavegameWriter7SetNameEPw},
    {"__ZN2mg11orbitclient14SavegameWriter5WriteEjPNS0_22ISavegameWriteListenerEPvj", imports::imp__ZN2mg11orbitclient14SavegameWriter5WriteEjPNS0_22ISavegameWriteListenerEPvj},
    {"__ZN2mg11orbitclient14SavegameWriter5CloseEb", imports::imp__ZN2mg11orbitclient14SavegameWriter5CloseEb},
    {"__ZN2mg11orbitclient12SavegameInfo7GetNameEv", imports::imp__ZN2mg11orbitclient12SavegameInfo7GetNameEv},
    {"__ZN2mg11orbitclient12SavegameInfo13GetSavegameIdEv", imports::imp__ZN2mg11orbitclient12SavegameInfo13GetSavegameIdEv},
    {"__ZN2mg11orbitclient12SavegameInfo7GetSizeEv", imports::imp__ZN2mg11orbitclient12SavegameInfo7GetSizeEv},
};
SCIMITAR_REGISTER_SHIMS(kOrbitClient);
}  // namespace
}  // namespace scimitar
