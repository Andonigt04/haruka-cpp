#version 460 core
#extension GL_GOOGLE_include_directive : require
#extension GL_ARB_gpu_shader_fp64      : require
/**
 * @file terrain_node_water.vert
 * @brief EL AGUA SOBRE EL QUADTREE DEL TERRENO. Mata el ultimo resto del clipmap.
 *
 * ── QUE SUSTITUYE, Y POR QUE ────────────────────────────────────────────────────────────────────
 *
 * El mar cercano no tenia geometria propia: reusaba `m_ringGridVB`, la rejilla CUADRADA que dejo el
 * clipmap del terreno al borrarse, con sus `ClipParams` por anillo y anclada bajo la camara. De ahi
 * colgaban todos sus defectos de sitio:
 *
 *   · CUADRADA con desvanecido RADIAL: la ola vivia en un disco inscrito y las esquinas (x1,41 mas
 *     lejos) se dibujaban planas. Geometria pagada y tirada.
 *   · ANCLADA A LA CAMARA: no movia la ola —el campo es funcion de la posicion del mundo— pero si el
 *     muestreo, y obligaba a mantener el marco, los anillos y sus UBOs solo para el agua.
 *   · UNA LEY DE LOD PROPIA (`4 m por segmento`, desvanecida a ojo entre 1200 y 9000 m) que no se
 *     hablaba con la del terreno y que, medida, dibujaba ola a plena amplitud sobre quads de 1 024 m.
 *
 * Aqui el agua usa EL MISMO nodo que el terreno: mismas instancias, mismo grid de vertices, mismo
 * direccionamiento entero por (cara, nivel, i, j) y el mismo LOD por error en pantalla. El agua deja
 * de tener sitio propio: esta donde esta el terreno, que es lo unico que tiene sentido.
 *
 * ── LO QUE ESTE SHADER **NO** HACE, A PROPOSITO ─────────────────────────────────────────────────
 *
 * Ni mapa de alturas del nodo, ni caida a ancestro, ni geomorph. El agua es una LAMINA: su cota no
 * sale del relieve del nodo sino del campo de agua (mar + lago horneado), asi que las 400 lineas que
 * `terrain_node.vert` dedica a leer el texel correcto de la cadena de ancestros aqui no pintan nada.
 * Por eso es un shader aparte y no un modo del otro: compartir el fichero habria significado meter
 * un `if` en el shader mas delicado del motor para no usar el 80 % de lo que hace.
 *
 * ⚠️ T-JUNCTIONS: no se cose. Dos nodos vecinos con stride distinto dejan una grieta del orden de la
 * sagita de la ola sobre un vano — milimetros sobre una superficie suave y continua en el mundo (la
 * fase de Gerstner depende solo de la posicion, asi que los dos lados evaluan LO MISMO donde
 * coinciden). En el terreno eso valia metros y por eso alli si se cose.
 */
#include "lib/backend.glsl"          // INSTANCE_INDEX: gl_InstanceID en GL, gl_InstanceIndex en Vulkan
#include "lib/cube_face.glsl"        // harukaCubeFaceToDir
#include "lib/terrain_detail.glsl"   // harukaSampleHeightField / harukaEquirectUV
#include "lib/ocean_params.glsl"     // harukaSeaLevelM, harukaWaveAt
#include "lib/ocean_wave.glsl"       // harukaGerstner
#include "lib/inland_water.glsl"     // harukaWaterLevelAt (mar + lago horneado + parche)

layout(location = 0) in vec2 aTexel;

layout(std140, binding = 0) uniform NodeDraw {
    mat4  uMVP; vec4 uCenter; vec4 uCenterLo; vec4 uLod; ivec4 uGrid; ivec4 uEdgeUnused;
    vec4  uMisc;        // x = radio del planeta · y = ¿hay bake equirect? · w = ¿hay campo de cubo?
    vec4  uShade;
    vec4  uTexAnchor;
    vec4  uLightDir;
};
struct NodeInst { ivec4 node; ivec4 edge; ivec4 slot; vec4 misc; };
layout(std430, binding = 2) readonly buffer Insts { NodeInst uInst[]; };
layout(binding = 16) uniform sampler2D uHeightTex;

layout(location = 0) out vec3  vNormal;
layout(location = 1) out vec3  vFragPos;
layout(location = 2) out float vDepth;    // profundidad del agua: < 0 = tierra, el fragmento descarta
layout(location = 3) out float vFoam;

void main() {
    const NodeInst I = uInst[INSTANCE_INDEX];
    const int   stride = 1 << I.slot.z;
    const int   cells  = uGrid.y;

    // ── EL TEXEL, CON LA ZANCADA DEL NODO ───────────────────────────────────────────────────────
    // El grid de vertices es el mismo que el del terreno; la zancada lo diezma. Se recorta al rango
    // para que el borde caiga EXACTAMENTE en la arista del nodo y no se pase.
    const ivec2 t = ivec2(min(int(aTexel.x) * stride, cells),
                          min(int(aTexel.y) * stride, cells));

    // ── COORDENADA DE CARA DESDE ENTEROS, gemelo de `nodeTexelFaceCoord` ────────────────────────
    // Numerador entero, UNA division, sin float por el camino: es lo que hace que dos nodos vecinos
    // coincidan BIT A BIT en su arista compartida.
    precise double cellsD = double(cells);
    precise double den    = double(1u << uint(I.node.y)) * cellsD;
    precise double lx     = -1.0LF + 2.0LF * ((double(I.node.z) * cellsD + double(t.x)) / den);
    precise double ly     = -1.0LF + 2.0LF * ((double(I.node.w) * cellsD + double(t.y)) / den);
    const dvec3  dirD = harukaCubeFaceToDir(I.node.x, lx, ly);
    const vec3   dir  = vec3(dirD);

    const float R = uMisc.x;
    // ── LA COTA DEL AGUA: mar, lago horneado o parche dinamico, en UNA respuesta ────────────────
    const vec3  posRelEye = dir * R + uCenter.xyz;
    const float level = harukaWaterLevelAt(posRelEye, dir, harukaSeaLevelM());

    // ── LA PROFUNDIDAD, del MISMO bake que dibuja el terreno y que pisa la fisica ───────────────
    // ⚠️ LA BANDERA SALE DE LA PROPIA TEXTURA, NO DE `uMisc.y`. Ese flag lo escribe el pase de
    // TERRENO para SU textura de altura, y el agua ata la suya: con un cuerpo donde el terreno no
    // tenia bake pero el agua si, `uMisc.y` valia 0, `baseH` salia 0, la profundidad 0 y **el agua se
    // descartaba entera**. Lo cazo `testNodeWaterDraws` con 0 pixeles en las dos pasadas. El relleno
    // que se ata cuando no hay bake es de 1x1, asi que el tamano lo distingue sin ambiguedad.
    const ivec2 hSize = textureSize(uHeightTex, 0);
    const float baseH = (hSize.x > 2)
                      ? harukaSampleHeightField(uHeightTex, hSize, harukaEquirectUV(dir))
                      : 0.0;
    const float depthRest = level - baseH;
    vDepth = depthRest;

    // ── LA OLA ──────────────────────────────────────────────────────────────────────────────────
    //
    // ⚠️ EL `quad` SALE DEL NODO, y ese es medio motivo de esta migracion. `harukaGerstner` apaga
    // cada tren cuando su longitud baja de dos quads (Nyquist); con la rejilla del clipmap ese dato
    // habia que reconstruirlo replicando su ley, y aqui es sencillamente el paso del nodo.
    const float quadM = float(uLod.z) / float(1 << I.node.y) / float(cells) * float(stride);
    vec3  wp = dir * (R + level);
    vec3  n  = dir;
    float foam = 0.0;
    vec3  disp = vec3(0.0);
    if (depthRest > 0.0) {
        disp = harukaGerstner(wp, dir, harukaOceanTime(), depthRest, harukaBakedFetchAt(dir), quadM, 1.0,
                              n, foam);
    }
    vNormal = n;
    vFoam   = foam;

    // ⚠️ LA POSICION EN DOUBLE Y CON `uCenter` PARTIDO, igual que el terreno y por lo mismo: en float
    // el ulp a radio terrestre son 0,5 m, o sea que el agua temblaria al mover la camara. Ver la nota
    // larga de `terrain_node.vert`.
    precise dvec3 pRel = dirD * (double(R) + double(level)) + dvec3(uCenter.xyz);
    vFragPos = vec3(pRel) + uCenterLo.xyz + disp;
    gl_Position = uMVP * vec4(vFragPos, 1.0);
}
