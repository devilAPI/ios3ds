/* Link-time stand-in for 5H11a's OpenGLES.framework (the IMG MBX driver):
 * only the names egl2.c interposes, so the dylib can name the originals.
 * Never installed; at run time dyld binds to the real framework.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#define S(n) void n(void); void n(void) {}
S(eglEnableInternalSurface) S(eglGetDisplay) S(eglGetError) S(eglInitialize) S(eglChooseConfig)
S(eglGetConfigAttrib) S(eglCreateContext) S(eglDestroyContext) S(eglCreatePixmapSurface)
S(eglDestroySurface) S(eglMakeCurrent)
S(glActiveTexture) S(glBindTexture) S(glBlendFunc) S(glClear) S(glClearColor) S(glClientActiveTexture)
S(glColor4f) S(glColor4ub) S(glColorPointer) S(glCullFace) S(glDeleteTextures) S(glDisable)
S(glDisableClientState) S(glDrawArrays) S(glDrawElements) S(glEnable) S(glEnableClientState)
S(glFrontFace) S(glGenTextures) S(glGetFloatv) S(glGetIntegerv) S(glLoadIdentity) S(glLoadMatrixf)
S(glMatrixMode) S(glOrthof) S(glPopMatrix) S(glPushMatrix) S(glRotatef) S(glScalef) S(glScissor)
S(glTexCoordPointer) S(glTexEnvfv) S(glTexEnvi) S(glTexImage2D) S(glTexParameteri) S(glTranslatef)
S(glVertexPointer) S(glViewport) S(glGetError) S(glGetString) S(glFlush) S(glFinish)
S(glTexImageCoreSurfaceAPPLE) S(glFinishTextureAPPLE)
