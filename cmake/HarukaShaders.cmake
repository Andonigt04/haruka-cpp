# ================================================================================================
# GLSL -> SPIR-V: shaders del motor, librerias lib/*.glsl y los del planeta; target HarukaShaders
# ================================================================================================
# Se incluye DESPUES de add_library(HarukaEngine): al final cuelga HarukaShaders del motor.

# --- 5. GLSL -> SPIR-V Shader Compilation ---
find_program(GLSLC     glslc)
find_program(GLSLANG_V glslangValidator)

if(NOT GLSLC AND NOT GLSLANG_V)
    message(FATAL_ERROR
        "glslc or glslangValidator is required to compile SPIR-V shaders.\n"
        "  Fedora/RHEL: sudo dnf install glslang\n"
        "  Ubuntu/Deb:  sudo apt install glslang-tools")
endif()

# Fuentes de shaders del motor bajo assets/ (raíz de assets del motor: aquí también
# irían texturas/fuentes por defecto que el motor traiga de serie). El runtime los busca
# en assets/shaders/ (AssetPaths::shaders()), así que source e install coinciden.
set(SHADER_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/assets/shaders")
if(NOT SHADER_OUT_DIR)
    set(SHADER_OUT_DIR "${CMAKE_BINARY_DIR}/assets/shaders")
endif()
file(MAKE_DIRECTORY "${SHADER_OUT_DIR}")

# CONFIGURE_DEPENDS por la MISMA razón que en SHADER_LIBS más abajo: sin él el glob solo se evalúa al
# configurar, así que un shader NUEVO no se compila hasta relanzar cmake a mano — y el fallo es mudo,
# porque falta el .spv, el pipeline no se crea y lo que fuera a dibujar simplemente no aparece. Se
# añadió tras perder un rato con exactamente eso al portar el preview de items al RHI.
file(GLOB SHADER_SOURCES CONFIGURE_DEPENDS
    "${SHADER_SRC_DIR}/*.vert"
    "${SHADER_SRC_DIR}/*.frag"
    "${SHADER_SRC_DIR}/*.geom"
    "${SHADER_SRC_DIR}/*.comp"
)

# No compiles los shaders de un módulo apagado (su renderer también está excluido → no se usan).
if(NOT HARUKA_MOD_FLUIDS)
    list(FILTER SHADER_SOURCES EXCLUDE REGEX "/(fluid_[^/]*|water)\\.(vert|frag)$")
endif()
if(NOT HARUKA_MOD_PARTICLES)
    list(FILTER SHADER_SOURCES EXCLUDE REGEX "/particle\\.(vert|frag)$")
endif()
# (El filtro de HARUKA_MOD_SHADOWS excluía shadow.* y point_shadow.*. Los dos se borraron por no
#  tener call-site: el pase de sombra real usa el shader del JUEGO —Survival/assets/shaders/
#  shadow_depth.*— y las sombras puntuales nunca se llegaron a construir. El filtro quedaba vacío.)
if(NOT HARUKA_MOD_SOFTBODY)
    list(FILTER SHADER_SOURCES EXCLUDE REGEX "/softbody\\.(vert|frag)$")
endif()

# Las librerías compartidas (assets/shaders/lib/*.glsl) NO se compilan por su cuenta: son
# fragmentos que otros shaders incluyen. Se copian al directorio de salida —el RHI las resuelve en
# tiempo de carga— y se declaran como DEPENDS para que tocar una recompile a quien la usa.
# CONFIGURE_DEPENDS: sin él el glob solo se evalúa al configurar, así que una librería NUEVA no se
# copia hasta volver a lanzar cmake a mano. El síntoma es idéntico al de la nota de abajo pero peor:
# el `#include` no se encuentra, el pipeline no linka y el objeto simplemente no se dibuja — sin
# error de dibujado, porque el fallo fue al crear el pipeline y el camino de respaldo lo tapa.
#
# Los del PLANETA (assets/shaders/planet/) SÍ se compilan a SPIR-V: el RHI los carga por ruta en
# tiempo de ejecución (`PipelineDesc::*Path`), y entre ellos hay .tesc/.tese, que el glob plano de
# arriba no recoge. PARIDAD GL=VK por construcción: el mismo GLSL → el mismo glslang/glslc, con el
# target de GL (bindings literales). El runtime GL prefiere este .spv; el GLSL del driver solo queda
# como fallback para GPUs sin GL_ARB_gl_spirv. (Antes solo se copiaban: el clipmap —el único que
# aporta detalle fino— caía siempre al compilador GLSL del driver, divergiendo de Vulkan.)
file(GLOB SHADER_LIBS CONFIGURE_DEPENDS
    "${SHADER_SRC_DIR}/lib/*.glsl")
list(FILTER SHADER_SOURCES EXCLUDE REGEX "/(lib|planet)/")
# ⚠️ `.comp` incluido: el culling de parches (planet/terrain_cull.comp) es un COMPUTE, y sin él en
# este glob el fichero no se copiaba al build ni se compilaba a SPIR-V. El síntoma era
# "createPipeline: falta la etapa COMPUTE" — el motor pedía una ruta que no existía en bin/.
file(GLOB PLANET_SHADERS CONFIGURE_DEPENDS
    "${SHADER_SRC_DIR}/planet/*.vert" "${SHADER_SRC_DIR}/planet/*.frag"
    "${SHADER_SRC_DIR}/planet/*.tesc" "${SHADER_SRC_DIR}/planet/*.tese"
    "${SHADER_SRC_DIR}/planet/*.comp")
# Copia en tiempo de BUILD, no de configure. Con `configure_file` la copia solo se rehace al
# re-ejecutar cmake, así que editar una librería y compilar dejaba el shader usando la versión
# ANTERIOR — y el síntoma es el peor posible: el cambio "no hace nada" y parece que la hipótesis
# era falsa. Costó una iteración entera en la paridad del terreno.
#
# Se preserva el subdirectorio (lib/ o planet/), porque las rutas que pide el motor lo incluyen.
set(SHADER_LIB_OUTPUTS "")
foreach(LIB ${SHADER_LIBS})
    file(RELATIVE_PATH LIBREL "${SHADER_SRC_DIR}" "${LIB}")
    set(LIBOUT "${SHADER_OUT_DIR}/${LIBREL}")
    add_custom_command(OUTPUT ${LIBOUT}
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${LIB} ${LIBOUT}
        DEPENDS ${LIB}
        COMMENT "shader lib  ${LIBREL}"
        VERBATIM)
    list(APPEND SHADER_LIB_OUTPUTS ${LIBOUT})
endforeach()

set(SPIRV_OUTPUTS "")
foreach(SRC ${SHADER_SOURCES})
    get_filename_component(NAME ${SRC} NAME)
    set(SPV     "${SHADER_OUT_DIR}/${NAME}.spv")
    set(DST_SRC "${SHADER_OUT_DIR}/${NAME}")

    if(GLSLC)
        add_custom_command(OUTPUT ${SPV}
            COMMAND ${GLSLC} --target-env=opengl -I${SHADER_SRC_DIR} ${SRC} -o ${SPV}
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SRC} ${DST_SRC}
            DEPENDS ${SRC} ${SHADER_LIBS}
            COMMENT "glslc  ${NAME} -> ${NAME}.spv"
            VERBATIM)
    else()
        add_custom_command(OUTPUT ${SPV}
            COMMAND ${GLSLANG_V} -G -I${SHADER_SRC_DIR} ${SRC} -o ${SPV}
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SRC} ${DST_SRC}
            DEPENDS ${SRC} ${SHADER_LIBS}
            COMMENT "glslangValidator  ${NAME} -> ${NAME}.spv"
            VERBATIM)
    endif()

    list(APPEND SPIRV_OUTPUTS ${SPV})
endforeach()

# Shaders del PLANETA → SPIR-V GL, preservando el subdirectorio planet/ (el RHI pide la ruta
# completa `assets/shaders/planet/*.spv`). Igual target que el resto: bindings literales GL.
foreach(SRC ${PLANET_SHADERS})
    get_filename_component(NAME ${SRC} NAME)
    set(SPV "${SHADER_OUT_DIR}/planet/${NAME}.spv")
    set(DST_SRC "${SHADER_OUT_DIR}/planet/${NAME}")

    if(GLSLC)
        add_custom_command(OUTPUT ${SPV}
            COMMAND ${CMAKE_COMMAND} -E make_directory "${SHADER_OUT_DIR}/planet"
            COMMAND ${GLSLC} --target-env=opengl -I${SHADER_SRC_DIR} ${SRC} -o ${SPV}
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SRC} ${DST_SRC}
            DEPENDS ${SRC} ${SHADER_LIBS}
            COMMENT "glslc  planet/${NAME} -> planet/${NAME}.spv"
            VERBATIM)
    else()
        add_custom_command(OUTPUT ${SPV}
            COMMAND ${CMAKE_COMMAND} -E make_directory "${SHADER_OUT_DIR}/planet"
            COMMAND ${GLSLANG_V} -G -I${SHADER_SRC_DIR} ${SRC} -o ${SPV}
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SRC} ${DST_SRC}
            DEPENDS ${SRC} ${SHADER_LIBS}
            COMMENT "glslangValidator  planet/${NAME} -> planet/${NAME}.spv"
            VERBATIM)
    endif()

    list(APPEND SPIRV_OUTPUTS ${SPV})
endforeach()

add_custom_target(HarukaShaders ALL DEPENDS ${SPIRV_OUTPUTS} ${SHADER_LIB_OUTPUTS})
add_dependencies(HarukaEngine HarukaShaders)
