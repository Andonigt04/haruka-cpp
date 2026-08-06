#ifndef HARUKA_TEST_COMMON_H
#define HARUKA_TEST_COMMON_H
// ================================================================================================
// Infra COMPARTIDA del banco de pruebas. Los tests se reparten por ÁREA en varios .cpp:
// CPU de terreno (test_terrain), física (test_physics), módulo DGS (test_dgs).
// `main()` vive en haruka_tests.cpp y los dirige todos.
//
// Aquí solo el mini-framework de aserciones + las DECLARACIONES de cada test.
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

// --- Declaraciones de TODOS los tests ---
// CPU puro de terreno  (test_terrain.cpp)
void test_cube_sphere_inverse();
void test_weather_fronts();
void test_ground_layer();
// Física determinista  (test_physics.cpp)
void test_physics_radial_fall();
void test_physics_mesh_ground();
void test_physics_static_wall();
void test_physics_character();
void test_physics_body_removal();
// Sistema de construcción  (test_construction.cpp)
void test_construction_placement();
// Módulo de reglas DGS  (test_dgs.cpp)
void test_dgs_rules_module();
// Golden wire-format / layout DGS  (test_dgs.cpp) — guarda el layout que viaja por red (P0)
void test_dgs_wire_format();
// Batería de robustez DGS (test_dgs.cpp): framing TCP (bug 6), evicción, escalado dry-run, fuzz (§4.3)
void test_dgs_robust();
// SimplePlanet mesh   (test_simple_planet.cpp)
void test_simple_planet_mesh();
void test_simple_planet_config();
// Terrain quality → resolution  (test_terrain_quality.cpp)
void test_terrain_quality_mapping();
// ProcGraph node system + helpers  (test_procgraph.cpp)
void test_procgraph_basic();
void test_procgraph_fbm();
void test_procgraph_voronoi();
void test_procgraph_blend();
void test_procgraph_clamp();
void test_procgraph_biome_config();
void test_procgraph_rgba_image();
void test_procgraph_evaluate_to_rgba();
void test_procgraph_evaluate_to_normal();
void test_procgraph_cycle();
void test_procgraph_determinism();
void test_procgraph_tree_mesh();
void test_procgraph_tree_prop();
void test_procgraph_tree_spawn();
void test_procgraph_zone_shape();
void test_procgraph_prop_layer();
void test_procgraph_prop_placer();
void test_procgraph_terrain_prop_field();
void test_procgraph_prop_assembler();
void test_procgraph_prop_layer_json();

#endif // HARUKA_TEST_COMMON_H
