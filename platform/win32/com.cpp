// COM objects in guest memory (com.h).
#include "com.h"

#include "scimitar/guest_heap.h"

#include <array>
#include <typeinfo>
#include <atomic>
#include <utility>

namespace scimitar::win32::com {

namespace {

// Stubs for methods without an implementation, each knowing its name.
constexpr size_t kStubs = 2048;
const char* g_stub_names[kStubs];
size_t g_stub_count = 0;
std::mutex g_lock;

template <size_t N> void unimplemented(Context& c) {
    (void)c;
    fatal("%s is not implemented", g_stub_names[N]);
}

template <size_t... N> constexpr std::array<GuestFn, sizeof...(N)> stub_table(std::index_sequence<N...>) {
    return {&unimplemented<N>...};
}
const std::array<GuestFn, kStubs> g_stubs = stub_table(std::make_index_sequence<kStubs>{});

std::map<uint32_t, Native*> g_objects;

}  // namespace

uint32_t make_vtable(const char* iface, const std::vector<Method>& methods) {
    const uint32_t vt = galloc(4 * (uint32_t)methods.size());
    std::lock_guard<std::mutex> l(g_lock);
    for (size_t k = 0; k < methods.size(); k++) {
        const std::string name = std::string(iface) + "::" + methods[k].name;
        const char* stable = strdup(name.c_str());
        GuestFn fn = methods[k].fn;
        if (!fn) {
            if (g_stub_count >= kStubs) fatal("too many unimplemented COM methods");
            g_stub_names[g_stub_count] = stable;
            fn = g_stubs[g_stub_count++];
        }
        wr(vt + 4 * (uint32_t)k, register_host_function(fn, stable));
    }
    return vt;
}

// Objects are found by their guest address on every method call: the index
// of their entry in a lock-free table sits in the object's second word
// (guest code never reads our objects' fields).
constexpr uint32_t kSlots = 1u << 20;
std::atomic<Native*> g_slots[kSlots];
std::vector<uint32_t> g_free_slots;
uint32_t g_next_slot = 1;

uint32_t create(uint32_t vtable, Native* n, uint32_t size) {
    size = size < 8 ? 8 : size;
    const uint32_t obj = galloc(size);
    std::memset(gptr(obj), 0, size);
    wr(obj, vtable);
    std::lock_guard<std::mutex> l(g_lock);
    uint32_t slot;
    if (!g_free_slots.empty()) slot = g_free_slots.back(), g_free_slots.pop_back();
    else slot = g_next_slot++;
    if (slot >= kSlots) fatal("too many COM objects");
    n->guest = obj, n->slot = slot;
    g_slots[slot].store(n, std::memory_order_release);
    wr(obj + 4, slot);
    g_objects[obj] = n;
    return obj;
}

Native* native(uint32_t object) {
    if (!object || !heap::contains(object)) return nullptr;  // every object is on the guest heap
    const uint32_t slot = rd(object + 4);
    if (slot && slot < kSlots) {
        Native* n = g_slots[slot].load(std::memory_order_acquire);
        if (n && n->guest == object) return n;
    }
    std::lock_guard<std::mutex> l(g_lock);
    auto it = g_objects.find(object);
    return it == g_objects.end() ? nullptr : it->second;
}

void destroy(uint32_t object) {
    Native* n = nullptr;
    {
        std::lock_guard<std::mutex> l(g_lock);
        auto it = g_objects.find(object);
        if (it == g_objects.end()) return;
        n = it->second;
        g_objects.erase(it);
        g_slots[n->slot].store(nullptr, std::memory_order_release);
        g_free_slots.push_back(n->slot);
    }
    delete n;
    gfree(object);
}

void QueryInterface(Context& c) {
    const uint32_t self = arg(c, 0), out = arg(c, 2);
    if (out) wr(out, self);
    if (Native* n = native(self)) n->refs++;
    ret(c, S_OK, 3);
}

void AddRef(Context& c) {
    Native* n = native(arg(c, 0));
    ret(c, n ? ++n->refs : 1, 1);
}

void Release(Context& c) {
    const uint32_t self = arg(c, 0);
    Native* n = native(self);
    if (!n) return ret(c, 0, 1);
    const uint32_t r = n->refs ? --n->refs : 0;
    if (!r) {
        if (tracing()) trace("Release(%08x) from %08x: destroyed (%s)", self, rd(c.esp), typeid(*n).name());
        destroy(self);
    }
    ret(c, r, 1);
}

}  // namespace scimitar::win32::com
