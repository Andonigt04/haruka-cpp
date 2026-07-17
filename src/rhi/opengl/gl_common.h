/**
 * @file gl_common.h
 * @brief Traducción de enums del RHI a constantes de OpenGL. Solo aquí (y en los .cpp del
 *        backend GL) se incluye <glad/glad.h>. Fuera de src/rhi/opengl/ no debe haber GL.
 */
#pragma once

#include <cstdio>

#include <glad/glad.h>
#include "rhi/rhi_types.h"

namespace Haruka::RHI::opengl
{
    /** @brief Descriptor de un atributo de vértice traducido a GL. */
    struct GLVertexFmt { GLint size; GLenum type; GLboolean normalized; };

    inline GLVertexFmt vertexFmt(Format f)
    {
        switch (f)
        {
            case Format::R32F:    return { 1, GL_FLOAT, GL_FALSE };
            case Format::RG32F:   return { 2, GL_FLOAT, GL_FALSE };
            case Format::RGB32F:  return { 3, GL_FLOAT, GL_FALSE };
            case Format::RGBA32F: return { 4, GL_FLOAT, GL_FALSE };
            case Format::RGBA8:   return { 4, GL_UNSIGNED_BYTE, GL_TRUE };
            // Empaquetados (el terreno): normal en 4 B y uv en 4 B.
            case Format::RGB10A2_SNORM: return { 4, GL_INT_2_10_10_10_REV, GL_TRUE };
            case Format::RG16F:         return { 2, GL_HALF_FLOAT, GL_FALSE };
            // El default ANTERIOR devolvía {4, GL_FLOAT} en silencio → un formato no mapeado
            // (p.ej. los empaquetados de arriba) habría leído BASURA del buffer sin avisar.
            // Ahora se queja: un layout mal declarado se ve al instante, no en pantalla.
            default:
                std::fprintf(stderr, "[RHI/GL] vertexFmt: formato de vertice NO soportado (%d)\n",
                             (int)f);
                return { 4, GL_FLOAT, GL_FALSE };
        }
    }

    /** @brief Descriptor de formato de textura traducido a GL (internal / format / type). */
    struct GLTexFmt { GLenum internal; GLenum format; GLenum type; };

    inline GLTexFmt texFmt(Format f)
    {
        switch (f)
        {
            case Format::RGBA8:   return { GL_RGBA8,               GL_RGBA,            GL_UNSIGNED_BYTE };
            case Format::RGBA16F: return { GL_RGBA16F,             GL_RGBA,            GL_HALF_FLOAT };
            case Format::RGB16F:  return { GL_RGB16F,              GL_RGB,             GL_FLOAT };
            case Format::RG16F:   return { GL_RG16F,               GL_RG,              GL_FLOAT };
            case Format::RGBA32F: return { GL_RGBA32F,             GL_RGBA,            GL_FLOAT };
            case Format::RGB32F:  return { GL_RGB32F,              GL_RGB,             GL_FLOAT };
            case Format::RG32F:   return { GL_RG32F,               GL_RG,              GL_FLOAT };
            case Format::RGB8:    return { GL_RGB8,                GL_RGB,             GL_UNSIGNED_BYTE };
            case Format::R11G11B10F: return { GL_R11F_G11F_B10F,  GL_RGB,             GL_FLOAT };
            case Format::R8:      return { GL_R8,                  GL_RED,             GL_UNSIGNED_BYTE };
            case Format::RG32UI:  return { GL_RG32UI,             GL_RG_INTEGER,      GL_UNSIGNED_INT };
            case Format::RGBA16UI:return { GL_RGBA16UI,           GL_RGBA_INTEGER,    GL_UNSIGNED_SHORT };
            case Format::R32F:    return { GL_R32F,                GL_RED,             GL_FLOAT };
            case Format::D24:     return { GL_DEPTH_COMPONENT24,   GL_DEPTH_COMPONENT, GL_UNSIGNED_INT };
            case Format::D24S8:   return { GL_DEPTH24_STENCIL8,    GL_DEPTH_STENCIL,   GL_UNSIGNED_INT_24_8 };
            case Format::D32F:    return { GL_DEPTH_COMPONENT32F,  GL_DEPTH_COMPONENT, GL_FLOAT };
            default:              return { GL_RGBA8,               GL_RGBA,            GL_UNSIGNED_BYTE };
        }
    }

    inline GLenum toTopology(PrimitiveTopology t)
    {
        switch (t)
        {
            case PrimitiveTopology::Triangles:     return GL_TRIANGLES;
            case PrimitiveTopology::TriangleStrip: return GL_TRIANGLE_STRIP;
            case PrimitiveTopology::Lines:         return GL_LINES;
            case PrimitiveTopology::LineStrip:     return GL_LINE_STRIP;
            case PrimitiveTopology::Points:        return GL_POINTS;
        }
        return GL_TRIANGLES;
    }

    inline GLenum toCompare(CompareOp c)
    {
        switch (c)
        {
            case CompareOp::Never:        return GL_NEVER;
            case CompareOp::Less:         return GL_LESS;
            case CompareOp::Equal:        return GL_EQUAL;
            case CompareOp::LessEqual:    return GL_LEQUAL;
            case CompareOp::Greater:      return GL_GREATER;
            case CompareOp::NotEqual:     return GL_NOTEQUAL;
            case CompareOp::GreaterEqual: return GL_GEQUAL;
            case CompareOp::Always:       return GL_ALWAYS;
        }
        return GL_LESS;
    }

    inline GLenum toTarget(BufferUsage u)
    {
        switch (u)
        {
            case BufferUsage::Vertex:  return GL_ARRAY_BUFFER;
            case BufferUsage::Index:   return GL_ELEMENT_ARRAY_BUFFER;
            case BufferUsage::Uniform: return GL_UNIFORM_BUFFER;
            case BufferUsage::Indirect: return GL_DRAW_INDIRECT_BUFFER;
            case BufferUsage::Storage: return GL_SHADER_STORAGE_BUFFER;
        }
        return GL_ARRAY_BUFFER;
    }

    inline GLenum toFilter(Filter f) { return f == Filter::Nearest ? GL_NEAREST : GL_LINEAR; }

    inline GLenum toWrap(Wrap w)
    {
        switch (w)
        {
            case Wrap::Repeat:         return GL_REPEAT;
            case Wrap::ClampToEdge:    return GL_CLAMP_TO_EDGE;
            case Wrap::MirroredRepeat: return GL_MIRRORED_REPEAT;
        }
        return GL_REPEAT;
    }

    inline GLenum shaderStage(ShaderStage s)
    {
        switch (s)
        {
            case ShaderStage::Vertex:   return GL_VERTEX_SHADER;
            case ShaderStage::Fragment: return GL_FRAGMENT_SHADER;
            case ShaderStage::Geometry: return GL_GEOMETRY_SHADER;
            case ShaderStage::Compute:  return GL_COMPUTE_SHADER;
        }
        return GL_VERTEX_SHADER;
    }
}
