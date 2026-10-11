#pragma once
// Lo COMPARTIDO por los tests del quadtree v5 (TestTerrainNode*.cpp): los helpers que usa mas de
// un archivo. Los de uno solo viven en ese archivo.
#include <chrono>
#include <tuple>
#include "Common/TestCommon.h"
#include <cmath>
#include "Haruka/World/Terrain/BaseField.h"          // baseFieldHeightAt: el muestreo del bake del CUBO
#include <glm/gtc/matrix_transform.hpp>   // perspective/lookAt: la matriz es la verdad de referencia
#include <cstring>
#include <vector>
#include <glm/glm.hpp>
#include "Haruka/World/Terrain/TerrainNode.h"
#include "Haruka/World/Terrain/TerrainNodePool.h"


using namespace Haruka::World::Terrain;

using Haruka::Tools::PlanetFace;

// --- Tests (cada uno en su archivo; los llama la tabla de grupos de main) ---
void test_terrain_node_overlap_on_turn();
void test_terrain_node_budget_starvation();
void test_terrain_node_level_balance();
void test_terrain_node_orbit_coverage();
void test_terrain_node_range_published();
void test_terrain_node_demand_with_range();
void test_terrain_node_inherited_range_flicker();
void test_terrain_node_walk_shimmer();
void test_terrain_node_ucenter_jitter();
void test_terrain_node_ancestor_disparity();
void test_terrain_node_face_seam_gap();
void test_terrain_node_face_seam();
void test_terrain_node_stitch();
void test_terrain_node_neighbours();
void test_terrain_node_stride_per_node();
void test_terrain_node_stride_pop();
void test_terrain_node_stride_seams();
void test_terrain_node_stride_history_loss();
void test_terrain_node_finer_neighbour_step();
void test_terrain_node_edge_audit_all();
void test_terrain_node_spike_hunt();
void test_terrain_node_stride_ramp_continuity();
void test_terrain_node_winding();
void test_terrain_node_morph_no_pop();
void test_terrain_node_distance_morph_per_vertex();
void test_terrain_render_vs_collision();
void test_terrain_node_as_heightfield();
void test_terrain_backup_bake_cost();
void test_terrain_node_octave_cut_nyquist();
void test_terrain_ring_sample_lateral_shift();
void test_terrain_render_vs_reference_cut();
void test_terrain_ring_anchor_drift();
void test_terrain_collision_walkability();
void test_terrain_node_float_dir_quantisation();
void test_terrain_node_quad_diagonal_bias();
void test_terrain_node_radial_noise_shift();
void test_terrain_node_normal_matches_geometry();
void test_terrain_prop_anchor_mismatch();
void test_terrain_collision_observer_dependence();
void test_terrain_collision_client_agreement();
void test_terrain_ground_query_cost();
void test_terrain_collision_refine_cost();
