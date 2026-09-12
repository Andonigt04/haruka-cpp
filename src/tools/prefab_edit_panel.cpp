// Motor — panel de edición de prefabricados. Ver prefab_edit_panel.h.
//
// ⚠️ DOS COSAS SE LLAMAN "PREFAB" Y NO SON LA MISMA. El editor guarda ESCENAS con extensión
// `.prefab` (lo que abre "Enter Prefab" en su jerarquía). Esto edita `Haruka::Prefab`: una lista de
// PIEZAS con item y pose (`assets/data/prefabs/<x>.json`), que es lo que se coloca como vehículo o
// estructura, lo que dibuja `previewPrefabGeo` y lo que valida el DGS.
#include "tools/prefab_edit_panel.h"

#include <imgui.h>

#include <algorithm>
#include <vector>
#include <cstdio>
#include <filesystem>

#include <glm/gtc/quaternion.hpp>

namespace fs = std::filesystem;

namespace Haruka {
namespace {

/// Clave de caché de la miniatura del CONJUNTO. Prefijo propio: la clave "prefab:<nombre>" ya la usa
/// el inventario del juego, y compartirla haría que editar aquí cambiara el icono de allí.
std::string prefabKey(const std::string& name) { return "prefabedit:" + name; }

} // namespace

void PrefabEditPanel::setPrefabDir(const std::string& dir) {
    if (dir == m_prefabDir) return;
    m_prefabDir = dir;
    m_prefab = {};
    m_openName.clear();
    m_selected = -1;
    m_dirty = false;
    rescan();
}

PrefabPiece* PrefabEditPanel::selectedPiece() {
    if (m_selected < 0 || m_selected >= (int)m_prefab.pieces.size()) return nullptr;
    return &m_prefab.pieces[m_selected];
}

void PrefabEditPanel::touch() {
    m_dirty = true;
    if (!m_openName.empty()) invalidatePreview(prefabKey(m_openName));
}

void PrefabEditPanel::rescan() {
    m_available.clear();
    if (m_prefabDir.empty()) return;
    std::error_code ec;
    if (!fs::is_directory(m_prefabDir, ec)) return;
    for (const auto& e : fs::directory_iterator(m_prefabDir, ec)) {
        if (!e.is_regular_file() || e.path().extension() != ".json") continue;
        m_available.push_back(e.path().stem().string());
    }
    std::sort(m_available.begin(), m_available.end());
}

void PrefabEditPanel::openPrefab(const std::string& name) {
    m_selected = -1;                       // el anfitrión pide la pieza cada frame: se queda sin ella
    m_prefab = {};
    m_openName.clear();
    m_dirty = false;
    if (m_prefabDir.empty()) return;
    if (!loadPrefab(m_prefabDir + "/" + name + ".json", m_prefab)) return;
    m_openName = name;
    invalidatePreview(prefabKey(name));     // por si quedaba la de una sesión anterior
}

void PrefabEditPanel::savePrefabFile() {
    if (m_openName.empty() || m_prefabDir.empty()) return;
    if (savePrefab(m_prefabDir + "/" + m_openName + ".json", m_prefab)) m_dirty = false;
}

void PrefabEditPanel::onImGuiRender() {
    if (!ImGui::Begin("Prefabricado")) { ImGui::End(); return; }

    if (m_prefabDir.empty()) {
        ImGui::TextWrapped("Sin carpeta de prefabricados. El anfitrión tiene que llamar a "
                           "setPrefabDir() — en el editor, al abrir un proyecto.");
        ImGui::End();
        return;
    }

    // ── QUÉ MONTAJE ─────────────────────────────────────────────────────────────────────────────
    ImGui::SetNextItemWidth(220.0f);
    const char* label = m_openName.empty() ? "(ninguno)" : m_openName.c_str();
    if (ImGui::BeginCombo("##prefab", label)) {
        for (const std::string& n : m_available)
            if (ImGui::Selectable(n.c_str(), n == m_openName)) openPrefab(n);
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("Recargar lista")) rescan();
    if (m_available.empty())
        ImGui::TextDisabled("No hay ningún .json en %s", m_prefabDir.c_str());

    if (m_openName.empty()) { ImGui::End(); return; }

    ImGui::SameLine();
    ImGui::BeginDisabled(!m_dirty);
    if (ImGui::Button("Guardar")) savePrefabFile();
    ImGui::EndDisabled();
    if (m_dirty) { ImGui::SameLine(); ImGui::TextColored(ImVec4(1,0.7f,0.2f,1), "sin guardar"); }

    ImGui::Separator();

    // Cómo se dibuja un item lo pone el ANFITRIÓN. Sin proveedor, caja: se ve el montaje, no el arte.
    auto geoOf = [this](const std::string& itemId) -> PreviewGeo {
        if (m_geoOf) {
            PreviewGeo g = m_geoOf(itemId);
            if (!g.empty()) return g;
        }
        PreviewGeo g;
        previewAddBox(g, glm::vec3(0.25f), glm::vec3(0.78f, 0.76f, 0.72f));
        return g;
    };

    // ── EL CONJUNTO ─────────────────────────────────────────────────────────────────────────────
    // La MISMA función que dibuja el icono de un item en la casilla del inventario. Una segunda
    // forma de dibujar un prefabricado es lo que hace que dos sitios enseñen cosas distintas.
    if (previewWorks()) {
        const uint64_t tex = previewTexture(prefabKey(m_openName),
                                            [this, &geoOf] { return previewPrefabGeo(m_prefab, geoOf); },
                                            192);
        if (tex) ImGui::Image((ImTextureID)(intptr_t)tex, ImVec2(192, 192));
        else     ImGui::TextDisabled("(miniatura en el siguiente frame)");
    } else {
        ImGui::TextDisabled("El render de miniaturas no está disponible aquí.");
    }
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::Text("%s", m_prefab.name.empty() ? m_openName.c_str() : m_prefab.name.c_str());
    ImGui::Text("%zu piezas · %zu anclajes", m_prefab.pieces.size(), m_prefab.anchors.size());
    ImGui::TextDisabled("Las poses son LOCALES al montaje.");
    ImGui::EndGroup();

    ImGui::Separator();

    // ── LAS PIEZAS ──────────────────────────────────────────────────────────────────────────────
    //
    // ⚠️ CON CLIPPER Y CON FILTRO, Y LAS DOS COSAS SON POR LO MISMO: **un montaje de verdad tiene
    // cientos de piezas**. El tanque tiene 162 y el casco del barco 870, y la versión de antes
    // dibujaba TODAS cada frame — cada una con su `ImGui::Image`, que en ImGui es un cambio de
    // textura y por tanto un lote de dibujo propio. 162 lotes por frame para enseñar seis filas.
    // Andoni lo reportó como "no puedo editar cómodamente" con el perfilador delante.
    //
    // El `ImGuiListClipper` dibuja SOLO lo visible (~6 filas), y con ello desaparecen también las
    // ~160 llamadas a `previewTexture` por frame: la caché las resolvía rápido, pero cada una pide
    // igualmente el id de textura al backend.
    const float kThumb = 32.0f;
    ImGui::SetNextItemWidth(220.0f);
    ImGui::InputTextWithHint("##filtro", "filtrar por item…", m_filter, sizeof(m_filter));
    ImGui::SameLine();
    ImGui::TextDisabled("%zu piezas", m_prefab.pieces.size());

    // El filtro se resuelve ANTES del clipper: éste necesita saber cuántas filas hay de verdad.
    std::vector<int> visibles;
    visibles.reserve(m_prefab.pieces.size());
    for (int i = 0; i < (int)m_prefab.pieces.size(); ++i)
        if (!m_filter[0] || m_prefab.pieces[i].id.find(m_filter) != std::string::npos)
            visibles.push_back(i);

    ImGui::BeginChild("piezas", ImVec2(0, 220), true);
    ImGuiListClipper clipper;
    clipper.Begin((int)visibles.size(), kThumb + ImGui::GetStyle().ItemSpacing.y);
    while (clipper.Step()) {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
            const int i = visibles[row];
            const PrefabPiece& p = m_prefab.pieces[i];
            ImGui::PushID(i);
            if (previewWorks()) {
                const uint64_t t = previewTexture("item:" + p.id, [&geoOf, &p] { return geoOf(p.id); },
                                                  (int)kThumb * 2);
                if (t) { ImGui::Image((ImTextureID)(intptr_t)t, ImVec2(kThumb, kThumb)); ImGui::SameLine(); }
            }
            char rowTxt[256];
            std::snprintf(rowTxt, sizeof(rowTxt), "%d · %s  (%.2f, %.2f, %.2f)%s",
                          i, p.id.c_str(), p.pos.x, p.pos.y, p.pos.z, p.mechanism ? "  [mec]" : "");
            if (ImGui::Selectable(rowTxt, i == m_selected, 0, ImVec2(0, kThumb))) m_selected = i;
            ImGui::PopID();
        }
    }
    // ⚠️ El clipper sólo dibuja lo visible, así que la pieza SELECCIONADA puede quedar fuera de la
    // lista y perderse de vista. Cuando la selección cambia por fuera (el gizmo, un borrado), se
    // arrastra la lista hasta ella en vez de dejar al usuario buscándola entre 870.
    if (m_scrollToSelected) {
        m_scrollToSelected = false;
        for (size_t r = 0; r < visibles.size(); ++r)
            if (visibles[r] == m_selected) {
                ImGui::SetScrollY((float)r * (kThumb + ImGui::GetStyle().ItemSpacing.y));
                break;
            }
    }
    ImGui::EndChild();

    // ── LA PIEZA SELECCIONADA ───────────────────────────────────────────────────────────────────
    if (PrefabPiece* p = selectedPiece()) {
        ImGui::Text("Pieza %d — %s", m_selected, p->id.c_str());

        glm::vec3 pos = glm::vec3(p->pos);
        if (ImGui::DragFloat3("Posición (m)", &pos.x, 0.01f)) p->pos = glm::dvec3(pos);
        // Invalidar la miniatura al SOLTAR, no por frame: cada invalidación recrea un render target.
        if (ImGui::IsItemDeactivatedAfterEdit()) touch();

        glm::vec3 euler = glm::degrees(glm::eulerAngles(glm::quat(p->rot)));
        if (ImGui::DragFloat3("Rotación (grados)", &euler.x, 0.5f))
            p->rot = glm::dquat(glm::quat(glm::radians(euler)));
        if (ImGui::IsItemDeactivatedAfterEdit()) touch();

        if (ImGui::Checkbox("Mecanismo (no funde islas rígidas)", &p->mechanism)) touch();

        float arc = (float)p->arc;
        if (ImGui::DragFloat("Arco (grados)", &arc, 0.1f)) p->arc = arc;
        if (ImGui::IsItemDeactivatedAfterEdit()) touch();

        if (ImGui::Button("Borrar pieza")) {
            // El vector se realoja: la selección se suelta ANTES. Quien tenga el puntero lo vuelve a
            // pedir en el siguiente frame y le sale `nullptr`, que es lo correcto.
            const int k = m_selected;
            m_selected = -1;
            m_prefab.pieces.erase(m_prefab.pieces.begin() + k);
            touch();
        }
    } else {
        ImGui::TextDisabled("Selecciona una pieza para editarla.");
    }

    ImGui::Separator();
    ImGui::SetNextItemWidth(200.0f);
    ImGui::InputText("itemId", m_newItemId, sizeof(m_newItemId));
    ImGui::SameLine();
    if (ImGui::Button("Añadir pieza") && m_newItemId[0]) {
        m_selected = -1;                      // idem: el vector puede realojarse al crecer
        PrefabPiece np;
        np.id = m_newItemId;
        m_prefab.pieces.push_back(np);
        m_selected = (int)m_prefab.pieces.size() - 1;
        touch();
    }

    ImGui::End();
}

} // namespace Haruka
