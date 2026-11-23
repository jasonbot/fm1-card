cmake_minimum_required(VERSION 3.22)
project(fm1_card_core_tests LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

add_library(lwjson STATIC
  ../components/lwjson/src/lwjson.c
  ../components/lwjson/src/lwjson_stream.c
  ../components/lwjson/src/lwjson_utils.c
  ../components/lwjson/src/lwjson_serializer.c)
target_include_directories(lwjson PUBLIC
  ../components/lwjson/include
  ../components/lwjson/include/lwjson)

add_library(fm1_core STATIC
  ../components/fm1_core/src/midi.cpp
  ../components/fm1_core/src/identity.cpp
  ../components/fm1_core/src/package.cpp
  ../components/fm1_core/src/update.cpp
  ../components/fm1_core/src/routing.cpp
  ../components/fm1_core/src/clock.cpp
  ../components/fm1_core/src/wl82.cpp
  ../components/fm1_core/src/flash_analysis.cpp
  ../components/fm1_core/src/catalog.cpp)
target_include_directories(fm1_core PUBLIC ../components/fm1_core/include)
target_link_libraries(fm1_core PUBLIC lwjson)

add_library(fm1_ui_canvas STATIC
  ../components/fm1_ui/src/canvas.cpp
  ../components/fm1_ui/src/font.cpp)
target_include_directories(fm1_ui_canvas PUBLIC ../components/fm1_ui/include)

add_executable(fm1_core_tests tests.cpp)
target_link_libraries(fm1_core_tests PRIVATE fm1_core fm1_ui_canvas)

enable_testing()
add_test(NAME fm1_core COMMAND fm1_core_tests WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/..)
