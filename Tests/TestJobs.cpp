/**
 * @file TestJobs.cpp
 * @brief EL POOL CENTRAL DE TRABAJOS (`Core/Jobs/JobSystem.h`): lo que promete, con su contraprueba.
 *
 *   1. Todo lo encolado se ejecuta, el resultado llega por el `future` y una excepción también.
 *   2. HAY TOPE DE HILOS: 64 trabajos que duermen no corren nunca más de `workers` a la vez.
 *      CONTRAPRUEBA: los mismos 64 con `std::async`, que es lo que hacía el motor, sí se disparan.
 *   3. LA PRIORIDAD MANDA: con un solo hilo bloqueado y 50 trabajos de fondo en cola, uno de frame
 *      encolado el ÚLTIMO corre el PRIMERO. CONTRAPRUEBA: encolado como fondo, corre el último.
 *   4. `parallelFor` toca cada índice exactamente una vez, y llamado DESDE un trabajo con el único
 *      hilo del pool ocupado no se cuelga (quien llama también trabaja).
 *   5. Lo publicado con `postToMain` no corre hasta `drainMain`, y corre en el hilo que la llama.
 *   6. Al destruir el pool, lo que quedaba en cola se ejecuta: ningún `future` se rompe.
 */
#include "TestCommon.h"
#include "Haruka/Core/Jobs/JobSystem.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <future>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace Haruka::Jobs;
using namespace std::chrono_literals;

namespace {
/// Cuenta cuántos trabajos corren A LA VEZ: el máximo es lo que hay que comparar con el tope.
struct Concurrency {
    std::atomic<int> now{0}, peak{0};
    void enter() { const int n = ++now; int p = peak.load(); while (n > p && !peak.compare_exchange_weak(p, n)) {} }
    void leave() { --now; }
};
}

void test_jobs_pool() {
    beginTest("trabajos: pool central con tope de hilos, prioridad y continuaciones");

    // 1. resultados y excepciones
    {
        JobSystem js(4);
        auto a = js.submit(Priority::Streaming, [] { return 6 * 7; });
        auto b = js.submit(Priority::Background, [] () -> int { throw std::runtime_error("x"); });
        CHECK(a.get() == 42, "el resultado llega por el future");
        bool threw = false;
        try { b.get(); } catch (const std::runtime_error&) { threw = true; }
        CHECK(threw, "una excepcion dentro del trabajo llega al future");
    }

    // 2. tope de hilos, y la contraprueba con std::async
    {
        const unsigned W = 4;
        JobSystem js(W);
        Concurrency c;
        std::vector<std::future<void>> fs;
        for (int i = 0; i < 64; ++i)
            fs.push_back(js.submit(Priority::Background, [&] { c.enter(); std::this_thread::sleep_for(2ms); c.leave(); }));
        for (auto& f : fs) f.get();
        Concurrency ca;
        std::vector<std::future<void>> fa;
        for (int i = 0; i < 64; ++i)
            fa.push_back(std::async(std::launch::async, [&] { ca.enter(); std::this_thread::sleep_for(2ms); ca.leave(); }));
        for (auto& f : fa) f.get();
        std::printf("    64 trabajos de 2 ms: pool de %u -> %d a la vez · std::async -> %d a la vez\n",
                    W, c.peak.load(), ca.peak.load());
        CHECK(c.peak.load() <= (int)W, "el pool nunca corre mas trabajos que hilos tiene");
        CHECK(js.stats().peakActive <= W, "y sus estadisticas dicen lo mismo");
        CHECK(ca.peak.load() > (int)W, "CONTRAPRUEBA: std::async (lo de antes) supera el tope");
    }

    // 3. prioridad: un solo hilo, bloqueado; 50 de fondo y luego 1 de frame
    auto orderOf = [](Priority last) {
        JobSystem js(1);
        std::promise<void> gate;
        std::shared_future<void> open = gate.get_future().share();
        js.submit(Priority::Background, [open] { open.wait(); });   // ocupa el único hilo
        std::this_thread::sleep_for(20ms);                          // que lo tome antes de encolar
        std::atomic<int> seq{0};
        int lastPos = -1;
        std::vector<std::future<void>> fs;
        for (int i = 0; i < 50; ++i) fs.push_back(js.submit(Priority::Background, [&] { ++seq; }));
        fs.push_back(js.submit(last, [&] { lastPos = seq++; }));
        gate.set_value();
        for (auto& f : fs) f.get();
        return lastPos;
    };
    const int posFrame = orderOf(Priority::Frame), posBg = orderOf(Priority::Background);
    std::printf("    encolado el ultimo tras 50 de fondo: como Frame corre en la posicion %d · como Background en la %d\n",
                posFrame, posBg);
    CHECK(posFrame == 0, "un trabajo de Frame adelanta a los 50 de fondo que habia en cola");
    CHECK(posBg == 50, "CONTRAPRUEBA: el mismo trabajo como Background espera su turno (el ultimo)");

    // 3b. la espera en cola se MIDE (es lo que el informe del juego enseña)
    {
        JobSystem js(1);
        js.submit(Priority::Frame, [] {}).get();                          // hilo libre: casi no espera
        const double libre = js.stats().maxWaitMs[(int)Priority::Frame];
        js.resetWindow();
        CHECK(js.stats().maxWaitMs[(int)Priority::Frame] == 0.0, "resetWindow pone a cero la espera maxima");
        js.submit(Priority::Background, [] { std::this_thread::sleep_for(30ms); });   // ocupa el hilo
        std::this_thread::sleep_for(5ms);
        js.submit(Priority::Frame, [] {}).get();
        const double ocupado = js.stats().maxWaitMs[(int)Priority::Frame];
        std::printf("    espera de un trabajo Frame: hilo libre %.2f ms · detras de una pieza de 30 ms %.2f ms\n", libre, ocupado);
        CHECK(ocupado >= 15.0, "la espera de un Frame detras de una pieza en curso se registra (>= 15 ms)");
        CHECK(libre < 10.0, "CONTRAPRUEBA: con el hilo libre la espera registrada es pequena");

        // Los ayudantes de parallelFor van APARTE: con el único hilo ocupado, quien llama hace todo el
        // trabajo y el ayudante llega tarde. Esa espera no es la de ningún trabajo de verdad.
        js.resetWindow();
        js.submit(Priority::Background, [] { std::this_thread::sleep_for(30ms); });
        std::this_thread::sleep_for(5ms);
        std::atomic<int> n{0};
        js.parallelFor(0, 4, [&](size_t) { ++n; }, Priority::Frame);
        std::this_thread::sleep_for(40ms);   // que el ayudante llegue a correr (y no encuentre nada)
        const JobStats st = js.stats();
        std::printf("    parallelFor con el hilo ocupado: espera de trabajos Frame %.2f ms · de ayudantes %.2f ms (%llu)\n",
                    st.maxWaitMs[(int)Priority::Frame], st.helperMaxWaitMs, (unsigned long long)st.helpers);
        CHECK(n.load() == 4, "parallelFor termina aunque el unico hilo este ocupado");
        CHECK(st.helpers >= 1 && st.helperMaxWaitMs >= 15.0, "la espera del ayudante tardio se cuenta como de ayudante");
        CHECK(st.maxWaitMs[(int)Priority::Frame] == 0.0,
              "CONTRAPRUEBA: y no infla la espera de los trabajos Frame (ninguno esperaba)");
    }

    // 4. parallelFor: cada índice una vez, y sin colgarse dentro de un trabajo
    {
        JobSystem js(4);
        std::vector<std::atomic<int>> hits(10000);
        js.parallelFor(0, hits.size(), [&](size_t i) { ++hits[i]; });
        bool once = true;
        for (auto& h : hits) once = once && h.load() == 1;
        CHECK(once, "parallelFor toca cada indice exactamente una vez (10 000)");

        JobSystem one(1);
        std::atomic<int> sum{0};
        auto f = one.submit(Priority::Streaming, [&] { one.parallelFor(0, 100, [&](size_t) { ++sum; }); });
        const bool finished = f.wait_for(5s) == std::future_status::ready;
        CHECK(finished && sum.load() == 100,
              "parallelFor DESDE el unico hilo del pool termina (quien llama tambien trabaja)");
    }

    // 5. continuaciones al hilo principal
    {
        JobSystem js(2);
        std::atomic<int> ran{0};
        std::thread::id where;
        js.submit(Priority::Streaming, [&] { js.postToMain([&] { ++ran; where = std::this_thread::get_id(); }); }).get();
        CHECK(ran.load() == 0, "postToMain no ejecuta nada por su cuenta");
        CHECK(js.drainMain() == 1 && ran.load() == 1, "drainMain ejecuta la continuacion");
        CHECK(where == std::this_thread::get_id(), "y la ejecuta en el hilo que llama a drainMain");
    }

    // 6. al destruirse no rompe futures pendientes
    {
        std::vector<std::future<int>> fs;
        {
            JobSystem js(1);
            for (int i = 0; i < 20; ++i) fs.push_back(js.submit(Priority::Background, [i] { std::this_thread::sleep_for(1ms); return i; }));
        }
        bool all = true;
        for (int i = 0; i < 20; ++i) all = all && fs[i].get() == i;
        CHECK(all, "destruir el pool termina lo que habia en cola: ningun future roto");
    }
}

/**
 * @brief CONTENCIÓN: lo que el pool central arregla, medido. CARGA SINTÉTICA, y se dice.
 *
 * El problema que motivó el pool no se ve en ningún banco que corra una cosa cada vez: aparece cuando
 * coinciden cargas. Aquí se recrea su FORMA: tres "horneados" a la vez, cada uno troceado en piezas
 * de ~2 ms de cálculo puro, y mientras tanto 40 trabajos URGENTES (lo que el frame espera, como la
 * malla del suelo cercano) cada 5 ms; cada urgente hace a su vez 2 ms de cálculo. Se mide cuánto
 * tarda en EMPEZAR y cuánto en TERMINAR: el coste de tener más hilos que núcleos no está tanto en
 * arrancar (Linux da turno pronto a un hilo nuevo) como en que el urgente, ya en marcha, reparte la
 * CPU con todos los demás y sus 2 ms se estiran.
 *
 *   · ANTES: cada horneado con SU pool de `hardware_concurrency()` hilos (lo que hacen los pools de
 *     `Planet.cpp`), y el urgente con `std::async`. Tres pools de N hilos en N núcleos: el urgente
 *     compite de igual a igual con 3N hilos.
 *   · AHORA: todo al pool central; los horneados como Background, el urgente como Frame.
 *   · CONTRAPRUEBA: igual que AHORA pero el urgente como Background. Separa lo que aporta la
 *     PRIORIDAD de lo que aporta tener un solo pool.
 *
 * ⚠️ El pool no expropia: un urgente espera a que un hilo TERMINE su pieza en curso. Por eso el límite
 * de AHORA es el tamaño de pieza, y por eso el trabajo de fondo tiene que ir troceado.
 */
void test_jobs_contention() {
    beginTest("trabajos: un urgente no compite con los horneados (carga sintetica, 3 horneados a la vez)");
    using clock = std::chrono::steady_clock;
    const unsigned N = std::max(2u, std::thread::hardware_concurrency());
    // ~2 ms de cálculo puro, sin dormir: dormir no ocupa núcleo y escondería la contención.
    auto piece = [] {
        volatile double x = 1.0;
        const auto t0 = clock::now();
        while (clock::now() - t0 < std::chrono::microseconds(2000)) x = x * 1.0000001 + 1e-9;
    };
    constexpr int kBakes = 3, kPiecesPerBake = 400, kUrgent = 40;

    // Latencias (ms) desde el submit, ordenadas: p50 y p95.
    auto pct = [](std::vector<double> v, double p) {
        std::sort(v.begin(), v.end());
        return v.empty() ? 0.0 : v[std::min(v.size() - 1, (size_t)(p * (double)(v.size() - 1) + 0.5))];
    };
    struct Lat { std::vector<double> start, end; };
    auto urgentLoop = [&](auto launchUrgent) {
        Lat lat{ std::vector<double>(kUrgent, 0.0), std::vector<double>(kUrgent, 0.0) };
        std::vector<std::future<void>> fs;
        for (int u = 0; u < kUrgent; ++u) {
            const auto sub = clock::now();
            fs.push_back(launchUrgent([&lat, u, sub, &piece] {
                lat.start[u] = std::chrono::duration<double, std::milli>(clock::now() - sub).count();
                piece();   // el urgente también CALCULA: lo que se estira con la contención
                lat.end[u] = std::chrono::duration<double, std::milli>(clock::now() - sub).count();
            }));
            std::this_thread::sleep_for(5ms);
        }
        for (auto& f : fs) f.get();
        return lat;
    };

    // ANTES: tres pools propios de N hilos, urgentes con std::async.
    Lat before;
    {
        std::atomic<int> next[kBakes] = {};
        std::vector<std::thread> threads;
        std::atomic<bool> stop{false};
        for (int b = 0; b < kBakes; ++b)
            for (unsigned t = 0; t < N; ++t)
                threads.emplace_back([&, b] { while (!stop && next[b]++ < kPiecesPerBake) piece(); });
        std::this_thread::sleep_for(20ms);
        before = urgentLoop([](auto fn) { return std::async(std::launch::async, fn); });
        stop = true;
        for (auto& t : threads) t.join();
    }
    // AHORA y CONTRAPRUEBA: todo al pool central.
    auto pooled = [&](Priority urgentPrio) {
        JobSystem js(N > 1 ? N - 1 : 1);
        std::vector<std::future<void>> bake;
        for (int b = 0; b < kBakes; ++b)
            for (int p = 0; p < kPiecesPerBake; ++p) bake.push_back(js.submit(Priority::Background, piece));
        std::this_thread::sleep_for(20ms);
        auto lat = urgentLoop([&](auto fn) { return js.submit(urgentPrio, fn); });
        for (auto& f : bake) f.get();
        return lat;
    };
    const Lat now = pooled(Priority::Frame);
    const Lat asBg = pooled(Priority::Background);

    std::printf("    %u nucleos · 3 horneados de %d piezas de 2 ms · 40 urgentes de 2 ms (ms desde el submit):\n",
                N, kPiecesPerBake);
    auto row = [&](const char* name, const Lat& l) {
        std::printf("      %-40s empieza p50 %6.2f p95 %6.2f · TERMINA p50 %6.2f p95 %6.2f peor %6.2f\n", name,
                    pct(l.start, 0.5), pct(l.start, 0.95), pct(l.end, 0.5), pct(l.end, 0.95), pct(l.end, 1.0));
    };
    row("antes (3 pools propios + std::async)", before);
    row("ahora (pool central, urgente = Frame)", now);
    row("contra (pool central, urgente = fondo)", asBg);
    // El trabajo del urgente son 2 ms: lo minimo que puede tardar en TERMINAR.
    CHECK(pct(now.end, 0.95) < pct(before.end, 0.95),
          "con el pool y prioridad Frame, el urgente TERMINA antes (p95) que con pools propios");
    CHECK(pct(now.end, 0.95) <= 2.0 + 4.0,
          "y su p95 queda acotado: sus 2 ms + esperar una pieza en curso (2 ms) + margen");
    CHECK(pct(asBg.end, 0.95) > 4.0 * pct(now.end, 0.95),
          "CONTRAPRUEBA: el mismo urgente como fondo espera detras de los horneados (la mejora es la PRIORIDAD)");
}
