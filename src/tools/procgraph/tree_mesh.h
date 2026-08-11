#pragma once

// ===========================================================================
// Malla de árbol generada por el node material graph.
//
// `TreeMeshNode` es el primer nodo de GEOMETRÍA del ProcGraph: a partir de una
// seed determinista emite el árbol final (tronco con taper + ramas + copa de
// blobs achatados), sin tocar GPU. `bakeTreeMesh` extrae el resultado como
// vectores CPU (posiciones/normales/colores/indices) listos para subir a un
// MeshRendererComponent o un Renderer::Mesh.
//
// Determinista: misma seed + parámetros ⇒ misma malla exacta (paridad con el
// resto del grafo). Las normales apuntan hacia fuera (flip por hint radial en
// el build), la base del tronco queda en y=0 en el origen.
// ===========================================================================

#include "proc_graph.h"
#include "proc_noise.h"

#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace Haruka { namespace Tools { namespace ProcGraph {

// ===========================================================================
// Datos de la malla generada (CPU, sin GPU).
// ===========================================================================
struct TreeMeshData {
    std::vector<glm::vec3>       positions;
    std::vector<glm::vec3>       normals;
    std::vector<glm::vec3>       colors;
    std::vector<glm::vec2>       uvs;
    std::vector<unsigned int>    indices;
    std::vector<unsigned char>   materialId;   // 0 = corteza, 1 = follaje
    /// PARTE del esqueleto a la que pertenece cada vértice (índice en `TreeSkeleton::parts`).
    /// Es lo que permite romper UNA rama sin rehornear la malla: el prototipo se comparte entre
    /// todas las instancias y el shader degenera los vértices cuyo bit esté puesto en la máscara
    /// de roturas de ESA instancia. La copa lleva la parte del TRONCO (cuelga de él): tumbar el
    /// fuste se lleva el follaje, romper una rama no.
    std::vector<unsigned char>   partId;

    void clear() {
        positions.clear(); normals.clear(); colors.clear();
        uvs.clear(); indices.clear(); materialId.clear(); partId.clear();
    }
    size_t vertexCount()   const { return positions.size(); }
    size_t triangleCount() const { return indices.size() / 3; }
};

// ===========================================================================
// ESQUELETO del árbol: los segmentos ESTRUCTURALES (tronco + ramas) de los que
// se deriva TODO — la geometría que se dibuja, el collider que se pisa y las
// partes que se pueden romper a hachazos.
//
// ⚠️ Existe porque el tronco y las ramas se calculaban EN LÍNEA dentro de
// `build()` y se los comía `appendCone`: la forma quedaba encerrada en el
// emisor de triángulos y nadie más podía preguntarla. Un collider derivado
// aparte sería una SEGUNDA implementación de la misma forma, y dos
// implementaciones de la misma cosa acaban divergiendo — que es exactamente
// como se dibuja un árbol en un sitio y se choca con él en otro.
//
// ⚠️ NO DEPENDE DE `detail`. El LOD cambia cuántos triángulos tiene un
// segmento, no dónde está: si el esqueleto siguiera al detalle, el árbol
// tendría un collider distinto según lo lejos que estuviera la cámara. Por eso
// `build()` recibe el esqueleto completo y decide QUÉ emitir (a detalle < 0.5
// no dibuja ramas), pero la colisión siempre ve el árbol entero.
// ===========================================================================
enum class TreePartKind : uint8_t {
    Trunk  = 0,   ///< el fuste: romperlo tumba el árbol entero
    Branch = 1,   ///< una rama: se rompe sola, y si es larga deja un palo
};

/** @brief Un segmento estructural: cono truncado de `a` a `b` con radio en cada extremo.
 *  Coordenadas LOCALES del árbol (base del tronco en el origen, +Y hacia arriba), sin escalar:
 *  quien lo use aplica la escala/orientación de la instancia. */
struct TreePart {
    TreePartKind kind    = TreePartKind::Trunk;
    glm::vec3    a       = glm::vec3(0.0f);   ///< extremo de nacimiento (el grueso)
    glm::vec3    b       = glm::vec3(0.0f);   ///< extremo libre (el fino)
    float        radiusA = 0.0f;
    float        radiusB = 0.0f;

    float length() const { return glm::length(b - a); }
    /** @brief Radio representativo para un collider de una sola pieza (media de los extremos). */
    float radiusMid() const { return 0.5f * (radiusA + radiusB); }
};

/** @brief Esqueleto completo. El índice en `parts` ES la identidad de la parte (`partId`):
 *  0 = tronco, 1..n = ramas. Ese índice viaja al vértice (para poder degenerarlo al romperse)
 *  y a la máscara de roturas por instancia, así que el ORDEN es contrato: no reordenar. */
struct TreeSkeleton {
    std::vector<TreePart> parts;
    float height = 0.0f;   ///< altura nominal del árbol (m), la del parámetro
    float trunkH = 0.0f;   ///< altura del fuste (m) — la copa arranca arriba de esto
    float trunkR = 0.0f;   ///< radio del tronco en la base (m)

    size_t partCount() const { return parts.size(); }
};

/**
 * @brief Deriva el esqueleto determinista de un árbol. Función PURA de (seed, height, trunkR).
 *
 * Es la MISMA aritmética que emitía `build()` antes de extraerla — mismos hashes, mismos índices
 * de canal (0,1 para la inclinación; 3..6 por rama), mismas constantes. Cambiar cualquiera de
 * ellas mueve árboles ya generados, así que se tocan a sabiendas.
 */
inline TreeSkeleton treeSkeleton(int seed, float height, float trunkR) {
    TreeSkeleton sk;
    sk.height = height;
    sk.trunkR = trunkR;

    const float kPi = 3.14159265358979323846f;

    // Inclinación determinista del fuste (la misma que da el "bend" al dibujarlo).
    const float bendX = (WhiteNode::hashFloat(0, 0, 0, (uint32_t)seed) - 0.5f) * 0.12f;
    const float bendZ = (WhiteNode::hashFloat(0, 1, 0, (uint32_t)seed) - 0.5f) * 0.12f;
    const float trunkH = height * 0.62f;
    sk.trunkH = trunkH;

    TreePart trunk;
    trunk.kind    = TreePartKind::Trunk;
    trunk.a       = glm::vec3(0.0f);
    trunk.b       = glm::vec3(bendX * trunkH, trunkH, bendZ * trunkH);
    trunk.radiusA = trunkR;
    trunk.radiusB = trunkR * 0.25f;
    sk.parts.push_back(trunk);

    // Ramas: 2-3, siempre TODAS en el esqueleto (el detalle decide si se dibujan, no si existen).
    const int nBranches = 2 + (int)(WhiteNode::hashFloat(0, 2, 0, (uint32_t)seed) * 2.0f);
    const float branchBaseY = trunkH * 0.55f;
    const glm::vec3 bBase(bendX * branchBaseY, branchBaseY, bendZ * branchBaseY);
    for (int b = 0; b < nBranches; ++b) {
        const float ang  = 2.0f * kPi * WhiteNode::hashFloat(b, 3, 0, (uint32_t)seed);
        const float lean = 0.35f + WhiteNode::hashFloat(b, 4, 0, (uint32_t)seed) * 0.35f;
        const float up   = 0.30f + WhiteNode::hashFloat(b, 5, 0, (uint32_t)seed) * 0.30f;
        const float lenB = height * (0.22f + WhiteNode::hashFloat(b, 6, 0, (uint32_t)seed) * 0.12f);
        const glm::vec3 dir = glm::normalize(glm::vec3(std::cos(ang) * lean, up, std::sin(ang) * lean));

        TreePart br;
        br.kind    = TreePartKind::Branch;
        br.a       = bBase;
        br.b       = bBase + dir * lenB;
        br.radiusA = trunkR * 0.30f;
        br.radiusB = trunkR * 0.05f;
        sk.parts.push_back(br);
    }
    return sk;
}

// ===========================================================================
// Nodo de geometría "Tree Mesh".
//
// Entradas: Seed, Height (m), Trunk Radius (m), Canopy Size (escala), Segments.
// Salidas:  VertexCount (int), MeshReady (bool).
// evaluate() construye la malla una sola vez por parámetros y la cachea.
// ===========================================================================
class TreeMeshNode : public Node {
public:
    /**
     * @param detail NIVEL DE DETALLE en [0,1]. 1 = malla completa (idéntica a la de siempre).
     *
     * Existe para el LOD de los props, y hacía falta de verdad: medido con RenderDoc, los árboles del
     * scatter son el **85,8 % de los triángulos del frame** (6320 instancias × 512 triángulos = 3,24 M),
     * y el instancing NO lo arregla — instanciar ahorra draw calls, no trabajo de vértices: la GPU
     * ejecuta el vertex shader de los 512 triángulos por cada instancia, vaya en un comando o en 6320.
     *
     * Y el mando que ya existía (`segments`) no bastaba: solo baja de 464 a 210 triángulos (2,2×)
     * porque la COPA —6 blobs con `lat` fijo a 4— es el 62 % de la malla y `segments` no la toca.
     * `detail` escala las tres cosas que de verdad cuentan: nº de blobs, sus paralelos y sus meridianos.
     *
     * ⚠️ La ALTURA Y EL RADIO no cambian con el detalle. Es la regla que hace usable un LOD: si la
     * envolvente cambiara, el árbol daría un salto de tamaño al conmutar de nivel — el artefacto clásico.
     * Lo fija un test.
     */
    TreeMeshNode(int seed = 0, float height = 6.5f, float trunkRadius = 0.35f,
                 float canopySize = 1.0f, int segments = 8, float detail = 1.0f)
        : m_seed(seed), m_height(height), m_trunkRadius(trunkRadius),
          m_canopySize(canopySize), m_segments(segments), m_detail(detail) {}

    std::string name() const override { return "Tree Mesh"; }

    std::vector<SocketDesc> inputs() const override {
        return {
            {"Seed",         DataType::Int,   Value::Int(m_seed)},
            {"Height",       DataType::Float, Value::Float(m_height)},
            {"Trunk Radius", DataType::Float, Value::Float(m_trunkRadius)},
            {"Canopy Size",  DataType::Float, Value::Float(m_canopySize)},
            {"Segments",     DataType::Int,   Value::Int(m_segments)},
        };
    }
    std::vector<SocketDesc> outputs() const override {
        return {{"VertexCount", DataType::Int}, {"MeshReady", DataType::Bool}};
    }

    void evaluate(const Value* inputs, int, Value* outputs, int,
                  float, float, float) override {
        int    seed   = m_seed + inputs[0].asInt();
        float  height = inputs[1].asFloat();
        float  trunkR = inputs[2].asFloat();
        float  canopy = inputs[3].asFloat();
        int    segs   = std::max(3, inputs[4].asInt());

        if (!m_built || seed   != m_lastSeed   || height != m_lastHeight ||
            trunkR   != m_lastTrunkR || canopy != m_lastCanopy || segs != m_lastSegs ||
            m_detail != m_lastDetail) {
            build(seed, height, trunkR, canopy, segs);
            m_lastSeed = seed; m_lastHeight = height; m_lastTrunkR = trunkR;
            m_lastCanopy = canopy; m_lastSegs = segs; m_lastDetail = m_detail;
            m_built = true;
        }

        outputs[0] = Value::Int((int)m_mesh.vertexCount());
        outputs[1] = Value::Bool(true);
    }

    const TreeMeshData& mesh() const { return m_mesh; }

private:
    void build(int seed, float height, float trunkR, float canopy, int segs);

    TreeMeshData m_mesh;
    bool m_built = false;

    int   m_seed;
    float m_height, m_trunkRadius, m_canopySize;
    int   m_segments;
    float m_detail = 1.0f;

    int   m_lastSeed   = 0x7fffffff;
    float m_lastHeight = -1, m_lastTrunkR = -1, m_lastCanopy = -1;
    int   m_lastSegs   = -1;
    float m_lastDetail = -1.0f;
};

// ===========================================================================
// Implementación del build: geometría determinista.
// ===========================================================================
inline void TreeMeshNode::build(int seed, float height, float trunkR,
                                float canopy, int segs) {
    const float detail = std::clamp(m_detail, 0.05f, 1.0f);
    // ⚠️ `segs` lo usaban tronco, ramas Y copa, pero solo la copa se escalaba: el tronco seguía con 8
    // lados a cualquier detalle y la malla se estancaba en 188 triángulos (de 464). Un solo valor
    // derivado que usan los tres.
    const int segsLod = std::max(3, (int)std::lround((float)segs * detail));
    TreeMeshData& m = m_mesh;
    m.clear();

    struct Ctx {
        TreeMeshData& m;
        std::vector<glm::vec3> hints; // referencia radial para orientar normales
        /// Parte del esqueleto que se está emitiendo ahora. Va como estado del emisor y no como
        /// parámetro de `addV` porque los tres emisores (cono/esfera/quad) la heredan sin saberlo.
        unsigned char part = 0;

        size_t addV(const glm::vec3& p, const glm::vec3& hint,
                    const glm::vec2& uv, unsigned char mat, const glm::vec3& color) {
            m.positions.push_back(p);
            m.normals.push_back(glm::vec3(0.0f));
            m.uvs.push_back(uv);
            m.materialId.push_back(mat);
            m.colors.push_back(color);
            m.partId.push_back(part);
            hints.push_back(hint);
            return m.positions.size() - 1;
        }

        void addTri(size_t a, size_t b, size_t c) {
            const glm::vec3& va = m.positions[a];
            const glm::vec3& vb = m.positions[b];
            const glm::vec3& vc = m.positions[c];
            glm::vec3 n = glm::cross(vb - va, vc - va);
            glm::vec3 hintSum = hints[a] + hints[b] + hints[c];
            glm::vec3 hint = glm::length(hintSum) > 1e-6f ? glm::normalize(hintSum)
                                                         : glm::vec3(0, 1, 0);
            if (glm::dot(n, hint) < 0.0f) {
                std::swap(b, c);
                n = -n;
            }
            m.indices.push_back((unsigned)a);
            m.indices.push_back((unsigned)b);
            m.indices.push_back((unsigned)c);
            m.normals[a] += n;
            m.normals[b] += n;
            m.normals[c] += n;
        }

        void addQuad(size_t a, size_t b, size_t c, size_t d) {
            addTri(a, b, c);
            addTri(a, c, d);
        }
    } ctx{m};

    const float kPi = 3.14159265358979323846f;

    // ---- colores anime por material ---------------------------------------
    const glm::vec3 barkCol   (0.42f, 0.30f, 0.20f);
    const glm::vec3 foliageCol(0.24f, 0.42f, 0.17f);

    auto appendCone = [&](const glm::vec3& base, const glm::vec3& top,
                          float baseR, float topR, int rings,
                          unsigned char mat, const glm::vec3& color) {
        glm::vec3 d = top - base;
        float len = glm::length(d);
        if (len < 1e-6f) return;
        glm::vec3 axis = d / len;
        glm::vec3 ref = (std::fabs(axis.y) < 0.99f) ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
        glm::vec3 u = glm::normalize(glm::cross(ref, axis));
        glm::vec3 v = glm::cross(axis, u);

        std::vector<std::vector<size_t>> ring(rings + 1);
        for (int i = 0; i <= rings; ++i) {
            float t = (float)i / (float)rings;
            glm::vec3 c = base + d * t;
            float r = baseR + (topR - baseR) * t;
            for (int j = 0; j < segsLod; ++j) {
                float ang = 2.0f * kPi * (float)j / (float)segsLod;
                float co = std::cos(ang), sn = std::sin(ang);
                glm::vec3 p = c + (u * co + v * sn) * r;
                glm::vec3 hint = glm::normalize(p - c);
                glm::vec2 uv((float)j / (float)segsLod, t);
                ring[i].push_back(ctx.addV(p, hint, uv, mat, color));
            }
        }
        for (int i = 0; i < rings; ++i) {
            for (int j = 0; j < segsLod; ++j) {
                int j2 = (j + 1) % segsLod;
                ctx.addQuad(ring[i][j], ring[i][j2], ring[i + 1][j2], ring[i + 1][j]);
            }
        }
    };

    // ---- tronco y ramas: LOS EMITE EL ESQUELETO -----------------------------
    // La forma ya no se calcula aquí. `treeSkeleton` es la única fuente, y este bucle solo decide
    // CUÁNTOS anillos gasta en cada segmento (eso sí es LOD) y qué partes se dibujan.
    const TreeSkeleton sk = treeSkeleton(seed, height, trunkR);
    const float trunkH = sk.trunkH;
    for (size_t pi = 0; pi < sk.parts.size(); ++pi) {
        const TreePart& p = sk.parts[pi];
        // Las ramas son detalle de cerca: a 10 px no se distinguen de la copa. Se van primero.
        // (El esqueleto las conserva igualmente — la colisión no depende del detalle.)
        if (p.kind == TreePartKind::Branch && detail < 0.5f) continue;
        const int rings = (p.kind == TreePartKind::Trunk)
                        ? std::max(1, (int)std::lround(5.0f * detail))
                        : 2;
        ctx.part = (unsigned char)pi;
        appendCone(p.a, p.b, p.radiusA, p.radiusB, rings, 0, barkCol);
    }
    ctx.part = 0;   // la copa cuelga del TRONCO: tumbarlo se lleva el follaje

    // ---- copa: cluster de blobs achatados (estilo anime) -------------------
    auto appendSphere = [&](const glm::vec3& center, float radius,
                            float squashY, unsigned char mat,
                            const glm::vec3& color) {
        // ⚠️ `lat` estaba FIJO a 4 y `lon` solo miraba `segs`. Con la copa siendo el 62 % de la malla,
        // ese `4` es la razón de que `segments` no sirviera como LOD. Ahora los dos siguen al detalle.
        // Mínimos: lat=2 da una bipirámide (dos abanicos), que sigue siendo un sólido cerrado y a 10 px
        // es indistinguible de una esfera; lon=3 es el mínimo con volumen.
        int lat = std::max(2, (int)std::lround(4.0f * detail));
        int lon = std::max(3, segsLod);
        size_t topV = ctx.addV(center + glm::vec3(0, radius * squashY, 0),
                               glm::vec3(0, 1, 0), glm::vec2(0.5f, 1.0f), mat, color);
        std::vector<std::vector<size_t>> rings(lat - 1);
        for (int i = 1; i < lat; ++i) {
            float theta = kPi * (float)i / (float)lat;
            float r = radius * std::sin(theta);
            float y = radius * std::cos(theta) * squashY;
            for (int j = 0; j < lon; ++j) {
                float phi = 2.0f * kPi * (float)j / (float)lon;
                glm::vec3 p(center.x + std::cos(phi) * r,
                            center.y + y,
                            center.z + std::sin(phi) * r);
                glm::vec3 hint = glm::normalize(p - center);
                glm::vec2 uv((float)j / (float)lon, (float)i / (float)lat);
                rings[i - 1].push_back(ctx.addV(p, hint, uv, mat, color));
            }
        }
        size_t botV = ctx.addV(center - glm::vec3(0, radius * squashY, 0),
                               glm::vec3(0, -1, 0), glm::vec2(0.5f, 0.0f), mat, color);
        for (int j = 0; j < lon; ++j) {
            int j2 = (j + 1) % lon;
            ctx.addTri(topV, rings[0][j2], rings[0][j]);
            for (int i = 1; i < lat - 1; ++i)
                ctx.addQuad(rings[i - 1][j], rings[i - 1][j2],
                            rings[i][j2], rings[i][j]);
            ctx.addTri(botV, rings[lat - 2][j], rings[lat - 2][j2]);
        }
    };

    // ---- copa: cluster de blobs, con LOD que CONSERVA LA ENVOLVENTE ---------
    //
    // ⚠️ Aquí está lo que hace usable un LOD, y costó dos intentos medirlo bien.
    //
    // Quitar blobs sin más adelgazaba la copa un 33 % de ancho, así que el árbol daba un SALTO DE
    // TAMAÑO al conmutar de nivel — el artefacto clásico. Y no basta con agrandar los que quedan: dos
    // blobs en ángulos aleatorios dejan una silueta estrecha por mucho que crezcan, porque la extensión
    // es DIRECCIONAL. Así que al reducir se hacen dos cosas:
    //
    //   1. Los ángulos pasan a ser REGULARES (2π·b/n) en vez de los aleatorios del nivel completo: con
    //      pocos blobs, repartirlos es lo único que da una silueta simétrica.
    //   2. El radio de cada blob y la altura del cúmulo se escalan para reproducir la EXTENSIÓN
    //      HORIZONTAL y la COTA SUPERIOR del nivel 1. Se calculan las dos con todos los blobs en una
    //      pasada previa sobre los mismos hashes, así que es determinista y no hace falta guardar nada.
    //
    // A nivel 1 no se toca nada: mismos ángulos aleatorios, mismos radios. Un test comprueba que la
    // malla de detalle 1.0 sigue siendo la de siempre y que las cajas de todos los niveles coinciden.
    const int nBlobsFull = 5 + (int)(WhiteNode::hashFloat(0, 7, 0, seed) * 3.0f);   // 5..7
    int nBlobs = std::max(1, (int)std::lround((float)nBlobsFull * detail));
    // ⚠️ Con DOS blobs la copa sale como una PESA: dos esferas en ángulos opuestos dan una caja de
    // 3,77 de ancho por 1,86 de fondo — anisótropa por construcción, y ninguna normalización isótropa
    // puede arreglar eso. Medido. Con dos o menos se usa UNO CENTRADO, que es isótropo de partida y a
    // la distancia donde se usa este nivel (>700 m, ~15 px) se lee igual que un cúmulo.
    if (nBlobs <= 2) nBlobs = 1;
    const bool reduced = (nBlobs < nBlobsFull);

    const float canopyY = trunkH * 0.95f;
    const float canopyR = trunkR * (3.2f + 1.6f * canopy);
    const float squash  = 0.82f;

    auto blobRadFull = [&](int b) { return canopyR * (0.35f + WhiteNode::hashFloat(b, 9, 0, seed) * 0.45f); };
    auto blobYOff    = [&](int b) { return (WhiteNode::hashFloat(b, 10, 0, seed) - 0.5f) * canopyR * 0.5f; };
    auto blobBR      = [&](int b) { return trunkR * (1.4f + WhiteNode::hashFloat(b, 11, 0, seed) * 0.9f) * canopy; };

    // Envolvente objetivo: la del nivel completo. Se mide como CAJA ALINEADA A LOS EJES, no como
    // extensión radial — y la diferencia importa: normalizando el radio (`rad + br` en la dirección de
    // cada blob) el ancho salía un 15 % GRANDE, porque con ángulos regulares las esquinas de la caja se
    // alinean con los ejes mientras que con los ángulos aleatorios del nivel 1 no. La caja es lo que se
    // ve, así que es lo que hay que igualar.
    float fullTop = -1e9f;
    float fx0 = 1e9f, fx1 = -1e9f, fz0 = 1e9f, fz1 = -1e9f;
    for (int b = 0; b < nBlobsFull; ++b) {
        const float ang = 2.0f * kPi * WhiteNode::hashFloat(b, 8, 0, seed);
        const float rad = blobRadFull(b), br = blobBR(b);
        const float cx = std::cos(ang) * rad, cz = std::sin(ang) * rad;
        fx0 = std::min(fx0, cx - br); fx1 = std::max(fx1, cx + br);
        fz0 = std::min(fz0, cz - br); fz1 = std::max(fz1, cz + br);
        fullTop = std::max(fullTop, canopyY + blobYOff(b) + br * squash);
    }
    const float fullBoxH = 0.5f * ((fx1 - fx0) + (fz1 - fz0));   // media de ancho y profundidad

    // Radios y posiciones del nivel pedido.
    std::vector<glm::vec3> bc(nBlobs);
    std::vector<float>     brv(nBlobs);
    for (int b = 0; b < nBlobs; ++b) {
        // Ángulos ALEATORIOS en todos los niveles: con 3 o más blobs ya reparten bien, y usar ángulos
        // regulares al reducir hacía que la copa cambiara de FORMA (no solo de densidad) al conmutar.
        const float ang = 2.0f * kPi * WhiteNode::hashFloat(b, 8, 0, seed);
        const float rad = (nBlobs == 1) ? 0.0f : blobRadFull(b);
        brv[b] = blobBR(b);
        bc[b]  = glm::vec3(std::cos(ang) * rad, canopyY + blobYOff(b), std::sin(ang) * rad);
    }
    if (reduced) {
        // Igualar la caja horizontal. Escalar radios y posiciones cambia la propia caja, así que se
        // itera: dos pasadas bastan (la corrección es multiplicativa y converge de inmediato).
        for (int it = 0; it < 2; ++it) {
            float x0 = 1e9f, x1 = -1e9f, z0 = 1e9f, z1 = -1e9f;
            for (int b = 0; b < nBlobs; ++b) {
                x0 = std::min(x0, bc[b].x - brv[b]); x1 = std::max(x1, bc[b].x + brv[b]);
                z0 = std::min(z0, bc[b].z - brv[b]); z1 = std::max(z1, bc[b].z + brv[b]);
            }
            const float redBoxH = 0.5f * ((x1 - x0) + (z1 - z0));
            if (redBoxH <= 1e-6f) break;
            const float k = fullBoxH / redBoxH;
            for (int b = 0; b < nBlobs; ++b) { brv[b] *= k; bc[b].x *= k; bc[b].z *= k; }
        }
        // …y desplazar el cúmulo en Y para igualar la cota superior (cambiar el radio la movió).
        float redTop = -1e9f;
        for (int b = 0; b < nBlobs; ++b) redTop = std::max(redTop, bc[b].y + brv[b] * squash);
        const float dy = fullTop - redTop;
        for (int b = 0; b < nBlobs; ++b) bc[b].y += dy;
    }
    for (int b = 0; b < nBlobs; ++b) appendSphere(bc[b], brv[b], squash, 1, foliageCol);

    // ---- normalización final de normales ------------------------------------
    for (size_t i = 0; i < m.normals.size(); ++i) {
        float l = glm::length(m.normals[i]);
        if (l > 1e-6f) m.normals[i] /= l;
        else m.normals[i] = glm::normalize(ctx.hints[i]);
    }
}

// ===========================================================================
// Extrae la malla del nodo tras forzar su evaluación.
// ===========================================================================
inline bool bakeTreeMesh(Graph& g, int nodeIndex, TreeMeshData& out) {
    g.evaluate(nodeIndex, 0, 0.0f, 0.0f, 0.0f);
    TreeMeshNode* n = dynamic_cast<TreeMeshNode*>(g.node(nodeIndex));
    if (!n) return false;
    out = n->mesh();
    return !out.positions.empty();
}

}}} // namespace Haruka::Tools::ProcGraph
