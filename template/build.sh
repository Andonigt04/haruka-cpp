#!/usr/bin/env bash
# =============================================================================
# build.sh — Compila el proyecto Haruka.
#
# Uso:
#   ./build.sh                          # configura (si falta) y compila
#   ./build.sh -DPROJECT_NAME=Foo       # pasa argumentos extra a cmake
#
# El IDE ejecuta este script desde la raíz del proyecto, p. ej.:
#   ./build.sh -DPROJECT_NAME=Juego -DHARUKA_ENGINE_LOCAL=/ruta/al/motor
# para que el juego compile contra el MISMO motor del editor sin redescargarlo.
# El layout resultante (build/bin/lib<Nombre>.so, shaders, scenes, assets) es el
# que el editor busca en Play Mode.
# =============================================================================
set -e

# Directorio raíz del proyecto: el de este script, no el CWD (así funciona desde
# cualquier sitio: ./build.sh, build/../build.sh, o rutas absolutas).
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build"

mkdir -p "${BUILD_DIR}"
cd "${BUILD_DIR}"

echo "==> Configurando (cmake) en ${BUILD_DIR}"
cmake .. "$@"

# PARALELISMO ACOTADO POR RAM, no por núcleos — mismo criterio que el build.sh del juego
# (Survival) y del editor. Cada g++ de este proyecto pide ~0.4-0.5 GB de pico, así que
# `make -j$(nproc)` lanza TODOS los trabajos a la vez y el build tira de swap. Se calcula
# desde MemAvailable (lo que el kernel puede reclamar de verdad), dejando 1.5 GB al sistema.
# Override manual: JOBS=8 ./build.sh
if [ -z "${JOBS:-}" ]; then
    CORES=$(nproc 2>/dev/null || echo 4)
    AVAIL_MB=$(awk '/^MemAvailable:/ {print int($2/1024)}' /proc/meminfo 2>/dev/null || echo 0)
    if [ "$AVAIL_MB" -gt 0 ]; then
        USABLE_MB=$(( AVAIL_MB - 1536 ))            # reserva para el sistema
        [ "$USABLE_MB" -lt 600 ] && USABLE_MB=600
        JOBS=$(( USABLE_MB / 600 ))                 # ~0.6 GB por trabajo (margen sobre los 0.5 medidos)
        [ "$JOBS" -lt 1 ] && JOBS=1
        [ "$JOBS" -gt "$CORES" ] && JOBS=$CORES
    else
        JOBS=$CORES
    fi
fi
echo "==> Compilando con -j${JOBS} (MemAvailable=$(awk '/^MemAvailable:/ {print int($2/1024)}' /proc/meminfo 2>/dev/null || echo ?) MB). Override: JOBS=N $0"

make -j"${JOBS}"

echo "==> Listo: plugin y ejecutable en ${BUILD_DIR}/bin"
