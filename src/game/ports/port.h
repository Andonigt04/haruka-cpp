/**
 * @file port.h
 * @brief PUERTOS AUTORIZADOS de un asset: puntos de MONTAJE, de INTERACCIÓN y RAÍLES.
 *
 * Ver `docs/guides/PLAN_PUERTOS.md`. Resumen de por qué existe esto:
 *
 * Hasta ahora los puntos de acople se DERIVABAN de la caja envolvente (los centros de cara del
 * AABB — ver `PLAN_CONSTRUCCION.md`, "Sockets/SNAP derivados del AABB"). Eso vale para muros y
 * bloques y no puede valer para nada con orientación propia: una toma de aire va en UNA cara, una
 * bancada de torreta tiene un anillo mirando arriba, un asiento mira hacia algún sitio. El puerto
 * se AUTORIZA en el asset; no se adivina.
 *
 * DOS USOS, UN DATO: un puerto `Mount` es dónde se atornilla algo; uno `Interact` es dónde vive un
 * panel del mundo. Misma estructura, misma edición, mismo raycast.
 *
 * ⚠️ RAÍLES (§4 del plan): puertas, rampas, escotillas, torretas y suspensiones NO son animaciones.
 * Son cuerpos restringidos a UN grado de libertad, con límites y opcionalmente motor. Un raíl es un
 * puerto con `rail`. Una animación no puede bloquearse contra una caja, no puede caerse cuando la
 * unión cede, y hay que reautorizarla si el objeto se mueve; una restricción hace las tres cosas
 * sola. El respaldo ya está en Jolt (Hinge/Slider/Motor); aquí vive el DATO.
 *
 * SEPARACIÓN MOTOR/PROYECTO: aquí solo hay clase, talla y geometría. El VOCABULARIO (`kind`:
 * "bulkhead", "chamber", "hull_bottom"…) y los predicados del juego (estanco, alimentado, afinidad)
 * son del proyecto — el motor nunca sabe qué es "estanco".
 */
#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace Haruka {

/** @brief Para qué sirve el puerto. Es lo único que el MOTOR interpreta. */
enum class PortClass {
    Mount,      ///< se atornilla algo aquí
    Interact,   ///< punto de interacción / ancla de panel del mundo
    Rail,       ///< mecanismo con grado de libertad (lleva `rail`)
    Socket      ///< enganche de construcción (el snap de piezas)
};

/** @brief Grado de libertad del mecanismo. */
enum class RailDof { Hinge, Slider };

/** @brief Quién lo mueve. */
enum class RailDrive {
    Manual,     ///< lo empuja un personaje
    Motor,      ///< motorizado (el proyecto decide si exige estar alimentado)
    Spring      ///< pasivo con muelle: la SUSPENSIÓN
};

/**
 * @brief Un grado de libertad con límites. Mapea 1:1 a Jolt (Hinge/Slider + MotorSettings).
 *
 * `limits` está en GRADOS para `Hinge` y en METROS para `Slider`, siempre relativo al reposo.
 * `compliance` es la α del XPBD (m²/N): 0 = rígido, >0 = amortiguado (suspensión).
 * `breakForce` <= 0 significa irrompible.
 */
struct Rail {
    RailDof   dof        = RailDof::Hinge;
    glm::vec3 axis{0.0f, 1.0f, 0.0f};   ///< eje LOCAL al puerto
    glm::vec2 limits{0.0f, 90.0f};      ///< [min,max] grados (Hinge) o metros (Slider)
    RailDrive drive      = RailDrive::Manual;
    float     motorForce = 0.0f;        ///< N·m (Hinge) o N (Slider)
    float     compliance = 0.0f;        ///< α (m²/N)
    float     breakForce = 0.0f;        ///< N; <=0 = no se rompe
};

/** @brief Un punto autorizado en el asset. Transform LOCAL al prop, nunca al mundo. */
struct Port {
    std::string id;                       ///< estable: "intake_fwd", "seat_pilot", "door_stbd"
    glm::vec3   position{0.0f};
    glm::quat   rotation{1.0f, 0.0f, 0.0f, 0.0f};
    PortClass   cls  = PortClass::Mount;
    std::string kind;                     ///< VOCABULARIO DEL PROYECTO (el motor no lo interpreta)
    int         size = 1;                 ///< talla; una pieza de talla N pide un puerto >= N
    // A QUÉ PARTE del prop pertenece. Los props ya tienen collider por parte, así que ligar el
    // puerto a una parte hace que montaje, interacción y daño hablen del MISMO trozo: un manómetro
    // de la cubierta 2 se rompe cuando se rompe la cubierta 2, sin código nuevo. -1 = el prop entero.
    int         partIndex = -1;
    float       radius = 0.15f;           ///< radio de enganche/selección (m)
    bool        hasRail = false;
    Rail        rail;
};

/** @brief Los puertos de un asset. Se carga una vez y se comparte. */
struct PortSet {
    std::string       assetId;
    std::vector<Port> ports;

    const Port* find(const std::string& portId) const {
        for (const auto& p : ports) if (p.id == portId) return &p;
        return nullptr;
    }
    /** @brief Puertos de una clase (para listar montajes o anclas de panel). */
    std::vector<const Port*> byClass(PortClass c) const {
        std::vector<const Port*> r;
        for (const auto& p : ports) if (p.cls == c) r.push_back(&p);
        return r;
    }
};

/** @brief Registro global assetId → PortSet. */
class PortRegistry {
public:
    static PortRegistry& get() { static PortRegistry r; return r; }
    void           add(const PortSet& s) { m_sets[s.assetId] = s; }
    const PortSet* find(const std::string& assetId) const {
        auto it = m_sets.find(assetId);
        return it == m_sets.end() ? nullptr : &it->second;
    }
    bool   exists(const std::string& assetId) const { return m_sets.count(assetId) != 0; }
    size_t size() const { return m_sets.size(); }
    void   clear() { m_sets.clear(); }
private:
    std::unordered_map<std::string, PortSet> m_sets;
};

// ── Consulta ────────────────────────────────────────────────────────────────────────────────────

/** @brief Transform del puerto en MUNDO. `objectXform` es el del prop (ya incluye el marco local
 *         del vehículo, si lo hay: un puerto de un crawler en marcha no necesita nada especial). */
glm::dmat4 portWorld(const glm::dmat4& objectXform, const Port& p);

/** @brief Eje del raíl en MUNDO (dirección, normalizada). */
glm::dvec3 railAxisWorld(const glm::dmat4& objectXform, const Port& p);

/** @brief ¿Encaja una pieza de esta clase/talla en el puerto? Regla del MOTOR: `kind` igual y talla
 *         del puerto >= talla de la pieza. Los predicados del juego (estanco, alimentado…) los
 *         evalúa el PROYECTO, encima de esto. */
bool portAccepts(const Port& p, const std::string& kind, int size);

/** @brief Rayo → puerto más cercano de un prop. `t` = distancia a lo largo del rayo.
 *         Sirve igual para apuntar a un panel que para elegir dónde atornillar. */
bool raycastPorts(const glm::dmat4& objectXform, const PortSet& set,
                  const glm::dvec3& rayOrigin, const glm::dvec3& rayDir,
                  double maxDist, const Port** outPort, double* outT);

// ── Mecanismo ───────────────────────────────────────────────────────────────────────────────────

/** @brief Acota un valor a los límites del raíl (grados o metros según `dof`). */
float railClamp(const Rail& r, float value);

/** @brief ¿La fuerza que atraviesa el raíl lo rompe? (VEHICULOS.md §5: la unión cede.) */
bool railBreaks(const Rail& r, float force);

/**
 * @brief Estado de un mecanismo en marcha.
 *
 * ⚠️ `target` se PIDE, no se impone: si algo bloquea, el motor empuja hasta `motorForce` y se queda
 * a medias. Eso es justo lo que una animación no sabe hacer.
 */
struct RailState {
    float value    = 0.0f;   ///< posición actual (grados o metros)
    float target   = 0.0f;   ///< posición pedida
    bool  blocked  = false;  ///< tocó algo antes de llegar
    bool  broken   = false;  ///< la unión cedió
};

/** @brief Avance de un tick del mecanismo hacia `target`, con `speed` en unidades/s.
 *         `resistance` (N o N·m) es lo que se le opone: si supera `motorForce`, se queda BLOQUEADO;
 *         si supera `breakForce`, se ROMPE. Determinista y sin GL: testeable. */
void railStep(const Rail& r, RailState& st, float dt, float speed, float resistance);

// ── Serialización ───────────────────────────────────────────────────────────────────────────────

/** @brief Carga un PortSet desde JSON (formato en PLAN_PUERTOS.md §3). Devuelve false si no parsea. */
bool loadPortSetJson(const std::string& path, PortSet& out);
/** @brief Carga todos los *.json de un directorio al registro. Devuelve cuántos assets registró. */
int  loadPortSetsDir(const std::string& dir);
/** @brief Serializa a JSON (lo que escribe el panel del editor). */
bool savePortSetJson(const std::string& path, const PortSet& set);

// Nombres <-> enums (los usa la serialización y el panel del editor).
const char* toString(PortClass);
const char* toString(RailDof);
const char* toString(RailDrive);
bool parsePortClass(const std::string&, PortClass&);
bool parseRailDof(const std::string&, RailDof&);
bool parseRailDrive(const std::string&, RailDrive&);

} // namespace Haruka
