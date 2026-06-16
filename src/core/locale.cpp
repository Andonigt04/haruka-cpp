#include "core/locale.h"

#include <nlohmann/json.hpp>
#include <fstream>
#include <filesystem>

namespace fs = std::filesystem;

namespace Haruka {

Locale& Locale::get() { static Locale l; return l; }

void Locale::scan(const std::string& langDir) {
    m_langDir = langDir;
    m_available.clear();
    std::error_code ec;
    if (!fs::exists(langDir, ec)) return;
    for (const auto& e : fs::directory_iterator(langDir, ec)) {
        if (ec) break;
        if (!e.is_regular_file() || e.path().extension() != ".json") continue;
        std::string code = e.path().stem().string();
        std::string name = code;
        std::ifstream f(e.path());
        if (f) { try { nlohmann::json j; f >> j; name = j.value("_name", code); } catch (...) {} }
        m_available.push_back({ code, name });
    }
}

void Locale::loadInto(const std::string& code, std::unordered_map<std::string, std::string>& out) const {
    out.clear();
    std::ifstream f(m_langDir + code + ".json");
    if (!f) return;
    nlohmann::json j;
    try { f >> j; } catch (...) { return; }
    for (auto it = j.begin(); it != j.end(); ++it) {
        if (it.key().rfind("_", 0) == 0) continue;       // metadatos (_name…) no son textos
        if (it.value().is_string()) out[it.key()] = it.value().get<std::string>();
    }
}

bool Locale::load(const std::string& code) {
    if (m_base.empty()) loadInto(m_baseCode, m_base);    // idioma base (fallback) una vez
    m_current = code;
    loadInto(code, m_strings);
    return !m_strings.empty() || code == m_baseCode;
}

const std::string& Locale::tr(const std::string& key) const {
    auto it = m_strings.find(key);
    if (it != m_strings.end()) return it->second;        // idioma actual
    auto ib = m_base.find(key);
    if (ib != m_base.end()) return ib->second;           // idioma base (es)
    return key;                                          // nada → la clave (visible = "falta")
}

} // namespace Haruka
