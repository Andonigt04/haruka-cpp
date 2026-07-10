# ---------------------------------------------------------------------------------------------
# vk_resize_test — SPIKE AISLADO (no toca el motor). Objetivo: probar que la recreación de
# swapchain de Vulkan al REDIMENSIONAR es robusta bajo Wayland + SDL3 + ImGui, que es donde
# Vulkan da problemas. Standalone: su propio CMake, solo depende de SDL3, Vulkan e ImGui (reusa
# las fuentes de ImGui del motor). Configúralo y compílalo aparte:
#   cmake -S tests/vk_resize -B tests/vk_resize/build && cmake --build tests/vk_resize/build
#   ./tests/vk_resize/build/vk_resize_test
cmake_minimum_required(VERSION 3.16)
project(vk_resize_test LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(SDL3   REQUIRED)
find_package(Vulkan REQUIRED)

set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/imgui)

add_executable(vk_resize_test
    main.cpp
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdl3.cpp
    ${IMGUI_DIR}/backends/imgui_impl_vulkan.cpp
)
target_include_directories(vk_resize_test PRIVATE ${IMGUI_DIR} ${IMGUI_DIR}/backends)
target_link_libraries(vk_resize_test PRIVATE SDL3::SDL3 Vulkan::Vulkan)

# El binario y los .spv van al MISMO directorio → main.cpp los carga junto al ejecutable (SDL_GetBasePath).
set_target_properties(vk_resize_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

# --- Compilar shaders GLSL → SPIR-V con glslangValidator ---
find_program(GLSLANG NAMES glslangValidator REQUIRED)
foreach(stage tri.vert tri.frag)
    add_custom_command(
        OUTPUT  ${CMAKE_CURRENT_BINARY_DIR}/${stage}.spv
        COMMAND ${GLSLANG} -V ${CMAKE_CURRENT_SOURCE_DIR}/${stage} -o ${CMAKE_CURRENT_BINARY_DIR}/${stage}.spv
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${stage}
        COMMENT "glslang ${stage} -> ${stage}.spv")
    list(APPEND SPV ${CMAKE_CURRENT_BINARY_DIR}/${stage}.spv)
endforeach()
add_custom_target(vk_resize_shaders DEPENDS ${SPV})
add_dependencies(vk_resize_test vk_resize_shaders)
