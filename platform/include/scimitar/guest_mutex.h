#pragma once
// A guest lock (pthread mutex on the Mac ports, CRITICAL_SECTION on the
// Windows ones): recursive, as both are used here. std::recursive_mutex's
// try_lock took Darwin's slow path whenever the mutex was held, and the games
// spin on trylock (Brotherhood's Gear::AdaptiveLock tries 4096 times before
// it blocks; task threads poll the scheduler's lock), which made that path
// ~10% of Brotherhood's main thread. Here a held lock fails a trylock with one
// plain load; lock spins a little, then sleeps on the owner word.
#include <atomic>
#include <cstdint>

namespace scimitar {

class GuestMutex {
public:
    bool try_lock() {
        const uint32_t me = self();
        uint32_t cur = owner_.load(std::memory_order_relaxed);
        if (cur == me) return ++count_, true;
        if (cur != 0 || !owner_.compare_exchange_strong(cur, me, std::memory_order_acquire)) return false;
        count_ = 1;
        return true;
    }
    void lock() {
        const uint32_t me = self();
        uint32_t cur = owner_.load(std::memory_order_relaxed);
        if (cur == me) return void(++count_);
        for (int i = 0; i < 128; i++) {
            if (cur == 0 && owner_.compare_exchange_weak(cur, me, std::memory_order_acquire)) return void(count_ = 1);
#if defined(__aarch64__)
            __builtin_arm_yield();
#endif
            cur = owner_.load(std::memory_order_relaxed);
        }
        waiters_.fetch_add(1, std::memory_order_seq_cst);
        for (;;) {
            cur = 0;
            if (owner_.compare_exchange_strong(cur, me, std::memory_order_seq_cst)) break;
            owner_.wait(cur, std::memory_order_seq_cst);
        }
        waiters_.fetch_sub(1, std::memory_order_relaxed);
        count_ = 1;
    }
    void unlock() {
        if (--count_) return;
        owner_.store(0, std::memory_order_seq_cst);
        if (waiters_.load(std::memory_order_seq_cst)) owner_.notify_one();
    }

private:
    // A nonzero identity per host thread.
    static uint32_t self() {
        static std::atomic<uint32_t> next{1};
        thread_local const uint32_t id = next.fetch_add(1, std::memory_order_relaxed);
        return id;
    }
    std::atomic<uint32_t> owner_{0};  // self() of the holder, 0 when free
    std::atomic<uint32_t> waiters_{0};
    uint32_t count_ = 0;  // only touched by the holder
};

}  // namespace scimitar
