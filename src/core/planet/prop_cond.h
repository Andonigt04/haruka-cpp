/**
 * @file prop_cond.h
 * @brief Condición BOOLEANA por capa de props: `when` en el JSON de la capa.
 *
 * El filtro de zona por lista (`PropLayer::zones`) es un veto DURO por nombres: "solo en oasis".
 * Para expresar reglas compuestas —"no en arena PERO sí en la zona oasis" (`layer != sand || zone
 * == oasis`)— cada capa acepta un campo `when` con una expresión booleana sobre las DOS identidades
 * del punto:
 *
 *     · `layer` — el MATERIAL del terreno en el punto ("sand", "forest", "water"…).
 *     · `zone`  — la ZONA nombrada (geométrica/pintada) en el punto; vacío si el punto no está en
 *                 ninguna zona declarada.
 *
 * Sintaxis (GL-free, header-only, sin dependencias):
 *
 *     expr     := or
 *     or       := and ('||' and)*
 *     and      := unary ('&&' unary)*
 *     unary    := '!' unary | '(' expr ')' | comparison
 *     comparison := ('layer' | 'zone') ('==' | '!=') (word | 'quoted string')
 *
 *   `word`      → identificador simple ([A-Za-z0-9_.,-]): nombres de material/zona sin espacios.
 *   quoted      → cadena con comillas simples o dobles, para nombres con espacios o vacío ('').
 *
 * Ejemplos:
 *     when: "zone == oasis"                     → solo dentro de la zona "oasis"
 *     when: "layer != sand"                     → no en arena
 *     when: "layer != sand || zone == oasis"    → no en arena, pero sí en el oasis
 *     when: "!(layer == water) && zone != ''"   → fuera del agua y dentro de alguna zona
 *
 * `PropCond::parse` devuelve la AST (o nullptr + mensaje si la sintaxis no vale). La evalúa
 * `PropCond::eval(layer, zone)`. La tabla de capas guarda SOLO el string (round-trip con el IDE);
 * el parseo ocurre en `PropLayer::whenAllowed`, cacheado. El SceneValidator usa `parse` para
 * validar que el `when` de cada capa es sintácticamente correcto antes de llegar al planeta.
 */
#pragma once
#include <memory>
#include <string>
#include <vector>
#include <cctype>

namespace Haruka { namespace Planet {

/** @brief Contexto de evaluación: las dos identidades del punto. `layer` = material del terreno,
 *  `zone` = zona nombrada (vacío = el punto no está en ninguna zona declarada). */
struct PropCondCtx {
    std::string layer;
    std::string zone;
};

/** @brief Nodo de la AST de la condición. */
struct PropCond {
    virtual ~PropCond() = default;
    virtual bool eval(const PropCondCtx& ctx) const = 0;
};

/** @brief Comparación `layer == X` / `zone != X`. `var` es "layer" o "zone". */
struct PropCondCompare : PropCond {
    std::string var;        ///< "layer" | "zone"
    bool        equal = true;
    std::string value;
    bool eval(const PropCondCtx& ctx) const override {
        const std::string& actual = (var == "zone") ? ctx.zone : ctx.layer;
        return equal ? (actual == value) : (actual != value);
    }
};

/** @brief Negación `!expr`. */
struct PropCondNot : PropCond {
    std::shared_ptr<PropCond> child;
    bool eval(const PropCondCtx& ctx) const override { return !child->eval(ctx); }
};

/** @brief Conjunción `a && b && …`. */
struct PropCondAnd : PropCond {
    std::vector<std::shared_ptr<PropCond>> children;
    bool eval(const PropCondCtx& ctx) const override {
        for (const auto& c : children) if (!c->eval(ctx)) return false;
        return true;
    }
};

/** @brief Disyunción `a || b || …`. */
struct PropCondOr : PropCond {
    std::vector<std::shared_ptr<PropCond>> children;
    bool eval(const PropCondCtx& ctx) const override {
        for (const auto& c : children) if (c->eval(ctx)) return true;
        return false;
    }
};

/** @brief Parser descendente de la condición. Devuelve la AST o `nullptr` (con `error` en español)
 *  si la sintaxis no es válida. */
class PropCondParser {
public:
    explicit PropCondParser(const std::string& text) : m_src(text) {}

    std::shared_ptr<PropCond> parse(std::string& error) {
        skipWs();
        auto node = parseOr();
        skipWs();
        if (!node) { error = error.empty() ? "expresión vacía" : error; return nullptr; }
        if (m_pos < m_src.size()) { error = "símbolo inesperado en '" + peekToken() + "'"; return nullptr; }
        return node;
    }

private:
    std::shared_ptr<PropCond> parseOr() {
        std::vector<std::shared_ptr<PropCond>> kids;
        auto first = parseAnd();
        if (!first) return nullptr;
        kids.push_back(first);
        while (match("||")) {
            auto n = parseAnd();
            if (!n) { m_error = "falta un término tras '||'"; return nullptr; }
            kids.push_back(n);
        }
        if (kids.size() == 1) return kids[0];
        auto node = std::make_shared<PropCondOr>();
        node->children = std::move(kids);
        return node;
    }

    std::shared_ptr<PropCond> parseAnd() {
        std::vector<std::shared_ptr<PropCond>> kids;
        auto first = parseUnary();
        if (!first) return nullptr;
        kids.push_back(first);
        while (match("&&")) {
            auto n = parseUnary();
            if (!n) { m_error = "falta un término tras '&&'"; return nullptr; }
            kids.push_back(n);
        }
        if (kids.size() == 1) return kids[0];
        auto node = std::make_shared<PropCondAnd>();
        node->children = std::move(kids);
        return node;
    }

    std::shared_ptr<PropCond> parseUnary() {
        skipWs();
        if (match("!")) {
            auto child = parseUnary();
            if (!child) { if (m_error.empty()) m_error = "falta expresión tras '!'"; return nullptr; }
            auto node = std::make_shared<PropCondNot>();
            node->child = child;
            return node;
        }
        if (match("(")) {
            auto inner = parseOr();
            skipWs();
            if (!inner) { if (m_error.empty()) m_error = "paréntesis sin cerrar"; return nullptr; }
            if (!match(")")) { m_error = "falta ')'"; return nullptr; }
            return inner;
        }
        return parseComparison();
    }

    std::shared_ptr<PropCond> parseComparison() {
        const std::string var = readIdent();
        if (var != "layer" && var != "zone") {
            m_error = var.empty() ? "se esperaba 'layer' o 'zone'" : "'" + var + "' no es una identidad (usa layer o zone)";
            return nullptr;
        }
        bool equal = false;
        if (match("==")) equal = true;
        else if (match("!=")) equal = false;
        else { m_error = "se esperaba '==' o '!='"; return nullptr; }
        skipWs();
        const size_t before = m_pos;
        std::string value = readValue();
        if (m_pos == before) {
            m_error = "falta el valor a comparar tras '" + var + "'";
            return nullptr;
        }
        auto node = std::make_shared<PropCondCompare>();
        node->var = var; node->equal = equal; node->value = value;
        return node;
    }

    // ------------------------------------------------------------------ tokens
    void skipWs() { while (m_pos < m_src.size() && std::isspace((unsigned char)m_src[m_pos])) ++m_pos; }

    bool match(const char* tok) {
        skipWs();
        const size_t n = std::char_traits<char>::length(tok);
        if (m_src.compare(m_pos, n, tok) == 0) { m_pos += n; return true; }
        return false;
    }

    std::string readIdent() {
        skipWs();
        size_t start = m_pos;
        while (m_pos < m_src.size() &&
               (std::isalnum((unsigned char)m_src[m_pos]) || m_src[m_pos] == '_'))
            ++m_pos;
        return m_src.substr(start, m_pos - start);
    }

    std::string readValue() {
        skipWs();
        if (m_pos >= m_src.size()) { m_error = "falta el valor a comparar"; return {}; }
        const char c = m_src[m_pos];
        if (c == '\'' || c == '"') {
            const char q = c; ++m_pos;
            size_t start = m_pos;
            while (m_pos < m_src.size() && m_src[m_pos] != q) ++m_pos;
            std::string val = m_src.substr(start, m_pos - start);
            if (m_pos < m_src.size()) ++m_pos;    // cierra la comilla
            return val;
        }
        size_t start = m_pos;
        while (m_pos < m_src.size()) {
            const char d = m_src[m_pos];
            if (std::isspace((unsigned char)d) || d == '&' || d == '|' || d == ')' ) break;
            ++m_pos;
        }
        return m_src.substr(start, m_pos - start);
    }

    std::string peekToken() const {
        size_t p = m_pos;
        while (p < m_src.size() && std::isspace((unsigned char)m_src[p])) ++p;
        size_t start = p;
        while (p < m_src.size() && !std::isspace((unsigned char)m_src[p]) && m_src[p] != ')') ++p;
        return m_src.substr(start, p - start);
    }

    std::string m_src;
    size_t      m_pos = 0;
    std::string m_error;
};

/** @brief Convierte la condición en string. `nullptr` + error si no es válida. */
inline std::shared_ptr<PropCond> parsePropCond(const std::string& text, std::string& error) {
    if (text.empty()) { error = "condición vacía"; return nullptr; }
    PropCondParser p(text);
    return p.parse(error);
}

}} // namespace Haruka::Planet
