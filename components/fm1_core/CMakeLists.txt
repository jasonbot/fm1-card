idf_component_register(
  SRCS
    "src/midi.cpp"
    "src/identity.cpp"
    "src/package.cpp"
    "src/update.cpp"
    "src/routing.cpp"
    "src/clock.cpp"
    "src/wl82.cpp"
    "src/flash_analysis.cpp"
    "src/catalog.cpp"
  INCLUDE_DIRS "include"
  REQUIRES lwjson)
