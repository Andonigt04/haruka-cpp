#pragma once
#include <glad/glad.h>
#include <vector>

#pragma once

/** @brief OpenGL vertex buffer RAII wrapper. */
class VertexBuffer {
public:
    /** @brief GL buffer id. */
    unsigned int ID;
    /** @brief Uploads vertex data into a GL array buffer. */
    VertexBuffer(const void* data, unsigned int size) {
        glGenBuffers(1, &ID);
        glBindBuffer(GL_ARRAY_BUFFER, ID);
        glBufferData(GL_ARRAY_BUFFER, size, data, GL_STATIC_DRAW);
    }
    /** @brief Deletes the GL buffer. */
    ~VertexBuffer() { glDeleteBuffers(1, &ID); }
    
    /** @brief Binds the buffer to GL_ARRAY_BUFFER. */
    void bind() const { glBindBuffer(GL_ARRAY_BUFFER, ID); }
};

/** @brief OpenGL vertex array object RAII wrapper. */
class VertexArray {
public:
    /** @brief GL VAO id. */
    unsigned int ID;
    /** @brief Creates a VAO. */
    VertexArray() { glGenVertexArrays(1, &ID); }
    /** @brief Deletes the VAO. */
    ~VertexArray() { glDeleteVertexArrays(1, &ID); }

    /** @brief Configures a vertex attribute layout from a bound VBO. */
    void add_buffer(const VertexBuffer& vbo, unsigned int index, int size, int stride, const void* pointer) {
        bind();
        vbo.bind();
        glVertexAttribPointer(index, size, GL_FLOAT, GL_FALSE, stride, pointer);
        glEnableVertexAttribArray(index);
    }

    /** @brief Binds the VAO. */
    void bind() const { glBindVertexArray(ID); }
    /** @brief Unbinds the VAO. */
    void unbind() const { glBindVertexArray(0); }
};

/** @brief OpenGL element/index buffer RAII wrapper. */
class IndexBuffer {
public:
    /** @brief GL buffer id. */
    unsigned int ID;
    /** @brief Uploads index data into a GL element buffer. */
    IndexBuffer(const unsigned int* indices, unsigned int count) {
        glGenBuffers(1, &ID);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ID);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, count * sizeof(unsigned int), indices, GL_STATIC_DRAW);
    }
    /** @brief Deletes the GL buffer. */
    ~IndexBuffer() { glDeleteBuffers(1, &ID); }
    /** @brief Binds the buffer to GL_ELEMENT_ARRAY_BUFFER. */
    void bind() const { glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ID); }
};