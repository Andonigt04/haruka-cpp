#pragma once
#include <glm/glm.hpp>
#include <algorithm>

namespace Haruka {

// ═══════════════════════════════════════════════════════════════════════════════════════════════
//  EL MOVIMIENTO DE LAS NUBES ENTRE FRAMES — lo que hace que vayan (o no) a tirones.
//
//  Dos piezas que vivian sueltas en `application_render.cpp` y que por eso nadie podia medir:
//    · la DERIVA del campo con el viento: era `viento(ahora) × simT` (un cambio de 0,1 m/s con un
//      reloj de horas movia el cielo cientos de km en un frame); es una INTEGRAL, y aqui se integra;
//    · el FUNDIDO entre dos horneados del cielo (cada ~2 s de mundo): se cambiaba de golpe, y entre
//      dos horneados un texel salta 1,4 km de techo y 0,05 de cobertura (medido).
//  `test_cloud_motion` mide las dos: frame a frame, cuanto salta lo que llega al shader.
// ═══════════════════════════════════════════════════════════════════════════════════════════════

/// La deriva del campo de nubes, integrada en doble. Unidades del campo (1 = ~1,4 km).
struct CloudDrift {
    glm::dvec2 offset{0.0};
    double     lastSimT = 0.0;
    bool       init     = false;
    /// Tope al dt de un frame: un salto del reloj (carga, pausa, reloj del cluster) NO es viento.
    static constexpr double kMaxStepS = 5.0;

    /// `windEN`: viento a nivel de nube en unidades del campo por segundo (este, norte).
    /// Devuelve cuanto se movio el campo en este frame (para medirlo).
    glm::dvec2 advance(const glm::dvec2& windEN, double simT) {
        if (!init) {   // arranca en viento × simT para que el cielo no empiece en el origen del campo
            offset = windEN * simT; lastSimT = simT; init = true; return glm::dvec2(0.0);
        }
        const double dts = std::clamp(simT - lastSimT, 0.0, kMaxStepS);
        const glm::dvec2 d = windEN * dts;
        offset += d; lastSimT = simT;
        return d;
    }
};

/// El fundido entre el horneado anterior y el nuevo. El peso del nuevo sube de 0 a 1 en una
/// fraccion del intervalo entre horneados, para que ESTE en 1 cuando llegue el siguiente: si
/// llegara antes de terminar, el cambio de "anterior" daria un salto (de mix(A,B,f) a B).
struct BakeBlend {
    double bakeWorldT = -1.0;   ///< tiempo de mundo en que se RECOGIO el ultimo horneado
    double intervalW  = 2.0;    ///< cuanto tardo en llegar respecto al anterior
    bool   havePrev   = false;  ///< hay un "anterior" con el que fundir
    /// El fundido dura este tanto del intervalo: el horneado llega con retraso variable (va en un
    /// hilo, 50-220 ms) y con 1,0 a veces no habia terminado cuando llegaba el siguiente.
    static constexpr double kBlendFraction = 0.75;

    void onBake(double worldNow) {
        if (bakeWorldT >= 0.0) { intervalW = std::max(worldNow - bakeWorldT, 0.5); havePrev = true; }
        bakeWorldT = worldNow;
    }
    /// Peso del horneado NUEVO en este instante.
    double factor(double worldNow) const {
        if (!havePrev || bakeWorldT < 0.0) return 1.0;
        return std::clamp((worldNow - bakeWorldT) / (intervalW * kBlendFraction), 0.0, 1.0);
    }
};

} // namespace Haruka
