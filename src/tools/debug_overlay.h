#pragma once
/**
 * @file tools/debug_overlay.h
 * @brief EL PANEL DE DEPURACION: anclado arriba a la derecha, sin fondo, sin bordes, no se mueve ni
 *        recibe el raton. Una columna de cifras que se leen de un vistazo:
 *
 *   fps · p50 / p98 / p99.5 del tiempo de frame (ms, ultimos 240 frames)
 *   mem: RSS del proceso / RAM total (MiB y %)
 *   red: integrado o head remoto · RTT al head (ultimo · min/avg/max) · tx/rx en KiB/s · reloj del mundo
 *   servidor / zona · objetos de la escena (+ props del scatter)
 *   coords XYZ del mundo · cpu · gpu · pantalla · API grafica
 *
 * Andoni (21-09): "anclado arriba derecha transparente fondo estatico". Lo estatico (cpu, gpu,
 * pantalla, API) se lee una vez; la memoria cada medio segundo; el resto llega por `pushFrame` desde
 * el bucle del juego, que es quien sabe de red y de escena. Reemplaza al overlay de metricas viejo,
 * que nadie llamaba.
 */
#include <cstdint>
#include <deque>
#include <string>
#include <glm/glm.hpp>

namespace Haruka { namespace Tools {

/// Lo de RED, que rellena el juego (el motor no sabe de DGS aqui). Bytes ACUMULADOS: el panel deriva.
struct DebugNetInfo {
    bool        connected = false;     ///< hay cliente DGS conectado
    std::string server;                ///< "host:puerto" del head, o "local (sin DGS)"
    std::string zone;                  ///< "host:puerto" de la zona, o vacio
    float       headMs = -1.0f, headMinMs = -1.0f, headAvgMs = -1.0f, headMaxMs = -1.0f;   ///< -1 = sin muestra
    float       zoneMs = -1.0f, zoneMinMs = -1.0f, zoneAvgMs = -1.0f, zoneMaxMs = -1.0f;   ///< ping/pong UDP a la zona
    uint32_t    pingsLost = 0;
    uint64_t    txBytes = 0, rxBytes = 0;
    int         ghosts = 0;
    int         players = 0, npcs = 0, worldObjects = 0;   ///< del feed, ahora en la escena
    /// Edad (ms) del ULTIMO ECO que la zona devolvio de TU transform. -1 = todavia no ha llegado
    /// ninguno, que es la diferencia entre "no hay nadie mas" (jugadores 0, eco si) y "mi transform
    /// no llega a ninguna zona" (jugadores 0, eco no). Ver `EntitySync::selfEchoAgeS`.
    float       selfEchoMs = -1.0f;
};

/// Lo del FRAME, que rellena quien lo dibuja (una vez por frame, antes de `render`).
struct DebugFrameInfo {
    float       dtSeconds = 0.0f;      ///< el tiempo del frame anterior
    double      worldTimeS = 0.0;      ///< reloj del mundo (simulacion; el del cluster si hay DGS)
    glm::dvec3  worldPos{0.0};         ///< coords del jugador (XYZ del mundo, m)
    int         sceneObjects = 0;
    int         localNpcs = 0;         ///< criaturas del juego (MonsterManager)
    int         scatterProps = 0;
    float       physicsMs = 0.0f;      ///< ms reales del último `PhysicsEngine::advance`
    int         physicsSteps = 0;      ///< pasos fijos 1/60 s ejecutados en ese advance
    DebugNetInfo net;
};

class DebugOverlay {
public:
    static DebugOverlay& get();

    void toggle() { m_visible = !m_visible; }
    void show(bool on) { m_visible = on; }
    bool visible() const { return m_visible; }

    /// Cada frame (aunque el panel este oculto: asi los percentiles estan listos al abrirlo).
    void pushFrame(const DebugFrameInfo& f);
    /// Dentro del frame de ImGui. No hace nada si esta oculto.
    void render();

    /// Recalcula fps y percentiles sobre la ventana (lo hace `render`; publico para el banco).
    void computeStats();
    /// Los percentiles del ultimo `computeStats` (para el banco / la consola). ms.
    float p50Ms() const { return m_p50; }
    float p98Ms() const { return m_p98; }
    float p995Ms() const { return m_p995; }
    float fps() const { return m_fps; }
    float txKiBs() const { return m_txKiBs; }
    float rxKiBs() const { return m_rxKiBs; }

private:
    DebugOverlay() = default;
    void readStatic();      ///< cpu, gpu, pantalla, API: una vez
    void readMemory();      ///< RSS y total: cada 0,5 s

    bool m_visible = false;
    bool m_staticRead = false;
    std::string m_cpu, m_gpu, m_display, m_api;

    static constexpr size_t kWindow = 240;   ///< frames para los percentiles (~4 s a 60)
    std::deque<float> m_dtMs;
    float m_fps = 0.0f, m_p50 = 0.0f, m_p98 = 0.0f, m_p995 = 0.0f;

    double   m_memClock = 0.0, m_memLast = -1.0;
    uint64_t m_rssBytes = 0, m_totalBytes = 0;

    // tx/rx: se deriva de los acumulados con el reloj del frame, media del ultimo segundo.
    double   m_netClock = 0.0, m_netLast = -1.0;
    uint64_t m_txLast = 0, m_rxLast = 0;
    float    m_txKiBs = 0.0f, m_rxKiBs = 0.0f;

    DebugFrameInfo m_last;
};

}} // namespace Haruka::Tools
