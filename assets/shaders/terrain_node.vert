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
    vec4  uCenter;      // centro del planeta relativo al ojo, parte GRUESA (multiplo de 64 m)
    vec4  uCenterLo;    // ...y el resto. Ver la nota de `vFragPos`: van SEPARADOS a proposito
    vec4  uLod;         // x = radianes por pixel · y = errorPx · z = arco del nivel 0 (m) ·
                        // w = 1 si el morph por distancia esta encendido (HARUKA_TERRAIN_V5_NOMORPH)
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
    ivec4 edge;   // ZANCADA del cosido en TEXELES (0 = nada que coser): x=izq y=der z=abajo w=arriba
    ivec4 slot;   // x = HUECO · y = bits de arista con vecino MAS GRUESO · z = stride ·
                  // w = bits de arista con vecino MAS FINO (ahi NO se morfea)
    vec4  misc;   // x = MORPH POR DISTANCIA hacia el padre · yzw = 0
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
layout(location = 6) flat out int vStride;  // indice de stride del nodo (vista de depuracion 4)
layout(location = 7) flat out int vFace;    // cara del cubo (vista 5: atribuir un agujero)

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
        if      (u == 0u && uEdgeCoarser.x > 1) { e = 0; idx = v; }
        else if (u == E  && uEdgeCoarser.y > 1) { e = 1; idx = v; }
        else if (v == 0u && uEdgeCoarser.z > 1) { e = 2; idx = u; }
        else if (v == E  && uEdgeCoarser.w > 1) { e = 3; idx = u; }
        if (e >= 0) {
            // ⚠️ LA ZANCADA VIENE DADA, YA NO SE DEDUCE DEL NIVEL. Era `1 << (niveles de
            // diferencia)`, lo que da por hecho que los dos lados dibujan con el MISMO stride. Con
            // stride por nodo eso es falso: un vecino del mismo nivel y el doble de stride tiene la
            // mitad de vertices en la arista y abre la misma grieta que un nivel de diferencia. La
            // CPU manda `max(stridePropio, strideVecino << nivelesDeDiferencia)`.
            const uint stride = uint((e == 0) ? uEdgeCoarser.x : (e == 1) ? uEdgeCoarser.y
                                   : (e == 2) ? uEdgeCoarser.z : uEdgeCoarser.w);
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
    // ⚠️ LA DIRECCION EN DOUBLE, Y AQUI ESTABA EL PINCHO. Esto llamaba a `harukaCubeFaceToDirF` —la
    // version FLOAT— mientras `terrain_node.comp` usa la de DOUBLE para el mismo punto.
    //
    // Un ulp de un vector unitario en float son ~6e-8, y a radio terrestre eso son **0,38-0,76 m de
    // superficie**. Los vertices del nivel 17 estan a 0,596 m: el error de redondeo es del MISMO
    // ORDEN que la separacion entre ellos. No "distorsiona un poco": cuantiza la superficie a una
    // rejilla de medio metro, y esa cuantizacion es lo que se ve como crestas finas.
    //
    // El comentario de `vFragPos` daba por bueno ese muro diciendo que el error es ESTATICO por
    // vertice y "no se ve". Es falso: estatico no significa invisible cuando su tamano iguala al
    // paso de la malla. Medido sobre la captura de RenderDoc: periodo de la estria 0,3-1,4 m, y el
    // ulp de un float a 6,37e6 es 0,5 m.
    //
    // `dir` en float se conserva para lo que NO necesita sub-metro (normal, clima, `vUp`); la
    // POSICION se compone desde `dirD` y no baja a float hasta despues de cancelar con `uCenter`.
    precise dvec3 dirD = harukaCubeFaceToDir(uNode.x, lx0, ly0);
    vec3 dir = vec3(dirD);

    // ⚠️ SE PROBO DECIDIR EL MORPH CON LA POSICION FINAL (cosida) EN VEZ DE CON LA BASE `u0,v0`,
    // y NO CAMBIA NADA — medido, numeros identicos en los 7 casos del barrido de GPU.
    //
    // El razonamiento era bueno: dos nodos del mismo nivel con stride distinto cosen con zancadas
    // distintas, asi que sus `u0,v0` diferen para el MISMO punto, y evaluar ahi el morph daria
    // valores distintos a cada lado. Pero las grietas de ese tipo estan TODAS en el nivel 17, que es
    // el maximo: ahi el nodo no puede subdividirse, su error en pantalla supera el presupuesto y el
    // morph vale **0**. Donde ocurre el fallo, el morph no interviene.
    //
    // Conclusion util: esas 6 grietas son del COSIDO, no del morph. Y un cambio que deja los numeros
    // exactamente iguales es un no-op — la firma que ya delato otro esta misma sesion.

    // ⚠️ EL HUECO GUARDA DOS MAPAS: el propio y el del PADRE (ver `terrain_node.comp`).
    const uint texels   = uint(N) * uint(N);

    // ── ⚠️ EL CORTE DE OCTAVAS NO CASA CON EL PASO QUE SE DIBUJA, Y AQUI ESTA MEDIDO ────────────
    //
    // `terrainDetail` corta las octavas por debajo de `lambda/2` del TEXEL — Nyquist bien puesto: el
    // texel hace su trabajo. Pero la malla se dibuja cada TEXEL x STRIDE, asi que con stride 2 se
    // admiten octavas resueltas para el texel y se muestrean al DOBLE de paso. Eso si cae por debajo
    // de Nyquist, y produce picos que NO son relieve: la misma onda leida en fases distintas por dos
    // vertices contiguos.
    //
    // Medido en `terrain_node_octave_cut_nyquist`, laplaciano sobre la malla que SE DIBUJA:
    //     nivel 14, stride 2:  corte por texel 0,6709 m  ->  corte al paso real 0,1493 m  (4,5x)
    //     nivel 14, stride 4:  1,4132 m  ->  0,4263 m
    //     stride 1:            IDENTICO — bajo los pies no cambia una coma
    //
    // ⚠️ NO SE ARREGLA LEYENDO EL MAPA DEL PADRE SEGUN EL STRIDE, aunque los mapas ya esten ahi (el
    // padre ES el corte a 2x el texel). SE PROBO Y ROMPE LAS COSTURAS: dos nodos vecinos con stride
    // distinto pasarian a leer mapas DISTINTOS, y la altura dejaria de ser funcion solo de (nivel,
    // posicion del vertice) — la regla que ya tumbo otros cuatro intentos esta sesion. Medido: de 0
    // agujeros a 40 en el peor caso.
    //
    // Para que funcione, los vertices de una arista COMPARTIDA tendrian que leer el mapa que
    // corresponde a la zancada del COSIDO —dato que los dos lados comparten— y el interior el suyo.
    // Es un cambio por arista, no una linea. Queda identificado y medido, sin hacer.
    // El mapa que lee ESTE vertice: el de su propio stride, salvo si esta EN una arista cosida, donde
    // lee el de la ZANCADA DEL COSIDO. Esa zancada es `max(mio, del vecino)` y los DOS lados la
    // calculan igual, asi que sobre la arista compartida ambos leen el MISMO mapa — la regla de que
    // la altura sea funcion solo de (nivel, posicion del vertice) se mantiene.
    uint skMap = uint(clamp(IN.slot.z, 0, 2));
    {
        const uint E2 = uint(uGrid.y);
        int se = 0;
        if      (u == 0u && uEdgeCoarser.x > 1) se = uEdgeCoarser.x;
        else if (u == E2 && uEdgeCoarser.y > 1) se = uEdgeCoarser.y;
        else if (v == 0u && uEdgeCoarser.z > 1) se = uEdgeCoarser.z;
        else if (v == E2 && uEdgeCoarser.w > 1) se = uEdgeCoarser.w;
        if (se > 1) {
            uint k = 0u;
            while (k < 2u && (1 << int(k + 1u)) <= se) ++k;      // log2 acotado a los 3 mapas
            skMap = max(skMap, k);
        }
    }
    const uint slotBase = uint(slot) * texels * 3u + texels * skMap;
    const uint parBase  = uint(slot) * texels * 3u + texels * min(skMap + 1u, 2u);
    const uint granBase = uint(slot) * texels * 3u + texels * 2u;

    // ── GEOMORPH: hacia la altura del PADRE en la arista que linda con un vecino MAS GRUESO ──────
    //
    // El cosido pone el vertice en su sitio, pero el vecino grueso evalua OTRA funcion de relieve
    // (su corte de octavas es el doble), asi que quedaba un escalon de hasta 2,4 m. Aqui el nodo
    // fino adopta la altura de su padre justo en esa arista — que es exactamente lo que el vecino
    // calcula, porque el vecino ESTA al nivel del padre. El grueso no morfea: su vecino es mas fino.
    //
    // La rampa entra `kMorphCells` hacia dentro para que no quede un pliegue de una celda.
    // ── DOS MORPHS, Y EL MAXIMO DE LOS DOS ──────────────────────────────────────────────────────
    //
    //   · POR DISTANCIA (`IN.misc.x`): cuanto ha adoptado ya el nodo la altura de su padre. Cierra el
    //     salto EN EL TIEMPO, cuando el nodo se funde en el padre al alejarte. Sin esto la superficie
    //     brinca de una funcion a otra — reportado como "sobre todo al mover la camara".
    //   · POR ARISTA: cierra el escalon EN EL ESPACIO contra un vecino mas grueso que se dibuja a la
    //     vez. Es local al borde y entra `kMorphCells` hacia dentro.
    //
    // El maximo, no la suma: los dos apuntan al MISMO destino (la altura del padre), asi que
    // sumarlos pasaria de largo.
    const float kMorphCells = 8.0;

    // ── EL MORPH POR DISTANCIA ES POR VERTICE, NO POR NODO ──────────────────────────────────────
    //
    // Esto era `IN.misc.x`: UN escalar por instancia, constante en toda la superficie del nodo. Dos
    // nodos vecinos a distancias distintas recibian morphs distintos, asi que el MISMO punto 3D de
    // la arista compartida salia como `mix(propia, padre, morphA)` por un lado y `morphB` por el
    // otro. La grieta es `|morphA - morphB| · (padre - propia)` y esta medida:
    // `terrain_node_distance_morph_per_node` daba hasta **10,07 m**, con 1175 de 1327 parejas
    // adyacentes recibiendo morphs distintos a los dos lados.
    //
    // Ahora sale de la distancia de ESTE VERTICE con la MISMA formula que el selector
    // (`nodeScreenError`: texel del nivel / (distancia · radianes por pixel), y la misma rampa
    // `2·(errPx − e)/errPx`). Las dos caras de una arista compartida comparten `dir` y comparten
    // nivel, y leen el mismo UBO, asi que dan el mismo numero y la grieta no puede existir. Es como
    // funciona el geomorph clasico y es la razon de que aquel casara solo en las fronteras.
    //
    // ⚠️ EL RADIO ES EL BASE, SIN RELIEVE, A PROPOSITO. El relieve es justo lo que se esta mezclando:
    // meterlo en la distancia haria que el morph dependiera del resultado del morph, y ademas los dos
    // lados dejarian de compartir la entrada (cada uno tiene su propio mapa de alturas).
    float morph, morphPar;
    {
        const vec3  pRel = (uCenter.xyz + uCenterLo.xyz) + dir * uMisc.x;
        const float dist = max(length(pRel), 1.0);
        // ⚠️ EL MORPH VA AL NIVEL DEL MAPA QUE SE LEE, NO AL DEL NODO. Si este vertice lee el mapa del
        // padre (porque su arista se cose a una zancada mas gruesa), su morph tiene que ser el de ESE
        // nivel: si no, los dos lados de la arista usan los MISMOS mapas con PESOS distintos y la
        // costura se abre igual. Medido al intentarlo sin esto: de 0 agujeros a 132, todos en
        // frontera de nivel.
        const int  lvRead = max(int(uNode.y) - int(skMap), 0);
        const float texM = (uLod.z / exp2(float(lvRead))) / float(uGrid.y);
        const float e    = texM / (dist * uLod.x);
        // Una raiz (nivel 0) no tiene padre en el que fundirse. Gemelo de `nodeParentMorph`.
        morph = (lvRead == 0) ? 0.0
                              : clamp(2.0 * (uLod.y - e) / uLod.y, 0.0, 1.0) * uLod.w;
        // Y EL MISMO MORPH UN NIVEL MAS ARRIBA. El texel del padre es el doble, asi que su error en
        // pantalla es `2·e` — no hace falta ningun dato nuevo, solo la misma formula.
        morphPar = (lvRead <= 1) ? 0.0
                                 : clamp(2.0 * (uLod.y - e * 2.0) / uLod.y, 0.0, 1.0) * uLod.w;

        // ⚠️ AQUI SE PROBO APAGAR EL MORPH JUNTO A UN VECINO MAS FINO, Y SE REVIRTIO CON MEDIDA.
        //
        // La idea era buena y el numero tambien: el vecino fino morfea hacia MI altura cruda mientras
        // yo morfeo hacia la de mi padre, asi que apagarme aqui deberia cerrar el escalon. Y lo cierra:
        // entre NIVELES pasa de 2,747 m a 0,692 m. Pero abre 2,517 m entre nodos del MISMO nivel,
        // donde antes habia 0,000 m — o sea cambia un pincho por otro, igual que hizo el
        // estrechamiento de rampas antes que el.
        //
        // LA REGLA QUE LO EXPLICA, y que conviene no volver a romper: para que dos vecinos coincidan,
        // el morph tiene que ser funcion SOLO de (nivel, posicion del vertice). En cuanto depende de
        // la configuracion de vecinos del nodo, dos nodos del mismo nivel con vecindarios distintos
        // evaluan distinto el MISMO punto compartido. Por eso funciono el morph por vertice y por eso
        // fallan todos los parches por arista.
        //
        // Medido con `terrain_node_edge_audit_all`. La unica salida que respeta la regla es que el
        // fino apunte a lo que el grueso DIBUJA —`mix(padre, abuelo, m(L-1))`—, y para eso hace falta
        // el mapa del abuelo en el hueco: +65 KB por nodo, +130 MB de pool.
    }
    // ⚠️ AQUI VIVIA EL MORPH POR ARISTA, Y SE BORRO POR MEDIDA (2026-08-25).
    //
    // Era una rampa que entraba `kMorphCells` desde cada arista con vecino mas grueso, mas un
    // "estrechamiento" contra las aristas que no morfeaban. Dos capas de parche. Medido sobre 2 910
    // parejas adyacentes de un frame real, en LOS DOS ejes, con `terrain_node_edge_audit_all`:
    //
    //                            MISMO nivel   DISTINTO nivel
    //     rampas estrechadas       0,000 m        2,747 m
    //     rampas SIN estrechar     0,470 m        2,747 m
    //     SIN morph por arista     0,000 m        2,747 m   <- lo que queda ahora
    //
    // **La columna que importa es identica en los tres.** No cerraba nada: el caso de mismo nivel lo
    // cierra el morph POR VERTICE, y el de distinto nivel no lo tocaba. Lo unico que aportaba era la
    // grieta de esquina que el estrechamiento venia a tapar — un problema que se causaba a si mismo.
    //
    // ⚠️ Lo que SI hace falta y sigue: el COSIDO (`uEdgeCoarser`, arriba), que coloca los vertices
    // sobrantes sobre la recta del vecino grueso. Eso mide 0,5533 m sin el y 0,000 m con el, y tiene
    // contraprueba. Cosido y morph son cosas distintas; venian confundidas porque los dos leian
    // `edge`.

    // ── EL DESTINO DEL MORPH ES LO QUE EL PADRE *DIBUJA*, NO SU ALTURA CRUDA ────────────────────
    //
    // ⚠️ ESTE ERA EL FALLO QUE PRODUCIA LOS PINCHOS, y sobrevivio a cuatro intentos de arreglarlo por
    // otro sitio. Aqui ponia `mix(propia, padre, morph)`: el nodo fino, al llegar a una frontera de
    // nivel, apuntaba a la altura CRUDA de su padre. Pero el vecino que dibuja al otro lado ESTA en
    // el nivel del padre y se esta morfeando a su vez hacia el ABUELO. Los dos lados apuntaban a
    // superficies distintas sobre el MISMO punto.
    //
    // Medido: 2,747 m de escalon entre niveles, y el 100 % de las grietas de pantalla caian en
    // fronteras de nivel (0 en cruces de cara, 0 dentro de un nivel).
    //
    // La correccion es recursiva y cierra por construccion: el destino es `mix(padre, abuelo,
    // m(L-1))`, que es EXACTAMENTE lo que el vecino de nivel L-1 evalua en ese punto. Con `morph = 1`
    // las dos caras dan el mismo valor; con `morph = 0` sale la altura propia, como antes.
    //
    // Y respeta la regla que las otras tres soluciones rompian: sigue siendo funcion SOLO de
    // (nivel, posicion del vertice). No mira a los vecinos, asi que no puede desacordar con ellos.
    // ⚠️ POR QUE NO SE FUERZA `morph = 1` EN LA ARISTA. Probado y medido (2026-08-25), 5º intento.
    //
    // UN NODO DIBUJADO EN SU NIVEL SIEMPRE TIENE morph < 1, y es estructural: para estar dibujado,
    // su padre tiene que ser demasiado grueso (`e_padre > errPx`), y como `e_padre = 2·e`, eso obliga
    // a `e > errPx/2`, que es exactamente la condicion de `morph < 1`. Medido: 0,7744 en la peor
    // pareja. O sea que morfear hacia el padre tiene un SUELO de `(1-m)·(padre-propia)` y no puede
    // cerrar la costura por si solo. No es un ajuste pendiente: es el techo del esquema.
    //
    // Forzarlo a 1 en la fila de la arista (sin rampa, para no repetir el fallo del morph por arista)
    // se probo: entre NIVELES baja de 0,785 a 0,073 m, y abre 0,470 m entre nodos del MISMO nivel
    // —en las esquinas, donde un vecino de mi nivel no comparte mi vecino grueso—. En GPU no cambia
    // nada: 0-1 px de grieta con y sin el. Cambia un pincho por otro sin comprar nada medible.
    //
    // Es el QUINTO parche de esta familia que hace lo mismo. La regla que los explica todos: el morph
    // tiene que ser funcion SOLO de (nivel, posicion del vertice); en cuanto mira a los vecinos, dos
    // nodos del mismo nivel con vecindarios distintos evaluan distinto el punto que comparten.
    const float hPar0 = mix(uHeights[parBase  + v0 * uint(N) + u0],
                            uHeights[granBase + v0 * uint(N) + u0], morphPar);
    float h = mix(uHeights[slotBase + v0 * uint(N) + u0], hPar0, morph);

    // ── EL VERTICE SOBRANTE SE COLAPSA, NO SE INTERPOLA ─────────────────────────────────────────
    //
    // ⚠️ AQUI SE INTERPOLABA SOBRE LA RECTA DEL VECINO GRUESO, Y ERA CORRECTO EN EL MUNDO Y ROTO EN
    // PANTALLA. El vertice quedaba EXACTAMENTE sobre esa recta —los tests de CPU lo miden en
    // 0,000000 m, y el careo GPU↔GPU da 0 bits— pero topologicamente seguia siendo un vertice que el
    // otro lado NO tiene: una T-junction. El rasterizador cuantiza posiciones a una rejilla
    // sub-pixel, asi que dos aristas que coinciden matematicamente pueden separarse una fraccion de
    // pixel. Eso son pinholes, y es lo unico que ningun modelo de world-space puede ver.
    //
    // Se descartaron antes, con medida: el dato del generador (0 bits entre vecinos), la formula del
    // cosido (gemela exacta), la precision float (4 micras), el morph (vale 0 en el nivel 17), la
    // histeresis del stride y la zancada que se envia (0 desajustes en 224 parejas).
    //
    // La solucion no es mover el vertice: es no tener T-junction. Colapsandolo sobre el vertice del
    // grueso (`t = 0`), los triangulos que lo usaban quedan DEGENERADOS —area cero, ni un fragmento—
    // y la silueta de la arista pasa a ser exactamente la del vecino, con su mismo numero de
    // vertices. Cuesta unos pocos triangulos degenerados por arista cosida y ni un buffer nuevo.
    //
    // (`u1`,`v1`,`st` siguen calculandose: los usa la NORMAL, y quitarlos cambiaria el sombreado.)
    if (st > 0.0) {
        // Nada que hacer con la posicion: `u0,v0` YA es el vertice del grueso sobre el que colapsa.
    }
    vHeight = h;
    vStride = IN.slot.z;

    const vec3 dirF = dir;
    // ⚠️ EL CENTRO VIENE EN DOS TROZOS, Y EL ORDEN DE LA SUMA NO ES NEGOCIABLE.
    //
    // Componer relativo al ojo ya estaba bien; lo que faltaba es que `uCenter` MISMO cabe mal en un
    // float: su magnitud es el radio del planeta (6,37e6) y ahi el ulp es **0,5 m**. Como cambia
    // cada frame al andar, el terreno entero saltaba a escalones de medio metro mientras la camara
    // se movia suave. Medido en `terrain_node_ucenter_jitter`: **0,53 m de salto entre frames** con
    // un paso de camara de 0,083 m — seis veces el movimiento real, y en direccion arbitraria. Eso
    // era "el terreno tiembla al mover el personaje".
    //
    // ⚠️ El muro de 0,5 m de `dirF * (uMisc.x + h)` SIGUE AHI y no se toca: ese error es ESTATICO
    // por vertice (solo depende de la direccion y la altura, no de la camara), asi que distorsiona
    // el terreno una vez y no se ve. El que se veia era el que se MOVIA.
    //
    // `uCenter` es multiplo de 64 m, y todo multiplo de 64 hasta 6,37e6 es EXACTO en float (el ulp
    // ahi es 0,5). Asi que:
    //   1. `uCenter + dirF*(...)` cancela dos magnitudes parecidas -> resta exacta (Sterbenz), y el
    //      unico error que queda es el estatico del vertice.
    //   2. `+ uCenterLo` anade el movimiento fino de la camara sobre un resultado ya pequeno, donde
    //      el ulp del float es de micras.
    // Sumar los dos trozos ANTES (`(uCenter + uCenterLo) + ...`) tiraria `uCenterLo` en la primera
    // suma y dejaria esto exactamente como estaba.
    // ⚠️ EL PRODUCTO DE MAGNITUD PLANETARIA VA EN DOUBLE. `dirF * (R + h)` en float se redondea a
    // 0,5 m ANTES de cancelar con `uCenter`, asi que la cancelacion exacta no salva nada: el error
    // ya esta dentro. En double el ulp a 6,37e6 es 1e-9 m, y solo se baja a float cuando el
    // resultado ya es pequeno (relativo al ojo).
    precise dvec3 pRel = dirD * (double(uMisc.x) + double(h)) + dvec3(uCenter.xyz);
    vFragPos = vec3(pRel) + uCenterLo.xyz;

    // Normal por diferencias del propio heightmap: los vecinos están en el pool, así que sale de dos
    // lecturas más y no de evaluar el gradiente del ruido. En los bordes se usa el téxel propio
    // (el nodo comparte esa fila con su vecino, así que la costura no se abre por esto).
    // ⚠️ LA DIFERENCIA FINITA VA AL PASO DEL STRIDE, NO A ±1 TEXEL. Aqui estaba el pincho DENTRO de
    // cada nodo, y por eso ningun instrumento de costuras lo veia: no es una grieta, es SOMBREADO.
    //
    // La geometria se dibuja cada `stride` texeles —un triangulo puede abarcar 2, 4 o mas—, pero la
    // normal se calculaba siempre con los vecinos a ±1. O sea que la iluminacion describia una
    // superficie MUCHO mas fina que la que existe: dos vertices contiguos del mismo triangulo
    // recibian normales de rugosidad sub-triangulo, y al interpolarlas a lo largo de un triangulo
    // grande el sombreado salta. Se lee exactamente como pinchos, y empeora con el stride.
    //
    // Con el paso del stride, la normal describe la MISMA superficie que se rasteriza. Es la misma
    // regla que ya se aplica al morph y a la altura: todo lo que decide el vertice tiene que hablar
    // de la geometria que se dibuja, no de otra.
    const int  nStep = 1 << IN.slot.z;                 // stride del nodo, en texeles
    const uint um = uint(max(int(u) - nStep, 0)),  up_ = uint(min(int(u) + nStep, N - 1));
    const uint vm = uint(max(int(v) - nStep, 0)),  vp  = uint(min(int(v) + nStep, N - 1));
    // La NORMAL sale del mismo mapa morfeado: si no, la iluminacion describiria una superficie que
    // no es la que se dibuja justo en la banda del morph.
    const uint base = slotBase;
    // La misma composicion de tres niveles que arriba: si la normal usara otra, la iluminacion
    // describiria una superficie distinta de la que se dibuja.
    #define HARUKA_NODE_H(IDX) mix(uHeights[base + (IDX)], \
                                   mix(uHeights[parBase + (IDX)], uHeights[granBase + (IDX)], morphPar), \
                                   morph)
    const float hL = HARUKA_NODE_H(v * uint(N) + um);
    const float hR = HARUKA_NODE_H(v * uint(N) + up_);
    const float hD = HARUKA_NODE_H(vm * uint(N) + u);
    const float hU = HARUKA_NODE_H(vp * uint(N) + u);
    #undef HARUKA_NODE_H
    // Paso entre téxeles en metros: el lado del nodo entre sus celdas.
    const float stepM = float(uMisc.x * 1.5707963267948966LF / double(1u << uint(uNode.y))) / float(uGrid.y);
    // ⚠️ LOS EJES DEL GRADIENTE SON LOS DEL TEXEL, NO UNOS CUALESQUIERA. AQUI ESTABA EL CORRUGADO.
    //
    // `hR-hL` y `hU-hD` son diferencias a lo largo de la rejilla (u,v) de la cara del cubo. Se
    // aplicaban sobre un marco inventado —`cross(dir, (0,1,0))`— que NO esta alineado con esos ejes:
    // el angulo entre los dos marcos cambia con la posicion sobre la cara, asi que el gradiente
    // llegaba ROTADO, y la rotacion barre la superficie. Eso se ve como estrias diagonales regulares
    // por todo el suelo — corrugado de chapa, no picos sueltos.
    //
    // Se veia y no se medía: la geometria esta limpia (la vista 2, blanco plano, no tiene ni una
    // estria y la silueta contra el cielo es suave). Era SOMBREADO, y ningun test de posiciones
    // podia verlo.
    //
    // Las tangentes correctas son las direcciones en que crecen u y v SOBRE ESTA CARA, sacadas de la
    // misma proyeccion que coloca los vertices. Dos evaluaciones mas de `harukaCubeFaceToDirF`.
    precise double lxA = -1.0LF + 2.0LF * ((double(uNode.z) * cells + double(up_)) / den);
    precise double lxB = -1.0LF + 2.0LF * ((double(uNode.z) * cells + double(um )) / den);
    precise double lyA = -1.0LF + 2.0LF * ((double(uNode.w) * cells + double(vp )) / den);
    precise double lyB = -1.0LF + 2.0LF * ((double(uNode.w) * cells + double(vm )) / den);
    const vec3 dirU = harukaCubeFaceToDirF(uNode.x, float(lxA), float(ly0))
                    - harukaCubeFaceToDirF(uNode.x, float(lxB), float(ly0));
    const vec3 dirV = harukaCubeFaceToDirF(uNode.x, float(lx0), float(lyA))
                    - harukaCubeFaceToDirF(uNode.x, float(lx0), float(lyB));
    // Ortogonalizados contra la radial: el gradiente es tangente a la superficie, no radial.
    vec3 t1 = dirU - dirF * dot(dirU, dirF);
    vec3 t2 = dirV - dirF * dot(dirV, dirF);
    t1 = (dot(t1, t1) > 1e-20) ? normalize(t1)
                               : normalize(abs(dirF.y) < 0.99 ? cross(dirF, vec3(0, 1, 0))
                                                              : cross(dirF, vec3(1, 0, 0)));
    t2 = (dot(t2, t2) > 1e-20) ? normalize(t2) : cross(dirF, t1);
    // ⚠️ EL DENOMINADOR ES LA SEPARACION REAL, NO "2 texeles". Con el paso al stride ya no son dos, y
    // en los BORDES tampoco lo eran antes: `um`/`up_` se recortan al rango, asi que junto al borde la
    // separacion es 1 texel y se estaba dividiendo por 2 igualmente. Eso inclinaba la normal el doble
    // de lo debido en la primera fila de cada nodo — otra fuente de pico, esta ya arreglada de paso.
    const float duM = float(up_ - um) * stepM;
    const float dvM = float(vp  - vm) * stepM;
    vNormal = normalize(dirF - t1 * ((hR - hL) / max(duM, 1e-6)) - t2 * ((hU - hD) / max(dvM, 1e-6)));

    vLevel      = uNode.y;
    vFace       = uNode.x;
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
