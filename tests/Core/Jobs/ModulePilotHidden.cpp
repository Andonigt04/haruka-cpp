/**
 * @file ModulePilotHidden.cpp
 * @brief CONTRAPRUEBA del piloto de módulos: esto TIENE QUE FALLAR AL COMPILAR.
 *
 * `internalOnly` está definido en el módulo `Haruka.Jobs` pero no está exportado. Si esto compila, el
 * `export` no separa nada y el piloto no demuestra lo que dice. El test `haruka_module_hidden` de
 * CTest construye este objetivo y espera el fallo (WILL_FAIL).
 */
import Haruka.Core.Jobs;

int main() { return Haruka::Core::Jobs::internalOnly(); }
