# ================================================================================================
# Every engine source that asks `#ifdef HARUKA_MOD_*` must include "Haruka/Core/Modules.h" itself.
#
# Why: the module macros come ONLY from that generated header, never from the command line. A file
# that tests one without seeing the header compiles the guarded block to NOTHING, with no error and
# no warning. That is how the props lost their collision (12-10): `PropSystem.cpp` never included it,
# so `refreshColliders` was empty and its object file held no call to the physics at all — and the
# collision debug wire (`CollisionWire.cpp`) was empty too.
#
# Transitive inclusion would be enough for the compiler, but it cannot be checked without
# preprocessing, and it breaks the day an unrelated include is removed. A direct include can.
#
# Usage: cmake -DSRC_DIR=<engine>/src -P check_modules_include.cmake
# ================================================================================================
file(GLOB_RECURSE _files "${SRC_DIR}/*.cpp" "${SRC_DIR}/*.h")
set(_bad "")
foreach(_f ${_files})
    if(_f MATCHES "/Core/Modules\\.h$")
        continue()
    endif()
    file(STRINGS "${_f}" _uses REGEX "^[ \t]*#[ \t]*(if|ifdef|ifndef|elif)[ \t].*HARUKA_MOD_")
    if(NOT _uses)
        continue()
    endif()
    file(STRINGS "${_f}" _inc REGEX "^[ \t]*#[ \t]*include[ \t]+\"Haruka/Core/Modules\\.h\"")
    if(NOT _inc)
        file(RELATIVE_PATH _rel "${SRC_DIR}" "${_f}")
        string(APPEND _bad "\n  ${_rel}")
    endif()
endforeach()
if(_bad)
    message(FATAL_ERROR "These files test HARUKA_MOD_* without including \"Haruka/Core/Modules.h\" "
                        "(the guarded code would compile to nothing):${_bad}")
endif()
