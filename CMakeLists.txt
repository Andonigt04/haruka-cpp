cmake_minimum_required(VERSION 3.14)
project(HarukaEngine VERSION 0.9.0)

include(GNUInstallDirs)
set(CMAKE_INSTALL_LIBDIR "lib64" CACHE PATH "Directory for libraries" FORCE)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- Optimización Release ---
if(NOT MSVC)
    set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -flto -ffast-math")
endif()

set(THIRD_PARTY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party")

# --- 1. Búsqueda de Dependencias (Sistema) ---
find_package(PkgConfig REQUIRED)
find_package(Vulkan REQUIRED)
find_package(SDL3 REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(OpenAL REQUIRED)
find_package(PostgreSQL REQUIRED)

# Asio es vital para websocketpp
find_path(ASIO_INCLUDE_DIR asio.hpp)
if(NOT ASIO_INCLUDE_DIR)
    message(FATAL_ERROR "ASIO no encontrado. Instálalo con: sudo dnf install asio-devel")
endif()

pkg_check_modules(GTK3 REQUIRED IMPORTED_TARGET gtk+-3.0)

# --- 2. Macro de Fallback Inteligente ---
macro(find_dependency_with_fallback NAME HEADER_FILE LOCAL_PATH)
    find_path(${NAME}_INCLUDE_DIR ${HEADER_FILE} 
              PATH_SUFFIXES ${NAME} imgui nfd websocketpp json)
    
    if(NOT ${NAME}_INCLUDE_DIR)
        message(STATUS "-- Usando copia local de ${NAME} en ${LOCAL_PATH}")
        set(${NAME}_INCLUDE_DIR "${THIRD_PARTY_DIR}/${LOCAL_PATH}")
        set(${NAME}_LOCAL TRUE)
    else()
        message(STATUS "-- Usando ${NAME} del sistema")
        set(${NAME}_LOCAL FALSE)
    endif()
endmacro()

find_dependency_with_fallback(IMGUI "imgui.h" "imgui")
find_dependency_with_fallback(STB "stb_image.h" "stb")
find_dependency_with_fallback(GLM "glm/glm.hpp" "glm")
find_dependency_with_fallback(NFD "nfd.h" "nativefiledialog/src/include")
find_dependency_with_fallback(NLOHMANN_JSON "nlohmann/json.hpp" "json/include")
find_dependency_with_fallback(WEBSOCKETPP "websocketpp/client.hpp" "websocketpp")

# --- 3. Fuentes ---
file(GLOB_RECURSE ENGINE_SOURCES "src/*.cpp")

# Fuentes de ImGui (si es local)
if(IMGUI_LOCAL)
    file(GLOB IMGUI_SOURCES "${IMGUI_INCLUDE_DIR}/*.cpp")
endif()

# Fuentes de NFD (necesario para compilar en Linux)
if(NFD_LOCAL)
    set(NFD_SOURCES 
        "${THIRD_PARTY_DIR}/nativefiledialog/src/nfd_common.c"
        "${THIRD_PARTY_DIR}/nativefiledialog/src/nfd_gtk.c"
    )
endif()

# --- 4. Target ---
add_library(HarukaEngine SHARED 
    ${ENGINE_SOURCES} 
    ${IMGUI_SOURCES} 
    ${NFD_SOURCES}
)

# Definiciones críticas
target_compile_definitions(HarukaEngine PUBLIC ASIO_STANDALONE)

target_include_directories(HarukaEngine PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    ${Vulkan_INCLUDE_DIRS}
    ${SDL3_INCLUDE_DIRS}
    ${PostgreSQL_INCLUDE_DIRS}
    ${IMGUI_INCLUDE_DIR}
    ${STB_INCLUDE_DIR}
    ${GLM_INCLUDE_DIR}
    ${NFD_INCLUDE_DIR}
    ${NLOHMANN_JSON_INCLUDE_DIR}
    ${WEBSOCKETPP_INCLUDE_DIR}
    ${ASIO_INCLUDE_DIR}
)

target_link_libraries(HarukaEngine PUBLIC
    Vulkan::Vulkan
    SDL3::SDL3
    OpenSSL::Crypto
    OpenAL::OpenAL
    PostgreSQL::PostgreSQL
    PkgConfig::GTK3
    ${CMAKE_DL_LIBS}
)

# --- 5. Shaders ---
file(GLOB SHADER_SOURCES 
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.vert" 
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.frag"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.comp"
)
set(SPIRV_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders")
file(MAKE_DIRECTORY ${SPIRV_OUTPUT_DIR})

foreach(SHADER ${SHADER_SOURCES})
    get_filename_component(FILE_NAME ${SHADER} NAME)
    set(SPIRV_FILE "${SPIRV_OUTPUT_DIR}/${FILE_NAME}.spv")
    add_custom_command(
        OUTPUT ${SPIRV_FILE}
        COMMAND glslangValidator -V ${SHADER} -o ${SPIRV_FILE}
        DEPENDS ${SHADER}
        COMMENT "Compilando shader ${FILE_NAME}"
    )
    list(APPEND SPIRV_SHADERS ${SPIRV_FILE})
endforeach()
add_custom_target(compile_shaders ALL DEPENDS ${SPIRV_SHADERS})

# --- 6. Instalación ---
install(TARGETS HarukaEngine
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# Instalar todos los headers (.h) en una carpeta específica
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/src/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/haruka-cpp
    FILES_MATCHING PATTERN "*.h"
)