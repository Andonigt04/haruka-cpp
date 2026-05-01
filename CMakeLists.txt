cmake_minimum_required(VERSION 3.14)

# Must be set before project() so -DENGINE_VERSION=x.x.x is picked up.
set(ENGINE_VERSION "1.0.0" CACHE STRING "Engine version (used as build/install subdirectory)")

project(HarukaEngine VERSION ${ENGINE_VERSION})

set(OpenGL_GL_PREFERENCE GLVND)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT MSVC)
    set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -flto -ffast-math")
endif()

set(THIRD_PARTY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party")

# Output layout: build/x.x.x/bin/ for .so and shaders.
# haruka/CMakeLists.txt reads from here directly (no install step needed during dev).
set(VERSION_DIR "${CMAKE_BINARY_DIR}/${PROJECT_VERSION}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${VERSION_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${VERSION_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${VERSION_DIR}/bin")

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

file(GLOB_RECURSE ENGINE_SOURCES "src/*.cpp")

# --- 4. Main Target ---
add_library(HarukaEngine SHARED
    ${ENGINE_SOURCES}
    ${GLAD_SOURCE}
    ${IMGUI_SOURCES}
    ${NFD_SOURCES}
)

target_include_directories(HarukaEngine PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${THIRD_PARTY_DIR}/glad/include"
    "${THIRD_PARTY_DIR}/imgui"
    "${THIRD_PARTY_DIR}/imgui/backends"
    "${THIRD_PARTY_DIR}/nativefiledialog/src/include"
)

target_link_libraries(HarukaEngine PUBLIC
    SDL3::SDL3
    OpenGL::GL
    assimp::assimp
    OpenSSL::Crypto
    OpenAL::OpenAL
    PostgreSQL::PostgreSQL
    PkgConfig::GTK3
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
set(SHADER_OUT_DIR "${VERSION_DIR}/bin/shaders")
file(MAKE_DIRECTORY "${SHADER_OUT_DIR}")

file(GLOB SHADER_SOURCES
    "${SHADER_SRC_DIR}/*.vert"
    "${SHADER_SRC_DIR}/*.frag"
    "${SHADER_SRC_DIR}/*.geom"
    "${SHADER_SRC_DIR}/*.comp"
)

set(SPIRV_OUTPUTS "")
foreach(SRC ${SHADER_SOURCES})
    get_filename_component(NAME ${SRC} NAME)
    set(SPV "${SHADER_OUT_DIR}/${NAME}.spv")

    if(GLSLC)
        add_custom_command(OUTPUT ${SPV}
            COMMAND ${GLSLC} --target-env=opengl ${SRC} -o ${SPV}
            DEPENDS ${SRC}
            COMMENT "glslc  ${NAME} -> ${NAME}.spv"
            VERBATIM)
    else()
        add_custom_command(OUTPUT ${SPV}
            COMMAND ${GLSLANG_V} -G ${SRC} -o ${SPV}
            DEPENDS ${SRC}
            COMMENT "glslangValidator  ${NAME} -> ${NAME}.spv"
            VERBATIM)
    endif()

    list(APPEND SPIRV_OUTPUTS ${SPV})
endforeach()

add_custom_target(HarukaShaders ALL DEPENDS ${SPIRV_OUTPUTS})
add_dependencies(HarukaEngine HarukaShaders)

# --- 6. Install Rules ---
# cmake --install build --prefix <haruka/build>
# Result: <prefix>/1.0.0/bin/libHarukaEngine.so
#         <prefix>/1.0.0/bin/shaders/*.spv
#         <prefix>/1.0.0/lib64/haruka-cpp/**.(h|cpp)
include(GNUInstallDirs)

install(TARGETS HarukaEngine
        LIBRARY DESTINATION "${PROJECT_VERSION}/bin"
        RUNTIME DESTINATION "${PROJECT_VERSION}/bin")

install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/src/"
        DESTINATION "${PROJECT_VERSION}/lib64/haruka-cpp"
        FILES_MATCHING
        PATTERN "*.h"
        PATTERN "*.hpp"
        PATTERN "*.cpp")

install(DIRECTORY "${SHADER_OUT_DIR}/"
        DESTINATION "${PROJECT_VERSION}/bin/shaders"
        FILES_MATCHING PATTERN "*.spv")
