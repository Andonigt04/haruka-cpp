// ── COTA SOBRE EL NIVEL DEL MAR COMO COLOR (vista de edición del IDE, `Application::EditView`) ───
//
// Para esculpir con el agua apagada y ver DÓNDE HABRÁ AGUA: sobre 0, verde → ocre → marrón → blanco;
// bajo 0, azul que oscurece con la profundidad. Línea de costa (cota 0) en amarillo e isolíneas cada
// 10/50/250/1000 m según el zoom (la más fina que deje al menos 8 px entre líneas).
//
// La usan `terrain_node.frag` (los nodos) y `planet/biome.frag` (el anillo cercano y la malla base):
// UNA función, para que la costura entre las dos superficies no se vea como un cambio de color.
//
//   h      = cota sobre el nivel del mar (m). El campo ya está desplazado para que el mar sea el radio
//            R (`m_seaLevelOffsetM`); quien llama la calcula sin cancelación (en double o con
//            `frameAlt`), porque un float a 6,37e6 m tiene 0,5 m de ulp.
//   shade  = 0..1, la luz del relieve (para que se siga leyendo la forma).
vec3 harukaSeaTint(float h, float shade) {
    vec3 tint;
    if (h >= 0.0) {
        tint = (h < 400.0)  ? mix(vec3(0.20, 0.55, 0.20), vec3(0.75, 0.70, 0.30), h / 400.0)
             : (h < 1500.0) ? mix(vec3(0.75, 0.70, 0.30), vec3(0.50, 0.33, 0.20), (h - 400.0) / 1100.0)
             :                mix(vec3(0.50, 0.33, 0.20), vec3(0.95), clamp((h - 1500.0) / 1500.0, 0.0, 1.0));
    } else {
        tint = mix(vec3(0.35, 0.65, 0.95), vec3(0.03, 0.12, 0.40), clamp(-h / 500.0, 0.0, 1.0));
    }
    vec3 c = tint * (0.35 + 0.65 * clamp(shade, 0.0, 1.0));
    const float w = max(fwidth(h), 1e-4);                 // metros por píxel en esta zona
    float stepM = 10.0;
    if (w * 8.0 > stepM) stepM = 50.0;
    if (w * 8.0 > stepM) stepM = 250.0;
    if (w * 8.0 > stepM) stepM = 1000.0;
    const float tIso = abs(fract(h / stepM + 0.5) - 0.5) * stepM / w;   // píxeles a la isolínea
    c = mix(c, c * 0.45, (1.0 - smoothstep(0.5, 1.5, tIso)) * 0.8);
    const float tCoast = abs(h) / w;                      // píxeles a la costa
    return mix(c, vec3(1.0, 0.9, 0.2), 1.0 - smoothstep(1.0, 2.5, tCoast));
}
