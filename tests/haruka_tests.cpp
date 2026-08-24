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
    if (want("weather") || want("clima")) { test_weather_fronts(); test_weather_3d(); }
    if (want("cloud") || want("nube") || want("clima")) test_cloud_shape();
    if (want("capa") || want("ground"))   test_ground_layer();
    if (want("clip") || want("parity"))   test_clipmap_parity();
    if (want("lod")  || want("parity"))   test_terrain_lod_invariants();
    if (want("ring") || want("parity"))   test_terrain_ring_grid();
    if (want("ring") || want("parity"))   test_terrain_ring_tiling();
    if (want("chord") || want("parity"))  test_terrain_chord_error();
    if (want("clipmap") || want("parity"))  test_clipmap_vertex_lattice();
    if (want("grad")  || want("parity"))  test_terrain_detail_gradient();
    if (want("physics"))                  test_physics_radial_fall();
    if (want("physics"))                  test_physics_mesh_ground();
    if (want("physics"))                  test_physics_static_wall();
    if (want("physics"))                  test_physics_character();
    if (want("physics"))                  test_physics_body_removal();
    if (want("physics") || want("rail"))   test_rail_mechanism_jolt();
    if (want("construction") || want("build")) test_construction_placement();
    if (want("construction") || want("vehicle")) test_construction_vehicle();
    if (want("port") || want("puerto") || want("rail")) {
        test_ports_transform(); test_ports_fit(); test_ports_raycast();
        test_rail_mechanism(); test_ports_json_roundtrip();
    }

    if (want("ocean") || want("mar") || want("agua") || want("physics")) {
        test_ocean_wave_velocity();
        test_ocean_float_precision();
        test_ocean_stokes_transport();
        test_ocean_shoaling();
        test_ocean_buoyancy_drift();
        test_ocean_no_water_no_float();
        test_ocean_sea_state();
        test_ocean_tide();
        test_ocean_swash();
        test_terrain_finest_octave();
        test_terrain_orbital_relief();
        test_terrain_orbital_albedo();
    }

    if (want("node") || want("quadtree") || want("v5") || want("parity")) {
        test_terrain_node_lattice();
        test_terrain_node_scale();
        test_terrain_node_content();
        test_terrain_node_select();
        test_terrain_node_frustum();
        test_terrain_node_frustum_corners();
        test_terrain_node_face_seam();
        test_terrain_node_face_seam_gap();
        test_terrain_node_horizon_cull();
        test_terrain_two_bakes_disagree();
        test_terrain_node_as_heightfield();
        test_terrain_render_vs_collision();
        test_terrain_node_range();
        test_terrain_node_pool();
        test_terrain_node_pool_reuse();
        test_terrain_node_stitch();
        test_terrain_node_neighbours();
    }

    if (want("dgs") || want("rules"))     test_dgs_rules_module();
    if (want("dgs") || want("wire"))      test_dgs_wire_format();
    if (want("robust") || want("dgs"))    test_dgs_robust();
    if (want("planet") || want("mesh"))   test_simple_planet_mesh();
    if (want("planet") || want("config")) test_simple_planet_config();
    if (want("quality") || want("tier"))  test_terrain_quality_mapping();
    if (want("sky") || want("ambient") || want("sh")) {
        test_sky_ambient_palette();
        test_sky_ambient_sh();
    }
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

    {   // Órbitas: Kepler con elementos precesantes + auditoría de no-choque
        test_orbit_basis();
        test_orbit_no_drift();
        test_orbit_precession();
        test_orbit_no_collision();
        test_soi_gravity();
        test_soi_orbital_energy();
    }

    {   // Escritor de PNG: ida y vuelta con decodificador independiente + compresión
        test_image_writer_roundtrip();
        test_prop_lod_mesh();
        test_prop_collider();
        test_rock_interior();
        test_clipmap_dir_parity();
    }

    std::printf("\n== %d OK · %d FALLOS ==\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
