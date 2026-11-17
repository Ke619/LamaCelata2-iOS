#pragma once
// COM objects for the guest (Direct3D, DirectInput, DirectSound): an object is
// guest memory holding a vtable pointer (and refcount), its vtable a guest
// array of host function addresses, its native state a C++ object found by the
// guest address. Methods are stdcall with `this` as argument 0.
#include <map>
#include <mutex>
#include <string>
#include <type_traits>
#include <vector>

#include "win32.h"

namespace scimitar::win32::com {

struct Method {
    const char* name;  // "IDirect3DDevice9::Clear"
    GuestFn fn;        // nullptr: not implemented (stops the run with the name)
};

// Builds a guest vtable; methods without an implementation get a stub that
// stops the run naming the method.
uint32_t make_vtable(const char* iface, const std::vector<Method>& methods);

// The native side of guest objects of type T, by guest address.
struct Native {
    virtual ~Native() = default;
    uint32_t refs = 1;
    uint32_t guest = 0, slot = 0;  // the guest object; its index in the lookup table
    uint32_t kind = 0;             // T::kKind for types that declare one (as<T> without dynamic_cast)
};
uint32_t create(uint32_t vtable, Native* native, uint32_t size = 8);
Native* native(uint32_t object);
// The native object as T, or null. Types hot enough to matter declare a
// nonzero `static constexpr uint32_t kKind` and set `kind` to it: the check is
// then one compare (dynamic_cast compares type names on macOS); the rest use
// dynamic_cast. Types with a kKind have no subclasses.
template <typename T, typename = void> struct kind_of { static constexpr uint32_t value = 0; };
template <typename T> struct kind_of<T, std::void_t<decltype(T::kKind)>> { static constexpr uint32_t value = T::kKind; };
template <typename T> T* as(uint32_t object) {
    Native* n = native(object);
    if constexpr (kind_of<T>::value != 0) return n && n->kind == kind_of<T>::value ? static_cast<T*>(n) : nullptr;
    else return dynamic_cast<T*>(n);
}
void destroy(uint32_t object);

// IUnknown for objects made with create(): AddRef/Release on Native::refs
// (an object whose count reaches zero is destroyed), QueryInterface returns
// the object itself for any interface.
void QueryInterface(Context& c);
void AddRef(Context& c);
void Release(Context& c);

constexpr uint32_t S_OK = 0, E_FAIL = 0x80004005u, E_NOINTERFACE = 0x80004002u,
                   E_OUTOFMEMORY = 0x8007000eu, E_INVALIDARG = 0x80070057u;

}  // namespace scimitar::win32::com
