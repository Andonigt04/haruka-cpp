#include "Haruka/Core/Jobs/JobSystem.h"
#include "Haruka/Core/Logger.h"

#include <algorithm>
#include <cstdlib>
#include <pthread.h>

namespace Haruka::Core::Jobs {

namespace {
unsigned defaultWorkers() {
    if (const char* e = std::getenv("HARUKA_JOBS"); e && *e) {
        const int n = std::atoi(e);
        if (n > 0) return (unsigned)n;
    }
    const unsigned hw = std::thread::hardware_concurrency();
    return hw > 1 ? hw - 1 : 1;   // el hilo principal también trabaja: no se le cuenta un núcleo
}

void nameThread(const std::string& name) {
    // Linux limita el nombre a 15 caracteres más el nulo; uno más largo hace fallar la llamada.
    pthread_setname_np(pthread_self(), name.substr(0, 15).c_str());
}
} // namespace

JobSystem::JobSystem(unsigned workers) {
    const unsigned n = workers ? workers : defaultWorkers();
    m_workers.reserve(n);
    for (unsigned i = 0; i < n; ++i) m_workers.emplace_back([this, i] { workerLoop(i); });
}

JobSystem::~JobSystem() {
    {
        std::lock_guard lk(m_mutex);
        m_stopping = true;
    }
    m_cv.notify_all();
    for (std::thread& t : m_workers) t.join();
}

void JobSystem::enqueue(Priority prio, Task task, bool helper) {
    {
        std::lock_guard lk(m_mutex);
        m_queues[(int)prio].push_back({ std::move(task), std::chrono::steady_clock::now(), helper });
    }
    m_cv.notify_one();
}

void JobSystem::workerLoop(unsigned index) {
    nameThread("haruka-job-" + std::to_string(index));
    for (;;) {
        Task task;
        std::chrono::steady_clock::time_point queued;
        bool helper = false;
        int prio = -1;
        {
            std::unique_lock lk(m_mutex);
            m_cv.wait(lk, [this] {
                if (m_stopping) return true;
                for (const auto& q : m_queues) if (!q.empty()) return true;
                return false;
            });
            for (int p = 0; p < kPriorityCount; ++p) {
                if (!m_queues[p].empty()) {
                    task = std::move(m_queues[p].front().task);
                    queued = m_queues[p].front().queued;
                    helper = m_queues[p].front().helper;
                    m_queues[p].pop_front();
                    prio = p;
                    break;
                }
            }
            // ⚠️ AL PARAR SE VACÍAN LAS COLAS ANTES DE SALIR: un trabajo encolado tiene un `future`
            // que alguien puede estar esperando, y descartarlo lo rompería (broken_promise).
            if (prio < 0) return;   // parando y sin nada en cola
        }
        const uint64_t waitUs = (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - queued).count();
        std::atomic<uint64_t>& slot = helper ? m_helperMaxWaitUs : m_maxWaitUs[prio];
        uint64_t mw = slot.load();
        while (waitUs > mw && !slot.compare_exchange_weak(mw, waitUs)) {}
        const unsigned now = m_active.fetch_add(1) + 1;
        unsigned peak = m_peakActive.load();
        while (now > peak && !m_peakActive.compare_exchange_weak(peak, now)) {}
        task();
        m_active.fetch_sub(1);
        if (helper) m_helpers.fetch_add(1); else m_executed[prio].fetch_add(1);
    }
}

void JobSystem::parallelFor(size_t begin, size_t end, const std::function<void(size_t)>& fn,
                            Priority prio) {
    if (end <= begin) return;
    const size_t n = end - begin;
    // Estado compartido en el montón: un ayudante que arranca TARDE (cuando quien llama ya terminó y
    // salió de esta función) solo lo encuentra agotado y sale, pero tiene que poder leerlo.
    struct Shared {
        std::atomic<size_t> next{0}, done{0};
        size_t n = 0, begin = 0;
        std::function<void(size_t)> fn;
        std::mutex m;
        std::condition_variable cv;
    };
    auto sh = std::make_shared<Shared>();
    sh->n = n; sh->begin = begin; sh->fn = fn;

    auto drain = [](Shared& s) {
        for (size_t i; (i = s.next.fetch_add(1)) < s.n;) {
            s.fn(s.begin + i);
            if (s.done.fetch_add(1) + 1 == s.n) {
                std::lock_guard lk(s.m);
                s.cv.notify_all();
            }
        }
    };
    const size_t helpers = std::min<size_t>(m_workers.size(), n - 1);
    for (size_t h = 0; h < helpers; ++h) enqueue(prio, [sh, drain] { drain(*sh); }, /*helper=*/true);
    drain(*sh);   // quien llama también trabaja: por eso no se cuelga dentro de un trabajo
    std::unique_lock lk(sh->m);
    sh->cv.wait(lk, [&] { return sh->done.load() == sh->n; });
}

void JobSystem::postToMain(std::function<void()> fn) {
    std::lock_guard lk(m_mainMutex);
    m_main.push_back(std::move(fn));
}

size_t JobSystem::drainMain() {
    std::vector<std::function<void()>> batch;
    {
        std::lock_guard lk(m_mainMutex);
        batch.swap(m_main);
    }
    for (auto& f : batch) f();
    return batch.size();
}

std::jthread JobSystem::spawnDedicated(std::string name, std::function<void(std::stop_token)> fn) {
    m_dedicated.fetch_add(1);
    HARUKA_LOGI("Jobs", "hilo dedicado '%s' (no es del pool: un bucle propio de larga vida)", name.c_str());
    return std::jthread([this, name = std::move(name), fn = std::move(fn)](std::stop_token st) {
        nameThread(name);
        fn(st);
        m_dedicated.fetch_sub(1);
    });
}

JobStats JobSystem::stats() const {
    JobStats s;
    s.workers = (unsigned)m_workers.size();
    s.dedicated = m_dedicated.load();
    s.peakActive = m_peakActive.load();
    {
        std::lock_guard lk(m_mutex);
        for (int p = 0; p < kPriorityCount; ++p) s.queued[p] = m_queues[p].size();
    }
    for (int p = 0; p < kPriorityCount; ++p) {
        s.executed[p] = m_executed[p].load();
        s.maxWaitMs[p] = (double)m_maxWaitUs[p].load() / 1000.0;
    }
    s.helpers = m_helpers.load();
    s.helperMaxWaitMs = (double)m_helperMaxWaitUs.load() / 1000.0;
    {
        std::lock_guard lk(m_mainMutex);
        s.mainPending = m_main.size();
    }
    return s;
}

void JobSystem::resetWindow() {
    m_peakActive.store(m_active.load());
    for (auto& w : m_maxWaitUs) w.store(0);
    m_helperMaxWaitUs.store(0);
}

JobSystem& jobs() {
    static JobSystem s_jobs;
    return s_jobs;
}

} // namespace Haruka::Core::Jobs
