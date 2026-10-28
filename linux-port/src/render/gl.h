// Minimal OpenGL 3.3 core loader: only the entry points dcp_viewer / dcp_run's renderer use, resolved via SDL_GL_GetProcAddress.
#pragma once
#include <cstddef>
#include <cstdint>

#define DCP_GLAPI  // GL entry points use the native (SysV) calling convention on Linux

using GLenum = unsigned int;
using GLuint = unsigned int;
using GLint = int;
using GLsizei = int;
using GLfloat = float;
using GLboolean = unsigned char;
using GLchar = char;
using GLbitfield = unsigned int;
using GLubyte = unsigned char;
using GLsizeiptr = std::ptrdiff_t;
using GLintptr = std::ptrdiff_t;

enum : GLenum {
    GL_FALSE_ = 0, GL_TRIANGLES = 0x0004, GL_SRC_ALPHA = 0x0302, GL_ONE_MINUS_SRC_ALPHA = 0x0303, GL_DST_COLOR = 0x0306,
    GL_ZERO = 0, GL_ONE = 1, GL_BLEND = 0x0BE2, GL_TEXTURE_2D = 0x0DE1, GL_UNSIGNED_BYTE = 0x1401, GL_FLOAT = 0x1406,
    GL_RGBA = 0x1908, GL_RGBA8 = 0x8058, GL_NEAREST = 0x2600, GL_LINEAR = 0x2601, GL_TEXTURE_MAG_FILTER = 0x2800,
    GL_TEXTURE_MIN_FILTER = 0x2801, GL_TEXTURE_WRAP_S = 0x2802, GL_TEXTURE_WRAP_T = 0x2803, GL_CLAMP_TO_EDGE = 0x812F,
    GL_COLOR_BUFFER_BIT = 0x4000, GL_TEXTURE0 = 0x84C0, GL_TEXTURE1 = 0x84C1, GL_ARRAY_BUFFER = 0x8892, GL_STREAM_DRAW = 0x88E0,
    GL_FRAGMENT_SHADER = 0x8B30, GL_VERTEX_SHADER = 0x8B31, GL_COMPILE_STATUS = 0x8B81, GL_LINK_STATUS = 0x8B82,
    GL_FRAMEBUFFER = 0x8D40, GL_COLOR_ATTACHMENT0 = 0x8CE0, GL_FRAMEBUFFER_COMPLETE = 0x8CD5, GL_UNPACK_ALIGNMENT = 0x0CF5,
    GL_PACK_ALIGNMENT = 0x0D05, GL_FUNC_ADD = 0x8006, GL_FUNC_REVERSE_SUBTRACT = 0x800B, GL_DST_ALPHA = 0x0304, GL_ONE_MINUS_DST_ALPHA = 0x0305, GL_VERSION = 0x1F02,
    GL_RENDERER = 0x1F01, GL_COMPRESSED_RGBA_BPTC_UNORM = 0x8E8C, GL_COMPRESSED_RGBA_S3TC_DXT1_EXT = 0x83F1,
    GL_COMPRESSED_RGBA_S3TC_DXT3_EXT = 0x83F2, GL_COMPRESSED_RGBA_S3TC_DXT5_EXT = 0x83F3,
    GL_SCISSOR_TEST = 0x0C11, GL_ELEMENT_ARRAY_BUFFER = 0x8893, GL_UNSIGNED_SHORT = 0x1403, GL_UNSIGNED_INT = 0x1405,
    GL_POINTS = 0, GL_LINES = 1, GL_LINE_STRIP = 3, GL_TRIANGLE_STRIP = 5, GL_TRIANGLE_FAN = 6, GL_FUNC_SUBTRACT = 0x800A,
    GL_MIN = 0x8007, GL_MAX = 0x8008, GL_SRC_COLOR = 0x0300, GL_ONE_MINUS_SRC_COLOR = 0x0301, GL_ONE_MINUS_DST_COLOR = 0x0307,
    GL_SRC_ALPHA_SATURATE = 0x0308, GL_READ_FRAMEBUFFER = 0x8CA8, GL_DRAW_FRAMEBUFFER = 0x8CA9, GL_REPEAT = 0x2901,
    GL_MIRRORED_REPEAT = 0x8370, GL_BYTE = 0x1400, GL_SHORT = 0x1402, GL_INT = 0x1404, GL_UNSIGNED_INT_ = 0x1405,
    GL_HALF_FLOAT = 0x140B, GL_SAMPLE_BUFFERS = 0x80A8, GL_SAMPLES = 0x80A9,
};

#define DCP_GL_FUNCS(X)                                                                                   \
    X(void, glViewport, (GLint, GLint, GLsizei, GLsizei))                                                 \
    X(void, glClearColor, (GLfloat, GLfloat, GLfloat, GLfloat))                                           \
    X(void, glClear, (GLbitfield))                                                                        \
    X(void, glEnable, (GLenum))                                                                           \
    X(void, glDisable, (GLenum))                                                                          \
    X(void, glBlendFunc, (GLenum, GLenum))                                                                \
    X(void, glBlendEquation, (GLenum))                                                                    \
    X(void, glGenTextures, (GLsizei, GLuint*))                                                            \
    X(void, glBindTexture, (GLenum, GLuint))                                                              \
    X(void, glGetTexImage, (GLenum, GLint, GLenum, GLenum, void*))                                        \
    X(void, glTexParameteri, (GLenum, GLenum, GLint))                                                     \
    X(void, glTexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*))   \
    X(void, glCompressedTexImage2D, (GLenum, GLint, GLenum, GLsizei, GLsizei, GLint, GLsizei, const void*)) \
    X(void, glPixelStorei, (GLenum, GLint))                                                               \
    X(void, glActiveTexture, (GLenum))                                                                    \
    X(GLuint, glCreateShader, (GLenum))                                                                   \
    X(void, glShaderSource, (GLuint, GLsizei, const GLchar* const*, const GLint*))                        \
    X(void, glCompileShader, (GLuint))                                                                    \
    X(void, glGetShaderiv, (GLuint, GLenum, GLint*))                                                      \
    X(void, glGetShaderInfoLog, (GLuint, GLsizei, GLsizei*, GLchar*))                                     \
    X(GLuint, glCreateProgram, ())                                                                        \
    X(void, glAttachShader, (GLuint, GLuint))                                                             \
    X(void, glBindAttribLocation, (GLuint, GLuint, const GLchar*))                                        \
    X(void, glLinkProgram, (GLuint))                                                                      \
    X(void, glGetProgramiv, (GLuint, GLenum, GLint*))                                                     \
    X(void, glGetProgramInfoLog, (GLuint, GLsizei, GLsizei*, GLchar*))                                    \
    X(void, glUseProgram, (GLuint))                                                                       \
    X(GLint, glGetUniformLocation, (GLuint, const GLchar*))                                               \
    X(void, glUniformMatrix4fv, (GLint, GLsizei, GLboolean, const GLfloat*))                              \
    X(void, glUniform1i, (GLint, GLint))                                                                  \
    X(void, glUniform1f, (GLint, GLfloat))                                                                \
    X(void, glGenVertexArrays, (GLsizei, GLuint*))                                                        \
    X(void, glBindVertexArray, (GLuint))                                                                  \
    X(void, glGenBuffers, (GLsizei, GLuint*))                                                             \
    X(void, glBindBuffer, (GLenum, GLuint))                                                               \
    X(void, glBufferData, (GLenum, GLsizeiptr, const void*, GLenum))                                      \
    X(void, glVertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void*))              \
    X(void, glEnableVertexAttribArray, (GLuint))                                                          \
    X(void, glDrawArrays, (GLenum, GLint, GLsizei))                                                       \
    X(void, glGenFramebuffers, (GLsizei, GLuint*))                                                        \
    X(void, glBindFramebuffer, (GLenum, GLuint))                                                          \
    X(void, glFramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint))                              \
    X(GLenum, glCheckFramebufferStatus, (GLenum))                                                         \
    X(void, glReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*))                        \
    X(GLenum, glGetError, ())                                                                             \
    X(const GLubyte*, glGetString, (GLenum))                                                               \
    X(void, glBlendFuncSeparate, (GLenum, GLenum, GLenum, GLenum))                                         \
    X(void, glBlendEquationSeparate, (GLenum, GLenum))                                                     \
    X(void, glScissor, (GLint, GLint, GLsizei, GLsizei))                                                   \
    X(void, glDrawElements, (GLenum, GLsizei, GLenum, const void*))                                        \
    X(void, glUniform4fv, (GLint, GLsizei, const GLfloat*))                                                \
    X(void, glUniform1iv, (GLint, GLsizei, const GLint*))                                                  \
    X(void, glBlitFramebuffer, (GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum)) \
    X(void, glDeleteTextures, (GLsizei, const GLuint*))                                                    \
    X(void, glDisableVertexAttribArray, (GLuint))                                                          \
    X(void, glVertexAttrib4f, (GLuint, GLfloat, GLfloat, GLfloat, GLfloat))                                \
    X(void, glColorMask, (GLboolean, GLboolean, GLboolean, GLboolean))                                     \
    X(void, glGetIntegerv, (GLenum, GLint*))

namespace dcp::gl {
#define DCP_GL_DECL(ret, name, args) extern ret(DCP_GLAPI* name) args;
DCP_GL_FUNCS(DCP_GL_DECL)
#undef DCP_GL_DECL

// Resolves every entry point; returns the name of the first missing one, or nullptr on success.
const char* load(void* (*get_proc)(const char*));
}  // namespace dcp::gl
