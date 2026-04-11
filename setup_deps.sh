#!/bin/bash

mkdir -p third_party
cd third_party

###############################################################
######               DOWNLOAD DEPENDENCIES               ######
###############################################################
if [ ! -d "glm" ]; then
    git clone --depth 1 https://github.com/g-truc/glm.git
fi

if [ ! -d "glad" ]; then
    git clone --depth 1 https://github.com/Dav1dde/glad.git
fi

if [ ! -d "glfw" ]; then
    git clone --depth 1 https://github.com/glfw/glfw.git
fi

if [ ! -d "stb" ]; then
    git clone --depth 1 https://github.com/nothings/stb.git
fi

echo "Dependencias descargadas en third_party."