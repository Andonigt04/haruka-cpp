#include "game/prefab/prefab.h"

#include <nlohmann/json.hpp>
#include <fstream>

namespace Haruka {

bool loadPrefab(const std::string& path, Prefab& out) {
    std::ifstream f(path);
    if (!f) return false;
    nlohmann::json j;
    try { f >> j; } catch (...) { return false; }
    if (!j.contains("pieces") || !j["pieces"].is_array()) return false;

    out.name = j.value("name", std::string{});
    out.pieces.clear();
    for (const auto& e : j["pieces"]) {
        PrefabPiece p;
        p.id = e.value("item", std::string{});
        if (p.id.empty()) continue;                    // una pieza sin item no es nada
        if (e.contains("pos") && e["pos"].is_array() && e["pos"].size() == 3)
            p.pos = glm::dvec3(e["pos"][0].get<double>(), e["pos"][1].get<double>(),
                               e["pos"][2].get<double>());
        if (e.contains("rot") && e["rot"].is_array() && e["rot"].size() == 4)
            p.rot = glm::dquat(e["rot"][0].get<double>(), e["rot"][1].get<double>(),
                               e["rot"][2].get<double>(), e["rot"][3].get<double>());
        p.mechanism = e.value("joint", std::string("rigid")) == "mechanism";
        p.arc = e.value("arc", 0.0);
        out.pieces.push_back(p);
    }
    // Los anclajes del MONTAJE (dónde acaba una pieza y empieza la siguiente). Opcionales: un
    // prefabricado que no los declare simplemente no tiene juntas que enseñar.
    out.anchors.clear();
    if (j.contains("anclajes") && j["anclajes"].is_array()) {
        for (const auto& a : j["anclajes"]) {
            if (!a.is_array() || a.size() != 3) continue;
            out.anchors.emplace_back(a[0].get<double>(), a[1].get<double>(), a[2].get<double>());
        }
    }
    return !out.pieces.empty();
}

bool savePrefab(const std::string& path, const Prefab& p) {
    nlohmann::json j;
    j["_doc"] = "PREFABRICADO: un conjunto de objetos ya montados. No sabe si es un vehiculo, una "
                "casa o un puesto de trabajo, y no debe saberlo: eso se decide al COLOCARLO (un "
                "vehiculo es una estructura NO ANCLADA). Las piezas son OBJETOS DISTINTOS a "
                "proposito — cada una conserva su item, su collider y su isla rigida, que es lo que "
                "permite que una puerta gire, que una suspension ceda y que un impacto arranque un "
                "trozo. 'joint':'mechanism' marca la union que NO funde islas.";
    j["name"] = p.name;
    j["pieces"] = nlohmann::json::array();
    for (const auto& pc : p.pieces) {
        nlohmann::json e;
        e["item"] = pc.id;
        e["pos"]  = { pc.pos.x, pc.pos.y, pc.pos.z };
        e["rot"]  = { pc.rot.w, pc.rot.x, pc.rot.y, pc.rot.z };
        if (pc.mechanism) e["joint"] = "mechanism";
        if (pc.arc != 0.0) e["arc"] = pc.arc;   // recta es lo normal: no se escribe el cero
        j["pieces"].push_back(e);
    }
    if (!p.anchors.empty()) {
        j["anclajes"] = nlohmann::json::array();
        for (const auto& a : p.anchors) j["anclajes"].push_back({ a.x, a.y, a.z });
    }
    std::ofstream f(path);
    if (!f) return false;
    f << j.dump(2) << '\n';
    return f.good();
}

void applyPrefabEdits(Prefab& pf, const std::vector<PrefabEdit>& edits) {
    if (edits.empty()) return;
    // ⚠️ PRIMERO LOS CAMBIOS, DESPUÉS LOS BORRADOS. Los índices son posiciones en `pieces`: si se
    // borrara sobre la marcha, el siguiente cambio caería en la pieza equivocada — y en silencio.
    std::vector<char> gone(pf.pieces.size(), 0);
    for (const PrefabEdit& e : edits) {
        if (e.index < 0 || e.index >= (int)pf.pieces.size()) continue;   // montaje reordenado
        if (e.removed) { gone[e.index] = 1; continue; }
        PrefabPiece& pc = pf.pieces[e.index];
        if (e.hasPos)       pc.pos = e.pos;
        if (e.hasRot)       pc.rot = e.rot;
        if (!e.item.empty()) pc.id = e.item;
    }
    size_t w = 0;
    for (size_t i = 0; i < pf.pieces.size(); ++i)
        if (!gone[i]) pf.pieces[w++] = pf.pieces[i];
    pf.pieces.resize(w);
}

} // namespace Haruka
