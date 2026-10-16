#include "Haruka/World/Props/PropSystem.h"
#include "Haruka/Core/Modules.h"   // HARUKA_MOD_PHYSICS: without it the colliders compiled to nothing
#include "Haruka/Core/Jobs/JobSystem.h"

#include "Haruka/Core/Camera.h"
#include "Haruka/Core/Logger.h"
#include "Haruka/World/WorldSystem.h"
#include "Haruka/World/Props/PropScatter.h"    // scatterPropsNear + IPropSphereField (scatter GLOBAL)
#include "Haruka/World/Props/PropCollider.h"   // colliders por PARTE derivados del esqueleto del arbol
#include "Haruka/World/Planet/PlanetarySystem.h"
#include "Haruka/World/Planet/Planet.h"
#include "Haruka/Physics/PhysicsEngine.h"
#include "Haruka/Renderer/FallbackTexture.h"
#include "Haruka/Renderer/Shader.h"
#include "Haruka/RHI/RHIContext.h"
#include "Haruka/RHI/RHIDevice.h"
#include "Haruka/RHI/RHIGPUScope.h"
#include "Haruka/Tools/Profiler.h"
#include "Haruka/Tools/ProcGraph/TreeMesh.h"
#include "Haruka/Tools/ProcGraph/PropMesh.h"
#include "Haruka/Tools/ProcGraph/ClumpMesh.h"           // bakeClumpMesh + kClumpFootprintM (cúmulos de lejanía)
#include "Haruka/Tools/ProcGraph/TreeTextures.h"   // TreeCombineRGBNode (bake de material per-pixel)
#include "Haruka/Tools/ProcGraph/ProcTexture.h"    // evaluateToRGBA / evaluateToNormalMap / createRHIFromRGBA

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>   // memcpy: los bits de cellM en el brote GPU
#include <future>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/packing.hpp>   // packSnorm3x10_1x2 / packUnorm4x8: PropVertex empaquetado
#include <glm/gtc/quaternion.hpp>   // glm::mat4_cast
#include <glm/gtx/quaternion.hpp>   // glm::rotation (up del prototipo → dir radial)

namespace Haruka::World::Props {

using Haruka::RHI::valid;
using Renderer::FallbackTex;
using Renderer::fallbackTexture;

namespace {

// `PropParams` (UBO binding 6) y `PropVertex` viven en PropGpu.h: la capa masiva usa los mismos.
/// UBO del pase de sombras de props: matriz de luz + el mismo origen relativo.
struct PropShadowUBO {
    glm::mat4 lightSpace;
    glm::vec4 originRel;
};
static_assert(sizeof(PropShadowUBO) == 80, "PropShadowUBO std140 size mismatch");

} // namespace

// ── vida ──────────────────────────────────────────────────────────────────────────────────────

PropSystem::~PropSystem() { shutdown(); }

void PropSystem::shutdown() {
    if (RHI::Device* dev = RHI::device()) {
        if (valid(m_shadowUBO)) dev->destroy(m_shadowUBO);
        if (valid(m_instPSO))   dev->destroy(m_instPSO);
        if (valid(m_shadowPSO)) dev->destroy(m_shadowPSO);
    }
    m_shadowUBO = {}; m_instPSO = {}; m_shadowPSO = {};
    m_instancing.reset();
    PropSystemBase::shutdown();   // worker + mallas de prototipo + UBOs de parámetros
}

/// La instancia GPU de un prop: posición relativa a `origin` (le llega `originToCenter` = centro del
/// planeta − origen), giro = vertical local · yaw, escala uniforme. La construyen el worker del
/// scatter y `rebuildGpu`: una sola receta, o el prop se movería al pasar de una a otra.
static Renderer::InstanceDataFloat propInstance(const InstancedObject& io, const glm::dvec3& originToCenter,
                                                double planetR) {
    const glm::vec3 up(0.0f, 1.0f, 0.0f);
    const glm::vec3 posF = glm::vec3(originToCenter + glm::dvec3(io.dir) * (planetR + (double)io.heightM));
    // ⚠️ El giro de yaw va sobre el eje LOCAL, no sobre `io.dir`: primero el yaw alrededor de +Y del
    // prototipo y DESPUÉS la vertical up→dir. Hacerlo al revés (yaw sobre `io.dir`) TUMBABA el
    // objeto tanto como lo giraba ("los arboles estan tumbados"). Es el mismo orden que tenía la
    // matriz: translate · rotation(up,dir) · rotate(yaw, up) · scale.
    const glm::quat q = glm::rotation(up, io.dir) * glm::angleAxis(io.yaw, up);
    return Renderer::InstanceDataFloat::make(posF, q, glm::vec3(io.scale), io.tint, (float)io.breakMask);
}

// Las instancias GPU, UNA vez por scatter. El origen es la camara en ese momento: las
// posiciones relativas caben en float con precision de mm (el scatter llega a 6 km) y el shader
// suma `originRel` (origen − camara actual), una resta en double por frame.
void PropSystem::rebuildGpu(const glm::dvec3& planetC, double planetR, Core::Camera* camera) {
    const auto& insts = m_registry.instances();
    m_origin = camera ? glm::dvec3(camera->position) : planetC;
    m_gpu.resize(insts.size());
    const glm::dvec3 originToCenter = planetC - m_origin;
    for (size_t i = 0; i < insts.size(); ++i) {
        const auto& io = insts[i];
        m_gpu[i] = propInstance(io, originToCenter, planetR);
    }
}

// ── scatter ───────────────────────────────────────────────────────────────────────────────────

// ── re-siembra (los huecos de PropSystemBase) ─────────────────────────────────────────────────

void PropSystem::configureScatter(PropScatterParams& params) {
    params.maxProps = 300000;
    params.fadeInM  = kRingFadeM;
    // ⚠️ NO hay horizonte de cúmulos "por carga" NI "por autor" AQUÍ. La fusión sale del bioma/clima
    // (la capa sólo se coloca donde su ecología la acepta) + el NIVEL de celda del cubo (lo decide
    // shouldClumpFar dentro del scatter): el contenido de una celda es función de (celda, seed del
    // planeta, config) y nada más — reproducible entre jugadores y sincronizable al destruirse. La
    // dinámica del presupuesto se queda en la RENDERIZACIÓN (umbrales por píxel, cull), que no toca
    // lo que existe en el mundo.
    // Banda exterior por carga: el radio lo manda el controlador (`m_ringM`); las coronas se
    // estiran con el — celdas ~proporcionales al radio, asi la densidad por area no explota y el
    // recuento crece logaritmico (no cuadratico). Con m_ringM=6000 (arranque, piso) es EXACTAMENTE
    // el anillo historico {1000/12, 3000/40, 6000/120}: en reposo nada cambia.
    params.lods.clear();
    {
        struct B { float rad; float cell; };
        const std::vector<B> kBase = { {1000.0f, 12.0f}, {3000.0f, 40.0f}, {6000.0f, 120.0f} };
        for (const B& b : kBase)
            if (b.rad <= m_ringM) params.lods.push_back({ b.rad, b.cell, 0.30f });
        for (float rad = 9000.0f; rad <= m_ringM; rad += 3000.0f)
            params.lods.push_back({ rad, 120.0f * (rad / 6000.0f), 0.30f });   // ceil gruesa y creciente
        if (params.lods.empty()) params.lods.push_back({ m_ringM, 12.0f, 0.30f });
    }

}

PropSystem::ScatterJob PropSystem::makeJob(const ScatterContext& ctx) {
    // El worker construye ADEMÁS las InstancedObject finales (prototipo + yaw + estado) y sus
    // instancias GPU: se le pasan copias del nombre→índice y del estado roto. `m_state` la toca el
    // juego en el hilo de render; esta copia es la foto de lanzamiento, y `applyScatter` vuelve a
    // cuadrar el estado ACTUAL y el filtro de bocas al aplicar (no puede ir aquí: `surfaceCut` lee
    // el vox).
    const std::unordered_map<std::string, int> protoByName = ctx.protoByName;
    const std::unordered_map<uint32_t, StateDelta> stateSnap = m_state;
    const uint32_t planetSeed = ctx.planetSeed;
    const glm::dvec3 camPos = ctx.camPos, planetC = ctx.planetC;
    const double planetR = ctx.planetR;
    return [protoByName, stateSnap, planetSeed, camPos, planetC, planetR]
           (std::vector<ScatteredProp>&& placed) -> std::shared_ptr<ScatterOutput> {
        auto out = std::make_shared<PropOutput>();
        out->origin = camPos;
        // Fase de transformación (antes estaba en `applyScatterResult`, en el hilo de render, y
        // eran los ~50 ms del tiron): prototipo por nombre, yaw determinista y estado inicial,
        // y las MATRICES GPU — todo contra copias capturadas, nunca el registro ni la GPU.
        // ⚠️ El filtro de BOCA no va aquí: `surfaceCut` lee el VOX VIVIENTE, que el hilo de
        // render está streameando/rehorneando; leerlo en paralelo es una raza. Se filtra al
        // aplicar, contra el vox actual.
        std::vector<InstancedObject>& objs = out->objs;
        objs.reserve(placed.size());
        for (const auto& sp : placed) {
            auto itP = protoByName.find(sp.mesh);
            if (itP == protoByName.end()) continue;

            InstancedObject io;
            io.prototype = itP->second;
            io.dir       = sp.dir;
            io.heightM   = sp.heightM;
            io.scale     = sp.scale;
            io.tint      = sp.tint;
            io.yaw = Haruka::Tools::ProcGraph::WhiteNode::hashFloat((int)sp.cellSeed, 500, 0, planetSeed) * 6.2831853f;
            io.seed  = sp.cellSeed;
            io.state = sp.state;
            auto itS = stateSnap.find(io.seed);
            if (itS != stateSnap.end()) {
                io.state     = itS->second.state;
                io.regrow    = itS->second.regrow;
                io.breakMask = itS->second.breakMask;
            }
            objs.push_back(io);
        }
        // La MITAD del coste del aplicar eran las ~200k matrices (rebuildGpu, ~30 ms). El worker
        // las construye CONTRA EL ORIGEN DE LANZAMIENTO (`camPos`); el pase de color compensa
        // cada frame con `originRel = m_origin - camD`, así que el origen no necesita ser la
        // cámara actual (ya se quedaba viejo entre re-siembras).
        std::vector<Renderer::InstanceDataFloat>& gpu = out->gpu;
        gpu.resize(objs.size());
        const glm::dvec3 originToCenter = planetC - camPos;   // origin = cámara de lanzamiento
        for (size_t i = 0; i < objs.size(); ++i) {
            const auto& io = objs[i];
            gpu[i] = propInstance(io, originToCenter, planetR);
        }
        return out;
    };
}

void PropSystem::applyScatter(std::shared_ptr<ScatterOutput> outBase, const ApplyContext& ctx) {
    PropOutput& res = static_cast<PropOutput&>(*outBase);
    const glm::dvec3 planetC = ctx.planetC;
    const double     planetR = ctx.planetR;
    Core::Camera*    camera  = ctx.camera;
    Physics::PhysicsEngine* physics = ctx.physics;
    const Planet::TerrestrialPlanet* planet = ctx.planet;
    const glm::dvec3 camPos = camera ? glm::dvec3(camera->position) : glm::dvec3(planetC);
    // El scatter regenera las instancias desde CERO: el estado marcado por el juego (destruido,
    // rama arrancada) se conserva por semilla en `m_state`, que sobrevive a que el prop salga del
    // radio. Se sincroniza primero lo que haya en el registro por si algo lo toco sin pasar por
    // `breakAt`, y luego el mapa manda.
    { HARUKA_PROFILE("re-siembra.sync");   // solo guarda los NO-Alive: barrido barato
      for (const auto& io : m_registry.instances()) {
          if (io.state == (uint32_t)InstancedObjectState::Alive && io.breakMask == 0u) continue;
          m_state[io.seed] = StateDelta{ io.seed, io.state, io.breakMask, io.regrow };
      }
    }
    // UN pase en el hilo de render: filtra las BOCAS contra el VOX VIVIENTE (pueden haber cambiado
    // durante el vuelo; por eso no puede ir al worker) y cuadra el estado ACTUAL (el worker usó un
    // snapshot de lanzamiento — lo roto a mitad de vuelo no resucita). `gpu` va en paralelo: la
    // matriz NO depende del estado (solo la `breakMask` del stream), así que se re-etiqueta aquí.
    std::vector<InstancedObject> objs;
    objs.reserve(res.objs.size());
    // BUFFER PERSISTENTE para `m_gpu`: un vector nuevo por aplicar era un mmap de ~17 MB por
    // re-siembra; en el primer barrido tras el apply sus páginas estaban aún frías (TLB/cache) y
    // `model[3]` se leía a ~112 ns/instancia (~20 ms) vs ~9 ns/instancia en régimen. Al reusar
    // la capacidad (solo se agranda si el anillo crece), las páginas quedan residentes y el
    // sweep se mantiene en ~2 ms incluso el frame que aplica.
    if (m_gpu.capacity() < res.gpu.size()) m_gpu.reserve(res.gpu.size());
    m_gpu.clear();
    size_t sobreBoca = 0;
    // ⚠️ EL FILTRO DE BOCA ES ~50 MS SI SE HACE POR INSTANCIA. `surfaceCut` paga el muestreo de
    // COTA del planeta (sampleHeight, ~0,4 µs) ANTES de mirar si hay chunk, y con ~130k props lo
    // único que importa es saber dónde el resultado puede ser ≠0. `surfaceCut` sólo puede recortar
    // donde el chunk del cinturón del probe tiene `d` (lo quitado: cuevas, trazos, edición) — si `d`
    // está vacío el chunk es todo roca y devuelve 0 por construcción. Un gate que admitiera
    // "cualquier chunk cargado" seguía pagando la muestra para todos los props del disco visible en
    // el spawn (~22 ms): la malla del terreno carga chunks en todas partes aunque sean roca maciza.
    // Se colecta una vez la lista de columnas con un chunk de HUECO y `surfaceCut` solo se llama en
    // las que existen; en el resto devuelve 0 por construcción (ver `VoxWorld::loadedCutColumns`).
    const std::unordered_set<Vox::VoxKey, Vox::VoxKeyHash> voxCols = planet->vox().loadedCutColumns();
    // EL FILTRO (boca + estado) EN PARALELO: es O(N) con dos lookups por instancia (~13 ms a 178k
    // en un hilo) pero SOLO LEE (`voxCols`, `m_state`, `res.gpu`) → se parte en K trozos y cada uno
    // acumula en buffers propios; el hilo de render concatena en orden sobre el nuevo anillo. La
    // generación de matrices GPU ya corre en el worker; esto es lo único del apply que queda aquí.
    const size_t N = res.objs.size();
    const size_t K = std::min<size_t>(8u, std::max<size_t>(1u, N / 20000u));
    std::atomic<size_t> aSobreBoca(0);
    auto filtroRango = [&](size_t b, size_t e) {
        std::vector<InstancedObject> lo; lo.reserve(e - b);
        std::vector<Renderer::InstanceDataFloat> li;       li.reserve(e - b);
        for (size_t i = b; i < e; ++i) {
            InstancedObject io = res.objs[i];
            if (!voxCols.empty()) {
                const Vox::VoxKey ck = planet->vox().columnAt(glm::dvec3(io.dir));
                if (voxCols.count(ck) != 0 && planet->vox().surfaceCut(glm::dvec3(io.dir)) > 0.5f) {
                    aSobreBoca.fetch_add(1, std::memory_order_relaxed); continue;
                }
            }
            // CRÁTERES: una explosión despeja lo que había dentro (tierra desnuda), y lo que quedara
            // estaría clavado a la cota de antes, flotando sobre el cuenco. Sin cráteres no cuesta:
            // `surfaceDeltaM` sale por un atómico.
            if (planet->vox().surfaceDeltaM(glm::dvec3(io.dir)) < -0.05f) { aSobreBoca.fetch_add(1, std::memory_order_relaxed); continue; }
            if (!m_state.empty()) {
                auto it = m_state.find(io.seed);
                if (it != m_state.end()) {
                    io.state     = it->second.state;
                    io.regrow    = it->second.regrow;
                    io.breakMask = it->second.breakMask;
                }
            }
            Renderer::InstanceDataFloat inst = res.gpu[i];
            inst.breakMask = (float)io.breakMask;
            lo.push_back(std::move(io));
            li.push_back(inst);
        }
        return std::pair{ std::move(lo) , std::move(li) };
    };
    if (K > 1) {
        // `parallelFor` y no `submit` + esperar los futures: si esto corriera DENTRO de un trabajo del
        // pool, esperar a otros trabajos del mismo pool podría dejarlo sin nadie libre (todos
        // esperando). En `parallelFor` quien llama también trabaja. El orden de `parts` es el de los
        // tramos, así que el resultado sale en el mismo orden que antes.
        std::vector<std::pair<std::vector<InstancedObject>, std::vector<Renderer::InstanceDataFloat>>> parts(K);
        Haruka::Core::Jobs::jobs().parallelFor(0, K, [&](size_t ki) {
            const size_t b = N * ki / K, e = N * (ki + 1) / K;
            if (b < e) parts[ki] = filtroRango(b, e);
        }, Haruka::Core::Jobs::Priority::Frame);
        for (auto& pr : parts) {
            std::vector<InstancedObject>& lo = pr.first;
            std::vector<Renderer::InstanceDataFloat>& li = pr.second;
            m_gpu.insert(m_gpu.end(), li.begin(), li.end());   // `m_gpu` MANTIENE su capacidad persistente
            objs.insert(objs.end(), lo.begin(), lo.end());
        }
    } else {
        for (size_t i = 0; i < N; ++i) {
            InstancedObject io = res.objs[i];
            if (!voxCols.empty()) {
                const Vox::VoxKey ck = planet->vox().columnAt(glm::dvec3(io.dir));
                if (voxCols.count(ck) != 0 && planet->vox().surfaceCut(glm::dvec3(io.dir)) > 0.5f) { aSobreBoca.fetch_add(1, std::memory_order_relaxed); continue; }
            }
            if (planet->vox().surfaceDeltaM(glm::dvec3(io.dir)) < -0.05f) { aSobreBoca.fetch_add(1, std::memory_order_relaxed); continue; }   // cráteres (ver arriba)
            if (!m_state.empty()) {
                auto it = m_state.find(io.seed);
                if (it != m_state.end()) {
                    io.state     = it->second.state;
                    io.regrow    = it->second.regrow;
                    io.breakMask = it->second.breakMask;
                }
            }
            Renderer::InstanceDataFloat inst = res.gpu[i];
            inst.breakMask = (float)io.breakMask;
            objs.push_back(io);
            m_gpu.push_back(inst);
        }
    }
    sobreBoca = aSobreBoca.load(std::memory_order_relaxed);
    m_origin = res.origin;                    // las matrices del worker son relativas a ESTE origen
    { HARUKA_PROFILE("re-siembra.filtro");
      m_registry.setInstances(std::move(objs));
    }
    if (sobreBoca > 0) HARUKA_LOGDIAG("Vox", "props retirados por estar sobre una boca: %zu", sobreBoca);

    // Los props COLISIONAN: se alimenta aqui porque el scatter es quien cambia el conjunto.
    { HARUKA_PROFILE("re-siembra.colliders");
      refreshColliders(planetC, planetR, camPos, physics);
    }
}

// ── pase de color ─────────────────────────────────────────────────────────────────────────────

void PropSystem::draw(RHI::Context* ctx, const DrawFrame& f, DrawStats& stats) {
    // `HARUKA_NOPROPS=1` salta el pase (A/B de atribucion de GPU: apagar pases y restar).
    if (propsDisabled() || !m_enabled || m_registry.prototypeCount() <= 0 || !ctx || !f.camera) return;
    RHI::Device* dev = RHI::device();
    if (!dev) return;
    HARUKA_PROFILE("scene.prop.instanced"); HARUKA_GPU_SCOPE("scene.prop.instanced");

    if (!valid(m_instPSO)) {
        const std::string vs = Renderer::Shader::baseDir() + "shaders/prop_inst.vert";
        const std::string fs = Renderer::Shader::baseDir() + "shaders/prop_inst.frag";
        RHI::PipelineDesc pd;
        pd.vertexPath   = vs.c_str();
        pd.fragmentPath = fs.c_str();
        appendPropVertexLayout(pd.vertexLayout, /*withPart*/true);          // binding 0 = prototipo
        Renderer::GPUInstancing::appendInstanceLayout(pd.vertexLayout, 1);  // binding 1 = instancias
        pd.topology     = RHI::PrimitiveTopology::Triangles;
        pd.depth.test   = true;  pd.depth.write = true;
        pd.blend.enable = false;
        pd.cull         = RHI::CullMode::Back;
        m_instPSO = dev->createPipeline(pd);
    }
    if (!valid(m_instPSO)) return;

    // Viento del CLIMA (m/s, marco del observador) y reloj del balanceo: los de la base, los mismos
    // que mecen la hierba.
    const glm::vec3 windWorld = frameWind(f);
    const float     propTime  = frameTime();

    if (!m_instancing) { m_instancing = std::make_unique<Renderer::GPUInstancing>(); m_instancing->init(20000); }
    ctx->bindPipeline(m_instPSO);
    ctx->bindUniformBuffer(0, f.perFrameUBO);

    // Frustum culling con esfera contra los 4 planos laterales + CULL POR TAMAÑO EN PANTALLA
    // (sub-pixel) + LOD por RADIO EN PIXELES (no en metros: un prop grande conserva detalle mas
    // lejos, sin tablas por tipo, y sale de la misma division que el descarte sub-pixel). Los
    // umbrales los pone el controlador de carga (abajo): historicos 90 px -> nivel 0 (464 tris),
    // 25 px -> nivel 1 (130), resto -> nivel 2 (40), pero con holgura de GPU suben a nivel 0 todo
    // lo que no sea sub-pixel. El cull en sí es el de la base (`makeCull`), el mismo que la hierba.

    // ── LOD POR CARGA (no por distancia) ───────────────────────────────────────────────────────
    // En vez de tabla fija, el detalle lo decide la CARGA del pase: con holgura todo lo que no sea
    // sub-pixel se dibuja a nivel 0 (la distancia NO limita); si el pase se pasa del presupuesto,
    // los umbrales en px suben y pierde nivel PRIMERO lo mas pequeno en pantalla (= lo mas lejano),
    // que es lo que menos se nota. La distancia SOLO ordena el gasto; cuantos ms se gastan lo pone
    // este controlador sobre el scope del frame anterior.
    //
    // Senal: ms del scope `scene.prop.instanced` (solo Vulkan: `RHI::gpuScopes()` trae la medicion
    // resuelta del frame ANTERIOR; GL no tiene timestamps y vuelve vacia → nos quedamos en fijo).
    // Control: EMA (~1 s) + paso cada 0.5 s + zona muerta, para que no patalée (el popping global
    // del que huimos). Prioridad: SOBRE presupuesto → baja el detalle (pierde nivel primero lo mas
    // pequeno en pantalla = lo mas lejano; el tramo cercano esta protegido) y el radio solo se
    // recorta como ultimo recurso; HOLGURA → sube el detalle hasta 1.0 y, ya a tope, DIBUJA MAS:
    // extiende el radio (re-siembra con celdas mas gruesas, recuento log). Nunca se pasa del
    // presupuesto: es la definicion de "no saturar la GPU".
    // `HARUKA_PROP_LOAD=0` lo apaga (comportamiento historico) · `HARUKA_PROP_BUDGET_MS=<n>`
    // presupuesto del pase · `HARUKA_PROP_BUDGET_KI=<n>` paso por unidad de error ·
    // `HARUKA_PROP_DETAIL=<0..1000>` fuerza el detalle (A/B; 1000 = todo nivel 0) ·
    // `HARUKA_PROP_DETAIL_MIN=<permil>` suelo del detalle por debajo del historico (def. -1000):
    // cuando ni 90/25 px alcanza el presupuesto, baja mas (umbrales mas altos) hasta llegar ·
    // `HARUKA_PROP_RING=<m>` techo del radio (<=6000 = bloqueado al piso historico).
    static const bool   s_loadEnv    = [] { const char* e = std::getenv("HARUKA_PROP_LOAD");      return !(e && e[0] == '0'); }();
    static const double s_budgetMs   = [] { const char* e = std::getenv("HARUKA_PROP_BUDGET_MS"); return e ? std::atof(e) : kPropBudgetMs; }();
    static const double s_loadKi     = [] { const char* e = std::getenv("HARUKA_PROP_BUDGET_KI"); return e ? std::atof(e) : kLoadKi; }();
    static const int    s_loadDetail = [] { const char* e = std::getenv("HARUKA_PROP_DETAIL");    return e ? std::atoi(e) : -1; }();
    static const float  s_detailMin  = [] { const char* e = std::getenv("HARUKA_PROP_DETAIL_MIN"); return e ? (float)std::atoi(e) / 1000.0f : kDetailMin; }();
    static const float  s_ringMax    = [] { const char* e = std::getenv("HARUKA_PROP_RING");      return e ? std::max(kRingFloorM, (float)std::atoi(e)) : kRingMaxM; }();
    static double       s_lastRingOp = -1e18;   // reloj diag del ultimo movimiento de radio
    if (s_loadEnv) {
        double propMs = 0.0;
        if (RHI::Device* dev = RHI::device())
            for (const auto& s : dev->gpuScopes())
                if (s.name == kPropScope) { propMs = s.ms; break; }
        if (propMs > 0.0) {
            m_loadDriven = true;
            // semilla: el primer muestreo ES el estado, y el detalle arranca en EQUILIBRIO
            // (budget/lectura) — no desde 1.0. "Todo a nivel 0" el primer frame cargado-de-GPU
            // seria un pico de triangulos de entrada que el controlador luego pagaría en popping,
            // justo lo que intentamos evitar con la rampa lenta.
            if (m_propMsSmooth <= 0.0) {
                m_propMsSmooth = propMs;
                m_propDetail   = (float)std::clamp(s_budgetMs / std::max(propMs, 1e-3), (double)s_detailMin, 1.0);
            } else {
                m_propMsSmooth += (propMs - m_propMsSmooth) * 0.05;          // tau≈1 s a 60 fps
            }
            if (f.diagClock - m_lastLoadStep >= kLoadStepS) {
                m_lastLoadStep = f.diagClock;
                const double err = (m_propMsSmooth - s_budgetMs) / s_budgetMs;   // >0 = sobre presupuesto
                if (err > 0.06) {
                    // SOBRE presupuesto → bajar el DETALLE: pierde nivel primero lo mas pequeno en
                    // pantalla (= lo mas lejano) y lo cercano esta protegido por la garantia
                    // LOD0. Es el mango de la GPU: el radio NO se recorta aqui — los props lejanos
                    // ya son sub-pixel (no se dibujan) y una re-siembra de recorte añadiria un
                    // refundido justo en el frame saturado. Solo como ULTIMO recurso (detalle ya en
                    // el suelo y aun sobre) se recorta el radio, lento, para aliviar el barrido.
                    m_propDetail = std::clamp(m_propDetail - (float)(s_loadKi * err), s_detailMin, 1.0f);
                    if (m_propDetail <= s_detailMin && m_ringM > kRingFloorM &&
                        f.diagClock - s_lastRingOp >= kRingPaceS) {
                        s_lastRingOp = f.diagClock;
                        m_rescatter  = true;
                        m_ringM      = std::max(kRingFloorM, m_ringM - kRingStepM);
                    }
                } else if (err < -0.06) {
                    // HOLGURA → sube el detalle hasta el maximo.
                    m_propDetail = std::clamp(m_propDetail - (float)(s_loadKi * err), s_detailMin, 1.0f);
                }
                // ── RADIO: DESACOPLADO DEL DETALLE ──
                // Las bandas lejanas son casi exclusivamente sub-pixel (el cull por tamaño ni las
                // dibuja) o LOD2 de 40 tris: coste GPU ~nulo. Que la GPU este saturada por el tramo
                // CERCANO (y el detalle bajando) NO debe negar "dibuja mas de lejos": el radio se
                // estira hasta su techo a su propio ritmo — el coste real del radio es CPU (barrido
                // O(N) + arena), y lo limite la cota `maxProps` del scatter. Solo se recorta como
                // ultimo recurso (arriba), cuando ni en el suelo de detalle se llega al presupuesto.
                if (m_ringM < s_ringMax && f.diagClock - s_lastRingOp >= kRingPaceS) {
                    s_lastRingOp = f.diagClock;
                    m_rescatter  = true;
                    m_ringM      = std::min(s_ringMax, m_ringM + kRingStepM);
                }
            }
        } else {
            m_loadDriven = false;   // sin medicion (GL): umbrales fijos de siempre
        }
    }
    if (s_loadDetail >= 0) m_propDetail = (float)s_loadDetail / 1000.0f;
    const float kLodPx0 = m_loadDriven ? (1.0f - m_propDetail) * 90.0f : 90.0f;
    const float kLodPx1 = m_loadDriven ? (1.0f - m_propDetail) * 25.0f : 25.0f;
    // /!\\ con detalle 1.0 los umbrales son 0 px: kLod0 = kPixPerM/0 = +inf y todo lo que paso el
    // cull sub-pixel (>1 px) cumple dist^2 <= radio^2 * inf → nivel 0. Ahi no hay division por cero
    // en el cull (el sub-pixel ya se descarto antes); solo flota a inf, que es exactamente el LOD0.
    static double s_lastLoadLog = -1e18;
    if (f.diagClock - s_lastLoadLog >= 2.0) {
        s_lastLoadLog = f.diagClock;
        HARUKA_LOGDIAG("PropLoad", "ms=%.2f (budget %.1f) detalle=%.2f umbral-px=%d/%d anillo=%dm%s",
                       m_propMsSmooth, s_budgetMs, m_loadDriven ? (double)m_propDetail : -1.0,
                       (int)std::lround(kLodPx0), (int)std::lround(kLodPx1),
                       (int)std::lround(m_ringM), m_loadDriven ? "" : " [fijo: sin timestamps]");
    }
    // Culling en ESPACIO AL CUADRADO: px = (radio/dist)/tanV*vpPx, y las comparaciones
    // (sub-pixel (1 px) y LOD) se reescriben como dist^2 comparado con (radio * k)^2. Con
    // 178k instancias en el registro, el `sqrt` del length y la division del px por instancia
    // eran el grueso del barrido (~22 ms en el frame tras la re-siembra).
    const Cull propCull = makeCull(f, kLodPx0, kLodPx1);

    const glm::dvec3 camD = glm::dvec3(f.camera->position);
    glm::dvec3 planetC; double planetR = 6371000.0;
    if (!(f.planets && f.planets->getActivePlanet(planetC, planetR))) planetC = glm::dvec3(0.0);
    const int protoCount = m_registry.prototypeCount();
    const int kLods = PrototypeGpu::kLods;
    // Radio de cull por PROTOTIPO (a escala 1): constante por nombre (`propCullRadiusM`), así que es
    // reproducible entre jugadores — la roca aguanta hasta el borde del anillo, el cúmulo usa su
    // huella, y el árbol se queda en su ~1 px natural. Un barrido antes del bucle: protoCount ≈ decenas.
    std::vector<float> cullRadius((size_t)protoCount, 8.0f);
    for (int pi = 0; pi < protoCount; ++pi)
        cullRadius[(size_t)pi] = propCullRadiusM(m_registry.prototype(pi).name);

    // Buckets por (prototipo, nivel), reusados entre frames: UN barrido O(N) del registro (antes
    // era O(prototipos × instancias) y era el grueso de los 39 ms del pase).
    if ((int)m_buckets.size() != protoCount * kLods) {
        m_buckets.resize((size_t)(protoCount * kLods));
        m_aliveCounts.resize((size_t)protoCount);
        m_drawnCounts.resize((size_t)protoCount);
    }
    for (int pi = 0; pi < protoCount; ++pi) {
        for (int l = 0; l < kLods; ++l) m_buckets[(size_t)(pi * kLods + l)].clear();
        m_aliveCounts[(size_t)pi] = 0;
        m_drawnCounts[(size_t)pi] = 0;
    }

    // Snapshot de debug para la jerarquia del editor, cada 10 frames (~6 Hz): 40k entradas por
    // frame costaban ~5 ms y es una vista de debug.
    static int s_dbgFrame = 0;
    const bool snapshotDbg = m_debugEnabled && (++s_dbgFrame % 10 == 1);
    if (snapshotDbg) {
        m_scatterDebug.clear();
        m_scatterDebug.reserve((size_t)protoCount);
        for (int pi = 0; pi < protoCount; ++pi) {
            PropPrototypeDebug dbg; dbg.name = m_registry.prototype(pi).name;
            m_scatterDebug.push_back(std::move(dbg));
        }
    }

    const int bufCap = m_instancing->getMaxInstances();
    std::vector<int> aliveSeenPerProto((size_t)protoCount, 0);
    if (m_gpu.size() != m_registry.instances().size()) rebuildGpu(planetC, planetR, f.camera);
    // El arena de instancias se reserva UNA vez por frame con el tope del frame: el pase principal y el
    // de sombras comparten este instancer y cada uno sub-asigna lo suyo, o sea 2× el anillo como
    // tope duro. Sin margen, la acumulacion cruzaba el tope tras cada re-siembra y `upload` recreaba
    // el arena (hasta 157..671 ms). El +64k extra es relleno para que el segundo pase no roce el borde.
    // TOPE DURO ADAPTATIVO: se fija al MÁXIMO HISTÓRICO del anillo, no al tamaño del frame actual.
    // El primer frame no vale (el registro aún está vacío y congelar 2×0+256k hacía que el arena se
    // llenara al llegar el scatter y RECORTAR los props: "faltan las rocas y los árboles"). Cada
    // re-siembra que agranda el anillo mueve el tope hacia arriba — los reallocs solo pasan en
    // frames de gen (gigantescos de por sí); en juego estable el tope queda fijo y `upload` no
    // vuelve a llamar a vkAllocateMemory (~110 ms c/u en este portátil híbrido).
    const size_t ringNow = m_registry.instances().size();
    if (ringNow > m_arenaHiRing) { m_arenaHiRing = ringNow; m_instancing->setHardCap(m_arenaHiRing * 2 + 262144); }
    m_instancing->reserve(m_registry.instances().size() * 2 + 65536);
    const glm::vec3 originRel = glm::vec3(m_origin - camD);

    // SONDA: cuanto cuesta el barrido (cullar + agrupar), medida cada 2 s.
    using PClock = std::chrono::steady_clock;
    const auto tSweep0 = PClock::now();
    size_t nSwept = 0, nKept = 0;
    const auto& insts = m_registry.instances();
    const size_t nInst = insts.size();
    for (uint32_t ii = 0; ii < (uint32_t)nInst; ++ii) {
        // Prefetch a 8 instancias de `m_gpu` y del registro: en el primer barrido tras el apply
        // las páginas están frías y la latencia de cada `model[3]`/`io` dominaba (~20 ms).
        if (ii + 8 < nInst) {
            __builtin_prefetch(&m_gpu[ii + 8], 0, 3);
            __builtin_prefetch(&insts[ii + 8], 0, 3);
        }
        const auto& io = insts[ii];
        ++nSwept;
        const int pi = io.prototype;
        if (pi < 0 || pi >= protoCount) continue;
        const bool alive = io.state == (uint32_t)InstancedObjectState::Alive;
        uint8_t cull = 0;
        if (alive) {
            ++m_aliveCounts[(size_t)pi];
            const glm::vec3 posF = m_gpu[ii].position + originRel;
            int lod = 0;
            cull = propCull(posF, io.scale * cullRadius[(size_t)pi], lod);
            if (cull == 0) { m_buckets[(size_t)(pi * kLods + lod)].push_back(ii); ++nKept; }
        }
        if (snapshotDbg) {
            PropPrototypeDebug& dbg = m_scatterDebug[(size_t)pi];
            ++dbg.totalInstances;
            PropInstanceDebug idbg;
            idbg.seed = io.seed; idbg.state = io.state; idbg.cullReason = cull;
            idbg.culled   = alive && cull != 0;
            idbg.rendered = alive && cull == 0 && aliveSeenPerProto[(size_t)pi] < bufCap;
            if (idbg.rendered) ++dbg.renderedInstances;
            if (alive && cull == 0) ++aliveSeenPerProto[(size_t)pi];
            dbg.instances.push_back(idbg);
        }
    }
    {
        static double s_lastLog = -1e9;
        const double ms = std::chrono::duration<double, std::milli>(PClock::now() - tSweep0).count();
        if (f.diagClock - s_lastLog > 2.0) {
            s_lastLog = f.diagClock;
            HARUKA_LOGDIAG("PropCost", "barrido+cull+matrices: %.2f ms para %zu instancias -> %zu dibujadas (%.0f ns/instancia)",
                           ms, nSwept, nKept, nSwept ? ms * 1e6 / (double)nSwept : 0.0);
        }
    }

    for (int pi = 0; pi < protoCount; ++pi) {
        const InstancedPrototype& proto = m_registry.prototype(pi);
        if (proto.name.empty()) continue;
        const PrototypeGpu* pgp = prototypeGpu(proto);
        if (!pgp) continue;
        const PrototypeGpu& pg = *pgp;
        if (!valid(pg.vbo[0]) || pg.indexCount[0] == 0) continue;

        // Per-pixel REAL del prototipo: se marca ANTES del bucket vacio (un prototipo sin instancias
        // visibles este frame no deja de tener material per-pixel en la jerarquia del editor).
        if (snapshotDbg) m_scatterDebug[(size_t)pi].hasPerPixel = pg.mask != 0u;

        // Stats TOTALES (sin cull) con la malla de nivel 0: el numero contra el que comparar el LOD.
        const int aliveN = m_aliveCounts[(size_t)pi];
        stats.totalVertices  += (int)pg.vertexCount[0] * aliveN;
        stats.totalTriangles += (int)(pg.indexCount[0] / 3) * aliveN;
        ++stats.totalDrawCalls;

        const PropParams pp = makeParams(pg, windWorld, propTime, originRel, f);

        // UN DRAW POR NIVEL: el instancing sigue intacto y lo que baja es la geometria por instancia.
        int drawnTotal = 0;
        for (int lod = 0; lod < kLods; ++lod) {
            auto& bucket = m_buckets[(size_t)(pi * kLods + lod)];
            if (bucket.empty()) continue;
            if (!valid(pg.vbo[lod]) || pg.indexCount[lod] == 0) continue;
            { HARUKA_PROFILE("prop.gather"); m_instancing->setInstancesGather(m_gpu.data(), bucket); }
            const int drawnN = m_instancing->getInstanceCount();
            drawnTotal += drawnN;
            stats.renderedVertices  += (int)pg.vertexCount[lod] * drawnN;
            stats.renderedTriangles += (int)(pg.indexCount[lod] / 3) * drawnN;

            ctx->bindPipeline(m_instPSO);   // re-bind (creacion perezosa de buffers)
            ctx->bindVertexBuffer(pg.vbo[lod], 0);
            ctx->bindIndexBuffer(pg.ebo[lod]);
            bindPrototypeMaterial(ctx, pg);
            const RHI::BufferHandle ppUbo = paramsUBO(dev, pi);
            if (valid(ppUbo)) {
                HARUKA_PROFILE("prop.ubo.update");
                dev->updateBuffer(ppUbo, 0, sizeof(pp), &pp);
                ctx->bindUniformBuffer(6, ppUbo);
            }
            { HARUKA_PROFILE("prop.upload+draw"); m_instancing->render(ctx, pg.indexCount[lod], 1); }
            ++stats.renderedDrawCalls;
        }
        m_drawnCounts[(size_t)pi] = drawnTotal;
    }

    // Diagnóstico no-diag (protegido por s_logProps cada ~2 s): tamaño REAL del registro y lotes
    // subidos. Despeja la duda del log del tope (¿el arena se queda corto o hay MÁS registro?).
    {
        static double s_lastLog2 = -1e9;
        if (f.diagClock - s_lastLog2 > 2.0) {
            s_lastLog2 = f.diagClock;
            size_t noVacios = 0;
            for (int li = 0; li < protoCount * kLods; ++li)
                if (!m_buckets[(size_t)li].empty()) ++noVacios;
            HARUKA_LOGI("PropInst",
                        "registro=%zu (arena hi-ring=%zu) · protoCount=%d · buckets no vacios=%zu · lotes subidos=%d · arena %zu/%zu",
                        m_registry.instances().size(), m_arenaHiRing, protoCount, noVacios,
                        m_instancing->ringSize(), m_instancing->arenaUsed(), m_instancing->arenaCap());
        }
    }

    // Diagnostico cada 120 frames: por prototipo vivos / en bucket / dibujados, y la instancia de
    // nivel 0 mas cercana (distingue "esta a 300 m" de "mide 2 cm" de "esta bajo tierra").
    {
        static int s_diagFrame = 0;
        if (++s_diagFrame % 120 == 1 && Core::diagLogs()) {
            std::string line; char tmp[96];
            for (int di = 0; di < protoCount; ++di) {
                std::snprintf(tmp, sizeof(tmp), " p%d:%d/[%d+%d+%d]/%d", di, m_aliveCounts[(size_t)di],
                              (int)m_buckets[(size_t)(di * kLods + 0)].size(), (int)m_buckets[(size_t)(di * kLods + 1)].size(),
                              (int)m_buckets[(size_t)(di * kLods + 2)].size(), m_drawnCounts[(size_t)di]);
                line += tmp;
            }
            HARUKA_LOGDIAG("PropDiag", "protoCount=%d instances=%zu%s", protoCount, m_registry.instances().size(), line.c_str());
            for (int di = 0; di < protoCount; ++di) {
                const auto& b0 = m_buckets[(size_t)(di * kLods + 0)];
                if (b0.empty()) continue;
                float best = 1e30f; glm::vec3 bp(0.0f); float bs = 0.0f;
                for (const uint32_t bi : b0) {
                    const glm::vec3 pp3 = m_gpu[bi].position + originRel;
                    const float d = glm::length(pp3);
                    if (d < best) { best = d; bp = pp3; bs = m_gpu[bi].scale.x; }
                }
                auto itM = m_protoMesh.find(m_registry.prototype(di).name);
                HARUKA_LOGDIAG("PropDiag", "  %s: nivel0 mas cercano a %.1f m (cam-rel %.1f %.1f %.1f · escala %.2f) · vbo0 verts %u idx %u",
                    m_registry.prototype(di).name.c_str(), best, bp.x, bp.y, bp.z, bs,
                    itM != m_protoMesh.end() ? itM->second.vertexCount[0] : 0u,
                    itM != m_protoMesh.end() ? itM->second.indexCount[0] : 0u);
            }
        }
    }
    if (valid(f.scenePSO)) ctx->bindPipeline(f.scenePSO);   // restaura el PSO de escena (el cierre lo asume)
}

// ── sombras ───────────────────────────────────────────────────────────────────────────────────

void PropSystem::drawShadows(RHI::Context* ctx, const glm::mat4& lightSpace, Core::Camera* camera,
                             Planet::PlanetarySystem* planets) {
    if (!ctx || !camera || !m_instancing) return;
    const int protoCount = m_registry.prototypeCount();
    if (protoCount <= 0 || m_registry.instances().empty()) return;
    RHI::Device* dev = RHI::device();
    if (!dev) return;

    // PSO de solo profundidad, con el MISMO layout de instancia que el pase de color.
    if (!valid(m_shadowPSO)) {
        const std::string vs = Renderer::Shader::baseDir() + "shaders/prop_depth_inst.vert";
        const std::string fs = Renderer::Shader::baseDir() + "shaders/depth_only.frag";
        RHI::PipelineDesc pd;
        pd.vertexPath   = vs.c_str();
        pd.fragmentPath = fs.c_str();
        pd.vertexLayout.strides = { (uint32_t)sizeof(PropVertex) };
        pd.vertexLayout.attributes = {
            { 0, (uint32_t)offsetof(PropVertex, pos), RHI::Format::RGB32F, 0 },
            // La parte hace falta TAMBIEN aqui: sin ella una rama arrancada seguiria proyectando.
            { 11, (uint32_t)offsetof(PropVertex, partId), RHI::Format::R32F, 0 },
        };
        Renderer::GPUInstancing::appendInstanceLayout(pd.vertexLayout, 1);
        pd.topology     = RHI::PrimitiveTopology::Triangles;
        pd.depth.test   = true;  pd.depth.write = true;
        pd.blend.enable = false;
        // SIN back-face culling: un tronco de una sola capa dejaria de proyectar por la mitad.
        pd.cull         = RHI::CullMode::None;
        m_shadowPSO = dev->createPipeline(pd);
        HARUKA_LOGI("PropShadow", "pipeline de profundidad instanciada: %s",
                    valid(m_shadowPSO) ? "ok" : "FALLO (los props no proyectan sombra)");
    }
    if (!valid(m_shadowPSO)) return;

    glm::dvec3 planetC(0.0); double planetR = 0.0;
    if (!planets || !planets->getActivePlanet(planetC, planetR)) return;
    const glm::dvec3 camD = glm::dvec3(camera->position);
    if (m_gpu.size() != m_registry.instances().size()) rebuildGpu(planetC, planetR, camera);
    // Ídem del pase principal: el tope adaptativo se actualiza ANTES de cualquier reserva.
    const size_t ringNow = m_registry.instances().size();
    if (ringNow > m_arenaHiRing) { m_arenaHiRing = ringNow; m_instancing->setHardCap(m_arenaHiRing * 2 + 262144); }
    m_instancing->reserve(m_registry.instances().size() * 2 + 65536);
    const glm::vec3 originRel = glm::vec3(m_origin - camD);

    if (!valid(m_shadowUBO))
        m_shadowUBO = dev->createBuffer(RHI::BufferUsage::Uniform, sizeof(PropShadowUBO), nullptr, RHI::BufferMemory::Dynamic);
    PropShadowUBO su; su.lightSpace = lightSpace; su.originRel = glm::vec4(originRel, 0.0f);
    dev->updateBuffer(m_shadowUBO, 0, sizeof(su), &su);

    // Caja de sombra ±42 m con margen 1,6×: un prop justo fuera puede proyectar dentro.
    const float kShadowBoxM = 42.0f * 1.6f;
    const float kBox2 = kShadowBoxM * kShadowBoxM;

    ctx->bindPipeline(m_shadowPSO);
    ctx->bindUniformBuffer(0, m_shadowUBO);

    m_shadowBuckets.resize((size_t)protoCount);
    for (auto& b : m_shadowBuckets) b.clear();
    const auto& insts = m_registry.instances();
    const size_t nInstS = insts.size();
    for (uint32_t ii = 0; ii < (uint32_t)nInstS; ++ii) {
        if (ii + 8 < nInstS) {
            __builtin_prefetch(&m_gpu[ii + 8], 0, 3);
            __builtin_prefetch(&insts[ii + 8], 0, 3);
        }
        const auto& io = insts[ii];
        if (io.prototype < 0 || io.prototype >= protoCount) continue;
        if (io.state != (uint32_t)InstancedObjectState::Alive) continue;
        const glm::vec3 posF = m_gpu[ii].position + originRel;
        if (glm::dot(posF, posF) > kBox2) continue;      // fuera de la caja de la luz
        m_shadowBuckets[(size_t)io.prototype].push_back(ii);
    }
    for (int pi = 0; pi < protoCount; ++pi) {
        const InstancedPrototype& proto = m_registry.prototype(pi);
        if (proto.name.empty()) continue;
        auto itMesh = m_protoMesh.find(proto.name);
        if (itMesh == m_protoMesh.end()) continue;      // malla aun sin hornear
        const PrototypeGpu& pg = itMesh->second;
        // LAS SOMBRAS USAN UN NIVEL FIJO, el intermedio: solo aporta la silueta y todo esta cerca.
        const int kShadowLod = 1;
        const int sl = (pg.indexCount[kShadowLod] > 0 && valid(pg.vbo[kShadowLod])) ? kShadowLod : 0;
        if (pg.indexCount[sl] == 0 || !valid(pg.vbo[sl]) || !valid(pg.ebo[sl])) continue;
        const auto& bucket = m_shadowBuckets[(size_t)pi];
        if (bucket.empty()) continue;
        m_instancing->setInstancesGather(m_gpu.data(), bucket);
        ctx->bindVertexBuffer(pg.vbo[sl], 0);
        ctx->bindIndexBuffer(pg.ebo[sl]);
        m_instancing->render(ctx, pg.indexCount[sl], 1);
    }
}

// ── estado y rotura ───────────────────────────────────────────────────────────────────────────

std::vector<PropSystem::StateDelta> PropSystem::serializeState() const {
    std::vector<StateDelta> out;
    out.reserve(m_state.size());
    for (const auto& [seed, st] : m_state) out.push_back(st);
    return out;
}

void PropSystem::restoreState(const std::vector<StateDelta>& deltas) {
    m_state.clear();
    for (const auto& d : deltas) m_state[d.seed] = d;
}

PropSystem::Hit PropSystem::breakAt(const glm::dvec3& center, double radius, Core::Camera* camera,
                                    Planet::PlanetarySystem* planets, Physics::PhysicsEngine* physics) {
    Hit best;
    if (!planets) return best;
    glm::dvec3 planetC; double planetR = 6371000.0;
    if (!planets->getActivePlanet(planetC, planetR)) return best;
    const int protoCount = m_registry.prototypeCount();
    if (protoCount == 0) return best;

    // Distancia de un punto a una caja orientada: la misma prueba que hace la fisica, no una
    // esfera envolvente (un tronco es mucho mas alto que ancho).
    auto distToOBB = [](const PropWorldOBB& o, const glm::dvec3& p) {
        const glm::dvec3 d = p - o.center;
        glm::dvec3 l(glm::dot(d, glm::dvec3(o.rot[0])), glm::dot(d, glm::dvec3(o.rot[1])), glm::dot(d, glm::dvec3(o.rot[2])));
        const glm::dvec3 c = glm::clamp(l, -o.halfExtents, o.halfExtents);
        return glm::length(l - c);
    };
    std::vector<std::vector<PropColliderPart>> protoParts((size_t)protoCount);
    for (int pi = 0; pi < protoCount; ++pi)
        protoParts[(size_t)pi] = propColliderParts(m_registry.prototype(pi).name, m_registry.prototype(pi).meshSeed);

    // El punto MAS CERCANO, no el primero que toque: rama y tronco se solapan en la axila.
    double bestD = radius;
    size_t bestIdx = (size_t)-1;
    const auto& insts = m_registry.instances();
    for (size_t ii = 0; ii < insts.size(); ++ii) {
        const auto& io = insts[ii];
        if (io.state != (uint32_t)InstancedObjectState::Alive) continue;
        if (io.prototype < 0 || io.prototype >= protoCount) continue;
        const glm::dvec3 wp = planetC + glm::dvec3(io.dir) * (planetR + (double)io.heightM);
        const double reach = radius + 12.0 * (double)io.scale;   // alto maximo razonable de un prop
        if (glm::length(wp - center) > reach) continue;
        const auto boxes = propWorldColliders(protoParts[(size_t)io.prototype], io.dir, io.heightM,
                                                              io.scale, io.yaw, planetC, planetR, io.breakMask);
        for (const auto& b : boxes) {
            if (!b.breakable) continue;
            const double d = distToOBB(b, center);
            if (d >= bestD) continue;
            bestD = d; bestIdx = ii;
            best.hit = true; best.seed = io.seed;
            best.prototype = m_registry.prototype(io.prototype).name;
            best.partId = b.partId; best.trunk = (b.partId == 0);
            best.lengthM = b.length; best.rBottomM = b.rBottom; best.rTopM = b.rTop;
            best.pos = b.center;
        }
    }
    if (!best.hit || bestIdx == (size_t)-1) return best;

    // El tronco se lleva el arbol entero; una rama solo pone su bit y el arbol sigue en pie.
    if (best.trunk) {
        m_registry.setStateBySeed(best.seed, InstancedObjectState::Destroyed);
    } else if (best.partId >= 0 && best.partId < 32) {
        m_registry.setBreakBitBySeed(best.seed, (uint32_t)best.partId);
        if (bestIdx < m_gpu.size()) m_gpu[bestIdx].breakMask = (float)m_registry.instances()[bestIdx].breakMask;
    }
    StateDelta& st = m_state[best.seed];
    st.seed = best.seed;
    if (best.trunk) st.state = (uint32_t)InstancedObjectState::Destroyed;
    else if (best.partId >= 0 && best.partId < 32) st.breakMask |= (1u << (uint32_t)best.partId);

    // Un prop DERRIBADO tumba la hierba a su alrededor: lo que cae aplasta donde cae. La hierba es
    // de OTRO sistema (`GrassSystem`): aquí solo se avisa (`onGroundImpact`) y la app lo conecta.
    // Esto cubre arboles, rocas y cumulos al PARTIRSE en el sitio. Solo el derrumbe completo
    // (tronco, caja, cumulo): arrancar una rama no tumba nada al suelo.
    if (best.trunk) {
        const float sc = (bestIdx < insts.size()) ? insts[bestIdx].scale : 1.0f;
        // Un cumulo arrasa su huella; un arbol/roca, ~4 radios del fuste (lo que cae).
        const float r = isClumpProto(best.prototype)
                        ? std::min(35.0f, 13.0f * sc)
                        : std::min(8.0f, std::max(0.6f, (float)best.rBottomM * 4.0f));
        m_planetC = planetC; m_planetR = planetR;
        if (onGroundImpact) onGroundImpact(best.pos, r);
    }

    // Los colliders se rehacen ya: si no, seguirias chocando con la rama que acabas de arrancar.
    refreshColliders(planetC, planetR, camera ? glm::dvec3(camera->position) : glm::dvec3(0.0), physics);
    return best;
}

std::vector<PropSystem::NearPart> PropSystem::partsNear(const glm::dvec3& center, double radius,
                                                        Planet::PlanetarySystem* planets) const {
    std::vector<NearPart> out;
    if (!planets) return out;
    glm::dvec3 planetC; double planetR = 6371000.0;
    if (!planets->getActivePlanet(planetC, planetR)) return out;
    const int protoCount = m_registry.prototypeCount();
    if (protoCount == 0) return out;
    std::unordered_map<int, std::vector<PropColliderPart>> protoParts;
    const auto& insts = m_registry.instances();
    for (const auto& io : insts) {
        if (io.state != (uint32_t)InstancedObjectState::Alive) continue;
        if (io.prototype < 0 || io.prototype >= protoCount) continue;
        const glm::dvec3 wp = planetC + glm::dvec3(io.dir) * (planetR + (double)io.heightM);
        if (glm::length(wp - center) > radius + 12.0 * (double)io.scale) continue;
        auto it = protoParts.find(io.prototype);
        if (it == protoParts.end())
            it = protoParts.emplace(io.prototype, propColliderParts(
                     m_registry.prototype(io.prototype).name, m_registry.prototype(io.prototype).meshSeed)).first;
        for (const auto& b : propWorldColliders(it->second, io.dir, io.heightM, io.scale, io.yaw,
                                                                planetC, planetR, io.breakMask)) {
            if (!b.breakable) continue;
            NearPart np;
            np.seed = io.seed; np.prototype = m_registry.prototype(io.prototype).name;
            np.partId = b.partId; np.trunk = (b.partId == 0); np.box = b;
            out.push_back(std::move(np));
        }
    }
    return out;
}

bool PropSystem::breakPart(uint32_t seed, int partId, bool trunk, const glm::dvec3& pos,
                           const std::string& prototype, double rBottomM, Planet::PlanetarySystem* planets) {
    // Lo MISMO que hace `breakAt` al encontrar la parte, sin buscarla ni rehacer colliders.
    if (trunk) {
        m_registry.setStateBySeed(seed, InstancedObjectState::Destroyed);
    } else if (partId >= 0 && partId < 32) {
        m_registry.setBreakBitBySeed(seed, (uint32_t)partId);
        const auto& insts = m_registry.instances();
        for (size_t i = 0; i < insts.size() && i < m_gpu.size(); ++i)
            if (insts[i].seed == seed) { m_gpu[i].breakMask = (float)insts[i].breakMask; break; }
    } else {
        return false;
    }
    StateDelta& st = m_state[seed];
    st.seed = seed;
    if (trunk) st.state = (uint32_t)InstancedObjectState::Destroyed;
    else       st.breakMask |= (1u << (uint32_t)partId);
    if (trunk && planets) {
        glm::dvec3 pc; double pr = 6371000.0;
        if (planets->getActivePlanet(pc, pr)) {
            m_planetC = pc; m_planetR = pr;
            const float r = isClumpProto(prototype) ? 20.0f
                                                                    : std::min(8.0f, std::max(0.6f, (float)rBottomM * 4.0f));
            if (onGroundImpact) onGroundImpact(pos, r);
        }
    }
    return true;
}

void PropSystem::refreshCollidersAround(Core::Camera* camera, Planet::PlanetarySystem* planets,
                                        Physics::PhysicsEngine* physics) {
    glm::dvec3 pc; double pr = 6371000.0;
    if (!planets || !planets->getActivePlanet(pc, pr)) return;
    refreshColliders(pc, pr, camera ? glm::dvec3(camera->position) : glm::dvec3(0.0), physics);
}

// ── colliders ─────────────────────────────────────────────────────────────────────────────────

// MALLAS DE COLISION DE LOS PROTOTIPOS: la geometria REAL del prop, partida por parte, registrada
// UNA vez por (prototipo, parte). Cada parte es un cuerpo aparte, y la rota simplemente no se
// coloca. La COPA no genera colision (material 1 = follaje): se filtra por material, no por parte.
const std::vector<int>& PropSystem::meshShapesFor(int protoIdx, Physics::PhysicsEngine* phys) {
    auto it = m_meshShapes.find(protoIdx);
    if (it != m_meshShapes.end()) return it->second;
    std::vector<int>& ids = m_meshShapes[protoIdx];
#ifdef HARUKA_MOD_PHYSICS
    if (!phys || protoIdx < 0 || protoIdx >= m_registry.prototypeCount()) return ids;
    const auto& proto = m_registry.prototype(protoIdx);
    // A DETALLE MAXIMO: la colision no depende del LOD.
    const Haruka::Tools::ProcGraph::TreeMeshData tm = bakePrototypeMesh(proto, 1.0f);
    if (tm.positions.empty() || tm.indices.empty()) return ids;

    int maxPart = 0;
    for (unsigned char p : tm.partId) maxPart = std::max(maxPart, (int)p);
    const int nParts = tm.partId.empty() ? 1 : (maxPart + 1);
    ids.assign((size_t)nParts, -1);

    std::vector<float> verts; std::vector<uint32_t> idx;
    for (int part = 0; part < nParts; ++part) {
        verts.clear(); idx.clear();
        std::vector<int> remap(tm.positions.size(), -1);
        for (size_t t = 0; t + 2 < tm.indices.size(); t += 3) {
            const unsigned i0 = tm.indices[t], i1 = tm.indices[t + 1], i2 = tm.indices[t + 2];
            if (!tm.partId.empty() && (int)tm.partId[i0] != part) continue;
            if (!tm.materialId.empty() && tm.materialId[i0] != 0) continue;   // solo corteza
            for (unsigned vi : { i0, i1, i2 }) {
                if (remap[vi] < 0) {
                    remap[vi] = (int)(verts.size() / 3);
                    verts.push_back(tm.positions[vi].x); verts.push_back(tm.positions[vi].y); verts.push_back(tm.positions[vi].z);
                }
                idx.push_back((uint32_t)remap[vi]);
            }
        }
        if (idx.size() < 3) continue;
        ids[(size_t)part] = phys->registerPropMesh(verts.data(), verts.size() / 3, idx.data(), idx.size());
        // Copia en CPU para el alambre: los mismos triangulos, en el mismo marco local.
        auto& cpu = m_meshCpu[protoIdx];
        if ((int)cpu.size() <= part) cpu.resize((size_t)part + 1);
        cpu[(size_t)part].clear();
        cpu[(size_t)part].reserve(idx.size());
        for (uint32_t vi : idx) cpu[(size_t)part].push_back(glm::vec3(verts[vi * 3 + 0], verts[vi * 3 + 1], verts[vi * 3 + 2]));
    }
    HARUKA_LOGD("PropCollider", "malla de colision '%s': %d parte(s) registradas", proto.name.c_str(), (int)ids.size());
#else
    (void)phys; (void)protoIdx;
#endif
    return ids;
}

void PropSystem::refreshColliders(const glm::dvec3& planetC, double planetR, const glm::dvec3& camPos,
                                  Physics::PhysicsEngine* phys) {
#ifdef HARUKA_MOD_PHYSICS
    if (!phys) return;
    // ⚠️ RADIO ACOTADO: cada `addPropOBB` sube `m_staticsVersion` y Jolt recrea todos los cuerpos
    // estaticos. El radio tiene que superar la deriva entre refrescos del scatter (30 m) + margen.
    constexpr double kColliderRadiusM = 96.0;
    const double r2 = kColliderRadiusM * kColliderRadiusM;

    const int protoCount = m_registry.prototypeCount();
    std::vector<std::vector<Haruka::World::Props::PropColliderPart>> protoParts((size_t)protoCount);
    for (int pi = 0; pi < protoCount; ++pi) {
        const auto& proto = m_registry.prototype(pi);
        protoParts[(size_t)pi] = Haruka::World::Props::propColliderParts(proto.name, proto.meshSeed);
    }

    const auto t0 = std::chrono::steady_clock::now();
    phys->clearPropOBBs();
    phys->clearPropCones();
    phys->clearPropMeshInstances();
    size_t nProps = 0, nBoxes = 0;
    std::vector<size_t> perProtoProps((size_t)protoCount, 0), perProtoBoxes((size_t)protoCount, 0);
    // `HARUKA_PROP_BOXES=1` vuelve a las primitivas (caja/cono) por si hace falta comparar.
    static const bool s_forcePrims = [] { const char* e = std::getenv("HARUKA_PROP_BOXES"); return e && e[0] == '1'; }();
    for (const auto& io : m_registry.instances()) {
        if (io.state != (uint32_t)InstancedObjectState::Alive) continue;   // tocon: no estorba
        if (io.prototype < 0 || io.prototype >= protoCount) continue;
        const glm::dvec3 wp = planetC + glm::dvec3(io.dir) * (planetR + (double)io.heightM);
        const glm::dvec3 d  = wp - camPos;
        if (glm::dot(d, d) > r2) continue;
        ++nProps;
        const auto boxes = Haruka::World::Props::propWorldColliders(protoParts[(size_t)io.prototype], io.dir, io.heightM,
                                                              io.scale, io.yaw, planetC, planetR, io.breakMask);
        perProtoProps[(size_t)io.prototype]++;
        for (const auto& b : boxes) {
            // LA MALLA REAL DEL PROP, no primitivas: con caja o cono queda holgura o se atraviesa.
            const std::vector<int>& shapeIds = meshShapesFor(io.prototype, phys);
            const int partIdx = (b.partId >= 0) ? b.partId : 0;
            const int shapeId = (!s_forcePrims && partIdx < (int)shapeIds.size()) ? shapeIds[(size_t)partIdx] : -1;
            if (shapeId >= 0) {
                // La malla esta en el marco local sin escalar y con la base en el origen; la base de
                // rotacion es la MISMA que usa el render (`propInstanceBasis`).
                phys->addPropMeshInstance(shapeId, wp, Haruka::World::Props::propInstanceBasis(io.dir, io.yaw), (double)io.scale);
            } else if (b.isCone && !s_forcePrims) {
                phys->addPropCone(b.center, b.halfExtents.y, b.rTop, b.rBottom, b.rot);
            } else {
                phys->addPropOBB(b.center, b.halfExtents, b.rot);
            }
            ++nBoxes;
            perProtoBoxes[(size_t)io.prototype]++;
        }
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

    static uint32_t s_refreshN = 0;
    ++s_refreshN;
    // Sonda de ESCALA (una vez): el tamaño del collider contra el de la malla dibujada.
    {
        static bool s_once = false;
        if (!s_once) {
            for (const auto& io : m_registry.instances()) {
                if (io.prototype < 0 || io.prototype >= protoCount) continue;
                if (m_registry.prototype(io.prototype).name.find("tree") == std::string::npos) continue;
                const auto& pp = protoParts[(size_t)io.prototype];
                if (pp.empty()) break;
                s_once = true;
                HARUKA_LOGD("PropCollider", "ESCALA: io.scale=%.3f · tronco malla r=%.3f h=%.3f -> collider r=%.3f halfH=%.3f (x%.2f)",
                            io.scale, pp[0].radiusA, pp[0].length, pp[0].radiusA * io.scale, pp[0].half.y * io.scale, io.scale);
                break;
            }
        }
    }
    std::string desglose;
    for (int pi = 0; pi < protoCount; ++pi)
        desglose += " · " + m_registry.prototype(pi).name + "=" + std::to_string(perProtoProps[(size_t)pi]) + "props/" +
                    std::to_string(perProtoBoxes[(size_t)pi]) + "cajas";
    HARUKA_LOGD("PropCollider", "#%u · %zu props -> %zu cajas (%.2f ms)%s", s_refreshN, nProps, nBoxes, ms, desglose.c_str());
#else
    (void)planetC; (void)planetR; (void)camPos; (void)phys;
#endif
}

} // namespace Haruka::World::Props
