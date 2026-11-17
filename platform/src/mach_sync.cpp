// Mach semaphores, OSAtomic and OSSpinLock, and time imports.
//
// OSAtomic* are the engine's main synchronisation primitive (13,000 call
// sites, mostly reference counts): they become host atomics, sequentially
// consistent, so the ordering the x86 code relied on holds on arm64 too.
#include <mach/mach_time.h>
#include <sys/time.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

#include "scimitar/imports.h"
#include "scimitar/runtime.h"

namespace scimitar::imports {

namespace {

template <typename T> std::atomic_ref<T> at(Context& c, uint32_t a) {
    return std::atomic_ref<T>(*reinterpret_cast<T*>(c.mem->ptr(a)));
}

// ---- semaphores: guest semaphore_t values are indices into this table ----
struct Semaphore {
    std::mutex m;
    std::condition_variable cv;
    int64_t count = 0;
};
std::mutex g_sem_lock;
std::map<uint32_t, std::unique_ptr<Semaphore>> g_sems;
uint32_t g_next_sem = 0x5e000001;

Semaphore* sem(uint32_t s) {
    std::lock_guard<std::mutex> l(g_sem_lock);
    auto it = g_sems.find(s);
    return it == g_sems.end() ? nullptr : it->second.get();
}

constexpr uint32_t kKernSuccess = 0, kKernInvalidArgument = 4, kKernOperationTimedOut = 49;

}  // namespace

// int32_t OSAtomicAdd32(int32_t amount, volatile int32_t* value): the new value
void imp_OSAtomicAdd32(Context& c) {
    const int32_t v = (int32_t)arg32(c, 0);
    shim_return(c, (uint32_t)(at<int32_t>(c, arg32(c, 1)).fetch_add(v) + v));
}
// int64_t OSAtomicAdd64(int64_t amount, volatile int64_t* value)
void imp_OSAtomicAdd64(Context& c) {
    const int64_t v = (int64_t)arg64(c, 0);
    shim_return64(c, (uint64_t)(at<int64_t>(c, arg32(c, 2)).fetch_add(v) + v));
}
// bool OSAtomicCompareAndSwap32Barrier(int32_t old, int32_t new, volatile int32_t* value)
void imp_OSAtomicCompareAndSwap32Barrier(Context& c) {
    int32_t expected = (int32_t)arg32(c, 0);
    shim_return(c, at<int32_t>(c, arg32(c, 2)).compare_exchange_strong(expected, (int32_t)arg32(c, 1)) ? 1 : 0);
}

// OSSpinLock: an int, 0 unlocked.
void imp_OSSpinLockLock(Context& c) {
    auto l = at<int32_t>(c, arg32(c, 0));
    for (int spins = 0;; spins++) {
        int32_t expected = 0;
        if (l.compare_exchange_weak(expected, 1, std::memory_order_acquire)) break;
        if (spins > 64) std::this_thread::yield();
    }
    shim_return(c, 0);
}
void imp_OSSpinLockTry(Context& c) {
    int32_t expected = 0;
    shim_return(c, at<int32_t>(c, arg32(c, 0)).compare_exchange_strong(expected, 1, std::memory_order_acquire) ? 1 : 0);
}
void imp_OSSpinLockUnlock(Context& c) {
    at<int32_t>(c, arg32(c, 0)).store(0, std::memory_order_release);
    shim_return(c, 0);
}

// kern_return_t semaphore_create(task_t, semaphore_t* out, int policy, int value)
void imp_semaphore_create(Context& c) {
    std::lock_guard<std::mutex> l(g_sem_lock);
    const uint32_t s = g_next_sem++;
    g_sems[s] = std::make_unique<Semaphore>();
    g_sems[s]->count = (int32_t)arg32(c, 3);
    c.mem->write<uint32_t>(arg32(c, 1), s);
    shim_return(c, kKernSuccess);
}
void imp_semaphore_destroy(Context& c) {
    std::lock_guard<std::mutex> l(g_sem_lock);
    g_sems.erase(arg32(c, 1));
    shim_return(c, kKernSuccess);
}
void imp_semaphore_signal(Context& c) {
    Semaphore* s = sem(arg32(c, 0));
    if (!s) return shim_return(c, kKernInvalidArgument);
    {
        std::lock_guard<std::mutex> l(s->m);
        s->count++;
    }
    s->cv.notify_one();
    shim_return(c, kKernSuccess);
}
// Wakes every waiter without leaving the count raised.
void imp_semaphore_signal_all(Context& c) {
    Semaphore* s = sem(arg32(c, 0));
    if (!s) return shim_return(c, kKernInvalidArgument);
    {
        std::lock_guard<std::mutex> l(s->m);
        if (s->count < 0) s->count = 0;
    }
    s->cv.notify_all();
    shim_return(c, kKernSuccess);
}
void imp_semaphore_wait(Context& c) {
    Semaphore* s = sem(arg32(c, 0));
    if (!s) return shim_return(c, kKernInvalidArgument);
    std::unique_lock<std::mutex> l(s->m);
    s->cv.wait(l, [&] { return s->count > 0; });
    s->count--;
    shim_return(c, kKernSuccess);
}
// kern_return_t semaphore_timedwait(semaphore_t, mach_timespec_t wait): the
// timespec (unsigned seconds, int nanoseconds) is passed by value.
void imp_semaphore_timedwait(Context& c) {
    Semaphore* s = sem(arg32(c, 0));
    if (!s) return shim_return(c, kKernInvalidArgument);
    const auto timeout = std::chrono::seconds(arg32(c, 1)) + std::chrono::nanoseconds((int32_t)arg32(c, 2));
    std::unique_lock<std::mutex> l(s->m);
    if (!s->cv.wait_for(l, timeout, [&] { return s->count > 0; })) return shim_return(c, kKernOperationTimedOut);
    s->count--;
    shim_return(c, kKernSuccess);
}

// uint64_t mach_absolute_time(void) and mach_timebase_info: the guest sees the
// Intel Macs' timebase, 1/1 (ticks are nanoseconds). The engine computes its
// clock frequency as (denom / numer) * 1e9 in integers, which Apple Silicon's
// 125/3 would turn into 0. Same clock as rdtsc (runtime.cpp).
void imp_mach_absolute_time(Context& c) { shim_return64(c, rdtsc()); }
void imp_mach_timebase_info(Context& c) {
    c.mem->write<uint32_t>(arg32(c, 0), 1);
    c.mem->write<uint32_t>(arg32(c, 0) + 4, 1);
    shim_return(c, 0);
}
// int gettimeofday(struct timeval* tv, void* tz): i386 timeval is two 32-bit fields.
void imp_gettimeofday(Context& c) {
    timeval tv;
    ::gettimeofday(&tv, nullptr);
    if (const uint32_t p = arg32(c, 0)) {
        c.mem->write<uint32_t>(p, (uint32_t)tv.tv_sec);
        c.mem->write<uint32_t>(p + 4, (uint32_t)tv.tv_usec);
    }
    shim_return(c, 0);
}
// time_t time(time_t*): 32-bit on i386
void imp_time(Context& c) {
    const uint32_t t = (uint32_t)::time(nullptr);
    if (arg32(c, 0)) c.mem->write<uint32_t>(arg32(c, 0), t);
    shim_return(c, t);
}

}  // namespace scimitar::imports

namespace scimitar {
namespace {
using namespace imports;
const ImportShim kMachSync[] = {
    {"_OSAtomicAdd32", imp_OSAtomicAdd32},
    {"_OSAtomicAdd64", imp_OSAtomicAdd64},
    {"_OSAtomicCompareAndSwap32Barrier", imp_OSAtomicCompareAndSwap32Barrier},
    {"_OSSpinLockLock", imp_OSSpinLockLock},
    {"_OSSpinLockTry", imp_OSSpinLockTry},
    {"_OSSpinLockUnlock", imp_OSSpinLockUnlock},
    {"_semaphore_create", imp_semaphore_create},
    {"_semaphore_destroy", imp_semaphore_destroy},
    {"_semaphore_signal", imp_semaphore_signal},
    {"_semaphore_signal_all", imp_semaphore_signal_all},
    {"_semaphore_wait", imp_semaphore_wait},
    {"_semaphore_timedwait", imp_semaphore_timedwait},
    {"_mach_absolute_time", imp_mach_absolute_time},
    {"_mach_timebase_info", imp_mach_timebase_info},
    {"_gettimeofday", imp_gettimeofday},
    {"_time", imp_time},
};
SCIMITAR_REGISTER_SHIMS(kMachSync);
}  // namespace
}  // namespace scimitar
