#pragma once

#include <vector>
#include <memory>

#include "scene_manager.h"
#include "tools/object_types.h"

namespace Haruka {

enum class RenderKind {
    None,
    Model,
    MeshComponent,
    Primitive
};

struct RenderCommand {
    const SceneObject* object = nullptr;
    RenderKind kind = RenderKind::None;
    PrimitiveType primitive = PrimitiveType::NONE;
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

}