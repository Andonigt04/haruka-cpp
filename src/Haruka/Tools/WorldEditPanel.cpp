/**
 * @file Tools/WorldEditPanel.cpp
 * @brief El pincel del mundo. Ver `WorldEditPanel.h`.
 */
#include "Haruka/Tools/WorldEditPanel.h"

#include <imgui.h>

#include "Haruka/World/Terrain/BakeUtil.h"
#include "Haruka/Core/Logger.h"
#include "Haruka/RHI/RHIContext.h"
#include "Haruka/RHI/RHIDevice.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace Haruka::Tools {

void WorldEditPanel::setWorld(World::Vox::VoxWorld* world, const glm::dvec3& planetCenter, double planetRadiusM) {
    m_world = world; m_center = planetCenter; m_radius = planetRadiusM;
    if (m_world) m_world->setDesignerMode(m_designer);
    if (m_world && m_bakesDir.empty()) m_bakesDir = m_world->bakesDir();
    releaseMaps();
    m_dragging = false;
    m_ind.visible = false;
}

std::string WorldEditPanel::uniqueName(const char* base, bool cave) const {
    // `cueva_01`, `cueva_02`… el primero que no exista ya en el mundo.
    for (int i = 1; i < 10000; ++i) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%s_%02d", base, i);
        bool taken = false;
        if (m_world) {
            if (cave) { for (const World::Vox::CaveDef& d : m_world->caveDefs()) if (d.name == buf) taken = true; }
            else      { for (const World::Vox::IslandDef& d : m_world->islandDefs()) if (d.name == buf) taken = true; }
        }
        if (!taken) return buf;
    }
    return base;
}

// ── El gesto ────────────────────────────────────────────────────────────────────────────────────

void WorldEditPanel::hover(bool hit, const glm::dvec3& worldPos) {
    m_hoverHit = hit; if (hit) m_hoverPos = worldPos;
    m_ind.visible = hit && m_world && m_tool != Tool::None;
    if (!m_ind.visible) return;
    m_ind.center = worldPos;
    const glm::dvec3 rel = worldPos - m_center;
    const double r = glm::length(rel);
    m_ind.up = r > 1e-9 ? rel / r : glm::dvec3(0.0, 1.0, 0.0);
    m_ind.remove = (m_brush.op == World::Vox::VoxWorld::Brush::Op::Remove || m_brush.op == World::Vox::VoxWorld::Brush::Op::Add) ? m_remove : (m_brush.op == World::Vox::VoxWorld::Brush::Op::Lower);
    switch (m_tool) {
        case Tool::Brush:
            m_ind.shape = (int)m_brush.shape; m_ind.radiusM = m_brush.radiusM; m_ind.halfM = m_brush.halfM;
            if (m_ind.shape == 3) m_ind.shape = 0;   // el túnel se indica como la esfera de su extremo
            if (m_brush.op == World::Vox::VoxWorld::Brush::Op::Raise || m_brush.op == World::Vox::VoxWorld::Brush::Op::Lower) { m_ind.shape = 1; m_ind.halfM = m_brush.amountM; }
            if (m_brush.op == World::Vox::VoxWorld::Brush::Op::Sculpt) { m_ind.shape = 0; m_ind.remove = m_brush.amountM < 0.0f; }
            break;
        case Tool::PlaceCave:
            m_ind.shape = 3; m_ind.radiusM = m_nextCave.radiusM; m_ind.halfM = m_nextCave.depthM; break;
        case Tool::PlaceIsland:
            m_ind.shape = 4; m_ind.radiusM = m_nextIsland.radiusM; m_ind.halfM = m_nextIsland.altitudeM; break;
        default: break;
    }
}

void WorldEditPanel::applyBrush(const glm::dvec3& worldPos) {
    if (!m_world) return;
    World::Vox::VoxWorld::Brush b = m_brush;
    if (b.op == World::Vox::VoxWorld::Brush::Op::Remove || b.op == World::Vox::VoxWorld::Brush::Op::Add) b.op = m_remove ? World::Vox::VoxWorld::Brush::Op::Remove : World::Vox::VoxWorld::Brush::Op::Add;
    // La cápsula (túnel) va del trazo anterior a éste: el primer clic es una esfera.
    if (b.shape == World::Vox::VoxWorld::Brush::Shape::Capsule) b.capsuleEnd = (m_dragging ? m_lastStroke : worldPos) - m_center;
    m_lastChanged = m_world->stroke(worldPos - m_center, b);
    m_lastStroke = worldPos;
    if (m_onChanged) m_onChanged();
}

void WorldEditPanel::press(const glm::dvec3& worldPos) {
    if (!m_world) return;
    const glm::dvec3 rel = worldPos - m_center;
    const double r = glm::length(rel);
    if (r < 1e-9) return;
    switch (m_tool) {
        case Tool::Brush:       applyBrush(worldPos); m_dragging = true; break;
        case Tool::PlaceCave:   createCaveAt(rel / r); break;
        case Tool::PlaceIsland: createIslandAt(rel / r); break;
        default: break;
    }
}

void WorldEditPanel::drag(const glm::dvec3& worldPos) {
    if (!m_dragging || m_tool != Tool::Brush) return;
    // Por DISTANCIA recorrida, no por frame: el surco que deja un arrastre no puede depender de
    // los FPS. Un trazo cada `radio · spacing` metros.
    const double paso = std::max(0.5, (double)m_brush.radiusM * (double)m_spacing);
    if (glm::length(worldPos - m_lastStroke) >= paso) applyBrush(worldPos);
}

// ── Colocar ─────────────────────────────────────────────────────────────────────────────────────

bool WorldEditPanel::createCaveAt(const glm::dvec3& dir) {
    if (!m_world) return false;
    World::Vox::CaveDef d = m_nextCave;
    d.name = uniqueName("cueva", true);
    d.centerDir = glm::normalize(dir);
    // Otro borrador cada vez (semilla distinta), aunque los parámetros sean los mismos.
    d.draftSeed = m_nextCave.draftSeed++;
    d.planta.clear(); d.perfil.clear(); d.layers.clear();
    // A new cave is born as LAYERS: the draft from its seed, saved as its 16-bit maps. Without an
    // asset folder it stays a draft in memory (the loader makes the same one from the seed).
    if (!m_assetDir.empty()) {
        const std::string base = (m_assetDir.back() == '/' ? m_assetDir : m_assetDir + "/") + d.name;
        const World::Vox::CaveBox box = World::Vox::caveBoxFromDef(d, m_radius);
        std::vector<World::Vox::CaveLayer> layers;
        World::Vox::caveBakeLayers(box, World::Vox::kCaveMapRes, layers);
        d.layers = World::Vox::caveLayerPathsFor(base, (int)layers.size());
        for (size_t i = 0; i < layers.size(); ++i)
            if (!World::Vox::caveSaveMap16(World::Vox::caveResolveAssetPath(d.layers[i].ceiling), layers[i].ceiling) ||
                !World::Vox::caveSaveMap16(World::Vox::caveResolveAssetPath(d.layers[i].floor), layers[i].floor)) {
                HARUKA_LOGW("Mundo", "could not save the layer maps of '%s': it stays a draft", d.name.c_str());
                d.layers.clear();
                break;
            }
    }
    if (!m_world->placeCave(d)) return false;
    if (m_onCave) m_onCave(d);
    if (m_onChanged) m_onChanged();
    return true;
}

bool WorldEditPanel::createIslandAt(const glm::dvec3& dir) {
    if (!m_world) return false;
    World::Vox::IslandDef d = m_nextIsland;
    d.name = uniqueName("isla", false);
    d.centerDir = glm::normalize(dir);
    if (!m_assetDir.empty()) {
        const std::string base = (m_assetDir.back() == '/' ? m_assetDir : m_assetDir + "/") + d.name;
        d.planta = base + "_planta.png"; d.cima = base + "_cima.png"; d.raiz = base + "_raiz.png";
    } else { d.planta.clear(); d.cima.clear(); d.raiz.clear(); }
    d.draftSeed = m_nextIsland.draftSeed++;
    if (!m_world->placeIsland(d)) return false;
    if (m_onIsland) m_onIsland(d);
    if (m_onChanged) m_onChanged();
    return true;
}

// ── El panel ────────────────────────────────────────────────────────────────────────────────────

void WorldEditPanel::onImGuiRender() {
    if (!ImGui::Begin("Mundo")) { ImGui::End(); return; }
    if (!m_world || !m_world->configured()) {
        ImGui::TextDisabled("Sin planeta con campo volumetrico (abre una escena con un planeta).");
        ImGui::End();
        return;
    }
    ImGui::TextDisabled("%zu chunks · %zu con contenido · %.1f MB · %s",
                        m_world->loadedCount(), m_world->loadedWithContent(), m_world->voxelBytes() / 1048576.0,
                        m_designer ? "trazos al MUNDO" : "trazos a la PARTIDA");
    if (ImGui::BeginTabBar("##mundo")) {
        if (ImGui::BeginTabItem("Pincel")) { renderBrushTab(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Colocar")) { renderPlaceTab(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Mapas")) { renderMapsTab(); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

void WorldEditPanel::renderBrushTab() {
    bool on = (m_tool == Tool::Brush);
    if (ImGui::Checkbox("Pincel activo (clic/arrastre en la vista)", &on)) m_tool = on ? Tool::Brush : Tool::None;
    // EL TIPO: qué hace el pincel. Quitar/Añadir son la unión de distancias de siempre; los de
    // suelo (allanar, subir, bajar) trabajan con la cota del punto de impacto; suavizar promedia.
    static const char* kOps[] = { "Quitar (abre aire)", "Añadir (pone roca)", "Allanar (a la cota del clic)", "Subir el suelo", "Bajar el suelo", "Suavizar", "Esculpir (Blender: +/- con caida, huella y patron)" };
    int op = (int)m_brush.op;
    if (m_brush.op == World::Vox::VoxWorld::Brush::Op::Remove || m_brush.op == World::Vox::VoxWorld::Brush::Op::Add) op = m_remove ? 0 : 1;
    if (ImGui::Combo("Tipo", &op, kOps, 7)) { m_brush.op = (World::Vox::VoxWorld::Brush::Op)op; if (op <= 1) m_remove = (op == 0); }
    const bool suelo = (m_brush.op == World::Vox::VoxWorld::Brush::Op::Flatten || m_brush.op == World::Vox::VoxWorld::Brush::Op::Raise || m_brush.op == World::Vox::VoxWorld::Brush::Op::Lower);
    if (m_brush.op == World::Vox::VoxWorld::Brush::Op::Sculpt) {
        // ESCULPIR: lo que hace un pincel de Blender. Signo = sube/baja; la caída es la curva del
        // centro al borde; la huella puede ser un polígono; el patrón modula el desplazamiento.
        ImGui::SliderFloat("Fuerza (m, +sube / -baja)", &m_brush.amountM, -20.0f, 20.0f, "%+.1f");
        ImGui::SameLine(); if (ImGui::SmallButton("+/-")) m_brush.amountM = -m_brush.amountM;
        static const char* kFall[] = { "Suave", "Esfera", "Raiz", "Aguda", "Lineal", "Constante", "Inv. cuadrado" };
        int fall = (int)m_brush.falloff; if (ImGui::Combo("Caida", &fall, kFall, 7)) m_brush.falloff = (World::Vox::VoxWorld::Brush::Falloff)fall;
        static const char* kSides[] = { "Circulo", "Triangulo", "Cuadrado", "Pentagono", "Hexagono", "Octogono" };
        static const uint8_t kSidesN[] = { 0, 3, 4, 5, 6, 8 };
        int si = 0; for (int i = 0; i < 6; ++i) if (kSidesN[i] == m_brush.sides) si = i;
        if (ImGui::Combo("Huella", &si, kSides, 6)) m_brush.sides = kSidesN[si];
        static const char* kPat[] = { "Ninguno", "Olas", "Anillos", "Hexagonos", "Damero", "Ruido" };
        int pat = (int)m_brush.pattern; if (ImGui::Combo("Patron", &pat, kPat, 6)) m_brush.pattern = (World::Vox::VoxWorld::Brush::Pattern)pat;
        if (m_brush.pattern != World::Vox::VoxWorld::Brush::Pattern::PNone) ImGui::SliderFloat("Paso del patron (m)", &m_brush.patternM, 2.0f, 60.0f, "%.0f");
        ImGui::SliderFloat("Radio (m)##sc", &m_brush.radiusM, 1.0f, 120.0f, "%.1f");
        ImGui::SliderFloat("Dureza del borde##sc", &m_brush.hardness, 0.0f, 1.0f, "%.2f");
        ImGui::SliderFloat("Paso del arrastre (x radio)##sc", &m_spacing, 0.1f, 1.0f, "%.2f");
        ImGui::TextDisabled("El voxel mide ~4 m: patrones de menos de ~8 m de paso salen desdibujados.");
    } else {
    // LA FORMA: la huella. La cápsula es el túnel: une cada trazo del arrastre con el anterior.
    int shape = (int)m_brush.shape;
    ImGui::RadioButton("Esfera", &shape, 0); ImGui::SameLine();
    ImGui::RadioButton("Cilindro", &shape, 1); ImGui::SameLine();
    ImGui::RadioButton("Caja", &shape, 2); ImGui::SameLine();
    ImGui::RadioButton("Tunel (capsula al arrastrar)", &shape, 3);
    m_brush.shape = (World::Vox::VoxWorld::Brush::Shape)shape;
    ImGui::SliderFloat(m_brush.shape == World::Vox::VoxWorld::Brush::Shape::Box ? "Medio lado (m)" : "Radio (m)", &m_brush.radiusM, 1.0f, 60.0f, "%.1f");
    if (m_brush.shape == World::Vox::VoxWorld::Brush::Shape::Cylinder || m_brush.shape == World::Vox::VoxWorld::Brush::Shape::Box)
        ImGui::SliderFloat("Medio alto (m)", &m_brush.halfM, 1.0f, 60.0f, "%.1f");
    if (m_brush.op == World::Vox::VoxWorld::Brush::Op::Raise || m_brush.op == World::Vox::VoxWorld::Brush::Op::Lower)
        ImGui::SliderFloat("Cuanto (m)", &m_brush.amountM, 0.5f, 40.0f, "%.1f");
    if (m_brush.op == World::Vox::VoxWorld::Brush::Op::Smooth)
        ImGui::SliderFloat("Fuerza", &m_brush.amountM, 0.05f, 1.0f, "%.2f");
    if (suelo || m_brush.op == World::Vox::VoxWorld::Brush::Op::Smooth)
        ImGui::SliderFloat("Dureza del borde", &m_brush.hardness, 0.0f, 1.0f, "%.2f");
    ImGui::SliderFloat("Paso del arrastre (x radio)", &m_spacing, 0.1f, 1.0f, "%.2f");
    if (suelo) ImGui::TextDisabled("Allanar/subir/bajar: la huella es la forma vista desde arriba; el alto lo pone el plano.");
    ImGui::TextDisabled("El voxel mide ~4 m: por debajo de eso la forma es aproximada.");
    }
    ImGui::Separator();
    ImGui::BeginDisabled(m_world->undoCount() == 0);
    if (ImGui::Button("Deshacer")) { m_world->undo(); if (m_onChanged) m_onChanged(); }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(m_world->redoCount() == 0);
    if (ImGui::Button("Rehacer")) { m_world->redo(); if (m_onChanged) m_onChanged(); }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("%zu por deshacer · ultimo trazo: %d voxeles", m_world->undoCount(), m_lastChanged);
    if (ImGui::Button("Guardar trazos")) { m_world->saveEdits(); }
    ImGui::SameLine(); ImGui::TextDisabled("(tambien al guardar la escena / la partida)");
}

void WorldEditPanel::renderPlaceTab() {
    int t = (m_tool == Tool::PlaceCave) ? 1 : (m_tool == Tool::PlaceIsland) ? 2 : 0;
    ImGui::RadioButton("Nada", &t, 0); ImGui::SameLine();
    ImGui::RadioButton("Cueva nueva al clic", &t, 1); ImGui::SameLine();
    ImGui::RadioButton("Isla nueva al clic", &t, 2);
    m_tool = (t == 1) ? Tool::PlaceCave : (t == 2) ? Tool::PlaceIsland : (m_tool == Tool::Brush ? Tool::Brush : Tool::None);

    if (ImGui::CollapsingHeader("Cueva nueva", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::SliderFloat("Medio lado (m)##c", &m_nextCave.radiusM, 100.0f, 500.0f, "%.0f");
        ImGui::SliderFloat("Fondo (m)##c", &m_nextCave.depthM, 40.0f, 300.0f, "%.0f");
        ImGui::SliderFloat2("Boca (u, v)", &m_nextCave.mouth.x, 0.1f, 0.9f, "%.2f");
        ImGui::SliderFloat("Boca radio (frac.)", &m_nextCave.mouth.z, 0.02f, 0.12f, "%.3f");
        ImGui::TextDisabled("Los mapas se hornean como borrador y se guardan en %s", m_assetDir.empty() ? "(sin carpeta: no se guardan)" : m_assetDir.c_str());
    }
    if (ImGui::CollapsingHeader("Isla nueva", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::SliderFloat("Medio lado (m)##i", &m_nextIsland.radiusM, 100.0f, 400.0f, "%.0f");
        ImGui::SliderFloat("Altitud sobre la cota (m)", &m_nextIsland.altitudeM, 50.0f, 500.0f, "%.0f");
        ImGui::SliderFloat("Cima (m)", &m_nextIsland.topM, 5.0f, 80.0f, "%.0f");
        ImGui::SliderFloat("Raiz (m)", &m_nextIsland.rootM, 20.0f, 200.0f, "%.0f");
    }
    ImGui::Separator();
    // Lo que hay: listas con quitar. Editar los parametros de una existente va por el objeto de
    // escena (inspector) — la definicion es suya.
    if (ImGui::CollapsingHeader("Cuevas en el mundo", ImGuiTreeNodeFlags_DefaultOpen)) {
        const auto defs = m_world->caveDefs();   // copia: quitar reasigna la lista
        for (size_t i = 0; i < defs.size(); ++i) {
            ImGui::PushID((int)i);
            const World::Vox::CaveDef& d = defs[i];
            if (ImGui::RadioButton("##selc", m_selectedCave == (int)i)) { m_selectedCave = (int)i; m_selectedIsland = -1; }
            ImGui::SameLine();
            ImGui::Text("%s · %.0f x %.0f m · %s", d.name.c_str(), d.radiusM, d.depthM,
                        !d.layers.empty() ? "layers" : (d.planta.empty() ? "draft" : "legacy maps"));
            ImGui::SameLine();
            if (ImGui::SmallButton("Quitar")) {
                const std::string name = d.name;
                m_world->removeCave(name);
                m_selectedCave = -1;
                if (m_onCaveRemoved) m_onCaveRemoved(name);
                if (m_onChanged) m_onChanged();
            }
            ImGui::PopID();
        }
        if (defs.empty()) ImGui::TextDisabled("ninguna");
    }
    if (ImGui::CollapsingHeader("Islas en el mundo", ImGuiTreeNodeFlags_DefaultOpen)) {
        const auto defs = m_world->islandDefs();
        for (size_t i = 0; i < defs.size(); ++i) {
            ImGui::PushID(1000 + (int)i);
            const World::Vox::IslandDef& d = defs[i];
            if (ImGui::RadioButton("##seli", m_selectedIsland == (int)i)) { m_selectedIsland = (int)i; m_selectedCave = -1; }
            ImGui::SameLine();
            ImGui::Text("%s · %.0f m · a %.0f m · %s", d.name.c_str(), d.radiusM, d.altitudeM, d.planta.empty() ? "borrador" : "con mapas");
            ImGui::SameLine();
            if (ImGui::SmallButton("Quitar")) {
                const std::string name = d.name;
                m_world->removeIsland(name);
                m_selectedIsland = -1;
                if (m_onIslandRemoved) m_onIslandRemoved(name);
                if (m_onChanged) m_onChanged();
            }
            ImGui::PopID();
        }
        if (defs.empty()) ImGui::TextDisabled("ninguna");
    }
}

// ── Mapas ───────────────────────────────────────────────────────────────────────────────────────

void WorldEditPanel::releaseMaps() {
    RHI::Device* dev = RHI::device();
    for (MapView& v : m_maps) {
        if (dev && RHI::valid(v.tex)) dev->destroy(v.tex);
        if (dev && RHI::valid(v.texOld)) dev->destroy(v.texOld);
    }
    m_maps.clear();
    m_mapsCave = -1; m_mapsIsland = -1; m_mapsEdited = false; m_corridorHasA = false; m_mapsLossy = 0;
}

void WorldEditPanel::loadMapsFor(int caveIndex, int islandIndex) {
    releaseMaps();
    if (!m_world) return;
    auto loadOrEmpty = [](const std::string& path, World::Vox::CaveMap& m) {
        int w = 0, h = 0; std::vector<float> px;
        const std::string p = World::Vox::caveResolveAssetPath(path);
        if (!p.empty() && World::Terrain::BakeUtil::loadPNGGray(p, w, h, px)) { m.w = w; m.h = h; m.v = px; return true; }
        return false;
    };
    if (caveIndex >= 0 && caveIndex < (int)m_world->caveDefs().size()) {
        const World::Vox::CaveDef d = m_world->caveDefs()[caveIndex];
        // The layers the game would see: its own maps, the legacy ones converted, or the draft.
        World::Vox::CaveSystem sys;
        World::Vox::caveLoad(d, sys, m_radius, m_bakesDir);
        m_mapsCaveBox = sys.box;
        m_mapsLossy = 0;
        // A legacy cave the loader kept as a volume (it does not fit in layers): shown converted
        // anyway, with the loss on screen. Rebaking accepts that loss; discarding keeps the volume.
        if (sys.layers.empty() && !sys.vol.empty()) {
            m_mapsLossy = std::max(1, World::Vox::caveLayersFromVolume(sys, World::Vox::kCaveMapRes, sys.layers));
        }
        for (World::Vox::CaveLayer& L : sys.layers) {
            m_maps.emplace_back(); m_maps.back().map = std::move(L.ceiling);
            m_maps.emplace_back(); m_maps.back().map = std::move(L.floor);
        }
        m_mapsCave = caveIndex;
    } else if (islandIndex >= 0 && islandIndex < (int)m_world->islandDefs().size()) {
        const World::Vox::IslandDef d = m_world->islandDefs()[islandIndex];
        m_maps.resize(3);
        if (!loadOrEmpty(d.planta, m_maps[0].map) || !loadOrEmpty(d.cima, m_maps[1].map) || !loadOrEmpty(d.raiz, m_maps[2].map)) {
            const World::Vox::IslandBox box = World::Vox::islandBoxFromDef(d, m_radius);
            World::Vox::islandBakeMaps(box, World::Vox::kIslandMapRes, m_maps[0].map, m_maps[1].map, m_maps[2].map);
        }
        // La planta de la isla es una DISTANCIA (0,5 = borde): se pinta como máscara dentro/fuera
        // y al rehornear se vuelve a convertir en distancia (chanfle), como hace el horneado.
        for (float& v : m_maps[0].map.v) v = v > 0.5f ? 1.0f : 0.0f;
        m_maps[0].mask = true;
        m_mapsIsland = islandIndex;
    }
    for (MapView& v : m_maps) v.dirty = true;
}

bool WorldEditPanel::addCaveLayer() {
    ensureMapsLoaded();
    if (m_mapsCave < 0 || (int)m_maps.size() / 2 >= World::Vox::kCaveMaxLayers) return false;
    const int w = m_maps.empty() ? World::Vox::kCaveMapRes : m_maps[0].map.w, h = m_maps.empty() ? w : m_maps[0].map.h;
    // Closed everywhere: ceiling at the bottom of the range, floor at the top (no air until painted).
    for (float v : { 1.0f, 0.0f }) {
        m_maps.emplace_back();
        m_maps.back().map.w = w; m_maps.back().map.h = h; m_maps.back().map.v.assign((size_t)w * h, v);
    }
    m_mapsEdited = true;
    return true;
}

void WorldEditPanel::uploadMap(MapView& v) {
    RHI::Device* dev = RHI::device();
    if (!dev || v.map.empty()) return;
    if (RHI::valid(v.texOld)) { dev->destroy(v.texOld); v.texOld = {}; }
    if (RHI::valid(v.tex)) v.texOld = v.tex;
    std::vector<unsigned char> px((size_t)v.map.w * v.map.h * 4);
    for (size_t i = 0; i < v.map.v.size(); ++i) {
        const unsigned char g = (unsigned char)std::lround(std::clamp(v.map.v[i], 0.0f, 1.0f) * 255.0f);
        px[i * 4] = px[i * 4 + 1] = px[i * 4 + 2] = g; px[i * 4 + 3] = 255;
    }
    RHI::TextureDesc td; td.width = (uint32_t)v.map.w; td.height = (uint32_t)v.map.h; td.format = RHI::Format::RGBA8;
    td.filter = RHI::Filter::Nearest; td.wrap = RHI::Wrap::ClampToEdge; td.mipmaps = false; td.initialData = px.data();
    v.tex = dev->createTexture(td);
    v.imTex = dev->imguiTextureId(v.tex);
    v.dirty = false;
}

void WorldEditPanel::paintOn(MapView& v, float u, float t) {
    if (v.map.empty()) return;
    const int cx = (int)std::lround(u * (float)(v.map.w - 1)), cy = (int)std::lround(t * (float)(v.map.h - 1));
    const int r = (int)std::ceil(m_paintRadiusTx);
    for (int y = std::max(0, cy - r); y <= std::min(v.map.h - 1, cy + r); ++y)
        for (int x = std::max(0, cx - r); x <= std::min(v.map.w - 1, cx + r); ++x) {
            const float d = std::sqrt((float)((x - cx) * (x - cx) + (y - cy) * (y - cy))) / std::max(m_paintRadiusTx, 1.0f);
            if (d > 1.0f) continue;
            // Dureza: 1 = disco plano; 0 = cae desde el centro.
            const float w = (d < m_paintHardness) ? 1.0f : 1.0f - (d - m_paintHardness) / std::max(1.0f - m_paintHardness, 1e-3f);
            float& px = v.map.v[(size_t)y * v.map.w + x];
            px = v.mask ? (w > 0.5f ? m_paintValue : px) : px + (m_paintValue - px) * std::clamp(w, 0.0f, 1.0f);
        }
    v.dirty = true; m_mapsEdited = true;
}

bool WorldEditPanel::rebakeSelected() {
    ensureMapsLoaded();
    if (!m_world || m_maps.empty()) return false;
    auto save = [&](const std::string& path, const World::Vox::CaveMap& m) {
        const std::string p = World::Vox::caveResolveAssetPath(path);
        if (p.empty()) return false;
        return World::Terrain::BakeUtil::savePNGGray(p, m.w, m.h, m.v);
    };
    auto basePath = [&](const std::string& name) {
        return (m_assetDir.empty() ? std::string("assets/vox/") : (m_assetDir.back() == '/' ? m_assetDir : m_assetDir + "/")) + name;
    };
    if (m_mapsCave >= 0 && m_mapsCave < (int)m_world->caveDefs().size()) {
        World::Vox::CaveDef d = m_world->caveDefs()[m_mapsCave];
        // Saved as LAYERS: a draft or a legacy cave gets layer paths now, and stops being legacy.
        const int nLayers = (int)m_maps.size() / 2;
        if ((int)d.layers.size() != nLayers) d.layers = World::Vox::caveLayerPathsFor(basePath(d.name), nLayers);
        std::vector<World::Vox::CaveLayer> layers(nLayers);
        for (int i = 0; i < nLayers; ++i) {
            layers[i].ceiling = m_maps[2 * i].map; layers[i].floor = m_maps[2 * i + 1].map;
            if (!World::Vox::caveSaveMap16(World::Vox::caveResolveAssetPath(d.layers[i].ceiling), layers[i].ceiling) ||
                !World::Vox::caveSaveMap16(World::Vox::caveResolveAssetPath(d.layers[i].floor), layers[i].floor)) {
                HARUKA_LOGW("Mundo", "could not save the layer maps of '%s'", d.name.c_str());
                return false;
            }
        }
        d.planta.clear(); d.perfil.clear();
        // Connectivity of what was painted, to show it (x = y: painted caves are not carved).
        int open = 0;
        for (const auto& L : layers) for (size_t k = 0; k < L.ceiling.v.size(); ++k) open += L.floor.v[k] > L.ceiling.v[k] ? 1 : 0;
        const int comps = World::Vox::caveLayerComponents(layers);
        m_lastStats = glm::ivec3(comps, comps, open);
        m_world->removeCave(d.name);
        m_world->placeCave(d);   // mismo nombre, mismas rutas: los chunks se rehornean con los texels nuevos
        if (m_onCaveRemoved) m_onCaveRemoved(d.name);
        if (m_onCave) m_onCave(d);
    } else if (m_mapsIsland >= 0 && m_mapsIsland < (int)m_world->islandDefs().size()) {
        World::Vox::IslandDef d = m_world->islandDefs()[m_mapsIsland];
        if (d.planta.empty()) { d.planta = basePath(d.name) + "_planta.png"; d.cima = basePath(d.name) + "_cima.png"; d.raiz = basePath(d.name) + "_raiz.png"; }
        // Máscara → distancia con signo (chanfle), saturada a ±kIslandRangeM, como el horneado.
        World::Vox::CaveMap planta = m_maps[0].map;
        std::vector<uint8_t> inside(planta.v.size());
        for (size_t i = 0; i < inside.size(); ++i) inside[i] = planta.v[i] > 0.5f ? 1 : 0;
        const int comps = World::Terrain::BakeUtil::keepLargestComponent2D(planta.w, planta.h, inside);
        std::vector<float> sd; World::Terrain::BakeUtil::chamferSigned2D(planta.w, planta.h, inside, sd);
        const float texelM = 2.0f * d.radiusM / (float)(planta.w - 1);
        for (size_t i = 0; i < sd.size(); ++i) planta.v[i] = std::clamp(sd[i] * texelM / (2.0f * World::Vox::kIslandRangeM) + 0.5f, 0.0f, 1.0f);
        m_lastStats = glm::ivec3(comps, 1, 0);
        if (!save(d.planta, planta) || !save(d.cima, m_maps[1].map) || !save(d.raiz, m_maps[2].map)) { HARUKA_LOGW("Mundo", "no se pudieron guardar los mapas de '%s'", d.name.c_str()); return false; }
        m_world->removeIsland(d.name);
        m_world->placeIsland(d);
        if (m_onIslandRemoved) m_onIslandRemoved(d.name);
        if (m_onIsland) m_onIsland(d);
    } else return false;
    m_mapsEdited = false;
    if (m_onChanged) m_onChanged();
    return true;
}

void WorldEditPanel::ensureMapsLoaded() {
    const int wantCave = m_selectedCave, wantIsland = m_selectedCave >= 0 ? -1 : m_selectedIsland;
    if (wantCave != m_mapsCave || wantIsland != m_mapsIsland) loadMapsFor(wantCave, wantIsland);
}

void WorldEditPanel::paintMap(int mapIndex, float u, float t) {
    ensureMapsLoaded();
    if (mapIndex < 0 || mapIndex >= (int)m_maps.size()) return;
    paintOn(m_maps[mapIndex], u, t);
}

void WorldEditPanel::renderMapsTab() {
    if (m_selectedCave < 0 && m_selectedIsland < 0) { ImGui::TextDisabled("Selecciona una cueva o isla en la pestaña Colocar."); return; }
    ensureMapsLoaded();
    const bool isCave = m_mapsCave >= 0;
    if (isCave) {
        // Caves paint DEPTHS: the value is metres below the ground, mapped to the layer range.
        float depthM = World::Vox::caveLayerToM(m_mapsCaveBox, m_paintValue);
        if (ImGui::SliderFloat("Depth (m)", &depthM, -World::Vox::kCaveOpenM, m_mapsCaveBox.depthM, "%.1f"))
            m_paintValue = World::Vox::caveLayerFromM(m_mapsCaveBox, depthM);
        ImGui::TextDisabled("Air where the floor is deeper than the ceiling. A ceiling at %.0f m (top) opens the ground.", -World::Vox::kCaveOpenM);
        if (m_mapsLossy > 0)
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "Legacy cave: it does NOT fit in %d layers (%d air spans over). Rebaking saves this lossy version.",
                               World::Vox::kCaveMaxLayers, m_mapsLossy);
        ImGui::BeginDisabled((int)m_maps.size() / 2 >= World::Vox::kCaveMaxLayers);
        if (ImGui::Button("Add layer")) addCaveLayer();
        ImGui::EndDisabled();
    } else ImGui::SliderFloat("Valor", &m_paintValue, 0.0f, 1.0f, "%.2f");
    ImGui::SliderFloat("Radio (texels)", &m_paintRadiusTx, 1.0f, 40.0f, "%.0f");
    ImGui::SliderFloat("Dureza", &m_paintHardness, 0.0f, 1.0f, "%.2f");
    ImGui::RadioButton("Pincel", &m_paintMode, 0); ImGui::SameLine();
    ImGui::RadioButton("Corredor (dos clics)", &m_paintMode, 1);
    ImGui::Separator();
    static const char* kIslandNames[3] = { "Planta (silueta: dentro/fuera)", "Cima (relieve, fraccion de topM)", "Raiz (fraccion de rootM)" };
    for (int i = 0; i < (int)m_maps.size(); ++i) {
        MapView& v = m_maps[i];
        if (v.dirty) uploadMap(v);
        ImGui::PushID(i);
        if (isCave) ImGui::Text("Layer %d %s (darker = shallower)", i / 2, (i % 2) ? "floor" : "ceiling");
        else        ImGui::Text("%s", kIslandNames[std::min(i, 2)]);
        const float side = std::min(ImGui::GetContentRegionAvail().x, 320.0f);
        const float aspect = v.map.w > 0 ? (float)v.map.h / (float)v.map.w : 1.0f;
        const ImVec2 size(side, side * aspect);
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        if (v.imTex) ImGui::Image((ImTextureID)(intptr_t)v.imTex, size); else ImGui::Dummy(size);
        if (ImGui::IsItemHovered()) {
            const ImVec2 m = ImGui::GetMousePos();
            const float u = std::clamp((m.x - p0.x) / size.x, 0.0f, 1.0f), t = std::clamp((m.y - p0.y) / size.y, 0.0f, 1.0f);
            if (m_paintMode == 0) {
                if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) paintOn(v, u, t);
            } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                if (!m_corridorHasA) { m_corridorAu = u; m_corridorAv = t; m_corridorHasA = true; }
                else {
                    // Un corredor = pinceladas a lo largo del segmento, cada medio radio.
                    const float du = u - m_corridorAu, dt = t - m_corridorAv;
                    const float lenTx = std::sqrt(du * du * v.map.w * v.map.w + dt * dt * v.map.h * v.map.h);
                    const int n = std::max(1, (int)std::ceil(lenTx / std::max(0.5f * m_paintRadiusTx, 1.0f)));
                    for (int k = 0; k <= n; ++k) paintOn(v, m_corridorAu + du * (float)k / (float)n, m_corridorAv + dt * (float)k / (float)n);
                    m_corridorHasA = false;
                }
            }
            ImGui::SetTooltip("%s (%.2f, %.2f)%s", m_paintMode == 0 ? "pintar" : (m_corridorHasA ? "segundo clic" : "primer clic"), u, t,
                              v.mask ? " · mascara" : "");
        }
        ImGui::PopID();
    }
    ImGui::Separator();
    ImGui::BeginDisabled(!m_mapsEdited);
    if (ImGui::Button("Rehornear (guardar PNG y rehacer)")) rebakeSelected();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Descartar cambios")) loadMapsFor(m_mapsCave, m_mapsIsland);
    if (isCave) ImGui::TextDisabled("last rebake: %d air piece(s) · %d open layer texels", m_lastStats.y, m_lastStats.z);
    else         ImGui::TextDisabled("ultimo rehorneado: %d componentes de silueta -> 1", m_lastStats.x);
}

} // namespace Haruka::Tools
