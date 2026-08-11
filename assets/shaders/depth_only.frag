#version 460 core
/**
 * @file depth_only.frag
 * @brief Fragment vacío para pases de SOLO PROFUNDIDAD (mapa de sombras, máscara de cielo).
 *
 * No escribe color a propósito: el pase de sombras solo necesita el depth buffer. Existe porque GL
 * rechaza un programa sin fragment stage y el RHI pide siempre las dos rutas.
 *
 * ⚠️ NO escribe `gl_FragDepth`: hacerlo desactivaría el early-Z de todo el pase, y aquí la
 * profundidad interpolada es exactamente la que queremos. (El motor usa glClipControl(ZERO_TO_ONE)
 * con reversed-Z; cualquier escritura manual tendría que ser `clip.z/clip.w` SIN el *0.5+0.5 de la
 * convención por defecto — el error que tenía `fluid_depth.frag`.)
 */
void main() {}
