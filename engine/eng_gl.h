/* eng_gl.h -- <GL/gl.h> plus the few post-1.1 constants the engine uses.
 * On PS Vita (__vita__), routes textures directly to the GXM texture manager. */
#ifndef ENG_GL_H
#define ENG_GL_H

#ifdef __vita__
#include <stdint.h>
#include <stdbool.h>
#include "gxm_tex.h"

typedef uint32_t GLuint;
typedef int32_t  GLint;
typedef float    GLfloat;
typedef uint32_t GLenum;
typedef int32_t  GLsizei;
typedef uint8_t  GLubyte;

#define GL_TEXTURE_2D    0x0DE1
#define GL_RGBA          0x1908
#define GL_UNSIGNED_BYTE 0x1401
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_NEAREST       0x2600
#define GL_LINEAR        0x2601

#define glGenTextures(n, p) (*(p) = gxm_tex_alloc())
#define glBindTexture(target, tex) gxm_tex_bind(tex)
#define glTexParameteri(target, pname, param) ((void)0)
#define glTexImage2D(target, level, ifmt, w, h, border, fmt, type, pixels) gxm_tex_realloc(gxm_tex_current, w, h)
#define glTexSubImage2D(target, level, x, y, w, h, fmt, type, pixels) gxm_tex_upload(gxm_tex_current, x, y, w, h, pixels)
#define glDeleteTextures(n, p) gxm_tex_free(*(p))

#else /* !__vita__ */

#ifdef _WIN32
#include <windows.h>
#endif
#include <GL/gl.h>
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
#ifndef GL_BGRA
#define GL_BGRA 0x80E1
#endif
#ifndef GL_COMBINE
#define GL_COMBINE     0x8570
#define GL_COMBINE_RGB 0x8571
#define GL_RGB_SCALE   0x8573
#endif

#endif /* __vita__ */

#endif /* ENG_GL_H */
