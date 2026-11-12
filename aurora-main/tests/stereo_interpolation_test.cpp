#include "stereo_interpolation.hpp"
#include "scene_camera.hpp"
#include <gtest/gtest.h>
#include <array>
#include <cmath>
#include <functional>
#include <limits>
#include <vector>

TEST(StereoInterpolation, SeparatesCameraFromHeldGeometryAndFirstPersonAnchor) {
  using aurora::Mat3x4;
  using aurora::gfx::stereo_replay::compose_affine;
  const Mat3x4<float> identity{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};
  const float angle = 0.08f;
  const float c = std::cos(angle), s = std::sin(angle);
  const Mat3x4<float> currentView{{c, 0, s, 0}, {0, 1, 0, 0}, {-s, 0, c, 0}};
  const Mat3x4<float> object{{1, 0, 0, 1000}, {0, 1, 0, 0}, {0, 0, 1, -100000}};
  const auto currentObject = compose_affine(currentView, object);
  aurora::stereo::SceneCameraMotion motion;
  for (bool firstPerson : {false, true}) {
    // A moving seat is distinct from the game's chase camera. Both must be
    // sampled once, and only once, even for geometry with no usable history.
    auto previousAnchor = identity, currentAnchor = identity;
    if (firstPerson) {
      previousAnchor.m1[3] = 200;
      currentAnchor.m1[3] = 230;
    }
    ASSERT_TRUE(motion.prepare(identity, currentView, previousAnchor, currentAnchor));
    for (float weight : {0.f, 1.f / 3, 2.f / 3, 1.f}) {
      Mat3x4<float> sampledAnchor{}, expectedPose{}, expectedView{};
      ASSERT_TRUE(motion.sample(weight, sampledAnchor));
      ASSERT_TRUE(aurora::gx::interpolate_transform(motion.previousPose, motion.currentPose, weight, expectedPose));
      ASSERT_TRUE(aurora::stereo::inverse_rigid_view(expectedPose, expectedView));
      const auto expected = compose_affine(expectedView, object);
      // Identical math covers held particle vertices baked into current view
      // space and an unmatched/rejected billboard using its current matrix.
      const auto actual = compose_affine(sampledAnchor, currentObject);
      EXPECT_NEAR(actual.m0.w(), expected.m0.w(), 0.02f);
      EXPECT_NEAR(actual.m1.w(), expected.m1.w(), 0.02f);
      EXPECT_NEAR(actual.m2.w(), expected.m2.w(), 0.02f);
    }
  }
}

TEST(StereoInterpolation, CameraCutsAndMalformedViewsDisableCameraSeparation) {
  const aurora::Mat3x4<float> identity{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};
  aurora::stereo::SceneCameraMotion motion;
  ASSERT_TRUE(motion.prepare(identity, identity, identity, identity));
  auto cut = identity;
  cut.m0[3] = 2000;
  EXPECT_FALSE(motion.prepare(identity, cut, identity, identity));
  EXPECT_FALSE(motion.active);
  cut = identity;
  cut.m0[0] = 2;
  EXPECT_FALSE(motion.prepare(identity, cut, identity, identity));
  cut.m0[0] = std::numeric_limits<float>::quiet_NaN();
  EXPECT_FALSE(motion.prepare(identity, cut, identity, identity));
  cut = {{-1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, -1, 0}};
  EXPECT_FALSE(motion.prepare(identity, cut, identity, identity));
}

TEST(StereoInterpolation, ContinuousMotionAcross60HzScenesAtHeadsetRates) {
  constexpr uint64_t interval = 16'666'667;
  // A camera/object moving one unit per guest frame must advance uniformly,
  // even at 72/90 Hz where many samples are neither midpoints nor endpoints.
  for (uint64_t hz : {72u, 90u, 120u}) {
    double previousPosition = -1;
    for (uint64_t sample = 1; sample <= hz; ++sample) {
      const uint64_t displayTime = 1'000'000'000 + sample * 1'000'000'000 / hz;
      const uint64_t scene = (displayTime - 1'000'000'000) / interval;
      const uint64_t boundary = 1'000'000'000 + scene * interval;
      const float weight = aurora::stereo::interpolation_weight(displayTime, boundary, interval);
      const double position = static_cast<double>(scene) + weight;
      if (sample > 1)
        EXPECT_NEAR(position - previousPosition, 1'000'000'000.0 / hz / interval, 1e-5);
      previousPosition = position;
    }
  }
}

TEST(StereoInterpolation, MissingTimingAndStallsDoNotExtrapolate) {
  using aurora::stereo::interpolation_weight;
  EXPECT_FLOAT_EQ(interpolation_weight(0, 100, 10), 1);
  EXPECT_FLOAT_EQ(interpolation_weight(105, 0, 10), 1);
  EXPECT_FLOAT_EQ(interpolation_weight(105, 100, 0), 1);
  EXPECT_FLOAT_EQ(interpolation_weight(95, 100, 10), 0);
  EXPECT_FLOAT_EQ(interpolation_weight(105, 100, 10), 0.5);
  EXPECT_FLOAT_EQ(interpolation_weight(500, 100, 10), 1);
}

namespace {
// Deterministic jitter in [0, maxUs] microseconds, in nanoseconds.
int64_t jitter_ns(uint64_t index, uint64_t salt, int64_t maxUs) {
  const uint64_t mixed = ((index + 1) * 2654435761ull) ^ (salt * 40503ull);
  return static_cast<int64_t>(mixed % static_cast<uint64_t>(maxUs + 1)) * 1000;
}

// A 60 Hz producer and a headset compositor driving ScenePlaybackClock in time order,
// recording what the images show: the scene time each one blends to, clamped to the blend
// window that is newest when it renders, as interpolation_weight clamps it.
struct PlaybackRun {
  static constexpr int64_t kStart = 1'000'000'000'000;
  static constexpr int64_t kInterval = 16'666'667;
  uint64_t hz = 90;
  uint64_t seconds = 4;
  bool displayTimes = true;
  // Display time minus the image's nominal render start, per image.
  std::function<int64_t(uint64_t)> lead = [](uint64_t) { return 18'000'000; };
  // How late the frame worker starts an image, per image (below one display period).
  std::function<int64_t(uint64_t)> renderDelay = [](uint64_t) { return 0; };
  // Seal time after the frame's VI retrace, per scene. Negative: the scene is never sealed.
  std::function<int64_t(uint64_t)> sealLatency = [](uint64_t) { return 9'000'000; };

  struct Image {
    int64_t render = 0;
    int64_t shown = 0; // scene time on screen
    bool clamped = false; // shown the newest scene held, or the previous one early
  };
  std::vector<Image> images;
  aurora::stereo::ScenePlaybackClock::Diagnostics diagnostics;

  int64_t period() const { return 1'000'000'000 / static_cast<int64_t>(hz); }

  void run() {
    aurora::stereo::ScenePlaybackClock clock;
    const int64_t displayPeriod = period();
    uint64_t scene = 0;
    bool sealed = false;
    bool skipped = false;
    int64_t newest = 0;
    for (uint64_t image = 1; image <= hz * seconds; ++image) {
      const int64_t nominal = kStart + static_cast<int64_t>(image) * displayPeriod;
      const int64_t render = nominal + renderDelay(image);
      // Every seal that happened before this image started rendering.
      for (;; ++scene) {
        const int64_t latency = sealLatency(scene);
        if (latency < 0) {
          skipped = true;
          continue;
        }
        const int64_t sealTime = kStart + static_cast<int64_t>(scene) * kInterval + latency;
        if (sealTime > render)
          break;
        // VI stamps the retrace after the one the frame was produced in.
        const int64_t boundary = kStart + static_cast<int64_t>(scene + 1) * kInterval;
        clock.begin_scene(static_cast<uint64_t>(boundary), kInterval, static_cast<uint64_t>(sealTime),
                          sealed && !skipped);
        sealed = true;
        skipped = false;
        newest = boundary + kInterval;
      }
      if (!sealed)
        continue;
      const int64_t display = nominal + lead(image);
      const auto sample = static_cast<int64_t>(clock.sample_time(displayTimes ? static_cast<uint64_t>(display) : 0,
                                                                 displayTimes ? static_cast<uint64_t>(displayPeriod) : 0,
                                                                 static_cast<uint64_t>(render)));
      const int64_t windowStart = newest - kInterval;
      images.push_back({render, std::clamp(sample, windowStart, newest), sample > newest || sample < windowStart});
    }
    diagnostics = clock.take_diagnostics();
  }

  // Steps between consecutive images rendered at or after `from`, in nanoseconds of scene time.
  std::vector<int64_t> steps_after(int64_t from) const {
    std::vector<int64_t> steps;
    for (size_t i = 1; i < images.size(); ++i)
      if (images[i - 1].render >= from)
        steps.push_back(images[i].shown - images[i - 1].shown);
    return steps;
  }
  std::vector<int64_t> steps_between(int64_t from, int64_t to) const {
    std::vector<int64_t> steps;
    for (size_t i = 1; i < images.size(); ++i)
      if (images[i - 1].render >= from && images[i].render < to)
        steps.push_back(images[i].shown - images[i - 1].shown);
    return steps;
  }
  uint32_t clamped_between(int64_t from, int64_t to) const {
    uint32_t count = 0;
    for (const auto& image : images)
      count += image.render >= from && image.render < to && image.clamped;
    return count;
  }
  uint32_t clamped_after(int64_t from) const { return clamped_between(from, std::numeric_limits<int64_t>::max()); }
  static int64_t at_seconds(double seconds) { return kStart + static_cast<int64_t>(seconds * 1e9); }
};

std::pair<int64_t, int64_t> step_range(const std::vector<int64_t>& steps) {
  const auto [low, high] = std::minmax_element(steps.begin(), steps.end());
  return {*low, *high};
}
double mean_step_error(const std::vector<int64_t>& steps, int64_t period) {
  double total = 0;
  for (const int64_t step : steps)
    total += static_cast<double>(std::llabs(step - period));
  return steps.empty() ? 0.0 : total / static_cast<double>(steps.size());
}
int64_t max_step_error(const std::vector<int64_t>& steps, int64_t period) {
  int64_t worst = 0;
  for (const int64_t step : steps)
    worst = std::max(worst, std::llabs(step - period));
  return worst;
}
} // namespace

TEST(StereoInterpolation, PlaybackFollowsDisplayTimeNotTheWorkersWakeTime) {
  for (uint64_t hz : {72u, 90u, 120u}) {
    double meanError[2]{};
    int64_t maxError[2]{};
    for (bool displayTimes : {true, false}) {
      PlaybackRun run;
      run.hz = hz;
      run.seconds = 6;
      run.displayTimes = displayTimes;
      // The worker starts an image 0-3 ms late; seals vary by 1 ms.
      run.renderDelay = [](uint64_t image) { return jitter_ns(image, 1, 3000); };
      run.sealLatency = [](uint64_t scene) { return 9'000'000 + jitter_ns(scene, 2, 1000); };
      run.run();
      const auto steps = run.steps_after(PlaybackRun::at_seconds(3.0));
      ASSERT_GT(steps.size(), 2 * hz);
      EXPECT_GE(step_range(steps).first, 0) << hz << " Hz";
      meanError[displayTimes] = mean_step_error(steps, run.period());
      maxError[displayTimes] = max_step_error(steps, run.period());
    }
    // The wake time puts the worker's jitter into every step (0.86-0.91 ms on average
    // here). The display time leaves it only in an image whose render may fall on either
    // side of a seal, and recentring puts the switch to the next scene on that image.
    EXPECT_LT(meanError[true] * 4, meanError[false]) << hz << " Hz";
    EXPECT_LE(maxError[true], maxError[false]) << hz << " Hz";
  }
}

TEST(StereoInterpolation, PlaybackIgnoresPredictionHorizonJumps) {
  for (uint64_t hz : {72u, 90u, 120u}) {
    // Virtual Desktop predicted 37-65 ms ahead: as whole-refresh horizon changes, and as
    // a lead that drifts off the display grid.
    const std::array<std::function<int64_t(uint64_t)>, 2> leads{
        [hz](uint64_t image) {
          return 37'000'000 + static_cast<int64_t>((image / 40) % 3) * (1'000'000'000 / static_cast<int64_t>(hz));
        },
        [](uint64_t image) { return static_cast<int64_t>(37 + image % 29) * 1'000'000; },
    };
    for (const auto& lead : leads) {
      PlaybackRun run;
      run.hz = hz;
      run.lead = lead;
      run.run();
      // Steady seals and renders: once centred, every image moves one display step.
      const auto steps = run.steps_after(PlaybackRun::at_seconds(2.5));
      const auto [low, high] = step_range(steps);
      EXPECT_EQ(low, run.period()) << hz << " Hz";
      EXPECT_EQ(high, run.period()) << hz << " Hz";
      EXPECT_EQ(run.clamped_after(PlaybackRun::at_seconds(2.5)), 0u) << hz << " Hz";
    }
  }
}

TEST(StereoInterpolation, SealJitterIsSplitAroundTheSeals) {
  for (uint64_t hz : {72u, 90u, 120u}) {
    PlaybackRun run;
    run.hz = hz;
    run.seconds = 6;
    // Seals spread over 4 ms.
    run.sealLatency = [](uint64_t scene) { return 6'000'000 + jitter_ns(scene, 3, 4000); };
    run.run();
    const auto all = run.steps_after(0);
    EXPECT_GE(step_range(all).first, 0) << hz << " Hz";
    // Centred between the earliest and the latest seals, no image is more than about
    // half the spread off its display step (a hold before a late seal, or the newest
    // scene a little early after an early one), and on average far less: 0.12 ms at 72 Hz,
    // where centring on the sixth-closest seal cycle (so that hitches do not count) leaves
    // the most extreme seals a little outside.
    const auto steps = run.steps_after(PlaybackRun::at_seconds(3.0));
    EXPECT_LE(max_step_error(steps, run.period()), 2'000'000) << hz << " Hz";
    EXPECT_LE(mean_step_error(steps, run.period()), 200'000) << hz << " Hz";
  }
}

TEST(StereoInterpolation, SealsThatMoveForGoodAreFollowedWithinAFewSealCycles) {
  struct Shape {
    const char* name;
    std::function<int64_t(uint64_t)> sealLatency;
    // Stretches that start a quarter second after each change.
    std::vector<std::pair<double, double>> calm;
  };
  const std::array<Shape, 2> shapes{
      // Seals 8 ms later from 2 s to 5 s, as when a scene gets heavier, then back.
      Shape{"heavier",
            [](uint64_t scene) {
              const double seconds = static_cast<double>(scene) / 60.0;
              return seconds >= 2.0 && seconds < 5.0 ? 12'000'000 : 4'000'000;
            },
            {{0.5, 2.0}, {2.25, 5.0}, {5.25, 8.0}}},
      // Seals 11 ms earlier after the first half second, as when shader compilation ends.
      Shape{"warm-up",
            [](uint64_t scene) { return static_cast<double>(scene) / 60.0 < 0.5 ? 14'000'000 : 3'000'000; },
            {{0.75, 8.0}}},
  };
  for (uint64_t hz : {72u, 90u, 120u}) {
    for (const auto& shape : shapes) {
      PlaybackRun run;
      run.hz = hz;
      run.seconds = 8;
      run.sealLatency = shape.sealLatency;
      run.run();
      const int64_t period = run.period();
      EXPECT_GE(step_range(run.steps_after(0)).first, 0) << shape.name << ' ' << hz << " Hz";
      // A change holds (or shows early) the images near the seals for a few seal cycles.
      // After that the only movement is recentring, by 2% of a step at most.
      for (const auto& [from, to] : shape.calm) {
        const auto steps = run.steps_between(PlaybackRun::at_seconds(from), PlaybackRun::at_seconds(to));
        EXPECT_LE(max_step_error(steps, period), period / 50 + 1)
            << shape.name << ' ' << hz << " Hz from " << from << " s";
      }
    }
  }
}

TEST(StereoInterpolation, SingleHitchesHoldOnlyTheirOwnImages) {
  for (uint64_t hz : {72u, 90u, 120u}) {
    PlaybackRun run;
    run.hz = hz;
    run.seconds = 8;
    // A frame 16 ms late now and then, as on a shader compile; the one after it is on time.
    const auto hitched = [](uint64_t scene) { return scene >= 120 && scene % 100 == 20; };
    run.sealLatency = [hitched](uint64_t scene) { return hitched(scene) ? 25'000'000 : 9'000'000; };
    run.run();
    // Every image more than a game frame away from a hitch moves exactly one display step:
    // playback is not recentred around them.
    std::vector<int64_t> calm;
    for (size_t i = 1; i < run.images.size(); ++i) {
      const int64_t render = run.images[i].render;
      if (render < PlaybackRun::at_seconds(2.5))
        continue;
      const int64_t scene = (render - PlaybackRun::kStart) / PlaybackRun::kInterval;
      bool nearHitch = false;
      for (int64_t near = scene - 2; near <= scene + 1; ++near)
        nearHitch = nearHitch || (near >= 0 && hitched(static_cast<uint64_t>(near)));
      if (!nearHitch)
        calm.push_back(run.images[i].shown - run.images[i - 1].shown);
    }
    ASSERT_GT(calm.size(), 4 * hz);
    EXPECT_EQ(step_range(calm).first, run.period()) << hz << " Hz";
    EXPECT_EQ(step_range(calm).second, run.period()) << hz << " Hz";
  }
}

TEST(StereoInterpolation, StalledProducerHoldsItsNewestSceneUntilPlaybackRestarts) {
  PlaybackRun run;
  run.hz = 90;
  run.seconds = 5;
  // Scenes 120-122 are never sealed: the stall breaks continuity.
  run.sealLatency = [](uint64_t scene) { return scene >= 120 && scene <= 122 ? -1 : 9'000'000; };
  run.run();
  for (size_t i = 1; i < run.images.size(); ++i)
    EXPECT_GE(run.images[i].shown, run.images[i - 1].shown);
  // The held images show the last sealed scene, not an extrapolation of it.
  uint32_t held = 0;
  for (size_t i = 1; i < run.images.size(); ++i)
    held += run.images[i].render < PlaybackRun::at_seconds(2.1) &&
            run.images[i].shown == run.images[i - 1].shown;
  EXPECT_GT(held, 2u);
  const auto settled = run.steps_after(PlaybackRun::at_seconds(4.0));
  EXPECT_EQ(step_range(settled).first, run.period());
  EXPECT_EQ(step_range(settled).second, run.period());
}

TEST(StereoInterpolation, PlaybackRestartsOnResetSchedulesAndClocks) {
  constexpr uint64_t interval = 16'666'667, period = 11'111'111;
  aurora::stereo::ScenePlaybackClock clock;
  EXPECT_EQ(clock.sample_time(5'000'000'000, period, 1'000'000'000), 0u);
  clock.begin_scene(2'000'000'000, interval, 1'000'000'000, false);
  // Starts as if the newest blend began at its seal.
  EXPECT_EQ(clock.sample_time(5'000'000'000, period, 1'004'000'000), 2'004'000'000u);
  EXPECT_EQ(clock.sample_time(5'000'000'000 + period, period, 1'004'000'000 + period),
            2'004'000'000u + period);
  // The producer's schedule restarted (an earlier boundary): start again from it.
  clock.begin_scene(100'000'000, interval, 1'020'000'000, true);
  EXPECT_EQ(clock.sample_time(5'000'000'000 + 2 * period, period, 1'022'000'000), 102'000'000u);
  // The host clock restarted (an earlier seal time): likewise.
  clock.begin_scene(200'000'000, interval, 10'000'000, true);
  EXPECT_EQ(clock.sample_time(6'000'000'000, period, 12'000'000), 202'000'000u);
}

TEST(StereoInterpolation, MotionDiagnosticsDistinguishCadenceFromSubmissionCount) {
  aurora::stereo::MotionSamples samples;
  constexpr uint64_t boundary = 1'000'000'000, interval = 16'666'667;
  const auto record = [&](uint64_t display, uint64_t sceneBoundary, bool continuous = true) {
    samples.record(display, sceneBoundary, interval, continuous,
                   aurora::stereo::interpolation_weight(display, sceneBoundary, interval));
  };
  record(boundary, boundary);
  record(boundary + interval / 2, boundary);
  record(boundary + interval, boundary);
  // A future display deadline outruns the retained scene; another submission
  // cannot advance its motion, even though it can apply a fresh head pose.
  record(boundary + 2 * interval, boundary);
  record(boundary + 2 * interval, boundary + interval);
  EXPECT_EQ(samples.samples, 5u);
  EXPECT_EQ(samples.blended, 1u);
  EXPECT_EQ(samples.atPrevious, 1u);
  EXPECT_EQ(samples.atCurrent, 3u);
  EXPECT_EQ(samples.repeated, 1u);
  EXPECT_EQ(samples.backwards, 0u);
  EXPECT_EQ(samples.minStep, 0u);
  EXPECT_EQ(samples.maxStep, interval);
  samples.clear_window();
  record(boundary + 2 * interval, boundary + interval);
  EXPECT_EQ(samples.samples, 1u);
  EXPECT_EQ(samples.repeated, 1u); // Preserve cadence across reporting windows.
  record(boundary + interval / 2, boundary);
  EXPECT_EQ(samples.backwards, 1u);
  record(boundary, boundary, false);
  record(boundary, boundary);
  EXPECT_EQ(samples.discontinuous, 1u);
  EXPECT_EQ(samples.backwards, 1u); // A camera cut starts a new sequence.
}
