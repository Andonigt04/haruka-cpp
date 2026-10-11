/**
 * @file HarukaTests.cpp
 * @brief Punto de entrada del banco de pruebas del MOTOR, HEADLESS (CPU only).
 *
 * Ejecuta:  ./haruka_tests            (todos)
 *           ./haruka_tests cube       (filtro por subcadena)
 * Devuelve 0 si TODO pasa, !=0 si algo falla.
 */
#include "Common/TestCommon.h"

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>


// ⚠️ ¿ESTE BINARIO ESTA OPTIMIZADO? SE LE PREGUNTA AL COMPILADOR, NO A UNA VARIABLE.
//
// `CMakeLists.txt` ya avisa cuando `CMAKE_BUILD_TYPE=Debug`, y su propia nota cuenta que eso "paso
// cinco veces en la sesion del 2026-08-31: la suite paso de ~1 min a no acabar en 4". El aviso salta
// al CONFIGURAR — y despues se compila con `ninja` mil veces sin volver a verlo. Paso una sexta vez:
// el arbol quedo en Debug, la suite dejo de terminar en 900 s, y estuve buscando la regresion en el
// codigo. No habia regresion: habia -O0.
//
// `__OPTIMIZE__` lo define el compilador cuando de verdad optimiza, asi que esto no puede quedarse
// desincronizado de la realidad como si se leyera `CMAKE_BUILD_TYPE`. Y sale en la PRIMERA linea de
// cada corrida: cualquier tiempo raro se explica solo.
static void harukaPrintBuildMode() {
#ifdef __OPTIMIZE__
    std::printf("== build: optimizado ==\n");
#else
    std::printf("\033[33m== build: SIN OPTIMIZAR (-O0) ==\n"
                "   La suite tarda ~4x y CUALQUIER medida de tiempo de aqui es basura.\n"
                "   Arreglo: cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo\033[0m\n");
#endif
}

// ── LOS TESTS, POR GRUPOS ───────────────────────────────────────────────────────────────────────
//
// Andoni (20-09): "marcar por terreno, nubes, agua... agrupados, contador secundario de la cantidad de
// estos grupos, ordenados de mas simples a mas complejos". Desde el 10-10 el GRUPO ES LA CARPETA: los
// tests viven en `tests/<Modulo>/` reflejando `src/Haruka/<Modulo>/` y se registran solos
// (HARUKA_REGISTER_TESTS, Common/TestCommon.h). Aqui solo queda el ORDEN de las carpetas —de la que no
// depende de nada a la que depende de todo— con su descripcion y las claves viejas del filtro.
//
// Filtro: `./haruka_tests <palabra>` corre los grupos cuya carpeta, descripcion o clave la contenga, Y
// ademas cualquier test cuyo nombre la contenga. `./haruka_tests --group World/Terrain` corre ESA
// carpeta y solo esa (lo usa CTest: un test de CTest por carpeta).
struct GroupInfo { const char* folder; const char* what; std::vector<const char*> keys; };
static const std::vector<GroupInfo>& groupInfo() {
    static const std::vector<GroupInfo> g = {
        { "Core/Jobs",         "el pool central de trabajos",                         { "servicio", "jobs", "hilos", "thread" } },
        { "Core",              "selector de monitor",                                 { "util", "display" } },
        { "IO",                "PNG: ida y vuelta + DEFLATE",                         { "util", "png" } },
        { "Input",             "acciones de entrada: teclado + mando",                { "util", "input" } },
        { "Settings",          "calidad de terreno -> resoluciones",                  { "util", "quality", "tier" } },
        { "Audio",             "sintesis + puntos logicos agrupados (sin OpenAL)",    { "sonido", "sound", "audio" } },
        { "Renderer",          "almacen CPU<->GPU, primitivas, ambiente SH del cielo", { "servicio", "recursos", "resource", "store", "prim", "sky", "cielo", "ambient", "sh" } },
        { "Tools/ProcGraph",   "nodos ProcGraph, arboles, props, roca enterrada",      { "procgraph", "pg", "rock", "roca" } },
        { "World/Planet",      "orbitas Kepler, SOI, malla SimplePlanet",              { "orbit", "orbita", "soi", "planet", "config" } },
        { "World",             "clima, tiempo adverso y nubes",                        { "weather", "clima", "viento", "wind", "cloud", "nube" } },
        { "World/Terrain",     "campo, paridad y quadtree v5",                         { "terreno", "terrain", "capa", "ground", "lod", "ring", "chord", "grad", "parity", "paridad", "node", "quadtree", "v5", "observer", "cube" } },
        { "World/Water",       "mar y agua interior",                                  { "agua", "water", "ocean", "mar", "shallow" } },
        { "World/Vox",         "cuevas, islas y el mundo vox",                         { "cueva", "cave", "isla", "island", "vox", "edit" } },
        { "World/Props",       "LOD y colliders de props",                             { "prop" } },
        { "Physics",           "Jolt: caida, personaje, raiz",                         { "physics", "fisica", "multi", "rail" } },
        { "Physics/XPBD",      "tela enganchada a un hueso",                           { "physics", "fisica", "xpbd" } },
        { "Game/Construction", "colocacion y vehiculos",                               { "construction", "build", "vehicle" } },
        { "Game/Prefab",       "prefabricados: cambios por conjunto, escena, .glb",    { "construction", "prefab" } },
        { "Game/Ports",        "puertos autorizados y railes",                         { "construction", "port", "puerto", "rail" } },
        { "Network",           "modulo de reglas, cable, robustez, entidad y clima replicados", { "dgs", "net", "red", "rules", "wire", "robust", "render", "multi", "weather", "clima" } },
    };
    return g;
}
struct TestGroup { std::string name; std::string what; std::vector<const char*> keys; std::vector<const RegisteredTest*> tests; };

// Grupos en el orden de groupInfo(); una carpeta con tests que no este en la lista va al final y se
// avisa (corre igual: un test nuevo nunca se queda sin correr por olvidar esta tabla).
static std::vector<TestGroup> harukaTestGroups() {
    std::vector<TestGroup> out;
    for (const GroupInfo& gi : groupInfo()) out.push_back({ gi.folder, gi.what, gi.keys, {} });
    for (const RegisteredTest& t : testRegistry()) {
        auto it = std::find_if(out.begin(), out.end(), [&](const TestGroup& g) { return g.name == t.group; });
        if (it == out.end()) {
            std::printf("\033[33maviso: la carpeta '%s' no esta en groupInfo() (HarukaTests.cpp); va al final\033[0m\n", t.group.c_str());
            out.push_back({ t.group, "(sin descripcion)", {}, {} });
            it = out.end() - 1;
        }
        it->tests.push_back(&t);
    }
    std::erase_if(out, [](const TestGroup& g) { return g.tests.empty(); });
    return out;
}

int main(int argc, char** argv) {
    // `--group <carpeta>`: exactamente esa carpeta (CTest). Si no, filtro por subcadena.
    std::string filter, exactGroup;
    if (argc > 2 && std::string(argv[1]) == "--group") exactGroup = argv[2];
    else if (argc > 1) filter = argv[1];
    auto has = [&](const std::string& s) { return s.find(filter) != std::string::npos; };

    std::printf("== Haruka tests (CPU) ==\n");
    harukaPrintBuildMode();

    const std::vector<TestGroup> groups = harukaTestGroups();
    // Que se va a correr: por grupo (carpeta, descripcion o clave) o test a test (nombre).
    struct Plan { const TestGroup* g; std::vector<const RegisteredTest*> tests; };
    std::vector<Plan> plan;
    for (const TestGroup& g : groups) {
        bool whole = exactGroup.empty() ? (filter.empty() || has(g.name) || has(g.what)) : g.name == exactGroup;
        for (const char* k : g.keys) if (!filter.empty() && std::string(k).find(filter) != std::string::npos) whole = true;
        Plan p{ &g, {} };
        for (const RegisteredTest* t : g.tests) if (whole || (exactGroup.empty() && !filter.empty() && has(t->name))) p.tests.push_back(t);
        if (!p.tests.empty()) plan.push_back(p);
    }
    // Un grupo pedido por nombre que no existe (o se quedo sin tests) es un FALLO, no un verde vacio:
    // si no, una carpeta renombrada dejaria su test de CTest pasando sin correr nada.
    if (!exactGroup.empty() && plan.empty()) {
        std::printf("\033[31mFALLO: el grupo '%s' no existe o no tiene tests\033[0m\n", exactGroup.c_str());
        return 2;
    }
    size_t total = 0; for (const Plan& p : plan) total += p.tests.size();
    std::printf("%zu grupos · %zu tests%s%s\n", plan.size(), total,
                filter.empty() ? "" : " · filtro: ", filter.c_str());

    struct Result { const char* name; size_t tests; int pass, fail; double secs; };
    std::vector<Result> results;
    for (size_t gi = 0; gi < plan.size(); ++gi) {
        const Plan& p = plan[gi];
        std::printf("\n\033[1m== [%zu/%zu] %s\033[0m · %zu tests · %s ==\n",
                    gi + 1, plan.size(), p.g->name.c_str(), p.tests.size(), p.g->what.c_str());
        const int pass0 = g_pass, fail0 = g_fail;
        const auto t0 = std::chrono::steady_clock::now();
        for (size_t ti = 0; ti < p.tests.size(); ++ti) {
            std::printf("[%zu/%zu · %zu/%zu] ", gi + 1, plan.size(), ti + 1, p.tests.size());
            std::fflush(stdout);
            p.tests[ti]->fn();
        }
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        results.push_back({ p.g->name.c_str(), p.tests.size(), g_pass - pass0, g_fail - fail0, secs });
    }

    // El resumen por grupo: donde estan las comprobaciones, donde los fallos y donde el tiempo.
    std::printf("\n== por grupo ==\n");
    for (const Result& r : results)
        std::printf("  %-28s %3zu tests · %7d comprobaciones · %s%d fallos\033[0m · %6.1f s\n",
                    r.name, r.tests, r.pass + r.fail, r.fail ? "\033[31m" : "", r.fail, r.secs);
    std::printf("\n== %d OK · %d FALLOS ==\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
