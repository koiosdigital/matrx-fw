#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace raii {

class MutexGuard {
public:
    explicit MutexGuard(SemaphoreHandle_t mutex, TickType_t timeout = portMAX_DELAY)
        : mutex_(mutex)
        , acquired_(mutex ? (xSemaphoreTake(mutex, timeout) == pdTRUE) : false) {}

    ~MutexGuard() {
        if (acquired_) {
            xSemaphoreGive(mutex_);
        }
    }

    MutexGuard(const MutexGuard&) = delete;
    MutexGuard& operator=(const MutexGuard&) = delete;

    explicit operator bool() const { return acquired_; }
    bool acquired() const { return acquired_; }

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

class SemaphoreGiver {
public:
    explicit SemaphoreGiver(SemaphoreHandle_t sem) : sem_(sem) {}

    ~SemaphoreGiver() {
        if (sem_) {
            xSemaphoreGive(sem_);
        }
    }

    SemaphoreGiver(const SemaphoreGiver&) = delete;
    SemaphoreGiver& operator=(const SemaphoreGiver&) = delete;

    void cancel() { sem_ = nullptr; }

private:
    SemaphoreHandle_t sem_;
};

}  // namespace raii
