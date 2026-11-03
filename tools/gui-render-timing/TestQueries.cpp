#include "guilib/GUIRenderTiming.cpp"

#include <cassert>
#include <chrono>
#include <cctype>
#include <map>
#include <memory>
#include <vector>
#include <string>
#include <thread>
using namespace GUI_RENDER_TIMING;
static EGLContext current = reinterpret_cast<EGLContext>(1);
static GLuint nextName = 1, activeQuery = 0;
static unsigned begins = 0, ends = 0, reads = 0, deleted = 0, glCalls = 0;
static bool ready = false, disjointFlag = false, support = true, disjointOnPoll = false;
static std::map<GLuint, GLuint64> results;
static GLuint64 nextResult = 1000000;
extern "C" EGLContext eglGetCurrentContext()
{
  return current;
}
extern "C" const GLubyte* glGetString(GLenum)
{
  ++glCalls;
  return reinterpret_cast<const GLubyte*>(support ? "GL_EXT_disjoint_timer_query" : "");
}
extern "C" void glGetIntegerv(GLenum pname, GLint* value)
{
  assert(pname == GL_GPU_DISJOINT_EXT);
  ++glCalls;
  *value = disjointFlag;
  disjointFlag = false;
}
static void Gen(GLsizei n, GLuint* p)
{
  ++glCalls;
  while (n--)
    *p++ = nextName++;
}
static void Delete(GLsizei n, const GLuint*)
{
  ++glCalls;
  deleted += n;
}
static void Begin(GLenum target, GLuint q)
{
  ++glCalls;
  assert(target == GL_TIME_ELAPSED_EXT && !activeQuery);
  activeQuery = q;
  begins++;
}
static void End(GLenum target)
{
  ++glCalls;
  assert(target == GL_TIME_ELAPSED_EXT && activeQuery);
  results[activeQuery] = nextResult;
  activeQuery = 0;
  ends++;
}
static void Query(GLenum, GLenum pname, GLint* value)
{
  ++glCalls;
  *value = pname == GL_CURRENT_QUERY_EXT ? activeQuery : 64;
}
static void Available(GLuint, GLenum pname, GLuint* value)
{
  ++glCalls;
  assert(pname == GL_QUERY_RESULT_AVAILABLE_EXT);
  *value = ready;
  if (disjointOnPoll)
  {
    disjointFlag = true;
    disjointOnPoll = false;
  }
}
static void Result(GLuint q, GLenum pname, GLuint64* value)
{
  ++glCalls;
  assert(ready && pname == GL_QUERY_RESULT_EXT);
  reads++;
  *value = results.at(q);
}
extern "C" __eglMustCastToProperFunctionPointerType eglGetProcAddress(const char* name)
{
#define FUNC(symbol, fn) \
  if (std::string(name) == symbol) \
  return reinterpret_cast<__eglMustCastToProperFunctionPointerType>(fn)
  FUNC("glGenQueriesEXT", Gen);
  FUNC("glDeleteQueriesEXT", Delete);
  FUNC("glBeginQueryEXT", Begin);
  FUNC("glEndQueryEXT", End);
  FUNC("glGetQueryivEXT", Query);
  FUNC("glGetQueryObjectuivEXT", Available);
  FUNC("glGetQueryObjectui64vEXT", Result);
  return nullptr;
}
// Label and value of every placeholder in a logged line, in order. A
// placeholder's label is the word right before its '='; others are empty.
static std::vector<std::pair<std::string, std::string>> Fields(const Logged& line)
{
  std::vector<std::pair<std::string, std::string>> fields;
  const std::string& format = line.format;
  for (size_t open = format.find('{'); open != std::string::npos; open = format.find('{', open + 1))
  {
    assert(format.find('}', open) != std::string::npos);
    std::string label;
    if (open > 0 && format[open - 1] == '=')
    {
      size_t begin = open - 1;
      while (begin > 0 && (std::isalnum(static_cast<unsigned char>(format[begin - 1])) ||
                           format[begin - 1] == '_'))
        --begin;
      label = format.substr(begin, open - 1 - begin);
    }
    assert(fields.size() < line.args.size()); // No placeholder without an argument.
    fields.emplace_back(label, line.args[fields.size()]);
  }
  assert(fields.size() == line.args.size()); // No argument without a placeholder.
  return fields;
}
static std::string Field(const Logged& line, const std::string& label)
{
  std::string value;
  unsigned found = 0;
  for (const auto& field : Fields(line))
    if (field.first == label)
    {
      value = field.second;
      ++found;
    }
  assert(found == 1);
  return value;
}
int main()
{
  {
    Frame off(false, 60);
    Scope scope(Stage::Gui);
    AttachGPU();
    assert(!Active());
  }
  assert(!glCalls);
  ReleaseContext(reinterpret_cast<EGLContext>(99));
  assert(!glCalls);
  {
    Frame frame(true, 60);
    Scope gui(Stage::Gui);
    AttachGPU();
    {
      Scope fade(Stage::Fade);
      FadeLayers(&s, 3);
    }
    assert(activeQuery);
  }
  assert(begins == 1 && ends == 1 && !reads && !activeQuery);
  Slot* first = nullptr;
  for (auto& p : s.slots)
    if (p.pending)
      first = &p;
  assert(first && first->used == 1);
  assert(first->segments[0].stage == Stage::Setup);
  assert(first->record.fadeLayers == 3 && first->record.fadeVisits == 1);
  {
    Frame frame(true, 60);
    Scope gui(Stage::Gui);
    AttachGPU();
    Scope same(Stage::Gui);
    assert(!reads);
  } // Unavailable results never read.
  ready = true;
  s.lastLog = Now();
  {
    Frame frame(true, 60);
    Scope gui(Stage::Gui);
    AttachGPU();
  }
  assert(reads == 2 && s.worst.gpuValid); // One envelope per previous frame.
  assert(s.worst.gpu[Index(Stage::Setup)] == 1000000);
  assert(s.worst.gpu[Index(Stage::Gui)] == 0 && s.worst.gpu[Index(Stage::Fade)] == 0);
  const auto oldEpoch = epoch.load();
  ReleaseContext(reinterpret_cast<EGLContext>(99));
  assert(epoch.load() == oldEpoch);
  // Disjoint invalidates all pending results, including those still unavailable.
  disjointFlag = true;
  ready = false;
  {
    Frame frame(true, 60);
    Scope gui(Stage::Gui);
    AttachGPU();
  }
  for (auto& p : s.slots)
    if (p.pending && p.record.id < s.current.id)
      assert(!p.valid); // Current frame can be valid after reset boundary.
  ready = true;
  {
    Frame frame(true, 60);
    Scope gui(Stage::Gui);
    AttachGPU();
  }
  // Clock changes after the first disjoint check must invalidate ready data too.
  ready = false;
  {
    Frame frame(true, 60);
    Scope gui(Stage::Gui);
    AttachGPU();
  }
  ready = true;
  disjointOnPoll = true;
  const auto disjoints = s.disjoint;
  {
    Frame frame(true, 60);
    Scope gui(Stage::Gui);
    AttachGPU();
  }
  assert(s.disjoint > disjoints);
  // Do not touch a query owned by another tool.
  activeQuery = 999;
  {
    Frame frame(true, 60);
    Scope gui(Stage::Gui);
    AttachGPU();
    assert(activeQuery == 999);
  }
  activeQuery = 0;
  // Saturated ring skips GPU capture rather than waiting or reusing pending names.
  ready = false;
  for (unsigned i = 0; i < SLOTS + 3; i++)
  {
    Frame frame(true, 60);
    Scope gui(Stage::Gui);
    AttachGPU();
  }
  assert(s.dropped > 0);
  // Frame segment bound is finite and does not leave a query active.
  ready = true;
  {
    Frame frame(true, 60);
    Scope gui(Stage::Gui);
    AttachGPU();
    for (unsigned i = 0; i < SEGMENTS + 2; i++)
    {
      SetStage(Stage::Present);
      SetStage(Stage::Gui);
    }
  }
  assert(!activeQuery);
  // Context teardown closes our query once; old query names cannot survive reuse.
  {
    Frame frame(true, 60);
    Scope gui(Stage::Gui);
    AttachGPU();
    ReleaseContext(current);
    assert(!activeQuery && !s.slot && s.context == EGL_NO_CONTEXT);
  }
  assert(deleted > 0);
  const auto before = deleted;
  current = reinterpret_cast<EGLContext>(2);
  {
    Frame frame(true, 60);
    Scope gui(Stage::Gui);
    AttachGPU();
    current = reinterpret_cast<EGLContext>(3);
    ReleaseContext(reinterpret_cast<EGLContext>(2));
  }
  assert(deleted == before);
  activeQuery = 0; // Simulate destruction of old context's query state.
  support = false;
  {
    Frame frame(true, 60);
    Scope gui(Stage::Gui);
    AttachGPU();
    assert(!s.api.supported);
  }
  // Unsupported extension keeps CPU timing alive.
  assert(s.frames > 0 && !messages.empty());
  // Toggle off without polling or freeing pending GPU work. On resume, old
  // capture data is discarded and only the fresh session is reported.
  ReleaseContext(current);
  support = true;
  ready = false;
  {
    Frame frame(true, 60);
    Scope gui(Stage::Gui);
    AttachGPU();
  }
  const auto oldId = s.current.id;
  const auto callsBeforeOff = glCalls;
  {
    Frame off(false, 60);
    Scope gui(Stage::Gui);
    AttachGPU();
    assert(!Active());
  }
  assert(glCalls == callsBeforeOff && !s.captureEnabled && s.frames == 0);
  for (const auto& slot : s.slots)
    if (slot.pending)
      assert(!slot.report);
  {
    Frame frame(true, 60);
    Scope gui(Stage::Gui);
    AttachGPU();
  }
  const auto resumedId = s.current.id;
  assert(resumedId > oldId);
  ready = true;
  {
    Frame frame(true, 60);
    Scope gui(Stage::Gui);
    AttachGPU();
  }
  assert(s.frames == 1 && s.worst.id == resumedId);
  {
    Frame off(false, 60);
  }
  ReleaseContext(current);
  assert(s.frames == 0 && s.aborted == 0); // Teardown must not leak old-session data.
  {
    Frame frame(true, 60);
  } // CPU-only capture can start again too.
  Record slow;
  slow.budget = 16.667;
  slow.gpuValid = true;
  slow.cpu[Index(Stage::Process)] = 20000000;
  slow.gpu[Index(Stage::Fade)] = 20000000;
  const auto cpuOver = s.overCPU, gpuOver = s.overGPU;
  s.lastLog = Now();
  Report(slow);
  assert(s.overCPU == cpuOver + 1 && s.overGPU == gpuOver + 1);

  // An impossible elapsed result invalidates the frame's GPU time and is never
  // chosen as the worst GPU frame.
  {
    Frame off(false, 60);
  }
  ready = false;
  nextResult = 18446744071537000000ULL;
  uint64_t wrapped = 0;
  {
    Frame frame(true, 60);
    Scope gui(Stage::Gui);
    AttachGPU();
    wrapped = s.current.id;
  }
  ready = true;
  nextResult = 2000000;
  s.lastLog = Now();
  {
    Frame frame(true, 60);
    Scope gui(Stage::Gui);
    AttachGPU();
  }
  assert(s.invalidGPU == 1 && s.worstGPUId != wrapped && s.worstGPU < 1000);

  // The detailed CPU frame is chosen by CPU work; a GPU result cannot displace it.
  {
    Record cpuHeavy, gpuHeavy;
    cpuHeavy.id = 1001;
    cpuHeavy.budget = gpuHeavy.budget = 16.667;
    cpuHeavy.cpu[Index(Stage::Controls)] = 30000000;
    gpuHeavy.id = 1002;
    gpuHeavy.gpuValid = true;
    gpuHeavy.cpu[Index(Stage::Controls)] = 1000000;
    gpuHeavy.gpu[Index(Stage::Setup)] = 50000000;
    s.lastLog = Now();
    s.worstScore = s.worstGPU = -1;
    Report(cpuHeavy);
    Report(gpuHeavy);
    assert(s.worst.id == 1001 && s.worstGPUId == 1002);
  }

  // Late presentations count only between consecutive swapping frames and
  // record their time since input and motion.
  {
    Frame off(false, 60);
  }
  s.lastLog = Now();
  {
    Frame frame(true, 1000);
    SetStage(Stage::Present);
    Presented(true);
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(3));
  {
    Frame frame(true, 1000);
    Motion();
    SetStage(Stage::Present);
    Presented(true);
    assert(!s.current.hasInput && s.current.moving);
  }
  assert(s.presents == 2 && s.late == 1 && s.lateMoving == 1 && s.lateAfterInput[4] == 1);
  // A frame that did not render breaks continuity, even when it entered the stage.
  {
    Frame idle(true, 1000);
    SetStage(Stage::Present);
    Presented(false);
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(3));
  {
    Frame frame(true, 1000);
    SetStage(Stage::Present);
    Presented(true);
  }
  assert(s.presents == 3 && s.late == 1); // A skipped swap and idle sleep are not late.
  // A key handled during a frame is that frame's input.
  std::this_thread::sleep_for(std::chrono::milliseconds(3));
  {
    Frame frame(true, 1000);
    Input();
    assert(s.current.hasInput && s.current.sinceInput == 0);
    SetStage(Stage::Present);
    Presented(true);
  }
  assert(s.presents == 4 && s.late == 2 && s.lateAfterInput[0] == 1);

  // Timed parts nest exclusively, count their entries and are ignored while
  // rendering. Counted parts read no clock: their time stays in the stage.
  {
    Frame frame(true, 60);
    {
      Scope controls(Stage::Controls);
      Scope info(Stage::Info);
      Scope items(Stage::Items);
      Scope load(Stage::Load);
      assert(s.stage == Stage::Controls);
      {
        Scope text(Stage::Text);
        const auto start = ThreadNow();
        while (ThreadNow() - start < 1000000)
        {
        }
      }
      assert(s.stage == Stage::Controls);
    }
    assert(s.current.entries[Index(Stage::Text)] == 1 && s.current.entries[Index(Stage::Info)] == 1 &&
           s.current.entries[Index(Stage::Items)] == 1 && s.current.entries[Index(Stage::Load)] == 1);
    assert(!s.current.cpu[Index(Stage::Info)] && !s.current.cpu[Index(Stage::Items)] &&
           !s.current.cpu[Index(Stage::Load)]);
    assert(s.current.threadCpu[Index(Stage::Text)] >= 1000000 &&
           // Wall and thread clocks are read one after the other at each transition.
           s.current.cpu[Index(Stage::Text)] + 100000 >= s.current.threadCpu[Index(Stage::Text)]);
    {
      Scope gui(Stage::Gui);
      Scope text(Stage::Text);
      assert(s.stage == Stage::Gui);
    }
    assert(s.current.entries[Index(Stage::Text)] == 1);
    int a = 0, b = 0;
    FadeLayers(&a, 2);
    FadeLayers(&a, 2);
    FadeLayers(&b, 3);
    assert(s.current.fadeVisits == 3 && s.current.fadeControlCount == 2 && s.current.fadeLayers == 3);
  }

  // Layout freeing is its own part; texture releases nested in it stay exclusive.
  {
    Frame frame(true, 60);
    {
      Scope controls(Stage::Controls);
      Scope layouts(Stage::Layouts);
      LayoutsFreed(2);
      {
        Scope release(Stage::Release);
        TextureReleased();
      }
      assert(s.stage == Stage::Layouts);
    }
    assert(s.current.entries[Index(Stage::Layouts)] == 2 && // Entered, and returned to.
           s.current.entries[Index(Stage::Release)] == 1);
    assert(s.current.layoutsFreed == 2 && s.current.texturesReleased == 1);
    Scope gui(Stage::Gui);
    Scope layouts(Stage::Layouts);
    assert(s.stage == Stage::Gui); // Ignored while rendering.
  }
  LayoutsFreed(5);
  TextureReleased();
  assert(s.current.layoutsFreed == 2 && s.current.texturesReleased == 1); // Inactive: ignored.

  // Scheduler time is attributed per top-level stage, including its nested
  // parts, and sampled only at top-level boundaries.
  {
    Frame frame(true, 60);
    const auto initial = s.lastSched.wall;
    {
      Scope controls(Stage::Controls);
      const auto entered = s.lastSched.wall;
      assert(entered > initial);
      {
        Scope items(Stage::Items);
        const auto start = ThreadNow();
        while (ThreadNow() - start < 2000000)
        {
        }
      }
      assert(s.lastSched.wall == entered); // Returning from a part is not a boundary.
    }
    {
      Scope move(Stage::WindowMove);
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }
  const Record& r = s.current; // No GPU capture: the record stays current.
  const auto controls = Index(Stage::Controls), move = Index(Stage::WindowMove);
  assert(r.groupWall[controls] >= 2000000 && r.groupWall[move] >= 5000000);
  assert(r.voluntary[move] >= 1); // The sleep blocked.
  assert(r.coreStart >= 0 && r.coreEnd >= 0);
  if (r.schedValid)
  {
    assert(r.groupExec[controls] >= 1900000); // Busy in a nested part, on the CPU.
    assert(r.groupExec[move] + 4000000 <= r.groupWall[move]); // Asleep, off the CPU.
  }
  assert(!SchedText(r).empty() && SchedText(r).find(" controls=") != std::string::npos &&
         SchedText(r).find(" items=") == std::string::npos);

  // Background crossfade paths are counted per interval and only while capturing.
  s.lastLog = Now();
  {
    Frame frame(true, 60);
    Crossfade(CrossfadePath::Drawn);
    Crossfade(CrossfadePath::Drawn);
    Crossfade(CrossfadePath::Stack);
    Crossfade(CrossfadePath::Count);
  }
  assert(s.crossfade[0] == 2 && s.crossfade[2] == 1 && s.crossfade[1] == 0);
  Crossfade(CrossfadePath::Disabled);
  assert(s.crossfade[5] == 0);

  // Input to the next accepted swap, also after idle frames, which the late
  // count cannot see. The earliest unanswered input is measured.
  {
    Frame off(false, 60);
  }
  {
    Frame frame(true, 60);
  }
  assert(s.inputToPresent == (std::array<size_t, 5>{}) && pendingInput.load() == 0);
  Input();
  std::this_thread::sleep_for(std::chrono::milliseconds(3));
  Input(); // A second input before any swap does not restart the measurement.
  {
    Frame frame(true, 60);
    SetStage(Stage::Present);
    Presented(false); // Not presented: still waiting.
  }
  assert(pendingInput.load() != 0 && s.inputToPresentMax == 0);
  std::this_thread::sleep_for(std::chrono::milliseconds(25));
  {
    Frame frame(true, 60);
    SetStage(Stage::Present);
    Presented(true);
  }
  assert(s.inputToPresent[1] + s.inputToPresent[2] + s.inputToPresent[3] == 1 &&
         s.inputToPresentMax >= 28000000 && pendingInput.load() == 0);
  {
    Frame frame(true, 60);
    SetStage(Stage::Present);
    Presented(true); // No input pending: nothing measured.
  }
  assert(s.inputToPresent[0] + s.inputToPresent[1] + s.inputToPresent[2] + s.inputToPresent[3] +
             s.inputToPresent[4] == 1);
  pendingInput.store(Now() - 2 * STALE_INPUT);
  {
    Frame frame(true, 60);
    SetStage(Stage::Present);
    Presented(true);
  }
  assert(s.staleInput == 1 && s.inputToPresentMax < STALE_INPUT);
  // An input before capture starts is not measured.
  {
    Frame off(false, 60);
  }
  Input();
  {
    Frame frame(true, 60);
    SetStage(Stage::Present);
    Presented(true);
  }
  assert(s.inputToPresentMax == 0 && s.staleInput == 0);

  // Presentations and late ones are tagged with a background fade drawn in the
  // frame and with decode or upload work running at any time during it.
  {
    Frame off(false, 60);
  }
  {
    Frame frame(true, 1000);
    SetStage(Stage::Present);
    Presented(true);
  }
  {
    Work decode(Job::Decode); // Running when the frame starts.
    std::this_thread::sleep_for(std::chrono::milliseconds(3));
    Frame frame(true, 1000);
    SetStage(Stage::Present);
    Presented(true);
  }
  assert(s.presentsWith[1] == 1 && s.lateWith[1] == 1 && s.lateWith[2] == 0 && s.lateWithNone == 0);
  assert(jobActive[0].load() == 0 && jobDone[0] == 1 && jobBusy[0] >= 3000000);
  std::this_thread::sleep_for(std::chrono::milliseconds(3));
  {
    Frame frame(true, 1000);
    {
      Work upload(Job::Upload); // Started and finished within the frame.
    }
    int incoming = 0, outgoing = 0;
    {
      FadeDraws visit(true); // Visited, but nothing drawn: not a blend.
      FadeLayers(&s, 2);
    }
    {
      FadeDraws one(true); // One image drawn twice is not a blend either.
      TextureDrawn(&incoming);
      TextureDrawn(&incoming);
    }
    assert(s.current.fadeBlends == 0);
    TextureDrawn(&outgoing); // Outside a fade: ignored.
    {
      FadeDraws blend(true);
      TextureDrawn(&incoming);
      TextureDrawn(&outgoing);
    }
    assert(s.current.fadeBlends == 1);
    {
      FadeDraws disabled(false);
      TextureDrawn(&incoming);
      TextureDrawn(&outgoing);
    }
    assert(s.current.fadeBlends == 1 && !s.inFade);
    SetStage(Stage::Present);
    Presented(true);
  }
  assert(s.presentsWith[0] == 1 && s.presentsWith[2] == 1 && s.lateWith[0] == 1 &&
         s.lateWith[2] == 1 && s.lateWith[1] == 1);
  std::this_thread::sleep_for(std::chrono::milliseconds(3));
  {
    Frame frame(true, 1000);
    SetStage(Stage::Present);
    Presented(true);
  }
  assert(s.lateWithNone == 1 && s.presentsWith[1] == 1 && s.presentsWith[2] == 1);

  // A fade visit alone does not tag the frame.
  std::this_thread::sleep_for(std::chrono::milliseconds(3));
  {
    Frame frame(true, 1000);
    FadeDraws visit(true);
    FadeLayers(&s, 2);
    SetStage(Stage::Present);
    Presented(true);
  }
  assert(s.presentsWith[0] == 1 && s.lateWith[0] == 1 && s.lateWithNone == 2);

  // Job totals start fresh with each capture; a job running across the start
  // counts from it.
  {
    Frame off(false, 60);
  }
  {
    Work before(Job::Decode); // Entirely before the capture.
  }
  uint64_t running = 0;
  {
    Work across(Job::Decode);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    {
      Frame frame(true, 60); // Capture starts.
    }
    assert(jobDone[0] == 0 && jobBusy[0] == 0);
    running = Now();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  const uint64_t after = Now() - running;
  assert(jobDone[0] == 1 && jobBusy[0] >= 5000000 && jobBusy[0] <= after + 1000000);
  assert(jobActive[0].load() == 0);

  // Capture start and a job's accounting are one transaction. A worker that
  // read the old capture start and is held inside its accounting cannot add
  // its earlier time after the reset: the capture waits for it, then clears.
  {
    static std::atomic<bool> inside{false}, release{false};
    {
      Frame off(false, 60);
    }
    inside = release = false;
    jobEndHook = []
    {
      inside = true;
      while (!release)
        std::this_thread::yield();
    };
    std::thread worker(
        []
        {
          Work decode(Job::Decode); // Entirely before the capture.
          std::this_thread::sleep_for(std::chrono::milliseconds(25));
        });
    while (!inside)
      std::this_thread::yield();
    std::thread releaser(
        []
        {
          std::this_thread::sleep_for(std::chrono::milliseconds(30));
          release = true;
        });
    const auto start = Now();
    {
      Frame frame(true, 60); // Capture starts while the worker is held.
    }
    const auto waited = Now() - start;
    worker.join();
    releaser.join();
    jobEndHook = nullptr;
    {
      std::lock_guard<std::mutex> lock(jobMutex);
      assert(jobDone[0] == 0 && jobBusy[0] == 0); // No pre-capture time.
    }
    assert(waited >= 20000000); // The reset waited for the accounting.
    assert(jobActive[0].load() == 0);
  }

  // Every label of a report carries its own value.
  {
    Frame frame(true, 60);
  }
  for (size_t i = 0; i < CROSSFADE_PATHS; ++i)
    s.crossfade[i] = 10 + i;
  s.presentsWith = {21, 22, 23};
  s.lateWith = {31, 32, 33};
  s.lateWithNone = 34;
  s.inputToPresent = {41, 42, 43, 44, 45};
  s.staleInput = 46;
  logged.clear();
  s.lastLog = Now() - 2000000000;
  {
    Record report;
    report.id = 77;
    report.cpu[Index(Stage::Controls)] = 50000000; // The interval's slowest frame.
    report.fadeBlends = 3;
    report.layoutsFreed = 4;
    report.texturesReleased = 5;
    Report(report);
  }
  assert(logged.size() == 2);
  const Logged& summary = logged[0];
  const Logged& worst = logged[1];
  assert(Field(summary, "drawn") == "10" && Field(summary, "clipped") == "11" &&
         Field(summary, "stack") == "12" && Field(summary, "not_ready") == "13" &&
         Field(summary, "ineligible") == "14" && Field(summary, "disabled") == "15");
  const auto tagged = Fields(summary);
  std::vector<std::string> tags;
  for (const auto& field : tagged)
    if (field.first == "fade" || field.first == "decode" || field.first == "upload" ||
        field.first == "none")
      tags.push_back(field.first + "=" + field.second);
  // From presents_with through late_with: late_after_input_ms[...] has its own
  // none= before, and jobs[...] reuses decode= and upload= after.
  tags.erase(tags.begin(), std::find(tags.begin(), tags.end(), "fade=21"));
  tags.resize(7);
  assert((tags == std::vector<std::string>{"fade=21", "decode=22", "upload=23", "fade=31",
                                           "decode=32", "upload=33", "none=34"}));
  assert(Field(summary, "stale") == "46");
  assert(Field(worst, "frame") == "77" && Field(worst, "fade_blends") == "3" &&
         Field(worst, "layout_items_freed") == "4" && Field(worst, "textures_released") == "5");
  assert(s.crossfade[0] == 0); // The interval restarts after its line.

  // The DDR monitor is read only when enabled, then cleared.
  {
    char directory[] = "/tmp/gui-render-timing-ddr-XXXXXX";
    assert(mkdtemp(directory));
    const std::string mode = std::string(directory) + "/mode", stat = std::string(directory) + "/stat";
    DDR_MODE = mode.c_str();
    DDR_STAT = stat.c_str();
    const auto write = [](const std::string& path, const char* text)
    {
      FILE* file = std::fopen(path.c_str(), "w");
      std::fputs(text, file);
      std::fclose(file);
    };
    const auto read = [](const std::string& path)
    {
      char text[64] = {};
      FILE* file = std::fopen(path.c_str(), "r");
      const size_t n = std::fread(text, 1, sizeof(text) - 1, file);
      std::fclose(file);
      return std::string(text, n);
    };
    write(mode, "0\n");
    assert(DDRText() == "[off]");
    write(mode, "1\n");
    // ddr_bandwidth.c usage_stat_show output.
    write(stat, "MAX bandwidth:  3456789 KB/s, usage: 41.20%, tick:123 us\n"
                "AVG bandwidth:  1234567 KB/s, usage: 14.70%, samples:250\n"
                "\nbandwidth status for each channel\n"
                "ch,        port mask, avg bw(KB/s), max bw(KB/s), bw@max sample(KB/s)\n"
                " 0,                2,       654321,      2000000,           1900000\n"
                " 1,                1,       300000,       900000,            800000\n"
                "\nusage distribution:\n");
    assert(DDRText() == "[avg_kbs=1234567 max_kbs=3456789 ch0=654321/2000000/1900000 "
                        "ch1=300000/900000/800000]");
    assert(read(stat)[0] == '0'); // "0" written: cleared for the next interval.
    write(stat, "No sample, please wait...\n");
    assert(DDRText() == "[unread]");
    std::remove(mode.c_str());
    std::remove(stat.c_str());
    rmdir(directory);
    DDR_MODE = "/sys/class/aml_ddr/mode";
    DDR_STAT = "/sys/class/aml_ddr/usage_stat";
  }
  // No logged line has a placeholder without an argument, or the reverse.
  for (const auto& line : logged)
    Fields(line);
}
