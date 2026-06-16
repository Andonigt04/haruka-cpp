# assets/

Datos del juego que NO son código, desplegados a `bin/assets/` en cada build.

- `data/` — JSON de datos (p.ej. `materials.json`). Resuelto en runtime con `AssetPaths::data()` → `assets/data/`. Se cargan UNA vez a memoria al arrancar (lookups O(1) después).
- `textures/`, `audio/`, etc. — añade lo que necesites; todo `assets/` se copia a `bin/`.

Editar un archivo aquí requiere rebuild para redesplegarlo (como `scenes/`). En dev puedes sustituir la copia por un symlink `bin/assets → ../../assets` si quieres edición en vivo.
