/**
 * @file application_render.cpp
 * @brief Application — el frame.
 *
 * Todo lo que ocurre entre dos `SwapBuffers`: construir la cola de render, los
 * pases de sombra en cascada, el G-buffer, la iluminación diferida, el terreno,
 * las islas, los props, el agua, las nubes volumétricas, el post-proceso y el
 * temporizador de GPU. El ciclo de vida está en `application.cpp` y las cachés de
 * assets en `application_assets.cpp`.
 *
 * `renderFrameContent()` es la función más grande del motor. No se lee de arriba
 * abajo: su árbol de ejecución (@ref flujo_interactivo) enseña los pases como una
 * lista plegable, y el nivel *Pasos grandes* deja a la vista las condiciones que
 * activan cada uno.
 *
 * La cola de render solo se reconstruye cuando la escena ha cambiado
 * (`m_renderQueueDirty`); el resto de frames se reutiliza tal cual.
 */
// glm/gtx/* (usado por glm::rotation del pase de props) exige la macro en GLM moderno.
#define GLM_ENABLE_EXPERIMENTAL

#include <chrono>
#include <cmath>
#include <cstdlib>   // getenv: HARUKA_COLLISION_WIRE
#include "application.h"
#include "world/water/water_fill.h"   // WATER_FILL_DRY: el centinela del mapa de lagos
#include "application_internal.h"
#include "rhi/rhi_context.h"   // ruta PSO: comandos de dibujo del frame (bloom migrado)

#include <algorithm>

#include "core/sky_ambient.h"   // ambiente integrado del MISMO cielo que se dibuja
#include "core/logger.h"
#include "core/progress_hook.h"

#include <SDL3/SDL.h>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>   // glm::mat4_cast (orientar el prototipo al suelo, pase de props)
#include <glm/gtx/quaternion.hpp>   // glm::rotation (eje→cuaternión: up del prototipo → dir radial)

#include "world/planet/planetary_system.h"
#include "world/terrain/terrain_lod.h"   // TERRAIN_COLLIDE_UNIFORM_M (radio del alambre de colision)
#include "core/components/mesh_renderer_component.h"
#include "core/components/material_component.h"
#include "renderer/model.h"
#include "renderer/simple_mesh.h"
#include "renderer/fallback_texture.h"
#include "renderer/scene_ubo.h"        // PerFrameUBOData / PerObjectUBOData / MaterialTexBit
#include "tools/object_types.h"
#include "tools/profiler.h"
#include "rhi/rhi_gpu_scope.h"   // HARUKA_GPU_SCOPE: el gemelo de HARUKA_PROFILE para la GPU
#include "settings/settings_manager.h"
#include "io/image_writer.h"
#include "core/asset_paths.h"
#include "world/props/prop_scatter.h"   // scatterPropsNear + IPropSphereField (scatter GLOBAL de props)
#include "world/props/prop_collider.h"  // colliders por PARTE derivados del esqueleto del árbol
#include "tools/procgraph/tree_mesh.h"
#include "tools/procgraph/prop_mesh.h"
#include "tools/procgraph/tree_textures.h"   // TreeCombineRGBNode (bake de material per-pixel de props)
#include "tools/procgraph/proc_texture.h"    // evaluateToRGBA / evaluateToNormalMap / createRHIFromRGBA

#include <vector>
#include <string>
#include <unordered_map>
#include <utility>
#include <ctime>
#include <cstdio>
#include <cstring>
#include <iostream>

namespace Haruka {
 namespace Core {

using AppInternal::g_sceneRenderQueue;

// (PerFrameUBOData / PerObjectUBOData / MaterialTexBit viven en renderer/scene_ubo.h.)
using Haruka::Renderer::PerFrameUBOData;
using Haruka::Renderer::PerObjectUBOData;
using Haruka::Renderer::kTexAlbedo; using Haruka::Renderer::kTexNormal; using Haruka::Renderer::kTexMetallic;
using Haruka::Renderer::kTexRoughness; using Haruka::Renderer::kTexAO;
namespace {
// (BloomParams/PresentParams viven en renderer/post_pass.cpp.)



// (ConstParams / ConstInstGroup y el material de grupo viven en renderer/scene_pass.cpp.)

// UBO del pase de LLUVIA (binding 5). x=tiempo · y=lluvia[0,1] · z=aspecto · w=inclinación(viento).
struct RainParams { glm::vec4 p; };
static_assert(sizeof(RainParams) == 16, "RainParams std140 size mismatch");

// (SkyParams vive en renderer/sky_pass.cpp.)

// (El UBO, las constantes y los mandos del pase de nubes viven en renderer/cloud_pass.cpp.)
} // namespace

void Application::buildRenderQueue() {
    HARUKA_PROFILE("buildRenderQueue");

    if (!_currentScene) { g_sceneRenderQueue.clear(); return; }

    // Cache the STATIC scene classification (kind/lod per object), which only
    // changes when the object set changes — not every frame. The expensive part
    // is the per-object string lowercasing + substring matching; transforms are
    // read live from the shared_ptr at draw time, so movement still updates.
    const auto& objects = _currentScene->getAllObjects();
    if (m_renderQueueDirty || objects.size() != m_renderQueueObjCount) {
        // INCREMENTAL: solo se clasifican los objetos NUEVOS. Clasificar es la parte cara (comparar
        // cadenas de tipo, mirar el mapa de propiedades) y antes se rehacía la escena ENTERA cada vez
        // que cambiaba el nº de objetos — con escombros apareciendo y caducando, eso es a todas horas.
        // Ahora un derrumbe cuesta clasificar sus 48 piezas, no reclasificar la ciudad.
        // Se identifica por UID, no por puntero: una dirección liberada se reutiliza para el objeto
        // siguiente, y entonces se daría por "ya clasificado" algo nuevo. Además así la fase de borrado
        // NO desreferencia punteros que pueden estar colgando.
        m_presentObjects.clear();
        m_presentObjects.reserve(objects.size());
        for (const auto& o : objects) if (o) m_presentObjects.insert(o->uid);

        // 1) Fuera los que ya no están en la escena (intercambio con el último → sin desplazar nada).
        if (m_staticQueueCount > g_sceneRenderQueue.size()) m_staticQueueCount = g_sceneRenderQueue.size();
        for (std::size_t k = 0; k < m_staticQueueCount; ) {
            if (m_presentObjects.count(g_sceneRenderQueue[k].uid)) { ++k; continue; }
            m_queuedObjects.erase(g_sceneRenderQueue[k].uid);
            g_sceneRenderQueue[k] = g_sceneRenderQueue[m_staticQueueCount - 1];
            --m_staticQueueCount;
        }
        g_sceneRenderQueue.resize(m_staticQueueCount);

        // 2) Entran los nuevos (los únicos que se clasifican).
        for (const auto& o : objects) {
            if (!o || m_queuedObjects.count(o->uid)) continue;
            Haruka::RenderCommand c = Haruka::classifySceneObject(*o);
            if (c.kind != Haruka::RenderKind::None) g_sceneRenderQueue.push_back(c);
            m_queuedObjects.insert(o->uid);    // se marca aunque no se dibuje: no reclasificarlo cada vez
        }
        m_staticQueueCount    = g_sceneRenderQueue.size();
        m_renderQueueObjCount = objects.size();
        m_renderQueueDirty    = false;
    } else {
        // Quita solo lo que se añadió el frame anterior (fantasmas de red). Sin copias ni realloc.
        g_sceneRenderQueue.resize(m_staticQueueCount);
    }

#ifdef HARUKA_NETWORK
    m_ghostObjects.clear();
    for (const auto& ghost : m_dgs.pollGhosts()) {
        Haruka::SceneObject obj;
        obj.name = "ghost_" + std::to_string(ghost.uuid);
        obj.type = "Character";
        obj.position = Haruka::WorldPos(
            ghost.chunkX * Haruka::Units::KM + ghost.pos[0],
            ghost.chunkY * Haruka::Units::KM + ghost.pos[1],
            ghost.chunkZ * Haruka::Units::KM + ghost.pos[2]
        );
        m_ghostObjects.push_back(std::move(obj));
    }
    for (const auto& obj : m_ghostObjects) {
        g_sceneRenderQueue.push_back({ &obj, Haruka::RenderKind::Primitive, Haruka::PrimitiveType::CHARACTER });
    }
#endif

    _iTotalDrawCalls = static_cast<int>(g_sceneRenderQueue.size());
    _iRenderedDrawCalls = _iTotalDrawCalls;
}


// Lo que los pases del frame comparten (ver `renderFrameContent`: la lista).
struct RenderFrame {
    uint32_t width = 0, height = 0;          ///< del target de escena (ventana, o viewport del editor)
    int      renderW = 0, renderH = 0;       ///< con la escala de render (= width/height sin post)
    RHI::RenderPassHandle sceneTargetPass{}; ///< invalido = backbuffer
    glm::vec3 skyColor{0.0f};
    bool      sceneOk = false;               ///< hay escena y camara
    float     aspect  = 1.0f;
    RHI::Context*  sceneCtx = nullptr;       ///< el contexto del pase de escena
    PerFrameUBOData frameData{};
    // TOTAL = geometria que el frame podia dibujar ANTES de culling; RENDERED = lo que entro.
    int renderedDrawCalls = 0, renderedVertices = 0, renderedTriangles = 0;
    int totalDrawCalls = 0,    totalVertices = 0,    totalTriangles = 0;
};

void Application::renderFrameContent() {
    HARUKA_PROFILE("renderFrameContent");
    // LOS ANILLOS DE INSTANCIAS EMPIEZAN DE CERO CADA FRAME *AQUÍ*, no en `renderFrame()`: el bucle
    // del juego (`Application::run`) llama a `renderFrameContent()` directo y a `renderFrame()` nunca
    // (es la ruta del editor). Hubo el mismo bug con `EntitySync`: el ring de props/scene se
    // acumulaba, el arena de instancias se llenaba y `upload` cortaba todo lote posterior → en
    // pantalla no salía ni un prop, con el log lleno de `tope duro`.
    m_scene.beginFrame();
    m_props.beginFrame();
    // LA LISTA DE PASES. Cada uno es una funcion con SU trozo del frame (`RenderFrame` lleva lo que
    // comparten: tamaño, target, contexto de escena, UBO por frame, contadores). El orden es el que
    // era; los `// ⚠️` de cada pase explican por que no puede moverse.
    RenderFrame f;
    frameBegin(f);            // tamaño, post, mecanica celeste, near, scatter de props, clear
    passCompute(f);           // compute del terreno: ANTES de abrir ningun render pass (Vulkan)
    passSky(f);               // fondo: cielo + clima + sol + aire del frame
    passSceneSetup(f);        // UBO por frame, luces, fisica, tonemapping, diagnostico de emisores
    passSceneObjects(f);      // objetos de la escena + piezas de construccion instanciadas
    passProps(f);             // props del mundo (modulo) y cierre del pase de escena
    passPlanetUpdate(f);      // LOD/stream del planeta + reloj del cluster
    passShadows(f);           // mapa de sombras del sol + mascara cenital + suelo mojado
    passPlanet(f);            // terreno + agua + alambre de colision + stats del terreno
    _iRenderedDrawCalls = f.renderedDrawCalls; _iRenderedVertices = f.renderedVertices; _iRenderedTriangles = f.renderedTriangles;
    // Sin escena: el total es la cola (lo que HABIA que dibujar), como antes.
    _iTotalDrawCalls    = f.sceneOk ? f.totalDrawCalls : (int)g_sceneRenderQueue.size();
    _iTotalVertices     = f.totalVertices;      _iTotalTriangles    = f.totalTriangles;
    passGameWorld(f);         // hook del juego (onRenderWorld)
    passPrecipitation(f);     // lluvia/nieve como geometria, con el depth de la escena aun atado
    passFluid(f);             // rios/lagos + splash (FluidHost)
    passClouds(f);            // nubes volumetricas (modulo): despues de toda la geometria
    m_post.composite((int)f.width, (int)f.height);   // FXAA + bloom + upscale al backbuffer
    frameEnd(f);              // backbuffer para el editor, captura limpia, ImGui
}

void Application::frameBegin(RenderFrame& f) {
    // Editor path calls this externally (no standalone loop) → reset here.
    // Standalone loop resets at its own frame boundary so game.onUpdate is measured.
    if (_editorTarget) Haruka::Profiler::get().newFrame();

    // ── AUDITORÍA DE RENDIMIENTO (`HARUKA_PROFILE_DUMP=segundos`) ───────────────────────────────
    //
    // El panel del HUD colapsa todo lo que baja de 0,5 ms ("+8 < 0.5 ms"), y para auditar eso es
    // justo lo que hay que ver: un scope de 0,3 ms que entra 200 veces por frame no sale en el panel
    // y es medio frame. Esto vuelca el árbol ENTERO al log, con tiempo propio (inclusivo menos
    // hijos), que es lo que señala al culpable en vez de al que lo contiene.
    //
    // Al log y no al HUD porque una auditoría se compara entre corridas, y para eso hace falta texto
    // que se pueda diffear, no una ventana que hay que fotografiar.
    {
        static const double s_dumpEvery = [] {
            const char* e = std::getenv("HARUKA_PROFILE_DUMP");
            return e ? atof(e) : 0.0;
        }();
        if (s_dumpEvery > 0.0) {
            static double s_last = -1e9;
            static int    s_frames = 0;
            ++s_frames;
            if (m_diagClock - s_last > s_dumpEvery) {
                s_last = m_diagClock;
                HARUKA_LOGI("Perf", "arbol del frame (media de %d frames):\n%s", s_frames,
                            Haruka::Profiler::get().dumpTree(1).c_str());
                s_frames = 0;
            }
        }
    }

    // ⚠️ CON TARGET DEL EDITOR, EL TAMAÑO ES EL DEL VIEWPORT, no el de la ventana. En el editor la
    // ventana existe (headless, 1280x720 de fábrica) y el viewport de ImGui mide lo que mida el
    // panel: con el aspecto de la ventana la imagen salía ESTIRADA en el panel y el rayo del ratón
    // (calculado con el aspecto del panel) no caía donde se veía el cursor ("no apunta bien por la
    // resolución/escala").
    const bool editorSized = _editorTarget && m_editorViewportW > 0 && m_editorViewportH > 0;
    const uint32_t width  = editorSized ? static_cast<uint32_t>(m_editorViewportW) : (_window ? _window->getWidth()  : static_cast<uint32_t>(m_editorViewportW));
    const uint32_t height = editorSized ? static_cast<uint32_t>(m_editorViewportH) : (_window ? _window->getHeight() : static_cast<uint32_t>(m_editorViewportH));

    // GPU timer disabled during RHI migration

    // --- Standalone post-processing target ---------------------------------
    // Route the 3D scene through an offscreen HDR target when any screen-space
    // effect or non-1.0 render scale is active. With everything off this stays
    // false → the scene renders straight to the screen exactly as before.
    // POST-PROCESO (renderer/post_pass.h): decide si la escena va a un target a escala de render.
    m_postActive = m_post.begin((int)width, (int)height, _editorTarget != nullptr);
    const int renderW = m_postActive ? m_post.renderW() : (int)width;
    const int renderH = m_postActive ? m_post.renderH() : (int)height;
    f.width = width; f.height = height; f.renderW = renderW; f.renderH = renderH;
    // Mecánica celeste: el WorldSystem usa el planeta REAL (su PlanetarySystem propio
    // está vacío). Le pasamos centro/radio del planeta activo y avanzamos día/noche +
    // luna + marea + viento cada frame (WorldSystem::update no se llama).
    if (_worldSystem && _planetarySystem) {
        glm::dvec3 pc; double pr;
        if (_planetarySystem->getActivePlanet(pc, pr))
            _worldSystem->setActivePlanet(pc, pr);
        { HARUKA_PROFILE("world.advanceCelestial");
          _worldSystem->advanceCelestial(deltaTime > 0.0f ? (double)deltaTime : 0.016); }
    }

    // NEAR PLANE DINÁMICO: pegado a cualquier superficie (alt ≤ 2 km) near=0.1 (precisión cercana
    // para terreno/objetos/ítem en mano). En órbita el near sube con la altitud → la precisión del
    // depth lejano mejora ~millones× → se acaba el z-fight agua↔lecho desde el espacio. Usa el
    // planeta MÁS CERCANO (no el home) para no recortar superficies cercanas en un sobrevuelo.
    if (_camera && _planetarySystem) {
        HARUKA_PROFILE("camera.nearPlane(recorre planetas)");
        const glm::dvec3 camD = glm::dvec3(_camera->position);
        double nearestAlt = 1e30;
        for (const auto& pl : _planetarySystem->getPlanets()) {
            double a = glm::length(camD - pl.position) - pl.radius;
            if (a < nearestAlt) nearestAlt = a;
        }
        // ⚠️ ARREGLADO: el umbral era 2 km y la pendiente 0,05, o sea que a 10 km el near valía
        // **400 m**. Todo lo que estuviera a menos de eso se recortaba — y eso incluye las dos cosas
        // que el jugador tiene siempre delante: el ÍTEM EN LA MANO (~0,5 m) y EL SUELO QUE PISA.
        // Bastaba subir a 2 km para quedarse sin manos y sin terreno cercano, mirando el mundo por un
        // agujero. El síntoma se leía como "el terreno desaparece", pero no desaparecía: se recortaba.
        //
        // El criterio estaba mal planteado: usaba la ALTITUD como si midiera "qué lejos está lo que
        // dibujo", cuando lo que decide el near es la distancia a lo MÁS CERCANO que hay que ver, y
        // eso a ras de cámara es medio metro pase lo que pase.
        //
        // Por qué se puede subir tanto el umbral: el depth es **D32F con reversed-Z** (clear a 0,
        // test GREATER) en todos los targets. Esa combinación concentra la precisión donde hace falta
        // y es justo la que hace innecesario inflar el near — que es la técnica que se usaba ANTES de
        // tener reversed-Z. Con 30 km de umbral el z-fight agua↔lecho que esto vino a arreglar sigue
        // cubierto (aparecía desde órbita, no desde 2 km) y el juego a pie o volando bajo conserva
        // manos y suelo.
        //
        // El tope de 200 m evita además que en órbita alta el near se dispare a `alt/2`, que era lo
        // que hacía imposible dibujar NADA cercano — una nave, una estación, tu propio vehículo.
        float dynNear = 0.1f;
        if (nearestAlt > 30000.0)
            dynNear = glm::clamp((float)((nearestAlt - 30000.0) * 0.02), 0.1f, 200.0f);
        _camera->setNearPlane(dynNear);
    }

    // Props del mundo: refresca el scatter global si la cámara cruzó un tramo (caché por posición).
    // Debe correr ANTES del render pass de escena, donde el pase instanciado lee el registro.
    { HARUKA_PROFILE("prop.scatter(re-siembra)");
      m_props.refreshScatter(_camera.get(), _planetarySystem.get(), getPhysicsEngineOrNull()); }

    // Cielo atmosférico: color por elevación solar + altitud (azul de día → cálido al
    // amanecer/atardecer → oscuro de noche → negro en el espacio). Fallback oscuro.
    // ⚠️ SIN PLANETA ACTIVO EL CIELO NO ES "OSCURO": ES QUE NO HAY CIELO.
    //
    // `getSkyColor` devuelve ~0,005 cuando no hay planeta —lo correcto para el espacio— y el frame se
    // limpiaba con eso. En el editor, o con la escena vacía, el resultado es un viewport casi negro
    // que parece roto: no distingues "no hay nada que dibujar" de "el render ha fallado".
    //
    // Ahora el fondo neutro es EXPLÍCITO y solo se usa cuando de verdad no hay mundo del que sacar
    // un cielo. Con planeta activo no cambia nada.
    constexpr glm::vec3 kNoWorldClear(0.12f, 0.13f, 0.15f);   // gris azulado declarado, no un cielo
    glm::vec3 sky = kNoWorldClear;
    if (_worldSystem && _camera && _worldSystem->hasActivePlanet()) {
        HARUKA_PROFILE("world.getSkyColor");
        sky = _worldSystem->getSkyColor(glm::dvec3(_camera->position));
    }
    RHI::Device* frameDev = RHI::device();
    RHI::Context* frameCtx = nullptr;
    { HARUKA_PROFILE("rhi.beginFrame"); frameCtx = frameDev ? frameDev->beginFrame() : nullptr; }
    RHI::RenderPassHandle sceneTargetPass;
    if (_editorTarget) {
        sceneTargetPass = _editorTarget->getPass();
    } else if (m_postActive) {
        sceneTargetPass = m_post.scenePass();
    }

    // CLEAR DEL FRAME, incondicional y en UN SOLO SITIO. Antes lo hacía el pase de cielo, que vive
    // bajo tres condiciones (`_worldSystem && _camera && _planetarySystem`, que haya planeta ACTIVO
    // y que el PSO del cielo compilara). Una escena sin planeta —la típica del editor: unas
    // primitivas y una luz— no cumplía la segunda, así que no se limpiaba NI color NI profundidad:
    // con reversed-Z el test es GREATER, el z-buffer conservaba el del frame anterior y a partir
    // del segundo frame ningún fragmento a la misma profundidad volvía a pasar → viewport negro.
    // El cielo, cuando corre, reabre este mismo pase SIN clear y pinta encima.
    if (frameCtx) {
        HARUKA_PROFILE("frame.clear(pass)"); HARUKA_GPU_SCOPE("frame.clear(pass)");
        RHI::ClearValues frameClear;
        frameClear.clearColor = true;
        frameClear.color[0] = sky.r; frameClear.color[1] = sky.g; frameClear.color[2] = sky.b;
        frameClear.color[3] = 1.0f;
        frameClear.clearDepth = true; frameClear.depth = 0.0f;   // reversed-Z: lejos = 0
        frameCtx->beginRenderPass(sceneTargetPass, frameClear);
        if (!RHI::valid(sceneTargetPass))            // el pass a pantalla NO fija viewport
            frameCtx->setViewport(0, 0, (int)width, (int)height);
        frameCtx->endRenderPass();
    }
    f.sceneTargetPass = sceneTargetPass;
    f.skyColor = sky;
    f.sceneOk  = _currentScene && _camera;
}

void Application::passCompute(RenderFrame& f) {
    (void)f;
    // ── TRABAJO DE COMPUTE, ANTES DE ABRIR NINGÚN RENDER PASS ───────────────────────────────────
    //
    // ⚠️ El orden es OBLIGATORIO, no una optimización. `vkCmdDispatch` dentro de una instancia de
    // render pass es ILEGAL en Vulkan, y el culling de parches del terreno se despachaba dentro del
    // pase de escena porque en OpenGL eso es legal y corriente. Cerraba el programa, y el síntoma
    // despistaba: en una captura de RenderDoc los draws salían BIEN uno a uno —el replay los ejecuta
    // aislados— mientras en ejecución el dispositivo se perdía a mitad de frame y quedaba la
    // pantalla negra. Si alguien mueve esta llamada dentro de un pase, vuelve el mismo fallo.
    if (_camera && _planetarySystem) {
        HARUKA_PROFILE("frame.compute.prepare"); HARUKA_GPU_SCOPE("frame.compute.prepare");
        uint32_t vpW = 0, vpH = 0;
        if (RHI::Device* d = RHI::device()) d->framebufferSize(vpW, vpH);
        if (_editorTarget && m_editorViewportW > 0 && m_editorViewportH > 0) { vpW = (uint32_t)m_editorViewportW; vpH = (uint32_t)m_editorViewportH; }   // el viewport del IDE, no la ventana headless

        // ⚠️ EL ASPECTO SALE DEL FRAMEBUFFER REAL, NO DE `_camera->aspectRatio`.
        //
        // El cono que envuelve al frustum lo decide la ESQUINA: atan(tan(fovY/2)·sqrt(1+aspect²)).
        // Con 60° y 16:9 son 49,7°; con aspecto 1,0 son 39,2°. Los diez grados de diferencia son
        // exactamente las esquinas del cuadro, y recortarlas se ve como "chunks cortados antes de
        // que acabe la pantalla".
        //
        // `Camera::aspectRatio` vale 1.0f de fábrica y solo lo escribe el callback de resize
        // (application.cpp), así que hasta el primer resize puede no valer lo que se está dibujando.
        // El tamaño del framebuffer sí es lo que se dibuja — y en esta máquina además NO coincide con
        // el de la ventana (el compositor escala; ver [[vulkan-is-opengl-assumed]]).
        const double vpAspect = (vpW > 0 && vpH > 0) ? (double)vpW / (double)vpH
                                                     : (double)_camera->aspectRatio;
        _planetarySystem->prepareSimplePlanets(
            glm::dvec3(_camera->position), glm::dvec3(glm::normalize(_camera->getFront())),
            glm::radians((double)_camera->zoom), vpAspect,
            (vpH > 0) ? (double)vpH : 1080.0,
            glm::dvec3(glm::normalize(_camera->getUp())));
    }
}

void Application::passSky(RenderFrame& f) {
    Haruka::Renderer::SkyPass::Frame sf;
    sf.camera = _camera.get(); sf.planets = _planetarySystem.get(); sf.world = _worldSystem.get();
    sf.sceneTargetPass = f.sceneTargetPass; sf.renderW = f.renderW; sf.renderH = f.renderH;
    sf.width = f.width; sf.height = f.height; sf.rainOverride = m_rainOverride;
    m_sky.draw(sf);
}


void Application::passSceneSetup(RenderFrame& f) {
    if (!f.sceneOk) return;
    RHI::Device* uboDev = RHI::device();
    f.aspect = (f.height > 0u) ? static_cast<float>(f.width) / static_cast<float>(f.height) : 1.0f;
    f.frameData = PerFrameUBOData{};
    HARUKA_PROFILE("scene.setup(UBOs+shaders+luces)");
    // Lazy-create UBOs (dinámicos; el update por frame sigue con glBufferSubData/glBindBufferBase).
    if (!RHI::valid(m_uboPerFrameH)) {
        m_uboPerFrameH = uboDev->createBuffer(RHI::BufferUsage::Uniform, sizeof(PerFrameUBOData), nullptr, RHI::BufferMemory::Dynamic);
    }

    const bool useFinalLook = getRenderFeatureHDR() || getRenderFeatureBloom()
                            || getRenderFeatureSSAO() || getRenderFeatureIBL()
                            || getRenderFeatureShadows();
    // (Los programas de planet.* y water.* los poseen ahora los PSO del TerrainRenderer y del
    //  WaterRenderer — cada pase ata su propio pipeline; ya no hay Shader suelto que bindear.)

    // El PASE DE ESCENA (renderer/scene_pass.h) crea su pipeline, abre el pass sin clear y ata los UBO.
    f.sceneCtx = uboDev->beginFrame();
    {
        Haruka::Renderer::ScenePass::Frame sf;
        sf.camera = _camera.get(); sf.planets = _planetarySystem.get(); sf.perFrameUBO = m_uboPerFrameH;
        sf.sceneTargetPass = f.sceneTargetPass; sf.width = f.width; sf.height = f.height;
        m_scene.begin(f.sceneCtx, sf, useFinalLook);
    }

    // Upload per-frame UBO
    const glm::vec3  cameraOrigin = glm::vec3(_camera->position);

    f.frameData.view            = glm::mat4(glm::mat3(_camera->getViewMatrix()));
    f.frameData.projection      = _camera->getProjectionMatrix(f.aspect);
    f.frameData.cameraPos       = cameraOrigin;
    // LAS LUCES DEL FRAME (sol, luna, ambiente del cielo) las pone `Renderer::FrameLights` (renderer/frame_lights.h).
    Haruka::Renderer::FrameLights::Frame lf;
    lf.camera = _camera.get(); lf.world = _worldSystem.get(); lf.planets = _planetarySystem.get(); lf.scene = _currentScene;
    lf.cameraOrigin = cameraOrigin; lf.heightPx = (int)f.height; lf.diagClock = m_diagClock;
    m_lights.fill(f.frameData, lf);
    // Viento atmosférico → arrastre aerodinámico de la física (por cuerpo, barato).
    if (_physicsEngine && _worldSystem)
        _physicsEngine->setWind(_worldSystem->getWind(glm::dvec3(cameraOrigin)));
    // Avanza el motor de física con TIMESTEP FIJO (determinista). Hoy solo procesa cuerpos
    // DINÁMICOS (el jugador aún es kinemático → no afecta); lo activa de verdad la Fase 2.
    if (_physicsEngine) {
        // Sonda: cuánto del frame se va en física. Sin esto, "Jolt cuesta poco/mucho" era una
        // opinión — y decisiones como el job system de UN hilo (determinismo para el DGS) solo se
        // pueden juzgar sabiendo qué se paga por ellas. Ver HARUKA_FRAMELOG en application.cpp.
        const auto tPhys0 = std::chrono::steady_clock::now();
        _physicsEngine->advance(deltaTime > 0.0f ? (double)deltaTime : 0.016);
        Haruka::physicsFrameMs() = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - tPhys0).count();
    }
    // Only advertise a feature if its GPU resources are actually allocated.
    // Enabling a flag without the corresponding FBO/texture bound causes
    // undefined behaviour in final.frag (samples from empty texture units).
    // ── EL TONEMAPPING NO PUEDE DEPENDER DEL ORDEN DE ARRANQUE ─────────────────────────────
    //
    // ⚠️ `prop_inst.frag` decide DENTRO del shader si tonemapea:
    //
    //     if (enableHDR != 0) { color = color/(color+1); color = pow(color, 1/2.2); }
    //     else                { color = clamp(color, 0.0, 1.0); }        // ← satura a BLANCO
    //
    // Y `enableHDR` exige que `_hdr` exista. Pero `_hdr` solo se creaba en `onResize`, así que
    // cualquier camino que no pase por ahí —el viewport del Editor, que dibuja a su propio
    // target— dejaba el puntero nulo y los props se RECORTABAN: todo lo que pasa de 1 en blanco
    // puro. Ese es el "props quemados" que parecía un bug de Vulkan y no lo era: era el Editor
    // contra el juego.
    //
    // Se crea aquí si falta. Es idempotente y no cuesta nada cuando ya existe.
    // El AJUSTE manda: el panel lo cambia en caliente y esto lo aplica al frame siguiente.
    setRenderFeatureHDR(Haruka::SettingsManager::get().graphics().hdr);
    if (getRenderFeatureHDR() && !_hdr && f.width > 0 && f.height > 0)
        _hdr = std::make_unique<HDR>((unsigned)f.width, (unsigned)f.height);
    {
        static bool s_logged = false;
        if (!s_logged) {
            s_logged = true;
            HARUKA_LOGI("Render", "tonemapping de props: %s (feature=%d, buffer=%s)",
                        (getRenderFeatureHDR() && _hdr) ? "ACTIVO" : "APAGADO -> clamp a blanco",
                        (int)getRenderFeatureHDR(), _hdr ? "si" : "NO");
        }
    }
    f.frameData.enableHDR       = (int)(getRenderFeatureHDR()     && _hdr    != nullptr);
    f.frameData.enableBloom     = (int)(getRenderFeatureBloom()   && _bloom  != nullptr);
    f.frameData.enableSSAO      = (int)(getRenderFeatureSSAO()    && _ssao   != nullptr);
    f.frameData.enableIBL       = (int)(getRenderFeatureIBL()     && _ibl    != nullptr);
    f.frameData.enableShadows   = (int)(getRenderFeatureShadows() && m_shadows.shadowReady());

    uboDev->updateBuffer(m_uboPerFrameH, 0, sizeof(PerFrameUBOData), &f.frameData);

    m_diagClock += (deltaTime > 0.0f ? (double)deltaTime : 0.016);
    { Haruka::Renderer::FrameLights::Frame lf;
      lf.camera = _camera.get(); lf.world = _worldSystem.get(); lf.planets = _planetarySystem.get(); lf.scene = _currentScene;
      lf.cameraOrigin = cameraOrigin; lf.heightPx = (int)f.height; lf.diagClock = m_diagClock;
      m_lights.starDiagnostics(f.frameData, lf); }
}

void Application::passSceneObjects(RenderFrame& f) {
    if (!f.sceneOk) return;
    Haruka::Renderer::ScenePass::Frame sf;
    sf.camera = _camera.get(); sf.planets = _planetarySystem.get(); sf.perFrameUBO = m_uboPerFrameH;
    sf.sceneTargetPass = f.sceneTargetPass; sf.width = f.width; sf.height = f.height;
    Haruka::Renderer::ScenePass::DrawStats st;
    m_scene.drawObjects(f.sceneCtx, sf, g_sceneRenderQueue, st);
    f.renderedDrawCalls += st.renderedDrawCalls; f.renderedVertices += st.renderedVertices; f.renderedTriangles += st.renderedTriangles;
    f.totalDrawCalls += st.totalDrawCalls; f.totalVertices += st.totalVertices; f.totalTriangles += st.totalTriangles;
}


void Application::passProps(RenderFrame& f) {
    if (!f.sceneOk) return;
    // --- Pase INSTANCIADO de PROPS del mundo (mismo render pass/depth que la escena) -------
    // El modulo (world/props/prop_system.h) hace scatter, cull, LOD y draw; aqui solo se le
    // pasa lo que es del frame.
    if (RHI::valid(m_scene.pso())) {
        Haruka::World::PropSystem::DrawFrame pf;
        pf.camera = _camera.get(); pf.planets = _planetarySystem.get(); pf.world = _worldSystem.get();
        pf.perFrameUBO = m_uboPerFrameH; pf.scenePSO = m_scene.pso();
        pf.aspect = f.aspect; pf.heightPx = (int)f.height; pf.aerial = m_sky.weather().aerial; pf.diagClock = m_diagClock;
        Haruka::World::PropSystem::DrawStats ps;
        m_props.draw(f.sceneCtx, pf, ps);
        f.renderedDrawCalls += ps.renderedDrawCalls; f.renderedVertices += ps.renderedVertices; f.renderedTriangles += ps.renderedTriangles;
        f.totalDrawCalls += ps.totalDrawCalls; f.totalVertices += ps.totalVertices; f.totalTriangles += ps.totalTriangles;
    }
    if (RHI::valid(m_scene.pso())) f.sceneCtx->endRenderPass();
}

void Application::passPlanetUpdate(RenderFrame& f) {
    if (!f.sceneOk || !_planetarySystem) return;
    (void)f;
    {
        HARUKA_PROFILE("planetary.update(LOD+stream)");
        _planetarySystem->syncFromScene(*_currentScene);
#ifdef HARUKA_NETWORK
        // ⚠️ THE SUN IS NOT AN ENTITY, so replicating positions never put two players in the
        // same day. Everything time-shaped in this world is an analytic function of the
        // simulation clock — the planets' orbits, and therefore sunrise; the weather fronts;
        // the tide — and that clock was a LOCAL accumulator starting at zero. Two players who
        // launched five minutes apart were five minutes apart in the world.
        //
        // One number fixes all of it, so it is handed over every frame rather than nudged:
        // an accumulator cannot stay in step across a stall or a loading screen, and the whole
        // point is that both machines compute the same `t`.
        if (m_dgs.hasWorldTime())
            _planetarySystem->setWorldClock(m_dgs.worldTimeSeconds());
#endif
        _planetarySystem->update(deltaTime > 0.0f ? deltaTime : 0.016, glm::dvec3(_camera->position));
    }
}

void Application::passShadows(RenderFrame& f) {
    if (!f.sceneOk || !_planetarySystem) return;
    Haruka::Renderer::ShadowPass::Frame sf;
    sf.camera = _camera.get(); sf.planets = _planetarySystem.get(); sf.props = &m_props;
    sf.onRenderShadow = _gameInterface ? _gameInterface->onRenderShadow : nullptr;
    sf.sunDirection = f.frameData.sunDirection;
    const auto& wx = m_sky.weather();
    const Haruka::Renderer::ShadowPass::WeatherView wv{ wx.rainAmount, wx.snowAmount, wx.groundWetness, wx.snowAccum };
    sf.weather = &wv;
    m_shadows.draw(f.sceneCtx, sf);
}


void Application::passPlanet(RenderFrame& f) {
    if (!f.sceneOk || !_planetarySystem) return;
    // Restore scene framebuffer + viewport before drawing SimplePlanet/game objects
    // (shadow/sky-mask passes changed both and GL's endRenderPass is a no-op).
    {
        RHI::ClearValues restore; restore.clearColor = false; restore.clearDepth = false;
        f.sceneCtx->beginRenderPass(f.sceneTargetPass, restore);
        if (!RHI::valid(f.sceneTargetPass))
            f.sceneCtx->setViewport(0, 0, (int)f.width, (int)f.height);
    }

    // --- SimplePlanet terrain rendering ---
    {
        HARUKA_PROFILE("simple_planet.draw"); HARUKA_GPU_SCOPE("simple_planet.draw");
        const float aspectC = (f.height > 0u) ? (float)f.width / (float)f.height : 1.0f;
        const glm::mat4 projC    = _camera->getProjectionMatrix(aspectC);
        const glm::mat4 viewC    = _camera->getViewMatrix();
        // El suelo cercano que viene de la FÍSICA se entrega ANTES de dibujar: el planeta lo
        // pinta con su propio material (mismo `biome.frag`, mismos bindings), así que lo
        // único distinto respecto al clipmap es de dónde salen los vértices.
        { Haruka::World::VoxSystem::Frame vf; vf.camera = _camera.get(); vf.planets = _planetarySystem.get(); vf.physics = getPhysicsEngineOrNull();
          m_vox.updateNearGroundRing(vf); }
        for (size_t i = 0; i < _planetarySystem->getSimplePlanetCount(); ++i) {
            const auto& sp = _planetarySystem->getSimplePlanet(i);
            _planetarySystem->renderSimplePlanet(sp.name,
                glm::dvec3(_camera->position), projC, viewC);
        }
        // ALAMBRE DE LA MALLA DE COLISIÓN, justo DESPUÉS del terreno y con su misma
        // profundidad: así el alambre queda oculto por el relieve donde pasa por debajo y
        // visible donde pasa por encima. Es lo que hace la comparación legible.
        // La vista va SIN traslación porque los vértices ya llegan relativos a la cámara.
        {
            Haruka::Renderer::CollisionWire::Frame wf;
            wf.camera = _camera.get(); wf.planets = _planetarySystem.get(); wf.physics = getPhysicsEngineOrNull();
            wf.meshShapes = &m_props.meshShapes(); wf.meshCpu = &m_props.meshCpu();
            m_wire.draw(f.sceneCtx, projC * glm::mat4(glm::mat3(viewC)), wf);
        }
    }

    // Stats del TERRENO (malla base + clipmap + agua): se suman a los contadores del frame
    // y se guardan para el panel. TOTAL == RENDERED aquí (el planeta dibuja todo lo que
    // tiene; solo el agua culla las caras tras el planeta, que es lo que refleja el desglose).
    m_terrainStats = _planetarySystem->getTerrainRenderStats();
    f.totalDrawCalls  += m_terrainStats.drawCalls;
    f.totalVertices   += (int)(m_terrainStats.baseVertices + m_terrainStats.clipVertices);
    f.totalTriangles  += (int)(m_terrainStats.baseTriangles + m_terrainStats.clipTriangles);
    f.renderedDrawCalls += m_terrainStats.drawCalls;
    f.renderedVertices  += (int)(m_terrainStats.baseVertices + m_terrainStats.clipVertices);
    f.renderedTriangles += (int)(m_terrainStats.baseTriangles + m_terrainStats.clipTriangles);
}

void Application::passGameWorld(RenderFrame& f) {
    if (_gameInterface && _gameInterface->onRenderWorld && _camera) {
        const float      aspectG = (f.height > 0u) ? static_cast<float>(f.width) / static_cast<float>(f.height) : 1.0f;
        const glm::mat4  view    = _camera->getViewMatrix();
        const glm::mat4  proj    = _camera->getProjectionMatrix(aspectG);
        const glm::vec3  camPos  = glm::vec3(_camera->position);
        _gameInterface->onRenderWorld(view, proj, camPos);
    }
}

void Application::passPrecipitation(RenderFrame& f) {
    // --- PRECIPITACIÓN (lluvia / nieve) — EN EL MUNDO, no sobre la imagen ---------------------
    // Va AQUÍ y no al final del frame a propósito: este es el último punto en el que el target de
    // escena y su DEPTH siguen bindeados. Dibujada después del composite, la gota no tendría contra
    // qué probarse y volveríamos al filtro de pantalla que se acaba de quitar.
    if (m_sky.weather().rainAmount > 0.01f || m_sky.weather().snowAmount > 0.01f) {
        HARUKA_PROFILE("precip.draw"); HARUKA_GPU_SCOPE("precip.draw");
        const glm::dvec3 camD = glm::dvec3(_camera->position);
        glm::dvec3 upD(0, 1, 0);
        // Altura de la BASE DE LA NUBE respecto a la cámara. Sin planeta activo no hay cota de la que
        // colgar la nube, así que se deja sin techo (el valor por defecto de Params).
        float cloudBaseRelCam = 1e9f;
        if (_planetarySystem) {
            glm::dvec3 pc; double pr;
            if (_planetarySystem->getActivePlanet(pc, pr)) {
                const glm::dvec3 r = camD - pc; const double rl = glm::length(r);
                if (rl > 1e-9) upD = r / rl;
                // `cloudBaseM` es cota sobre el nivel del mar, igual que la que consume el pase
                // volumétrico (`CloudParams::slab.x`): se le resta la de la cámara para dejarla en la
                // MISMA referencia que usa el shader de gotas (altura sobre la cámara).
                const Haruka::WeatherSample wxP = _planetarySystem->weatherAt(camD);
                cloudBaseRelCam = (float)(wxP.cloudBaseM - (rl - pr));
            }
        }
        Haruka::PrecipitationRenderer::Params pp;
        pp.cloudBaseRelCamM = cloudBaseRelCam;
        pp.cameraPos   = camD;
        pp.up          = glm::vec3(upD);
        pp.lookDir     = _camera->getFront();
        pp.wind        = m_sky.weather().windVec;
        pp.sunColor    = _worldSystem ? _worldSystem->getDominantLightColor(camD) : glm::vec3(1.0f);
        pp.snow        = (m_sky.weather().snowAmount > 0.01f);
        pp.amount      = pp.snow ? m_sky.weather().snowAmount : m_sky.weather().rainAmount;
        // TODO (fase C): `skyVisibility` saldrá de la máscara cenital — hoy 1.0, así que bajo un
        // tejado la gota que está ENTRE el tejado y tu cara sigue viéndose (el depth solo descarta
        // lo que tiene algo DELANTE). Es lo único de "no llueve bajo cubierto" que falta.
        pp.skyVisibility = 1.0f;
        pp.timeSeconds = _planetarySystem ? _planetarySystem->simulationTime() : 0.0;
        pp.viewportHeightPx = f.renderH;
        if (m_shadows.skyMaskOn()) {             // fase C: cada gota mira si tiene algo ENCIMA
            pp.skyMask  = m_shadows.skyMaskTexture();
            pp.skySpace = m_shadows.skySpace();
        }
        m_precip.render(pp);

    }
}

void Application::passFluid(RenderFrame& f) {
#ifdef HARUKA_MOD_FLUIDS
    // Rios/lagos/charcos + splash: el enganche al planeta es `World::FluidBridge` (world/water).
    Haruka::World::FluidBridge::Frame ff;
    ff.planets = _planetarySystem.get(); ff.camera = _camera.get(); ff.dt = deltaTime;
    ff.rainAmount = m_sky.weather().rainAmount; ff.renderW = f.renderW; ff.renderH = f.renderH;
    ff.sceneTargetPass = f.sceneTargetPass; ff.perFrameUBO = m_uboPerFrameH;
    Haruka::World::FluidBridge::run(_fluidHost, ff);
#else
    (void)f;
#endif
}


void Application::passClouds(RenderFrame& f) {
    // --- NUBES VOLUMÉTRICAS ------------------------------------------------
    //
    // ⚠️ VA AQUÍ, DESPUÉS DE TODA LA GEOMETRÍA, y ese es el cambio de fondo. El cúmulo se pintaba
    // en `sky.frag`, que es un pase de FONDO (sin depth, antes que la escena): eso lo convertía en
    // un telón que cualquier objeto tapaba y, sobre todo, **sin interior** — no se podía atravesar
    // una nube porque un fondo no tiene dentro. Dibujándolo aquí, con la profundidad de la escena
    // a mano, la nube es un cuerpo que se recorre: si la cámara está dentro, el rayo arranca dentro
    // y la pérdida de visibilidad sale de la integración, sin ningún efecto de pantalla añadido.
    // --- NUBES VOLUMÉTRICAS: el modulo (renderer/cloud_pass.h) hornea, marcha y compone ---------
    {
        Haruka::Renderer::CloudPass::Frame cf;
        cf.camera = _camera.get(); cf.planets = _planetarySystem.get(); cf.world = _worldSystem.get();
        cf.sceneTargetPass = f.sceneTargetPass;
        cf.width  = f.renderW;
        cf.height = f.renderH;
        cf.rainOverride = m_rainOverride; cf.windVec = m_sky.weather().windVec; cf.aerial = m_sky.weather().aerial;
#ifdef HARUKA_MOD_FLUIDS
        cf.dynamicWaterM3 = _fluidHost ? _fluidHost->dynamicWaterM3 : 0.0;
#endif
        m_clouds.draw(cf);
    }
}

void Application::frameEnd(RenderFrame& f) {
    // Devolver la PANTALLA como framebuffer atado antes de salir. En GL `endRenderPass` es un
    // no-op, así que al terminar la escena sigue atado el FBO del último pase — con `_editorTarget`,
    // el RenderTarget del viewport. Quien nos llama (el IDE) dibuja su ImGui justo después dando por
    // hecho que el framebuffer es la pantalla: sin esto, el UI ENTERO se pinta dentro del target del
    // viewport y el backbuffer no lo escribe nadie, así que la ventana alterna entre dos buffers
    // viejos → parpadea entera.
    // El VIEWPORT lo deja el llamante: en modo editor `_window` es null y `width`/`height` son los
    // del viewport, no los de la ventana — fijarlo aquí sería adivinar. El IDE lo hace con el tamaño
    // real justo antes de su `glClear`.
    if (_editorTarget) {
        if (RHI::Device* dev = RHI::device()) {
            RHI::Context* ctx = dev->beginFrame();
            RHI::ClearValues keep; keep.clearColor = false; keep.clearDepth = false;
            ctx->beginRenderPass(RHI::RenderPassHandle{}, keep);   // id 0 = backbuffer (pantalla)
            ctx->endRenderPass();
        }
    }

    // Clean screenshot (no HUD): capture here, after the 3D scene is composited to
    // the screen but BEFORE ImGui draws over it.
    if (!_editorTarget) captureScreenshotIfPending((int)f.width, (int)f.height);

    if (_imguiCallback) {
        _imguiCallback();
    }

    // GPU timer disabled during RHI migration
}


// Los PROPS del mundo (scatter, pase instanciado, sombras, colliders) son `World::PropSystem`
// (world/props/prop_system.h): antes vivian aqui, ~900 lineas.

// (El alambre de la malla de colision es `Renderer::CollisionWire`; las cuevas y el anillo del suelo
//  cercano, `World::VoxSystem`.)

Application::PropHit Application::breakPropAt(const glm::dvec3& center, double radius) {
    return m_props.breakAt(center, radius, _camera.get(), _planetarySystem.get(), getPhysicsEngineOrNull());
}

unsigned int Application::getMaterialTextureGL(const std::string& path) {
    RHI::TextureHandle tex = AppInternal::getOrLoadMaterialTexture(path);
    if (!RHI::valid(tex)) return 0;
    RHI::Device* dev = RHI::device();
    return dev ? dev->nativeTexture(tex) : 0;
}

void Application::renderMaterialPreview(const Haruka::MaterialComponent& material,
                                        Haruka::Renderer::RenderTarget& target) {
    RHI::Device* dev = RHI::device();
    if (!dev || !RHI::valid(target.getPass())) return;

    // PSO propio, no el de la escena: `m_scenePSO` alterna entre final.frag y preview.frag según los
    // ajustes de render del usuario, y una preview que cambia de aspecto al tocar un ajuste global
    // no sirve para juzgar un material. Éste se queda SIEMPRE en el look final.
    if (!RHI::valid(m_matPreviewPSO)) {
        const std::string vsPath = Shader::baseDir() + "shaders/simple.vert";
        const std::string fsPath = Shader::baseDir() + "shaders/final.frag";
        using V = Haruka::Renderer::Vertex;
        RHI::PipelineDesc pd;
        pd.vertexPath           = vsPath.c_str();
        pd.fragmentPath         = fsPath.c_str();
        pd.vertexLayout.strides = { (uint32_t)(sizeof(V)) };
        pd.vertexLayout.attributes = {
            { 0, (uint32_t)offsetof(V, Position),  RHI::Format::RGB32F },
            { 1, (uint32_t)offsetof(V, Normal),    RHI::Format::RGB32F },
            { 2, (uint32_t)offsetof(V, TexCoords), RHI::Format::RG32F  },
            { 3, (uint32_t)offsetof(V, Tangent),   RHI::Format::RGB32F },
            { 4, (uint32_t)offsetof(V, Bitangent), RHI::Format::RGB32F },
        };
        pd.topology     = RHI::PrimitiveTopology::Triangles;
        pd.depth.test   = true;  pd.depth.write = true;
        pd.blend.enable = false;
        pd.cull         = RHI::CullMode::Back;
        m_matPreviewPSO = dev->createPipeline(pd);
    }
    if (!RHI::valid(m_matPreviewPSO)) return;
    if (!RHI::valid(m_uboPerFrameH))
        m_uboPerFrameH = dev->createBuffer(RHI::BufferUsage::Uniform, sizeof(PerFrameUBOData), nullptr, RHI::BufferMemory::Dynamic);
    const RHI::BufferHandle perObjectUBO = m_scene.perObjectUBO();

    SimpleMesh* sphere = AppInternal::getPrimitiveMesh(Haruka::PrimitiveType::SPHERE);
    if (!sphere) return;

    // Cámara e iluminación FIJAS: dos materiales solo se pueden comparar si se miran igual. La luz
    // de tres cuartos (arriba-izquierda-delante) es la que enseña a la vez el lado iluminado, el
    // terminador y el rim — que es donde se lee un material.
    // La esfera primitiva es de radio 1 y el FOV es de 35°: a distancia d subtiende asin(1/d), que
    // tiene que caber en el semiángulo de 17.5°. A 2.6 la esfera era MÁS GRANDE que el encuadre y
    // salía recortada en un cuadrado con las esquinas redondeadas. 4.0 → 14.5°, con aire alrededor.
    const float kDist = 4.0f;
    PerFrameUBOData f{};
    f.view       = glm::mat4(glm::mat3(glm::lookAt(glm::vec3(0, 0, kDist), glm::vec3(0), glm::vec3(0, 1, 0))));
    f.projection = glm::perspective(glm::radians(35.0f), 1.0f, 0.05f, 100.0f);
    f.cameraPos  = glm::vec3(0, 0, kDist);
    f.sunDirection    = glm::normalize(glm::vec3(-0.45f, 0.65f, 0.62f));
    f.sunLightColor   = glm::vec3(1.0f, 0.97f, 0.92f);
    f.ambientStrength = 0.25f;
    f.enableHDR = 1; f.enableBloom = 0; f.enableSSAO = 0; f.enableIBL = 0; f.enableShadows = 0;
    f.moonDirection = glm::vec3(0, 1, 0); f.moonIntensity = 0.0f;
    f.moonLightColor = glm::vec3(0.0f);
    dev->updateBuffer(m_uboPerFrameH, 0, sizeof(f), &f);

    PerObjectUBOData o{};
    o.model = glm::translate(glm::mat4(1.0f), glm::vec3(0, 0, -kDist));  // la view solo lleva rotación
    o.baseColorAndPlanetRadius = glm::vec4(material.albedo, 1.0f);
    o.planetCenterAndFlag      = glm::vec4(0.0f);
    o.materialEmission         = glm::vec4(material.emission, 0.0f);

    RHI::Context* ctx = dev->beginFrame();
    RHI::ClearValues clear;
    clear.clearColor = true;
    clear.color[0] = 0.09f; clear.color[1] = 0.10f; clear.color[2] = 0.12f; clear.color[3] = 1.0f;
    clear.clearDepth = true; clear.depth = 0.0f;   // reversed-Z
    ctx->beginRenderPass(target.getPass(), clear);
    ctx->bindPipeline(m_matPreviewPSO);

    int texMask = 0;
    auto bindSlot = [&](const char* key, uint32_t unit, int bit) {
        auto it = material.textures.find(key);
        if (it == material.textures.end() || it->second.empty()) return;
        RHI::TextureHandle tex = AppInternal::getOrLoadMaterialTexture(it->second);
        if (!RHI::valid(tex)) return;
        ctx->bindTexture(unit, tex);
        texMask |= bit;
    };
    bindSlot("albedo",    0, kTexAlbedo);
    bindSlot("normal",    1, kTexNormal);
    bindSlot("metallic",  2, kTexMetallic);
    bindSlot("roughness", 3, kTexRoughness);
    bindSlot("ao",        4, kTexAO);
    o.materialPBR = glm::vec4(material.metallic, material.roughness, material.ao, (float)texMask);
    dev->updateBuffer(perObjectUBO, 0, sizeof(o), &o);

    ctx->bindUniformBuffer(0, m_uboPerFrameH);
    ctx->bindUniformBuffer(1, perObjectUBO);
    sphere->drawRHI(*ctx);
    ctx->endRenderPass();

    // Devolver la pantalla: en GL el fin de pase no desata nada y quien llama (el editor) dibuja
    // su ImGui a continuación. Misma razón que en renderFrameContent.
    RHI::ClearValues keep; keep.clearColor = false; keep.clearDepth = false;
    ctx->beginRenderPass(RHI::RenderPassHandle{}, keep);
    ctx->endRenderPass();
}

// (Las entidades remotas en la escena son `Net::EntitySync`: net/entity_sync.h.)

void Application::renderFrame() {
    auto now = std::chrono::high_resolution_clock::now();
    const std::chrono::duration<float> elapsed = now - _frameStart;
    _frameStart = now;

    deltaTime = elapsed.count();
    _lastFrameTimeMs = deltaTime * 1000.0f;

#ifdef HARUKA_NETWORK
    m_entitySync.sync(m_dgs, _currentScene);
#endif

    // In standalone mode (run()), the main loop handles swap + FPS.
    // In editor mode (no _window), renderFrame() is called externally
    // and the editor manages the swap.
    buildRenderQueue();
    renderFrameContent();

    if (!_window) return; // editor path — caller handles swap

    if (RHI::device())
        RHI::device()->endFrame();

    _fpsFrameCount++;
    _fpsLastTime += deltaTime;
    if (_fpsLastTime >= 1.0) {
        _lastFps      = static_cast<float>(_fpsFrameCount / _fpsLastTime);
        _fpsFrameCount = 0;
        _fpsLastTime   = 0.0;
    }
}

void Application::requestScreenshot(const std::string& path) {
    m_screenshotPath    = path;
    m_screenshotPending = true;
}

void Application::captureScreenshotIfPending(int width, int height) {
    if (!m_screenshotPending || width <= 0 || height <= 0) return;
    m_screenshotPending = false;

    // ⚠️ EL TAMAÑO SALE DEL FRAMEBUFFER REAL, NO DE LA VENTANA. Medido: con una ventana de
    // 1920x1080 el swapchain de Vulkan era de 1280x720, así que la copia escribía filas de 1280 en
    // un buffer que se leía como de 1920 — la captura salía REPETIDA en horizontal y comprimida en
    // vertical. El tamaño lógico de SDL y el del framebuffer no tienen por qué coincidir (escalado
    // del compositor, HiDPI, o un swapchain creado a otro tamaño).
    if (RHI::Device* dev = RHI::device()) {
        uint32_t fbw = 0, fbh = 0;
        dev->framebufferSize(fbw, fbh);
        if (fbw > 0 && fbh > 0) { width = (int)fbw; height = (int)fbh; }
    }

    // Read the composited back buffer (RGBA8), then flip rows (GL is bottom-up).
    std::vector<unsigned char> buf((size_t)width * height * 4);
    if (RHI::Device* dev = RHI::device()) {
        dev->readPixels(0, 0, width, height, RHI::Format::RGBA8, buf.data());
    }

    // ⚠️ EL VOLTEO ES SOLO DE OPENGL. GL tiene el origen ABAJO-izquierda, así que hay que dar la
    // vuelta a las filas; las imágenes de Vulkan son top-down y ya vienen en el orden bueno.
    // Volteando siempre, la captura de Vulkan salía DEL REVÉS — y como hasta ahora el mundo se veía
    // negro, el fallo estaba escondido: solo se notó cuando por fin hubo algo que mirar.
    const bool flipRows = !RHI::device() || RHI::device()->backend() == RHI::Backend::OpenGL;
    std::vector<unsigned char> flipped((size_t)width * height * 4);
    const size_t stride = (size_t)width * 4;
    if (flipRows) {
        for (int y = 0; y < height; ++y)
            std::memcpy(&flipped[(size_t)y * stride], &buf[(size_t)(height - 1 - y) * stride], stride);
    } else {
        flipped = buf;
    }

    std::string path = m_screenshotPath;
    if (path.empty()) {
        std::time_t t = std::time(nullptr);
        std::tm tm{};
#if defined(_WIN32)
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        char name[64];
        std::strftime(name, sizeof(name), "screenshots/shot_%Y%m%d_%H%M%S.png", &tm);
        path = name;
    }

    if (Haruka::writePNG(path, width, height, 4, flipped.data()))
        std::cout << "[Application] Screenshot saved: " << path << std::endl;
    else
        std::cerr << "[Application] Screenshot FAILED: " << path << std::endl;
}


}} // namespace Haruka::Core
