cmake_minimum_required(VERSION 3.14)

# Nombre del juego. Al crear el proyecto desde el editor, el placeholder
# PROJECT_NAME_PLACEHOLDER se sustituye por el nombre real.
set(PROJECT_NAME "PROJECT_NAME_PLACEHOLDER" CACHE STRING "Game project name")

project(${PROJECT_NAME} VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# =====================================================================================
# --- DESCARGA Y COMPILACIÓN AUTOMÁTICA DEL MOTOR (haruka-cpp) ---
# Igual que el juego (Survival): motor LOCAL por defecto, REMOTO (GitHub) si no existe.
# Al crear el proyecto desde el editor, HARUKA_ENGINE_LOCAL_PLACEHOLDER se sustituye
# por la ruta del motor que usa el editor (compila sin descargar nada).
# Override manual:
#   Forzar remoto: cmake -B build -DHARUKA_ENGINE_LOCAL=""
#   Otra rama:     cmake -B build -DBRANCH_NAME=otra-rama
# =====================================================================================
include(FetchContent)

# Engine shader output dir consumed by haruka-cpp CMake. Va a bin/assets/shaders para
# que COINCIDA con donde el runtime los busca (AssetPaths::shaders() = "assets/shaders/").
set(SHADER_OUT_DIR "${CMAKE_BINARY_DIR}/bin/assets/shaders" CACHE PATH "Engine SPIR-V output directory")

set(BRANCH_NAME "main" CACHE STRING "Branch/tag del motor a descargar")

set(_default_engine_local "HARUKA_ENGINE_LOCAL_PLACEHOLDER")
if(_default_engine_local STREQUAL "HARUKA_ENGINE_LOCAL_PLACEHOLDER" OR
   NOT EXISTS "${_default_engine_local}/CMakeLists.txt")
    set(_default_engine_local "")
endif()
set(HARUKA_ENGINE_LOCAL "${_default_engine_local}" CACHE PATH "Ruta a un motor LOCAL (vacío = descargar de GitHub)")

if(HARUKA_ENGINE_LOCAL)
    get_filename_component(_eng_abs "${HARUKA_ENGINE_LOCAL}" ABSOLUTE)
    message(STATUS "HarukaEngine: LOCAL @ ${_eng_abs}")
    FetchContent_Declare(haruka_engine SOURCE_DIR "${_eng_abs}")
else()
    message(STATUS "HarukaEngine: REMOTO (GitHub @ ${BRANCH_NAME})")
    FetchContent_Declare(
        haruka_engine
        GIT_REPOSITORY https://github.com/Andonigt04/haruka-cpp.git
        GIT_TAG        ${BRANCH_NAME}
    )
endif()

FetchContent_MakeAvailable(haruka_engine)

set(ENGINE_ROOT          "${haruka_engine_SOURCE_DIR}")
# Linka el TARGET del motor: hereda include-dirs (glm, imgui, glad, json, core/renderer)
# y compile-definitions PUBLIC (GLM_FORCE_DEPTH_ZERO_TO_ONE).
set(HarukaEngine_LIBRARY HarukaEngine)

# --- DGS network (optional) ---
option(HARUKA_NETWORK "Enable DGS network client" OFF)
if(HARUKA_NETWORK)
    find_package(OpenSSL REQUIRED)
endif()

function(target_apply_network TGT)
    if(HARUKA_NETWORK)
        target_compile_definitions(${TGT} PRIVATE HARUKA_NETWORK)
        target_include_directories(${TGT} PRIVATE "${ENGINE_ROOT}/external/dgs")
        target_link_libraries(${TGT} PRIVATE
            "${ENGINE_ROOT}/external/dgs/sdk/libdgs_client.a"
            OpenSSL::SSL)
    endif()
endfunction()

file(GLOB_RECURSE GAME_SOURCES CONFIGURE_DEPENDS scripts/*.cpp)
if(NOT GAME_SOURCES)
    message(FATAL_ERROR "No source files in scripts/")
endif()

# --- Plugin .so (el editor lo carga vía getGameInterface) ---
# El editor busca lib<Nombre>.so en build/bin/ (layout del juego moderno).
add_library(${PROJECT_NAME}Lib SHARED ${GAME_SOURCES})
target_include_directories(${PROJECT_NAME}Lib PRIVATE scripts)
target_link_libraries(${PROJECT_NAME}Lib PRIVATE ${HarukaEngine_LIBRARY})
target_apply_network(${PROJECT_NAME}Lib)
set_target_properties(${PROJECT_NAME}Lib PROPERTIES
    OUTPUT_NAME              "${PROJECT_NAME}"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# --- Ejecutable standalone ---
find_package(SDL3 QUIET)
add_executable(${PROJECT_NAME}Exe main.cpp ${GAME_SOURCES})
target_include_directories(${PROJECT_NAME}Exe PRIVATE scripts)
target_link_libraries(${PROJECT_NAME}Exe PRIVATE ${HarukaEngine_LIBRARY})
if(SDL3_FOUND)
    target_link_libraries(${PROJECT_NAME}Exe PRIVATE SDL3::SDL3)
else()
    find_library(SDL3_LIB NAMES SDL3)
    if(SDL3_LIB)
        target_link_libraries(${PROJECT_NAME}Exe PRIVATE ${SDL3_LIB})
    endif()
endif()
target_apply_network(${PROJECT_NAME}Exe)

set(BIN_DIR "${CMAKE_BINARY_DIR}/bin")
set_target_properties(${PROJECT_NAME}Exe PROPERTIES
    OUTPUT_NAME              "${PROJECT_NAME}"
    RUNTIME_OUTPUT_DIRECTORY "${BIN_DIR}"
    BUILD_RPATH              "${BIN_DIR}:${CMAKE_BINARY_DIR}"
    INSTALL_RPATH            "$ORIGIN")

# Copiar motor + shaders + scenes + assets a bin/ en cada build (layout dev == release).
add_custom_command(TARGET ${PROJECT_NAME}Exe POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_BINARY_DIR}/libHarukaEngine.so" "${BIN_DIR}/libHarukaEngine.so"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${BIN_DIR}/assets/shaders"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${SHADER_OUT_DIR}" "${BIN_DIR}/assets/shaders"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/scenes" "${BIN_DIR}/scenes"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/assets" "${BIN_DIR}/assets"
    VERBATIM)

# --- Release layout ---
install(TARGETS ${PROJECT_NAME}Exe RUNTIME DESTINATION .)
install(FILES "${CMAKE_BINARY_DIR}/libHarukaEngine.so" DESTINATION .)
install(DIRECTORY "${SHADER_OUT_DIR}/" DESTINATION assets/shaders OPTIONAL
    FILES_MATCHING
    PATTERN "*.spv"
    PATTERN "*.vert"
    PATTERN "*.frag"
    PATTERN "*.geom"
    PATTERN "*.comp")
install(DIRECTORY scenes DESTINATION .)
install(DIRECTORY assets DESTINATION .)
