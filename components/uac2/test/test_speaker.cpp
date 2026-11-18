// Tests the counting in ESPHome's USB speaker (components/usb_audio/speaker) against a model of the USB driver.
// The speaker's source is the real one; ESPHome's base classes, FreeRTOS and the hub are stand-ins, and "other tasks"
// run from inside the stubbed vTaskDelay(), where a real writer would be waiting.
// Usage: test_speaker
// Built with -DSTOCK_SPEAKER against ESPHome's unchanged usb_audio, it shows what that speaker does in the same cases.

#include <cinttypes>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <functional>

#include "esphome/components/usb_audio/speaker/usb_audio_speaker.h"
#include "esphome/core/hal.h"

using namespace esphome;
using namespace esphome::usb_audio;

static constexpr uint32_t BUFFER_BYTES = 6400;   // ESPHome's default speaker buffer
static constexpr uint32_t TRANSFER_BYTES = 576;  // 3 ms of 48 kHz 16-bit stereo
static constexpr int TRANSFERS = CONFIG_UAC_NUM_ISOC_URBS;

static const char *g_test = "";
static uint32_t g_time_ms;
static std::function<void()> g_while_waiting;  // runs once, inside the next vTaskDelay()

[[noreturn]] static void fail(const char *format, ...) {
  va_list args;
  va_start(args, format);
  printf("FAIL: %s: ", g_test);
  vprintf(format, args);
  printf("\n");
  va_end(args);
  exit(1);
}

uint32_t esphome::millis() { return g_time_ms; }

extern "C" int64_t esp_timer_get_time(void) { return static_cast<int64_t>(g_time_ms) * 1000; }

static void pass_time(uint32_t ms);

extern "C" void vTaskDelay(TickType_t ticks) {
  pass_time(ticks);
  if (g_while_waiting) {
    const std::function<void()> run = std::move(g_while_waiting);
    g_while_waiting = nullptr;
    run();
  }
}

class TestSpeaker : public USBAudioSpeaker {
 public:
  using USBAudioSpeaker::USBAudioSpeaker;
  uint32_t counted() const { return this->ring_bytes_; }
};

// The driver as the speaker meets it through the hub: a buffer it takes whole transfers from, a few transfers it can
// have in flight, and totals it reports after every attempt to take one.
struct FakeDriver {
  TestSpeaker *speaker{nullptr};
  bool active{false};
  uint32_t fill{0};
  int in_flight{0};
  uac_host_tx_progress_t totals{};
  int resume_requests{0};
  int suspend_requests{0};
  // A transfer in flight completes every 3 ms of the test's clock, as on the bus; otherwise only when a test says so
  bool plays_on_its_own{false};
  // The hub's worker holds the driver for a control request until this time; writes wait for it
  uint32_t busy_until_ms{0};
  // What the next transfer takes. A device's feedback can change it by a frame between two attempts, and the speaker
  // hears of the new size only at the next attempt.
  uint32_t transfer_bytes{TRANSFER_BYTES};
  // Runs once, right after the driver has taken its next transfer
  std::function<void()> after_take;

  // One free transfer tries to take its data, and the hub tells the speaker
  void attempt() {
    const bool takes = this->fill >= this->transfer_bytes;
    if (takes) {
      this->fill -= this->transfer_bytes;
      this->in_flight++;
      this->totals.bytes_taken += this->transfer_bytes;
      this->totals.transfers_taken++;
    }
    this->totals.next_transfer_bytes = this->transfer_bytes;
#ifdef STOCK_SPEAKER
    this->speaker->on_transfer_taken();
#else
    this->speaker->on_transfer_taken(this->totals);
#endif
    if (takes && this->after_take) {
      const std::function<void()> run = std::move(this->after_take);
      this->after_take = nullptr;
      run();
    }
  }
  esp_err_t write(size_t length) {
    if (!this->active || this->fill + length > BUFFER_BYTES + 4) {
      // The hub reports a stream that is not active and a full buffer alike
      return ESP_ERR_TIMEOUT;
    }
    this->fill += length;
    for (int free_transfers = TRANSFERS - this->in_flight; free_transfers > 0 && this->fill > 0; free_transfers--) {
      this->attempt();
    }
    return ESP_OK;
  }
  // The oldest transfer in flight finishes playing and tries to take the next one
  void complete() {
    if (this->active && this->in_flight > 0) {
      this->in_flight--;
      this->attempt();
    }
  }
  // What the hub's worker does for a queued suspend and resume
  void suspend() {
    this->active = false;
    this->fill = 0;
    this->in_flight = 0;
  }
  void resume() {
    this->active = true;
    this->totals.next_transfer_bytes = this->transfer_bytes;
  }
};

static FakeDriver g_driver;

static void pass_time(uint32_t ms) {
  for (uint32_t i = 0; i < ms; i++) {
    g_time_ms++;
    if (g_driver.plays_on_its_own && g_time_ms % 3 == 0) {
      g_driver.complete();
    }
  }
}

// The hub, reduced to the calls the speaker makes
namespace esphome::usb_audio {
void USBAudioComponent::setup() {}
void USBAudioComponent::dump_config() {}
void USBAudioComponent::loop() {}
bool USBAudioComponent::teardown() { return true; }
void USBAudioComponent::resume(Endpoint endpoint) { g_driver.resume_requests++; }
void USBAudioComponent::suspend(Endpoint endpoint) { g_driver.suspend_requests++; }
void USBAudioComponent::set_speaker_volume_level(float volume) {}
void USBAudioComponent::set_speaker_mute_state(bool mute_state) {}
esp_err_t USBAudioComponent::write_speaker(const uint8_t *data, size_t length, uint32_t timeout_ms) {
  // As the hub does: wait, within the timeout, until the worker is done with the driver. The bus plays on meanwhile.
  for (uint32_t waited = 0; g_time_ms < g_driver.busy_until_ms; waited++) {
    if (waited >= timeout_ms) {
      return ESP_ERR_TIMEOUT;
    }
    pass_time(1);
  }
  return g_driver.write(length);
}
#ifndef STOCK_SPEAKER
bool USBAudioComponent::get_speaker_progress(uac_host_tx_progress_t &progress) {
  progress = g_driver.totals;
  return true;
}
#endif
}  // namespace esphome::usb_audio

static uint8_t g_audio[32000];

static void expect_count_matches(TestSpeaker &speaker, const char *when) {
  if (speaker.counted() != g_driver.fill) {
    fail("%s the speaker counts %" PRIu32 " bytes, the driver's buffer holds %" PRIu32, when, speaker.counted(),
         g_driver.fill);
  }
}

// The main loop and the hub's worker bring a requested stream up
static void start_stream(TestSpeaker &speaker) {
  speaker.start();
  speaker.loop();
  if (g_driver.resume_requests != 1) {
    fail("the speaker asked the hub to resume %d times", g_driver.resume_requests);
  }
  g_driver.resume_requests = 0;
  g_driver.resume();
  speaker.on_stream_state(StreamEvent::RUNNING);
  if (!speaker.is_running()) {
    fail("the speaker is not running after the hub reported the stream");
  }
}

// finish() must stop the stream once the audio has played
static void expect_finish_stops(TestSpeaker &speaker) {
  speaker.finish();
  for (int i = 0; i < 500 && !speaker.is_stopped(); i++) {
    speaker.loop();
    g_driver.complete();
    g_time_ms += 3;
    if (g_driver.suspend_requests > 0) {
      g_driver.suspend_requests = 0;
      g_driver.suspend();
    }
  }
  if (!speaker.is_stopped()) {
    fail("the stream never stopped after finish(): the speaker counts %" PRIu32 " bytes, the driver's buffer holds %" PRIu32,
         speaker.counted(), g_driver.fill);
  }
}

static void new_test(const char *name, TestSpeaker &speaker) {
  g_test = name;
  g_driver = FakeDriver{};
  g_driver.speaker = &speaker;
  g_while_waiting = nullptr;
}

static void test_count_follows_the_driver() {
  const audio::AudioStreamInfo info(16, 2, 48000);
  USBAudioComponent hub;
  TestSpeaker speaker(info, BUFFER_BYTES, 100);
  speaker.set_parent(&hub);
  speaker.setup();
  new_test("the count follows the driver while playing", speaker);

  start_stream(speaker);
  for (int i = 0; i < 2000; i++) {
    speaker.play(g_audio, 1600, 0);
    expect_count_matches(speaker, "after a write");
    g_driver.complete();
    expect_count_matches(speaker, "after a transfer completed");
    speaker.loop();
    g_time_ms += 3;
  }
  expect_finish_stops(speaker);
  printf("ok: %s\n", g_test);
}

// A caller is inside play(), waiting for room in a full buffer, when the stream is stopped and another caller starts
// it again. The driver is active again before the main loop hears of it.
static void test_writer_waiting_across_a_stop_and_start() {
  const audio::AudioStreamInfo info(16, 2, 48000);
  USBAudioComponent hub;
  TestSpeaker speaker(info, BUFFER_BYTES, 100);
  speaker.set_parent(&hub);
  speaker.setup();
  new_test("a writer waiting for room across a stop and start", speaker);

  start_stream(speaker);
  bool restarted = false;
  g_while_waiting = [&]() {
    speaker.stop();
    speaker.loop();  // the main loop stops the stream and asks the hub to suspend
    g_driver.suspend_requests = 0;
    g_driver.suspend();  // the hub's worker
    speaker.start();
    speaker.loop();  // the main loop starts it again and asks the hub to resume
    g_driver.resume_requests = 0;
    g_driver.resume();  // the hub's worker; the main loop has not been told yet
    restarted = true;
  };
  speaker.play(g_audio, 20000, 100);
  if (!restarted) {
    fail("the writer never had to wait for room");
  }
  speaker.on_stream_state(StreamEvent::RUNNING);  // the main loop hears that the stream runs
  expect_count_matches(speaker, "once the restarted stream runs");
  speaker.play(g_audio, 1600, 0);
  expect_count_matches(speaker, "after a write to the restarted stream");
  expect_finish_stops(speaker);
  printf("ok: %s\n", g_test);
}

// The hub's worker holds the driver for a control request, here a volume change, for longer than the audio in the
// buffer lasts, while a writer keeps feeding the stream. Seen on hardware on 2026-11-13 with ESPHome's stock speaker:
// audio stopped at the first volume change and never came back.
static void test_volume_change_while_playing() {
  const audio::AudioStreamInfo info(16, 2, 48000);
  USBAudioComponent hub;
  TestSpeaker speaker(info, BUFFER_BYTES, 20);
  speaker.set_parent(&hub);
  speaker.setup();
  new_test("a volume change that holds the driver while playing", speaker);

  start_stream(speaker);
  g_driver.plays_on_its_own = true;
  // The writer's task, the main loop and the bus, for a while
  const auto play_for = [&](uint32_t ms) {
    size_t written = 0;
    for (const uint32_t end = g_time_ms + ms; g_time_ms < end;) {
      written += speaker.play(g_audio, 1600, 20);
      speaker.loop();
      pass_time(1);
      if (speaker.counted() > BUFFER_BYTES) {
        fail("the speaker counts %" PRIu32 " bytes in a buffer of %" PRIu32, speaker.counted(), BUFFER_BYTES);
      }
    }
    return written;
  };
  play_for(300);
  expect_count_matches(speaker, "before the volume change");
  g_driver.busy_until_ms = g_time_ms + 60;
  play_for(200);
  // 300 ms of 48 kHz 16-bit stereo is 57600 bytes
  const size_t written = play_for(300);
  if (written < 50000) {
    fail("only %zu bytes were written in the 300 ms after the volume change", written);
  }
  expect_count_matches(speaker, "after the volume change");
  g_driver.plays_on_its_own = false;
  expect_finish_stops(speaker);
  printf("ok: %s\n", g_test);
}

// A stream pauses on less than a transfer of audio, and loop() pads that tail once the bus is idle. The device's
// feedback has meanwhile made the driver's next transfer one frame shorter than the speaker was told, so the driver
// takes the transfer before the last piece of padding is written, and that last piece then finds the hub busy.
// Writers are held back while a tail is padded; they must be let through again afterwards.
static void test_tail_flush_cut_short_by_the_driver() {
  const audio::AudioStreamInfo info(16, 2, 48000);
  USBAudioComponent hub;
  TestSpeaker speaker(info, BUFFER_BYTES, 100);
  speaker.set_parent(&hub);
  speaker.setup();
  new_test("a tail flush the driver cuts short", speaker);

  start_stream(speaker);
  // The padding comes in pieces of 240 bytes: this tail needs one whole piece and a last one of 4 bytes
  const size_t tail = TRANSFER_BYTES - 244;
  if (speaker.play(g_audio, tail, 0) != tail) {
    fail("the tail of %zu bytes was not written", tail);
  }
  g_driver.transfer_bytes = TRANSFER_BYTES - 4;
  g_driver.after_take = [] { g_driver.busy_until_ms = g_time_ms + 5; };
  g_time_ms += 50;  // the bus goes idle
  speaker.loop();   // pads the tail: the driver takes its transfer after the first piece, the second finds the hub busy
  if (g_driver.totals.transfers_taken != 1 || g_driver.busy_until_ms == 0) {
    fail("the driver took %" PRIu32 " transfers during the tail flush, 1 was expected", g_driver.totals.transfers_taken);
  }
  g_time_ms += 50;
  speaker.loop();
  const size_t written = speaker.play(g_audio, 1600, 0);
  if (written != 1600) {
    fail("play() wrote %zu of 1600 bytes after the tail flush", written);
  }
  expect_count_matches(speaker, "after the write that follows the tail flush");
  expect_finish_stops(speaker);
  printf("ok: %s\n", g_test);
}

int main() {
  test_count_follows_the_driver();
#ifndef STOCK_SPEAKER
  test_writer_waiting_across_a_stop_and_start();
  test_tail_flush_cut_short_by_the_driver();
#endif
  test_volume_change_while_playing();
  printf("PASS\n");
  return 0;
}
