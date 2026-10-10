// Definiciones del mini-framework de aserciones compartido por todos los archivos de test.
#include "TestCommon.h"

int         g_pass = 0, g_fail = 0;
std::string g_curTest;

void beginTest(const char* name) { g_curTest = name; std::printf("• %s\n", name); }
