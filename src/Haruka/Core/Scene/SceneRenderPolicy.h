#pragma once

#include <vector>
#include <memory>
#include <unordered_set>

#include "SceneManager.h"
#include "Haruka/Tools/ObjectTypes.h"

namespace Haruka::Core::Scene {

enum class RenderKind {
    None,
    Model,
    MeshComponent,
    Primitive
};

struct RenderCommand {
    const SceneObject* object = nullptr;
    RenderKind kind = RenderKind::None;
    Tools::PrimitiveType primitive = Tools::PrimitiveType::NONE;
    // ¿Va por el pase INSTANCIADO? Se decide UNA vez, al construir la cola. Antes se preguntaba a
    // `properties` (un mapa JSON con clave de CADENA) por CADA objeto y CADA frame: con una ciudad
    // son decenas de miles de búsquedas por cadena por frame, y eso se veía en el perfil.
    bool instanced = false;
    // Identidad del objeto COPIADA aquí: permite depurar la cola sin desreferenciar `object`, que
    // puede haber sido destruido entre que se encoló y se revisa.
    uint64_t uid = 0;
};

std::vector<RenderCommand> buildSceneRenderQueue(const SceneManager& scene);

/** @brief Clasifica UN objeto (tipo de render + primitiva + si va instanciado). Es la parte CARA de
 *  construir la cola —comparaciones de cadena y consultas al mapa de propiedades—, así que se expone
 *  para poder hacerlo SOLO con los objetos nuevos en vez de reclasificar la escena entera. */
RenderCommand classifySceneObject(const SceneObject& obj);

/** @brief Lo que la cola incremental recuerda entre frames (ver `syncSceneRenderQueue`). */
struct RenderQueueState {
    std::size_t staticCount = 0;                    ///< entradas de la escena; detrás van las de un frame
    std::unordered_set<uint64_t> queued, present;   ///< por UID, no por puntero
};

/** @brief Pone la cola al día con el conjunto de objetos: quita los que ya no están y clasifica SOLO
 *  los nuevos. Deja `queue` con `st.staticCount` entradas. La usa `Application::buildRenderQueue`. */
void syncSceneRenderQueue(const std::vector<std::shared_ptr<SceneObject>>& objects,
                          std::vector<RenderCommand>& queue, RenderQueueState& st);

} // namespace Haruka::Core::Scene