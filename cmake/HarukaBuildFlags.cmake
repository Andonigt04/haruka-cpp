# ================================================================================================
# COMO se compila: tipo de build, tests si/no, flags de coma flotante, CPU objetivo, sanitizadores
# ================================================================================================
# Cada bloque lleva su historia; el orden importa (el tipo de build va antes de leerlo).

# ⚠️ SIN ESTO EL MOTOR SE COMPILA A -O0, Y NADIE LO SABÍA.
#
# `CMAKE_CXX_FLAGS_RELEASE` estaba definido abajo desde hace tiempo, pero `CMAKE_BUILD_TYPE` no se
# fijaba en NINGÚN sitio (ni aquí, ni en el juego, ni en `build.sh`, que solo pasa `-DRELEASE=ON` —
# y esa opción cambia el LAYOUT DE ASSETS, no la optimización). Resultado: todos los binarios salían
# sin `-O`, incluidos los de `./build.sh release`.
#
# Lo que costaba, medido con el mismo escenario y la misma sonda:
#     fluid.render.lamina  5,80 ms -> 0,28 ms   (21x)
#     post.composite       0,142   -> 0,003     (47x)
#     shadow.pass          2,31    -> 0,40      (5,8x)
#     barrido de props     584 ns/inst -> 35    (17x)
# O sea que cualquier juicio de rendimiento hecho hasta hoy estaba sesgado por ese factor, y varias
# "optimizaciones necesarias" se evaporan al compilar bien.
#
# Por defecto RelWithDebInfo y no Release: es -O2 CON símbolos, así que el build de todos los días
# es rápido y además se puede depurar y perfilar. `Release` (-O3 -flto) queda para el empaquetado,
# que es donde el coste de compilar con LTO tiene sentido. La CPU objetivo la fija `HARUKA_ARCH`
# (ver abajo) y es la MISMA en los dos tipos: portable por defecto.
get_property(_haruka_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT _haruka_multi_config AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Tipo de build (vacío = -O0, ver nota)" FORCE)
    message(STATUS "CMAKE_BUILD_TYPE vacío -> RelWithDebInfo (-O2 -g). Antes se compilaba a -O0.")
endif()

# ⚠️ Y SI ALGUIEN LO PONE EN `Debug`, QUE SE VEA. No se fuerza —depurar con breakpoints es
# legítimo— pero tiene que CANTAR, porque el modo en que se cuela es en silencio: alguna herramienta
# reconfigura el directorio de build a Debug y a partir de ahí todo va a -O0 sin que nadie lo note.
# Pasó cinco veces en la sesión del 2026-08-31: la suite pasó de ~1 min a no acabar en 4, y peor,
# el banco de coste del terreno dio una línea base inflada con la que se eligió mal un valor por
# defecto (se creyó que un cambio costaba +0,1 ms cuando cuesta +1,8).
# ⚠️ EL AVISO NO BASTABA, y ésta es la sexta vez. Era un `message(WARNING)`: sale una vez, al
# configurar, y a partir de ahí el directorio se queda en Debug para siempre sin decir nada más.
# Medido hoy con la misma suite y el mismo binario:
#
#     Debug (-O0)         la suite NO TERMINA en 10 min, y una pasada murió con SIGTRAP
#     RelWithDebInfo      26247 comprobaciones · 0 fallos · 52 s
#
# Casi cuatro minutos de espera por vuelta, que es lo que hace que uno deje de correr los tests.
# Así que ahora Debug hay que PEDIRLO: sigue siendo legítimo para poner breakpoints, pero tiene que
# ser una decisión escrita, no algo en lo que un directorio de build se queda de paso.
if(NOT _haruka_multi_config AND CMAKE_BUILD_TYPE STREQUAL "Debug" AND NOT HARUKA_ALLOW_DEBUG)
    message(FATAL_ERROR
        "CMAKE_BUILD_TYPE=Debug compila a -O0: la suite pasa de 52 s a no terminar en 10 min, y "
        "cualquier medida de tiempo hecha aquí es basura.\n"
        "  Para trabajar:  cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo\n"
        "  Si de verdad quieres -O0 para depurar, dilo explícitamente:\n"
        "                  cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DHARUKA_ALLOW_DEBUG=ON")
endif()

# --- Tests: NO en release ------------------------------------------------------------------------
# Andoni: "si es release que los tests no se compilen y que no quede constancia de ellos". Release es
# CUALQUIERA de las dos cosas: `CMAKE_BUILD_TYPE=Release` (el empaquetado con LTO) o `RELEASE=ON` del
# juego (el layout de assets distribuible; el juego lo define ANTES de traerse el motor para que aquí
# se vea). Con esto apagado no existe `haruka_tests`, ni `haruka_tests_rhi`, ni el cruzado del DGS,
# ni `bench_dgs_bandwidth`, ni `enable_testing()` → no hay CTestTestfile.cmake, ni binario en bin/, ni
# `ctest` que responda. La CI compila en Release y SÍ quiere los tests: los pide con -DHARUKA_BUILD_TESTS=ON.
set(_haruka_tests_default ON)
if(CMAKE_BUILD_TYPE STREQUAL "Release" OR RELEASE)
    set(_haruka_tests_default OFF)
endif()
# A propósito NO es `option()`: una opción se queda en la caché, y al pasar el mismo directorio de
# RelWithDebInfo a Release seguiría en ON (la misma caché pegada de siempre). Así sigue al tipo de
# build en cada configuración, salvo que se fije a mano con -DHARUKA_BUILD_TESTS=ON/OFF.
if(NOT DEFINED HARUKA_BUILD_TESTS)
    set(HARUKA_BUILD_TESTS ${_haruka_tests_default})
endif()
if(NOT HARUKA_BUILD_TESTS)
    message(STATUS "Tests: NO se compilan (release)")
endif()
# El juego (FetchContent) gobierna sus propios tests con la MISMA decisión: se la pasamos.
if(NOT PROJECT_IS_TOP_LEVEL)
    set(HARUKA_BUILD_TESTS ${HARUKA_BUILD_TESTS} PARENT_SCOPE)
endif()

if(NOT MSVC)
    # ⚠️ SIN `-ffast-math`, Y CON `-ffp-contract=off` GLOBAL. Medido, no supuesto: con
    # `-ffast-math` los tests de paridad FALLAN en Release —
    #     terrain_chord_error: "lo que se dibuja y lo que se pisa es la MISMA superficie (0 m)"
    #     terrain_detail_gradient: "devuelve EXACTAMENTE la misma altura que terrainDetail"
    # — que es justo lo que la nota de abajo predecía que pasaría.
    #
    # El blindaje por FICHERO era insuficiente por un motivo estructural: la aritmética del terreno
    # vive en CABECERAS (`terrain_detail.h`, `terrain_lod.h`), así que se compila dentro de cada
    # unidad que las incluya. Marcar tres `.cpp` no puede cubrirla.
    #
    # Y `-ffp-contract` en GCC vale `fast` POR DEFECTO desde -O2: la fusión `a*b+c` -> FMA ocurriría
    # aunque no hubiera `-ffast-math`. Por eso va explícito y global, no solo en Release.
    #
    # Lo que se pierde es reasociación y vectorización agresiva de coma flotante. Lo que se gana es
    # que el suelo que se dibuja y el que se pisa sigan siendo el mismo en la build que juega el
    # usuario — que es la propiedad central del proyecto, no un detalle.
    set(CMAKE_CXX_FLAGS_RELEASE "-O3 -flto")
    add_compile_options(-ffp-contract=off)

    # ── PARA QUÉ CPU SE COMPILA ─────────────────────────────────────────────────────────────────
    #
    # ⚠️ AQUÍ PONÍA `-march=native`, y eso produce un binario que SOLO ARRANCA EN ESTA MÁQUINA.
    # `native` en este equipo resuelve a `znver3` (Ryzen 7 5800H) y exige AVX2, FMA, BMI2, F16C. En
    # una CPU sin ellas no da un error legible: muere con SIGILL («instrucción ilegal») al ejecutar
    # la primera. Para `build/` daba igual —se compila y se ejecuta en el mismo sitio— pero
    # `./build.sh release` empaqueta en `dist/`, que es justo lo que se reparte.
    #
    # `x86-64-v3` es el nivel base que exige AVX2+FMA (Haswell/Excavator, 2013 en adelante): conserva
    # el grueso de la ganancia y arranca en cualquier equipo de la última década. El ajuste fino de
    # latencias se recupera con `-mtune=native`, que REORDENA instrucciones sin exigir nada nuevo al
    # hardware — o sea, gratis en portabilidad.
    #
    # `-DHARUKA_ARCH=native` para una build local a máxima velocidad, a sabiendas de que no es
    # distribuible. `-DHARUKA_ARCH=x86-64-v2` si hiciera falta llegar a máquinas de ~2009.
    #
    # Va con `add_compile_options` y no solo en los flags de Release para que el build de todos los
    # días (RelWithDebInfo) también aproveche AVX2: sin esto se compilaba al x86-64 base, o sea SSE2.
    set(HARUKA_ARCH "x86-64-v3" CACHE STRING
        "Nivel de CPU objetivo: x86-64-v3 (portable, AVX2) | native (solo esta maquina) | x86-64-v2")
    include(CheckCXXCompilerFlag)
    check_cxx_compiler_flag("-march=${HARUKA_ARCH}" HARUKA_ARCH_OK)
    if(HARUKA_ARCH_OK)
        add_compile_options(-march=${HARUKA_ARCH})
        if(NOT HARUKA_ARCH STREQUAL "native")
            add_compile_options(-mtune=native)   # solo ordena; no añade instrucciones exigidas
        endif()
        message(STATUS "CPU objetivo: -march=${HARUKA_ARCH}")
    else()
        message(WARNING "El compilador no acepta -march=${HARUKA_ARCH}; se compila al x86-64 base")
    endif()

    # ⚠️ EL CAMINO DE ALTURA DEL TERRENO SE COMPILA SIN fast-math. No es purismo: el invariante
    # render↔colisión es de 1 cm, y se cumple porque la CPU y el shader evalúan la MISMA fórmula con
    # la MISMA aritmética (ver terrain_gen.comp y `haruka_tests paridad`). `-ffast-math` autoriza a
    # GCC a reasociar sumas y —con `-march=native`, que sí tiene FMA— a fundir `a*b + c` en un FMA.
    # Las dos cosas cambian el resultado en ulps, y en el terreno un ulp de la dirección son
    # decímetros de superficie: la paridad se rompería SOLO en la build de Release, o sea justo en la
    # que juega el usuario, y los tests (que se compilan sin BUILD_TYPE) seguirían en verde.
    # Coste: estos ficheros pierden vectorización agresiva; el grueso del tiempo de generación está en
    # la GPU, no aquí. Si algún día hace falta, se mide antes de tocarlo.
    set_source_files_properties(
        "${CMAKE_CURRENT_SOURCE_DIR}/src/world/noise_generator.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/world/planet/planet_geology.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/world/terrain/cube_sphere.cpp"
        DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        PROPERTIES COMPILE_OPTIONS "-fno-fast-math;-ffp-contract=off")
endif()

# Sanitizadores (diagnóstico, apagado por defecto). Para cazar QUIÉN pisa memoria ajena o qué acceso
# entre hilos no está sincronizado, en vez de deducirlo del volcado: el sanitizador señala la línea.
#   cmake -B build-asan -DHARUKA_SANITIZE=address   → escrituras fuera de rango, uso tras liberar
#   cmake -B build-tsan -DHARUKA_SANITIZE=thread    → carreras de datos
# Van en un directorio de build APARTE: no se mezclan con el build normal.
set(HARUKA_SANITIZE "" CACHE STRING "Sanitizador: vacío, 'address' o 'thread'")
if(HARUKA_SANITIZE AND NOT MSVC)
    message(STATUS "Sanitizador ACTIVO: ${HARUKA_SANITIZE} (build de diagnóstico, más lento)")
    add_compile_options(-fsanitize=${HARUKA_SANITIZE} -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=${HARUKA_SANITIZE})
endif()
