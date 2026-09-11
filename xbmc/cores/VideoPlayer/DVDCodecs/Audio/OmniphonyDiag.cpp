/*
 *  Copyright (C) 2026-present Team CoreELEC (https://coreelec.org)
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

// TEMPORARY instrumentation - see OmniphonyDiag.h.

#include "OmniphonyDiag.h"

#include "ServiceBroker.h"
#include "threads/Thread.h"
#include "utils/StringUtils.h"
#include "utils/log.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <dirent.h>
#include <unistd.h>

namespace
{
using Clock = std::chrono::steady_clock;

//! \brief How often the renderer and feed lines are emitted.
constexpr int DIAG_TICK_MS = 250;

//! \brief Ticks between the CPU, thread and poll lines. One second.
constexpr int DIAG_SLOW_EVERY = 4;

//! \brief Ticks in the rolling delivery-rate window. Ten seconds.
constexpr size_t DIAG_ROLL_TICKS = 40;

//! \brief Threads listed on the thread line, busiest first.
constexpr size_t DIAG_TOP_THREADS = 6;

uint64_t NowUs()
{
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch())
          .count());
}

std::string ReadFirstLine(const std::string& path)
{
  std::ifstream file(path);
  std::string line;
  if (file)
    std::getline(file, line);
  return line;
}

/*!
 * \brief One thread's line out of /proc/<pid>/task/<tid>/stat.
 *
 * Field numbering is the one this box's kernel emits in
 * fs/proc/array.c do_task_stat: 14 utime, 15 stime, 19 nice, 39 the CPU the
 * task was last on. The comm in field 2 may contain spaces and brackets, so
 * everything is counted from the last ')' - after which the first token is
 * field 3.
 */
struct TaskStat
{
  std::string comm;
  uint64_t ticks{0}; //!< utime + stime
  int cpu{-1};
  int nice{0};
};

bool ReadTaskStat(const std::string& path, TaskStat& out)
{
  const std::string line = ReadFirstLine(path);
  const size_t open = line.find('(');
  const size_t close = line.rfind(')');
  if (open == std::string::npos || close == std::string::npos || close < open ||
      close + 2 >= line.size())
    return false;

  out.comm = line.substr(open + 1, close - open - 1);

  std::vector<std::string> f = StringUtils::Split(line.substr(close + 2), ' ');
  // f[0] is field 3, so field N sits at f[N - 3].
  if (f.size() < 37)
    return false;

  out.ticks = std::strtoull(f[11].c_str(), nullptr, 10) + // utime
              std::strtoull(f[12].c_str(), nullptr, 10); // stime
  out.nice = std::atoi(f[16].c_str());
  out.cpu = std::atoi(f[36].c_str());
  return true;
}

//! \brief Every thread under one /proc task directory, keyed by tid.
void ReadTasks(const std::string& dir, std::map<int, TaskStat>& out)
{
  DIR* d = opendir(dir.c_str());
  if (!d)
    return;

  while (const dirent* e = readdir(d))
  {
    if (e->d_name[0] < '0' || e->d_name[0] > '9')
      continue;
    TaskStat st;
    if (ReadTaskStat(dir + "/" + e->d_name + "/stat", st))
      out[std::atoi(e->d_name)] = st;
  }
  closedir(d);
}

void ReadOwnTasks(std::map<int, TaskStat>& out)
{
  ReadTasks("/proc/self/task", out);
}

/*!
 * \brief The helper's threads, if there is a helper.
 *
 * No pid, or one that resolves to us, means there is nothing separate to read -
 * a passthrough film has no helper at all. Reading /proc/self here instead
 * would list every Kodi thread a second time and call them the helper's.
 */
void ReadHelperTasks(pid_t pid, std::map<int, TaskStat>& out)
{
  if (pid <= 0 || pid == getpid())
    return;
  ReadTasks(StringUtils::Format("/proc/{}/task", pid), out);
}

struct CpuTime
{
  uint64_t busy{0};
  uint64_t total{0};
};

//! \brief Per-core busy and total jiffies from /proc/stat, cpu0 first.
void ReadCpuTimes(std::vector<CpuTime>& out)
{
  out.clear();
  std::ifstream file("/proc/stat");
  std::string line;
  while (std::getline(file, line))
  {
    if (line.compare(0, 3, "cpu") != 0 || line[3] < '0' || line[3] > '9')
      continue;

    std::istringstream in(line);
    std::string name;
    in >> name;

    CpuTime t;
    uint64_t v = 0;
    for (int i = 0; in >> v; ++i)
    {
      t.total += v;
      // user, nice, system, idle, iowait, irq, softirq, steal - 3 and 4 are the
      // two the core was not doing our work in.
      if (i != 3 && i != 4)
        t.busy += v;
    }
    out.push_back(t);
  }
}

//! \brief Cortex part number out of the per-core MIDR, as a short label.
std::string CoreLabel(int cpu)
{
  const std::string midr = ReadFirstLine(
      StringUtils::Format("/sys/devices/system/cpu/cpu{}/regs/identification/midr_el1", cpu));
  if (midr.empty())
    return {};

  const unsigned long long v = std::strtoull(midr.c_str(), nullptr, 0);
  switch ((v >> 4) & 0xfff)
  {
    case 0xd03:
      return "A53";
    case 0xd09:
      return "A73";
    default:
      return {};
  }
}

int ReadKhz(int cpu)
{
  const std::string s = ReadFirstLine(
      StringUtils::Format("/sys/devices/system/cpu/cpu{}/cpufreq/scaling_cur_freq", cpu));
  return s.empty() ? -1 : std::atoi(s.c_str());
}

double ReadTempC()
{
  const std::string s = ReadFirstLine("/sys/class/thermal/thermal_zone0/temp");
  return s.empty() ? -1.0 : std::atoi(s.c_str()) / 1000.0;
}

//! \brief Frames as milliseconds of audio, for a rate that may not be set yet.
int64_t FramesToMs(int64_t frames, uint32_t rate)
{
  return rate ? frames * 1000 / static_cast<int64_t>(rate) : 0;
}

/*!
 * \brief How long ago an event stamp was.
 *
 * The caller loads the stamp and this takes the clock afterwards, so the stamp
 * cannot be from the future however recently the producing thread moved it.
 * Taking "now" first - which an earlier draft did, once per tick, before a
 * /proc walk that costs tens of milliseconds - let the subtraction wrap and
 * print ages of eighteen quintillion milliseconds.
 */
uint64_t AgeUs(uint64_t stampUs)
{
  if (!stampUs)
    return 0;
  const uint64_t now = NowUs();
  return now > stampUs ? now - stampUs : 0;
}

//! \brief \ref AgeUs as text, saying "never" rather than "0ms" for no event.
std::string AgeText(uint64_t stampUs)
{
  return stampUs ? StringUtils::Format("{}ms", AgeUs(stampUs) / 1000) : std::string("never");
}
} // namespace

//==============================================================================
// CSampler
//==============================================================================

class COmniphonyDiag::CSampler : public CThread
{
public:
  explicit CSampler(COmniphonyDiag& owner) : CThread("OmniDiag"), m_owner(owner) {}
  ~CSampler() override { StopThread(true); }

private:
  void Process() override;
  void Rebase(uint64_t now);
  void EmitRender(uint64_t now, uint64_t elapsedUs);
  void EmitFeed();
  void EmitCpu();
  void EmitThreads(uint64_t elapsedUs);
  void EmitPoll(uint64_t elapsedUs);

  COmniphonyDiag& m_owner;

  uint64_t m_lastRendered{0};
  uint64_t m_lastHanded{0};

  //! \brief Frames and time this run's average is measured from - see Rebase.
  uint64_t m_baseRendered{0};
  uint64_t m_baseUs{0};

  /*!
   * \brief One tick of the rolling rate: what arrived, and over how long.
   *
   * The elapsed time is stored rather than assumed to be DIAG_TICK_MS. A tick
   * that ran late - which is the very thing under investigation - would
   * otherwise divide real frames by a nominal window and read as a renderer
   * going faster than real time.
   */
  struct RollTick
  {
    uint64_t frames{0};
    uint64_t us{0};
  };
  std::array<RollTick, DIAG_ROLL_TICKS> m_roll{};
  size_t m_rollAt{0};
  size_t m_rollUsed{0};

  //! \brief The tick a rebase landed in covers only part of itself - see Rebase.
  bool m_rollSkip{false};

  //! \brief Rendered frames at the last slow tick, for the capacity figure.
  uint64_t m_slowRendered{0};

  std::vector<CpuTime> m_lastCpu;
  std::vector<std::string> m_coreLabels;
  std::map<int, TaskStat> m_lastTasks;
  std::map<int, TaskStat> m_lastHelperTasks;
  long m_hz{100};

  //! \brief Helper CPU over the last slow tick, carried to the thread line.
  double m_helperPct{0.0};
};

void COmniphonyDiag::CSampler::Process()
{
  m_hz = sysconf(_SC_CLK_TCK);
  if (m_hz <= 0)
    m_hz = 100;

  ReadCpuTimes(m_lastCpu);
  for (size_t i = 0; i < m_lastCpu.size(); ++i)
    m_coreLabels.push_back(CoreLabel(static_cast<int>(i)));

  ReadOwnTasks(m_lastTasks);
  ReadHelperTasks(m_owner.m_helperPid.load(), m_lastHelperTasks);

  // After the /proc reads above, not before: the frame baseline and the time
  // baseline have to be taken at the same instant or the average counts frames
  // from a window it does not measure.
  uint64_t last = NowUs();
  uint64_t slowStart = last;
  Rebase(last);
  int tick = 0;

  while (!m_bStop)
  {
    CThread::Sleep(std::chrono::milliseconds(DIAG_TICK_MS));
    if (m_bStop)
      break;

    const uint64_t now = NowUs();
    if (m_owner.m_rebase.exchange(false))
      Rebase(now);

    // Only while a renderer run is attached: on a passthrough film these two
    // would be a steady stream of zeroes saying nothing, and that film is the
    // control the CPU and poll lines below are there to measure.
    if (m_owner.m_run.load())
    {
      EmitRender(now, now - last);
      EmitFeed();
    }
    last = now;

    if (++tick % DIAG_SLOW_EVERY == 0)
    {
      EmitCpu();
      EmitThreads(now - slowStart);
      EmitPoll(now - slowStart);
      slowStart = now;
    }
  }
}

void COmniphonyDiag::CSampler::Rebase(uint64_t now)
{
  m_baseRendered = m_owner.m_renderedFrames.load();
  m_baseUs = now;
  m_lastRendered = m_baseRendered;
  m_lastHanded = m_owner.m_handedFrames.load();
  m_slowRendered = m_baseRendered;
  m_roll.fill(RollTick{});
  m_rollAt = 0;
  m_rollUsed = 0;

  // The tick this landed in has already spent part of itself against the old
  // baseline, so its frame count covers less than its elapsed time. Stored, it
  // would hold the rolling rate down for the whole ten seconds after every
  // seek; it is dropped and the window starts at the next whole tick.
  m_rollSkip = true;
}

void COmniphonyDiag::CSampler::EmitRender(uint64_t now, uint64_t elapsedUs)
{
  const uint64_t rendered = m_owner.m_renderedFrames.load();
  const uint64_t handed = m_owner.m_handedFrames.load();

  /*
   * A counter that went backwards is AttachHelper having zeroed it since the
   * rebase flag was read at the top of this tick - a replacement codec opening
   * while this one is still winding down. Start again rather than subtract:
   * the subtraction wraps, and the tick that did it reported eighteen
   * quintillion frames.
   */
  if (rendered < m_lastRendered || rendered < m_baseRendered || handed < m_lastHanded)
  {
    Rebase(now);
    return;
  }

  const uint64_t outFrames = rendered - m_lastRendered;
  const uint64_t handedFrames = handed - m_lastHanded;
  m_lastRendered = rendered;
  m_lastHanded = handed;

  if (m_rollSkip)
  {
    m_rollSkip = false;
  }
  else
  {
    m_roll[m_rollAt] = {outFrames, elapsedUs};
    m_rollAt = (m_rollAt + 1) % DIAG_ROLL_TICKS;
    if (m_rollUsed < DIAG_ROLL_TICKS)
      ++m_rollUsed;
  }

  uint64_t rollFrames = 0;
  uint64_t rollUs = 0;
  for (size_t i = 0; i < m_rollUsed; ++i)
  {
    rollFrames += m_roll[i].frames;
    rollUs += m_roll[i].us;
  }

  const uint32_t rate = m_owner.m_rate.load();
  const int64_t pump = static_cast<int64_t>(m_owner.m_pumpHeldFrames.load());
  const int64_t codec = m_owner.m_codecHeldFrames.load();

  const double tickExpected = static_cast<double>(rate) * elapsedUs / 1e6;
  const double rollExpected = static_cast<double>(rate) * rollUs / 1e6;
  const double sinceBase =
      now > m_baseUs ? static_cast<double>(rate) * (now - m_baseUs) / 1e6 : 0.0;

  // An in-progress stall never reaches the completed-gap maximum, so the age of
  // the last block is reported directly and at this tick rate rather than the
  // slow one. "never" rather than zero when none has arrived at all: zero here
  // would read as one having just been delivered, which is the opposite.
  const std::string idle = AgeText(m_owner.m_lastBlockUs.load());

  CLog::Log(LOGDEBUG,
            "OmniDiag rend: pcm={}ms (pump {} + codec {}) inq={}B out={}fr/{}ms {:.2f}x "
            "(10s {:.2f}x, run {:.2f}x) idle={} handed={}fr",
            FramesToMs(pump + codec, rate), FramesToMs(pump, rate), FramesToMs(codec, rate),
            m_owner.m_queuedBytes.load(), outFrames, elapsedUs / 1000,
            tickExpected > 0 ? outFrames / tickExpected : 0.0,
            rollExpected > 0 ? rollFrames / rollExpected : 0.0,
            sinceBase > 0 ? (rendered - m_baseRendered) / sinceBase : 0.0, idle, handedFrames);
}

void COmniphonyDiag::CSampler::EmitFeed()
{
  const std::string since = AgeText(m_owner.m_lastFeedUs.load());

  CLog::Log(LOGDEBUG,
            "OmniDiag feed: au={} bytes={} refused bank={} queue={} waited={}ms lastfeed={}",
            m_owner.m_fedUnits.load(), m_owner.m_fedBytes.load(), m_owner.m_refusedBank.load(),
            m_owner.m_refusedQueue.load(), m_owner.m_feedWaitUs.load() / 1000, since);
}

void COmniphonyDiag::CSampler::EmitCpu()
{
  std::vector<CpuTime> now;
  ReadCpuTimes(now);

  std::string line;
  for (size_t i = 0; i < now.size() && i < m_lastCpu.size(); ++i)
  {
    // Guarded like every other delta here: these are monotonic in practice, but
    // a core going offline and back is not worth a nine-digit percentage.
    const uint64_t total =
        now[i].total >= m_lastCpu[i].total ? now[i].total - m_lastCpu[i].total : 0;
    const uint64_t busy = now[i].busy >= m_lastCpu[i].busy ? now[i].busy - m_lastCpu[i].busy : 0;
    const int khz = ReadKhz(static_cast<int>(i));
    const std::string label = i < m_coreLabels.size() ? m_coreLabels[i] : std::string();

    line += StringUtils::Format("{}cpu{}{} {}%", i ? " | " : "", i,
                                label.empty() ? std::string() : "(" + label + ")",
                                total ? static_cast<int>(busy * 100 / total) : 0);
    if (khz > 0)
      line += StringUtils::Format(" {}MHz", khz / 1000);
  }
  m_lastCpu = now;

  CLog::Log(LOGDEBUG, "OmniDiag  cpu: {} | temp={:.1f}C", line, ReadTempC());
}

void COmniphonyDiag::CSampler::EmitThreads(uint64_t elapsedUs)
{
  const pid_t helper = m_owner.m_helperPid.load();

  std::map<int, TaskStat> mine;
  std::map<int, TaskStat> theirs;
  ReadOwnTasks(mine);
  ReadHelperTasks(helper, theirs);

  // Percent of one core, so a saturated core reads 100 whatever else is running.
  const double oneCore = static_cast<double>(m_hz) * elapsedUs / 1e6;

  struct Row
  {
    double pct;
    std::string text;
  };
  std::vector<Row> rows;

  auto collect = [&](const std::map<int, TaskStat>& now, const std::map<int, TaskStat>& before)
  {
    for (const auto& [tid, st] : now)
    {
      const auto prev = before.find(tid);
      const uint64_t delta = (prev == before.end() || st.ticks < prev->second.ticks)
                                 ? 0
                                 : st.ticks - prev->second.ticks;
      if (!delta)
        continue;
      const double pct = oneCore > 0 ? delta * 100.0 / oneCore : 0.0;
      rows.push_back({pct, StringUtils::Format("{}/{} {:.0f}%@cpu{} ni{}", st.comm, tid, pct,
                                               st.cpu, st.nice)});
    }
  };
  collect(mine, m_lastTasks);
  collect(theirs, m_lastHelperTasks);

  // The helper as a whole, because its work may be spread over its own threads
  // and no single one of them would make the list.
  double helperCore = 0.0;
  for (const auto& [tid, st] : theirs)
  {
    const auto prev = m_lastHelperTasks.find(tid);
    if (prev != m_lastHelperTasks.end() && st.ticks >= prev->second.ticks)
      helperCore += static_cast<double>(st.ticks - prev->second.ticks);
  }
  m_helperPct = oneCore > 0 ? helperCore * 100.0 / oneCore : 0.0;

  m_lastTasks = std::move(mine);
  m_lastHelperTasks = std::move(theirs);

  std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.pct > b.pct; });

  std::string line;
  for (size_t i = 0; i < rows.size() && i < DIAG_TOP_THREADS; ++i)
    line += (i ? " | " : "") + rows[i].text;

  /*
   * Audio-seconds produced per CPU-second the helper actually spent. This is
   * the capacity figure the delivery rate is not: a helper fed only at playback
   * speed delivers 1.00x however fast it could go, but its cost per second of
   * audio does not change. Only meaningful while it has work - an idle helper
   * spends nothing and reads as infinite, which is why it is suppressed below
   * a tenth of a core.
   */
  const uint64_t rendered = m_owner.m_renderedFrames.load();
  const uint64_t made = rendered >= m_slowRendered ? rendered - m_slowRendered : 0;
  m_slowRendered = rendered;

  const uint32_t rate = m_owner.m_rate.load();
  const double cpuSecs = m_helperPct / 100.0 * elapsedUs / 1e6;
  const double audioSecs = rate ? static_cast<double>(made) / rate : 0.0;

  // Both halves must be real. CPU without audio is a helper doing something
  // other than rendering, and dividing by it produced nine-digit nonsense the
  // first time this was run.
  const std::string cap = (cpuSecs > 0.01 && audioSecs > 0.0)
                              ? StringUtils::Format(" cost={:.2f}c/s cap={:.1f}x",
                                                    cpuSecs / audioSecs, audioSecs / cpuSecs)
                              : std::string();

  CLog::Log(LOGDEBUG, "OmniDiag  thr: {} || helper {} {:.0f}% over {} thr{}", line,
            helper > 0 ? StringUtils::Format("pid={}", helper) : std::string("none"), m_helperPct,
            m_lastHelperTasks.size(), cap);
}

void COmniphonyDiag::CSampler::EmitPoll(uint64_t elapsedUs)
{
  const uint64_t calls = m_owner.m_pollCalls.exchange(0);
  const uint64_t totalUs = m_owner.m_pollTotalUs.exchange(0);
  const uint64_t maxUs = m_owner.m_pollMaxUs.exchange(0);

  // Folded together so a stall that has not ended yet still shows: the
  // completed maximum alone would report zero through a permanent one.
  const uint64_t gapUs =
      std::max(m_owner.m_blockGapUs.exchange(0), AgeUs(m_owner.m_lastBlockUs.load()));

  CLog::Log(LOGDEBUG,
            "OmniDiag poll: PollFrame calls={} avg={:.1f}ms max={:.1f}ms blocked={}/{}ms || "
            "gap max={}ms",
            calls, calls ? totalUs / 1000.0 / calls : 0.0, maxUs / 1000.0, totalUs / 1000,
            elapsedUs / 1000, gapUs / 1000);
}

//==============================================================================
// COmniphonyDiag
//==============================================================================

COmniphonyDiag& COmniphonyDiag::Get()
{
  static COmniphonyDiag diag;
  return diag;
}

COmniphonyDiag::~COmniphonyDiag() = default;

void COmniphonyDiag::Sync()
{
  const bool want = m_playback || m_run.load() != 0;
  if (want == (m_sampler != nullptr))
    return;

  if (!want)
  {
    m_sampling = false;
    m_sampler.reset();
    CLog::Log(LOGDEBUG, "OmniDiag: sampling stopped");
    return;
  }

  // Nothing is sampled at all when the log would drop it: the /proc walk is
  // the measurement's own cost, and an otherwise identical build should be able
  // to run without it.
  if (!CServiceBroker::GetLogging().IsLogLevelLogged(LOGDEBUG))
    return;

  m_sampler = std::make_unique<CSampler>(*this);
  m_sampler->Create();
  m_sampling = true;
  CLog::Log(LOGDEBUG, "OmniDiag: sampling started");
}

void COmniphonyDiag::StartPlayback()
{
  std::unique_lock<CCriticalSection> lock(m_lifetime);
  if (m_playback)
    return;
  m_playback = true;
  m_pollCalls = 0;
  m_pollTotalUs = 0;
  m_pollMaxUs = 0;
  Sync();
}

void COmniphonyDiag::StopPlayback()
{
  std::unique_lock<CCriticalSection> lock(m_lifetime);
  if (!m_playback)
    return;
  m_playback = false;
  Sync();
}

uint64_t COmniphonyDiag::AttachHelper(pid_t helperPid, unsigned int rate)
{
  std::unique_lock<CCriticalSection> lock(m_lifetime);
  m_helperPid = helperPid;
  m_rate = rate ? rate : 48000;

  m_renderedFrames = 0;
  m_handedFrames = 0;
  m_pumpHeldFrames = 0;
  m_codecHeldFrames = 0;
  m_queuedBytes = 0;
  m_lastBlockUs = 0;
  m_blockGapUs = 0;
  m_fedUnits = 0;
  m_fedBytes = 0;
  m_refusedBank = 0;
  m_refusedQueue = 0;
  m_feedWaitUs = 0;
  m_lastFeedUs = 0;
  m_rebase = true;

  m_run = ++m_nextRun;
  Sync();

  CLog::Log(LOGDEBUG, "OmniDiag: run {} attached, helper pid={} rate={}", m_run.load(), helperPid,
            rate);
  return m_run.load();
}

void COmniphonyDiag::DetachHelper(uint64_t run)
{
  std::unique_lock<CCriticalSection> lock(m_lifetime);

  // A stale token is the codec this replaced going away behind us.
  if (!run || run != m_run.load())
    return;

  m_run = 0;
  m_helperPid = -1;
  CLog::Log(LOGDEBUG, "OmniDiag: run {} detached", run);
  Sync();
}

void COmniphonyDiag::Mark(uint64_t run, const char* why)
{
  if (!run || run != m_run.load())
    return;

  m_rebase = true;
  m_lastBlockUs = 0;
  m_blockGapUs = 0;
  CLog::Log(LOGDEBUG, "OmniDiag: run {} accounting restarted - {}", run, why);
}

void COmniphonyDiag::OnRendered(uint64_t run, uint32_t frames)
{
  if (!run || run != m_run.load())
    return;

  m_renderedFrames += frames;

  const uint64_t now = NowUs();
  const uint64_t last = m_lastBlockUs.exchange(now);
  if (!last)
    return;

  const uint64_t gap = now > last ? now - last : 0;
  uint64_t worst = m_blockGapUs.load();
  while (gap > worst && !m_blockGapUs.compare_exchange_weak(worst, gap))
    ;
}

void COmniphonyDiag::SetPumpHeld(uint64_t run, uint64_t frames)
{
  if (run && run == m_run.load())
    m_pumpHeldFrames = frames;
}

void COmniphonyDiag::SetCodecHeld(uint64_t run, int frames)
{
  if (run && run == m_run.load())
    m_codecHeldFrames = frames;
}

void COmniphonyDiag::SetInputQueue(uint64_t run, uint64_t bytes)
{
  if (run && run == m_run.load())
    m_queuedBytes = bytes;
}

void COmniphonyDiag::OnHandedOut(uint64_t run, uint32_t frames)
{
  if (run && run == m_run.load())
    m_handedFrames += frames;
}

void COmniphonyDiag::OnFed(uint64_t run, size_t bytes)
{
  if (!run || run != m_run.load())
    return;

  m_fedUnits++;
  m_fedBytes += bytes;
  m_lastFeedUs = NowUs();
}

void COmniphonyDiag::OnFeedRefused(uint64_t run, Refusal why)
{
  if (!run || run != m_run.load())
    return;

  if (why == Refusal::BankFull)
    m_refusedBank++;
  else
    m_refusedQueue++;
}

void COmniphonyDiag::OnFeedWait(uint64_t run, uint64_t microseconds)
{
  if (run && run == m_run.load())
    m_feedWaitUs += microseconds;
}

void COmniphonyDiag::OnPollFrame(int64_t microseconds)
{
  if (microseconds < 0 || !m_sampling.load())
    return;

  const uint64_t us = static_cast<uint64_t>(microseconds);
  m_pollCalls++;
  m_pollTotalUs += us;

  uint64_t worst = m_pollMaxUs.load();
  while (us > worst && !m_pollMaxUs.compare_exchange_weak(worst, us))
    ;
}
