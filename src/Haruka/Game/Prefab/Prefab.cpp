#include "Haruka/Game/Prefab/Prefab.h"
#include "Haruka/Core/Logger.h"

#include <nlohmann/json.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <regex>

namespace Haruka::Game::Prefab {

namespace {

/// El trozo JSON de un .glb (o el .gltf entero). La GEOMETRÍA NO SE LEE: un .glb es cabecera de 12
/// bytes + trozos (longitud, tipo, datos) y el JSON va el primero, así que se para ahí. Un montaje de
/// mil ladrillos pesa lo que sus mil nodos, no lo que su malla.
bool readGltfJson(const std::string& path, nlohmann::json& j) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    char head[20] = {};
    f.read(head, 20);
    if (f.gcount() == 20 && std::memcmp(head, "glTF", 4) == 0) {
        std::uint32_t version, len, type;
        std::memcpy(&version, head + 4, 4);
        std::memcpy(&len, head + 12, 4);
        std::memcpy(&type, head + 16, 4);
        if (version != 2 || type != 0x4E4F534Au /* "JSON" */) return false;
        std::string text(len, '\0');
        f.read(text.data(), len);
        if ((std::uint32_t)f.gcount() != len) return false;
        j = nlohmann::json::parse(text, nullptr, false);
    } else {
        f.clear();
        f.seekg(0);
        j = nlohmann::json::parse(f, nullptr, false);
    }
    return !j.is_discarded() && j.is_object();
}

/// La transformación LOCAL de un nodo glTF: `matrix` si la trae, si no T·R·S.
glm::dmat4 gltfLocal(const nlohmann::json& n) {
    if (n.contains("matrix") && n["matrix"].is_array() && n["matrix"].size() == 16) {
        glm::dmat4 m(1.0);
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r) m[c][r] = n["matrix"][c * 4 + r].get<double>();   // por columnas
        return m;
    }
    glm::dvec3 t(0.0), s(1.0);
    glm::dquat q(1.0, 0.0, 0.0, 0.0);
    if (n.contains("translation")) t = { n["translation"][0].get<double>(), n["translation"][1].get<double>(),
                                         n["translation"][2].get<double>() };
    if (n.contains("rotation"))    // glTF guarda [x, y, z, w]; glm::dquat se construye (w, x, y, z)
        q = glm::dquat(n["rotation"][3].get<double>(), n["rotation"][0].get<double>(),
                       n["rotation"][1].get<double>(), n["rotation"][2].get<double>());
    if (n.contains("scale"))       s = { n["scale"][0].get<double>(), n["scale"][1].get<double>(),
                                         n["scale"][2].get<double>() };
    return glm::translate(glm::dmat4(1.0), t) * glm::mat4_cast(q) * glm::scale(glm::dmat4(1.0), s);
}

/// "brick.001" → "brick": Blender numera las copias NO enlazadas, y siguen siendo el mismo item.
std::string stripBlenderSuffix(const std::string& s) {
    static const std::regex suffix(R"(\.\d{3,}$)");
    return std::regex_replace(s, suffix, "");
}

bool loadPrefabGltf(const std::string& path, Prefab& out) {
    nlohmann::json j;
    if (!readGltfJson(path, j)) return false;
    const auto& nodes  = j.value("nodes",  nlohmann::json::array());
    const auto& meshes = j.value("meshes", nlohmann::json::array());

    // Las raíces: las de la escena activa; sin escenas, los nodos que no son hijo de nadie.
    std::vector<int> roots;
    const auto& scenes = j.value("scenes", nlohmann::json::array());
    if (!scenes.empty()) {
        const int s = j.value("scene", 0);
        if (s >= 0 && s < (int)scenes.size())
            for (const auto& r : scenes[s].value("nodes", nlohmann::json::array())) roots.push_back(r.get<int>());
    } else {
        std::vector<char> child(nodes.size(), 0);
        for (const auto& n : nodes)
            for (const auto& c : n.value("children", nlohmann::json::array()))
                if (c.get<int>() >= 0 && c.get<int>() < (int)nodes.size()) child[c.get<int>()] = 1;
        for (int i = 0; i < (int)nodes.size(); ++i) if (!child[i]) roots.push_back(i);
    }

    out.name = std::filesystem::path(path).stem().string();
    out.pieces.clear();
    out.anchors.clear();
    int scaled = 0, unnamed = 0;
    std::vector<char> seen(nodes.size(), 0);   // un glTF con ciclos es inválido; no colgarse por él

    std::function<void(int, const glm::dmat4&)> visit = [&](int i, const glm::dmat4& parent) {
        if (i < 0 || i >= (int)nodes.size() || seen[i]) return;
        seen[i] = 1;
        const auto& n = nodes[i];
        const glm::dmat4 world = parent * gltfLocal(n);
        const nlohmann::json extras = n.value("extras", nlohmann::json::object());
        const std::string nodeName = n.value("name", std::string{});

        // Descomponer: la escala tiene que ser 1 y sin espejo (ver la cabecera).
        const glm::dvec3 c0(world[0]), c1(world[1]), c2(world[2]);
        const glm::dvec3 scl(glm::length(c0), glm::length(c1), glm::length(c2));
        const bool unitScale = std::abs(scl.x - 1.0) < 1e-4 && std::abs(scl.y - 1.0) < 1e-4 &&
                               std::abs(scl.z - 1.0) < 1e-4 && glm::determinant(glm::dmat3(world)) > 0.0;
        const glm::dvec3 pos(world[3]);

        if (n.contains("mesh")) {
            std::string item = extras.value("item", std::string{});
            const int m = n["mesh"].get<int>();
            if (item.empty() && m >= 0 && m < (int)meshes.size()) item = meshes[m].value("name", std::string{});
            if (item.empty()) item = nodeName;
            item = stripBlenderSuffix(item);
            if (item.empty()) ++unnamed;
            else if (!unitScale) {
                ++scaled;
                HARUKA_LOGW("Prefab", "%s: '%s' (%s) escalada %.3f x %.3f x %.3f o espejada: se descarta",
                            path.c_str(), nodeName.c_str(), item.c_str(), scl.x, scl.y, scl.z);
            } else {
                PrefabPiece p;
                p.id  = item;
                p.pos = pos;
                p.rot = glm::normalize(glm::quat_cast(glm::dmat3(world)));
                p.mechanism = extras.value("joint", std::string("rigid")) == "mechanism";
                p.arc = extras.value("arc", 0.0);
                out.pieces.push_back(p);
            }
        } else if (extras.value("anchor", false) || nodeName.rfind("anclaje", 0) == 0) {
            out.anchors.push_back(pos);
        }
        for (const auto& c : n.value("children", nlohmann::json::array())) visit(c.get<int>(), world);
    };
    for (int r : roots) visit(r, glm::dmat4(1.0));

    if (scaled || unnamed)
        HARUKA_LOGW("Prefab", "%s: %d piezas descartadas por escala/espejo, %d sin item", path.c_str(),
                    scaled, unnamed);
    return !out.pieces.empty();
}

} // namespace

std::string findPrefabFile(const std::string& dir, const std::string& name) {
    namespace fs = std::filesystem;
    const std::string base = dir + (dir.empty() || dir.back() == '/' ? "" : "/") + name;
    std::error_code ec;
    const bool json = fs::is_regular_file(base + ".json", ec);
    const bool glb  = fs::is_regular_file(base + ".glb", ec);
    const bool gltf = fs::is_regular_file(base + ".gltf", ec);
    if (json && (glb || gltf))
        HARUKA_LOGW("Prefab", "%s: hay .json y .%s; se usa el .json (el del editor) y el otro se ignora",
                    base.c_str(), glb ? "glb" : "gltf");
    if (json) return base + ".json";
    if (glb)  return base + ".glb";
    if (gltf) return base + ".gltf";
    return {};
}

bool loadPrefab(const std::string& path, Prefab& out) {
    const std::string ext = std::filesystem::path(path).extension().string();
    if (ext == ".glb" || ext == ".gltf") return loadPrefabGltf(path, out);
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

} // namespace Haruka::Game::Prefab
