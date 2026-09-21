# ================================================================================================
# version.h: version del motor + hash de git, regenerado en cada compilacion
# ================================================================================================
# (incluido desde CMakeLists.txt justo despues de project(); ver la nota de por que DESPUES)

# --- version.h -----------------------------------------------------------------------------------
# ⚠️ Esto va DESPUES de project() a proposito. Antes se generaba en la linea 21, o sea ANTES de que
# project() definiera PROJECT_VERSION — asi que `HARUKA_VERSION` no cogia la version del MOTOR sino la
# del proyecto padre: compilado dentro de Survival decia "0.2" (la de Survival) y compilado suelto
# decia "" (vacio, porque aun no habia ninguna). Y como no lo incluia nadie, nunca se noto.
#
# El hash, ademas, se capturaba al CONFIGURAR, asi que entre commit y commit el binario seguia
# diciendo el anterior. Ahora lo regenera un target en cada compilacion (ver cmake/version_gen.cmake,
# que solo reescribe el fichero si cambia para no arrastrar recompilaciones).
set(HARUKA_VERSION_IN  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.h.in")
set(HARUKA_VERSION_OUT "${CMAKE_CURRENT_BINARY_DIR}/generated/version.h")
set(HARUKA_VERSION_ARGS
    -DSRC_DIR=${CMAKE_CURRENT_SOURCE_DIR}
    -DIN_FILE=${HARUKA_VERSION_IN}
    -DOUT_FILE=${HARUKA_VERSION_OUT}
    -DPROJECT_VERSION=${PROJECT_VERSION}
    -DPROJECT_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    -DPROJECT_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    -DPROJECT_VERSION_PATCH=${PROJECT_VERSION_PATCH}
    -DHARUKA_BUILD_NUMBER=${HARUKA_BUILD_NUMBER}
)
# Una pasada al configurar para que el fichero EXISTA antes de la primera compilacion.
execute_process(COMMAND ${CMAKE_COMMAND} ${HARUKA_VERSION_ARGS} -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/version_gen.cmake")
add_custom_target(haruka_version ALL
    COMMAND ${CMAKE_COMMAND} ${HARUKA_VERSION_ARGS} -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/version_gen.cmake"
    COMMENT "version.h (hash de git del momento)"
)
