#pragma once
#include <queue>
#include <mutex>
#include <condition_variable>
#include <memory>
#include <functional>
#include <atomic>
#include <optional>
#include <vector>
#include <type_traits>

namespace Haruka {

// Base event type. Extend for custom events.
struct Event {
    virtual ~Event() = default;
};

class EventManager {
public:
    using EventPtr = std::shared_ptr<Event>;
    using Handler  = std::function<void(const EventPtr&)>;

    // Post an event from any thread; wakes blocked wait() callers.
    void post(EventPtr event) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.push(std::move(event));
        }
        cv_.notify_one();
    }

    // Poll for one event (non-blocking).
    std::optional<EventPtr> poll() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) return std::nullopt;
        auto evt = queue_.front();
        queue_.pop();
        return evt;
    }

    // Block until an event is available.
    EventPtr wait() {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [&]{ return !queue_.empty(); });
        auto evt = queue_.front();
        queue_.pop();
        return evt;
    }

    // Register a typed handler.  Called from the main thread before dispatch().
    // T must derive from Event.
    template<typename T>
    void subscribe(std::function<void(const T&)> handler) {
        static_assert(std::is_base_of<Event, T>::value, "T must derive from Event");
        handlers_.push_back([h = std::move(handler)](const EventPtr& evt) {
            if (auto typed = std::dynamic_pointer_cast<T>(evt)) {
                h(*typed);
            }
        });
    }

    // Drain the queue and call all matching handlers (main-thread only).
    void dispatch() {
        std::queue<EventPtr> local;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            std::swap(local, queue_);
        }
        while (!local.empty()) {
            auto& evt = local.front();
            for (auto& h : handlers_) h(evt);
            local.pop();
        }
    }

    // Remove all queued events without calling handlers.
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        while (!queue_.empty()) queue_.pop();
    }

    // Remove all registered handlers.
    void clearHandlers() { handlers_.clear(); }

private:
    std::queue<EventPtr> queue_;
    std::mutex mutex_;
    std::condition_variable cv_;
    // Handlers are registered at init time; not thread-safe to modify concurrently.
    std::vector<Handler> handlers_;
};

} // namespace Haruka
