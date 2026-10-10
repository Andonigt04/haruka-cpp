/**
 * @file ModulePilot.cpp
 * @brief El piloto de módulos desde el lado de quien IMPORTA: ningún header del motor, solo `import`.
 *
 * Comprueba que lo exportado por `Haruka.Jobs` se puede usar entero —una plantilla (`submit`), un
 * enum, el singleton— y que el resultado es el mismo que por el header. La contraprueba de que lo NO
 * exportado no se ve es `ModulePilotHidden.cpp` (tiene que fallar al compilar).
 */
#include <atomic>
#include <cstdio>
#include <future>

import Haruka.Core.Jobs;

int main() {
    int fails = 0;
    auto check = [&](bool ok, const char* what) {
        std::printf("  %s %s\n", ok ? "OK  " : "FAIL", what);
        if (!ok) ++fails;
    };
    using namespace Haruka::Core::Jobs;

    JobSystem js(2);
    auto f = js.submit(Priority::Frame, [] { return 6 * 7; });
    check(f.get() == 42, "submit (una plantilla del header) funciona a traves de import");

    std::atomic<int> n{0};
    js.parallelFor(0, 100, [&](size_t) { ++n; });
    check(n.load() == 100, "parallelFor toca cada indice una vez");
    check(js.stats().workers == 2 && kPriorityCount == 3, "JobStats y las constantes exportadas son visibles");
    check(jobs().workerCount() >= 1, "el pool del proceso (jobs()) es accesible");

    std::printf("== piloto de modulos: %s ==\n", fails ? "FALLA" : "OK");
    return fails ? 1 : 0;
}
