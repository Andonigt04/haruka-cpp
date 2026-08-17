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
#include <vector>
#include <cstddef>

namespace Haruka::RHI
{
    class Context;

    class Device
    {
        public:
            virtual ~Device() = default;

            /** @brief Una GPU que el sistema ofrece para dibujar. */
            struct AdapterInfo {
                std::string name;              ///< Nombre del driver (`deviceName` / `GL_RENDERER`).
                bool        discrete = false;  ///< GPU dedicada (frente a integrada o software).
                bool        usable   = true;   ///< Tiene cola de graphics+present sobre esta ventana.
            };

            /** @brief GPUs disponibles para `backend`, en el orden en que las da el sistema.
             *
             *  Existe para que la UI pueda OFRECER una lista en vez de que el usuario adivine un
             *  nombre para `HARUKA_VK_GPU`. Es una consulta pura: no crea el device ni deja estado.
             *
             *  ⚠️ OpenGL devuelve UNA sola entrada y no es una limitación del motor: GL no expone
             *  enumeración de adaptadores. Qué GPU usa un contexto GL lo decide el driver/SO antes de
             *  que el proceso arranque (`DRI_PRIME`, la configuración de Optimus/PRIME), así que en GL
             *  la lista es informativa y elegir no puede tener efecto. La selección real es de Vulkan.
             *
             *  En Vulkan se crea una instancia TEMPORAL para enumerar y se destruye antes de volver:
             *  llamarlo desde el panel de ajustes no puede interferir con el device en uso. */
            static std::vector<AdapterInfo> enumerateAdapters(Backend backend, SDL_Window* window);

            /** @brief Fábrica: elige backend y crea el device sobre una ventana SDL existente.
             *
             *  `preferredGpus` son NOMBRES por orden de preferencia (subcadena, sin distinguir
             *  mayúsculas); vacío = elección automática (discreta > integrada). Es una LISTA y no un
             *  nombre suelto a propósito: hoy solo se usa la primera, pero un reparto multi-GPU
             *  (render en una, compute en otra) necesita expresar "esta y luego esta", y cambiar la
             *  forma del ajuste después obligaría a migrar la configuración guardada de los usuarios.
             *
             *  ⚠️ Se guarda el NOMBRE, no el índice. El orden en que el sistema enumera las GPUs
             *  cambia al actualizar drivers, al conectar un eGPU o al arrancar en otra máquina, así
             *  que un índice guardado apunta mañana a otra tarjeta — y el usuario no entendería por
             *  qué. Un nombre que ya no está simplemente no casa y se cae a la elección automática. */
            static std::unique_ptr<Device> create(Backend backend, SDL_Window* window,
                                                  const std::vector<std::string>& preferredGpus = {});

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
             * @brief Formato de profundidad del BACKBUFFER (el render target por defecto).
             *
             * ⚠️ Existe porque un blit de PROFUNDIDAD exige que los dos formatos sean IDÉNTICOS —no
             * compatibles, idénticos— y el backbuffer no lo elige el motor: lo elige SDL/el driver.
             * Copiar la profundidad de pantalla a un RT `D32F` daba
             * `GL_INVALID_OPERATION: Depth formats do not match`, el blit no se hacía, y la textura de
             * profundidad que consumen las nubes y el fluido se quedaba con basura: ocluían mal contra
             * la escena. El síntoma era "las nubes se ven a través del terreno".
             *
             * Se CONSULTA en vez de suponerse: SDL no fija el tamaño del buffer de profundidad en este
             * motor, así que 24 bits es lo habitual pero no una garantía.
             */
            virtual Format             backbufferDepthFormat() = 0;

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

            // ImGui (fase 7): el host inicia el renderer de UI del backend cuando corresponde
            // (Vulkan → ImGui_ImplVulkan; GL → no-op, se usa el impl GL existente). false = no-op.
            virtual bool initUi() { return false; }

            /** @brief Tamaño real (en píxeles) del framebuffer / swapchain del backbuffer. Devuelve 0,0
             *         si no está disponible. Lo usa el host para alinear el viewport de ImGui con el
             *         tamaño de presentación (físico), que puede diferir del tamaño lógico de SDL. */
            virtual void framebufferSize(uint32_t& w, uint32_t& h) const { w = 0; h = 0; }

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
