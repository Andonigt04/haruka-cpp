#include "io/image_writer.h"

#include <cstdint>
#include <cstring>
#include <vector>
#include <fstream>
#include <filesystem>
#include <algorithm>

namespace Haruka {
namespace {

uint32_t crc32(const uint8_t* data, size_t len) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        init = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) c = table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

uint32_t adler32(const uint8_t* data, size_t len) {
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < len; ++i) { a = (a + data[i]) % 65521; b = (b + a) % 65521; }
    return (b << 16) | a;
}

void putBE32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back((x >> 24) & 0xFF); v.push_back((x >> 16) & 0xFF);
    v.push_back((x >> 8) & 0xFF);  v.push_back(x & 0xFF);
}

void chunk(std::vector<uint8_t>& out, const char type[4], const std::vector<uint8_t>& data) {
    putBE32(out, (uint32_t)data.size());
    std::vector<uint8_t> tc(type, type + 4);
    tc.insert(tc.end(), data.begin(), data.end());
    out.insert(out.end(), tc.begin(), tc.end());
    putBE32(out, crc32(tc.data(), tc.size()));
}

// =================================================================================================
// DEFLATE de verdad (RFC 1951), Huffman FIJO + LZ77
// =================================================================================================
//
// ⚠️ POR QUÉ ESTO EXISTE, Y POR QUÉ ANTES NO.
//
// Este escritor nació para GUARDAR CAPTURAS DE PANTALLA, y para eso su cabecera decía la verdad:
// «self-contained encoder (uncompressed zlib "stored" blocks) … not size-optimised». Una captura de
// 8 MB en vez de 2 MB no le importa a nadie.
//
// Después se reutilizó para la CACHÉ DE HORNEADO del planeta, y ahí el mismo código pasó a escribir
// mapas de 18750×9375. Medido en el proyecto: un bake de bioma RGBA de **703 MB** y uno de altura de
// 16 bits de **351 MB**, con ratio de compresión exactamente 1,00× — el fichero era PNG por fuera y
// píxeles crudos por dentro. Había 3,7 GB de caché acumulada.
//
// Cuánto se pierde, medido re-comprimiendo esos mismos ficheros con un PNG normal:
//     macro 2048×1024 RGBA :  8,4 MB -> 0,2 MB   (41,8x)
//     height 512×256 16-bit:  0,3 MB -> 0,04 MB  ( 8,4x)
//
// Y no es solo disco: el bake se RELEE en cada arranque, así que son cientos de MB de E/S por
// lanzamiento para datos que caben en decenas.
//
// Se mantiene sin dependencias (ni zlib ni stb_image_write) porque hay una razón real para el
// escritor a mano: **stb_image_write no sabe escribir PNG de 16 bits**, y el bake de altura lo
// necesita — a 8 bits la cuantización de la elevación sería de 78 m. Así que en vez de traer una
// dependencia se implementa el DEFLATE que faltaba.
//
// Se usa Huffman FIJO (BTYPE=01) y no dinámico: las tablas dinámicas darían un 5-15 % más, pero
// obligan a un histograma y a construir códigos canónicos con su recorte de longitudes, que es donde
// vive la mayor parte de los errores de un compresor escrito a mano. El fijo saca casi todo el
// beneficio —el grueso viene de LZ77 sobre filas filtradas, que son casi todo ceros— con una fracción
// del código que puede fallar.

/// Escritor de bits de DEFLATE. Los bits se empaquetan de LSB a MSB dentro de cada byte.
struct BitWriter {
    std::vector<uint8_t> out;
    uint32_t acc = 0;
    int nbits = 0;

    /// Campos "normales" (bits extra de longitud/distancia): el bit menos significativo primero.
    void bits(uint32_t v, int n) {
        acc |= (v & ((1u << n) - 1u)) << nbits;
        nbits += n;
        while (nbits >= 8) { out.push_back((uint8_t)(acc & 0xFF)); acc >>= 8; nbits -= 8; }
    }
    /// ⚠️ Códigos de Huffman: se empaquetan empezando por su bit MÁS significativo. Es la excepción
    /// del formato y la fuente clásica de errores — un compresor con esto al revés produce un fichero
    /// que parece plausible y que ningún decodificador lee.
    void huff(uint32_t code, int len) {
        for (int i = len - 1; i >= 0; --i) bits((code >> i) & 1u, 1);
    }
    void flush() { if (nbits > 0) { out.push_back((uint8_t)(acc & 0xFF)); acc = 0; nbits = 0; } }
};

/// Código de Huffman fijo del alfabeto literal/longitud (RFC 1951 §3.2.6).
inline void fixedLitCode(int sym, uint32_t& code, int& len) {
    if (sym <= 143)      { code = 0x30u  + (uint32_t)sym;         len = 8; }
    else if (sym <= 255) { code = 0x190u + (uint32_t)(sym - 144); len = 9; }
    else if (sym <= 279) { code = 0x00u  + (uint32_t)(sym - 256); len = 7; }
    else                 { code = 0xC0u  + (uint32_t)(sym - 280); len = 8; }
}

const uint16_t kLenBase[29] = { 3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,
                                59,67,83,99,115,131,163,195,227,258 };
const uint8_t  kLenExtra[29] = { 0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0 };
const uint16_t kDistBase[30] = { 1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,
                                 1025,1537,2049,3073,4097,6145,8193,12289,16385,24577 };
const uint8_t  kDistExtra[30] = { 0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13 };

constexpr int kWindow   = 32768;
constexpr int kWMask    = kWindow - 1;
constexpr int kMinMatch = 3;
constexpr int kMaxMatch = 258;
/// Cuántos candidatos de la cadena de hash se prueban. Es el mando de velocidad frente a tamaño; con
/// filas filtradas de imagen (largas tiradas de ceros) el primer candidato ya suele dar una coincidencia
/// muy larga, así que subirlo apenas mejora y sí cuesta: estos ficheros tienen cientos de MB.
constexpr int kChainLimit = 8;

std::vector<uint8_t> deflateFixed(const std::vector<uint8_t>& raw) {
    BitWriter bw;
    bw.out.reserve(raw.size() / 4 + 64);
    bw.bits(1, 1);   // BFINAL = 1 (un solo bloque; Huffman no impone límite de tamaño)
    bw.bits(1, 2);   // BTYPE  = 01 (Huffman fijo)

    const size_t n = raw.size();
    // `head` guarda posiciones ABSOLUTAS por hash; `prev` encadena, indexado por posición dentro de
    // la ventana. Indexar `prev` por `pos & kWMask` en vez de por posición absoluta es lo que hace
    // esto viable con entradas de 700 MB: son 128 KB fijos en vez de 4 bytes por byte de entrada
    // (2,8 GB). Las entradas que se sobreescriben son justo las que ya quedaron fuera de la ventana.
    std::vector<int32_t> head(65536, -1);
    std::vector<int32_t> prev((size_t)kWindow, -1);

    auto hash3 = [&](size_t p) -> uint32_t {
        return (uint32_t)(((uint32_t)raw[p] << 10) ^ ((uint32_t)raw[p + 1] << 5) ^ (uint32_t)raw[p + 2])
               & 0xFFFFu;
    };

    size_t pos = 0;
    while (pos < n) {
        int bestLen = 0;
        size_t bestDist = 0;
        if (pos + kMinMatch <= n) {
            const uint32_t h = hash3(pos);
            int32_t cand = head[h];
            const size_t maxLen = std::min<size_t>(kMaxMatch, n - pos);
            for (int step = 0; step < kChainLimit && cand >= 0; ++step) {
                const size_t dist = pos - (size_t)cand;
                if (dist == 0 || dist > (size_t)kWindow) break;
                // Comparar primero el byte que decidiría mejorar la marca: descarta candidatos sin
                // recorrer la coincidencia entera.
                if (bestLen >= kMinMatch && raw[(size_t)cand + (size_t)bestLen] != raw[pos + (size_t)bestLen]) {
                    cand = prev[(size_t)cand & kWMask];
                    continue;
                }
                size_t l = 0;
                while (l < maxLen && raw[(size_t)cand + l] == raw[pos + l]) ++l;
                if ((int)l > bestLen) { bestLen = (int)l; bestDist = dist; if (l == maxLen) break; }
                cand = prev[(size_t)cand & kWMask];
            }
            // Insertar la posición actual en la cadena (después de buscar: una coincidencia consigo
            // misma tendría distancia 0, que el formato no admite).
            prev[pos & kWMask] = head[h];
            head[h] = (int32_t)pos;
        }

        if (bestLen >= kMinMatch) {
            // Símbolo de longitud: el último tramo cuya base no pase de `bestLen`.
            int li = 0;
            while (li < 28 && kLenBase[li + 1] <= bestLen) ++li;
            uint32_t code; int len;
            fixedLitCode(257 + li, code, len);
            bw.huff(code, len);
            if (kLenExtra[li]) bw.bits((uint32_t)(bestLen - kLenBase[li]), kLenExtra[li]);
            int di = 0;
            while (di < 29 && kDistBase[di + 1] <= (int)bestDist) ++di;
            bw.huff((uint32_t)di, 5);                        // distancias: 5 bits fijos
            if (kDistExtra[di]) bw.bits((uint32_t)((int)bestDist - kDistBase[di]), kDistExtra[di]);
            // Las posiciones que la coincidencia se salta también se indexan: si no, se pierden
            // arranques de coincidencia y el ratio se cae en imágenes con estructura.
            for (size_t k = pos + 1; k < pos + (size_t)bestLen && k + kMinMatch <= n; ++k) {
                const uint32_t hk = hash3(k);
                prev[k & kWMask] = head[hk];
                head[hk] = (int32_t)k;
            }
            pos += (size_t)bestLen;
        } else {
            uint32_t code; int len;
            fixedLitCode(raw[pos], code, len);
            bw.huff(code, len);
            ++pos;
        }
    }
    uint32_t code; int len;
    fixedLitCode(256, code, len);                            // fin de bloque
    bw.huff(code, len);
    bw.flush();

    std::vector<uint8_t> z;
    z.reserve(bw.out.size() + 6);
    z.push_back(0x78); z.push_back(0x01);                    // cabecera zlib (CM=8, CINFO=7)
    z.insert(z.end(), bw.out.begin(), bw.out.end());
    putBE32(z, adler32(raw.data(), raw.size()));             // Adler32 de los datos SIN comprimir
    return z;
}

// =================================================================================================
// Filtros de fila de PNG
// =================================================================================================
//
// El otro 90 % del ahorro, y el que el escritor no hacía: escribía el byte de filtro 0 (None) en cada
// fila, o sea los píxeles tal cual. Los filtros restan al píxel una predicción a partir de sus vecinos
// (izquierda, arriba, media, Paeth), y en una imagen suave el residuo es casi todo ceros — que es
// justo lo que LZ77 convierte en nada. Sin filtro, LZ77 tiene que encontrar repeticiones de valores
// absolutos, y en un degradado no hay ninguna.
//
// El filtro se elige POR FILA probando los cinco y quedándose con el de menor suma de residuos
// absolutos (con signo). Es la heurística estándar de libpng: no es óptima, pero cuesta O(n) y acierta
// casi siempre.

inline uint8_t paethPredictor(int a, int b, int c) {
    const int p = a + b - c;
    const int pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
    if (pa <= pb && pa <= pc) return (uint8_t)a;
    return (pb <= pc) ? (uint8_t)b : (uint8_t)c;
}

/// Suma de |residuo con signo|: el coste que la heurística minimiza.
inline uint32_t absCost(const std::vector<uint8_t>& v) {
    uint32_t s = 0;
    for (uint8_t b : v) s += (b < 128) ? b : (uint32_t)(256 - b);
    return s;
}

/**
 * @brief Filtra la imagen a las líneas de barrido que van dentro del IDAT.
 * @param bpp bytes por píxel (3 RGB, 4 RGBA, 2 gris de 16 bits). Es la distancia al vecino izquierdo:
 *            con un valor mal puesto el fichero sigue siendo válido pero comprime mucho peor.
 */
std::vector<uint8_t> filterScanlines(const uint8_t* px, size_t rowBytes, int h, size_t bpp) {
    std::vector<uint8_t> raw;
    raw.reserve((size_t)h * (rowBytes + 1));
    std::vector<uint8_t> cand[5];
    for (int k = 0; k < 5; ++k) cand[k].resize(rowBytes);
    std::vector<uint8_t> prevRow(rowBytes, 0);

    for (int y = 0; y < h; ++y) {
        const uint8_t* cur = px + (size_t)y * rowBytes;
        for (size_t i = 0; i < rowBytes; ++i) {
            const int a = (i >= bpp) ? cur[i - bpp] : 0;             // izquierda
            const int b = prevRow[i];                                // arriba
            const int c = (i >= bpp) ? prevRow[i - bpp] : 0;          // arriba-izquierda
            const int x = cur[i];
            cand[0][i] = (uint8_t)x;
            cand[1][i] = (uint8_t)((x - a) & 0xFF);
            cand[2][i] = (uint8_t)((x - b) & 0xFF);
            cand[3][i] = (uint8_t)((x - ((a + b) >> 1)) & 0xFF);
            cand[4][i] = (uint8_t)((x - paethPredictor(a, b, c)) & 0xFF);
        }
        int best = 0;
        uint32_t bestCost = absCost(cand[0]);
        for (int k = 1; k < 5; ++k) {
            const uint32_t cost = absCost(cand[k]);
            if (cost < bestCost) { bestCost = cost; best = k; }
        }
        raw.push_back((uint8_t)best);
        raw.insert(raw.end(), cand[best].begin(), cand[best].end());
        std::memcpy(prevRow.data(), cur, rowBytes);
    }
    return raw;
}

/// Flujo zlib con bloques DEFLATE "stored" (sin comprimir). Era lo ÚNICO que hacía este escritor;
/// ahora es el respaldo para datos incompresibles.
std::vector<uint8_t> zlibStored(const std::vector<uint8_t>& raw) {
    std::vector<uint8_t> z;
    z.reserve(raw.size() + raw.size() / 65535 * 5 + 16);
    z.push_back(0x78); z.push_back(0x01);
    size_t pos = 0;
    do {
        const size_t block = std::min<size_t>(65535, raw.size() - pos);
        const bool last = (pos + block >= raw.size());
        z.push_back(last ? 1 : 0);                     // BFINAL, BTYPE=00 (stored)
        const uint16_t len = (uint16_t)block, nlen = (uint16_t)~len;
        z.push_back(len & 0xFF); z.push_back((len >> 8) & 0xFF);
        z.push_back(nlen & 0xFF); z.push_back((nlen >> 8) & 0xFF);
        z.insert(z.end(), raw.begin() + (long)pos, raw.begin() + (long)(pos + block));
        pos += block;
    } while (pos < raw.size());                        // `do/while`: una imagen vacía necesita 1 bloque
    putBE32(z, adler32(raw.data(), raw.size()));
    return z;
}

/// Comprime y, si el resultado no mejora al dato crudo, lo guarda sin comprimir.
///
/// El Huffman FIJO hace CRECER un ~6 % los datos incompresibles (un literal de 144-255 ocupa 9 bits
/// en vez de 8: medido, ruido blanco pasaba de 162 708 a 171 804 B). Con este respaldo el peor caso
/// vuelve a ser ~1,00x, así que el cambio no puede empeorar NINGÚN fichero respecto al escritor
/// anterior — que era exactamente esta rama, siempre.
std::vector<uint8_t> zlibBest(const std::vector<uint8_t>& raw) {
    std::vector<uint8_t> comp = deflateFixed(raw);
    if (comp.size() >= raw.size()) {
        std::vector<uint8_t> stored = zlibStored(raw);
        if (stored.size() < comp.size()) return stored;
    }
    return comp;
}

} // namespace

// Serializa IHDR+IDAT+IEND a disco (lo comparten writePNG y writePNG16).
static bool writePNGChunks(const std::string& path, const std::vector<uint8_t>& ihdr,
                           const std::vector<uint8_t>& raw) {
    std::vector<uint8_t> out = {0x89,'P','N','G',0x0D,0x0A,0x1A,0x0A};
    chunk(out, "IHDR", ihdr);
    chunk(out, "IDAT", zlibBest(raw));
    chunk(out, "IEND", {});

    std::error_code ec;
    std::filesystem::path p(path);
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(out.data()), (std::streamsize)out.size());
    return (bool)f;
}

bool writePNG(const std::string& path, int w, int h, int channels, const unsigned char* pixels) {
    if (w <= 0 || h <= 0 || (channels != 3 && channels != 4) || !pixels) return false;
    const size_t rowBytes = (size_t)w * (size_t)channels;
    const std::vector<uint8_t> raw = filterScanlines(pixels, rowBytes, h, (size_t)channels);

    std::vector<uint8_t> ihdr;
    putBE32(ihdr, (uint32_t)w); putBE32(ihdr, (uint32_t)h);
    ihdr.push_back(8);                                 // bit depth
    ihdr.push_back(channels == 4 ? 6 : 2);             // color type: 6=RGBA, 2=RGB
    ihdr.push_back(0); ihdr.push_back(0); ihdr.push_back(0); // compression/filter/interlace

    return writePNGChunks(path, ihdr, raw);
}

bool writePNG16(const std::string& path, int w, int h, const unsigned short* gray) {
    if (w <= 0 || h <= 0 || !gray) return false;

    // Grayscale 16-bit: las muestras van BIG-ENDIAN en el flujo PNG (spec), así que primero se pasa a
    // ese orden y luego se filtra — el filtro trabaja sobre BYTES del flujo, no sobre las muestras.
    const size_t rowBytes = (size_t)w * 2;
    std::vector<uint8_t> be((size_t)h * rowBytes);
    for (int y = 0; y < h; ++y) {
        const unsigned short* row = gray + (size_t)y * w;
        uint8_t* dst = be.data() + (size_t)y * rowBytes;
        for (int x = 0; x < w; ++x) {
            dst[2 * x + 0] = (uint8_t)(row[x] >> 8);
            dst[2 * x + 1] = (uint8_t)(row[x] & 0xFF);
        }
    }
    // bpp = 2: el vecino "izquierdo" de un byte es el byte de la misma mitad de la muestra anterior.
    // Con bpp=1 el fichero seguiría siendo válido pero el filtro Sub restaría el byte bajo del alto y
    // el residuo sería basura — un error que no rompe nada y solo se ve en el tamaño.
    const std::vector<uint8_t> raw = filterScanlines(be.data(), rowBytes, h, 2);

    std::vector<uint8_t> ihdr;
    putBE32(ihdr, (uint32_t)w); putBE32(ihdr, (uint32_t)h);
    ihdr.push_back(16);                                // bit depth
    ihdr.push_back(0);                                 // color type: 0=grayscale
    ihdr.push_back(0); ihdr.push_back(0); ihdr.push_back(0); // compression/filter/interlace

    return writePNGChunks(path, ihdr, raw);
}

} // namespace Haruka
