# Plan: Layout del build final (scenes/shaders en release)

> Estado: DOCUMENTADO (implementar al empaquetar). Decisiones del usuario:
> assets/ + lib/, mapas .hmap + shaders solo .spv en release, flag dev/release.
> Por ahora el dev sigue como está (todo plano junto al exe, legible).

## 0. Estado actual (DEV) — no cambiar todavía
```
Survival/build/bin/
├── survival              exe
├── libHarukaEngine.so    motor
├── libSurvival.so        lógica juego
├── mapc                  herramienta dev (NO va en release)
├── shaders/              .spv + fuentes .vert/.frag (legibles)
└── scenes/               .scene JSON (legibles)
```
Resolución de rutas: main.cpp hace `current_path(exeDir)` y busca `shaders/`,
`scenes/` RELATIVOS al exe. Shaders vía `Shader::setBaseDir(SDL_GetBasePath())`.

## 1. Layout objetivo (RELEASE)
```
SurvivalGame/
├── survival              exe
├── lib/
│   ├── libHarukaEngine.so
│   └── libSurvival.so
└── assets/
    ├── shaders/          SOLO .spv (sin fuentes)
    └── maps/             SOLO .hmap (cifrados, sin .scene)
```
- Binarios en lib/ → rpath del exe = `$ORIGIN/lib`.
- Datos en assets/ → una raíz de assets que el motor resuelve.
- mapc NO se incluye (es tooling de dev).

## 2. Cambios de CÓDIGO necesarios (resolución de rutas)
El motor hoy asume `shaders/` y `scenes/` junto al exe. Para assets/:
- Añadir `AssetPaths::root()` (engine) que devuelve la raíz de datos:
  - DEV  : el cwd actual (donde están shaders/ y scenes/ planos).
  - RELEASE: `<exeDir>/assets/`.
  Decidido por una macro de build `HARUKA_RELEASE` o por detectar si existe assets/.
- `Shader::setBaseDir(...)` → apuntar a `<assetsRoot>/shaders/` en release
  (hoy = SDL_GetBasePath()).
- `app.run("scenes/main")` → en release resolver a `<assetsRoot>/maps/main.hmap`.
  resolveScenePath() ya prueba .scene→.hmap; solo cambia la carpeta base
  (scenes/ en dev, assets/maps/ en release).
- main.cpp: `current_path(exeDir)` se mantiene; las rutas base salen de AssetPaths.

## 3. Cambios de BUILD (CMake)
Flag `-DRELEASE=ON` (default OFF = dev). Cuando RELEASE:
1. **Binarios → lib/**: copiar/instalar los .so en `lib/`, set INSTALL_RPATH
   `$ORIGIN/lib` en el exe (hoy es `$ORIGIN`).
2. **Shaders → assets/shaders, solo .spv**: copiar SOLO `*.spv` (excluir
   .vert/.frag/.comp/.geom fuentes). El engine ya genera .spv.
3. **Maps → assets/maps, solo .hmap**: por cada `scenes/*.scene`, ejecutar `mapc`
   en build (add_custom_command) → `assets/maps/*.hmap`. NO copiar los .scene.
4. **No incluir mapc** en el paquete release.
Cuando DEV (default): el POST_BUILD actual se queda igual (todo plano, legible).

## 4. Pipeline de mapas en release (auto)
`add_custom_command`: para cada scene, `mapc <scene> assets/maps/<name>.hmap`.
Así el release SIEMPRE tiene los .hmap al día sin paso manual (en dev sigues
editando .scene y opcionalmente corriendo mapc a mano).

## 5. Empaquetado (CPack / zip)
El target install ya existe (Survival/CMakeLists install rules). Actualizarlo al
layout assets/+lib/ y `cpack` para generar el .tar.gz/.zip distribuible. El plan
de packaging (PLAN_PACKAGING_SAVES_CONFIG.md) cubre el .pak embebido como nivel
superior; este doc es el layout de archivos sueltos (más simple, primer paso).

## 6. Orden de implementación (al empaquetar)
1. AssetPaths::root() + macro HARUKA_RELEASE (código, no rompe dev).
2. Apuntar Shader::setBaseDir y resolveScenePath a AssetPaths en release.
3. CMake -DRELEASE: lib/ + assets/shaders(.spv) + assets/maps(.hmap via mapc).
4. Probar el exe release desde una copia limpia (sin el árbol de fuentes).
5. CPack para el .zip final.

## 7. Notas
- Saves NO van aquí: están en la carpeta de usuario (SDL_GetPrefPath), no en el
  build. (Pendiente: portable-mode opcional — ver memoria.)
- mapc usa la clave kMapKey="haruka.map.v1"; el loader la misma → .hmap del build
  se cargan en runtime.
- Mismo codec (zstd+XOR) para mapas y saves: io/blob_codec.
