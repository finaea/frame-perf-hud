cmake_minimum_required(VERSION 3.16)
project(frame-perf-hud VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# core/: shared C++17 with no platform dependencies (metric registry, value store, scheduler).
# The headset agent links it now; the Windows app will later.
add_library(fph-core STATIC
  core/reason.cpp
  core/metric_registry.cpp
  core/metric_store.cpp
  core/scheduler.cpp
)
target_include_directories(fph-core PUBLIC core)
target_compile_options(fph-core PRIVATE -Wall -Wextra)

# Everything below is the headset agent, which only builds on the Steam Frame (SteamOS, aarch64).
option(FPH_BUILD_HEADSET "Build the headset agent (needs cairo, freetype, vulkan, libnl, SteamVR's OpenVR)" ON)

if(FPH_BUILD_HEADSET)
  # The OpenVR library that ships with SteamVR (its path on the Frame's SteamOS)
  set(OPENVR_LIB_DIR "/opt/steamvr/bin/linuxarm64" CACHE PATH "Directory holding libopenvr_api.so")

  find_package(PkgConfig REQUIRED)
  pkg_check_modules(CAIRO REQUIRED IMPORTED_TARGET cairo)
  pkg_check_modules(FREETYPE REQUIRED IMPORTED_TARGET freetype2)
  pkg_check_modules(VULKAN REQUIRED IMPORTED_TARGET vulkan)
  pkg_check_modules(LIBNL REQUIRED IMPORTED_TARGET libnl-genl-3.0)

  add_executable(frame-perf-hud
    headset/main.cpp
    headset/sensor_bridge.cpp
    headset/placement.cpp
    headset/clock.cpp
    headset/config.cpp
    headset/json.cpp
    headset/sensors.cpp
    headset/panel.cpp
    headset/vr_overlay.cpp
    headset/vk_texture.cpp
    headset/draw.cpp
    headset/settings_panel.cpp
    headset/gpu_usage.cpp
    headset/wifi_link.cpp
    headset/i18n.cpp
    headset/autostart.cpp
    headset/theme.cpp
    headset/paths.cpp
    vendor/frame-updater/cpp/update_check.cpp
  )
  target_include_directories(frame-perf-hud PRIVATE headset third_party/openvr vendor/frame-updater/cpp)
  target_compile_options(frame-perf-hud PRIVATE -Wall -Wextra)
  # The version printed by --version (from project(VERSION) above)
  target_compile_definitions(frame-perf-hud PRIVATE FRAME_PERF_HUD_VERSION="${PROJECT_VERSION}")
  target_link_libraries(frame-perf-hud PRIVATE
    fph-core
    PkgConfig::CAIRO
    PkgConfig::FREETYPE
    PkgConfig::VULKAN
    PkgConfig::LIBNL
    "${OPENVR_LIB_DIR}/libopenvr_api.so"
  )
  # Use SteamVR's library at run time too. Only INSTALL_RPATH, from the build on: a build RPATH ends in an
  # empty element, and an empty element means "also search the current directory".
  set_target_properties(frame-perf-hud PROPERTIES
    BUILD_WITH_INSTALL_RPATH TRUE
    INSTALL_RPATH "${OPENVR_LIB_DIR}"
  )

  install(TARGETS frame-perf-hud RUNTIME DESTINATION bin)
endif()

# Tests: ctest --test-dir build. The release build (scripts/package.sh) passes -DBUILD_TESTING=OFF.
include(CTest)
if(BUILD_TESTING)
  # core/ tests: platform-free, link fph-core only
  add_executable(core-registry-test tests/core_registry_test.cpp)
  target_link_libraries(core-registry-test PRIVATE fph-core)
  target_compile_options(core-registry-test PRIVATE -Wall -Wextra)
  add_test(NAME core-registry COMMAND core-registry-test)

  add_executable(core-scheduler-test tests/core_scheduler_test.cpp)
  target_link_libraries(core-scheduler-test PRIVATE fph-core)
  target_compile_options(core-scheduler-test PRIVATE -Wall -Wextra)
  add_test(NAME core-scheduler COMMAND core-scheduler-test)

  if(FPH_BUILD_HEADSET)
    # Upstream's tests: wrist placement maths, clock, config file, panel height, settings panel hit-testing
    add_executable(wrist-clock-test
      tests/wrist_clock_test.cpp
      headset/placement.cpp
      headset/clock.cpp
      headset/config.cpp
      headset/json.cpp
      headset/panel.cpp
      headset/settings_panel.cpp
      headset/draw.cpp
      headset/i18n.cpp
      headset/theme.cpp
      headset/wifi_link.cpp
      headset/paths.cpp
    )
    target_include_directories(wrist-clock-test PRIVATE headset third_party/openvr vendor/frame-updater/cpp)
    target_compile_options(wrist-clock-test PRIVATE -Wall -Wextra)
    target_link_libraries(wrist-clock-test PRIVATE PkgConfig::CAIRO PkgConfig::FREETYPE PkgConfig::LIBNL)
    add_test(NAME wrist-clock COMMAND wrist-clock-test)
  endif()
endif()
