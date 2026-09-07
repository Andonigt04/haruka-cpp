#pragma once
/**
 * @file tools/preview_render.h
 * @brief MINIATURA 3D de cualquier geometría, a una textura que la UI dibuja como imagen.
 *
 * Nació en el inventario del juego (una casilla no enseña un icono 2D: enseña el objeto) y vivía
 * ahí, en el proyecto. El editor no podía llamarlo, así que su panel de objetos dibujaba tarjetas
 * con un CUADRADO DE COLOR y la inicial del tipo — dos representaciones del mismo objeto, condenadas
 * a divergir. Esto es lo único que juego y editor comparten, así que es donde tiene que estar.
 *
 * Lo de aquí es lo GENÉRICO: encuadrar, crear el render target, un UBO por dibujo, el pase y la
 * caché. De dónde sale la malla lo decide quien llama, porque eso sí es suyo: el juego sabe de
 * materiales y de formas procedurales por semilla, y el editor sabe de `modelPath`.
 *
 * ⚠️ EL id QUE DEVUELVE ES UN `ImTextureID`, no un nombre de textura de GL. Devolver el GLuint
 * funcionaba solo con ImGui_ImplOpenGL3 —donde coinciden— y dejaba la UI sin iconos bajo Vulkan, que
 * quiere un VkDescriptorSet. Lo traduce el RHI y el valor sirve en los dos backends.
 */
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "game/prefab/prefab.h"

namespace Haruka {

/** @brief Malla intercalada tal y como la espera el pipeline: pos3 + nrm3 + col3. */
struct PreviewGeo {
    std::vector<float>        data;
    std::vector<unsigned int> idx;
    bool empty() const { return idx.empty(); }
};

/** @brief Añade vértices con sus normales, colores e índices (los índices se rebasan solos). */
void previewAddMesh(PreviewGeo& g,
                    const std::vector<glm::vec3>& pos,
                    const std::vector<glm::vec3>& nrm,
                    const std::vector<glm::vec3>& col,
                    const std::vector<unsigned int>& idx);

/** @brief Una caja de semiejes `half` con un color: el respaldo de las formas rectas. */
void previewAddBox(PreviewGeo& g, const glm::vec3& half, const glm::vec3& col);

/** @brief Añade `src` a `dst` con una pose. Las posiciones se giran y trasladan; las NORMALES sólo
 *  se giran — trasladarlas es el error clásico que deja el sombreado plano. */
void previewAddPosed(PreviewGeo& dst, const PreviewGeo& src,
                     const glm::dvec3& pos, const glm::dquat& rot);

/** @brief La malla de un `.glb` ya cargado por el motor, con un color plano. Vacía si no se carga. */
PreviewGeo previewModelGeo(const std::string& modelPath, const glm::vec3& tint);

/** @brief Cómo llegar de un itemId a su geometría. La pone quien llama: el juego mira materiales y
 *  formas procedurales, el editor se queda con el `.glb`. El motor no sabe qué es "piedra". */
using PreviewGeoOf = std::function<PreviewGeo(const std::string& itemId)>;

/** @brief Un PREFABRICADO entero: sus piezas compuestas en sus poses.
 *
 *  Las mallas se cachean por itemId DENTRO de la llamada: un casco de 43 piezas usa cinco items
 *  distintos, y sin eso se trocearía el mismo `.glb` cuarenta veces para una imagen de 128 px. */
PreviewGeo previewPrefabGeo(const Prefab& pf, const PreviewGeoOf& geoOf);

/**
 * @brief Renderiza una geometría a una textura cacheada por `key`, y devuelve su `ImTextureID`.
 *
 * `make` se llama SÓLO si no está en caché. Puede devolver 0 en los primeros frames si el backend de
 * UI aún no está activo: no es un fallo, la siguiente llamada lo resuelve. Llamar durante el dibujo
 * de la UI — usa el Context del frame en curso.
 *
 * Las claves son libres: pon un prefijo por familia ("item:", "prefab:", "obj:") o un item y un
 * prefabricado que se llamen igual compartirán textura.
 */
uint64_t previewTexture(const std::string& key, const std::function<PreviewGeo()>& make, int px);

/** @brief ¿Llega a pintar algo en esta máquina? Si el pipeline no se pudo crear, no tiene sentido
 *  pagar un render target por objeto para no ver nada: la UI debe degradar a un icono plano. */
bool previewWorks();

/** @brief Libera texturas y buffers de la caché. Antes de destruir el contexto. */
void clearPreviews();

} // namespace Haruka
