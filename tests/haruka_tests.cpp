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

int main(int argc, char** argv) {
    std::string filter = (argc > 1) ? argv[1] : "";
    auto want = [&](const char* name) { return filter.empty() || std::string(name).find(filter) != std::string::npos; };

    std::printf("== Haruka tests (CPU) ==\n");
    harukaPrintBuildMode();

    if (want("cube") || want("mesh"))   test_cube_sphere_inverse();
    if (want("prim") || want("mesh"))   test_primitive_winding();
    if (want("weather") || want("clima")) { test_weather_fronts(); test_weather_3d(); }
    if (want("cloud") || want("nube") || want("clima")) test_cloud_shape();
    if (want("capa") || want("ground"))   test_ground_layer();
    if (want("lod")  || want("parity"))   test_terrain_lod_invariants();
    if (want("ring") || want("parity"))   test_terrain_ring_grid();
    if (want("ring") || want("parity"))   test_terrain_ring_tiling();
    if (want("chord") || want("parity"))  test_terrain_chord_error();
    if (want("grad")  || want("parity"))  test_terrain_detail_gradient();
    if (want("physics"))                  test_physics_radial_fall();
    if (want("physics"))                  test_physics_mesh_ground();
    if (want("physics"))                  test_physics_static_wall();
    if (want("physics"))                  test_physics_character();
    if (want("physics"))                  test_physics_character_resting_velocity();
    if (want("physics"))                  test_physics_body_removal();
    if (want("physics") || want("multi")) test_physics_far_ground_from_query();
    if (want("physics") || want("net") || want("multi")) test_physics_two_instances_agree();
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
        test_ocean_finite_depth();
    if (want("ocean") || want("water")) test_ocean_gerstner_ellipse();
    if (want("ocean") || want("water")) test_water_fill_window();
    if (want("ocean") || want("water")) test_water_window_policy();
    if (want("ocean") || want("water")) test_ocean_refraction_and_break();
    if (want("ocean") || want("water")) test_ocean_spectrum_limits();
        test_water_fill_global();
        test_ocean_fetch();
        test_water_bake_resolution_limit();
        test_ocean_wave_reach();
        test_ocean_grid_carries_wave();
        test_ocean_tide();
        test_ocean_swash();
        test_ocean_break_limit();
        test_ocean_break_fold();
        test_ocean_foam();
        test_ocean_skewness();
        test_ocean_whitewater_drag();
        test_ocean_whitecaps();
        test_shallow_water_reanchor_stability();
        test_shallow_water_mountain_coverage();
        test_shallow_water_inherits_ocean_swell();
        test_shallow_water_physics();
        test_terrain_finest_octave();
        test_terrain_orbital_relief();
        test_terrain_orbital_albedo();
        test_terrain_twist_vs_cell();
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
        test_terrain_node_split_uses_elevation();
        test_terrain_node_morph_no_pop();
        test_terrain_node_stride_per_node();
        test_terrain_node_stride_pop();
        test_terrain_node_stride_seams();
        test_terrain_node_ancestor_disparity();
        test_terrain_node_budget_starvation();
        test_terrain_node_walk_shimmer();
        test_terrain_node_ucenter_jitter();
        test_terrain_node_level_balance();
        test_terrain_prop_anchor_mismatch();
        test_terrain_node_orbit_coverage();
        test_terrain_node_range_published();
        test_terrain_node_demand_with_range();
        test_terrain_node_inherited_range_flicker();
        test_terrain_node_spike_hunt();
        test_terrain_node_finer_neighbour_step();
        test_terrain_node_edge_audit_all();
        test_terrain_node_distance_morph_per_vertex();
        test_terrain_node_angle_independence();
        test_terrain_backup_bake_cost();
        test_terrain_node_as_heightfield();
        test_terrain_render_vs_collision();
        test_terrain_node_range();
        test_terrain_node_pool();
        test_terrain_node_pool_reuse();
        test_terrain_node_pool_chain();
        test_terrain_node_subrect_mapping();
        test_terrain_node_fallback_footprint();
        test_terrain_node_overlap_on_turn();
        test_terrain_node_normal_matches_geometry();
        test_terrain_node_radial_noise_shift();
        test_terrain_node_octave_cut_nyquist();
        test_terrain_node_quad_diagonal_bias();
        test_terrain_node_float_dir_quantisation();
        test_terrain_collision_walkability();
        test_terrain_ring_anchor_drift();
        test_terrain_render_vs_reference_cut();
        test_terrain_ring_sample_lateral_shift();
        test_terrain_node_stride_history_loss();
        test_terrain_node_winding();
        test_terrain_node_stitch();
        test_terrain_node_neighbours();
    }

    // Con filtro propio: es el criterio de aceptacion del multijugador ("lo visible tiene que ser
    // colisionable, 10-100 km") y se consulta suelto, sin pagar el bloque entero del v5.
    if (want("node") || want("v5") || want("parity") || want("observer") || want("multi"))
    {   test_terrain_collision_observer_dependence();
        test_terrain_collision_client_agreement();
        test_terrain_collision_refine_cost();
        test_terrain_ground_query_cost(); }

    if (want("dgs") || want("rules"))     test_dgs_rules_module();
    if (want("dgs") || want("wire"))      test_dgs_wire_format();
    if (want("robust") || want("dgs"))    test_dgs_robust();
    if (want("dgs") || want("net"))       test_dgs_server_simulates();
    if (want("dgs") || want("net"))    test_dgs_ground_matches_engine();
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

    // Que una entidad de red SE DIBUJE, no solo que exista. Va con filtro propio: correrla suelta
    // no debe costar la suite entera (`./build/haruka_tests net`).
    if (want("net") || want("dgs") || want("render")) test_net_entity_visible();
    if (want("net") || want("dgs") || want("weather") || want("clima")) test_weather_replicated();

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
        test_prop_lod_attributes();
        test_prop_scatter_radial();
        test_prop_scatter_stability();
        test_prop_collider();
        test_rock_interior();
    }

    std::printf("\n== %d OK · %d FALLOS ==\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
