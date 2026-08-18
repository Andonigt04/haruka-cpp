/**
 * @file progress_hook.h
 * @brief Un punto de respiro para las tareas LARGAS que corren en el hilo principal.
 *
 * ⚠️ POR QUÉ EXISTE. Crear o cargar un mundo hornea los mapas del planeta en el hilo principal, y
 * con caché fría eso son ~21 s medidos (bioma 14,8 s · altura 2,7 s · escribir los PNG 3,4 s).
 * Durante ese rato nadie vacía la cola de SDL, así que el compositor deja de recibir respuesta y
 * marca la ventana como NO RESPONDE: el usuario ve el juego "congelado" y no sabe si sigue vivo.
 * El síntoma no es la lentitud —es el silencio.
 *
 * Esto NO es un planificador de tareas ni convierte el horneado en asíncrono. Es lo mínimo: quien
 * hornea avisa de vez en cuando, y quien tiene la ventana aprovecha para responder al sistema.
 *
 * ⚠️ SOLO DESDE EL HILO PRINCIPAL. El callback toca SDL, que no es seguro desde otros hilos. Los
 * horneados paralelos deben llamar a esto desde el hilo que ESPERA, no desde los trabajadores.
 *
 * Lo que sigue sin arreglar, y conviene no confundir: la ventana no se REPINTA. El contenido se
 * queda como estaba porque repintar exige el renderizador y una pantalla de carga de verdad. Esto
 * solo evita el "no responde".
 */
#pragma once

#include <functional>

namespace Haruka {

/// @param stage  etiqueta legible de la etapa ("bioma 18750x9375").
/// @param frac   avance [0,1], o negativo si no se puede estimar.
using ProgressFn = std::function<void(const char* stage, float frac)>;

/// Referencia al enganche global. Vacío por defecto: sin ventana (tests, herramientas) no hace nada.
inline ProgressFn& progressHook() {
    static ProgressFn fn;
    return fn;
}

/// Avisa de que una tarea larga sigue viva. No hace nada si nadie ha instalado el enganche.
inline void reportProgress(const char* stage, float frac) {
    const ProgressFn& fn = progressHook();
    if (fn) fn(stage, frac);
}

/// Milisegundos que consumió `PhysicsEngine::advance` en el último frame. Vive aquí, junto al resto
/// de la instrumentación ligera, para no arrastrar una dependencia por un `double`.
inline double& physicsFrameMs() {
    static double ms = 0.0;
    return ms;
}

} // namespace Haruka
