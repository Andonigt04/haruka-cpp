# Documentación {#guia_docs}
<!-- doc: pin -->

Cómo está montada esta documentación y cómo se escribe una guía. Una guía cuenta la **intención** de un sistema (qué tiene que hacer y por qué) y remite a los ficheros; el detalle de cada función sale del código, no se copia a mano.

## Cómo funciona

- **Doxygen** genera el HTML desde `src/` y `docs/guides/` con la cabecera y el tema de `docs/`. La portada es @ref guia_arquitectura.
- **Barra izquierda**: arriba las guías **Fijadas**, después el resto de **Guías** y debajo **Comunes** (clases, archivos, namespaces, todas las páginas). Las guías las pinta `haruka_docs.js` con los datos de `doc_meta.js`; Comunes es HTML fijo de `header.html`.
- **Cabecera de cada guía**: debajo del título salen la ruta del `.md` y la fecha de modificación (último commit, o la del fichero si tiene cambios sin commitear). Las genera `tools/doc_meta.py`; no se escriben a mano.
- **Plantilla comprobada**: `doc_meta.py` avisa si a una guía le falta una sección o si cita en *Archivos principales* un fichero que ya no existe. Así una guía vieja se nota al generar.
- **Flujo de ejecución**: cada función documentada tiene un botón que despliega lo que llama, por qué ramas pasa y qué variables escribe, sacado del AST de Clang (`tools/doc_flow.py`). Por eso las guías no transcriben secuencias de llamadas.
- Las guías y planes antiguos están en `docs/archivo/`, fuera de Doxygen.

## Archivos principales

| Archivo | Qué hace |
|---|---|
| `tools/build_docs.sh` | Genera toda la documentación |
| `tools/doc_meta.py` | Metadatos y comprobación de guías |
| `tools/doc_flow.py` | Árbol de ejecución desde Clang |
| `docs/header.html` | Barra superior e izquierda |
| `docs/theme/haruka_docs.js` | Pinta guías y cabecera |
| `docs/theme/haruka_docs.css` | Estilo de barra y cabecera |
| `docs/theme/haruka_flow.js` | Botón de flujo de ejecución |
| `docs/theme/input.css` | Tema oscuro (fuente Tailwind) |
| `Doxyfile` | Configuración de Doxygen |

## Opciones

| Opción | Efecto |
|---|---|
| `<!-- doc: pin -->` en una guía | La pone en **Fijadas** y muestra 📌 |
| `./tools/build_docs.sh --fast` | Sin grafos de dot (mucho más rápido) |
| `./tools/build_docs.sh --flow` | Solo regenera el árbol de ejecución |
| `python3 tools/doc_meta.py --check` | Sale con error si una guía no cumple |
| `DOC_FLOW_ARGS="--filter X"` | Limita el árbol de ejecución a unos `.cpp` |

El árbol de ejecución necesita `build/compile_commands.json` (`-DCMAKE_EXPORT_COMPILE_COMMANDS=ON`) y `pip install --user libclang`. El tema Tailwind se recompila con `npm run build` en `docs/theme/` solo si cambian las clases de `header.html`.

## Cómo usarlo

Generar y abrir:

```bash
./tools/build_docs.sh --fast
xdg-open docs/html/index.html
```

Escribir una guía nueva: un fichero en `docs/guides/` con esta forma exacta (las secciones, en este orden):

```markdown
# Nombre del sistema {#guia_nombre}
<!-- doc: pin -->            ← opcional

Resumen en una o dos frases: qué tiene que hacer el sistema y por qué existe.

## Cómo funciona
Las ideas que hay que saber para tocarlo, en viñetas. Intención, no secuencias de llamadas.

## Archivos principales
| Archivo | Qué hace |
|---|---|
| `src/Haruka/…/Fichero.h` | Tres o cuatro palabras |

## Opciones
Variables de entorno, ajustes, campos de JSON o flags de CMake que lo cambian.

## Cómo usarlo
El ejemplo mínimo o los pasos para usarlo / probarlo.
```

Reglas:

- El nombre, la ruta y la fecha **no se escriben**: salen solos.
- Una guía por sistema. Si un plan termina, lo que queda vigente se pasa a la guía del sistema; el plan va a `docs/archivo/`.
- Si cambias un sistema, revisa su guía en el mismo cambio. `doc_meta.py` detecta ficheros renombrados o borrados, no ideas viejas.
