# TinyUSB host stack as an ESP-IDF component.
# Only the host core, hub, MIDI and MSC classes are built.
idf_build_get_property(target IDF_TARGET)
string(TOUPPER "OPT_MCU_${target}" tusb_mcu)

set(tusb_src ${CMAKE_CURRENT_LIST_DIR}/src)

set(srcs
  ${tusb_src}/tusb.c
  ${tusb_src}/common/tusb_fifo.c
  ${tusb_src}/host/usbh.c
  ${tusb_src}/host/hub.c
  ${tusb_src}/class/midi/midi_host.c
  ${tusb_src}/class/msc/msc_host.c
  ${tusb_src}/portable/synopsys/dwc2/dwc2_common.c
  ${tusb_src}/portable/synopsys/dwc2/hcd_dwc2.c
  ${tusb_src}/portable/synopsys/dwc2/dcd_dwc2.c
)

idf_component_register(SRCS ${srcs}
  INCLUDE_DIRS ${CMAKE_CURRENT_LIST_DIR}/include ${tusb_src}
  PRIV_REQUIRES esp_hw_support esp_rom soc esp_mm
  REQUIRES freertos)

target_compile_definitions(${COMPONENT_LIB} PUBLIC
  CFG_TUSB_MCU=${tusb_mcu}
  CFG_TUSB_OS=OPT_OS_FREERTOS
  BOARD_TUH_RHPORT=0
  BOARD_TUH_MAX_SPEED=OPT_MODE_FULL_SPEED
)
