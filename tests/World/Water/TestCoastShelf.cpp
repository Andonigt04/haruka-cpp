// ================================================================================================
// LAS COSTAS DEL MUNDO TIENEN DONDE ROMPER (2026-10-17).
//
// Andoni: "el agua no tiene olas con rollo como esperaba". El mar del juego SI rompe (3 m de ola a
// 3,5 m/s de viento, rompe desde 6,45 m de fondo: `coast_surf_breaks`, `coast_lip_barrel`), pero esos
// tests suponen una playa IDEAL de pendiente 1:20. Este mira las costas DE VERDAD: carga el campo de
// altura que hornea el juego (`<bakes>/<key>_height.hfield`, el mismo `m_heightCPU`), toma cientos de
// puntos de la linea de costa y camina mar adentro sobre el suelo que se DIBUJA (relieve base + el
// detalle procedural, atenuado bajo el agua como en `updateLakeWindow`). Mide cuantos metros hay desde
// la orilla hasta que el fondo pasa la profundidad de rotura: esa es la franja donde la ola se
// levanta y se enrosca. Sin franja, la ola llega a la orilla sin haber tocado fondo.
//
// En el juego (31.08, 126.05) `donde` dio el terreno a -95 m EN la linea de costa del mapa: la costa
// es un escalon. Este test lo convierte en un numero por costa del mundo entero.
//
// Necesita el horneado: HARUKA_PROBE_HFIELD=<ruta>, o el mas reciente de `Survival/build/bin/bakes`
// junto a este repositorio. Sin el, se salta (como `water_real_bake_probe`).
// ================================================================================================
#include "Common/TestCommon.h"
#include "Haruka/World/Water/OceanWave.h"
#include "Haruka/World/Terrain/TerrainDetail.h"
#include "Haruka/World/Terrain/TerrainLOD.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <glm/glm.hpp>

namespace {

namespace W = Haruka::World::Water;
namespace T = Haruka::World::Terrain;
namespace fs = std::filesystem;

// El horneado: el pedido por entorno, o el mas reciente del juego junto a este repositorio.
std::string findHeightBake() {
    if (const char* p = std::getenv("HARUKA_PROBE_HFIELD")) return p;
    std::error_code ec;
    const fs::path repo = fs::path(__FILE__).parent_path().parent_path().parent_path().parent_path();
    const fs::path dir = repo.parent_path() / "Survival" / "build" / "bin" / "bakes";
    std::string best; fs::file_time_type bestT{};
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        const std::string n = e.path().filename().string();
        if (n.size() < 14 || n.compare(n.size() - 14, 14, "_height.hfield") != 0) continue;
        const auto t = e.last_write_time(ec);
        if (best.empty() || t > bestT) { best = e.path().string(); bestT = t; }
    }
    return best;
}

}  // namespace

void test_coast_shelf_real_world() {
    beginTest("coast_shelf_real_world");
    const std::string path = findHeightBake();
    if (path.empty()) { std::printf("    (saltado: sin horneado; HARUKA_PROBE_HFIELD=<ruta>)\n"); return; }
    std::ifstream f(path, std::ios::binary);
    char magic[4]; int32_t ver = 0, hw = 0, hh = 0; float baseR = 0.0f;
    f.read(magic, 4); f.read((char*)&ver, 4); f.read((char*)&hw, 4); f.read((char*)&hh, 4); f.read((char*)&baseR, 4);
    std::vector<float> height((size_t)std::max(hw, 0) * std::max(hh, 0));
    f.read((char*)height.data(), (std::streamsize)(height.size() * sizeof(float)));
    CHECK(f.good() && hw > 0 && hh > 0, "el campo de altura del juego carga");
    if (!f.good() || hw <= 0 || hh <= 0) return;
    std::printf("    horneado %s (%dx%d, %.1f km por texel en el ecuador)\n",
                fs::path(path).filename().string().c_str(), hw, hh, 2.0 * M_PI * baseR / hw / 1000.0);

    // El mar del juego en calma (el log: 3,5 m/s) y su profundidad de rotura.
    const W::OceanState sea = W::oceanAddSwell(W::oceanStateFromWind(3.5f, 1.0f, 0.0f, 0.0f));
    const float db = W::oceanBreakDepth(sea);
    std::printf("    mar del juego a 3,5 m/s: rompe desde %.2f m de fondo\n", db);
    CHECK(db > 0.5f, "el mar del juego rompe en algun fondo");
    if (db <= 0.5f) return;

    // EL SUELO QUE SE DIBUJA, igual que `TerrestrialPlanet::updateLakeWindow` (y `water_real_bake_probe`).
    const float triM = T::terrainTriM(0.0);
    auto texel = [&](int x, int y) { x = ((x % hw) + hw) % hw; y = std::clamp(y, 0, hh - 1); return height[(size_t)y * hw + x]; };
    auto dirOfTexel = [&](int x, int y) {
        const double lon = ((x + 0.5) / hw - 0.5) * 2.0 * M_PI, lat = (0.5 - (y + 0.5) / hh) * M_PI;
        return glm::dvec3(std::cos(lat) * std::cos(lon), std::sin(lat), std::cos(lat) * std::sin(lon));
    };
    auto ground = [&](const glm::dvec3& d) -> float {
        const float baseH = T::sampleHeightField(T::equirectUV(glm::vec3(d)), hw, hh, height.data());
        float det = T::terrainDetail(glm::vec3(d), baseR + baseH, triM) * T::seaLevelAttenuation(baseH);
        if (baseH > 0.0f) det = glm::max(det, -baseH);
        return baseH + det;
    };

    // PUNTOS DE COSTA: texel de tierra con un vecino de mar. Latitudes de -60 a 60 (donde se juega) y
    // repartidos por todo el mapa, no los primeros que salgan.
    struct Coast { int x, y, dx, dy; };
    std::vector<Coast> coast;
    const int y0 = hh / 6, y1 = hh - hh / 6;
    for (int y = y0; y < y1; y += 3)
        for (int x = 0; x < hw; x += 3) {
            if (texel(x, y) <= 0.0f) continue;
            const int nd[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
            for (const auto& n : nd)
                if (texel(x + n[0], y + n[1]) < 0.0f) { coast.push_back({ x, y, n[0], n[1] }); break; }
        }
    CHECK(coast.size() > 100, "hay costa en el mapa");
    if (coast.size() <= 100) return;
    const size_t want = 400, step = std::max<size_t>(1, coast.size() / want);

    // Camina de la tierra al mar (y otro texel mas alla) cada 2 m: orilla = primer suelo bajo 0,
    // franja = de ahi hasta que el suelo pasa -db. Sin pasarla en el camino, la franja es lo andado.
    std::vector<double> widths, cliffs;
    for (size_t i = 0; i < coast.size(); i += step) {
        const Coast& c = coast[i];
        const glm::dvec3 a = dirOfTexel(c.x, c.y), b = dirOfTexel(c.x + c.dx, c.y + c.dy);
        const double spanM = std::acos(std::clamp(glm::dot(a, b), -1.0, 1.0)) * baseR;
        const int n = (int)(2.0 * spanM / 2.0);
        double shore = -1.0, deep = -1.0;
        for (int k = 0; k <= n; ++k) {
            const double t = 2.0 * k / n;   // 0 = tierra, 1 = texel de mar, 2 = uno mas alla
            const float g = ground(glm::normalize(a + (b - a) * t));
            const double s = t * spanM;
            if (shore < 0.0 && g < 0.0f) shore = s;
            if (shore >= 0.0 && g < -db) { deep = s; break; }
        }
        if (shore < 0.0) continue;   // el detalle tapo el mar en este tramo: no es costa dibujada
        const double w = (deep >= 0.0 ? deep : 2.0 * spanM) - shore;
        widths.push_back(w);
        if (w < 2.0 * db) cliffs.push_back(w);   // mas empinado que 1:2: un escalon, no una playa
    }
    CHECK(widths.size() > 50, "se midieron costas suficientes");
    if (widths.size() <= 50) return;
    std::sort(widths.begin(), widths.end());
    auto pct = [&](double p) { return widths[(size_t)std::min<double>(widths.size() - 1, p * widths.size())]; };
    auto fracAtLeast = [&](double m) {
        return (double)(widths.end() - std::lower_bound(widths.begin(), widths.end(), m)) / widths.size();
    };
    const double med = pct(0.5);
    std::printf("    %zu costas: franja de rompiente (fondo < %.2f m) p10 %.0f m · mediana %.0f m · p90 %.0f m\n",
                widths.size(), db, pct(0.1), med, pct(0.9));
    std::printf("    pendiente equivalente en la mediana 1:%.1f · escalones (mas empinado que 1:2) %.0f %%\n",
                med / db, 100.0 * cliffs.size() / widths.size());
    std::printf("    playas 1:10 o mas tendidas %.0f %% · 1:20 o mas tendidas %.0f %%\n",
                100.0 * fracAtLeast(10.0 * db), 100.0 * fracAtLeast(20.0 * db));

    // LO QUE SE EXIGE: que la costa tipica sea una PLAYA donde la ola tiene sitio para levantarse.
    // 1:10 es una playa empinada de verdad (gravas); la mitad de las costas tiene que ser al menos eso,
    // y como mucho una de cada cinco un escalon. Hoy falla: ver la cabecera.
    CHECK(med >= 10.0 * db, "la mitad de las costas tiene playa de 1:10 o mas tendida (sitio para romper)");
    CHECK((double)cliffs.size() / widths.size() <= 0.2, "como mucho una costa de cada cinco es un escalon");
}

HARUKA_REGISTER_TESTS({ "coast_shelf_real_world", test_coast_shelf_real_world });
