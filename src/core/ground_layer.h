/**
 * @file ground_layer.h
 * @brief LA CAPA GRANULAR: lo que hay ENCIMA del suelo (nieve, arena, barro) y las HUELLAS que deja
 *        cualquier cosa que lo pise.
 *
 * ── POR QUÉ NO VA SOBRE `DeformationField` ──────────────────────────────────────────────────────
 * La nota de diseño vieja decía que las huellas irían sobre el módulo DEFORM. Es un error, y de los
 * caros: las brochas de `DeformationField` entran en la GENERACIÓN del terreno — invalidan chunks,
 * los re-generan en la GPU y obligan a reconstruir el parche de colisión de Jolt. Una pisada por
 * zancada serían decenas de rebuilds de chunk por minuto. Y lo más grave: moverían la SUPERFICIE DE
 * REFERENCIA, que es el invariante que cuesta 6 mm de error render↔colisión mantener.
 *
 * La capa granular va **POR ENCIMA** de esa superficie y no la toca:
 *   · visualmente, es albedo + normal en el shader del terreno (huella = zona pisada, más oscura y
 *     con su borde levantado), no geometría;
 *   · para el juego, es un ESCALAR (`sinkAt`) que el character controller resta a la altura de los
 *     pies y que penaliza la velocidad. Hundirse 8 cm en la nieve no exige que el suelo baje 8 cm.
 * Consecuencia: cero riesgo para la paridad render↔colisión, y las huellas cuestan lo que cuesta
 * pintar un disco.
 *
 * ── LA MATERIA IMPORTA ──────────────────────────────────────────────────────────────────────────
 * No es "hay capa sí/no": cada material se comporta distinto, y esa diferencia es la feature.
 *   · NIEVE  — huella profunda y PERSISTENTE; la nevada la rellena; te hundes y andas más lento.
 *   · ARENA  — huella somera que se DESMORONA sola (el borde cae); dura poco.
 *   · BARRO  — huella que se queda y BRILLA (agua); agarre bajo.
 *   · CENIZA — como la arena pero oscura (volcánico; aún no lo produce nadie).
 *
 * ── CÓMO SE GUARDA ──────────────────────────────────────────────────────────────────────────────
 * NO hay una retícula persistente. Hay una LISTA acotada de pisadas recientes (`Stamp`), y cada
 * frame se re-pintan en una ventana cenital alrededor del jugador. Eso evita de un plumazo el
 * problema de "la textura tiene que desplazarse con el jugador sin arrastrar su contenido" (scroll
 * toroidal), y hace que re-centrar sea gratis: se vuelve a proyectar la misma lista.
 * El precio es que solo hay memoria dentro de la ventana y durante `lifetime`; para un camino de
 * huellas a tu espalda es exactamente lo que se quiere ver.
 */
#pragma once

#include <glm/glm.hpp>
#include <deque>
#include <cstdint>

namespace Haruka {

/** @brief De qué está hecha la capa. Gobierna look, vida de la huella, hundimiento y agarre. */
enum class GroundMaterial : uint8_t {
    None = 0,
    Snow = 1,
    Sand = 2,
    Mud  = 3,
    Ash  = 4,
};

class GroundLayer {
public:
    /** @brief Una pisada: dónde, de qué tamaño, con cuánta fuerza y cuándo. */
    struct Stamp {
        glm::dvec3 pos{0.0};     ///< posición ABSOLUTA de mundo (el planeta mide 6.4e6 m: double)
        float      radius = 0.22f;  ///< radio en metros (un pie ≈ 0.22; una rueda, más)
        float      depth  = 1.0f;   ///< [0,1] cuánto hunde (masa del que pisa × blandura del material)
        double     bornAt = 0.0;    ///< reloj del mundo al pisar
        GroundMaterial material = GroundMaterial::Snow;
    };

    /** @brief Máximo de huellas VIVAS. Acotado a propósito: es una cola, no un historial. 384 pisadas
     *  a ~0.75 m de zancada son ~290 m de camino, más de lo que cabe en la ventana. */
    static constexpr size_t kMaxStamps = 384;

    /** @brief Radio (m) de la ventana que se pinta alrededor del jugador. Fuera de aquí no hay
     *  huellas: es el mismo compromiso que la máscara cenital (resolución por metro vs alcance). */
    static constexpr double kWindowM = 24.0;

    /** @brief Cuánto vive una huella, por material. La arena se desmorona; la nieve aguanta. */
    static float lifetimeFor(GroundMaterial m);
    /** @brief Cuánto te HUNDES (m) con la capa a espesor pleno. */
    static float sinkDepthFor(GroundMaterial m);
    /** @brief Multiplicador de velocidad al caminar por la capa a espesor pleno (1 = no estorba). */
    static float speedFactorFor(GroundMaterial m);

    /** @brief Registra una pisada. La usa CUALQUIER cosa con collider: el jugador por zancada, una
     *  criatura, una rueda. El motor no sabe quién pisa — solo dónde y con qué huella. */
    void stamp(const Stamp& s);

    /** @brief Retira las huellas caducadas. `now` = reloj del mundo (`simulationTime`). */
    void update(double now);

    /** @brief Cuánto está pisada la capa en un punto [0,1] — para el juego (sonido, rastro de IA).
     *  El RENDER no usa esto: lee la ventana pintada, que es lo mismo evaluado en GPU. */
    float trampledAt(const glm::dvec3& worldPos, double now) const;

    const std::deque<Stamp>& stamps() const { return m_stamps; }
    size_t size() const { return m_stamps.size(); }
    void clear() { m_stamps.clear(); }

    /** @brief [0,1] de una huella según su edad: plena al pisar, desvanecida al caducar. La NIEVE se
     *  borra despacio y de golpe al final (la rellena la nevada); la ARENA se desmorona enseguida. */
    static float fade(const Stamp& s, double now);

private:
    std::deque<Stamp> m_stamps;
};

} // namespace Haruka
