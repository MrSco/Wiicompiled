#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <limits>

namespace aurora::stereo {
// Scene playback for the headset. The image the compositor shows at display time d
// shows the scene as it was a playback delay earlier, so the scene advances exactly as
// the display does, however late the frame worker starts that image. Sampling the
// worker's own wake time made each step depend on scheduling: 7.1 to 9.6 ms steps for
// an 8.33 ms refresh on a PS VR2 at 120 Hz.
//
// Only the display time's steps are used, never its absolute value: a runtime may
// predict head poses 40 to 60 ms ahead, beyond the newest sealed scene. A step that is
// not a whole number of display periods, or that disagrees with the render step by
// more than three quarters of one (the runtime moved its prediction horizon), is
// replaced by the render step.
//
// Only the newest pair of scenes is retained, so an image can only show the blend window
// that is newest when it is rendered: a sample before it shows the window's start (the
// previous scene, early) and one past it its end (the newest scene, held). Which window
// that is depends on whether the image was rendered before or after a seal, so the
// moment playback crosses into a new window has to stay near the seals. Playback starts
// as if each blend began at its seal, and once a window (about a second) has passed
// without a correction, it is moved, by at most 2% of a display period per image,
// halfway towards whichever end had more room. Room is measured per seal cycle (its
// closest sample to each end), and the sixth-closest cycle counts, so a few hitches do
// not move it. When seals have moved for good (a clamp deeper than
// kSnap in three of the last six seal cycles, all on the same side, as when a scene gets
// heavier or shader compilation ends), playback is set to the clamped end at once: the
// image shows that end either way, so this moves nothing on screen. A cut, a restarted producer
// schedule or host clock, or playback more than a whole scene behind starts again; scene
// time never moves backwards on screen, and a stalled producer holds its newest scene,
// never extrapolated.
class ScenePlaybackClock {
public:
  // Counters for the [vr-motion] log, taken (and cleared) once per log window.
  struct Diagnostics {
    // Nanoseconds from a sample to the start and to the end of the blend window it fell
    // in, lowest seen; negative when it was clamped to that end.
    int64_t minFromStart = std::numeric_limits<int64_t>::max();
    int64_t minToEnd = std::numeric_limits<int64_t>::max();
    // Latest sample: display time (render time without one) minus scene time.
    int64_t delay = 0;
    uint32_t shifted = 0;          // samples moved to recentre playback
    uint32_t snapped = 0;          // samples set to the clamped end after seals moved for good
    uint32_t displayIrregular = 0; // display steps that were not whole display periods
    uint32_t horizonJumps = 0;     // display steps that disagreed with the render step
  };

  static constexpr int64_t kWindow = 1'000'000'000;
  // Smaller imbalances are left alone.
  static constexpr int64_t kDeadband = 250'000;
  // A clamp deeper than this in three of the last six seal cycles, none of them on the
  // other side, moves playback to the clamped end. Not every cycle has an image near its
  // seal: at 72 Hz about half of them do.
  static constexpr int64_t kSnap = 2'000'000;
  static constexpr uint32_t kSnapCycles = 6;
  static constexpr uint32_t kSnapClamps = 3;
  // Used for the step checks when the runtime gives no display period.
  static constexpr int64_t kDefaultPeriod = 11'111'111;

  // A sealed scene: the samples blend into it over [boundary, boundary + interval].
  void begin_scene(uint64_t boundary, uint64_t interval, uint64_t now, bool continuous) noexcept {
    if (!continuous || boundary <= newestBoundary_ || now < sealTime_) {
      anchored_ = false;
      earlyCycles_ = 0;
      lateCycles_ = 0;
    } else {
      earlyCycles_ = push_cycle(earlyCycles_, cycleEarly_);
      lateCycles_ = push_cycle(lateCycles_, cycleLate_);
      if (cycleFromStart_ != kNone) {
        fromStart_.add(cycleFromStart_);
        toEnd_.add(cycleToEnd_);
      }
    }
    cycleEarly_ = false;
    cycleLate_ = false;
    cycleFromStart_ = kNone;
    cycleToEnd_ = kNone;
    newestBoundary_ = boundary;
    interval_ = interval;
    sealTime_ = now;
  }

  // The scene time to show in an image displayed at displayTime (steady-clock ns, 0 when
  // unknown) on a display refreshing every displayPeriod (0 when unknown), rendered now.
  // Zero before any scene.
  uint64_t sample_time(uint64_t displayTime, uint64_t displayPeriod, uint64_t now) noexcept {
    if (newestBoundary_ == 0 || interval_ == 0)
      return 0;
    const int64_t interval = static_cast<int64_t>(interval_);
    const int64_t start = static_cast<int64_t>(newestBoundary_);
    const int64_t render = static_cast<int64_t>(now);
    int64_t scene;
    if (!anchored_ || render < lastRender_) {
      scene = start + std::clamp<int64_t>(render - static_cast<int64_t>(sealTime_), 0, interval);
      anchored_ = true;
      pending_ = 0;
      startWindow(render);
    } else {
      const int64_t period = displayPeriod != 0 ? static_cast<int64_t>(displayPeriod) : kDefaultPeriod;
      int64_t step = render - lastRender_;
      if (displayTime != 0 && lastDisplay_ != 0) {
        const int64_t displayStep = static_cast<int64_t>(displayTime) - static_cast<int64_t>(lastDisplay_);
        const int64_t periods = displayStep >= 0 ? (displayStep + period / 2) / period : 0;
        if (displayStep == 0)
          step = 0; // the same display time again shows the same scene
        else if (displayStep < 0 || std::llabs(displayStep - periods * period) > period / 16)
          ++diagnostics_.displayIrregular;
        else if (std::llabs(displayStep - step) > period * 3 / 4)
          ++diagnostics_.horizonJumps;
        else
          step = displayStep;
      }
      scene = scene_ + step;
      if (pending_ != 0) {
        const int64_t limit = period / 50;
        const int64_t shift = std::max(std::clamp(pending_, -limit, limit), -step);
        scene += shift;
        pending_ -= shift;
        ++diagnostics_.shifted;
        adjustedInWindow_ = true;
      }
      if (scene < start - interval) {
        // More than a whole scene behind (the producer got faster): rejoin the newest blend.
        scene = start;
        pending_ = 0;
        adjustedInWindow_ = true;
      } else if (scene < start - kSnap) {
        cycleEarly_ = true;
        if (persistent(earlyCycles_, lateCycles_ != 0 || cycleLate_))
          snap(scene, start);
      } else if (scene > start + interval + kSnap) {
        cycleLate_ = true;
        if (persistent(lateCycles_, earlyCycles_ != 0 || cycleEarly_))
          snap(scene, start + interval);
      }
    }
    lastRender_ = render;
    lastDisplay_ = displayTime;
    scene_ = scene;

    const int64_t fromStart = scene - start;
    const int64_t toEnd = start + interval - scene;
    cycleFromStart_ = std::min(cycleFromStart_, fromStart);
    cycleToEnd_ = std::min(cycleToEnd_, toEnd);
    diagnostics_.minFromStart = std::min(diagnostics_.minFromStart, fromStart);
    diagnostics_.minToEnd = std::min(diagnostics_.minToEnd, toEnd);
    diagnostics_.delay = (displayTime != 0 ? static_cast<int64_t>(displayTime) : render) - scene;
    if (render - windowStart_ >= kWindow) {
      if (!adjustedInWindow_ && fromStart_.full() && toEnd_.full()) {
        const int64_t imbalance = (toEnd_.value() - fromStart_.value()) / 2;
        if (std::llabs(imbalance) > kDeadband)
          pending_ = imbalance;
      }
      startWindow(render);
    }
    return static_cast<uint64_t>(std::max<int64_t>(scene, 1));
  }

  Diagnostics take_diagnostics() noexcept {
    const Diagnostics taken = diagnostics_;
    diagnostics_ = {};
    diagnostics_.delay = taken.delay;
    return taken;
  }

private:
  static constexpr int64_t kNone = std::numeric_limits<int64_t>::max();
  // The sixth-lowest of the seal cycles' room in a window: five hitched cycles do not count.
  struct SixthLowest {
    static constexpr uint32_t kRank = 6;
    int64_t lowest[kRank]{};
    uint32_t count = 0;
    void add(int64_t value) noexcept {
      uint32_t at = std::min(count, kRank);
      while (at > 0 && lowest[at - 1] > value) {
        if (at < kRank)
          lowest[at] = lowest[at - 1];
        --at;
      }
      if (at < kRank)
        lowest[at] = value;
      ++count;
    }
    bool full() const noexcept { return count >= kRank; }
    int64_t value() const noexcept { return lowest[kRank - 1]; }
  };

  // Seal-cycle history: one bit per cycle, the newest lowest.
  static uint32_t push_cycle(uint32_t history, bool clamped) noexcept {
    return ((history << 1) | (clamped ? 1u : 0u)) & ((1u << kSnapCycles) - 1);
  }
  // This cycle's deep clamp and those of the previous cycles of the history, when the
  // other side had none.
  static bool persistent(uint32_t history, bool otherSide) noexcept {
    uint32_t clamps = 1;
    for (uint32_t bits = history & ((1u << (kSnapCycles - 1)) - 1); bits != 0; bits &= bits - 1)
      ++clamps;
    return !otherSide && clamps >= kSnapClamps;
  }

  void snap(int64_t& scene, int64_t to) noexcept {
    scene = to;
    pending_ = 0;
    adjustedInWindow_ = true;
    ++diagnostics_.snapped;
  }

  void startWindow(int64_t render) noexcept {
    windowStart_ = render;
    fromStart_ = {};
    toEnd_ = {};
    adjustedInWindow_ = false;
  }

  uint64_t newestBoundary_ = 0;
  uint64_t interval_ = 0;
  uint64_t sealTime_ = 0;
  bool anchored_ = false;
  int64_t scene_ = 0;
  int64_t lastRender_ = 0;
  uint64_t lastDisplay_ = 0;
  int64_t pending_ = 0;
  int64_t windowStart_ = 0;
  SixthLowest fromStart_;
  SixthLowest toEnd_;
  // This seal cycle's closest sample to each end of its window.
  int64_t cycleFromStart_ = kNone;
  int64_t cycleToEnd_ = kNone;
  bool adjustedInWindow_ = false;
  // Per side, the recent seal cycles with a clamp deeper than kSnap, and this cycle's.
  uint32_t earlyCycles_ = 0;
  uint32_t lateCycles_ = 0;
  bool cycleEarly_ = false;
  bool cycleLate_ = false;
  Diagnostics diagnostics_{};
};

// The current scene belongs to boundary, and playback blends from the preceding
// scene for one guest interval. Head tracking is applied afterwards, undelayed.
inline float interpolation_weight(uint64_t sampleTime, uint64_t boundary, uint64_t interval) noexcept {
  if (sampleTime == 0 || boundary == 0 || interval == 0)
    return 1.0f;
  if (sampleTime <= boundary)
    return 0.0f;
  return static_cast<float>(std::min(static_cast<double>(sampleTime - boundary) / static_cast<double>(interval), 1.0));
}

// Worker-owned window. A fresh eye pair can still repeat the same game-scene
// time, or move it backwards when an overdue scene is replaced. Neither is
// visible in the compositor's submission FPS. Discontinuities have no comparable
// scene time, so they break the sequence rather than counting as backwards motion.
struct MotionSamples {
  uint32_t samples = 0;
  uint32_t blended = 0;
  uint32_t atPrevious = 0;
  uint32_t atCurrent = 0;
  uint32_t discontinuous = 0;
  uint32_t repeated = 0;
  uint32_t backwards = 0;
  uint64_t lastSceneTime = 0;
  uint64_t minStep = UINT64_MAX;
  uint64_t maxStep = 0;

  void record(uint64_t displayTime, uint64_t boundary, uint64_t interval,
              bool continuous, float weight) noexcept {
    ++samples;
    if (!continuous || displayTime == 0 || boundary < interval || interval == 0) {
      ++discontinuous;
      lastSceneTime = 0;
      return;
    }
    if (weight <= 0.0f)
      ++atPrevious;
    else if (weight >= 1.0f)
      ++atCurrent;
    else
      ++blended;
    const uint64_t sceneTime = boundary - interval +
        static_cast<uint64_t>(static_cast<double>(interval) * weight);
    if (lastSceneTime != 0) {
      if (sceneTime >= lastSceneTime) {
        const auto step = sceneTime - lastSceneTime;
        minStep = std::min(minStep, step);
        maxStep = std::max(maxStep, step);
      }
      // Float interpolation weights can differ by a few nanoseconds at a boundary.
      if (sceneTime + 1000 < lastSceneTime)
        ++backwards;
      else if (sceneTime <= lastSceneTime + 1000)
        ++repeated;
    }
    lastSceneTime = sceneTime;
  }

  void clear_window() noexcept {
    const auto last = lastSceneTime;
    *this = {};
    lastSceneTime = last;
  }
};
} // namespace aurora::stereo
