/**
 * @file TestResourceStore.cpp
 * @brief EL ALMACÉN CPU↔GPU (`Renderer/ResourceStore.h`), sin GPU: un destructor falso que anota.
 *
 *   1. UNA subida por revisión: publicar tres veces antes de procesar sube una vez, la última.
 *      CONTRAPRUEBA: publicar-procesar-publicar-procesar sube dos.
 *   2. NADA SE DESTRUYE CON UN FRAME EN VUELO: lo que sustituye una resubida sobrevive
 *      `framesInFlight` frames y luego se destruye. CONTRAPRUEBA: con 0 frames en vuelo, al siguiente.
 *   3. El presupuesto por frame se respeta (subidas de 3 ms, presupuesto de 5 ms → como mucho 2)
 *      y lo que no cupo sale al frame siguiente.
 *   4. `keepCpu=false` suelta los datos tras subirlos; `true` los conserva con su revisión.
 *   5. `release` también espera a los frames en vuelo; y pedir los datos con otro tipo da nullptr.
 */
#include "Common/TestCommon.h"
#include "Haruka/Renderer/ResourceStore.h"

#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

using namespace Haruka;
using namespace Haruka::Renderer;

namespace {
struct FakeGpu {
    uint32_t next = 1;
    std::vector<uint32_t> destroyed;   ///< ids de buffer destruidos, en orden
    ResourceStore::Uploader uploader(int* uploads = nullptr, int sleepMs = 0) {
        return [this, uploads, sleepMs](const void*) {
            if (sleepMs) std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
            if (uploads) ++*uploads;
            GpuSet s; s.buffers.push_back({ next++ }); return s;
        };
    }
    ResourceStore::Destroyer destroyer() {
        return [this](const GpuSet& s) { for (auto b : s.buffers) destroyed.push_back(b.id); };
    }
};
bool contains(const std::vector<uint32_t>& v, uint32_t x) {
    for (auto y : v) if (y == x) return true;
    return false;
}
}

void test_resource_store() {
    beginTest("recursos CPU<->GPU: una subida por revision, nada se destruye en vuelo");

    // 1. coalescencia de revisiones
    {
        FakeGpu g; ResourceStore st(g.destroyer(), 2);
        int ups = 0;
        const ResourceId id = st.create("malla", true);
        for (int v = 1; v <= 3; ++v) st.publish<int>(id, std::make_shared<const int>(v), g.uploader(&ups));
        st.processUploads(100.0);
        uint64_t gr = 0; st.gpu(id, &gr);
        CHECK(ups == 1 && gr == 3, "tres publicaciones antes de procesar: UNA subida, de la revision 3");
        uint64_t cr = 0; auto d = st.cpu<int>(id, &cr);
        CHECK(d && *d == 3 && cr == 3, "y la CPU tiene la ultima version");

        int ups2 = 0;
        const ResourceId id2 = st.create("malla2", true);
        st.publish<int>(id2, std::make_shared<const int>(1), g.uploader(&ups2)); st.processUploads(100.0);
        st.publish<int>(id2, std::make_shared<const int>(2), g.uploader(&ups2)); st.processUploads(100.0);
        CHECK(ups2 == 2, "CONTRAPRUEBA: publicar y procesar dos veces sube dos");
    }

    // 2. destrucción diferida
    auto destroyedAfter = [](uint32_t inFlight) {
        FakeGpu g; ResourceStore st(g.destroyer(), inFlight);
        const ResourceId id = st.create("tex", true);
        st.publish<int>(id, std::make_shared<const int>(1), g.uploader()); st.processUploads(100.0);
        const uint32_t old = st.gpu(id)->buffers[0].id;
        st.publish<int>(id, std::make_shared<const int>(2), g.uploader()); st.processUploads(100.0);
        int frames = 0;
        while (!contains(g.destroyed, old) && frames < 10) { st.beginFrame(); ++frames; }
        return frames;
    };
    const int f2 = destroyedAfter(2), f0 = destroyedAfter(0);
    std::printf("    la GPU sustituida se destruye tras %d frames (2 en vuelo) · tras %d (0 en vuelo)\n", f2, f0);
    CHECK(f2 == 2, "con 2 frames en vuelo, lo sustituido sobrevive 2 frames y luego se destruye");
    CHECK(f0 == 1, "CONTRAPRUEBA: sin frames en vuelo se destruye en el siguiente beginFrame");

    // 3. presupuesto por frame
    {
        FakeGpu g; ResourceStore st(g.destroyer(), 2);
        int ups = 0;
        for (int i = 0; i < 6; ++i) {
            const ResourceId id = st.create("lento", true);
            st.publish<int>(id, std::make_shared<const int>(i), g.uploader(&ups, 3));
        }
        const size_t a = st.processUploads(5.0);
        const size_t pend = st.stats().pendingUploads;
        size_t frames = 1;
        while (st.stats().pendingUploads && frames < 20) { st.processUploads(5.0); ++frames; }
        std::printf("    6 subidas de 3 ms con 5 ms de presupuesto: %zu en el primer frame, %zu frames en total\n", a, frames);
        CHECK(a >= 1 && a <= 2, "el presupuesto corta: 1 o 2 subidas de 3 ms en 5 ms");
        CHECK(pend == 6 - a && ups == 6, "lo que no cupo queda pendiente y sale en los frames siguientes");
    }

    // 4. keepCpu
    {
        FakeGpu g; ResourceStore st(g.destroyer(), 2);
        const ResourceId bake = st.create("horneado", false), cave = st.create("cueva", true);
        st.publish<int>(bake, std::make_shared<const int>(7), g.uploader());
        st.publish<int>(cave, std::make_shared<const int>(8), g.uploader());
        CHECK(st.cpu<int>(bake) != nullptr, "antes de subir, el horneado tiene sus datos");
        st.processUploads(100.0);
        CHECK(st.cpu<int>(bake) == nullptr, "keepCpu=false: tras subirlo se sueltan los datos de CPU");
        CHECK(st.cpu<int>(cave) && *st.cpu<int>(cave) == 8, "CONTRAPRUEBA: keepCpu=true los conserva");
    }

    // 5. release diferido y tipo equivocado
    {
        FakeGpu g; ResourceStore st(g.destroyer(), 2);
        const ResourceId id = st.create("chunk", true);
        st.publish<int>(id, std::make_shared<const int>(1), g.uploader()); st.processUploads(100.0);
        const uint32_t buf = st.gpu(id)->buffers[0].id;
        CHECK(st.cpu<float>(id) == nullptr, "pedir los datos con otro tipo da nullptr, no basura");
        st.release(id);
        CHECK(!contains(g.destroyed, buf), "release no destruye en el acto");
        st.beginFrame(); st.beginFrame();
        CHECK(contains(g.destroyed, buf), "release destruye pasados los frames en vuelo");
        CHECK(st.gpu(id) == nullptr && st.stats().resources == 0, "y el recurso ya no existe");
    }
}

// Registro: el grupo sale de la carpeta de este archivo (ver Common/TestCommon.h).
HARUKA_REGISTER_TESTS(
    { "resource_store", test_resource_store });
