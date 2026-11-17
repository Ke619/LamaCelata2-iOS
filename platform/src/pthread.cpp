// pthread imports. Guest synchronisation objects are real host ones: a guest
// pthread_mutex_t (44 bytes on i386) is looked up by its guest address and
// backed by a host mutex created on first use, so statically initialised
// mutexes work too; likewise condition variables and rwlocks. Thread-specific
// data is per host thread.
//
// Guest threads are host threads, each with its own Context and a guest stack
// from the guest heap. pthread_t is a guest value derived from the thread's
// index (and its Mach port from the same index), so pthread_self,
// pthread_mach_thread_np and thread_resume agree.
#include <pthread.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>

#include "scimitar/guest_heap.h"
#include "scimitar/guest_mutex.h"
#include "scimitar/imports.h"
#include "scimitar/runtime.h"

namespace scimitar::imports {

namespace {

std::mutex g_lock;  // guards the maps below

using scimitar::GuestMutex;  // scimitar/guest_mutex.h

std::map<uint32_t, std::unique_ptr<GuestMutex>>& mutexes() {
    static std::map<uint32_t, std::unique_ptr<GuestMutex>> m;
    return m;
}

// Every pthread_mutex_lock/unlock looks its host mutex up by guest address, so
// a lock-free open-addressing cache sits in front of the map: a hit needs no
// lock. Host mutexes are never freed (destroy keeps them for the address's next
// use), so a cached pointer stays valid. When the cache fills up, lookups fall
// back to the map under the lock.
constexpr uint32_t kMutexCacheBits = 18;
struct MutexSlot {
    std::atomic<uint32_t> addr{0};
    std::atomic<GuestMutex*> mutex{nullptr};
};
MutexSlot g_mutex_cache[1u << kMutexCacheBits];
std::atomic<uint32_t> g_mutex_cache_used{0};

GuestMutex& mutex_slow(uint32_t a) {
    std::lock_guard<std::mutex> l(g_lock);
    auto& p = mutexes()[a];
    if (!p) p = std::make_unique<GuestMutex>();
    return *p;
}

GuestMutex& mutex_at(uint32_t a) {
    constexpr uint32_t mask = (1u << kMutexCacheBits) - 1;
    for (uint32_t k = (a * 0x9E3779B1u) >> (32 - kMutexCacheBits), n = 0; n < 64; k = (k + 1) & mask, n++) {
        MutexSlot& s = g_mutex_cache[k];
        const uint32_t key = s.addr.load(std::memory_order_acquire);
        if (key == a) {
            if (GuestMutex* m = s.mutex.load(std::memory_order_acquire)) return *m;
            break;  // being filled in by another thread
        }
        if (key) continue;
        if (g_mutex_cache_used.load(std::memory_order_relaxed) > (3u << kMutexCacheBits) / 4) break;
        uint32_t expected = 0;
        if (s.addr.compare_exchange_strong(expected, a, std::memory_order_acq_rel)) {
            GuestMutex& m = mutex_slow(a);
            s.mutex.store(&m, std::memory_order_release);
            g_mutex_cache_used.fetch_add(1, std::memory_order_relaxed);
            return m;
        }
        if (expected == a) break;  // another thread claimed this address just now
    }
    return mutex_slow(a);
}

// A per-host-thread identity the guest sees as pthread_t and as a mach port.
// Threads the guest creates get theirs before they start.
std::atomic<uint32_t> g_next_thread{1};
thread_local uint32_t t_index = 0;
uint32_t thread_index() {
    if (!t_index) t_index = g_next_thread++;
    return t_index;
}
constexpr uint32_t kPthreadBase = 0x7e000000, kPthreadStride = 0x1000, kPortBase = 0x1000;

std::atomic<uint32_t> g_next_key{1};
thread_local std::unordered_map<uint32_t, uint32_t> t_specific;

}  // namespace

// The type is kept in the guest attribute object (its first word holds a
// marker, the second the type), which only the shims below read.
void imp_pthread_mutexattr_init(Context& c) {
    c.mem->write<uint32_t>(arg32(c, 0), 0x4d415454);
    c.mem->write<uint32_t>(arg32(c, 0) + 4, 0);
    shim_return(c, 0);
}
void imp_pthread_mutexattr_settype(Context& c) {
    c.mem->write<uint32_t>(arg32(c, 0) + 4, arg32(c, 1));
    shim_return(c, 0);
}
void imp_pthread_mutexattr_setprotocol(Context& c) { shim_return(c, 0); }
void imp_pthread_mutexattr_destroy_UNIX2003(Context& c) { shim_return(c, 0); }

// All guest mutexes are recursive host mutexes: a game that relocks a normal
// mutex on the same thread would deadlock on x86 too, so this only matters for
// code that relies on trylock failing for the owner, which the game does not.
// A host mutex is kept for good once its address has been used (the cache in
// mutex_at points at it); init and destroy leave it, unlocked, for reuse.
void imp_pthread_mutex_init(Context& c) {
    mutex_at(arg32(c, 0));
    shim_return(c, 0);
}
void imp_pthread_mutex_destroy(Context& c) { shim_return(c, 0); }
void imp_pthread_mutex_lock(Context& c) {
    mutex_at(arg32(c, 0)).lock();
    shim_return(c, 0);
}
void imp_pthread_mutex_trylock(Context& c) {
    shim_return(c, mutex_at(arg32(c, 0)).try_lock() ? 0 : 16 /* EBUSY */);
}
void imp_pthread_mutex_unlock(Context& c) {
    mutex_at(arg32(c, 0)).unlock();
    shim_return(c, 0);
}

// int pthread_once(pthread_once_t* once, void (*init)(void))
void imp_pthread_once(Context& c) {
    static std::mutex m;
    static std::condition_variable cv;
    static std::map<uint32_t, int> state;  // 1 running, 2 done
    const uint32_t once = arg32(c, 0), init = arg32(c, 1);
    {
        std::unique_lock<std::mutex> l(m);
        cv.wait(l, [&] { return state[once] != 1; });
        if (state[once] == 2) return shim_return(c, 0);
        state[once] = 1;
    }
    call_guest(c, init, {});
    {
        std::lock_guard<std::mutex> l(m);
        state[once] = 2;
    }
    cv.notify_all();
    shim_return(c, 0);
}

// int pthread_key_create(pthread_key_t* key, void (*destructor)(void*))
// (destructors are not run).
void imp_pthread_key_create(Context& c) {
    c.mem->write<uint32_t>(arg32(c, 0), g_next_key++);
    shim_return(c, 0);
}
void imp_pthread_key_delete(Context& c) { shim_return(c, 0); }
void imp_pthread_getspecific(Context& c) {
    auto it = t_specific.find(arg32(c, 0));
    shim_return(c, it == t_specific.end() ? 0 : it->second);
}
void imp_pthread_setspecific(Context& c) {
    t_specific[arg32(c, 0)] = arg32(c, 1);
    shim_return(c, 0);
}

void imp_pthread_self(Context& c) { shim_return(c, kPthreadBase + kPthreadStride * thread_index()); }
void imp_pthread_mach_thread_np(Context& c) {
    shim_return(c, kPortBase + (arg32(c, 0) - kPthreadBase) / kPthreadStride);
}
void imp_pthread_from_mach_thread_np(Context& c) {
    shim_return(c, kPthreadBase + kPthreadStride * (arg32(c, 0) - kPortBase));
}

// ---- threads ----

namespace {

struct ThreadExit {
    uint32_t value;
};

struct GuestThread {
    uint32_t index = 0, start = 0, arg = 0, stack = 0, stack_size = 0, result = 0;
    std::mutex m;
    std::condition_variable cv;
    bool suspended = false, finished = false;
};

std::mutex g_threads_lock;
std::map<uint32_t, std::shared_ptr<GuestThread>> g_threads;  // by index

std::shared_ptr<GuestThread> thread_by_index(uint32_t k) {
    std::lock_guard<std::mutex> l(g_threads_lock);
    auto it = g_threads.find(k);
    return it == g_threads.end() ? nullptr : it->second;
}

// Attributes live in the guest pthread_attr_t: [0] marker, [4] stack size,
// [8] detach state.
constexpr uint32_t kAttrMarker = 0x41545452, kDefaultStack = 1u << 20, kWindowStack = 256u << 10;

uint32_t create(Context& c, bool suspended) {
    const uint32_t out = arg32(c, 0), attr = arg32(c, 1), start = arg32(c, 2), arg = arg32(c, 3);
    if (std::getenv("SCIMITAR_TRACE_THREADS"))
        std::fprintf(stderr, "pthread_create%s(start %08x, stack %u KB asked) from %08x\n",
                     suspended ? "_suspended_np" : "", start,
                     attr && c.mem->read<uint32_t>(attr) == kAttrMarker ? c.mem->read<uint32_t>(attr + 4) >> 10 : 0,
                     c.mem->read<uint32_t>(c.esp));
    auto t = std::make_shared<GuestThread>();
    t->index = g_next_thread++;
    t->start = start, t->arg = arg, t->suspended = suspended;
    t->stack_size = kDefaultStack;
    if (attr && c.mem->read<uint32_t>(attr) == kAttrMarker && c.mem->read<uint32_t>(attr + 4) >= 0x10000)
        t->stack_size = c.mem->read<uint32_t>(attr + 4);
    // In a small window every stack byte is real memory: AC2's threads use at
    // most 60 KB in Florence (the heap report), so 256 KB each.
    if (guest_layout() == GuestLayout::Window) t->stack_size = std::min<uint32_t>(t->stack_size, kWindowStack);
    t->stack = heap::alloc(*c.mem, t->stack_size, 16);
    if (!t->stack) return 35;  // EAGAIN
    {
        std::lock_guard<std::mutex> l(g_threads_lock);
        g_threads[t->index] = t;
    }
    if (out) c.mem->write<uint32_t>(out, kPthreadBase + kPthreadStride * t->index);
    GuestMemory* mem = c.mem;
    std::thread([t, mem] {
        t_index = t->index;
        {
            std::unique_lock<std::mutex> l(t->m);
            t->cv.wait(l, [&] { return !t->suspended; });
        }
        Context tc;
        tc.mem = mem;
        set_thread_context(&tc);
        // At the entry of start(arg), esp + 4 is 16-byte aligned (Darwin i386 ABI).
        tc.esp = ((t->stack + t->stack_size) & ~15u) - 12;
        uint32_t result = 0;
        try {
            result = call_guest(tc, t->start, {t->arg});
        } catch (const ThreadExit& e) {
            result = e.value;
        }
        // The guest stack goes back to the heap (Wwise starts a thread per read).
        set_thread_context(nullptr);
        heap::release(*mem, t->stack);
        std::lock_guard<std::mutex> l(t->m);
        t->result = result;
        t->finished = true;
        t->cv.notify_all();
    }).detach();
    return 0;
}

}  // namespace

// int pthread_create(pthread_t*, const pthread_attr_t*, void* (*start)(void*), void* arg)
void imp_pthread_create(Context& c) { shim_return(c, create(c, false)); }
void imp_pthread_create_suspended_np(Context& c) { shim_return(c, create(c, true)); }

// kern_return_t thread_resume(thread_act_t)
void imp_thread_resume(Context& c) {
    if (auto t = thread_by_index(arg32(c, 0) - kPortBase)) {
        std::lock_guard<std::mutex> l(t->m);
        t->suspended = false;
        t->cv.notify_all();
        return shim_return(c, 0);
    }
    shim_return(c, 0);  // not a thread we created (e.g. the main thread): nothing to resume
}
// Suspending another running thread has no safe equivalent here; the engine
// only does this around debugging and crash reports.
void imp_thread_suspend(Context& c) { shim_return(c, 0); }

// int pthread_join(pthread_t, void** value)
void imp_pthread_join(Context& c) {
    auto t = thread_by_index((arg32(c, 0) - kPthreadBase) / kPthreadStride);
    if (!t) return shim_return(c, 3);  // ESRCH
    std::unique_lock<std::mutex> l(t->m);
    t->cv.wait(l, [&] { return t->finished; });
    if (arg32(c, 1)) c.mem->write<uint32_t>(arg32(c, 1), t->result);
    shim_return(c, 0);
}
void imp_pthread_join_UNIX2003(Context& c) { imp_pthread_join(c); }
void imp_pthread_detach(Context& c) { shim_return(c, 0); }
[[noreturn]] void imp_pthread_exit(Context& c) { throw ThreadExit{arg32(c, 0)}; }
void imp_pthread_yield_np(Context& c) {
    std::this_thread::yield();
    shim_return(c, 0);
}

void imp_pthread_attr_init(Context& c) {
    const uint32_t a = arg32(c, 0);
    c.mem->write<uint32_t>(a, kAttrMarker);
    c.mem->write<uint32_t>(a + 4, kDefaultStack);
    c.mem->write<uint32_t>(a + 8, 1);  // PTHREAD_CREATE_JOINABLE
    shim_return(c, 0);
}
void imp_pthread_attr_destroy(Context& c) { shim_return(c, 0); }
void imp_pthread_attr_setstacksize(Context& c) {
    c.mem->write<uint32_t>(arg32(c, 0) + 4, arg32(c, 1));
    shim_return(c, 0);
}
void imp_pthread_attr_getstacksize(Context& c) {
    c.mem->write<uint32_t>(arg32(c, 1), c.mem->read<uint32_t>(arg32(c, 0) + 4));
    shim_return(c, 0);
}
void imp_pthread_attr_setdetachstate(Context& c) {
    c.mem->write<uint32_t>(arg32(c, 0) + 8, arg32(c, 1));
    shim_return(c, 0);
}
void imp_pthread_attr_setschedparam(Context& c) { shim_return(c, 0); }
void imp_pthread_attr_setschedpolicy(Context& c) { shim_return(c, 0); }
void imp_pthread_attr_getschedparam(Context& c) {
    if (arg32(c, 1)) c.mem->write<uint32_t>(arg32(c, 1), 31);
    shim_return(c, 0);
}

// ---- condition variables, rwlocks ----

namespace {

std::map<uint32_t, std::unique_ptr<std::condition_variable_any>>& conds() {
    static std::map<uint32_t, std::unique_ptr<std::condition_variable_any>> m;
    return m;
}
std::condition_variable_any& cond_at(uint32_t a) {
    std::lock_guard<std::mutex> l(g_lock);
    auto& p = conds()[a];
    if (!p) p = std::make_unique<std::condition_variable_any>();
    return *p;
}
// rwlocks keep their own state rather than a std::shared_mutex, because the
// game unlocks a write lock from another thread than the one that took it (and
// pthread_rwlock_unlock serves both kinds). Readers wait only while a writer
// holds the lock, never for waiting writers, so recursive read locks cannot
// deadlock; a thread asking again for a write lock it holds gets EDEADLK, as
// on Darwin.
struct RWLock {
    std::mutex m;
    std::condition_variable cv;
    uint32_t readers = 0;
    bool writer = false;
    std::thread::id owner;
};
std::map<uint32_t, std::unique_ptr<RWLock>>& rwlocks() {
    static std::map<uint32_t, std::unique_ptr<RWLock>> m;
    return m;
}
// Like mutexes, a host rwlock is kept once its address has been used: another
// thread may still be waiting on it when the game initializes the address again.
RWLock& rwlock_at(uint32_t a) {
    std::lock_guard<std::mutex> l(g_lock);
    auto& p = rwlocks()[a];
    if (!p) p = std::make_unique<RWLock>();
    return *p;
}

// Waits on `cond` with guest mutex `m` (locked by the caller), for at most
// `timeout` (none if negative). Returns 0 or ETIMEDOUT.
uint32_t cond_wait(uint32_t cond, uint32_t m, std::chrono::nanoseconds timeout) {
    auto& cv = cond_at(cond);
    auto& mx = mutex_at(m);
    std::unique_lock<GuestMutex> l(mx, std::adopt_lock);
    uint32_t r = 0;
    if (timeout.count() < 0) cv.wait(l);
    else if (cv.wait_for(l, timeout) == std::cv_status::timeout) r = 60;  // ETIMEDOUT
    l.release();  // still locked, as pthread_cond_wait returns
    return r;
}

}  // namespace

void imp_pthread_cond_init(Context& c) {
    cond_at(arg32(c, 0));
    shim_return(c, 0);
}
void imp_pthread_cond_init_UNIX2003(Context& c) { imp_pthread_cond_init(c); }
void imp_pthread_cond_destroy(Context& c) {
    std::lock_guard<std::mutex> l(g_lock);
    conds().erase(arg32(c, 0));
    shim_return(c, 0);
}
void imp_pthread_condattr_init(Context& c) { shim_return(c, 0); }
void imp_pthread_condattr_destroy(Context& c) { shim_return(c, 0); }
void imp_pthread_cond_signal(Context& c) {
    cond_at(arg32(c, 0)).notify_one();
    shim_return(c, 0);
}
void imp_pthread_cond_broadcast(Context& c) {
    cond_at(arg32(c, 0)).notify_all();
    shim_return(c, 0);
}
void imp_pthread_cond_wait(Context& c) {
    shim_return(c, cond_wait(arg32(c, 0), arg32(c, 1), std::chrono::nanoseconds(-1)));
}
void imp_pthread_cond_wait_UNIX2003(Context& c) { imp_pthread_cond_wait(c); }
// pthread_cond_timedwait(cond, mutex, const struct timespec* abstime): an
// absolute wall-clock time (i386 timespec: 32-bit seconds, 32-bit nanoseconds).
void imp_pthread_cond_timedwait_UNIX2003(Context& c) {
    const uint32_t ts = arg32(c, 2);
    const int64_t sec = (int32_t)c.mem->read<uint32_t>(ts), nsec = (int32_t)c.mem->read<uint32_t>(ts + 4);
    const auto deadline = std::chrono::system_clock::time_point(
        std::chrono::duration_cast<std::chrono::system_clock::duration>(
            std::chrono::seconds(sec) + std::chrono::nanoseconds(nsec)));
    auto left = std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - std::chrono::system_clock::now());
    if (left.count() < 0) left = std::chrono::nanoseconds(0);
    shim_return(c, cond_wait(arg32(c, 0), arg32(c, 1), left));
}
// pthread_cond_timedwait_relative_np(cond, mutex, const struct timespec* reltime)
void imp_pthread_cond_timedwait_relative_np(Context& c) {
    const uint32_t ts = arg32(c, 2);
    const int64_t sec = (int32_t)c.mem->read<uint32_t>(ts), nsec = (int32_t)c.mem->read<uint32_t>(ts + 4);
    shim_return(c, cond_wait(arg32(c, 0), arg32(c, 1),
                             std::chrono::seconds(sec) + std::chrono::nanoseconds(nsec)));
}

void imp_pthread_rwlock_init_UNIX2003(Context& c) {
    rwlock_at(arg32(c, 0));
    shim_return(c, 0);
}
void imp_pthread_rwlock_destroy_UNIX2003(Context& c) { shim_return(c, 0); }
void imp_pthread_rwlock_rdlock_UNIX2003(Context& c) {
    RWLock& rw = rwlock_at(arg32(c, 0));
    std::unique_lock<std::mutex> l(rw.m);
    rw.cv.wait(l, [&] { return !rw.writer; });
    rw.readers++;
    shim_return(c, 0);
}
void imp_pthread_rwlock_wrlock_UNIX2003(Context& c) {
    RWLock& rw = rwlock_at(arg32(c, 0));
    std::unique_lock<std::mutex> l(rw.m);
    if (rw.writer && rw.owner == std::this_thread::get_id()) return shim_return(c, EDEADLK);
    rw.cv.wait(l, [&] { return !rw.writer && rw.readers == 0; });
    rw.writer = true;
    rw.owner = std::this_thread::get_id();
    shim_return(c, 0);
}
// One unlock call for both kinds: the write lock if it is held, else one reader.
void imp_pthread_rwlock_unlock_UNIX2003(Context& c) {
    RWLock& rw = rwlock_at(arg32(c, 0));
    {
        std::lock_guard<std::mutex> l(rw.m);
        if (rw.writer) rw.writer = false;
        else if (rw.readers) rw.readers--;
        else return shim_return(c, EPERM);
    }
    rw.cv.notify_all();
    shim_return(c, 0);
}
// int pthread_getschedparam(pthread_t, int* policy, struct sched_param* param)
void imp_pthread_getschedparam(Context& c) {
    if (arg32(c, 1)) c.mem->write<uint32_t>(arg32(c, 1), 1);   // SCHED_OTHER
    if (arg32(c, 2)) c.mem->write<uint32_t>(arg32(c, 2), 31);  // default priority
    shim_return(c, 0);
}
void imp_pthread_setschedparam(Context& c) { shim_return(c, 0); }
void imp_sched_get_priority_min(Context& c) { shim_return(c, 15); }
void imp_sched_get_priority_max(Context& c) { shim_return(c, 47); }

}  // namespace scimitar::imports

namespace scimitar {
namespace {
using namespace imports;
const ImportShim kPthread[] = {
    {"_pthread_mutexattr_init", imp_pthread_mutexattr_init},
    {"_pthread_mutexattr_settype", imp_pthread_mutexattr_settype},
    {"_pthread_mutexattr_setprotocol", imp_pthread_mutexattr_setprotocol},
    {"_pthread_mutexattr_destroy$UNIX2003", imp_pthread_mutexattr_destroy_UNIX2003},
    {"_pthread_mutex_init", imp_pthread_mutex_init},
    {"_pthread_mutex_destroy", imp_pthread_mutex_destroy},
    {"_pthread_mutex_lock", imp_pthread_mutex_lock},
    {"_pthread_mutex_trylock", imp_pthread_mutex_trylock},
    {"_pthread_mutex_unlock", imp_pthread_mutex_unlock},
    {"_pthread_once", imp_pthread_once},
    {"_pthread_key_create", imp_pthread_key_create},
    {"_pthread_key_delete", imp_pthread_key_delete},
    {"_pthread_getspecific", imp_pthread_getspecific},
    {"_pthread_setspecific", imp_pthread_setspecific},
    {"_pthread_self", imp_pthread_self},
    {"_pthread_mach_thread_np", imp_pthread_mach_thread_np},
    {"_pthread_from_mach_thread_np", imp_pthread_from_mach_thread_np},
    {"_pthread_getschedparam", imp_pthread_getschedparam},
    {"_pthread_setschedparam", imp_pthread_setschedparam},
    {"_sched_get_priority_min", imp_sched_get_priority_min},
    {"_sched_get_priority_max", imp_sched_get_priority_max},
    {"_pthread_create", imp_pthread_create},
    {"_pthread_create_suspended_np", imp_pthread_create_suspended_np},
    {"_thread_resume", imp_thread_resume},
    {"_thread_suspend", imp_thread_suspend},
    {"_pthread_join", imp_pthread_join},
    {"_pthread_join$UNIX2003", imp_pthread_join_UNIX2003},
    {"_pthread_detach", imp_pthread_detach},
    {"_pthread_exit", imp_pthread_exit},
    {"_pthread_yield_np", imp_pthread_yield_np},
    {"_pthread_attr_init", imp_pthread_attr_init},
    {"_pthread_attr_destroy", imp_pthread_attr_destroy},
    {"_pthread_attr_setstacksize", imp_pthread_attr_setstacksize},
    {"_pthread_attr_getstacksize", imp_pthread_attr_getstacksize},
    {"_pthread_attr_setdetachstate", imp_pthread_attr_setdetachstate},
    {"_pthread_attr_setschedparam", imp_pthread_attr_setschedparam},
    {"_pthread_attr_setschedpolicy", imp_pthread_attr_setschedpolicy},
    {"_pthread_attr_getschedparam", imp_pthread_attr_getschedparam},
    {"_pthread_cond_init", imp_pthread_cond_init},
    {"_pthread_cond_init$UNIX2003", imp_pthread_cond_init_UNIX2003},
    {"_pthread_cond_destroy", imp_pthread_cond_destroy},
    {"_pthread_condattr_init", imp_pthread_condattr_init},
    {"_pthread_condattr_destroy", imp_pthread_condattr_destroy},
    {"_pthread_cond_signal", imp_pthread_cond_signal},
    {"_pthread_cond_broadcast", imp_pthread_cond_broadcast},
    {"_pthread_cond_wait", imp_pthread_cond_wait},
    {"_pthread_cond_wait$UNIX2003", imp_pthread_cond_wait_UNIX2003},
    {"_pthread_cond_timedwait$UNIX2003", imp_pthread_cond_timedwait_UNIX2003},
    {"_pthread_cond_timedwait_relative_np", imp_pthread_cond_timedwait_relative_np},
    {"_pthread_rwlock_init$UNIX2003", imp_pthread_rwlock_init_UNIX2003},
    {"_pthread_rwlock_destroy$UNIX2003", imp_pthread_rwlock_destroy_UNIX2003},
    {"_pthread_rwlock_rdlock$UNIX2003", imp_pthread_rwlock_rdlock_UNIX2003},
    {"_pthread_rwlock_wrlock$UNIX2003", imp_pthread_rwlock_wrlock_UNIX2003},
    {"_pthread_rwlock_unlock$UNIX2003", imp_pthread_rwlock_unlock_UNIX2003},
};
SCIMITAR_REGISTER_SHIMS(kPthread);
}  // namespace
}  // namespace scimitar
