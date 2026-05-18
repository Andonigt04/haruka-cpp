cmake_minimum_required(VERSION 3.14)

set(VERSION "0.6.0" CACHE STRING "HarukaEngine build version to link against")
project(ServerTest VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(ENGINE_ROOT      "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(ENGINE_BUILD_DIR "${ENGINE_ROOT}/build/${VERSION}/bin")

# Default install prefix = project's own bin/ (cmake --install build)
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "${CMAKE_BINARY_DIR}/bin" CACHE PATH "" FORCE)
endif()

find_library(HarukaEngine_LIBRARY NAMES HarukaEngine
    PATHS ${ENGINE_BUILD_DIR} NO_DEFAULT_PATH REQUIRED)

file(GLOB_RECURSE GAME_SOURCES CONFIGURE_DEPENDS scripts/*.cpp)
if(NOT GAME_SOURCES)
    message(FATAL_ERROR "No source files found in scripts/")
endif()

set(ENGINE_INCLUDES
    "${ENGINE_ROOT}/src"
    "${ENGINE_ROOT}/third_party/glm"
    "${ENGINE_ROOT}/third_party/json/include"
    "${ENGINE_ROOT}/third_party/glad/include"
    "${ENGINE_ROOT}/third_party/imgui"
)

# --- DGS (optional) ---
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

# --- Plugin .so — editor lo carga desde build/ del proyecto ---
add_library(ServerTestLib SHARED ${GAME_SOURCES})
target_include_directories(ServerTestLib PRIVATE ${ENGINE_INCLUDES})
target_link_libraries(ServerTestLib PRIVATE ${HarukaEngine_LIBRARY})
target_apply_network(ServerTestLib)
set_target_properties(ServerTestLib PROPERTIES
    OUTPUT_NAME              "ServerTest"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# --- Standalone exe ---
find_package(SDL3 QUIET)
add_executable(ServerTestExe main.cpp ${GAME_SOURCES})
target_include_directories(ServerTestExe PRIVATE ${ENGINE_INCLUDES})
target_link_libraries(ServerTestExe PRIVATE ${HarukaEngine_LIBRARY})
if(SDL3_FOUND)
    target_link_libraries(ServerTestExe PRIVATE SDL3::SDL3)
else()
    find_library(SDL3_LIB NAMES SDL3)
    if(SDL3_LIB)
        target_link_libraries(ServerTestExe PRIVATE ${SDL3_LIB})
    endif()
endif()
target_apply_network(ServerTestExe)
set(BIN_DIR "${CMAKE_BINARY_DIR}/bin")

set_target_properties(ServerTestExe PROPERTIES
    OUTPUT_NAME   "server_test"
    RUNTIME_OUTPUT_DIRECTORY "${BIN_DIR}"
    BUILD_RPATH   "${BIN_DIR}"
    INSTALL_RPATH "$ORIGIN")

# Copia engine + shaders + scenes al bin/ del proyecto en cada build
add_custom_command(TARGET ServerTestExe POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
        "${ENGINE_BUILD_DIR}/libHarukaEngine.so"
        "${BIN_DIR}/libHarukaEngine.so"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${ENGINE_BUILD_DIR}/shaders"
        "${BIN_DIR}/shaders"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/scenes"
        "${BIN_DIR}/scenes"
    VERBATIM)

# Release: cmake --install build --prefix dist/
install(TARGETS ServerTestExe RUNTIME DESTINATION .)
install(FILES     "${ENGINE_BUILD_DIR}/libHarukaEngine.so" DESTINATION .)
install(DIRECTORY "${ENGINE_BUILD_DIR}/shaders"            DESTINATION .)
install(DIRECTORY scenes                                   DESTINATION .)
