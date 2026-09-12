#pragma once
/**
 * @file tools/prefab_edit_panel.h
 * @brief EDITAR UN PREFABRICADO: las piezas de un montaje, con su miniatura, en un panel de ImGui.
 *
 * ── POR QUÉ ESTÁ EN EL MOTOR Y NO EN EL EDITOR ──────────────────────────────────────────────────
 *
 * Empezó en `haruka/src/panels/` y ahí estaba mal por la misma razón que lo estaba `preview_render`
 * antes de moverse: **el juego no enlaza el editor**. Un panel de prefabricados que sólo existe en
 * el IDE obliga a salir del juego para tocar un montaje, y condena a que el día que el juego quiera
 * montar algo (un taller, un banco de vehículos) haya que escribir un SEGUNDO editor que diverja del
 * primero — que es exactamente la historia de las tarjetas de color contra las miniaturas.
 *
 * Aquí lo genérico: la lista, la selección, las poses, el guardado y la miniatura. Lo que cambia
 * entre anfitriones se INYECTA:
 *
 *   · `setPrefabDir`     — dónde están los `.json`. El editor los saca del proyecto abierto; el
 *                          juego, de sus assets.
 *   · `setGeoProvider`   — cómo se dibuja un item. El juego sabe de materiales y de formas
 *                          procedurales por semilla; el editor sólo del `.glb` y del tamaño. El
 *                          motor no sabe qué es "piedra" — misma frontera que `PreviewGeoOf`.
 *
 * Y lo que NO puede ser común se queda fuera a propósito: **el gizmo 3D**. El editor tiene ImGuizmo
 * sobre su viewport; el juego no tiene ni una cosa ni la otra. Por eso el panel no manipula la
 * escena: expone qué pieza está seleccionada (`selectedPiece`) y el anfitrión la conecta a lo que
 * tenga. En el editor eso es una línea (`ViewportPanel::setPrefabEdit`).
 */
#include "game/prefab/prefab.h"
#include "tools/preview_render.h"

#include <string>
#include <vector>

#include <glm/glm.hpp>

namespace Haruka {

/** @brief Panel de edición de prefabricados, común a juego y editor. */
class PrefabEditPanel {
public:
    /** @brief Carpeta con los `<nombre>.json`. Cambiarla cierra el montaje abierto. */
    void setPrefabDir(const std::string& dir);

    /** @brief Cómo se dibuja cada item. Sin esto las piezas salen como cajas de 1 m. */
    void setGeoProvider(PreviewGeoOf geoOf) { m_geoOf = std::move(geoOf); }

    /** @brief Dibuja el panel. Llamar dentro del frame de ImGui. */
    void onImGuiRender();

    /**
     * @brief La pieza seleccionada, o `nullptr`. El anfitrión la conecta a su gizmo.
     *
     * ⚠️ EL PUNTERO VALE SÓLO HASTA LA PRÓXIMA LLAMADA A `onImGuiRender`. Apunta DENTRO del vector
     * de piezas: añadir o borrar una lo realoja. Por eso hay que volver a pedirlo cada frame en vez
     * de guardarlo — el panel ya suelta la selección él mismo antes de tocar el vector, pero un
     * anfitrión que se lo guarde de un frame para otro se salta esa garantía.
     */
    PrefabPiece* selectedPiece();
    /** @brief Origen del montaje en el mundo: sus poses son LOCALES y sin ancla no significan nada. */
    const glm::dvec3& origin() const { return m_origin; }
    void setOrigin(const glm::dvec3& o) { m_origin = o; }

    /**
     * @brief Suelta la pieza seleccionada.
     *
     * ⚠️ LO PIDE QUIEN MANEJA EL GIZMO. Mientras haya pieza seleccionada, el anfitrión se la cede al
     * gizmo — y el gizmo es UNO: si el usuario pasa a tocar un objeto de la escena y la selección de
     * aquí sigue viva, el gizmo se queda con la pieza y el objeto no tiene con qué moverse. Desde
     * fuera eso se ve como "el gizmo ha desaparecido".
     */
    void clearSelection() { m_selected = -1; }

    /** @brief Selecciona una pieza por índice y arrastra la lista hasta ella. La usa el anfitrión
     *  cuando la pieza la elige algo que no es esta lista (picar en el viewport, por ejemplo). */
    void selectPiece(int index) { m_selected = index; m_scrollToSelected = true; }

    /** @brief ¿Hay un montaje abierto? */
    bool isOpen() const { return !m_openName.empty(); }
    /** @brief Nombre del montaje abierto ("" si ninguno). */
    const std::string& openName() const { return m_openName; }

private:
    void rescan();
    void openPrefab(const std::string& name);
    void savePrefabFile();
    /// El dato ha cambiado: la miniatura cacheada ya no lo describe.
    void touch();

    PreviewGeoOf m_geoOf;

    std::string m_prefabDir;
    std::vector<std::string> m_available;      ///< nombres (sin .json) de la carpeta

    Prefab      m_prefab;
    std::string m_openName;
    int         m_selected = -1;
    bool        m_dirty = false;
    glm::dvec3  m_origin{0.0};

    char m_newItemId[128] = {0};
    /// Filtro por item. Con 870 tablas, buscar "la tabla que he movido" a ojo no es una operación.
    char m_filter[64] = {0};
    /// Pedir que la lista se desplace hasta la pieza seleccionada (la eligió algo de fuera).
    bool m_scrollToSelected = false;
};

} // namespace Haruka
