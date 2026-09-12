/**
 * @file rhitest_waveprobe.frag
 * @brief Saca la ALTURA DE LA OLA que calcula la GPU, punto por punto, para carearla con la CPU.
 *
 * La ola vive DOS VECES: en `lib/ocean_wave.glsl` (la que se ve) y en `core/planet/ocean_wave.h` (la
 * que usa la fisica para flotar y arrastrar). Estan escritas como gemelas y hasta ahora no habia ni
 * un test que las comparase — si divergen, lo que se nada deja de ser lo que se ve y nadie se entera.
 *
 * Cada pixel es una MUESTRA: de su coordenada salen una posicion de mundo y una profundidad, las dos
 * deterministas, asi que la CPU puede pedir exactamente los mismos puntos. El barrido de profundidad
 * recorre a proposito la franja de rompiente (agua somera), que es donde el modelo hace mas cosas.
 *
 * La altura se codifica en 24 bits sobre RGB porque un RGBA8 normal da 1/255 (~8 mm) y eso no basta
 * para afirmar paridad: asi la resolucion es ~0,5 micras sobre un rango de +-4 m.
 */
#version 450 core
#extension GL_GOOGLE_include_directive : require

#include "lib/ocean_wave.glsl"

layout(location = 0) out vec4 FragColor;

layout(std140, binding = 0) uniform ProbeParams {
    vec4 uProbeOrigin;   // xyz = esquina del barrido en mundo · w = tiempo
    vec4 uProbeStep;     // xyz = avance por pixel en X · w = sin uso
    vec4 uProbeStepY;    // xyz = avance por pixel en Y · w = sin uso
    vec4 uProbeUp;       // xyz = normal de la superficie · w = sin uso
    vec4 uProbeDepth;    // x = profundidad minima · y = incremento por fila · z = alto en pixeles
};

// La altura se codifica en [-kRange, +kRange]. ⚠️ EL RANGO DEPENDE DEL MODO Y NO ES UN DETALLE: la
// altura no pasa de ~3 m, pero el vaiven HORIZONTAL llega a 9,79 m a 5 m de fondo (medido por
// `ocean_mesh_stretch`). Con los ±4 m de la altura, el modo horizontal saturaria justo en la franja
// que se quiere mirar —la rompiente— y el careo compararia dos valores recortados, que siempre
// coinciden. Un test que satura es un test que pasa por el motivo equivocado.
const float kRangeH = 4.0;    // altura
const float kRangeX = 16.0;   // horizontal

void main() {
    vec2  px = floor(gl_FragCoord.xy);
    vec3  wp = uProbeOrigin.xyz + uProbeStep.xyz * px.x + uProbeStepY.xyz * px.y;
    vec3  up = normalize(uProbeUp.xyz);
    float depth = uProbeDepth.x + uProbeDepth.y * px.y;

    vec3  n; float foam;
    // `vec3(0)` es el centinela de "sin dato" de `harukaRefract`, asi que pasar la pendiente a cero
    // es exactamente la sobrecarga de antes: los tests que no la usan no cambian ni un bit.
    vec3  pend = vec3(uProbeStep.w, uProbeStepY.w, uProbeUp.w);
    vec3  disp = harukaGerstner(wp, up, uProbeOrigin.w, depth, HARUKA_FETCH_UNLIMITED, 4.0, 1.0,
                                pend, n, foam);
    // La componente a lo largo de `up` ES la altura; las otras dos son el vaiven horizontal, que es
    // donde vive el vuelco. El marco tangente es EL MISMO que construyen `harukaGerstner` y
    // `oceanDisplacement` — si no lo fuera, se compararian proyecciones sobre ejes distintos.
    vec3  t1 = normalize(abs(up.y) < 0.99 ? cross(up, vec3(0.0, 1.0, 0.0))
                                          : cross(up, vec3(1.0, 0.0, 0.0)));
    vec3  t2 = cross(up, t1);
    int   modo = int(uProbeDepth.w + 0.5);
    float h = (modo == 1) ? dot(disp, t1) : (modo == 2) ? dot(disp, t2) : dot(disp, up);

    // 24 bits fijos: se parte el valor normalizado en tres bytes.
    float kRange = (modo == 0) ? kRangeH : kRangeX;
    float t = clamp((h + kRange) / (2.0 * kRange), 0.0, 1.0);
    float scaled = t * 16777215.0;
    float r = floor(scaled / 65536.0);
    float g = floor((scaled - r * 65536.0) / 256.0);
    float b = floor(scaled - r * 65536.0 - g * 256.0);
    // ⚠️ LA ESPUMA VIAJA EN EL ALFA, que estaba sin usar. Es la unica magnitud de la ola que no tenia
    // careo CPU<->GPU: se calculaba SOLO aqui, asi que no habia con que compararla. Con `oceanFoam`
    // en el gemelo de CPU, sale gratis en este mismo barrido — misma posicion, mismo tiempo y misma
    // profundidad que ya se decodifican para la altura.
    // 8 bits bastan: la espuma es un 0..1 que se usa para MEZCLAR color, y 1/255 esta muy por debajo
    // de lo que el ojo separa. La tolerancia del test es esa cuantizacion, no un margen elegido.
    FragColor = vec4(r / 255.0, g / 255.0, b / 255.0, clamp(foam, 0.0, 1.0));
}
