/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */
#include "GUIBackgroundBlend.h"

#include "utils/log.h"

#include <algorithm>
#include <cmath>
#include <map>

#include "system_gl.h"

namespace
{
// Shared by background controls, bounded to one temporary target per context.
// All access and retirement run on the GUI thread, never an upload worker.
struct Resources
{
  GLuint texture{0}, fbo{0}, program{0};
  GLint depth{-1};
  unsigned int width{0}, height{0};
  bool failed{false};

  void Delete()
  {
    glDeleteTextures(1, &texture);
    glDeleteFramebuffers(1, &fbo);
    if (program)
      glDeleteProgram(program);
    texture = fbo = program = 0;
    width = height = 0;
  }
};
std::map<EGLContext, Resources> resources;

void Enable(GLenum state, GLboolean enabled)
{
  if (enabled)
    glEnable(state);
  else
    glDisable(state);
}

// Preserve the enclosing framebuffer (which can itself be a Dolby Vision GUI
// composite), viewport, clipping and depth. No GPU waits or readback are used.
struct State
{
  GLint fbo{}, viewport[4]{}, scissors[4]{}, program{}, active{}, texture{};
  GLint array{}, elements{}, blend[4]{};
  GLboolean scissor{}, depth{}, stencil{}, cull{}, blending{}, depthMask{}, colorMask[4]{};
  GLfloat clear[4]{};
  GLint attribute[6]{};
  void* pointer{};

  State()
  {
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);
    glGetIntegerv(GL_VIEWPORT, viewport);
    glGetIntegerv(GL_SCISSOR_BOX, scissors);
    glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &array);
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &elements);
    glGetIntegerv(GL_BLEND_SRC_RGB, &blend[0]);
    glGetIntegerv(GL_BLEND_DST_RGB, &blend[1]);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &blend[2]);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &blend[3]);
    scissor = glIsEnabled(GL_SCISSOR_TEST);
    depth = glIsEnabled(GL_DEPTH_TEST);
    stencil = glIsEnabled(GL_STENCIL_TEST);
    cull = glIsEnabled(GL_CULL_FACE);
    blending = glIsEnabled(GL_BLEND);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMask);
    glGetBooleanv(GL_COLOR_WRITEMASK, colorMask);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, clear);
    glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &attribute[0]);
    glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_SIZE, &attribute[1]);
    glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_TYPE, &attribute[2]);
    glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &attribute[3]);
    glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &attribute[4]);
    glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &attribute[5]);
    glGetVertexAttribPointerv(0, GL_VERTEX_ATTRIB_ARRAY_POINTER, &pointer);
  }

  void Destination() const
  {
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    glScissor(scissors[0], scissors[1], scissors[2], scissors[3]);
    Enable(GL_SCISSOR_TEST, scissor);
    Enable(GL_DEPTH_TEST, depth);
    Enable(GL_STENCIL_TEST, stencil);
    glColorMask(colorMask[0], colorMask[1], colorMask[2], colorMask[3]);
  }

  ~State()
  {
    Destination();
    Enable(GL_CULL_FACE, cull);
    Enable(GL_BLEND, blending);
    glDepthMask(depthMask);
    glClearColor(clear[0], clear[1], clear[2], clear[3]);
    glBlendFuncSeparate(blend[0], blend[1], blend[2], blend[3]);
    glUseProgram(program);
    glBindBuffer(GL_ARRAY_BUFFER, attribute[5]);
    glVertexAttribPointer(0, attribute[1], attribute[2], attribute[3], attribute[4], pointer);
    if (attribute[0])
      glEnableVertexAttribArray(0);
    else
      glDisableVertexAttribArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, array);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, elements);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);
    glActiveTexture(active);
  }
};

GLuint Compile(GLenum type, const char* source)
{
  const GLuint shader = glCreateShader(type);
  glShaderSource(shader, 1, &source, nullptr);
  glCompileShader(shader);
  GLint ok = 0;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
  if (!ok)
  {
    glDeleteShader(shader);
    return 0;
  }
  return shader;
}

bool Ensure(Resources& r, unsigned int width, unsigned int height)
{
  if (r.failed)
    return false;
  if (r.texture && r.width == width && r.height == height)
    return true;
  if (!r.program)
  {
    const char* vertex = "attribute vec2 position; uniform float depth; varying highp vec2 uv;"
                         "void main(){uv=position*.5+.5;gl_Position=vec4(position,depth,1.);}";
    const char* fragment = "precision highp float; varying highp vec2 uv; uniform sampler2D image;"
                           "void main(){gl_FragColor=texture2D(image,uv);}";
    const GLuint vs = Compile(GL_VERTEX_SHADER, vertex);
    const GLuint fs = Compile(GL_FRAGMENT_SHADER, fragment);
    r.program = glCreateProgram();
    if (vs)
      glAttachShader(r.program, vs);
    if (fs)
      glAttachShader(r.program, fs);
    glBindAttribLocation(r.program, 0, "position");
    glLinkProgram(r.program);
    GLint linked = 0;
    glGetProgramiv(r.program, GL_LINK_STATUS, &linked);
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (!linked)
      return false;
    r.depth = glGetUniformLocation(r.program, "depth");
    glUseProgram(r.program);
    glUniform1i(glGetUniformLocation(r.program, "image"), 0);
    glGenFramebuffers(1, &r.fbo);
    glGenTextures(1, &r.texture);
  }
  glBindTexture(GL_TEXTURE_2D, r.texture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  glBindFramebuffer(GL_FRAMEBUFFER, r.fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, r.texture, 0);
  if (!r.texture || !r.fbo || glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE ||
      glGetError() != GL_NO_ERROR)
    return false;
  r.width = width;
  r.height = height;
  CLog::Log(LOGINFO, "GUI background blend: active; temporary target {}x{}", width, height);
  return true;
}
} // namespace

bool GUI_BACKGROUND_BLEND::Draw(unsigned int maxHeight,
                                float depth,
                                const std::function<void()>& draw)
{
  const EGLContext context = eglGetCurrentContext();
  if (!maxHeight || context == EGL_NO_CONTEXT)
    return false;
  const auto cached = resources.find(context);
  if (cached != resources.end() && cached->second.failed)
    return false;
  // Check before saving the rest of the state: native-sized output uses the
  // ordinary path and allocates no temporary target.
  GLint viewport[4];
  glGetIntegerv(GL_VIEWPORT, viewport);
  if (viewport[2] <= 0 || viewport[3] <= 0)
    return false;
  const double scale =
      std::min({1.0, double(maxHeight) / viewport[3], (double(maxHeight) * 16 / 9) / viewport[2]});
  if (scale >= 1.0)
    return false;
  const unsigned int width =
      std::max(1u, static_cast<unsigned int>(std::ceil(viewport[2] * scale)));
  const unsigned int height =
      std::max(1u, static_cast<unsigned int>(std::ceil(viewport[3] * scale)));
  State state;
  auto& r = resources[context];
  if (!Ensure(r, width, height))
  {
    if (!r.failed)
      CLog::Log(LOGWARNING, "GUI background blend: unavailable; using direct rendering");
    r.failed = true;
    r.Delete();
    return false;
  }

  glBindFramebuffer(GL_FRAMEBUFFER, r.fbo);
  glViewport(0, 0, width, height);
  // Populate the entire temporary target. Apply the destination damage scissor
  // only to the upsample, so filtering at a damage edge never samples a hole.
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_STENCIL_TEST);
  glDepthMask(GL_FALSE);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glClearColor(0, 0, 0, 0);
  glClear(GL_COLOR_BUFFER_BIT);
  draw();

  state.Destination();
  glDisable(GL_CULL_FACE);
  glDepthMask(GL_FALSE);
  glEnable(GL_BLEND);
  // Normal GUI drawing into transparent storage produces premultiplied RGB.
  // Applying SRC_ALPHA again here would darken translucent fanart and fades.
  glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
  glUseProgram(r.program);
  glUniform1f(r.depth, depth);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, r.texture);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  const GLfloat quad[]{-1, -1, 1, -1, -1, 1, 1, 1};
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
  glEnableVertexAttribArray(0);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  glDisableVertexAttribArray(0);
  return true;
}

void GUI_BACKGROUND_BLEND::ReleaseContext(EGLContext context)
{
  const auto it = resources.find(context);
  if (it == resources.end())
    return;
  if (context != EGL_NO_CONTEXT && context == eglGetCurrentContext())
    it->second.Delete();
  // Without the exact context, EGL owns final reclamation. Never delete names
  // from a different or replacement namespace.
  resources.erase(it);
}
