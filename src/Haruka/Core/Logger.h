#pragma once
#include <cstdio>
#include <cstdarg>

namespace Haruka {

enum class LogLevel {
    Debug = 0,
    Info  = 1,
    Warn  = 2,
    Error = 3,
    /// Silencio total. `level < s_level` descarta todo, incluidos los errores.
    /// Existe para poder PROVOCAR un error a propósito en un test sin ensuciar la salida: un banco
    /// en verde que escupe una línea roja hace dudar de un resultado bueno.
    None  = 4,
};

void setLogLevel(LogLevel level);
LogLevel getLogLevel();

void vlog(LogLevel level, const char* component, const char* fmt, va_list args);
void log(LogLevel level, const char* component, const char* fmt, ...);

/**
 * @brief ¿Están encendidas las trazas de DIAGNÓSTICO por frame? (`HARUKA_DIAG=1`)
 *
 * Existe porque el log útil se estaba ahogando en el log de investigaciones YA CERRADAS: paridad del
 * terreno, contactos del personaje, coste de props, encuadre del sol… cada una servía para responder
 * UNA pregunta, y todas seguían imprimiendo cada frame mucho después de responderla. En una sesión
 * normal eso son cientos de líneas idénticas entre las que se pierde lo que sí importa (un modelo que
 * no carga, una capa de props que no coloca nada).
 *
 * ⚠️ No se BORRAN: las sondas costaron trabajo y volverán a hacer falta. Se apagan por defecto y se
 * recuperan con `HARUKA_DIAG=1`, igual que `HARUKA_COLLISION_WIRE` con el alambre de colisión.
 */
bool diagLogs();

} // namespace Haruka

#define HARUKA_LOGD(comp, ...)  Haruka::log(Haruka::LogLevel::Debug, (comp), __VA_ARGS__)
#define HARUKA_LOGI(comp, ...)  Haruka::log(Haruka::LogLevel::Info,  (comp), __VA_ARGS__)
#define HARUKA_LOGW(comp, ...)  Haruka::log(Haruka::LogLevel::Warn,  (comp), __VA_ARGS__)
#define HARUKA_LOGE(comp, ...)  Haruka::log(Haruka::LogLevel::Error, (comp), __VA_ARGS__)

/** @brief Traza de diagnóstico por frame: solo con `HARUKA_DIAG=1`. Ver `Haruka::diagLogs()`. */
#define HARUKA_LOGDIAG(comp, ...) do { if (Haruka::diagLogs()) HARUKA_LOGI((comp), __VA_ARGS__); } while (0)
