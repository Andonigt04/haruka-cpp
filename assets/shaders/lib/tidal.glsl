/**
 * @file tidal.glsl
 * @brief Cuerpos masivos que atraen/mueven el mar (Bloque UBO 21).
 *
 * Compartido por water.vert (desplaza la geometría de la esfera del océano) y water.frag
 * (efecto de fragmento). Antes el océano era una esfera RÍGIDA: sus olas perturbaban solo la
 * normal (luz), no la geometría. Aquí cada cuerpo masivo (sol, planeta, luna) abre un pozo de
 * marea en el punto de superficie: sumamos el término clásico (3·cos²θ−1)/2 del potencial de
 * marea, evaluado con la posición ACTUAL de cada cuerpo. Como los cuerpos orbitan, el pozo
 * barre el océano cada frame → el mar sube/baja bajo la masa y las ondas se mueven solas.
 *
 * Es ADITIVO Y NEUTRO: con `uTide.x == 0` (sin cuerpos —p. ej. un planeta suelto sin sistema
 * orbital) la altura devuelta es 0 y el agua queda exactamente igual que antes.
 */
#ifndef HARUKA_TIDAL_GLSL
#define HARUKA_TIDAL_GLSL

const int kMaxTidals = 8;

// std140. `uBody[i].xyz` = posición relativa al CENTRO del planeta (m); `uBody[i].w` = GM del
// cuerpo (m³/s²). `uTide.x` = número de cuerpos activos; `uTide.y` = escala artística que convierte
// el potencial (1/s²) en metros (≈ r²/g · factor de exageración); `uTide.z` = suelo mínimo de la
// distancia |cuerpo − superficie| para no dividir por cero; `uTide.w` = radio del planeta (m).
layout(std140, binding = 21) uniform TidalsUBO {
    vec4 uBody[kMaxTidals];
    vec4 uTide;
};

/**
 * @brief Altura de marea (m) que eleva/hunde la superficie en el punto `surf` (relativo al centro
 * del planeta), por TODOS los cuerpos masivos. Positivo = bulto hacia el cuerpo y en su antípoda;
 * negativo = leve caída en los lados. Con 0 cuerpos devuelve 0.
 */
float tidalHeight(vec3 surf) {
    float h = 0.0;
    int   cnt = int(min(kMaxTidals, uTide.x));
    for (int i = 0; i < cnt; ++i) {
        vec3  to = uBody[i].xyz - surf;
        float D  = length(to);
        // Suelo a la distancia al cuerpo (uTide.z): un cuerpo en el centro o pegado a la
        // superficie no debe inflar el término a infinito.
        D = max(D, uTide.z);
        float L = max(length(uBody[i].xyz), 1.0);
        float c = dot(normalize(surf), uBody[i].xyz) / L;
        // (3·c²−1)/2 del potencial de marea (bulto hacia el cuerpo y a la antípoda, caída en el
        // plano perpendicular), normalizado por D³. `uTide.y` lo convierte a metros y lo exagera.
        h += uBody[i].w * (3.0 * c * c - 1.0) / (2.0 * D * D * D);
    }
    return h * uTide.y;
}

/**
 * @brief Oleaje VISIBLE que radia desde cada cuerpo masivo (0..~cnt). La marea geométrica es de
 * pocos metros y en la escala de un planeta no se percibe; esto da el MOVIMIENTO. Cada cuerpo
 * emite un TREN DE CRESTAS circulares NÍTIDAS (tipo ola real) que se propagan hacia afuera con el
 * tiempo (`cos(arc·11 − t·3)` elevado a 12 = bandas delgadas), multiplicadas por una gaussiana en
 * la distancia angular para que sean intensas cerca del pozo y se apaguen al alejarse. Como la
 * dirección del cuerpo cambia al orbitar, el origen del tren barre con él.
 */
float tidalRings(vec3 surf, float t) {
    float a = 0.0;
    vec3  d = normalize(surf);
    int   cnt = int(min(kMaxTidals, uTide.x));
    for (int i = 0; i < cnt; ++i) {
        vec3  bdir = normalize(uBody[i].xyz);
        float arc  = acos(clamp(dot(d, bdir), -1.0, 1.0));
        float phase = arc * 11.0 - t * 3.0;              // onda circular que se propaga hacia afuera
        float crest = pow(max(cos(phase), 0.0), 12.0);   // cresta delgada y nítida
        float env   = exp(-arc * arc * 0.8);             // decae con la distancia al pozo
        a += crest * env;
    }
    return a;   // 0..cnt: alto justo en las crestas (espuma/agitación)
}

#endif // HARUKA_TIDAL_GLSL