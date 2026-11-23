#pragma once

#include <TritonController.h>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <thread>

struct StereoFrame {
  float left, right;
};
static_assert(sizeof(StereoFrame) == 2 * sizeof(float));

class HapticOutput {
public:
  explicit HapticOutput(TritonController& controller);
  ~HapticOutput();

  // PipeWire delivers interleaved native floats. A null pointer represents silence.
  void push(const void* frames, size_t count);

private:
  static constexpr size_t capacity = 2048;
  static constexpr size_t prefill = 256; // 32 ms at the controller's 8 kHz rate.
  void run();
  bool waitForAudio();
  size_t take(std::array<StereoFrame, 31>& block, size_t wanted);

  TritonController& controller;
  std::array<StereoFrame, capacity> ring{};
  size_t head = 0, size = 0;
  bool stopping = false;
  std::mutex mutex;
  std::condition_variable available;
  std::thread worker;
};
