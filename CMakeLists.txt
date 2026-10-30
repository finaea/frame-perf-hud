cmake_minimum_required(VERSION 3.16)
project(frame-perf-hud VERSION 0.4.3 LANGUAGES CXX)

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
  core/stream_metrics.cpp   # T08: typed PC streaming snapshots and freshness
  core/layout_edit.cpp      # T05: move / insert / remove widgets, drop slots (headset editor, PC editor)
  core/camera_frame.cpp     # T10: camera ids, tiles, frame copy + dark filter, recheck gate, camera BLOB
  core/mjpeg_stream.cpp     # HTTP camera: MJPEG-over-HTTP answer -> JPEG frames
)
target_include_directories(fph-core PUBLIC core)
if(NOT MSVC)
  target_compile_options(fph-core PRIVATE -Wall -Wextra)
endif()

# T11: session logs (summaries + .csv.gz timelines). Apart from fph-core so only what writes logs needs zlib
find_package(ZLIB)
if(ZLIB_FOUND)
  add_library(fph-session STATIC core/session_log.cpp)
  target_link_libraries(fph-session PUBLIC fph-core ZLIB::ZLIB)
  if(NOT MSVC)
    target_compile_options(fph-session PRIVATE -Wall -Wextra)
  endif()
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
  pkg_check_modules(TURBOJPEG REQUIRED IMPORTED_TARGET libturbojpeg)  # T10: camera JPEGs for the PC
  find_package(Threads REQUIRED)  # T10: the camera thread

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
    headset/wifi_link.cpp
    headset/process_scan.cpp  # T03
    headset/hide_rules.cpp    # R2: Hide system rules (hide_rules.json)
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
    headset/camera_helper.cpp     # T09: the camera helper's agent side, --camera-helper-dump
    headset/camera_cache.cpp      # T10: dc civac copies of buffers the camera hardware wrote
    headset/camera_eye_ring.cpp   # T10: framestream's eye ring discovery (MIT, ported)
    headset/camera_service.cpp    # T10: the camera thread
    headset/camera_overlay.cpp    # T10: one overlay per camera tile
    headset/camera_link.cpp       # T10: camera BLOBs to the PCs
    headset/camera_dump.cpp       # T10: --camera-dump
    headset/http_camera.cpp       # HTTP camera (14): its MJPEG reader thread, --http-camera-bench
    headset/crash_recorder.cpp    # T11: the 1 Hz crash ring
    headset/power_off.cpp         # T11: the last power-off reason (qcom-pon lines, kernel journal)
    headset/session_service.cpp   # T11: session logs, Save / Record in the main loop
    headset/profile_store.cpp     # R2: layout profiles (profiles/*.json)
    vendor/frame-updater/cpp/update_check.cpp
  )
  target_include_directories(frame-perf-hud PRIVATE headset helper third_party/openvr vendor/frame-updater/cpp)
  target_compile_options(frame-perf-hud PRIVATE -Wall -Wextra)
  # The version printed by --version (from project(VERSION) above)
  target_compile_definitions(frame-perf-hud PRIVATE FRAME_PERF_HUD_VERSION="${PROJECT_VERSION}")
  if(NOT TARGET fph-session)
    message(FATAL_ERROR "the headset agent needs zlib (fph-session, the session logs)")
  endif()
  target_link_libraries(frame-perf-hud PRIVATE
    fph-core
    fph-session
    fph-render
    PkgConfig::CAIRO
    PkgConfig::FREETYPE
    PkgConfig::VULKAN
    PkgConfig::LIBNL
    PkgConfig::SYSTEMD
    PkgConfig::TURBOJPEG
    Threads::Threads
    "${OPENVR_LIB_DIR}/libopenvr_api.so"
  )
  # Use SteamVR's library at run time too. Only INSTALL_RPATH, from the build on: a build RPATH ends in an
  # empty element, and an empty element means "also search the current directory".
  set_target_properties(frame-perf-hud PROPERTIES
    BUILD_WITH_INSTALL_RPATH TRUE
    INSTALL_RPATH "${OPENVR_LIB_DIR}"
  )

  # Release size (upstream's PR #2, without its -Os): per-function sections so the linker drops what's unused,
  # plus LTO. On the agent and the static libs it pulls in, or gc/LTO miss most of the code. Not the helper.
  option(FPH_OPTIMIZE_SIZE "Release headset agent: --gc-sections and LTO" ON)
  if(FPH_OPTIMIZE_SIZE)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT FPH_IPO_SUPPORTED OUTPUT FPH_IPO_ERROR)
    if(NOT FPH_IPO_SUPPORTED)
      message(STATUS "LTO not supported here, --gc-sections only: ${FPH_IPO_ERROR}")
    endif()
    foreach(target frame-perf-hud fph-core fph-session fph-render)
      target_compile_options(${target} PRIVATE "$<$<CONFIG:Release>:-ffunction-sections;-fdata-sections>")
      if(FPH_IPO_SUPPORTED)
        set_property(TARGET ${target} PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
      endif()
    endforeach()
    target_link_options(frame-perf-hud PRIVATE $<$<CONFIG:Release>:-Wl,--gc-sections>)
  endif()

  install(TARGETS frame-perf-hud RUNTIME DESTINATION bin)

  # T09: the camera helper. Plain C, no libraries, kept tiny: installed once with sudo (install-camera-helper.sh)
  enable_language(C)
  add_executable(fph-camgrab helper/fph-camgrab.c)
  target_include_directories(fph-camgrab PRIVATE helper)
  target_compile_options(fph-camgrab PRIVATE -Wall -Wextra -Wshadow)
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

  # T11: session logs (summary, timeline, gzip, files, Record, end card)
  if(TARGET fph-session)
    add_executable(core-session-log-test tests/core_session_log_test.cpp)
    target_link_libraries(core-session-log-test PRIVATE fph-session)
    target_compile_options(core-session-log-test PRIVATE -Wall -Wextra)
    add_test(NAME core-session-log COMMAND core-session-log-test)
  endif()

  add_executable(core-processes-demand-test tests/core_processes_demand_test.cpp)
  target_link_libraries(core-processes-demand-test PRIVATE fph-core)
  target_compile_options(core-processes-demand-test PRIVATE -Wall -Wextra)
  add_test(NAME core-processes-demand COMMAND core-processes-demand-test)

  # jsonString escaping + round trip through parseJson
  add_executable(core-json-test tests/core_json_test.cpp)
  target_link_libraries(core-json-test PRIVATE fph-core)
  if(NOT MSVC)
    target_compile_options(core-json-test PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME core-json COMMAND core-json-test)

  # gpu.busy_pct from frame timing: formula, registry / demand contract
  add_executable(core-gpu-load-test tests/core_gpu_load_test.cpp)
  target_link_libraries(core-gpu-load-test PRIVATE fph-core)
  if(NOT MSVC)
    target_compile_options(core-gpu-load-test PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME core-gpu-load COMMAND core-gpu-load-test)

  # T04: layout model, JSON round trip, atomic save
  add_executable(core-layout-test tests/core_layout_test.cpp)
  target_link_libraries(core-layout-test PRIVATE fph-core)
  target_compile_options(core-layout-test PRIVATE -Wall -Wextra)
  add_test(NAME core-layout COMMAND core-layout-test)

  # HTTP camera (14): the MJPEG-over-HTTP splitter
  add_executable(core-mjpeg-stream-test tests/core_mjpeg_stream_test.cpp)
  target_link_libraries(core-mjpeg-stream-test PRIVATE fph-core)
  target_compile_options(core-mjpeg-stream-test PRIVATE -Wall -Wextra)
  add_test(NAME core-mjpeg-stream COMMAND core-mjpeg-stream-test)

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

  # T10: camera pieces in core (pitch copy, dark filter, recheck gate, tiles, BLOB format) and two cameras' BLOBs
  foreach(t camera-frame camera-blob camera-layout)
    string(REPLACE "-" "_" src ${t})
    add_executable(${t}-test tests/${src}_test.cpp)
    target_link_libraries(${t}-test PRIVATE fph-core)
    if(NOT MSVC)
      target_compile_options(${t}-test PRIVATE -Wall -Wextra)
    endif()
    add_test(NAME ${t} COMMAND ${t}-test)
  endforeach()

  if(TARGET fph-render)
    # T04: grid geometry, 2048 cap, widget draw rules (renders PNGs into a temp dir)
    add_executable(render-grid-test tests/render_grid_test.cpp)
    target_link_libraries(render-grid-test PRIVATE fph-render)
    target_compile_definitions(render-grid-test PRIVATE
      FPH_FONT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts")
    target_compile_options(render-grid-test PRIVATE -Wall -Wextra)
    add_test(NAME render-grid COMMAND render-grid-test)

    # T07: the Network widget's kbps / Mbps text
    add_executable(render-rate-text-test tests/render_rate_text_test.cpp)
    target_link_libraries(render-rate-text-test PRIVATE fph-render)
    if(NOT MSVC)
      target_compile_options(render-rate-text-test PRIVATE -Wall -Wextra)
    endif()
    add_test(NAME render-rate-text COMMAND render-rate-text-test)
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
      headset/hide_rules.cpp
      headset/stream_link.cpp
      headset/wifi_link.cpp
    )
    target_include_directories(t03-sources-test PRIVATE headset)
    target_compile_options(t03-sources-test PRIVATE -Wall -Wextra)
    target_link_libraries(t03-sources-test PRIVATE fph-core PkgConfig::LIBNL PkgConfig::SYSTEMD)
    add_test(NAME t03-sources COMMAND t03-sources-test)

    # R2 Hide system: glob / rules file / defaults vs the captured Frame process list / scanner drops hidden pids
    add_executable(hide-rules-test
      tests/hide_rules_test.cpp
      headset/process_scan.cpp
      headset/hide_rules.cpp
    )
    target_include_directories(hide-rules-test PRIVATE headset)
    target_compile_options(hide-rules-test PRIVATE -Wall -Wextra)
    target_link_libraries(hide-rules-test PRIVATE fph-core)
    add_test(NAME hide-rules COMMAND hide-rules-test ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures)

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

    # T09: the camera helper's parsers and command check (C), the agent side, and the real helper process
    # (uninstalled, so without the capability: protocol, EOF and parent-death exit; it never turns a camera on)
    add_executable(camgrab-helper-test tests/camgrab_helper_test.c)
    target_include_directories(camgrab-helper-test PRIVATE helper)
    target_compile_options(camgrab-helper-test PRIVATE -Wall -Wextra)
    add_test(NAME camgrab-helper COMMAND camgrab-helper-test)

    add_executable(camera-helper-test tests/camera_helper_test.cpp headset/camera_helper.cpp)
    target_include_directories(camera-helper-test PRIVATE headset helper)
    target_compile_options(camera-helper-test PRIVATE -Wall -Wextra)
    add_test(NAME camera-helper COMMAND camera-helper-test)

    add_executable(camgrab-process-test tests/camgrab_process_test.cpp headset/camera_helper.cpp)
    target_include_directories(camgrab-process-test PRIVATE headset helper)
    target_compile_options(camgrab-process-test PRIVATE -Wall -Wextra)
    add_dependencies(camgrab-process-test fph-camgrab)
    add_test(NAME camgrab-process COMMAND camgrab-process-test $<TARGET_FILE:fph-camgrab>)

    # T10: the eye ring port (synthetic arenas), the camera thread's mailbox / eventfd, the PC's camera feed
    add_executable(camera-eye-ring-test tests/camera_eye_ring_test.cpp headset/camera_eye_ring.cpp)
    target_include_directories(camera-eye-ring-test PRIVATE headset)
    target_compile_options(camera-eye-ring-test PRIVATE -Wall -Wextra)
    add_test(NAME camera-eye-ring COMMAND camera-eye-ring-test)

    add_executable(camera-service-mailbox-test
      tests/camera_service_mailbox_test.cpp
      headset/camera_service.cpp
      headset/camera_cache.cpp
      headset/camera_eye_ring.cpp
      headset/camera_helper.cpp
    )
    target_include_directories(camera-service-mailbox-test PRIVATE headset helper)
    target_compile_options(camera-service-mailbox-test PRIVATE -Wall -Wextra)
    target_link_libraries(camera-service-mailbox-test PRIVATE fph-core PkgConfig::TURBOJPEG Threads::Threads)
    add_test(NAME camera-service-mailbox COMMAND camera-service-mailbox-test)

    # HTTP camera (14): the reader thread against a local MJPEG server (reconnect, Unreachable / NotMjpeg, rotation)
    add_executable(http-camera-test tests/http_camera_test.cpp headset/http_camera.cpp)
    target_include_directories(http-camera-test PRIVATE headset)
    target_compile_options(http-camera-test PRIVATE -Wall -Wextra)
    target_link_libraries(http-camera-test PRIVATE fph-core PkgConfig::TURBOJPEG Threads::Threads)
    add_test(NAME http-camera COMMAND http-camera-test)

    # T10 review: the eye search's wake-up vs its work (no busy spin), the shown watermark reset with a fresh CAM
    add_executable(camera-review-fixes-test tests/camera_review_fixes_test.cpp)
    target_include_directories(camera-review-fixes-test PRIVATE headset)
    target_compile_options(camera-review-fixes-test PRIVATE -Wall -Wextra)
    target_link_libraries(camera-review-fixes-test PRIVATE fph-core)
    add_test(NAME camera-review-fixes COMMAND camera-review-fixes-test)

    add_executable(camera-tile-geometry-test tests/camera_tile_geometry_test.cpp)
    target_include_directories(camera-tile-geometry-test PRIVATE headset third_party/openvr)
    target_compile_options(camera-tile-geometry-test PRIVATE -Wall -Wextra)
    target_link_libraries(camera-tile-geometry-test PRIVATE fph-render)
    add_test(NAME camera-tile-geometry COMMAND camera-tile-geometry-test)

    add_executable(camera-feed-test tests/pc_camera_feed_test.cpp pc/camera_feed.cpp)
    target_include_directories(camera-feed-test PRIVATE pc)
    target_compile_options(camera-feed-test PRIVATE -Wall -Wextra)
    target_link_libraries(camera-feed-test PRIVATE fph-render PkgConfig::TURBOJPEG)
    add_test(NAME camera-feed COMMAND camera-feed-test)

    # T11: the crash recorder (ring file, marker, recovery) and the power-off reason decoder
    if(TARGET fph-session)
      add_executable(crash-recorder-test tests/crash_recorder_test.cpp headset/crash_recorder.cpp)
      target_include_directories(crash-recorder-test PRIVATE headset)
      target_compile_options(crash-recorder-test PRIVATE -Wall -Wextra)
      target_link_libraries(crash-recorder-test PRIVATE fph-session)
      add_test(NAME crash-recorder COMMAND crash-recorder-test)
    endif()

    add_executable(power-off-test tests/power_off_test.cpp headset/power_off.cpp)
    target_include_directories(power-off-test PRIVATE headset)
    target_compile_options(power-off-test PRIVATE -Wall -Wextra)
    target_link_libraries(power-off-test PRIVATE fph-core PkgConfig::SYSTEMD)
    add_test(NAME power-off COMMAND power-off-test)

    # T11 review: the session service's Record switch follows the writer
    add_executable(session-service-test tests/session_service_test.cpp headset/session_service.cpp
                   headset/crash_recorder.cpp headset/power_off.cpp)
    target_include_directories(session-service-test PRIVATE headset)
    target_compile_options(session-service-test PRIVATE -Wall -Wextra)
    target_link_libraries(session-service-test PRIVATE fph-session PkgConfig::SYSTEMD)
    add_test(NAME session-service COMMAND session-service-test)

    # R2: layout profiles (the store, with LayoutAuthority as a PC's LAYOUT_SET / Reset reach it)
    add_executable(profile-store-test
      tests/profile_store_test.cpp
      headset/profile_store.cpp
      headset/layout_authority.cpp
      headset/config.cpp
      headset/i18n.cpp
      headset/paths.cpp
    )
    target_include_directories(profile-store-test PRIVATE headset)
    target_compile_options(profile-store-test PRIVATE -Wall -Wextra)
    target_link_libraries(profile-store-test PRIVATE fph-core)
    add_test(NAME profile-store COMMAND profile-store-test)
  endif()
endif()

# T07: the Windows app (pc/). Build it with pc\build.cmd (MSVC + the vcpkg manifest in pc/, x64-windows-static).
option(FPH_BUILD_PC "Build the Windows app in pc/ (needs vcpkg's cairo + freetype; see pc\\build.cmd)" OFF)
if(WIN32 AND FPH_BUILD_PC)
  add_subdirectory(pc)
endif()
