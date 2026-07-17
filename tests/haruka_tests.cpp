/**
 * @file haruka_tests.cpp
 * @brief Punto de entrada del banco de pruebas del MOTOR REAL, headless (ventana GL OCULTA).
 *
 * CLAVE (petición del autor): NO reimplementa nada — instancia el `PlanetarySystem` REAL y dirige su
 * `update()` REAL, leyendo su ESTADO real. Los tests se reparten por ÁREA en varios .cpp (ver
 * test_common.h): terreno-CPU (test_terrain), física (test_physics), módulo DGS (test_dgs) y todo lo
 * que necesita GL (test_render_gl). Este archivo solo contiene `main()`, que los selecciona/dirige.
 *
 * Ejecuta:  ./haruka_tests            (todos)
 *           ./haruka_tests lod        (filtro por subcadena)
 * Devuelve 0 si TODO pasa, !=0 si algo falla.
 */
#include "test_common.h"
#include "test_gl_fixture.h"   // GLContext / TerrainDrawGL / WaterDrawGL para el bloque GL de main()

#include <string>

int main(int argc, char** argv) {
    std::string filter = (argc > 1) ? argv[1] : "";
    auto want = [&](const char* name) { return filter.empty() || std::string(name).find(filter) != std::string::npos; };

    std::printf("== Haruka tests (MOTOR REAL, headless GL) ==\n");

    // El sampler es puro-CPU (no necesita GL); los de LOD/streaming/paridad SÍ (compute + renderer GPU).
    if (want("sampler")) test_sampler_determinism();
    if (want("sampler") || want("physics")) test_physics_radial_fall();
    if (want("sampler") || want("dgs") || want("rules")) test_dgs_rules_module();
    if (want("sampler") || want("f10") || want("mesh")) test_cube_sphere_inverse();
    if (want("geology")) test_geology_plates();
    if (want("erosion")) test_erosion_hydrology();
    if (want("erosion") || want("clima")) test_climate_orographic();
    if (want("meso")) test_meso_tiles();

    const bool wantGL = want("lod") || want("engine") || want("parity") || want("gpu") || want("orbit")
                     || want("earthdiag")
                     || want("stress") || want("quality") || want("sweep") || want("drawset")
                     || want("bench") || want("gating") || want("delta") || want("pool");
    if (wantGL) {
        GLContext glc;
        if (!glc.ok) { std::printf("  \033[33mSKIP\033[0m tests GPU: no hay contexto GL headless disponible\n"); }
        else {
            TerrainDrawGL draw; draw.init();
            WaterDrawGL water; water.init();
            if (want("parity") || want("gpu")) test_cpu_gpu_parity();
            if (!filter.empty() && want("earthdiag")) test_earth_diag(&draw);  // diag manual (no en la suite)
            if (want("parity") || want("gpu")) test_gpu_derived_dirs();
            if (want("orbit")) test_orbit_stability();
            if (want("instancing") || want("gpu")) test_gpu_instancing();
            if (want("lod") || want("engine")) {
                if (!draw.ok) std::printf("  (aviso: shader de terreno no cargó — solo update(); eviction/draw-set no)\n");
                test_engine_loads_terrain(&draw);
                test_engine_rotate_keeps_terrain(&draw);
                test_mesh_height_matches_vertices(&draw);
            }
            if (want("drawset") || want("bench")) test_drawset_bench(&draw);
            if (want("drawset") || want("gating")) test_drawset_cache_gating(&draw);
            if (want("drawset") || want("delta"))  test_drawset_delta_streaming(&draw);
            if (want("pool")) {
                test_pool_fixed_no_grow(&draw);
                test_pool_multitopology_bounded(&draw);
            }
            if (want("stress")) test_stress(&draw, &water);
            if (want("stress") || want("quality") || want("sweep")) test_quality_sweep(&draw);
        }
    }

    std::printf("\n== %d OK · %d FALLOS ==\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
