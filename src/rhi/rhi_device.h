/**
 * @file rhi_device.h
 * @brief Interfaz del "device": crea/destruye recursos GPU y gobierna el frame.
 *
 * El Device vive todo el programa. La fábrica Device::create elige el backend concreto
 * (GLDevice / VKDevice) y devuelve la interfaz. El resto del motor solo ve esta clase.
 *
 * @par API Status — FROZEN
 * Breaking changes will not be made without a major version bump.
 */
#pragma once
#include <string>

#include "rhi_types.h"
#include "rhi_resources.h"

#include <SDL3/SDL.h>
#include <memory>
#include <cstddef>

namespace Haruka::RHI
{
    class Context;

    class Device
    {
        public:
            virtual ~Device() = default;

            /** @brief Fábrica: elige backend y crea el device sobre una ventana SDL existente. */
            static std::unique_ptr<Device> create(Backend backend, SDL_Window* window);

            // --- Creación de recursos (lo que hoy hacen los ctors de tus wrappers RAII). ---
            virtual BufferHandle       createBuffer(BufferUsage, size_t bytes, const void* data,
                                                    BufferMemory = BufferMemory::Static) = 0;
            virtual void               updateBuffer(BufferHandle, size_t offset, size_t bytes,
                                                    const void* data) = 0;
            /** @brief Reasigna y sube (glBufferData). Solo para buffers BufferMemory::Stream
             *  (mallas dinámicas que cambian de tamaño). Para tamaño fijo usa updateBuffer. */
            virtual void               uploadBuffer(BufferHandle, size_t bytes, const void* data) = 0;
            /** @brief Copia GPU→GPU entre buffers (glCopyBufferSubData). Para realloc que preserva
             *  datos (p.ej. growPool del terreno). */
            virtual void               copyBuffer(BufferHandle src, BufferHandle dst,
                                                  size_t srcOffset, size_t dstOffset, size_t bytes) = 0;
            /** @brief Puntero mapeado de un buffer BufferMemory::Readback (nullptr en el resto).
             *  El mapeo es PERSISTENTE: se hace al crear y vive hasta destroy(). Leerlo solo es
             *  válido tras esperar la fence del trabajo que lo escribió. */
            virtual const void*        mappedData(BufferHandle) = 0;
            virtual TextureHandle      createTexture(const TextureDesc&) = 0;
            virtual SamplerHandle      createSampler(const SamplerDesc&) = 0;
            virtual PipelineHandle     createPipeline(const PipelineDesc&) = 0;
            virtual RenderPassHandle   createRenderTarget(const RenderTargetDesc&) = 0;

            /** @brief Textura de color de un render target (attachment `index`) para muestrearla. */
            virtual TextureHandle      getColorTexture(RenderPassHandle, uint32_t index = 0) = 0;
            /** @brief Textura de profundidad de un render target (shadow maps; requiere depthAsTexture/Cube). */
            virtual TextureHandle      getDepthTexture(RenderPassHandle) = 0;

            /**
             * @brief Escotilla de escape: id NATIVO del backend (en GL, el GLuint de la textura).
             *
             * Necesaria SOLO durante la migración: código que aún llama a GL directo (ImGui,
             * glBindTexture) necesita el id crudo. Desaparecerá cuando todo pase por el Context.
             */
            virtual uint32_t           nativeTexture(TextureHandle) = 0;
            /** @brief Escotilla de escape: id NATIVO del FBO de un render target (GLuint en GL). */
            virtual uint32_t           nativeFramebuffer(RenderPassHandle) = 0;
            /** @brief Escotilla de escape: id NATIVO del programa de un pipeline (GLuint en GL).
             *  Para los `setUniform` sueltos que aún usan glUniform durante el leaf-swap. */
            virtual uint32_t           nativeProgram(PipelineHandle) = 0;
            /** @brief Escotilla de escape: id NATIVO de un buffer (GLuint en GL).
             *  Para el ensamblado de VAO/attrib-pointers que sigue en GL hasta el refactor PSO. */
            virtual uint32_t           nativeBuffer(BufferHandle) = 0;

            // --- Destrucción (lo que hoy hacen los dtors ~VertexBuffer, etc.). ---
            virtual void destroy(BufferHandle) = 0;
            virtual void destroy(TextureHandle) = 0;
            virtual void destroy(SamplerHandle) = 0;
            virtual void destroy(PipelineHandle) = 0;
            virtual void destroy(RenderPassHandle) = 0;

            // --- Frame: obtener el Context de este frame y presentarlo (swap). ---
            virtual Context* beginFrame() = 0;
            virtual void     endFrame() = 0;

            virtual Backend backend() const = 0;

            /** @brief Updates a single face of a cubemap texture with pixel data.
             *  Used by IBL sky generation. The texture must have been created with cube=true. */
            virtual void updateCubemapFace(TextureHandle tex, int face, int width, int height,
                                            Format format, const void* data) = 0;

            /** @brief Lee píxeles del framebuffer activo a RAM (screenshots).
             *  x,y,w,h = región en píxeles. format = RGBA8 o R32F (depth). data debe tener tamaño suficiente.
             *  ⚠️ Sincroniza con la GPU (glReadPixels). Solo para depuración/capturas. */
            virtual void readPixels(int x, int y, int w, int h, Format format, void* data) = 0;
    };

    // -----------------------------------------------------------------------------------
    // Device global. Lo fija Application tras crear el device. Permite que los wrappers
    // RAII (Texture, RenderTarget…) lleguen al device sin arrastrarlo por sus constructores.
    // Durante la migración puede ser null (modo editor/headless) -> el wrapper cae a GL directo.
    // -----------------------------------------------------------------------------------
    Device* device();
    void    setDevice(Device*);

    /**
     * @brief Raíz donde se resuelven los `#include "..."` de los shaders (con "/" final).
     *
     * GLSL no tiene #include, así que lo resuelve el backend por texto antes de compilar. Sin una
     * raíz, un shader COMPILADO DESDE CADENA (los inline del SimplePlanet) no tendría contra qué
     * resolver: no hay fichero del que colgar la ruta relativa. La fija `Shader::setBaseDir`.
     */
    void setShaderIncludeDir(const std::string&);
}
