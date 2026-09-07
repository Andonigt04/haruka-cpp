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
/// ROPE: una CUERDA, que no es un grado de libertad más sino una unión que sólo TIRA.
///
/// ⚠️ Y no une piezas vecinas: cuelga de un punto del casco y pasa por un ANILLO. Por eso el puerto
/// que la declara no necesita una pieza atada enfrente —las bisagras sí— y el montaje la trata
/// aparte: ver `vehicle_spawn.cpp`. Es lo que iza un portalón plegable sin ser un pistón: al acortar
/// el recorrido tira de lo que menos se resiste, así que primero pliega la hoja y luego la sube.
/// ⚠️ `Weld` NO ES UN GRADO DE LIBERTAD, y aun así su sitio es este. Une dos piezas SIN juego, pero
/// las deja en CUERPOS distintos — que es justo lo que no sabe hacer una unión rígida normal, porque
/// esa las funde en la misma isla. Existe porque `joint: "mechanism"` se marca POR PIEZA y marca
/// TODAS sus uniones (ver `prefab_runtime.cpp`), así que un conjunto que se mueve sólo podía ser UNA
/// pieza. Con esto, la pieza marcada puede llevarse consigo a sus vecinas: en la suspensión del
/// tanque el vástago se une a la camisa por un MUELLE y a la oruga por una SOLDADURA, y así las dos
/// mitades telescopan de verdad en vez de ser decorado. Se rompe como cualquier otra unión
/// (`breakForce`), que es lo que la distingue de fundir las piezas.
enum class RailDof { Hinge, Slider, Rope, Weld };

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
    /// BLANDURA del muelle: los metros que cede por newton (m/N), o los radianes por N·m en una
    /// bisagra. Es 1/k, así que **más grande = más blando**; 0 = rígido y el muelle no actúa.
    ///
    /// ⚠️ ESTE CAMPO NO LO LEÍA NADIE HASTA EL 2026-09-07. Se declaraba, se parseaba, viajaba en el
    /// `RailSpec`... y `addRail` sólo configuraba el motor `if (drive == Motor)`: un `drive: spring`
    /// salía como un DESLIZANTE LIBRE con topes. O sea que la suspensión no amortiguaba nada, caía a
    /// plomo hasta el tope de abajo y se quedaba ahí. La documentación decía «α del XPBD (m²/N)»,
    /// unidades de un solver que este motor no usa: aquí va a `ESpringMode::StiffnessAndDamping` de
    /// Jolt como `k = 1/compliance`, que es la ecuación `F = -k·x - c·v` y no una analogía.
    float     compliance = 0.0f;
    /// AMORTIGUACIÓN, la `c` de `F = -k·x - c·v`: N·s/m en una guía, N·m·s/rad en una bisagra.
    ///
    /// ⚠️ Sin esto un muelle es un POGO: almacena y devuelve toda la energía, así que un vehículo que
    /// cae rebota indefinidamente. No hay valor por defecto sensato en abstracto —depende de la masa
    /// suspendida— así que se declara por pieza, y quien la declare puede sacarlo de la crítica
    /// `c_cr = 2·sqrt(k·m)`: un coche anda por 0,2-0,5 de esa.
    float     damping    = 0.0f;
    float     breakForce = 0.0f;        ///< N; <=0 = no se rompe
    /// A qué ritmo obedece: m/s en una cuerda, grados/s en una bisagra. 0 = al instante.
    /// Un cabrestante no teletransporta, y el avance va en el paso FIJO de la física (no en el frame).
    float     speed      = 0.0f;
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
    /// CON QUÉ SE EMPAREJA este raíl: `kind` que la OTRA pieza tiene que declarar en alguno de sus
    /// puertos para que la unión se monte. Vacío (lo normal) = como siempre, vale cualquier vecina.
    ///
    /// ⚠️ EXISTE PORQUE UN RAÍL SE RESUELVE CONTRA UNA PIEZA, NO CONTRA UN PUERTO. `railPortFor`
    /// (vehicle_spawn.cpp) coge el puerto de raíl más cercano al CENTRO de la vecina, y eso basta
    /// mientras una pieza sólo tenga un mecanismo. Deja de bastar en una cadena de oruga: el eslabón
    /// declara bisagras hacia sus dos vecinos, y la unión eslabón-RODILLO encontraba una de ellas y
    /// ataba el rodillo a un eslabón cualquiera. La regla obvia —"el puerto tiene que caer dentro de
    /// la otra pieza"— se midió sobre el barco y NO SE SOSTIENE: la bisagra del portón cae entre
    /// 0,025 y 0,187 m FUERA de todas las piezas a las que se une, y la del portalón entre 0,015 y
    /// 0,293 m. O sea que los datos que ya funcionan no la cumplen.
    ///
    /// Así que el emparejamiento se DECLARA, y sólo donde hace falta: un puerto sin este campo se
    /// comporta exactamente como antes, y por construcción nada de lo que ya andaba cambia.
    std::string pairsWith;
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
