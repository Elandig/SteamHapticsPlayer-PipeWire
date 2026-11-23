#include "HapticOutput.h"
#include <TritonFinder.h>
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <iostream>
#include <memory>

namespace {
volatile std::sig_atomic_t exiting = 0;
void stop(int) { exiting = 1; }

struct Sink {
  HapticOutput output;
  pw_stream* stream = nullptr;
  bool failed = false;
};

void stateChanged(void* userdata, pw_stream_state old, pw_stream_state state, const char* error) {
  auto& sink = *static_cast<Sink*>(userdata);
  if (state == PW_STREAM_STATE_ERROR ||
      (state == PW_STREAM_STATE_UNCONNECTED && old != PW_STREAM_STATE_UNCONNECTED)) {
    sink.failed = true;
    if (!exiting) std::cerr << "PipeWire disconnected: " << (error ? error : "stream closed") << '\n';
  }
  if (state == PW_STREAM_STATE_PAUSED && old == PW_STREAM_STATE_CONNECTING)
    std::cerr << "Select Steam Controller Haptics in your sound settings. Ctrl+C to stop.\n";
}

void process(void* userdata) {
  auto& sink = *static_cast<Sink*>(userdata);
  pw_buffer* buffer = pw_stream_dequeue_buffer(sink.stream);
  if (!buffer) return;
  const auto* buf = buffer->buffer;
  if (buf && buf->n_datas == 1) {
    const auto& data = buf->datas[0];
    if (data.chunk && data.maxsize) {
      const auto bytes = std::min(data.chunk->size, data.maxsize);
      const auto offset = data.chunk->offset % data.maxsize;
      const size_t frames = bytes / sizeof(StereoFrame);
      if (!data.data || (data.chunk->flags & (SPA_CHUNK_FLAG_EMPTY | SPA_CHUNK_FLAG_CORRUPTED))) {
        sink.output.push(nullptr, frames);
      } else {
        const size_t first = std::min(frames, (data.maxsize - offset) / sizeof(StereoFrame));
        sink.output.push(static_cast<const std::byte*>(data.data) + offset, first);
        sink.output.push(data.data, frames - first);
      }
    }
  }
  pw_stream_queue_buffer(sink.stream, buffer);
}

bool serve(TritonController& controller, pw_loop* loop) {
  Sink sink{HapticOutput(controller)};
  pw_stream_events events{};
  events.version = PW_VERSION_STREAM_EVENTS;
  events.state_changed = stateChanged;
  events.process = process;
  auto* props = pw_properties_new(
      PW_KEY_MEDIA_CLASS, "Audio/Sink",
      PW_KEY_NODE_NAME, "steam_controller_haptics",
      PW_KEY_NODE_DESCRIPTION, "Steam Controller Haptics",
      PW_KEY_NODE_VIRTUAL, "true",
      PW_KEY_NODE_WANT_DRIVER, "true",
      PW_KEY_PRIORITY_SESSION, "1",
      PW_KEY_NODE_LATENCY, "128/8000",
      PW_KEY_AUDIO_CHANNELS, "2",
      SPA_KEY_AUDIO_POSITION, "[ FL FR ]",
      "resample.quality", "8",
      nullptr);
  sink.stream = pw_stream_new_simple(loop, "Steam Controller Haptics", props, &events, &sink);
  if (!sink.stream) {
    std::cerr << "Cannot create PipeWire stream: " << std::strerror(errno) << '\n';
    return false;
  }

  alignas(8) uint8_t storage[512];
  spa_pod_builder builder = SPA_POD_BUILDER_INIT(storage, sizeof(storage));
  spa_audio_info_raw format{};
  format.format = SPA_AUDIO_FORMAT_F32;
  format.rate = 8000;
  format.channels = 2;
  format.position[0] = SPA_AUDIO_CHANNEL_FL;
  format.position[1] = SPA_AUDIO_CHANNEL_FR;
  const spa_pod* param = spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &format);
  int result = pw_stream_connect(sink.stream, PW_DIRECTION_INPUT, PW_ID_ANY,
                                 PW_STREAM_FLAG_MAP_BUFFERS, &param, 1);
  if (result >= 0) {
    pw_loop_enter(loop);
    while (!exiting && !sink.failed && !controller.disconnected) {
      result = pw_loop_iterate(loop, 200);
      if (result == -EINTR) result = 0;
      if (result < 0) break;
    }
    pw_loop_leave(loop);
  }
  if (result < 0) std::cerr << "PipeWire: " << std::strerror(-result) << '\n';
  const bool ok = result >= 0 && !sink.failed;
  pw_stream_destroy(sink.stream);
  return ok;
}
} // namespace

int main(int argc, char** argv) {
  if (argc != 1) {
    std::cerr << "Usage: steam-haptics-sink\n"
                 "Leave running, then select Steam Controller Haptics in your desktop audio outputs.\n";
    return argc == 2 && std::strcmp(argv[1], "--help") == 0 ? 0 : 2;
  }
  std::signal(SIGINT, stop);
  std::signal(SIGTERM, stop);
  pw_init(&argc, &argv);
  auto loop = std::unique_ptr<pw_main_loop, decltype(&pw_main_loop_destroy)>(
      pw_main_loop_new(nullptr), pw_main_loop_destroy);
  if (!loop) {
    std::cerr << "Cannot create PipeWire main loop\n";
    return 1;
  }
  TritonFinder finder(true);
  while (!exiting) {
    std::cerr << "Waiting for a Steam Controller (USB or puck) with hidraw access...\n";
    std::unique_ptr<TritonController> controller;
    while (!exiting && !controller) {
      controller.reset(finder.getController());
      if (!controller) std::this_thread::sleep_for(std::chrono::milliseconds(750));
    }
    if (exiting) break;
    std::cerr << (controller->pairType == ETritonPairType::k_ETritonPairType_Wired
                      ? "USB: stereo 8 kHz, 16-bit PCM.\n" : "Puck: stereo 8 kHz, µ-law.\n");
    if (!serve(*controller, pw_main_loop_get_loop(loop.get()))) return 1;
  }
  return 0;
}
