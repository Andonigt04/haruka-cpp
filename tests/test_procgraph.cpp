// ================================================================================================
// Tests for ProcGraph node system, BiomeConfig, RGBAImage, and texture generation helpers.
// CPU-only (no GPU needed — all code is header-only/inline).
// ================================================================================================
#include "test_common.h"

#include <glm/glm.hpp>
#include <cmath>

#include "tools/procgraph/proc_graph.h"
#include "tools/procgraph/proc_math.h"
#include "tools/procgraph/proc_noise.h"
#include "tools/procgraph/proc_texture.h"
#include "tools/procgraph/proc_climate.h"
#include "tools/procgraph/tree_mesh.h"
#include "tools/procgraph/tree_prop.h"
#include "tools/procgraph/tree_spawn.h"
#include "core/planet/prop_layer.h"
#include "core/planet/prop_layer_spawn.h"
#include "core/planet/zone_shape.h"
#include "core/planet/climate.h"
#include "game/terrain_prop_field.h"
#include "game/prop_assembler.h"

using namespace Haruka::Tools::ProcGraph;

// ---------------------------------------------------------------------------
// Basic Graph: create nodes, connect, compile, evaluate
// ---------------------------------------------------------------------------
void test_procgraph_basic() {
    beginTest("procgraph_basic");

    Graph g;
    int c1 = g.emplaceNode<ConstNode>(3.14f);
    int c2 = g.emplaceNode<ConstNode>(2.72f);
    int add = g.emplaceNode<MathNode>(MathOp::Add);
    g.connect(c1, 0, add, 0);
    g.connect(c2, 0, add, 1);

    CHECK(g.compile(), "graph compiles (no cycles)");
    Value v = g.evaluate(add, 0, 0, 0, 0);
    CHECK(std::abs(v.asFloat() - (3.14f + 2.72f)) < 1e-5f, "Add: 3.14 + 2.72 == 5.86");
}

// ---------------------------------------------------------------------------
// FBMNode with Perlin noise produces varied output for different coords
// ---------------------------------------------------------------------------
void test_procgraph_fbm() {
    beginTest("procgraph_fbm");

    Graph g;
    int fbm = g.emplaceNode<FBMNode>(4, 2.0f, 0.5f, FBMNode::Sum, 42);
    FBMNode* n = static_cast<FBMNode*>(g.node(fbm));
    n->setNoiseFn([](float x, float y, float z, int s) {
        return PerlinNode::perlin(x, y, z, s);
    });

    g.compile();

    // Same coord → same value (determinism)
    float v1 = g.evaluate(fbm, 0, 1.23f, 4.56f, 7.89f).asFloat();
    float v2 = g.evaluate(fbm, 0, 1.23f, 4.56f, 7.89f).asFloat();
    CHECK(v1 == v2, "FBM is deterministic (same coord → same value)");

    // Different coords → different value (usually)
    float v3 = g.evaluate(fbm, 0, 9.87f, 6.54f, 3.21f).asFloat();
    CHECK(std::abs(v1 - v3) > 1e-6f, "FBM varies with coordinate");

    // Output in approx [-1, 1] range for Sum mode
    CHECK(v1 >= -1.0f && v1 <= 1.0f, "FBM Sum output in [-1, 1]");
}

// ---------------------------------------------------------------------------
// VoronoiNode produces cell-like patterns
// ---------------------------------------------------------------------------
void test_procgraph_voronoi() {
    beginTest("procgraph_voronoi");

    Graph g;
    int vor = g.emplaceNode<VoronoiNode>(0, 1.0f, VoronoiNode::Euclidean, VoronoiNode::F1);
    g.compile();

    float d0 = g.evaluate(vor, 0, 0.0f, 0.0f, 0.0f).asFloat();
    CHECK(d0 >= 0.0f, "Voronoi distance is non-negative");

    // Deterministic
    float d1 = g.evaluate(vor, 0, 0.0f, 0.0f, 0.0f).asFloat();
    CHECK(d0 == d1, "Voronoi is deterministic");

    // Near-center should be small distance
    float dNear = g.evaluate(vor, 0, 0.05f, 0.0f, 0.0f).asFloat();
    CHECK(dNear < 2.0f, "Voronoi near cell center has small distance");
}

// ---------------------------------------------------------------------------
// BlendNode mixes two inputs with a factor
// ---------------------------------------------------------------------------
void test_procgraph_blend() {
    beginTest("procgraph_blend");

    Graph g;
    int a = g.emplaceNode<ConstNode>(0.2f);
    int b = g.emplaceNode<ConstNode>(0.8f);
    int blend = g.emplaceNode<BlendNode>(BlendMode::Mix);
    int factor = g.emplaceNode<ConstNode>(0.5f);
    g.connect(a, 0, blend, 0);
    g.connect(b, 0, blend, 1);
    g.connect(factor, 0, blend, 2);
    g.compile();

    float v = g.evaluate(blend, 0, 0, 0, 0).asFloat();
    CHECK(std::abs(v - 0.5f) < 1e-5f, "Blend 0.2+0.8 at 0.5 → 0.5");

    // Verify extremes
    int f0 = g.emplaceNode<ConstNode>(0.0f);
    int blend2 = g.emplaceNode<BlendNode>(BlendMode::Mix);
    g.connect(a, 0, blend2, 0);
    g.connect(b, 0, blend2, 1);
    g.connect(f0, 0, blend2, 2);
    g.compile();
    CHECK(std::abs(g.evaluate(blend2, 0, 0, 0, 0).asFloat() - 0.2f) < 1e-5f,
          "Blend factor 0 → pure A");
}

// ---------------------------------------------------------------------------
// ClampNode clamps values correctly
// ---------------------------------------------------------------------------
void test_procgraph_clamp() {
    beginTest("procgraph_clamp");

    Graph g;
    int val = g.emplaceNode<ConstNode>(1.5f);
    int clamp = g.emplaceNode<ClampNode>();
    g.connect(val, 0, clamp, 0);
    g.compile();

    float v = g.evaluate(clamp, 0, 0, 0, 0).asFloat();
    CHECK(std::abs(v - 1.0f) < 1e-5f, "Clamp 1.5 → 1.0 (default max=1)");

    int neg = g.emplaceNode<ConstNode>(-0.5f);
    int clamp2 = g.emplaceNode<ClampNode>();
    g.connect(neg, 0, clamp2, 0);
    g.compile();
    CHECK(std::abs(g.evaluate(clamp2, 0, 0, 0, 0).asFloat()) < 1e-5f,
          "Clamp -0.5 → 0.0 (default min=0)");
}

// ---------------------------------------------------------------------------
// BiomeConfig::evaluate() — palette thresholds
// ---------------------------------------------------------------------------
void test_procgraph_biome_config() {
    beginTest("procgraph_biome_config");

    BiomeConfig cfg;
    // Default palette values
    glm::vec3 desert(0.62f, 0.53f, 0.35f);
    glm::vec3 steppe(0.47f, 0.44f, 0.26f);
    glm::vec3 grass(0.26f, 0.34f, 0.16f);
    glm::vec3 forest(0.18f, 0.30f, 0.13f);
    glm::vec3 jungle(0.13f, 0.31f, 0.11f);

    // H=0 → desert
    glm::vec3 c0 = cfg.evaluate(0.0f);
    CHECK(glm::all(glm::equal(c0, desert)), "H=0 → desert color");

    // H=1 → jungle
    glm::vec3 c1 = cfg.evaluate(1.0f);
    CHECK(glm::all(glm::equal(c1, jungle)), "H=1 → jungle color");

    // Midpoint checks: cumulative mixing means each result is a blend of
    // all biomes up to that point, not a pure biome color.
    float midSteppe = (cfg.steppeEdge0 + cfg.steppeEdge1) * 0.5f;
    glm::vec3 cSteppe = cfg.evaluate(midSteppe);
    float distSteppe = glm::distance(cSteppe, steppe);
    CHECK(distSteppe < 0.2f, "mid-steppe H → near steppe color");

    float midGrass = (cfg.grassEdge0 + cfg.grassEdge1) * 0.5f;
    glm::vec3 cGrass = cfg.evaluate(midGrass);
    CHECK(glm::distance(cGrass, grass) < 0.2f, "mid-grass H → near grass color");

    float midForest = (cfg.forestEdge0 + cfg.forestEdge1) * 0.5f;
    glm::vec3 cForest = cfg.evaluate(midForest);
    CHECK(glm::distance(cForest, forest) < 0.2f, "mid-forest H → near forest color");

    // Custom config
    BiomeConfig custom;
    custom.desert = glm::vec3(1.0f, 0.0f, 0.0f);
    custom.jungle = glm::vec3(0.0f, 0.0f, 1.0f);
    glm::vec3 cc0 = custom.evaluate(0.0f);
    CHECK(glm::all(glm::equal(cc0, glm::vec3(1.0f, 0.0f, 0.0f))),
          "custom desert → red");
    glm::vec3 cc1 = custom.evaluate(1.0f);
    CHECK(glm::all(glm::equal(cc1, glm::vec3(0.0f, 0.0f, 1.0f))),
          "custom jungle → blue");

    // -----------------------------------------------------------------------
    // SEGUNDO EJE: temperatura. Lo que estos asertos protegen es la PROPIEDAD que faltaba —
    // que a IGUAL humedad, el bioma cambie con la temperatura. Sin ella no existen tundra,
    // taiga ni sabana, y el planeta solo puede leerse como bandas de humedad.
    // -----------------------------------------------------------------------
    const float H_wet = 0.75f;   // húmedo: selva si hace calor, taiga si hace frío
    const float H_mid = 0.30f;   // humedad media: estepa templada, sabana caliente, tundra fría

    glm::vec3 wetCold = cfg.evaluate(H_wet, -6.0f);
    glm::vec3 wetTemp = cfg.evaluate(H_wet, 15.0f);
    glm::vec3 wetHot  = cfg.evaluate(H_wet, 30.0f);
    CHECK(glm::distance(wetCold, wetTemp) > 0.05f, "misma humedad, frío ≠ templado");
    CHECK(glm::distance(wetHot,  wetTemp) > 0.02f, "misma humedad, calor ≠ templado");
    CHECK(glm::distance(wetCold, cfg.taiga) < 0.12f, "frío + húmedo → taiga");

    glm::vec3 midCold = cfg.evaluate(H_mid, -6.0f);
    CHECK(glm::distance(midCold, cfg.tundra) < 0.15f, "frío + seco → tundra");

    // Hielo por debajo de la banda: a −25 °C manda el blanco, llueva o no.
    glm::vec3 freezingWet = cfg.evaluate(H_wet, -25.0f);
    glm::vec3 freezingDry = cfg.evaluate(0.05f, -25.0f);
    CHECK(glm::distance(freezingWet, cfg.ice) < 0.05f, "helado + húmedo → hielo");
    CHECK(glm::distance(freezingDry, cfg.ice) < 0.05f, "helado + seco → hielo");

    // COMPATIBILIDAD: la sobrecarga de un solo argumento debe seguir dando lo de siempre, o
    // cualquier escena existente cambiaría de aspecto sin tocar nada.
    for (float h = 0.0f; h <= 1.0f; h += 0.1f) {
        CHECK(glm::distance(cfg.evaluate(h), cfg.evaluate(h, 15.0f)) < 1e-6f,
              "evaluate(H) == evaluate(H, 15 °C)");
    }
    CHECK(glm::all(glm::equal(cfg.evaluate(0.0f), desert)), "templado H=0 sigue siendo desierto");
    CHECK(glm::all(glm::equal(cfg.evaluate(1.0f), jungle)), "templado H=1 sigue siendo selva");

    // La transición entre filas es CONTINUA: un escalón aquí saldría como una línea de nieve
    // recortada con la forma de la retícula del campo.
    // El paso es de 0.05 °C, no de medio grado: la banda del hielo son 10 °C y el salto de color
    // ice→taiga vale ~1.1, así que una rampa PERFECTAMENTE continua ya avanza ~0.08 por medio
    // grado. Con un paso grande el umbral mediría PENDIENTE, no continuidad — y una rampa
    // legítimamente empinada saldría marcada como discontinua. Un escalón real daría ~1.1 de
    // golpe, dos órdenes de magnitud por encima de este umbral.
    float maxJump = 0.0f;
    glm::vec3 prev = cfg.evaluate(H_wet, -30.0f);
    for (float t = -30.0f; t <= 40.0f; t += 0.05f) {
        glm::vec3 cur = cfg.evaluate(H_wet, t);
        maxJump = std::max(maxJump, glm::distance(prev, cur));
        prev = cur;
    }
    CHECK(maxJump < 0.02f, "sin escalones en el eje de temperatura");

    // -----------------------------------------------------------------------
    // EL PLANETA ENTERO, con el clima REAL que hornea el mapa de biomas. Lo que se protege aquí
    // no es un color: es que el rango de climas que produce `ClimateOutput` ALCANCE los biomas.
    // El modelo anterior daba 15 °C de máximo y humedad 0.15–0.24 sobre tierra, así que selva,
    // bosque y sabana eran inalcanzables y toda la tierra emergida salía desierto o estepa —
    // un planeta liso. Un test de `BiomeConfig` aislado NO lo detecta: la paleta estaba bien,
    // lo que no llegaba eran las entradas.
    // -----------------------------------------------------------------------
    {
        Haruka::Planet::ClimateOutput clim;
        auto biomeAt = [&](double latDeg) {
            const double r = latDeg * 3.14159265358979 / 180.0;
            const glm::dvec3 dir(std::cos(r), std::sin(r), 0.0);
            const double elevKm = 0.2;                    // tierra emergida, cota típica
            return cfg.evaluate((float)clim.humidity(dir, elevKm, false),
                                (float)clim.temperature(dir, elevKm, false));
        };

        CHECK(clim.temperature(glm::dvec3(1, 0, 0), 0.0, false) > 24.0,
              "ecuador por encima de 24 C (si no, la fila calida nunca se activa)");
        CHECK(clim.humidity(glm::dvec3(1, 0, 0), 0.2, false) > 0.70,
              "ecuador humedo sobre TIERRA (si no, la selva es inalcanzable)");
        CHECK(clim.humidity(glm::dvec3(std::cos(0.436), std::sin(0.436), 0.0), 0.2, false) < 0.20,
              "cinturon subtropical (25 grados) SECO");

        // Los tres biomas que el modelo viejo no podía producir sobre tierra.
        CHECK(glm::distance(biomeAt(0.0),  cfg.jungle) < 0.10f, "ecuador -> selva");
        CHECK(glm::distance(biomeAt(25.0), cfg.desert) < 0.10f, "25 grados -> desierto");
        CHECK(glm::distance(biomeAt(50.0), cfg.grass)  < 0.15f, "50 grados -> bosque templado");
        CHECK(glm::distance(biomeAt(75.0), cfg.tundra) < 0.15f, "75 grados -> tundra");
        CHECK(glm::distance(biomeAt(90.0), cfg.ice)    < 0.10f, "polo -> hielo");

        // Y que el planeta tenga VARIEDAD: recorrer la latitud debe recorrer paleta, no un tono.
        float spread = 0.0f;
        for (double a = 0.0; a <= 90.0; a += 5.0)
            for (double b = a; b <= 90.0; b += 5.0)
                spread = std::max(spread, glm::distance(biomeAt(a), biomeAt(b)));
        CHECK(spread > 0.8f, "el planeta recorre la paleta de polo a ecuador");
    }
}

// ---------------------------------------------------------------------------
// RGBAImage creation and pixel access
// ---------------------------------------------------------------------------
void test_procgraph_rgba_image() {
    beginTest("procgraph_rgba_image");

    RGBAImage img(64, 32);
    CHECK(img.width == 64, "RGBAImage width");
    CHECK(img.height == 32, "RGBAImage height");
    CHECK(img.sizeBytes() == (size_t)64 * 32 * 4, "RGBAImage size");

    // Set pixel, verify
    img.setPixel(10, 5, 128, 64, 32, 255);
    size_t idx = (5 * 64 + 10) * 4;
    CHECK(img.pixels[idx + 0] == 128, "pixel R");
    CHECK(img.pixels[idx + 1] == 64,  "pixel G");
    CHECK(img.pixels[idx + 2] == 32,  "pixel B");
    CHECK(img.pixels[idx + 3] == 255, "pixel A");

    // resize
    img.resize(16, 16);
    CHECK(img.width == 16, "resize width");
    CHECK(img.height == 16, "resize height");
}

// ---------------------------------------------------------------------------
// evaluateToRGBA produces correct-sized image
// ---------------------------------------------------------------------------
void test_procgraph_evaluate_to_rgba() {
    beginTest("procgraph_evaluate_to_rgba");

    Graph g;
    int c = g.emplaceNode<ConstNode>(0.5f);
    g.compile();

    RGBAImage img = evaluateToRGBA(g, c, 0, 32, 16);
    CHECK(img.width == 32, "evaluateToRGBA width");
    CHECK(img.height == 16, "evaluateToRGBA height");

    // All pixels should be ~128 (0.5 * 255 = 127.5)
    // Allow ±1 for rounding
    for (int y = 0; y < img.height; ++y) {
        for (int x = 0; x < img.width; ++x) {
            size_t idx = ((size_t)y * img.width + x) * 4;
            CHECK(img.pixels[idx + 0] == 127 || img.pixels[idx + 0] == 128,
                  "const 0.5 → pixel ~128");
        }
    }
}

// ---------------------------------------------------------------------------
// evaluateToNormalMap produces correct-sized output with central differences
// ---------------------------------------------------------------------------
void test_procgraph_evaluate_to_normal() {
    beginTest("procgraph_evaluate_to_normal");

    // A sloped height field: output = x (so normal should point in -x direction)
    struct SlopeNode : public Node {
        std::string name() const override { return "Slope"; }
        std::vector<SocketDesc> inputs() const override { return {}; }
        std::vector<SocketDesc> outputs() const override {
            return {{"Height", DataType::Float}};
        }
        void evaluate(const Value*, int, Value* outputs, int,
                      float x, float y, float) override {
            outputs[0] = Value::Float(x);
        }
    };

    Graph g;
    int slope = g.emplaceNode<SlopeNode>();
    g.compile();

    RGBAImage nm = evaluateToNormalMap(g, slope, 0, 16, 16);
    CHECK(nm.width == 16, "normal map width");
    CHECK(nm.height == 16, "normal map height");

    // At center, slope increases to the right → normal x should be negative
    size_t center = (7 * 16 + 7) * 4;
    CHECK(nm.pixels[center + 0] < 128, "slope → normal x < 0.5"); // negative x
}

// ---------------------------------------------------------------------------
// Graph compilation: cycle detection
// ---------------------------------------------------------------------------
void test_procgraph_cycle() {
    beginTest("procgraph_cycle");

    // Two math nodes feeding each other → cycle → compile fails
    Graph g;
    int a = g.emplaceNode<MathNode>(MathOp::Add);
    int b = g.emplaceNode<MathNode>(MathOp::Add);
    g.connect(a, 0, b, 0);
    g.connect(b, 0, a, 0);
    CHECK(!g.compile(), "cycle detected → compile fails");
}

// ---------------------------------------------------------------------------
// Graph determinism: same inputs always produce same outputs
// ---------------------------------------------------------------------------
void test_procgraph_determinism() {
    beginTest("procgraph_determinism");

    Graph g;
    int fbm = g.emplaceNode<FBMNode>(3, 2.0f, 0.5f, FBMNode::Sum, 123);
    FBMNode* n = static_cast<FBMNode*>(g.node(fbm));
    n->setNoiseFn([](float x, float y, float z, int s) {
        return PerlinNode::perlin(x, y, z, s);
    });
    g.compile();

    // Evaluate 100 times at the same point
    float first = g.evaluate(fbm, 0, 0.5f, 0.3f, 0.8f).asFloat();
    for (int i = 0; i < 100; ++i) {
        float v = g.evaluate(fbm, 0, 0.5f, 0.3f, 0.8f).asFloat();
        CHECK(v == first, "FBM is deterministic (100 evaluations)");
    }
}

// ---------------------------------------------------------------------------
// TreeMeshNode: deterministic procedural tree mesh from the node graph
// ---------------------------------------------------------------------------
void test_procgraph_tree_mesh() {
    beginTest("procgraph_tree_mesh");

    Graph g;
    int tree = g.emplaceNode<TreeMeshNode>(42, 6.5f, 0.35f, 1.0f, 8);
    g.compile();

    TreeMeshData mesh;
    CHECK(bakeTreeMesh(g, tree, mesh), "bakeTreeMesh produces a mesh");
    CHECK(mesh.vertexCount() > 100, "tree mesh has many vertices");
    CHECK(mesh.triangleCount() > 100, "tree mesh has many triangles");

    // --- determinismo: misma seed => misma malla exacta ---
    Graph g2;
    int tree2 = g2.emplaceNode<TreeMeshNode>(42, 6.5f, 0.35f, 1.0f, 8);
    g2.compile();
    TreeMeshData mesh2;
    CHECK(bakeTreeMesh(g2, tree2, mesh2), "bakeTreeMesh second mesh");
    bool samePos = mesh.positions.size() == mesh2.positions.size();
    bool sameIdx = mesh.indices.size() == mesh2.indices.size();
    if (samePos && sameIdx) {
        for (size_t i = 0; i < mesh.positions.size() && samePos; ++i)
            if (mesh.positions[i] != mesh2.positions[i]) samePos = false;
        for (size_t i = 0; i < mesh.indices.size() && sameIdx; ++i)
            if (mesh.indices[i] != mesh2.indices[i]) sameIdx = false;
    }
    CHECK(samePos, "same seed => identical vertex positions");
    CHECK(sameIdx, "same seed => identical indices");

    // --- seed distinta => malla distinta (no trivial) ---
    Graph g3;
    int tree3 = g3.emplaceNode<TreeMeshNode>(43, 6.5f, 0.35f, 1.0f, 8);
    g3.compile();
    TreeMeshData mesh3;
    CHECK(bakeTreeMesh(g3, tree3, mesh3), "bakeTreeMesh third mesh");
    bool differs = mesh3.positions.size() != mesh.positions.size();
    if (!differs) {
        size_t dif = 0;
        for (size_t i = 0; i < mesh3.positions.size(); ++i)
            if (mesh3.positions[i] != mesh.positions[i]) ++dif;
        differs = dif > 0;
    }
    CHECK(differs, "different seed => different mesh");

    // --- geometría: base del tronco en y≈0 cerca del origen ---
    float minY = 1e9f, minX = 1e9f, minZ = 1e9f;
    for (const auto& p : mesh.positions) {
        minY = std::min(minY, p.y);
        minX = std::min(minX, std::fabs(p.x));
        minZ = std::min(minZ, std::fabs(p.z));
    }
    CHECK(std::fabs(minY) < 0.5f, "trunk base sits at y≈0");
    CHECK(minX < 0.6f && minZ < 0.6f, "trunk base sits near origin (bend modest)");

    // --- copa por encima del tronco ---
    // canopyY = trunkH*0.95 y el peor blob sube hasta center.y + br*squashY
    // (mínimo garantizado ≈ trunkH*0.9), por encima del punto medio del tronco.
    const float trunkH = 6.5f * 0.62f;
    float maxY = -1e9f;
    for (const auto& p : mesh.positions) maxY = std::max(maxY, p.y);
    CHECK(maxY > trunkH * 0.9f, "canopy rises above trunk mid");

    // --- normales: unitarias y apuntando hacia fuera ---
    bool normalsOk = true;
    for (const auto& n : mesh.normals) {
        float l = glm::length(n);
        if (std::fabs(l - 1.0f) > 1e-3f) { normalsOk = false; break; }
    }
    CHECK(normalsOk, "all normals are unit length");

    // Orientación: la normal de cada vértice apunta alejándose del eje del árbol.
    // Solo el TRONCO (bajo las ramas, que arrancan en ~0.55*trunkH≈2.2 m) tiene
    // normales referidas al eje del árbol; las ramas apuntan a su propio eje.
    bool outward = true;
    for (size_t i = 0; i < mesh.positions.size(); ++i) {
        const glm::vec3& p = mesh.positions[i];
        if (p.y > 1.6f) continue;              // solo tronco inferior
        glm::vec3 radial(p.x, 0.0f, p.z);
        float r = glm::length(radial);
        if (r > 1e-4f && mesh.materialId[i] == 0) { // corteza: lateral del tronco
            if (glm::dot(mesh.normals[i], radial / r) < 0.0f) { outward = false; break; }
        }
    }
    CHECK(outward, "trunk normals point away from tree axis");

    // --- índices dentro de rango y múltiplos de 3 ---
    bool idxOk = (mesh.indices.size() % 3 == 0);
    for (size_t i = 0; i < mesh.indices.size() && idxOk; ++i)
        if (mesh.indices[i] >= mesh.vertexCount()) idxOk = false;
    CHECK(idxOk, "indices in range and triangle-aligned");

    // --- dos materiales: corteza y follaje ---
    bool hasBark = false, hasFoliage = false;
    for (auto id : mesh.materialId) {
        if (id == 0) hasBark = true;
        if (id == 1) hasFoliage = true;
    }
    CHECK(hasBark, "mesh has bark material (id 0)");
    CHECK(hasFoliage, "mesh has foliage material (id 1)");

    // --- parámetros distintos => malla distinta ---
    Graph g4;
    int tall = g4.emplaceNode<TreeMeshNode>(42, 12.0f, 0.35f, 1.0f, 8);
    g4.compile();
    TreeMeshData tallMesh;
    CHECK(bakeTreeMesh(g4, tall, tallMesh), "bakeTreeMesh tall mesh");
    float maxYTall = -1e9f;
    for (const auto& p : tallMesh.positions) maxYTall = std::max(maxYTall, p.y);
    CHECK(maxYTall > maxY, "taller tree parameter => taller mesh");
}

// ---------------------------------------------------------------------------
// TreeProp: ecológica (campo → acepta/rechaza) + Poisson determinista
// ---------------------------------------------------------------------------
void test_procgraph_tree_prop() {
    beginTest("procgraph_tree_prop");
    using Haruka::FieldSample;

    // --- base: bosque templado, húmedo, cauce bajo, pendiente leve → OK -------
    FieldSample good;
    good.humidity = 0.60f;
    good.tempC    = 18.0f;
    good.flow     = 0.05f;
    good.isLake   = false;

    TreeSuit s = treeSuitability(good, 0.15f, 1234u);
    CHECK(s.ok, "forest water/T frankly wet accepts");

    // --- rechazos (cada campo apagado por separado) ---------------------------
    FieldSample dry = good;  dry.humidity = 0.20f;
    CHECK(!treeSuitability(dry, 0.1f, 1u).ok, "desert (too dry) rejected");

    FieldSample frozen = good;  frozen.tempC = -10.0f;
    CHECK(!treeSuitability(frozen, 0.1f, 1u).ok, "frozen rejected");

    FieldSample hot = good;  hot.tempC = 45.0f;
    CHECK(!treeSuitability(hot, 0.1f, 1u).ok, "torrid rejected");

    FieldSample river = good;  river.flow = 0.7f;   // kRiverFlow = 0.45
    CHECK(!treeSuitability(river, 0.1f, 1u).ok, "in-river rejected");

    FieldSample lake = good;  lake.isLake = true;
    CHECK(!treeSuitability(lake, 0.1f, 1u).ok, "lake rejected");

    FieldSample cliff = good;
    CHECK(!treeSuitability(cliff, 0.9f, 1u).ok, "steep slope rejected");

    // --- determinismo: mismos campos -> misma densidad ------------------------
    TreeSuit a = treeSuitability(good, 0.15f, 4242u);
    TreeSuit b = treeSuitability(good, 0.15f, 4242u);
    CHECK(a.density == b.density && a.scale == b.scale, "same field → same result");

    // --- humedad distinta → densidad distinta (más humedad = más bosque) ------
    FieldSample wetter = good;  wetter.humidity = 0.80f;
    TreeSuit w = treeSuitability(wetter, 0.15f, 777u);
    CHECK(w.ok && w.density > treeSuitability(good, 0.15f, 777u).density,
          "more humidity → higher density");

    // --- Poisson: determinista y dentro de la celda --------------------------
    PoissonPoint p1 = treePoisson(99u, 10.0f, 0.1f, 7u);
    PoissonPoint p2 = treePoisson(99u, 10.0f, 0.1f, 7u);
    CHECK(p1.uv.x == p2.uv.x && p1.uv.y == p2.uv.y, "same cell → same Poisson point");
    CHECK(p1.uv.x >= 0.0f && p1.uv.x <= 1.0f, "Poisson point inside cell (x)");
    CHECK(p1.uv.y >= 0.0f && p1.uv.y <= 1.0f, "Poisson point inside cell (y)");

    // Celdas distintas → puntos distintos
    PoissonPoint q = treePoisson(100u, 10.0f, 0.1f, 7u);
    CHECK(p1.uv.x != q.uv.x || p1.uv.y != q.uv.y, "different cell → different point");
}

// ---------------------------------------------------------------------------
// TreeProp spawner: grid ecológica determinista sobre un campo simulado
// ---------------------------------------------------------------------------
namespace {
// Campo artificial: sintetiza la ecología/cota de un punto de la retícula.
struct SimField : public Haruka::Tools::ProcGraph::IPropField {
    float h0 = 0.0f;                 // cota plana por defecto
    bool  rejectAll = false;        // si se simula un desierto/ribera inutilizable
    Haruka::FieldSample fs;         // ecología base del punto

    Haruka::FieldSample sampleAt(float px, float pz) const override {
        (void)px; (void)pz;
        if (rejectAll) { Haruka::FieldSample d = fs; d.humidity = 0.05f; return d; }
        return fs;
    }
    float heightAt(float, float) const override { return h0; }
};
}

void test_procgraph_tree_spawn() {
    beginTest("procgraph_tree_spawn");

    Haruka::FieldSample good;
    good.humidity = 0.60f; good.tempC = 18.0f; good.flow = 0.05f; good.isLake = false;

    // --- campo bueno en toda la retícula → se llenan TODAS las celdas ---------
    SimField green; green.fs = good; green.rejectAll = false; green.h0 = 120.0f;
    PropGrid grid;
    grid.spanM = 1000.0f; grid.cellsPerSide = 8; grid.seed = 7u;

    auto trees = spawnTrees(green, grid);
    CHECK(!trees.empty(), "habitable field produces trees");

    // Todos los árboles quedan dentro de la retícula y correctamente asentados.
    bool inRange = true, seated = true;
    for (const auto& t : trees) {
        if (t.localPos.x < 0.0f || t.localPos.x > grid.spanM ||
            t.localPos.z < 0.0f || t.localPos.z > grid.spanM) inRange = false;
        if (std::fabs(t.heightM - 120.0f) > 1e-4f) seated = false;
    }
    CHECK(inRange, "all placed trees are inside the patch");
    CHECK(seated, "all trees sit at the field terrain height");

    // --- determinismo: misma seed → misma lista exacta -----------------------
    auto trees2 = spawnTrees(green, grid);
    bool same = trees.size() == trees2.size();
    if (same) {
        for (size_t i = 0; i < trees.size() && same; ++i)
            if (trees[i].meshSeed != trees2[i].meshSeed ||
                trees[i].heightM  != trees2[i].heightM) same = false;
    }
    CHECK(same, "same seed + field → identical placed trees");

    // --- seed distinta → colocación distinta (no trivial) --------------------
    PropGrid g2 = grid; g2.seed = 8u;
    auto trees2seed = spawnTrees(green, g2);
    bool different = trees2seed.size() != trees.size();
    if (!different) {
        size_t dif = 0;
        for (size_t i = 0; i < trees.size(); ++i)
            if (trees[i].meshSeed != trees2seed[i].meshSeed) ++dif;
        different = dif > 0;
    }
    CHECK(different, "different seed → different placement");

    // --- campo inhóspito (desierto) → sin árboles ----------------------------
    SimField dry = green; dry.fs.humidity = 0.05f;
    auto dryTrees = spawnTrees(dry, grid);
    CHECK(dryTrees.empty(), "desert field spawns no trees");
}

// ---------------------------------------------------------------------------
// ZoneShape: zonas GEOMÉTRICAS con perímetro (círculo y polígono en lat/lon)
// ---------------------------------------------------------------------------
void test_procgraph_zone_shape() {
    beginTest("procgraph_zone_shape");
    using namespace Haruka::Planet;
    const double R = 6371000.0;
    auto D = [](double lat, double lon) {          // (lat,lon) grados → dir float (como el motor)
        const glm::dvec3 d = latLonDegToDir(lat, lon);
        return glm::vec3((float)d.x, (float)d.y, (float)d.z);
    };

    // Círculo: centro (lat=10, lon=20), radio 50 km. 1° ≈ 111 km en meridiano.
    ZoneShape c;
    c.hasCircle = true;
    c.circleCenterDeg = glm::dvec2(10.0, 20.0);
    c.circleRadiusM   = 50000.0;
    CHECK(zoneShapeContains(c, D(10.0, 20.0), R), "circle center inside");
    CHECK(zoneShapeContains(c, D(10.2, 20.0), R), "~22 km inside circle");
    CHECK(!zoneShapeContains(c, D(11.0, 20.0), R), "~111 km outside circle");

    // Polígono: cuadrado ±0.5° alrededor del (0,0).
    ZoneShape p;
    p.hasPolygon = true;
    p.polygonDeg = { {-0.5, -0.5}, {-0.5, 0.5}, {0.5, 0.5}, {0.5, -0.5} };
    CHECK(zoneShapeContains(p, D(0.0, 0.0), R), "polygon centroid inside");
    CHECK(zoneShapeContains(p, D(0.4, 0.4), R), "polygon near-corner inside");
    CHECK(!zoneShapeContains(p, D(1.0, 0.0), R), "outside the polygon");

    // Un punto también dentro de un PERÍMETRO con muesca (cóncavo): even-odd cuenta el doble cruce.
    ZoneShape notch;
    notch.hasPolygon = true;
    notch.polygonDeg = { {-1, -1}, { 1,-1}, { 1, 0}, {-0.2, 0}, {-0.2, 1}, {-1, 1} };
    CHECK(zoneShapeContains(notch, D(0.3, -0.5), R), "inside the notch body");
    CHECK(!zoneShapeContains(notch, D(0.5,  0.5), R), "inside the notch cut");
}

// ---------------------------------------------------------------------------
// PropLayerTable: capas de props como TABLA de materiales (espejo del terreno)
// ---------------------------------------------------------------------------
void test_procgraph_prop_layer() {
    beginTest("procgraph_prop_layer");
    using namespace Haruka::Planet;

    // --- defaults(): el espejo de TerrainMaterialTable::defaults() -------------
    PropLayerTable t = PropLayerTable::defaults();
    CHECK(t.layers.size() >= 4, "default table declares build/path/tree/rock");
    CHECK(t.layers[0].mesh == "house", "first layer (highest priority) is construction");
    CHECK(t.layers[1].mesh == "path", "second layer is path");
    CHECK(t.indexOfMesh("tree") > t.indexOfMesh("house"),
          "tree is placed AFTER construction");
    CHECK(t.indexOfMesh("rock") > t.indexOfMesh("tree"), "rock after tree");

    // --- el ORDEN de la lista = prioridad de construcción ---------------------
    const int b = t.indexOfMesh("house");  // casas (nombre "build", instala "house")
    const int p = t.indexOfMesh("path");   // caminos
    const int e = t.indexOfMesh("tree");   // árboles
    CHECK(b >= 0 && p >= 0 && e >= 0, "defaults expose build/path/tree indices");
    CHECK(b < p && p < e, "construction < path < tree (order is build priority)");

    // --- softBand: límites duros sin feather, degradado con feather ------------
    CHECK(softBand(0.50f, 0.0f, 1.0f, 0.0f) == 1.0f, "softBand no-feather inside → 1");
    CHECK(softBand(-1.0f, 0.0f, 1.0f, 0.0f) == 0.0f, "softBand no-feather below → 0");
    CHECK(softBand(0.02f, 0.0f, 1.0f, 0.08f) > 0.0f, "softBand feather partial below-min");
    CHECK(softBand(0.50f, 0.0f, 1.0f, 0.08f) == 1.0f, "softBand feather fully inside → 1");

    // --- coverage: un punto rescata la capa que le toca -------------------------
    PropLayer& build = t.layers[(size_t)b];
    Haruka::FieldSample fs; fs.humidity = 0.60f; fs.tempC = 18.0f; fs.flow = 0.1f; fs.isLake = false;
    CHECK(build.coverage(fs, 0.1f) > 0.0f, "habitable flat point covered by build layer");
    CHECK(build.coverage(fs, 0.9f) <= 1e-6f, "steep slope not covered by build");
    Haruka::FieldSample arid = fs; arid.humidity = 0.05f;
    CHECK(build.coverage(arid, 0.1f) <= 1e-6f, "desert not covered by build");
}

// ---------------------------------------------------------------------------
// PropPlacer: pase ORDENADO con claims — "la casa convierte al árbol"
// ---------------------------------------------------------------------------
namespace {
// Campo PLANO para placeProps (IPropField ecológico, GL-free).
struct FlatGreenField : Haruka::Tools::ProcGraph::IPropField {
    Haruka::FieldSample fs;
    Haruka::FieldSample sampleAt(float, float) const override { return fs; }
    float heightAt(float, float) const override { return 100.0f; }
    // Coherente con `TerrainMaterialTable::defaults()`: un campo húmedo (hum 0.60) es el
    // material `green`, así el `when` de la capa `tree` lo deja instalar (ver defaults()).
    std::string layerAt(float, float) const override { return "green"; }
};
}

void test_procgraph_prop_placer() {
    beginTest("procgraph_prop_placer");
    using namespace Haruka::Planet;

    Haruka::FieldSample habitable;
    habitable.humidity = 0.60f; habitable.tempC = 18.0f;
    habitable.flow = 0.1f; habitable.isLake = false;
    FlatGreenField f; f.fs = habitable;

    PropGrid grid; grid.spanM = 512.0f; grid.cellsPerSide = 8; grid.seed = 1u;

    // ESCENARIO A — sin claims (claimRadius 0): la casa y el árbol compiten a pleno.
    auto noClaims = PropLayerTable::defaults();
    noClaims.layers[(size_t)noClaims.indexOfMesh("house")].density    = 1.0f;
    noClaims.layers[(size_t)noClaims.indexOfMesh("tree")].density     = 1.0f;
    noClaims.layers[(size_t)noClaims.indexOfMesh("path")].density     = 0.0f;
    noClaims.layers[(size_t)noClaims.indexOfMesh("house")].claimRadius = 0.0f;
    PropsPlaced a = placeProps(f, grid, noClaims);

    // ESCENARIO B — la casa reclama su solar (16 m): los árboles pierden los spots cercanos.
    auto withClaims = PropLayerTable::defaults();
    withClaims.layers[(size_t)withClaims.indexOfMesh("house")].density    = 1.0f;
    withClaims.layers[(size_t)withClaims.indexOfMesh("tree")].density     = 1.0f;
    withClaims.layers[(size_t)withClaims.indexOfMesh("path")].density     = 0.0f;
    withClaims.layers[(size_t)withClaims.indexOfMesh("house")].claimRadius = 16.0f;
    PropsPlaced b = placeProps(f, grid, withClaims);

    // Ambas colocan construcción (la capa manda en suelo habitable).
    auto countMesh = [](const PropsPlaced& p, const char* mesh) {
        int n = 0; for (const auto& pr : p.props) if (pr.mesh == mesh) ++n; return n;
    };
    CHECK(countMesh(a, "house") > 0, "construction layer places houses");
    CHECK(countMesh(a, "tree") > 0, "trees exist when nothing claims territory");

    // EL ORDEN: los claims de la casa RECORTAN los árboles (propiedad, no casualidad).
    CHECK(countMesh(b, "tree") < countMesh(a, "tree"),
          "house claims reduce the number of trees (order = priority)");

    // Y NINGÚN árbol queda dentro del radio de exclusión de una casa.
    const int houseIdx = withClaims.indexOfMesh("house");
    bool treeTrespassed = false;
    for (const auto& pr : b.props) {
        if (pr.mesh != "tree") continue;
        for (const auto& c : b.claims) {
            if (c.layer != houseIdx) continue;
            const float dx = pr.localPos.x - c.pos.x, dz = pr.localPos.z - c.pos.y;
            if (dx*dx + dz*dz <= c.radius * c.radius) treeTrespassed = true;
        }
    }
    CHECK(!treeTrespassed, "no tree inside a house solar claim (order = priority)");

    // Determinismo: misma semilla + campo → misma colocación exacta.
    PropsPlaced again = placeProps(f, grid, withClaims);
    bool same = b.props.size() == again.props.size();
    if (same) for (size_t i = 0; i < b.props.size() && same; ++i)
        if (b.props[i].mesh != again.props[i].mesh ||
            b.props[i].localPos != again.props[i].localPos)
            same = false;
    CHECK(same, "same seed+field → identical placement (deterministic)");
}

// ---------------------------------------------------------------------------
// TerrainPropField: adapta el campo esférico del planeta al parche plano de props
// ---------------------------------------------------------------------------
void test_procgraph_terrain_prop_field() {
    beginTest("procgraph_terrain_prop_field");

    // Planeta "de juguete": radio real; base tangente del parche en el ecuador.
    const double R = 6.371e6;                 // Tierra
    const glm::vec3 centerDir(1, 0, 0);       // centro del parche en el ecuador
    const glm::vec3 u(0, 1, 0);               // px → hacia el polo
    const glm::vec3 v(0, 0, 1);               // pz → hacia el este

    // Campo sintético: contesta con la cota como si el suelo fuera uniforme.
    Haruka::TerrainPropField f;
    f.configure(centerDir, R, u, v);
    f.fieldFn  = [](const glm::vec3&) {
        Haruka::FieldSample s; s.humidity = 0.6f; s.tempC = 18.0f; return s;
    };
    // Colina simulada: la cota crece con la latitud (px). Así "hacia el polo subimos" es real.
    f.heightFn = [](const glm::vec3& dir) { return dir.y * 5000.0f; };

    // (0,0) del parche = el centro: dirección unitaria, el campo sigue el ecuador.
    Haruka::FieldSample c = f.sampleAt(0.0f, 0.0f);
    CHECK(c.humidity == 0.6f, "center of patch samples the field");
    CHECK(std::fabs(f.heightAt(0.0f, 0.0f)) < 1.0f, "center of patch at terrain height");

    // Un punto a 1 km hacia el polo (px>0) NO es el centro: la dirección cambia y la cota crece
    // (subimos la colina). La dirección en px=1000 desvía ~1/6371 rad → y≈0.000157 → 0.78 m.
    float h1000 = f.heightAt(1000.0f, 0.0f);
    CHECK(h1000 > 0.0f && h1000 < 50.0f, "1 km tangent offset climbs the hill a little");

    // Determinismo: mismo punto → misma dirección (mismo FieldSample/cota).
    CHECK(f.sampleAt(500.0f, -300.0f).humidity == f.sampleAt(500.0f, -300.0f).humidity,
          "same patch point → same field sample");
    CHECK(f.heightAt(500.0f, -300.0f) == f.heightAt(500.0f, -300.0f),
          "same patch point → same height");

    // El parche es una APROXIMACIÓN TANGENTE: sin fieldFn devuelve un sample por defecto, sin
    // crashear. (El default de FieldSample es humidity=0.5, tempC=15 — no 0.)
    Haruka::TerrainPropField empty;
    empty.configure(centerDir, R, u, v);
    CHECK(empty.sampleAt(0.0f, 0.0f).humidity == 0.5f,
          "no fieldFn → default field sample (safe)");
    CHECK(std::fabs(empty.heightAt(0.0f, 0.0f)) < 1e-6f, "no heightFn → 0 m (safe)");
}

// ---------------------------------------------------------------------------
// PropAssembler: de capa colocada a SceneObject — fase CPU (bake + anclaje).
// La subida a GPU (assembleSceneObjects → setMesh) necesita device RHI y NO
// corre en este suite: aquí se valida que el bake produce malla + texturas y
// que el objeto quedaría asentado sobre el suelo que muestreó el campo.
// ---------------------------------------------------------------------------
void test_procgraph_prop_assembler() {
    beginTest("procgraph_prop_assembler");
    using namespace Haruka::Planet;

    // Mismo escenario que el placer: campo habitable plano, retícula pequeña.
    Haruka::FieldSample habitable;
    habitable.humidity = 0.60f; habitable.tempC = 18.0f;
    habitable.flow = 0.1f; habitable.isLake = false;
    FlatGreenField f; f.fs = habitable;

    PropGrid grid; grid.spanM = 256.0f; grid.cellsPerSide = 4; grid.seed = 7u;
    auto table = PropLayerTable::defaults();
    table.layers[(size_t)table.indexOfMesh("house")].density    = 0.0f;
    table.layers[(size_t)table.indexOfMesh("path")].density     = 0.0f;
    table.layers[(size_t)table.indexOfMesh("tree")].density     = 1.0f;

    PropsPlaced placed = placeProps(f, grid, table);
    CHECK(placed.props.size() > 0, "assembler starts from a placed prop set");

    // --- FASE CPU 1: bake de malla + texturas (GL-free) ----------------------
    auto baked = bakeProps(placed, "/tmp/haruka_assembler_test", "testtree");
    CHECK(baked.size() == placed.props.size(),
          "every placed prop bakes into an asset set");
    if (!baked.empty()) {
        const BakedProp& b = baked.front();
        CHECK(b.mesh.vertexCount() > 0,   "baked mesh has vertices");
        CHECK(b.mesh.triangleCount() > 0, "baked mesh has triangles");
        CHECK(b.mesh.materialId.size() == b.mesh.vertexCount(),
              "materialId per vertex (bark/foliage)");
        CHECK(b.tex.ok, "textures baked ok");
        CHECK(!b.tex.albedo.empty() && !b.tex.normal.empty() && !b.tex.ao.empty(),
              "albedo/normal/ao paths written");
        // El bake respeta la escala de la capa (determinista, seed = meshSeed).
        CHECK(b.mesh.vertexCount() > 4, "mesh is a real tree (not degenerate)");
    }

    // --- FASE CPU 2: anclaje al mundo = el mismo del campo -------------------
    // El prop se coloca en planetCenter + dir*(radius + heightM), con dir del
    // ancla del parche. Si el objeto quedara exactamente sobre el suelo del
    // campo, su distancia al centro es radius+heightM.
    const double R = 6.371e6;
    const glm::vec3 centerDir(1, 0, 0);
    const glm::vec3 u(0, 1, 0), v(0, 0, 1);
    PropWorldAnchor anchor;
    anchor.planetCenter = glm::dvec3(-4000.0, 0.0, 8000.0);
    anchor.radius       = R;
    anchor.centerDir    = centerDir;
    anchor.u            = u;
    anchor.v            = v;

    for (const auto& p : placed.props) {
        const glm::vec3 dir = propPatchDir(anchor, p.localPos.x, p.localPos.z);
        CHECK(std::fabs(glm::length(dir) - 1.0f) < 1e-5f, "patch dir is unit length");
        const glm::dvec3 world = anchor.planetCenter + glm::dvec3(dir) * (R + p.heightM);
        const double len = glm::length(world - anchor.planetCenter);
        // Tolerancia PLANETARIA, no 1e-3: `dir` es unitario en float32 (≈1e-7 de error relativo),
        // que a radio 6.371e6 m amplifica a ≈0.5 m. El objeto está asentado si el radial es
        // radius+heightM dentro de ese margen de precisión de la normalización.
        CHECK(std::fabs(len - (R + p.heightM)) < 2.0,
              "assembled prop sits exactly on the field height");
        // La dirección del ancla coincide con la del campo (mismo cálculo):
        CHECK(glm::length(propPatchDir(anchor, p.localPos.x, p.localPos.z) - dir) < 1e-6f,
              "patch dir stable (deterministic)");
    }
}

// ---------------------------------------------------------------------------
// PropLayerTable JSON: round-trip con el formato `surfaceConfig.propLayers`.
// El IDE edita ESA lista; `fromJSON` debe reconstruir la tabla EN EL MISMO
// ORDEN (prioridad) y el mismo `placeProps` que usaría el motor.
// ---------------------------------------------------------------------------
void test_procgraph_prop_layer_json() {
    beginTest("procgraph_prop_layer_json");
    using namespace Haruka::Planet;

    // Tabla con orden intencional: house < path < tree < rock (prioridad decreciente).
    auto table = PropLayerTable::defaults();
    CHECK(table.layers.size() == 4, "defaults expose build/path/tree/rock layers");
    CHECK(table.indexOfMesh("house") == 0 && table.indexOfMesh("path") == 1 &&
          table.indexOfMesh("tree") == 2 && table.indexOfMesh("rock") == 3,
          "defaults order is construction priority");

    // Serializa → JSON del IDE (`surfaceConfig["propLayers"]`).
    nlohmann::json j = table.toJSON();
    CHECK(j.is_array() && j.size() == 4, "toJSON emits one object per layer");

    // Round-trip: la tabla reconstruida conserva orden y valores.
    nlohmann::json wrapped; wrapped["propLayers"] = j;
    auto back = PropLayerTable::fromJSON(wrapped);
    CHECK(back.layers.size() == 4, "fromJSON reads every layer");
    bool order = true, fields = true;
    for (size_t i = 0; i < back.layers.size() && order; ++i)
        if (back.layers[i].mesh != table.layers[i].mesh ||
            back.layers[i].name != table.layers[i].name) order = false;
    const PropLayer& tr = back.layers[(size_t)back.indexOfMesh("tree")];
    if (std::fabs(tr.humMin - 0.15f) > 1e-4f || std::fabs(tr.humMax - 1.0f) > 1e-4f ||
        std::fabs(tr.density - 0.9f) > 1e-4f || std::fabs(tr.claimRadius - 0.3f) > 1e-4f ||
        tr.mesh != "tree") fields = false;
    CHECK(order,  "fromJSON preserves list order = build priority");
    CHECK(fields, "fromJSON preserves per-layer values (humidity/density/claim)");

    // densityMap: opcional en JSON, se conserva en el round-trip.
    nlohmann::json withMap = j;
    withMap[(size_t)back.indexOfMesh("tree")]["densityMap"] = "assets/textures/trees.png";
    nlohmann::json wmap; wmap["propLayers"] = withMap;
    auto backMap = PropLayerTable::fromJSON(wmap);
    const PropLayer& trMap = backMap.layers[(size_t)backMap.indexOfMesh("tree")];
    CHECK(trMap.densityMap == "assets/textures/trees.png",
          "fromJSON reads per-layer densityMap");
    CHECK(backMap.layers[(size_t)backMap.indexOfMesh("rock")].densityMap.empty(),
          "layers without densityMap stay empty");
    // coverage multiplica por el mapa: con mapa 0 el punto está vedado aunque el clima quepa.
    // `layerName "green"`: la capa `tree` por defecto lleva `when` que exige material húmedo
    // (ver defaults()), y este es un campo verde.
    Haruka::FieldSample fs;
    fs.humidity = 0.8f; fs.tempC = 20.0f; fs.elevKm = 0.3f;
    CHECK(std::fabs(trMap.coverage(fs, 0.1f, 0.0f, "", "green")) < 1e-6f,
          "densityMap=0 vetoes the spot even inside climate bands");
    CHECK(std::fabs(trMap.coverage(fs, 0.1f, 0.75f, "", "green") -
                    0.75f * trMap.coverage(fs, 0.1f, 1.0f, "", "green")) < 1e-6f,
          "densityMap scales coverage linearly");

    // ZONAS: `zones` declara DÓNDE (nombre de material del zoneMap) puede instalar la capa.
    PropLayer zoned = PropLayerTable::defaults().layers[
        (size_t)PropLayerTable::defaults().indexOfMesh("tree")];
    CHECK(zoned.zoneAllowed("forest") && zoned.zoneAllowed("") && zoned.zoneAllowed("x"),
          "no zones declared → any zone (or none) is allowed");
    zoned.zones = {"forest", "wetland"};
    CHECK(zoned.zoneAllowed("forest") && zoned.zoneAllowed("wetland"),
          "zone in the declared list is allowed");
    CHECK(!zoned.zoneAllowed("desert") && !zoned.zoneAllowed("tundra"),
          "zone outside the declared list is vetoed");
    CHECK(!zoned.zoneAllowed(""), "declared zones + no zone map → vetoed (no silent forest)");
    // coverage aplica el filtro: clima OK pero zona no declarada → 0. `layerName "green"` para
    // que el `when` de la capa `tree` por defecto no vete el punto antes de la zona.
    const float covIn  = zoned.coverage(fs, 0.1f, 1.0f, "forest", "green");
    const float covOut = zoned.coverage(fs, 0.1f, 1.0f, "desert", "green");
    CHECK(covIn > 0.0f, "zone allowed → coverage from climate bands");
    CHECK(std::fabs(covOut) < 1e-6f, "zone vetoed → coverage 0 regardless of climate");

    // zones viaja por JSON (round-trip del inspector).
    nlohmann::json zj = j;
    zj[(size_t)back.indexOfMesh("tree")]["zones"] = {"forest", "wetland"};
    nlohmann::json wz; wz["propLayers"] = zj;
    auto backZ = PropLayerTable::fromJSON(wz);
    const PropLayer& trZ = backZ.layers[(size_t)backZ.indexOfMesh("tree")];
    CHECK(trZ.zones.size() == 2 && trZ.zones[0] == "forest" && trZ.zones[1] == "wetland",
          "fromJSON reads per-layer zones");
    CHECK(backZ.layers[(size_t)backZ.indexOfMesh("rock")].zones.empty(),
          "layers without zones stay unrestricted");

    // JSON ausente o basura → tabla vacía (sin props), no un crash.
    CHECK(PropLayerTable::fromJSON(nlohmann::json::object()).layers.empty(),
          "no propLayers → empty table (safe)");
    nlohmann::json garbage; garbage["propLayers"] = "not-an-array";
    CHECK(PropLayerTable::fromJSON(garbage).layers.empty(),
          "non-array propLayers → empty table (safe)");

    // Una entrada sin name ni mesh es basura y se descarta.
    nlohmann::json withBad = j;
    withBad.push_back(nlohmann::json::object({{"density", 0.5}}));
    nlohmann::json wbad; wbad["propLayers"] = withBad;
    CHECK(PropLayerTable::fromJSON(wbad).layers.size() == 4,
          "malformed layer entry is dropped");

    // ===== CONDICIÓN BOOLEANA `when`: `layer` (material) y `zone` (zona nombrada). =====
    PropLayer cond = PropLayerTable::defaults().layers[
        (size_t)PropLayerTable::defaults().indexOfMesh("house")];
    CHECK(cond.whenAllowed("sand", "oasis"), "no when → always allowed");

    // Parser: sintaxis válida.
    std::string perr;
    auto expr = parsePropCond("layer != sand || zone == oasis", perr);
    CHECK(expr != nullptr, "parser accepts 'layer != sand || zone == oasis'");
    CHECK(parsePropCond("!zone == ''", perr) != nullptr, "parser accepts quoted empty string");
    CHECK(parsePropCond("(layer == forest && !zone == oasis) || zone == city", perr) != nullptr,
          "parser accepts parens + && + || + !");
    // Sintaxis inválida → nullptr con mensaje.
    CHECK(parsePropCond("sand == layer", perr) == nullptr, "operand must be layer/zone first");
    CHECK(parsePropCond("layer == ", perr) == nullptr, "missing value after operator");
    CHECK(parsePropCond("layer = sand", perr) == nullptr, "single '=' is not a valid operator");
    CHECK(parsePropCond("", perr) == nullptr, "empty when is invalid (but treated as absent)");
    CHECK(parsePropCond("(layer == forest", perr) == nullptr, "unbalanced paren");

    // Evaluación: la semántica de capa/zona.
    auto sandNoOasis = parsePropCond("layer != sand || zone == oasis", perr);
    PropCondCtx inSand{ "sand", "" };
    PropCondCtx inSandOasis{ "sand", "oasis" };
    PropCondCtx inForest{ "forest", "" };
    CHECK(sandNoOasis && sandNoOasis->eval(inSand) == false, "sand, no oasis → false");
    CHECK(sandNoOasis->eval(inSandOasis) == true, "sand BUT inside oasis → true");
    CHECK(sandNoOasis->eval(inForest) == true, "forest (layer != sand) → true");
    auto oasisOnly = parsePropCond("zone == oasis", perr);
    CHECK(oasisOnly->eval(PropCondCtx{"sand", ""}) == false, "zone == oasis: outside → false");
    CHECK(oasisOnly->eval(PropCondCtx{"sand", "oasis"}) == true, "zone == oasis: inside → true");

    // coverage integra `when` ADEMÁS de `zones`: punto dentro de la zona pero que no pasa la
    // condición → 0. Punto vedado por zona aunque la condición diga sí → 0.
    cond.when = "zone == pueblo && layer != sand";
    cond.zones = {"pueblo"};
    const float covCondIn = cond.coverage(fs, 0.1f, 1.0f, "pueblo", "land");
    const float covCondOut = cond.coverage(fs, 0.1f, 1.0f, "pueblo", "sand");
    CHECK(covCondIn > 0.0f, "when true + zone listed → climate bands apply");
    CHECK(std::fabs(covCondOut) < 1e-6f, "when false (layer == sand) → vetoed");
    cond.zones.clear();
    CHECK(std::fabs(cond.coverage(fs, 0.1f, 1.0f, "", "land")) < 1e-6f,
          "when true but zone empty → coverage 0 (zone filter with when still needs the zone)");

    // `when` viaja por JSON (round-trip del inspector) y no rompe las capas sin él.
    nlohmann::json wj = j;
    wj[(size_t)back.indexOfMesh("house")]["when"] = "zone == pueblo";
    nlohmann::json wjraw; wjraw["propLayers"] = wj;
    auto backWhen = PropLayerTable::fromJSON(wjraw);
    CHECK(backWhen.layers[(size_t)backWhen.indexOfMesh("house")].when == "zone == pueblo",
          "fromJSON reads per-layer when");
    // Las capas que no declaran `when` (aquí `path`) viajan vacías; las que sí (tree/rock en
    // defaults()) conservan el suyo.
    CHECK(backWhen.layers[(size_t)backWhen.indexOfMesh("path")].when.empty(),
          "layers without when stay empty");
    // toJSON vuelve a emitir `when`.
    const nlohmann::json round = backWhen.toJSON();
    bool foundWhen = false;
    for (const auto& l : round)
        if (l.contains("when") && l["when"] == "zone == pueblo") foundWhen = true;
    CHECK(foundWhen, "toJSON emits the when condition");
}
