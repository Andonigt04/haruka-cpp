// ================================================================================================
// PROBE ON THE REAL WORLD'S BAKE: where does the water stand at two levels side by side?
//
// Andoni (12-10): "desniveles notorios y el agua no se nivela". The drawn lake level is
// `max(global fill, fine window)`, carried two texels past a rising shore (`lakeWindowGpuRG`). This
// loads the game's own height field (`<bakes>/<key>_height.hfield`, the exact `m_heightCPU`), runs the
// same two fills the planet runs, at the same place, and counts the steps between neighbouring water
// texels — and which source each side's level comes from.
//
// Opt-in, it needs the file: HARUKA_PROBE_HFIELD=<path> [HARUKA_PROBE_LATLON=lat,lon in degrees].
// ================================================================================================
#include "Common/TestCommon.h"
#include "Haruka/World/Water/WaterFill.h"
#include "Haruka/World/Terrain/TerrainDetail.h"
#include "Haruka/World/Terrain/TerrainLOD.h"
#include "Haruka/World/Terrain/TerrainSample.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <vector>
#include <glm/glm.hpp>

void test_water_real_bake_probe() {
    beginTest("water_real_bake_probe");
    namespace W = Haruka::World::Water;
    namespace T = Haruka::World::Terrain;
    const char* path = std::getenv("HARUKA_PROBE_HFIELD");
    if (!path) { std::printf("    (skipped: HARUKA_PROBE_HFIELD not set)\n"); return; }
    std::ifstream f(path, std::ios::binary);
    char magic[4]; int32_t ver = 0, hw = 0, hh = 0; float baseR = 0.0f;
    f.read(magic, 4); f.read((char*)&ver, 4); f.read((char*)&hw, 4); f.read((char*)&hh, 4); f.read((char*)&baseR, 4);
    std::vector<float> height((size_t)hw * hh);
    f.read((char*)height.data(), (std::streamsize)(height.size() * sizeof(float)));
    CHECK(f.good() && hw > 0 && hh > 0, "the height field loads");
    if (!f.good()) return;
    const double R = 6371000.0;

    double latD = 30.40378, lonD = 130.06993;
    if (const char* ll = std::getenv("HARUKA_PROBE_LATLON")) std::sscanf(ll, "%lf,%lf", &latD, &lonD);
    const double lat = glm::radians(latD), lon = glm::radians(lonD);

    const W::WaterFillResult coarse = W::waterFillEquirect(height.data(), hw, hh, 0.0f, R);
    auto dirOf = [](double lo, double la) { return glm::dvec3(std::cos(la) * std::cos(lo), std::sin(la), std::cos(la) * std::sin(lo)); };
    auto coarseAt = [&](double lo, double la) -> float {
        const glm::vec2 uv = T::equirectUV(glm::vec3(dirOf(lo, la)));
        int x = (int)std::floor((double)uv.x * hw), y = (int)std::floor((double)uv.y * hh);
        x = ((x % hw) + hw) % hw; y = std::clamp(y, 0, hh - 1);
        return coarse.levelM[(size_t)y * hw + x];
    };
    const float triM = T::terrainTriM(0.0);
    auto ground = [&](double lo, double la) -> float {   // `TerrestrialPlanet::updateLakeWindow`'s `terreno`
        const glm::dvec3 d = dirOf(lo, la);
        const glm::vec2 uv = T::equirectUV(glm::vec3(d));
        const float baseH = T::sampleHeightField(uv, hw, hh, height.data());
        float det = T::terrainDetail(glm::vec3(d), baseR + baseH, triM) * T::seaLevelAttenuation(baseH);
        if (baseH > 0.0f) det = glm::max(det, -baseH);
        return baseH + det;
    };
    const double halfM = 8000.0, halfLat = halfM / R, halfLon = halfLat / std::max(std::cos(lat), 1e-6);
    const int N = 256;
    // HARUKA_PROBE_SUB=k: fill with the MINIMUM of k x k fine samples in each texel (the planet: 4).
    const int sub = std::getenv("HARUKA_PROBE_SUB") ? std::atoi(std::getenv("HARUKA_PROBE_SUB")) : 4;
    const double dLonT = 2.0 * halfLon / N, dLatT = 2.0 * halfLat / N;
    auto groundFill = [&](double lo, double la) -> float {
        if (sub <= 1) return ground(lo, la);
        float m = 1e30f;
        for (int a = 0; a < sub; ++a)
            for (int b = 0; b < sub; ++b)
                m = std::min(m, ground(lo + ((a + 0.5) / sub - 0.5) * dLonT, la + ((b + 0.5) / sub - 0.5) * dLatT));
        return m;
    };
    const auto tF0 = std::chrono::steady_clock::now();
    const W::WaterWindowResult win = W::waterFillWindow(N, N, lon, lat, halfLon, halfLat, R, groundFill, coarseAt, 0.0f);
    std::printf("    window fill with %dx%d samples a texel: %.0f ms\n", sub, sub,
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tF0).count());
    std::vector<float> rg;
    W::lakeWindowGpuRG(win.levelM, win.fetchM, N, N, W::LAKE_WINDOW_SHORE_TEXELS, rg, &win.heightM);

    // The level each texel DRAWS (`harukaWaterLevelAt` without the dynamic patch): max(sea, coarse, window).
    const double dLon = 2.0 * halfLon / N, dLat = 2.0 * halfLat / N;
    std::vector<float> drawn((size_t)N * N), cLv((size_t)N * N);
    std::vector<char> src((size_t)N * N, 0);   // 0 dry, 1 window lake, 2 window shore band, 3 coarse wins, 4 sea
    size_t nWin = 0, nBand = 0, nCoarse = 0, nCoarseOverWin = 0;
    for (int y = 0; y < N; ++y)
        for (int x = 0; x < N; ++x) {
            const size_t i = (size_t)y * N + x;
            const double lo = lon - halfLon + (x + 0.5) * dLon, la = lat - halfLat + (y + 0.5) * dLat;
            const float c = coarseAt(lo, la), wv = rg[i * 2];
            cLv[i] = c;
            const float lv = std::max({ 0.0f, c, wv });
            const float g = win.heightM[i];
            drawn[i] = (lv > g) ? lv : W::WATER_FILL_DRY;   // wet where the ground (texel centre) is under it
            if (drawn[i] <= W::WATER_FILL_DRY) continue;
            if (lv == 0.0f && c < 0.0f && wv < 0.0f) { src[i] = 4; continue; }
            if (c > wv) { src[i] = 3; ++nCoarse; if (win.levelM[i] > W::WATER_FILL_DRY) ++nCoarseOverWin; continue; }
            if (win.levelM[i] > W::WATER_FILL_DRY) { src[i] = 1; ++nWin; } else { src[i] = 2; ++nBand; }
        }
    // Steps: neighbouring wet texels at different levels, by the pair of sources.
    std::map<std::pair<int, int>, std::pair<size_t, double>> steps;
    for (int y = 0; y < N; ++y)
        for (int x = 0; x < N; ++x) {
            const size_t i = (size_t)y * N + x;
            if (!src[i]) continue;
            for (int k = 0; k < 2; ++k) {
                const int nx = x + (k == 0), ny = y + (k == 1);
                if (nx >= N || ny >= N) continue;
                const size_t j = (size_t)ny * N + nx;
                if (!src[j]) continue;
                const double dL = std::fabs((double)drawn[i] - drawn[j]);
                if (dL < 0.25) continue;
                auto key = std::make_pair(std::min(src[i], src[j]), std::max(src[i], src[j]));
                steps[key].first++;
                steps[key].second = std::max(steps[key].second, dL);
            }
        }
    const char* names[] = { "dry", "window lake", "window shore band", "COARSE fill", "sea" };
    std::printf("    real bake %dx%d (%.1f km a texel) · window at lat %.5f lon %.5f, +-8 km, 62.5 m texels\n",
                hw, hh, 2.0 * 3.14159265 * R / hw / 1000.0, latD, lonD);
    std::printf("    wet texels: window lake %zu · shore band %zu · COARSE level wins %zu (%zu of them over a window lake)\n",
                nWin, nBand, nCoarse, nCoarseOverWin);
    std::printf("    window lakes: %zu bodies, fill took %zu lake texels\n", win.bodies, win.lakeCells);
    if (steps.empty()) std::printf("    no steps over 0.25 m between neighbouring wet texels\n");
    for (const auto& [k, v] : steps)
        std::printf("    STEP %s | %s: %zu neighbour pairs, worst %.2f m\n", names[k.first], names[k.second], v.first, v.second);
    // ARTIFICIAL SHORE: an edge between a texel that draws a lake (lake or band) and one that does not,
    // where the FINE ground along the edge is still under the level — there the water stops on the
    // texel's straight edge instead of where the terrain rises. Sampled at 8 points along each edge.
    {
        size_t edges = 0, artificial = 0;
        for (int y = 0; y < N; ++y)
            for (int x = 0; x < N; ++x) {
                const size_t i = (size_t)y * N + x;
                const float L = rg[i * 2];
                if (L <= W::WATER_FILL_DRY) continue;
                const int nx4[4] = { x - 1, x + 1, x, x }, ny4[4] = { y, y, y - 1, y + 1 };
                for (int k = 0; k < 4; ++k) {
                    if (nx4[k] < 0 || ny4[k] < 0 || nx4[k] >= N || ny4[k] >= N) continue;
                    const size_t j = (size_t)ny4[k] * N + nx4[k];
                    if (rg[j * 2] > W::WATER_FILL_DRY) continue;   // both draw water: not a shore edge
                    ++edges;
                    int under = 0;
                    for (int t = 0; t < 8; ++t) {
                        const double a = (t + 0.5) / 8.0;
                        double ex = x + 0.5, ey = y + 0.5;
                        if (k < 2) { ex = (k == 0) ? x : x + 1; ey = y + a; } else { ey = (k == 2) ? y : y + 1; ex = x + a; }
                        const double lo = lon - halfLon + ex * dLon, la = lat - halfLat + ey * dLat;
                        if (ground(lo, la) < L - 0.05f) ++under;
                    }
                    if (under >= 2) ++artificial;
                }
            }
        std::printf("    %s: shore edges between a texel that draws water and one that does not: %zu · with the fine ground"
                    " still UNDER the level along it (a straight cut): %zu (%.0f %%)\n", "drawn",
                    edges, artificial, edges ? 100.0 * artificial / edges : 0.0);
    }
    // Two DIFFERENT lakes with only a one-texel rim between them (62 m): two flat surfaces side by side
    // at different heights, which is what reads as a step. Histogram of the level difference.
    size_t rims = 0, rim1 = 0, rim3 = 0; double rimWorst = 0.0;
    for (int y = 1; y < N - 1; ++y)
        for (int x = 1; x < N - 1; ++x) {
            const size_t i = (size_t)y * N + x;
            if (win.levelM[i] > W::WATER_FILL_DRY) continue;
            float lo = 1e30f, hi = -1e30f;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    const float l = win.levelM[(size_t)(y + dy) * N + (x + dx)];
                    if (l <= W::WATER_FILL_DRY) continue;
                    lo = std::min(lo, l); hi = std::max(hi, l);
                }
            if (hi - lo < 0.25f) continue;
            ++rims; if (hi - lo > 1.0f) ++rim1; if (hi - lo > 3.0f) ++rim3;
            rimWorst = std::max(rimWorst, (double)(hi - lo));
        }
    std::printf("    one-texel rims between two lakes at different levels: %zu (%zu over 1 m, %zu over 3 m, worst %.2f m)\n",
                rims, rim1, rim3, rimWorst);
    // Lake sizes and depths (texel centres).
    std::map<float, std::pair<size_t, float>> bodies;   // level -> (texels, deepest)
    for (size_t i = 0; i < (size_t)N * N; ++i)
        if (win.levelM[i] > W::WATER_FILL_DRY) {
            auto& b = bodies[win.levelM[i]];
            b.first++; b.second = std::max(b.second, win.levelM[i] - win.heightM[i]);
        }
    size_t tiny = 0, shallow = 0, big = 0; size_t texTiny = 0;
    for (const auto& [l, b] : bodies) {
        if (b.first <= 2) { ++tiny; texTiny += b.first; }
        if (b.second < 1.0f) ++shallow;
        if (b.first >= 50) ++big;
    }
    std::printf("    distinct levels %zu: %zu of 1-2 texels (%zu texels), %zu never deeper than 1 m, %zu of 50+ texels\n",
                bodies.size(), tiny, texTiny, shallow, big);
    // PRUNING: keep a lake (bodies keyed by level) only with >= A texels AND deeper than D somewhere.
    // What is left: lakes, wet texels, rims between two KEPT lakes over 1 m apart.
    for (int A : { 1, 4, 8, 16 })
        for (float D : { 0.25f, 1.0f, 2.0f }) {
            auto kept = [&](float l) { const auto it = bodies.find(l); return it != bodies.end() && it->second.first >= (size_t)A && it->second.second >= D; };
            size_t nb = 0, nt = 0, r1 = 0;
            for (const auto& [l, b] : bodies) if (kept(l)) { ++nb; nt += b.first; }
            for (int y = 1; y < N - 1; ++y)
                for (int x = 1; x < N - 1; ++x) {
                    const size_t i = (size_t)y * N + x;
                    if (win.levelM[i] > W::WATER_FILL_DRY && kept(win.levelM[i])) continue;
                    float lo = 1e30f, hi = -1e30f;
                    for (int dy = -1; dy <= 1; ++dy)
                        for (int dx = -1; dx <= 1; ++dx) {
                            const float l = win.levelM[(size_t)(y + dy) * N + (x + dx)];
                            if (l <= W::WATER_FILL_DRY || !kept(l)) continue;
                            lo = std::min(lo, l); hi = std::max(hi, l);
                        }
                    if (hi - lo > 1.0f) ++r1;
                }
            std::printf("    keep >= %2d texels and > %.2f m deep: %4zu lakes, %5zu wet texels, %4zu rims between lakes > 1 m apart\n",
                        A, D, nb, nt, r1);
        }
    // The coarse levels inside the window, for the record.
    std::map<int, size_t> coarseLevels;
    for (float c : cLv) if (c > W::WATER_FILL_DRY) coarseLevels[(int)std::lround(c)]++;
    for (const auto& [l, n] : coarseLevels) std::printf("    coarse fill level %d m over %zu window texels\n", l, n);
    CHECK(true, "probe ran");
}

HARUKA_REGISTER_TESTS(
    { "water_real_bake_probe", test_water_real_bake_probe });
