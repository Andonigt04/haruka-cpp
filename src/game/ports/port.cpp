#include "game/ports/port.h"

#include <nlohmann/json.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>

namespace Haruka {

// ── nombres <-> enums ───────────────────────────────────────────────────────────────────────────
const char* toString(PortClass c) {
    switch (c) {
        case PortClass::Mount:    return "mount";
        case PortClass::Interact: return "interact";
        case PortClass::Rail:     return "rail";
        case PortClass::Socket:   return "socket";
    }
    return "mount";
}
const char* toString(RailDof d) {
    switch (d) {
        case RailDof::Hinge:  return "hinge";
        case RailDof::Slider: return "slider";
        case RailDof::Rope:   return "rope";
        case RailDof::Weld:   return "weld";
    }
    return "slider";
}
const char* toString(RailDrive d) {
    switch (d) {
        case RailDrive::Manual: return "manual";
        case RailDrive::Motor:  return "motor";
        case RailDrive::Spring: return "spring";
    }
    return "manual";
}
bool parsePortClass(const std::string& s, PortClass& out) {
    if (s == "mount")    { out = PortClass::Mount;    return true; }
    if (s == "interact") { out = PortClass::Interact; return true; }
    if (s == "rail")     { out = PortClass::Rail;     return true; }
    if (s == "socket")   { out = PortClass::Socket;   return true; }
    return false;
}
bool parseRailDof(const std::string& s, RailDof& out) {
    if (s == "hinge")  { out = RailDof::Hinge;  return true; }
    if (s == "slider") { out = RailDof::Slider; return true; }
    if (s == "rope")   { out = RailDof::Rope;   return true; }
    if (s == "weld")   { out = RailDof::Weld;   return true; }
    return false;
}
bool parseRailDrive(const std::string& s, RailDrive& out) {
    if (s == "manual") { out = RailDrive::Manual; return true; }
    if (s == "motor")  { out = RailDrive::Motor;  return true; }
    if (s == "spring") { out = RailDrive::Spring; return true; }
    return false;
}

// ── consulta ────────────────────────────────────────────────────────────────────────────────────
glm::dmat4 portWorld(const glm::dmat4& objectXform, const Port& p) {
    // El transform del puerto es LOCAL al prop. Componer con el del objeto ya trae gratis el marco
    // local del vehículo: una puerta de un crawler en marcha no necesita nada especial (PLAN §4).
    glm::dmat4 local = glm::translate(glm::dmat4(1.0), glm::dvec3(p.position))
                     * glm::dmat4(glm::mat4_cast(glm::dquat(p.rotation)));
    return objectXform * local;
}

glm::dvec3 railAxisWorld(const glm::dmat4& objectXform, const Port& p) {
    const glm::dmat4 w = portWorld(objectXform, p);
    const glm::dvec3 a = glm::dvec3(w * glm::dvec4(glm::dvec3(p.rail.axis), 0.0));
    const double len = glm::length(a);
    return len > 1e-12 ? a / len : glm::dvec3(0.0, 1.0, 0.0);
}

bool portAccepts(const Port& p, const std::string& kind, int size) {
    // Regla del MOTOR y nada más: mismo `kind` y el puerto no puede ser más pequeño que la pieza.
    // Lo demás (estanco, alimentado, volumen libre…) lo evalúa el PROYECTO encima de esto.
    if (p.cls != PortClass::Mount && p.cls != PortClass::Socket) return false;
    if (p.kind != kind) return false;
    return p.size >= size;
}

bool raycastPorts(const glm::dmat4& objectXform, const PortSet& set,
                  const glm::dvec3& rayOrigin, const glm::dvec3& rayDir,
                  double maxDist, const Port** outPort, double* outT) {
    if (outPort) *outPort = nullptr;
    if (outT)    *outT    = 0.0;
    const double dirLen = glm::length(rayDir);
    if (dirLen < 1e-12) return false;
    const glm::dvec3 d = rayDir / dirLen;

    const Port* best = nullptr;
    double bestT = maxDist;
    for (const auto& p : set.ports) {
        // Rayo contra la ESFERA de enganche del puerto: es lo que se quiere para apuntar (perdona
        // el pulso) y es barato. La orientación importa para montar, no para señalar.
        const glm::dvec3 c = glm::dvec3(portWorld(objectXform, p)[3]);
        const glm::dvec3 m = c - rayOrigin;
        const double tca = glm::dot(m, d);
        const double d2  = glm::dot(m, m) - tca * tca;
        const double r2  = double(p.radius) * double(p.radius);
        if (d2 > r2) continue;
        const double thc = std::sqrt(std::max(0.0, r2 - d2));
        double t = tca - thc;
        if (t < 0.0) t = tca + thc;      // origen dentro de la esfera
        if (t < 0.0 || t > bestT) continue;
        bestT = t; best = &p;
    }
    if (!best) return false;
    if (outPort) *outPort = best;
    if (outT)    *outT    = bestT;
    return true;
}

// ── mecanismo ───────────────────────────────────────────────────────────────────────────────────
float railClamp(const Rail& r, float value) {
    const float lo = std::min(r.limits.x, r.limits.y);
    const float hi = std::max(r.limits.x, r.limits.y);
    return std::clamp(value, lo, hi);
}

bool railBreaks(const Rail& r, float force) {
    return r.breakForce > 0.0f && std::fabs(force) > r.breakForce;
}

void railStep(const Rail& r, RailState& st, float dt, float speed, float resistance) {
    if (st.broken) return;
    // Lo primero: ¿cede la unión? (VEHICULOS.md §5 — la puerta se CAE, cosa que una animación no
    // sabe hacer.) Una vez rota, el mecanismo deja de obedecer para siempre.
    if (railBreaks(r, resistance)) { st.broken = true; st.blocked = false; return; }

    st.target = railClamp(r, st.target);
    const float delta = st.target - st.value;
    if (std::fabs(delta) < 1e-5f) { st.blocked = false; return; }

    // ⚠️ El motor PIDE, no impone: si lo que se opone supera su fuerza, se queda a medias. Con
    // `Manual` no hay límite propio (lo empuja un personaje) y con `Spring` tampoco: el muelle
    // siempre tira, y lo que decide es la compliance del solver.
    if (r.drive == RailDrive::Motor && std::fabs(resistance) > r.motorForce) {
        st.blocked = true;
        return;
    }
    st.blocked = false;

    const float step = speed * dt;
    st.value = (std::fabs(delta) <= step) ? st.target
                                          : st.value + (delta > 0.0f ? step : -step);
    st.value = railClamp(r, st.value);
}

// ── serialización ───────────────────────────────────────────────────────────────────────────────
namespace {
glm::vec3 vec3OrDefault(const nlohmann::json& j, const char* key, glm::vec3 def) {
    if (!j.contains(key) || !j[key].is_array() || j[key].size() != 3) return def;
    return glm::vec3(j[key][0].get<float>(), j[key][1].get<float>(), j[key][2].get<float>());
}
} // namespace

bool loadPortSetJson(const std::string& path, PortSet& out) {
    std::ifstream f(path);
    if (!f) return false;
    nlohmann::json j;
    try { f >> j; } catch (...) { return false; }
    if (!j.contains("ports") || !j["ports"].is_array()) return false;

    out.assetId = j.value("asset", std::string{});
    out.ports.clear();
    for (const auto& e : j["ports"]) {
        Port p;
        p.id        = e.value("id", std::string{});
        if (p.id.empty()) continue;                    // un puerto sin id no es direccionable
        p.position  = vec3OrDefault(e, "position", glm::vec3(0.0f));
        if (e.contains("rotation") && e["rotation"].is_array() && e["rotation"].size() == 4)
            p.rotation = glm::quat(e["rotation"][0].get<float>(), e["rotation"][1].get<float>(),
                                   e["rotation"][2].get<float>(), e["rotation"][3].get<float>());
        parsePortClass(e.value("cls", std::string("mount")), p.cls);
        p.kind      = e.value("kind", std::string{});
        p.pairsWith = e.value("pairsWith", std::string{});
        p.size      = e.value("size", 1);
        p.partIndex = e.value("partIndex", -1);
        p.radius    = e.value("radius", 0.15f);
        if (e.contains("rail") && e["rail"].is_object()) {
            const auto& r = e["rail"];
            p.hasRail = true;
            parseRailDof(r.value("dof", std::string("hinge")), p.rail.dof);
            p.rail.speed = r.value("speed", 0.0f);
            parseRailDrive(r.value("drive", std::string("manual")), p.rail.drive);
            p.rail.axis = vec3OrDefault(r, "axis", glm::vec3(0.0f, 1.0f, 0.0f));
            if (r.contains("limits") && r["limits"].is_array() && r["limits"].size() == 2)
                p.rail.limits = glm::vec2(r["limits"][0].get<float>(), r["limits"][1].get<float>());
            p.rail.motorForce = r.value("motorForce", 0.0f);
            p.rail.compliance = r.value("compliance", 0.0f);
            p.rail.damping    = r.value("damping", 0.0f);
            p.rail.breakForce = r.value("breakForce", 0.0f);
            // Un puerto con raíl ES de clase Rail aunque el fichero diga otra cosa: el mecanismo
            // manda sobre la etiqueta, para que no haya dos verdades.
            p.cls = PortClass::Rail;
        }
        out.ports.push_back(p);
    }
    return true;
}

int loadPortSetsDir(const std::string& dir) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return 0;
    int n = 0;
    // Orden estable: `directory_iterator` no lo garantiza y el registro acabaría dependiendo del
    // sistema de ficheros. Mismo criterio que el cargador de items.
    std::vector<std::string> files;
    for (const auto& e : fs::directory_iterator(dir, ec))
        if (e.is_regular_file() && e.path().extension() == ".json")
            files.push_back(e.path().string());
    std::sort(files.begin(), files.end());
    for (const auto& f : files) {
        PortSet s;
        if (!loadPortSetJson(f, s) || s.assetId.empty()) continue;
        PortRegistry::get().add(s);
        ++n;
    }
    return n;
}

bool savePortSetJson(const std::string& path, const PortSet& set) {
    nlohmann::json j;
    j["asset"] = set.assetId;
    j["ports"] = nlohmann::json::array();
    for (const auto& p : set.ports) {
        nlohmann::json e;
        e["id"]       = p.id;
        e["position"] = { p.position.x, p.position.y, p.position.z };
        e["rotation"] = { p.rotation.w, p.rotation.x, p.rotation.y, p.rotation.z };
        e["cls"]      = toString(p.cls);
        if (!p.kind.empty()) e["kind"] = p.kind;
        if (!p.pairsWith.empty()) e["pairsWith"] = p.pairsWith;
        e["size"]      = p.size;
        e["partIndex"] = p.partIndex;
        e["radius"]    = p.radius;
        if (p.hasRail) {
            nlohmann::json r;
            r["dof"]        = toString(p.rail.dof);
            if (p.rail.speed != 0.0f) r["speed"] = p.rail.speed;
            r["drive"]      = toString(p.rail.drive);
            r["axis"]       = { p.rail.axis.x, p.rail.axis.y, p.rail.axis.z };
            r["limits"]     = { p.rail.limits.x, p.rail.limits.y };
            r["motorForce"] = p.rail.motorForce;
            r["compliance"] = p.rail.compliance;
            r["damping"]    = p.rail.damping;
            r["breakForce"] = p.rail.breakForce;
            e["rail"] = r;
        }
        j["ports"].push_back(e);
    }
    std::ofstream f(path);
    if (!f) return false;
    f << j.dump(2) << '\n';
    return f.good();
}

} // namespace Haruka
