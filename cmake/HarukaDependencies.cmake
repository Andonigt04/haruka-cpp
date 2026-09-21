# ================================================================================================
# Dependencias del sistema, cabeceras header-only y fuentes de terceros (glad, imgui, nfd)
# ================================================================================================

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
