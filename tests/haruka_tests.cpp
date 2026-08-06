/**
 * @file haruka_tests.cpp
 * @brief Punto de entrada del banco de pruebas del MOTOR, HEADLESS (CPU only).
 *
 * Ejecuta:  ./haruka_tests            (todos)
 *           ./haruka_tests cube       (filtro por subcadena)
 * Devuelve 0 si TODO pasa, !=0 si algo falla.
 */
#include "test_common.h"

#include <string>

int main(int argc, char** argv) {
    std::string filter = (argc > 1) ? argv[1] : "";
    auto want = [&](const char* name) { return filter.empty() || std::string(name).find(filter) != std::string::npos; };

    std::printf("== Haruka tests (CPU) ==\n");

    if (want("cube") || want("mesh"))   test_cube_sphere_inverse();
    if (want("weather") || want("clima")) test_weather_fronts();
    if (want("capa") || want("ground"))   test_ground_layer();
    if (want("physics"))                  test_physics_radial_fall();
    if (want("physics"))                  test_physics_mesh_ground();
    if (want("physics"))                  test_physics_static_wall();
    if (want("physics"))                  test_physics_character();
    if (want("physics"))                  test_physics_body_removal();
    if (want("construction") || want("build")) test_construction_placement();
    if (want("dgs") || want("rules"))     test_dgs_rules_module();
    if (want("dgs") || want("wire"))      test_dgs_wire_format();
    if (want("robust") || want("dgs"))    test_dgs_robust();
    if (want("planet") || want("mesh"))   test_simple_planet_mesh();
    if (want("planet") || want("config")) test_simple_planet_config();
    if (want("quality") || want("tier"))  test_terrain_quality_mapping();
    if (want("procgraph") || want("pg")) {
        test_procgraph_basic();
        test_procgraph_fbm();
        test_procgraph_voronoi();
        test_procgraph_blend();
        test_procgraph_clamp();
        test_procgraph_biome_config();
        test_procgraph_rgba_image();
        test_procgraph_evaluate_to_rgba();
        test_procgraph_evaluate_to_normal();
        test_procgraph_cycle();
        test_procgraph_determinism();
        test_procgraph_tree_mesh();
        test_procgraph_tree_prop();
        test_procgraph_tree_spawn();
        test_procgraph_zone_shape();
        test_procgraph_prop_layer();
        test_procgraph_prop_placer();
        test_procgraph_terrain_prop_field();
        test_procgraph_prop_assembler();
        test_procgraph_prop_layer_json();
    }

    std::printf("\n== %d OK · %d FALLOS ==\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
