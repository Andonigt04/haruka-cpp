/**
 * @file rhitest_skysh.frag
 * @brief Shader SOLO DE TEST: expone `harukaSkySHEval` para que el banco pueda mirarlo por píxeles.
 *
 * ⚠️ Existe porque la "sombra plana" no se ve en ningún número que la CPU pueda comprobar sola. El
 * ambiente lo evalúa el FRAGMENT, y si su versión GPU pierde la direccionalidad —o diverge del
 * gemelo de CPU— el resultado es que todo lo que está en sombra sale del mismo color, que es
 * exactamente el bug que hubo. Este shader deja leer ese valor sin montar un planeta entero.
 *
 * Pinta dos mitades con DOS NORMALES OPUESTAS: izquierda mirando al cénit, derecha al nadir. Si el
 * ambiente es direccional, las dos mitades tienen que salir DISTINTAS; si sale plano, iguales.
 */
#version 450 core
#extension GL_GOOGLE_include_directive : require
layout(location = 0) out vec4 fragColor;
#include "lib/sky_sh.glsl"

void main()
{
    // Mitad izquierda: normal al cénit. Mitad derecha: al nadir. `up` fijo, que es el marco en el
    // que se integró el SH.
    vec3 up = vec3(0.0, 1.0, 0.0);
    vec3 n  = (gl_FragCoord.x < 128.0) ? up : -up;
    // Sin dividir por π: el test compara contra `skyAmbientEval` de la CPU, que devuelve irradiancia.
    // La normalización lambertiana la hace cada consumidor, no esta función.
    fragColor = vec4(harukaSkySHEval(n, up), 1.0);
}
