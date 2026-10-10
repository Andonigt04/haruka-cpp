# Planet Demo — standalone example
add_executable(planet_demo main.cpp)
target_link_libraries(planet_demo PRIVATE HarukaEngine)
set_target_properties(planet_demo PROPERTIES
    BUILD_RPATH   "$ORIGIN"
    INSTALL_RPATH "$ORIGIN")