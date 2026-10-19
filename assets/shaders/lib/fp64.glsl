/**
 * @file fp64.glsl
 * @brief Herramientas para MEDIR doble precision en la GPU sin que el compilador mienta.
 *
 * ⚠️ `a - double(float(a))` NO SIRVE PARA MEDIR NADA. El compilador del driver (NIR, en Mesa) trata
 * la ida y vuelta double -> float -> double como la identidad, tambien con `precise`, y el residuo
 * sale CERO aunque la aritmetica sea de doble de verdad. Medido el 16-10 en una RX 6600, GL y Vulkan:
 * los 16 bits bajos de `6371000 * 0.31f` salian 30720 en GPU y 30720 en CPU (producto exacto) y el
 * residuo por ida y vuelta salia 0. Con eso el banco acuso durante semanas al driver de degradar el
 * producto y la division en doble.
 *
 * Aqui el redondeo a float se hace con ENTEROS sobre los bits del double: no hay conversion que
 * plegar. Gemelo de `(double)(float)a` en CPU para valores normales en rango de float.
 */
#ifndef HARUKA_FP64_GLSL
#define HARUKA_FP64_GLSL

/// `double(float(a))`, redondeo al par mas cercano, hecho sobre los bits. Valido para `|a|` normal
/// dentro del rango de float (no trata denormales ni desbordes: es para medir, no para calcular).
double harukaRoundToFloatD(double a) {
    uvec2 b = unpackDouble2x32(a);                 // x = 32 bits bajos, y = altos
    // El float guarda 23 bits de fraccion; el double 52. Se tiran los 29 bajos, todos en `b.x`.
    const uint drop = b.x & 0x1FFFFFFFu;
    const uint half_ = 0x10000000u;
    uint lo = b.x & ~0x1FFFFFFFu;
    uint hi = b.y;
    const bool odd = (lo & 0x20000000u) != 0u;
    if (drop > half_ || (drop == half_ && odd)) {
        lo += 0x20000000u;
        if (lo == 0u) hi += 1u;                    // acarreo a la palabra alta (y al exponente)
    }
    return packDouble2x32(uvec2(lo, hi));
}

/// Lo que un float NO puede guardar de `a`: el residuo sub-float. Cero exacto si `a` salio de float.
double harukaSubFloatResidual(double a) { return a - harukaRoundToFloatD(a); }

#endif // HARUKA_FP64_GLSL
