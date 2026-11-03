/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "GUIRenderTiming.h"

#include "utils/log.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <ctime>
#include <string>
#include <time.h>

#include <fcntl.h>
#include <sched.h>
#include <sys/resource.h>
#include <unistd.h>

#include <EGL/egl.h>

// Extension typedefs require the base GLES declarations first.
// clang-format off
#include "system_gl.h"
#include <GLES2/gl2ext.h>
// clang-format on

namespace GUI_RENDER_TIMING
{
namespace
{
constexpr size_t STAGES = static_cast<size_t>(Stage::Count);
constexpr size_t SLOTS = 24, SEGMENTS = 32, FADE_CONTROLS = 8;
constexpr double MS = 1000000.0;
// An elapsed query result above one second is not a GUI frame. Mali has
// returned wrapped negative intervals when a render pass is split.
constexpr GLuint64 MAX_GPU_ELAPSED = 1000000000;
using Clock = std::chrono::steady_clock;
uint64_t Now()
{
  return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
      .count();
}
uint64_t ThreadNow()
{
  timespec t{};
  if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t) != 0)
    return 0;
  return uint64_t(t.tv_sec) * 1000000000 + t.tv_nsec;
}
size_t Index(Stage stage)
{
  return static_cast<size_t>(stage);
}
bool GPUStage(Stage stage)
{
  return stage >= Stage::Setup && stage <= Stage::EndRender;
}
bool SubStage(Stage stage)
{
  return stage >= Stage::Items && stage <= Stage::Layouts;
}
constexpr const char* NAMES[] = {"idle",   "process",     "move_other", "input", "controls",
                                  "items",  "info",        "text",       "load",  "release",
                                  "layouts", "window_move", "external",   "setup", "video",
                                  "gui",    "fade",        "after",      "composite",
                                  "end",    "present"};
static_assert(sizeof(NAMES) / sizeof(NAMES[0]) == STAGES, "a name for every stage");
constexpr size_t CROSSFADE_PATHS = static_cast<size_t>(CrossfadePath::Count);
// Input to the next accepted swap: under 20, 34, 50 and 100 ms, or later.
// A swap more than a second after the input did not answer it.
constexpr uint64_t STALE_INPUT = 1000000000;
// Earliest input not yet followed by an accepted swap; 0 when none.
std::atomic<uint64_t> pendingInput{0};
constexpr size_t JOBS = static_cast<size_t>(Job::Count);
// Background image work on job workers: running now, started, and per
// interval finished and busy time.
std::array<std::atomic<int>, JOBS> jobActive{};
std::array<std::atomic<uint64_t>, JOBS> jobStarts{};
// Job totals and the capture start they are measured from change together:
// a capture reset and a job's accounting never interleave.
std::mutex jobMutex;
std::array<uint64_t, JOBS> jobDone{}, jobBusy{};
uint64_t captureBegin{0}; // Job time before it is not reported.
// Tests only: runs inside a job's accounting, under jobMutex.
std::atomic<void (*)()> jobEndHook{nullptr};
// Amlogic DMC bandwidth monitor, read once per log line when enabled.
const char* DDR_MODE = "/sys/class/aml_ddr/mode";
const char* DDR_STAT = "/sys/class/aml_ddr/usage_stat";
// Scheduler counters of the calling thread. run is time spent runnable but
// waiting for a CPU; exec is time on a CPU (CONFIG_SCHED_INFO).
struct Sched
{
  uint64_t wall{0}, exec{0}, run{0};
  long voluntary{0}, involuntary{0};
  int cpu{-1};
  bool valid{false};
};
// Input actions can be handled on another thread than the GUI frame loop.
std::atomic<uint64_t> lastInput{0};
struct Record
{
  uint64_t id{0}, begin{0}, threadBegin{0}, wall{0}, thread{0}, wallClock{0};
  double budget{0};
  std::array<uint64_t, STAGES> cpu{}, threadCpu{}, gpu{};
  std::array<unsigned int, STAGES> entries{};
  std::array<const void*, FADE_CONTROLS> fadeControls{};
  unsigned int fadeVisits{0}, fadeLayers{0}, fadeControlCount{0}, fadeBlends{0};
  // Per top-level stage including its nested parts, between scheduler samples.
  std::array<uint64_t, STAGES> groupWall{}, groupExec{}, runWait{};
  std::array<long, STAGES> voluntary{}, involuntary{};
  int coreStart{-1}, coreEnd{-1};
  unsigned int migrations{0}, layoutsFreed{0}, texturesReleased{0};
  bool schedValid{false};
  std::array<uint64_t, JOBS> jobStartsAtBegin{};
  std::array<bool, JOBS> jobRunningAtBegin{};
  uint64_t sinceInput{0};
  bool hasInput{false}, moving{false}, presented{false}, gpuValid{false};
};
struct Segment
{
  GLuint query{0};
  Stage stage{Stage::Idle};
};
struct Slot
{
  Record record;
  std::array<Segment, SEGMENTS> segments{};
  size_t used{0};
  bool pending{false}, valid{true}, report{true};
};
struct API
{
  PFNGLGENQUERIESEXTPROC gen{};
  PFNGLDELETEQUERIESEXTPROC del{};
  PFNGLBEGINQUERYEXTPROC begin{};
  PFNGLENDQUERYEXTPROC end{};
  PFNGLGETQUERYIVEXTPROC query{};
  PFNGLGETQUERYOBJECTUIVEXTPROC available{};
  PFNGLGETQUERYOBJECTUI64VEXTPROC result{};
  bool supported{false};
};
std::atomic<uint64_t> epoch{0};
std::atomic<EGLContext> observedContext{EGL_NO_CONTEXT};
struct State
{
  bool active{false}, initialized{false}, queryActive{false}, captureEnabled{false};
  bool attemptedGPU{false}, previousPresented{false}, previousMoving{false};
  uint64_t generation{0}, nextId{0}, stageBegin{0}, stageThread{0}, lastLog{0}, lastPresent{0};
  uint64_t dropped{0}, disjoint{0}, aborted{0}, invalidGPU{0};
  size_t frames{0}, overCPU{0}, overGPU{0}, presents{0}, late{0}, lateMoving{0};
  // Late presentations by time since the last input: <100, <300, <600, >=600 ms, none.
  std::array<size_t, 5> lateAfterInput{};
  std::array<size_t, CROSSFADE_PATHS> crossfade{};
  std::array<size_t, 5> inputToPresent{};
  // Presentations and late ones by what the frame overlapped: a background
  // fade it drew, decoding or uploading on job workers; late with none.
  std::array<size_t, 3> presentsWith{}, lateWith{};
  size_t lateWithNone{0};
  size_t staleInput{0};
  uint64_t inputToPresentMax{0};
  int schedFd{-1};
  bool schedOpened{false};
  bool inFade{false};
  std::array<const void*, 4> fadeImages{};
  unsigned int fadeImageCount{0};
  Sched lastSched;
  Stage topStage{Stage::Idle};
  EGLContext context{EGL_NO_CONTEXT};
  API api;
  Stage stage{Stage::Idle};
  Record current, worst;
  double worstScore{-1}, worstGPU{-1};
  uint64_t worstGPUId{0};
  std::array<Slot, SLOTS> slots{};
  Slot* slot{nullptr};
};
thread_local State s;

Sched ReadSched()
{
  Sched sample;
  sample.wall = Now();
  if (!s.schedOpened)
  {
    s.schedOpened = true;
    s.schedFd = open("/proc/thread-self/schedstat", O_RDONLY | O_CLOEXEC);
  }
  if (s.schedFd >= 0)
  {
    char text[96];
    const ssize_t n = pread(s.schedFd, text, sizeof(text) - 1, 0);
    unsigned long long exec = 0, run = 0;
    if (n > 0)
    {
      text[n] = 0;
      sample.valid = std::sscanf(text, "%llu %llu", &exec, &run) == 2;
      sample.exec = exec;
      sample.run = run;
    }
  }
  rusage usage{};
  if (getrusage(RUSAGE_THREAD, &usage) == 0)
  {
    sample.voluntary = usage.ru_nvcsw;
    sample.involuntary = usage.ru_nivcsw;
  }
  sample.cpu = sched_getcpu();
  return sample;
}
// Attribute scheduler time since the previous sample to the top-level stage
// that was active, including any parts nested in it.
void SampleSched()
{
  const Sched now = ReadSched();
  const Sched& last = s.lastSched;
  auto& r = s.current;
  const size_t i = Index(s.topStage);
  const auto delta = [](auto after, auto before) { return after > before ? after - before : 0; };
  r.groupWall[i] += delta(now.wall, last.wall);
  r.schedValid = now.valid && last.valid;
  if (r.schedValid)
  {
    r.groupExec[i] += delta(now.exec, last.exec);
    r.runWait[i] += delta(now.run, last.run);
  }
  r.voluntary[i] += delta(now.voluntary, last.voluntary);
  r.involuntary[i] += delta(now.involuntary, last.involuntary);
  if (now.cpu >= 0 && last.cpu >= 0 && now.cpu != last.cpu)
    ++r.migrations;
  r.coreEnd = now.cpu;
  s.lastSched = now;
}
std::string SchedText(const Record& r)
{
  std::string text;
  char part[96];
  for (size_t i = 0; i < STAGES; ++i)
  {
    if (!r.groupWall[i])
      continue;
    std::snprintf(part, sizeof(part), " %s=%.2f/%.2f/%.2f/%ld/%ld", NAMES[i], r.groupWall[i] / MS,
                  r.groupExec[i] / MS, r.runWait[i] / MS, r.voluntary[i], r.involuntary[i]);
    text += part;
  }
  return text;
}

uint64_t CPUWork(const Record& record)
{
  uint64_t total = 0;
  for (size_t i = Index(Stage::Process); i <= Index(Stage::EndRender); ++i)
    total += record.cpu[i];
  return total;
}
uint64_t GPUTotal(const Record& record)
{
  uint64_t total = 0;
  for (const auto value : record.gpu)
    total += value;
  return total;
}
std::string Start(uint64_t wallClock)
{
  const std::time_t seconds = static_cast<std::time_t>(wallClock / 1000);
  std::tm local{};
  localtime_r(&seconds, &local);
  char text[16];
  std::snprintf(text, sizeof(text), "%02d:%02d:%02d.%03u", local.tm_hour, local.tm_min,
                local.tm_sec, static_cast<unsigned int>(wallClock % 1000));
  return text;
}
// Average and peak DDR bandwidth since the previous read, then cleared. Only
// read when the monitor was enabled, for example with
// echo 1 > /sys/class/aml_ddr/mode; the GUI never enables it.
std::string DDRText()
{
  char text[2048];
  const auto read = [&text](const char* path)
  {
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
      return false;
    const ssize_t n = ::read(fd, text, sizeof(text) - 1);
    close(fd);
    text[n > 0 ? n : 0] = 0;
    return n > 0;
  };
  if (!read(DDR_MODE) || std::atoi(text) != 1 || !read(DDR_STAT))
    return "[off]";
  unsigned long maxBw = 0;
  unsigned long long avgBw = 0;
  const char* max = std::strstr(text, "MAX bandwidth:");
  const char* avg = std::strstr(text, "AVG bandwidth:");
  if (!max || !avg || std::sscanf(max, "MAX bandwidth: %lu", &maxBw) != 1 ||
      std::sscanf(avg, "AVG bandwidth: %llu", &avgBw) != 1)
    return "[unread]";
  std::string result = "[avg_kbs=" + std::to_string(avgBw) + " max_kbs=" + std::to_string(maxBw);
  // Per channel: ch, port mask, avg, max, bandwidth at the total maximum.
  const char* line = std::strstr(text, "bandwidth status for each channel");
  for (line = line ? std::strchr(line, '\n') : nullptr; line; line = std::strchr(line + 1, '\n'))
  {
    int ch = 0, atMax = 0;
    unsigned long long mask = 0, chAvg = 0;
    unsigned long chMax = 0;
    if (std::sscanf(line + 1, "%d, %llx, %llu, %lu, %d", &ch, &mask, &chAvg, &chMax, &atMax) == 5)
      result += " ch" + std::to_string(ch) + "=" + std::to_string(chAvg) + "/" +
                std::to_string(chMax) + "/" + std::to_string(atMax);
  }
  const int fd = open(DDR_STAT, O_WRONLY | O_CLOEXEC);
  if (fd >= 0)
  {
    if (write(fd, "0", 1) != 1)
      result += " uncleared";
    close(fd);
  }
  return result + "]";
}
void ResetWindow()
{
  s.frames = s.overCPU = s.overGPU = s.presents = s.late = s.lateMoving = 0;
  s.lateAfterInput = {};
  s.crossfade = {};
  s.inputToPresent = {};
  s.presentsWith = s.lateWith = {};
  s.lateWithNone = 0;
  s.staleInput = 0;
  s.inputToPresentMax = 0;
  s.worstScore = s.worstGPU = -1;
  s.worstGPUId = 0;
  s.dropped = s.disjoint = s.aborted = s.invalidGPU = 0;
}
void Report(const Record& record)
{
  ++s.frames;
  const double cpu = CPUWork(record) / MS, gpu = GPUTotal(record) / MS;
  s.overCPU += cpu > record.budget;
  s.overGPU += record.gpuValid && gpu > record.budget;
  // The detailed CPU frame is chosen by CPU work only; a GPU result cannot
  // displace it. The slowest valid GPU frame is reported separately.
  if (cpu > s.worstScore)
  {
    s.worst = record;
    s.worstScore = cpu;
  }
  if (record.gpuValid && gpu > s.worstGPU)
  {
    s.worstGPU = gpu;
    s.worstGPUId = record.id;
  }
  const uint64_t now = Now();
  if (!s.lastLog)
  {
    s.lastLog = now;
    return;
  }
  if (now - s.lastLog < 1000000000)
    return;
  std::array<uint64_t, JOBS> done{}, busy{};
  {
    std::lock_guard<std::mutex> lock(jobMutex);
    done = jobDone;
    busy = jobBusy;
    jobDone = jobBusy = {};
  }
  const auto path = [](CrossfadePath p) { return s.crossfade[static_cast<size_t>(p)]; };
  static_assert(CROSSFADE_PATHS == 6, "report every crossfade path");
  static_assert(JOBS == 2, "report decode and upload jobs");
  const auto& r = s.worst;
  const auto c = [&](Stage stage) { return r.cpu[Index(stage)] / MS; };
  const auto t = [&](Stage stage) { return r.threadCpu[Index(stage)] / MS; };
  const auto e = [&](Stage stage) { return r.entries[Index(stage)]; };
  CLog::Log(LOGINFO,
            "GUI timing: frames={} presents={} late={} late_moving={} "
            "late_after_input_ms[<100={} <300={} <600={} >=600={} none={}] cpu_over={} "
            "gpu_over={} gpu_invalid={} dropped={} disjoint={} aborted={} gpu_worst_ms={:.3f} "
            "gpu_worst_frame={} crossfade[drawn={} clipped={} stack={} not_ready={} "
            "ineligible={} disabled={}] input_to_present_ms[<20={} <34={} <50={} <100={} >=100={} stale={}] "
            "input_to_present_max_ms={:.3f} presents_with[fade={} decode={} upload={}] "
            "late_with[fade={} decode={} upload={} none={}] jobs[decode={}/{:.1f}ms "
            "upload={}/{:.1f}ms] ddr{}",
            s.frames, s.presents, s.late, s.lateMoving, s.lateAfterInput[0], s.lateAfterInput[1],
            s.lateAfterInput[2], s.lateAfterInput[3], s.lateAfterInput[4], s.overCPU, s.overGPU,
            s.invalidGPU, s.dropped, s.disjoint, s.aborted, std::max(s.worstGPU, 0.0),
            s.worstGPUId, path(CrossfadePath::Drawn), path(CrossfadePath::Clipped),
            path(CrossfadePath::Stack), path(CrossfadePath::NotReady),
            path(CrossfadePath::Ineligible), path(CrossfadePath::Disabled), s.inputToPresent[0], s.inputToPresent[1], s.inputToPresent[2],
            s.inputToPresent[3], s.inputToPresent[4], s.staleInput, s.inputToPresentMax / MS,
            s.presentsWith[0], s.presentsWith[1], s.presentsWith[2], s.lateWith[0], s.lateWith[1],
            s.lateWith[2], s.lateWithNone, done[0], busy[0] / MS, done[1], busy[1] / MS, DDRText());
  CLog::Log(LOGINFO,
            "GUI timing worst CPU: frame={} start={} since_input_ms={} moving={} budget_ms={:.3f} "
            "cpu_work_ms={:.3f} thread_ms={:.3f} loop_ms={:.3f} fade_visits={} fade_blends={} "
            "fade_controls={} "
            "max_fade_layers={} entries[items={} info={} text={} load={} release={} layouts={}] "
            "layout_items_freed={} textures_released={} "
            "cpu_ms[process={:.3f} move_other={:.3f} input={:.3f} controls={:.3f} "
            "text={:.3f} release={:.3f} layouts={:.3f} window_move={:.3f} "
            "external={:.3f} setup={:.3f} video={:.3f} gui={:.3f} fade={:.3f} after={:.3f} "
            "composite={:.3f} end={:.3f} present={:.3f}] "
            "thread_cpu_ms[input={:.3f} controls={:.3f} text={:.3f} "
            "release={:.3f} layouts={:.3f} window_move={:.3f} gui={:.3f} fade={:.3f} "
            "present={:.3f}] sched_valid={} core={}->{} migrations={} "
            "sched_ms[stage=wall/cpu/runq/voluntary/involuntary:{}]",
            r.id, Start(r.wallClock),
            r.hasInput ? std::to_string(r.sinceInput / 1000000) : std::string("none"), r.moving,
            r.budget, CPUWork(r) / MS, r.thread / MS, r.wall / MS, r.fadeVisits, r.fadeBlends,
            r.fadeControlCount, r.fadeLayers, e(Stage::Items), e(Stage::Info), e(Stage::Text),
            e(Stage::Load), e(Stage::Release), e(Stage::Layouts), r.layoutsFreed,
            r.texturesReleased, c(Stage::Process), c(Stage::FrameMove),
            c(Stage::Input), c(Stage::Controls), c(Stage::Text),
            c(Stage::Release), c(Stage::Layouts), c(Stage::WindowMove),
            c(Stage::External), c(Stage::Setup), c(Stage::Video), c(Stage::Gui), c(Stage::Fade),
            c(Stage::AfterRender), c(Stage::Composite), c(Stage::EndRender), c(Stage::Present),
            t(Stage::Input), t(Stage::Controls), t(Stage::Text),
            t(Stage::Release), t(Stage::Layouts), t(Stage::WindowMove),
            t(Stage::Gui), t(Stage::Fade), t(Stage::Present), r.schedValid, r.coreStart, r.coreEnd,
            r.migrations, SchedText(r));
  s.lastLog = now;
  ResetWindow();
}
void EndQuery()
{
  if (s.queryActive)
  {
    if (s.context == eglGetCurrentContext() && s.generation == epoch.load())
      s.api.end(GL_TIME_ELAPSED_EXT);
    else if (s.slot)
      s.slot->valid = false;
    s.queryActive = false;
  }
}
void BeginQuery()
{
  if (!s.slot || !s.slot->valid || !GPUStage(s.stage))
    return;
  if (s.context != eglGetCurrentContext() || s.generation != epoch.load())
  {
    s.slot->valid = false;
    return;
  }
  if (s.slot->used == SEGMENTS)
  {
    s.slot->valid = false;
    ++s.dropped;
    return;
  }
  GLint other = 0;
  s.api.query(GL_TIME_ELAPSED_EXT, GL_CURRENT_QUERY_EXT, &other);
  if (other)
  {
    s.slot->valid = false;
    ++s.dropped;
    return;
  }
  auto& part = s.slot->segments[s.slot->used++];
  part.stage = Stage::Setup; // Whole render envelope; no per-draw GPU attribution.
  s.api.begin(GL_TIME_ELAPSED_EXT, part.query);
  s.queryActive = true;
}
void Forget()
{
  EndQuery();
  for (auto& slot : s.slots)
    if (slot.pending && slot.report)
    {
      slot.record.gpuValid = false;
      Report(slot.record);
    }
  s.slots = {};
  s.slot = nullptr;
  s.context = EGL_NO_CONTEXT;
  s.initialized = false;
  s.api = {};
}
void Poll()
{
  GLint disjoint = 0;
  glGetIntegerv(GL_GPU_DISJOINT_EXT, &disjoint);
  if (disjoint)
  {
    ++s.disjoint;
    for (auto& slot : s.slots)
      slot.valid = false;
  }
  std::array<bool, SLOTS> ready{};
  for (size_t n = 0; n < SLOTS; ++n)
  {
    const auto& slot = s.slots[n];
    if (!slot.pending)
      continue;
    ready[n] = true;
    for (size_t i = 0; i < slot.used; ++i)
    {
      GLuint available = 0;
      s.api.available(slot.segments[i].query, GL_QUERY_RESULT_AVAILABLE_EXT, &available);
      if (!available)
      {
        ready[n] = false;
        break;
      }
    }
  }
  // A query can complete while availability is being polled. Check again
  // before publishing any ready result so clock changes during that window
  // cannot turn an invalid interval into an apparently valid measurement.
  glGetIntegerv(GL_GPU_DISJOINT_EXT, &disjoint);
  if (disjoint)
  {
    ++s.disjoint;
    for (auto& slot : s.slots)
      slot.valid = false;
  }
  for (size_t n = 0; n < SLOTS; ++n)
  {
    auto& slot = s.slots[n];
    if (!ready[n])
      continue; // Never fetch an unavailable result or wait for the GPU.
    bool plausible = true;
    for (size_t i = 0; i < slot.used; ++i)
    {
      GLuint64 elapsed = 0;
      s.api.result(slot.segments[i].query, GL_QUERY_RESULT_EXT, &elapsed);
      if (elapsed > MAX_GPU_ELAPSED)
        plausible = false;
      else
        slot.record.gpu[Index(slot.segments[i].stage)] += elapsed;
    }
    if (!plausible)
    {
      ++s.invalidGPU;
      slot.valid = false;
      slot.record.gpu = {};
    }
    slot.record.gpuValid = slot.valid && slot.used > 0;
    if (slot.report)
      Report(slot.record);
    slot.pending = false;
  }
}
} // namespace

Frame::Frame(bool enabled, double fps)
{
  if (s.active)
    return;
  if (!enabled)
  {
    if (s.captureEnabled)
    {
      // Keep outstanding names reserved, but discard their old-session data.
      // No query polling, deletion or GPU synchronization is needed to stop.
      for (auto& slot : s.slots)
        slot.report = false;
      s.captureEnabled = false;
      ResetWindow();
      s.lastLog = s.lastPresent = 0;
      s.previousPresented = s.previousMoving = false;
      s.worst = {};
    }
    return;
  }
  if (!s.captureEnabled)
  {
    s.captureEnabled = true;
    pendingInput.store(0); // Inputs before the capture are not measured.
    // Jobs finished before the capture are not reported; running jobs count
    // from now on.
    {
      std::lock_guard<std::mutex> lock(jobMutex);
      captureBegin = Now();
      jobDone = jobBusy = {};
    }
    CLog::Log(LOGINFO, "GUI timing: debug capture started");
  }
  m_owner = true;
  s.active = true;
  s.stage = Stage::Idle;
  s.attemptedGPU = false;
  s.current = {};
  s.current.id = ++s.nextId;
  s.current.begin = s.stageBegin = Now();
  s.current.threadBegin = s.stageThread = ThreadNow();
  s.current.wallClock = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
  const uint64_t input = lastInput.load();
  s.current.hasInput = input && input <= s.current.begin;
  s.current.sinceInput = s.current.hasInput ? s.current.begin - input : 0;
  s.current.budget = std::isfinite(fps) && fps >= 1 && fps <= 1000 ? 1000.0 / fps : 1000.0 / 60;
  for (size_t i = 0; i < JOBS; ++i)
  {
    s.current.jobStartsAtBegin[i] = jobStarts[i].load();
    s.current.jobRunningAtBegin[i] = jobActive[i].load() > 0;
  }
  s.topStage = Stage::Idle;
  s.lastSched = ReadSched();
  s.current.coreStart = s.current.coreEnd = s.lastSched.cpu;
}
Frame::~Frame()
{
  if (!m_owner)
    return;
  SetStage(Stage::Idle);
  s.current.wall = Now() - s.current.begin;
  const auto thread = ThreadNow();
  s.current.thread = thread >= s.current.threadBegin ? thread - s.current.threadBegin : 0;
  s.previousPresented = s.current.presented;
  s.previousMoving = s.current.moving;
  if (s.slot)
  {
    s.slot->record = s.current;
    s.slot->pending = true;
    s.slot = nullptr;
  }
  else
    Report(s.current);
  s.active = false;
}
Stage SetStage(Stage stage)
{
  if (!s.active)
    return Stage::Idle;
  if (stage == s.stage)
    return stage;
  // Sub-stages split control processing only. Inside rendering they would
  // break the GPU envelope into many segments, so they are ignored there.
  if (SubStage(stage) && GPUStage(s.stage))
    return s.stage;
  // Mali can complete all draws of a render pass together. Keep one GPU
  // envelope across CPU stages instead of adding overlapping draw estimates.
  if (!GPUStage(stage))
    EndQuery();
  const auto now = Now();
  const auto thread = ThreadNow();
  const auto previous = s.stage;
  s.current.cpu[Index(previous)] += now - s.stageBegin;
  if (thread >= s.stageThread)
    s.current.threadCpu[Index(previous)] += thread - s.stageThread;
  s.stage = stage;
  s.stageBegin = now;
  s.stageThread = thread;
  ++s.current.entries[Index(stage)];
  // Scheduler counters are sampled only between top-level stages: returning
  // from a nested part to its stage is not a boundary.
  if (!SubStage(stage) && stage != s.topStage)
  {
    SampleSched();
    s.topStage = stage;
  }
  if (!s.queryActive)
    BeginQuery();
  return previous;
}
void FadeLayers(const void* control, unsigned int layers)
{
  if (!s.active)
    return;
  ++s.current.fadeVisits;
  s.current.fadeLayers = std::max(s.current.fadeLayers, layers);
  auto& controls = s.current.fadeControls;
  const auto end = controls.begin() + std::min<size_t>(s.current.fadeControlCount, FADE_CONTROLS);
  if (std::find(controls.begin(), end, control) == end)
  {
    if (s.current.fadeControlCount < FADE_CONTROLS)
      controls[s.current.fadeControlCount] = control;
    ++s.current.fadeControlCount;
  }
}
void TextureDrawn(const void* texture)
{
  if (!s.inFade)
    return;
  const auto end = s.fadeImages.begin() + std::min<size_t>(s.fadeImageCount, s.fadeImages.size());
  if (std::find(s.fadeImages.begin(), end, texture) != end)
    return;
  if (s.fadeImageCount < s.fadeImages.size())
    s.fadeImages[s.fadeImageCount] = texture;
  ++s.fadeImageCount;
}
FadeDraws::FadeDraws(bool enabled) : m_enabled(enabled && s.active)
{
  if (!m_enabled)
    return;
  s.inFade = true;
  s.fadeImageCount = 0;
}
FadeDraws::~FadeDraws()
{
  if (!m_enabled)
    return;
  s.inFade = false;
  if (s.active && s.fadeImageCount >= 2)
    ++s.current.fadeBlends;
}
void Presented(bool swapped)
{
  if (!s.active)
    return;
  // A skipped swap, possibly followed by an idle sleep, leaves the frame
  // unpresented, which breaks continuity: the next swap is not late to it.
  if (!swapped)
    return;
  // A swap more than half a frame late. Only consecutive swapping frames
  // count, so idle periods without rendering are not late.
  const auto now = Now();
  std::array<bool, 3> with{s.current.fadeBlends > 0, false, false};
  for (size_t i = 0; i < JOBS; ++i)
    with[1 + i] = s.current.jobRunningAtBegin[i] || jobActive[i].load() > 0 ||
                  jobStarts[i].load() != s.current.jobStartsAtBegin[i];
  for (size_t i = 0; i < with.size(); ++i)
    s.presentsWith[i] += with[i];
  // The first accepted swap after an input answers it, also after an idle
  // period, which the late count above cannot see.
  const uint64_t input = pendingInput.exchange(0);
  if (input && input <= now)
  {
    const uint64_t latency = now - input;
    if (latency > STALE_INPUT)
      ++s.staleInput;
    else
    {
      const double ms = latency / MS;
      ++s.inputToPresent[ms < 20 ? 0 : ms < 34 ? 1 : ms < 50 ? 2 : ms < 100 ? 3 : 4];
      s.inputToPresentMax = std::max(s.inputToPresentMax, latency);
    }
  }
  ++s.presents;
  if (s.previousPresented && s.lastPresent &&
      double(now - s.lastPresent) / MS > 1.5 * s.current.budget)
  {
    ++s.late;
    if (s.current.moving || s.previousMoving)
      ++s.lateMoving;
    for (size_t i = 0; i < with.size(); ++i)
      s.lateWith[i] += with[i];
    s.lateWithNone += !with[0] && !with[1] && !with[2];
    const double since = s.current.sinceInput / MS;
    ++s.lateAfterInput[!s.current.hasInput ? 4
                       : since < 100        ? 0
                       : since < 300        ? 1
                       : since < 600        ? 2
                                            : 3];
  }
  s.lastPresent = now;
  s.current.presented = true;
}
void LayoutsFreed(unsigned int items)
{
  if (s.active)
    s.current.layoutsFreed += items;
}
void TextureReleased()
{
  if (s.active)
    ++s.current.texturesReleased;
}
void Count(Stage stage)
{
  if (s.active)
    ++s.current.entries[Index(stage)];
}
uint64_t JobBegin(Job job)
{
  const size_t i = static_cast<size_t>(job);
  ++jobStarts[i];
  ++jobActive[i];
  return Now();
}
void JobEnd(Job job, uint64_t begin)
{
  const size_t i = static_cast<size_t>(job);
  const uint64_t now = Now();
  --jobActive[i];
  std::lock_guard<std::mutex> lock(jobMutex);
  const uint64_t from = std::max(begin, captureBegin);
  if (const auto hook = jobEndHook.load())
    hook();
  if (now < from)
    return; // Finished before the current capture started.
  jobBusy[i] += now - from;
  ++jobDone[i];
}
void Crossfade(CrossfadePath path)
{
  if (s.active && path < CrossfadePath::Count)
    ++s.crossfade[static_cast<size_t>(path)];
}
void Input()
{
  const uint64_t now = Now();
  lastInput.store(now);
  // Keep the earliest input that is still waiting for a swap.
  uint64_t none = 0;
  pendingInput.compare_exchange_strong(none, now);
  // Input handled during a frame is that frame's newest input.
  if (s.active)
  {
    s.current.hasInput = true;
    s.current.sinceInput = 0;
  }
}
void Motion()
{
  if (s.active)
    s.current.moving = true;
}
bool Active()
{
  return s.active;
}
void AttachGPU()
{
  if (!s.active || s.attemptedGPU)
    return;
  s.attemptedGPU = true;
  const EGLContext context = eglGetCurrentContext();
  if (context == EGL_NO_CONTEXT)
    return;
  if (s.context != context || s.generation != epoch.load())
    Forget();
  s.context = context;
  observedContext.store(context);
  s.generation = epoch.load();
  if (!s.initialized)
  {
    s.initialized = true;
    const char* ext = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
    const bool advertised = ext && std::strstr(ext, "GL_EXT_disjoint_timer_query");
#define LOAD(member, type, name) s.api.member = reinterpret_cast<type>(eglGetProcAddress(name))
    if (advertised)
    {
      LOAD(gen, PFNGLGENQUERIESEXTPROC, "glGenQueriesEXT");
      LOAD(del, PFNGLDELETEQUERIESEXTPROC, "glDeleteQueriesEXT");
      LOAD(begin, PFNGLBEGINQUERYEXTPROC, "glBeginQueryEXT");
      LOAD(end, PFNGLENDQUERYEXTPROC, "glEndQueryEXT");
      LOAD(query, PFNGLGETQUERYIVEXTPROC, "glGetQueryivEXT");
      LOAD(available, PFNGLGETQUERYOBJECTUIVEXTPROC, "glGetQueryObjectuivEXT");
      LOAD(result, PFNGLGETQUERYOBJECTUI64VEXTPROC, "glGetQueryObjectui64vEXT");
    }
#undef LOAD
    GLint bits = 0;
    if (s.api.query)
      s.api.query(GL_TIME_ELAPSED_EXT, GL_QUERY_COUNTER_BITS_EXT, &bits);
    s.api.supported = s.api.gen && s.api.del && s.api.begin && s.api.end && s.api.query &&
                      s.api.available && s.api.result && bits > 0;
    if (s.api.supported)
    {
      std::array<GLuint, SLOTS * SEGMENTS> names{};
      s.api.gen(names.size(), names.data());
      size_t n = 0;
      for (auto& slot : s.slots)
        for (auto& part : slot.segments)
          part.query = names[n++];
      s.api.supported = std::all_of(names.begin(), names.end(), [](GLuint id) { return id != 0; });
    }
    CLog::Log(LOGINFO,
              "GUI timing: enabled gpu_queries={} elapsed_bits={} ring_frames={} "
              "segments_per_frame={} results=deferred gpu_mode=render_envelope "
              "cpu_stage_times=exclusive gpu_stage_attribution=false scanout_measured=false",
              s.api.supported, bits, SLOTS, SEGMENTS);
  }
  if (!s.api.supported)
    return;
  Poll();
  for (auto& slot : s.slots)
    if (!slot.pending)
    {
      s.slot = &slot;
      slot.record = {};
      slot.used = 0;
      slot.valid = true;
      slot.report = true;
      BeginQuery();
      return;
    }
  ++s.dropped;
}
void ReleaseContext(void* context)
{
  if (context == EGL_NO_CONTEXT || observedContext.load() != context)
    return;
  ++epoch;
  if (s.context != context)
    return;
  // End only the query owned by this context; never touch a different current one.
  if (s.queryActive && eglGetCurrentContext() == context)
  {
    s.api.end(GL_TIME_ELAPSED_EXT);
    s.queryActive = false;
  }
  if (s.captureEnabled)
    ++s.aborted;
  if (s.api.del && eglGetCurrentContext() == context)
    for (const auto& slot : s.slots)
      for (const auto& part : slot.segments)
        if (part.query)
          s.api.del(1, &part.query);
  Forget(); // Context-local queries otherwise die with EGL; do not reuse their names.
}
} // namespace GUI_RENDER_TIMING
