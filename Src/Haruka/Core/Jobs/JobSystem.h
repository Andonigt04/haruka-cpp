#pragma once
/**
 * @file JobSystem.h
 * @brief EL ÚNICO SITIO DEL MOTOR QUE TIENE HILOS DE TRABAJO. Un pool para todo el proceso.
 *
 * Antes cada subsistema creaba los suyos: cuatro pools en `Planet.cpp` de `hardware_concurrency()`
 * hilos cada uno, otro en `WorldSystemProvider`, otro en `ImageWriter`, y una docena de
 * `std::async(std::launch::async)` —que en libstdc++ crea un hilo del sistema POR LLAMADA—. Sin
 * nadie que los coordinara: si coincidían un horneado del planeta, el reparto de props y el horneado
 * del cielo había varias veces más hilos que núcleos, y un trabajo que el frame necesitaba ya
 * competía de igual a igual con escribir un PNG.
 *
 * Aquí hay un número FIJO de hilos y tres colas por prioridad:
 *   · Frame      — lo que el frame actual espera (el suelo de colisión cercano).
 *   · Streaming  — cargas y remallados que se notan si tardan, pero no bloquean el frame.
 *   · Background — horneados, escritura de ficheros: lo que puede esperar.
 * Un hilo libre siempre toma de la cola más prioritaria que tenga algo.
 *
 * Y una cola de "APLICAR EN EL HILO PRINCIPAL" (`postToMain` / `drainMain`): un trabajo calcula en un
 * hilo, pero lo que toca la GPU o la escena tiene que hacerse en el principal. El trabajo deja ahí
 * la continuación y la aplicación la ejecuta al empezar el frame.
 *
 * ⚠️ LO QUE NO VA AQUÍ: la física (Jolt corre en UN hilo a propósito, para que el servidor DGS la
 * reproduzca exacta). Un hilo de tiempo real con su propio bucle (la captura del micrófono) tampoco
 * es un "trabajo": se registra con `spawnDedicated` para que conste y se cuente, no para repartirlo.
 */
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

namespace Haruka::Jobs {

enum class Priority : uint8_t { Frame = 0, Streaming = 1, Background = 2 };
inline constexpr int kPriorityCount = 3;

/** @brief Lo que ha pasado por el pool, para tests y para el panel de depuración. */
struct JobStats {
    unsigned workers = 0;                       ///< hilos del pool (fijo)
    unsigned dedicated = 0;                     ///< hilos dedicados vivos (`spawnDedicated`)
    uint64_t executed[kPriorityCount] = {};     ///< trabajos terminados por prioridad (sin ayudantes)
    uint64_t helpers = 0;                       ///< ayudantes de `parallelFor` ejecutados
    size_t   queued[kPriorityCount] = {};       ///< en cola ahora mismo
    unsigned peakActive = 0;                    ///< máximo de trabajos corriendo a la vez (desde `resetWindow`)
    double   maxWaitMs[kPriorityCount] = {};    ///< mayor espera en cola de un TRABAJO, encolar → empezar (desde `resetWindow`)
    /// Lo mismo para los ayudantes de `parallelFor`, aparte: uno que llega tarde espera detrás de sus
    /// hermanos y, al empezar, ya no encuentra nada que hacer. Mezclado con los trabajos de verdad,
    /// inflaba la espera de prioridad Frame del juego (4-7 ms) sin que ningún trabajo urgente esperase.
    double   helperMaxWaitMs = 0.0;
    size_t   mainPending = 0;                   ///< continuaciones esperando a `drainMain`
};

class JobSystem {
public:
    /** @param workers hilos del pool; 0 = `hardware_concurrency() - 1` (el principal ya trabaja),
     *  o lo que diga `HARUKA_JOBS`. Siempre al menos 1. */
    explicit JobSystem(unsigned workers = 0);
    /** Termina lo que quede en cola (los `future` no se rompen) y espera a los hilos. */
    ~JobSystem();
    JobSystem(const JobSystem&) = delete;
    JobSystem& operator=(const JobSystem&) = delete;

    /** @brief Encola `fn` y devuelve su resultado como `future`. Una excepción viaja en el future. */
    template <class F>
    auto submit(Priority prio, F&& fn) -> std::future<std::invoke_result_t<std::decay_t<F>>> {
        using R = std::invoke_result_t<std::decay_t<F>>;
        auto task = std::make_shared<std::packaged_task<R()>>(std::forward<F>(fn));
        std::future<R> fut = task->get_future();
        enqueue(prio, [task] { (*task)(); });
        return fut;
    }

    /** @brief `fn(i)` para cada i de [begin, end), repartido entre el pool Y el hilo que llama.
     *  Bloquea hasta terminar. Se puede llamar desde dentro de un trabajo: quien llama también
     *  toma índices, así que avanza aunque todos los hilos del pool estén ocupados (no se cuelga). */
    void parallelFor(size_t begin, size_t end, const std::function<void(size_t)>& fn,
                     Priority prio = Priority::Streaming);

    /** @brief Deja `fn` para el hilo principal. Seguro desde cualquier hilo. */
    void postToMain(std::function<void()> fn);
    /** @brief Ejecuta las continuaciones pendientes EN EL HILO QUE LLAMA (el principal, al empezar
     *  el frame). Devuelve cuántas ejecutó. Las que se publiquen mientras tanto esperan a la próxima. */
    size_t drainMain();

    /** @brief Un hilo propio de larga vida (un bucle de tiempo real), registrado para que cuente.
     *  `fn` recibe el `stop_token`: hay que salir cuando se pida parada. */
    std::jthread spawnDedicated(std::string name, std::function<void(std::stop_token)> fn);

    unsigned workerCount() const { return (unsigned)m_workers.size(); }
    JobStats stats() const;
    /** @brief Empieza una ventana nueva para `peakActive` y `maxWaitMs` (las de informe periódico). */
    void resetWindow();

private:
    using Task = std::move_only_function<void()>;
    /// El trabajo y CUÁNDO entró en la cola: sin eso no se puede medir lo que el pool promete (que un
    /// trabajo de frame no espera detrás de los de fondo).
    struct Item { Task task; std::chrono::steady_clock::time_point queued; bool helper = false; };
    void enqueue(Priority prio, Task task, bool helper = false);
    void workerLoop(unsigned index);

    mutable std::mutex      m_mutex;
    std::condition_variable m_cv;
    std::deque<Item>        m_queues[kPriorityCount];
    bool                    m_stopping = false;
    std::vector<std::thread> m_workers;

    std::atomic<uint64_t> m_executed[kPriorityCount] = {};
    std::atomic<unsigned> m_active{0}, m_peakActive{0};
    std::atomic<unsigned> m_dedicated{0};
    std::atomic<uint64_t> m_maxWaitUs[kPriorityCount] = {};
    std::atomic<uint64_t> m_helperMaxWaitUs{0}, m_helpers{0};

    mutable std::mutex                m_mainMutex;
    std::vector<std::function<void()>> m_main;
};

/** @brief El pool del proceso. Se crea en el primer uso. Los tests crean su propio `JobSystem`. */
JobSystem& jobs();

} // namespace Haruka::Jobs
