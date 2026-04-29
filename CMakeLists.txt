cmake_minimum_required(VERSION 3.14)
project(HarukaEngine)

set(OpenGL_GL_PREFERENCE GLVND)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- Optimización Release ---
if(NOT MSVC)
    set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -flto -ffast-math")
endif()

set(THIRD_PARTY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party")

# --- 1. Búsqueda de Dependencias (Sistema) ---
find_package(PkgConfig REQUIRED)
find_package(SDL3 REQUIRED)
find_package(OpenGL REQUIRED)
find_package(assimp REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(OpenAL REQUIRED)
find_package(PostgreSQL REQUIRED)

# GTK3 via PkgConfig
pkg_check_modules(GTK3 REQUIRED IMPORTED_TARGET gtk+-3.0)

# --- 2. Macro de Fallback para Librerías Header-Only ---
# Esta macro busca en el sistema y si falla, apunta a third_party
macro(find_header_fallback NAME HEADER_FILE SUBDIR)
    find_path(${NAME}_INCLUDE_DIR ${HEADER_FILE}
        PATH_SUFFIXES ${SUBDIR}
        PATHS ${THIRD_PARTY_DIR}/${SUBDIR}
    )
    if(${NAME}_INCLUDE_DIR)
        message(STATUS "Encontrado ${NAME}: ${${NAME}_INCLUDE_DIR}")
        include_directories(${${NAME}_INCLUDE_DIR})
    else()
        message(FATAL_ERROR "${NAME} no encontrado. Instala el paquete -devel o ponlo en third_party")
    endif()
endmacro()

# Aplicar fallbacks para las dependencias que te daban error
find_header_fallback(NLOHMANN_JSON "nlohmann/json.hpp" "json")
find_header_fallback(GLM "glm/glm.hpp" "glm")
find_header_fallback(STB "stb_image.h" "stb")

# --- 3. Configuración de Fuentes ---

# GLAD (Cargador OpenGL - siempre desde source para evitar líos de versión)
set(GLAD_SOURCE "${THIRD_PARTY_DIR}/glad/src/glad.c")

# ImGui (Backends SDL3 + OpenGL3)
set(IMGUI_SOURCES
    ${THIRD_PARTY_DIR}/imgui/imgui.cpp
    ${THIRD_PARTY_DIR}/imgui/imgui_draw.cpp
    ${THIRD_PARTY_DIR}/imgui/imgui_tables.cpp
    ${THIRD_PARTY_DIR}/imgui/imgui_widgets.cpp
    ${THIRD_PARTY_DIR}/imgui/backends/imgui_impl_sdl3.cpp
    ${THIRD_PARTY_DIR}/imgui/backends/imgui_impl_opengl3.cpp
)

# Native File Dialog
set(NFD_SOURCES
    ${THIRD_PARTY_DIR}/nativefiledialog/src/nfd_common.c
    ${THIRD_PARTY_DIR}/nativefiledialog/src/nfd_gtk.c
)

file(GLOB_RECURSE ENGINE_SOURCES "src/*.cpp")

# --- 4. Target Principal ---
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

# --- 5. Shaders (OpenGL usa archivos de texto, no requiere compilar a SPIR-V) ---
# Copiamos los archivos .vert y .frag directamente al binario
file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/shaders" DESTINATION "${CMAKE_CURRENT_BINARY_DIR}")

# --- 6. Reglas de Instalación ---
include(GNUInstallDirs)
install(TARGETS HarukaEngine LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# Instalar headers del motor
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/src/"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/engines/haruka-cpp"
        FILES_MATCHING PATTERN "*.h" PATTERN "*.cpp")