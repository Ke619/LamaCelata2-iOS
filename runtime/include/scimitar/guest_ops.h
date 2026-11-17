#pragma once
// Helpers used by generated code. `M` is the host base of guest memory.
#include <cstdint>
#include <cstring>

#include "scimitar/context.h"
#include "scimitar/guest_memory.h"

namespace scimitar::ops {

inline uint8_t rd8(const uint8_t* M, uint32_t a) { return M[a]; }
inline uint16_t rd16(const uint8_t* M, uint32_t a) {
    uint16_t v;
    std::memcpy(&v, M + a, 2);
    return v;
}
inline uint32_t rd32(const uint8_t* M, uint32_t a) {
    uint32_t v;
    std::memcpy(&v, M + a, 4);
    return v;
}
inline uint64_t rd64(const uint8_t* M, uint32_t a) {
    uint64_t v;
    std::memcpy(&v, M + a, 8);
    return v;
}
inline F80 rd80(const uint8_t* M, uint32_t a) {
    F80 v;
    std::memcpy(&v.mant, M + a, 8);
    std::memcpy(&v.se, M + a + 8, 2);
    return v;
}
inline void wr8(uint8_t* M, uint32_t a, uint8_t v) { M[a] = v; }
inline void wr16(uint8_t* M, uint32_t a, uint16_t v) { std::memcpy(M + a, &v, 2); }
inline void wr32(uint8_t* M, uint32_t a, uint32_t v) { std::memcpy(M + a, &v, 4); }
inline void wr64(uint8_t* M, uint32_t a, uint64_t v) { std::memcpy(M + a, &v, 8); }
inline void wr80(uint8_t* M, uint32_t a, F80 v) {
    std::memcpy(M + a, &v.mant, 8);
    std::memcpy(M + a + 8, &v.se, 2);
}

// Atomic read-modify-write on guest memory (lock prefix, xchg with memory),
// sequentially consistent like x86 locked operations. Misaligned addresses
// (which x86 still handles atomically) fall back to plain accesses.
template <typename T> inline T atomic_rmw(uint8_t* M, uint32_t a, T v, int op) {
    T* p = reinterpret_cast<T*>(M + a);
    if (a % sizeof(T) == 0) {
        switch (op) {
        case 0: return __atomic_fetch_add(p, v, __ATOMIC_SEQ_CST);
        case 1: return __atomic_fetch_sub(p, v, __ATOMIC_SEQ_CST);
        case 2: return __atomic_fetch_and(p, v, __ATOMIC_SEQ_CST);
        case 3: return __atomic_fetch_or(p, v, __ATOMIC_SEQ_CST);
        case 4: return __atomic_fetch_xor(p, v, __ATOMIC_SEQ_CST);
        default: return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST);
        }
    }
    T old;
    std::memcpy(&old, M + a, sizeof(T));
    T r = op == 0 ? T(old + v) : op == 1 ? T(old - v) : op == 2 ? T(old & v)
        : op == 3 ? T(old | v) : op == 4 ? T(old ^ v) : v;
    std::memcpy(M + a, &r, sizeof(T));
    return old;
}
enum : int { kAtomicAdd = 0, kAtomicSub, kAtomicAnd, kAtomicOr, kAtomicXor, kAtomicXchg };

// lock cmpxchg: stores `desired` if the word holds `expected`; returns the old value.
template <typename T> inline T atomic_cas(uint8_t* M, uint32_t a, T expected, T desired) {
    T* p = reinterpret_cast<T*>(M + a);
    if (a % sizeof(T) == 0) {
        __atomic_compare_exchange_n(p, &expected, desired, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
        return expected;
    }
    T old;
    std::memcpy(&old, M + a, sizeof(T));
    if (old == expected) std::memcpy(M + a, &desired, sizeof(T));
    return old;
}

// x86 PF: set when the low byte of the result has an even number of 1 bits.
inline bool parity(uint32_t v) { return (__builtin_popcount(v & 0xff) & 1) == 0; }

}  // namespace scimitar::ops
