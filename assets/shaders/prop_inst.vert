/**
 * @file prop_inst.vert
 * @brief Vertex shader INSTANCIADO para props del mundo (árboles, rocas, objetos). Los prototipos
 *        se hornean con el node material graph del IDE (malla + UVs + material PBR), y el modelo
 *        va CAMERA-RELATIVO en el stream de instancia (binding 1) con `view` solo rotación.
 *
 * In (per-vertex, binding 0):  aPos (loc 0), aNormal (loc 1, 10/10/10/2), aColorPk (loc 2, RGBA8), aUv (loc 9)
 *                              — layout del prototipo prop (POS/NORMAL/COLOR/UV). El UV va a 9
 *                              para no chocar con el stream de instancia (loc 3..8, ver abajo).
 * In (per-instance, binding 1): ver lib/instance.glsl (pos 3, giro 4, escala 5, tinte 7, rotas 10)
 *                              — MISMO layout que GPUInstancing::appendInstanceLayout.
 * Out: Normal (0), FragPos (1), Color (2), TexCoord (3) → prop_inst.frag
 * UBOs: PerFrameData (binding 0, MISMO que construction_inst) + PropParams (binding 6).
 */
#version 450 core
#extension GL_GOOGLE_include_directive : require

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec4 aNormal;     // RGB10A2_SNORM: xyz = normal (w sin usar)
layout(location = 2) in vec4 aColorPk;    // RGBA8: rgb × (a·4) = color (escala en el alfa, ver PropVertex)
layout(location = 9) in vec2 aUv;

// Stream de instancia (divisor=1): posición + giro + escala + tinte + partes rotas (40 B).
#include "lib/instance.glsl"
layout(location = 11) in float aPart;      // parte del esqueleto de ESTE vértice (per-vertex)

#include "lib/frame_ubo.glsl"   // PerFrameData (binding 0)

#include "lib/prop_params.glsl"   // PropParams (binding 6)
#include "lib/prop_sway.glsl"     // harukaPropSway: el viento, el mismo que la capa masiva

layout(location = 0) out vec3 Normal;
layout(location = 1) out vec3 FragPos;
layout(location = 2) out vec3 Color;
layout(location = 3) out vec2 TexCoord;

void main() {
    const vec3 aColor = aColorPk.rgb * (aColorPk.a * 4.0);
    // PARTE ROTA (rama arrancada a hachazos): la malla prototipo se comparte entre TODAS las
    // instancias, así que una rama no se puede quitar del buffer — se quita por instancia aquí.
    //
    // ⚠️ SE COLAPSA A UN PUNTO, NO SE EMPUJA FUERA DEL NDC. Mandar el vértice a (2,2,2) parece
    // equivalente y no lo es: la GPU RECORTA el triángulo contra el frustum y rasteriza el trozo que
    // queda, ESTIRADO entre los vértices reales y los expulsados. Ese fue exactamente el origen de
    // las bandas negras del terreno (ver terrain.tesc). Colapsando los tres vértices al MISMO punto
    // el triángulo tiene área cero y no produce ni un fragmento — y vale porque ningún triángulo
    // cruza dos partes (cada cono emite los suyos; lo fija el test `prop_collider`).
    if (harukaInstancePartBroken(aPart)) {
        gl_Position = projection * view * vec4(iPos + u_originRel.xyz, 1.0);
        Normal = vec3(0.0, 1.0, 0.0); FragPos = iPos + u_originRel.xyz; Color = vec3(0.0); TexCoord = vec2(0.0);
        return;
    }

    // VIENTO: solo las HOJAS (verde) ondean, más cuanto más alto en el prototipo (aPos.y = arriba
    // local). Fase por posición del prop (su traslación) → no van todas a la vez.
    vec3  pos  = aPos;
    float leaf = (aColor.g > aColor.r + 0.02 && aColor.g > aColor.b + 0.02) ? 1.0 : 0.0;
    float h    = clamp(aPos.y, 0.0, 4.0);
    float ph   = iPos.x * 0.15 + iPos.z * 0.15;
    pos += leaf * harukaPropSway(u_wind, u_time, ph, h);

    const vec4 q = harukaInstanceQuat();
    vec3 camRel = harukaInstancePoint(q, pos) + u_originRel.xyz;   // prototipo → origen → cámara
    FragPos  = camRel;
    Normal   = harukaInstanceNormal(q, aNormal.xyz);   // R·(n/s): exacta con escala no uniforme, sin inverse()
    Color    = aColor * harukaInstanceTint();
    TexCoord = aUv;
    gl_Position = projection * view * vec4(camRel, 1.0);
}
