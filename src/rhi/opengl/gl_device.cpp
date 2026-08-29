/**
 * @file gl_device.cpp
 * @brief Implementación OpenGL de RHI::Device: crea el contexto GL y gestiona recursos GPU.
 */
#include "rhi/opengl/gl_device.h"

#include <cstdio>
#include <cmath>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <vector>
#include <string>
#include <unordered_map>
#include <cstdlib>
#include "core/logger.h"
#include "core/asset_paths.h"

namespace Haruka::RHI::opengl
{
    // ------------------------------------------------------------------ helpers internos
    // PARIDAD GL=VK (decisión GLOBAL, NO por-etapa): un programa GL NO puede mezclar etapas
    // SPIR-V y GLSL — el link falla con "not all attached shaders have the same SPIR_V_BINARY_ARB
    // state". Así que el modo se decide UNA VEZ para todo el dispositivo:
    //   · true  → TODAS las etapas cargan SPIR-V precompilado (build time, glslang/glslc -G),
    //             con glShaderBinary + glSpecializeShader. Mismo compilador que Vulkan → paridad.
    //   · false → TODAS con el compilador GLSL del driver (fallback para GPUs sin GL_ARB_gl_spirv).
    // Se detecta la PRIMERA vez (hay contexto GL) y se cachea; NUNCA cambia a mitad de sesión.
    // Override: HARUKA_GL_SPIRV=0 fuerza el driver; =1 fuerza SPIR-V aunque la detección no lo vea.
    static bool gUseSpirv = false;
    static bool gSpirvChecked = false;
    static bool glspirvAvailable()
    {
        if (gSpirvChecked) return gUseSpirv;
        gSpirvChecked = true;

        // ⚠️ ESTA VARIABLE SOLO SABIA ENCENDER. `HARUKA_GL_SPIRV=0` no apagaba nada: `forced` salia
        // false y se caia al camino automatico, que enciende SPIR-V en cualquier GL 4.6. Con eso, un
        // experimento que creia estar comparando "SPIR-V si / SPIR-V no" comparaba SPIR-V consigo
        // mismo — y me dio por descartada una hipotesis correcta.
        const char* opt = std::getenv("HARUKA_GL_SPIRV");
        const bool asked = (opt && *opt);
        const bool forcedOn  = asked && (*opt != '0' && *opt != 'n');
        const bool forcedOff = asked && !forcedOn;

        const bool haveFuncs = glShaderBinary && glSpecializeShader;
        const char* version = (const char*)glGetString(GL_VERSION);
        bool core = version && atoi(version) >= 4 && version[2] >= '6';

        if (forcedOff)
            gUseSpirv = false;
        else if (forcedOn)
            gUseSpirv = haveFuncs;
        else if (core && haveFuncs)
            gUseSpirv = true;
        else
            gUseSpirv = false;

        if (!gUseSpirv)
            HARUKA_LOGW("RHI/GL", "GL=VK por driver (SPIR-V GL no disponible%s%s).",
                        haveFuncs ? "" : ": faltan glShaderBinary/glSpecializeShader",
                        (!asked && !core) ? ": GL < 4.6" : "");
        return gUseSpirv;
    }

    // Compila un módulo SPIR-V a un shader GL (GL_ARB_gl_spirv, core en 4.6). Igual que
    // hace hoy Shader::loadSPV, pero recibiendo los bytes en vez de la ruta del fichero.
    static GLuint compileSpirv(GLenum stage, const void* bytes, size_t size)
    {
        if (!bytes || size == 0) return 0;
        GLuint sh = glCreateShader(stage);
        glShaderBinary(1, &sh, GL_SHADER_BINARY_FORMAT_SPIR_V, bytes, (GLsizei)size);
        glSpecializeShader(sh, "main", 0, nullptr, nullptr);

        GLint ok = 0;
        glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
        if (!ok)
        {
            char log[1024] = {0};
            glGetShaderInfoLog(sh, sizeof(log), nullptr, log);
            HARUKA_LOGE("RHI/GL", "SPIR-V specialize error: %s", log);
            glDeleteShader(sh);
            return 0;
        }
        return sh;
    }

    // ------------------------------------------------------------------------------------------
    // Compila GLSL→SPIR-V (GL_ARB_gl_spirv) con glslangValidator, target OPENGL (-G).
    //
    // POR QUÉ: GL y Vulkan deben apuntarnos al MISMO shader. Si GL compila con el compilador GLSL
    // del driver y Vulkan con glslang, los dos pueden divergir (el clipmap fino no se ve en GL).
    // Compilando GL también con glslang (-G) y cargando el resultado por glShaderBinary +
    // glSpecializeShader, EL MISMO compilador genera el binario de cada backend a partir del mismo
    // fuente: paridad garantizada. GLSL_source sigue sin duplicarse; solo cambia *quién* compila.
    //
    // -G produce SPIR-V orientado a OpenGL: bindings literales (sin shifts, ver backend.glsl) y
    // builtins GL (gl_VertexID/gl_InstanceID) — a diferencia de -V para VULKAN. glShaderBinary+GL
    // specialize lo acepta (GL_ARB_gl_spirv, core en GL 4.6).
    static uint64_t glFnv1a(const std::string& s);   // fwd: definida más abajo
    static std::vector<uint8_t> compileGlslToGlSpv(const std::string& src, const char* stageSuffix)
    {
        if (src.empty()) { HARUKA_LOGE("RHI/GL", "compileGlslToGlSpv: fuente vacía"); return {}; }
        if (!glspirvAvailable()) { HARUKA_LOGE("RHI/GL", "compileGlslToGlSpv: SPIR-V no activo"); return {}; }
        const uint64_t hash = glFnv1a(src + "\n@" + stageSuffix);
        static std::unordered_map<uint64_t, std::vector<uint8_t>> cache;
        if (auto it = cache.find(hash); it != cache.end()) return it->second;

        const char* tmp = std::getenv("TMPDIR");
        if (!tmp || !*tmp) tmp = "/tmp";
        const std::string inPath  = std::string(tmp) + "/haruka_" + std::to_string(hash) + ".glsl";
        const std::string outPath = std::string(tmp) + "/haruka_" + std::to_string(hash) + ".spv";

        {
            std::ofstream f(inPath);
            if (!f.is_open()) return {};
            f << src;
        }

        const std::string errPath = std::string(tmp) + "/haruka_" + std::to_string(hash) + ".err";
        // OJO: algunos builds de glslangValidator escriben los errores en STDOUT (no stderr).
        // Se redirigen AMBOS al mismo fichero para no perder el diagnóstico del fallo.
        const std::string cmd =
            "glslangValidator -G -S " + std::string(stageSuffix) +
            " --auto-map-locations \"" + inPath + "\" -o \"" + outPath + "\" >\"" + errPath + "\" 2>&1";

        const int rc = std::system(cmd.c_str());
        std::vector<uint8_t> spv;
        if (rc == 0)
        {
            std::ifstream f(outPath, std::ios::binary | std::ios::ate);
            if (f) {
                auto n = (std::streamsize)f.tellg(); f.seekg(0);
                spv.resize((size_t)n); f.read(reinterpret_cast<char*>(spv.data()), n);
            }
        }
        if (rc != 0 || spv.empty()) {
            // Conserva el .err y el .glsl en disco (hash conocido) para que puedas
            // reproducir el fallo a mano: glslangValidator -G -S <etapa> /tmp/haruka_<hash>.glsl
            std::vector<uint8_t> err;
            std::ifstream ef(errPath, std::ios::binary | std::ios::ate);
            if (ef) { auto n = ef.tellg(); ef.seekg(0); err.resize((size_t)n);
                      ef.read(reinterpret_cast<char*>(err.data()), n); }
            if (!err.empty())
                HARUKA_LOGE("RHI/GL", "glslang -G falló (rc=%d, etapa %s, input %s):\n%s",
                            rc, stageSuffix, inPath.c_str(), err.data());
            else
                HARUKA_LOGE("RHI/GL", "glslang -G falló (rc=%d, etapa %s, input %s) sin mensaje.",
                            rc, stageSuffix, inPath.c_str());
            return {};
        }
        std::remove(inPath.c_str());
        std::remove(outPath.c_str());
        std::remove(errPath.c_str());
        cache[hash] = std::move(spv);
        return spv;
    }

    static uint64_t glFnv1a(const std::string& s)
    {
        uint64_t h = 1469598103934665603ull;
        for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
        return h;
    }

    // Suffix de etapa para glslangValidator (-S vert/frag/geom/tesc/tese/comp).
    static const char* stageSuffix(GLenum stage)
    {
        switch (stage)
        {
            case GL_VERTEX_SHADER:              return "vert";
            case GL_FRAGMENT_SHADER:            return "frag";
            case GL_GEOMETRY_SHADER:            return "geom";
            case GL_TESS_CONTROL_SHADER:        return "tesc";
            case GL_TESS_EVALUATION_SHADER:     return "tese";
            case GL_COMPUTE_SHADER:             return "comp";
            default:                            return nullptr;
        }
    }

    // ------------------------------------------------------------------------------------------
    // #include para GLSL. GLSL no lo tiene (ARB_shading_language_include existe pero apenas se
    // implementa), así que el terreno vivía DUPLICADO: `planet.frag` y el shader inline del
    // SimplePlanet eran dos copias del mismo look, y cada ajuste había que hacerlo dos veces —
    // exactamente la clase de duplicación que en este motor ya produjo "tres suelos" y "dos climas".
    // Resolución textual, relativa a `assets/shaders/`, con tope de profundidad para que un include
    // circular falle en vez de colgar el arranque.
    // ------------------------------------------------------------------------------------------
    static std::string resolveIncludes(const std::string& src, const std::string& baseDir, int depth = 0)
    {
        if (src.find("#include") == std::string::npos) return src;   // caso común: sin coste
        if (depth > 8) {
            HARUKA_LOGE("RHI/GL", "#include: profundidad > 8 (¿ciclo?)");
            return src;
        }
        std::string out;
        out.reserve(src.size() + 4096);
        size_t pos = 0;
        while (pos < src.size()) {
            size_t eol  = src.find('\n', pos);
            if (eol == std::string::npos) eol = src.size();
            std::string line = src.substr(pos, eol - pos);

            // La extensión que habilita #include en glslc/glslangValidator NO la reconoce el
            // compilador GLSL del driver: se retira aquí, después de haber servido para que el
            // build valide el fichero.
            if (line.find("GL_GOOGLE_include_directive") != std::string::npos) {
                pos = eol + 1;
                continue;
            }

            size_t h = line.find("#include");
            // Solo si `#include` abre la línea (sin contar espacios): así una mención dentro de un
            // comentario o de una cadena no dispara una carga de fichero.
            size_t firstNonWs = line.find_first_not_of(" \t");
            if (h != std::string::npos && firstNonWs == h) {
                size_t q0 = line.find('"', h);
                size_t q1 = (q0 == std::string::npos) ? std::string::npos : line.find('"', q0 + 1);
                if (q0 != std::string::npos && q1 != std::string::npos) {
                    const std::string rel  = line.substr(q0 + 1, q1 - q0 - 1);
                    const std::string full = baseDir + rel;
                    std::ifstream inc(full);
                    if (inc.is_open()) {
                        std::string body((std::istreambuf_iterator<char>(inc)),
                                          std::istreambuf_iterator<char>());
                        out += resolveIncludes(body, baseDir, depth + 1);
                        out += '\n';
                    } else {
                        HARUKA_LOGE("RHI/GL", "#include no encontrado: %s", full.c_str());
                    }
                    pos = eol + 1;
                    continue;
                }
            }
            out += line;
            out += '\n';
            pos = eol + 1;
        }
        return out;
    }

    // Raíz de resolución de los #include. La fija `RHI::setShaderIncludeDir` (ver rhi_device.cpp).
    // Fallback: si nadie la fijó (juego standalone que no pasa por Shader::setBaseDir), se deriva
    // de AssetPaths::shaders(), que SIEMPRE devuelve una raíz válida (absoluta si el motor la fijó,
    // o "assets/shaders/" relativa al cwd — el ejecutable hace `cd` a su directorio).
    std::string& shaderIncludeDir() {
        static std::string s;
        if (s.empty())
            s = Haruka::AssetPaths::shaders();
        return s;
    }

    // Carga una etapa desde ruta COMPLETA. PARIDAD GL=VK (build time): el CMake compila cada
    // shader a `<ruta>.spv` con glslang/glslc target GL; aquí se PREFIERE ese SPIR-V (mismo
    // compilador que Vulkan). Solo si el SPIR-V no está activo (GPU sin GL_ARB_gl_spirv) o no
    // existe el .spv se cae al GLSL del driver.
    static GLuint compileFromPath(GLenum stage, const char* fullPath)
    {
        if (!fullPath) return 0;

        // 1. SPIR-V precompilado (build time): paridad GL=VK por construcción.
        if (glspirvAvailable())
        {
            std::string spv = std::string(fullPath) + ".spv";
            if (std::ifstream f(spv, std::ios::binary | std::ios::ate); f.is_open())
            {
                auto n = (std::streamsize)f.tellg(); f.seekg(0);
                std::vector<char> buf(n); f.read(buf.data(), n);
                GLuint sh = compileSpirv(stage, buf.data(), (size_t)n);
                if (sh) return sh;
                // specialize fallido: se devuelve 0 → el pipeline falla visible (no mezcla).
                return 0;
            }
            // Sin .spv (dev sin recompilar): se deja pasar al driver, pero logueando la causa
            // para que no parezca un "no dibuja" mágico.
            HARUKA_LOGW("RHI/GL", "SPIR-V activo pero sin %s (¿recompilar shaders?) → driver GLSL.", spv.c_str());
        }

        // 2. GLSL source (fallback driver; también se usa en dev antes de compilar a .spv).
        if (std::ifstream g(fullPath); g.is_open())
        {
            const std::string src((std::istreambuf_iterator<char>(g)), std::istreambuf_iterator<char>());
            const std::string resolved = resolveIncludes(src, shaderIncludeDir());
            const char* p = resolved.c_str();
            GLuint sh = glCreateShader(stage);
            glShaderSource(sh, 1, &p, nullptr);
            glCompileShader(sh);
            GLint ok = 0; glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
            if (!ok) { char log[1024] = {0}; glGetShaderInfoLog(sh, sizeof(log), nullptr, log);
                       HARUKA_LOGE("RHI/GL", "GLSL compile [%s]: %s", fullPath, log); }
            return sh;
        }

        HARUKA_LOGW("RHI/GL", "shader no encontrado: %s (ni %s.spv)", fullPath, fullPath);
        return 0;
    }

    // Compila una etapa desde una cadena GLSL en línea (shaders generados/embebidos). Estos no
    // tienen `.spv` precompilado; con SPIR-V activo se compilan aquí mismo con glslang -G (mismo
    // compilador que Vulkan) para NO mezclar etapas SPIR-V+GLSL dentro de un programa.
    static GLuint compileFromSource(GLenum stage, const char* source)
    {
        if (!source) return 0;
        const std::string resolved = resolveIncludes(source, shaderIncludeDir());
        if (glspirvAvailable())
        {
            auto spv = compileGlslToGlSpv(resolved, stageSuffix(stage));
            if (spv.empty()) { HARUKA_LOGE("RHI/GL", "SPIR-V compile falló [inline]"); return 0; }
            return compileSpirv(stage, spv.data(), spv.size());
        }

        const char* p = resolved.c_str();
        GLuint sh = glCreateShader(stage);
        glShaderSource(sh, 1, &p, nullptr);
        glCompileShader(sh);
        GLint ok = 0; glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
        if (!ok) { char log[1024] = {0}; glGetShaderInfoLog(sh, sizeof(log), nullptr, log);
                   HARUKA_LOGE("RHI/GL", "GLSL source compile: %s", log); }
        return sh;
    }

    // Devuelve false si el enlazado falló. El llamador DEBE descartar el pipeline: un programa sin
    // linkar no da error al crearse, sino un GL_INVALID_OPERATION opaco en el primer glUseProgram
    // (lejos de la causa). Mejor fallar aquí y en voz alta.
    static bool checkLink(GLuint program)
    {
        GLint ok = 0;
        glGetProgramiv(program, GL_LINK_STATUS, &ok);
        if (!ok)
        {
            char log[1024] = {0};
            glGetProgramInfoLog(program, sizeof(log), nullptr, log);
            HARUKA_LOGE("RHI/GL", "program link error: %s", log);
        }
        return ok != 0;
    }

    static GLsizei mipLevels(uint32_t w, uint32_t h)
    {
        return 1 + (GLsizei)std::floor(std::log2((float)std::max(w, h)));
    }

    // ------------------------------------------------------------------ ciclo de vida
    GLDevice::GLDevice(SDL_Window* window) : m_window(window)
    {
        // Transición: si la Window ya creó un contexto GL (lo normal en el motor actual),
        // lo ADOPTAMOS en vez de crear uno segundo (dos contextos = recursos no compartidos).
        // Si no hay ninguno (arranque futuro RHI-first), lo creamos nosotros.
        if (SDL_GLContext existing = SDL_GL_GetCurrentContext())
        {
            m_glContext   = existing;
            m_ownsContext = false;   // glad ya está cargado por la Window; no re-inicializar
        }
        else
        {
            m_glContext = SDL_GL_CreateContext(m_window);
            if (!m_glContext)
            {
                HARUKA_LOGE("RHI/GL", "SDL_GL_CreateContext falló: %s", SDL_GetError());
                return;
            }
            if (!gladLoadGLLoader((GLADloadproc)SDL_GL_GetProcAddress))
            {
                HARUKA_LOGE("RHI/GL", "gladLoadGLLoader falló");
                return;
            }
            glEnable(GL_DEPTH_TEST);
            m_ownsContext = true;
        }
        // ⚠️ QUÉ GPU ESTÁ CORRIENDO DE VERDAD. Vulkan ya lo dice (`GPU = …`) y GL no decía nada, así
        // que "OpenGL contra Vulkan" y "una GPU contra otra" quedaban CONFUNDIDOS en la misma medida:
        // en un portátil híbrido GL va por donde diga el offload PRIME y Vulkan elige por su cuenta.
        // Sin esta línea no se puede atribuir una diferencia entre backends a la API.
        {
            const char* vend = (const char*)glGetString(GL_VENDOR);
            const char* rend = (const char*)glGetString(GL_RENDERER);
            const char* vers = (const char*)glGetString(GL_VERSION);
            HARUKA_LOGI("RHI/GL", "GPU = %s · %s · GL %s", vend ? vend : "(?)",
                        rend ? rend : "(?)", vers ? vers : "(?)");
        }
        // Reversed-Z con near→1, infinito→0 (ver Camera::getProjectionMatrix). La matriz emite
        // z_ndc ∈ [0,1], así que GL DEBE mapear [0,1]→depth (glClipControl(GL_ZERO_TO_ONE)); sin
        // esta llamada GL asume [-1,1] y comprime toda la profundidad a [0.5,1] — el z-buffer pierde
        // la mitad de su precisión a escala planetaria (z-fight del clipmap y del agua↔lecho).
        // ⚠️ SI NO ESTA, HAY QUE DECIRLO. El `if` silencioso era el fallo: sin `glClipControl` el
        // motor sigue dibujando —el ORDEN de profundidad es correcto— y solo se nota como z-fighting
        // donde no deberia haberlo. Un modo degradado que no se anuncia es indistinguible de un bug.
        if (glad_glClipControl) {
            glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE);
        } else {
            HARUKA_LOGE("RHI/GL", "SIN glClipControl: el reversed-Z pierde LA MITAD de su precision "
                                  "(GL asume [-1,1] y la profundidad util queda en (0.5,1]). "
                                  "Esperable: z-fighting del clipmap y del agua contra el lecho.");
        }
        m_context = std::make_unique<GLContext>(this);
    }

    GLDevice::~GLDevice()
    {
        for (auto& b : m_buffers)   if (b.id) glDeleteBuffers(1, &b.id);
        for (auto& t : m_textures)  if (t.id) glDeleteTextures(1, &t.id);
        for (auto& s : m_samplers)  if (s.id) glDeleteSamplers(1, &s.id);
        for (auto& p : m_pipelines) { if (p.program) glDeleteProgram(p.program); if (p.vao) glDeleteVertexArrays(1, &p.vao); }
        for (auto& rt : m_targets)  { if (rt.fbo) glDeleteFramebuffers(1, &rt.fbo); if (rt.depthRbo) glDeleteRenderbuffers(1, &rt.depthRbo); }
        // Solo destruimos el contexto si lo creamos nosotros; si es adoptado, lo destruye la Window.
        if (m_glContext && m_ownsContext) SDL_GL_DestroyContext(m_glContext);
        // OJO: NO destruye m_window; de eso se encarga la clase Window del motor.
    }

    // ------------------------------------------------------------------ buffers
    BufferHandle GLDevice::createBuffer(BufferUsage usage, size_t bytes, const void* data, BufferMemory mem)
    {
        GLBuffer b;
        b.target = toTarget(usage);
        glCreateBuffers(1, &b.id);
        // Almacenamiento MUTABLE (glBufferData) con el mismo hint que el código GL original.
        // Se probó storage inmutable (glBufferStorage) pero en iGPU de portátil penaliza el
        // glBufferSubData por-slot (memoria device-local lenta para escrituras CPU) → mutable
        // deja al driver elegir la región óptima. Es EXACTAMENTE el comportamiento previo.
        if (mem == BufferMemory::Readback)
        {
            // La GPU escribe, la CPU lee: storage inmutable + mapeo PERSISTENTE. Así el harvest
            // es un memcpy desde memoria de sistema en vez de un glGetBufferSubData por buffer
            // (que sincroniza con el driver: ~100 µs × 3 buffers × N chunks/frame).
            const GLbitfield flags = GL_MAP_READ_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT
                                   | GL_CLIENT_STORAGE_BIT; // pista: aloja en RAM, no en VRAM
            glNamedBufferStorage(b.id, (GLsizeiptr)bytes, data, flags);
            b.mapped = glMapNamedBufferRange(b.id, 0, (GLsizeiptr)bytes,
                                             GL_MAP_READ_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT);
        }
        else
        {
            const GLenum hint = (mem == BufferMemory::Static) ? GL_STATIC_DRAW : GL_DYNAMIC_DRAW;
            glNamedBufferData(b.id, (GLsizeiptr)bytes, data, hint);
        }
        return BufferHandle{ alloc(m_buffers, m_freeBuffers, b) };
    }

    void GLDevice::updateBuffer(BufferHandle h, size_t offset, size_t bytes, const void* data)
    {
        const GLBuffer* b = buffer(h);
        if (!b) return;
        glNamedBufferSubData(b->id, (GLintptr)offset, (GLsizeiptr)bytes, data);
    }

    void GLDevice::uploadBuffer(BufferHandle h, size_t bytes, const void* data)
    {
        const GLBuffer* b = buffer(h);
        if (!b) return;
        glNamedBufferData(b->id, (GLsizeiptr)bytes, data, GL_DYNAMIC_DRAW);   // reasigna (buffer mutable)
    }

    void GLDevice::copyBuffer(BufferHandle src, BufferHandle dst, size_t srcOff, size_t dstOff, size_t bytes)
    {
        const GLBuffer* s = buffer(src);
        const GLBuffer* d = buffer(dst);
        if (!s || !d) return;
        glCopyNamedBufferSubData(s->id, d->id, (GLintptr)srcOff, (GLintptr)dstOff, (GLsizeiptr)bytes);
    }

    // ------------------------------------------------------------------ texturas
    TextureHandle GLDevice::createTexture(const TextureDesc& d)
    {
        GLTexture t;
        const bool isArray = (d.layers > 1) && !d.cube;
        const GLenum target = d.cube ? GL_TEXTURE_CUBE_MAP
                                     : (isArray ? GL_TEXTURE_2D_ARRAY : GL_TEXTURE_2D);
        glCreateTextures(target, 1, &t.id);
        t.target = target;
        GLTexFmt fmt = texFmt(d.format);
        GLsizei levels = (d.mipmaps && d.width && d.height) ? mipLevels(d.width, d.height) : 1;
        // glTextureStorage2D asigna las 6 caras si el target es cubemap (una llamada). El render
        // por-cara (attach a FBO) lo hace el llamador (p.ej. IBL) contra la textura inmutable.
        if (isArray)
            glTextureStorage3D(t.id, levels, fmt.internal, (GLsizei)d.width, (GLsizei)d.height,
                               (GLsizei)d.layers);
        else
            glTextureStorage2D(t.id, levels, fmt.internal, (GLsizei)d.width, (GLsizei)d.height);
        if (d.cube) glTextureParameteri(t.id, GL_TEXTURE_WRAP_R, toWrap(d.wrap));

        if (d.initialData)
        {
            // Alinea filas a 1 byte: sin esto, un RGB8 de anchura no-múltiplo-de-4 sale sesgado
            // (GL_UNPACK_ALIGNMENT por defecto = 4). Robusto para cualquier formato/anchura.
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            if (isArray)
                // Las capas van CONTIGUAS en initialData: una sola subida para todo el array.
                glTextureSubImage3D(t.id, 0, 0, 0, 0, (GLsizei)d.width, (GLsizei)d.height,
                                    (GLsizei)d.layers, fmt.format, fmt.type, d.initialData);
            else
                glTextureSubImage2D(t.id, 0, 0, 0, (GLsizei)d.width, (GLsizei)d.height, fmt.format, fmt.type, d.initialData);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        }

        GLenum minF = toFilter(d.filter);
        if (d.mipmaps)
            minF = (d.filter == Filter::Linear) ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_NEAREST;
        glTextureParameteri(t.id, GL_TEXTURE_MIN_FILTER, minF);
        glTextureParameteri(t.id, GL_TEXTURE_MAG_FILTER, toFilter(d.filter));
        glTextureParameteri(t.id, GL_TEXTURE_WRAP_S, toWrap(d.wrap));
        glTextureParameteri(t.id, GL_TEXTURE_WRAP_T, toWrap(d.wrap));

        // Calidad: LOD bias y anisotropía (clampeada al máximo del driver). Como en Texture::setQuality.
        glTextureParameterf(t.id, GL_TEXTURE_LOD_BIAS, d.lodBias);
        if (d.maxAnisotropy > 1.0f)
        {
            float maxSupported = 1.0f;
            glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY, &maxSupported);
            glTextureParameterf(t.id, GL_TEXTURE_MAX_ANISOTROPY, std::min(d.maxAnisotropy, maxSupported));
        }

        if (d.mipmaps && d.initialData) glGenerateTextureMipmap(t.id);

        return TextureHandle{ alloc(m_textures, m_freeTextures, t) };
    }

    SamplerHandle GLDevice::createSampler(const SamplerDesc& d)
    {
        GLSampler s;
        glCreateSamplers(1, &s.id);
        glSamplerParameteri(s.id, GL_TEXTURE_MIN_FILTER, toFilter(d.filter));
        glSamplerParameteri(s.id, GL_TEXTURE_MAG_FILTER, toFilter(d.filter));
        glSamplerParameteri(s.id, GL_TEXTURE_WRAP_S, toWrap(d.wrap));
        glSamplerParameteri(s.id, GL_TEXTURE_WRAP_T, toWrap(d.wrap));
        if (d.maxAnisotropy > 1.0f)
            glSamplerParameterf(s.id, GL_TEXTURE_MAX_ANISOTROPY, d.maxAnisotropy);
        return SamplerHandle{ alloc(m_samplers, m_freeSamplers, s) };
    }

    // ------------------------------------------------------------------ pipelines
    PipelineHandle GLDevice::createPipeline(const PipelineDesc& d)
    {
        GLPipeline p;
        p.depth = d.depth;
        p.blend = d.blend;
        p.cull  = d.cull;

        // Cada etapa: source inline > ruta (GLSL-first) > bytes SPIR-V directos.
        auto stage = [](GLenum s, const char* source, const char* path, const void* bytes, size_t size) -> GLuint {
            if (source) return compileFromSource(s, source);
            if (path)   return compileFromPath(s, path);
            if (bytes)  return compileSpirv(s, bytes, size);
            return 0;
        };

        if (d.computeSource || d.computePath || d.spirvCompute)
        {
            GLuint cs = stage(GL_COMPUTE_SHADER, d.computeSource, d.computePath, d.spirvCompute, d.spirvComputeSize);
            if (!cs) { HARUKA_LOGE("RHI/GL", "createPipeline: falta la etapa COMPUTE"); return {}; }
            p.program = glCreateProgram();
            glAttachShader(p.program, cs);
            glLinkProgram(p.program);
            const bool linked = checkLink(p.program);
            glDeleteShader(cs);
            if (!linked) { glDeleteProgram(p.program); return {}; }
            p.compute = true;
            if (std::getenv("HARUKA_GL_TEXPROBE")) {
                GLint nUni = 0;
                glGetProgramiv(p.program, GL_ACTIVE_UNIFORMS, &nUni);
                for (GLint i = 0; i < nUni; ++i) {
                    char nm[128]; GLsizei len = 0; GLint sz = 0; GLenum ty = 0;
                    glGetActiveUniform(p.program, (GLuint)i, sizeof(nm), &len, &sz, &ty, nm);
                    const GLint loc = glGetUniformLocation(p.program, nm);
                    GLint val = -1;
                    if (loc >= 0) glGetUniformiv(p.program, loc, &val);
                    HARUKA_LOGI("RHI/GL", "  compute uniform '%s' tipo=0x%X loc=%d valor=%d",
                                nm, (unsigned)ty, loc, val);
                }
            }
        }
        else
        {
            GLuint vs = stage(GL_VERTEX_SHADER,   d.vertexSource,   d.vertexPath,   d.spirvVertex,   d.spirvVertexSize);
            GLuint fs = stage(GL_FRAGMENT_SHADER, d.fragmentSource, d.fragmentPath, d.spirvFragment, d.spirvFragmentSize);
            GLuint gs = stage(GL_GEOMETRY_SHADER, nullptr,          d.geometryPath, d.spirvGeometry, d.spirvGeometrySize);
            // TESELACIÓN: control y evaluación. Van juntas o ninguna — GL rechaza un programa con
            // control sin evaluación, y el error que da ("tessellation control shader must be
            // paired") no dice cuál falta.
            const bool wantsTess = d.tessControlSource || d.tessControlPath ||
                                   d.tessEvalSource    || d.tessEvalPath;
            GLuint tcs = 0, tes = 0;
            if (wantsTess)
            {
                tcs = stage(GL_TESS_CONTROL_SHADER,    d.tessControlSource, d.tessControlPath, nullptr, 0);
                tes = stage(GL_TESS_EVALUATION_SHADER, d.tessEvalSource,    d.tessEvalPath,    nullptr, 0);
            }
            // Una etapa a 0 = shader no encontrado o que no compila (la causa ya se logueó). Enlazar
            // igualmente produce un programa NO linkado que solo revienta en el draw → abortar aquí.
            if (!vs || !fs || (wantsTess && (!tcs || !tes)))
            {
                HARUKA_LOGE("RHI/GL", "createPipeline: etapa invalida (vs=%u fs=%u tcs=%u tes=%u). "
                           "¿La ruta esta enraizada con el base dir de assets?", vs, fs, tcs, tes);
                if (vs) glDeleteShader(vs);
                if (fs) glDeleteShader(fs);
                if (gs) glDeleteShader(gs);
                if (tcs) glDeleteShader(tcs);
                if (tes) glDeleteShader(tes);
                return {};
            }
            p.program = glCreateProgram();
            glAttachShader(p.program, vs);
            glAttachShader(p.program, fs);
            if (gs)  glAttachShader(p.program, gs);
            if (tcs) glAttachShader(p.program, tcs);
            if (tes) glAttachShader(p.program, tes);
            glLinkProgram(p.program);
            const bool linked = checkLink(p.program);
            glDeleteShader(vs);
            glDeleteShader(fs);
            if (gs)  glDeleteShader(gs);
            if (tcs) glDeleteShader(tcs);
            if (tes) glDeleteShader(tes);
            if (!linked) { glDeleteProgram(p.program); return {}; }

            // Tamaño del parche. Se guarda en el pipeline y lo aplica bindPipeline: en GL
            // glPatchParameteri es estado GLOBAL, así que si lo pusiera el draw, dos pipelines de
            // teselación con distinto tamaño de parche se pisarían el uno al otro.
            if (d.topology == PrimitiveTopology::Patches)
                p.patchVertices = (GLint)(d.patchVertices > 0 ? d.patchVertices : 4);

            // VAO con SOLO el formato de los atributos (DSA). El buffer se ata en el draw
            // vía glVertexArrayVertexBuffer -> desacopla layout de datos (mapea a Vulkan).
            glCreateVertexArrays(1, &p.vao);
            for (const VertexAttribute& a : d.vertexLayout.attributes)
            {
                if (a.binding >= 8) continue;   // cota del array de strides
                GLVertexFmt vf = vertexFmt(a.format);
                glEnableVertexArrayAttrib(p.vao, a.location);
                glVertexArrayAttribFormat(p.vao, a.location, vf.size, vf.type, vf.normalized, a.offset);
                glVertexArrayAttribBinding(p.vao, a.location, a.binding);   // de qué buffer sale
            }
            // Un stride por binding: el terreno alimenta cada atributo desde su propio buffer.
            p.bindingCount = (uint32_t)std::min<size_t>(d.vertexLayout.strides.size(), 8);
            for (uint32_t i = 0; i < p.bindingCount; ++i)
            {
                p.strides[i] = (GLsizei)d.vertexLayout.strides[i];
                // INSTANCING: el divisor va en el BINDING del VAO (no por atributo) → es estado del
                // PIPELINE, no del draw. Así el instancer no tiene que tocar glVertexAttribDivisor
                // (ni acordarse de RESETEARLO, que era la trampa: el divisor se queda pegado al VAO
                // y contamina los draws normales que vengan detrás).
                const bool perInstance = (i < d.vertexLayout.rates.size()) &&
                                         (d.vertexLayout.rates[i] == InputRate::Instance);
                glVertexArrayBindingDivisor(p.vao, i, perInstance ? 1u : 0u);
            }
            p.topology = toTopology(d.topology);
        }

        return PipelineHandle{ alloc(m_pipelines, m_freePipelines, p) };
    }

    // ------------------------------------------------------------------ render targets
    RenderPassHandle GLDevice::createRenderTarget(const RenderTargetDesc& d)
    {
        GLRenderTarget rt;
        rt.width  = d.width;
        rt.height = d.height;
        glCreateFramebuffers(1, &rt.fbo);

        // Color attachments (MRT). Cada uno es una textura muestreable.
        std::vector<GLenum> drawBufs;
        for (size_t i = 0; i < d.colorFormats.size(); ++i)
        {
            TextureDesc cd;
            cd.width = d.width; cd.height = d.height; cd.format = d.colorFormats[i];
            cd.renderTarget = true; cd.filter = d.colorFilter; cd.wrap = Wrap::ClampToEdge;
            TextureHandle th = createTexture(cd);
            rt.colors.push_back(th);
            glNamedFramebufferTexture(rt.fbo, GL_COLOR_ATTACHMENT0 + (GLenum)i, texture(th)->id, 0);
            drawBufs.push_back(GL_COLOR_ATTACHMENT0 + (GLenum)i);
        }

        // Profundidad: renderbuffer (rápido), textura 2D muestreable, o cubemap (shadow maps).
        if (d.hasDepth)
        {
            GLTexFmt df = texFmt(d.depthFormat);
            if (d.depthCube || d.depthAsTexture)
            {
                GLTexture t;
                GLenum tgt = d.depthCube ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D;
                glCreateTextures(tgt, 1, &t.id);
                glTextureStorage2D(t.id, 1, df.internal, (GLsizei)d.width, (GLsizei)d.height);
                glTextureParameteri(t.id, GL_TEXTURE_MIN_FILTER, toFilter(d.depthFilter));
                glTextureParameteri(t.id, GL_TEXTURE_MAG_FILTER, toFilter(d.depthFilter));
                if (d.depthCompare)   // sampler de sombra: comparación hardware (PCF)
                {
                    glTextureParameteri(t.id, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
                    glTextureParameteri(t.id, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
                }
                if (d.depthBorderClamp)
                {
                    glTextureParameteri(t.id, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
                    glTextureParameteri(t.id, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
                    float border[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
                    glTextureParameterfv(t.id, GL_TEXTURE_BORDER_COLOR, border);
                }
                else
                {
                    GLenum w = GL_CLAMP_TO_EDGE;
                    glTextureParameteri(t.id, GL_TEXTURE_WRAP_S, w);
                    glTextureParameteri(t.id, GL_TEXTURE_WRAP_T, w);
                    if (d.depthCube) glTextureParameteri(t.id, GL_TEXTURE_WRAP_R, w);
                }
                rt.depthTex = TextureHandle{ alloc(m_textures, m_freeTextures, t) };
                glNamedFramebufferTexture(rt.fbo, GL_DEPTH_ATTACHMENT, t.id, 0);  // cubemap entero
            }
            else
            {
                glCreateRenderbuffers(1, &rt.depthRbo);
                glNamedRenderbufferStorage(rt.depthRbo, df.internal, (GLsizei)d.width, (GLsizei)d.height);
                GLenum attach = (d.depthFormat == Format::D24S8) ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT;
                glNamedFramebufferRenderbuffer(rt.fbo, attach, GL_RENDERBUFFER, rt.depthRbo);
            }
        }

        // Sin color (shadow depth-only) -> draw/read buffer NONE; si no, los N attachments.
        if (drawBufs.empty())
        {
            glNamedFramebufferDrawBuffer(rt.fbo, GL_NONE);
            glNamedFramebufferReadBuffer(rt.fbo, GL_NONE);
        }
        else
        {
            glNamedFramebufferDrawBuffers(rt.fbo, (GLsizei)drawBufs.size(), drawBufs.data());
        }

        if (glCheckNamedFramebufferStatus(rt.fbo, GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            HARUKA_LOGE("RHI/GL", "framebuffer incompleto (%ux%u, %zu color)",
                         d.width, d.height, d.colorFormats.size());

        return RenderPassHandle{ alloc(m_targets, m_freeTargets, rt) };
    }

    TextureHandle GLDevice::getColorTexture(RenderPassHandle h, uint32_t index)
    {
        const GLRenderTarget* rt = renderTarget(h);
        return (rt && index < rt->colors.size()) ? rt->colors[index] : TextureHandle{};
    }

    TextureHandle GLDevice::getDepthTexture(RenderPassHandle h)
    {
        const GLRenderTarget* rt = renderTarget(h);
        return rt ? rt->depthTex : TextureHandle{};
    }

    // El formato de profundidad del BACKBUFFER, PREGUNTADO al driver.
    //
    // Es la única forma de que un blit de profundidad contra pantalla sea legal: GL exige que los dos
    // formatos coincidan EXACTAMENTE, y este no lo elige el motor —lo elige SDL al crear el contexto,
    // que en este motor no fija `SDL_GL_DEPTH_SIZE`—. Suponer 24 bits acierta casi siempre; "casi" es
    // justo lo que producía un `GL_INVALID_OPERATION` mudo y una textura de profundidad sin escribir.
    //
    // El RHI solo tiene dos formatos de profundidad, así que la respuesta es binaria: coma flotante
    // (32F) o punto fijo. Cualquier punto fijo se mapea a D24S8, que es lo que crea GL para él.
    Format GLDevice::backbufferDepthFormat()
    {
        GLint prev = 0;
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        GLint type = GL_NONE, bits = 0;
        glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_DEPTH,
                                              GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE, &type);
        glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_DEPTH,
                                              GL_FRAMEBUFFER_ATTACHMENT_DEPTH_SIZE, &bits);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)prev);
        return (type == GL_FLOAT && bits >= 32) ? Format::D32F : Format::D24S8;
    }

    uint32_t GLDevice::nativeTexture(TextureHandle h)
    {
        const GLTexture* t = texture(h);
        return t ? t->id : 0;
    }

    uint32_t GLDevice::nativeFramebuffer(RenderPassHandle h)
    {
        const GLRenderTarget* rt = renderTarget(h);
        return rt ? rt->fbo : 0;
    }

    uint32_t GLDevice::nativeProgram(PipelineHandle h)
    {
        const GLPipeline* p = pipeline(h);
        return p ? p->program : 0;
    }

    uint32_t GLDevice::nativeBuffer(BufferHandle h)
    {
        const GLBuffer* b = buffer(h);
        return b ? b->id : 0;
    }

    uint64_t GLDevice::imguiTextureId(TextureHandle h)
    {
        // ImGui_ImplOpenGL3: el ImTextureID ES el nombre de textura de GL.
        const GLTexture* t = texture(h);
        return t ? (uint64_t)t->id : 0;
    }

    const void* GLDevice::mappedData(BufferHandle h)
    {
        const GLBuffer* b = buffer(h);
        return b ? b->mapped : nullptr;
    }

    // ------------------------------------------------------------------ destrucción
    // libera el objeto GL y recicla el slot (free-list).
    void GLDevice::destroy(BufferHandle h)   { if (const GLBuffer* b = buffer(h))   { GLuint id = b->id;  if (b->mapped) glUnmapNamedBuffer(id); glDeleteBuffers(1, &id);  release(m_buffers, m_freeBuffers, h.id); } }
    void GLDevice::destroy(TextureHandle h)  { if (const GLTexture* t = texture(h)) { GLuint id = t->id;  glDeleteTextures(1, &id); release(m_textures, m_freeTextures, h.id); } }
    void GLDevice::destroy(SamplerHandle h)  { if (const GLSampler* s = sampler(h)) { GLuint id = s->id;  glDeleteSamplers(1, &id); release(m_samplers, m_freeSamplers, h.id); } }

    void GLDevice::destroy(PipelineHandle h)
    {
        if (const GLPipeline* p = pipeline(h))
        {
            if (p->program) glDeleteProgram(p->program);
            if (p->vao)     { GLuint vao = p->vao; glDeleteVertexArrays(1, &vao); }
            release(m_pipelines, m_freePipelines, h.id);
        }
    }

    void GLDevice::destroy(RenderPassHandle h)
    {
        if (const GLRenderTarget* rt = renderTarget(h))
        {
            for (TextureHandle c : rt->colors) destroy(c);
            if (RHI::valid(rt->depthTex)) destroy(rt->depthTex);
            if (rt->fbo)      { GLuint fbo = rt->fbo; glDeleteFramebuffers(1, &fbo); }
            if (rt->depthRbo) { GLuint rbo = rt->depthRbo; glDeleteRenderbuffers(1, &rbo); }
            release(m_targets, m_freeTargets, h.id);
        }
    }

    // ------------------------------------------------------------------ frame
    Context* GLDevice::beginFrame()
    {
        return m_context.get();
    }

    void GLDevice::endFrame()
    {
        SDL_GL_SwapWindow(m_window);
    }

    void GLDevice::readPixels(int x, int y, int w, int h, Format format, void* data)
    {
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        if (format == Format::RGBA8) {
            glReadPixels(x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, data);
        } else if (format == Format::R32F) {
            glReadPixels(x, y, w, h, GL_DEPTH_COMPONENT, GL_FLOAT, data);
        } else if (format == Format::RGB8) {
            glReadPixels(x, y, w, h, GL_RGB, GL_UNSIGNED_BYTE, data);
        }
    }

    void GLDevice::updateCubemapFace(TextureHandle th, int face, int width, int height,
                                      Format format, const void* data)
    {
        const GLTexture* t = texture(th);
        if (!t) return;
        GLTexFmt fmt = texFmt(format);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        // Use glTextureSubImage3D with the face as the z-offset (layer).
        glTextureSubImage3D(t->id, 0, 0, 0, face, (GLsizei)width, (GLsizei)height, 1,
                            fmt.format, fmt.type, data);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    }
}
