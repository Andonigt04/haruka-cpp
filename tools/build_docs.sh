#!/usr/bin/env bash
# Genera la documentacion completa: HTML de Doxygen + arbol de ejecucion reactivo.
#
#   ./tools/build_docs.sh            # todo
#   ./tools/build_docs.sh --fast     # sin grafos de dot (mucho mas rapido)
#   ./tools/build_docs.sh --flow     # solo regenera el arbol de ejecucion
#
# El arbol de ejecucion necesita:
#   * build/compile_commands.json  (cmake -DCMAKE_EXPORT_COMPILE_COMMANDS=ON)
#   * pip install --user libclang
set -euo pipefail
cd "$(dirname "$0")/.."

FAST=0
FLOW_ONLY=0
for a in "$@"; do
  case "$a" in
    --fast) FAST=1 ;;
    --flow) FLOW_ONLY=1 ;;
    *) echo "opcion desconocida: $a" >&2; exit 2 ;;
  esac
done

if [ "$FLOW_ONLY" -eq 0 ]; then
  # Doxygen NO limpia su salida: las paginas de clases borradas del codigo se
  # quedan ahi para siempre y siguen apareciendo en los indices. La salida es
  # 100% generada (y esta en .gitignore), asi que se rehace desde cero.
  echo "== limpieza =="
  rm -rf docs/html docs/xml

  echo "== doxygen =="
  if [ "$FAST" -eq 1 ]; then
    ( cat Doxyfile; echo "HAVE_DOT = NO"; echo "QUIET = YES" ) | doxygen -
  else
    doxygen Doxyfile
  fi
fi

echo "== arbol de ejecucion =="
python3 tools/doc_flow.py ${DOC_FLOW_ARGS:-}

if command -v node >/dev/null 2>&1; then
  echo "== prueba de la interfaz =="
  node tools/doc_flow_ui_test.js
fi

echo
echo "Listo: abre docs/html/index.html"
