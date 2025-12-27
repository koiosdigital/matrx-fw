#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace raii {

// RAII wrapper for FreeRTOS mutex/semaphore
// Automatically releases on destruction
class MutexGuard {
public:
    explicit MutexGuard(SemaphoreHandle_t mutex, TickType_t timeout = portMAX_DELAY)
        : mutex_(mutex)
        , acquired_(xSemaphoreTake(mutex, timeout) == pdTRUE) {}

    ~MutexGuard() {
        if (acquired_) {
            xSemaphoreGive(mutex_);
        }
    }

    // Non-copyable
    MutexGuard(const MutexGuard&) = delete;
    MutexGuard& operator=(const MutexGuard&) = delete;

    // Check if mutex was successfully acquired
    explicit operator bool() const { return acquired_; }
    bool acquired() const { return acquired_; }

    // Manually release the mutex early (before destruction)
    void release() {
        if (acquired_) {
            xSemaphoreGive(mutex_);
            acquired_ = false;
        }
    }

private:
    SemaphoreHandle_t mutex_;
    bool acquired_;
};

// RAII wrapper that automatically gives a semaphore on destruction
// Useful for signaling completion
class SemaphoreGiver {
public:
    explicit SemaphoreGiver(SemaphoreHandle_t sem) : sem_(sem) {}

    ~SemaphoreGiver() {
        if (sem_) {
            xSemaphoreGive(sem_);
        }
    }

    // Non-copyable
    SemaphoreGiver(const SemaphoreGiver&) = delete;
    SemaphoreGiver& operator=(const SemaphoreGiver&) = delete;

    // Allow canceling the give
    void cancel() { sem_ = nullptr; }

private:
    SemaphoreHandle_t sem_;
};

}  // namespace raii
