# ================================================================================================
# Bancos de pruebas: haruka_tests (CPU), haruka_tests_rhi (GL+VK), el cruzado del DGS y bench_dgs_bandwidth
# ================================================================================================
# Solo se incluye con HARUKA_BUILD_TESTS (ver HarukaBuildFlags.cmake): en release nada de esto existe.

# --- 6a-bis. bench_dgs_bandwidth: test de estrés de transferencia entre nodos DGS (sin .a, sin
# sockets). Modela el límite BDP (min(enlace, ventana/RTT)) y estresa el PacketFramer real (bug 6)
# con el volumen completo. Header-only → compila en CI; `--fast` para verificación rápida. ---
add_executable(bench_dgs_bandwidth tools/BenchDGSBandwidth.cpp)
target_include_directories(bench_dgs_bandwidth PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/external/dgs")

# (el if(HARUKA_BUILD_TESTS) lo pone quien incluye este fichero)
# --- 6a-bis. haruka_tests: banco de pruebas HEADLESS (LOD/sampler/órbitas, sin ventana/GL) ---
# Subsistemas puros-CPU dirigidos con entrada sintética + verificación de invariantes → depurar a
# pequeña escala, determinista y rápido, sin cargar un planeta entero. `ctest` o `./bin/haruka_tests`.
add_executable(haruka_tests
    tests/HarukaTests.cpp      # main() — selecciona/dirige
    tests/TestCommon.cpp       # framework de aserciones compartido
    tests/TestJobs.cpp         # pool central de trabajos (Core/Jobs)
    tests/TestResourceStore.cpp # almacen de recursos CPU<->GPU (Renderer/ResourceStore)
    tests/TestTerrain.cpp      # cube-sphere / climate / weather fronts / ground layer (CPU)
    tests/TestPhysics.cpp      # caída radial + determinismo (sin GL)
    tests/TestXPBD.cpp         # enganche de tela a un hueso (piel/membranas de criaturas)
    tests/TestConstruction.cpp # sistema de construcción: validación de colocación F1 (sin GL)
    tests/TestPorts.cpp        # puertos autorizados + raíles (mecanismos, no animaciones)
    tests/TestPrefab.cpp       # prefabricados: los cambios de UN conjunto no tocan el montaje
    tests/TestPrimitives.cpp   # primitivas: ninguna cara bobinada al revés (una tapa invertida no da error, desaparece)
    tests/TestDGS.cpp
    tests/TestSimplePlanet.cpp         # SimplePlanet mesh & config
    tests/TestTerrainQuality.cpp       # terrain quality → resolution mappings
    tests/TestInput.cpp                 # acciones de entrada: teclado + mando en la misma accion
    tests/TestDisplay.cpp               # selector de monitor: re-enumerar al enchufar, indice por pantalla
    tests/TestSound.cpp                 # sonido: sintesis + puntos logicos agrupados (sin OpenAL)
    tests/TestSkyAmbient.cpp           # ambiente SH: paridad con sky_palette.glsl + simetría
    tests/TestProcGraph.cpp             # ProcGraph nodes, BiomeConfig, RGBAImage
    tests/TestOrbit.cpp                 # órbitas Kepler con elementos precesantes
    tests/TestImageWriter.cpp          # PNG: ida y vuelta + compresión DEFLATE
    tests/TestPropLOD.cpp              # LOD de malla de props: envolvente estable
    tests/TestPropCollider.cpp         # colliders de props por PARTE (esqueleto del arbol)
    tests/TestRockInterior.cpp         # penones ENTERRADOS de la roca: no se ven (rayos + contraprueba)
    tests/TestCaves.cpp                 # cuevas: mapas en disco, conectividad garantizada, campo
    tests/TestVoxWorld.cpp             # el mundo por chunks: sin costura, trazos de la partida
    tests/TestIslands.cpp               # islas flotantes: roca AÑADIDA al mismo campo
    tests/TestWeatherSevere.cpp        # tiempo adverso: severidad por condiciones + vortices (Rankine)
    tests/TestCloudMotion.cpp          # frame a frame: la deriva integrada y el fundido de horneados (sin tirones)
    tests/TestCloudFormation.cpp       # el UNICO test de formacion: la columna de aire (CloudColumn.h)
    tests/TestOcean.cpp                 # el mar: derivada de la ola, transporte, bajio y flotacion
    tests/TestShallowWater.cpp         # agua interior: estabilidad al re-anclar y cobertura en montana
    tests/TestTerrainNode.cpp)         # v5 F1: direccionamiento de nodos del quadtree (bit-exacto)
target_link_libraries(haruka_tests PRIVATE HarukaEngine)
# TestDGS.cpp incluye el ABI del DGS (external/dgs/include/dgs/game_module.h). El include no siempre
# se propaga desde HarukaEngine (solo con HARUKA_NETWORK), así que se lo damos al test directamente.
target_include_directories(haruka_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/external/dgs")
# RAÍZ DE ASSETS DEL BANCO, absoluta y fijada aquí (ver GLContext en test_gl_fixture.h). El banco se
# construye también dentro del árbol del JUEGO (FetchContent), donde NO hay assets al lado del
# ejecutable: sin esto, ese binario corría sin shaders — terreno GPU deshabilitado — y daba números
# incomparables con los del motor, con parte de la suite en verde igualmente.
# ⚠️ Se DERIVA de SHADER_OUT_DIR, que es la variable que ya decide dónde acaban los shaders — y que
# el JUEGO redefine (`<build>/bin/assets/shaders`). Escribir aquí una ruta propia funcionaba en el
# árbol del motor y apuntaba a un directorio INEXISTENTE en el del juego: mismo síntoma que se venía
# a arreglar, con la ruta ahora absoluta y equivocada en vez de relativa y equivocada.
# ⚠️ Y es el dir de BUILD, no el de fuente: el build compila los shaders a SPIR-V (`*.spv`) y el RHI
# los prefiere; apuntando al fuente el banco pasaría en silencio a la ruta GLSL — otro backend, no
# otro directorio (probado: un test de lluvia bajo cubierto cambiaba de resultado).
get_filename_component(HARUKA_TESTS_ASSET_ROOT "${SHADER_OUT_DIR}" DIRECTORY)
target_compile_definitions(haruka_tests PRIVATE
    HARUKA_TEST_ASSET_DIR="${HARUKA_TESTS_ASSET_ROOT}/")
set_target_properties(haruka_tests PROPERTIES
    BUILD_RPATH   "$ORIGIN"
    INSTALL_RPATH "$ORIGIN")
enable_testing()
add_test(NAME haruka_tests COMMAND haruka_tests)

# --- 6a-bis-2. haruka_tests_rhi: banco ABIERTO del RHI (GL + Vulkan, requiere VENTANA) -------
# A diferencia de haruka_tests (headless, CPU), esto abre una SDL_Window y crea un device real
# con swapchain, así que SOLO se lanza a mano o en un runner con display:
#   ./bin/haruka_tests_rhi all    (GL + Vulkan, el que exista)
#   ./bin/haruka_tests_rhi vk     (solo Vulkan)
# Cubre: textura por formato, buffers, render targets (incl. depth-as-texture / MRT), array &
# cubemaps & samplers, y un ciclo completo del frame (acquire->clear->present: la cadena que
# daba TDR en NVIDIA). Si no hay display, SDL_CreateWindow falla y el test se salta con "skip".
add_executable(haruka_tests_rhi
    tests/RHITestMain.cpp)
target_link_libraries(haruka_tests_rhi PRIVATE HarukaEngine)
set_target_properties(haruka_tests_rhi PROPERTIES
    BUILD_RPATH   "$ORIGIN"
    INSTALL_RPATH "$ORIGIN")

# Registrado en CTest, pero SIN display: el driver de vídeo `offscreen` de SDL más el rasterizador
# software de Mesa bastan para la suite entera, en los dos backends. Eso es lo que hace que el banco
# de GPU pueda correr en CI y no solo en la máquina de uno. Los fallos de fp64 del rasterizador
# software van declarados en `g_driverDefects` (RHITestMain.cpp) y salen como XFAIL.
#
# ⚠️ `LIBGL_ALWAYS_SOFTWARE=1` NO BASTA PARA ELEGIR SOFTWARE, y costó una medida entera darse cuenta:
# en un equipo con NVIDIA, `/usr/share/glvnd/egl_vendor.d/10_nvidia.json` ordena antes que
# `50_mesa.json`, gana el vendor EGL de NVIDIA y esa variable —que es de Mesa— no hace nada. La tanda
# corre en la GPU dedicada mientras uno cree estar midiendo el rasterizador software. Por eso van las
# DOS cosas: `__EGL_VENDOR_LIBRARY_FILENAMES` para forzarlo, y `HARUKA_REQUIRE_DEVICE` para que el
# banco FALLE si aun así acabó en otro sitio.
add_test(NAME haruka_tests_rhi_gl COMMAND haruka_tests_rhi gl)
set_tests_properties(haruka_tests_rhi_gl PROPERTIES
    ENVIRONMENT "SDL_VIDEODRIVER=offscreen;__EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/50_mesa.json;LIBGL_ALWAYS_SOFTWARE=1;GALLIUM_DRIVER=llvmpipe;HARUKA_REQUIRE_DEVICE=llvmpipe;HARUKA_NO_VSYNC=1"
    TIMEOUT 1800)

add_test(NAME haruka_tests_rhi_vk COMMAND haruka_tests_rhi vk)
set_tests_properties(haruka_tests_rhi_vk PROPERTIES
    ENVIRONMENT "SDL_VIDEODRIVER=offscreen;VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json;HARUKA_REQUIRE_DEVICE=llvmpipe;HARUKA_NO_VSYNC=1"
    TIMEOUT 1800)

# ── Test CRUZADO: modulo REAL + terreno REAL + `validador_node` VIVO ─────────────────────────────
# Cierra el hueco entre dos tests que ya existian y que, por separado, no prueban lo que protege al
# juego: `TestDGS.cpp` prueba la REGLA en proceso, y `dgs/tests/validator_e2e.cpp` prueba el NODO con
# un modulo de juguete. Entre medias se puede romper que el nodo cargue el `.so`, que le llegue el
# campo de altura, y que el `.hfield` del bake sea el que el modulo sabe leer.
#
# ⚠️ SOLO SE REGISTRA SI EL NODO ESTA CONSTRUIDO. El proyecto de red es OTRO repositorio a proposito,
# asi que aqui no se inventa un verde cuando falta: o corre de verdad, o el test no existe.
find_program(HARUKA_DGS_VALIDADOR validador_node
             PATHS "${CMAKE_CURRENT_SOURCE_DIR}/../dgs/build" NO_DEFAULT_PATH)
# ⚠️ Se enlaza la biblioteca RECIEN construida del repo hermano, NO `external/dgs/sdk/libdgs_client.a`,
# y ahora es a proposito y no por avería. El vendorizado estaba desactualizado (le faltaban
# `Packet::pack(ValidateRequest)` y `unpackValidateAck`, justo los del camino de validacion); se ha
# refrescado con `cmake --install` desde el repo hermano y ya los trae. Pero este test existe para
# cazar la DIVERGENCIA entre los dos proyectos: si enlazara la copia, compararia el vendorizado
# consigo mismo y no notaria nada. Enlazando el original, el dia que las dos versiones se separen
# este test es el que lo dice.
find_library(HARUKA_DGS_CLIENT_LIB dgs_client
             PATHS "${CMAKE_CURRENT_SOURCE_DIR}/../dgs/build" NO_DEFAULT_PATH)
if (HARUKA_DGS_VALIDADOR AND HARUKA_DGS_CLIENT_LIB)
    add_executable(haruka_dgs_live tests/DGSLiveTest.cpp)
    target_include_directories(haruka_dgs_live PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/../dgs"
        "${CMAKE_CURRENT_SOURCE_DIR}/../dgs/include")
    target_link_libraries(haruka_dgs_live PRIVATE
        HarukaEngine
        "${HARUKA_DGS_CLIENT_LIB}"
        OpenSSL::SSL OpenSSL::Crypto)
    set_target_properties(haruka_dgs_live PROPERTIES BUILD_RPATH "$ORIGIN" INSTALL_RPATH "$ORIGIN")
    add_dependencies(haruka_dgs_live haruka_rules)

    add_test(NAME dgs_live_terrain COMMAND haruka_dgs_live)
    set_tests_properties(dgs_live_terrain PROPERTIES
        ENVIRONMENT "HARUKA_DGS_VALIDADOR=${HARUKA_DGS_VALIDADOR}"
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
        RUN_SERIAL TRUE TIMEOUT 120)
    message(STATUS "DGS vivo: test cruzado ACTIVADO (${HARUKA_DGS_VALIDADOR})")
else()
    message(STATUS "DGS vivo: sin validador_node -> el test cruzado NO se registra")
endif()


# --- PILOTO DE MÓDULOS C++20 (Haruka.Jobs) -----------------------------------------------------------
# Mide si `import` funciona aquí antes de decidir nada para el motor. AISLADO: ni el motor ni el juego
# lo usan, y Survival/el IDE (Makefiles) no lo ven — CMake solo construye módulos con Ninja.
# `cmake_minimum_required` es 3.14, así que la política de escaneo de módulos (CMP0155) va en OLD: se
# activa por objetivo con CXX_SCAN_FOR_MODULES.
if(CMAKE_GENERATOR MATCHES "Ninja")
    add_library(haruka_jobs_module STATIC)
    target_sources(haruka_jobs_module PUBLIC
        FILE_SET haruka_jobs_mod TYPE CXX_MODULES FILES src/Haruka/Core/Jobs/Jobs.cppm)
    target_link_libraries(haruka_jobs_module PUBLIC HarukaEngine)
    set_target_properties(haruka_jobs_module PROPERTIES CXX_SCAN_FOR_MODULES ON)

    add_executable(haruka_module_pilot tests/ModulePilot.cpp)
    target_link_libraries(haruka_module_pilot PRIVATE haruka_jobs_module)
    set_target_properties(haruka_module_pilot PROPERTIES CXX_SCAN_FOR_MODULES ON)
    add_test(NAME haruka_module_pilot COMMAND haruka_module_pilot)

    # CONTRAPRUEBA: usar algo NO exportado tiene que fallar al compilar.
    add_executable(haruka_module_pilot_hidden EXCLUDE_FROM_ALL tests/ModulePilotHidden.cpp)
    target_link_libraries(haruka_module_pilot_hidden PRIVATE haruka_jobs_module)
    set_target_properties(haruka_module_pilot_hidden PROPERTIES CXX_SCAN_FOR_MODULES ON)
    add_test(NAME haruka_module_hidden
             COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target haruka_module_pilot_hidden)
    set_tests_properties(haruka_module_hidden PROPERTIES WILL_FAIL TRUE)
else()
    message(STATUS "Piloto de modulos C++20: solo con Ninja (generador actual: ${CMAKE_GENERATOR})")
endif()
