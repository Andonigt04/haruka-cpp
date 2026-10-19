#pragma once
/**
 * @file LoadController.h
 * @brief EL LAZO CERRADO "DETALLE CONTRA MS DE GPU", sin GPU: entra un tiempo medido y sale un
 *        detalle en [detailMin, detailMax]. GL-free y con reloj explícito, para probarlo en CPU.
 *
 * Es la ley de `PropSystem` (media exponencial del pase, paso integral cada `stepS` con banda
 * muerta, arranque en equilibrio `budget/ms`) sacada a una pieza que cualquier pase usa tal cual. El
 * presupuesto lo pone quien la usa; aquí no hay ninguna distancia.
 *
 * Además admite un TOPE DURO aparte del tiempo (`over`): si un pase tiene un buffer de instancias finito,
 * desbordarlo deja huecos aleatorios aunque sobren ms. Con `over = true` el detalle
 * baja aunque el tiempo diga que hay holgura.
 */
#include <algorithm>

namespace Haruka::World::Props {

struct LoadController {
    double budgetMs  = 3.0;     ///< lo que puede costar el pase (ms de GPU)
    double ki        = 0.25;    ///< paso de detalle por unidad de error relativo
    double stepS     = 0.5;     ///< cada cuánto da un paso (s)
    double deadband  = 0.06;    ///< error relativo que no mueve nada (evita bailar en el sitio)
    float  detailMin = 0.0f, detailMax = 1.0f;

    float  detail    = 1.0f;    ///< la salida
    double msSmooth  = 0.0;     ///< media exponencial de lo medido (τ ≈ 1 s a 60 fps)
    double lastStep  = -1e300;
    bool   seeded    = false;

    /** @brief Un frame medido. `ms <= 0` = sin medida (GL sin timestamps): solo actúa el tope duro.
     *  @return true si el detalle cambió. */
    bool update(double ms, double now, bool over = false) {
        const float before = detail;
        if (ms <= 0.0) {
            if (!over || now - lastStep < stepS) return false;
            lastStep = now;
            detail = std::clamp(detail - (float)(ki * 0.25), detailMin, detailMax);
            return detail != before;
        }
        if (!seeded) {
            // EL PRIMER MUESTREO ES EL ESTADO: se arranca en equilibrio (el coste va ~lineal con el
            // detalle), no en el máximo — un primer frame a tope sería un pico que luego se paga.
            seeded   = true;
            msSmooth = ms;
            detail   = (float)std::clamp((double)detail * budgetMs / std::max(ms, 1e-3),
                                         (double)detailMin, (double)detailMax);
            lastStep = now;
            return detail != before;
        }
        msSmooth += (ms - msSmooth) * 0.05;
        if (now - lastStep < stepS) return false;
        lastStep = now;
        double err = (msSmooth - budgetMs) / budgetMs;    // > 0 = sobre presupuesto
        if (over) err = std::max(err, 0.25);              // el tope duro manda sobre la holgura
        if (std::abs(err) > deadband)
            detail = std::clamp(detail - (float)(ki * err), detailMin, detailMax);
        return detail != before;
    }
};

} // namespace Haruka::World::Props
