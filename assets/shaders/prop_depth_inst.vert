#version 460 core
/**
 * @file prop_depth_inst.vert
 * @brief Profundidad de props INSTANCIADOS para el mapa de sombras.
 *
 * Por qué existe: el motor dibuja los props del scatter (`m_propRegistry` + `m_propInstPSO`) pero el
 * pase de sombras solo llamaba al hook del juego, que dibuja SUS recursos. Resultado: ni un árbol en
 * el mapa de sombras, y por tanto ninguna sombra de prop.
 *
 * Es el mismo layout de instancia que `gpu_instancing.h` declara (locations 3..6 = mat4 model,
 * 7 = color, 8 = escala), para poder reusar el MISMO buffer de instancias que el pase de color —
 * si el layout divergiera, las sombras saldrían en otro sitio que los objetos.
 *
 * La matriz de luz llega por UBO en vez de por uniform suelto: los uniforms sueltos no existen en
 * Vulkan y el RHI va por PSO.
 */

layout(location = 0) in vec3 aPos;
// locations 1 y 2 (normal, uv) existen en el vertex buffer pero no se usan aquí: la profundidad no
// necesita sombreado. Se declaran para que el layout del PSO case con el del pase de color.
layout(location = 3) in vec4 iModel0;
layout(location = 4) in vec4 iModel1;
layout(location = 5) in vec4 iModel2;
layout(location = 6) in vec4 iModel3;
layout(location = 10) in float iBreakMask;  // partes rotas de esta instancia
layout(location = 11) in float aPart;       // parte del esqueleto de este vértice

layout(std140, binding = 0) uniform PropShadowUBO {
    mat4 uLightSpace;
};

void main() {
    mat4 model = mat4(iModel0, iModel1, iModel2, iModel3);
    // Misma regla que el pase de color: una parte rota no proyecta sombra. Si faltara aquí, la rama
    // arrancada seguiría dibujando su sombra en el suelo — la forma más barata de delatar que lo que
    // se ve y lo que hay no son lo mismo. Colapsa a un punto (área cero), NO se expulsa del NDC.
    if ((uint(iBreakMask) & (1u << uint(aPart + 0.5))) != 0u) {
        gl_Position = uLightSpace * vec4(iModel3.xyz, 1.0);
        return;
    }
    gl_Position = uLightSpace * model * vec4(aPos, 1.0);
}
