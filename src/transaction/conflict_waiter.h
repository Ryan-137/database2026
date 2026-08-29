#pragma once

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <pthread.h>

class ConflictWaiter {
public:
    ConflictWaiter() {
        pthread_mutex_init(&wait_mutex_, nullptr);
        pthread_cond_init(&cv_, nullptr);
    }

    ~ConflictWaiter() {
        pthread_cond_destroy(&cv_);
        pthread_mutex_destroy(&wait_mutex_);
    }

    ConflictWaiter(const ConflictWaiter &) = delete;
    ConflictWaiter &operator=(const ConflictWaiter &) = delete;

    std::uint64_t CurrentEpochAtomic() const {
        return epoch_.load(std::memory_order_acquire);
    }

    void NotifyRelease() {
        epoch_.fetch_add(1, std::memory_order_release);
        pthread_cond_broadcast(&cv_);
    }

    bool WaitForRelease(std::uint64_t observed_epoch, std::chrono::milliseconds timeout) {
        if (CurrentEpochAtomic() != observed_epoch) {
            return true;
        }
        auto deadline = std::chrono::system_clock::now() + timeout;
        auto deadline_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            deadline.time_since_epoch()).count();
        timespec ts{};
        ts.tv_sec = static_cast<time_t>(deadline_ns / 1000000000);
        ts.tv_nsec = static_cast<long>(deadline_ns % 1000000000);

        pthread_mutex_lock(&wait_mutex_);
        bool released = false;
        while (!(released = (epoch_.load(std::memory_order_acquire) != observed_epoch))) {
            int rc = pthread_cond_timedwait(&cv_, &wait_mutex_, &ts);
            if (rc == ETIMEDOUT) {
                released = epoch_.load(std::memory_order_acquire) != observed_epoch;
                break;
            }
        }
        pthread_mutex_unlock(&wait_mutex_);
        return released;
    }

private:
    pthread_mutex_t wait_mutex_{};
    pthread_cond_t cv_{};
    std::atomic<std::uint64_t> epoch_{0};
};

class ConflictRetryStats {
public:
    void RecordAttempt() { conflict_retry_attempts.fetch_add(1, std::memory_order_relaxed); }
    void RecordSuccess() { conflict_retry_success.fetch_add(1, std::memory_order_relaxed); }
    void RecordExhausted() { conflict_retry_exhausted.fetch_add(1, std::memory_order_relaxed); }
    void RecordTrueFcwAbort() { true_fcw_aborts.fetch_add(1, std::memory_order_relaxed); }

    void PrintToStderr() const {
    }

    std::atomic<std::uint64_t> conflict_retry_attempts{0};
    std::atomic<std::uint64_t> conflict_retry_success{0};
    std::atomic<std::uint64_t> conflict_retry_exhausted{0};
    std::atomic<std::uint64_t> true_fcw_aborts{0};
};

inline ConflictWaiter &GlobalConflictWaiter() {
    static ConflictWaiter waiter;
    return waiter;
}

inline ConflictRetryStats &GlobalConflictRetryStats() {
    static ConflictRetryStats stats;
    return stats;
}
