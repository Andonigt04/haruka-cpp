/**
 * @file haruka_tests.cpp
 * @brief Punto de entrada del banco de pruebas del MOTOR, HEADLESS (CPU only).
 *
 * Ejecuta:  ./haruka_tests            (todos)
 *           ./haruka_tests cube       (filtro por subcadena)
 * Devuelve 0 si TODO pasa, !=0 si algo falla.
 */
#include "test_common.h"

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
// estos grupos, ordenados de mas simples a mas complejos". Antes era una lista de `if(want(...))`
// con claves sueltas y bloques sin nombre: 200 tests seguidos sin saber en cual se estaba ni cuantos
// quedaban. Ahora cada grupo lleva nombre, claves de filtro y sus tests, y el orden es del que no
// depende de nada (utilidades puras) al que depende de todo (red y DGS). El contador que imprime cada
// test es `[grupo i/N · test j/M]`.
//
// Filtro: `./haruka_tests <palabra>` corre los grupos cuya clave o nombre la contenga, Y ademas
// cualquier test cuyo nombre la contenga (antes un test suelto solo se podia lanzar con la clave de su
// bloque). Las claves viejas siguen valiendo (node, parity, physics, ocean, agua, clima, dgs, net...).
struct TestEntry { const char* name; void (*fn)(); };
struct TestGroup {
    const char* name;
    const char* what;
    std::vector<const char*> keys;
    std::vector<TestEntry>   tests;
};

static std::vector<TestGroup> harukaTestGroups() {
    return {
        { "utilidades",
          "funciones puras: PNG, malla de primitivas, cubo-esfera, planeta simple, calidad",
          { "util", "mesh", "png", "prim", "cube", "planet", "config", "quality", "tier" },
          {
              { "image_writer_roundtrip", test_image_writer_roundtrip },
              { "primitive_winding", test_primitive_winding },
              { "character_capsule_standing", test_character_capsule_standing },
              { "cube_sphere_inverse", test_cube_sphere_inverse },
              { "simple_planet_mesh", test_simple_planet_mesh },
              { "simple_planet_config", test_simple_planet_config },
              { "terrain_quality_mapping", test_terrain_quality_mapping },
              { "input_gamepad_bindings", test_input_gamepad_bindings },
              { "display_selector_list", test_display_selector_list },
          } },
        { "sonido",
          "sintesis de bucles y disparos; puntos logicos de sonido agrupados por sector (sin OpenAL)",
          { "sonido", "sound", "audio" },
          {
              { "sound_synth", test_sound_synth },
              { "sound_points_grouping", test_sound_points_grouping },
              { "debug_overlay_stats", test_debug_overlay_stats },
          } },
        { "procgraph",
          "el grafo procedural: nodos, ruido, biomas, arboles y capas de props",
          { "procgraph", "pg" },
          {
              { "procgraph_basic", test_procgraph_basic },
              { "procgraph_fbm", test_procgraph_fbm },
              { "procgraph_voronoi", test_procgraph_voronoi },
              { "procgraph_blend", test_procgraph_blend },
              { "procgraph_clamp", test_procgraph_clamp },
              { "procgraph_biome_config", test_procgraph_biome_config },
              { "procgraph_rgba_image", test_procgraph_rgba_image },
              { "procgraph_evaluate_to_rgba", test_procgraph_evaluate_to_rgba },
              { "procgraph_evaluate_to_normal", test_procgraph_evaluate_to_normal },
              { "procgraph_cycle", test_procgraph_cycle },
              { "procgraph_determinism", test_procgraph_determinism },
              { "procgraph_tree_mesh", test_procgraph_tree_mesh },
              { "procgraph_tree_prop", test_procgraph_tree_prop },
              { "procgraph_tree_spawn", test_procgraph_tree_spawn },
              { "procgraph_zone_shape", test_procgraph_zone_shape },
              { "procgraph_prop_layer", test_procgraph_prop_layer },
              { "procgraph_prop_placer", test_procgraph_prop_placer },
              { "procgraph_terrain_prop_field", test_procgraph_terrain_prop_field },
              { "procgraph_prop_assembler", test_procgraph_prop_assembler },
              { "procgraph_prop_layer_json", test_procgraph_prop_layer_json },
          } },
        { "orbitas",
          "Kepler con precesion, esferas de influencia, auditoria de no-choque",
          { "orbit", "orbita", "soi" },
          {
              { "orbit_basis", test_orbit_basis },
              { "orbit_no_drift", test_orbit_no_drift },
              { "orbit_precession", test_orbit_precession },
              { "orbit_no_collision", test_orbit_no_collision },
              { "soi_gravity", test_soi_gravity },
              { "soi_orbital_energy", test_soi_orbital_energy },
          } },
        { "cielo",
          "el ambiente SH: paridad con sky_palette.glsl y simetria",
          { "sky", "cielo", "ambient", "sh" },
          {
              { "sky_ambient_palette", test_sky_ambient_palette },
              { "sky_ambient_sh", test_sky_ambient_sh },
          } },
        { "clima",
          "frentes, tiempo adverso, ciclo del agua, viento sobre el personaje, horneado del cielo",
          { "weather", "clima", "viento", "wind" },
          {
              { "weather_fronts", test_weather_fronts },
              { "weather_severe", test_weather_severe },
              { "weather_water_cycle", test_weather_water_cycle },
              { "wind_pushes_character", test_wind_pushes_character },
              { "sky_bake", test_sky_bake },
          } },
        { "nubes",
          "formacion (la columna de aire) y movimiento frame a frame",
          { "cloud", "nube", "clima" },
          {
              { "cloud_formation", test_cloud_formation },
              { "cloud_motion", test_cloud_motion },
          } },
        { "terreno: campo y paridad",
          "capa del suelo, LOD, anillos, cuerda, gradiente, octavas: lo dibujado contra lo pisado",
          { "terreno", "terrain", "capa", "ground", "lod", "ring", "chord", "grad", "parity", "paridad" },
          {
              { "ground_layer", test_ground_layer },
              { "terrain_lod_invariants", test_terrain_lod_invariants },
              { "terrain_ring_grid", test_terrain_ring_grid },
              { "terrain_ring_tiling", test_terrain_ring_tiling },
              { "terrain_chord_error", test_terrain_chord_error },
              { "terrain_detail_gradient", test_terrain_detail_gradient },
              { "terrain_finest_octave", test_terrain_finest_octave },
              { "terrain_orbital_relief", test_terrain_orbital_relief },
              { "terrain_orbital_albedo", test_terrain_orbital_albedo },
              { "terrain_twist_vs_cell", test_terrain_twist_vs_cell },
          } },
        { "terreno: quadtree v5",
          "direccionamiento, seleccion, zancadas, cosido, pool, colision del observador",
          { "terreno", "terrain", "node", "quadtree", "v5", "parity", "paridad", "observer", "multi" },
          {
              { "terrain_node_lattice", test_terrain_node_lattice },
              { "terrain_node_scale", test_terrain_node_scale },
              { "terrain_node_content", test_terrain_node_content },
              { "terrain_node_select", test_terrain_node_select },
              { "terrain_node_frustum", test_terrain_node_frustum },
              { "terrain_node_frustum_corners", test_terrain_node_frustum_corners },
              { "terrain_node_face_seam", test_terrain_node_face_seam },
              { "terrain_node_face_seam_gap", test_terrain_node_face_seam_gap },
              { "terrain_node_horizon_cull", test_terrain_node_horizon_cull },
              { "terrain_node_split_uses_elevation", test_terrain_node_split_uses_elevation },
              { "terrain_node_morph_no_pop", test_terrain_node_morph_no_pop },
              { "terrain_node_stride_per_node", test_terrain_node_stride_per_node },
              { "terrain_node_stride_pop", test_terrain_node_stride_pop },
              { "terrain_node_stride_seams", test_terrain_node_stride_seams },
              { "terrain_node_ancestor_disparity", test_terrain_node_ancestor_disparity },
              { "terrain_node_budget_starvation", test_terrain_node_budget_starvation },
              { "terrain_node_walk_shimmer", test_terrain_node_walk_shimmer },
              { "terrain_node_ucenter_jitter", test_terrain_node_ucenter_jitter },
              { "terrain_node_level_balance", test_terrain_node_level_balance },
              { "terrain_prop_anchor_mismatch", test_terrain_prop_anchor_mismatch },
              { "terrain_node_orbit_coverage", test_terrain_node_orbit_coverage },
              { "terrain_node_range_published", test_terrain_node_range_published },
              { "terrain_node_demand_with_range", test_terrain_node_demand_with_range },
              { "terrain_node_inherited_range_flicker", test_terrain_node_inherited_range_flicker },
              { "terrain_node_spike_hunt", test_terrain_node_spike_hunt },
              { "terrain_node_finer_neighbour_step", test_terrain_node_finer_neighbour_step },
              { "terrain_node_edge_audit_all", test_terrain_node_edge_audit_all },
              { "terrain_node_shallow_split", test_terrain_node_shallow_split },
              { "terrain_node_stride_ramp_continuity", test_terrain_node_stride_ramp_continuity },
              { "terrain_node_distance_morph_per_vertex", test_terrain_node_distance_morph_per_vertex },
              { "terrain_node_angle_independence", test_terrain_node_angle_independence },
              { "terrain_backup_bake_cost", test_terrain_backup_bake_cost },
              { "terrain_node_as_heightfield", test_terrain_node_as_heightfield },
              { "terrain_render_vs_collision", test_terrain_render_vs_collision },
              { "terrain_node_range", test_terrain_node_range },
              { "terrain_node_pool", test_terrain_node_pool },
              { "terrain_node_pool_reuse", test_terrain_node_pool_reuse },
              { "terrain_node_pool_chain", test_terrain_node_pool_chain },
              { "terrain_node_subrect_mapping", test_terrain_node_subrect_mapping },
              { "terrain_node_fallback_footprint", test_terrain_node_fallback_footprint },
              { "terrain_node_overlap_on_turn", test_terrain_node_overlap_on_turn },
              { "terrain_node_normal_matches_geometry", test_terrain_node_normal_matches_geometry },
              { "terrain_node_radial_noise_shift", test_terrain_node_radial_noise_shift },
              { "terrain_node_octave_cut_nyquist", test_terrain_node_octave_cut_nyquist },
              { "terrain_node_quad_diagonal_bias", test_terrain_node_quad_diagonal_bias },
              { "terrain_node_float_dir_quantisation", test_terrain_node_float_dir_quantisation },
              { "terrain_collision_walkability", test_terrain_collision_walkability },
              { "terrain_ring_anchor_drift", test_terrain_ring_anchor_drift },
              { "terrain_render_vs_reference_cut", test_terrain_render_vs_reference_cut },
              { "terrain_ring_sample_lateral_shift", test_terrain_ring_sample_lateral_shift },
              { "terrain_node_stride_history_loss", test_terrain_node_stride_history_loss },
              { "terrain_node_winding", test_terrain_node_winding },
              { "terrain_node_stitch", test_terrain_node_stitch },
              { "terrain_node_neighbours", test_terrain_node_neighbours },
              { "terrain_collision_observer_dependence", test_terrain_collision_observer_dependence },
              { "terrain_collision_client_agreement", test_terrain_collision_client_agreement },
              { "terrain_collision_refine_cost", test_terrain_collision_refine_cost },
              { "terrain_ground_query_cost", test_terrain_ground_query_cost },
          } },
        { "agua",
          "el mar (olas, transporte, bajio, rompiente, espuma, flotacion) y el agua interior",
          { "agua", "water", "ocean", "mar", "shallow" },
          {
              { "ocean_wave_velocity", test_ocean_wave_velocity },
              { "ocean_float_precision", test_ocean_float_precision },
              { "ocean_stokes_transport", test_ocean_stokes_transport },
              { "ocean_shoaling", test_ocean_shoaling },
              { "ocean_buoyancy_drift", test_ocean_buoyancy_drift },
              { "ocean_no_water_no_float", test_ocean_no_water_no_float },
              { "ocean_sea_state", test_ocean_sea_state },
              { "ocean_low_wind_spectrum", test_ocean_low_wind_spectrum },
              { "ocean_sea_follows_wind", test_ocean_sea_follows_wind },
              { "ocean_finite_depth", test_ocean_finite_depth },
              { "ocean_gerstner_ellipse", test_ocean_gerstner_ellipse },
              { "water_fill_window", test_water_fill_window },
              { "water_window_policy", test_water_window_policy },
              { "ocean_refraction_and_break", test_ocean_refraction_and_break },
              { "ocean_spectrum_limits", test_ocean_spectrum_limits },
              { "water_fill_global", test_water_fill_global },
              { "ocean_fetch", test_ocean_fetch },
              { "water_bake_resolution_limit", test_water_bake_resolution_limit },
              { "ocean_wave_reach", test_ocean_wave_reach },
              { "ocean_grid_carries_wave", test_ocean_grid_carries_wave },
              { "ocean_tide", test_ocean_tide },
              { "ocean_swash", test_ocean_swash },
              { "ocean_break_limit", test_ocean_break_limit },
              { "ocean_break_fold", test_ocean_break_fold },
              { "ocean_mesh_stretch", test_ocean_mesh_stretch },
              { "ocean_foam", test_ocean_foam },
              { "ocean_skewness", test_ocean_skewness },
              { "ocean_whitewater_drag", test_ocean_whitewater_drag },
              { "ocean_whitecaps", test_ocean_whitecaps },
              { "shallow_water_reanchor_stability", test_shallow_water_reanchor_stability },
              { "shallow_water_mountain_coverage", test_shallow_water_mountain_coverage },
              { "shallow_water_inherits_ocean_swell", test_shallow_water_inherits_ocean_swell },
              { "shallow_water_physics", test_shallow_water_physics },
              { "shallow_water_cycle", test_shallow_water_cycle },
              { "shallow_water_friction", test_shallow_water_friction },
              { "shallow_water_fetch", test_shallow_water_fetch },
          } },
        { "cuevas, islas y vox",
          "mapas en disco, chunks sin costura, trazos de la partida, roca añadida, penones enterrados",
          { "cueva", "cave", "isla", "island", "vox", "rock", "roca", "edit" },
          {
              { "caves_bake", test_caves_bake },
              { "caves_cache", test_caves_cache },
              { "caves_mesh_and_strokes", test_caves_mesh_and_strokes },
              { "caves_coupling", test_caves_coupling },
              { "islands_bake", test_islands_bake },
              { "vox_island", test_vox_island },
              { "vox_world", test_vox_world },
              { "vox_placed_cave", test_vox_placed_cave },
              { "vox_draped_cave", test_vox_draped_cave },
              { "vox_columns", test_vox_columns },
              { "vox_scene_cave", test_vox_scene_cave },
              { "vox_designer_strokes", test_vox_designer_strokes },
              { "vox_brush", test_vox_brush },
              { "world_edit_panel", test_world_edit_panel },
              { "vox_raycast_grazing", test_vox_raycast_grazing },
              { "vox_brush_ops", test_vox_brush_ops },
              { "vox_sculpt", test_vox_sculpt },
              { "rock_interior", test_rock_interior },
          } },
        { "props",
          "LOD de malla, dispersion, colliders por parte, bioma y variantes, coste",
          { "prop" },
          {
              { "prop_lod_mesh", test_prop_lod_mesh },
              { "prop_lod_attributes", test_prop_lod_attributes },
              { "prop_scatter_radial", test_prop_scatter_radial },
              { "prop_scatter_stability", test_prop_scatter_stability },
              { "prop_collider", test_prop_collider },
              { "prop_biome", test_prop_biome },
              { "prop_variants", test_prop_variants },
              { "prop_density", test_prop_density },
              { "prop_map_biome", test_prop_map_biome },
              { "prop_scatter_cost", test_prop_scatter_cost },
          } },
        { "fisica",
          "Jolt: caida radial, suelo por malla, personaje, cuevas, islas, dos instancias de acuerdo, railes",
          { "physics", "fisica", "multi", "rail", "net" },
          {
              { "physics_radial_fall", test_physics_radial_fall },
              { "physics_mesh_ground", test_physics_mesh_ground },
              { "physics_static_wall", test_physics_static_wall },
              { "physics_character", test_physics_character },
              { "physics_character_resting_velocity", test_physics_character_resting_velocity },
              { "physics_character_capsule", test_physics_character_capsule },
              { "physics_body_removal", test_physics_body_removal },
              { "physics_far_ground_from_query", test_physics_far_ground_from_query },
              { "physics_two_instances_agree", test_physics_two_instances_agree },
              { "physics_cave_mouth", test_physics_cave_mouth },
              { "physics_island", test_physics_island },
              { "rail_mechanism_jolt", test_rail_mechanism_jolt },
          } },
        { "construccion",
          "colocacion, vehiculos, prefabricados, puertos y railes",
          { "construction", "build", "vehicle", "prefab", "port", "puerto", "rail" },
          {
              { "construction_placement", test_construction_placement },
              { "construction_vehicle", test_construction_vehicle },
              { "prefab_edits", test_prefab_edits },
              { "prefab_scene_roundtrip", test_prefab_scene_roundtrip },
              { "ports_transform", test_ports_transform },
              { "ports_fit", test_ports_fit },
              { "ports_raycast", test_ports_raycast },
              { "rail_mechanism", test_rail_mechanism },
              { "ports_json_roundtrip", test_ports_json_roundtrip },
          } },
        { "red y DGS",
          "modulo de reglas, formato de cable, robustez, el servidor simula, entidad visible, clima replicado",
          { "dgs", "net", "red", "rules", "wire", "robust", "render", "multi", "weather", "clima" },
          {
              { "dgs_rules_module", test_dgs_rules_module },
              { "dgs_wire_format", test_dgs_wire_format },
              { "dgs_robust", test_dgs_robust },
              { "dgs_server_simulates", test_dgs_server_simulates },
              { "dgs_ground_matches_engine", test_dgs_ground_matches_engine },
              { "net_entity_visible", test_net_entity_visible },
              { "weather_replicated", test_weather_replicated },
          } },
    };
}

int main(int argc, char** argv) {
    const std::string filter = (argc > 1) ? argv[1] : "";
    auto has = [&](const std::string& s) { return s.find(filter) != std::string::npos; };

    std::printf("== Haruka tests (CPU) ==\n");
    harukaPrintBuildMode();

    const std::vector<TestGroup> groups = harukaTestGroups();
    // Que se va a correr: por grupo (clave o nombre) o test a test (nombre).
    struct Plan { const TestGroup* g; std::vector<const TestEntry*> tests; };
    std::vector<Plan> plan;
    for (const TestGroup& g : groups) {
        bool whole = filter.empty() || has(g.name);
        for (const char* k : g.keys) if (!filter.empty() && std::string(k).find(filter) != std::string::npos) whole = true;
        Plan p{ &g, {} };
        for (const TestEntry& t : g.tests) if (whole || has(t.name)) p.tests.push_back(&t);
        if (!p.tests.empty()) plan.push_back(p);
    }
    size_t total = 0; for (const Plan& p : plan) total += p.tests.size();
    std::printf("%zu grupos · %zu tests%s%s\n", plan.size(), total,
                filter.empty() ? "" : " · filtro: ", filter.c_str());

    struct Result { const char* name; size_t tests; int pass, fail; double secs; };
    std::vector<Result> results;
    for (size_t gi = 0; gi < plan.size(); ++gi) {
        const Plan& p = plan[gi];
        std::printf("\n\033[1m== [%zu/%zu] %s\033[0m · %zu tests · %s ==\n",
                    gi + 1, plan.size(), p.g->name, p.tests.size(), p.g->what);
        const int pass0 = g_pass, fail0 = g_fail;
        const auto t0 = std::chrono::steady_clock::now();
        for (size_t ti = 0; ti < p.tests.size(); ++ti) {
            std::printf("[%zu/%zu · %zu/%zu] ", gi + 1, plan.size(), ti + 1, p.tests.size());
            std::fflush(stdout);
            p.tests[ti]->fn();
        }
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        results.push_back({ p.g->name, p.tests.size(), g_pass - pass0, g_fail - fail0, secs });
    }

    // El resumen por grupo: donde estan las comprobaciones, donde los fallos y donde el tiempo.
    std::printf("\n== por grupo ==\n");
    for (const Result& r : results)
        std::printf("  %-28s %3zu tests · %7d comprobaciones · %s%d fallos\033[0m · %6.1f s\n",
                    r.name, r.tests, r.pass + r.fail, r.fail ? "\033[31m" : "", r.fail, r.secs);
    std::printf("\n== %d OK · %d FALLOS ==\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
