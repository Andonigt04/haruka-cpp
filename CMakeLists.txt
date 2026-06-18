cmake_minimum_required(VERSION 3.14)

if(DEFINED ENV{GITHUB_RUN_NUMBER})
    set(HARUKA_BUILD_NUMBER $ENV{GITHUB_RUN_NUMBER})
else()
    set(HARUKA_BUILD_NUMBER "local")
endif()

execute_process(
    COMMAND git rev-parse --short HEAD
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE HARUKA_BUILD_HASH
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)

if(NOT HARUKA_BUILD_HASH)
    set(HARUKA_BUILD_HASH "unknown")
endif()

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/version.h"
)

# Must be set before project() so -DVERSION=x.x.x is picked up.
set(VERSION "1.0.0" CACHE STRING "Engine version (used as build/install subdirectory)")

project(HarukaEngine VERSION ${VERSION})

set(OpenGL_GL_PREFERENCE GLVND)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT MSVC)
    set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -flto -ffast-math")
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

# Per-module source patterns to exclude when the module is OFF.
if(NOT HARUKA_MOD_DEFORM)
    list(FILTER ENGINE_SOURCES EXCLUDE REGEX "core/terrain/deformation_field\\.cpp$")
endif()
if(NOT HARUKA_MOD_SOFTBODY)
    list(FILTER ENGINE_SOURCES EXCLUDE REGEX "physics/xpbd/.*\\.cpp$")
    list(FILTER ENGINE_SOURCES EXCLUDE REGEX "renderer/softbody_renderer\\.cpp$")
endif()
if(NOT HARUKA_MOD_FLUIDS)
    list(FILTER ENGINE_SOURCES EXCLUDE REGEX "physics/fluid/.*\\.cpp$")
    list(FILTER ENGINE_SOURCES EXCLUDE REGEX "renderer/water_renderer\\.cpp$")
    list(FILTER ENGINE_SOURCES EXCLUDE REGEX "renderer/shallow_water_renderer\\.cpp$")
    list(FILTER ENGINE_SOURCES EXCLUDE REGEX "renderer/fluid_renderer\\.cpp$")
endif()
if(NOT HARUKA_MOD_AUDIO)
    list(FILTER ENGINE_SOURCES EXCLUDE REGEX "audio/.*\\.cpp$")
endif()
if(NOT HARUKA_MOD_PHYSICS)
    # Solo el motor de físicas. octree.cpp/raycast_simple.cpp se quedan (los usan
    # scene_manager y main, no son exclusivos de físicas).
    list(FILTER ENGINE_SOURCES EXCLUDE REGEX "physics/physics_engine\\.cpp$")
endif()
if(NOT HARUKA_MOD_PARTICLES)
    list(FILTER ENGINE_SOURCES EXCLUDE REGEX "renderer/particle_system\\.cpp$")
endif()

# --- 4. Main Target ---
add_library(HarukaEngine SHARED
    ${ENGINE_SOURCES}
    ${GLAD_SOURCE}
    ${IMGUI_SOURCES}
    ${NFD_SOURCES}
)


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

target_link_libraries(HarukaEngine PUBLIC
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

# --- 5. GLSL -> SPIR-V Shader Compilation ---
find_program(GLSLC     glslc)
find_program(GLSLANG_V glslangValidator)

if(NOT GLSLC AND NOT GLSLANG_V)
    message(FATAL_ERROR
        "glslc or glslangValidator is required to compile SPIR-V shaders.\n"
        "  Fedora/RHEL: sudo dnf install glslang\n"
        "  Ubuntu/Deb:  sudo apt install glslang-tools")
endif()

set(SHADER_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/shaders")
if(NOT SHADER_OUT_DIR)
    set(SHADER_OUT_DIR "${CMAKE_BINARY_DIR}/shaders")
endif()
file(MAKE_DIRECTORY "${SHADER_OUT_DIR}")

file(GLOB SHADER_SOURCES
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
if(NOT HARUKA_MOD_SHADOWS)
    list(FILTER SHADER_SOURCES EXCLUDE REGEX "/(shadow|point_shadow)\\.(vert|frag|geom)$")
endif()
if(NOT HARUKA_MOD_SOFTBODY)
    list(FILTER SHADER_SOURCES EXCLUDE REGEX "/softbody\\.(vert|frag)$")
endif()

set(SPIRV_OUTPUTS "")
foreach(SRC ${SHADER_SOURCES})
    get_filename_component(NAME ${SRC} NAME)
    set(SPV     "${SHADER_OUT_DIR}/${NAME}.spv")
    set(DST_SRC "${SHADER_OUT_DIR}/${NAME}")

    if(GLSLC)
        add_custom_command(OUTPUT ${SPV}
            COMMAND ${GLSLC} --target-env=opengl ${SRC} -o ${SPV}
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SRC} ${DST_SRC}
            DEPENDS ${SRC}
            COMMENT "glslc  ${NAME} -> ${NAME}.spv"
            VERBATIM)
    else()
        add_custom_command(OUTPUT ${SPV}
            COMMAND ${GLSLANG_V} -G ${SRC} -o ${SPV}
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SRC} ${DST_SRC}
            DEPENDS ${SRC}
            COMMENT "glslangValidator  ${NAME} -> ${NAME}.spv"
            VERBATIM)
    endif()

    list(APPEND SPIRV_OUTPUTS ${SPV})
endforeach()

add_custom_target(HarukaShaders ALL DEPENDS ${SPIRV_OUTPUTS})
add_dependencies(HarukaEngine HarukaShaders)

# --- 6a. mapc: map compiler tool (.scene -> .hmap) ---
add_executable(mapc tools/mapc.cpp)
target_link_libraries(mapc PRIVATE HarukaEngine)
set_target_properties(mapc PROPERTIES
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
# When consumed via FetchContent (e.g. Survival), the parent project owns dist.
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
        DESTINATION "bin/shaders"
        FILES_MATCHING PATTERN "*.spv")
endif()