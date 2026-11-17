// The local Orbit replacement (platform/src/orbit.cpp), driven the way the
// game's mg::orbitclient wrappers drive the real library: requests with a
// fresh request id and a listener object whose vtable slot 0 receives the
// result. Covers the save flow of scimitar::OrbitClient (list, find by name or
// take the smallest free id, write in pieces, read back, remove), the options
// blob, the offline session, and saves surviving a "restart". Then the same
// through the C++ client's methods, as Brotherhood imports them
// (mg::orbitclient::OrbitClient, SavegameReader/Writer/Info). No game files.

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "scimitar/imports.h"
#include "scimitar/orbit.h"
#include "scimitar/runtime.h"

using namespace scimitar;

namespace {

constexpr uint32_t kStackLo = 0x50000000, kStackTop = 0x50100000;
constexpr uint32_t kHeap = 0x60000000, kHeapSize = 0x100000;

enum Fn {
    GetSession, GetFakeSession, Update, GetRequestUniqueId, StartLauncher, GetLoginDetails,
    GetOrbitServer, GetSavegameReader, GetSavegameWriter, GetSavegameList, RemoveSavegame,
    GetNetworkTraffic, GetLocText, ReaderRead, ReaderClose, WriterSetName, WriterWrite,
    WriterClose, InfoGetName, InfoGetProductId, InfoGetSavegameId, InfoGetSize,
};

int failures = 0;
void check(bool ok, const std::string& what) {
    if (!ok) {
        failures++;
        std::printf("FAIL %s\n", what.c_str());
    }
}

GuestMemory* g_mem;
uint32_t g_heap = kHeap;
uint32_t halloc(uint32_t n) {
    const uint32_t a = (g_heap + 15) & ~15u;
    g_heap = a + n;
    return a;
}

// Every callback received: listener, request id, up to four more words.
struct Callback {
    uint32_t listener, req, a[4];
};
std::vector<Callback> g_calls;

// The list callback also reads every entry through the info getters while it
// runs, as the game does.
struct Entry {
    std::u32string name;
    uint32_t id, size, pid;
};
std::vector<Entry> g_listed;

uint32_t fn(int k) { return orbit::functions()[k]; }

void record(Context& c) {
    Callback cb{arg32(c, 0), arg32(c, 1), {arg32(c, 2), arg32(c, 3), arg32(c, 4), arg32(c, 5)}};
    g_calls.push_back(cb);
    shim_return(c, 0);
}

void record_list(Context& c) {
    Callback cb{arg32(c, 0), arg32(c, 1), {arg32(c, 2), arg32(c, 3), 0, 0}};
    g_calls.push_back(cb);
    g_listed.clear();
    const uint32_t cells = cb.a[0], count = cb.a[1];
    for (uint32_t k = 0; k < count; k++) {
        const uint32_t info = cells + 4 * k;
        Entry e;
        const uint32_t name = call_guest(c, fn(InfoGetName), {info});
        for (uint32_t p = name; p && c.mem->read<uint32_t>(p); p += 4)
            e.name.push_back((char32_t)c.mem->read<uint32_t>(p));
        e.id = call_guest(c, fn(InfoGetSavegameId), {info});
        e.size = call_guest(c, fn(InfoGetSize), {info});
        e.pid = call_guest(c, fn(InfoGetProductId), {info});
        g_listed.push_back(e);
    }
    shim_return(c, 0);
}

// A listener object: [vtable] -> [slot 0].
uint32_t make_listener(GuestFn f) {
    const uint32_t vtable = halloc(4), object = halloc(16);
    g_mem->write<uint32_t>(vtable, register_host_function(f, "test listener"));
    g_mem->write<uint32_t>(object, vtable);
    return object;
}
// A listener that only records (for the client's list, whose entries are objects).
uint32_t l_record_raw() {
    static const uint32_t l = make_listener(record);
    return l;
}

uint32_t put_wstring(const std::u32string& s) {
    const uint32_t a = halloc(4 * (uint32_t)s.size() + 4);
    for (size_t k = 0; k <= s.size(); k++)
        g_mem->write<uint32_t>(a + 4 * (uint32_t)k, k < s.size() ? (uint32_t)s[k] : 0);
    return a;
}

// One request: returns the single callback it must produce.
Callback request(Context& c, uint32_t listener, int f, std::vector<uint32_t> args, const char* what) {
    const size_t before = g_calls.size();
    switch (args.size()) {
    case 3: call_guest(c, fn(f), {args[0], args[1], args[2]}); break;
    case 4: call_guest(c, fn(f), {args[0], args[1], args[2], args[3]}); break;
    case 5: call_guest(c, fn(f), {args[0], args[1], args[2], args[3], args[4]}); break;
    case 6: call_guest(c, fn(f), {args[0], args[1], args[2], args[3], args[4], args[5]}); break;
    default: fatal("bad arity");
    }
    check(g_calls.size() == before + 1, std::string(what) + ": exactly one callback");
    if (g_calls.size() != before + 1) return {};
    const Callback cb = g_calls.back();
    check(cb.listener == listener, std::string(what) + ": callback on the listener passed");
    check(cb.req == args[1], std::string(what) + ": callback carries the request id");
    return cb;
}

// The game's SaveGameWrite: list, reuse the id with this name or take the
// smallest free id >= 1, get a writer, set the name, write in pieces, close.
uint32_t save(Context& c, uint32_t pid, const std::u32string& name, const std::vector<uint8_t>& data,
              uint32_t l_list, uint32_t l_writer, uint32_t l_write) {
    const uint32_t s = orbit::kSession;
    request(c, l_list, GetSavegameList, {s, call_guest(c, fn(GetRequestUniqueId), {s}), l_list, pid}, "list");
    uint32_t id = 0, free_id = 1;
    for (bool again = true; again;) {
        again = false;
        for (const Entry& e : g_listed)
            if (e.id == free_id) free_id++, again = true;
    }
    for (const Entry& e : g_listed)
        if (e.name == name) id = e.id;
    if (!id) id = free_id;
    const Callback w = request(c, l_writer, GetSavegameWriter,
                               {s, call_guest(c, fn(GetRequestUniqueId), {s}), l_writer, pid, id, 1},
                               "get writer");
    check(w.a[0] == 0 && w.a[1] != 0, "writer: status 0 and a handle");
    check(call_guest(c, fn(WriterSetName), {w.a[1], put_wstring(name)}) & 0xff, "set name");
    const uint32_t buf = halloc((uint32_t)data.size() + 1);
    std::memcpy(g_mem->ptr(buf), data.data(), data.size());
    // Write in pieces to check appending; the game asks for the rest each time.
    uint32_t done = 0;
    while (done < data.size()) {
        const uint32_t piece = std::min<uint32_t>((uint32_t)data.size() - done, 1000);
        const Callback cb = request(c, l_write, WriterWrite,
                                    {w.a[1], call_guest(c, fn(GetRequestUniqueId), {s}), l_write,
                                     buf + done, piece},
                                    "write");
        check(cb.a[0] == piece, "write reports the bytes written");
        if (cb.a[0] == 0) break;
        done += cb.a[0];
    }
    call_guest(c, fn(WriterClose), {w.a[1], 1});
    return id;
}

std::vector<uint8_t> load(Context& c, uint32_t pid, const std::u32string& name, bool* found,
                          uint32_t l_list, uint32_t l_reader, uint32_t l_read) {
    const uint32_t s = orbit::kSession;
    request(c, l_list, GetSavegameList, {s, call_guest(c, fn(GetRequestUniqueId), {s}), l_list, pid}, "list");
    *found = false;
    uint32_t id = 0, size = 0;
    for (const Entry& e : g_listed)
        if (e.name == name) id = e.id, size = e.size, *found = true;
    if (!*found) return {};
    const Callback r = request(c, l_reader, GetSavegameReader,
                               {s, call_guest(c, fn(GetRequestUniqueId), {s}), l_reader, pid, id},
                               "get reader");
    check(r.a[0] == 0 && r.a[1] != 0, "reader: status 0 and a handle");
    const uint32_t buf = halloc(size + 1);
    uint32_t remaining = size;
    while (remaining) {
        const Callback cb = request(c, l_read, ReaderRead,
                                    {r.a[1], call_guest(c, fn(GetRequestUniqueId), {s}), l_read,
                                     size - remaining, buf + (size - remaining), remaining},
                                    "read");
        if (cb.a[0] == 0) break;
        remaining -= cb.a[0];
    }
    check(remaining == 0, "read the whole save");
    call_guest(c, fn(ReaderClose), {r.a[1]});
    return std::vector<uint8_t>(g_mem->ptr(buf), g_mem->ptr(buf) + size);
}

}  // namespace

int main() {
    char dir[] = "/tmp/scimitar-orbit-test-XXXXXX";
    if (!mkdtemp(dir)) return 2;
    orbit::set_save_directory(std::string(dir) + "/saves");

    GuestMemory mem;
    g_mem = &mem;
    mem.commit(kStackLo, kStackTop - kStackLo);
    mem.commit(kHeap, kHeapSize);
    Context c;
    c.mem = &mem;
    c.esp = kStackTop - 0x100;

    const uint32_t s = orbit::kSession;
    const uint32_t l_login = make_listener(record), l_server = make_listener(record),
                   l_reader = make_listener(record), l_writer = make_listener(record),
                   l_list = make_listener(record_list), l_remove = make_listener(record),
                   l_traffic = make_listener(record), l_read = make_listener(record),
                   l_write = make_listener(record);

    // Session and startup.
    check(call_guest(c, fn(GetSession), {}) == s, "GetSession returns the session");
    check(call_guest(c, fn(GetFakeSession), {0, 0}) == s, "GetFakeSession returns the session");
    check((call_guest(c, fn(StartLauncher), {s, 4, 0, 0, 0}) & 0xff) == 0,
          "StartLauncher returns false (the game keeps running)");
    const uint32_t r1 = call_guest(c, fn(GetRequestUniqueId), {s});
    const uint32_t r2 = call_guest(c, fn(GetRequestUniqueId), {s});
    check(r1 != 0 && r2 != r1 && r1 != 0xffffffffu, "request ids are unique");
    const Callback login = request(c, l_login, GetLoginDetails, {s, r2, l_login}, "login");
    check(login.a[0] && login.a[1] && login.a[2], "login strings are all non-null");
    {
        const size_t before = g_calls.size();
        call_guest(c, fn(GetOrbitServer), {s, call_guest(c, fn(GetRequestUniqueId), {s}), l_server, 4, 7});
        check(g_calls.size() == before, "GetOrbitServer is never completed");
    }
    request(c, l_traffic, GetNetworkTraffic, {s, call_guest(c, fn(GetRequestUniqueId), {s}), l_traffic},
            "traffic");
    const uint32_t key = put_wstring(U"Some.Key");
    check(call_guest(c, fn(GetLocText), {s, key, 0}) == key, "GetLocText returns the key");
    call_guest(c, fn(Update), {s});

    // Nothing saved yet.
    bool found = true;
    load(c, 4, U"ACII 0", &found, l_list, l_reader, l_read);
    check(!found && g_listed.empty(), "no saves at first");

    // Two slots and the options blob ("ACII -1").
    std::vector<uint8_t> a(5000), b(17), opts(300);
    for (size_t k = 0; k < a.size(); k++) a[k] = (uint8_t)(k * 7 + 1);
    for (size_t k = 0; k < b.size(); k++) b[k] = (uint8_t)(255 - k);
    for (size_t k = 0; k < opts.size(); k++) opts[k] = (uint8_t)k;
    const uint32_t id0 = save(c, 4, U"ACII 0", a, l_list, l_writer, l_write);
    const uint32_t id1 = save(c, 4, U"ACII 1", b, l_list, l_writer, l_write);
    const uint32_t idopt = save(c, 4, U"ACII -1", opts, l_list, l_writer, l_write);
    check(id0 == 1 && id1 == 2 && idopt == 3, "new saves take the smallest free ids from 1");
    check(load(c, 4, U"ACII 0", &found, l_list, l_reader, l_read) == a && found, "slot 0 reads back");
    check(load(c, 4, U"ACII 1", &found, l_list, l_reader, l_read) == b && found, "slot 1 reads back");
    check(load(c, 4, U"ACII -1", &found, l_list, l_reader, l_read) == opts && found, "options read back");
    check(g_listed.size() == 3 && g_listed[0].pid == 4 && g_listed[0].size == a.size(),
          "the list reports product id and size");

    // Overwriting keeps the id and replaces the data.
    std::vector<uint8_t> a2(1234, 0x5a);
    check(save(c, 4, U"ACII 0", a2, l_list, l_writer, l_write) == id0, "overwrite reuses the id");
    check(load(c, 4, U"ACII 0", &found, l_list, l_reader, l_read) == a2, "overwritten data reads back");

    // Another product id does not see these saves.
    load(c, 5, U"ACII 0", &found, l_list, l_reader, l_read);
    check(!found && g_listed.empty(), "saves are per product id");

    // A missing save gives a failing reader status.
    {
        const Callback r = request(c, l_reader, GetSavegameReader,
                                   {s, call_guest(c, fn(GetRequestUniqueId), {s}), l_reader, 4, 99},
                                   "missing reader");
        check(r.a[0] != 0 && r.a[1] == 0, "reading a missing save fails");
    }

    // Removing takes effect in the next list (the game loops until it does).
    const Callback rm = request(c, l_remove, RemoveSavegame,
                                {s, call_guest(c, fn(GetRequestUniqueId), {s}), l_remove, 4, id1},
                                "remove");
    check(rm.a[0] == 0, "remove succeeds");
    load(c, 4, U"ACII 1", &found, l_list, l_reader, l_read);
    check(!found && g_listed.size() == 2, "a removed save is gone from the list");
    check(save(c, 4, U"ACII 2", b, l_list, l_writer, l_write) == id1, "its id is free again");

    // Saves are files: a new process (simulated by listing again from disk) sees them.
    orbit::set_save_directory(std::string(dir) + "/saves");
    check(load(c, 4, U"ACII -1", &found, l_list, l_reader, l_read) == opts && found,
          "saves persist on disk");
    check(c.esp == kStackTop - 0x100, "guest stack balanced");

    // The C++ client (Brotherhood): the same requests through the methods the
    // game imports, with client objects for readers, writers and listed infos.
    {
        auto method = [](const char* name) {
            const ImportShim* shim = find_import_shim(name);
            if (!shim) fatal("no shim for %s", name);
            return register_host_function(shim->fn, name);
        };
        const uint32_t ctor = method("__ZN2mg11orbitclient11OrbitClientC1Ev"),
                       uid = method("__ZN2mg11orbitclient11OrbitClient18GetRequestUniqueIdEv"),
                       list = method("__ZN2mg11orbitclient11OrbitClient15GetSavegameListEjPNS0_24IGetSavegameListListenerEj"),
                       writer = method("__ZN2mg11orbitclient11OrbitClient17GetSavegameWriterEjPNS0_26IGetSavegameWriterListenerEjjb"),
                       reader = method("__ZN2mg11orbitclient11OrbitClient17GetSavegameReaderEjPNS0_26IGetSavegameReaderListenerEjj"),
                       remove = method("__ZN2mg11orbitclient11OrbitClient14RemoveSavegameEjPNS0_23IRemoveSavegameListenerEjj"),
                       set_name = method("__ZN2mg11orbitclient14SavegameWriter7SetNameEPw"),
                       write = method("__ZN2mg11orbitclient14SavegameWriter5WriteEjPNS0_22ISavegameWriteListenerEPvj"),
                       wclose = method("__ZN2mg11orbitclient14SavegameWriter5CloseEb"),
                       read = method("__ZN2mg11orbitclient14SavegameReader4ReadEjPNS0_21ISavegameReadListenerEjPvj"),
                       rclose = method("__ZN2mg11orbitclient14SavegameReader5CloseEv"),
                       info_name = method("__ZN2mg11orbitclient12SavegameInfo7GetNameEv"),
                       info_id = method("__ZN2mg11orbitclient12SavegameInfo13GetSavegameIdEv"),
                       info_size = method("__ZN2mg11orbitclient12SavegameInfo7GetSizeEv"),
                       err = method("__ZN2mg11orbitclient11OrbitClient23GetInstallationErrorNumEv");
        const uint32_t client = halloc(4);
        call_guest(c, ctor, {client});
        check(call_guest(c, err, {}) == 0, "client: the launcher library counts as installed");
        const uint32_t pid = 7;
        auto last = [&](size_t before, const char* what) {
            check(g_calls.size() == before + 1, std::string("client ") + what + ": one callback");
            return g_calls.empty() ? Callback{} : g_calls.back();
        };
        // Write a save through a writer object.
        size_t before = g_calls.size();
        uint32_t req = call_guest(c, uid, {client});
        call_guest(c, writer, {client, req, l_writer, pid, 1, 1});
        const Callback w = last(before, "writer");
        check(w.req == req && w.a[0] == 0 && w.a[1], "client writer: status 0 and an object");
        check(call_guest(c, set_name, {w.a[1], put_wstring(U"ACB 0")}) & 0xff, "client: set name");
        std::vector<uint8_t> data(3000);
        for (size_t k = 0; k < data.size(); k++) data[k] = (uint8_t)(k * 13 + 5);
        const uint32_t buf = halloc((uint32_t)data.size());
        std::memcpy(g_mem->ptr(buf), data.data(), data.size());
        before = g_calls.size();
        call_guest(c, write, {w.a[1], call_guest(c, uid, {client}), l_write, buf, (uint32_t)data.size()});
        check(last(before, "write").a[0] == data.size(), "client: write reports the bytes");
        call_guest(c, wclose, {w.a[1], 1});
        // List: an array (count in front, as new[] leaves it) of info objects.
        before = g_calls.size();
        call_guest(c, list, {client, call_guest(c, uid, {client}), l_record_raw(), pid});
        const Callback l = last(before, "list");
        check(l.a[1] == 1 && g_mem->read<uint32_t>(l.a[0] - 4) == 1, "client list: one save, counted in front");
        const uint32_t info = g_mem->read<uint32_t>(l.a[0]);
        std::u32string name;
        for (uint32_t p = call_guest(c, info_name, {info}); p && g_mem->read<uint32_t>(p); p += 4)
            name.push_back((char32_t)g_mem->read<uint32_t>(p));
        check(name == U"ACB 0" && call_guest(c, info_id, {info}) == 1 &&
                  call_guest(c, info_size, {info}) == data.size(),
              "client info: name, id and size, after the callback too");
        // Read it back through a reader object.
        before = g_calls.size();
        call_guest(c, reader, {client, call_guest(c, uid, {client}), l_reader, pid, 1});
        const Callback r = last(before, "reader");
        check(r.a[0] == 0 && r.a[1], "client reader: status 0 and an object");
        const uint32_t dst = halloc((uint32_t)data.size());
        before = g_calls.size();
        call_guest(c, read, {r.a[1], call_guest(c, uid, {client}), l_read, 0, dst, (uint32_t)data.size()});
        check(last(before, "read").a[0] == data.size() &&
                  std::memcmp(g_mem->ptr(dst), data.data(), data.size()) == 0,
              "client: the save reads back");
        call_guest(c, rclose, {r.a[1]});
        before = g_calls.size();
        call_guest(c, remove, {client, call_guest(c, uid, {client}), l_remove, pid, 1});
        check(last(before, "remove").a[0] == 0, "client: remove succeeds");
        check(c.esp == kStackTop - 0x100, "client: guest stack balanced");
    }

    std::string cmd = std::string("rm -rf ") + dir;
    std::system(cmd.c_str());
    std::printf("orbit: %s\n", failures ? "FAILED" : "all checks passed");
    return failures ? 1 : 0;
}
