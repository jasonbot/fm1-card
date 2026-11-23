#include "app.hpp"

#include "fm1/platform/board.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

extern "C" void app_main() {
  static fm1::platform::Display display;
  static fm1::platform::Keyboard keyboard;
  static fm1::platform::Storage storage;
  static fm1::platform::Battery battery;
  static fm1::App app(display, keyboard, storage, battery);
  app.run();
  vTaskDelay(portMAX_DELAY);
}
