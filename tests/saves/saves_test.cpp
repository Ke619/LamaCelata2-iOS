// Saves through the game's own code. scimitar::OrbitClient (Initialize,
// IsOrbitReady, Setup, SaveGameWrite/Read/Delete) and the mg::orbitclient
// wrappers under it run recompiled, with the launcher library replaced by the
// local one (games/common/overrides.cpp: PrivLoadLibrary installs it). Checks that
// the game starts its Orbit layer offline, that what it saves reads back
// identically, that slots and the options blob are separate, that
// overwriting and deleting work, and that the saves are files on disk.
//
//   saves_test GAME.app
//
// Needs the game installed locally; nothing from it is stored in the repo.

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "scimitar/guest_heap.h"
#include "scimitar/image_loader.h"
#include "scimitar/imports.h"
#include "scimitar/orbit.h"
#include "scimitar/runtime.h"

using namespace scimitar;

namespace {

constexpr uint32_t kStackLo = 0x50000000, kStackTop = 0x50400000;
constexpr uint32_t kSentinel = 0xdead0000;

// The engine reaches its global memory manager through this pointer slot
// (see tests/forge): *(*(*slot)) is the manager object.
constexpr uint32_t kManagerSlot = 0x0278f944;
constexpr int kVtblSlots = 32;

const char* kStaticInits[] = {
    "__GLOBAL__I__ZN8scimitar11OrbitClient11ms_InstanceE",
    "__GLOBAL__I__ZN2mg11orbitclient167_GLOBAL__N__Volumes_Work_Projects_AC2_assassin_mac_main_"
    "projects_mac_engine_.._.._.._extern_orbit_mac_liborbit_src_mg_orbitclient_OrbitClientBase.cpp_"
    "00000000_2A92481D15GetErrLocStringEPKw",
};

int failures = 0;
void check(bool ok, const std::string& what) {
    if (!ok) failures++;
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
}

// ---- the engine's memory manager, natively on the guest heap ----

// vtable +0x8: Alloc(this, kind, size, MemInterface*, ...)
void manager_alloc(Context& c) { shim_return(c, heap::alloc(*c.mem, arg32(c, 2))); }
// vtable +0xc: AllocAligned(this, kind, size, alignment, MemInterface*, ...)
void manager_alloc_aligned(Context& c) { shim_return(c, heap::alloc(*c.mem, arg32(c, 2), arg32(c, 3))); }
// vtable +0x18: Free(this, kind, pointer, ...)
void manager_free(Context& c) {
    heap::release(*c.mem, arg32(c, 2));
    shim_return(c, 0);
}
template <int Slot>
void trap(Context& c) {
    fatal("unexpected memory manager call: vtable slot %d from %08x", Slot, c.mem->read<uint32_t>(c.esp));
}
template <int... Slots>
void register_traps(uint32_t* out, std::integer_sequence<int, Slots...>) {
    ((out[Slots] = register_host_function(trap<Slots>, "trap")), ...);
}

class Game {
public:
    explicit Game(const std::string& binary) {
        image_ = load_image(mem_, binary);
        mem_.commit(kStackLo, kStackTop - kStackLo);
        // Memory manager: slot -> global -> cell -> manager object -> vtable.
        uint32_t vt[kVtblSlots];
        register_traps(vt, std::make_integer_sequence<int, kVtblSlots>());
        vt[2] = register_host_function(manager_alloc, "manager_alloc");
        vt[3] = register_host_function(manager_alloc_aligned, "manager_alloc_aligned");
        vt[6] = register_host_function(manager_free, "manager_free");
        const uint32_t vtable = heap::alloc(mem_, 4 * kVtblSlots), manager = heap::alloc(mem_, 16),
                       cell = heap::alloc(mem_, 16);
        for (int k = 0; k < kVtblSlots; k++) mem_.write<uint32_t>(vtable + 4 * k, vt[k]);
        mem_.write<uint32_t>(manager, vtable);
        mem_.write<uint32_t>(cell, manager);
        mem_.write<uint32_t>(mem_.read<uint32_t>(kManagerSlot), cell);
    }

    uint32_t sym(const char* name) {
        const uint32_t a = image_.image->symbol(name);
        if (!a) fatal("symbol %s not found", name);
        return a;
    }

    // Calls a game function (cdecl) on a fresh stack; returns eax.
    uint32_t call(uint32_t fn, std::vector<uint32_t> args) {
        const FuncEntry* f = find_function(fn);
        if (!f) fatal("%08x was not recompiled", fn);
        Context c;
        c.mem = &mem_;
        c.esp = kStackTop - 0x1000;
        c.ebx = 0x11111111, c.esi = 0x22222222, c.edi = 0x33333333, c.ebp = 0x44444444;
        mem_.write<uint32_t>(c.esp, kSentinel);
        for (size_t i = 0; i < args.size(); i++) mem_.write<uint32_t>(c.esp + 4 + 4 * (uint32_t)i, args[i]);
        const uint32_t esp = c.esp;
        f->fn(c);
        if (c.esp != esp + 4 || c.ebx != 0x11111111 || c.esi != 0x22222222 || c.edi != 0x33333333 ||
            c.ebp != 0x44444444)
            fatal("%s: stack or callee-saved registers not restored", f->name);
        return c.eax;
    }
    uint32_t call(const char* name, std::vector<uint32_t> args) { return call(sym(name), std::move(args)); }

    GuestMemory& mem() { return mem_; }

private:
    GuestMemory mem_;
    LoadedImage image_;
};

std::vector<uint8_t> pattern(size_t n, uint8_t seed) {
    std::vector<uint8_t> v(n);
    for (size_t k = 0; k < n; k++) v[k] = (uint8_t)(seed + k * 31 + (k >> 8));
    return v;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s GAME.app\n", argv[0]);
        return 2;
    }
    char dir[] = "/tmp/scimitar-saves-test-XXXXXX";
    if (!mkdtemp(dir)) return 2;
    const std::string saves = std::string(dir) + "/saves";
    orbit::set_save_directory(saves);

    Game g(std::string(argv[1]) + "/Contents/MacOS/scimitar_sf");
    for (const char* init : kStaticInits) g.call(init, {});

    // Startup as scimitar::main does it, then Setup as MainMac does.
    g.call("__ZN8scimitar11OrbitClient10InitializeEv", {});
    const uint32_t client = g.call("__ZN8scimitar11OrbitClient11GetInstanceEv", {});
    check(client != 0, "OrbitClient::Initialize created the client");
    const bool ready = g.call("__ZN8scimitar11OrbitClient12IsOrbitReadyEv", {client}) & 0xff;
    check(ready, "IsOrbitReady: the game would keep running (no 'launcher not found' exit)");
    g.call("__ZN8scimitar11OrbitClient5SetupEv", {client});
    check(g.mem().read<uint8_t>(client + 0x62) == 1, "Setup: logged in (offline)");

    // The game's own save functions.
    auto write = [&](uint32_t slot, const std::vector<uint8_t>& data) {
        const uint32_t buf = heap::alloc(g.mem(), (uint32_t)data.size() + 1);
        std::memcpy(g.mem().ptr(buf), data.data(), data.size());
        g.call("__ZN8scimitar11OrbitClient13SaveGameWriteEmPhm", {client, slot, buf, (uint32_t)data.size()});
        heap::release(g.mem(), buf);
    };
    auto read = [&](uint32_t slot, bool* ok) {
        const uint32_t array = heap::alloc(g.mem(), 16);  // BigArray<unsigned char>: data, size, ...
        std::memset(g.mem().ptr(array), 0, 16);
        *ok = g.call("__ZN8scimitar11OrbitClient12SaveGameReadEmRNS_8BigArrayIhNS_21DefaultAllocatorTraitEEE",
                     {client, slot, array}) & 0xff;
        const uint32_t data = g.mem().read<uint32_t>(array), size = g.mem().read<uint32_t>(array + 4);
        std::vector<uint8_t> out;
        if (data) out.assign(g.mem().ptr(data), g.mem().ptr(data) + size);
        return out;
    };

    bool ok = true;
    read(0, &ok);
    check(!ok, "reading an empty slot fails");

    const auto s0 = pattern(100000, 1), s1 = pattern(7777, 2), opts = pattern(512, 3);
    write(0, s0);
    write(1, s1);
    write(0xffffffffu, opts);  // the options blob ("ACII -1")
    check(read(0, &ok) == s0 && ok, "slot 0 reads back what the game saved (100,000 bytes)");
    check(read(1, &ok) == s1 && ok, "slot 1 reads back");
    check(read(0xffffffffu, &ok) == opts && ok, "the options blob reads back");

    const auto s0b = pattern(4321, 9);
    write(0, s0b);
    check(read(0, &ok) == s0b && ok, "overwriting slot 0 replaces it");
    check(read(1, &ok) == s1 && ok, "slot 1 is untouched");

    g.call("__ZN8scimitar11OrbitClient14SaveGameDeleteEm", {client, 1});
    read(1, &ok);
    check(!ok, "a deleted slot is gone");
    check(read(0, &ok) == s0b && ok, "the other slot survives the delete");

    // Files on disk: slot 0 and the options blob.
    int files = 0;
    if (FILE* p = popen(("ls '" + saves + "' | grep -c '\\.sav$'").c_str(), "r")) {
        if (std::fscanf(p, "%d", &files) != 1) files = -1;
        pclose(p);
    }
    check(files == 2, "two save files on disk (slot 0 and options)");

    std::system((std::string("rm -rf ") + dir).c_str());
    std::printf("saves: %s\n", failures ? "FAILED" : "all checks passed");
    return failures ? 1 : 0;
}
