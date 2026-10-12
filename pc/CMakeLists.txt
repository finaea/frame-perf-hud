# The Windows app (ticket 07). Added by the root CMakeLists on WIN32 with FPH_BUILD_PC=ON; build it with
# pc\build.cmd (MSVC, vcpkg manifest in this folder, triplet x64-windows-static, static CRT).

enable_language(RC)

if(NOT TARGET fph-render)
  message(FATAL_ERROR "the PC app needs cairo + freetype (fph-render): run pc\\build.cmd, which points CMake at vcpkg")
endif()

# Everything but WinMain and the window: the tests link this.
add_library(fph-pc-lib STATIC
  link_client.cpp
  settings.cpp
  editor_model.cpp
  panel_view.cpp
  pc_editor.cpp
  # The headset's UI strings, so the PC editor says exactly what the dashboard editor says (platform-free)
  ${CMAKE_SOURCE_DIR}/headset/i18n.cpp
  log.cpp
  win_text.cpp
  udp_socket.cpp
  single_instance.cpp
  platform.cpp
)
target_include_directories(fph-pc-lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(fph-pc-lib PRIVATE ${CMAKE_SOURCE_DIR}/headset)  # i18n.h only
target_link_libraries(fph-pc-lib PUBLIC fph-core fph-render ws2_32 iphlpapi shell32 advapi32 ole32)
target_compile_definitions(fph-pc-lib PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)

# The fonts are embedded as resources (one exe, nothing else to ship)
set(FPH_FONT_DIR "${CMAKE_SOURCE_DIR}/assets/fonts")
configure_file(app.rc.in ${CMAKE_CURRENT_BINARY_DIR}/app.rc @ONLY)

add_executable(frame-perf-hud-pc WIN32
  main.cpp
  app_window.cpp
  settings_dialog.cpp
  ${CMAKE_CURRENT_BINARY_DIR}/app.rc
)
target_include_directories(frame-perf-hud-pc PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(frame-perf-hud-pc PRIVATE fph-pc-lib user32 gdi32 comctl32 shcore dwmapi)
target_compile_definitions(frame-perf-hud-pc PRIVATE FRAME_PERF_HUD_VERSION="${PROJECT_VERSION}")
set_target_properties(frame-perf-hud-pc PROPERTIES OUTPUT_NAME "FramePerfHud")
# The manifest (per-monitor-v2 DPI, common controls 6) comes from app.rc; don't let the linker add a second one
if(MSVC)
  target_link_options(frame-perf-hud-pc PRIVATE /MANIFEST:NO)
endif()

if(BUILD_TESTING)
  # Tests live in tests/pc_*.cpp (written alongside, ticket 07)
  file(GLOB FPH_PC_TESTS ${CMAKE_SOURCE_DIR}/tests/pc_*_test.cpp)
  foreach(test_src ${FPH_PC_TESTS})
    get_filename_component(test_name ${test_src} NAME_WE)
    string(REPLACE "_" "-" test_target ${test_name})
    add_executable(${test_target} ${test_src})
    target_link_libraries(${test_target} PRIVATE fph-pc-lib)
    add_test(NAME ${test_target} COMMAND ${test_target})
  endforeach()
endif()

# pc\build.cmd builds this: the exe and the PC tests (core's tests still pass -Wall -Wextra unguarded, which cl rejects)
add_custom_target(fph-pc-all ALL)
add_dependencies(fph-pc-all frame-perf-hud-pc)
if(BUILD_TESTING)
  foreach(test_src ${FPH_PC_TESTS})
    get_filename_component(test_name ${test_src} NAME_WE)
    string(REPLACE "_" "-" test_target ${test_name})
    add_dependencies(fph-pc-all ${test_target})
  endforeach()
endif()
