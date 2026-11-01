#pragma once

#include "native-resource-state.h"

#include <cassert>
#include <memory>
#include <optional>
#include <vector>

inline bool NativePreparationScratchEnabled() {
    return kyty_local_preparation_scratch_mode.load(std::memory_order_relaxed) != 0;
}

// Reuse storage, never values. Every caller resets logical state before use. A
// separate slot for each live invocation preserves callback reentrancy; heap
// ownership keeps outer references stable if a nested invocation grows the pool.
template <typename T>
class NativePreparationScratch {
    struct Pool {
        std::vector<std::unique_ptr<T>> slots;
        size_t depth = 0;
    };
    static Pool& ThreadPool() {
        thread_local Pool pool;
        return pool;
    }
    std::optional<T> local;
    Pool* pool = nullptr;
    T* value = nullptr;
    size_t depth = 0;
public:
    explicit NativePreparationScratch(bool enabled = NativePreparationScratchEnabled()) {
        if (!enabled) {
            value = &local.emplace();
            return;
        }
        pool = &ThreadPool();
        depth = pool->depth;
        if (depth == pool->slots.size()) {
            pool->slots.push_back(std::make_unique<T>());
        }
        value = pool->slots[depth].get();
        ++pool->depth;
    }
    ~NativePreparationScratch() {
        if (pool != nullptr) {
            assert(pool->depth == depth + 1);
            --pool->depth;
        }
    }
    NativePreparationScratch(const NativePreparationScratch&) = delete;
    NativePreparationScratch& operator=(const NativePreparationScratch&) = delete;
    T& Get() { return *value; }
};
