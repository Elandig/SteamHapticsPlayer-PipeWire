#include "HapticOutput.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <sys/prctl.h>

using namespace std::chrono_literals;

namespace {
int16_t quantize(float sample) {
  if (!std::isfinite(sample)) return 0;
  // Smooth knee above -2.5 dBFS, leaving quieter material at unity gain.
  const float excess = std::abs(sample) - 0.75f;
  if (excess > 0)
    sample = std::copysign(1.0f - 0.25f / (1.0f + 4.0f * excess), sample);
  return static_cast<int16_t>(std::lround(sample * 32767.0f));
}

uint8_t muLaw(int16_t sample) {
  const int sign = sample < 0 ? 0x80 : 0;
  const int biased = std::min(std::abs(static_cast<int>(sample)), 32635) + 132;
  int exponent = 7;
  for (int bit = 0x4000; exponent > 0 && !(biased & bit); bit >>= 1) --exponent;
  const int fraction = (biased >> (exponent + 3)) & 15;
  return static_cast<uint8_t>(~(sign | (exponent << 4) | fraction));
}

void encode(char* channel, size_t frame, float sample, bool wired) {
  const int16_t value = quantize(sample);
  if (wired) {
    const uint16_t bits = static_cast<uint16_t>(value);
    channel[2 * frame] = static_cast<char>(bits & 255);
    channel[2 * frame + 1] = static_cast<char>(bits >> 8);
  } else {
    channel[frame] = static_cast<char>(muLaw(value));
  }
}
} // namespace

HapticOutput::HapticOutput(TritonController& controller)
    : controller(controller), worker(&HapticOutput::run, this) {}

HapticOutput::~HapticOutput() {
  {
    std::lock_guard lock(mutex);
    stopping = true;
  }
  available.notify_one();
  worker.join();
}

void HapticOutput::push(const void* frames, size_t count) {
  if (count == 0) return;
  auto input = static_cast<const std::byte*>(frames);
  if (count > capacity) {
    if (input) input += (count - capacity) * sizeof(StereoFrame);
    count = capacity;
  }
  {
    std::lock_guard lock(mutex);
    // If the sender stalls, retain the most recent quarter-second of audio.
    const size_t overflow = size + count > capacity ? size + count - capacity : 0;
    head = (head + overflow) % capacity;
    size -= overflow;
    const size_t tail = (head + size) % capacity;
    const size_t first = std::min(count, capacity - tail);
    if (input) {
      std::memcpy(&ring[tail], input, first * sizeof(StereoFrame));
      std::memcpy(ring.data(), input + first * sizeof(StereoFrame), (count - first) * sizeof(StereoFrame));
    } else {
      std::fill_n(&ring[tail], first, StereoFrame{});
      std::fill_n(ring.data(), count - first, StereoFrame{});
    }
    size += count;
  }
  available.notify_one();
}

bool HapticOutput::waitForAudio() {
  std::unique_lock lock(mutex);
  available.wait(lock, [&] { return stopping || size > 0; });
  // Also play clips shorter than the prefill threshold.
  available.wait_for(lock, 32ms, [&] { return stopping || size >= prefill; });
  return !stopping;
}

size_t HapticOutput::take(std::array<StereoFrame, 31>& block, size_t wanted) {
  std::lock_guard lock(mutex);
  if (stopping) return 0;
  const size_t count = std::min(size, wanted);
  const size_t first = std::min(count, capacity - head);
  std::copy_n(&ring[head], first, block.begin());
  std::copy_n(ring.begin(), count - first, block.begin() + first);
  head = (head + count) % capacity;
  size -= count;
  return count;
}

void HapticOutput::run() {
  using Clock = std::chrono::steady_clock;
  // Reduce timer coalescing on this thread without requiring realtime privileges.
  prctl(PR_SET_TIMERSLACK, 1UL, 0UL, 0UL, 0UL);
  const bool wired = controller.pairType == ETritonPairType::k_ETritonPairType_Wired;
  const auto mode = wired ? TritonPCMMode::Khz8_16Bit : TritonPCMMode::Khz8_8Bit_ulaw;
  const size_t frames = wired ? 15 : 31;
  const auto period = 125us * static_cast<int>(frames);
  while (!controller.disconnected && waitForAudio()) {
    controller.setupPCMStreaming(mode);
    auto due = Clock::now();
    bool attack = true;
    StereoFrame previous{};
    for (;;) {
      std::array<StereoFrame, 31> block{};
      const size_t count = take(block, frames);
      MsgHapticPCMStereo packet{};
      packet.length = wired ? 30 : 31;
      for (size_t i = 0; i < frames; ++i) {
        StereoFrame sample = block[i];
        if (i < count) {
          const float gain = attack ? std::min(1.0f, (i + 1) / 12.0f) : 1.0f;
          sample.left *= gain;
          sample.right *= gain;
          previous = sample;
        } else {
          sample = previous;
          sample.left *= 0.65f;
          sample.right *= 0.65f;
          previous = sample;
        }
        encode(packet.left, i, sample.left, wired);
        encode(packet.right, i, sample.right, wired);
      }
      std::this_thread::sleep_until(due);
      const auto sent = Clock::now();
      if (controller.disconnected || controller.sendPCMStereo(&packet) < 0) return;
      // Preserve the nominal cadence, but never burst out several late packets.
      due = std::max(due + period, sent + period * 3 / 4);
      attack = false;
      if (count == 0) break;
    }
    std::this_thread::sleep_until(due);
    if (!controller.disconnected) controller.setupPCMStreaming(TritonPCMMode::None);
  }
}
