#pragma once

#include <cstddef>
#include <atomic>

namespace Haruka { namespace Cache {

class MemoryBudget {
public:
    explicit MemoryBudget(size_t maxBytes = 128 * 1024 * 1024)
        : m_budget(maxBytes) {}

    void allocate(size_t bytes) {
        m_used.fetch_add(bytes, std::memory_order_relaxed);
    }

    void deallocate(size_t bytes) {
        m_used.fetch_sub(bytes, std::memory_order_relaxed);
    }

    void setBudget(size_t bytes) {
        m_budget = bytes;
    }

    void resetUsage() {
        m_used.store(0, std::memory_order_relaxed);
    }

    size_t budget() const { return m_budget; }
    size_t used() const { return m_used.load(std::memory_order_relaxed); }
    bool isOverBudget() const { return used() > budget(); }

private:
    std::atomic<size_t> m_used{0};
    size_t m_budget;
};

}} // namespace Haruka::Cache