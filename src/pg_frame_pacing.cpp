// Uncapped frame rate without speeding up the game's vblank clock.
//
// The game limits itself by waiting for vblanks: its main loop (sub_8247E318) and its swap
// (sub_824853A8) each wait for a presentation interval of vblanks, 1 by default. Without vsync
// the SDK delivers a vblank every millisecond, which the game's Direct3D also needs to retire
// queued swaps (with 60 Hz vblanks gameplay dropped to 30 FPS every few seconds). But the
// engine's vblank counter (sub_8234BD00, called on each vblank) is also a clock: the cutscene
// cameras (sub_82432538, "Cam segment %d ready ...") measure time as vblanks / 60 Hz, so they
// ran many times too fast. Here both intervals are 0, so the game doesn't wait for vblanks, and
// the engine's counter advances at a real 60 Hz, in step with the main loop's frames (see
// EngineVblankClock).
#include "pg_frame_pacing.h"
#include "pg_dev.h"
#include "native/pg_d3d_trace.h"
#include "native/pg_engine_mesh.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

#include <rex/cvar.h>
#include <rex/perf/counter.h>
#include <rex/ppc.h>
#include <rex/ui/present_sync.h>

#if REX_PLATFORM_WIN32
#include <windows.h>
#endif
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <immintrin.h>
#define PG_CPU_RELAX() _mm_pause()
#else
#define PG_CPU_RELAX() std::this_thread::yield()
#endif

REXCVAR_DEFINE_BOOL(uncapped_real_time, true, "GPU",
                    "Without vsync, don't wait for vblanks in the game, and advance its vblank "
                    "counter at the real 60 Hz so cutscenes run at the right speed")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(frame_rate_limit, 60, "Video",
                     "Frame rate limit: 30 is the console's rate (paced by 60 Hz guest "
                     "vblanks), 0 is unlimited. With present_sync=auto, a limit that equals or "
                     "divides the display's refresh rate is presented with vsync and paced on "
                     "its vblanks; others are held by a timer and presented without vsync")
    // 40 and 90 divide 120 Hz screens (handhelds such as the ROG Xbox Ally) evenly, so with
    // present_sync they are shown for exactly 3 and 4 refreshes without VRR; 30 and 60 divide 60,
    // 120 and 240 Hz screens.
    .allowed({"0", "30", "40", "60", "90", "120", "144", "165", "240"});
REXCVAR_DEFINE_STRING(present_sync, "auto", "Video",
                      "Present with vsync: auto when the frame rate limit equals or divides the "
                      "display's refresh rate (no tearing, even frame times), otherwise without "
                      "(for variable refresh rate); on always; off never")
    .allowed({"auto", "on", "off"})
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(swap_waits_for_new_frame, true, "Video",
                    "After a swap, the render thread waits for the game's next frame instead of "
                    "presenting the last one again")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(cutscene_clock_follows_frames, true, "GPU",
                    "Without vsync, advance the engine's 60 Hz vblank counter (the cutscene "
                    "cameras' clock) at the main loop's frames, so each frame at 60 FPS sees "
                    "one step, rather than at real 1/60 s boundaries that frames cross at "
                    "random phase")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(pacing_trace_csv, "", "Perf",
                      "Write one line per swap (host time, main loop frames, engine time) to "
                      "this CSV, inside the measurement window of an input replay")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace {

// Script integer with the main loop's presentation interval, read through sub_823608C0.
constexpr uint32_t kMainLoopIntervalChecksum = 0xE8DE9454;
// Presentation interval of the swap, in the game's data (1 in the image).
constexpr uint32_t kSwapIntervalAddress = 0x828D400C;
// The engine's frame time in seconds (float), averaged over the last four frames
// (written by sub_8234C108, read by sub_8234BE38).
constexpr uint32_t kFrameDeltaAddress = 0x82A1D0B4;

float LoadGuestFloat(const uint8_t* base, uint32_t address) {
  uint32_t bits;
  std::memcpy(&bits, base + address, sizeof(bits));
  bits = __builtin_bswap32(bits);
  float value;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

// The GPU plugin's vsync setting. Uncapped() runs on every vblank (1000 times a second without
// vsync) and in the swap, and a lookup by name takes the cvar registry's lock and parses a
// string, so it is read once per swap and when the frame rate limit changes.
std::atomic<bool> g_vsync{false};

void RefreshVsync() { g_vsync.store(rex::cvar::Query<bool>("vsync"), std::memory_order_relaxed); }

bool Uncapped() {
  if (!REXCVAR_GET(uncapped_real_time)) {
    return false;
  }
  return !g_vsync.load(std::memory_order_relaxed);
}

// Set while the swap runs on this thread; see sub_8234BE28.
thread_local bool in_swap = false;

using Clock = std::chrono::steady_clock;

std::atomic<double> presented_fps{0.0};

void CountFrame(Clock::time_point now) {
  static Clock::time_point window_start = now;
  static uint32_t frames = 0;
  ++frames;
  const double seconds = std::chrono::duration<double>(now - window_start).count();
  if (seconds >= 0.5) {
    presented_fps.store(frames / seconds, std::memory_order_relaxed);
    window_start = now;
    frames = 0;
  }
}

// Sleeps until the deadline: a high-resolution timer for most of the wait, then a short spin,
// as a plain sleep can overshoot by a millisecond or more. High-resolution timers wake within
// about 50-100 us, so the spin is kept short: it holds a core busy and out of its sleep states.
void WaitUntil(Clock::time_point deadline) {
  constexpr auto kSpin = std::chrono::microseconds(200);
#if REX_PLATFORM_WIN32
  struct Timer {
    HANDLE handle = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                           TIMER_ALL_ACCESS);
    ~Timer() {
      if (handle) CloseHandle(handle);
    }
  };
  static thread_local Timer timer;
  const auto sleep = deadline - kSpin - Clock::now();
  if (sleep > std::chrono::microseconds(0)) {
    LARGE_INTEGER due;
    due.QuadPart = -std::chrono::duration_cast<std::chrono::nanoseconds>(sleep).count() / 100;
    if (timer.handle && SetWaitableTimerEx(timer.handle, &due, 0, nullptr, nullptr, nullptr, 0)) {
      WaitForSingleObject(timer.handle, INFINITE);
    } else if (sleep >= std::chrono::milliseconds(2)) {
      // No high-resolution timer (Windows 10 before 1803, some Wine builds): Sleep, which can
      // wake up to a timer period late, for all but the last millisecond and a bit.
      ::Sleep(DWORD(std::chrono::duration_cast<std::chrono::milliseconds>(sleep).count() - 1));
    }
  }
#else
  std::this_thread::sleep_until(deadline - kSpin);
#endif
  while (Clock::now() < deadline) {
    PG_CPU_RELAX();
  }
}

// Holds the game at frame_rate_limit when it runs uncapped.
void LimitFrameRate() {
  static Clock::time_point next;
  const int32_t limit = REXCVAR_GET(frame_rate_limit);
  if (limit <= 0 || !Uncapped()) {
    next = {};
    return;
  }
  // With presentation synced to the display (present_sync), the presenter may hold each frame
  // for whole vblanks itself: its present blocks the GPU thread until the frame's vblank, and
  // the game's swap waits for the GPU thread. A timer at 1 / limit as well would beat against
  // the real refresh rate (59.94 Hz is not 60) and make the presenter miss vblanks, so it steps
  // aside. Where the presenter can't hold a frame for several vblanks (Vulkan without present
  // wait), the timer runs at whole refreshes of the measured rate instead of 1 / limit, so the
  // frames don't drift against the scan-out. The presenter's report lapses when it stops
  // presenting (minimized, or the Insert menu painting on the UI thread), and the timer takes
  // over again.
  double display_period = 0.0;
  const rex::ui::present_sync::Pacing pacing = rex::ui::present_sync::GetPacing(&display_period);
  if (pacing == rex::ui::present_sync::Pacing::kPresenter) {
    next = {};
    return;
  }
  const double period_seconds =
      pacing == rex::ui::present_sync::Pacing::kAppAtDisplayPeriod && display_period > 0.0
          ? display_period
          : 1.0 / limit;
  const auto period =
      std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(period_seconds));
  const auto now = Clock::now();
  // A frame that took longer than the period ends at once, and the schedule restarts from it:
  // neither waiting a whole period after it (above 60 FPS a 20 ms frame was followed by a 7 ms
  // sleep) nor rushing the next frames to catch up, which made uneven intervals.
  if (next == Clock::time_point{} || now >= next + period) {
    next = now;
    return;
  }
  next += period;
  WaitUntil(next);
}

// Pacing trace: the render thread's swaps against the main loop's frames, to tell repeated
// or skipped game frames and the engine time each presented frame advanced.
std::atomic<bool> trace_active{false};
std::atomic<uint64_t> main_loop_frames{0};
// Wakes the render thread waiting in WaitForNewMainLoopFrame when the main loop hands over a frame.
std::mutex main_loop_mutex;
std::condition_variable main_loop_frame_ready;
// Sum of the engine's frame time over the main loop's frames (seconds), written by the main loop.
std::atomic<double> main_loop_engine_time{0.0};
std::FILE* trace_file = nullptr;

// Called after each swap. When the main loop is slower than the render thread, the render
// thread drew the main loop's last frame again: uncapped, half the presented frames were
// repeats (286 presented for 153 game frames per second), and at 30 FPS the swap's vblank wait
// let it present some frames after one vblank and others after two, so the game's frames were
// shown for 17 or 50 ms. Each repeat also took CPU and GPU time from the main loop. Waits until
// the main loop hands over its next frame, for at most twice its recent frame time, and not at
// all when it has handed over none for a while (loading screens, which the render thread
// animates on its own).
void WaitForNewMainLoopFrame() {
  static uint64_t last_count = 0;
  static Clock::time_point last_new_frame = Clock::now();
  static double frame_interval_s = 1.0 / 30.0;
  uint64_t count = main_loop_frames.load(std::memory_order_acquire);
  if (count == last_count && REXCVAR_GET(swap_waits_for_new_frame) &&
      Clock::now() - last_new_frame < std::chrono::milliseconds(250)) {
    const auto deadline = last_new_frame + std::chrono::duration_cast<Clock::duration>(
                                               std::chrono::duration<double>(2.0 * frame_interval_s));
    // Woken by the hand-over rather than polling (it checked every 100 us, which kept a CPU core
    // from idling: battery on handhelds).
    std::unique_lock lock(main_loop_mutex);
    main_loop_frame_ready.wait_until(lock, deadline, [&] {
      return main_loop_frames.load(std::memory_order_acquire) != last_count;
    });
    count = main_loop_frames.load(std::memory_order_acquire);
  }
  if (count != last_count) {
    const auto now = Clock::now();
    const double since = std::chrono::duration<double>(now - last_new_frame).count();
    frame_interval_s =
        std::clamp(0.8 * frame_interval_s + 0.2 * since / double(count - last_count), 0.002, 0.1);
    last_count = count;
    last_new_frame = now;
  }
}

void TraceSwap(const uint8_t* base, Clock::time_point swap_start, Clock::time_point swap_end,
               Clock::time_point limit_end) {
  if (!trace_file || !trace_active.load(std::memory_order_relaxed)) return;
  static const Clock::time_point origin = swap_start;
  auto us = [](Clock::duration d) {
    return (long long)std::chrono::duration_cast<std::chrono::microseconds>(d).count();
  };
  std::fprintf(trace_file, "%lld,%lld,%lld,%llu,%.6f,%.6f\n", us(swap_start - origin),
               us(swap_end - origin), us(limit_end - origin),
               (unsigned long long)main_loop_frames.load(std::memory_order_acquire),
               main_loop_engine_time.load(std::memory_order_acquire),
               LoadGuestFloat(base, kFrameDeltaAddress));
}

}  // namespace

namespace pg {

uint32_t EngineTimeMillis() {
  return uint32_t(main_loop_engine_time.load(std::memory_order_relaxed) * 1000.0);
}

void SetPacingTraceActive(bool active) {
  static std::once_flag open;
  std::call_once(open, [] {
    const std::string path = REXCVAR_GET(pacing_trace_csv);
    if (path.empty()) return;
    trace_file = std::fopen(path.c_str(), "w");
    if (trace_file) {
      std::fputs("swap_start_us,swap_end_us,limit_end_us,main_frames,engine_s,frame_delta_s\n",
                 trace_file);
    }
  });
  trace_active.store(active, std::memory_order_relaxed);
  if (!active && trace_file) std::fflush(trace_file);
}

double PresentedFps() { return presented_fps.load(std::memory_order_relaxed); }

void ApplyFrameRateLimit() {
  const int32_t limit = REXCVAR_GET(frame_rate_limit);
  const bool vsync = limit == 30;
  if (rex::cvar::Query<bool>("vsync") != vsync) {
    rex::cvar::SetFlagByName("vsync", vsync ? "true" : "false");
  }
  RefreshVsync();
  // The presenters read these on every present, and recreate the swapchain when the present
  // mode they call for changes.
  rex::ui::present_sync::SetTargetFrameRate(uint32_t(std::max(limit, 0)));
  rex::ui::present_sync::SetMode(rex::ui::present_sync::ParseMode(REXCVAR_GET(present_sync)));
}

}  // namespace pg

extern "C" void __imp__sub_823608C0(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void sub_823608C0(PPCContext& __restrict ctx, uint8_t* base) {
  const bool main_loop_interval = ctx.r3.u32 == kMainLoopIntervalChecksum;
  __imp__sub_823608C0(ctx, base);
  if (main_loop_interval && Uncapped()) {
    ctx.r3.u64 = 0;
  }
}

extern "C" void __imp__sub_824853A8(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void sub_824853A8(PPCContext& __restrict ctx, uint8_t* base) {
  auto* swap_interval = reinterpret_cast<uint32_t*>(base + kSwapIntervalAddress);
  static const uint32_t original_swap_interval = *swap_interval;
  RefreshVsync();
  *swap_interval = Uncapped() ? 0 : original_swap_interval;
  rex::perf::TimelineScope scope(rex::perf::TimelineEvent::kGuest1);
  const auto swap_start = Clock::now();
  in_swap = true;
  __imp__sub_824853A8(ctx, base);
  in_swap = false;
  const auto swap_end = Clock::now();
  WaitForNewMainLoopFrame();
  LimitFrameRate();
  const auto limit_end = Clock::now();
  TraceSwap(base, swap_start, swap_end, limit_end);
  CountFrame(limit_end);
  pg::native::OnSwapForTrace();
  pg::native::OnSwapForEngineHooks();
  pg::dev::OnSwap(base);
}

// Getter of the engine's vblank count. After each frame the swap also waits for this count to
// reach the previous target plus the presentation interval, and some scenes (menus, parts of
// gameplay) set that interval to 2 from within the swap, after it was cleared above. With the
// count at the real 60 Hz those scenes were held at 30 FPS. Inside the swap the getter returns a
// private count that advances on every read, so these waits end at once; the cutscene cameras
// and everything else still read the real count.
//
// The private count runs far ahead of the real one (it advances on every read). Switching to
// 30 FPS in game then froze the picture: the swap waited for the real count to reach targets set
// from the private one, longer the longer the game had run uncapped. With vsync the swap reads the
// real count plus the lead the private count had, so its targets carry on from where they were;
// switching back continues the private count from there.
extern "C" void __imp__sub_8234BE28(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void sub_8234BE28(PPCContext& __restrict ctx, uint8_t* base) {
  static uint32_t swap_count = 0;
  static uint32_t capped_lead = 0;
  static bool lead_set = true;
  if (in_swap && Uncapped()) {
    lead_set = false;
    ctx.r3.u64 = ++swap_count;
    return;
  }
  __imp__sub_8234BE28(ctx, base);
  if (in_swap) {
    const uint32_t real = ctx.r3.u32;
    if (!lead_set) {
      capped_lead = int32_t(swap_count - real) > 0 ? swap_count - real : 0;
      lead_set = true;
    }
    swap_count = real + capped_lead;
    ctx.r3.u64 = swap_count;
  }
}

// Havok physics step. Each frame sub_82418560 hands the physics thread (sub_8268D868) a step of
// game speed x 1/30 s through these two setters: real time at the console's 30 FPS, but above
// 30 FPS ragdolls and other rigid bodies ran too fast (a bailing skater hit the ground at twice
// the speed at 60 FPS). Scale the step by the engine's measured frame time, which is the same
// 1/30 s at 30 FPS.
static double PhysicsStepScale(const uint8_t* base) {
  const double frame = LoadGuestFloat(base, kFrameDeltaAddress);
  return frame > 0.0 ? std::clamp(frame * 30.0, 0.01, 1.0) : 1.0;
}

extern "C" void __imp__sub_8268D3B0(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void sub_8268D3B0(PPCContext& __restrict ctx, uint8_t* base) {
  if (Uncapped()) ctx.f1.f64 *= PhysicsStepScale(base);
  __imp__sub_8268D3B0(ctx, base);
}

extern "C" void __imp__sub_8268D3A8(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void sub_8268D3A8(PPCContext& __restrict ctx, uint8_t* base) {
  if (Uncapped()) ctx.f1.f64 *= PhysicsStepScale(base);
  __imp__sub_8268D3A8(ctx, base);
}

namespace {

// The engine's vblank counter as a clock. sub_8234BD00 runs on each vblank interrupt and
// advances the counter that the cutscene cameras (sub_82432538) read as vblanks / 60. Without
// vsync the SDK interrupts every millisecond, so uncapped the hook below runs the original only
// as many times as this clock says are due.
//
// Advancing it at real 1/60 s boundaries kept the speed right, but a boundary has no fixed phase
// relative to the game's frames: at 60 FPS frames close to a boundary saw 0 or 2 steps, a visible
// judder in camera pans, and above 60 FPS the steps fell unevenly (gaps of 1, 2 or 3 frames at
// 120 FPS). So the clock advances when the main loop hands over a frame: it adds the 1/60 s
// elapsed since the last frame to the time owed, and gives the nearest whole number of steps.
//
// The time owed is never changed other than by real time and the steps given, so the count
// follows real time exactly in the long run: at 59.94 FPS (the real rate of most 60 Hz displays)
// one frame in 1000 gets two steps, and cutscenes don't run slow. Rounding alone would keep the
// steps at whatever phase the first frame had, and the frame limiter restarting its schedule
// after a late frame would sooner or later leave that phase near half a step, where timer jitter
// brings back the 0/2 steps. So when the frames last a simple fraction of a step (1, 1/2, 2/3,
// 1/4: near 60, 120, 90 and 240 FPS, and 30 and 45), a separate bias moves the rounding point
// toward the frames' phase. It is held within a step, so it can delay or advance a step but never
// lose or gain time. At other rates (144 FPS: 5 steps in 12 frames) the pattern is too long to
// hold, and plain rounding spaces the steps as evenly as whole steps allow.
//
// The steps are still delivered by the vblank interrupt, at most a millisecond after the frame
// was handed over (right after the swap with vblank_after_swap), so the callback keeps running on
// the interrupt thread with its own arguments. While the main loop hands over no frames (loading
// screens), or with cutscene_clock_follows_frames off, the interrupt advances the clock itself
// at the real 1/60 s boundaries, as before.
//
// The cameras could move smoothly at any frame rate only with a fractional clock: see
// docs/DEVELOPMENT.md, "Cutscene camera clock", for how to give sub_82432538 one.
class EngineVblankClock {
 public:
  // The main loop handed over a frame.
  void OnMainLoopFrame(Clock::time_point now) {
    std::lock_guard lock(mutex_);
    last_frame_ = now;
    const double elapsed = AddElapsed(now);
    frame_steps_ = frame_steps_ > 0.0
                       ? frame_steps_ + kFrameStepsSmoothing * (elapsed - frame_steps_)
                       : elapsed;
    Give(std::floor(owed_ + bias_ + 0.5));
    // Frames of about n/q steps (q = 1 near 60 and 30 FPS, 2 near 120, 3 near 45 and 90, 4 near
    // 240) repeat a pattern of q rounding phases, 1/q of a step apart. Within kMaxPhaseDrift of a
    // phase per frame the bias can hold one.
    double q = 0.0;
    for (double n = 1.0; n <= 4.0; n += 1.0) {
      const double phases = n * frame_steps_;
      if (phases >= 0.5 && std::fabs(phases - std::round(phases)) < kMaxPhaseDrift) {
        q = n;
        break;
      }
    }
    if (q == 0.0) return;
    // Move the rounding point toward this frame's phase: afterwards owed_ + bias_ is nearer 0
    // (with q phases, they sit symmetrically about 0 and their pulls cancel).
    bias_ -= kPhasePull * (owed_ + bias_);
    // When the frames drift against real time (59.94 FPS), the bias follows them until it is
    // three quarters of a phase away. It then moves by a whole phase, so one frame gets a step
    // more or less (or, with q > 1, a gap between steps is one frame shorter or longer), and
    // settles again. Taking the slip at once avoids rounding near half a step for many frames,
    // where jitter gave 0 and 2 steps in turn.
    while (bias_ < -kWrapBias / q) bias_ += 1.0 / q;
    while (bias_ > kWrapBias / q) bias_ -= 1.0 / q;
  }

  // Called on each vblank interrupt: returns the number of engine vblanks to deliver now.
  uint64_t Due(Clock::time_point now, bool follow_frames) {
    std::lock_guard lock(mutex_);
    if (!follow_frames || last_frame_ == Clock::time_point{} ||
        now - last_frame_ > kFrameTimeout) {
      AddElapsed(now);
      // Only whole 1/60 s that have passed.
      Give(std::floor(owed_));
    }
    const uint64_t due = target_ - delivered_;
    delivered_ = target_;
    return due;
  }

  // Called on each vblank while capped, when the hook runs the original on every vblank: the
  // clock restarts from the next uncapped vblank or frame, rather than counting the time spent
  // capped (which the original has counted already) as a stall to catch up on.
  void Suspend() {
    std::lock_guard lock(mutex_);
    last_ = {};
    last_frame_ = {};
    owed_ = 0.0;
    bias_ = 0.0;
    frame_steps_ = 0.0;
  }

 private:
  // After this long without a frame the interrupt advances the clock on its own. It is longer
  // than any frame the game shows outside loading screens.
  static constexpr auto kFrameTimeout = std::chrono::milliseconds(100);
  // Most steps owed at once: after a longer stall the count skips ahead, as the game would see
  // after a stall. Above kFrameTimeout, so frames down to 10 FPS lose none.
  static constexpr double kMaxCatchUp = 8.0;
  // Fraction of the phase error corrected per frame: about 10 frames to settle on a new phase,
  // while timer jitter (a few hundredths of a step) moves it little.
  static constexpr double kPhasePull = 0.1;
  // Bound of the bias, in phases (whole steps near 60 FPS, half steps near 120 and so on), so
  // owed_ stays within 1.25 steps of zero. Beyond half a phase, so a bias that moved by a whole
  // phase must come half a phase back before it moves again.
  static constexpr double kWrapBias = 0.75;
  // Most drift of the frames against a phase pattern, in phases per frame, for the bias to hold
  // it (59.94 FPS drifts 0.001; 165 FPS, 0.09 from thirds of a step, is left to plain rounding).
  static constexpr double kMaxPhaseDrift = 0.03;
  // Weight of each frame in the average frame length, so one late frame doesn't change the mode.
  static constexpr double kFrameStepsSmoothing = 0.1;

  // Returns the steps of time added.
  double AddElapsed(Clock::time_point now) {
    double elapsed = 0.0;
    if (last_ != Clock::time_point{}) {
      elapsed = std::min(std::chrono::duration<double>(now - last_).count() * 60.0, kMaxCatchUp);
      owed_ += elapsed;
    }
    last_ = now;
    return elapsed;
  }

  void Give(double steps) {
    if (steps > 0.0) {
      owed_ -= steps;
      target_ += uint64_t(steps);
    }
  }

  std::mutex mutex_;
  Clock::time_point last_;        // when elapsed time was last added
  Clock::time_point last_frame_;  // when the main loop last handed over a frame
  double owed_ = 0.0;             // 1/60 s elapsed but not yet given as steps
  double bias_ = 0.0;             // shift of the frames' rounding point, in steps
  double frame_steps_ = 0.0;      // average frame length, in steps (0 until the first frame)
  uint64_t target_ = 0;           // steps given
  uint64_t delivered_ = 0;        // steps delivered to the game by the interrupt
};

EngineVblankClock engine_vblank_clock;

}  // namespace

extern "C" void __imp__sub_8234BD00(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void sub_8234BD00(PPCContext& __restrict ctx, uint8_t* base) {
  if (!Uncapped()) {
    engine_vblank_clock.Suspend();
    __imp__sub_8234BD00(ctx, base);
    return;
  }
  const uint64_t due =
      engine_vblank_clock.Due(Clock::now(), REXCVAR_GET(cutscene_clock_follows_frames));
  for (uint64_t i = 0; i < due; ++i) {
    __imp__sub_8234BD00(ctx, base);
  }
}

// The main loop's hand-over of a frame to the render thread (sub_824852F8 called from
// sub_8247E318): counts the main loop's frames for WaitForNewMainLoopFrame and the trace.
extern "C" void __imp__sub_824852F8(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void sub_824852F8(PPCContext& __restrict ctx, uint8_t* base) {
  constexpr uint32_t kMainLoopHandOverCall = 0x8247E368;
  const bool from_main_loop = uint32_t(ctx.lr) == kMainLoopHandOverCall;
  __imp__sub_824852F8(ctx, base);
  if (from_main_loop) {
    if (Uncapped() && REXCVAR_GET(cutscene_clock_follows_frames)) {
      engine_vblank_clock.OnMainLoopFrame(Clock::now());
    }
    main_loop_engine_time.store(main_loop_engine_time.load(std::memory_order_relaxed) +
                                    LoadGuestFloat(base, kFrameDeltaAddress),
                                std::memory_order_relaxed);
    {
      // Under the lock, so a waiter between its check and its sleep cannot miss the wake-up.
      std::lock_guard lock(main_loop_mutex);
      main_loop_frames.fetch_add(1, std::memory_order_release);
    }
    main_loop_frame_ready.notify_one();
  }
}
