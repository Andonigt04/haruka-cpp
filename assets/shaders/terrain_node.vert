#version 460 core
#extension GL_GOOGLE_include_directive : require
#extension GL_ARB_gpu_shader_fp64      : require
/**
 * @file terrain_node.vert
 * @brief Dibuja UN nodo del quadtree leyendo su heightmap del pool (v5, fase F3).
 *
 * ── LO QUE ESTE SHADER *NO* HACE, QUE ES EL PUNTO ───────────────────────────────────────────────
 *
 * No evalúa ruido. Ni una octava. La altura sale de un `texelFetch` al pool que
 * `terrain_node.comp` llenó una vez.
 *
 * Compárese con lo que sustituye: `clipmap.tese` evalúa `harukaTerrainDetail` POR VÉRTICE Y POR
 * FRAME, y `biome.frag` la vuelve a evaluar por PÍXEL dentro de un sphere-trace de hasta 16 pasos
 * (`reanchorToFine`) — que es el coste que puso `present.swap` en 60 ms. Aquí el frame no paga ruido:
 * lo pagó el frame en que el nodo entró en el pool, y se amortiza mientras viva (medido: al andar
 * 48 m, el 89,3 % de los nodos sobreviven).
 *
 * ── LA POSICIÓN SALE DE ENTEROS, IGUAL QUE EN EL COMPUTE ────────────────────────────────────────
 *
 * ⚠️ El vértice `(u,v)` de este nodo tiene que caer EXACTAMENTE donde el compute evaluó el téxel
 * `(u,v)`; si no, la geometría describe una superficie y el heightmap otra. Por eso la coordenada de
 * cara se reconstruye aquí con la MISMA cuenta —numerador entero, una división, sin float por el
 * camino— y no se interpola desde un atributo del vértice.
 *
 * Es también lo que hace la paridad render↔colisión por construcción: los dos leen el mismo téxel.
 */

// (u, v) del vértice dentro del nodo. Llegan como float porque el RHI no declara un formato de
// vértice entero de dos canales — da igual: con 129 téxeles por lado, los enteros hasta 2^24 son
// exactos en float, así que `int(aTexel.x)` recupera el valor sin pérdida.
layout(location = 0) in vec2 aTexel;

#include "lib/backend.glsl"   // INSTANCE_INDEX: gl_InstanceID en GL, gl_InstanceIndex en Vulkan

// Solo lo que es igual para TODOS los nodos del frame. Lo de cada nodo va en el SSBO de abajo.
layout(std140, binding = 0) uniform NodeDraw {
    mat4  uMVP;
    vec4  uCenter;      // centro del planeta, relativo al ojo
    ivec4 uNodeUnused;  // (libre: era el nodo, ahora por instancia)
    ivec4 uGrid;        // x = téxeles · y = celdas · zw = 0
    ivec4 uEdgeUnused;  // (libre: eran los bordes, ahora por instancia)
    vec4  uMisc;        // x = radio del planeta (m) · y = ¿hay bake EQUIRECT? ·
                        // z = vista de depuración · w = ¿hay campo del cubo?
    // ⚠️ ESTOS TRES NO LOS USA EL VERTICE, Y AUN ASI TIENEN QUE ESTAR. Un bloque uniforme es UNA
    // definicion compartida por las etapas: si el fragment declara mas campos, GL rechaza el enlace
    // con "buffer block with binding 0 has mismatching definitions". Vulkan NO se queja —enlaza por
    // etapa— asi que el fallo solo aparece en OpenGL. Cualquier campo nuevo va en LOS DOS.
    vec4  uShade;
    vec4  uTexAnchor;
    vec4  uLightDir;
};

// ⚠️ SE PROBÓ CON UN SSBO INSTANCIADO Y SE REVIRTIÓ. La idea era colapsar 1 008 draws en uno, y
// midió **0 ms de mejora** (24,07 contra 24,24) — o sea que los draw calls no eran el cuello. Pero sí
// introdujo un bug: todas las instancias acababan dibujando el MISMO nodo, así que se veía un solo
// parche de terreno y nada alrededor. Coste cero y un fallo de correctitud es un mal cambio.
//
// Si algún día los draws SÍ son el cuello, el camino correcto es `drawIndexedIndirect` (que el RHI ya
// expone) y verificar el indexado por instancia con un test que MIRE la imagen, no solo el heightmap.

layout(std430, binding = 1) readonly buffer NodeHeights { float uHeights[]; };

// ⚠️ UN UBO REESCRITO ENTRE DRAWS NO EXISTE EN VULKAN, Y ESO VACIABA LA PANTALLA.
//
// Esto era un draw por nodo con `updateBuffer` del MISMO uniform buffer entre medias. En OpenGL eso
// funciona (el driver hace ghosting del buffer por draw). En Vulkan `updateBuffer` sobre memoria
// mapeada es un `memcpy` en tiempo de GRABACIÓN: los 365 draws quedan grabados apuntando al mismo
// buffer y, al ejecutarse, TODOS leen el último valor escrito. Se dibuja 365 veces el mismo nodo.
//
// Es exactamente el síntoma que se había atribuido al instanciado y por el que se revirtió ("todas
// las instancias dibujaban el MISMO nodo"): el instanciado no tenía la culpa, la tenía el UBO único.
// Medido con el test de cobertura: GL 100 %, Vulkan 0,2 % — o sea un parche y nada más.
//
// Ahora los datos por nodo van en un array indexado por instancia, escrito UNA vez antes del pase.
struct NodeInst {
    ivec4 node;   // x = cara · y = nivel · z = i · w = j
    ivec4 edge;   // niveles que el vecino es más grueso: x=izq y=der z=abajo w=arriba
    ivec4 slot;   // x = HUECO en el pool · yzw = 0
};
layout(std430, binding = 2) readonly buffer NodeInsts { NodeInst uInst[]; };

#include "lib/cube_face.glsl"     // harukaCubeFaceToDir — el mismo gemelo que usa el compute
#include "lib/terrain_detail.glsl" // harukaEquirectUV + harukaSampleHeightField (el bake equirect)
#include "lib/base_field.glsl"  // harukaSampleBaseField — el bake, para el CLIMA

// ⚠️ EL CLIMA SALE DEL BAKE, NO DE LA ALTURA DEL POOL.
//
// El pool guarda `baseH + detalle`, y la seleccion de material necesita `baseH` SOLA: es la que
// define la banda de arena de la costa (−30 m … +12 m). Sumarle el detalle la corrompe — el mismo
// error que `clipmap.tese` documenta al reves. Asi que se vuelve a muestrear el bake aqui, que es
// exactamente lo que hace el clipmap por vertice teselado.
layout(binding = 15) uniform sampler2DArray uBaseField;
layout(binding = 16) uniform sampler2D      uHeightTex;   // el bake EQUIRECT: ver terrain_node.comp

layout(location = 0) out vec3 vNormal;
layout(location = 1) out vec3 vFragPos;
layout(location = 2) out float vHeight;
layout(location = 3) flat out int vLevel;   // el nivel ya no está en el UBO: viaja por aquí
layout(location = 4) out vec3 vClimate;     // x = elevacion BASE en km · y = temp C · z = humedad
layout(location = 5) out vec3 vUp;          // radial: la normal "del planeta", para clima y costa

void main() {
    const NodeInst  IN = uInst[INSTANCE_INDEX];
    const ivec4 uNode        = IN.node;
    const ivec4 uEdgeCoarser = IN.edge;
    const int   slot         = IN.slot.x;
    const int N = uGrid.x;
    const uint u = uint(clamp(int(aTexel.x), 0, N - 1));
    const uint v = uint(clamp(int(aTexel.y), 0, N - 1));

    // ── COSTURA CON EL VECINO GRUESO (T-junction). Gemelo de `nodeStitch`. ──────────────────────
    //
    // Un vecino más grueso tiene la MITAD de vértices en la arista compartida, así que los que
    // sobran aquí se salen de su recta y abren una grieta (medido: 0,5533 m). Se colocan sobre esa
    // recta interpolando entre los dos que el grueso SÍ tiene — y como esos caen bit a bit sobre los
    // finos (`terrain_node_lattice`), la arista queda idéntica y NO hacen falta faldas.
    uint u0 = u, v0 = v, u1 = u, v1 = v;
    float st = 0.0;
    {
        const uint E = uint(uGrid.y);
        int  e = -1; uint idx = 0;
        if      (u == 0u && uEdgeCoarser.x > 0) { e = 0; idx = v; }
        else if (u == E  && uEdgeCoarser.y > 0) { e = 1; idx = v; }
        else if (v == 0u && uEdgeCoarser.z > 0) { e = 2; idx = u; }
        else if (v == E  && uEdgeCoarser.w > 0) { e = 3; idx = u; }
        if (e >= 0) {
            const int lv = (e == 0) ? uEdgeCoarser.x : (e == 1) ? uEdgeCoarser.y
                         : (e == 2) ? uEdgeCoarser.z : uEdgeCoarser.w;
            const uint stride = 1u << uint(lv);
            const uint b = (idx / stride) * stride;
            const uint nx = min(b + stride, E);
            if (nx != b) {
                st = float(idx - b) / float(nx - b);
                if (e <= 1) { v0 = b; v1 = nx; } else { u0 = b; u1 = nx; }
            }
        }
    }

    // GEMELO EXACTO de `nodeTexelFaceCoord` y del bloque equivalente de `terrain_node.comp`.
    // No reordenar: de esto depende que el vértice caiga sobre el téxel que le corresponde.
    precise double cells = double(uGrid.y);
    precise double den   = double(1u << uint(uNode.y)) * cells;
    precise double lx0 = -1.0LF + 2.0LF * ((double(uNode.z) * cells + double(u0)) / den);
    precise double ly0 = -1.0LF + 2.0LF * ((double(uNode.w) * cells + double(v0)) / den);
    vec3 dir = harukaCubeFaceToDirF(uNode.x, float(lx0), float(ly0));

    // ⚠️ EL HUECO GUARDA DOS MAPAS: el propio y el del PADRE (ver `terrain_node.comp`).
    const uint texels   = uint(N) * uint(N);
    const uint slotBase = uint(slot) * texels * 2u;
    const uint parBase  = slotBase + texels;

    // ── GEOMORPH: hacia la altura del PADRE en la arista que linda con un vecino MAS GRUESO ──────
    //
    // El cosido pone el vertice en su sitio, pero el vecino grueso evalua OTRA funcion de relieve
    // (su corte de octavas es el doble), asi que quedaba un escalon de hasta 2,4 m. Aqui el nodo
    // fino adopta la altura de su padre justo en esa arista — que es exactamente lo que el vecino
    // calcula, porque el vecino ESTA al nivel del padre. El grueso no morfea: su vecino es mas fino.
    //
    // La rampa entra `kMorphCells` hacia dentro para que no quede un pliegue de una celda.
    const float kMorphCells = 8.0;
    float morph = 0.0;
    {
        const float fu = float(u), fv = float(v), E = float(uGrid.y);
        if (uEdgeCoarser.x > 0) morph = max(morph, 1.0 - min(fu / kMorphCells, 1.0));
        if (uEdgeCoarser.y > 0) morph = max(morph, 1.0 - min((E - fu) / kMorphCells, 1.0));
        if (uEdgeCoarser.z > 0) morph = max(morph, 1.0 - min(fv / kMorphCells, 1.0));
        if (uEdgeCoarser.w > 0) morph = max(morph, 1.0 - min((E - fv) / kMorphCells, 1.0));
    }

    float h = mix(uHeights[slotBase + v0 * uint(N) + u0],
                  uHeights[parBase  + v0 * uint(N) + u0], morph);

    if (st > 0.0) {
        // ⚠️ Se interpolan las DIRECCIONES (y las alturas) y NO se re-normaliza: la arista del vecino
        // grueso es una RECTA entre sus dos vértices, no un arco. Normalizar aquí devolvería el
        // vértice a la esfera y reabriría la grieta, más pequeña pero grieta.
        precise double lx1 = -1.0LF + 2.0LF * ((double(uNode.z) * cells + double(u1)) / den);
        precise double ly1 = -1.0LF + 2.0LF * ((double(uNode.w) * cells + double(v1)) / den);
        const vec3 d1 = harukaCubeFaceToDirF(uNode.x, float(lx1), float(ly1));
        dir = dir + (d1 - dir) * st;
        const float h1 = mix(uHeights[slotBase + v1 * uint(N) + u1],
                             uHeights[parBase  + v1 * uint(N) + u1], morph);
        h   = h + (h1 - h) * st;
    }
    vHeight = h;

    const vec3 dirF = dir;
    // ⚠️ La posición se compone RELATIVA AL OJO (`uCenter` ya lo es). Sumar el radio del planeta en
    // coordenadas absolutas y restar la cámara después perdería toda la precisión: 6,37e6 en float
    // tiene un ulp de 0,5 m, que es el muro que ya documenta `clipmap_dir_parity`.
    vFragPos = uCenter.xyz + dirF * (uMisc.x + h);

    // Normal por diferencias del propio heightmap: los vecinos están en el pool, así que sale de dos
    // lecturas más y no de evaluar el gradiente del ruido. En los bordes se usa el téxel propio
    // (el nodo comparte esa fila con su vecino, así que la costura no se abre por esto).
    const uint um = uint(max(int(u) - 1, 0)),      up_ = uint(min(int(u) + 1, N - 1));
    const uint vm = uint(max(int(v) - 1, 0)),      vp  = uint(min(int(v) + 1, N - 1));
    // La NORMAL sale del mismo mapa morfeado: si no, la iluminacion describiria una superficie que
    // no es la que se dibuja justo en la banda del morph.
    const uint base = slotBase;
    const float hL = mix(uHeights[base + v * uint(N) + um],  uHeights[parBase + v * uint(N) + um],  morph);
    const float hR = mix(uHeights[base + v * uint(N) + up_], uHeights[parBase + v * uint(N) + up_], morph);
    const float hD = mix(uHeights[base + vm * uint(N) + u],  uHeights[parBase + vm * uint(N) + u],  morph);
    const float hU = mix(uHeights[base + vp * uint(N) + u],  uHeights[parBase + vp * uint(N) + u],  morph);
    // Paso entre téxeles en metros: el lado del nodo entre sus celdas.
    const float stepM = float(uMisc.x * 1.5707963267948966LF / double(1u << uint(uNode.y))) / float(uGrid.y);
    vec3 t1 = normalize(abs(dirF.y) < 0.99 ? cross(dirF, vec3(0, 1, 0)) : cross(dirF, vec3(1, 0, 0)));
    vec3 t2 = cross(dirF, t1);
    vNormal = normalize(dirF - t1 * ((hR - hL) / (2.0 * stepM)) - t2 * ((hU - hD) / (2.0 * stepM)));

    vLevel      = uNode.y;
    vUp         = dirF;
    // uMisc.w > 0.5 = hay bake atado. Ver la nota de `terrain_node.comp`: en Vulkan un descriptor
    // sin escribir es INDEFINIDO, no ceros, asi que no se muestrea a ciegas.
    // El CLIMA (temp, humedad) sale siempre del campo del cubo; la ELEVACION del mismo sitio que la
    // usan el clipmap y la fisica. Gemelo de `clipmap.tese`, que hace exactamente este reparto.
    const vec3 fld = (uMisc.w > 0.5) ? harukaSampleBaseField(uBaseField, dirF) : vec3(0.0);
    const float baseH = (uMisc.y > 0.5)
                      ? harukaSampleHeightField(uHeightTex, textureSize(uHeightTex, 0),
                                                harukaEquirectUV(dirF))
                      : fld.x;
    vClimate    = vec3(baseH * 0.001, fld.y, fld.z);
    gl_Position = uMVP * vec4(vFragPos, 1.0);
}
