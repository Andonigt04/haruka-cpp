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
# `tests/` refleja `src/Haruka/`: cada carpeta es un GRUPO del banco y un test de CTest. Las fuentes se
# recogen por glob (un test nuevo en su carpeta entra solo, y se registra solo: HARUKA_REGISTER_TESTS).
# Fuera: los binarios propios (RHI con ventana, el DGS vivo y el piloto de módulos).
file(GLOB_RECURSE HARUKA_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp")
list(FILTER HARUKA_TEST_SOURCES EXCLUDE REGEX "/tests/(RHI|Shaders|Data)/")
# `*GPU.cpp` son del banco de GPU (haruka_tests_rhi): viven junto a los de CPU de su módulo.
list(FILTER HARUKA_TEST_SOURCES EXCLUDE REGEX "GPU\\.cpp$")
list(FILTER HARUKA_TEST_SOURCES EXCLUDE REGEX "/tests/Network/DGSLiveTest\\.cpp$")
list(FILTER HARUKA_TEST_SOURCES EXCLUDE REGEX "/tests/Core/Jobs/ModulePilot[^/]*\\.cpp$")
add_executable(haruka_tests ${HARUKA_TEST_SOURCES})
target_include_directories(haruka_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
target_link_libraries(haruka_tests PRIVATE HarukaEngine)
# TestDGS.cpp incluye el ABI del DGS (external/dgs/include/dgs/game_module.h). El include no siempre
# se propaga desde HarukaEngine (solo con HARUKA_NETWORK), así que se lo damos al test directamente.
target_include_directories(haruka_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/external/dgs")
# El módulo de reglas que TestDGS carga con dlopen: se construye con el banco y se le pasa su ruta
# absoluta, así el banco no depende del directorio desde el que se lance.
add_dependencies(haruka_tests haruka_rules)
target_compile_definitions(haruka_tests PRIVATE HARUKA_RULES_LIB="$<TARGET_FILE:haruka_rules>")
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
    HARUKA_TEST_ASSET_DIR="${HARUKA_TESTS_ASSET_ROOT}/"
    HARUKA_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/Data/")   # ficheros de prueba versionados (p. ej. el .glb de Blender)
set_target_properties(haruka_tests PROPERTIES
    BUILD_RPATH   "$ORIGIN"
    INSTALL_RPATH "$ORIGIN")
enable_testing()
# Un test de CTest por CARPETA de tests/ (= grupo del banco): `ctest -R World.Terrain`, `ctest -L cpu -j8`,
# `ctest --rerun-failed`. Etiquetas: `cpu` (todo este banco es headless) y el módulo de primer nivel en
# minúsculas (`world`, `physics`...). Si una carpeta se queda sin tests, su test FALLA (--group lo exige).
set(_haruka_test_groups "")
foreach(_src ${HARUKA_TEST_SOURCES})
    file(RELATIVE_PATH _rel "${CMAKE_CURRENT_SOURCE_DIR}/tests" "${_src}")
    get_filename_component(_dir "${_rel}" DIRECTORY)
    if(_dir AND NOT _dir STREQUAL "Common")
        list(APPEND _haruka_test_groups "${_dir}")
    endif()
endforeach()
list(REMOVE_DUPLICATES _haruka_test_groups)
foreach(_g ${_haruka_test_groups})
    string(REPLACE "/" "." _name "${_g}")
    string(REGEX REPLACE "/.*" "" _top "${_g}")
    string(TOLOWER "${_top}" _top)
    add_test(NAME ${_name} COMMAND haruka_tests --group ${_g})
    set_tests_properties(${_name} PROPERTIES LABELS "cpu;${_top}" TIMEOUT 1200)
endforeach()

# --- 6a-bis-2. haruka_tests_rhi: banco ABIERTO del RHI (GL + Vulkan, requiere VENTANA) -------
# A diferencia de haruka_tests (headless, CPU), esto abre una SDL_Window y crea un device real
# con swapchain, así que SOLO se lanza a mano o en un runner con display:
#   ./bin/haruka_tests_rhi all    (GL + Vulkan, el que exista)
#   ./bin/haruka_tests_rhi vk     (solo Vulkan)
# Cubre: textura por formato, buffers, render targets (incl. depth-as-texture / MRT), array &
# cubemaps & samplers, y un ciclo completo del frame (acquire->clear->present: la cadena que
# daba TDR en NVIDIA). Si no hay display, SDL_CreateWindow falla y el test se salta con "skip".
# El banco de GPU: `tests/RHI/` (main, tabla de grupos y lo común) + los `*GPU.cpp` de cada módulo
# (`tests/World/Water/TestWaterGPU.cpp`...), que viven junto a los tests de CPU de lo mismo.
file(GLOB_RECURSE HARUKA_RHI_TEST_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/RHI/*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/tests/*GPU.cpp")
add_executable(haruka_tests_rhi ${HARUKA_RHI_TEST_SOURCES})
target_include_directories(haruka_tests_rhi PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
target_link_libraries(haruka_tests_rhi PRIVATE HarukaEngine)

# Shaders SOLO de test (rhitest_* y las sondas *_probe): viven en tests/Shaders/ y se compilan aquí, no
# con los del motor, así que un build sin tests no los compila ni los instala. Salen a
# `<shaders>/tests/` (el banco los pide como "shaders/tests/<nombre>"). Sus `#include "lib/..."` se
# resuelven desde la raíz de shaders —con -I al compilar y con `shaderIncludeDir()` en tiempo de
# ejecución—, no desde su carpeta, por eso pueden vivir aquí.
file(GLOB HARUKA_TEST_SHADERS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/Shaders/*.vert" "${CMAKE_CURRENT_SOURCE_DIR}/tests/Shaders/*.frag"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/Shaders/*.comp")
set(HARUKA_TEST_SPIRV "")
foreach(SRC ${HARUKA_TEST_SHADERS})
    get_filename_component(NAME ${SRC} NAME)
    set(SPV "${SHADER_OUT_DIR}/tests/${NAME}.spv")
    if(GLSLC)
        set(_compile ${GLSLC} --target-env=opengl -I${SHADER_SRC_DIR} ${SRC} -o ${SPV})
    else()
        set(_compile ${GLSLANG_V} -G -I${SHADER_SRC_DIR} ${SRC} -o ${SPV})
    endif()
    add_custom_command(OUTPUT ${SPV}
        COMMAND ${CMAKE_COMMAND} -E make_directory "${SHADER_OUT_DIR}/tests"
        COMMAND ${_compile}
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SRC} "${SHADER_OUT_DIR}/tests/${NAME}"
        DEPENDS ${SRC} ${SHADER_LIBS}
        COMMENT "glsl (test)  tests/${NAME} -> tests/${NAME}.spv"
        VERBATIM)
    list(APPEND HARUKA_TEST_SPIRV ${SPV})
endforeach()
add_custom_target(haruka_test_shaders DEPENDS ${HARUKA_TEST_SPIRV})
add_dependencies(haruka_tests_rhi haruka_test_shaders)
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
    LABELS "gpu;rhi;slow"
    ENVIRONMENT "SDL_VIDEODRIVER=offscreen;__EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/50_mesa.json;LIBGL_ALWAYS_SOFTWARE=1;GALLIUM_DRIVER=llvmpipe;HARUKA_REQUIRE_DEVICE=llvmpipe;HARUKA_NO_VSYNC=1"
    TIMEOUT 1800)

add_test(NAME haruka_tests_rhi_vk COMMAND haruka_tests_rhi vk)
set_tests_properties(haruka_tests_rhi_vk PROPERTIES
    LABELS "gpu;rhi;slow"
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
    add_executable(haruka_dgs_live tests/Network/DGSLiveTest.cpp)
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
        LABELS "network;live"
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

    add_executable(haruka_module_pilot tests/Core/Jobs/ModulePilot.cpp)
    target_link_libraries(haruka_module_pilot PRIVATE haruka_jobs_module)
    set_target_properties(haruka_module_pilot PROPERTIES CXX_SCAN_FOR_MODULES ON)
    add_test(NAME haruka_module_pilot COMMAND haruka_module_pilot)
    set_tests_properties(haruka_module_pilot PROPERTIES LABELS "cpu;modules")

    # CONTRAPRUEBA: usar algo NO exportado tiene que fallar al compilar.
    add_executable(haruka_module_pilot_hidden EXCLUDE_FROM_ALL tests/Core/Jobs/ModulePilotHidden.cpp)
    target_link_libraries(haruka_module_pilot_hidden PRIVATE haruka_jobs_module)
    set_target_properties(haruka_module_pilot_hidden PROPERTIES CXX_SCAN_FOR_MODULES ON)
    add_test(NAME haruka_module_hidden
             COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target haruka_module_pilot_hidden)
    set_tests_properties(haruka_module_hidden PROPERTIES WILL_FAIL TRUE LABELS "cpu;modules")
else()
    message(STATUS "Piloto de modulos C++20: solo con Ninja (generador actual: ${CMAKE_GENERATOR})")
endif()
