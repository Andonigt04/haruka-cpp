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
#include "core/planet/climate.h"

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
