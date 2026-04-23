#!/bin/bash
# Compila todos los shaders GLSL a SPIR-V en la carpeta shaders/ y los deja en build/shaders/

SRC_DIR="shaders"
OUT_DIR="build/shaders"
mkdir -p "$OUT_DIR"

for shader in "$SRC_DIR"/*.{vert,frag,comp,geom}; do
    [ -e "$shader" ] || continue
    base=$(basename "$shader")
    glslangValidator -V "$shader" -o "$OUT_DIR/$base.spv"
    if [ $? -eq 0 ]; then
        echo "Compilado: $shader -> $OUT_DIR/$base.spv"
    else
        echo "Error compilando: $shader"
    fi
done
