/* The OpenGL ES 1.1 entry points gltest uses, as an X-macro list:
 *   F(return type, name, (parameters), (argument names)), F0(type, name) for no arguments
 * On iOS they are OpenGLES.framework's exports; the host build (host_gl.c)
 * routes them straight into the engine's dispatch table.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef GLAPI_H
#define GLAPI_H
#include "../gl_defs.h"

#define GLTEST_API(F, F0) \
    F(void, glActiveTexture, (GLenum t), (t)) \
    F(void, glAlphaFunc, (GLenum f, GLclampf r), (f, r)) \
    F(void, glBindFramebufferOES, (GLenum t, GLuint n), (t, n)) \
    F(void, glBindRenderbufferOES, (GLenum t, GLuint n), (t, n)) \
    F(void, glBindTexture, (GLenum t, GLuint n), (t, n)) \
    F(void, glBlendFunc, (GLenum s, GLenum d), (s, d)) \
    F(GLenum, glCheckFramebufferStatusOES, (GLenum t), (t)) \
    F(void, glClear, (GLbitfield m), (m)) \
    F(void, glClientActiveTexture, (GLenum t), (t)) \
    F(void, glColorMask, (GLboolean r, GLboolean g, GLboolean b, GLboolean a), (r, g, b, a)) \
    F(void, glDepthMask, (GLboolean f), (f)) \
    F(void, glFramebufferTexture2DOES, (GLenum t, GLenum a, GLenum tt, GLuint x, GLint l), (t, a, tt, x, l)) \
    F(void, glTexEnvfv, (GLenum t, GLenum p, const GLfloat *v), (t, p, v)) \
    F(void, glClearColor, (GLclampf r, GLclampf g, GLclampf b, GLclampf a), (r, g, b, a)) \
    F(void, glColor4f, (GLfloat r, GLfloat g, GLfloat b, GLfloat a), (r, g, b, a)) \
    F(void, glColorPointer, (GLint s, GLenum t, GLsizei st, const GLvoid *p), (s, t, st, p)) \
    F(void, glCompressedTexImage2D, (GLenum t, GLint l, GLenum i, GLsizei w, GLsizei h, GLint b, GLsizei sz, const GLvoid *d), (t, l, i, w, h, b, sz, d)) \
    F(GLuint, glCreateShader, (GLenum t), (t)) \
    F(void, glCurrentPaletteMatrixOES, (GLuint m), (m)) \
    F(void, glCullFace, (GLenum m), (m)) \
    F(void, glDepthFunc, (GLenum f), (f)) \
    F(void, glDisable, (GLenum c), (c)) \
    F(void, glDisableClientState, (GLenum a), (a)) \
    F(void, glDrawArrays, (GLenum m, GLint f, GLsizei n), (m, f, n)) \
    F(void, glDrawElements, (GLenum m, GLsizei n, GLenum t, const GLvoid *i), (m, n, t, i)) \
    F(void, glDrawTexiOES, (GLint x, GLint y, GLint z, GLint w, GLint h), (x, y, z, w, h)) \
    F(void, glEnable, (GLenum c), (c)) \
    F(void, glEnableClientState, (GLenum a), (a)) \
    F0(void, glFinish) \
    F0(void, glFlush) \
    F(void, glFramebufferRenderbufferOES, (GLenum t, GLenum a, GLenum rt, GLuint r), (t, a, rt, r)) \
    F(void, glFrustumf, (GLfloat l, GLfloat r, GLfloat b, GLfloat t, GLfloat n, GLfloat f), (l, r, b, t, n, f)) \
    F(void, glGenFramebuffersOES, (GLsizei n, GLuint *o), (n, o)) \
    F(void, glGenRenderbuffersOES, (GLsizei n, GLuint *o), (n, o)) \
    F(void, glGenTextures, (GLsizei n, GLuint *o), (n, o)) \
    F(void, glGenerateMipmapOES, (GLenum t), (t)) \
    F(void, glDeleteTextures, (GLsizei n, const GLuint *o), (n, o)) \
    F0(GLenum, glGetError) \
    F(void, glGetIntegerv, (GLenum p, GLint *o), (p, o)) \
    F(void, glGetRenderbufferParameterivOES, (GLenum t, GLenum p, GLint *o), (t, p, o)) \
    F(const GLubyte *, glGetString, (GLenum n), (n)) \
    F(void, glLightfv, (GLenum l, GLenum p, const GLfloat *v), (l, p, v)) \
    F0(void, glLoadIdentity) \
    F0(void, glLoadPaletteFromModelViewMatrixOES) \
    F(void, glMaterialfv, (GLenum f, GLenum p, const GLfloat *v), (f, p, v)) \
    F(void, glMatrixIndexPointerOES, (GLint s, GLenum t, GLsizei st, const GLvoid *p), (s, t, st, p)) \
    F(void, glMatrixMode, (GLenum m), (m)) \
    F(void, glNormalPointer, (GLenum t, GLsizei st, const GLvoid *p), (t, st, p)) \
    F(void, glOrthof, (GLfloat l, GLfloat r, GLfloat b, GLfloat t, GLfloat n, GLfloat f), (l, r, b, t, n, f)) \
    F0(void, glPopMatrix) \
    F0(void, glPushMatrix) \
    F(void, glReadPixels, (GLint x, GLint y, GLsizei w, GLsizei h, GLenum f, GLenum t, GLvoid *p), (x, y, w, h, f, t, p)) \
    F(void, glRenderbufferStorageOES, (GLenum t, GLenum f, GLsizei w, GLsizei h), (t, f, w, h)) \
    F(void, glRotatef, (GLfloat a, GLfloat x, GLfloat y, GLfloat z), (a, x, y, z)) \
    F(void, glScalef, (GLfloat x, GLfloat y, GLfloat z), (x, y, z)) \
    F(void, glScissor, (GLint x, GLint y, GLsizei w, GLsizei h), (x, y, w, h)) \
    F(void, glShadeModel, (GLenum m), (m)) \
    F(void, glTexCoordPointer, (GLint s, GLenum t, GLsizei st, const GLvoid *p), (s, t, st, p)) \
    F(void, glTexEnvi, (GLenum t, GLenum p, GLint v), (t, p, v)) \
    F(void, glTexImage2D, (GLenum t, GLint l, GLint i, GLsizei w, GLsizei h, GLint b, GLenum f, GLenum ty, const GLvoid *p), (t, l, i, w, h, b, f, ty, p)) \
    F(void, glTexParameteri, (GLenum t, GLenum p, GLint v), (t, p, v)) \
    F(void, glTexParameteriv, (GLenum t, GLenum p, const GLint *v), (t, p, v)) \
    F(void, glTranslatef, (GLfloat x, GLfloat y, GLfloat z), (x, y, z)) \
    F(void, glVertexPointer, (GLint s, GLenum t, GLsizei st, const GLvoid *p), (s, t, st, p)) \
    F(void, glWeightPointerOES, (GLint s, GLenum t, GLsizei st, const GLvoid *p), (s, t, st, p)) \
    F(void, glViewport, (GLint x, GLint y, GLsizei w, GLsizei h), (x, y, w, h))

#define GLTEST_DECL(ret, name, params, args) ret name params;
#define GLTEST_DECL0(ret, name) ret name(void);
GLTEST_API(GLTEST_DECL, GLTEST_DECL0)
#endif
