/** @page flujo_interactivo Flujo de ejecución interactivo

Cada función documentada tiene, bajo su descripción, un botón
**Flujo de ejecución**. Al desplegarlo aparece el árbol de lo que la función
hace realmente: las funciones que llama, los condicionales y bucles por los que
pasa, y las variables que declara o modifica.

No es documentación escrita a mano: sale del AST real de Clang sobre el código
que compila el motor, así que no puede desincronizarse del código.

## Los tres niveles de detalle

| Nivel | Qué muestra |
|-------|-------------|
| **Pasos grandes** (por defecto) | El esqueleto: `if`, bucles, `switch`, `return` y las llamadas. |
| **Solo funciones** | Únicamente las funciones ejecutadas, en orden, sin control de flujo. |
| **Variables y cálculos** | Todo lo anterior más cada declaración y cada asignación. |

Al ocultar un nivel sus hijos no se pierden: suben de nivel. Por eso "Solo
funciones" sigue enseñando las llamadas que ocurren dentro de un bucle o dentro
de un cálculo.

## Lectura del árbol

- `ƒ` llamada a función. Si está documentada, es un enlace a su página.
- `?` condicional, `↺` bucle, `⑂` `switch`, `↩` `return`.
- `=` asignación (variable modificada), `□` declaración de variable.
- El `:1234` de la derecha lleva a esa línea exacta del código fuente.

`Desplegar todo` abre el árbol entero; por defecto vienen abiertos los dos
primeros niveles.

## Cómo se genera

```bash
./tools/build_docs.sh          # doxygen completo + árbol de ejecución
./tools/build_docs.sh --fast   # sin grafos de dot, mucho más rápido
./tools/build_docs.sh --flow   # solo regenera el árbol de ejecución
```

Requisitos: `build/compile_commands.json` (lo genera CMake con
`-DCMAKE_EXPORT_COMPILE_COMMANDS=ON`) y `pip install --user libclang`.

El analizador es `tools/doc_flow.py`: lee el XML de Doxygen para saber a qué
página pertenece cada función y recorre el AST de cada unidad de traducción para
construir el árbol. Escribe un fichero por página en `docs/html/flow/`, que la
página carga solo cuando hace falta.

## Límites conocidos

- Es el flujo **estático**: el árbol enseña lo que la función puede ejecutar, no
  una traza de una ejecución concreta. Las ramas se muestran todas.
- Las llamadas a través de puntero a función o de un virtual se muestran, pero el
  enlace apunta a la declaración que ve el compilador, no a la implementación que
  se elija en tiempo de ejecución.
- Dentro de una lambda se recogen sus llamadas, no su control de flujo interno.
- Las funciones de plantilla se analizan tal como están escritas, no por cada
  instanciación.
*/
