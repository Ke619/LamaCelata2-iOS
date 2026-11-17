// The GL imports the Metal backend implements (platform/src/metal/shims.mm):
// with SCIMITAR_RENDERER=metal, each of these shims in platform/src/opengl.cpp
// forwards to its Metal counterpart, scimitar::metal::m_<name>.
#pragma once
#include "scimitar/runtime.h"

#define SCIMITAR_METAL_GL(X) \
    X(glActiveTexture) X(glBegin) X(glBeginQuery) X(glBindBuffer) \
    X(glBindBufferARB) X(glBindFramebufferEXT) X(glBindProgramARB) X(glBindRenderbufferEXT) \
    X(glBindTexture) X(glBlendEquation) X(glBlendFunc) X(glClear) \
    X(glClearStencil) X(glCullFace) X(glDepthFunc) X(glDisable) \
    X(glDisableVertexAttribArray) X(glDrawArrays) X(glEnable) X(glEnableVertexAttribArray) \
    X(glEnd) X(glEndQuery) X(glFlush) X(glFramebufferRenderbufferEXT) \
    X(glFramebufferTexture2DEXT) X(glFrontFace) X(glLoadIdentity) X(glMatrixMode) \
    X(glPixelStorei) X(glPolygonMode) X(glPopAttrib) X(glPopMatrix) \
    X(glPushAttrib) X(glPushMatrix) X(glReadBuffer) X(glRenderbufferStorageEXT) \
    X(glRenderbufferStorageMultisampleEXT) X(glStencilFunc) X(glStencilMask) X(glStencilOp) \
    X(glTexParameteri) X(glViewport) X(glDeleteBuffersARB) X(glDeleteFramebuffersEXT) \
    X(glDeleteProgramsARB) X(glDeleteQueries) X(glDeleteRenderbuffersEXT) X(glDeleteTextures) \
    X(glGenBuffersARB) X(glGenFramebuffersEXT) X(glGenProgramsARB) X(glGenQueries) \
    X(glGenRenderbuffersEXT) X(glGenTextures) X(glAlphaFunc) X(glBlendColor) \
    X(glClearColor) X(glClearDepth) X(glDepthRange) X(glColorMask) \
    X(glDepthMask) X(glPolygonOffset) X(glRotatef) X(glTranslatef) \
    X(glVertex3f) X(glTexCoord2f) X(glTexEnvf) X(glTexParameterfv) \
    X(glGetError) X(glGetBooleanv) X(glGetFloatv) X(glGetIntegerv) \
    X(glGetQueryObjectiv) X(glCheckFramebufferStatusEXT) X(glBlitFramebufferEXT) X(glBufferDataARB) \
    X(glBufferSubData) X(glMapBufferARB) X(glUnmapBufferARB) X(glVertexAttribPointer) \
    X(glDrawElements) X(glProgramStringARB) X(glProgramEnvParameters4fvEXT) X(glProgramLocalParameters4fvEXT) \
    X(glTexImage1D) X(glTexImage2D) X(glTexImage3D) X(glTexSubImage1D) \
    X(glTexSubImage2D) X(glTexSubImage3D) X(glCompressedTexImage1DARB) X(glCompressedTexImage2DARB) \
    X(glCompressedTexImage3DARB) X(glCompressedTexSubImage1DARB) X(glCompressedTexSubImage2DARB) X(glCompressedTexSubImage3DARB) \
    X(glReadPixels) X(gluPerspective) X(glBindVertexArrayAPPLE) X(glGenVertexArraysAPPLE) \
    X(glDeleteVertexArraysAPPLE) X(glDrawBuffers)

namespace scimitar::metal {
bool requested();
#define SCIMITAR_METAL_DECLARE(N) void m_##N(Context& c);
SCIMITAR_METAL_GL(SCIMITAR_METAL_DECLARE)
#undef SCIMITAR_METAL_DECLARE
}  // namespace scimitar::metal
