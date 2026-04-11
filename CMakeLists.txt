cmake_minimum_required(VERSION 3.14)
project(HarukaEngine)

set(OpenGL_GL_PREFERENCE GLVND)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ===== Optimization Flags =====
if(MSVC)
    # Visual Studio optimizations
    set(CMAKE_CXX_FLAGS_RELEASE "/O2 /Oi /Ot /Oy /GL /fp:fast /arch:AVX2")
    set(CMAKE_EXE_LINKER_FLAGS_RELEASE "/LTCG /OPT:REF /OPT:ICF")
else()
    # GCC/Clang optimizations
    set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -mtune=native -flto -ffast-math -fno-signed-zeros -fno-trapping-math")
    set(CMAKE_EXE_LINKER_FLAGS_RELEASE "-O3 -flto")
    set(CMAKE_SHARED_LINKER_FLAGS_RELEASE "-O3 -flto")
endif()

# Default to Release build
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

include(FetchContent)

set(THIRD_PARTY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party")
set(GLAD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/glad")
set(GLM_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/glm")
set(STB_DIR "${THIRD_PARTY_DIR}/stb")
set(IMGUI_DIR "${THIRD_PARTY_DIR}/imgui")
set(WEBSOCKETPP_DIR "${THIRD_PARTY_DIR}/websocketpp")
set(OPENAL_DIR "${THIRD_PARTY_DIR}/OpenAL")
set(JSON_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/json")
set(NFD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/nativefiledialog")

set(NFD_SOURCES
    ${NFD_DIR}/src/nfd_common.c
    ${NFD_DIR}/src/nfd_gtk.c
)

set(NFD_INCLUDE_DIRS "${NFD_DIR}/src/include")
set(GTK3_INCLUDE_DIRS ${GTK3_INCLUDE_DIRS})

find_package(glfw3 REQUIRED)
find_package(assimp REQUIRED)
find_package(OpenGL REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(OpenAL REQUIRED)
find_package(PostgreSQL REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK3 REQUIRED gtk+-3.0)

include_directories(
  src
  ${GLAD_DIR}/include
  ${GLM_DIR}
  ${STB_DIR}
  ${IMGUI_DIR}
  ${IMGUI_DIR}/backends
  ${ASSIMP_INCLUDE_DIRS}
  ${WEBSOCKETPP_DIR}
  ${OPENAL_INCLUDE_DIR}
  ${PostgreSQL_INCLUDE_DIRS}
  ${NLOHMANN_DIR}
  ${JSON_DIR}/include
  ${NFD_DIR}/src/include
  ${GTK3_INCLUDE_DIRS}
)

file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/shaders" DESTINATION "${CMAKE_CURRENT_BINARY_DIR}")

# ImGui sources
set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
)

# Engine sources
file(GLOB_RECURSE ENGINE_SOURCES 
    "src/core/*.cpp" 
    "src/renderer/*.cpp"
    "src/io/*.cpp"
    "src/physics/*.cpp"
    "src/audio/*.cpp"
    "src/game/*.cpp"
    "src/network/*.cpp"
    "src/database/*.cpp"
)

# Network sources (cliente + servidor)
file(GLOB_RECURSE NETWORK_SOURCES
    "src/network/*.cpp"
)

# Database sources
file(GLOB_RECURSE DATABASE_SOURCES
    "src/database/*.cpp"
)

set(GLAD_SOURCE "${GLAD_DIR}/src/glad.c")

# ==== Librería compartida del Motor ====
add_library(HarukaEngine SHARED
    ${ENGINE_SOURCES}
    ${GLAD_SOURCE}
    ${IMGUI_SOURCES}
)

# Enable optimizations for HarukaEngine
set_target_properties(HarukaEngine PROPERTIES
    INTERPROCEDURAL_OPTIMIZATION TRUE
    POSITION_INDEPENDENT_CODE ON
    VISIBILITY_INLINES_HIDDEN ON
)

target_include_directories(HarukaEngine PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(HarukaEngine PUBLIC
    glfw 
    OpenGL::GL
    assimp::assimp
    OpenSSL::Crypto
    ${OPENAL_LIBRARY}
    ${CMAKE_DL_LIBS}
    PostgreSQL::PostgreSQL
)

install(TARGETS HarukaEngine
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

option(BUILD_RUNTIME "Build HarukaEngine" ON)

# ==== Install rules ====
include(GNUInstallDirs)

install(TARGETS HarukaEngine LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})

install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/renderer"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/io"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/physics"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/audio"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/game"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/network"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/database"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/engines/haruka-cpp"
        FILES_MATCHING PATTERN "*.h" PATTERN "*.cpp"
)
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/shaders DESTINATION ${CMAKE_INSTALL_BINDIR})

# ==== CPack ====
set(CPACK_PACKAGE_NAME "HarukaEngine")
set(CPACK_PACKAGE_VERSION "0.1.0")
set(CPACK_GENERATOR "ZIP;DEB")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Haruka")
include(CPack)