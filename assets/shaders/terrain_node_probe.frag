#version 460 core
#extension GL_GOOGLE_include_directive : require
// ================================================================================================
// SONDA: ¿la geometría DIBUJADA cae sobre el campo de terreno, o está desplazada?
//
// ⚠️ ESTE ES EL ESLABON QUE NO COMPROBABA NADIE. El banco valida el bake (`terrain_node.comp` contra
// la referencia CPU: 0,02 m) y la colision esta medida contra el campo real (4 cm). Lo que va EN
// MEDIO —textura de alturas -> `terrain_node.vert` -> posicion en mundo— no lo miraba ningun test:
// `testTerrainNodeRender` dibuja, pero despues compara el SSBO de alturas, NO lo dibujado. Y en ese
// tramo viven `dirD`, el cosido de aristas, el morph y la cancelacion con `uCenter`.
//
// Se compara la posicion que el rasterizador interpola de verdad contra `harukaTerrainDetail`, que es
// una referencia INDEPENDIENTE del bake: el bake evalua en el centro del texel, esto evalua en el
// punto exacto que se esta pintando. Si la colocacion es correcta solo queda el error de cuerda de la
// rejilla (centimetros). Si el vertice esta mal puesto, sale en metros.
//
// ⚠️ EL RADIO SE CALCULA EN DOUBLE, Y NO ES OPCIONAL. Un float a 6,37e6 tiene un ulp de 0,5 m, que es
// EXACTAMENTE la magnitud a medir: en float esta sonda mediria su propio redondeo y daria un numero
// plausible y falso. `vFragPos` si puede ser float porque es relativo al OJO (escala de km, ulp 1e-4).
// ================================================================================================
#include "lib/terrain_detail.glsl"    // harukaTerrainDetail — gemelo validado de la funcion CPU

layout(location = 1) in vec3 vFragPos;    // relativo al ojo, tal cual lo compone el vertex shader

// ⚠️ GEMELO EXACTO del bloque de `terrain_node.vert` y de `DrawUBO`. Un campo de mas o de menos NO da
// error de compilacion: desplaza todo lo que viene detras y se lee basura. Los parametros propios de
// la sonda van en OTRO binding justo por esto — para no tocar el shader que se esta midiendo.
layout(std140, binding = 0) uniform NodeDraw {
    mat4  uMVP; vec4 uCenter; vec4 uCenterLo; vec4 uLod; ivec4 uGrid; ivec4 uEdgeUnused;
    vec4  uMisc;        // x = radio del planeta (m)
    vec4  uShade;
    vec4  uTexAnchor;
    vec4  uLightDir;
};

layout(std140, binding = 3) uniform Probe {
    vec4 uProbe;        // x = minFeatureM del nodo · y = metros a fondo de escala
                        // z = pendiente de `terrainTriM` (0 = comparar contra el CAMPO) · w = su piso
    vec4 uAnchor;       // xyz = direccion del ancla de la colision (donde esta el jugador)
};

layout(location = 0) out vec4 fragColor;

void main()
{
    // Posicion respecto al CENTRO DEL PLANETA. Los dos sumandos son de escala de km, asi que la resta
    // no pierde nada; de aqui en adelante, todo en double.
    dvec3  rel = dvec3(vFragPos) - dvec3(uCenter.xyz) - dvec3(uCenterLo.xyz);
    double r   = length(rel);
    dvec3  dir = rel / r;

    // ── LA REFERENCIA, EN DOS MODOS ─────────────────────────────────────────────────────────────
    //
    // z = 0: contra EL CAMPO con el corte del nodo. Responde "¿el vertice esta bien puesto?".
    // z > 0: contra LA SUPERFICIE DE COLISION, replicando `terrainTriM(radM) = max(radM*p, piso)`.
    //        Responde "¿cuanto se separa lo que se VE de lo que se PISA?", que es el numero que ve
    //        el jugador y el que dibuja el alambre de depuracion.
    //
    // ⚠️ `radM` es la distancia SOBRE LA SUPERFICIE desde el ancla, no la distancia a la camara:
    // es lo que recibe `terrainTriM` en `world_system_provider.h`. Medirla desde el ojo daria un
    // corte mas grueso del real en cuanto la camara se levanta del suelo.
    float cut = uProbe.x;
    if (uProbe.z > 0.0) {
        // ⚠️ POR LA CUERDA, NO CON `acos`, Y NO ES UN DETALLE: ESTO DABA CERO SIEMPRE.
        //
        // Estaba en `acos(float(dot(dir, ancla)))`. Un nodo de nivel 17 subtiende 6e-6 rad, asi que
        // `cosA = 1 - 1,8e-11` y en float eso ES 1,0 exacto (eps = 1,19e-7): `acos` devolvia 0 y
        // `radM` era cero en toda la pantalla. La referencia de colision se quedaba en su piso y la
        // sonda parecia funcionar —a nivel 14 hasta daba cifras distintas del otro modo, pero por el
        // PISO, no por la distancia. Lo canto la contraprueba de perturbar la pendiente x50 y no
        // cambiar nada. La cuerda `|dir - ancla|` no cancela: para angulos pequenos es el angulo.
        const dvec3  anchor = normalize(dvec3(uAnchor.xyz));
        const double radM   = double(uMisc.x) * length(dir - anchor);
        cut = float(max(radM * double(uProbe.z), double(uProbe.w)));
    }
    double href = double(harukaTerrainDetail(dir, double(uMisc.x), cut));
    double err  = r - (double(uMisc.x) + href);      // + = dibujado POR ENCIMA del campo

    // Codificado en RGBA8 porque `readPixels` no admite RGBA32F en los dos backends. Con fondo de
    // escala `uProbe.y` sobre 16 bits (R = byte alto, G = bajo) la resolucion es sub-milimetrica, de
    // sobra para un sintoma de medio metro. B marca el pixel como CUBIERTO: el clear deja B = 0 y asi
    // el lector distingue "sin terreno" de "error cero", que en 0,5 gris se verian igual.
    double fs = double(uProbe.y);
    double t  = clamp((err + fs) / (2.0LF * fs), 0.0LF, 1.0LF);   // 0,5 = error cero
    uint   q  = uint(t * 65535.0LF + 0.5LF);
    fragColor = vec4(float(q >> 8u) / 255.0, float(q & 255u) / 255.0, 1.0, 1.0);
}
