#include "audio/sound_synth.h"

#include <algorithm>
#include <cmath>

namespace Haruka::Audio {

// ── mini DSP ──────────────────────────────────────────────────────────────────────────────────

namespace {

constexpr float kTwoPi = 6.28318530718f;

struct Rng {
    uint32_t s;
    explicit Rng(uint32_t seed) : s(seed ? seed : 0x9E3779B9u) {}
    float uni() {                                   // blanco en [-1, 1]
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        return (float)s * (2.0f / 4294967296.0f) - 1.0f;
    }
};

/// Un polo: y += a (x − y). fc en Hz.
struct LP1 {
    float y = 0.0f, a;
    explicit LP1(float fc) : a(1.0f - std::exp(-kTwoPi * fc / (float)kRate)) {}
    float operator()(float x) { y += a * (x - y); return y; }
};

/// Banda = LP(alta) − LP(baja).
struct Band {
    LP1 lo, hi;
    Band(float fLo, float fHi) : lo(fLo), hi(fHi) {}
    float operator()(float x) { return hi(x) - lo(x); }
};

/// Ruido rosa (Paul Kellet, version economica): 1/f entre 20 Hz y Nyquist.
struct Pink {
    float b0 = 0, b1 = 0, b2 = 0;
    float operator()(float w) {
        b0 = 0.99765f * b0 + w * 0.0990460f;
        b1 = 0.96300f * b1 + w * 0.2965164f;
        b2 = 0.57000f * b2 + w * 1.0526913f;
        return (b0 + b1 + b2 + w * 0.1848f) * 0.25f;
    }
};

/// Ruido marron (blanco integrado con fuga): 1/f².
struct Brown {
    float b = 0.0f;
    float operator()(float w) { b = 0.995f * b + 0.05f * w; return b * 3.5f; }
};

/// Fundido cola→cabeza: los ultimos `xf` segundos se mezclan sobre los primeros y se recortan.
void makeSeamless(std::vector<float>& v, float xfSeconds) {
    const int n  = (int)v.size();
    const int xf = std::min(n / 2, (int)(xfSeconds * kRate));
    if (xf <= 0) return;
    for (int i = 0; i < xf; ++i) {
        const float f = (float)i / (float)xf;          // 0 → 1
        v[(size_t)i] = v[(size_t)i] * f + v[(size_t)(n - xf + i)] * (1.0f - f);
    }
    v.resize((size_t)(n - xf));
}

/// Normaliza a un RMS objetivo (con tope de pico): todos los ruidos suenan a la misma sonoridad
/// nominal y la sonoridad RELATIVA la decide el punto (`power`), no el fichero.
void normalizeRms(std::vector<float>& v, float rms, float peakMax) {
    double e = 0.0; float pk = 0.0f;
    for (float x : v) { e += (double)x * x; pk = std::max(pk, std::fabs(x)); }
    if (v.empty() || e <= 0.0) return;
    float g = rms / (float)std::sqrt(e / (double)v.size());
    if (pk * g > peakMax) g = peakMax / pk;
    for (float& x : v) x *= g;
}
void normalizePeak(std::vector<float>& v, float peak) {
    float pk = 0.0f;
    for (float x : v) pk = std::max(pk, std::fabs(x));
    if (pk <= 0.0f) return;
    const float g = peak / pk;
    for (float& x : v) x *= g;
}

/// LFO PERIODICO en el bucle: `cycles` vueltas enteras en T segundos → no salta en la costura.
inline float lfo(float t, float T, float cycles, float phase) {
    return 0.5f + 0.5f * std::sin(kTwoPi * cycles * t / T + phase);
}

inline float attackDecay(float t, float attackS, float decayS) {
    if (t < 0.0f) return 0.0f;
    return (1.0f - std::exp(-t / attackS)) * std::exp(-t / decayS);
}

} // namespace

// ── nombres y longitudes ──────────────────────────────────────────────────────────────────────

const char* soundName(Sound s) {
    switch (s) {
        case Sound::WindEar:    return "viento";
        case Sound::Leaves:     return "hojas";
        case Sound::Surf:       return "rompiente";
        case Sound::Lapping:    return "chapoteo";
        case Sound::Underwater: return "sumergido";
        case Sound::StepGrass:  return "pisada.hierba";
        case Sound::StepSand:   return "pisada.arena";
        case Sound::StepRock:   return "pisada.roca";
        case Sound::StepSnow:   return "pisada.nieve";
        case Sound::StepWater:  return "pisada.agua";
        case Sound::Splash:     return "chapuzon";
        case Sound::Crack:      return "crujido";
        default:                return "?";
    }
}

float soundSeconds(Sound s) {
    switch (s) {
        case Sound::WindEar:    return 10.0f;
        case Sound::Leaves:     return 8.0f;
        case Sound::Surf:       return 27.0f;   // 3 olas de 9 s
        case Sound::Lapping:    return 8.0f;
        case Sound::Underwater: return 6.0f;
        case Sound::StepGrass:  return 0.18f;
        case Sound::StepSand:   return 0.22f;
        case Sound::StepRock:   return 0.12f;
        case Sound::StepSnow:   return 0.20f;
        case Sound::StepWater:  return 0.30f;
        case Sound::Splash:     return 0.70f;
        case Sound::Crack:      return 0.35f;
        default:                return 0.1f;
    }
}

// ── la sintesis ───────────────────────────────────────────────────────────────────────────────

std::vector<float> synthSoundF(Sound s, uint32_t seed) {
    const float T = soundSeconds(s);
    const int   n = (int)(T * kRate);
    std::vector<float> v((size_t)n, 0.0f);
    Rng rng(seed * 2654435761u + (uint32_t)s * 97u);

    switch (s) {
    case Sound::WindEar: {
        // Marron por debajo de 600 Hz: el "soplo" sordo del aire en la oreja. Dos rachas lentas.
        Brown br; LP1 lp(600.0f);
        for (int i = 0; i < n; ++i) {
            const float t = (float)i / kRate;
            const float env = 0.55f + 0.45f * lfo(t, T, 2.0f, 0.0f) * (0.6f + 0.4f * lfo(t, T, 5.0f, 1.3f));
            v[(size_t)i] = lp(br(rng.uni())) * env;
        }
        makeSeamless(v, 0.5f);
        normalizeRms(v, 0.14f, 0.95f);
        break;
    }
    case Sound::Leaves: {
        // Rosa en la banda 800–5000 Hz: el siseo de las hojas, con un aleteo a dos velocidades.
        Pink pk; Band bd(800.0f, 5000.0f);
        for (int i = 0; i < n; ++i) {
            const float t = (float)i / kRate;
            const float env = 0.5f + 0.5f * lfo(t, T, 3.0f, 0.0f) * (0.7f + 0.3f * lfo(t, T, 11.0f, 0.4f));
            v[(size_t)i] = bd(pk(rng.uni())) * env;
        }
        makeSeamless(v, 0.4f);
        normalizeRms(v, 0.12f, 0.95f);
        break;
    }
    case Sound::Surf: {
        // Blanco 200–3000 Hz con TRES olas de 9 s: sube en medio segundo, se apaga en 3,5. Las
        // colas se evaluan tambien para la ola "anterior" (k = −1) para que la vuelta sea exacta.
        Band bd(200.0f, 3000.0f);
        const float amp[3] = { 1.0f, 0.75f, 0.9f };
        for (int i = 0; i < n; ++i) {
            const float t = (float)i / kRate;
            float env = 0.12f;
            for (int k = -1; k < 3; ++k) {
                const float tk = 9.0f * (float)k;
                env += amp[(k + 3) % 3] * attackDecay(t - tk, 0.5f, 3.5f);
            }
            v[(size_t)i] = bd(rng.uni()) * env;
        }
        makeSeamless(v, 0.5f);
        normalizeRms(v, 0.12f, 0.95f);
        break;
    }
    case Sound::Lapping: {
        // Rosa 400–4000 Hz con rizos de 5 y 9 ciclos por vuelta: el agua contra la orilla en calma.
        Pink pk; Band bd(400.0f, 4000.0f);
        for (int i = 0; i < n; ++i) {
            const float t = (float)i / kRate;
            const float env = 0.35f + 0.65f * lfo(t, T, 5.0f, 0.0f) * lfo(t, T, 9.0f, 0.7f);
            v[(size_t)i] = bd(pk(rng.uni())) * env;
        }
        makeSeamless(v, 0.4f);
        normalizeRms(v, 0.10f, 0.95f);
        break;
    }
    case Sound::Underwater: {
        // Marron bajo 150 Hz con un vaiven lento: el oido tapado por el agua.
        Brown br; LP1 lp(150.0f);
        for (int i = 0; i < n; ++i) {
            const float t = (float)i / kRate;
            v[(size_t)i] = lp(br(rng.uni())) * (0.7f + 0.3f * lfo(t, T, 1.0f, 0.0f));
        }
        makeSeamless(v, 0.5f);
        normalizeRms(v, 0.14f, 0.95f);
        break;
    }
    // ── disparos ──
    case Sound::StepGrass: {
        Pink pk; Band bd(300.0f, 3000.0f);
        for (int i = 0; i < n; ++i) {
            const float t = (float)i / kRate;
            const float env = attackDecay(t, 0.003f, 0.045f) + 0.4f * attackDecay(t - 0.06f, 0.004f, 0.03f);
            v[(size_t)i] = bd(pk(rng.uni())) * env;
        }
        normalizePeak(v, 0.9f);
        break;
    }
    case Sound::StepSand: {
        Band bd(1000.0f, 6000.0f);
        for (int i = 0; i < n; ++i) {
            const float t = (float)i / kRate;
            v[(size_t)i] = bd(rng.uni()) * attackDecay(t, 0.01f, 0.07f);
        }
        normalizePeak(v, 0.9f);
        break;
    }
    case Sound::StepRock: {
        LP1 lp(900.0f); Band click(2000.0f, 6000.0f);
        for (int i = 0; i < n; ++i) {
            const float t = (float)i / kRate;
            const float w = rng.uni();
            v[(size_t)i] = lp(w) * attackDecay(t, 0.002f, 0.06f) * 1.5f + click(w) * attackDecay(t, 0.001f, 0.02f);
        }
        normalizePeak(v, 0.9f);
        break;
    }
    case Sound::StepSnow: {
        // Crujido: tres sub-rafagas que se apagan.
        Pink pk; LP1 lp(1500.0f);
        for (int i = 0; i < n; ++i) {
            const float t = (float)i / kRate;
            const float env = attackDecay(t, 0.004f, 0.04f) + 0.7f * attackDecay(t - 0.04f, 0.004f, 0.035f)
                            + 0.45f * attackDecay(t - 0.09f, 0.004f, 0.03f);
            v[(size_t)i] = lp(pk(rng.uni())) * env;
        }
        normalizePeak(v, 0.9f);
        break;
    }
    case Sound::StepWater: {
        Band bd(500.0f, 5000.0f);
        for (int i = 0; i < n; ++i) {
            const float t = (float)i / kRate;
            const float env = attackDecay(t, 0.03f, 0.10f) + 0.5f * attackDecay(t - 0.12f, 0.005f, 0.03f)
                            + 0.35f * attackDecay(t - 0.19f, 0.005f, 0.025f);
            v[(size_t)i] = bd(rng.uni()) * env;
        }
        normalizePeak(v, 0.9f);
        break;
    }
    case Sound::Splash: {
        Band bd(300.0f, 6000.0f); LP1 bub(400.0f);
        for (int i = 0; i < n; ++i) {
            const float t = (float)i / kRate;
            const float w = rng.uni();
            const float bubbles = bub(w) * (0.5f + 0.5f * std::sin(kTwoPi * 12.0f * t)) * attackDecay(t - 0.15f, 0.05f, 0.25f) * 2.0f;
            v[(size_t)i] = bd(w) * attackDecay(t, 0.02f, 0.22f) + bubbles;
        }
        normalizePeak(v, 0.9f);
        break;
    }
    case Sound::Crack: {
        // Cuatro chasquidos secos en los primeros 120 ms (fibras que ceden) y un golpe sordo.
        Band snap(800.0f, 5000.0f); LP1 thud(300.0f);
        float tk[4]; for (float& x : tk) x = 0.12f * (0.5f + 0.5f * rng.uni());
        std::sort(tk, tk + 4);
        for (int i = 0; i < n; ++i) {
            const float t = (float)i / kRate;
            const float w = rng.uni();
            float env = 0.0f;
            for (float x : tk) env += attackDecay(t - x, 0.0005f, 0.008f);
            v[(size_t)i] = snap(w) * env + thud(w) * attackDecay(t - tk[0], 0.004f, 0.12f) * 3.0f;
        }
        normalizePeak(v, 0.9f);
        break;
    }
    default: break;
    }
    return v;
}

std::vector<int16_t> synthSound(Sound s, uint32_t seed) {
    const std::vector<float> f = synthSoundF(s, seed);
    std::vector<int16_t> out(f.size());
    for (size_t i = 0; i < f.size(); ++i)
        out[i] = (int16_t)(std::clamp(f[i], -1.0f, 1.0f) * 32000.0f);
    return out;
}

} // namespace Haruka::Audio
