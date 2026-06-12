cmake_minimum_required(VERSION 3.14)

# Change PROJECT_NAME to your game name (no spaces).
set(PROJECT_NAME "MyGame" CACHE STRING "Game project name")
set(VERSION      "6"      CACHE STRING "HarukaEngine build version to link against")

project(${PROJECT_NAME} VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(ENGINE_ROOT      "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(ENGINE_BUILD_DIR "${ENGINE_ROOT}/build/${VERSION}/bin")

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

# --- Plugin .so (editor loads this) ---
add_library(${PROJECT_NAME}Lib SHARED ${GAME_SOURCES})
target_include_directories(${PROJECT_NAME}Lib PRIVATE ${ENGINE_INCLUDES})
target_link_libraries(${PROJECT_NAME}Lib PRIVATE ${HarukaEngine_LIBRARY})
target_apply_network(${PROJECT_NAME}Lib)
set_target_properties(${PROJECT_NAME}Lib PROPERTIES
    OUTPUT_NAME              "${PROJECT_NAME}"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# --- Standalone executable ---
find_package(SDL3 QUIET)
add_executable(${PROJECT_NAME}Exe main.cpp ${GAME_SOURCES})
target_include_directories(${PROJECT_NAME}Exe PRIVATE ${ENGINE_INCLUDES})
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
    BUILD_RPATH              "${BIN_DIR}"
    INSTALL_RPATH            "$ORIGIN")

# Copy engine + shaders + scenes + assets to bin/ on every build
add_custom_command(TARGET ${PROJECT_NAME}Exe POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
        "${ENGINE_BUILD_DIR}/libHarukaEngine.so"
        "${BIN_DIR}/libHarukaEngine.so"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${ENGINE_BUILD_DIR}/shaders"
        "${BIN_DIR}/shaders"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/scenes"
        "${BIN_DIR}/scenes"
    # Datos del juego (assets/data/materials.json, assets/textures/, ...).
    # Resueltos en runtime con AssetPaths::data() = "assets/data/".
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/assets"
        "${BIN_DIR}/assets"
    VERBATIM)

install(TARGETS ${PROJECT_NAME}Exe RUNTIME DESTINATION .)
install(FILES     "${ENGINE_BUILD_DIR}/libHarukaEngine.so" DESTINATION .)
install(DIRECTORY "${ENGINE_BUILD_DIR}/shaders"            DESTINATION .)
install(DIRECTORY scenes                                   DESTINATION .)
install(DIRECTORY assets                                   DESTINATION .)
