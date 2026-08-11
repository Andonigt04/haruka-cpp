// ================================================================================================
// Escritor de PNG (src/io/image_writer.cpp): ida y vuelta contra un decodificador INDEPENDIENTE.
//
// Por qué así y no comparando contra sí mismo
// ------------------------------------------
// El escritor implementa DEFLATE a mano, y el error clásico de un DEFLATE escrito a mano es el
// empaquetado de bits: los códigos de Huffman se guardan empezando por su bit MÁS significativo
// mientras el resto de campos van del menos significativo al más. Con eso al revés sale un fichero
// que parece plausible —cabecera PNG correcta, CRC correcto, tamaño razonable— y que NINGÚN
// decodificador puede leer.
//
// Un test que compare el escritor consigo mismo no puede ver eso. Aquí se escribe con nuestro
// escritor y se LEE CON stb_image, que es código ajeno con su propio inflate, y se exige igualdad
// BYTE A BYTE. Si el empaquetado, los filtros de fila, el orden big-endian de 16 bits o el Adler32
// estuvieran mal, la lectura falla o los píxeles no cuadran.
//
// Y se comprueba el TAMAÑO, que es la razón del cambio: el escritor emitía bloques DEFLATE "stored"
// (sin comprimir) y la caché de horneado del planeta llegó a ocupar 3,7 GB con ratio 1,00x.
// ================================================================================================
#include "test_common.h"
#include "io/image_writer.h"
#include "stb_image.h"

#include <cmath>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace {

std::string tmpPath(const char* name) {
    return (std::filesystem::temp_directory_path() / name).string();
}

size_t fileSize(const std::string& p) {
    std::error_code ec;
    const auto s = std::filesystem::file_size(p, ec);
    return ec ? 0 : (size_t)s;
}

/// Generador determinista: no puede depender de rand() porque un fallo tiene que ser reproducible.
uint32_t lcg(uint32_t& s) { s = s * 1664525u + 1013904223u; return s; }

} // namespace

void test_image_writer_roundtrip() {
    beginTest("image_writer_roundtrip");

    const int W = 273, H = 149;   // NO potencias de dos, y anchura impar: el relleno de filas y el
                                  // vecino izquierdo del filtro son justo donde se cuela un off-by-one.

    // ── (1) RGBA de 8 bits, contenido SUAVE (lo que de verdad hornea el motor: mapas de bioma) ────
    {
        std::vector<unsigned char> src((size_t)W * H * 4);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                const size_t i = ((size_t)y * W + x) * 4;
                src[i + 0] = (unsigned char)(x * 255 / (W - 1));
                src[i + 1] = (unsigned char)(y * 255 / (H - 1));
                src[i + 2] = (unsigned char)(((x + y) / 2) & 0xFF);
                src[i + 3] = 255;
            }
        const std::string p = tmpPath("haruka_test_rgba.png");
        CHECK(Haruka::writePNG(p, W, H, 4, src.data()), "writePNG(RGBA) escribe");

        int rw = 0, rh = 0, rn = 0;
        unsigned char* got = stbi_load(p.c_str(), &rw, &rh, &rn, 4);
        CHECK(got != nullptr, "stb_image (inflate AJENO) puede leer el fichero");
        if (got) {
            CHECK(rw == W && rh == H, "dimensiones correctas al releer");
            bool same = true;
            for (size_t i = 0; i < src.size(); ++i) if (got[i] != src[i]) { same = false; break; }
            CHECK(same, "RGBA: ida y vuelta BYTE A BYTE (sin perdidas)");
            stbi_image_free(got);
        }
        // El degradado es el caso favorable de los filtros de fila: el residuo es casi constante.
        const size_t raw = src.size(), on = fileSize(p);
        std::printf("    RGBA suave  %dx%d: crudo %zu B -> fichero %zu B (%.1fx)\n",
                    W, H, raw, on, raw / (double)std::max<size_t>(on, 1));
        CHECK(on > 0 && on < raw / 4, "RGBA suave: comprime al menos 4x (antes era 1.00x)");
    }

    // ── (2) RGB de 8 bits ────────────────────────────────────────────────────────────────────────
    {
        std::vector<unsigned char> src((size_t)W * H * 3);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                const size_t i = ((size_t)y * W + x) * 3;
                const double r = std::sqrt((double)(x * x + y * y));
                src[i + 0] = (unsigned char)(127.5 + 127.0 * std::sin(r * 0.08));
                src[i + 1] = (unsigned char)(x & 0xFF);
                src[i + 2] = (unsigned char)((255 - y) & 0xFF);
            }
        const std::string p = tmpPath("haruka_test_rgb.png");
        CHECK(Haruka::writePNG(p, W, H, 3, src.data()), "writePNG(RGB) escribe");
        int rw = 0, rh = 0, rn = 0;
        unsigned char* got = stbi_load(p.c_str(), &rw, &rh, &rn, 3);
        CHECK(got != nullptr, "RGB: legible por stb_image");
        if (got) {
            bool same = true;
            for (size_t i = 0; i < src.size(); ++i) if (got[i] != src[i]) { same = false; break; }
            CHECK(same, "RGB: ida y vuelta byte a byte");
            stbi_image_free(got);
        }
    }

    // ── (3) GRIS DE 16 BITS — el caso por el que este escritor existe ─────────────────────────────
    //
    // `stbi_write_png` no sabe escribir 16 bits, y el bake de altura lo necesita: a 8 bits la
    // cuantización de la elevación sería de 78 m. Aquí se verifican DOS cosas que solo fallan en 16
    // bits: el orden BIG-ENDIAN de las muestras en el flujo PNG, y que el filtro use bpp=2 (el vecino
    // izquierdo de un byte es el byte homólogo de la muestra anterior, no el byte de al lado).
    {
        std::vector<unsigned short> src((size_t)W * H);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
                src[(size_t)y * W + x] = (unsigned short)((x * 65535) / (W - 1));
        const std::string p = tmpPath("haruka_test_g16.png");
        CHECK(Haruka::writePNG16(p, W, H, src.data()), "writePNG16 escribe");

        int rw = 0, rh = 0, rn = 0;
        unsigned short* got = stbi_load_16(p.c_str(), &rw, &rh, &rn, 1);
        CHECK(got != nullptr, "16 bits: legible por stb_image");
        if (got) {
            CHECK(rw == W && rh == H, "16 bits: dimensiones correctas");
            bool same = true;
            unsigned worst = 0;
            for (size_t i = 0; i < src.size(); ++i) {
                if (got[i] != src[i]) {
                    same = false;
                    worst = std::max<unsigned>(worst, (unsigned)std::abs((int)got[i] - (int)src[i]));
                }
            }
            CHECK(same, "16 bits: ida y vuelta EXACTA (si el orden de bytes estuviera mal, fallaria)");
            if (!same) std::printf("    peor desvio de 16 bits: %u\n", worst);
            stbi_image_free(got);
        }
        const size_t raw = src.size() * 2, on = fileSize(p);
        std::printf("    gris 16bit  %dx%d: crudo %zu B -> fichero %zu B (%.1fx)\n",
                    W, H, raw, on, raw / (double)std::max<size_t>(on, 1));
        CHECK(on > 0 && on < raw / 4, "16 bits suave: comprime al menos 4x");
    }

    // ── (4) EL CASO PEOR: ruido puro ──────────────────────────────────────────────────────────────
    //
    // El ruido blanco es INCOMPRESIBLE, así que aquí no se pide ratio: se pide que no se corrompa y
    // que no CREZCA de forma desbocada. Un compresor con un fallo en la selección de coincidencias
    // suele delatarse justo aquí (emitiendo distancias imposibles) y con datos suaves no se nota.
    {
        std::vector<unsigned char> src((size_t)W * H * 4);
        uint32_t s = 12345u;
        for (auto& b : src) b = (unsigned char)(lcg(s) >> 24);
        const std::string p = tmpPath("haruka_test_noise.png");
        CHECK(Haruka::writePNG(p, W, H, 4, src.data()), "ruido: escribe");
        int rw = 0, rh = 0, rn = 0;
        unsigned char* got = stbi_load(p.c_str(), &rw, &rh, &rn, 4);
        CHECK(got != nullptr, "ruido: legible");
        if (got) {
            bool same = true;
            for (size_t i = 0; i < src.size(); ++i) if (got[i] != src[i]) { same = false; break; }
            CHECK(same, "ruido incompresible: ida y vuelta byte a byte");
            stbi_image_free(got);
        }
        const size_t raw = src.size(), on = fileSize(p);
        std::printf("    ruido puro  %dx%d: crudo %zu B -> fichero %zu B (%.2fx)\n",
                    W, H, raw, on, raw / (double)std::max<size_t>(on, 1));
        // Con el respaldo "stored" el peor caso es ~1,00x del crudo: si el Huffman fijo hiciera crecer
        // el dato (los literales 144-255 cuestan 9 bits), `zlibBest` guarda sin comprimir. Sin ese
        // respaldo esto medía 1,056x — o sea que comprimir EMPEORABA el fichero.
        CHECK(on < raw + raw / 100 + 4096, "ruido: el respaldo stored evita que el fichero crezca");
    }

    // ── (5) CASOS LÍMITE de tamaño ────────────────────────────────────────────────────────────────
    //
    // 1×1 y 1 píxel de alto rompen el LZ77 (no hay 3 bytes para hashear) y el filtro (no hay fila
    // anterior). Son los tamaños que nadie prueba y que aparecen en cuanto alguien hornea un mapa
    // degenerado.
    {
        const unsigned char one[4] = { 10, 20, 30, 40 };
        const std::string p = tmpPath("haruka_test_1x1.png");
        CHECK(Haruka::writePNG(p, 1, 1, 4, one), "1x1: escribe");
        int rw = 0, rh = 0, rn = 0;
        unsigned char* got = stbi_load(p.c_str(), &rw, &rh, &rn, 4);
        CHECK(got != nullptr && rw == 1 && rh == 1, "1x1: legible");
        if (got) {
            CHECK(got[0] == 10 && got[1] == 20 && got[2] == 30 && got[3] == 40, "1x1: pixel correcto");
            stbi_image_free(got);
        }

        std::vector<unsigned char> row((size_t)W * 3);
        for (size_t i = 0; i < row.size(); ++i) row[i] = (unsigned char)(i & 0xFF);
        const std::string p2 = tmpPath("haruka_test_1row.png");
        CHECK(Haruka::writePNG(p2, W, 1, 3, row.data()), "una sola fila: escribe");
        unsigned char* g2 = stbi_load(p2.c_str(), &rw, &rh, &rn, 3);
        CHECK(g2 != nullptr && rw == W && rh == 1, "una sola fila: legible");
        if (g2) {
            bool same = true;
            for (size_t i = 0; i < row.size(); ++i) if (g2[i] != row[i]) { same = false; break; }
            CHECK(same, "una sola fila: ida y vuelta byte a byte");
            stbi_image_free(g2);
        }

        // Argumentos malos: no debe escribir nada ni reventar.
        CHECK(!Haruka::writePNG(tmpPath("haruka_bad.png"), 0, 10, 4, one), "w=0 -> false");
        CHECK(!Haruka::writePNG(tmpPath("haruka_bad.png"), 10, 10, 2, one), "canales=2 -> false");
        CHECK(!Haruka::writePNG(tmpPath("haruka_bad.png"), 10, 10, 4, nullptr), "pixeles nulos -> false");
        CHECK(!Haruka::writePNG16(tmpPath("haruka_bad.png"), 10, 0, nullptr), "16bit invalido -> false");
    }

    // ── (6) COINCIDENCIAS LARGAS: una imagen constante ────────────────────────────────────────────
    //
    // Todo ceros es el caso que mejor ejercita el LZ77 (coincidencias de 258, el máximo) y el que
    // detecta un error en la tabla de longitudes: si el símbolo o los bits extra se calcularan mal, la
    // salida se corrompería justo aquí, donde las coincidencias son máximas.
    {
        const int BW = 512, BH = 512;
        std::vector<unsigned char> flat((size_t)BW * BH * 4, 0);
        for (size_t i = 3; i < flat.size(); i += 4) flat[i] = 255;   // alfa opaco
        const std::string p = tmpPath("haruka_test_flat.png");
        CHECK(Haruka::writePNG(p, BW, BH, 4, flat.data()), "constante: escribe");
        int rw = 0, rh = 0, rn = 0;
        unsigned char* got = stbi_load(p.c_str(), &rw, &rh, &rn, 4);
        CHECK(got != nullptr, "constante: legible");
        if (got) {
            bool same = true;
            for (size_t i = 0; i < flat.size(); ++i) if (got[i] != flat[i]) { same = false; break; }
            CHECK(same, "constante: ida y vuelta byte a byte (coincidencias de 258)");
            stbi_image_free(got);
        }
        const size_t raw = flat.size(), on = fileSize(p);
        std::printf("    constante  %dx%d: crudo %zu B -> fichero %zu B (%.0fx)\n",
                    BW, BH, raw, on, raw / (double)std::max<size_t>(on, 1));
        // 136x medido. El techo lo pone el Huffman FIJO: cada coincidencia de 258 bytes gasta su
        // código de longitud (8 bits) + distancia (5+extra), o sea ~26 bits por 258 bytes. Con tablas
        // dinámicas se pasaría de 1000x, y ese es el precio consciente de no implementarlas.
        CHECK(on < raw / 100, "constante: comprime >100x (las coincidencias largas funcionan)");
    }
}
