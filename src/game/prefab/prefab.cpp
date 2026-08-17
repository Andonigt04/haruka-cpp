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
        out.pieces.push_back(p);
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
        j["pieces"].push_back(e);
    }
    std::ofstream f(path);
    if (!f) return false;
    f << j.dump(2) << '\n';
    return f.good();
}

} // namespace Haruka
