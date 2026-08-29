cmake_minimum_required(VERSION 3.14)

if(DEFINED ENV{GITHUB_RUN_NUMBER})
    set(HARUKA_BUILD_NUMBER $ENV{GITHUB_RUN_NUMBER})
else()
    set(HARUKA_BUILD_NUMBER "local")
endif()

set(VERSION "1.0.0" CACHE STRING "Engine version (used as build/install subdirectory)")

project(HarukaEngine VERSION ${VERSION})

# --- version.h -----------------------------------------------------------------------------------
# ⚠️ Esto va DESPUES de project() a proposito. Antes se generaba en la linea 21, o sea ANTES de que
# project() definiera PROJECT_VERSION — asi que `HARUKA_VERSION` no cogia la version del MOTOR sino la
# del proyecto padre: compilado dentro de Survival decia "0.2" (la de Survival) y compilado suelto
# decia "" (vacio, porque aun no habia ninguna). Y como no lo incluia nadie, nunca se noto.
#
# El hash, ademas, se capturaba al CONFIGURAR, asi que entre commit y commit el binario seguia
# diciendo el anterior. Ahora lo regenera un target en cada compilacion (ver cmake/version_gen.cmake,
# que solo reescribe el fichero si cambia para no arrastrar recompilaciones).
set(HARUKA_VERSION_IN  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.h.in")
set(HARUKA_VERSION_OUT "${CMAKE_CURRENT_BINARY_DIR}/generated/version.h")
set(HARUKA_VERSION_ARGS
    -DSRC_DIR=${CMAKE_CURRENT_SOURCE_DIR}
    -DIN_FILE=${HARUKA_VERSION_IN}
    -DOUT_FILE=${HARUKA_VERSION_OUT}
    -DPROJECT_VERSION=${PROJECT_VERSION}
    -DPROJECT_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    -DPROJECT_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    -DPROJECT_VERSION_PATCH=${PROJECT_VERSION_PATCH}
    -DHARUKA_BUILD_NUMBER=${HARUKA_BUILD_NUMBER}
)
# Una pasada al configurar para que el fichero EXISTA antes de la primera compilacion.
execute_process(COMMAND ${CMAKE_COMMAND} ${HARUKA_VERSION_ARGS} -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/version_gen.cmake")
add_custom_target(haruka_version ALL
    COMMAND ${CMAKE_COMMAND} ${HARUKA_VERSION_ARGS} -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/version_gen.cmake"
    COMMENT "version.h (hash de git del momento)"
)

set(OpenGL_GL_PREFERENCE GLVND)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ⚠️ SIN ESTO EL MOTOR SE COMPILA A -O0, Y NADIE LO SABÍA.
#
# `CMAKE_CXX_FLAGS_RELEASE` estaba definido abajo desde hace tiempo, pero `CMAKE_BUILD_TYPE` no se
# fijaba en NINGÚN sitio (ni aquí, ni en el juego, ni en `build.sh`, que solo pasa `-DRELEASE=ON` —
# y esa opción cambia el LAYOUT DE ASSETS, no la optimización). Resultado: todos los binarios salían
# sin `-O`, incluidos los de `./build.sh release`.
#
# Lo que costaba, medido con el mismo escenario y la misma sonda:
#     fluid.render.lamina  5,80 ms -> 0,28 ms   (21x)
#     post.composite       0,142   -> 0,003     (47x)
#     shadow.pass          2,31    -> 0,40      (5,8x)
#     barrido de props     584 ns/inst -> 35    (17x)
# O sea que cualquier juicio de rendimiento hecho hasta hoy estaba sesgado por ese factor, y varias
# "optimizaciones necesarias" se evaporan al compilar bien.
#
# Por defecto RelWithDebInfo y no Release: es -O2 CON símbolos, así que el build de todos los días
# es rápido y además se puede depurar y perfilar. `Release` (-O3 -flto) queda para el empaquetado,
# que es donde el coste de compilar con LTO tiene sentido. La CPU objetivo la fija `HARUKA_ARCH`
# (ver abajo) y es la MISMA en los dos tipos: portable por defecto.
get_property(_haruka_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT _haruka_multi_config AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Tipo de build (vacío = -O0, ver nota)" FORCE)
    message(STATUS "CMAKE_BUILD_TYPE vacío -> RelWithDebInfo (-O2 -g). Antes se compilaba a -O0.")
endif()

if(NOT MSVC)
    # ⚠️ SIN `-ffast-math`, Y CON `-ffp-contract=off` GLOBAL. Medido, no supuesto: con
    # `-ffast-math` los tests de paridad FALLAN en Release —
    #     terrain_chord_error: "lo que se dibuja y lo que se pisa es la MISMA superficie (0 m)"
    #     terrain_detail_gradient: "devuelve EXACTAMENTE la misma altura que terrainDetail"
    # — que es justo lo que la nota de abajo predecía que pasaría.
    #
    # El blindaje por FICHERO era insuficiente por un motivo estructural: la aritmética del terreno
    # vive en CABECERAS (`terrain_detail.h`, `terrain_lod.h`), así que se compila dentro de cada
    # unidad que las incluya. Marcar tres `.cpp` no puede cubrirla.
    #
    # Y `-ffp-contract` en GCC vale `fast` POR DEFECTO desde -O2: la fusión `a*b+c` -> FMA ocurriría
    # aunque no hubiera `-ffast-math`. Por eso va explícito y global, no solo en Release.
    #
    # Lo que se pierde es reasociación y vectorización agresiva de coma flotante. Lo que se gana es
    # que el suelo que se dibuja y el que se pisa sigan siendo el mismo en la build que juega el
    # usuario — que es la propiedad central del proyecto, no un detalle.
    set(CMAKE_CXX_FLAGS_RELEASE "-O3 -flto")
    add_compile_options(-ffp-contract=off)

    # ── PARA QUÉ CPU SE COMPILA ─────────────────────────────────────────────────────────────────
    #
    # ⚠️ AQUÍ PONÍA `-march=native`, y eso produce un binario que SOLO ARRANCA EN ESTA MÁQUINA.
    # `native` en este equipo resuelve a `znver3` (Ryzen 7 5800H) y exige AVX2, FMA, BMI2, F16C. En
    # una CPU sin ellas no da un error legible: muere con SIGILL («instrucción ilegal») al ejecutar
    # la primera. Para `build/` daba igual —se compila y se ejecuta en el mismo sitio— pero
    # `./build.sh release` empaqueta en `dist/`, que es justo lo que se reparte.
    #
    # `x86-64-v3` es el nivel base que exige AVX2+FMA (Haswell/Excavator, 2013 en adelante): conserva
    # el grueso de la ganancia y arranca en cualquier equipo de la última década. El ajuste fino de
    # latencias se recupera con `-mtune=native`, que REORDENA instrucciones sin exigir nada nuevo al
    # hardware — o sea, gratis en portabilidad.
    #
    # `-DHARUKA_ARCH=native` para una build local a máxima velocidad, a sabiendas de que no es
    # distribuible. `-DHARUKA_ARCH=x86-64-v2` si hiciera falta llegar a máquinas de ~2009.
    #
    # Va con `add_compile_options` y no solo en los flags de Release para que el build de todos los
    # días (RelWithDebInfo) también aproveche AVX2: sin esto se compilaba al x86-64 base, o sea SSE2.
    set(HARUKA_ARCH "x86-64-v3" CACHE STRING
        "Nivel de CPU objetivo: x86-64-v3 (portable, AVX2) | native (solo esta maquina) | x86-64-v2")
    include(CheckCXXCompilerFlag)
    check_cxx_compiler_flag("-march=${HARUKA_ARCH}" HARUKA_ARCH_OK)
    if(HARUKA_ARCH_OK)
        add_compile_options(-march=${HARUKA_ARCH})
        if(NOT HARUKA_ARCH STREQUAL "native")
            add_compile_options(-mtune=native)   # solo ordena; no añade instrucciones exigidas
        endif()
        message(STATUS "CPU objetivo: -march=${HARUKA_ARCH}")
    else()
        message(WARNING "El compilador no acepta -march=${HARUKA_ARCH}; se compila al x86-64 base")
    endif()

    # ⚠️ EL CAMINO DE ALTURA DEL TERRENO SE COMPILA SIN fast-math. No es purismo: el invariante
    # render↔colisión es de 1 cm, y se cumple porque la CPU y el shader evalúan la MISMA fórmula con
    # la MISMA aritmética (ver terrain_gen.comp y `haruka_tests paridad`). `-ffast-math` autoriza a
    # GCC a reasociar sumas y —con `-march=native`, que sí tiene FMA— a fundir `a*b + c` en un FMA.
    # Las dos cosas cambian el resultado en ulps, y en el terreno un ulp de la dirección son
    # decímetros de superficie: la paridad se rompería SOLO en la build de Release, o sea justo en la
    # que juega el usuario, y los tests (que se compilan sin BUILD_TYPE) seguirían en verde.
    # Coste: estos ficheros pierden vectorización agresiva; el grueso del tiempo de generación está en
    # la GPU, no aquí. Si algún día hace falta, se mide antes de tocarlo.
    set_source_files_properties(
        "${CMAKE_CURRENT_SOURCE_DIR}/src/core/noise_generator.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/core/terrain/planet_geology.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/core/terrain/cube_sphere.cpp"
        DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        PROPERTIES COMPILE_OPTIONS "-fno-fast-math;-ffp-contract=off")
endif()

# Sanitizadores (diagnóstico, apagado por defecto). Para cazar QUIÉN pisa memoria ajena o qué acceso
# entre hilos no está sincronizado, en vez de deducirlo del volcado: el sanitizador señala la línea.
#   cmake -B build-asan -DHARUKA_SANITIZE=address   → escrituras fuera de rango, uso tras liberar
#   cmake -B build-tsan -DHARUKA_SANITIZE=thread    → carreras de datos
# Van en un directorio de build APARTE: no se mezclan con el build normal.
set(HARUKA_SANITIZE "" CACHE STRING "Sanitizador: vacío, 'address' o 'thread'")
if(HARUKA_SANITIZE AND NOT MSVC)
    message(STATUS "Sanitizador ACTIVO: ${HARUKA_SANITIZE} (build de diagnóstico, más lento)")
    add_compile_options(-fsanitize=${HARUKA_SANITIZE} -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=${HARUKA_SANITIZE})
endif()

set(THIRD_PARTY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party")

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

# --- 1. System Dependencies ---
find_package(PkgConfig   REQUIRED)
find_package(SDL3        REQUIRED)
find_package(OpenGL      REQUIRED)
find_package(assimp      REQUIRED)
find_package(OpenSSL     REQUIRED)
find_package(OpenAL      REQUIRED)
find_package(PostgreSQL  REQUIRED)
pkg_check_modules(GTK3 REQUIRED IMPORTED_TARGET gtk+-3.0)

# --- 2. Header-Only Fallback Macro ---
macro(find_header_fallback NAME HEADER_FILE SUBDIR)
    find_path(${NAME}_INCLUDE_DIR ${HEADER_FILE}
        PATH_SUFFIXES ${SUBDIR}
        PATHS ${THIRD_PARTY_DIR}/${SUBDIR}
    )
    if(${NAME}_INCLUDE_DIR)
        message(STATUS "Found ${NAME}: ${${NAME}_INCLUDE_DIR}")
        include_directories(${${NAME}_INCLUDE_DIR})
    else()
        message(FATAL_ERROR "${NAME} not found. Install the -devel package or place it in third_party/")
    endif()
endmacro()

find_header_fallback(NLOHMANN_JSON "nlohmann/json.hpp" "json/include")
find_header_fallback(GLM           "glm/glm.hpp"       "glm")
find_header_fallback(STB           "stb_image.h"       "stb")

# --- 3. Sources ---
set(GLAD_SOURCE "${THIRD_PARTY_DIR}/glad/src/glad.c")

set(IMGUI_SOURCES
    ${THIRD_PARTY_DIR}/imgui/imgui.cpp
    ${THIRD_PARTY_DIR}/imgui/imgui_draw.cpp
    ${THIRD_PARTY_DIR}/imgui/imgui_tables.cpp
    ${THIRD_PARTY_DIR}/imgui/imgui_widgets.cpp
    ${THIRD_PARTY_DIR}/imgui/imgui_demo.cpp
    ${THIRD_PARTY_DIR}/imgui/backends/imgui_impl_sdl3.cpp
    ${THIRD_PARTY_DIR}/imgui/backends/imgui_impl_opengl3.cpp
    ${THIRD_PARTY_DIR}/imgui/backends/imgui_impl_vulkan.cpp
)

set(NFD_SOURCES
    ${THIRD_PARTY_DIR}/nativefiledialog/src/nfd_common.c
    ${THIRD_PARTY_DIR}/nativefiledialog/src/nfd_gtk.c
)

# --- 3b. Build-time modules (MODULES=1011...) ---
# Bitmask string, one char per module, in this FIXED order. '1'=on, '0'=off.
# Absent/short string → missing positions default ON. See docs/MODULES.md.
set(HARUKA_MODULE_LIST
    FLUIDS      # 0: ocean + shallow-water + PBF + hybrid
    DEFORM      # 1: terrain deformation field
    SOFTBODY    # 2: XPBD cloth/jelly/wind
    NETWORK     # 3: DGS client
    AUDIO       # 4: audio manager
    PARTICLES   # 5: reserved (particle system)
    TERRAIN     # 6: planetary terrain (default-ON; off = no planet)
    POSTFX      # 7: bloom/ssao/hdr/ibl
    SHADOWS     # 8: shadow maps
    PHYSICS     # 9: rigid-body physics engine + colliders (default-ON; off = no PhysicsEngine)
)
set(MODULES "" CACHE STRING "Per-module on/off bitmask, e.g. 101101111")

list(LENGTH HARUKA_MODULE_LIST _mod_count)
set(_module_defines "")
math(EXPR _last "${_mod_count} - 1")
foreach(_i RANGE ${_last})
    list(GET HARUKA_MODULE_LIST ${_i} _mod)
    string(LENGTH "${MODULES}" _mlen)
    set(_bit "1") # default ON
    if(_i LESS _mlen)
        string(SUBSTRING "${MODULES}" ${_i} 1 _bit)
    endif()
    if(_bit STREQUAL "0")
        set(HARUKA_MOD_${_mod} OFF)
    else()
        set(HARUKA_MOD_${_mod} ON)
        string(APPEND _module_defines "#define HARUKA_MOD_${_mod} 1\n")
    endif()
    message(STATUS "  module ${_mod} = ${HARUKA_MOD_${_mod}}")
endforeach()

# --- Module dependencies ---
# "<MOD>_REQUIRES" lists modules that MOD needs. If MOD is ON but a requirement
# is OFF, auto-enable the requirement (with a warning) so the build stays valid.
# (PARTICLES = sistema de efectos GL_POINTS standalone, sin dependencias.
#  DEFORM, SOFTBODY, AUDIO, NETWORK son self-contained — no requirements.)

foreach(_mod ${HARUKA_MODULE_LIST})
    if(HARUKA_MOD_${_mod})
        foreach(_dep ${${_mod}_REQUIRES})
            if(NOT HARUKA_MOD_${_dep})
                message(WARNING "Module ${_mod} requires ${_dep}; auto-enabling ${_dep}.")
                set(HARUKA_MOD_${_dep} ON)
                string(APPEND _module_defines "#define HARUKA_MOD_${_dep} 1\n")
            endif()
        endforeach()
    endif()
endforeach()

# Generate the single source of truth header used by #ifdef across the code.
# Written INTO the source tree (src/core/) so any consumer that includes
# ENGINE_ROOT/src (e.g. the game) sees it without extra include paths.
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/modules.h.in"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/modules.h"
    @ONLY)

# Collect sources, then drop the ones belonging to OFF modules.
file(GLOB_RECURSE ENGINE_SOURCES CONFIGURE_DEPENDS "src/*.cpp")

# --- src/main.cpp FUERA de la librería compartida -------------------------------------------
# Es el `main()` del runtime antiguo (arrancar el motor a pelo desde un project.hrk). El glob lo
# metía en libHarukaEngine.so, así que la .so EXPORTABA el símbolo `main`:
#     $ nm -D build/libHarukaEngine.so | grep -w main
#     00000000001da25e T main
# No rompía nada —el ejecutable del juego define el suyo y el símbolo fuerte del ejecutable gana—
# pero un `main` público en una librería compartida es una colisión esperando a pasar. Ningún
# add_executable lo usa: el punto de entrada vivo es el del juego.
list(FILTER ENGINE_SOURCES EXCLUDE REGEX "src/main\\.cpp$")

# --- HarukaPhysics: la física en su PROPIA librería, SIN GL ---------------------------------
# Motivo: pasarla al DGS (servidor autoritativo) y validar el movimiento con el MISMO código en
# cliente y servidor. La física solo depende de glm + IWorldProvider (ver src/physics/world_provider.h);
# no toca renderer, SDL ni assets. `octree` es su broad-phase (GL-free, solo lo usa la física).
# El engine EXCLUYE estas fuentes y linka HarukaPhysics.
list(FILTER ENGINE_SOURCES EXCLUDE REGEX "physics/octree\\.cpp$")
list(FILTER ENGINE_SOURCES EXCLUDE REGEX "physics/physics_engine\\.cpp$")
set(PHYSICS_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/physics/octree.cpp")
if(HARUKA_MOD_PHYSICS)
    list(APPEND PHYSICS_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/physics/physics_engine.cpp")
endif()
add_library(HarukaPhysics STATIC ${PHYSICS_SOURCES})
target_include_directories(HarukaPhysics PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src" "${GLM_INCLUDE_DIR}")
set_target_properties(HarukaPhysics PROPERTIES POSITION_INDEPENDENT_CODE ON)  # para el .so del DGS

# --- HarukaConstruction: el núcleo del sistema de construcción, GL-free y FUERA de físicas ----
# El sistema de construcción NO es física: es lógica de VALIDACIÓN determinista de colocación (OBB SAT
# + terreno vía IWorldProvider). Vive en game/construction, GL-free y PIC, en su PROPIA lib para que
# el cliente prediga y el DGS valide con el MISMO código. No cuelga de HarukaPhysics (no es física).
list(FILTER ENGINE_SOURCES EXCLUDE REGEX "game/construction/construction\\.cpp$")
add_library(HarukaConstruction STATIC "${CMAKE_CURRENT_SOURCE_DIR}/src/game/construction/construction.cpp")
target_include_directories(HarukaConstruction PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src" "${GLM_INCLUDE_DIR}")
set_target_properties(HarukaConstruction PROPERTIES POSITION_INDEPENDENT_CODE ON)  # para el .so del DGS

# Per-module source patterns to exclude when the module is OFF.
if(NOT HARUKA_MOD_SOFTBODY)
    list(FILTER ENGINE_SOURCES EXCLUDE REGEX "physics/xpbd/.*\\.cpp$")
    list(FILTER ENGINE_SOURCES EXCLUDE REGEX "renderer/softbody_renderer\\.cpp$")
endif()
if(NOT HARUKA_MOD_FLUIDS)
    list(FILTER ENGINE_SOURCES EXCLUDE REGEX "physics/fluid/.*\\.cpp$")
    list(FILTER ENGINE_SOURCES EXCLUDE REGEX "renderer/shallow_water_renderer\\.cpp$")
    list(FILTER ENGINE_SOURCES EXCLUDE REGEX "renderer/fluid_renderer\\.cpp$")
endif()
if(NOT HARUKA_MOD_AUDIO)
    list(FILTER ENGINE_SOURCES EXCLUDE REGEX "audio/.*\\.cpp$")
endif()
# (physics_engine.cpp / octree.cpp los gestiona el target HarukaPhysics — ver arriba. El gating de
#  PHYSICS off ya se aplica allí: sin PHYSICS, HarukaPhysics contiene solo octree.)
if(NOT HARUKA_MOD_PARTICLES)
    list(FILTER ENGINE_SOURCES EXCLUDE REGEX "renderer/particle_system\\.cpp$")
endif()

# --- Módulo de reglas DGS (ABI dgs_game_module_v1) — se compila como .so APARTE (dlopen) -------
# El módulo valida el movimiento (velocidad/teleport + terreno) con el MISMO código que usará el
# cliente para predicción y el host/DGS para validar. Es GL-FREE: solo el ABI + el sampler de terreno
# CPU (haruka_simbase) + HarukaPhysics. Para el DGS real cargaría con dlopen; en LOCAL, el host de la
# partida lo usa igual. No lo linka el motor (se excluye de ENGINE_SOURCES).
list(FILTER ENGINE_SOURCES EXCLUDE REGEX "net/default_rules\\.cpp$")

# haruka_simbase: el sampler de terreno CPU (sin GL) que el módulo necesita para validar contra el
# suelo. Mismas fuentes que usa el engine, aquí en una lib GL-free reusable por el .so de reglas.
add_library(haruka_simbase STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/terrain/planet_geology.cpp"
    # El cubo↔esfera, que el servidor necesita para hablar de direcciones en la misma retícula que
    # el cliente.
    #
    # ⚠️ Aquí estaba también `reference_surface.cpp`, con el argumento de que el servidor debía
    # evaluar EL MISMO suelo que pisa el cliente. Buen argumento, implementación vacía: su `cornerM`
    # devolvía 0.0, así que lo que compartían era un cero. Retirada; el problema que describía ese
    # comentario SIGUE ABIERTO —el DGS valida contra el nivel del mar— y el arreglo real es que el
    # servidor reproduzca el bake de altura, no una clase que devuelve constantes.
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/terrain/cube_sphere.cpp"
    # EL CLIMA (frentes deterministas). GL-free y aquí, no solo en el motor, por el mismo motivo
    # que la superficie de referencia: el servidor tiene que poder ver LA MISMA tormenta que el
    # cliente para validarla. Es una funcion pura de (seed, tiempo, direccion) → replicarla solo
    # exige compartir el reloj.
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/weather_system.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/noise_generator.cpp")
target_include_directories(haruka_simbase PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src" "${GLM_INCLUDE_DIR}" "${NLOHMANN_JSON_INCLUDE_DIR}")
target_link_libraries(haruka_simbase PUBLIC HarukaPhysics HarukaConstruction)
set_target_properties(haruka_simbase PROPERTIES POSITION_INDEPENDENT_CODE ON)

# haruka_rules.so: el módulo por defecto (dgs_game_module_v1). GL-free.
add_library(haruka_rules SHARED "${CMAKE_CURRENT_SOURCE_DIR}/src/net/default_rules.cpp")
target_include_directories(haruka_rules PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}/external/dgs"
    "${GLM_INCLUDE_DIR}" "${NLOHMANN_JSON_INCLUDE_DIR}")
target_link_libraries(haruka_rules PRIVATE haruka_simbase HarukaPhysics)

# --- 4. Main Target ---
add_library(HarukaEngine SHARED
    ${ENGINE_SOURCES}
    ${GLAD_SOURCE}
    ${IMGUI_SOURCES}
    ${NFD_SOURCES}
)


add_dependencies(HarukaEngine haruka_version)   # version.h fresco antes de compilar

target_include_directories(HarukaEngine PUBLIC
    "${CMAKE_CURRENT_BINARY_DIR}/generated"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${NLOHMANN_JSON_INCLUDE_DIR}"
    "${GLM_INCLUDE_DIR}"
    "${STB_INCLUDE_DIR}"
    "${THIRD_PARTY_DIR}/glad/include"
    "${THIRD_PARTY_DIR}/imgui"
    "${THIRD_PARTY_DIR}/imgui/backends"
    "${THIRD_PARTY_DIR}/nativefiledialog/src/include"
)

# zstd for save/asset compression (codec). System lib via pkg-config, fallback to find_library.
pkg_check_modules(ZSTD libzstd)
if(NOT ZSTD_FOUND)
    find_library(ZSTD_LIB NAMES zstd)
else()
    set(ZSTD_LIB ${ZSTD_LIBRARIES})
endif()

# REVERSED-Z: glm debe emitir profundidad en [0,1] (como D3D/Vulkan), no en [-1,1]. Va PUBLIC
# porque el juego también construye matrices con glm y TODAS deben usar la misma convención
# (el motor llama a glClipControl(GL_ZERO_TO_ONE) al crear el contexto).
target_compile_definitions(HarukaEngine PUBLIC GLM_FORCE_DEPTH_ZERO_TO_ONE)

target_link_libraries(HarukaEngine PUBLIC
    HarukaPhysics
    HarukaConstruction
    SDL3::SDL3
    OpenGL::GL
    assimp::assimp
    OpenSSL::Crypto
    OpenAL::OpenAL
    PostgreSQL::PostgreSQL
    PkgConfig::GTK3
    ${ZSTD_LIB}
    ${CMAKE_DL_LIBS}
)

# Vulkan (backend VK, OPCIONAL): el código se compila bajo `#if __has_include(<vulkan/vulkan.h>)`
# (HARUKA_RHI_HAS_VULKAN). Si hay SDK, hay que enlazar también el loader (libvulkan): la .so
# transparenta los símbolos vk* y los ejecutables fallarían al enlazar sin la librería.
find_package(Vulkan QUIET)
if(Vulkan_FOUND AND TARGET Vulkan::Vulkan)
    target_link_libraries(HarukaEngine PUBLIC Vulkan::Vulkan)
else()
    # Fedora: `vulkan` puede traer solo cabeceras → FindVulkan sin target de librería. Fallback a
    # libvulkan.so del loader (paquete `vulkan-loader`). Si tampoco está, el backend VK no se
    # enlaza y habría que instalarlo (los símbolos vk* de la .so quedarían undefined).
    find_library(VULKAN_LOADER_LIB NAMES vulkan)
    if(VULKAN_LOADER_LIB)
        target_link_libraries(HarukaEngine PUBLIC ${VULKAN_LOADER_LIB})
    endif()
endif()

# --- 5. GLSL -> SPIR-V Shader Compilation ---
find_program(GLSLC     glslc)
find_program(GLSLANG_V glslangValidator)

if(NOT GLSLC AND NOT GLSLANG_V)
    message(FATAL_ERROR
        "glslc or glslangValidator is required to compile SPIR-V shaders.\n"
        "  Fedora/RHEL: sudo dnf install glslang\n"
        "  Ubuntu/Deb:  sudo apt install glslang-tools")
endif()

# Fuentes de shaders del motor bajo assets/ (raíz de assets del motor: aquí también
# irían texturas/fuentes por defecto que el motor traiga de serie). El runtime los busca
# en assets/shaders/ (AssetPaths::shaders()), así que source e install coinciden.
set(SHADER_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/assets/shaders")
if(NOT SHADER_OUT_DIR)
    set(SHADER_OUT_DIR "${CMAKE_BINARY_DIR}/assets/shaders")
endif()
file(MAKE_DIRECTORY "${SHADER_OUT_DIR}")

# CONFIGURE_DEPENDS por la MISMA razón que en SHADER_LIBS más abajo: sin él el glob solo se evalúa al
# configurar, así que un shader NUEVO no se compila hasta relanzar cmake a mano — y el fallo es mudo,
# porque falta el .spv, el pipeline no se crea y lo que fuera a dibujar simplemente no aparece. Se
# añadió tras perder un rato con exactamente eso al portar el preview de items al RHI.
file(GLOB SHADER_SOURCES CONFIGURE_DEPENDS
    "${SHADER_SRC_DIR}/*.vert"
    "${SHADER_SRC_DIR}/*.frag"
    "${SHADER_SRC_DIR}/*.geom"
    "${SHADER_SRC_DIR}/*.comp"
)

# No compiles los shaders de un módulo apagado (su renderer también está excluido → no se usan).
if(NOT HARUKA_MOD_FLUIDS)
    list(FILTER SHADER_SOURCES EXCLUDE REGEX "/(fluid_[^/]*|water)\\.(vert|frag)$")
endif()
if(NOT HARUKA_MOD_PARTICLES)
    list(FILTER SHADER_SOURCES EXCLUDE REGEX "/particle\\.(vert|frag)$")
endif()
# (El filtro de HARUKA_MOD_SHADOWS excluía shadow.* y point_shadow.*. Los dos se borraron por no
#  tener call-site: el pase de sombra real usa el shader del JUEGO —Survival/assets/shaders/
#  shadow_depth.*— y las sombras puntuales nunca se llegaron a construir. El filtro quedaba vacío.)
if(NOT HARUKA_MOD_SOFTBODY)
    list(FILTER SHADER_SOURCES EXCLUDE REGEX "/softbody\\.(vert|frag)$")
endif()

# Las librerías compartidas (assets/shaders/lib/*.glsl) NO se compilan por su cuenta: son
# fragmentos que otros shaders incluyen. Se copian al directorio de salida —el RHI las resuelve en
# tiempo de carga— y se declaran como DEPENDS para que tocar una recompile a quien la usa.
# CONFIGURE_DEPENDS: sin él el glob solo se evalúa al configurar, así que una librería NUEVA no se
# copia hasta volver a lanzar cmake a mano. El síntoma es idéntico al de la nota de abajo pero peor:
# el `#include` no se encuentra, el pipeline no linka y el objeto simplemente no se dibuja — sin
# error de dibujado, porque el fallo fue al crear el pipeline y el camino de respaldo lo tapa.
#
# Los del PLANETA (assets/shaders/planet/) SÍ se compilan a SPIR-V: el RHI los carga por ruta en
# tiempo de ejecución (`PipelineDesc::*Path`), y entre ellos hay .tesc/.tese, que el glob plano de
# arriba no recoge. PARIDAD GL=VK por construcción: el mismo GLSL → el mismo glslang/glslc, con el
# target de GL (bindings literales). El runtime GL prefiere este .spv; el GLSL del driver solo queda
# como fallback para GPUs sin GL_ARB_gl_spirv. (Antes solo se copiaban: el clipmap —el único que
# aporta detalle fino— caía siempre al compilador GLSL del driver, divergiendo de Vulkan.)
file(GLOB SHADER_LIBS CONFIGURE_DEPENDS
    "${SHADER_SRC_DIR}/lib/*.glsl")
list(FILTER SHADER_SOURCES EXCLUDE REGEX "/(lib|planet)/")
# ⚠️ `.comp` incluido: el culling de parches (planet/terrain_cull.comp) es un COMPUTE, y sin él en
# este glob el fichero no se copiaba al build ni se compilaba a SPIR-V. El síntoma era
# "createPipeline: falta la etapa COMPUTE" — el motor pedía una ruta que no existía en bin/.
file(GLOB PLANET_SHADERS CONFIGURE_DEPENDS
    "${SHADER_SRC_DIR}/planet/*.vert" "${SHADER_SRC_DIR}/planet/*.frag"
    "${SHADER_SRC_DIR}/planet/*.tesc" "${SHADER_SRC_DIR}/planet/*.tese"
    "${SHADER_SRC_DIR}/planet/*.comp")
# Copia en tiempo de BUILD, no de configure. Con `configure_file` la copia solo se rehace al
# re-ejecutar cmake, así que editar una librería y compilar dejaba el shader usando la versión
# ANTERIOR — y el síntoma es el peor posible: el cambio "no hace nada" y parece que la hipótesis
# era falsa. Costó una iteración entera en la paridad del terreno.
#
# Se preserva el subdirectorio (lib/ o planet/), porque las rutas que pide el motor lo incluyen.
set(SHADER_LIB_OUTPUTS "")
foreach(LIB ${SHADER_LIBS})
    file(RELATIVE_PATH LIBREL "${SHADER_SRC_DIR}" "${LIB}")
    set(LIBOUT "${SHADER_OUT_DIR}/${LIBREL}")
    add_custom_command(OUTPUT ${LIBOUT}
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${LIB} ${LIBOUT}
        DEPENDS ${LIB}
        COMMENT "shader lib  ${LIBREL}"
        VERBATIM)
    list(APPEND SHADER_LIB_OUTPUTS ${LIBOUT})
endforeach()

set(SPIRV_OUTPUTS "")
foreach(SRC ${SHADER_SOURCES})
    get_filename_component(NAME ${SRC} NAME)
    set(SPV     "${SHADER_OUT_DIR}/${NAME}.spv")
    set(DST_SRC "${SHADER_OUT_DIR}/${NAME}")

    if(GLSLC)
        add_custom_command(OUTPUT ${SPV}
            COMMAND ${GLSLC} --target-env=opengl -I${SHADER_SRC_DIR} ${SRC} -o ${SPV}
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SRC} ${DST_SRC}
            DEPENDS ${SRC} ${SHADER_LIBS}
            COMMENT "glslc  ${NAME} -> ${NAME}.spv"
            VERBATIM)
    else()
        add_custom_command(OUTPUT ${SPV}
            COMMAND ${GLSLANG_V} -G -I${SHADER_SRC_DIR} ${SRC} -o ${SPV}
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SRC} ${DST_SRC}
            DEPENDS ${SRC} ${SHADER_LIBS}
            COMMENT "glslangValidator  ${NAME} -> ${NAME}.spv"
            VERBATIM)
    endif()

    list(APPEND SPIRV_OUTPUTS ${SPV})
endforeach()

# Shaders del PLANETA → SPIR-V GL, preservando el subdirectorio planet/ (el RHI pide la ruta
# completa `assets/shaders/planet/*.spv`). Igual target que el resto: bindings literales GL.
foreach(SRC ${PLANET_SHADERS})
    get_filename_component(NAME ${SRC} NAME)
    set(SPV "${SHADER_OUT_DIR}/planet/${NAME}.spv")
    set(DST_SRC "${SHADER_OUT_DIR}/planet/${NAME}")

    if(GLSLC)
        add_custom_command(OUTPUT ${SPV}
            COMMAND ${CMAKE_COMMAND} -E make_directory "${SHADER_OUT_DIR}/planet"
            COMMAND ${GLSLC} --target-env=opengl -I${SHADER_SRC_DIR} ${SRC} -o ${SPV}
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SRC} ${DST_SRC}
            DEPENDS ${SRC} ${SHADER_LIBS}
            COMMENT "glslc  planet/${NAME} -> planet/${NAME}.spv"
            VERBATIM)
    else()
        add_custom_command(OUTPUT ${SPV}
            COMMAND ${CMAKE_COMMAND} -E make_directory "${SHADER_OUT_DIR}/planet"
            COMMAND ${GLSLANG_V} -G -I${SHADER_SRC_DIR} ${SRC} -o ${SPV}
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SRC} ${DST_SRC}
            DEPENDS ${SRC} ${SHADER_LIBS}
            COMMENT "glslangValidator  planet/${NAME} -> planet/${NAME}.spv"
            VERBATIM)
    endif()

    list(APPEND SPIRV_OUTPUTS ${SPV})
endforeach()

add_custom_target(HarukaShaders ALL DEPENDS ${SPIRV_OUTPUTS} ${SHADER_LIB_OUTPUTS})
add_dependencies(HarukaEngine HarukaShaders)

# --- 6a. mapc: map compiler tool (.scene -> .hmap) ---
add_executable(mapc tools/mapc.cpp)
target_link_libraries(mapc PRIVATE HarukaEngine)
set_target_properties(mapc PROPERTIES
    BUILD_RPATH   "$ORIGIN"
    INSTALL_RPATH "$ORIGIN")

# --- 6a-bis. bench_dgs_bandwidth: test de estrés de transferencia entre nodos DGS (sin .a, sin
# sockets). Modela el límite BDP (min(enlace, ventana/RTT)) y estresa el PacketFramer real (bug 6)
# con el volumen completo. Header-only → compila en CI; `--fast` para verificación rápida. ---
add_executable(bench_dgs_bandwidth tools/bench_dgs_bandwidth.cpp)
target_include_directories(bench_dgs_bandwidth PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/external/dgs")

# --- Jolt Physics (PRUEBA de integración): motor de rígidos maduro (SAT, apilado estable,
# determinista, GL-free). Submódulo git en third_party/JoltPhysics. Solo se compila si el módulo
# PHYSICS está ACTIVO y el submódulo está presente → con la física apagada, Jolt NO existe. ---
if(HARUKA_MOD_PHYSICS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/JoltPhysics/Build/CMakeLists.txt")
    set(DOUBLE_PRECISION OFF CACHE BOOL "" FORCE)                 # física en marco LOCAL (floats bastan)
    set(GENERATE_DEBUG_SYMBOLS OFF CACHE BOOL "" FORCE)
    set(OVERRIDE_CXX_FLAGS OFF CACHE BOOL "" FORCE)               # no pisar los flags del proyecto padre
    set(INTERPROCEDURAL_OPTIMIZATION OFF CACHE BOOL "" FORCE)     # build más rápido
    set(FLOATING_POINT_EXCEPTIONS_ENABLED OFF CACHE BOOL "" FORCE)
    set(CROSS_PLATFORM_DETERMINISTIC OFF CACHE BOOL "" FORCE)     # ON cuando toque el DGS
    set(ENABLE_ALL_WARNINGS OFF CACHE BOOL "" FORCE)              # que -Werror de Jolt no rompa el build
    set(USE_ASSERTS OFF CACHE BOOL "" FORCE)
    # Apagar el compute GPU/CPU de Jolt (simulación de pelo): compila shaders HLSL→SPV y necesita glslc
    # de Vulkan, que aquí no está. El núcleo de rígidos NO lo necesita.
    set(JPH_USE_DX12 OFF CACHE BOOL "" FORCE)
    set(JPH_USE_VK   OFF CACHE BOOL "" FORCE)
    set(JPH_USE_MTL  OFF CACHE BOOL "" FORCE)
    set(JPH_USE_CPU_COMPUTE OFF CACHE BOOL "" FORCE)
    add_subdirectory(third_party/JoltPhysics/Build "${CMAKE_BINARY_DIR}/jolt" EXCLUDE_FROM_ALL)
    # HarukaEngine es SHARED → todo lo que enlaza debe ser PIC. Jolt no lo pone por defecto.
    set_target_properties(Jolt PROPERTIES POSITION_INDEPENDENT_CODE ON)
    # Optimizar Jolt para TAMAÑO (-Os) y sin símbolos de depuración → objetos/lib mucho más pequeños en
    # el build (Jolt es grande). No afecta a la corrección; sí acelera y adelgaza.
    target_compile_options(Jolt PRIVATE -Os -g0)

    # Fase 1: HarukaPhysics ENVUELVE a Jolt (PIMPL) → el juego nunca ve Jolt. PRIVATE = encapsulado;
    # las include-dirs llegan a physics_engine.cpp y los símbolos se enlazan en el consumidor final.
    # HARUKA_HAS_JOLT activa el camino Jolt en physics_engine.cpp (si no, cae al solver a mano).
    target_link_libraries(HarukaPhysics PRIVATE Jolt)
    target_compile_definitions(HarukaPhysics PRIVATE HARUKA_HAS_JOLT)
endif()

# --- 6a-bis. haruka_tests: banco de pruebas HEADLESS (LOD/sampler/órbitas, sin ventana/GL) ---
# Subsistemas puros-CPU dirigidos con entrada sintética + verificación de invariantes → depurar a
# pequeña escala, determinista y rápido, sin cargar un planeta entero. `ctest` o `./bin/haruka_tests`.
add_executable(haruka_tests
    tests/haruka_tests.cpp      # main() — selecciona/dirige
    tests/test_common.cpp       # framework de aserciones compartido
    tests/test_terrain.cpp      # cube-sphere / climate / weather fronts / ground layer (CPU)
    tests/test_physics.cpp      # caída radial + determinismo (sin GL)
    tests/test_construction.cpp # sistema de construcción: validación de colocación F1 (sin GL)
    tests/test_ports.cpp        # puertos autorizados + raíles (mecanismos, no animaciones)
    tests/test_dgs.cpp
    tests/test_simple_planet.cpp         # SimplePlanet mesh & config
    tests/test_terrain_quality.cpp       # terrain quality → resolution mappings
    tests/test_sky_ambient.cpp           # ambiente SH: paridad con sky_palette.glsl + simetría
    tests/test_procgraph.cpp             # ProcGraph nodes, BiomeConfig, RGBAImage
    tests/test_orbit.cpp                 # órbitas Kepler con elementos precesantes
    tests/test_image_writer.cpp          # PNG: ida y vuelta + compresión DEFLATE
    tests/test_prop_lod.cpp              # LOD de malla de props: envolvente estable
    tests/test_prop_collider.cpp         # colliders de props por PARTE (esqueleto del arbol)
    tests/test_rock_interior.cpp         # penones ENTERRADOS de la roca: no se ven (rayos + contraprueba)
    tests/test_weather_3d.cpp            # clima 3D: la nube como volumen (base + techo)
    tests/test_cloud_shape.cpp           # forma de la nube: cuerpo y no lamina (ancho/alto + campo 3D)
    tests/test_ocean.cpp                 # el mar: derivada de la ola, transporte, bajio y flotacion
    tests/test_terrain_node.cpp)         # v5 F1: direccionamiento de nodos del quadtree (bit-exacto)
target_link_libraries(haruka_tests PRIVATE HarukaEngine)
# test_dgs.cpp incluye el ABI del DGS (external/dgs/include/dgs/game_module.h). El include no siempre
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
    tests/rhi_test_main.cpp)
target_link_libraries(haruka_tests_rhi PRIVATE HarukaEngine)
set_target_properties(haruka_tests_rhi PROPERTIES
    BUILD_RPATH   "$ORIGIN"
    INSTALL_RPATH "$ORIGIN")

# --- 6b. DGS Network Client (optional) ---
option(HARUKA_NETWORK "Enable DGS network client" OFF)
if(HARUKA_NETWORK)
    target_compile_definitions(HarukaEngine PUBLIC HARUKA_NETWORK)
    target_include_directories(HarukaEngine PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/external/dgs")
    target_link_libraries(HarukaEngine PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/external/dgs/sdk/libdgs_client.a"
        OpenSSL::SSL
    )
endif()

# --- 6. Install Rules ---
# Only install the engine's standalone layout when this project is top-level.
if(PROJECT_IS_TOP_LEVEL)
    include(GNUInstallDirs)

    install(TARGETS HarukaEngine
        LIBRARY DESTINATION "bin"
        RUNTIME DESTINATION "bin")

    install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/src/"
        DESTINATION "lib64/haruka-cpp"
        FILES_MATCHING
        PATTERN "*.h"
        PATTERN "*.hpp"
        PATTERN "*.cpp")

    install(DIRECTORY "${SHADER_OUT_DIR}/"
        DESTINATION "bin/assets/shaders"
        FILES_MATCHING PATTERN "*.spv")
endif()

# --- 7. Examples ---
option(HARUKA_BUILD_EXAMPLES "Build example projects" OFF)
if(HARUKA_BUILD_EXAMPLES)
    add_subdirectory(examples/planet_demo)
endif()