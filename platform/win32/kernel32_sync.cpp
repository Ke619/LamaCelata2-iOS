// KERNEL32, part 2: threads, synchronisation objects, critical sections,
// interlocked operations, thread-local storage, heaps, virtual memory, time.
#include <mach/mach_time.h>
#include <pthread.h>
#include <sys/time.h>
#include <unistd.h>

#include <atomic>

#include "scimitar/guest_mutex.h"
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <ctime>
#include <map>
#include <mutex>
#include <thread>

#include "scimitar/guest_heap.h"
#include "win32.h"

namespace scimitar::win32 {

namespace {

struct Event : Waitable {
    std::atomic<bool> state{false};
    bool manual = false;
    bool try_acquire() override {
        if (manual) return state.load();
        bool expected = true;
        return state.compare_exchange_strong(expected, false);
    }
    bool signalled() const override { return state.load(); }
};

struct Semaphore : Waitable {
    std::atomic<int32_t> count{0};
    int32_t max = 0;
    bool try_acquire() override {
        int32_t n = count.load();
        while (n > 0)
            if (count.compare_exchange_weak(n, n - 1)) return true;
        return false;
    }
    bool signalled() const override { return count.load() > 0; }
};

// A guest thread: its host thread, exit code, and suspension before it starts.
struct Thread : Waitable {
    std::atomic<bool> done{false};
    uint32_t exit_code = 259;  // STILL_ACTIVE
    uint32_t tid = 0;
    std::mutex lock;
    std::condition_variable cv;
    int suspend = 0;
    bool try_acquire() override { return done.load(); }
    bool signalled() const override { return done.load(); }
};

std::mutex g_threads_lock;
std::map<uint32_t, std::shared_ptr<Thread>> g_threads;  // by thread id
thread_local std::shared_ptr<Thread> t_self;

std::shared_ptr<Thread> self() {
    if (!t_self) {
        t_self = std::make_shared<Thread>();
        t_self->tid = current_thread_id();
        std::lock_guard<std::mutex> l(g_threads_lock);
        g_threads[t_self->tid] = t_self;
    }
    return t_self;
}

// ExitThread unwinds the host thread's stack of translated frames.
struct ThreadExit {
    uint32_t code;
};

}  // namespace

void signal_event(uint32_t handle) {
    if (auto e = handle_as<Event>(handle)) {
        e->state = true;
        notify_waiters();
    }
}

}  // namespace scimitar::win32

namespace scimitar::imports {
using namespace scimitar::win32;

// ---- threads ----

W32(kernel32, CreateThread) {
    const uint32_t stack_size = arg(c, 1), start = arg(c, 2), param = arg(c, 3), flags = arg(c, 4),
                   tid_out = arg(c, 5);
    auto t = std::make_shared<Thread>();
    t->suspend = flags & 4 ? 1 : 0;  // CREATE_SUSPENDED
    const uint32_t handle = add_handle(t);
    const uint32_t size = std::max<uint32_t>(stack_size, 1u << 20) + (1u << 20);
    const uint32_t stack = galloc(size, 16);
    const uint16_t cw = c.fpu.cw;
    std::mutex started_lock;
    std::condition_variable started_cv;
    bool started = false;
    // Translated code needs deep host stacks: each thread gets 256 MB of
    // address space for its stack (pages are only touched as needed).
    auto body = [=, &started_lock, &started_cv, &started] {
        Context* tc = new Context;
        tc->esp = stack + size - 64;
        tc->fpu.cw = cw;
        attach_thread(*tc, stack + size, stack);
        t_self = t;
        t->tid = current_thread_id();
        {
            std::lock_guard<std::mutex> l(g_threads_lock);
            g_threads[t->tid] = t;
        }
        {
            std::lock_guard<std::mutex> l(started_lock);
            started = true;
        }
        started_cv.notify_all();
        {
            std::unique_lock<std::mutex> l(t->lock);
            t->cv.wait(l, [&] { return t->suspend <= 0; });
        }
        trace("thread %x starts at %08x(%08x)", t->tid, start, param);
        uint32_t code;
        try {
            code = call_guest(*tc, start, {param});
        } catch (const ThreadExit& e) {
            code = e.code;
        }
        trace("thread %x ends with %u", t->tid, code);
        t->exit_code = code;
        t->done = true;
        notify_waiters();
    };
    using Body = decltype(body);
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256u << 20);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t thread;
    auto* heap_body = new Body(body);
    pthread_create(&thread, &attr, [](void* p) -> void* {
        auto* b = static_cast<Body*>(p);
        (*b)();
        delete b;
        return nullptr;
    }, heap_body);
    pthread_attr_destroy(&attr);
    {
        std::unique_lock<std::mutex> l(started_lock);
        started_cv.wait(l, [&] { return started; });
    }
    if (tid_out) wr(tid_out, t->tid);
    ret(c, handle, 6);
}

W32(kernel32, ExitThread) { throw ThreadExit{arg(c, 0)}; }

W32(kernel32, GetExitCodeThread) {
    auto t = handle_as<Thread>(arg(c, 0));
    if (!t) return ret(c, 0, 2);
    wr(arg(c, 1), t->done ? t->exit_code : 259);
    ret(c, 1, 2);
}

W32(kernel32, ResumeThread) {
    auto t = arg(c, 0) == kCurrentThread ? self() : handle_as<Thread>(arg(c, 0));
    if (!t) return ret(c, 0xffffffffu, 1);
    int prev;
    {
        std::lock_guard<std::mutex> l(t->lock);
        prev = t->suspend;
        if (t->suspend > 0) t->suspend--;
    }
    t->cv.notify_all();
    ret(c, (uint32_t)std::max(prev, 0), 1);
}

W32(kernel32, SuspendThread) {
    // Only threads that have not started yet really stop; a running thread
    // keeps running (the game suspends threads around profiling only).
    auto t = arg(c, 0) == kCurrentThread ? self() : handle_as<Thread>(arg(c, 0));
    if (!t) return ret(c, 0xffffffffu, 1);
    trace("SuspendThread(%x): not suspended", t->tid);
    std::lock_guard<std::mutex> l(t->lock);
    ret(c, (uint32_t)t->suspend, 1);
}

W32(kernel32, OpenThread) {
    std::shared_ptr<Thread> t;
    {
        std::lock_guard<std::mutex> l(g_threads_lock);
        auto it = g_threads.find(arg(c, 2));
        if (it != g_threads.end()) t = it->second;
    }
    ret(c, t ? add_handle(t) : 0, 3);
}

W32(kernel32, GetCurrentThread) { ret(c, kCurrentThread, 0); }
W32(kernel32, GetCurrentThreadId) { ret(c, current_thread_id(), 0); }
W32(kernel32, SetThreadPriority) { ret(c, 1, 2); }
W32(kernel32, SetThreadPriorityBoost) { ret(c, 1, 2); }
W32(kernel32, SetThreadAffinityMask) { ret(c, 0xf, 2); }

W32(kernel32, GetProcessAffinityMask) {
    const uint32_t mask = std::getenv("SCIMITAR_WIN32_CPUS") ? (1u << std::atoi(std::getenv("SCIMITAR_WIN32_CPUS"))) - 1 : 0xf;
    if (arg(c, 1)) wr(arg(c, 1), mask);
    if (arg(c, 2)) wr(arg(c, 2), mask);
    ret(c, 1, 3);
}

W32(kernel32, GetProcessTimes) {
    for (int k = 1; k <= 4; k++)
        if (arg(c, k)) wr(arg(c, k), 0), wr(arg(c, k) + 4, 0);
    ret(c, 1, 5);
}

W32(kernel32, SwitchToThread) {
    std::this_thread::yield();
    ret(c, 1, 0);
}

W32(kernel32, Sleep) {
    const uint32_t ms = arg(c, 0);
    if (ms) std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    else std::this_thread::yield();
    ret(c, 0, 1);
}

W32(kernel32, DuplicateHandle) {
    // (srcProcess, src, dstProcess, dst*, access, inherit, options)
    const uint32_t src = arg(c, 1), dst = arg(c, 3);
    std::shared_ptr<Object> o = src == kCurrentThread ? std::static_pointer_cast<Object>(self()) : get_handle(src);
    if (!o) return ret(c, 0, 7);
    if (dst) wr(dst, add_handle(o));
    if (arg(c, 6) & 1) close_handle(src);  // DUPLICATE_CLOSE_SOURCE
    ret(c, 1, 7);
}

W32(kernel32, CloseHandle) {
    const uint32_t h = arg(c, 0);
    if (h == kCurrentThread || h == kCurrentProcess) return ret(c, 1, 1);
    ret(c, close_handle(h) ? 1 : 0, 1);
}

// ---- events, semaphores, waiting ----

W32(kernel32, CreateEventA) {
    auto e = std::make_shared<Event>();
    e->manual = arg(c, 1) != 0;
    e->state = arg(c, 2) != 0;
    ret(c, add_handle(e), 4);
}

W32(kernel32, SetEvent) {
    auto e = handle_as<Event>(arg(c, 0));
    if (!e) return ret(c, 0, 1);
    e->state = true;
    notify_waiters();
    ret(c, 1, 1);
}

W32(kernel32, ResetEvent) {
    auto e = handle_as<Event>(arg(c, 0));
    if (!e) return ret(c, 0, 1);
    e->state = false;
    ret(c, 1, 1);
}

W32(kernel32, PulseEvent) {
    // Releases waiters as SetEvent then ResetEvent would; with polling waits
    // that is approximate: the event stays set briefly.
    auto e = handle_as<Event>(arg(c, 0));
    if (!e) return ret(c, 0, 1);
    e->state = true;
    notify_waiters();
    std::this_thread::sleep_for(std::chrono::microseconds(200));
    if (e->manual) e->state = false;
    ret(c, 1, 1);
}

W32(kernel32, CreateSemaphoreA) {
    auto s = std::make_shared<Semaphore>();
    s->count = (int32_t)arg(c, 1);
    s->max = (int32_t)arg(c, 2);
    ret(c, add_handle(s), 4);
}

W32(kernel32, ReleaseSemaphore) {
    auto s = handle_as<Semaphore>(arg(c, 0));
    if (!s) return ret(c, 0, 3);
    const int32_t prev = s->count.fetch_add((int32_t)arg(c, 1));
    if (arg(c, 2)) wr(arg(c, 2), (uint32_t)prev);
    notify_waiters();
    ret(c, 1, 3);
}

W32(kernel32, WaitForSingleObject) {
    const uint32_t h = arg(c, 0), ms = arg(c, 1);
    auto w = handle_as<Waitable>(h);
    if (!w) {
        trace("WaitForSingleObject(%x): not waitable", h);
        return ret(c, 0xffffffffu, 2);  // WAIT_FAILED
    }
    ret(c, wait_until(ms, [&] { return w->try_acquire(); }) ? 0 : 0x102, 2);
}

W32(kernel32, WaitForMultipleObjects) {
    const uint32_t n = arg(c, 0), handles = arg(c, 1), all = arg(c, 2), ms = arg(c, 3);
    std::vector<std::shared_ptr<Waitable>> ws;
    for (uint32_t k = 0; k < n; k++) {
        auto w = handle_as<Waitable>(rd(handles + 4 * k));
        if (!w) return ret(c, 0xffffffffu, 4);
        ws.push_back(w);
    }
    uint32_t which = 0;
    const bool ok = wait_until(ms, [&] {
        if (all) {
            for (auto& w : ws)
                if (!w->signalled()) return false;
            for (auto& w : ws) w->try_acquire();
            return true;
        }
        for (uint32_t k = 0; k < n; k++)
            if (ws[k]->try_acquire()) {
                which = k;
                return true;
            }
        return false;
    });
    ret(c, ok ? which : 0x102, 4);
}

// ---- critical sections ----
// The guest CRITICAL_SECTION (24 bytes) keeps an index into a table of host
// locks (scimitar/guest_mutex.h) in its LockSemaphore field (+0x10); OwningThread (+0xc)
// and RecursionCount (+8) are kept up to date for code that reads them.

namespace {
// The host locks, in chunks that are never moved or freed, so a lookup needs
// no lock (it took a global one at every Enter and Leave before).
constexpr uint32_t kCsTag = 0x5c000000u, kCsChunk = 4096, kCsChunks = 4096;
std::mutex g_cs_lock;  // guards creation
std::atomic<GuestMutex*> g_cs_chunks[kCsChunks];
std::atomic<uint32_t> g_cs_count{0};

uint32_t cs_new() {
    std::lock_guard<std::mutex> l(g_cs_lock);
    const uint32_t n = g_cs_count.load(std::memory_order_relaxed);
    if (n >= kCsChunk * kCsChunks) fatal("too many critical sections");
    if (!g_cs_chunks[n / kCsChunk].load(std::memory_order_relaxed))
        g_cs_chunks[n / kCsChunk].store(new GuestMutex[kCsChunk], std::memory_order_release);
    g_cs_count.store(n + 1, std::memory_order_release);
    return kCsTag | n;
}

GuestMutex& cs_mutex(uint32_t cs) {
    uint32_t id = rd(cs + 0x10);
    if ((id & 0xff000000u) != kCsTag) {
        // Never initialised (zeroed static storage): initialise now.
        static std::mutex first;
        std::lock_guard<std::mutex> l(first);
        id = rd(cs + 0x10);
        if ((id & 0xff000000u) != kCsTag) wr(cs + 0x10, id = cs_new());
    }
    const uint32_t n = id & 0xffffff;
    return g_cs_chunks[n / kCsChunk].load(std::memory_order_acquire)[n % kCsChunk];
}

void cs_init(uint32_t cs) {
    std::memset(gptr(cs), 0, 24);
    wr(cs + 4, 0xffffffffu);  // LockCount
    wr(cs + 0x10, cs_new());
}
}  // namespace

W32(kernel32, InitializeCriticalSection) {
    cs_init(arg(c, 0));
    ret(c, 0, 1);
}

W32(kernel32, InitializeCriticalSectionAndSpinCount) {
    cs_init(arg(c, 0));
    ret(c, 1, 2);
}

W32(kernel32, DeleteCriticalSection) { ret(c, 0, 1); }  // the mutex stays (it is small)

W32(kernel32, EnterCriticalSection) {
    const uint32_t cs = arg(c, 0);
    cs_mutex(cs).lock();
    wr(cs + 0xc, current_thread_id());
    wr(cs + 8, rd(cs + 8) + 1);
    ret(c, 0, 1);
}

W32(kernel32, TryEnterCriticalSection) {
    const uint32_t cs = arg(c, 0);
    if (!cs_mutex(cs).try_lock()) return ret(c, 0, 1);
    wr(cs + 0xc, current_thread_id());
    wr(cs + 8, rd(cs + 8) + 1);
    ret(c, 1, 1);
}

W32(kernel32, LeaveCriticalSection) {
    const uint32_t cs = arg(c, 0);
    const uint32_t n = rd(cs + 8);
    wr(cs + 8, n ? n - 1 : 0);
    if (n <= 1) wr(cs + 0xc, 0);
    cs_mutex(cs).unlock();
    ret(c, 0, 1);
}

// ---- interlocked ----

W32(kernel32, InterlockedIncrement) {
    ret(c, __atomic_add_fetch(reinterpret_cast<uint32_t*>(gptr(arg(c, 0))), 1u, __ATOMIC_SEQ_CST), 1);
}
W32(kernel32, InterlockedDecrement) {
    ret(c, __atomic_sub_fetch(reinterpret_cast<uint32_t*>(gptr(arg(c, 0))), 1u, __ATOMIC_SEQ_CST), 1);
}
W32(kernel32, InterlockedExchange) {
    ret(c, __atomic_exchange_n(reinterpret_cast<uint32_t*>(gptr(arg(c, 0))), arg(c, 1), __ATOMIC_SEQ_CST), 2);
}
W32(kernel32, InterlockedExchangeAdd) {
    ret(c, __atomic_fetch_add(reinterpret_cast<uint32_t*>(gptr(arg(c, 0))), arg(c, 1), __ATOMIC_SEQ_CST), 2);
}

// ---- thread-local storage: slots in the TEB at +0xe10 (64 of them) ----

namespace {
std::mutex g_tls_lock;
uint64_t g_tls_used = 0;
}  // namespace

W32(kernel32, TlsAlloc) {
    std::lock_guard<std::mutex> l(g_tls_lock);
    for (uint32_t k = 0; k < 64; k++)
        if (!(g_tls_used >> k & 1)) {
            g_tls_used |= 1ull << k;
            return ret(c, k, 0);
        }
    ret(c, 0xffffffffu, 0);  // TLS_OUT_OF_INDEXES
}

W32(kernel32, TlsFree) {
    std::lock_guard<std::mutex> l(g_tls_lock);
    if (arg(c, 0) < 64) g_tls_used &= ~(1ull << arg(c, 0));
    ret(c, 1, 1);
}

W32(kernel32, TlsGetValue) {
    const uint32_t k = arg(c, 0);
    set_last_error(0);
    ret(c, k < 64 ? rd(c.fs_base + 0xe10 + 4 * k) : 0, 1);
}

W32(kernel32, TlsSetValue) {
    const uint32_t k = arg(c, 0);
    if (k < 64) wr(c.fs_base + 0xe10 + 4 * k, arg(c, 1));
    ret(c, k < 64, 2);
}

// ---- heaps: all on the guest heap ----

W32(kernel32, GetProcessHeap) { ret(c, 0x00140000, 0); }
W32(kernel32, HeapCreate) { ret(c, 0x00150000, 3); }
W32(kernel32, HeapDestroy) { ret(c, 1, 1); }

W32(kernel32, HeapAlloc) {
    const uint32_t flags = arg(c, 1), size = arg(c, 2);
    const uint32_t p = heap::alloc(mem(), size ? size : 1, 16);
    if (p && (flags & 8)) std::memset(gptr(p), 0, size);  // HEAP_ZERO_MEMORY
    ret(c, p, 3);
}

W32(kernel32, HeapFree) {
    gfree(arg(c, 2));
    ret(c, 1, 3);
}

W32(kernel32, HeapSize) { ret(c, arg(c, 2) ? heap::size_of(mem(), arg(c, 2)) : 0xffffffffu, 3); }

W32(kernel32, HeapReAlloc) {
    const uint32_t flags = arg(c, 1), old = arg(c, 2), size = arg(c, 3);
    const uint32_t old_size = old ? heap::size_of(mem(), old) : 0;
    if (size <= old_size) return ret(c, old, 4);
    if (flags & 0x10) return ret(c, 0, 4);  // HEAP_REALLOC_IN_PLACE_ONLY
    const uint32_t p = heap::alloc(mem(), size, 16);
    if (!p) return ret(c, 0, 4);
    std::memcpy(gptr(p), gptr(old), old_size);
    if (flags & 8) std::memset(gptr(p + old_size), 0, size - old_size);
    gfree(old);
    ret(c, p, 4);
}

// ---- virtual memory: regions on the guest heap, 64 KB aligned ----

namespace {
std::mutex g_vm_lock;
std::map<uint32_t, uint32_t> g_regions;  // base -> size
}  // namespace

W32(kernel32, VirtualAlloc) {
    const uint32_t addr = arg(c, 0), size = (arg(c, 1) + 0xfff) & ~0xfffu, type = arg(c, 2);
    std::lock_guard<std::mutex> l(g_vm_lock);
    if (addr) {
        // Committing part of a region reserved before.
        auto it = g_regions.upper_bound(addr);
        if (it != g_regions.begin() && addr < std::prev(it)->first + std::prev(it)->second)
            return ret(c, addr & ~0xfffu, 4);
        trace("VirtualAlloc at %08x: not in a reserved region", addr);
        set_last_error(487);  // ERROR_INVALID_ADDRESS
        return ret(c, 0, 4);
    }
    const uint32_t p = heap::alloc(mem(), size, 0x10000);
    if (!p) return ret(c, 0, 4);
    std::memset(gptr(p), 0, size);
    g_regions[p] = size;
    trace("VirtualAlloc(%u, %x) = %08x", size, type, p);
    ret(c, p, 4);
}

W32(kernel32, VirtualFree) {
    const uint32_t addr = arg(c, 0), type = arg(c, 2);
    if (type & 0x8000) {  // MEM_RELEASE
        std::lock_guard<std::mutex> l(g_vm_lock);
        if (g_regions.erase(addr)) gfree(addr);
    }
    ret(c, 1, 3);
}

W32(kernel32, VirtualQuery) {
    const uint32_t addr = arg(c, 0), info = arg(c, 1);
    std::memset(gptr(info), 0, 28);
    uint32_t base = addr & ~0xfffu, size = 0x1000;
    {
        std::lock_guard<std::mutex> l(g_vm_lock);
        auto it = g_regions.upper_bound(addr);
        if (it != g_regions.begin() && addr < std::prev(it)->first + std::prev(it)->second)
            base = std::prev(it)->first, size = std::prev(it)->second;
    }
    wr(info, base);
    wr(info + 4, base);
    wr(info + 8, 4);           // PAGE_READWRITE
    wr(info + 12, size);
    wr(info + 16, 0x1000);     // MEM_COMMIT
    wr(info + 20, 4);
    wr(info + 24, 0x20000);    // MEM_PRIVATE
    ret(c, 28, 3);
}

// ---- time ----

namespace {
uint64_t now_100ns() {  // FILETIME: 100 ns since 1601
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return (uint64_t)tv.tv_sec * 10000000ull + (uint64_t)tv.tv_usec * 10 + 116444736000000000ull;
}

uint64_t monotonic_ns() {
    static mach_timebase_info_data_t tb = [] {
        mach_timebase_info_data_t t;
        mach_timebase_info(&t);
        return t;
    }();
    return mach_absolute_time() * tb.numer / tb.denom;
}

void put_systemtime(uint32_t st, uint64_t ft) {
    const time_t secs = (time_t)((ft - 116444736000000000ull) / 10000000ull);
    struct tm tm;
    gmtime_r(&secs, &tm);
    wr16(st, (uint16_t)(tm.tm_year + 1900));
    wr16(st + 2, (uint16_t)(tm.tm_mon + 1));
    wr16(st + 4, (uint16_t)tm.tm_wday);
    wr16(st + 6, (uint16_t)tm.tm_mday);
    wr16(st + 8, (uint16_t)tm.tm_hour);
    wr16(st + 10, (uint16_t)tm.tm_min);
    wr16(st + 12, (uint16_t)tm.tm_sec);
    wr16(st + 14, (uint16_t)((ft / 10000) % 1000));
}

uint64_t get_systemtime(uint32_t st) {
    struct tm tm{};
    tm.tm_year = rd16(st) - 1900;
    tm.tm_mon = rd16(st + 2) - 1;
    tm.tm_mday = rd16(st + 6);
    tm.tm_hour = rd16(st + 8);
    tm.tm_min = rd16(st + 10);
    tm.tm_sec = rd16(st + 12);
    return (uint64_t)timegm(&tm) * 10000000ull + rd16(st + 14) * 10000ull + 116444736000000000ull;
}

int64_t utc_offset_100ns() {
    time_t t = time(nullptr);
    struct tm lt;
    localtime_r(&t, &lt);
    return (int64_t)lt.tm_gmtoff * 10000000ll;
}

uint64_t rd64(uint32_t a) { return rd(a) | (uint64_t)rd(a + 4) << 32; }
void wr64(uint32_t a, uint64_t v) { wr(a, (uint32_t)v), wr(a + 4, (uint32_t)(v >> 32)); }
}  // namespace

W32(kernel32, GetTickCount) { ret(c, (uint32_t)(monotonic_ns() / 1000000), 0); }

// The performance counter as current Windows has it: 10 MHz, counted from
// about when the process started. Host nanoseconds since boot were too large:
// code that converts ticks with ticks * 1000000 / frequency (Revelations' UI)
// overflowed 64 bits after a few hours of uptime, and its animations jumped.
uint64_t qpc_ticks() {
    static const uint64_t start = monotonic_ns();
    return (monotonic_ns() - start) / 100 + 10000000ull;  // starts at one second
}
W32(kernel32, QueryPerformanceFrequency) {
    wr64(arg(c, 0), 10000000ull);
    ret(c, 1, 1);
}

W32(kernel32, QueryPerformanceCounter) {
    wr64(arg(c, 0), qpc_ticks());
    ret(c, 1, 1);
}

W32(kernel32, GetSystemTimeAsFileTime) {
    wr64(arg(c, 0), now_100ns());
    ret(c, 0, 1);
}

W32(kernel32, GetSystemTime) {
    put_systemtime(arg(c, 0), now_100ns());
    ret(c, 0, 1);
}

W32(kernel32, GetLocalTime) {
    put_systemtime(arg(c, 0), now_100ns() + utc_offset_100ns());
    ret(c, 0, 1);
}

W32(kernel32, SystemTimeToFileTime) {
    wr64(arg(c, 1), get_systemtime(arg(c, 0)));
    ret(c, 1, 2);
}

W32(kernel32, FileTimeToSystemTime) {
    put_systemtime(arg(c, 1), rd64(arg(c, 0)));
    ret(c, 1, 2);
}

W32(kernel32, FileTimeToLocalFileTime) {
    wr64(arg(c, 1), rd64(arg(c, 0)) + utc_offset_100ns());
    ret(c, 1, 2);
}

W32(kernel32, LocalFileTimeToFileTime) {
    wr64(arg(c, 1), rd64(arg(c, 0)) - utc_offset_100ns());
    ret(c, 1, 2);
}

W32(kernel32, GetTimeZoneInformation) {
    const uint32_t tz = arg(c, 0);
    std::memset(gptr(tz), 0, 172);
    wr(tz, (uint32_t)(int32_t)(-utc_offset_100ns() / 600000000ll));  // Bias in minutes
    ret(c, 0, 1);  // TIME_ZONE_ID_UNKNOWN
}

}  // namespace scimitar::imports
