/**
 * @file ground_stamp.vert
 * @brief Pinta las HUELLAS en su ventana (textura cenital alrededor del jugador).
 *
 * Sin VBO ni instancias: 6 vértices por huella, el índice sale de `gl_VertexID/6` y los datos de un
 * array del UBO. La CPU ya ha proyectado cada pisada a coordenadas de la ventana [-1,1] (lo hace en
 * DOUBLE: las posiciones son absolutas sobre un planeta de 6.4e6 m y en float no quedaría resolución
 * para distinguir dos pisadas seguidas).
 *
 * La ventana se RE-PINTA ENTERA cada frame desde la lista de pisadas. Parece derrochador y es justo
 * lo contrario: evita tener que desplazar el contenido de la textura cuando el jugador se mueve
 * (scroll toroidal), que es el problema clásico de este tipo de campo. Re-centrar sale gratis.
 */
#version 450 core

layout(location = 0) out vec2  vUV;      // [-1,1]² dentro de la huella
layout(location = 1) out float vDepth;   // cuánto hunde esta huella, ya desvanecida por edad

// 384 huellas × vec4 = 6 KB. Cabe de sobra en un UBO (mín. garantizado 16 KB) y evita un SSBO.
// xy = centro en coords de ventana [-1,1] · z = radio (en las mismas unidades) · w = profundidad.
layout(std140, binding = 5) uniform StampParams {
    vec4 u_stamps[384];
};

void main() {
    const int id  = gl_VertexID / 6;
    const int crn = gl_VertexID % 6;
    vec4 st = u_stamps[id];

    vec2 c = vec2((crn == 1 || crn == 2 || crn == 4) ?  1.0 : -1.0,
                  (crn == 2 || crn == 4 || crn == 5) ?  1.0 : -1.0);
    vUV    = c;
    vDepth = st.w;
    // Profundidad fuera de rango para las huellas MUERTAS (w<=0): el triángulo sale degenerado y no
    // cuesta rasterizado. Así el número de huellas dibujadas es siempre el mismo (sin recompilar ni
    // rehacer el draw) pero solo pintan las vivas.
    gl_Position = (st.w > 0.0) ? vec4(st.xy + c * st.z, 0.0, 1.0)
                               : vec4(2.0, 2.0, 0.0, 1.0);
}
