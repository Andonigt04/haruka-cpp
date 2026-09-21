/**
 * @file instanced_object.h
 * @brief Registro GENERAL de objetos instanciados: prototipos compartidos + instancias con estado.
 *
 * Es el sistema de instancing que eligió el usuario: "un instancing para objetos EN GENERAL,
 * sirve para objetos también, con las diferencias de cada objeto si han sido destruido y así".
 *
 * Un objeto ya NO es "un SceneObject con su malla y su draw": es una ENTRADA en un buffer de
 * instancias (transform + tinte + escala + estado), dibujada con UNA malla prototipo compartida
 * por tipo (y por LOD). La variedad la dan la transformación, el tinte y la escala por instancia
 * — no geometría nueva por objeto (el mismo argumento que la cabecera de GPUInstancing).
 *
 * ── GENERAL ─────────────────────────────────────────────────────────────────────────────
 * No distingue "prop" de "objeto": cualquier cosa con una malla reutilizable es un PROTOTIPO
 * (un árbol procedimental de bakeTreeMesh, una roca, o un Model cargado por path). La diferencia
 * entre "árbol del scatter" y "casa del jugador" es solo QUÉ prototipo apunta y qué estado tiene
 * cada instancia. Esto unifica el pase instanciado de construcción (piezas de obra) y el de props.
 *
 * ── ESTADO POR INSTANCIA ──────────────────────────────────────────────────────────────────
 * `state` es un flag por instancia: 0 = viva, >0 = oculta/destruida (tocón, edificio demolido…).
 * El render la salta o la dibuja degenerada según el PSO; el registro no borra la entrada, así el
 * estado es barato de alternar sin re-generar geometría.
 *
 * CPU-only: solo retiene los datos. El renderer (application_render) sube el buffer por prototipo
 * con GPUInstancing y dispara un draw por (prototipo, LOD). GL-free a propósito (testeable).
 */
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <glm/glm.hpp>

namespace Haruka {

/** @brief Estado de una instancia. Extensible: el renderer decide cómo pintar cada uno. */
enum class InstancedObjectState : uint32_t {
    Alive      = 0,   ///< normal
    Destroyed  = 1,   ///< oculto (tocón/derribado): no se dibuja (o se dibuja el residuo)
    Regrowing  = 2,   ///< rebrotando: el renderer puede escalar/atenuar por `regrow`
};

/** @brief Una instancia: qué prototipo, dónde, cómo y en qué estado. ~el mínimo para instancing. */
struct InstancedObject {
    int      prototype   = -1;      ///< índice en `InstancedObjectRegistry::prototypes`
    glm::vec3  dir       = glm::vec3(0,1,0);   ///< dirección sobre la esfera (asienta en el suelo)
    float    heightM     = 0.0f;    ///< cota del suelo (m) en la dirección
    float    scale       = 1.0f;    ///< escala del objeto
    glm::vec3 tint       = glm::vec3(1.0f);    ///< tinte por instancia (variedad)
    float    yaw         = 0.0f;    ///< giro alrededor del "up" de la superficie (rad)
    uint32_t seed        = 0;       ///< identidad determinista (celda del mundo, id de cosecha)
    uint32_t state       = 0;       ///< InstancedObjectState
    float    regrow      = 0.0f;    ///< 0..1 progreso de rebrote (si state == Regrowing)
    /// PARTES ROTAS: bit `p` puesto = la parte `p` del esqueleto ya no está (rama arrancada).
    /// Es una máscara y no un contador porque las partes se rompen en cualquier orden y hay que
    /// saber CUÁL falta: la que falta no da collider ni se dibuja. 32 bits sobran — un árbol tiene
    /// tronco + 2-3 ramas. Ortogonal a `state`: se puede tener un árbol vivo al que le falta una
    /// rama, y `state=Destroyed` (tronco talado) se lleva el árbol entero sin mirar la máscara.
    uint32_t breakMask   = 0;
};

/** @brief Un prototipo: la malla COMPARTIDA que dibujan todas sus instancias.
 *  `meshPath` (Model cargado por path) o malla procedimental (bakeTreeMesh con `seed` fija).
 *  `lodLevel` permite tener el mismo objeto con LODs más baratos lejos. */
struct InstancedPrototype {
    std::string name;              ///< tipo ("tree", "rock", "wall", …)
    std::string meshPath;          ///< ruta de Model cargable (vacía = malla procedimental)
    uint32_t    meshSeed  = 1u;    ///< semilla de la malla procedimental (bake determinista)
    int         lodLevel  = 0;     ///< 0 = más detalle … n = más barato (lejos)
    float       lodDist   = 0.0f;  ///< distancia (m) desde la que se usa este LOD
    uint32_t    tint      = 0u;    ///< multiplicador de tinte base (1 = blanco) [reservado]
};

/** @brief Registro GENERAL de objetos instanciados. CPU-only; el renderer lo consume por frame.
 *  Los prototipos se registran una vez (al construir el planeta / cargar el escenario); las
 *  instancias se rellenan con `setInstances`/`addInstance` (scatter, construcción, jugador). */
class InstancedObjectRegistry {
public:
    // --- Prototipos -------------------------------------------------------------
    int addPrototype(const InstancedPrototype& p) {
        int idx = (int)m_protos.size();
        m_protos.push_back(p);
        return idx;
    }
    int prototypeCount() const { return (int)m_protos.size(); }
    const InstancedPrototype& prototype(int i) const { return m_protos[(size_t)i]; }

    /** @brief Vacía prototipos E instancias (al cambiar de planeta/escenario). */
    void reset() { m_protos.clear(); m_instances.clear(); }

    // --- Instancias -------------------------------------------------------------
    /** @brief Sustituye TODAS las instancias (patrón del scatter: regenera el anillo). */
    void setInstances(std::vector<InstancedObject> objs) { m_instances = std::move(objs); }

    void addInstance(const InstancedObject& o) { m_instances.push_back(o); }

    void clearInstances() { m_instances.clear(); }

    const std::vector<InstancedObject>& instances() const { return m_instances; }
    size_t instanceCount() const { return m_instances.size(); }

    /** @brief Oculta/destruye una instancia por su identidad determinista (celda). Barato:
     *  no borra, solo marca — el mismo árbol puede rebrotar después. Devuelve true si existía. */
    bool setStateBySeed(uint32_t seed, InstancedObjectState state, float regrow = 0.0f) {
        for (auto& o : m_instances) {
            if (o.seed != seed) continue;
            o.state  = (uint32_t)state;
            o.regrow = regrow;
            return true;
        }
        return false;
    }

    /** @brief Marca UNA parte como rota (rama arrancada) sin tocar el estado de la instancia: el
     *  árbol sigue vivo y en pie, solo que le falta ese trozo. Devuelve true si existía. */
    bool setBreakBitBySeed(uint32_t seed, uint32_t partId) {
        if (partId >= 32) return false;
        for (auto& o : m_instances) {
            if (o.seed != seed) continue;
            o.breakMask |= (1u << partId);
            return true;
        }
        return false;
    }

    /** @brief Máscara de partes rotas de una instancia (0 si no existe). Para persistirla. */
    uint32_t breakMaskBySeed(uint32_t seed) const {
        for (const auto& o : m_instances) if (o.seed == seed) return o.breakMask;
        return 0u;
    }

private:
    std::vector<InstancedPrototype> m_protos;
    std::vector<InstancedObject>    m_instances;
};

} // namespace Haruka
