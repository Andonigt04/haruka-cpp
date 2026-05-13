cmake_minimum_required(VERSION 3.14)
project(PROJECT_NAME_PLACEHOLDER)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Rutas al engine principal
set(ENGINE_ROOT "/mnt/sdb1/haruka")
set(ENGINE_BUILD_DIR "${ENGINE_ROOT}/build")

# Buscar librería del engine
find_library(HarukaEngine_LIBRARY
    NAMES HarukaEngine
    PATHS
        ${ENGINE_BUILD_DIR}
        ${ENGINE_BUILD_DIR}/lib
    NO_DEFAULT_PATH
)

if(NOT HarukaEngine_LIBRARY)
    message(FATAL_ERROR "HarukaEngine not found. Compile the engine first.")
endif()

find_path(HarukaEngineLib_INCLUDE_DIR
    NAMES core/scene.h
    PATHS ${ENGINE_ROOT}/src
    NO_DEFAULT_PATH
    REQUIRED
)

include_directories(
    ${HarukaEngineLib_INCLUDE_DIR}
    ${ENGINE_ROOT}/third_party/glm
    ${ENGINE_ROOT}/third_party/json/include
    ${ENGINE_ROOT}/third_party/glad/include
)

file(GLOB_RECURSE PROJECT_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/scripts/*.cpp"
)

if(PROJECT_SOURCES STREQUAL "")
    message(FATAL_ERROR "No source files found in scripts/*.cpp")
endif()

add_library(PROJECT_NAME_PLACEHOLDER SHARED ${PROJECT_SOURCES})
target_link_libraries(PROJECT_NAME_PLACEHOLDER PRIVATE ${HarukaEngineLib_LIBRARY})
set_target_properties(PROJECT_NAME_PLACEHOLDER PROPERTIES OUTPUT_NAME "PROJECT_NAME_PLACEHOLDER")

# El editor/motor carga ./libPROJECT_NAME_PLACEHOLDER.so desde ${ENGINE_BUILD_DIR}
set_target_properties(Test PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${ENGINE_BUILD_DIR}"
    RUNTIME_OUTPUT_DIRECTORY "${ENGINE_BUILD_DIR}"
)
