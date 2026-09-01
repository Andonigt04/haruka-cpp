#if defined(__has_include)
#  if __has_include(<vulkan/vulkan.h>)
#    define HARUKA_RHI_HAS_VULKAN 1
#  endif
#endif

#if defined(HARUKA_RHI_HAS_VULKAN)

#include "rhi/vulkan/vk_pipeline.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <set>

#include "core/logger.h"

// Reutiliza la raíz de los #include del RHI (la fija RHI::setShaderIncludeDir, ver rhi_device.cpp).
namespace Haruka::RHI::opengl { std::string& shaderIncludeDir(); }

namespace Haruka::RHI::vulkan
{
    static bool vkSuccess(VkResult r, const char* what)
    {
        if (r == VK_SUCCESS) return true;
        HARUKA_LOGE("RHI/VK", "%s -> VkResult %d", what, (int)r);
        return false;
    }

    // ------------------------------------------------------------------ includes textuales
    // Copia del resolver de gl_device.cpp: el GLSL del motor usa #include textual que se resuelve
    // ANTES de compilar (GL no lo soporta nativo). Para VK hay que hacer lo mismo y quitar además
    // GL_GOOGLE_include_directive, que glslang SÍ interpretaría (y nosotros ya resolvimos).
    static std::string resolveIncludes(const std::string& src, const std::string& baseDir, int depth = 0)
    {
        if (src.find("#include") == std::string::npos) return src;   // caso común: sin coste
        if (depth > 8) {
            HARUKA_LOGE("RHI/VK", "#include: profundidad > 8 (¿ciclo?)");
            return src;
        }
        std::string out;
        out.reserve(src.size() + 4096);
        size_t pos = 0;
        while (pos < src.size()) {
            size_t eol  = src.find('\n', pos);
            if (eol == std::string::npos) eol = src.size();
            std::string line = src.substr(pos, eol - pos);

            if (line.find("GL_GOOGLE_include_directive") != std::string::npos) {
                pos = eol + 1;
                continue;
            }

            size_t h = line.find("#include");
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
                        HARUKA_LOGE("RHI/VK", "#include no encontrado: %s", full.c_str());
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

    // ------------------------------------------------------------------ compilación GLSL→SPIR-V
    static uint64_t fnv1a(const std::string& s)
    {
        uint64_t h = 1469598103934665603ull;
        for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
        return h;
    }

    // Suffix de etapa para glslangValidator (-S vert/frag/geom/tesc/tese/comp).
    static const char* stageSuffix(VkShaderStageFlagBits stage)
    {
        switch (stage)
        {
            case VK_SHADER_STAGE_VERTEX_BIT:             return "vert";
            case VK_SHADER_STAGE_FRAGMENT_BIT:           return "frag";
            case VK_SHADER_STAGE_GEOMETRY_BIT:           return "geom";
            case VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT:    return "tesc";
            case VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT: return "tese";
            case VK_SHADER_STAGE_COMPUTE_BIT:            return "comp";
            default: return nullptr;
        }
    }

    // Reemplaza los builtins GL por su nombre SPIR-V (Vulkan). GLSL 450 los llama gl_VertexID /
    // gl_InstanceID; para generar SPIR-V válido glslang exige gl_VertexIndex / gl_InstanceIndex.
    // Los shaders del engine se reutilizan para GL y Vulkan → se renombra aquí, no en el fuente.
    static std::string vulkanizeGlsl(const std::string& src)
    {
        std::string out = src;
        auto repl = [&](const char* a, const char* b) {
            const std::string A(a), B(b);
            for (size_t p = 0; (p = out.find(A, p)) != std::string::npos; p += B.size())
                out.replace(p, A.size(), B);
        };
        repl("gl_VertexID",  "gl_VertexIndex");
        repl("gl_InstanceID", "gl_InstanceIndex");
        return out;
    }

    static std::vector<uint32_t> readSpv(const std::string& path)
    {
        std::ifstream f(path, std::ios::binary | std::ios::ate);
        if (!f.is_open()) return {};
        auto n = (std::streamsize)f.tellg(); f.seekg(0);
        std::vector<uint32_t> out((size_t)n / sizeof(uint32_t));
        if (!out.empty()) f.read(reinterpret_cast<char*>(out.data()), n - n % sizeof(uint32_t));
        return out;
    }

    std::vector<uint32_t> compileGlslToSpv(const std::string& source, VkShaderStageFlagBits stage)
    {
        if (source.empty()) return {};
        const char* s = stageSuffix(stage);
        if (!s) { HARUKA_LOGE("RHI/VK", "compileGlslToSpv: etapa no soportada (%d)", (int)stage); return {}; }

        // ⚠️ EL HASH INCLUYE LAS OPCIONES DEL COMPILADOR, NO SOLO LA FUENTE. El .spv se guarda en
        // disco (ver abajo), asi que si cambian los shifts de binding y el hash no lo reflejara, se
        // reutilizaria un SPIR-V con el layout viejo — un fallo silencioso y dificilisimo de ver.
        const std::string opts = "|ssbo=" + std::to_string(kSsboBindingBase)
                               + "|tex="  + std::to_string(kTextureBindingBase) + "|v2";
        const uint64_t hash = fnv1a(vulkanizeGlsl(source) + "\n@" + s + opts);
        static std::map<uint64_t, std::vector<uint32_t>> cache;
        if (auto it = cache.find(hash); it != cache.end()) return it->second;

        const char* tmp = std::getenv("TMPDIR");
        if (!tmp || !*tmp) tmp = "/tmp";
        const std::string inPath  = std::string(tmp) + "/haruka_" + std::to_string(hash) + ".glsl";
        const std::string outPath = std::string(tmp) + "/haruka_" + std::to_string(hash) + ".spv";

        // ── EL .spv SE REUTILIZA ENTRE EJECUCIONES ──────────────────────────────────────────────
        //
        // ⚠️ ESTO ERA UN SEGUNDO DE ARRANQUE. Cada shader que no viene precompilado se traducia
        // forkeando `glslangValidator` con `std::system` —escribir .glsl, fork+exec, leer .spv— y
        // **el .spv se borraba al terminar**. La unica cache era un `std::map` en memoria, que muere
        // con el proceso: se pagaba entero en CADA arranque. Medido en esta maquina: **6 ms por
        // fork**, y en el log de Andoni salen ~30 shaders por arranque en Vulkan.
        //
        // El SPIR-V es independiente del fabricante, asi que la cache sirve igual al cambiar de GPU
        // — que es justo cuando el se lo encontro (AMD -> NVIDIA -> AMD).
        if (std::vector<uint32_t> pre = readSpv(outPath); !pre.empty()) {
            cache.emplace(hash, pre);
            return pre;
        }

        {
            std::ofstream f(inPath);
            if (!f.is_open()) { HARUKA_LOGE("RHI/VK", "compileGlslToSpv: no se pudo escribir %s", inPath.c_str()); return {}; }
            f << vulkanizeGlsl(source);
        }

        // Shifts por tipo: separan UBO/SSBO/textura en el espacio de binding ÚNICO de Vulkan,
        // con las MISMAS constantes de vk_common.h (§4.3). Si cambian aquí, cambian también los
        // helpers uboSlotToBinding/ssboSlotToBinding/textureSlotToBinding.
        const std::string errPath = std::string(tmp) + "/haruka_" + std::to_string(hash) + ".err";
        const std::string cmd =
            "glslangValidator -V -S " + std::string(s) +
            " --auto-map-locations" +
            " --shift-ssbo-binding " + std::to_string(kSsboBindingBase) +
            " --shift-combined-sampler-binding " + std::to_string(kTextureBindingBase) +
            " --shift-texture-binding " + std::to_string(kTextureBindingBase) +
            " \"" + inPath + "\" -o \"" + outPath + "\" 2>\"" + errPath + "\"";

        const int rc = std::system(cmd.c_str());
        std::vector<uint32_t> spv = readSpv(outPath);
        std::remove(inPath.c_str());
        // ⚠️ EL .spv NO SE BORRA: es la cache entre ejecuciones (ver arriba). Solo se limpia si la
        // compilacion fallo, para no dejar un fichero vacio que luego se lea como valido.
        if (spv.empty()) std::remove(outPath.c_str());

        if (rc != 0 || spv.empty())
        {
            // La primera vez de cada fuente, volcamos el error REAL de glslang (si no, es un misterio
            // cuál shader/pipeline falla). Deduplicado por hash de fuente.
            static std::set<uint64_t> reported;
            std::ifstream ef(errPath);
            std::string err((std::istreambuf_iterator<char>(ef)), std::istreambuf_iterator<char>());
            std::remove(errPath.c_str());
            if (!err.empty() && reported.insert(hash).second)
                HARUKA_LOGE("RHI/VK", "glslang falló (etapa %s):\n%s", s, err.c_str());
            else
                HARUKA_LOGW("RHI/VK", "glslangValidator falló o no generó SPIR-V (etapa %s) — shader omitido.", s);
            return {};
        }
        std::remove(errPath.c_str());

        cache[hash] = spv;
        return spv;
    }

    std::vector<uint32_t> loadStageSpv(const char* path, const char* source,
                                       const void* bytes, size_t size,
                                       VkShaderStageFlagBits stage)
    {
        // 1. GLSL inline (misma prioridad que GL): se compila a SPIR-V con los shifts.
        if (source && *source)
            return compileGlslToSpv(resolveIncludes(source, Haruka::RHI::opengl::shaderIncludeDir()), stage);

        // 2. Ruta completa: GLSL source-first, igual que GL. OJO: el ".spv" que genera el build del
        //    motor es para GL (--target-env=opengl, sin shifts) → NO consumible por Vulkan. Solo
        //    vale el source re-encodificado aquí con -V + shifts.
        if (path)
        {
            std::ifstream f(path);
            if (f.is_open())
            {
                std::string src((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
                return compileGlslToSpv(resolveIncludes(src, Haruka::RHI::opengl::shaderIncludeDir()), stage);
            }
            HARUKA_LOGW("RHI/VK", "shader GLSL no encontrado (el backend VK necesita el source, no el .spv de GL): %s", path);
            return {};
        }

        // 3. Bytes SPIR-V directos (shaders generados): tal cual.
        if (bytes && size)
        {
            const size_t n = size - size % sizeof(uint32_t);
            std::vector<uint32_t> out(n / sizeof(uint32_t));
            if (!out.empty()) std::memcpy(out.data(), bytes, n);
            return out;
        }

        return {};
    }

    // ------------------------------------------------------------------ layouts compartidos
    VkDescriptorSetLayout createSharedSetLayout(VkDevice device)
    {
        // Los 96 bindings del set 0, todos PARTIALLY_BOUND: un slot sin recurso no invalida el set
        // (el motor GL bindea solo el subconjunto que cada pipeline usa en cada draw).
        std::vector<VkDescriptorSetLayoutBinding> bindings;
        bindings.reserve(kTotalBindings);
        const VkShaderStageFlags stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT |
                                          VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT | VK_SHADER_STAGE_GEOMETRY_BIT |
                                          VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
        for (uint32_t i = 0; i < kTotalBindings; ++i)
        {
            VkDescriptorType type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            if (i >= kUboBindingCount && i < kSsboBindingBase + kSsboBindingCount) type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            else if (i >= kSsboBindingBase + kSsboBindingCount)                    type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bindings.push_back({ i, type, 1, stages, nullptr });
        }

        std::vector<VkDescriptorBindingFlags> flags(kTotalBindings, VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT);
        VkDescriptorSetLayoutBindingFlagsCreateInfo fci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO };
        fci.bindingCount = (uint32_t)flags.size();
        fci.pBindingFlags = flags.data();

        VkDescriptorSetLayoutCreateInfo ci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        ci.bindingCount = (uint32_t)bindings.size();
        ci.pBindings = bindings.data();
        ci.pNext = &fci;

        VkDescriptorSetLayout layout = VK_NULL_HANDLE;
        if (vkCreateDescriptorSetLayout(device, &ci, nullptr, &layout) != VK_SUCCESS)
        {
            // HW sin descriptorBindingPartiallyBound: reintenta SIN el flag (exige bindear TODOS los
            // slots antes de usar el set — menos eficiente, pero funcional en Vulkan < 1.2).
            HARUKA_LOGW("RHI/VK", "descriptorBindingPartiallyBound no soportado → set sin PARTIALLY_BOUND");
            ci.pNext = nullptr;
            if (vkCreateDescriptorSetLayout(device, &ci, nullptr, &layout) != VK_SUCCESS)
            {
                HARUKA_LOGE("RHI/VK", "createSharedSetLayout: no se pudo crear el descriptor set layout");
                return VK_NULL_HANDLE;
            }
        }
        return layout;
    }

    VkPipelineLayout createSharedPipelineLayout(VkDevice device, VkDescriptorSetLayout setLayout)
    {
        VkPipelineLayoutCreateInfo pl{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        pl.setLayoutCount = 1;
        pl.pSetLayouts = &setLayout;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        if (vkCreatePipelineLayout(device, &pl, nullptr, &layout) != VK_SUCCESS)
        {
            HARUKA_LOGE("RHI/VK", "createSharedPipelineLayout: fallo al crear el pipeline layout");
            return VK_NULL_HANDLE;
        }
        return layout;
    }

    // ------------------------------------------------------------------ builders de pipeline
    // TODO el estado de bindPipeline de GL (gl_context.cpp) es aquí INMUTABLE dentro del pipeline.
    // El VKContext de la fase 5 solo hará vkCmdBindPipeline + bindear el descriptor set compartido.

    VkPipeline buildGraphicsPipeline(VkDevice device, const PipelineDesc& d,
                                     const std::vector<std::vector<uint32_t>>& spv,
                                     VkPipelineLayout layout, VkRenderPass renderPass,
                                     uint32_t colorAttachmentCount)
    {
        if (!device || !layout || !renderPass) return VK_NULL_HANDLE;
        if (spv.size() < kStageCount || spv[kStageVert].empty() || spv[kStageFrag].empty())
        {
            HARUKA_LOGE("RHI/VK", "buildGraphicsPipeline: faltan las etapas vertex/fragment");
            return VK_NULL_HANDLE;
        }

        // Módulos de shader (se destruyen al salir; el pipeline ya es un objeto autónomo).
        const VkShaderStageFlagBits stageOf[kStageCount] = {
            VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT, VK_SHADER_STAGE_GEOMETRY_BIT,
            VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT, VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT,
            VK_SHADER_STAGE_COMPUTE_BIT };
        VkShaderModule mods[kStageCount] = {};
        for (uint32_t i = 0; i < kStageCount; ++i)
        {
            if (spv[i].empty()) continue;
            VkShaderModuleCreateInfo ci{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
            ci.codeSize = spv[i].size() * sizeof(uint32_t);
            ci.pCode = spv[i].data();
            vkCreateShaderModule(device, &ci, nullptr, &mods[i]);
        }
        auto cleanup = [&]() { for (auto m : mods) if (m) vkDestroyShaderModule(device, m, nullptr); };

        std::vector<VkPipelineShaderStageCreateInfo> stages;
        for (uint32_t i = 0; i < kStageCount; ++i)
        {
            if (!mods[i]) continue;
            VkPipelineShaderStageCreateInfo s{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
            s.stage = stageOf[i];
            s.module = mods[i];
            s.pName = "main";
            stages.push_back(s);
        }
        if (stages.empty()) { cleanup(); HARUKA_LOGE("RHI/VK", "buildGraphicsPipeline: sin módulos válidos"); return VK_NULL_HANDLE; }

        // TESELACIÓN: control + evaluación van juntas o ninguna (igual que GL).
        const bool wantsTess = !spv[kStageTesc].empty() && !spv[kStageTese].empty();

        // Vertex input: MÁX 8 bindings, un stride por binding (como GL). rates vacío/corto = Vertex.
        std::vector<VkVertexInputBindingDescription> bindings;
        std::vector<VkVertexInputAttributeDescription> attrs;
        for (size_t i = 0; i < std::min<size_t>(d.vertexLayout.strides.size(), 8); ++i)
        {
            const bool perInstance = (i < d.vertexLayout.rates.size()) &&
                                     (d.vertexLayout.rates[i] == InputRate::Instance);
            bindings.push_back({ (uint32_t)i, d.vertexLayout.strides[i],
                                 perInstance ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX });
        }
        for (const VertexAttribute& a : d.vertexLayout.attributes)
        {
            if (a.binding >= 8) continue;
            attrs.push_back({ a.location, a.binding, vertexFmt(a.format), a.offset });
        }

        VkPipelineVertexInputStateCreateInfo vin{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        vin.vertexBindingDescriptionCount = (uint32_t)bindings.size();
        vin.pVertexBindingDescriptions = bindings.empty() ? nullptr : bindings.data();
        vin.vertexAttributeDescriptionCount = (uint32_t)attrs.size();
        vin.pVertexAttributeDescriptions = attrs.empty() ? nullptr : attrs.data();

        VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
        ia.topology = toTopology(d.topology);
        ia.primitiveRestartEnable = VK_FALSE;

        // ⚠️ ORIGEN DEL DOMINIO DE TESELACIÓN: OpenGL usa LOWER_LEFT, Vulkan usa UPPER_LEFT.
        //
        // Sin declararlo, `gl_TessCoord.y` llega INVERTIDO respecto a lo que esperan unos shaders
        // escritos para GL, y cada parche se evalúa espejado en v. El síntoma no se parece a la
        // causa: la geometría normal sale perfecta y SOLO el terreno teselado aparece donde no debe
        // —"terreno en el cielo"—, porque es lo único que pasa por el teselador. Se acorraló con
        // `HARUKA_NO_TESS=1`: sin teselar, todo correcto.
        //
        // `VkPipelineTessellationDomainOriginStateCreateInfo` es core desde Vulkan 1.1
        // (antes `VK_KHR_maintenance2`) y deja la convención igual que GL sin tocar un shader.
        VkPipelineTessellationDomainOriginStateCreateInfo tdo{
            VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_DOMAIN_ORIGIN_STATE_CREATE_INFO };
        tdo.domainOrigin = VK_TESSELLATION_DOMAIN_ORIGIN_LOWER_LEFT;   // la de OpenGL

        VkPipelineTessellationStateCreateInfo ts{ VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO };
        ts.pNext = &tdo;
        ts.patchControlPoints = (d.topology == PrimitiveTopology::Patches) ? d.patchVertices : 0;

        VkPipelineViewportStateCreateInfo vp{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
        vp.viewportCount = 1;
        vp.scissorCount = 1;   // viewport/scissor DINÁMICOS → el resize no recrea el pipeline

        VkPipelineRasterizationStateCreateInfo rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
        rs.depthClampEnable = VK_FALSE;
        rs.rasterizerDiscardEnable = VK_FALSE;
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = toCull(d.cull);
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rs.depthBiasEnable = (d.depth.biasConstant != 0.0f || d.depth.biasSlope != 0.0f);
        rs.depthBiasConstantFactor = d.depth.biasConstant;
        rs.depthBiasSlopeFactor = d.depth.biasSlope;
        rs.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineDepthStencilStateCreateInfo ds{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
        ds.depthTestEnable = d.depth.test;
        ds.depthWriteEnable = d.depth.write;
        ds.depthCompareOp = toCompare(d.depth.compare);   // reversed-Z: Greater por defecto
        ds.stencilTestEnable = VK_FALSE;

        VkPipelineColorBlendAttachmentState cba{};
        cba.colorWriteMask = 0xF;
        cba.blendEnable = d.blend.enable;
        if (d.blend.enable)
        {
            const VKBlend bl = toBlend(d.blend.mode);
            cba.srcColorBlendFactor = bl.srcFactor;
            cba.dstColorBlendFactor = bl.dstFactor;
            cba.colorBlendOp = VK_BLEND_OP_ADD;
            cba.srcAlphaBlendFactor = bl.srcFactor;
            cba.dstAlphaBlendFactor = bl.dstFactor;
            cba.alphaBlendOp = VK_BLEND_OP_ADD;
        }
        // Estado de blend POR attachment: el nº debe coincidir con los color attachments del pass
        // usado (VK lo exige). La fase 4 solo los construía contra el backbuffer (1); la fase 5
        // pasa el conteo real del target (variante por render pass).
        const uint32_t nColor = std::max(1u, colorAttachmentCount);
        std::vector<VkPipelineColorBlendAttachmentState> cbState(nColor);
        for (uint32_t i = 0; i < nColor; ++i)
            cbState[i] = cba;
        VkPipelineColorBlendStateCreateInfo cb{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        cb.attachmentCount = nColor;
        cb.pAttachments = cbState.data();

        VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dsDyn{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
        dsDyn.dynamicStateCount = 2;
        dsDyn.pDynamicStates = dyn;

        VkGraphicsPipelineCreateInfo gp{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        gp.stageCount = (uint32_t)stages.size();
        gp.pStages = stages.data();
        gp.pVertexInputState = &vin;
        gp.pInputAssemblyState = &ia;
        if (wantsTess) gp.pTessellationState = &ts;
        gp.pViewportState = &vp;
        gp.pRasterizationState = &rs;
        gp.pMultisampleState = &ms;
        gp.pDepthStencilState = &ds;
        gp.pColorBlendState = &cb;
        gp.pDynamicState = &dsDyn;
        gp.layout = layout;
        gp.renderPass = renderPass;
        gp.subpass = 0;

        VkPipeline pipeline = VK_NULL_HANDLE;
        const VkResult r = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gp, nullptr, &pipeline);
        cleanup();
        if (!vkSuccess(r, "create graphics pipeline")) return VK_NULL_HANDLE;
        return pipeline;
    }

    VkPipeline buildComputePipeline(VkDevice device, const std::vector<uint32_t>& spv,
                                    VkPipelineLayout layout)
    {
        if (!device || !layout || spv.empty())
        {
            HARUKA_LOGE("RHI/VK", "buildComputePipeline: sin SPIR-V o layout inválido");
            return VK_NULL_HANDLE;
        }

        VkShaderModuleCreateInfo ci{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        ci.codeSize = spv.size() * sizeof(uint32_t);
        ci.pCode = spv.data();
        VkShaderModule m = VK_NULL_HANDLE;
        if (!vkSuccess(vkCreateShaderModule(device, &ci, nullptr, &m), "create compute module")) return VK_NULL_HANDLE;

        VkPipelineShaderStageCreateInfo st{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
        st.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        st.module = m;
        st.pName = "main";

        VkComputePipelineCreateInfo cpi{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
        cpi.stage = st;
        cpi.layout = layout;

        VkPipeline pipeline = VK_NULL_HANDLE;
        const VkResult r = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpi, nullptr, &pipeline);
        vkDestroyShaderModule(device, m, nullptr);
        if (!vkSuccess(r, "create compute pipeline")) return VK_NULL_HANDLE;
        return pipeline;
    }
}

#endif // HARUKA_RHI_HAS_VULKAN
