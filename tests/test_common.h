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
// Puertos autorizados y RAILES (docs/guides/PLAN_PUERTOS.md): orientacion propia (no caras de un
// AABB), encaje por clase/kind/talla, raycast, y el mecanismo que BLOQUEA y ROMPE — con contraprueba
// en cada uno.  (test_ports.cpp)
void test_rail_mechanism_jolt();   // raíles contra Jolt (test_physics.cpp)
void test_construction_vehicle();   // mecanismos, masa y distancia sobre el grafo de construcción
void test_ports_transform();
void test_ports_fit();
void test_ports_raycast();
void test_rail_mechanism();
void test_ports_json_roundtrip();
void test_cube_sphere_inverse();
void test_weather_fronts();
void test_weather_3d();
// Forma de la nube: que sea un CUERPO y no una lamina (relacion ancho/alto, campo 3D, visibilidad
// con la cobertura mediana del planeta). Con contraprueba de las cifras viejas en los tres.
void test_cloud_shape();
void test_ground_layer();
// Paridad clipmap ↔ recorte (test_terrain.cpp): el cuadro del recorte y la rejilla que dibuja el
// clipmap cubren EXACTAMENTE el mismo cuadrado tangente (el hueco 0-2 km no aparece).
void test_clipmap_parity();
// Las cifras de LOD (terrain_lod.h) casan entre sí y con el hardware: ningún tope de teselación por
// encima de GL_MAX_TESS_GEN_LEVEL, y el piso de triM == el lado real del quad (Nyquist).
void test_terrain_lod_invariants();
// La rejilla por anillos de la colisión (§9 Fase 3): monótona, simétrica, cubre su radio y acotada.
void test_terrain_ring_grid();
// La disparidad REAL en metros entre la superficie que se DIBUJA y la que se PISA: las dos son
// poligonales, con celdas distintas, así que se separan por la sagita de la celda.
void test_terrain_chord_error();
void test_clipmap_vertex_lattice();
// El gradiente ANALÍTICO del detalle coincide con las diferencias finitas: es lo que permite bajar
// de 3 evaluaciones por vértice a 1 (§8) sin que la iluminación deje de describir la geometría.
void test_terrain_detail_gradient();
// Los ANILLOS de heightfield que sustituyen a la malla lejana TESELAN el plano: ni hueco (por el que
// se cae) ni solape (contactos dobles de Jolt), y todo nodo en la retícula del render.
void test_terrain_ring_tiling();
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
// Ambiente por SH (test_sky_ambient.cpp): paridad con sky_palette.glsl + simetría azimutal
void test_sky_ambient_palette();
void test_sky_ambient_sh();
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
// Órbitas con elementos precesantes (test_orbit.cpp): compatibilidad de la base, ausencia de deriva
// tras 400k órbitas, que la precesión de verdad rompe la periodicidad, y la prueba de no-choque.
void test_orbit_basis();
void test_orbit_no_drift();
void test_orbit_precession();
void test_orbit_no_collision();
// Esferas de influencia + gravedad N-cuerpos (test_orbit.cpp): fija los números del sistema real —
// 9,81 m/s² en superficie, SOI de la Tierra a 9,25e8 m — y que una órbita integrada se cierra.
void test_soi_gravity();
void test_soi_orbital_energy();
// Escritor de PNG (test_image_writer.cpp): ida y vuelta contra el inflate de stb_image (código ajeno)
// más el ratio de compresión, que es la razón del cambio — antes emitía bloques "stored" (1,00x).
void test_image_writer_roundtrip();
// LOD de malla de props (test_prop_lod.cpp): que la envolvente NO cambie entre niveles (el artefacto
// del salto de tamaño), que la malla baje de verdad y que detalle 1.0 siga siendo la de siempre.
void test_prop_lod_mesh();
void test_prop_collider();
void test_rock_interior();   // peñones enterrados de la roca (con contraprueba por rayos)
// La reconstrucción de `dir` del clipmap (test_clipmap_dir.cpp): el vértice del clipmap llega a su
// dirección en FLOAT desde el marco tangente, mientras la CPU la calcula en DOUBLE desde la posición.
// Mide cuánta altura separa eso — la única pieza del terreno que nunca se había comparado.
void test_clipmap_dir_parity();
// EL MAR (test_ocean.cpp): que la ola de la física sea la derivada exacta de la superficie, que el
// agua TRANSPORTE (firma de Stokes), el bajío/rompiente que define la costa, y el acople real en el
// motor — flotar y ser arrastrado. Con contraprueba en los cinco.
void test_ocean_wave_velocity();
void test_ocean_float_precision();
void test_ocean_stokes_transport();
void test_ocean_shoaling();
void test_ocean_buoyancy_drift();
void test_ocean_no_water_no_float();
void test_ocean_sea_state();
void test_ocean_tide();
void test_ocean_swash();
// La octava mas fina del terreno sigue viva (murio en silencio al bajar el tope de tesela).
void test_terrain_finest_octave();
// F1 del PLAN TERRENO v5 (test_terrain_node.cpp): el direccionamiento de nodos del quadtree es
// exacto BIT A BIT — grueso subconjunto de fino, aristas sin grieta, determinista. Con contraprueba.
void test_terrain_node_lattice();
void test_terrain_node_scale();
void test_terrain_node_content();
void test_terrain_node_select();
void test_terrain_node_frustum();
// El cono del recorte tiene que envolver las ESQUINAS del frustum, no sus bordes: si no, los chunks
// se cortan antes de llegar al borde de la pantalla. Verdad de referencia = los 6 planos de la matriz.
void test_terrain_node_frustum_corners();
// La costura entre CARAS del cubo: el vecino de un nodo del borde vive en otra cara. Oraculo doble:
// simetria de la relacion en las 24 combinaciones, y que la arista compartida coincida en metros.
void test_terrain_node_face_seam();
void test_terrain_node_range();
void test_terrain_node_pool();
void test_terrain_node_pool_reuse();
void test_terrain_node_stitch();
void test_terrain_node_neighbours();

#endif // HARUKA_TEST_COMMON_H
