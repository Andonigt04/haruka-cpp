/**
 * @file gl_device.cpp
 * @brief Implementación OpenGL de RHI::Device: crea el contexto GL y gestiona recursos GPU.
 */
#include "rhi/opengl/gl_device.h"

#include <cstdio>
#include <cmath>
#include <algorithm>
#include <fstream>
#include <vector>
#include <string>
#include "core/logger.h"
#include "core/asset_paths.h"

namespace Haruka::RHI::opengl
{
    // ------------------------------------------------------------------ helpers internos
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
        }
        return sh;
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

    // Carga una etapa desde ruta COMPLETA, prefiriendo GLSL source y cayendo a ".spv".
    // Replica la política probada de Shader::loadSPV: glSpecializeShader en Mesa/AMD tiene
    // soporte incompleto (programas inválidos / cuelgues) → GLSL source es más fiable.
    static GLuint compileFromPath(GLenum stage, const char* fullPath)
    {
        if (!fullPath) return 0;

        // 1. GLSL source (presente en dev; fiable en todos los drivers).
        if (std::ifstream g(fullPath); g.is_open())
        {
            std::string src((std::istreambuf_iterator<char>(g)), std::istreambuf_iterator<char>());
            src = resolveIncludes(src, shaderIncludeDir());
            const char* p = src.c_str();
            GLuint sh = glCreateShader(stage);
            glShaderSource(sh, 1, &p, nullptr);
            glCompileShader(sh);
            GLint ok = 0; glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
            if (!ok) { char log[1024] = {0}; glGetShaderInfoLog(sh, sizeof(log), nullptr, log);
                       HARUKA_LOGE("RHI/GL", "GLSL compile [%s]: %s", fullPath, log); }
            return sh;
        }

        // 2. SPIR-V binario (producción: solo se envía ".spv").
        std::string spv = std::string(fullPath) + ".spv";
        if (std::ifstream f(spv, std::ios::binary | std::ios::ate); f.is_open())
        {
            auto n = (std::streamsize)f.tellg(); f.seekg(0);
            std::vector<char> buf(n); f.read(buf.data(), n);
            return compileSpirv(stage, buf.data(), (size_t)n);
        }

        HARUKA_LOGW("RHI/GL", "shader no encontrado: %s (ni %s)", fullPath, spv.c_str());
        return 0;
    }

    // Compila una etapa desde una cadena GLSL en línea (shaders generados/embebidos).
    static GLuint compileFromSource(GLenum stage, const char* source)
    {
        if (!source) return 0;
        const std::string src = resolveIncludes(source, shaderIncludeDir());
        const char* p = src.c_str();
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
