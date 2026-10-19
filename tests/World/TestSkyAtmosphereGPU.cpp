// ================================================================================================
// EL CIELO DE LA GPU ES EL GEMELO DE LA INTEGRAL DE CPU (`lib/atmosphere.glsl` <-> `Atmosphere.h`).
//
// El cielo se pinta con una integral por pixel (Rayleigh + Mie + ozono + la tabla de dispersion
// multiple), y la luz que reciben el terreno, el agua y las nubes sale de la MISMA fisica evaluada en
// CPU (`Renderer/SkyLight`). Si las dos caras divergen, el suelo queda iluminado por un cielo que no
// es el que se ve. Aqui se dibuja `sky.frag` con una luz conocida y, en una rejilla de pixeles, se
// compara con `atmoInscatter` de CPU pasado por la misma salida a pantalla (`skyDisplay`).
//
// CONTRAPRUEBA: la CPU SIN la tabla de dispersion multiple tiene que quedar lejos de lo que pinta la
// GPU (si no, el careo no veria que la GPU dejase de leer la tabla).
// ================================================================================================
#include "RHI/Common/RHITestCommon.h"
#include "Haruka/Renderer/Shader.h"
#include "Haruka/Renderer/SkyLight.h"
#include "Haruka/World/Planet/Atmosphere.h"

#include <glm/gtc/matrix_transform.hpp>

void testSkyAtmosphereParity()
{
    BEGIN("cielo: la GPU pinta la MISMA integral que la CPU (con dispersion multiple)");
    namespace P = Haruka::World::Planet;

    uint32_t uw = 0, uh = 0; g_dev->framebufferSize(uw, uh);
    const int w = (uw > 0) ? (int)uw : 256, h = (uh > 0) ? (int)uh : 256;

    const double R = 6371000.0;
    const glm::dvec3 centerRelEye(0.0, -(R + 2.0), 0.0);
    const glm::dvec3 sunDir = glm::normalize(glm::dvec3(0.60, 0.50, 0.20));   // Sol a ~38°
    setTestSkyLight(centerRelEye, R, sunDir);

    // La MISMA luz en CPU: mismo `SkyLight`, misma entrada, misma exposicion (1).
    Haruka::Renderer::SkyLight cpu;
    {
        Haruka::Renderer::SkyLight::Input in;
        in.eye = glm::dvec3(0.0); in.planetCenter = centerRelEye; in.planetRadius = R;
        in.hasSun = true; in.sunPos = sunDir * 1.495978707e11;
        cpu.setFixedExposure(1.0);
        cpu.update(in);
    }
    const Haruka::Renderer::SkyLightUBO& L = cpu.data();

    const std::string base = Haruka::Renderer::Shader::baseDir();
    const std::string vs = base + "shaders/sky.vert", fs = base + "shaders/sky.frag";
    PipelineDesc pd;
    pd.vertexPath = vs.c_str(); pd.fragmentPath = fs.c_str();
    pd.topology = PrimitiveTopology::Triangles;
    pd.depth.test = false; pd.depth.write = false; pd.blend.enable = false;
    PipelineHandle pipe = g_dev->createPipeline(pd);
    CHECK(valid(pipe), "pipeline del cielo creado");
    if (!valid(pipe)) { cpu.shutdown(); return; }

    struct SkyUBO {
        float invVP[16];
        float sunDir[3]; float sunElev;
        float up[3];     float atmo;
        float sunColor[3]; float time;
        float weather[4]; float wind[4]; float planet[4];
    } u{};
    // Mirando al horizonte de lado, 15° arriba, 70° de campo: cenit, horizonte y el halo del Sol.
    const glm::vec3 fwd = glm::normalize(glm::vec3(0.0f, std::sin(glm::radians(15.0f)), -std::cos(glm::radians(15.0f))));
    const glm::mat4 proj = glm::perspective(glm::radians(70.0f), (float)w / (float)h, 0.1f, 100.0f);
    const glm::mat4 view = glm::lookAt(glm::vec3(0.0f), fwd, glm::vec3(0, 1, 0));
    const glm::mat4 invVP = glm::inverse(proj * glm::mat4(glm::mat3(view)));
    std::memcpy(u.invVP, &invVP[0][0], sizeof(u.invVP));
    u.up[1] = 1.0f; u.atmo = 1.0f; u.planet[0] = (float)R;
    BufferHandle ubo = g_dev->createBuffer(BufferUsage::Uniform, sizeof(u), &u, BufferMemory::Dynamic);

    std::vector<uint8_t> px((size_t)w * h * 4, 0xAA);
    const bool isVk = (g_dev->backend() == Backend::Vulkan);
    for (int f = 0; f < 3; ++f) {
        Context* ctx = g_dev->beginFrame();
        if (!ctx) break;
        ClearValues cv; cv.clearColor = true; cv.clearDepth = true; cv.depth = 0.0f;
        ctx->beginRenderPass({}, cv);
        ctx->bindPipeline(pipe); ctx->bindUniformBuffer(5, ubo);
        ctx->draw(3, 1);
        ctx->endRenderPass();
        if (!isVk && f == 2) g_dev->readPixels(0, 0, w, h, Format::RGBA8, px.data());
        g_dev->endFrame(); pumpWindowEvents();
        if (isVk && f == 2) g_dev->readPixels(0, 0, w, h, Format::RGBA8, px.data());
    }

    // El careo, en una rejilla de 16 x 16 pixeles. El orden de filas del readback depende del backend
    // (ver `rowToNdcY`): se prueban los dos y vale el que casa — con el cenit oscuro arriba y el
    // horizonte claro abajo, el orden equivocado no puede casar.
    const glm::dvec3 eye = -centerRelEye;
    const glm::dvec3 sunE(L.sunE), moonE(L.moonE), moonDir(L.moonDir);
    auto worstFor = [&](bool flip, bool multiple) {
        double worst = 0.0;
        for (int gy = 0; gy < 16; ++gy)
            for (int gx = 0; gx < 16; ++gx) {
                const int x = (gx * w) / 16 + w / 32, y = (gy * h) / 16 + h / 32;
                const float ndcX = 2.0f * ((float)x + 0.5f) / (float)w - 1.0f;
                const float ndcY = rowToNdcY(y, h, flip);
                const glm::vec4 far = invVP * glm::vec4(ndcX, ndcY, 1.0f, 1.0f);
                const glm::dvec3 dir = glm::normalize(glm::dvec3(glm::vec3(far) / far.w));
                const glm::dvec3 lin = P::atmoInscatter(eye, dir, -1.0, R, sunDir, sunE, moonDir, moonE, nullptr,
                                                        multiple ? &cpu.msLut() : nullptr);
                const glm::dvec3 d = Haruka::Renderer::skyDisplay(lin) * 255.0;
                const uint8_t* p = &px[((size_t)y * w + x) * 4];
                for (int c = 0; c < 3; ++c) worst = std::max(worst, std::abs(d[c] - (double)p[c]));
            }
        return worst;
    };
    const double eA = worstFor(false, true), eB = worstFor(true, true);
    const bool flip = eB < eA;
    const double worst = std::min(eA, eB), worstNoMs = worstFor(flip, false);
    const uint8_t* mid = &px[((size_t)(h / 2) * w + w / 2) * 4];
    std::printf("    256 pixeles: peor diferencia GPU<->CPU %.2f/255 (filas %s) · centro (%u, %u, %u)\n",
                worst, flip ? "invertidas" : "directas", mid[0], mid[1], mid[2]);
    std::printf("    CONTRAPRUEBA: contra la CPU SIN dispersion multiple, peor %.2f/255\n", worstNoMs);
    CHECK(worst <= 3.0, "el cielo de la GPU es el de la CPU (<= 3/255 en cada canal: float contra double y 8 bits)");
    CHECK(worstNoMs > 3.0 * std::max(worst, 1.0), "CONTRAPRUEBA: sin la tabla de dispersion multiple no casa (la GPU la lee)");

    g_dev->destroy(pipe); g_dev->destroy(ubo);
    cpu.shutdown();
}
