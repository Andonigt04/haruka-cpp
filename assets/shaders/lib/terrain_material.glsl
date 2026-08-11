// ===========================================================================================
// MATERIALES DEL TERRENO — definición ÚNICA, compartida por los dos shaders de terreno.
//
// La incluyen `planet.frag` (terreno V3 por chunks) y el shader inline del SimplePlanet
// (`kBiomeFragSrc` en planetary_system.cpp). Antes cada uno tenía su copia del mismo look y
// cualquier ajuste había que hacerlo dos veces — la misma clase de duplicación que en este motor
// ya produjo "tres suelos distintos" y "dos climas describiendo el mismo planeta". Si el color de
// un bioma se ve distinto en el terreno de cerca y en el planeta de lejos, es que alguien tocó
// una copia.
//
// Un material NO es una textura: es una REGLA sobre el clima y la forma (humedad, temperatura,
// pendiente) más cómo se ve lo que cae dentro de ella. La regla la declara el JSON de la escena
// (`surface.materials`) y llega aquí en un UBO, así que añadir un material no toca GLSL.
//
// El COLOR no está aquí: lo pone el mapa de biomas, que sale del clima. El material aporta el
// matiz (`tint`), cuánto grano deja pasar su textura y cuánto relieve saca de su normal map.
//
// Layout (debe coincidir con `Haruka::Planet::TerrainMaterial` y con su subida a la GPU):
//   a = (humMin, humMax, tempMin, tempMax)
//   b = (slopeMin, slopeMax, tile, priority)   tile: -1 ninguno · 0 sand · 1 grass · 2 land · 3 rock
//   c = (tint.rgb, grain)
//   d = (detail, feather, zonePacked, flags)
//   e = (baseColor.rgb, colorWeight)   colorWeight <= 0 → este material no tiñe, manda el bioma
//       zonePacked = r*65536 + g*256 + b con r,g,b en 0..255. Cabe EXACTO en un float32 (mantisa de
//       24 bits → enteros exactos hasta 2^24 = 16777216, y el máximo aquí es 16777215). Se empaqueta
//       para no crecer el UBO por tres floats.
//       flags: bit0 = este material tiene zona pintada
// ===========================================================================================
#ifndef HARUKA_TERRAIN_MATERIAL_GLSL
#define HARUKA_TERRAIN_MATERIAL_GLSL

#define MAX_TERRAIN_MATERIALS 16

// `f` = banda de ALTURA en km: (elevMin, elevMax, elevFeather, libre). Gemelo de `GpuMat` en
// planet.cpp — si uno crece y el otro no, el UBO se desalinea y TODOS los materiales salen mal.
struct TerrainMat { vec4 a; vec4 b; vec4 c; vec4 d; vec4 e; vec4 f; };

layout(std140, binding = 12) uniform TerrainMaterials {
    vec4 uMatCount;                              // x = cuántos hay activos
    TerrainMat uMat[MAX_TERRAIN_MATERIALS];
};

// Pertenencia SUAVE a una banda [lo,hi]: 1 dentro, 0 fuera, con un degradado de anchura `f` en los
// bordes. Con un corte duro, dos materiales contiguos dejan una línea recta sobre el terreno que se
// ve a kilómetros; el degradado los funde.
float harukaBandWeight(float x, float lo, float hi, float f) {
    f = max(f, 1e-4);
    return smoothstep(lo - f, lo + f, x) * (1.0 - smoothstep(hi - f, hi + f, x));
}

// Recorre la tabla y devuelve el aspecto resultante en este píxel.
//   · El ASPECTO (tint/grain/detail) se MEZCLA por peso → transiciones suaves.
//   · El TILE lo gana la PRIORIDAD: mezclar dos texturas por píxel costaría el doble de muestreos
//     para algo que solo aporta grano, y el tile se nota mucho menos que el color.
// Desempaqueta el color de zona de un material (ver el layout de arriba).
vec3 harukaZoneColor(int i) {
    float p = uMat[i].d.z;
    float r = floor(p / 65536.0);
    float g = floor(mod(p, 65536.0) / 256.0);
    float b = mod(p, 256.0);
    return vec3(r, g, b);
}
bool harukaHasZone(int i) { return mod(uMat[i].d.w, 2.0) >= 1.0; }

/**
 * Material en este píxel.
 *
 * `zoneRGB` es el color leído del mapa de zonas (0..255) y `hasZoneMap` dice si ese mapa existe.
 * Cuando existe, la ZONA MANDA: se elige el material cuyo color de zona está más cerca y se ignoran
 * las reglas de clima. Es lo que hace que un mapa pintado a mano signifique algo — si el clima
 * pudiera contradecirlo, pintar una isla no garantizaría que ahí hubiera isla.
 *
 * Sin mapa (o en un material sin zona declarada) se cae a las reglas de humedad/temperatura/pendiente.
 */
void harukaSelectMaterial(float humid, float tempC, float slope, float elevKm,
                          vec3 zoneRGB, bool hasZoneMap,
                          out vec3 tint, out float grainAmt, out float detailAmt, out int tile,
                          out vec4 baseColor, out int matIdx)
{
    tint = vec3(1.0); grainAmt = 1.0; detailAmt = 1.0; tile = 2;   // land por defecto
    // ⚠️ SIN ARRAY DE TERRENO, NINGÚN MATERIAL TIENE TILE.
    //
    // `uMatCount.y` dice si el array (bindings 12/13) existe de verdad. Si no existe, el sitio que lo
    // enlaza se salta el bind en silencio (`if (RHI::valid(...))`) y el shader se queda muestreando un
    // `sampler2DArray` SIN ENLAZAR: comportamiento indefinido, y en muchos drivers valores que varían
    // por téxel — o sea ruido. Forzando `tile = -1` el shader usa su valor neutro, que es el camino que
    // ya existe para los materiales sin textura (hielo, sal), y el suelo sale liso en vez de sucio.
    const bool hasTileArray = uMatCount.y > 0.5;
    if (!hasTileArray) tile = -1;   // también el valor por defecto: nada debe tocar el array
    baseColor = vec4(0.0);                                          // a = peso; 0 = manda el bioma
    matIdx = -1;
    int count = int(uMatCount.x);

    // Índice y peso del material que la ZONA PINTADA propone (si hay mapa y el píxel está pintado).
    // No sustituye a las reglas: compite con ellas en la mezcla de abajo. Ver la nota del `zoneW`.
    int   zoneMat = -1;
    float zoneW   = 0.0;

    if (hasZoneMap) {
        // VECINO MÁS CERCANO en el espacio de color, PERO CON UMBRAL — y esto es lo que hace que el
        // mapa de zonas sea PINTABLE en vez de todo-o-nada.
        //
        // ⚠️ Antes no había umbral, con el argumento de que un píxel desviado por el antialias del
        // pincel o por la recompresión del PNG debía caer en el material más parecido en vez de
        // quedarse sin material. El argumento es bueno para un píxel PINTADO; el problema es que se
        // aplicaba a TODOS. Consecuencia: en cuanto un material declaraba un color de zona, cada
        // píxel del planeta —incluido el fondo sin pintar— caía al más cercano y la función retornaba.
        // Clima, pendiente y altura quedaban ignorados EN TODO EL PLANETA. No se podía pintar solo un
        // volcán: pintabas uno y te llevabas el planeta entero.
        //
        // Con umbral, el mapa pasa a ser una CAPA ADITIVA: se pinta lo que se quiere fijar a mano y
        // el resto lo siguen decidiendo las reglas. Es el flujo que hace útil pintar texturas.
        //
        // Dos criterios, y hacen falta los dos:
        //   · NEGRO = SIN PINTAR. Convención explícita, porque un fondo negro está a distancia finita
        //     de cualquier material oscuro y solo con el umbral acabaría reclamado por él.
        //   · TOLERANCIA de ~28 por canal: absorbe el antialias del pincel y la recompresión, pero no
        //     confunde dos zonas distintas ni se traga el fondo.
        const float kZoneTol2   = 28.0 * 28.0 * 3.0;   // distancia² máxima para considerarlo pintado
        const float kUnpainted2 = 12.0 * 12.0 * 3.0;   // por debajo de esto es "negro" = sin pintar
        bool painted = dot(zoneRGB, zoneRGB) > kUnpainted2;

        int best = -1; float bestD = 0.0;
        if (painted) {
            for (int i = 0; i < count && i < MAX_TERRAIN_MATERIALS; ++i) {
                if (!harukaHasZone(i)) continue;
                vec3 d = harukaZoneColor(i) - zoneRGB;
                float dist = dot(d, d);
                if (best < 0 || dist < bestD) { best = i; bestD = dist; }
            }
            if (best >= 0 && bestD > kZoneTol2) best = -1;   // pintado, pero no de NINGUNA zona
        }
        // ⚠️ La zona NO retorna: entra en la mezcla normal con un peso fuerte.
        //
        // Retornando de golpe, una zona pintada apagaba TODAS las reglas dentro de ella: si pintas
        // una ladera volcánica, la roca por pendiente deja de aplicarse ahí y la montaña sale del
        // mismo material que el llano. Justo lo contrario de lo que uno quiere al pintar.
        //
        // Con peso, la zona dice "aquí el material BASE es este" y las reglas siguen pudiendo ganar
        // donde son más específicas: roca en los cortados, nieve en la cumbre. Quién gana lo decide
        // `priority`, que es el mecanismo que ya existía para eso — así que el control es del autor.
        //
        // El peso (8) es alto a propósito: sin nada que compita, la zona manda; un material con
        // `priority` mayor Y su banda cumpliéndose puede superarla. La intención original —"pintar una
        // isla garantiza que ahí hay isla"— sigue intacta, porque la decisión mar/tierra no es de aquí:
        // la toma `baseHeight` con el mismo mapa, y esa sí es un veto duro.
        if (best >= 0) { zoneMat = best; zoneW = 8.0; }
    }

    float wSum = 0.0, bestScore = -1.0;
    vec3  tintAcc = vec3(0.0), colAcc = vec3(0.0);
    float grainAcc = 0.0, detailAcc = 0.0, colW = 0.0;

    for (int i = 0; i < count && i < MAX_TERRAIN_MATERIALS; ++i) {
        float f = uMat[i].d.y;
        // El feather de temperatura va en GRADOS, no en la escala [0,1] de humedad/pendiente: medio
        // grado de degradado sería un corte duro, y 0.08 °C ni se ve.
        // Cuatro ejes: el clima dice en qué LATITUD estás, la pendiente distingue ladera de llano,
        // y la ALTURA da los pisos altitudinales — que es lo que hace que una montaña se lea como
        // montaña en vez de como una loma del mismo material.
        float w = harukaBandWeight(humid, uMat[i].a.x, uMat[i].a.y, f)
                * harukaBandWeight(tempC, uMat[i].a.z, uMat[i].a.w, max(f * 12.0, 0.5))
                * harukaBandWeight(slope, uMat[i].b.x, uMat[i].b.y, f)
                * harukaBandWeight(elevKm, uMat[i].f.x, uMat[i].f.y, max(uMat[i].f.z, 0.01));

        // ⚠️ El bonus de zona va ANTES del descarte, no después. Si fuera después, un material con
        // bandas restrictivas (`roca`: pendiente >0,5) daría peso 0, se descartaría aquí, y su zona
        // PINTADA no llegaría a aplicarse nunca — que es exactamente lo contrario de pintar.
        // Pintar significa "aquí este material va, aunque las reglas no lo pidieran".
        if (i == zoneMat) w += zoneW;
        if (w <= 0.0) continue;

        // ⚠️ El bonus de zona entra en el PESO (la mezcla de aspecto) pero NO en el SCORE (quién
        // gana la textura). Si entrara en los dos, un peso de zona alto haría matemáticamente
        // imposible que `priority` importara, y `priority` es justo la palanca del autor para decir
        // "la roca gana en los cortados". Separándolos: el color pintado domina el aspecto, y la
        // textura la decide la prioridad entre las reglas que de verdad se cumplen ahí.
        // El SCORE (quién gana la textura) se mide sin el bonus: si lo llevara, un peso de zona alto
        // haría matemáticamente imposible que `priority` importara, y `priority` es la palanca del
        // autor para decir "la roca gana en los cortados".
        float score = (w - (i == zoneMat ? zoneW : 0.0)) * uMat[i].b.w;
        if (i == zoneMat) score = max(score, 0.001 * uMat[i].b.w);   // pintado: compite, aunque poco
        wSum      += w;
        tintAcc   += uMat[i].c.rgb * w;
        grainAcc  += uMat[i].c.a   * w;
        detailAcc += uMat[i].d.x   * w;
        colAcc    += uMat[i].e.rgb * w;
        colW      += uMat[i].e.a   * w;
        if (score > bestScore) {
            bestScore = score;
            tile = hasTileArray ? int(uMat[i].b.z) : -1;
            matIdx = i;
        }
    }

    if (wSum > 1e-4) {
        tint = tintAcc / wSum; grainAmt = grainAcc / wSum; detailAmt = detailAcc / wSum;
        baseColor = vec4(colAcc / wSum, colW / wSum);
    }
}

#endif // HARUKA_TERRAIN_MATERIAL_GLSL
