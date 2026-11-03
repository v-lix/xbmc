/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <cstdint>

namespace GUI_RENDER_TIMING
{
enum class Stage
{
  Idle,
  Process,
  FrameMove,
  Input,
  Controls,
  // Parts of control processing, attributed exclusively while nested.
  // Items, Info and Load run hundreds of times a frame and are only counted:
  // a 32-bit process on this kernel reads clocks by system call.
  Items, // container item layouts
  Info, // visibility conditions and info updates
  Text, // text layout of changed labels
  Load, // texture allocation
  Release, // texture release
  Layouts, // container item layouts freed outside the kept range
  WindowMove,
  External,
  Setup,
  Video,
  Gui,
  Fade,
  AfterRender,
  Composite,
  EndRender,
  Present,
  Count
};
// How a background fade was drawn in the back-to-front pass.
enum class CrossfadePath
{
  Drawn, // single-pass shader
  Clipped, // single-pass path, nothing visible to draw
  Stack, // more than one outgoing image
  NotReady, // an image of the pair is not ready
  Ineligible, // the pair cannot share one quad, or the shader is unavailable
  Disabled, // singlepasscrossfade is off
  Count
};
// Background image work on job workers.
enum class Job
{
  Decode,
  Upload,
  Count
};
constexpr bool Counted(Stage stage)
{
  return stage == Stage::Items || stage == Stage::Info || stage == Stage::Load;
}
#if defined(TARGET_LINUX) && defined(HAS_LIBAMCODEC) && defined(HAS_GLES) && HAS_GLES >= 2
class Frame
{
public:
  Frame(bool enabled, double fps);
  ~Frame();
  Frame(const Frame&) = delete;
  Frame& operator=(const Frame&) = delete;

private:
  bool m_owner{false};
};
bool Active();
void AttachGPU();
Stage SetStage(Stage stage);
// Called once per drawing pass; distinct controls are counted per frame.
void FadeLayers(const void* control, unsigned int layers);
// A texture submitted a draw. Inside FadeDraws, distinct textures are counted:
// two or more, or a single-pass pair, is a blend the frame actually drew.
void TextureDrawn(const void* texture);
class FadeDraws
{
public:
  explicit FadeDraws(bool enabled);
  ~FadeDraws();
  FadeDraws(const FadeDraws&) = delete;
  FadeDraws& operator=(const FadeDraws&) = delete;

private:
  bool m_enabled;
};
// After presentation: whether a frame was actually swapped.
void Presented(bool swapped);
// An input action was handled.
void Input();
// A container freed the layouts of this many list items.
void LayoutsFreed(unsigned int items);
// A counted part was entered; it is not timed.
void Count(Stage stage);
// A job worker started or finished background image work. Any thread.
uint64_t JobBegin(Job job);
void JobEnd(Job job, uint64_t begin);
class Work
{
public:
  explicit Work(Job job) : m_job(job), m_begin(JobBegin(job)) {}
  ~Work() { JobEnd(m_job, m_begin); }
  Work(const Work&) = delete;
  Work& operator=(const Work&) = delete;

private:
  Job m_job;
  uint64_t m_begin;
};
// An allocated texture was released.
void TextureReleased();
void Crossfade(CrossfadePath path);
// A container scrolled or a control animated in this frame.
void Motion();
// Called before EGL context destruction, even if it cannot be made current.
void ReleaseContext(void* context);
class Scope
{
public:
  explicit Scope(Stage stage, bool enabled = true)
    : m_enabled(enabled && !Counted(stage)), m_previous(m_enabled ? SetStage(stage) : Stage::Idle)
  {
    if (enabled && Counted(stage))
      Count(stage);
  }
  ~Scope()
  {
    if (m_enabled)
      SetStage(m_previous);
  }
  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;

private:
  bool m_enabled;
  Stage m_previous;
};
#else
class Frame
{
public:
  Frame(bool, double) {}
};
inline bool Active()
{
  return false;
}
inline void AttachGPU()
{
}
inline Stage SetStage(Stage)
{
  return Stage::Idle;
}
inline void FadeLayers(const void*, unsigned int)
{
}
inline void TextureDrawn(const void*)
{
}
class FadeDraws
{
public:
  explicit FadeDraws(bool) {}
};
inline void Presented(bool)
{
}
inline void Input()
{
}
inline void LayoutsFreed(unsigned int)
{
}
class Work
{
public:
  explicit Work(Job) {}
};
inline void TextureReleased()
{
}
inline void Crossfade(CrossfadePath)
{
}
inline void Motion()
{
}
inline void ReleaseContext(void*)
{
}
class Scope
{
public:
  explicit Scope(Stage, bool = true) {}
};
#endif
} // namespace GUI_RENDER_TIMING
