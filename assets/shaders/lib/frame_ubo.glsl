/**
 * @file frame_ubo.glsl
 * @brief El bloque PerFrameData (binding 0) COMPLETO y el tonemap de salida que cuelga de él.
 *
 * ⚠️ HABÍA 19 COPIAS de este bloque, en seis variantes: unas lo cortaban tras `enableHDR` y otras
 * renombraban campos con `_`. Todas leen el MISMO buffer (`m_uboPerFrameH`, 240 B), así que declararlo
 * entero es seguro y ahora todas incluyen esto.
 *
 * ⚠️ `cameraPos` ES LA POSICIÓN ABSOLUTA (~1,5e8 m en un sistema solar). Varias copias la llamaban
 * `_cameraPos` para que nadie la usara por error: los pases que dibujan CÁMARA-RELATIVO (props,
 * construcción, partículas, fluidos) tienen el ojo en el origen y deben usar `-FragPos`, no esto.
 *
 * Gemelo en C++: `SceneUBO.h`. Si se añade un campo, se añade en los dos y AL FINAL.
 */
#ifndef HARUKA_FRAME_UBO_GLSL
#define HARUKA_FRAME_UBO_GLSL

layout(std140, binding = 0) uniform PerFrameData {
    mat4 view;
    mat4 projection;
    vec3 cameraPos;      float _pad0;
    vec3 sunDirection;   float _pad1;
    vec3 sunLightColor;  float ambientStrength;
    int  enableHDR;
    int  enableBloom;
    int  enableSSAO;
    int  enableIBL;
    int  enableShadows;
    int  _pad3a; int _pad3b; int _pad3c; // 3 ints SUELTOS (un array std140 daría stride 16 → desalinea moon)
    vec3 moonDirection;  float moonIntensity;   // 2ª luz (luna)
    vec3 moonLightColor; float _pad4;
};

/// Salida de los pases forward. ⚠️ YA NO ES REINHARD + GAMMA: la luz llega expuesta y codificada
/// para pantalla desde `lib/sky_light.glsl` (`harukaLit`, `harukaDisplay`), igual que el terreno, el
/// agua y el cielo. Codificar otra vez aquí oscurecía las mallas frente a todo lo demás.
vec3 harukaOutputTonemap(vec3 c) {
    return clamp(c, 0.0, 1.0);
}

#endif // HARUKA_FRAME_UBO_GLSL
