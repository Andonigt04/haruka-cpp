#pragma once
/**
 * @file locale.h
 * @brief i18n modular. Carga textos de assets/lang/<código>.json (clave → traducción) y
 *        resuelve tr("clave"). Los idiomas se DETECTAN del directorio (modular: añade/borra
 *        un .json y aparece/desaparece). Los assets pesados por idioma (modelo de voz en
 *        assets/voice/<código>, audio en assets/audio/<código>) se gestionan aparte y son
 *        instalables/borrables sin afectar al texto.
 */
#include <string>
#include <vector>
#include <unordered_map>

namespace Haruka {

class Locale {
public:
    static Locale& get();

    struct LangInfo { std::string code; std::string name; }; // name = autónimo ("Español")

    // Escanea assets/lang/*.json → idiomas disponibles (lee solo el campo "_name").
    void scan(const std::string& langDir = "assets/lang/");
    const std::vector<LangInfo>& available() const { return m_available; }

    // Carga el idioma <code> (sus textos). Fallback: deja las claves crudas si falta.
    bool load(const std::string& code);
    const std::string& current() const { return m_current; }

    // Traducción: idioma actual → si falta, idioma BASE (es) → si falta, la propia clave.
    const std::string& tr(const std::string& key) const;

private:
    void loadInto(const std::string& code, std::unordered_map<std::string, std::string>& out) const;

    std::string m_langDir = "assets/lang/";
    std::string m_baseCode = "es";    // idioma fuente: relleno completo; los demás lo heredan
    std::string m_current;
    std::unordered_map<std::string, std::string> m_strings;   // idioma actual
    std::unordered_map<std::string, std::string> m_base;      // idioma base (fallback)
    std::vector<LangInfo> m_available;
};

} // namespace Haruka

// Azúcar para el código de UI.
#define TR(key) Haruka::Locale::get().tr(key)
