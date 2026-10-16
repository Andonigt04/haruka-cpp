#version 460 core
#extension GL_GOOGLE_include_directive : require
/**
 * @file prop_depth_inst.vert
 * @brief Profundidad de props INSTANCIADOS para el mapa de sombras.
 *
 * Por qué existe: el motor dibuja los props del scatter (`m_propRegistry` + `m_propInstPSO`) pero el
 * pase de sombras solo llamaba al hook del juego, que dibuja SUS recursos. Resultado: ni un árbol en
 * el mapa de sombras, y por tanto ninguna sombra de prop.
 *
 * Es el mismo layout de instancia que el pase de color (`lib/instance.glsl`), para poder reusar el
 * MISMO buffer de instancias — si el layout divergiera, las sombras saldrían en otro sitio que los
 * objetos.
 *
 * La matriz de luz llega por UBO en vez de por uniform suelto: los uniforms sueltos no existen en
 * Vulkan y el RHI va por PSO.
 */

layout(location = 0) in vec3 aPos;
// locations 1 y 2 (normal, uv) existen en el vertex buffer pero no se usan aquí: la profundidad no
// necesita sombreado. Se declaran para que el layout del PSO case con el del pase de color.
#include "lib/instance.glsl"
layout(location = 11) in float aPart;       // parte del esqueleto de este vértice

layout(std140, binding = 0) uniform PropShadowUBO {
    mat4 uLightSpace;
    vec4 uOriginRel;   // origen del scatter relativo a la cámara (las instancias van relativas a él)
};

void main() {
    // Misma regla que el pase de color: una parte rota no proyecta sombra. Si faltara aquí, la rama
    // arrancada seguiría dibujando su sombra en el suelo — la forma más barata de delatar que lo que
    // se ve y lo que hay no son lo mismo. Colapsa a un punto (área cero), NO se expulsa del NDC.
    if (harukaInstancePartBroken(aPart)) {
        gl_Position = uLightSpace * vec4(iPos + uOriginRel.xyz, 1.0);
        return;
    }
    gl_Position = uLightSpace * vec4(harukaInstancePoint(harukaInstanceQuat(), aPos) + uOriginRel.xyz, 1.0);
}
