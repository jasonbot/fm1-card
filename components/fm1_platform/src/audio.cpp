#include "fm1/platform/board.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_codec_dev_vol.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

namespace fm1::platform {

constexpr const char* kTag = "fm1-audio";
constexpr int kSampleRate = 16000;
constexpr int kChunkFrames = 256;
constexpr int kQueueDepth = 8;

struct Beep {
  uint16_t hz;
  uint16_t ms;
  Wave wave;
};

void Audio::task_entry(void* arg) {
  auto* audio = static_cast<Audio*>(arg);
  auto* queue = static_cast<QueueHandle_t>(audio->queue_);
  if (queue == nullptr) {
    vTaskDelete(nullptr);
    return;
  }
  for (;;) {
    Beep note{};
    if (xQueueReceive(queue, &note, portMAX_DELAY) != pdTRUE) continue;
    std::array<int16_t, kChunkFrames * 2> pcm{};
    const std::uint32_t total = static_cast<std::uint32_t>(note.ms) * kSampleRate / 1000;
    std::uint32_t done = 0;
    double phase = 0;
    const double step = static_cast<double>(note.hz) / kSampleRate;
    while (done < total) {
      const std::uint32_t frames = std::min<std::uint32_t>(kChunkFrames, total - done);
      for (std::uint32_t i = 0; i < frames; ++i) {
        const double p = phase - std::floor(phase);
        const double value = note.wave == Wave::Triangle
                                 ? (p < 0.5 ? p * 4.0 - 1.0 : 3.0 - p * 4.0)
                                 : note.wave == Wave::Saw ? p * 2.0 - 1.0
                                                          : (p < 0.5 ? 1.0 : -1.0);
        const double remaining = static_cast<double>(total - done - i) / kSampleRate;
        const double envelope = std::min({1.0, static_cast<double>(done + i) / 80.0,
                                          remaining * 80.0});
        const auto sample = static_cast<int16_t>(value * envelope * 5000.0);
        pcm[i * 2] = sample;
        pcm[i * 2 + 1] = sample;
        phase += step;
      }
      auto* codec = static_cast<esp_codec_dev_handle_t>(audio->codec_);
      if (codec == nullptr ||
          esp_codec_dev_write(codec, pcm.data(), frames * 2 * sizeof(int16_t)) !=
              ESP_CODEC_DEV_OK) {
        break;
      }
      done += frames;
    }
  }
}

Result<void> Audio::start(void* i2c_bus) {
  if (codec_ != nullptr) return {};
  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
  chan_cfg.auto_clear = true;
  if (i2s_new_channel(&chan_cfg, reinterpret_cast<i2s_chan_handle_t*>(&i2s_tx_), nullptr) !=
      ESP_OK) {
    return fail("I2S channel init failed");
  }
  i2s_std_config_t std_cfg = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(kSampleRate),
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                       I2S_SLOT_MODE_STEREO),
      .gpio_cfg = {.mclk = I2S_GPIO_UNUSED,
                   .bclk = GPIO_NUM_41,
                   .ws = GPIO_NUM_43,
                   .dout = GPIO_NUM_42,
                   .din = I2S_GPIO_UNUSED,
                   .invert_flags = {false, false, false}},
  };
  std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
  auto* tx = static_cast<i2s_chan_handle_t>(i2s_tx_);
  if (i2s_channel_init_std_mode(tx, &std_cfg) != ESP_OK ||
      i2s_channel_enable(tx) != ESP_OK) {
    i2s_del_channel(tx);
    i2s_tx_ = nullptr;
    return fail("I2S config failed");
  }

  audio_codec_i2c_cfg_t i2c_cfg{};
  i2c_cfg.port = I2C_NUM_0;
  i2c_cfg.addr = ES8311_CODEC_DEFAULT_ADDR;
  i2c_cfg.bus_handle = static_cast<i2c_master_bus_handle_t>(i2c_bus);
  i2c_cfg.clock_speed_hz = 100000;
  const auto* ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
  audio_codec_i2s_cfg_t i2s_cfg{};
  i2s_cfg.port = I2S_NUM_1;
  i2s_cfg.tx_handle = tx;
  i2s_cfg.rx_handle = nullptr;
  i2s_cfg.clk_src = I2S_CLK_SRC_DEFAULT;
  const auto* data_if = audio_codec_new_i2s_data(&i2s_cfg);
  const auto* gpio_if = audio_codec_new_gpio();
  if (ctrl_if == nullptr || data_if == nullptr || gpio_if == nullptr) {
    return fail("codec interfaces failed");
  }
  es8311_codec_cfg_t codec_cfg{};
  codec_cfg.ctrl_if = ctrl_if;
  codec_cfg.gpio_if = gpio_if;
  codec_cfg.codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC;
  codec_cfg.master_mode = false;
  codec_cfg.use_mclk = false;
  codec_cfg.pa_pin = GPIO_NUM_NC;
  codec_cfg.pa_reverted = false;
  codec_cfg.digital_mic = false;
  codec_cfg.invert_mclk = false;
  codec_cfg.invert_sclk = false;
  codec_cfg.hw_gain.pa_voltage = 5.0f;
  codec_cfg.hw_gain.codec_dac_voltage = 3.3f;
  codec_cfg.no_dac_ref = false;
  codec_cfg.mclk_div = 256;
  const auto* codec_if = es8311_codec_new(&codec_cfg);
  if (codec_if == nullptr) return fail("ES8311 init failed");
  esp_codec_dev_cfg_t dev_cfg{};
  dev_cfg.dev_type = ESP_CODEC_DEV_TYPE_OUT;
  dev_cfg.codec_if = codec_if;
  dev_cfg.data_if = data_if;
  auto* codec = esp_codec_dev_new(&dev_cfg);
  if (codec == nullptr) return fail("codec device failed");
  esp_codec_dev_sample_info_t sample_cfg{};
  sample_cfg.bits_per_sample = 16;
  sample_cfg.channel = 2;
  sample_cfg.channel_mask = 0x03;
  sample_cfg.sample_rate = kSampleRate;
  sample_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
  if (esp_codec_dev_open(codec, &sample_cfg) != ESP_CODEC_DEV_OK) {
    esp_codec_dev_delete(codec);
    return fail("codec open failed");
  }
  (void)esp_codec_dev_set_out_vol(codec, 45);
  codec_ = codec;

  queue_ = xQueueCreate(kQueueDepth, sizeof(Beep));
  if (queue_ == nullptr) return fail("audio queue failed");
  if (xTaskCreate(Audio::task_entry, "fm1-audio", 4096, this, 1,
                  reinterpret_cast<TaskHandle_t*>(&task_)) != pdPASS) {
    return fail("audio task failed");
  }
  ESP_LOGI(kTag, "ES8311 audio started");
  return {};
}

void Audio::beep(uint16_t hz, uint16_t ms, Wave wave) {
  if (queue_ == nullptr) return;
  Beep note{hz, ms, wave};
  (void)xQueueSend(static_cast<QueueHandle_t>(queue_), &note, 0);
}

void Audio::chime() {
  beep(523, 80, Wave::Saw);
  beep(659, 80, Wave::Saw);
  beep(784, 140, Wave::Saw);
}

}  // namespace fm1::platform
