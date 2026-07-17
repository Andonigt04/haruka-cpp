#ifndef HARUKA_TEST_COMMON_H
#define HARUKA_TEST_COMMON_H
// ================================================================================================
// Infra COMPARTIDA del banco de pruebas. El monolito histórico se repartió por ÁREA (petición del
// autor): CPU de terreno (test_terrain), física (test_physics), módulo DGS (test_dgs) y todo lo que
// necesita GL (test_render_gl); `main()` vive en haruka_tests.cpp y los dirige todos.
//
// Aquí solo el mini-framework de aserciones (sin GL, sin dominio) + las DECLARACIONES de cada test,
// para que main() los llame desde otra unidad de traducción. Las definiciones están en su .cpp.
// ================================================================================================
#include <cstdio>
#include <string>

// --- mini-framework de aserciones (contadores definidos en test_common.cpp) ---
extern int         g_pass, g_fail;
extern std::string g_curTest;
#define CHECK(cond, msg) do { \
    if (cond) { ++g_pass; } \
    else { ++g_fail; std::printf("  \033[31mFAIL\033[0m [%s] %s  (%s:%d)\n", g_curTest.c_str(), msg, __FILE__, __LINE__); } \
} while (0)
void beginTest(const char* name);

// Fixtures GL (definidos en test_gl_fixture.h). Forward-decl para las firmas de los tests GL — los
// .cpp puros-CPU no necesitan arrastrar glad/SDL solo por ver estos punteros.
struct TerrainDrawGL;
struct WaterDrawGL;

// --- Declaraciones de TODOS los tests, agrupadas como en su .cpp ---
// CPU puro de terreno  (test_terrain.cpp)
void test_sampler_determinism();
void test_cube_sphere_inverse();
void test_geology_plates();
void test_climate_orographic();
void test_erosion_hydrology();
void test_meso_tiles();
// Física determinista  (test_physics.cpp)
void test_physics_radial_fall();
// Módulo de reglas DGS por dlopen  (test_dgs.cpp)
void test_dgs_rules_module();
// Render / streaming — requieren contexto GL  (test_render_gl.cpp)
void test_engine_loads_terrain(TerrainDrawGL* gl);
void test_engine_rotate_keeps_terrain(TerrainDrawGL* gl);
void test_drawset_bench(TerrainDrawGL* gl);
void test_drawset_cache_gating(TerrainDrawGL* gl);
void test_drawset_delta_streaming(TerrainDrawGL* gl);
void test_cpu_gpu_parity();
void test_earth_diag(TerrainDrawGL* gl);
void test_gpu_derived_dirs();
void test_gpu_instancing();
void test_mesh_height_matches_vertices(TerrainDrawGL* gl);
void test_orbit_stability();
void test_stress(TerrainDrawGL* gl, WaterDrawGL* wgl);
void test_pool_fixed_no_grow(TerrainDrawGL* gl);
void test_pool_multitopology_bounded(TerrainDrawGL* gl);
void test_quality_sweep(TerrainDrawGL* gl);

#endif // HARUKA_TEST_COMMON_H
