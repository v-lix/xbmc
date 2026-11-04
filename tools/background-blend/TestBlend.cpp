// SPDX-License-Identifier: GPL-2.0-or-later
// Software GLES correctness checks; readback is confined to this test.
#include "system_gl.h"
static bool rejectFramebuffer = false;
static unsigned int framebufferChecks = 0;
static GLenum CheckFramebuffer(GLenum target)
{
  ++framebufferChecks;
  return rejectFramebuffer ? GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT
                           : glCheckFramebufferStatus(target);
}
#define glCheckFramebufferStatus CheckFramebuffer
#include "guilib/GUIBackgroundBlend.cpp"
#undef glCheckFramebufferStatus

#include <cassert>
#include <iostream>
#include <stdexcept>
#include <vector>

GLuint sourceProgram;
GLint sourceColor, sourceDepth, sourceGradient;

void Quad(float red,
          float green,
          float blue,
          float alpha,
          float depth,
          bool gradient = false,
          bool half = false)
{
  glUseProgram(sourceProgram);
  glUniform4f(sourceColor, red, green, blue, alpha);
  glUniform1f(sourceDepth, depth);
  glUniform1f(sourceGradient, gradient ? 1 : 0);
  const float right = half ? 0 : 1;
  const GLfloat vertices[]{-1, -1, right, -1, -1, 1, right, 1};
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, vertices);
  glEnableVertexAttribArray(0);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  glDisableVertexAttribArray(0);
}

void Sources(bool gradient, float alpha)
{
  glEnable(GL_BLEND);
  glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE_MINUS_DST_ALPHA, GL_ONE);
  Quad(.85, .15, .2, alpha * .6f, .5, gradient);
  Quad(.1, .2, .9, alpha * .4f, .5, gradient);
}

void Scene(unsigned int w, unsigned int h, bool occlusion)
{
  glViewport(0, 0, w, h);
  glDisable(GL_SCISSOR_TEST);
  glEnable(GL_DEPTH_TEST);
  glDepthFunc(GL_LEQUAL);
  glDepthMask(GL_TRUE);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glClearColor(.08, .16, .24, .4);
  glClearDepthf(1);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  if (occlusion)
  {
    glDisable(GL_BLEND);
    Quad(.15, .8, .2, 1, -.5, false, true);
  }
  glEnable(GL_SCISSOR_TEST);
  glScissor(5, 7, w - 11, h - 15);
  glDepthMask(GL_FALSE);
}

std::vector<unsigned char> Read(unsigned int w, unsigned int h)
{
  std::vector<unsigned char> pixels(size_t(w) * h * 4);
  glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
  assert(glGetError() == GL_NO_ERROR);
  return pixels;
}

int main()
{
  assert(!GUI_BACKGROUND_BLEND::Draw(1080, .5, [] {}));
  EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  assert(display != EGL_NO_DISPLAY && eglInitialize(display, nullptr, nullptr));
  assert(eglBindAPI(EGL_OPENGL_ES_API));
  const EGLint configAttributes[]{EGL_SURFACE_TYPE,
                                  EGL_PBUFFER_BIT,
                                  EGL_RENDERABLE_TYPE,
                                  EGL_OPENGL_ES2_BIT,
                                  EGL_RED_SIZE,
                                  8,
                                  EGL_GREEN_SIZE,
                                  8,
                                  EGL_BLUE_SIZE,
                                  8,
                                  EGL_ALPHA_SIZE,
                                  8,
                                  EGL_NONE};
  EGLConfig config{};
  EGLint count = 0;
  assert(eglChooseConfig(display, configAttributes, &config, 1, &count) && count);
  const EGLint contextAttributes[]{EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
  EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, contextAttributes);
  const EGLint surfaceAttributes[]{EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
  EGLSurface surface = eglCreatePbufferSurface(display, config, surfaceAttributes);
  assert(context != EGL_NO_CONTEXT && surface != EGL_NO_SURFACE);
  assert(eglMakeCurrent(display, surface, surface, context));
  const char* vertex = "attribute vec2 position; uniform float depth; varying highp vec2 uv;"
                       "void main(){uv=position*.5+.5;gl_Position=vec4(position,depth,1.);}";
  const char* fragment =
      "precision highp float; uniform vec4 color; uniform float gradient;"
      "varying highp vec2 uv; void main(){gl_FragColor=vec4(color.rgb*(1.-gradient*.4+"
      "gradient*(uv.x*.15+uv.y*.25)),color.a);}";
  GLuint vs = Compile(GL_VERTEX_SHADER, vertex), fs = Compile(GL_FRAGMENT_SHADER, fragment);
  assert(vs && fs);
  sourceProgram = glCreateProgram();
  glAttachShader(sourceProgram, vs);
  glAttachShader(sourceProgram, fs);
  glBindAttribLocation(sourceProgram, 0, "position");
  glLinkProgram(sourceProgram);
  GLint linked = 0;
  glGetProgramiv(sourceProgram, GL_LINK_STATUS, &linked);
  assert(linked);
  glDeleteShader(vs);
  glDeleteShader(fs);
  sourceColor = glGetUniformLocation(sourceProgram, "color");
  sourceDepth = glGetUniformLocation(sourceProgram, "depth");
  sourceGradient = glGetUniformLocation(sourceProgram, "gradient");
  GLuint fbo = 0, texture = 0, depth = 0;
  glGenFramebuffers(1, &fbo);
  glGenTextures(1, &texture);
  glGenRenderbuffers(1, &depth);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  glBindTexture(GL_TEXTURE_2D, texture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glBindRenderbuffer(GL_RENDERBUFFER, depth);
  GLuint cachedProgram = 0;
  for (const auto& size :
       {std::pair<unsigned int, unsigned int>{128, 72}, {129, 73}, {1920, 1080}, {3840, 2160}})
  {
    const auto [w, h] = size;
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, w, h);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth);
    assert(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
    const unsigned int resolution = h / 2;
    for (bool occlusion : {false, true})
      for (bool gradient : {false, true})
        for (float alpha : {.22f, .7f, 1.f})
        {
          Scene(w, h, occlusion);
          Sources(gradient, alpha);
          const auto direct = Read(w, h);
          Scene(w, h, occlusion);
          // Start with a non-default active unit and blending state, and ensure
          // the helper preserves them along with framebuffer/depth/scissoring.
          glActiveTexture(GL_TEXTURE1);
          glEnable(GL_BLEND);
          glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ZERO, GL_ONE);
          assert(GUI_BACKGROUND_BLEND::Draw(resolution, .5, [&] { Sources(gradient, alpha); }));
          GLint value = -1, viewport[4]{}, scissor[4]{};
          GLboolean mask = GL_TRUE;
          glGetIntegerv(GL_FRAMEBUFFER_BINDING, &value);
          assert(value == int(fbo));
          glGetIntegerv(GL_ACTIVE_TEXTURE, &value);
          assert(value == GL_TEXTURE1);
          glGetIntegerv(GL_BLEND_DST_RGB, &value);
          assert(value == GL_ONE);
          glGetIntegerv(GL_BLEND_SRC_ALPHA, &value);
          assert(value == GL_ZERO);
          glGetIntegerv(GL_VIEWPORT, viewport);
          assert(viewport[2] == int(w) && viewport[3] == int(h));
          glGetIntegerv(GL_SCISSOR_BOX, scissor);
          assert(scissor[0] == 5 && scissor[1] == 7);
          glGetBooleanv(GL_DEPTH_WRITEMASK, &mask);
          assert(!mask);
          assert(glIsEnabled(GL_DEPTH_TEST) && glIsEnabled(GL_SCISSOR_TEST));
          const auto reduced = Read(w, h);
          int largest = 0;
          for (size_t n = 0; n < direct.size(); ++n)
            largest = std::max(largest, std::abs(int(direct[n]) - int(reduced[n])));
          if (largest > 3)
            throw std::runtime_error("downscaled blend differs from native composition: " +
                                     std::to_string(largest));
          glActiveTexture(GL_TEXTURE0);
          const auto& cached = resources.at(context);
          if (cachedProgram)
            assert(cached.program == cachedProgram);
          cachedProgram = cached.program;
          assert(cached.width <= 1920 && cached.height <= 1080);
        }
    assert(!GUI_BACKGROUND_BLEND::Draw(0, .5, [] { assert(false); }));
    assert(!GUI_BACKGROUND_BLEND::Draw(h * 2, .5, [] { assert(false); }));
    Scene(w, h, false);
    const auto before = Read(w, h);
    try
    {
      GUI_BACKGROUND_BLEND::Draw(resolution, .5, [] { throw std::runtime_error("test callback"); });
      assert(false);
    }
    catch (const std::runtime_error&)
    {
    }
    assert(Read(w, h) == before);
  }
  const GLuint cachedTexture = resources.at(context).texture;
  GUI_BACKGROUND_BLEND::ReleaseContext(context);
  assert(resources.empty() && !glIsTexture(cachedTexture));
  // An incomplete target must not draw on the destination or retry every frame.
  Scene(3840, 2160, false);
  const auto unchanged = Read(3840, 2160);
  rejectFramebuffer = true;
  assert(!GUI_BACKGROUND_BLEND::Draw(1080, .5, [] { assert(false); }));
  const auto checks = framebufferChecks;
  assert(!GUI_BACKGROUND_BLEND::Draw(1080, .5, [] { assert(false); }));
  assert(framebufferChecks == checks && Read(3840, 2160) == unchanged);
  rejectFramebuffer = false;
  GUI_BACKGROUND_BLEND::ReleaseContext(context);
  assert(GUI_BACKGROUND_BLEND::Draw(1080, .5, [] { Sources(false, .7); }));
  const GLuint orphan = resources.at(context).texture;
  const EGLContext other = eglCreateContext(display, config, EGL_NO_CONTEXT, contextAttributes);
  assert(other != EGL_NO_CONTEXT && eglMakeCurrent(display, surface, surface, other));
  // Deliberately create a colliding texture name in another namespace.
  std::vector<GLuint> otherTextures(orphan);
  glGenTextures(otherTextures.size(), otherTextures.data());
  assert(std::find(otherTextures.begin(), otherTextures.end(), orphan) != otherTextures.end());
  glBindTexture(GL_TEXTURE_2D, orphan);
  assert(glIsTexture(orphan));
  GUI_BACKGROUND_BLEND::ReleaseContext(context);
  assert(resources.empty() && glIsTexture(orphan));
  glDeleteTextures(otherTextures.size(), otherTextures.data());
  assert(eglMakeCurrent(display, surface, surface, context));
  assert(glIsTexture(orphan)); // The original context owns final reclamation.
  eglDestroyContext(display, other);
  assert(!glGetError());
  glDeleteTextures(1, &texture);
  glDeleteRenderbuffers(1, &depth);
  glDeleteFramebuffers(1, &fbo);
  glDeleteProgram(sourceProgram);
  eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  eglDestroySurface(display, surface);
  eglDestroyContext(display, context);
  eglTerminate(display);
  std::cout << "PASS Mesa production background blend: RGBA composition, orientation, scissor, "
               "depth occlusion, state restoration, resource reuse, resize, fallback and "
               "exceptions through 4K\n";
}
