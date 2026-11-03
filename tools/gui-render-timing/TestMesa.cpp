#include "guilib/GUIRenderTiming.cpp"

#include <cassert>
#include <iostream>
int main()
{
  EGLDisplay d = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  assert(d != EGL_NO_DISPLAY && eglInitialize(d, nullptr, nullptr));
  assert(eglBindAPI(EGL_OPENGL_ES_API));
  EGLint attrs[] = {EGL_SURFACE_TYPE,
                    EGL_PBUFFER_BIT,
                    EGL_RENDERABLE_TYPE,
                    EGL_OPENGL_ES2_BIT,
                    EGL_RED_SIZE,
                    8,
                    EGL_GREEN_SIZE,
                    8,
                    EGL_BLUE_SIZE,
                    8,
                    EGL_NONE};
  EGLConfig cfg;
  EGLint n = 0;
  assert(eglChooseConfig(d, attrs, &cfg, 1, &n) && n);
  EGLint ca[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
  EGLContext c = eglCreateContext(d, cfg, EGL_NO_CONTEXT, ca);
  assert(c != EGL_NO_CONTEXT);
  EGLint pa[] = {EGL_WIDTH, 128, EGL_HEIGHT, 128, EGL_NONE};
  EGLSurface p = eglCreatePbufferSurface(d, cfg, pa);
  assert(p != EGL_NO_SURFACE && eglMakeCurrent(d, p, p, c));
  using namespace GUI_RENDER_TIMING;
  for (unsigned i = 0; i < 4; i++)
  {
    {
      Frame f(true, 60);
      Scope gui(Stage::Gui);
      AttachGPU();
      glClearColor(0.2, 0.4, 0.6, 1);
      glClear(GL_COLOR_BUFFER_BIT);
      {
        Scope fade(Stage::Fade);
        FadeLayers(nullptr, 2);
        glClearColor(0.1, 0.2, 0.3, 1);
        glClear(GL_COLOR_BUFFER_BIT);
      }
      assert(glGetError() == GL_NO_ERROR);
    }
    glFinish(); // Test driver completion only: production never forces a finish.
  }
  assert(s.frames >= 3 && s.worstGPU > 0 && s.worstGPUId != 0);
  ReleaseContext(c);
  assert(glGetError() == GL_NO_ERROR);
  eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  eglDestroySurface(d, p);
  eglDestroyContext(d, c);
  eglTerminate(d);
  std::cout << "PASS Mesa real elapsed-query scopes, deferred resolution and context retirement\n";
}
