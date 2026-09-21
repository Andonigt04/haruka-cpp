/**
 * @file test_sound.cpp
 * @brief Sonido del mundo sin dispositivo: la SINTESIS (sound_synth.h) y los PUNTOS LOGICOS
 *        agrupados por sector (sound_points.h). Nada de OpenAL: es lo que se puede medir aqui.
 *
 * Contrapruebas: la metrica de costura detecta un salto metido a mano; el criterio espectral de
 * las hojas lo suspende el viento y al reves; cien puntos al este son UNA voz, pero 50 al este y
 * 50 al oeste son DOS, y dos sonidos distintos en el mismo sector tambien son dos.
 */
#include "test_common.h"
#include "audio/sound_synth.h"
#include "audio/sound_points.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace Haruka::Audio;

namespace {

// Fraccion de la energia por ENCIMA de `fc` (un polo): sirve de "espectro" sin FFT.
float energyAbove(const std::vector<float>& v, float fc) {
    const float a = 1.0f - std::exp(-6.2831853f * fc / (float)kRate);
    float y = 0.0f; double eHi = 0.0, eAll = 0.0;
    for (float x : v) { y += a * (x - y); const float hi = x - y; eHi += (double)hi * hi; eAll += (double)x * x; }
    return eAll > 0.0 ? (float)(eHi / eAll) : 0.0f;
}

// Percentil 99 del salto entre muestras vecinas, y el salto en la costura (ultima → primera).
void seamStats(const std::vector<float>& v, float& p99, float& seam) {
    std::vector<float> d; d.reserve(v.size());
    for (size_t i = 1; i < v.size(); ++i) d.push_back(std::fabs(v[i] - v[i - 1]));
    std::sort(d.begin(), d.end());
    p99  = d[(size_t)(0.99 * (double)(d.size() - 1))];
    seam = std::fabs(v.front() - v.back());
}

// RMS por ventanas de `win` segundos.
std::vector<float> windowRms(const std::vector<float>& v, float win) {
    const size_t w = (size_t)(win * kRate);
    std::vector<float> out;
    for (size_t i = 0; i + w <= v.size(); i += w) {
        double e = 0.0; for (size_t j = i; j < i + w; ++j) e += (double)v[j] * v[j];
        out.push_back((float)std::sqrt(e / (double)w));
    }
    return out;
}

int activeVoices(const SoundPoints& sp, Sound s, const SoundPoints::Voice** first = nullptr) {
    int n = 0;
    for (const auto& v : sp.voices()) if (v.active && v.sound == s) { if (first && !*first) *first = &v; ++n; }
    return n;
}

void settle(SoundPoints& sp, const glm::dvec3& L, const glm::vec3& up, float seconds) {
    for (int i = 0; i < (int)(seconds * 60.0f); ++i) sp.update(L, up, 1.0f / 60.0f);
}

} // namespace

// `HARUKA_SOUND_WAV=<dir>` vuelca los 12 sonidos a WAV para escucharlos fuera del juego.
static void dumpWav(const char* dir, Sound s) {
    const std::vector<int16_t> pcm = synthSound(s);
    std::string path = std::string(dir) + "/" + soundName(s) + ".wav";
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    const uint32_t dataLen = (uint32_t)(pcm.size() * 2), rate = kRate, byteRate = rate * 2;
    const uint16_t ch = 1, bits = 16, align = 2, fmt = 1;
    auto w32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto w16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    std::fwrite("RIFF", 1, 4, f); w32(36 + dataLen); std::fwrite("WAVE", 1, 4, f);
    std::fwrite("fmt ", 1, 4, f); w32(16); w16(fmt); w16(ch); w32(rate); w32(byteRate); w16(align); w16(bits);
    std::fwrite("data", 1, 4, f); w32(dataLen); std::fwrite(pcm.data(), 2, pcm.size(), f);
    std::fclose(f);
}

void test_sound_synth() {
    beginTest("sound_synth");
    if (const char* dir = std::getenv("HARUKA_SOUND_WAV")) {
        for (int k = 0; k < (int)Sound::COUNT; ++k) dumpWav(dir, (Sound)k);
        std::printf("  (12 WAV volcados en %s)\n", dir);
    }

    for (int k = 0; k < (int)Sound::COUNT; ++k) {
        const Sound s = (Sound)k;
        const std::vector<float> v = synthSoundF(s);
        bool finite = true; float pk = 0.0f; double e = 0.0;
        for (float x : v) { if (!std::isfinite(x)) finite = false; pk = std::max(pk, std::fabs(x)); e += (double)x * x; }
        const float rms = (float)std::sqrt(e / (double)std::max<size_t>(1, v.size()));
        char msg[160];
        std::snprintf(msg, sizeof msg, "%s: %zu muestras, finito, pico %.2f <= 0.95, rms %.3f > 0.02", soundName(s), v.size(), pk, rms);
        CHECK(!v.empty() && finite && pk <= 0.951f && rms > 0.02f, msg);
        if (isLoop(s)) {
            float p99, seam; seamStats(v, p99, seam);
            std::snprintf(msg, sizeof msg, "%s: costura %.4f <= p99 de saltos vecinos %.4f (bucle sin salto)", soundName(s), seam, p99);
            CHECK(seam <= p99, msg);
            // La longitud del bucle es la anunciada menos el fundido (≤ 0,5 s).
            CHECK(std::fabs((float)v.size() / kRate - soundSeconds(s)) <= 0.51f, "la longitud del bucle es la anunciada menos el fundido");
        }
    }
    // CONTRAPRUEBA de la costura: un salto metido a mano SI se detecta.
    {
        std::vector<float> v = synthSoundF(Sound::Lapping);
        v.back() = 0.9f;
        float p99, seam; seamStats(v, p99, seam);
        CHECK(seam > p99, "contraprueba: con la ultima muestra a 0,9 la costura supera el p99");
    }
    // Espectro: las hojas son agudas, el viento en el oido es grave; y cada uno suspende el criterio del otro.
    {
        const float leavesHi = energyAbove(synthSoundF(Sound::Leaves), 800.0f);
        const float windHi   = energyAbove(synthSoundF(Sound::WindEar), 800.0f);
        const float underHi  = energyAbove(synthSoundF(Sound::Underwater), 300.0f);
        char msg[160];
        std::snprintf(msg, sizeof msg, "hojas: %.0f %% de la energia por encima de 800 Hz (> 60)", leavesHi * 100.0f);
        CHECK(leavesHi > 0.60f, msg);
        std::snprintf(msg, sizeof msg, "viento en el oido: %.0f %% por encima de 800 Hz (< 20) — suspende el criterio de las hojas", windHi * 100.0f);
        CHECK(windHi < 0.20f, msg);
        std::snprintf(msg, sizeof msg, "sumergido: %.0f %% por encima de 300 Hz (< 15)", underHi * 100.0f);
        CHECK(underHi < 0.15f, msg);
    }
    // La rompiente tiene TRES olas por vuelta: tres maximos de la envolvente por encima del 60 % del pico.
    {
        const std::vector<float> env = windowRms(synthSoundF(Sound::Surf), 0.5f);
        const float mx = *std::max_element(env.begin(), env.end());
        const float mn = *std::min_element(env.begin(), env.end());
        int peaks = 0;
        for (size_t i = 1; i + 1 < env.size(); ++i)
            if (env[i] > env[i - 1] && env[i] >= env[i + 1] && env[i] > 0.6f * mx) ++peaks;
        char msg[160];
        std::snprintf(msg, sizeof msg, "rompiente: %d olas por vuelta (3), envolvente max/min %.1f (> 3)", peaks, mx / std::max(mn, 1e-6f));
        CHECK(peaks == 3 && mx / std::max(mn, 1e-6f) > 3.0f, msg);
        // Contraprueba: el chapoteo NO tiene esa forma de ola (mas de 3 maximos, todos parecidos).
        const std::vector<float> env2 = windowRms(synthSoundF(Sound::Lapping), 0.5f);
        const float mx2 = *std::max_element(env2.begin(), env2.end());
        const float mn2 = *std::min_element(env2.begin(), env2.end());
        CHECK(mx2 / std::max(mn2, 1e-6f) < 3.0f, "contraprueba: la envolvente del chapoteo es plana (max/min < 3)");
    }
    // Determinista: dos sintesis con la misma semilla son identicas; otra semilla, no.
    {
        const auto a = synthSound(Sound::Crack, 7), b = synthSound(Sound::Crack, 7), c = synthSound(Sound::Crack, 8);
        CHECK(a == b, "misma semilla → mismo PCM");
        CHECK(a != c, "otra semilla → otro PCM");
    }
}

void test_sound_points_grouping() {
    beginTest("sound_points_grouping");
    const glm::dvec3 L(1000.0, 2000.0, 3000.0);
    const glm::vec3  up(0, 0, 1);            // vertical local Z: el plano tangente es XY
    // "Este" y "oeste" en el CENTRO de un sector (15° de la frontera): un punto justo en una
    // frontera de sector cambia de grupo con un milimetro, y eso es lo que el suavizado tapa, no
    // lo que se mide aqui. Con e1 = up × Y = (−1,0,0), la frontera del sector 0 cae en +X.
    const glm::vec3  east(std::cos(15.0f * 3.14159265f / 180.0f), std::sin(15.0f * 3.14159265f / 180.0f), 0.0f);
    const glm::vec3  west = -east;
    SoundPoints::Config cfg;                 // ref 6 m, 12 sectores, anillos 15/45/160, 24 voces

    // ── 100 arboles al este a 40 m → UNA voz de hojas con la energia sumada ────────────────────
    {
        SoundPoints sp(cfg);
        for (int i = 0; i < 100; ++i)
            sp.add(L + glm::dvec3(east) * 40.0 + glm::dvec3(0.0, (i % 10) * 0.6 - 3.0, 0.0), Sound::Leaves, 0.5f);
        settle(sp, L, up, 1.0f);
        const SoundPoints::Voice* v = nullptr;
        const int n = activeVoices(sp, Sound::Leaves, &v);
        CHECK(n == 1, "100 arboles en el mismo sector son UNA voz");
        if (v) {
            CHECK(v->count == 100, "y la voz mezcla los 100");
            const float d = glm::dot(glm::normalize(v->relPos), east);
            CHECK(d > 0.99f, "la voz suena al ESTE (centroide)");
            // amplitud de uno = 0,5 · 6/40 = 0,075; cien incoherentes = √100 · 0,075 = 0,75
            char msg[120]; std::snprintf(msg, sizeof msg, "ganancia %.3f = √100 × 0,075 = 0,75 (suma incoherente)", v->gain);
            CHECK(std::fabs(v->gain - 0.75f) < 0.04f, msg);
        }
        // Un solo arbol: 0,075 — la agrupacion SUMA, no promedia.
        SoundPoints one(cfg);
        one.add(L + glm::dvec3(east) * 40.0, Sound::Leaves, 0.5f);
        settle(one, L, up, 1.0f);
        const SoundPoints::Voice* v1 = nullptr; activeVoices(one, Sound::Leaves, &v1);
        CHECK(v1 && std::fabs(v1->gain - 0.075f) < 0.005f, "contraprueba: un arbol solo suena a 0,075 (10× menos que cien)");
    }
    // ── CONTRAPRUEBAS de la agrupacion ─────────────────────────────────────────────────────────
    {
        SoundPoints sp(cfg);
        for (int i = 0; i < 50; ++i) sp.add(L + glm::dvec3(east) * 40.0, Sound::Leaves, 0.5f);
        for (int i = 0; i < 50; ++i) sp.add(L + glm::dvec3(west) * 40.0, Sound::Leaves, 0.5f);
        settle(sp, L, up, 0.5f);
        CHECK(activeVoices(sp, Sound::Leaves) == 2, "50 al este y 50 al oeste son DOS voces (sectores distintos)");
        SoundPoints sp2(cfg);
        for (int i = 0; i < 50; ++i) sp2.add(L + glm::dvec3(east) * 40.0, Sound::Leaves, 0.5f);
        for (int i = 0; i < 50; ++i) sp2.add(L + glm::dvec3(east) * 40.0, Sound::Surf, 0.5f);
        settle(sp2, L, up, 0.5f);
        CHECK(activeVoices(sp2, Sound::Leaves) == 1 && activeVoices(sp2, Sound::Surf) == 1,
              "hojas y rompiente en el mismo sector NO se mezclan: una voz cada una");
        // Mismo azimut, distinto anillo: dos voces (10 m y 40 m).
        SoundPoints sp3(cfg);
        sp3.add(L + glm::dvec3(east) * 10.0, Sound::Leaves, 0.5f);
        sp3.add(L + glm::dvec3(east) * 40.0, Sound::Leaves, 0.5f);
        settle(sp3, L, up, 0.5f);
        CHECK(activeVoices(sp3, Sound::Leaves) == 2, "mismo azimut a 10 y a 40 m: anillos distintos, dos voces");
    }
    // ── Fuera de alcance y demasiado flojo: sin voz ────────────────────────────────────────────
    {
        SoundPoints sp(cfg);
        sp.add(L + glm::dvec3(east) * 200.0, Sound::Surf, 1.0f);
        sp.add(L + glm::dvec3(west) * 100.0, Sound::Surf, 0.001f);   // 0,001·6/100 = 6e-5 < 0,004
        settle(sp, L, up, 0.5f);
        CHECK(activeVoices(sp, Sound::Surf) == 0, "a 200 m (fuera del ultimo anillo) o a 6e-5 de ganancia no hay voz");
        CHECK(sp.groupsBeforeBudget() == 1, "el flojo forma grupo pero no pasa el umbral; el lejano ni grupo");
    }
    // ── Presupuesto: 72 grupos → 24 voces, las mas fuertes ─────────────────────────────────────
    {
        SoundPoints sp(cfg);
        int k = 0;
        for (int ring = 0; ring < 3; ++ring)
            for (int s = 0; s < 12; ++s) {
                const double r = (ring == 0) ? 8.0 : (ring == 1) ? 30.0 : 100.0;
                const double a = (s + 0.5) / 12.0 * 6.2831853;
                const glm::dvec3 p = L + glm::dvec3(std::cos(a) * r, std::sin(a) * r, 0.0);
                sp.add(p, Sound::Leaves, 0.3f + 0.01f * (float)(k++));
                sp.add(p, Sound::Surf,   0.3f + 0.01f * (float)(k++));
            }
        settle(sp, L, up, 1.0f);
        int active = 0; float minKept = 1e9f;
        for (const auto& v : sp.voices()) if (v.active) { ++active; minKept = std::min(minKept, v.gain); }
        char msg[160];
        std::snprintf(msg, sizeof msg, "%d grupos antes del presupuesto (72), %d voces activas (24)", sp.groupsBeforeBudget(), active);
        CHECK(sp.groupsBeforeBudget() == 72 && active == 24, msg);
        // Los 24 que suenan son los mas fuertes: el mas flojo de los que quedan (anillo cercano,
        // 8 m: ~0,3·0,75 = 0,22) supera a cualquiera del anillo lejano (100 m: ~0,5·0,06 = 0,03).
        CHECK(minKept > 0.1f, "los 24 con voz son del anillo cercano (ganancia > 0,1); los de 100 m se quedan fuera");
    }
    // ── El grupo conserva su ranura; al cruzar de sector, la vieja se apaga y nace otra ─────────
    {
        SoundPoints sp(cfg);
        const auto id = sp.add(L + glm::dvec3(east) * 30.0, Sound::Leaves, 1.0f);
        settle(sp, L, up, 0.5f);
        int slot = -1; uint32_t key = 0;
        for (size_t i = 0; i < sp.voices().size(); ++i) if (sp.voices()[i].active) { slot = (int)i; key = sp.voices()[i].key; }
        CHECK(slot >= 0, "un punto: una ranura activa");
        sp.set(id, L + glm::dvec3(east) * 31.0 + glm::dvec3(0.0, 0.5, 0.0), 1.0f);   // se mueve dentro del sector
        settle(sp, L, up, 0.2f);
        CHECK(slot >= 0 && sp.voices()[(size_t)slot].active && sp.voices()[(size_t)slot].key == key,
              "moverse 1 m dentro del sector: MISMA ranura y misma clave (la voz no se reinicia)");
        sp.set(id, L + glm::dvec3(west) * 30.0, 1.0f);                                   // cruza al oeste
        sp.update(L, up, 1.0f / 60.0f);
        bool oldFading = slot >= 0 && !sp.voices()[(size_t)slot].active && sp.voices()[(size_t)slot].gain > 0.0f;
        int newSlot = -1;
        for (size_t i = 0; i < sp.voices().size(); ++i) if (sp.voices()[i].active) newSlot = (int)i;
        CHECK(oldFading && newSlot >= 0 && newSlot != slot, "al cruzar de sector: la ranura vieja se apaga (gain > 0, inactiva) y nace otra");
        settle(sp, L, up, 0.6f);
        CHECK(slot >= 0 && sp.voices()[(size_t)slot].gain == 0.0f, "y 0,6 s despues la vieja esta a 0");
        // Al quitar el punto todo se apaga.
        sp.remove(id);
        settle(sp, L, up, 0.6f);
        bool allOff = true;
        for (const auto& v : sp.voices()) if (v.active || v.gain > 0.0f) allOff = false;
        CHECK(allOff, "sin puntos, sin voces (todas a 0 en 0,6 s)");
    }
    // ── En el propio oido: sector centro, sin direccion ────────────────────────────────────────
    {
        SoundPoints sp(cfg);
        sp.add(L, Sound::WindEar, 0.7f, 1.1f);
        settle(sp, L, up, 0.5f);
        const SoundPoints::Voice* v = nullptr;
        CHECK(activeVoices(sp, Sound::WindEar, &v) == 1, "el viento en el oido es una voz");
        CHECK(v && glm::length(v->relPos) < 0.01f && std::fabs(v->gain - 0.7f) < 0.02f && std::fabs(v->pitch - 1.1f) < 0.01f,
              "en el origen (sin direccion), ganancia 0,7 (a menos de ref no atenua) y tono 1,1");
    }
    // ── Disparos: cinco crujidos en el mismo sitio son UNO mas fuerte, y solo duran un frame ───
    {
        SoundPoints sp(cfg);
        for (int i = 0; i < 5; ++i) sp.emit(L + glm::dvec3(east) * 6.0, Sound::Crack, 0.4f);
        sp.emit(L + glm::dvec3(west) * 6.0, Sound::StepGrass, 0.4f);
        sp.update(L, up, 1.0f / 60.0f);
        CHECK(sp.oneShots().size() == 2, "5 crujidos + 1 pisada = 2 disparos mezclados");
        for (const auto& sh : sp.oneShots()) {
            if (sh.sound == Sound::Crack) {
                char msg[120]; std::snprintf(msg, sizeof msg, "crujido: 5 mezclados, ganancia %.3f = √5 × 0,4", sh.gain);
                CHECK(sh.count == 5 && std::fabs(sh.gain - std::sqrt(5.0f) * 0.4f) < 0.01f, msg);
            }
        }
        sp.update(L, up, 1.0f / 60.0f);
        CHECK(sp.oneShots().empty(), "al frame siguiente no queda ningun disparo");
        CHECK(sp.pointCount() == 0, "y los disparos no dejan puntos persistentes");
    }
}

// ── El panel de depuracion: percentiles y tx/rx (tools/debug_overlay.h), sin ImGui ────────────
#include "tools/debug_overlay.h"
void test_debug_overlay_stats() {
    beginTest("debug_overlay_stats");
    auto& ov = Haruka::Tools::DebugOverlay::get();
    Haruka::Tools::DebugFrameInfo f;
    // Con 240 frames, p99.5 es la SEGUNDA peor muestra (rango 0,995·239 = 238) y p98 la sexta: un
    // tiron aislado NO sale en p99.5; dos si. Es la naturaleza del percentil con esta ventana.
    for (int i = 0; i < 240; ++i) { f.dtSeconds = (i == 100) ? 0.100f : 0.016f; ov.pushFrame(f); }
    ov.computeStats();
    char msg[160];
    std::snprintf(msg, sizeof msg, "p50 %.1f p98 %.1f p99.5 %.1f: UN tiron de 100 ms en 240 no llega a p99.5 (2ª peor)", ov.p50Ms(), ov.p98Ms(), ov.p995Ms());
    CHECK(std::fabs(ov.p50Ms() - 16.0f) < 0.01f && std::fabs(ov.p98Ms() - 16.0f) < 0.01f && std::fabs(ov.p995Ms() - 16.0f) < 0.01f, msg);
    std::snprintf(msg, sizeof msg, "fps %.1f = 240 frames / (239·16 + 100) ms", ov.fps());
    CHECK(std::fabs(ov.fps() - 240.0f * 1000.0f / (239.0f * 16.0f + 100.0f)) < 0.1f, msg);
    for (int i = 0; i < 240; ++i) { f.dtSeconds = (i == 100 || i == 200) ? 0.100f : 0.016f; ov.pushFrame(f); }   // 2 tirones
    ov.computeStats();
    std::snprintf(msg, sizeof msg, "p98 %.1f p99.5 %.1f: DOS tirones en 240 salen en p99.5 y no en p98", ov.p98Ms(), ov.p995Ms());
    CHECK(std::fabs(ov.p98Ms() - 16.0f) < 0.01f && std::fabs(ov.p995Ms() - 100.0f) < 0.01f, msg);
    for (int i = 0; i < 240; ++i) { f.dtSeconds = (i % 40 == 0) ? 0.100f : 0.016f; ov.pushFrame(f); }   // 6 tirones (2,5 %)
    ov.computeStats();
    CHECK(std::fabs(ov.p98Ms() - 100.0f) < 0.01f && std::fabs(ov.p50Ms() - 16.0f) < 0.01f, "contraprueba: seis tirones en 240 (2,5 %) SI entran en p98 y no en p50");
    // tx/rx: acumulados → KiB/s con el reloj del frame (ventana de 1 s). Un frame de 1,5 s fuerza el
    // corte de ventana; el siguiente, de 1 s justo, mide.
    f.dtSeconds = 1.5f; f.net.txBytes = 0; f.net.rxBytes = 0; ov.pushFrame(f);
    f.dtSeconds = 1.0f; f.net.txBytes = 8192; f.net.rxBytes = 40960; ov.pushFrame(f);
    std::snprintf(msg, sizeof msg, "tx %.1f rx %.1f KiB/s (8 y 40 esperados)", ov.txKiBs(), ov.rxKiBs());
    CHECK(std::fabs(ov.txKiBs() - 8.0f) < 0.2f && std::fabs(ov.rxKiBs() - 40.0f) < 0.5f, msg);
}
