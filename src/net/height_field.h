#pragma once
// ================================================================================================
// EL SUELO DEL SERVIDOR — el MISMO campo de altura horneado que pisa el cliente, sin GL.
//
// ⚠️ POR QUE EXISTE ESTE FICHERO. El validador del DGS juzgaba el noclip contra `sampleTerrainV2`,
// que en esta rama es un stub (`terrain sampler removed`, devuelve 0). Medido el 2026-09-01 sobre un
// planeta de radio terrestre, 512 direcciones:
//
//     RELIEVE del analitico:            +0,00 m .. +0,00 m
//     lo que acota el minimo local:     min 0,0000 · medio 0,0000 · MAX 0,0000 m
//
// O sea que el servidor decidia "estas atravesando el suelo" comparando contra una ESFERA LISA a
// nivel del mar, mientras el cliente camina sobre la rejilla horneada del `TerrestrialPlanet`. Con
// relieve de verdad eso expulsa a quien este legitimamente en un valle o en el fondo del mar, y deja
// atravesar una montaña entera hasta la cota 0. El propio CMake lo tenia escrito: *"el arreglo real
// es que el servidor reproduzca el bake de altura, no una clase que devuelve constantes"*.
//
// ⚠️ Y NO SE ARREGLA CON UNA CONSTANTE. Ya se intento dos veces (un margen de 5,0 m, luego de 2,0 m):
// el primero expulsaba a jugadores honestos, el segundo tapaba el agujero lo justo para que el test
// pasara sin medir nada.
//
// ── QUE ES EXACTAMENTE ESTE CAMPO ──────────────────────────────────────────────────────────────
//
// `TerrestrialPlanet::bakeHeightMap` hornea la altura base a una equirect 2:1 (`m_mapRes` x
// `m_mapRes/2`) **en CPU**, con hilos, y la cachea como PNG de 16 bits. `m_heightCPU` es la copia en
// float de ese campo, y `TerrestrialPlanet::sampleHeight` —lo que pisa Jolt y lo que ancla los
// props— es bilineal sobre el + `terrainDetail` + `seaLevelAttenuation`. Las tres son funciones
// PURAS de `terrain_detail.h`, gemelas declaradas de la GPU. Nada de eso necesita un contexto
// grafico: lo unico que ataba el campo al RHI era que se recogia de vuelta de `uploadHeight`.
//
// Asi que el servidor puede tener el suelo de verdad. Este fichero es el campo + su muestreo, y
// `heightAt` es el GEMELO EXACTO de `TerrestrialPlanet::sampleHeight` — misma cuenta, mismo orden,
// mismos literales. Si las dos se separan, `dgs_ground_matches_engine` lo caza.
//
// ── COMO LLEGA AL MODULO ───────────────────────────────────────────────────────────────────────
//
// ⚠️ EL ABI DEL DGS NO TIENE CANAL PARA ESTO, Y EL DGS ES OTRO PROYECTO. `DGS::WorldQuery` lleva
// seed, centro, radio, `reliefStrength` y `profile`, y nada mas — no hay puntero ni ruta. Y el campo
// **no se puede rederivar del seed**: `heightBakeKey` incluye el mapa de elevacion de la escena
// (`surface.elevationMap`), el de zonas y la tabla de materiales, o sea assets. Un planeta sin
// ninguno de los tres si sale del seed; uno con ellos, no.
//
// La salida NO es tocar `external/dgs/`: es que este modulo —que es NUESTRO— exporte simbolos
// ADICIONALES que el host de Haruka llama por `dlsym` antes de crear zonas. Un host del DGS que no
// los conozca sigue funcionando exactamente igual que hoy (cae al analitico, con su aviso). Ver
// `haruka_rules_set_height_field` / `haruka_rules_load_height_field` en `default_rules.cpp`.
// ================================================================================================

#include "core/planet/terrain_detail.h"   // equirectUV · sampleHeightField · terrainDetail (gemelos)
#include "core/planet/terrain_lod.h"      // terrainTriM: el corte de octavas, NO un literal

#include <glm/glm.hpp>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace Haruka { namespace Net {

/**
 * @brief El campo de altura horneado, tal cual, sin GL.
 *
 * `data` son METROS sobre el nivel del mar en una equirect 2:1 con la convencion de `equirectUV`
 * (norte en la fila 0). Es literalmente `TerrestrialPlanet::m_heightCPU`.
 */
struct HeightField
{
    int   w = 0, h = 0;
    float baseRadiusM = 0.0f;      ///< `m_baseRadius`: el radio al que se evalua el detalle
    std::vector<float> data;

    bool valid() const {
        return w > 0 && h > 0 && data.size() == (size_t)w * (size_t)h && baseRadiusM > 0.0f;
    }

    /**
     * @brief Cota del suelo en `dir` (m sobre el nivel del mar).
     *
     * ⚠️ GEMELO EXACTO de `TerrestrialPlanet::sampleHeight(dir, minFeatureM)`. Cualquier cambio alli
     * tiene que venir aqui, y al reves. El test `dgs_ground_matches_engine` compara las dos sobre
     * cientos de direcciones y falla si se separan un milimetro.
     */
    double heightAt(const glm::dvec3& dirIn, float minFeatureM) const {
        if (!valid()) return 0.0;
        const glm::dvec3 dir = glm::normalize(dirIn);
        const glm::vec2  uv  = Haruka::Planet::equirectUV(glm::vec3(dir));
        const float baseH = Haruka::Planet::sampleHeightField(uv, w, h, data.data());
        const float baseR = baseRadiusM + baseH;
        float det = Haruka::Planet::terrainDetail(glm::vec3(dir), baseR, minFeatureM)
                  * Haruka::Planet::seaLevelAttenuation(baseH);
        // Misma paridad con los eval de teselado que en el motor: en TIERRA la funcion no baja del
        // nivel del mar, o el oceano (esfera en R) la taparia.
        if (baseH > 0.0f) det = glm::max(det, -baseH);
        return (double)baseH + (double)det;
    }

    /// Campo cercano, con el MISMO piso que el motor (`terrainTriM(0)`), no un literal.
    double heightAt(const glm::dvec3& dir) const {
        return heightAt(dir, Haruka::Planet::terrainTriM(0.0));
    }
};

// ── Persistencia ────────────────────────────────────────────────────────────────────────────────
//
// Formato CRUDO a proposito, no PNG: el modulo es GL-free y tambien quiere ser codec-free. El bake
// ya deja un PNG de 16 bits para la cache del motor; esto es su hermano en float, que es lo que la
// fisica muestrea de verdad (el PNG cuantiza a 1 m por paso, y el suelo se compara en centimetros).
//
// ⚠️ Se escribe tal cual la memoria: mismo orden de bytes que la maquina. Cliente y servidor de una
// misma partida son la misma clase de maquina; si algun dia dejan de serlo, aqui es donde se ve.

inline constexpr uint32_t HEIGHT_FIELD_MAGIC   = 0x484B4648u;  // "HFKH"
inline constexpr uint32_t HEIGHT_FIELD_VERSION = 1u;

inline bool saveHeightField(const std::string& path, const HeightField& f) {
    if (!f.valid()) return false;
    std::FILE* fp = std::fopen(path.c_str(), "wb");
    if (!fp) return false;
    const uint32_t magic = HEIGHT_FIELD_MAGIC, ver = HEIGHT_FIELD_VERSION;
    const int32_t  ww = f.w, hh = f.h;
    bool ok = std::fwrite(&magic, sizeof(magic), 1, fp) == 1
           && std::fwrite(&ver,   sizeof(ver),   1, fp) == 1
           && std::fwrite(&ww,    sizeof(ww),    1, fp) == 1
           && std::fwrite(&hh,    sizeof(hh),    1, fp) == 1
           && std::fwrite(&f.baseRadiusM, sizeof(float), 1, fp) == 1
           && std::fwrite(f.data.data(), sizeof(float), f.data.size(), fp) == f.data.size();
    std::fclose(fp);
    return ok;
}

inline bool loadHeightField(const std::string& path, HeightField& out) {
    std::FILE* fp = std::fopen(path.c_str(), "rb");
    if (!fp) return false;
    uint32_t magic = 0, ver = 0; int32_t ww = 0, hh = 0; float rad = 0.0f;
    bool ok = std::fread(&magic, sizeof(magic), 1, fp) == 1
           && std::fread(&ver,   sizeof(ver),   1, fp) == 1
           && std::fread(&ww,    sizeof(ww),    1, fp) == 1
           && std::fread(&hh,    sizeof(hh),    1, fp) == 1
           && std::fread(&rad,   sizeof(rad),   1, fp) == 1;
    // Techo de cordura: un campo mayor que 16384x8192 no es un bake, es un fichero corrupto o de
    // otro formato. Sin esto, un `ww` basura pide un `resize` de gigabytes antes de fallar.
    if (ok) ok = (magic == HEIGHT_FIELD_MAGIC) && (ver == HEIGHT_FIELD_VERSION)
              && ww > 0 && hh > 0 && ww <= 16384 && hh <= 8192 && rad > 0.0f;
    if (ok) {
        out.w = ww; out.h = hh; out.baseRadiusM = rad;
        out.data.assign((size_t)ww * (size_t)hh, 0.0f);
        ok = std::fread(out.data.data(), sizeof(float), out.data.size(), fp) == out.data.size();
    }
    std::fclose(fp);
    if (!ok) out = HeightField{};
    return ok;
}

}} // namespace Haruka::Net
