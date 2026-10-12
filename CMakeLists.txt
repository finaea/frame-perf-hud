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
  core/session_tracker.cpp  # T03
  core/json.cpp             # T04: moved from headset/ (the layout is JSON on disk and on the link)
  core/layout.cpp           # T04
  core/atomic_file.cpp      # T04: temp + fsync + rename, POSIX and Windows
  core/link_wire.cpp        # T06: the headset <-> PC wire format
  core/layout_edit.cpp      # T05: move / insert / remove widgets, drop slots (headset editor, PC editor)
)
target_include_directories(fph-core PUBLIC core)
if(NOT MSVC)
  target_compile_options(fph-core PRIVATE -Wall -Wextra)
endif()

# T06: a tiny link client (hello / watch / layout-get / layout-set / bye) for testing before the PC app.
# Builds on Windows and Linux: cmake -S . -B build-cli -DFPH_BUILD_HEADSET=OFF; cmake --build build-cli --target fph-link-cli
add_executable(fph-link-cli tools/fph_link_cli.cpp)
target_link_libraries(fph-link-cli PRIVATE fph-core)
if(WIN32)
  target_link_libraries(fph-link-cli PRIVATE ws2_32)
endif()
if(NOT MSVC)
  target_compile_options(fph-link-cli PRIVATE -Wall -Wextra)
endif()

# core/render/: the widgets and the grid renderer, shared by the headset and the PC app. Needs cairo + freetype
# only (draw.* / theme.* moved here from headset/).
find_package(PkgConfig)
if(PKG_CONFIG_FOUND)
  pkg_check_modules(CAIRO IMPORTED_TARGET cairo)
  pkg_check_modules(FREETYPE IMPORTED_TARGET freetype2)
endif()
if(TARGET PkgConfig::CAIRO AND TARGET PkgConfig::FREETYPE)
  add_library(fph-render STATIC
    core/render/draw.cpp
    core/render/theme.cpp
    core/render/widget_text.cpp
    core/render/widgets.cpp
    core/render/grid_renderer.cpp
  )
  target_include_directories(fph-render PUBLIC core/render)
  target_link_libraries(fph-render PUBLIC fph-core PkgConfig::CAIRO PkgConfig::FREETYPE)
  if(NOT MSVC)
    target_compile_options(fph-render PRIVATE -Wall -Wextra)
  endif()
endif()

# Everything below is the headset agent, which only builds on the Steam Frame (SteamOS, aarch64).
option(FPH_BUILD_HEADSET "Build the headset agent (needs cairo, freetype, vulkan, libnl, SteamVR's OpenVR)" ON)

if(FPH_BUILD_HEADSET)
  # The OpenVR library that ships with SteamVR (its path on the Frame's SteamOS)
  set(OPENVR_LIB_DIR "/opt/steamvr/bin/linuxarm64" CACHE PATH "Directory holding libopenvr_api.so")

  find_package(PkgConfig REQUIRED)
  if(NOT TARGET fph-render)
    message(FATAL_ERROR "the headset agent needs cairo and freetype2 (fph-render)")
  endif()
  pkg_check_modules(VULKAN REQUIRED IMPORTED_TARGET vulkan)
  pkg_check_modules(LIBNL REQUIRED IMPORTED_TARGET libnl-genl-3.0)
  pkg_check_modules(SYSTEMD REQUIRED IMPORTED_TARGET libsystemd)  # T03: sd_journal for vrlink's events

  add_executable(frame-perf-hud
    headset/main.cpp
    headset/sensor_bridge.cpp
    headset/placement.cpp
    headset/clock.cpp
    headset/config.cpp
    headset/sensors.cpp
    headset/sensor_math.cpp     # T02
    headset/system_metrics.cpp  # T02
    headset/vr_overlay.cpp
    headset/vk_texture.cpp
    headset/settings_panel.cpp
    headset/gpu_usage.cpp
    headset/wifi_link.cpp
    headset/process_scan.cpp  # T03
    headset/stream_link.cpp   # T03
    headset/dump_metrics.cpp  # T03
    headset/i18n.cpp
    headset/autostart.cpp
    headset/paths.cpp
    headset/layout_authority.cpp  # T06: layout compare-and-set (link + dashboard editor)
    headset/link_server.cpp       # T06: the PC link's protocol logic + UDP socket
    headset/link_agent.cpp        # T06: the link wired into the agent, --serve-link
    headset/detached.cpp          # T05: Detached mode maths
    headset/layout_editor.cpp     # T05: the dashboard tab's layout editor
    vendor/frame-updater/cpp/update_check.cpp
  )
  target_include_directories(frame-perf-hud PRIVATE headset third_party/openvr vendor/frame-updater/cpp)
  target_compile_options(frame-perf-hud PRIVATE -Wall -Wextra)
  # The version printed by --version (from project(VERSION) above)
  target_compile_definitions(frame-perf-hud PRIVATE FRAME_PERF_HUD_VERSION="${PROJECT_VERSION}")
  target_link_libraries(frame-perf-hud PRIVATE
    fph-core
    fph-render
    PkgConfig::CAIRO
    PkgConfig::FREETYPE
    PkgConfig::VULKAN
    PkgConfig::LIBNL
    PkgConfig::SYSTEMD
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

  add_executable(core-session-test tests/core_session_test.cpp)
  target_link_libraries(core-session-test PRIVATE fph-core)
  target_compile_options(core-session-test PRIVATE -Wall -Wextra)
  add_test(NAME core-session COMMAND core-session-test)

  add_executable(core-processes-demand-test tests/core_processes_demand_test.cpp)
  target_link_libraries(core-processes-demand-test PRIVATE fph-core)
  target_compile_options(core-processes-demand-test PRIVATE -Wall -Wextra)
  add_test(NAME core-processes-demand COMMAND core-processes-demand-test)

  # T04: layout model, JSON round trip, atomic save
  add_executable(core-layout-test tests/core_layout_test.cpp)
  target_link_libraries(core-layout-test PRIVATE fph-core)
  target_compile_options(core-layout-test PRIVATE -Wall -Wextra)
  add_test(NAME core-layout COMMAND core-layout-test)

  # T05: layout edits (insert / move / remove, limits, drop slots)
  add_executable(core-layout-edit-test tests/core_layout_edit_test.cpp)
  target_link_libraries(core-layout-edit-test PRIVATE fph-core)
  target_compile_options(core-layout-edit-test PRIVATE -Wall -Wextra)
  add_test(NAME core-layout-edit COMMAND core-layout-edit-test)

  # T06: link wire format (header, messages, METRICS split, BLOB reassembly)
  add_executable(core-link-wire-test tests/core_link_wire_test.cpp)
  target_link_libraries(core-link-wire-test PRIVATE fph-core)
  target_compile_options(core-link-wire-test PRIVATE -Wall -Wextra)
  add_test(NAME core-link-wire COMMAND core-link-wire-test)

  if(TARGET fph-render)
    # T04: grid geometry, 2048 cap, widget draw rules (renders PNGs into a temp dir)
    add_executable(render-grid-test tests/render_grid_test.cpp)
    target_link_libraries(render-grid-test PRIVATE fph-render)
    target_compile_definitions(render-grid-test PRIVATE
      FPH_FONT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts")
    target_compile_options(render-grid-test PRIVATE -Wall -Wextra)
    add_test(NAME render-grid COMMAND render-grid-test)
  endif()

  if(FPH_BUILD_HEADSET)
    # Upstream's tests: wrist placement maths, clock, config file, panel height, settings panel hit-testing
    add_executable(wrist-clock-test
      tests/wrist_clock_test.cpp
      headset/placement.cpp
      headset/clock.cpp
      headset/config.cpp
      headset/settings_panel.cpp
      headset/i18n.cpp
      headset/wifi_link.cpp
      headset/paths.cpp
      headset/detached.cpp       # T05
      headset/layout_editor.cpp  # T05
    )
    target_include_directories(wrist-clock-test PRIVATE headset third_party/openvr vendor/frame-updater/cpp)
    target_compile_options(wrist-clock-test PRIVATE -Wall -Wextra)
    target_link_libraries(wrist-clock-test PRIVATE fph-render PkgConfig::LIBNL)
    add_test(NAME wrist-clock COMMAND wrist-clock-test)

    # T05: the dashboard layout editor (drag / drop, palette, settings buttons) + Detached settings actions
    add_executable(layout-editor-test
      tests/layout_editor_test.cpp
      headset/placement.cpp
      headset/clock.cpp
      headset/config.cpp
      headset/settings_panel.cpp
      headset/i18n.cpp
      headset/wifi_link.cpp
      headset/paths.cpp
      headset/detached.cpp
      headset/layout_editor.cpp
    )
    target_include_directories(layout-editor-test PRIVATE headset third_party/openvr vendor/frame-updater/cpp)
    target_compile_options(layout-editor-test PRIVATE -Wall -Wextra)
    target_link_libraries(layout-editor-test PRIVATE fph-render PkgConfig::LIBNL)
    add_test(NAME layout-editor COMMAND layout-editor-test)

    # T05: Detached pose maths (grab, place in front, face, nudge, rotate) + the config's detached_pose key
    add_executable(detached-test
      tests/detached_test.cpp
      headset/detached.cpp
      headset/placement.cpp
      headset/config.cpp
      headset/i18n.cpp
      headset/paths.cpp
    )
    target_include_directories(detached-test PRIVATE headset third_party/openvr)
    target_compile_options(detached-test PRIVATE -Wall -Wextra)
    target_link_libraries(detached-test PRIVATE fph-core)
    add_test(NAME detached COMMAND detached-test)

    # T05: install.sh's "disable the old overlay?" question against a throwaway user unit (exit 77 = skipped)
    add_test(NAME install-ask COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/install_ask_test.sh
             ${CMAKE_CURRENT_SOURCE_DIR}/install.sh)
    set_tests_properties(install-ask PROPERTIES SKIP_RETURN_CODE 77)

    # T04: config.json layout key (missing / broken files) and the atomic save under kill -9
    add_executable(config-layout-test
      tests/config_layout_test.cpp
      headset/config.cpp
      headset/i18n.cpp
      headset/paths.cpp
    )
    target_include_directories(config-layout-test PRIVATE headset)
    target_compile_options(config-layout-test PRIVATE -Wall -Wextra)
    target_link_libraries(config-layout-test PRIVATE fph-core)
    add_test(NAME config-layout COMMAND config-layout-test)

    # System sensor parsing/derivations: pure, fed from tests/fixtures/sensors
    add_executable(sensor-math-test tests/sensor_math_test.cpp headset/sensor_math.cpp)
    target_include_directories(sensor-math-test PRIVATE headset)
    target_compile_options(sensor-math-test PRIVATE -Wall -Wextra)
    add_test(NAME sensor-math COMMAND sensor-math-test ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/sensors)

    # System sensors end to end: fake /sys + /proc tree built at run time, Sensors + scheduler + metric store
    add_executable(system-sources-test
      tests/system_sources_test.cpp
      headset/sensors.cpp
      headset/sensor_math.cpp
      headset/system_metrics.cpp
      headset/gpu_usage.cpp
      headset/wifi_link.cpp
    )
    target_include_directories(system-sources-test PRIVATE headset core)
    target_compile_options(system-sources-test PRIVATE -Wall -Wextra)
    target_link_libraries(system-sources-test PRIVATE fph-core PkgConfig::LIBNL)
    add_test(NAME system-sources COMMAND system-sources-test)

    # T03 sources: /proc scan (fixture trees), Wi-Fi helpers, stream link + vrlink lines
    add_executable(t03-sources-test
      tests/t03_sources_test.cpp
      headset/process_scan.cpp
      headset/stream_link.cpp
      headset/wifi_link.cpp
    )
    target_include_directories(t03-sources-test PRIVATE headset)
    target_compile_options(t03-sources-test PRIVATE -Wall -Wextra)
    target_link_libraries(t03-sources-test PRIVATE PkgConfig::LIBNL PkgConfig::SYSTEMD)
    add_test(NAME t03-sources COMMAND t03-sources-test)

    # T06: LinkServer protocol logic (fake host env), vrlink host IPs, LayoutAuthority, the "link" config key
    add_executable(link-server-test
      tests/link_server_test.cpp
      headset/link_server.cpp
      headset/layout_authority.cpp
      headset/config.cpp
      headset/i18n.cpp
      headset/paths.cpp
    )
    target_include_directories(link-server-test PRIVATE headset)
    target_compile_options(link-server-test PRIVATE -Wall -Wextra)
    target_link_libraries(link-server-test PRIVATE fph-core)
    add_test(NAME link-server COMMAND link-server-test)
  endif()
endif()
