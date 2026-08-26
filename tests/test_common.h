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
// Las cifras de LOD (terrain_lod.h) casan entre sí y con el hardware: ningún tope de teselación por
// encima de GL_MAX_TESS_GEN_LEVEL, y el piso de triM == el lado real del quad (Nyquist).
void test_terrain_lod_invariants();
// La rejilla por anillos de la colisión (§9 Fase 3): monótona, simétrica, cubre su radio y acotada.
void test_terrain_ring_grid();
// La disparidad REAL en metros entre la superficie que se DIBUJA y la que se PISA: las dos son
// poligonales, con celdas distintas, así que se separan por la sagita de la celda.
void test_terrain_chord_error();
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
void test_physics_character_resting_velocity();
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
// F5: ¿hay relieve procedural desde orbita? Antes del 2026-08-24 era EXACTAMENTE cero a partir de
// 119 km de camara — el planeta era solo el bake y su bilineal.
void test_terrain_orbital_relief();
// F5-albedo: de donde sale el color desde orbita y por que se ve uniforme. Mide las DOS fuentes
// (tintes de material y mapa de biomas) para señalar cual es la que falla.
void test_terrain_orbital_albedo();
// Que celda de colision hace falta para un twist dado. El twist es la forma del collider de Jolt
// (dos triangulos por celda), no el campo: solo baja afinando la celda.
void test_terrain_twist_vs_cell();
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
// La GRIETA en metros a ambos lados de una arista del cubo. El nivel correcto no implica cosido
// correcto: el autor vio grietas con el cosido "demostrado" por el test de arriba.
void test_terrain_node_face_seam_gap();
// El recorte por HORIZONTE contra rayos de pantalla: ningun suelo que se ve puede caer en un nodo
// descartado. Es el otro recorte, y el que el cono ya descartado dejaba libre de sospecha.
void test_terrain_node_horizon_cull();
// El criterio de subdivision medía al NIVEL DEL MAR, no al terreno: en una ladera subdividia de
// menos, y el sintoma parecia direccional. Cuantos niveles se perdian.
void test_terrain_node_split_uses_elevation();
// El POPPING al alejarse: que el nodo ya sea su padre cuando le toque fundirse en el. Es el otro
// geomorph, el del TIEMPO — el de aristas solo cose vecinos que se dibujan a la vez.
void test_terrain_node_morph_no_pop();
void test_terrain_node_stride_per_node();
void test_terrain_node_stride_pop();
void test_terrain_node_stride_seams();
void test_terrain_node_ancestor_disparity();
void test_terrain_node_budget_starvation();
void test_terrain_node_walk_shimmer();
void test_terrain_node_ucenter_jitter();
void test_terrain_node_level_balance();
void test_terrain_prop_anchor_mismatch();
void test_terrain_node_orbit_coverage();
void test_terrain_node_range_published();
void test_terrain_node_demand_with_range();
void test_terrain_node_inherited_range_flicker();
void test_terrain_node_spike_hunt();
void test_terrain_node_finer_neighbour_step();
void test_terrain_node_edge_audit_all();
void test_terrain_node_distance_morph_per_vertex();
// ¿El suelo depende de hacia donde miras? Separa las dos causas: la SELECCION (no debe) y el pool
// desalojando lo que sale de cuadro (si, y es transitorio).
void test_terrain_node_angle_independence();
// ⚠️ NO es una disparidad viva. El bake de altura es UNO: fisica y render leen el MISMO equirect. El
// bake del CUBO solo queda como RESPALDO, y esto mide en metros lo que costaria si llegara a usarse.
void test_terrain_backup_bake_cost();
// ¿Es un nodo del quadtree una rejilla REGULAR para Jolt, o hace falta convertir? La pregunta que
// abre F4, medida en metros por nivel.
void test_terrain_node_as_heightfield();
// CIERRE DE F4: la cota medida entre el suelo que se DIBUJA y el que se PISA. Sustituye al criterio
// "chord_error = 0 por construccion", que era imposible (ver terrain_node_as_heightfield).
void test_terrain_render_vs_collision();
void test_terrain_node_range();
void test_terrain_node_pool();
void test_terrain_node_pool_reuse();
// La caida por ancestro no puede saltarse niveles: el selector solo pide HOJAS, asi que el pool
// tiene que encolar tambien la cadena. A/B con `setChainAncestors`, con el presupuesto real.
void test_terrain_node_pool_chain();
void test_terrain_node_overlap_on_turn();
// La normal se calculaba a ±1 texel mientras la geometria se dibuja cada `stride`: iluminacion de una
// superficie que no existe, y se lee como pinchos DENTRO del nodo.
void test_terrain_node_normal_matches_geometry();
// El ruido se muestrea en dir*(R+baseH) y `baseH` cambia entre texeles: dos vertices contiguos leen
// el ruido en fases distintas. Separa lo tangencial (relieve) de lo radial (artefacto).
void test_terrain_node_radial_noise_shift();
// El corte de octavas esta en el TEXEL y Nyquist pide el doble: mide si los picos del campo crudo son
// aliasing de la octava mas fina.
void test_terrain_node_octave_cut_nyquist();
// La rejilla parte cada quad por la MISMA diagonal, asi que el error de faceta tiene el mismo signo
// en todo el nodo y se acumula en crestas. Mide el sesgo con signo, fija contra alterna.
void test_terrain_node_quad_diagonal_bias();
// La direccion del vertice en FLOAT cuantiza la superficie a ~0,5 m, del orden del texel: ruido a la
// frecuencia de la malla. Es lo que se ve como crestas finas.
void test_terrain_node_float_dir_quantisation();
// Pendiente del suelo QUE SE PISA a la escala de la celda de colision, contra los limites del
// controlador (50 gr, escalon 0,40 m). Contesta "andar se hace dificil" con numeros.
void test_terrain_collision_walkability();
// Los puntos de muestreo de la colision son offsets desde un ancla que salta con el jugador: cuando
// salta, el suelo se re-muestrea en otros sitios. El render no tiene ese problema (enteros).
void test_terrain_ring_anchor_drift();
// El render corta las octavas en el TEXEL del nodo (0,596 m) y la referencia en TERRAIN_TRIM_FLOOR
// (0,5 m): dos superficies distintas EN TODAS PARTES, no en vertices sueltos.
void test_terrain_render_vs_reference_cut();
// `ringSample` muestrea a nivel del mar y el vertice representa el punto a R+h: el punto real cae en
// `x*(R+h)/R`, no en `x`. Mide ese desplazamiento lateral y el error de altura que produce.
void test_terrain_ring_sample_lateral_shift();
// La histeresis del stride se PIERDE cuando un nodo sale del conjunto dibujado un frame: vuelve sin
// banda muerta. Es el hueco entre `walk_shimmer` (historia perfecta) y `stride_pop` (tamano del pop).
void test_terrain_node_stride_history_loss();
// El bobinado de la rejilla es CCW desde fuera en las SEIS caras: requisito para dibujar el pase v5
// con CullMode::Back en vez de None (que rasteriza las dos caras -> parpadeo en el limbo).
void test_terrain_node_winding();
void test_terrain_node_stitch();
void test_terrain_node_neighbours();

#endif // HARUKA_TEST_COMMON_H
