// Definiciones del mini-framework de aserciones compartido por todos los archivos de test.
#include "Common/TestCommon.h"

int         g_pass = 0, g_fail = 0;
std::string g_curTest;

void beginTest(const char* name) { g_curTest = name; std::printf("• %s\n", name); }

std::vector<RegisteredTest>& testRegistry() {
    static std::vector<RegisteredTest> r;   // local: existe antes que cualquier registrador estático
    return r;
}

// "…/tests/World/Terrain/TestTerrain.cpp" -> "World/Terrain". Un archivo directamente en tests/ no
// tiene grupo: es un error de colocación y se avisa al arrancar (grupo vacío).
static std::string groupOf(const std::string& file) {
    const size_t t = file.rfind("/tests/");
    const size_t b = (t == std::string::npos) ? 0 : t + 7;
    const size_t e = file.rfind('/');
    return (e == std::string::npos || e < b) ? std::string() : file.substr(b, e - b);
}

TestRegistrar::TestRegistrar(const char* file, std::initializer_list<std::pair<const char*, void (*)()>> tests) {
    const std::string g = groupOf(file);
    for (const auto& [name, fn] : tests) testRegistry().push_back({ g, name, fn });
}
