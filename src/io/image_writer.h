/**
 * @file image_writer.h
 * @brief Escritor de PNG sin dependencias, 8 bits y 16 bits, CON compresión real.
 *
 * Codificador propio (filtros de fila + DEFLATE con Huffman fijo y LZ77 + CRC32/Adler32) para que el
 * motor guarde PNG estándar sin arrastrar stb_image_write ni zlib. La razón de no usar
 * `stbi_write_png` no es la dependencia: es que **no sabe escribir 16 bits**, y el horneado de altura
 * del planeta los necesita (a 8 bits la elevación se cuantizaría a 78 m).
 *
 * ⚠️ Esta cabecera decía «uncompressed zlib "stored" blocks … not size-optimised», y era cierto: el
 * escritor nació para CAPTURAS DE PANTALLA, donde el tamaño da igual. Luego se reutilizó para la
 * caché de horneado del planeta y la misma frase pasó a costar gigabytes — un mapa de bioma de
 * 18750×9375 ocupaba 703 MB con ratio 1,00×, y la caché acumuló 3,7 GB. Medido tras añadir la
 * compresión, sobre los bakes reales del proyecto:
 *
 *     bioma  18750×9375 RGBA :  703,2 MB -> 5,71 MB   (123x)
 *     altura 18750×9375 16bit:  351,6 MB -> 60,13 MB  (5,8x)
 *     macro   2048×1024 RGBA :    8,4 MB -> 0,30 MB   (28x)
 *
 * Los datos INCOMPRESIBLES (ruido) no crecen: si el Huffman fijo empeorara el tamaño, se guarda un
 * bloque "stored" — o sea el comportamiento anterior, que así queda como el peor caso y no como el
 * único. `tests/test_image_writer.cpp` verifica la ida y vuelta contra el inflate de stb_image, que
 * es un decodificador ajeno: es la única forma de detectar un error de empaquetado de bits.
 */
#ifndef HARUKA_IMAGE_WRITER_H
#define HARUKA_IMAGE_WRITER_H

#include <string>

namespace Haruka {

/**
 * @brief Writes an 8-bit PNG. `channels` = 3 (RGB) or 4 (RGBA).
 * @param pixels row-major, **top-to-bottom**, `w*h*channels` bytes.
 * @return false on bad args or file-open failure.
 */
bool writePNG(const std::string& path, int w, int h, int channels, const unsigned char* pixels);

/**
 * @brief Writes a 16-bit GRAYSCALE PNG (color type 0, bit depth 16, big-endian).
 * @param gray row-major, **top-to-bottom**, `w*h` samples in HOST byte order.
 * @return false on bad args or file-open failure.
 */
bool writePNG16(const std::string& path, int w, int h, const unsigned short* gray);

} // namespace Haruka

#endif // HARUKA_IMAGE_WRITER_H
