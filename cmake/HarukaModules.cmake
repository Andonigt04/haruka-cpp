# ================================================================================================
# Modulos en tiempo de compilacion (MODULES=1011...): mascara, dependencias y src/core/modules.h
# ================================================================================================

# --- 3b. Build-time modules (MODULES=1011...) ---
# Bitmask string, one char per module, in this FIXED order. '1'=on, '0'=off.
# Absent/short string → missing positions default ON. See docs/MODULES.md.
set(HARUKA_MODULE_LIST
    FLUIDS      # 0: ocean + shallow-water + PBF + hybrid
    DEFORM      # 1: terrain deformation field
    SOFTBODY    # 2: XPBD cloth/jelly/wind
    NETWORK     # 3: DGS client
    AUDIO       # 4: audio manager
    PARTICLES   # 5: reserved (particle system)
    TERRAIN     # 6: planetary terrain (default-ON; off = no planet)
    POSTFX      # 7: bloom/ssao/hdr/ibl
    SHADOWS     # 8: shadow maps
    PHYSICS     # 9: rigid-body physics engine + colliders (default-ON; off = no PhysicsEngine)
)
set(MODULES "" CACHE STRING "Per-module on/off bitmask, e.g. 101101111")

list(LENGTH HARUKA_MODULE_LIST _mod_count)
set(_module_defines "")
math(EXPR _last "${_mod_count} - 1")
foreach(_i RANGE ${_last})
    list(GET HARUKA_MODULE_LIST ${_i} _mod)
    string(LENGTH "${MODULES}" _mlen)
    set(_bit "1") # default ON
    if(_i LESS _mlen)
        string(SUBSTRING "${MODULES}" ${_i} 1 _bit)
    endif()
    if(_bit STREQUAL "0")
        set(HARUKA_MOD_${_mod} OFF)
    else()
        set(HARUKA_MOD_${_mod} ON)
        string(APPEND _module_defines "#define HARUKA_MOD_${_mod} 1\n")
    endif()
    message(STATUS "  module ${_mod} = ${HARUKA_MOD_${_mod}}")
endforeach()

# --- Module dependencies ---
# "<MOD>_REQUIRES" lists modules that MOD needs. If MOD is ON but a requirement
# is OFF, auto-enable the requirement (with a warning) so the build stays valid.
# (PARTICLES = sistema de efectos GL_POINTS standalone, sin dependencias.
#  DEFORM, SOFTBODY, AUDIO, NETWORK son self-contained — no requirements.)

foreach(_mod ${HARUKA_MODULE_LIST})
    if(HARUKA_MOD_${_mod})
        foreach(_dep ${${_mod}_REQUIRES})
            if(NOT HARUKA_MOD_${_dep})
                message(WARNING "Module ${_mod} requires ${_dep}; auto-enabling ${_dep}.")
                set(HARUKA_MOD_${_dep} ON)
                string(APPEND _module_defines "#define HARUKA_MOD_${_dep} 1\n")
            endif()
        endforeach()
    endif()
endforeach()

# Generate the single source of truth header used by #ifdef across the code.
# Written INTO the source tree (src/core/) so any consumer that includes
# ENGINE_ROOT/src (e.g. the game) sees it without extra include paths.
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/modules.h.in"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/modules.h"
    @ONLY)
