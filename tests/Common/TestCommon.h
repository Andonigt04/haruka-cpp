#pragma once
// ================================================================================================
// Infra COMPARTIDA del banco de pruebas. `tests/` refleja `src/Haruka/`: los tests de
// `World/Terrain/` viven en `tests/World/Terrain/`, y esa carpeta ES su grupo.
// `main()` vive en Common/HarukaTests.cpp y los dirige todos.
//
// Aquí solo el mini-framework de aserciones + el REGISTRO de tests.
// ================================================================================================
#include <cstdio>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

// --- mini-framework de aserciones (contadores definidos en TestCommon.cpp) ---
extern int         g_pass, g_fail;
extern std::string g_curTest;
#define CHECK(cond, msg) do { \
    if (cond) { ++g_pass; } \
    else { ++g_fail; std::printf("  \033[31mFAIL\033[0m [%s] %s  (%s:%d)\n", g_curTest.c_str(), msg, __FILE__, __LINE__); } \
} while (0)
void beginTest(const char* name);

// --- Registro ---
// Cada archivo de test registra los suyos al final con HARUKA_REGISTER_TESTS. El grupo es la carpeta
// del archivo relativa a `tests/` (sale de __FILE__), así que no hay una lista central que mantener
// ni un test que pueda quedarse fuera de su grupo: escribirlo en la carpeta ya lo pone en ella.
// Dentro de un grupo, el orden es el del registro.
struct RegisteredTest { std::string group; const char* name; void (*fn)(); };
std::vector<RegisteredTest>& testRegistry();

struct TestRegistrar {
    TestRegistrar(const char* file, std::initializer_list<std::pair<const char*, void (*)()>> tests);
};

#define HARUKA_TEST_CONCAT2(a, b) a##b
#define HARUKA_TEST_CONCAT(a, b) HARUKA_TEST_CONCAT2(a, b)
#define HARUKA_REGISTER_TESTS(...) \
    static const TestRegistrar HARUKA_TEST_CONCAT(g_harukaTestRegistrar_, __LINE__)(__FILE__, { __VA_ARGS__ })
