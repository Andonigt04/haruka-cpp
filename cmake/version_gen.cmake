# Regenera version.h EN CADA COMPILACION, no solo al configurar.
#
# ⚠️ Existe porque el hash se capturaba con execute_process() al configurar, y eso solo vuelve a
# correr cuando CMake se reconfigura. Entre commit y commit el binario seguia diciendo el hash
# ANTERIOR: una cadena de version que afirma un commit que no es lo que se esta ejecutando, que es
# justo lo que la version tenia que evitar. Se llama desde un custom target con -P.
#
# Solo REESCRIBE el fichero si el contenido cambia. Si se escribiera siempre, su marca de tiempo
# cambiaria en cada build y arrastraria a recompilar todo lo que lo incluye.

execute_process(
    COMMAND git rev-parse --short HEAD
    WORKING_DIRECTORY "${SRC_DIR}"
    OUTPUT_VARIABLE HARUKA_BUILD_HASH
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT HARUKA_BUILD_HASH)
    set(HARUKA_BUILD_HASH "unknown")
endif()

# Arbol sucio = el binario NO es ese commit. Sin esta marca, una compilacion con cambios sin
# confirmar se presenta como el commit limpio y la cadena miente.
execute_process(
    COMMAND git status --porcelain --untracked-files=no
    WORKING_DIRECTORY "${SRC_DIR}"
    OUTPUT_VARIABLE _haruka_dirty
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(_haruka_dirty)
    set(HARUKA_BUILD_HASH "${HARUKA_BUILD_HASH}-dirty")
endif()

configure_file("${IN_FILE}" "${OUT_FILE}.tmp" @ONLY)

set(_same FALSE)
if(EXISTS "${OUT_FILE}")
    file(READ "${OUT_FILE}"     _old)
    file(READ "${OUT_FILE}.tmp" _new)
    if(_old STREQUAL _new)
        set(_same TRUE)
    endif()
endif()
if(NOT _same)
    file(RENAME "${OUT_FILE}.tmp" "${OUT_FILE}")
else()
    file(REMOVE "${OUT_FILE}.tmp")
endif()
