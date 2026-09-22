cmake_minimum_required(VERSION 3.14)

if(DEFINED ENV{GITHUB_RUN_NUMBER})
    set(HARUKA_BUILD_NUMBER $ENV{GITHUB_RUN_NUMBER})
else()
    set(HARUKA_BUILD_NUMBER "local")
endif()

set(VERSION "1.0.0" CACHE STRING "Engine version (used as build/install subdirectory)")

project(HarukaEngine VERSION ${VERSION})


# --- version.h (ver cmake/HarukaVersion.cmake: va DESPUES de project() a proposito) ---
include(cmake/HarukaVersion.cmake)

set(OpenGL_GL_PREFERENCE GLVND)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- Como se compila: tipo de build, HARUKA_BUILD_TESTS, -ffp-contract, HARUKA_ARCH, sanitizadores ---
include(cmake/HarukaBuildFlags.cmake)

set(THIRD_PARTY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party")

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

# --- 1/2/3. Dependencias del sistema, header-only y fuentes de terceros ---
include(cmake/HarukaDependencies.cmake)

# --- 3b. Modulos (MODULES=1011...) -> HARUKA_MOD_* y src/core/modules.h ---
include(cmake/HarukaModules.cmake)

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
    "${CMAKE_CURRENT_SOURCE_DIR}/src/world/planet/planet_geology.cpp"
    # El cubo↔esfera, que el servidor necesita para hablar de direcciones en la misma retícula que
    # el cliente.
    #
    # ⚠️ Aquí estaba también `reference_surface.cpp`, con el argumento de que el servidor debía
    # evaluar EL MISMO suelo que pisa el cliente. Buen argumento, implementación vacía: su `cornerM`
    # devolvía 0.0, así que lo que compartían era un cero. Retirada; el problema que describía ese
    # comentario SIGUE ABIERTO —el DGS valida contra el nivel del mar— y el arreglo real es que el
    # servidor reproduzca el bake de altura, no una clase que devuelve constantes.
    "${CMAKE_CURRENT_SOURCE_DIR}/src/world/terrain/cube_sphere.cpp"
    # EL CLIMA (frentes deterministas). GL-free y aquí, no solo en el motor, por el mismo motivo
    # que la superficie de referencia: el servidor tiene que poder ver LA MISMA tormenta que el
    # cliente para validarla. Es una funcion pura de (seed, tiempo, direccion) → replicarla solo
    # exige compartir el reloj.
    "${CMAKE_CURRENT_SOURCE_DIR}/src/world/weather_system.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/world/noise_generator.cpp")
target_include_directories(haruka_simbase PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src" "${GLM_INCLUDE_DIR}" "${NLOHMANN_JSON_INCLUDE_DIR}")
target_link_libraries(haruka_simbase PUBLIC HarukaPhysics HarukaConstruction)
set_target_properties(haruka_simbase PROPERTIES POSITION_INDEPENDENT_CODE ON)

# haruka_rules.so: el módulo por defecto (dgs_game_module_v1). GL-free.
#
# ⚠️ `logger.cpp` VA AQUÍ, y no es un detalle de compilación: sin él el `.so` queda con
# `Haruka::diagLogs()` y `Haruka::log(...)` SIN RESOLVER —los usa por `HARUKA_LOGI`/`HARUKA_LOGW`— y
# `dlopen` FALLA en cualquier proceso que no tenga ya el motor cargado. O sea: en un `validador_node`
# headless, que es el único sitio donde este módulo tiene sentido. El nodo no se cae; dice
# "sin modulo de reglas -> fallback generico" y sigue con su regla propia, así que **el anti-cheat del
# juego no se estaba aplicando en despliegue** y nada lo delataba.
#
# No lo veía ningún test: `tests/test_dgs.cpp` hace `dlopen` desde `haruka_tests`, que YA enlaza el
# motor, así que el símbolo aparecía por la puerta de atrás. Lo destapó `dgs_live_terrain`, que carga
# el módulo desde el nodo de verdad.
add_library(haruka_rules SHARED
    "${CMAKE_CURRENT_SOURCE_DIR}/src/net/default_rules.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/logger.cpp")
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

# --- 5. GLSL -> SPIR-V (target HarukaShaders, del que depende HarukaEngine) ---
include(cmake/HarukaShaders.cmake)

# --- 6a. mapc: map compiler tool (.scene -> .hmap) ---
add_executable(mapc tools/mapc.cpp)
target_link_libraries(mapc PRIVATE HarukaEngine)
set_target_properties(mapc PROPERTIES
    BUILD_RPATH   "$ORIGIN"
    INSTALL_RPATH "$ORIGIN")

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

# --- 6a-bis. Bancos de pruebas (solo con HARUKA_BUILD_TESTS; ver HarukaBuildFlags.cmake) ---
if(HARUKA_BUILD_TESTS)
    include(cmake/HarukaTests.cmake)
endif()

# --- 6b. DGS Network Client (optional) ---
option(HARUKA_NETWORK "Enable DGS network client" OFF)
if(HARUKA_NETWORK)
    # ⚠️ El cliente se RESUELVE (checkout local o git), nunca desde un `.a` vendido: el precompilado en
    # external/dgs/sdk quedaba obsoleto en cuanto se tocaba network.cpp y el juego seguía mandando con
    # el código viejo sin que nadie lo notara — el síntoma real era "en cli todo bien, la zona nunca
    # recibe nada". Mismo patrón que HARUKA_ENGINE_LOCAL en el juego: checkout local si hay uno junto
    # al motor, si no git desde GitHub; el tag pinnea releases reproducibles.
    set(_default_dgs_local "")
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../dgs/CMakeLists.txt")
        set(_default_dgs_local "${CMAKE_CURRENT_SOURCE_DIR}/../dgs")
    endif()
    set(HARUKA_DGS_LOCAL "${_default_dgs_local}" CACHE PATH "Checkout LOCAL de dgs (vacío = descargar de GitHub)")
    set(DGS_GIT_TAG "main" CACHE STRING "Tag/rama de dgs a usar cuando no hay checkout local")
    set(DGS_BUILD_NODES OFF CACHE BOOL "dgs: solo la lib del cliente (nunca nodos/tests) cuando lo trae el motor/juego" FORCE)
    set(DGS_BUILD_NODES OFF CACHE BOOL "" FORCE)
    include(FetchContent)
    if(HARUKA_DGS_LOCAL)
        get_filename_component(_dgs_abs "${HARUKA_DGS_LOCAL}" ABSOLUTE)
        message(STATUS "DGS client: LOCAL @ ${_dgs_abs}")
        FetchContent_Declare(dgs SOURCE_DIR "${_dgs_abs}")
    else()
        message(STATUS "DGS client: REMOTO (GitHub @ ${DGS_GIT_TAG})")
        FetchContent_Declare(
            dgs
            GIT_REPOSITORY https://github.com/Andonigt04/dgs.git
            GIT_TAG        ${DGS_GIT_TAG}
        )
    endif()
    FetchContent_MakeAvailable(dgs)

    target_compile_definitions(HarukaEngine PUBLIC HARUKA_NETWORK)
    # `"include/dgs/..."` se resuelve contra la RAÍZ del checkout de dgs (include/dgs/...), igual que
    # hace `include_directories(.)` en su propio CMake. PUBLIC para que el juego lo herede al linkar
    # el motor y el .so de reglas (dlopen) lo use.
    target_include_directories(HarukaEngine PUBLIC "${dgs_SOURCE_DIR}")
    target_link_libraries(HarukaEngine PRIVATE dgs_client OpenSSL::SSL)
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
