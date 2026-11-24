#include "fm1/platform/board.hpp"

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"

namespace fm1::platform {
namespace {

constexpr const char* kTag = "fm1-board";

constexpr int kPinLcdSclk = 36;
constexpr int kPinLcdMosi = 35;
constexpr int kPinLcdCs = 37;
constexpr int kPinLcdDc = 34;
constexpr int kPinLcdRst = 33;
constexpr int kPinLcdBacklight = 38;

constexpr int kPinKbdSda = 8;
constexpr int kPinKbdScl = 9;
constexpr int kPinKbdInt = 11;

constexpr int kPinSdCs = 12;
constexpr int kPinSdMosi = 14;
constexpr int kPinSdClk = 40;
constexpr int kPinSdMiso = 39;

constexpr int kPinBattery = 10;

constexpr uint8_t kTcaAddress = 0x34;
constexpr uint8_t kRegCfg = 0x01;
constexpr uint8_t kRegIntStat = 0x02;
constexpr uint8_t kRegKeyCount = 0x03;
constexpr uint8_t kRegKeyEvent = 0x04;
constexpr uint8_t kRegGpioDir1 = 0x23;
constexpr uint8_t kRegGpioDir2 = 0x24;
constexpr uint8_t kRegGpioDir3 = 0x25;
constexpr uint8_t kRegGpioIntEn1 = 0x1A;
constexpr uint8_t kRegGpioIntEn2 = 0x1B;
constexpr uint8_t kRegGpioIntEn3 = 0x1C;
constexpr uint8_t kRegKpGpio1 = 0x1D;
constexpr uint8_t kRegKpGpio2 = 0x1E;
constexpr uint8_t kRegGpiEm1 = 0x20;
constexpr uint8_t kRegGpiEm2 = 0x21;
constexpr uint8_t kRegGpiEm3 = 0x22;
constexpr uint8_t kRegGpioIntLvl1 = 0x26;
constexpr uint8_t kRegGpioIntLvl2 = 0x27;
constexpr uint8_t kRegGpioIntLvl3 = 0x28;

struct KeyDef {
  char normal;
  char shifted;
  uint8_t special;  // 0 = none
};

constexpr uint8_t kKeyBackspace = 0x2A;
constexpr uint8_t kKeyTab = 0x2B;
constexpr uint8_t kKeyEnter = 0x28;
constexpr uint8_t kKeyEscape = 0x29;
constexpr uint8_t kKeyDelete = 0x4C;
constexpr uint8_t kKeyRight = 0x4F;
constexpr uint8_t kKeyLeft = 0x50;
constexpr uint8_t kKeyDown = 0x51;
constexpr uint8_t kKeyUp = 0x52;
constexpr uint8_t kKeyFn = 0xFF;
constexpr uint8_t kKeyCtrl = 0x80;
constexpr uint8_t kKeyShift = 0x81;
constexpr uint8_t kKeyAlt = 0x82;

constexpr KeyDef kKeys[4][14] = {
    {{'`', '~', kKeyEscape},
     {'1', '!', 0x3A},
     {'2', '@', 0x3B},
     {'3', '#', 0x3C},
     {'4', '$', 0x3D},
     {'5', '%', 0x3E},
     {'6', '^', 0x3F},
     {'7', '&', 0x40},
     {'8', '*', 0x41},
     {'9', '(', 0x42},
     {'0', ')', 0x43},
     {'-', '_', 0x44},
     {'=', '+', 0x45},
     {0, 0, kKeyBackspace}},
    {{0, 0, kKeyTab},
     {'q', 'Q', 0},
     {'w', 'W', 0},
     {'e', 'E', 0},
     {'r', 'R', 0},
     {'t', 'T', 0},
     {'y', 'Y', 0},
     {'u', 'U', 0},
     {'i', 'I', 0},
     {'o', 'O', 0},
     {'p', 'P', 0},
     {'[', '{', 0},
     {']', '}', 0},
     {'\\', '|', 0}},
    {{0, 0, kKeyFn},
     {0, 0, kKeyShift},
     {'a', 'A', 0},
     {'s', 'S', 0},
     {'d', 'D', 0},
     {'f', 'F', 0},
     {'g', 'G', 0},
     {'h', 'H', 0},
     {'j', 'J', 0},
     {'k', 'K', 0},
     {'l', 'L', 0},
     {';', ':', kKeyUp},
     {'\'', '"', 0},
     {0, 0, kKeyEnter}},
    {{0, 0, kKeyCtrl},
     {0, 0, 0},
     {0, 0, kKeyAlt},
     {'z', 'Z', 0},
     {'x', 'X', 0},
     {'c', 'C', 0},
     {'v', 'V', 0},
     {'b', 'B', 0},
     {'n', 'N', 0},
     {'m', 'M', 0},
     {',', '<', kKeyLeft},
     {'.', '>', kKeyDown},
     {'/', '?', kKeyRight},
     {' ', ' ', 0}},
};

bool on_color_done(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t*, void* ctx) {
  auto semaphore = static_cast<SemaphoreHandle_t>(ctx);
  BaseType_t higher = pdFALSE;
  xSemaphoreGiveFromISR(semaphore, &higher);
  return higher == pdTRUE;
}

Result<void> check(esp_err_t error, std::string_view what) {
  if (error == ESP_OK) return {};
  return fail(std::string(what) + ": " + esp_err_to_name(error));
}

}  // namespace

Result<void> Display::init() {
  spi_bus_config_t bus{};
  bus.mosi_io_num = kPinLcdMosi;
  bus.miso_io_num = -1;
  bus.sclk_io_num = kPinLcdSclk;
  bus.quadwp_io_num = -1;
  bus.quadhd_io_num = -1;
  bus.max_transfer_sz = kWidth * 16 * sizeof(uint16_t);
  auto err = check(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO), "lcd spi");
  if (!err) return err;

  gpio_config_t bl{};
  bl.pin_bit_mask = 1ull << kPinLcdBacklight;
  bl.mode = GPIO_MODE_OUTPUT;
  bl.pull_down_en = GPIO_PULLDOWN_DISABLE;
  bl.pull_up_en = GPIO_PULLUP_DISABLE;
  gpio_config(&bl);
  gpio_set_level(static_cast<gpio_num_t>(kPinLcdBacklight), 0);

  gpio_config_t rst{};
  rst.pin_bit_mask = 1ull << kPinLcdRst;
  rst.mode = GPIO_MODE_OUTPUT;
  gpio_config(&rst);
  gpio_set_level(static_cast<gpio_num_t>(kPinLcdRst), 0);
  vTaskDelay(pdMS_TO_TICKS(20));
  gpio_set_level(static_cast<gpio_num_t>(kPinLcdRst), 1);
  vTaskDelay(pdMS_TO_TICKS(120));

  done_ = xSemaphoreCreateBinary();
  if (done_ == nullptr) return fail("no lcd semaphore");

  esp_lcd_panel_io_spi_config_t io_config{};
  io_config.dc_gpio_num = static_cast<gpio_num_t>(kPinLcdDc);
  io_config.cs_gpio_num = static_cast<gpio_num_t>(kPinLcdCs);
  io_config.pclk_hz = 40 * 1000 * 1000;
  io_config.trans_queue_depth = 10;
  io_config.on_color_trans_done = on_color_done;
  io_config.user_ctx = done_;
  io_config.lcd_cmd_bits = 8;
  io_config.lcd_param_bits = 8;
  err = check(esp_lcd_new_panel_io_spi(static_cast<esp_lcd_spi_bus_handle_t>(SPI2_HOST), &io_config,
                                       reinterpret_cast<esp_lcd_panel_io_handle_t*>(&io_)),
              "lcd panel io");
  if (!err) return err;

  const auto cmd = [&](uint8_t c, std::initializer_list<uint8_t> data = {}) -> Result<void> {
    return command(c, std::span<const uint8_t>(data.begin(), data.size()));
  };
  if (auto r = cmd(0x01); !r) return r;  // software reset
  vTaskDelay(pdMS_TO_TICKS(120));
  if (auto r = cmd(0x11); !r) return r;  // sleep out
  vTaskDelay(pdMS_TO_TICKS(120));
  if (auto r = cmd(0x3A, {0x55}); !r) return r;  // 16 bits/pixel
  if (auto r = cmd(0x36, {0x60}); !r) return r;  // landscape, RGB order
  if (auto r = cmd(0x21); !r) return r;          // inversion on
  if (auto r = cmd(0xB2, {0x0C, 0x0C, 0x00, 0x33, 0x33}); !r) return r;
  if (auto r = cmd(0xB7, {0x35}); !r) return r;
  if (auto r = cmd(0xBB, {0x19}); !r) return r;
  if (auto r = cmd(0xC0, {0x2C}); !r) return r;
  if (auto r = cmd(0xC2, {0x01}); !r) return r;
  if (auto r = cmd(0xC3, {0x12}); !r) return r;
  if (auto r = cmd(0xC4, {0x20}); !r) return r;
  if (auto r = cmd(0xC6, {0x0F}); !r) return r;
  if (auto r = cmd(0xD0, {0xA4, 0xA1}); !r) return r;
  if (auto r = cmd(0xE0, {0xD0, 0x04, 0x0D, 0x11, 0x13, 0x2B, 0x3F, 0x54, 0x4C, 0x18, 0x0D, 0x0B,
                          0x1F, 0x23});
      !r)
    return r;
  if (auto r = cmd(0xE1, {0xD0, 0x04, 0x0C, 0x11, 0x13, 0x2C, 0x3F, 0x44, 0x51, 0x2F, 0x1F, 0x1F,
                          0x20, 0x23});
      !r)
    return r;
  if (auto r = cmd(0x13); !r) return r;  // normal display
  vTaskDelay(pdMS_TO_TICKS(10));
  if (auto r = cmd(0x29); !r) return r;  // display on
  vTaskDelay(pdMS_TO_TICKS(20));

  gpio_set_level(static_cast<gpio_num_t>(kPinLcdBacklight), 1);
  ready_ = true;
  return {};
}

Result<void> Display::command(uint8_t command, std::span<const uint8_t> data) {
  return check(esp_lcd_panel_io_tx_param(reinterpret_cast<esp_lcd_panel_io_handle_t>(io_), command,
                                         data.data(), data.size()),
               "lcd command");
}

Result<void> Display::set_window(int x, int y, int width, int height) {
  constexpr int kOffsetX = 40;
  constexpr int kOffsetY = 53;
  const int x0 = x + kOffsetX;
  const int x1 = x + width - 1 + kOffsetX;
  const int y0 = y + kOffsetY;
  const int y1 = y + height - 1 + kOffsetY;
  const std::array<uint8_t, 4> columns{static_cast<uint8_t>(x0 >> 8), static_cast<uint8_t>(x0),
                                       static_cast<uint8_t>(x1 >> 8), static_cast<uint8_t>(x1)};
  const std::array<uint8_t, 4> rows{static_cast<uint8_t>(y0 >> 8), static_cast<uint8_t>(y0),
                                    static_cast<uint8_t>(y1 >> 8), static_cast<uint8_t>(y1)};
  if (auto r = command(0x2A, columns); !r) return r;
  if (auto r = command(0x2B, rows); !r) return r;
  return command(0x2C);
}

Result<void> Display::blit(int x, int y, int width, int height, std::span<const uint16_t> pixels) {
  if (!ready_) return fail("display is not ready");
  if (pixels.size() < static_cast<std::size_t>(width * height)) {
    return fail("display blit buffer too small");
  }
  if (auto r = set_window(x, y, width, height); !r) return r;
  xSemaphoreTake(static_cast<SemaphoreHandle_t>(done_), 0);
  auto err = check(esp_lcd_panel_io_tx_color(reinterpret_cast<esp_lcd_panel_io_handle_t>(io_), -1,
                                             pixels.data(), pixels.size() * 2),
                   "lcd color");
  if (!err) return err;
  if (xSemaphoreTake(static_cast<SemaphoreHandle_t>(done_), pdMS_TO_TICKS(1000)) != pdTRUE) {
    return fail("lcd transfer timed out");
  }
  return {};
}

void Display::set_backlight(bool on) {
  gpio_set_level(static_cast<gpio_num_t>(kPinLcdBacklight), on ? 1 : 0);
}

Result<void> Keyboard::init() {
  i2c_master_bus_config_t bus_config{};
  bus_config.i2c_port = I2C_NUM_0;
  bus_config.sda_io_num = static_cast<gpio_num_t>(kPinKbdSda);
  bus_config.scl_io_num = static_cast<gpio_num_t>(kPinKbdScl);
  bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
  bus_config.glitch_ignore_cnt = 7;
  bus_config.flags.enable_internal_pullup = true;
  auto err =
      check(i2c_new_master_bus(&bus_config, reinterpret_cast<i2c_master_bus_handle_t*>(&bus_)),
            "keyboard i2c");
  if (!err) return err;

  i2c_device_config_t device_config{};
  device_config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
  device_config.device_address = kTcaAddress;
  device_config.scl_speed_hz = 400'000;
  err = check(
      i2c_master_bus_add_device(reinterpret_cast<i2c_master_bus_handle_t>(bus_), &device_config,
                                reinterpret_cast<i2c_master_dev_handle_t*>(&device_)),
      "keyboard device");
  if (!err) return err;

  // All pins inputs, all pins event-mode, falling edge, interrupts on.
  for (uint8_t reg : {kRegGpioDir1, kRegGpioDir2, kRegGpioDir3}) {
    if (auto r = write(reg, 0x00); !r) return r;
  }
  for (uint8_t reg : {kRegGpiEm1, kRegGpiEm2, kRegGpiEm3}) {
    if (auto r = write(reg, 0xFF); !r) return r;
  }
  for (uint8_t reg : {kRegGpioIntLvl1, kRegGpioIntLvl2, kRegGpioIntLvl3}) {
    if (auto r = write(reg, 0x00); !r) return r;
  }
  for (uint8_t reg : {kRegGpioIntEn1, kRegGpioIntEn2, kRegGpioIntEn3}) {
    if (auto r = write(reg, 0xFF); !r) return r;
  }
  if (auto r = write(kRegKpGpio1, 0x7F); !r) return r;  // 7 rows
  if (auto r = write(kRegKpGpio2, 0xFF); !r) return r;  // 8 columns
  if (auto r = write(kRegIntStat, 0x03); !r) return r;
  while (read(kRegKeyEvent) != 0) {
  }
  uint8_t config = 0;
  {
    auto value = read(kRegCfg);
    if (!value) return std::unexpected(value.error());
    config = *value;
  }
  if (auto r = write(kRegCfg, static_cast<uint8_t>(config | 0x03)); !r) return r;

  // The INT pin is not needed while polling; keep it as an input with no pull.
  gpio_config_t int_pin{};
  int_pin.pin_bit_mask = 1ull << kPinKbdInt;
  int_pin.mode = GPIO_MODE_INPUT;
  gpio_config(&int_pin);
  ready_ = true;
  return {};
}

Result<void> Keyboard::write(uint8_t reg, uint8_t value) {
  const std::array<uint8_t, 2> bytes{reg, value};
  return check(i2c_master_transmit(reinterpret_cast<i2c_master_dev_handle_t>(device_), bytes.data(),
                                   bytes.size(), 50),
               "keyboard write");
}

Result<uint8_t> Keyboard::read(uint8_t reg) {
  uint8_t value = 0;
  auto err = check(i2c_master_transmit_receive(reinterpret_cast<i2c_master_dev_handle_t>(device_),
                                               &reg, 1, &value, 1, 50),
                   "keyboard read");
  if (!err) return std::unexpected(err.error());
  return value;
}

std::optional<KeyEvent> Keyboard::poll() {
  if (!ready_) return std::nullopt;
  auto count = read(kRegKeyCount);
  if (!count) return std::nullopt;
  const uint8_t events = *count & 0x0F;
  for (uint8_t i = 0; i < events; ++i) {
    auto event = read(kRegKeyEvent);
    if (!event || *event == 0) break;
    apply(*event);
    // The API is one event per call; the UI polls again next tick.
    auto out = next_event_;
    next_event_.reset();
    return out;
  }
  return std::nullopt;
}

void Keyboard::apply(uint8_t event) {
  const bool press = (event & 0x80) != 0;
  const uint8_t raw = static_cast<uint8_t>((event & 0x7F) - 1);
  if (raw > 79) return;
  const uint8_t matrix_row = raw / 10;
  const uint8_t matrix_col = raw % 10;
  const uint8_t col = static_cast<uint8_t>(matrix_row * 2 + (matrix_col > 3 ? 1 : 0));
  const uint8_t row = static_cast<uint8_t>((matrix_col + 4) % 4);
  if (col >= 14) return;

  const auto& def = kKeys[row][col];
  if (press) {
    if (pressed_[row][col]) return;
    pressed_[row][col] = true;
    switch (def.special) {
      case kKeyShift:
        shift_ = true;
        break;
      case kKeyCtrl:
        ctrl_ = true;
        break;
      case kKeyAlt:
        alt_ = true;
        break;
      case kKeyFn:
        fn_ = true;
        break;
      default:
        break;
    }
    KeyEvent out{};
    out.kind = KeyKind::Press;
    out.shift = shift_;
    out.ctrl = ctrl_;
    out.alt = alt_;
    out.fn = fn_;
    out.character = shift_ ? def.shifted : def.normal;
    out.code = (fn_ && def.special != 0) ? def.special : 0;
    if (out.character == 0 && out.code == 0) out.code = def.special;
    if (def.special == kKeyFn || def.special == kKeyShift || def.special == kKeyCtrl ||
        def.special == kKeyAlt) {
      out.code = def.special;
    }
    next_event_ = out;
  } else {
    if (!pressed_[row][col]) return;
    pressed_[row][col] = false;
    KeyEvent out{};
    out.kind = KeyKind::Release;
    out.shift = shift_;
    out.ctrl = ctrl_;
    out.alt = alt_;
    out.fn = fn_;
    out.character = shift_ ? def.shifted : def.normal;
    out.code = def.special;
    switch (def.special) {
      case kKeyShift:
        shift_ = false;
        break;
      case kKeyCtrl:
        ctrl_ = false;
        break;
      case kKeyAlt:
        alt_ = false;
        break;
      case kKeyFn:
        fn_ = false;
        break;
      default:
        break;
    }
    next_event_ = out;
  }
}

Result<void> Storage::mount() {
  spi_bus_config_t bus{};
  bus.mosi_io_num = kPinSdMosi;
  bus.miso_io_num = kPinSdMiso;
  bus.sclk_io_num = kPinSdClk;
  bus.quadwp_io_num = -1;
  bus.quadhd_io_num = -1;
  bus.max_transfer_sz = 4096;
  auto err = check(spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO), "sd spi");
  if (!err) return err;

  esp_vfs_fat_sdmmc_mount_config_t mount_config{};
  mount_config.format_if_mount_failed = false;
  mount_config.max_files = 8;
  mount_config.allocation_unit_size = 16 * 1024;

  sdmmc_host_t host = SDSPI_HOST_DEFAULT();
  host.slot = SPI3_HOST;
  sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
  slot.host_id = SPI3_HOST;
  slot.gpio_cs = static_cast<gpio_num_t>(kPinSdCs);
  slot.gpio_cd = GPIO_NUM_NC;
  slot.gpio_wp = GPIO_NUM_NC;
  slot.gpio_int = GPIO_NUM_NC;
  sdmmc_card_t* card = nullptr;
  err = check(esp_vfs_fat_sdspi_mount("/sdcard", &host, &slot, &mount_config, &card), "sd mount");
  if (!err) return err;
  mounted_ = true;
  ESP_LOGI(kTag, "SD mounted at /sdcard");
  return {};
}

std::string Storage::path(std::string_view relative) const {
  std::string out = "/sdcard/";
  out.append(relative);
  return out;
}

Result<std::string> Storage::read_text(std::string_view relative, std::size_t limit) const {
  if (!mounted_) return fail("no SD card");
  const auto file_path = path(relative);
  std::FILE* file = std::fopen(file_path.c_str(), "rb");
  if (file == nullptr) return fail("cannot open " + file_path);
  std::string out;
  std::array<char, 512> buffer{};
  while (out.size() < limit) {
    const auto got = std::fread(buffer.data(), 1, buffer.size(), file);
    if (got == 0) break;
    out.append(buffer.data(), got);
  }
  std::fclose(file);
  return out;
}

Result<void> Storage::write_text(std::string_view relative, std::string_view text) const {
  if (!mounted_) return fail("no SD card");
  const auto file_path = path(relative);
  const auto tmp_path = file_path + ".tmp";
  std::FILE* file = std::fopen(tmp_path.c_str(), "wb");
  if (file == nullptr) return fail("cannot create " + tmp_path);
  const auto wrote = std::fwrite(text.data(), 1, text.size(), file);
  std::fflush(file);
  std::fclose(file);
  if (wrote != text.size()) {
    std::remove(tmp_path.c_str());
    return fail("short write");
  }
  std::remove(file_path.c_str());
  if (std::rename(tmp_path.c_str(), file_path.c_str()) != 0) {
    return fail("cannot replace " + file_path);
  }
  return {};
}

Result<void> Storage::ensure_dir(std::string_view relative) const {
  if (!mounted_) return fail("no SD card");
  std::string current = "/sdcard";
  std::size_t at = 0;
  while (at <= relative.size()) {
    const auto slash = relative.find('/', at);
    const auto part =
        relative.substr(at, slash == std::string_view::npos ? std::string_view::npos : slash - at);
    if (!part.empty()) {
      current.push_back('/');
      current.append(part);
      if (::mkdir(current.c_str(), 0777) != 0 && errno != EEXIST) {
        return fail("cannot create " + current);
      }
    }
    if (slash == std::string_view::npos) break;
    at = slash + 1;
  }
  return {};
}

uint64_t Storage::free_bytes() const {
  struct statvfs info{};
  if (statvfs("/sdcard", &info) != 0) return 0;
  return static_cast<uint64_t>(info.f_bavail) * info.f_frsize;
}

Result<void> Battery::init() {
  adc_oneshot_unit_handle_t handle = nullptr;
  adc_oneshot_unit_init_cfg_t unit{};
  unit.unit_id = ADC_UNIT_1;
  unit.ulp_mode = ADC_ULP_MODE_DISABLE;
  auto err = check(adc_oneshot_new_unit(&unit, &handle), "battery adc");
  if (!err) return err;
  adc_oneshot_chan_cfg_t channel{};
  channel.atten = ADC_ATTEN_DB_12;
  channel.bitwidth = ADC_BITWIDTH_DEFAULT;
  err = check(adc_oneshot_config_channel(handle, ADC_CHANNEL_9, &channel), "battery channel");
  if (!err) return err;
  adc_ = handle;
  ready_ = true;
  return {};
}

int Battery::millivolts() const {
  if (!ready_) return -1;
  int raw = 0;
  if (adc_oneshot_read(static_cast<adc_oneshot_unit_handle_t>(adc_), ADC_CHANNEL_9, &raw) !=
      ESP_OK) {
    return -1;
  }
  // The Cardputer divides the cell voltage by 2 into the ADC.
  return static_cast<int>((static_cast<float>(raw) / 4095.0f) * 3.3f * 2.0f * 1000.0f);
}

int Battery::percent() const {
  const int mv = millivolts();
  if (mv < 0) return -1;
  const float pct = (static_cast<float>(mv) / 1000.0f - 3.3f) / (4.2f - 3.3f) * 100.0f;
  if (pct < 0.0f) return 0;
  if (pct > 100.0f) return 100;
  return static_cast<int>(pct);
}

}  // namespace fm1::platform
