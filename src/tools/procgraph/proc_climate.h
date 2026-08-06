#pragma once

#include "proc_graph.h"
#include "core/planet/climate.h"
#include "core/planet/geology.h"
#include "core/weather_system.h"
#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>   // glm::pi — llegaba de rebote por el cube_sphere.h borrado

namespace Haruka { namespace Tools { namespace ProcGraph {

// ===========================================================================
// SampleClimateNode — samples temperature, humidity, precipitation at (x,y,z)
//   Input:  none (uses evaluate x,y,z as direction)
//   Output 0: temperature (float, °C)
//   Output 1: humidity    (float, 0-1)
//   Output 2: precipitation (float, m/year)
// ===========================================================================
class SampleClimateNode : public Node {
public:
    const Haruka::Planet::ClimateOutput* climate = nullptr;
    const Haruka::Planet::GeologyOutput* geology = nullptr;

    SampleClimateNode() = default;

    std::string name() const override { return "SampleClimate"; }

    std::vector<SocketDesc> inputs() const override { return {}; }
    std::vector<SocketDesc> outputs() const override {
        return {
            {"Temperature", DataType::Float},
            {"Humidity", DataType::Float},
            {"Precipitation", DataType::Float},
        };
    }

    void evaluate(const Value*, int,
                  Value* outputs, int numOutputs,
                  float x, float y, float z) override {
        glm::dvec3 dir(x, y, z);
        if (glm::length(dir) < 0.001) dir = glm::dvec3(0, 1, 0);
        else dir = glm::normalize(dir);

        double elevKm = geology ? geology->elevationModifier(dir) : 0.0;
        bool ocean = elevKm < 0;

        if (numOutputs > 0) {
            double t = climate ? climate->temperature(dir, elevKm, ocean) : 15.0;
            outputs[0] = Value::Float((float)t);
        }
        if (numOutputs > 1) {
            double h = climate ? climate->humidity(dir, elevKm, ocean) : 0.5;
            outputs[1] = Value::Float((float)h);
        }
        if (numOutputs > 2) {
            double p = climate ? climate->precipitation(dir, elevKm, ocean) : 0.5;
            outputs[2] = Value::Float((float)p);
        }
    }
};

// ===========================================================================
// SampleElevationNode — samples elevation at (x,y,z)
//   Input:  none (uses evaluate x,y,z as direction)
//   Output 0: elevation (float, km)
// ===========================================================================
class SampleElevationNode : public Node {
public:
    const Haruka::Planet::GeologyOutput* geology = nullptr;

    SampleElevationNode() = default;

    std::string name() const override { return "SampleElevation"; }
    std::vector<SocketDesc> inputs() const override { return {}; }
    std::vector<SocketDesc> outputs() const override {
        return {{"Elevation", DataType::Float}};
    }

    void evaluate(const Value*, int,
                  Value* outputs, int numOutputs,
                  float x, float y, float z) override {
        glm::dvec3 dir(x, y, z);
        if (glm::length(dir) < 0.001) dir = glm::dvec3(0, 1, 0);
        else dir = glm::normalize(dir);

        double elev = geology ? geology->elevationModifier(dir) : 0.0;
        if (numOutputs > 0)
            outputs[0] = Value::Float((float)elev);
    }
};

// ===========================================================================
// SampleSlopeNode — estimates slope at (x,y,z) via finite differences
//   Input:  none (uses evaluate x,y,z as direction + geology)
//   Output 0: slope (float, degrees 0-90)
// ===========================================================================
class SampleSlopeNode : public Node {
public:
    const Haruka::Planet::GeologyOutput* geology = nullptr;

    SampleSlopeNode() = default;

    std::string name() const override { return "SampleSlope"; }
    std::vector<SocketDesc> inputs() const override { return {}; }
    std::vector<SocketDesc> outputs() const override {
        return {{"Slope", DataType::Float}};
    }

    void evaluate(const Value*, int,
                  Value* outputs, int numOutputs,
                  float x, float y, float z) override {
        glm::dvec3 dir(x, y, z);
        if (glm::length(dir) < 0.001) dir = glm::dvec3(0, 1, 0);
        else dir = glm::normalize(dir);

        if (!geology || numOutputs < 1) {
            outputs[0] = Value::Float(0);
            return;
        }

        double h = geology->elevationModifier(dir) * 1000.0;

        // Finite differences in tangent plane
        const double eps = 1e-5;
        glm::dvec3 u, v;
        if (std::abs(dir.y) < 0.999) {
            u = glm::normalize(glm::cross(dir, glm::dvec3(0, 1, 0)));
            v = glm::cross(dir, u);
        } else {
            u = glm::dvec3(1, 0, 0);
            v = glm::cross(dir, u);
        }

        glm::dvec3 dirU = glm::normalize(dir + u * eps);
        glm::dvec3 dirV = glm::normalize(dir + v * eps);
        double hU = geology->elevationModifier(dirU) * 1000.0;
        double hV = geology->elevationModifier(dirV) * 1000.0;

        double gradU = (hU - h) / eps;
        double gradV = (hV - h) / eps;
        double slopeRad = std::atan(std::sqrt(gradU * gradU + gradV * gradV));
        outputs[0] = Value::Float((float)(slopeRad * 180.0 / glm::pi<double>()));
    }
};

// ===========================================================================
// SampleWeatherNode — wraps WeatherSystem to output weather at (x,y,z)
//   Input:  none (uses evaluate x,y,z as direction)
//   Output 0: cloudCover (float, 0-1)
//   Output 1: precip     (float, 0-1)
//   Output 2: tempC      (float, °C)
//   Output 3: humidity   (float, 0-1)
//   Output 4: windSpeed  (float, m/s)
// ===========================================================================
class SampleWeatherNode : public Node {
public:
    const Haruka::WeatherSystem* weather = nullptr;

    SampleWeatherNode() = default;

    std::string name() const override { return "SampleWeather"; }
    std::vector<SocketDesc> inputs() const override { return {}; }
    std::vector<SocketDesc> outputs() const override {
        return {
            {"CloudCover", DataType::Float},
            {"Precip", DataType::Float},
            {"TempC", DataType::Float},
            {"Humidity", DataType::Float},
            {"WindSpeed", DataType::Float},
        };
    }

    void evaluate(const Value*, int,
                  Value* outputs, int numOutputs,
                  float x, float y, float z) override {
        glm::dvec3 dir(x, y, z);
        if (glm::length(dir) < 0.001) dir = glm::dvec3(0, 1, 0);
        else dir = glm::normalize(dir);

        if (!weather) {
            for (int i = 0; i < numOutputs && i < 5; i++)
                outputs[i] = Value::Float(0);
            return;
        }

        // sampleAt needs tempC and humidity from the climate field as input,
        // but we don't always have that. Use defaults (15°C, 0.5 humidity).
        // For a fully connected graph, wire SampleClimateNode outputs into
        // a MathNode → SampleWeatherNode when exact values are needed.
        Haruka::WeatherSample ws = weather->sampleAt(dir, 15.0f, 0.5f);

        if (numOutputs > 0) outputs[0] = Value::Float(ws.cloudCover);
        if (numOutputs > 1) outputs[1] = Value::Float(ws.precip);
        if (numOutputs > 2) outputs[2] = Value::Float(ws.tempC);
        if (numOutputs > 3) outputs[3] = Value::Float(ws.humidity);
        if (numOutputs > 4) outputs[4] = Value::Float(glm::length(ws.wind));
    }
};

// ===========================================================================
// BiomeConfig — configurable palette and thresholds for BiomeClassifyNode.
// Default values match the original hardcoded GLSL constants.
// ===========================================================================
struct BiomeConfig {
    // --- FILA TEMPLADA: la escala de humedad de siempre (seco → húmedo) --------------------
    glm::vec3 desert  = glm::vec3(0.62f, 0.53f, 0.35f);
    glm::vec3 steppe  = glm::vec3(0.47f, 0.44f, 0.26f);
    glm::vec3 grass   = glm::vec3(0.26f, 0.34f, 0.16f);
    glm::vec3 forest  = glm::vec3(0.18f, 0.30f, 0.13f);
    glm::vec3 jungle  = glm::vec3(0.13f, 0.31f, 0.11f);

    // Thresholds for smoothstep transitions (edge0, edge1)
    float steppeEdge0 = 0.10f, steppeEdge1 = 0.26f;
    float grassEdge0  = 0.22f, grassEdge1  = 0.42f;
    float forestEdge0 = 0.46f, forestEdge1 = 0.66f;
    float jungleEdge0 = 0.72f, jungleEdge1 = 0.92f;

    // --- FILA FRÍA y FILA CÁLIDA: el segundo eje ------------------------------------------
    // Sin temperatura, la clasificación era una LÍNEA: a igual humedad salía el mismo bioma a
    // −30 °C que a +35 °C, así que tundra, taiga y sabana no podían existir por construcción y el
    // planeta se leía como bandas de humedad. La temperatura estaba disponible (la calcula
    // `ClimateOutput::temperature`) pero solo se usaba como TINTE de la luz en el shader.
    glm::vec3 ice     = glm::vec3(0.86f, 0.89f, 0.93f);  // frío extremo: hielo/nieve permanente
    glm::vec3 tundra  = glm::vec3(0.52f, 0.51f, 0.44f);  // frío + seco: musgo y roca desnuda
    glm::vec3 taiga   = glm::vec3(0.20f, 0.28f, 0.22f);  // frío + húmedo: conífera, verde apagado
    glm::vec3 savanna = glm::vec3(0.60f, 0.55f, 0.28f);  // cálido + humedad media: herbazal seco

    // Bandas de temperatura en °C. Entre `cold` y `warm` manda la fila templada; fuera, se mezcla
    // hacia la fría o la cálida. En grados y no normalizado a propósito: son el mismo número que
    // lee la consola y el mismo que sale del clima, así que se pueden razonar.
    // Calibradas sobre la Tierra: el hielo permanente empieza donde la media anual ronda −15 °C,
    // y entre ahí y ~10 °C vive la franja boreal (tundra/taiga). Con la banda de hielo más alta
    // (−14/−4) el hielo se comía la fila fría entera y no llegaba a verse ni tundra ni taiga.
    float iceEdge0  = -22.0f, iceEdge1  = -12.0f;  // por debajo → hielo
    float coldEdge0 =  -1.0f, coldEdge1 =  11.0f;  // fría ↔ templada
    float warmEdge0 =  19.0f, warmEdge1 = 27.0f;   // templada ↔ cálida

    // Dentro de la fila fría, dónde pasa de tundra (seco) a taiga (húmedo).
    float taigaEdge0 = 0.30f, taigaEdge1 = 0.55f;
    // Dentro de la cálida, dónde pasa de desierto a sabana y de sabana a selva.
    float savannaEdge0 = 0.18f, savannaEdge1 = 0.40f;
    float hotJungleEdge0 = 0.55f, hotJungleEdge1 = 0.80f;

    float edgeNoiseStrength = 0.12f; // how much edge noise perturbs humidity

    /**
     * @brief Color del bioma en (humedad, temperatura). H en [0,1], tempC en grados.
     *
     * Tres filas por temperatura, cada una con su propia escala de humedad, mezcladas con
     * smoothstep: la frontera entre filas es un degradado, no un escalón, o la línea de nieve
     * saldría recortada con la forma de la retícula del campo.
     */
    glm::vec3 evaluate(float H, float tempC) const {
        // Fila templada (comportamiento histórico; a ~15 °C esto es EXACTAMENTE lo de antes).
        glm::vec3 temperate = desert;
        temperate = glm::mix(temperate, steppe, smoothstep(steppeEdge0, steppeEdge1, H));
        temperate = glm::mix(temperate, grass,  smoothstep(grassEdge0,  grassEdge1,  H));
        temperate = glm::mix(temperate, forest, smoothstep(forestEdge0, forestEdge1, H));
        temperate = glm::mix(temperate, jungle, smoothstep(jungleEdge0, jungleEdge1, H));

        glm::vec3 coldRow = glm::mix(tundra, taiga, smoothstep(taigaEdge0, taigaEdge1, H));
        coldRow = glm::mix(ice, coldRow, smoothstep(iceEdge0, iceEdge1, tempC));

        glm::vec3 hotRow = glm::mix(desert, savanna, smoothstep(savannaEdge0, savannaEdge1, H));
        hotRow = glm::mix(hotRow, jungle, smoothstep(hotJungleEdge0, hotJungleEdge1, H));

        glm::vec3 c = glm::mix(coldRow, temperate, smoothstep(coldEdge0, coldEdge1, tempC));
        c = glm::mix(c, hotRow, smoothstep(warmEdge0, warmEdge1, tempC));
        return c;
    }

    /** @brief Compatibilidad: la escala de humedad sola, a temperatura templada. */
    glm::vec3 evaluate(float H) const { return evaluate(H, 15.0f); }

private:
    static float smoothstep(float e0, float e1, float x) {
        float t = glm::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    }
};

// ===========================================================================
// BiomeClassifyNode — classifies biome from climate data and outputs color
//   Input 0: edgeNoise (float, optional, default 0) — from FBM for boundary breaking
//   Output 0: Vec4(biomeColor.r, biomeColor.g, biomeColor.b, biomeIdx)
//
//   biomeIdx = 0=desert … 1=jungle (continuous humidity-based index)
//
// Auto-samples humidity from internal climate pointer. The edge noise input
// perturbs humidity to break clean biome boundaries. Per-pixel overrides
// (slope→rock, flow→riparian) stay in the shader.
// ===========================================================================
class BiomeClassifyNode : public Node {
public:
    const Haruka::Planet::ClimateOutput* climate = nullptr;
    const Haruka::Planet::GeologyOutput* geology = nullptr;
    BiomeConfig config;

    BiomeClassifyNode() = default;

    std::string name() const override { return "BiomeClassify"; }
    std::vector<SocketDesc> inputs() const override {
        return {{"EdgeNoise", DataType::Float}};
    }
    std::vector<SocketDesc> outputs() const override {
        return {{"BiomeColor", DataType::Vec4}};
    }

    void evaluate(const Value* inputs, int numInputs,
                  Value* outputs, int numOutputs,
                  float x, float y, float z) override {
        glm::dvec3 dir(x, y, z);
        if (glm::length(dir) < 0.001) dir = glm::dvec3(0, 1, 0);
        else dir = glm::normalize(dir);

        float humidity = 0.5f;
        float tempC    = 15.0f;   // templado si no hay clima: la fila de siempre
        if (climate) {
            double e = geology ? geology->elevationModifier(dir) : 0.0;
            const bool ocean = e < 0;
            humidity = (float)climate->humidity(dir, e, ocean);
            // MISMA elevación y misma bandera de océano que la humedad: la temperatura baja con la
            // cota (lapse rate), así que muestrearla en otro punto pondría la línea de nieve a una
            // altura distinta de la del bioma que la rodea.
            tempC    = (float)climate->temperature(dir, e, ocean);
        }

        float edgeNoise = (numInputs > 0) ? inputs[0].asFloat() : 0.0f;
        float n = edgeNoise - 0.5f;
        float H = glm::clamp(humidity + n * config.edgeNoiseStrength, 0.0f, 1.0f);

        glm::vec3 c = config.evaluate(H, tempC);
        float biomeIdx = H;

        if (numOutputs > 0)
            outputs[0] = Value::Vec4(c.x, c.y, c.z, biomeIdx);
    }
};

// ===========================================================================
// generateClimateField — evaluate a climate graph across a cube-sphere grid
//
// Builds a graph using SampleClimateNode (+optional SampleWeatherNode) and
// evaluates it at every cell of a 6-face cube-sphere of the given resolution.
// Results are written to the output vectors in face-major order:
//   index = face * R * R + j * R + i
//
// This can be used to populate the PlanetClimate SSBO (bindings 9-10 in the
// shader) as an alternative to PlanetFields::computeClimate().
//
// Parameters:
//   graph        — compiled ProcGraph whose output(s) to sample
//   nodeIdx      — node index to read
//   outputIdx    — output index of that node (0 = first float)
//   faceRes      — cube face resolution (e.g. 256)
//   result       — output float buffer (size = 6 * faceRes * faceRes)
inline void generateClimateField(const Graph& graph,
                                  int nodeIdx, int outputIdx,
                                  int faceRes,
                                  std::vector<float>& result)
{
    result.resize((size_t)6 * faceRes * faceRes);
    Graph& g = const_cast<Graph&>(graph);
    const double step = 2.0 / faceRes;

    for (int face = 0; face < 6; ++face) {
        for (int j = 0; j < faceRes; ++j) {
            for (int i = 0; i < faceRes; ++i) {
                // Cube face → unit direction
                double u = (i + 0.5) * step - 1.0;
                double v = (j + 0.5) * step - 1.0;
                double w = 1.0;
                glm::dvec3 dir;
                switch (face) {
                    case 0: dir = glm::dvec3( w, -v, -u); break; // +X
                    case 1: dir = glm::dvec3(-w, -v,  u); break; // -X
                    case 2: dir = glm::dvec3( u,  w,  v); break; // +Y
                    case 3: dir = glm::dvec3( u, -w, -v); break; // -Y
                    case 4: dir = glm::dvec3( u, -v,  w); break; // +Z
                    case 5: dir = glm::dvec3(-u, -v, -w); break; // -Z
                }
                dir = glm::normalize(dir);
                float val = g.evaluate(nodeIdx, outputIdx,
                                       (float)dir.x, (float)dir.y, (float)dir.z).asFloat();
                result[(size_t)face * faceRes * faceRes + (size_t)j * faceRes + i] = val;
            }
        }
    }
}

}}} // namespace Haruka::Tools::ProcGraph
