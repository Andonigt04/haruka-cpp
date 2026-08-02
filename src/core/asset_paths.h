/**
 * @file asset_paths.h
 * @brief Resolves where game data (shaders, maps) lives, for dev vs release.
 *
 *  DEV (default):     assets are flat next to the executable —
 *                       <exe>/shaders/  <exe>/scenes/
 *  RELEASE (-DHARUKA_RELEASE): assets live under an assets/ root —
 *                       <exe>/assets/shaders/  <exe>/assets/maps/
 *
 * The executable cd's to its own dir at startup (main.cpp), so relative paths
 * resolve correctly either way. These helpers centralise the dev/release
 * difference so loaders don't hard-code layout.
 */
#pragma once
#include <string>

namespace Haruka {

class AssetPaths {
public:
    /**
     * @brief Fija la raíz ABSOLUTA de assets (con "/" final). Vacía = comportamiento histórico:
     *        rutas RELATIVAS al cwd, que funcionan porque el ejecutable hace `cd` a su directorio.
     *
     * Existe por el BANCO DE PRUEBAS, que no hace ese `cd` y que además se construye en DOS árboles
     * (el del motor y el del juego, vía FetchContent). Sin raíz, el mismo binario cargaba shaders o
     * no según desde dónde se lanzara: en el árbol del juego arrancaba con "terrain_gen.comp NO
     * linkó → terreno GPU deshabilitado" y aun así buena parte de la suite salía VERDE, porque los
     * tests del suelo miden la superficie de referencia, que es CPU. Un banco cuyo resultado depende
     * del directorio no compara nada.
     *
     * Lo fija `Shader::setBaseDir` (una sola llamada pone las dos raíces: la de los Shader y ésta),
     * porque tenerlas separadas es justo cómo se coló que el compute siguiera yendo por el cwd
     * mientras los shaders de dibujo ya iban por la raíz.
     */
    static void setAssetsDir(const std::string& dir) { s_assetsDir = dir; }
    static const std::string& assetsDir() { return s_assetsDir; }

    /**
     * @brief Raíz ABSOLUTA del proyecto abierto (carpeta que contiene project.hrk, con "/" final).
     *
     * Segunda raíz además de la del motor. El IDE fija la del motor (sus assets/shaders) al
     * arrancar, pero las texturas de escena —"assets/textures/x.png", tal como las escriben el
     * material editor y el bake del node graph— viven en la del PROYECTO. Sin ésta, bajo el IDE
     * toda textura del proyecto fallaba y las biomas por defecto de SimplePlanet caían al fallback
     * procedural (8 WARN por planeta). Vacía = comportamiento histórico (rutas relativas al cwd,
     * que funcionan porque el juego hace `cd` a su raíz).
     */
    static void setProjectRoot(const std::string& dir) {
        s_projectRoot = dir;
        if (!s_projectRoot.empty() && s_projectRoot.back() != '/' && s_projectRoot.back() != '\\')
            s_projectRoot += '/';
    }
    static const std::string& projectRoot() { return s_projectRoot; }

    /** @brief Root prefix for shader files (with trailing separator). Layout UNIFICADO
     *  dev==release: shaders SIEMPRE bajo assets/shaders/ (sin split dev/release). */
    static std::string shaders() {
        return s_assetsDir.empty() ? std::string("assets/shaders/") : s_assetsDir + "shaders/";
    }

    /** @brief Root prefix + directory for map/scene files. */
    static std::string maps() {
#ifdef HARUKA_RELEASE
        return "assets/maps/";
#else
        return "scenes/";
#endif
    }

    /** @brief Root prefix for game data files (materials.json, etc.). Dev usa el
     *  symlink build/bin/assets → ../../assets; release lo empaqueta bajo assets/. */
    static std::string data() {
        return "assets/data/";
    }

    /** @brief Root prefix for texture files (with trailing separator). */
    static std::string textures() {
        return s_assetsDir.empty() ? std::string("assets/textures/") : s_assetsDir + "textures/";
    }

    /** @brief Root prefix for PROJECT texture files (proyecto/assets/textures/). */
    static std::string projectTextures() {
        return s_projectRoot.empty() ? std::string("assets/textures/") : s_projectRoot + "assets/textures/";
    }

    /** @brief True when built for distribution (packed assets, no sources). */
    static constexpr bool isRelease() {
#ifdef HARUKA_RELEASE
        return true;
#else
        return false;
#endif
    }

private:
    inline static std::string s_assetsDir;   // vacía = rutas relativas al cwd (el juego hace cd)
    inline static std::string s_projectRoot; // vacía = sin proyecto abierto (rutas relativas)
};

} // namespace Haruka
