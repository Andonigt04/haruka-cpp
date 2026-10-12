#include "Haruka/Renderer/VoxRenderer.h"

#include "Haruka/Core/Jobs/JobSystem.h"
#include "Haruka/Core/Logger.h"
#include "Haruka/IO/ImageWriter.h"
#include "Haruka/Renderer/Shader.h"
#include "Haruka/RHI/RHIContext.h"
#include "Haruka/RHI/RHIDevice.h"
#include "Haruka/Tools/Profiler.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <tuple>
#include <unordered_set>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace Haruka::Renderer {

VoxRenderer::~VoxRenderer() {
    if (m_fineCutPending) m_fineCutJob.wait();   // reads the world
    m_fine.finishJobs();   // the jobs read the world: done before anything goes
    for (auto& kv : m_gpuBricks) if (kv.second.id) Renderer::resources().release(kv.second.id);
    for (auto& kv : m_push) if (kv.second.id) Renderer::resources().release(kv.second.id);
}

bool VoxRenderer::ensurePipeline() {
    if (m_failed) return false;
    if (RHI::valid(m_pipe)) return true;
    RHI::Device* dev = RHI::device();
    if (!dev) return false;
    const std::string vs = Shader::baseDir() + "shaders/vox.vert";
    const std::string fs = Shader::baseDir() + "shaders/vox.frag";
    RHI::PipelineDesc pd;
    pd.vertexPath = vs.c_str(); pd.fragmentPath = fs.c_str();
    pd.topology = RHI::PrimitiveTopology::Triangles;
    pd.vertexLayout.strides = { 6 * sizeof(float) };
    pd.vertexLayout.attributes = { { 0, 0, RHI::Format::RGB32F, 0 }, { 1, 3 * sizeof(float), RHI::Format::RGB32F, 0 } };
    pd.depth.test = true; pd.depth.write = true;      // Greater por defecto: reversed-Z del motor
    // `HARUKA_VOX_DRAW=2`: sin test de profundidad (A/B de "no se ve": ¿lo tapa algo o no se dibuja?)
    { const char* e = std::getenv("HARUKA_VOX_DRAW"); if (e && e[0] == '2') { pd.depth.test = false; pd.depth.write = false; } }
    pd.cull = RHI::CullMode::None;                    // se ve desde DENTRO y desde fuera
    m_pipe = dev->createPipeline(pd);
    if (!RHI::valid(m_pipe)) {
        m_failed = true;
        HARUKA_LOGE("Vox", "el pipeline de paredes NO linkó → las cuevas no se dibujan");
        return false;
    }
    VoxUBO init{};
    m_ubo = dev->createBuffer(RHI::BufferUsage::Uniform, sizeof(VoxUBO), &init, RHI::BufferMemory::Dynamic);
    return true;
}

void VoxRenderer::update(World::Vox::VoxWorld& world, const glm::dvec3& camRelPlanet, double surfaceElevM) {
    if (!ensurePipeline()) return;
    RHI::Device* dev = RHI::device();
    if (RHI::valid(m_cutTexOld)) { dev->destroy(m_cutTexOld); m_cutTexOld = {}; }

    // ── Mallas: los sucios, acotado por frame ──────────────────────────────────────────────────
    int remeshed = 0;
    std::vector<World::Vox::VoxKey> keys = world.loadedKeys();
    const auto tRemesh0 = std::chrono::high_resolution_clock::now();
    { HARUKA_PROFILE("vox.mesh");
    for (const World::Vox::VoxKey& k : keys) {
        World::Vox::VoxChunk* c = world.get(k);
        if (!c || !c->dirty) continue;
        // Presupuesto por TIEMPO, no por número: un chunk todo roca sale en microsegundos y uno
        // con isla en ~2 ms; con "3 por frame" una isla de 20 chunks tardaba 7 s en aparecer (y el
        // jugador que caía sobre ella la atravesaba antes de que existiera).
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - tRemesh0).count();
        if (remeshed >= kMaxRemeshPerFrame || (remeshed > 0 && ms > kRemeshBudgetMs)) break;
        // La malla se construye DIRECTAMENTE en el objeto que se publica: una vez publicado no se toca
        // (lo leen la subida y la física), así que todo lo de abajo solo lo lee.
        auto shared = std::make_shared<World::Vox::CaveMesh>();
        World::Vox::CaveMesh& mesh = *shared;
        const auto tB0 = std::chrono::high_resolution_clock::now();
        world.buildMesh(k, mesh);
        const double buildMs = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - tB0).count();
        ++remeshed;
        // La ventana de recorte SOLO se rehace si el chunk remallado cae dentro (± margen): un
        // chunk lejano no pinta bocas en ella y no merece rebarridar los kCutRes².
        if (glm::dot(camRelPlanet, camRelPlanet) > 1.0) {
            const glm::dvec3 cc = world.chunkCenterDir(k);
            const double ang = std::acos(std::clamp(glm::dot(camRelPlanet / glm::length(camRelPlanet), cc), -1.0, 1.0));
            if (ang * world.planetRadius() < kCutHalfM * 1.7) m_cutDirty = true;
        }
        // PUBLICAR, no subir: el almacén central la sube con el presupuesto común del frame y destruye
        // la versión vieja cuando ningún frame en vuelo la usa (antes se destruía aquí, en el acto).
        // La copia de CPU se queda (`keepCpu`): la física la lee por revisión.
        Renderer::ResourceId& id = m_ids[k];
        if (!id) id = Renderer::resources().create("vox", true);
        Renderer::resources().publish<World::Vox::CaveMesh>(id, shared, &VoxRenderer::uploadMesh);
        if (mesh.empty()) continue;   // todo roca o todo aire: nada que dibujar, y no cuesta
        HARUKA_LOGDIAG("Vox", "malla chunk (%d,%d,%d,%d): %zu vertices · %zu triangulos · %.2f ms",
                    k.face, k.i, k.j, k.k, mesh.positions.size(), mesh.triangleCount(), buildMs);
        // `HARUKA_VOX_DUMP=<carpeta>`: cada malla a un OBJ (posiciones relativas al origen del
        // chunk, con normales) para mirarla fuera del juego cuando lo que se ve no cuadra con lo
        // que dicen los tests.
        static const char* s_dump = std::getenv("HARUKA_VOX_DUMP");
        if (s_dump && s_dump[0]) {
            char name[256];
            std::snprintf(name, sizeof(name), "%s/vox_%d_%d_%d_%d.obj", s_dump, k.face, k.i, k.j, k.k);
            if (std::FILE* f = std::fopen(name, "w")) {
                std::fprintf(f, "# origen %.3f %.3f %.3f\n", mesh.origin.x, mesh.origin.y, mesh.origin.z);
                for (const glm::vec3& p : mesh.positions) std::fprintf(f, "v %.4f %.4f %.4f\n", p.x, p.y, p.z);
                for (const glm::vec3& n : mesh.normals) std::fprintf(f, "vn %.4f %.4f %.4f\n", n.x, n.y, n.z);
                for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3)
                    std::fprintf(f, "f %u//%u %u//%u %u//%u\n", mesh.indices[i] + 1, mesh.indices[i] + 1,
                                 mesh.indices[i + 1] + 1, mesh.indices[i + 1] + 1, mesh.indices[i + 2] + 1, mesh.indices[i + 2] + 1);
                std::fclose(f);
            }
        }
    } }
    // Lo descargado se suelta (el almacén espera a los frames en vuelo antes de destruir).
    for (auto it = m_ids.begin(); it != m_ids.end();) {
        if (world.get(it->first)) { ++it; continue; }
        Renderer::resources().release(it->second);
        it = m_ids.erase(it);
    }

    // ── La ventana de recorte: si el campo cambió o el pie se movió más de 40 m ────────────────
    const double camR = glm::length(camRelPlanet);
    if (camR < 1.0) return;
    const glm::dvec3 foot = camRelPlanet / camR * (world.planetRadius() + surfaceElevM);
    bool upload = false;
    { HARUKA_PROFILE("vox.cut");
    if (world.version() != m_cutVersion || m_cutDirty || glm::length(foot - m_cutCenter) > 40.0) {
        m_cutDirty = false;
        rebuildCut(world, camRelPlanet, surfaceElevM);
        upload = true;
    }
    // ⚠️ LA MATRIZ SE REHACE CADA FRAME, LA TEXTURA NO. `vFragPos` es relativo a la cámara de ESTE
    // frame; la textura se rehace cada 40 m. Con la matriz congelada en el rehecho, la ventana se
    // movía con la cámara hasta 40 m antes de volver a su sitio: el agujero se desplazaba al andar.
    // El marco (e1, e2, up, pie) es el del rehecho; sólo la traslación cambia.
    else refreshCutSpace(camRelPlanet);
    }

    // ── The fine bricks and the fine window ─────────────────────────────────────────────────────
    { HARUKA_PROFILE("vox.fine");
    static const bool s_fineOff = [] { const char* e = std::getenv("HARUKA_VOX_FINE"); return e && e[0] == '0'; }();
    m_fineOn = !s_fineOff;
    updateFine(world, camRelPlanet);
    bool anyWanted = false;
    m_fine.forEach([&](const World::Vox::VoxFineBricks::Brick& b) { anyWanted = anyWanted || b.wanted; });
    if (collectFineCut(camRelPlanet)) upload = true;   // a fine window computed in a job landed
    uint64_t readySig = 0;
    for (const auto& [bk, g] : m_gpuBricks) if (brickReady(g)) readySig += (uint64_t)World::Vox::VoxFineBricks::KeyHash{}(bk) * 31u + g.brickRev;
    if (!anyWanted) {
        if (m_fineCutHave) { m_fineCutHave = false; upload = true; }   // back to the plain coarse texture
    } else {
        if (!m_fineCutPending && (!m_fineCutHave || world.version() + readySig != m_fineCutVersion || glm::length(foot - m_fineCutCenter) > 2.0))
        { m_readySig = readySig; rebuildFineCut(world, camRelPlanet, surfaceElevM); }   // starts the job; the current window stays
        if (m_fineCutHave) refreshFineCutSpace(camRelPlanet);
    }
    if (upload) uploadCutAtlas();
    // The terrain may read the fine window wherever it covers (its alpha says what was evaluated).
    m_fineR = m_fineCutHave ? (float)std::max(kFineCutHalfM - glm::length(foot - m_fineCutCenter) - 0.5, 0.0) : 0.0f;
    updatePush(world);
    }
}

bool VoxRenderer::brickReady(const GpuBrick& g) const {
    uint64_t gpuRev = 0;
    return g.empty || (g.id && Renderer::resources().gpu(g.id, &gpuRev) && gpuRev >= g.storeRev);
}

void VoxRenderer::updatePush(World::Vox::VoxWorld& world) {
    // Which cells each chunk has covered by an UPLOADED fine brick (with its revision).
    std::unordered_map<World::Vox::VoxKey, std::vector<std::pair<glm::ivec3, uint64_t>>, World::Vox::VoxKeyHash> cover;
    for (const auto& [bk, g] : m_gpuBricks) if (brickReady(g)) cover[bk.key].push_back({ bk.cell, g.brickRev });
    for (const auto& [k, id] : m_ids) {
        auto cv = cover.find(k);
        uint64_t meshRev = 0;
        auto mesh = Renderer::resources().cpu<World::Vox::CaveMesh>(id, &meshRev);
        const World::Vox::VoxChunk* c = world.get(k);
        auto pit = m_push.find(k);
        if (cv == cover.end() || !mesh || mesh->empty() || !c) {
            if (pit != m_push.end()) { if (pit->second.id) Renderer::resources().release(pit->second.id); m_push.erase(pit); }
            continue;
        }
        std::sort(cv->second.begin(), cv->second.end(), [](const auto& a, const auto& b) {
            return std::tie(a.first.x, a.first.y, a.first.z) < std::tie(b.first.x, b.first.y, b.first.z); });
        uint64_t sig = 1469598103934665603ull ^ meshRev;
        for (const auto& [cell, rev] : cv->second) {
            const uint64_t v[4] = { (uint64_t)(uint32_t)cell.x, (uint64_t)(uint32_t)cell.y, (uint64_t)(uint32_t)cell.z, rev };
            for (uint64_t x : v) sig = (sig ^ x) * 1099511628211ull;
        }
        PushMesh& pm = m_push[k];
        if (pm.sig == sig) continue;
        pm.sig = sig;
        auto pushed = std::make_shared<World::Vox::CaveMesh>(*mesh);
        std::unordered_set<uint64_t> cells;
        for (const auto& [cell, rev] : cv->second) cells.insert(((uint64_t)(uint32_t)cell.x << 42) ^ ((uint64_t)(uint32_t)cell.y << 21) ^ (uint64_t)(uint32_t)cell.z);
        for (size_t i = 0; i < pushed->positions.size(); ++i) {
            const glm::dvec3 g = c->gridOf(pushed->origin + glm::dvec3(pushed->positions[i]));
            const uint64_t key = ((uint64_t)(uint32_t)(int)std::floor(g.x) << 42) ^ ((uint64_t)(uint32_t)(int)std::floor(g.y) << 21) ^ (uint64_t)(uint32_t)(int)std::floor(g.z);
            if (cells.count(key)) pushed->positions[i] += pushed->normals[i] * kCoarsePushM;   // the normal points into the rock
        }
        if (!pm.id) pm.id = Renderer::resources().create("voxpush", false);
        pm.storeRev = Renderer::resources().publish<World::Vox::CaveMesh>(pm.id, pushed, &VoxRenderer::uploadMesh);
    }
    for (auto it = m_push.begin(); it != m_push.end();) {
        if (m_ids.count(it->first)) { ++it; continue; }
        if (it->second.id) Renderer::resources().release(it->second.id);
        it = m_push.erase(it);
    }
}

void VoxRenderer::updateFine(World::Vox::VoxWorld& world, const glm::dvec3& cam) {
    if (m_fineOn) m_fine.update(world, cam);
    // The GPU side follows the bricks: a new revision of `walls` is published (the store uploads it
    // within the frame budget); a brick the world dropped is released.
    std::unordered_set<World::Vox::VoxFineBricks::Key, World::Vox::VoxFineBricks::KeyHash> alive;
    m_fine.forEach([&](const World::Vox::VoxFineBricks::Brick& b) {
        if (!b.wanted || !b.built()) return;
        alive.insert(b.key);
        GpuBrick& g = m_gpuBricks[b.key];
        g.dist = b.dist; g.halfDiag = b.halfDiag;
        if (g.brickRev == b.revision) return;
        g.brickRev = b.revision;
        g.empty = !b.walls || b.walls->empty();
        if (g.empty) return;
        if (!g.id) g.id = Renderer::resources().create("voxfine", true);
        g.storeRev = Renderer::resources().publish<World::Vox::CaveMesh>(g.id, b.walls, &VoxRenderer::uploadMesh);
    });
    for (auto it = m_gpuBricks.begin(); it != m_gpuBricks.end();) {
        if (alive.count(it->first)) { ++it; continue; }
        if (it->second.id) Renderer::resources().release(it->second.id);
        it = m_gpuBricks.erase(it);
    }
}

void VoxRenderer::rebuildFineCut(World::Vox::VoxWorld& world, const glm::dvec3& camRelPlanet, double surfaceElevM) {
    const double camR = glm::length(camRelPlanet);
    const glm::dvec3 up = camRelPlanet / camR;
    const glm::dvec3 foot = up * (world.planetRadius() + surfaceElevM);
    const glm::dvec3 ref = (std::fabs(up.y) < 0.9) ? glm::dvec3(0, 1, 0) : glm::dvec3(1, 0, 0);
    const glm::dvec3 e1 = glm::normalize(glm::cross(ref, up)), e2 = glm::cross(up, e1);
    const int W = kFineCutRes;
    // Only the texels under a wanted brick WITH a wall or with air in it are evaluated (its 8 corners
    // projected on the window's plane, one texel of margin): elsewhere nothing is cut. Measured on a
    // 30 cm pick: 11 bricks chosen, 3.8 ms for all of them; most have no wall.
    std::vector<uint8_t> want((size_t)W * W, 0);
    const double footUp = glm::dot(foot, up);
    m_fine.forEach([&](const World::Vox::VoxFineBricks::Brick& b) {
        // Only bricks already UPLOADED (a hole is never cut before its wall is drawn), with a wall or
        // with air in them: elsewhere the alpha stays 0 and the terrain reads the coarse window.
        if (!b.wanted || !b.built() || ((!b.walls || b.walls->empty()) && !b.air)) return;
        auto gb = m_gpuBricks.find(b.key);
        if (gb == m_gpuBricks.end() || !brickReady(gb->second) || gb->second.brickRev != b.revision) return;
        const World::Vox::VoxFineBricks::Key& bk = b.key;
        const World::Vox::VoxChunk* c = world.get(bk.key);
        if (!c) return;
        double x0 = 1e30, y0 = 1e30, x1 = -1e30, y1 = -1e30;
        bool ok = true;
        for (int corner = 0; corner < 8 && ok; ++corner) {
            const glm::dvec3 d = glm::normalize(c->worldOfGrid(bk.cell.x + (corner & 1), bk.cell.y + ((corner >> 1) & 1), bk.cell.z + ((corner >> 2) & 1)));
            const double du = glm::dot(d, up);
            if (du <= 1e-6) { ok = false; break; }
            const glm::dvec3 q = d * (footUp / du) - foot;
            const double tx = (glm::dot(q, e1) / (2.0 * kFineCutHalfM) + 0.5) * W - 0.5, ty = (glm::dot(q, e2) / (2.0 * kFineCutHalfM) + 0.5) * W - 0.5;
            x0 = std::min(x0, tx); x1 = std::max(x1, tx); y0 = std::min(y0, ty); y1 = std::max(y1, ty);
        }
        if (!ok) return;
        const int ix0 = std::max(0, (int)std::floor(x0) - 1), ix1 = std::min(W - 1, (int)std::ceil(x1) + 1);
        const int iy0 = std::max(0, (int)std::floor(y0) - 1), iy1 = std::min(W - 1, (int)std::ceil(y1) + 1);
        for (int y = iy0; y <= iy1; ++y) for (int x = ix0; x <= ix1; ++x) want[(size_t)y * W + x] = 1;
    });
    // Texel CENTRES ((x + 0.5) / W), the same convention the shader's bilinear uses; all in one call.
    std::vector<glm::dvec3> dirs; std::vector<size_t> where;
    for (int y = 0; y < W; ++y)
        for (int x = 0; x < W; ++x) {
            if (!want[(size_t)y * W + x]) continue;
            const double fx = (((double)x + 0.5) / W - 0.5) * 2.0 * kFineCutHalfM;
            const double fy = (((double)y + 0.5) / W - 0.5) * 2.0 * kFineCutHalfM;
            dirs.push_back(foot + e1 * fx + e2 * fy);
            where.push_back((size_t)y * W + x);
        }
    // THE TEXELS ARE EVALUATED IN A JOB: measured 1.8-4.5 ms on the render thread every 2 m walked
    // (and the game's heightfield costs more than the tests'). The window in use stays valid until
    // the new one lands: it carries its own frame, and the fine radius is capped by its coverage.
    const World::Vox::VoxWorld* wp = &world;
    const uint64_t version = world.version() + m_readySig;
    m_fineCutJob = Core::Jobs::jobs().submit(Core::Jobs::Priority::Frame,
        [wp, dirs = std::move(dirs), where = std::move(where), foot, e1, e2, up, version]() {
            FineCut r;
            r.pixels.assign((size_t)kFineCutRes * kFineCutRes * 4, 0);   // alpha 0 = not evaluated: the coarse window rules
            std::vector<float> cuts;
            wp->surfaceCutFineMany(dirs, cuts);
            for (size_t i = 0; i < where.size(); ++i) {
                unsigned char* px = &r.pixels[where[i] * 4];
                px[0] = (unsigned char)std::lround(cuts[i] * 255.0f);
                px[3] = 255;
                if (cuts[i] > 0.5f) {
                    ++r.open;
                    // THE WATER BED (G): in tenths of a metre where it fits (B = 255 says so), in
                    // metres like the coarse window when it does not (B = 0). lib/vox_cut.glsl decodes it.
                    const float bed = wp->floorDepthM(dirs[i], 255.0f);
                    if (bed < 25.5f) { px[1] = (unsigned char)std::lround(bed * 10.0f); px[2] = 255; }
                    else             { px[1] = (unsigned char)std::lround(std::min(bed, 255.0f)); px[2] = 0; }
                }
            }
            r.evaluated = dirs.size();
            r.center = foot; r.e1 = e1; r.e2 = e2; r.up = up; r.version = version;
            return r;
        });
    m_fineCutPending = true;
    (void)camRelPlanet;
}

bool VoxRenderer::collectFineCut(const glm::dvec3& camRelPlanet) {
    if (!m_fineCutPending || m_fineCutJob.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return false;
    FineCut r = m_fineCutJob.get();
    m_fineCutPending = false;
    m_fineCutPixels = std::move(r.pixels);
    m_fineCutCenter = r.center; m_fineE1 = r.e1; m_fineE2 = r.e2; m_fineUp = r.up;
    m_fineCutVersion = r.version;
    m_fineCutHave = true;
    refreshFineCutSpace(camRelPlanet);
    HARUKA_LOGDIAG("Vox", "fine cut window: %zu texels evaluated · %zu open", r.evaluated, r.open);
    return true;
}

void VoxRenderer::refreshFineCutSpace(const glm::dvec3& camRelPlanet) {
    const glm::dvec3 footRelCam = m_fineCutCenter - camRelPlanet;
    const glm::dvec3 &e1 = m_fineE1, &e2 = m_fineE2, &up = m_fineUp;
    glm::dmat4 toFrame(1.0);
    toFrame[0] = glm::dvec4(e1.x, e2.x, up.x, 0.0);
    toFrame[1] = glm::dvec4(e1.y, e2.y, up.y, 0.0);
    toFrame[2] = glm::dvec4(e1.z, e2.z, up.z, 0.0);
    toFrame[3] = glm::dvec4(-glm::dot(footRelCam, e1), -glm::dot(footRelCam, e2), -glm::dot(footRelCam, up), 1.0);
    glm::dmat4 toUV(1.0);
    toUV[0][0] = 0.5 / kFineCutHalfM; toUV[1][1] = 0.5 / kFineCutHalfM; toUV[3] = glm::dvec4(0.5, 0.5, 0.0, 1.0);
    m_fineCutSpace = glm::mat4(toUV * toFrame);
}

float VoxRenderer::fineBedAt(const glm::vec3& posRelEye) const {
    if (!m_fineCutHave || m_fineCutPixels.empty()) return -1.0f;
    const glm::vec4 uv = m_fineCutSpace * glm::vec4(posRelEye, 1.0f);
    if (uv.x <= 0.0f || uv.x >= 1.0f || uv.y <= 0.0f || uv.y >= 1.0f) return -1.0f;
    const int x = std::min((int)(uv.x * kFineCutRes), kFineCutRes - 1), y = std::min((int)(uv.y * kFineCutRes), kFineCutRes - 1);
    const unsigned char* px = &m_fineCutPixels[((size_t)y * kFineCutRes + x) * 4];
    return px[2] > 127 ? (float)px[1] * 0.1f : (float)px[1];
}

float VoxRenderer::fineCutAt(const glm::vec3& posRelEye) const {
    if (!m_fineCutHave || m_fineCutPixels.empty()) return -1.0f;
    const glm::vec4 uv = m_fineCutSpace * glm::vec4(posRelEye, 1.0f);
    if (uv.x <= 0.0f || uv.x >= 1.0f || uv.y <= 0.0f || uv.y >= 1.0f) return -1.0f;
    const int x = std::min((int)(uv.x * kFineCutRes), kFineCutRes - 1), y = std::min((int)(uv.y * kFineCutRes), kFineCutRes - 1);
    return (float)m_fineCutPixels[((size_t)y * kFineCutRes + x) * 4] / 255.0f;
}

glm::vec4 VoxRenderer::cutAtlas() const {
    if (!m_fineCutHave) return glm::vec4(0.0f);
    const float aw = (float)(kCutRes + kFineCutRes), ah = (float)std::max(kCutRes, kFineCutRes);
    return glm::vec4((float)kCutRes / aw, (float)kCutRes / ah, (float)kCutRes / aw, (float)kFineCutRes / aw);
}

void VoxRenderer::uploadCutAtlas() {
    RHI::Device* dev = RHI::device();
    if (!dev || m_cutPixels.empty()) return;
    // The RHI has no "update texture": it is recreated, and the old one is parked until the next
    // `update` (it may be bound to a command buffer in flight — Vulkan).
    if (RHI::valid(m_cutTex)) {
        if (RHI::valid(m_cutTexOld)) dev->destroy(m_cutTexOld);   // only one upload per update: this one is a frame old
        m_cutTexOld = m_cutTex;
    }
    RHI::TextureDesc td; td.format = RHI::Format::RGBA8;   // R = cut · G = water bed (m, coarse only)
    td.filter = RHI::Filter::Linear; td.wrap = RHI::Wrap::ClampToEdge; td.mipmaps = false;
    std::vector<unsigned char> atlas;
    if (!m_fineCutHave) {
        td.width = kCutRes; td.height = kCutRes; td.initialData = m_cutPixels.data();
    } else {
        // ATLAS: the coarse window in the lower-left corner, the fine one to its right, full height.
        const int aw = kCutRes + kFineCutRes, ah = std::max(kCutRes, kFineCutRes);
        atlas.assign((size_t)aw * ah * 4, 0);
        for (size_t o = 3; o < atlas.size(); o += 4) atlas[o] = 255;
        for (int y = 0; y < kCutRes; ++y)
            std::memcpy(&atlas[((size_t)y * aw) * 4], &m_cutPixels[((size_t)y * kCutRes) * 4], (size_t)kCutRes * 4);
        for (int y = 0; y < kFineCutRes; ++y)
            std::memcpy(&atlas[((size_t)y * aw + kCutRes) * 4], &m_fineCutPixels[((size_t)y * kFineCutRes) * 4], (size_t)kFineCutRes * 4);
        td.width = aw; td.height = ah; td.initialData = atlas.data();
    }
    m_cutTex = dev->createTexture(td);
}


void VoxRenderer::rebuildCut(World::Vox::VoxWorld& world, const glm::dvec3& camRelPlanet, double surfaceElevM) {
    RHI::Device* dev = RHI::device();
    const double camR = glm::length(camRelPlanet);
    const glm::dvec3 up = camRelPlanet / camR;
    const glm::dvec3 foot = up * (world.planetRadius() + surfaceElevM);
    const glm::dvec3 ref = (std::fabs(up.y) < 0.9) ? glm::dvec3(0, 1, 0) : glm::dvec3(1, 0, 0);
    const glm::dvec3 e1 = glm::normalize(glm::cross(ref, up)), e2 = glm::cross(up, e1);

    m_cutPixels.assign((size_t)kCutRes * kCutRes * 4, 0);
    size_t abiertos = 0, evaluados = 0;
    { HARUKA_PROFILE("vox.cut.loop");
    double depthMs = 0.0;
    // ── QUÉ TÉXELES PUEDEN ABRIRSE ──────────────────────────────────────────────────────────────
    // `surfaceCut` sólo sale > 0 donde el chunk de la superficie tiene el canal QUITADO reservado
    // (cueva, trazo). Se proyecta la huella de cada columna así a la ventana (sus 4 esquinas sobre
    // el plano del pie, ±2 téxeles de margen por la curvatura) y sólo esos téxeles se evalúan: el
    // resto es 0 exacto, sin muestrear la cota 25 600 veces.
    std::vector<uint8_t> want((size_t)kCutRes * kCutRes, 0);
    const double footUp = glm::dot(foot, up);
    auto toTexel = [&](const glm::dvec3& d, double& tx, double& ty) {
        const double du = glm::dot(d, up);
        if (du <= 1e-6) return false;
        const glm::dvec3 q = d * (footUp / du) - foot;
        tx = (glm::dot(q, e1) / (2.0 * kCutHalfM) + 0.5) * (kCutRes - 1);
        ty = (glm::dot(q, e2) / (2.0 * kCutHalfM) + 0.5) * (kCutRes - 1);
        return true;
    };
    for (const World::Vox::VoxKey& k : world.loadedKeys()) {
        const World::Vox::VoxChunk* c = world.get(k);
        if (!c || c->d.empty()) continue;
        if (glm::dot(c->centerDir, up) < std::cos((kCutHalfM * 1.5 + World::Vox::kVoxChunkM) / world.planetRadius())) continue;
        double x0 = 1e30, y0 = 1e30, x1 = -1e30, y1 = -1e30;
        bool ok = true;
        const double N = (double)(World::Vox::kVoxN - 1);
        for (int cy = 0; cy <= 1 && ok; ++cy)
            for (int cx = 0; cx <= 1 && ok; ++cx) {
                double tx, ty;
                ok = toTexel(glm::normalize(c->worldOfGrid(cx * N, cy * N, 0.0)), tx, ty);
                x0 = std::min(x0, tx); x1 = std::max(x1, tx); y0 = std::min(y0, ty); y1 = std::max(y1, ty);
            }
        if (!ok) continue;
        const int ix0 = std::max(0, (int)std::floor(x0) - 2), ix1 = std::min(kCutRes - 1, (int)std::ceil(x1) + 2);
        const int iy0 = std::max(0, (int)std::floor(y0) - 2), iy1 = std::min(kCutRes - 1, (int)std::ceil(y1) + 2);
        for (int y = iy0; y <= iy1; ++y)
            for (int x = ix0; x <= ix1; ++x) want[(size_t)y * kCutRes + x] = 1;
    }
    for (int y = 0; y < kCutRes; ++y)
        for (int x = 0; x < kCutRes; ++x) {
            if (!want[(size_t)y * kCutRes + x]) continue;
            ++evaluados;
            const double fx = ((double)x / (kCutRes - 1) - 0.5) * 2.0 * kCutHalfM;
            const double fy = ((double)y / (kCutRes - 1) - 0.5) * 2.0 * kCutHalfM;
            const glm::dvec3 dir = glm::normalize(foot + e1 * fx + e2 * fy);
            // ⚠️ SÓLO DONDE YA HAY PARED QUE ENSEÑAR. Si el chunk de la superficie está sin remallar
            // (3 por frame), recortar el suelo deja un agujero al vacío durante unos frames — y por él
            // se ve el plano del mar/lago, o el cielo. El recorte espera a la malla.
            // ⚠️ "MALLADO ALGUNA VEZ", NO "NO SUCIO". Con `!dirty`, cada trazo (que ensucia el chunk y
            // a sus 26 vecinos) apagaba el recorte hasta que el remallado los alcanzaba: el suelo
            // original reaparecía un momento sobre el agujero ("la malla original fantasma").
            // Mientras se remalla, la malla vieja sigue dibujándose, así que el recorte puede quedarse.
            const World::Vox::VoxKey sk = world.keyAt(dir * (world.planetRadius() + (double)(world.elevAt(dir)) - 3.0));
            float cut = 0.0f, depth = 0.0f;
            if (world.get(sk) && m_ids.count(sk)) {
                cut = world.surfaceCut(dir);
                if (cut > 0.0f) {
                    const auto t0 = std::chrono::high_resolution_clock::now();
                    depth = world.floorDepthM(dir, 255.0f);
                    depthMs += std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - t0).count();
                }
            }
            const unsigned char v = (unsigned char)std::lround(cut * 255.0f);
            // G = el LECHO: metros de aire bajo la superficie (hasta 255). Es lo que baja el suelo
            // para el agua: la lámina de un lago que cubra la boca se dibuja DENTRO del foso, y la
            // que quede por debajo del lecho, no. Sólo se calcula donde hay recorte (baja por la
            // columna metro a metro).
            const size_t o = ((size_t)y * kCutRes + x) * 4;
            m_cutPixels[o + 0] = v;
            m_cutPixels[o + 1] = (unsigned char)std::lround(std::min(depth, 255.0f));
            m_cutPixels[o + 2] = 0; m_cutPixels[o + 3] = 255;
            if (v > 127) ++abiertos;
        }
    Tools::Profiler::get().add("vox.cut.depth", depthMs);
    }
    // Alfa a 255 también fuera de la huella (lo que había antes en toda la ventana).
    for (size_t o = 3; o < m_cutPixels.size(); o += 4) m_cutPixels[o] = 255;
    // The texture itself is made once per `update`, from both windows (`uploadCutAtlas`).
    (void)dev;
    m_cutCenter = foot; m_cutE1 = e1; m_cutE2 = e2; m_cutUp = up;
    refreshCutSpace(camRelPlanet);
    const glm::dvec3 footRelCam = foot - camRelPlanet;
    m_cutVersion = world.version();
    if (const char* dump = std::getenv("HARUKA_VOX_CUT_PNG")) {   // lo que la CPU cree que sube
        IO::writePNG(dump, kCutRes, kCutRes, 4, m_cutPixels.data());
    }
    m_cutReady = true;
    // DIAG DEL BUG "LA HIERBA TAPA LA BOCA": la primera ventana con boca (abiertos>0) se volca
    // SIEMPRE a `voxcut_first.png` y se resume — sin depender de env ni del ciclo de 30 rebuilds.
    // Con eso se ve si la textura que recibe la hierba tiene la boca (blanco) o no (negro).
    static bool s_hadOpen = false;
    if (abiertos > 0 && !s_hadOpen) {
        s_hadOpen = true;
        IO::writePNG("voxcut_first.png", kCutRes, kCutRes, 4, m_cutPixels.data());
    }
    static uint64_t s_log = 0;
    if (abiertos > 0 && (s_log++ % 30) == 0) {
        // Y el valor BAJO LOS PIES: si la cámara está sobre una boca y aquí sale 0, el recorte no
        // puede verse por mucho que la ventana tenga téxeles abiertos en otra parte.
        const unsigned char pie = m_cutPixels[((size_t)(kCutRes / 2) * kCutRes + kCutRes / 2) * 4];
        // Comprobación de la matriz con el propio pie: tiene que caer en (0,5, 0,5).
        const glm::vec4 uvPie = m_cutSpace * glm::vec4(glm::vec3(footRelCam), 1.0f);
        const glm::vec4 uv100 = m_cutSpace * glm::vec4(glm::vec3(footRelCam + e1 * 100.0), 1.0f);
        HARUKA_LOGDIAG("Vox", "  matriz: pie+100m·e1 -> uv (%.3f, %.3f) (esperado 0,656, 0,5)", uv100.x, uv100.y);
        HARUKA_LOGDIAG("Vox", "ventana de recorte: %zu texeles abiertos de %d (%.2f %%; %zu evaluados) · %zu chunks · bajo los pies %u/255 · uv(pie)=(%.3f, %.3f)",
                    abiertos, kCutRes * kCutRes, 100.0 * abiertos / (kCutRes * kCutRes), evaluados, world.loadedCount(),
                    (unsigned)pie, uvPie.x, uvPie.y);
    }
}

void VoxRenderer::refreshCutSpace(const glm::dvec3& camRelPlanet) {
    // De posición RELATIVA A LA CÁMARA a uv de la ventana: primero al marco (e1, e2) centrado en
    // el pie, luego a [0,1]. El pie relativo a la cámara es (foot − cam), con la cámara de AHORA.
    const glm::dvec3 footRelCam = m_cutCenter - camRelPlanet;
    const glm::dvec3 &e1 = m_cutE1, &e2 = m_cutE2, &up = m_cutUp;
    glm::dmat4 toFrame(1.0);
    toFrame[0] = glm::dvec4(e1.x, e2.x, up.x, 0.0);
    toFrame[1] = glm::dvec4(e1.y, e2.y, up.y, 0.0);
    toFrame[2] = glm::dvec4(e1.z, e2.z, up.z, 0.0);
    toFrame[3] = glm::dvec4(-glm::dot(footRelCam, e1), -glm::dot(footRelCam, e2), -glm::dot(footRelCam, up), 1.0);
    glm::dmat4 toUV(1.0);
    toUV[0][0] = 0.5 / kCutHalfM; toUV[1][1] = 0.5 / kCutHalfM; toUV[3] = glm::dvec4(0.5, 0.5, 0.0, 1.0);
    m_cutSpace = glm::mat4(toUV * toFrame);
}

Renderer::GpuSet VoxRenderer::uploadMesh(const void* cpu) {
    const World::Vox::CaveMesh& mesh = *static_cast<const World::Vox::CaveMesh*>(cpu);
    Renderer::GpuSet set;
    if (mesh.empty()) return set;   // nada que dibujar: sin buffers (la física sí la ve, vacía)
    RHI::Device* dev = RHI::device();
    std::vector<float> vtx; vtx.reserve(mesh.positions.size() * 6);
    for (size_t i = 0; i < mesh.positions.size(); ++i) {
        const glm::vec3& p = mesh.positions[i]; const glm::vec3& n = mesh.normals[i];
        vtx.insert(vtx.end(), { p.x, p.y, p.z, n.x, n.y, n.z });
    }
    const VoxUBO z{};
    set.buffers = {
        dev->createBuffer(RHI::BufferUsage::Vertex, vtx.size() * sizeof(float), vtx.data(), RHI::BufferMemory::Static),
        dev->createBuffer(RHI::BufferUsage::Index, mesh.indices.size() * sizeof(unsigned), mesh.indices.data(), RHI::BufferMemory::Static),
        dev->createBuffer(RHI::BufferUsage::Uniform, sizeof(VoxUBO), &z, RHI::BufferMemory::Dynamic),
    };
    set.meta = std::make_shared<const VoxDraw>(VoxDraw{ mesh.origin, (uint32_t)mesh.indices.size() });
    return set;
}

void VoxRenderer::forEachCpuMesh(
        const std::function<void(const World::Vox::VoxKey&, const World::Vox::CaveMesh&, uint64_t revision)>& fn) const {
    for (const auto& [key, id] : m_ids) {
        uint64_t rev = 0;
        if (auto m = Renderer::resources().cpu<World::Vox::CaveMesh>(id, &rev)) fn(key, *m, rev);
    }
}

void VoxRenderer::draw(const World::Vox::VoxWorld& world, const glm::mat4& rotVP, const glm::dvec3& camRelPlanet,
                       const glm::vec3& sunDir, const glm::vec3& viewDir) {
    m_drawn = 0; m_tris = 0; m_culled = 0;
    static const bool s_off = [] { const char* e = std::getenv("HARUKA_VOX_DRAW"); return e && e[0] == '0'; }();
    if (s_off || !RHI::valid(m_pipe) || (m_ids.empty() && m_gpuBricks.empty())) return;
    RHI::Device* dev = RHI::device();
    RHI::Context* ctx = dev->beginFrame();
    if (!ctx) return;
    // FRUSTUM: los seis planos salen de `rotVP` (Gribb-Hartmann) en el espacio relativo al ojo, que
    // es el de las posiciones. Reversed-Z con lejano infinito: el plano "lejano" es degenerado y no
    // se usa. Cada chunk se prueba como ESFERA (centro + radio de su diagonal/2): conservador, nunca
    // descarta algo visible.
    glm::vec4 planes[5];
    {
        const glm::mat4 m = glm::transpose(rotVP);   // filas de rotVP
        planes[0] = m[3] + m[0];   // izquierda
        planes[1] = m[3] - m[0];   // derecha
        planes[2] = m[3] + m[1];   // abajo
        planes[3] = m[3] - m[1];   // arriba
        planes[4] = m[2];          // cercano (reversed-Z: z_clip >= 0)
        for (auto& p : planes) { const float l = glm::length(glm::vec3(p)); if (l > 1e-9f) p /= l; }
    }
    (void)viewDir;
    ctx->bindPipeline(m_pipe);
    for (const auto& [key, id] : m_ids) {
        if (!m_drawCoarse) break;
        const Renderer::GpuSet* gs = Renderer::resources().gpu(id);
        // The PUSHED version when a fine brick covers part of this chunk and it is uploaded.
        if (auto pit = m_push.find(key); pit != m_push.end() && pit->second.id) {
            uint64_t prev = 0;
            const Renderer::GpuSet* ps = Renderer::resources().gpu(pit->second.id, &prev);
            if (ps && prev >= pit->second.storeRev && ps->metaAs<VoxDraw>()) gs = ps;
        }
        const VoxDraw* d = gs ? gs->metaAs<VoxDraw>() : nullptr;
        if (!d || !d->indexCount || gs->buffers.size() < 3) continue;   // aún sin subir, o vacía
        const RHI::BufferHandle vb = gs->buffers[0], ib = gs->buffers[1], ubo = gs->buffers[2];
        const World::Vox::VoxChunk* c = world.get(key);
        if (!c) continue;
        {
            const glm::vec3 centerRel = glm::vec3(c->centerDir * (c->baseR + 0.5 * World::Vox::kVoxChunkM) - camRelPlanet);
            // ⚠️ ×1,3 de margen: el chunk no es una esfera ni un cubo (es un trozo de casquete), y con
            // el radio justo la contraprueba de abajo cazó descartes con una esquina en pantalla.
            const float radius = 1.3f * 0.5f * std::sqrt(c->sideU * c->sideU + c->sideV * c->sideV + (float)(World::Vox::kVoxChunkM * World::Vox::kVoxChunkM)) + 4.0f;
            static const bool s_noCull = [] { const char* e = std::getenv("HARUKA_VOX_NOCULL"); return e && e[0] == '1'; }();
            bool out = false;
            for (const auto& p : planes)
                if (s_noCull) break; else
                if (glm::dot(glm::vec3(p), centerRel) + p.w < -radius) { out = true; break; }
            if (out) {
                // CONTRAPRUEBA del cull: si alguna de las 8 esquinas del chunk cae DENTRO del clip
                // (todas las coordenadas |x|,|y| <= w con w > 0), el descarte es falso y se dice.
                bool esquinaDentro = false;
                for (int cz = 0; cz <= 1 && !esquinaDentro; ++cz)
                    for (int cy = 0; cy <= 1 && !esquinaDentro; ++cy)
                        for (int cx = 0; cx <= 1 && !esquinaDentro; ++cx) {
                            const double N = (double)(World::Vox::kVoxN - 1);
                            const glm::vec3 rel = glm::vec3(c->worldOfGrid(cx * N, cy * N, cz * N) - camRelPlanet);
                            const glm::vec4 q = rotVP * glm::vec4(rel, 1.0f);
                            if (q.w > 0.0f && std::fabs(q.x) <= q.w && std::fabs(q.y) <= q.w) esquinaDentro = true;
                        }
                static int s_falsos = 0;
                if (esquinaDentro && (s_falsos++ % 60) == 0)
                    HARUKA_LOGW("Vox", "CULL FALSO: chunk (%d,%d,%d,%d) descartado con una esquina en pantalla",
                                key.face, key.i, key.j, key.k);
                ++m_culled; continue;
            }
        }
        VoxUBO u{};
        u.rotVP = rotVP;
        u.originRel = glm::vec4(glm::vec3(d->origin - camRelPlanet), 0.0f);
        u.lightDir = glm::vec4(sunDir, 0.0f);
        u.color = glm::vec4(0.42f, 0.38f, 0.34f, 1.0f);
        u.up    = glm::vec4(glm::vec3(c->centerDir), 0.0f);
        // Origen del chunk reducido módulo el tile EN DOUBLES: lo que llega al shader es pequeño y
        // exacto, y como el origen es fijo en el mundo la textura no se mueve con la cámara.
        const double T = (double)std::max(m_tiling, 0.01f);
        const glm::dvec3 o = d->origin;
        u.texOrigin = glm::vec4((float)(o.x - std::floor(o.x / T) * T), (float)(o.y - std::floor(o.y / T) * T),
                                (float)(o.z - std::floor(o.z / T) * T), RHI::valid(m_albedo) ? (float)m_rockLayer : -1.0f);
        u.texInfo = glm::vec4((float)T, 0.0f, 0.0f, 0.0f);
        u.fineClip = glm::vec4(0.0f);   // never cut: the covered cells are pushed back instead (see the header)
        // ⚠️ UN UBO POR CHUNK. `updateBuffer` sobre el mismo buffer entre dos draws del mismo
        // command buffer NO se versiona: todos los draws leerían la última escritura (el pase de
        // nodos, por eso, sube su UBO UNA vez antes de cualquier draw).
        dev->updateBuffer(ubo, 0, sizeof(u), &u);
        ctx->bindUniformBuffer(5, ubo);
        // Siempre atado (descriptor indefinido en Vulkan = pérdida del dispositivo): sin array de
        // roca se ata el mismo dummy del recorte, y `texOrigin.w < 0` hace que no se lea.
        ctx->bindTexture(1, RHI::valid(m_albedo) ? m_albedo : m_cutTex);
        ctx->bindVertexBuffer(vb);
        ctx->bindIndexBuffer(ib);
        ctx->drawIndexed(d->indexCount, 0, 1);
        ++m_drawn; m_tris += d->indexCount / 3;
    }
    // THE FINE BRICKS, inside the fine sphere only (the coarse meshes discard there).
    m_fineDrawn = 0;
    {
        for (const auto& [bk, b] : m_gpuBricks) {
            if (!b.id || b.empty) continue;
            const Renderer::GpuSet* gs = Renderer::resources().gpu(b.id);
            const VoxDraw* d = gs ? gs->metaAs<VoxDraw>() : nullptr;
            if (!d || !d->indexCount || gs->buffers.size() < 3) continue;
            const World::Vox::VoxChunk* c = world.get(bk.key);
            VoxUBO u{};
            u.rotVP = rotVP;
            u.originRel = glm::vec4(glm::vec3(d->origin - camRelPlanet), 0.0f);
            u.lightDir = glm::vec4(sunDir, 0.0f);
            u.color = glm::vec4(0.42f, 0.38f, 0.34f, 1.0f);
            u.up = glm::vec4(glm::vec3(c ? c->centerDir : glm::normalize(d->origin)), 0.0f);
            const double T = (double)std::max(m_tiling, 0.01f);
            const glm::dvec3 o = d->origin;
            u.texOrigin = glm::vec4((float)(o.x - std::floor(o.x / T) * T), (float)(o.y - std::floor(o.y / T) * T),
                                    (float)(o.z - std::floor(o.z / T) * T), RHI::valid(m_albedo) ? (float)m_rockLayer : -1.0f);
            u.texInfo = glm::vec4((float)T, 0.0f, 0.0f, 0.0f);
            u.fineClip = glm::vec4(0.0f);
            dev->updateBuffer(gs->buffers[2], 0, sizeof(u), &u);
            ctx->bindUniformBuffer(5, gs->buffers[2]);
            ctx->bindTexture(1, RHI::valid(m_albedo) ? m_albedo : m_cutTex);
            ctx->bindVertexBuffer(gs->buffers[0]);
            ctx->bindIndexBuffer(gs->buffers[1]);
            ctx->drawIndexed(d->indexCount, 0, 1);
            ++m_fineDrawn; m_tris += d->indexCount / 3;
        }
    }
    static bool s_first = true;
    static auto s_last = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    if ((m_drawn > 0 && s_first) || std::chrono::duration<double>(now - s_last).count() > 5.0) {
        s_first = m_drawn == 0; s_last = now;
        size_t sucios = 0, sinMalla = 0;
        for (const World::Vox::VoxKey& k : world.loadedKeys()) {
            const World::Vox::VoxChunk* c = world.get(k);
            if (c && c->dirty) ++sucios;
            if (!m_ids.count(k)) ++sinMalla;
        }
        int caras = 0; const float costura = world.seamMismatch(&caras);
        HARUKA_LOGDIAG("Vox", "dibujados %zu chunks · %zu triangulos · fuera del frustum %zu · cargados %zu (con contenido %zu · %.1f MB de voxel · sucios %zu, sin malla %zu) · costura peor %.3f m en %d caras",
                    m_drawn, m_tris, m_culled, world.loadedCount(), world.loadedWithContent(), world.voxelBytes() / 1048576.0, sucios, sinMalla, costura, caras);
    }
}

} // namespace Haruka::Renderer
