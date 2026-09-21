#pragma once
/**
 * @file tools/world_edit_panel.h
 * @brief EL PINCEL DEL MUNDO: un panel de ImGui sobre `VoxWorld` — trazos con forma, deshacer,
 *        y colocar cuevas e islas. Ver `docs/guides/PLAN_PINCEL_MUNDO.md`.
 *
 * Vive en el MOTOR y no en el editor por lo mismo que `prefab_edit_panel`: el juego no enlaza el
 * editor, y Andoni quiere poder tocar el mundo "desde el juego, sin tener que editarlo". Aquí está
 * todo lo que es común: los parámetros del pincel, el gesto (clic/arrastre → trazo), el historial y
 * la creación de definiciones de cueva/isla. Lo que NO puede ser común se queda fuera y el
 * anfitrión lo aporta:
 *
 *   · EL RAYO: el anfitrión sabe de su cámara y su viewport; el panel recibe el punto de impacto
 *     (`hover`/`press`/`drag`) ya calculado con `VoxWorld::raycast`.
 *   · EL DIBUJO del indicador: el panel dice dónde y cómo (`indicator()`), el anfitrión lo pinta.
 *   · LA ESCENA: al crear una cueva o isla el panel la pone en el `VoxWorld` (se ve al instante) y
 *     avisa (`onCaveCreated`/`onIslandCreated`) para que el anfitrión añada la entrada al `.scene`
 *     (o el juego, a su partida). Sin anfitrión, queda sólo en memoria.
 */
#include "world/vox/vox_world.h"
#include "rhi/rhi_resources.h"

#include <functional>
#include <string>
#include <vector>

#include <glm/glm.hpp>

namespace Haruka {

class WorldEditPanel {
public:
    enum class Tool { None, Brush, PlaceCave, PlaceIsland };

    /** @brief El mundo a editar y el centro de su planeta (los puntos del anfitrión llegan en
     *  coordenadas de MUNDO; `VoxWorld` habla relativo al centro). Nulo = panel inerte. */
    void setWorld(VoxWorld* world, const glm::dvec3& planetCenter, double planetRadiusM);
    VoxWorld* world() const { return m_world; }

    /** @brief Carpeta (relativa al proyecto, p. ej. `assets/vox/`) donde se guardan los PNG de las
     *  cuevas e islas nuevas. Vacía = borradores sin fichero. */
    void setAssetDir(const std::string& dir) { m_assetDir = dir; }
    /** @brief Modo diseñador: los trazos van al MUNDO (carpeta `voxEdits`), no a la partida. */
    void setDesignerMode(bool on) { m_designer = on; if (m_world) m_world->setDesignerMode(on); }

    void setOnCaveCreated(std::function<void(const CaveDef&)> fn)     { m_onCave = std::move(fn); }
    void setOnIslandCreated(std::function<void(const IslandDef&)> fn) { m_onIsland = std::move(fn); }
    void setOnCaveRemoved(std::function<void(const std::string&)> fn)   { m_onCaveRemoved = std::move(fn); }
    void setOnIslandRemoved(std::function<void(const std::string&)> fn) { m_onIslandRemoved = std::move(fn); }
    /** @brief Algo cambió que la escena/partida debería guardar. */
    void setOnChanged(std::function<void()> fn) { m_onChanged = std::move(fn); }

    /** @brief Dibuja el panel. Llamar dentro del frame de ImGui. */
    void onImGuiRender();

    Tool tool() const { return m_tool; }
    void setTool(Tool t) { m_tool = t; }
    const VoxWorld::Brush& brush() const { return m_brush; }
    bool removing() const { return m_remove; }

    // ── EL GESTO (lo llama el anfitrión con puntos de MUNDO) ────────────────────────────────────
    /** @brief El cursor está sobre este punto del campo (o no hay impacto). Sólo actualiza el indicador. */
    void hover(bool hit, const glm::dvec3& worldPos);
    /** @brief Botón pulsado sobre el punto: un trazo (o una cueva/isla nueva, según la herramienta). */
    void press(const glm::dvec3& worldPos);
    /** @brief Arrastre con el botón pulsado: un trazo cada `radio·spacing` m recorridos, no por frame. */
    void drag(const glm::dvec3& worldPos);
    void release() { m_dragging = false; }

    /** @brief Lo que el anfitrión tiene que pintar. */
    struct Indicator {
        bool       visible = false;
        glm::dvec3 center{0.0};        ///< mundo
        glm::dvec3 up{0.0, 1.0, 0.0};  ///< vertical local (eje del cilindro/caja)
        float      radiusM = 0.0f, halfM = 0.0f;
        int        shape = 0;          ///< 0 esfera · 1 cilindro · 2 caja · 3 caja de cueva · 4 caja de isla
        bool       remove = true;      ///< color: verde quitar / naranja añadir
    };
    const Indicator& indicator() const { return m_ind; }

    // ── Colocar ─────────────────────────────────────────────────────────────────────────────────
    /** @brief Parámetros con los que se crea la próxima cueva/isla (editables en el panel). */
    CaveDef&   nextCave()   { return m_nextCave; }
    IslandDef& nextIsland() { return m_nextIsland; }
    /** @brief Crea una cueva/isla en `dir` (unitaria, desde el centro del planeta) con los
     *  parámetros actuales: nombre único, PNG en `assetDir`, y la coloca en el mundo. */
    bool createCaveAt(const glm::dvec3& dir);
    bool createIslandAt(const glm::dvec3& dir);

    /** @brief Carpeta de horneados derivados (`bakes/`), para rehornear tras pintar. Por defecto,
     *  la del mundo. */
    void setBakesDir(const std::string& dir) { m_bakesDir = dir; }

    // ── Mapas (también sin ratón: el anfitrión puede pintar por programa) ───────────────────────
    void selectCave(int index)   { m_selectedCave = index; m_selectedIsland = -1; }
    void selectIsland(int index) { m_selectedIsland = index; m_selectedCave = -1; }
    /** @brief Pinta en el mapa `mapIndex` (0 planta · 1 perfil/cima · 2 raíz) de la selección, en
     *  (u, t) ∈ [0,1]², con el valor/radio/dureza actuales. Carga los mapas si hace falta. */
    void paintMap(int mapIndex, float u, float t);
    void setPaint(float value, float radiusTexels, float hardness) { m_paintValue = value; m_paintRadiusTx = radiusTexels; m_paintHardness = hardness; }
    /** @brief Guarda los PNG de la selección en sus rutas y la vuelve a colocar (rehornea). */
    bool rebakeSelected();
    /** @brief Conectividad del último rehorneado (x = componentes antes, y = después, z = vóxeles de aire). */
    glm::ivec3 lastRebakeStats() const { return m_lastStats; }

private:
    void renderBrushTab();
    void renderPlaceTab();
    // ── Mapas: pintar los PNG de la cueva/isla seleccionada ────────────────────────────────────
    // Cada mapa es una imagen de ImGui sobre una textura del RHI que se recrea cuando se pinta.
    // Pintar cambia los texels en memoria; "Rehornear" los guarda en sus rutas y vuelve a colocar
    // la cueva/isla (la clave del derivado lleva el hash de los texels: se rehace solo).
    struct MapView {
        CaveMap  map;
        RHI::TextureHandle tex{}, texOld{};   // la vieja se aparca un frame (command buffer en vuelo)
        uint64_t imTex = 0;                   // ImTextureID de `tex`
        bool     dirty = true;                // hay que subirla
        bool     mask = false;          // isla: la planta se edita como MÁSCARA (dentro/fuera), no como distancia
    };
    void renderMapsTab();
    void loadMapsFor(int caveIndex, int islandIndex);
    void uploadMap(MapView& v);
    void paintOn(MapView& v, float u, float t);
    void releaseMaps();
    void ensureMapsLoaded();

    std::string m_bakesDir;
    MapView     m_maps[3];
    int         m_mapsCave = -1, m_mapsIsland = -1;   // qué está cargado en `m_maps`
    int         m_mapCount = 0;
    float       m_paintValue = 1.0f, m_paintRadiusTx = 8.0f, m_paintHardness = 0.6f;
    int         m_paintMode = 0;          // 0 pincel · 1 corredor (dos clics)
    bool        m_corridorHasA = false; float m_corridorAu = 0, m_corridorAv = 0;
    glm::ivec3  m_lastStats{0, 0, 0};     // conectividad del último rehorneado
    bool        m_mapsEdited = false;
    void applyBrush(const glm::dvec3& worldPos);
    std::string uniqueName(const char* base, bool cave) const;

    VoxWorld*   m_world = nullptr;
    glm::dvec3  m_center{0.0};
    double      m_radius = 0.0;
    std::string m_assetDir;
    bool        m_designer = false;

    Tool            m_tool = Tool::None;
    VoxWorld::Brush m_brush{ VoxWorld::Brush::Sphere, 6.0f, 6.0f };
    bool            m_remove = true;
    float           m_spacing = 0.5f;     ///< fracción del radio entre trazos al arrastrar
    bool            m_dragging = false;
    glm::dvec3      m_lastStroke{0.0};
    Indicator       m_ind;
    int             m_lastChanged = 0;    ///< vóxeles del último trazo (se muestra)

    CaveDef   m_nextCave;
    IslandDef m_nextIsland;
    int       m_selectedCave = -1, m_selectedIsland = -1;

    std::function<void(const CaveDef&)>     m_onCave;
    std::function<void(const IslandDef&)>   m_onIsland;
    std::function<void(const std::string&)> m_onCaveRemoved, m_onIslandRemoved;
    std::function<void()>                   m_onChanged;
};

} // namespace Haruka
