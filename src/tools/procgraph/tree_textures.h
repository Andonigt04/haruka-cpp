#pragma once

// ===========================================================================
// Texturas de árbol (estilo anime) generadas por semilla.
//
// Construye el MISMO grafo que el preset "Árbol" del editor de node graph del
// IDE (corteza: grano perlin − ramas − nudos; copa: fbm × detalle de hoja) y
// hornea albedo/normal/roughness/ao a PNG usando solo el motor, para que un
// prop colocado reciba sus texturas al instante sin abrir el editor.
// ===========================================================================

#include "proc_graph.h"
#include "proc_noise.h"
#include "proc_math.h"
#include "proc_organic.h"
#include "proc_texture.h"
#include "io/image_writer.h"

#include <filesystem>
#include <string>
#include <vector>

namespace Haruka { namespace Tools { namespace ProcGraph {

// ===========================================================================
// Combine RGB del motor (3 floats -> Vec3) para el color del albedo.
// ===========================================================================
class TreeCombineRGBNode : public Node {
public:
    std::string name() const override { return "Combine RGB"; }
    std::vector<SocketDesc> inputs() const override {
        return {{"R", DataType::Float}, {"G", DataType::Float}, {"B", DataType::Float}};
    }
    std::vector<SocketDesc> outputs() const override {
        return {{"Color", DataType::Vec3}};
    }
    void evaluate(const Value* inputs, int, Value* outputs, int,
                  float, float, float) override {
        outputs[0] = Value::Vec3(inputs[0].asFloat(), inputs[1].asFloat(), inputs[2].asFloat());
    }
};

// ===========================================================================
// Construye el grafo del material de árbol.
// @param albedoNode salida Vec3 para el albedo, @param heightNode float para
// normal (from height) y AO.
// ===========================================================================
inline void buildTreeGraph(Graph& g, int seed, bool foliage,
                           int& albedoNode, int& heightNode) {
    int grain = g.emplaceNode<PerlinNode>(seed, foliage ? 9.0f : 5.0f);
    int norm  = g.emplaceNode<MapRangeNode>(-1.0f, 1.0f, 0.0f, 1.0f);
    g.connect(grain, 0, norm, 0);

    int src;
    if (foliage) {
        auto fbm = std::make_unique<FBMNode>(4, 2.0f, 0.5f, FBMNode::Mode::Sum, seed + 1);
        fbm->setNoiseFn(PerlinNode::perlin);
        int fbmI = g.addNode(std::move(fbm));
        int norm2 = g.emplaceNode<MapRangeNode>(-1.0f, 1.0f, 0.0f, 1.0f);
        g.connect(fbmI, 0, norm2, 0);
        int detail = g.emplaceNode<MathNode>(MathOp::Multiply);
        g.connect(norm, 0, detail, 0);
        g.connect(norm2, 0, detail, 1);
        src = detail;
    } else {
        int branchI = g.emplaceNode<BranchNode>(seed, 0.02f, 4);
        int knotI   = g.emplaceNode<KnotNode>(seed + 2, 12, 1.0f);
        int dark = g.emplaceNode<MathNode>(MathOp::Subtract);
        g.connect(norm, 0, dark, 0);
        g.connect(branchI, 0, dark, 1);
        int withKnots = g.emplaceNode<MathNode>(MathOp::Subtract);
        g.connect(dark, 0, withKnots, 0);
        g.connect(knotI, 0, withKnots, 1);
        src = withKnots;
    }

    auto makeRamp = [&](const std::vector<std::pair<float, float>>& stops) -> int {
        auto n = std::make_unique<ColorRampNode>();
        std::vector<ColorStop> cs;
        for (const auto& s : stops) cs.push_back({s.first, s.second});
        n->setStops(cs);
        return g.addNode(std::move(n));
    };
    std::vector<std::pair<float, float>> stopsR, stopsG, stopsB;
    if (foliage) {
        stopsR = {{0, 0.10f}, {0.45f, 0.22f}, {0.8f, 0.40f}, {1, 0.55f}};
        stopsG = {{0, 0.32f}, {0.45f, 0.50f}, {0.8f, 0.68f}, {1, 0.78f}};
        stopsB = {{0, 0.12f}, {0.45f, 0.22f}, {0.8f, 0.36f}, {1, 0.45f}};
    } else {
        stopsR = {{0, 0.14f}, {0.45f, 0.30f}, {0.8f, 0.50f}, {1, 0.68f}};
        stopsG = {{0, 0.09f}, {0.45f, 0.21f}, {0.8f, 0.37f}, {1, 0.52f}};
        stopsB = {{0, 0.08f}, {0.45f, 0.15f}, {0.8f, 0.26f}, {1, 0.38f}};
    }
    int rR = makeRamp(stopsR), rG = makeRamp(stopsG), rB = makeRamp(stopsB);
    g.connect(src, 0, rR, 0);
    g.connect(src, 0, rG, 0);
    g.connect(src, 0, rB, 0);

    int combine = g.emplaceNode<TreeCombineRGBNode>();
    g.connect(rR, 0, combine, 0);
    g.connect(rG, 0, combine, 1);
    g.connect(rB, 0, combine, 2);

    albedoNode = combine;
    heightNode = src;
}

// ===========================================================================
// Resultado del bake: rutas RELATIVAS para meter en MaterialComponent.textures.
// ===========================================================================
struct TreeBakeResult {
    bool ok = false;
    std::string albedo, normal, roughness, metallic, ao;
};

// ===========================================================================
// Hornea las texturas del árbol en `outDir` (absoluta) con nombres basados en
// `baseName`. Devuelve rutas relativas `relDir/<base>_<slot>.png` (por defecto
// "assets/textures/", para la raíz de texturas; el llamador la cambia cuando
// hornea a una subcarpeta dedicada de props).
// ===========================================================================
inline TreeBakeResult bakeTreeTextures(int seed, bool foliage,
                                       const std::string& outDir,
                                       const std::string& baseName,
                                       int size = 256,
                                       const std::string& relDir = "assets/textures/") {
    TreeBakeResult r;
    std::error_code ec;
    std::filesystem::create_directories(outDir, ec);

    Graph g;
    int albedo = -1, height = -1;
    buildTreeGraph(g, seed, foliage, albedo, height);
    if (!g.compile()) return r;

    auto write = [&](const char* suffix, const RGBAImage& img) -> bool {
        std::string path = outDir + "/" + baseName + "_" + suffix + ".png";
        return Haruka::writePNG(path, img.width, img.height, 4, img.data());
    };
    auto rel = [&](const char* suffix) {
        return relDir + baseName + "_" + suffix + ".png";
    };

    // Albedo: alpha forzado a 255 (el grafo da Vec3, data[3]=0).
    int cmap[4] = {0, 1, 2, -1};
    RGBAImage alb = evaluateToRGBA(g, albedo, 0, size, size, 0, 0, 1, cmap);
    if (!write("albedo", alb)) return r;
    r.albedo = rel("albedo");

    RGBAImage nrm = evaluateToNormalMap(g, height, 0, size, size, 0, 0, 1, 1.0f);
    if (!write("normal", nrm)) return r;
    r.normal = rel("normal");

    // AO desde la altura: hendiduras (ramas/nudos) oscuras, anime suave.
    RGBAImage ao(size, size);
    float dark = foliage ? 0.45f : 0.60f;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            float h = g.evaluate(height, 0, (float)x, (float)y, 0).asFloat();
            uint8_t v = (uint8_t)(glm::clamp(1.0f - h * dark, 0.0f, 1.0f) * 255.0f);
            ao.setPixel(x, y, v, v, v);
        }
    }
    if (!write("ao", ao)) return r;
    r.ao = rel("ao");

    // Roughness / metallic constantes (corteza áspera, copa más satinada).
    float rough = foliage ? 0.55f : 0.85f;
    RGBAImage rm(size, size);
    uint8_t rv = (uint8_t)(rough * 255.0f);
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) rm.setPixel(x, y, rv, rv, rv);
    if (!write("roughness", rm)) return r;
    r.roughness = rel("roughness");

    r.ok = true;
    return r;
}

// ===========================================================================
// Disco del Sol (estilo anime): núcleo blanco-amarillo -> naranja -> borde rojo
// con oscurecimiento de limbo. Determinsta; se usa como albedo del material EMISSIVE
// del sol (el shader multiplica baseColor×textura y suma la emisión).
// ===========================================================================
inline bool bakeSunTexture(const std::string& outDir, const std::string& baseName,
                           int size = 256) {
    std::error_code ec;
    std::filesystem::create_directories(outDir, ec);

    auto smooth = [](float t) { t = glm::clamp(t, 0.0f, 1.0f); return t * t * (3.0f - 2.0f * t); };
    auto clamp01 = [](float v) { return glm::clamp(v, 0.0f, 1.0f); };

    RGBAImage img(size, size);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            float u = (float(x) + 0.5f) / (float)size;
            float v = (float(y) + 0.5f) / (float)size;
            float dx = u - 0.5f, dy = v - 0.5f;
            float r = std::min(std::sqrt(dx * dx + dy * dy) * 2.0f, 1.5f);

            float core = std::exp(-r * r * 2.6f);
            float mid  = std::exp(-((r - 0.55f) * (r - 0.55f)) * 6.0f);
            float rim  = std::exp(-((r - 0.9f)  * (r - 0.9f))  * 16.0f);
            float R = core * 1.0f + mid * 0.95f + rim * 0.95f;
            float G = core * 0.99f + mid * 0.80f + rim * 0.45f;
            float B = core * 0.94f + mid * 0.50f + rim * 0.16f;
            img.setPixel(x, y, (uint8_t)(clamp01(R) * 255.0f),
                               (uint8_t)(clamp01(G) * 255.0f),
                               (uint8_t)(clamp01(B) * 255.0f), 255);
        }
    }

    std::string path = outDir + "/" + baseName + ".png";
    return Haruka::writePNG(path, size, size, 4, img.data());
}

}}} // namespace Haruka::Tools::ProcGraph
