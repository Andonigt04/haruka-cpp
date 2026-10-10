/**
 * @file Jobs.cppm
 * @brief PILOTO DE MÓDULOS C++20: `import Haruka.Core.Jobs;` en vez de `#include "Haruka/Core/Jobs/JobSystem.h"`.
 *
 * Existe para medir si el camino del estándar a "qué es público" (`export`, comprobado por el
 * compilador) funciona aquí —GCC 16, CMake 4.3, Ninja— antes de decidir nada para el resto del motor.
 * No lo usa el motor ni el juego: solo `tests/ModulePilot.cpp` (ver `cmake/HarukaTests.cmake`).
 *
 * Es un módulo ENVOLTORIO, el patrón del módulo `std` de libstdc++: el header entra en el fragmento
 * global y se exportan sus nombres con `using`. La implementación sigue en `JobSystem.cpp` y en la
 * biblioteca, así que el header y el módulo no pueden separarse.
 *
 * Lo que NO se exporta no existe para quien importa: `internalOnly` está aquí al lado y un `import`
 * no lo ve (lo comprueba `tests/ModulePilotHidden.cpp`, que tiene que FALLAR al compilar).
 */
module;
#include "Haruka/Core/Jobs/JobSystem.h"

export module Haruka.Core.Jobs;

export namespace Haruka::Core::Jobs {
    using Haruka::Core::Jobs::JobSystem;
    using Haruka::Core::Jobs::Priority;
    using Haruka::Core::Jobs::JobStats;
    using Haruka::Core::Jobs::kPriorityCount;
    using Haruka::Core::Jobs::jobs;
}

namespace Haruka::Core::Jobs {
    /// Interno del módulo: NO exportado. Existe solo para la contraprueba.
    int internalOnly() { return 7; }
}
