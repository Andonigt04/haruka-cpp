#include "Haruka/Renderer/ScenePass.h"

#include "Haruka/Core/ApplicationInternal.h"   // getTransformMatrix / modelos / primitivas / texturas de material
#include "Haruka/Core/Camera.h"
#include "Haruka/Core/Components/MaterialComponent.h"
#include "Haruka/Core/Components/MeshRendererComponent.h"
#include "Haruka/Core/Scene/SceneManager.h"
#include "Haruka/Renderer/FallbackTexture.h"
#include "Haruka/Renderer/Model.h"
#include "Haruka/Renderer/SceneUBO.h"
#include "Haruka/Renderer/Shader.h"
#include "Haruka/Renderer/SimpleMesh.h"
#include "Haruka/RHI/RHIContext.h"
#include "Haruka/RHI/RHIDevice.h"
#include "Haruka/RHI/RHIGPUScope.h"
#include "Haruka/Tools/Profiler.h"
#include "Haruka/World/Planet/PlanetarySystem.h"
#include "Haruka/World/Props/PropGpu.h"            // PropParams: el UBO de prop_inst.frag

#include <algorithm>
#include <cstring>
#include <string>
#include <unordered_map>

namespace Haruka::Renderer {

using Haruka::Core::AppInternal::getTransformMatrix;
using Haruka::Core::AppInternal::getCharacterTransform;
using Haruka::Core::AppInternal::getOrLoadModelCached;
using Haruka::Core::AppInternal::getPrimitiveMesh;
using Haruka::Core::AppInternal::getOrLoadMaterialTexture;

namespace {


// Material PER-PIXEL de un grupo de piezas instanciadas (objetos NPC/construcción): los MISMOS
// slots que el pase de escena (MaterialComponent). mask == 0 → solo color por instancia.
struct ConstGroupMaterial {
    Haruka::RHI::TextureHandle albedo = {}, normal = {}, metallic = {}, roughness = {}, ao = {};
    float   metallicS  = 0.0f;
    float   roughnessS = 0.5f;
    float   aoS        = 1.0f;
    uint32_t mask      = 0u;
};

// Resuelve el material de un objeto instanciado → texturas cargadas + escalares + máscara.
// Sin MaterialComponent (o sin slots) → mascara 0 (el aspecto de siempre por color de instancia).
static ConstGroupMaterial resolveInstancedMaterial(const Core::Components::MaterialComponent* mat) {
    ConstGroupMaterial m;
    if (!mat) return m;
    m.metallicS = mat->metallic; m.roughnessS = mat->roughness; m.aoS = mat->ao;
    auto bind = [&](const char* key, uint32_t unit, uint32_t bit, Haruka::RHI::TextureHandle& out) {
        (void)unit;
        auto it = mat->textures.find(key);
        if (it == mat->textures.end() || it->second.empty()) return;
        Haruka::RHI::TextureHandle tex = Haruka::Core::AppInternal::getOrLoadMaterialTexture(it->second);
        if (!Haruka::RHI::valid(tex)) return;
        out = tex; m.mask |= bit;
    };
    bind("albedo",    0, kTexAlbedo,    m.albedo);
    bind("normal",    1, kTexNormal,    m.normal);
    bind("metallic",  2, kTexMetallic,  m.metallic);
    bind("roughness", 3, kTexRoughness, m.roughness);
    bind("ao",        4, kTexAO,        m.ao);
    return m;
}

// Clave de material para el agrupamiento: las rutas de texturas + los escalares. Dos piezas con la
// misma malla y la MISMA clave comparten un draw (mismo bind de texturas y misma máscara).
static std::string instancedMaterialKey(const Core::Components::MaterialComponent* mat) {
    std::string k;
    if (!mat) return k;
    static const char* kSlots[] = {"albedo", "normal", "metallic", "roughness", "ao"};
    for (const char* s : kSlots) {
        auto it = mat->textures.find(s);
        if (it != mat->textures.end() && !it->second.empty()) { k += s; k += '='; k += it->second; k += ';'; }
    }
    k += "m"; { uint32_t bits; std::memcpy(&bits, &mat->metallic, 4); k += std::to_string(bits); }
    k += "r"; { uint32_t bits; std::memcpy(&bits, &mat->roughness, 4); k += std::to_string(bits); }
    k += "a"; { uint32_t bits; std::memcpy(&bits, &mat->ao, 4);        k += std::to_string(bits); }
    return k;
}

// Grupo de piezas instanciadas (NPC/construcción): MISMA malla + MISMO material → un draw con un
// bind de texturas. `meshKey` = modelPath, o "prim:N" para primitivas de edificio (cubos tejidos).
struct ConstInstGroup {
    std::string  meshKey;
    int          primitive = -1;
    ConstGroupMaterial mat;
    std::vector<InstanceDataFloat> instances;
};

} // namespace

ScenePass::~ScenePass() { shutdown(); }

void ScenePass::shutdown() {
    if (RHI::Device* dev = RHI::device()) {
        if (RHI::valid(m_scenePSO))       dev->destroy(m_scenePSO);
        if (RHI::valid(m_skyBodyPSO))     dev->destroy(m_skyBodyPSO);
        if (RHI::valid(m_perObjectUBO))   dev->destroy(m_perObjectUBO);
        for (RHI::BufferHandle b : m_objectUBOs) if (RHI::valid(b)) dev->destroy(b);
        if (RHI::valid(m_constInstPSO))   dev->destroy(m_constInstPSO);
        if (RHI::valid(m_constParamsUBO)) dev->destroy(m_constParamsUBO);
    }
    m_scenePSO = m_constInstPSO = m_skyBodyPSO = {}; m_perObjectUBO = m_constParamsUBO = {};
    m_objectUBOs.clear();
    m_instancing.reset();
}

RHI::BufferHandle ScenePass::perObjectUBO() {
    if (!RHI::valid(m_perObjectUBO))
        if (RHI::Device* dev = RHI::device())
            m_perObjectUBO = dev->createBuffer(RHI::BufferUsage::Uniform, sizeof(PerObjectUBOData), nullptr, RHI::BufferMemory::Dynamic);
    return m_perObjectUBO;
}

// ⚠️ UN UBO POR OBJETO, NO UNO PARA TODOS (17-10). Se reescribia `m_perObjectUBO` antes de cada draw:
// en GL `glBufferSubData` queda ordenado entre draws, pero en Vulkan el buffer es memoria MAPEADA y la
// escritura es un `memcpy` inmediato — los draws ya grabados en el command buffer leen al ejecutarse,
// asi que TODOS veian los datos del ULTIMO objeto. El Sol y la Luna se dibujaban con la matriz de otro
// y no salian (medido: la Luna aparecia en GL y no en Vulkan con el mismo frame).
RHI::BufferHandle ScenePass::objectUBO(size_t i) {
    while (m_objectUBOs.size() <= i) {
        RHI::Device* dev = RHI::device();
        if (!dev) return {};
        m_objectUBOs.push_back(dev->createBuffer(RHI::BufferUsage::Uniform, sizeof(PerObjectUBOData),
                                                 nullptr, RHI::BufferMemory::Dynamic));
    }
    return m_objectUBOs[i];
}

void ScenePass::begin(RHI::Context* ctx, const Frame& f, bool useFinalLook) {
    RHI::Device* dev = RHI::device();
    if (!dev || !ctx) return;
    perObjectUBO();
    // === PASE 4 MIGRADO A PSO/Context: los OBJETOS de la escena. ===
    // El pipeline hornea shader + layout `Vertex` + estado (depth on). Se recrea solo si
    // cambia la variante de fragment (final.frag <-> preview.frag).
    if (!RHI::valid(m_scenePSO) || m_scenePSOFinalLook != useFinalLook) {
        if (RHI::valid(m_scenePSO)) dev->destroy(m_scenePSO);
        const std::string vsPath = Shader::baseDir() + "shaders/simple.vert";
        const std::string fsPath = Shader::baseDir() +
            (useFinalLook ? "shaders/final.frag" : "shaders/preview.frag");
        using V = Haruka::Renderer::Vertex;
        RHI::PipelineDesc pd;
        pd.vertexPath              = vsPath.c_str();
        pd.fragmentPath            = fsPath.c_str();
        pd.vertexLayout.strides     = { (uint32_t)(sizeof(V)) };
        pd.vertexLayout.attributes = {
            { 0, (uint32_t)offsetof(V, Position),  RHI::Format::RGB32F },
            { 1, (uint32_t)offsetof(V, Normal),    RHI::Format::RGB32F },
            { 2, (uint32_t)offsetof(V, TexCoords), RHI::Format::RG32F  },
            { 3, (uint32_t)offsetof(V, Tangent),   RHI::Format::RGB32F },
            { 4, (uint32_t)offsetof(V, Bitangent), RHI::Format::RGB32F },
        };
        pd.topology     = RHI::PrimitiveTopology::Triangles;
        pd.depth.test   = true;  pd.depth.write = true;   // escena sólida
        pd.blend.enable = false;
        pd.cull         = RHI::CullMode::Back;   // lo hacía el glEnable(GL_CULL_FACE) global
        m_scenePSO = dev->createPipeline(pd);
        // Cuerpos del cielo: la radiancia del disco se SUMA a la del aire de delante, sin tapar el
        // depth de nada cercano (estan a cientos de miles de km).
        if (RHI::valid(m_skyBodyPSO)) dev->destroy(m_skyBodyPSO);
        pd.depth.write  = false;
        pd.blend.enable = true; pd.blend.mode = RHI::BlendMode::Additive;
        m_skyBodyPSO = dev->createPipeline(pd);
        m_scenePSOFinalLook = useFinalLook;
    }

    if (RHI::valid(m_scenePSO)) {
        // El target de escena ya está bindeado y limpiado arriba → reabrir el MISMO pass SIN
        // clear (borraría el cielo ya pintado).
        RHI::ClearValues keep; keep.clearColor = false; keep.clearDepth = false;
        ctx->beginRenderPass(f.sceneTargetPass, keep);
        if (!RHI::valid(f.sceneTargetPass))           // el pass a pantalla NO fija viewport
            ctx->setViewport(0, 0, (int)f.width, (int)f.height);
        ctx->bindPipeline(m_scenePSO);         // programa + depth on (absorbe glUseProgram)
        ctx->bindUniformBuffer(0, f.perFrameUBO);
        ctx->bindUniformBuffer(1, m_perObjectUBO);
    }
}

void ScenePass::drawObjects(RHI::Context* ctx, const Frame& f, const std::vector<Core::Scene::RenderCommand>& queue, DrawStats& st) {
    RHI::Device* dev = RHI::device();
    if (!dev || !ctx || !f.camera) return;
    // Cull face handled by the pipeline

    // TOTAL = geometría que el frame podía dibujar ANTES de culling; RENDERED = lo que de verdad
    // entró en el draw. Solo divergen donde hay cull de verdad: props (frustum/sub-pixel + tope
    // del buffer) y agua (caras tras el planeta). El resto (objetos, piezas, terreno) dibuja
    // todo lo que llega, así que TOTAL == RENDERED ahí.

    // Piezas de CONSTRUCCIÓN (prop "construction"/"building"): en vez de 1 draw por pieza, se
    // acumulan por (malla, material) y se dibujan INSTANCIADAS tras el bucle (1 draw por grupo).
    // El material del grupo es el del MaterialComponent del objeto (per-pixel, MISMO aspecto que
    // el pase de escena); dos piezas con la misma malla y el mismo material comparten el draw.
    std::unordered_map<std::string, ConstInstGroup> constGroups;

    { HARUKA_PROFILE("scene.objects.draw"); HARUKA_GPU_SCOPE("scene.objects.draw");
    size_t objSlot = 0;   // UBO por objeto: ver `objectUBO`
    for (const auto& command : queue) {
        const auto* obj = command.object;
        if (!obj) continue;

        // "Emisivo" (estrella, sin sombreado) = el cuerpo EMITE luz. Así un
        // CelestialBody que NO emite (la Luna) queda SOMBREADO por el Sol → se
        // ven sus cráteres y fases, en vez de salir a pleno brillo.
        const bool isStar = obj->flags.castLight;

        // Distance LOD/cull for scene objects (they have no chunk LOD like terrain):
        // skip far props/models so they don't cost draw calls at any range. Scale-aware
        // (bigger objects stay visible farther). Stars/celestial bodies are never culled
        // (they must show from orbit/space).
        if (!isStar) {
            double objDist  = glm::length(obj->position - f.camera->position);
            double maxScale = std::max({obj->scale.x, obj->scale.y, obj->scale.z});
            double maxDist  = 500.0 + maxScale * 300.0;   // 1 m → ~800 m, 50 m → ~15 km
            if (objDist > maxDist) continue;
        }

        glm::vec3 baseColor = glm::vec3(obj->color);
        if (glm::length(baseColor) < 0.001f) baseColor = glm::vec3(0.75f, 0.76f, 0.80f);

        // Stars emit from their light component color, not the default object color.
        if (isStar && obj->components.contains("light")) {
            const auto& lc = obj->components["light"];
            if (lc.contains("color") && lc["color"].is_array() && lc["color"].size() >= 3)
                baseColor = glm::vec3(lc["color"][0].get<float>(),
                                     lc["color"][1].get<float>(),
                                     lc["color"][2].get<float>());
        }

        // El albedo del material TIÑE el color de la pieza (misma regla que el pase de escena:
        // `baseColor × mat.albedo`). El material entero (texturas + escalares) viaja con el grupo.
        const Core::Components::MaterialComponent* mat = obj->material ? obj->material.get() : nullptr;
        const glm::vec3 instColor = baseColor * (mat ? mat->albedo : glm::vec3(1.0f));

        // Pieza de construcción → al pase INSTANCIADO (no draw individual). Mismo model/color que
        // llevaría por-objeto, así se ve idéntica; el cull de arriba ya se aplicó.
        if (command.instanced && command.kind == Core::Scene::RenderKind::Model && !obj->modelPath.empty()) {
            const InstanceDataFloat inst = InstanceDataFloat::fromMatrix(
                Haruka::Core::AppInternal::getTransformMatrix(*obj, f.camera->position), instColor);
            const std::string key = obj->modelPath + "|" + instancedMaterialKey(mat);
            ConstInstGroup& g = constGroups[key];
            if (g.instances.empty()) {
                g.meshKey = obj->modelPath; g.primitive = -1;
                g.mat = resolveInstancedMaterial(mat);
            }
            g.instances.push_back(inst);
            continue;
        }
        // Primitiva de construcción/edificio → al mismo pase instanciado, agrupada por (primitiva,
        // material): un edificio tejido son cientos o miles de cubos —las tablas/sillares del muro—
        // y por-objeto serían cientos o miles de draws.
        if (command.instanced && command.kind == Core::Scene::RenderKind::Primitive) {
            const InstanceDataFloat inst = InstanceDataFloat::fromMatrix(
                Haruka::Core::AppInternal::getTransformMatrix(*obj, f.camera->position), instColor);
            const std::string key = std::string("prim:") + std::to_string((int)command.primitive) +
                                    "|" + instancedMaterialKey(mat);
            ConstInstGroup& g = constGroups[key];
            if (g.instances.empty()) {
                g.meshKey.clear(); g.primitive = (int)command.primitive;
                g.mat = resolveInstancedMaterial(mat);
            }
            g.instances.push_back(inst);
            continue;
        }

        PerObjectUBOData objData{};
        if (command.kind == Core::Scene::RenderKind::Primitive && command.primitive == Tools::PrimitiveType::CHARACTER) {
            glm::dvec3 pc(0.0); double pr = 0.0;
            const glm::dvec3 upC = (f.planets && f.planets->getActivePlanet(pc, pr) && glm::length(obj->position - pc) > 1e-9)
                                 ? glm::normalize(obj->position - pc) : glm::dvec3(0.0, 1.0, 0.0);
            objData.model = Haruka::Core::AppInternal::getCharacterTransform(*obj, f.camera->position, upC);
        } else {
            objData.model                    = Haruka::Core::AppInternal::getTransformMatrix(*obj, f.camera->position);
        }
        objData.baseColorAndPlanetRadius = glm::vec4(baseColor, 1.0f);
        // w = 0: normal, 1: procedural terrain, 2: emisivo sin sombreado (lamparas...),
        //     3: cuerpo del cielo ILUMINADO (la Luna), 4: cuerpo del cielo que EMITE (el Sol).
        // Los dos ultimos sacan su luz de la radiometria (`lib/sky_light.glsl`), no de un color.
        const bool skyStar = obj->objectType == Tools::ObjectType::STAR;
        const bool skyBody = skyStar ||
            ((obj->objectType == Tools::ObjectType::PLANET || obj->objectType == Tools::ObjectType::SATELLITE) &&
             command.kind == Core::Scene::RenderKind::MeshComponent);
        objData.planetCenterAndFlag      = glm::vec4(0.0f, 0.0f, 0.0f,
                                                     skyStar ? 4.0f : skyBody ? 3.0f : isStar ? 2.0f : 0.0f);
        // Sin MaterialComponent: dieléctrico mate sin texturas → el aspecto de siempre.
        objData.materialPBR              = glm::vec4(0.0f, 0.5f, 1.0f, 0.0f);
        objData.materialEmission         = glm::vec4(0.0f);

        // --- MATERIAL del objeto (albedo/normal/metallic/roughness/ao) -------------------
        // Los slots del MaterialComponent son los MISMOS que hornea el editor de node graph
        // del IDE, así que un grafo bakeado se ve aquí sin más paso intermedio.
        if (obj->material) {
            const Core::Components::MaterialComponent& mat = *obj->material;
            // El color del material TIÑE el del objeto: un material no debe borrar el tinte
            // por objeto (dos props del mismo material se distinguen por su color).
            objData.baseColorAndPlanetRadius = glm::vec4(baseColor * mat.albedo, 1.0f);
            objData.materialEmission         = glm::vec4(mat.emission, 0.0f);

            int texMask = 0;
            auto bindSlot = [&](const char* key, uint32_t unit, int bit) {
                auto it = mat.textures.find(key);
                if (it == mat.textures.end() || it->second.empty()) return;
                RHI::TextureHandle tex = Haruka::Core::AppInternal::getOrLoadMaterialTexture(it->second);
                if (!RHI::valid(tex)) return;
                ctx->bindTexture(unit, tex);
                texMask |= bit;
            };
            bindSlot("albedo",    0, kTexAlbedo);
            bindSlot("normal",    1, kTexNormal);
            bindSlot("metallic",  2, kTexMetallic);
            bindSlot("roughness", 3, kTexRoughness);
            bindSlot("ao",        4, kTexAO);

            objData.materialPBR = glm::vec4(mat.metallic, mat.roughness, mat.ao, (float)texMask);
        }

        const RHI::BufferHandle objUBO = objectUBO(objSlot++);
        dev->updateBuffer(objUBO, 0, sizeof(PerObjectUBOData), &objData);
        ctx->bindUniformBuffer(1, objUBO);

        switch (command.kind) {
            case Core::Scene::RenderKind::Model: {
                Model* model = Haruka::Core::AppInternal::getOrLoadModelCached(obj->modelPath);
                if (!model) break;
                if (!RHI::valid(m_scenePSO)) break;
                // La carga PEREZOSA de arriba crea mallas, y Mesh::setupMesh() deja el VAO a 0
                // → desbindearía el VAO del pipeline JUSTO antes de dibujar (glDrawElements con
                // VAO 0 = GL_INVALID_OPERATION). Rebindeamos el pipeline después de cargar.
                // Desaparece cuando la creación de recursos deje de tocar el estado global.
                ctx->bindPipeline(m_scenePSO);
                model->drawRHI(*ctx);
                ++st.renderedDrawCalls;
                st.renderedVertices  += model->getVertexCount();
                st.renderedTriangles += model->getTriangleCount();
                ++st.totalDrawCalls;
                st.totalVertices  += model->getVertexCount();
                st.totalTriangles += model->getTriangleCount();
                break;
            }
            case Core::Scene::RenderKind::MeshComponent: {
                if (!obj->meshRenderer || !obj->meshRenderer->isResident()) break;
                if (!RHI::valid(m_scenePSO)) break;
                ctx->bindPipeline(skyBody && RHI::valid(m_skyBodyPSO) ? m_skyBodyPSO : m_scenePSO);   // ver nota del caso Model (estado GL)
                obj->meshRenderer->renderRHI(*ctx);
                ++st.renderedDrawCalls;
                st.renderedVertices  += obj->meshRenderer->getResidentVertexCount();
                st.renderedTriangles += obj->meshRenderer->getResidentTriangleCount();
                ++st.totalDrawCalls;
                st.totalVertices  += obj->meshRenderer->getResidentVertexCount();
                st.totalTriangles += obj->meshRenderer->getResidentTriangleCount();
                break;
            }
            case Core::Scene::RenderKind::Primitive: {
                SimpleMesh* primitiveMesh = Haruka::Core::AppInternal::getPrimitiveMesh(command.primitive);
                if (!primitiveMesh) break;
                if (!RHI::valid(m_scenePSO)) break;
                ctx->bindPipeline(skyBody && RHI::valid(m_skyBodyPSO) ? m_skyBodyPSO : m_scenePSO);   // ver nota del caso Model (estado GL)
                primitiveMesh->drawRHI(*ctx);
                ++st.renderedDrawCalls;
                st.renderedVertices  += primitiveMesh->getVertexCount();
                st.renderedTriangles += primitiveMesh->getTriangleCount();
                ++st.totalDrawCalls;
                st.totalVertices  += primitiveMesh->getVertexCount();
                st.totalTriangles += primitiveMesh->getTriangleCount();
                break;
            }
            case Core::Scene::RenderKind::None:
                break;
        }
    }
    } // scene.objects.draw

    // --- Pase INSTANCIADO de piezas de construcción (mismo render pass/depth que la escena) ---
    if (!constGroups.empty() && RHI::valid(m_scenePSO)) {
        HARUKA_PROFILE("scene.construction.instanced"); HARUKA_GPU_SCOPE("scene.construction.instanced");
        if (!RHI::valid(m_constInstPSO)) {                    // PSO instanciado (una vez)
            using V = Haruka::Renderer::Vertex;
            const std::string vs = Shader::baseDir() + "shaders/mesh_inst.vert";
            const std::string fs = Shader::baseDir() + "shaders/prop_inst.frag";
            RHI::PipelineDesc pd;
            pd.vertexPath   = vs.c_str();
            pd.fragmentPath = fs.c_str();
            pd.vertexLayout.strides    = { (uint32_t)sizeof(V) };            // binding 0 = Vertex
            pd.vertexLayout.attributes = {
                { 0, (uint32_t)offsetof(V, Position), RHI::Format::RGB32F, 0 },
                { 1, (uint32_t)offsetof(V, Normal),   RHI::Format::RGB32F, 0 },
                { 2, (uint32_t)offsetof(V, TexCoords), RHI::Format::RG32F,  0 },   // UV → material per-pixel
            };
            Haruka::Renderer::GPUInstancing::appendInstanceLayout(pd.vertexLayout, 1);  // binding 1 = instancias
            pd.topology     = RHI::PrimitiveTopology::Triangles;
            pd.depth.test   = true;  pd.depth.write = true;
            pd.blend.enable = false;
            pd.cull         = RHI::CullMode::Back;
            m_constInstPSO  = dev->createPipeline(pd);
        }
        if (RHI::valid(m_constInstPSO)) {
            if (!m_instancing) { m_instancing = std::make_unique<GPUInstancing>(); m_instancing->init(20000); }
            if (!RHI::valid(m_constParamsUBO)) {              // UBO del material del grupo (binding 6)
                m_constParamsUBO = dev->createBuffer(RHI::BufferUsage::Uniform,
                                                        sizeof(Haruka::World::Props::PropParams), nullptr,
                                                        RHI::BufferMemory::Dynamic);
            }
            ctx->bindPipeline(m_constInstPSO);
            ctx->bindUniformBuffer(0, f.perFrameUBO);   // view/proj + luces (mismo UBO que la escena)

            // El buffer de instancias es FIJO (`addInstance` descarta al pasarse), y un edificio
            // tejido puede traer miles de piezas → se dibuja POR LOTES del tamaño del buffer, así
            // no desaparece ninguna. `fill` sube el lote [off, off+n) y el llamador dispara el draw.
            const std::size_t cap = (std::size_t)std::max(1, m_instancing->getMaxInstances());
            auto fillChunk = [&](const std::vector<InstanceDataFloat>& src, std::size_t off) {
                const std::size_t n = std::min(cap, src.size() - off);
                m_instancing->clear();
                for (std::size_t i = 0; i < n; ++i)
                    m_instancing->addInstance(src[off + i]);
            };
            // Enlaza las texturas del material del grupo + su UBO (escalares/máscara) → per-pixel.
            auto bindGroupMaterial = [&](const ConstGroupMaterial& gm) {
                // Mismo motivo que en el pase de props: en Vulkan un slot sin atar es basura.
                ctx->bindTexture(0, RHI::valid(gm.albedo)    ? gm.albedo
                                         : fallbackTexture(FallbackTex::White));
                ctx->bindTexture(1, RHI::valid(gm.normal)    ? gm.normal
                                         : fallbackTexture(FallbackTex::Normal));
                ctx->bindTexture(2, RHI::valid(gm.metallic)  ? gm.metallic
                                         : fallbackTexture(FallbackTex::Metallic));
                ctx->bindTexture(3, RHI::valid(gm.roughness) ? gm.roughness
                                         : fallbackTexture(FallbackTex::White));
                ctx->bindTexture(4, RHI::valid(gm.ao)        ? gm.ao
                                         : fallbackTexture(FallbackTex::White));
                // El MISMO fragment que los props (prop_inst.frag): lo que distingue la obra es
                // material — rim al 35 % y sin especular (ver la cabecera del shader). Sin viento,
                // sin aire y con el origen en la cámara: las piezas ya llegan cámara-relativas.
                Haruka::World::Props::PropParams cp{};
                cp.matPBR = glm::vec4(gm.metallicS, gm.roughnessS, gm.aoS, (float)gm.mask);
                cp.flags  = glm::vec4(dev->backend() == RHI::Backend::Vulkan ? 1.0f : 0.0f, 0.35f, 0.0f, 0.0f);
                dev->updateBuffer(m_constParamsUBO, 0, sizeof(cp), &cp);
                ctx->bindUniformBuffer(6, m_constParamsUBO);
            };

            for (auto& kv : constGroups) {
                ConstInstGroup& g = kv.second;
                if (g.instances.empty()) continue;
                // Malla del modelo (pieza de obra) o primitiva de edificio (cubos tejidos).
                Model* model = g.primitive < 0 ? Haruka::Core::AppInternal::getOrLoadModelCached(g.meshKey) : nullptr;
                SimpleMesh* pm = g.primitive >= 0
                                 ? Haruka::Core::AppInternal::getPrimitiveMesh((Tools::PrimitiveType)g.primitive) : nullptr;
                if (g.primitive < 0 && !model) continue;
                if (g.primitive >= 0 && (!pm || pm->getIndexCount() == 0 ||
                                         !RHI::valid(pm->vertexBuffer()) || !RHI::valid(pm->indexBuffer()))) continue;
                // La carga/creación PEREZOSA de arriba crea recursos GL y deja el VAO a 0 → habría
                // desbindeado el VAO del pipeline justo antes del draw (GL_INVALID_OPERATION).
                // Re-bindear tras resolver la malla. Misma trampa que documenta el pase de escena.
                ctx->bindPipeline(m_constInstPSO);
                bindGroupMaterial(g.mat);
                for (std::size_t off = 0; off < g.instances.size(); off += cap) {
                    const std::size_t n = std::min(cap, g.instances.size() - off);
                    fillChunk(g.instances, off);
                    if (model) {
                        model->drawInstancedRHI(*ctx, *m_instancing, 1);   // 1 draw por malla del modelo
                        ++st.renderedDrawCalls;
                        // Piezas de OBRA (casas/puentes): contaban 0 en el panel — se cuentan
                        // aquí por instancia del lote (TOTAL == RENDERED: sin cull, todas entran).
                        st.renderedVertices  += model->getVertexCount() * (int)n;
                        st.renderedTriangles += model->getTriangleCount() * (int)n;
                        ++st.totalDrawCalls;
                        st.totalVertices  += model->getVertexCount() * (int)n;
                        st.totalTriangles += model->getTriangleCount() * (int)n;
                    } else {
                        ctx->bindVertexBuffer(pm->vertexBuffer(), 0);
                        ctx->bindIndexBuffer(pm->indexBuffer());
                        m_instancing->render(ctx, (uint32_t)pm->getIndexCount(), 1);
                        ++st.renderedDrawCalls;
                        st.renderedVertices  += pm->getVertexCount()   * (int)n;
                        st.renderedTriangles += pm->getTriangleCount() * (int)n;
                        ++st.totalDrawCalls;
                        st.totalVertices  += pm->getVertexCount()   * (int)n;
                        st.totalTriangles += pm->getTriangleCount() * (int)n;
                    }
                }
            }
            ctx->bindPipeline(m_scenePSO);   // restaura el PSO de escena (el cierre del pass lo asume)
        }
    }
}

} // namespace Haruka::Renderer
