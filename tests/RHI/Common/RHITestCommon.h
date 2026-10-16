#pragma once
// Lo COMPARTIDO por el banco de GPU (RHITestMain.cpp y los *GPU.cpp de cada modulo): el
// mini-framework (CHECK/XFAIL, defectos conocidos de driver, metricas de linea base), el device y
// la ventana, y los helpers que usa mas de un archivo. Los de uno solo viven en ese archivo.
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstddef>   // offsetof
#include <memory>
#include <string>
#include <vector>
#include <cmath>
#include <chrono>
#include "Haruka/RHI/RHIDevice.h"
#include "Haruka/Renderer/GPUInstancing.h"
#include "Haruka/World/Terrain/CubeSphere.h"      // dirToCubeFaceClosed (la sonda de la hierba)
#include "Haruka/RHI/RHIContext.h"
#include "Haruka/RHI/RHIResources.h"
#include "Haruka/Core/Logger.h"
#include "Haruka/Renderer/Shader.h"
#include "Haruka/Core/Camera.h"
#include "Haruka/Core/SkyAmbient.h"   // gemelo CPU del ambiente (paridad)
#include "Haruka/World/Terrain/TerrainNode.h"   // v5 F1: referencia CPU del nodo + hash golden
#include "Haruka/World/Terrain/TerrainNodePool.h"
#include "Haruka/World/Terrain/BaseField.h"   // baseFieldHeightAt: el gemelo CPU que este test valida
#include "Haruka/World/Terrain/TerrainNodeGpu.h"
#include "Haruka/World/WeatherSystem.h"   // kFieldScale: la escala del campo de nube, no un literal
#include "Haruka/World/CloudColumn.h"
#include "Haruka/World/CloudMotion.h"
#include "Haruka/World/Water/OceanWave.h"   // gemelo CPU de la ola (paridad con lib/ocean_wave.glsl)
#include "Haruka/IO/ImageWriter.h"          // HARUKA_WATER_PNG: volcar las formas para MIRARLAS
#include "Haruka/World/Water/WaterFill.h"   // el relleno de cuencas: gemelo del campo que lee la GPU
#include "Haruka/World/Terrain/TerrainNodeRenderer.h"   // Shader::baseDir() para los shaders del banco
#include <SDL3/SDL.h>
#include <array>
#include <map>
#include <fstream>
#include <cctype>
#include <sys/stat.h>
#include <unordered_set>

#define HARUKA_AMD_GL_FP64 \
    "AMD+OpenGL degrada el fp64 en su compilador GLSL (medido: el mismo hardware en Vulkan da lo " \
    "mismo que NVIDIA). Cerrado, sin arreglo por codigo; el motor avisa al arrancar y por defecto " \
    "usa Vulkan. Ver la nota de esta lista."
#define HARUKA_LLVMPIPE_FP64 "el rasterizador software de Mesa anuncia fp64 y no lo cumple"
#define CHECK(cond, msg) do { \
    if (cond) { ++g_pass; } \
    else if (const char* _r = knownDefect(g_cur, msg)) { \
        ++g_xfail; std::printf("  [XFAIL] [%s] %s  <- %s\n", g_cur, msg, _r); \
    } \
    else { ++g_fail; std::printf("  [FAIL] [%s] %s\n", g_cur, msg); } \
} while (0)
#define BEGIN(name) do { g_cur = name; std::printf("== %s ==\n", name); } while (0)


extern int g_xfail;
extern int g_fail;
extern int g_pass;


extern const char * g_cur;


using namespace Haruka::RHI;
   // `Backend` lo usa la lista de defectos de abajo

// ── DEFECTOS CONOCIDOS DE UN DRIVER ─────────────────────────────────────────────────────────────
// Un test puede fallar sin que el motor tenga nada: hay drivers que dan mal un resultado que la
// especificación exige. El caso vivo es el fp64 — el driver de AMD y el rasterizador software
// lavapipe ANUNCIAN `shaderFloat64 = true` y aun así degradan los doubles.
//
// La tentación es borrar el test o bajarle el umbral. Las dos cosas pierden información: en AMD ese
// fallo es el bug CRÍTICO del suelo (el jugador pisa un terreno distinto según la GPU) y tiene que
// seguir en rojo. Lo que hace falta es poder decir "en ESTE driver esto es un defecto conocido, y
// aquí está el porqué", sin silenciarlo en ningún otro.
//
// Reglas, y la segunda es la que hace que esto no se pudra:
//   · un fallo que casa con una entrada se cuenta como XFAIL y no tumba la tanda;
//   · una entrada que NO llega a usarse en la tanda de su backend es un FALLO. Si el driver mejora,
//     o si alguien reescribe el mensaje del CHECK, la lista deja de describir la realidad y hay que
//     enterarse — una lista de excepciones que nadie revisa acaba tapando bugs de verdad.
struct DriverDefect {
    Backend     backend;
    const char* device;   ///< subcadena de `Device::deviceName()` (GL_RENDERER / deviceName)
    const char* test;     ///< subcadena del nombre del test (`BEGIN`)
    const char* check;    ///< subcadena del mensaje del `CHECK`
    const char* reason;   ///< por qué NO es del motor. Obligatorio: sin motivo no entra.
    int         hits;
};


// ── AMD + OpenGL: CERRADO, Y POR ESO BAJA A ESTA LISTA (2026-09-09) ─────────────────────────────
//
// ⚠️ AQUÍ PONÍA "nada de AMD a propósito: su fp64 malo es un bug REAL y se quiere ver en rojo". Era
// lo correcto MIENTRAS la investigación seguía abierta: un fallo en rojo es un recordatorio de que
// falta entender algo. Ya no falta. El caso está cerrado y lo que queda no es información nueva cada
// vez que corre el banco, es la misma frase repetida nueve veces.
//
// Lo que se sabe, medido (2026-08-31 / 09-01):
//   · NO es la GPU: el MISMO hardware con Vulkan da las mismas cifras que NVIDIA (bake peor 0,0001 m
//     contra 0,0253 m en GL). Lo rompe el front-end GLSL del driver, que Vulkan no usa.
//   · No hay arreglo por código: están degradados hasta el PRODUCTO y la DIVISIÓN en doble, no sólo
//     las trascendentes. `harukaSqrtD` (Newton en float, sin `sqrt` de double) bajó el bake de
//     0,0253 a 0,0070 m y se quedó porque es mejor código, pero no cierra: la proyección cara→esfera
//     no se puede hacer sin multiplicar.
//   · La consecuencia sigue viva y NO se silencia donde importa: `GLDevice.cpp` suelta un bloque de
//     WARN al arrancar con la cifra y la frase "tu mundo NO coincide con el de los demás jugadores",
//     y el backend por defecto es Vulkan. Un XFAIL en el banco no le quita el aviso a nadie.
//
// La regla de abajo es la que hace que esto siga siendo honesto: si el driver mejora, estas entradas
// dejan de usarse y la tanda FALLA por entrada muerta. O sea que el día que Mesa lo arregle nos
// enteramos, en vez de arrastrar una excepción que ya no describe nada.



extern DriverDefect g_driverDefects[19];



extern Backend g_curBackend;


extern std::string g_curDevice;


/** @brief ¿Este fallo está declarado como defecto de ESTE driver? Devuelve el motivo, o nullptr. */
const char* knownDefect(const char* test, const char* msg);





// ── LINEA BASE: lo que el banco MIDE pasa a EXIGIRSE ─────────────────────────────────────────────
//
// El banco imprimia ms, presencias, grano, convergencia... y no fallaba si empeoraban: cada numero
// se miraba una vez y se olvidaba. Aqui cada metrica se compara con la ultima aprobada, guardada en
// `tests/baseline/<backend>_<gpu>.txt` (las de rendimiento dependen de la GPU) o `any.txt` (las de
// imagen). Sin linea base la metrica se ANOTA y se avisa; con `HARUKA_BASELINE_UPDATE=1` se
// reescribe (eso es "aprobar": lo hace una persona, no el banco). `HARUKA_BASELINE_DIR` cambia el
// directorio (por defecto `tests/baseline`, relativo a donde se lanza el banco).

extern std::map<std::string, double> g_baseline;
       // clave -> valor aprobado (el fichero actual)

extern std::map<std::string, double> g_baselineNew;
    // lo medido en esta tanda (para escribirlo)

extern bool g_baselineOk;
extern bool g_baselineLoaded;


extern std::string g_baselineDir;

std::string baselineSlug(const std::string& s);

std::string baselinePath(bool perGpu);

void baselineLoadFile(bool perGpu);

void baselineInit();

/// `key`: nombre de la metrica · `value`: lo medido · `tolRel`: tolerancia relativa (0,3 = 30 %) ·
/// `upperOnly`: solo importa que no SUBA (coste, grano) · `perGpu`: depende de la maquina.
void baselineMetric(const char* key, double value, double tolRel, bool upperOnly, bool perGpu, bool valid = true);



extern Device * g_dev;


// ================================================================================================
// CAREO GL ↔ VULKAN: el banco compara cada backend CONSIGO MISMO, nunca uno contra otro
//
// ⚠️ ESE ERA EL HUECO. Cada test dibuja, lee pixeles y comprueba una propiedad; los dos backends
// corren en el MISMO proceso, uno detras de otro, y nadie compara las dos imagenes. Aqui se guarda la
// captura de cada uno bajo un nombre y al terminar los dos se carean. Es lo que convierte un "se ve
// raro" en un numero.
struct Shot { int w = 0, h = 0; std::vector<uint8_t> px; };


extern std::map<std::string, Shot> g_shots[2];
      // 0 = OpenGL · 1 = Vulkan

void recordShot(const char* name, int w, int h, const std::vector<uint8_t>& px);


// Un ciclo de frame completo: acquire→clear→present (la cadena que daba TDR en NVIDIA) y,
// en GL, verificación del clear vía readPixels (paridad de valores).

// ⚠️ VACIAR LA COLA DE EVENTOS NO ES OPCIONAL, aunque el test no lea ninguno.
//
// Una ventana que presenta frames pero nunca responde a eventos es, para el compositor, una
// aplicación COLGADA: deja de repintarla y el escritorio la marca como no responde. El banco pasaba
// sus 152 comprobaciones correctamente y aun así la pantalla se quedaba helada a media ejecución —
// un síntoma que parece un cuelgue de GPU y no lo es.
//
// Se llama tras cada `endFrame`, que es donde el frame acaba de presentarse.
/// Copia y ESPERA antes de leer el mapeo. Ver la nota larga en su definición: no esperar no da error,
/// da datos a medio llenar — y eso se lee como si el shader hubiera calculado mal.
void copyThenWait(Haruka::RHI::BufferHandle src, Haruka::RHI::BufferHandle dst,
                         size_t srcOff, size_t bytes);


void pumpWindowEvents();


// ⚠️ FILA DEL READBACK -> NDC.Y, Y NO ES UN DETALLE: `readPixels` de GL devuelve las filas de ABAJO
// ARRIBA y las de Vulkan de ARRIBA ABAJO (ya estaba escrito en este fichero, en el volcado de PNG del
// agua). Sin esto, cualquier medida que relacione una FILA con un angulo sale con el signo cambiado:
// me costo concluir que `sky.frag` pintaba nubes por DEBAJO del horizonte —a -20 grados— cuando lo
// que pasaba es que yo llamaba "abajo" a la mitad de arriba. Su propia guarda (`t > 0.02`) hacia
// imposible lo que yo estaba midiendo, y esa contradiccion fue la pista.
inline float rowToNdcY(int y, int height, bool flip) {
    const float f = ((float)y + 0.5f) / (float)height;
    return flip ? (2.0f * f - 1.0f) : (1.0f - 2.0f * f);
}

/// ⚠️ Y EL SENTIDO SE CALIBRA CON EL DATO, NO SE SUPONE. Primero lo ate al backend (`isVk`) siguiendo
/// la nota de este fichero sobre `readPixels`, y salio bien en UNO de los dos y espejado en el otro:
/// imprimia que las nubes estaban a -20 grados, bajo el horizonte, donde la propia guarda de
/// `sky.frag` (`t > 0.02`) hace imposible pintarlas. Esa contradiccion fue la pista.
///
/// Aqui se decide mirando la imagen: el SOL esta a una elevacion conocida y positiva, asi que la fila
/// mas brillante tiene que caer arriba. Si con la convencion supuesta cae abajo, se voltea. Es una
/// medida, no una suposicion sobre el backend, y por eso no puede fallar en uno de los dos.
inline bool detectRowFlip(const std::vector<uint8_t>& px, int w, int h) {
    double bestL = -1.0; int bestRow = 0;
    for (int y = 0; y < h; ++y) {
        double acc = 0.0;
        for (int x = 0; x < w; ++x) {
            const size_t k = ((size_t)y * w + x) * 4;
            if (k + 2 >= px.size()) break;
            acc += 0.2126 * px[k] + 0.7152 * px[k+1] + 0.0722 * px[k+2];
        }
        if (acc > bestL) { bestL = acc; bestRow = y; }
    }
    // Sin voltear, la fila 0 es la de ARRIBA. Si la mas brillante (el sol) esta en la mitad de
    // abajo, es que las filas vienen al reves.
    return bestRow > h / 2;
}



// ================================================================================================
// BUG 9 — ¿PUEDE UN COMPUTE MUESTREAR UN `sampler2DArray`? Repro minimo.
//
// `terrain_node.comp` lee el bake del planeta asi y en OpenGL devolvia 0 en los 16 641 texeles
// —terreno SIN CONTINENTES— mientras Vulkan casaba a 0,0007 m. Sobre el pase entero no se podia
// aislar: hay pool, parametros, publicacion y 150 lineas de shader por medio.
//
// ⚠️ Y no es solo el v5: `clipmap.tese` saca el CLIMA (temperatura, humedad) del mismo
// `sampler2DArray`. Si en GL devolviera 0, la seleccion de material saldria degenerada en todo el
// planeta — que es exactamente el sintoma de "verde uniforme" que F5 no consiguio explicar.
// ================================================================================================

// ⚠️ TRAS `copyBuffer` HAY QUE ESPERAR ANTES DE LEER EL MAPEO. Y no esperar no da un error: da datos
// A MEDIO LLENAR, que se leen como si el shader hubiera calculado mal.
//
// Esto costó una sesión entera. Sin esta espera, en OpenGL:
//   · `testTerrainNodeBaseField` leía 0,00 en los 16 641 téxeles -> "en GL el terreno no tiene
//     continentes" (bug 9). FALSO: la generación siempre fue correcta.
//   · `testTerrainNodeRender` daba 399,59 m contra la CPU -> "GL y Vulkan producen suelos distintos".
//     FALSO: el mismo dato leído directo del pool da 0,0242 m, idéntico al camino de control.
// En Vulkan no se notaba, así que parecía un fallo del backend de GL. Era del banco.
void copyThenWait(Haruka::RHI::BufferHandle src, Haruka::RHI::BufferHandle dst,
                         size_t srcOff, size_t bytes);

// --- Tests (cada uno en su archivo; los llama la tabla de grupos de main) ---
void testTextureFormats();
void testBuffers();
void testRenderTargets();
void testArrayAndCube();
void testFrameCycle();
void testBindingPersistence();
void testComputeArraySampler();
void testDispatchInsideRenderPass();
void testTextureContent();
void testVertexColor();
void testVertexInterpolation();
void testGpuFp64();
void testCullWindingWithProjection();
void testEnginePipelines();
void testItemPreviewShader();
void testInstancingRing();
void testSkyAmbientGPU();
void testPropLighting();
void testPropLightSweep();
void testGrassBurstDraw();
void testSceneTerrainAndProp();
void testTerrainNodeGpuParity();
void testTerrainNodeGpuCost();
void testTerrainNodePoolGpu();
void testTerrainNodeRendererInit();
void testTerrainNodeBaseField();
void testNodeGpuAdapterBisect();
void testBakeStageBisect();
void testTerrainBakeAcrossLevels();
void testOctaveDivergence();
void testTerrainNodeRender();
void testTerrainNodeDrawnVsField();
void testTerrainStrideMatchCost();
void testStrideRingsOnLand();
void testFrameDrawnVsBaked();
void testHorizonPlateau();
void testTerrainNodeCoverage();
void testTerrainNodeSeamHoles();
void testTerrainNodeSharedEdgeGpu();
void testTerrainNodeStitchSymmetryGpu();
void testTerrainNodeSceneSeams();
void testTerrainNodeShadeCost();
void testTerrainLighting();
void testTerrainShadow();
void testNodeVoxCut();
void testNodeVoxCutFine();
void testVoxFineBricks();
void testVoxNoCrack();
void testWaterFieldParity();
void testWaveNumberParity();
void testNodeWaterDraws();
void testLakeShoreline();
void testOceanWaveParity();
void testOceanFoldParity();
void testWaterShapes();
void testSoftBodyMesh();
void testCloudPassDraws();
void testCloudPassOrbit();
void testCloudHighLayers();
void testCloudPassSceneDepth();
void testCloudFlatTop();
void testCloudMotionFrames();
void testCloudMarchConvergence();
void testCloudPassCost();
void testCloudCoverageByDirection();
void testCloudOrbitView();
void testCloudFieldDistribution();
void testCloudShape3D();
void testCloudColumnParity();
void testCloudNoiseParity();
void testSkyLayersHaveDepth();
void testAtmosphereLimb();
