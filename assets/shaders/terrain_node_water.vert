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

    // ── EL SUELO PRIMERO, QUE LA COTA DEL AGUA DEPENDE DE EL ───────────────────────────────────
    //
    // El orden importa: el parche dinamico publica METROS DE LAMINA, no cota, y se apoya en ESTE
    // suelo (ver `harukaInlandWaterDepthAt`). Calcular el nivel antes obligaria a restar dos suelos
    // distintos, que es de donde salio la pelicula blanca.
    //
    // ── LA PROFUNDIDAD, contra EL SUELO QUE SE DIBUJA (bake + relieve) ─────────────────────────
    //
    // ⚠️ AQUI ESTABA EL AGUA QUE ATRAVESABA EL CERRO. Esto restaba `baseH`, el bake equirect A PELO,
    // y decia en su titulo que era "el MISMO bake que dibuja el terreno y que pisa la fisica". NO LO
    // ERA: el nodo le suma `harukaTerrainDetail` encima (`terrain_node.comp`, el bloque de `uMisc.z`)
    // y `sampleTerrainHeight` —la fisica, y el suelo con el que el fluido MIDE su lamina— tambien.
    //
    // Las dos puntas de la cadena median con campos distintos, asi que la resta acumulaba el relieve
    // entero. La cota que publica el parche de aguas someras es `suelo_COMPLETO + charco`
    // (`fluid_host.cpp`, "publicar la superficie del agua interior"), de modo que aqui salia:
    //
    //     profundidad = (bake + relieve + charco) − bake = relieve + charco
    //
    // Un charco de 5 cm sobre una ladera con 60 m de relieve se dibujaba como 60 m de agua, y esa
    // lamina plana se extendia hasta donde el BAKE subia por encima de ella — cortando el cerro de
    // verdad, con el borde recto donde acaba el soporte del parche. Reportado como "agua dinamica en
    // terreno sin agua inicial, con forma de cuadrado, que se mueve": el cuadrado es el parche de
    // 420 m anclado bajo el jugador, y lo que lo hacia visible era esta resta.
    //
    // El relieve llega a ±915 m (ver la nota de las dos octavas continentales en
    // `lib/terrain_detail.glsl`), y `harukaSeaLevelAttenuation` solo lo apaga por debajo de 5 m de
    // cota — por eso el MAR (nivel 0) se veia bien y el agua interior no. El sintoma vivia entero en
    // los lagos, los rios y el parche.
    //
    // ⚠️ LA BANDERA SALE DE LA PROPIA TEXTURA, NO DE `uMisc.y`. Ese flag lo escribe el pase de
    // TERRENO para SU textura de altura, y el agua ata la suya: con un cuerpo donde el terreno no
    // tenia bake pero el agua si, `uMisc.y` valia 0, `baseH` salia 0, la profundidad 0 y **el agua se
    // descartaba entera**. Lo cazo `testNodeWaterDraws` con 0 pixeles en las dos pasadas. El relleno
    // que se ata cuando no hay bake es de 1x1, asi que el tamano lo distingue sin ambiguedad.
    const ivec2 hSize = textureSize(uHeightTex, 0);
    const float baseH = (hSize.x > 2)
                      ? harukaSampleHeightField(uHeightTex, hSize, harukaEquirectUV(dir))
                      : 0.0;

    // El relieve, con EL MISMO orden de operaciones que `terrain_node.comp` —incluido el tope de "el
    // detalle no hunde tierra bajo el mar"—, para que la orilla que se ve sea la del suelo que se ve.
    //
    // ⚠️ EL CORTE ES EL DEL VANO DIBUJADO (texel x ZANCADA), NO EL TEXEL DEL NODO. Es la diferencia
    // entre que la lamina se apoye en el suelo o flote sobre el. Con `stride > 1` el terreno NO
    // dibuja su propio mapa: lee el del ANCESTRO `log2(stride)` niveles arriba, que se horneo con el
    // corte `texel x stride`. Cortando mas fino, el agua seca se apoyaba en un relieve que el terreno
    // no dibuja —el fino— y quedaba por ENCIMA de la superficie real: lamina en el aire. Es el mismo
    // error que mide `v5 F3` por zancada (0,0752 m con stride 1 · 0,1260 con 2 · 0,3857 con 4), y con
    // el corte del vano desaparece por construccion en el interior del nodo.
    const float quadM = float(uLod.z) / float(1 << I.node.y) / float(cells) * float(stride);
    float groundH = baseH;
    if (hSize.x > 2) {
        float det = harukaTerrainDetail(dirD, double(R) + double(baseH), quadM)
                  * harukaSeaLevelAttenuation(baseH);
        if (baseH > 0.0 && det < -baseH) det = -baseH;
        groundH = baseH + det;
    }

    // ── Y AHORA SI, LA COTA DEL AGUA: mar, lago horneado, ventana o parche, en UNA respuesta ────
    const vec3  posRelEye = dir * R + uCenter.xyz;
    const float level = harukaWaterLevelAt(posRelEye, dir, harukaSeaLevelM(), groundH);

    const float depthRest = level - groundH;
    vDepth = depthRest;

    // ── Y LA COTA A LA QUE SE DIBUJA EL VERTICE, QUE NO ES LA MISMA ─────────────────────────────
    //
    // ⚠️ AQUI ESTABAN LOS CUBOS. `harukaWaterLevelAt` devuelve `max(nivelDelMar, agua interior)`, asi
    // que un vertice SIN agua interior no devuelve "no hay agua": devuelve **la cota 0**. El
    // fragmento lo descarta (`vDepth <= 0`) y por eso parecia inofensivo — pero el VERTICE ya se
    // habia colocado ahi. Un charco a 1 025 m rodeado de vertices secos generaba triangulos que
    // caian de 1 025 m a 0 m: una CORTINA vertical de agua colgando del charco hasta el nivel del
    // mar. Lo que se ve de esa cortina es la parte con `vDepth > 0` mas lo que el terreno no tapa:
    // una caja con la cara de arriba rizada por Gerstner y paredes verticales. Reportado como
    // "aparecen cubos con relieve en la cara superior" y como "estela hacia abajo" — la estela es la
    // cortina cayendo por la ladera.
    //
    // El arreglo no toca el criterio: `vDepth` sigue siendo `nivel - suelo` y sigue descartando. Lo
    // que se corrige es la GEOMETRIA — en seco el vertice se apoya en el suelo en vez de desplomarse
    // al nivel del mar, asi que el borde del agua es una cuña que muere en la orilla y no una pared.
    //
    // ⚠️ EL MAR NO CAMBIA: alli `level` es 0 y `groundH` es negativo, o sea `max` = 0, el mismo
    // vertice de siempre. Por eso el oceano nunca enseño cubos y el agua interior si.
    const float drawLevel = max(level, groundH);

    // ── LA PENDIENTE DEL FONDO, para que la ola REFRACTE ────────────────────────────────────────
    //
    // Unitario en el plano tangente hacia agua MENOS profunda. Sale del MISMO campo del que sale la
    // profundidad (`uHeightTex`, bilineal), asi que la ola gira por el fondo que de verdad tiene
    // debajo y no por otro. Cuatro muestras, diferencias centradas en las dos tangentes.
    //
    // ⚠️ EL PASO ES UN TEXEL DEL BAKE, y sigue siendo lo correcto AQUI aunque la PROFUNDIDAD ya lleve
    // relieve (ver arriba). Lo que refracta una ola es la forma del fondo a la escala de su longitud
    // de onda —decenas de metros—, no el rizado del relieve fino: el campo base es justo esa escala.
    // Un paso mas fino giraria las crestas con detalle que la ola no puede sentir.
    vec3 slope = vec3(0.0);
    if (hSize.x > 2) {
        vec3 s1 = normalize(abs(dir.y) < 0.99 ? cross(dir, vec3(0,1,0)) : cross(dir, vec3(1,0,0)));
        vec3 s2 = cross(dir, s1);
        float e = 6.2831853 / float(hSize.x);          // un texel de longitud, en radianes
        float h1p = harukaSampleHeightField(uHeightTex, hSize, harukaEquirectUV(normalize(dir + s1*e)));
        float h1m = harukaSampleHeightField(uHeightTex, hSize, harukaEquirectUV(normalize(dir - s1*e)));
        float h2p = harukaSampleHeightField(uHeightTex, hSize, harukaEquirectUV(normalize(dir + s2*e)));
        float h2m = harukaSampleHeightField(uHeightTex, hSize, harukaEquirectUV(normalize(dir - s2*e)));
        vec3 g = s1 * (h1p - h1m) + s2 * (h2p - h2m);  // hacia el fondo que SUBE = hacia la orilla
        float gl = length(g);
        slope = (gl > 1.0e-6) ? (g / gl) : vec3(0.0);  // nulo = fondo llano = sin refraccion
    }

    // ── LA OLA ──────────────────────────────────────────────────────────────────────────────────
    //
    // ⚠️ EL `quad` SALE DEL NODO, y ese es medio motivo de esta migracion. `harukaGerstner` apaga
    // cada tren cuando su longitud baja de dos quads (Nyquist); con la rejilla del clipmap ese dato
    // habia que reconstruirlo replicando su ley, y aqui es sencillamente el paso del nodo.
    vec3  wp = dir * (R + drawLevel);   // == `level` donde hay agua: la ola no se entera
    vec3  n  = dir;
    float foam = 0.0;
    vec3  disp = vec3(0.0);
    if (depthRest > 0.0) {
        disp = harukaGerstner(wp, dir, harukaOceanTime(), depthRest, harukaWaterFetchAt(dir), quadM, 1.0,
                              slope, n, foam);
    }
    vNormal = n;
    vFoam   = foam;

    // ⚠️ LA POSICION EN DOUBLE Y CON `uCenter` PARTIDO, igual que el terreno y por lo mismo: en float
    // el ulp a radio terrestre son 0,5 m, o sea que el agua temblaria al mover la camara. Ver la nota
    // larga de `terrain_node.vert`.
    precise dvec3 pRel = dirD * (double(R) + double(drawLevel)) + dvec3(uCenter.xyz);
    vFragPos = vec3(pRel) + uCenterLo.xyz + disp;
    gl_Position = uMVP * vec4(vFragPos, 1.0);
}
