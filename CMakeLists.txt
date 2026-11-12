cmake_minimum_required(VERSION 3.20)
project(RE5Trainer LANGUAGES C CXX)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR "Build with the Win32 (x86) toolchain: re5dx9.exe is a 32-bit process.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_compile_options(/utf-8 /W3 /permissive-)
add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

add_subdirectory(third_party/minhook EXCLUDE_FROM_ALL)

set(IMGUI third_party/imgui)
add_library(imgui STATIC
  ${IMGUI}/imgui.cpp ${IMGUI}/imgui_draw.cpp ${IMGUI}/imgui_tables.cpp ${IMGUI}/imgui_widgets.cpp
  ${IMGUI}/backends/imgui_impl_dx9.cpp ${IMGUI}/backends/imgui_impl_win32.cpp)
target_include_directories(imgui PUBLIC ${IMGUI} ${IMGUI}/backends)

add_library(RE5Trainer SHARED
  src/dllmain.cpp
  src/common/log.cpp
  src/common/mem.cpp
  src/common/game.cpp
  src/common/hotkeys.cpp
  src/common/settings.cpp
  src/common/features.cpp
  src/common/i18n.cpp
  src/hooks/d3d9_hook.cpp
  src/hooks/dinput_hook.cpp
  src/ui/overlay.cpp
  src/features/ammo.cpp
  src/features/diagnostics.cpp
  src/features/patch_features.cpp
  src/features/player.cpp
  src/features/enemies.cpp
  src/features/mercenaries.cpp
  src/features/action_log.cpp
  src/features/swaps.cpp
  src/features/camera.cpp
  src/features/body_cut.cpp
  src/features/inventory.cpp
  src/features/licker.cpp
  src/features/dying.cpp
  src/features/knife.cpp
  src/features/quick_knife.cpp
  src/features/parry.cpp
  src/features/combo.cpp
  src/features/beta_anims.cpp
  src/features/pickup.cpp
  src/features/menu_keys.cpp
  src/features/movement.cpp
  src/features/grabs.cpp
  src/features/fire_rate.cpp
  src/features/enemy_spawn.cpp
  src/features/re6_motions.cpp
  src/features/melee_tuning.cpp
  src/features/time_speed.cpp
  src/features/wesker_moves.cpp
  src/features/hud_colors.cpp
  src/features/wesker_boss.cpp
  src/features/stamina.cpp
  src/features/coop_solo.cpp
  src/features/belt_lamp.cpp
  src/features/crank.cpp
  src/features/levers.cpp
  src/features/obstacle_climb.cpp
  src/features/env_exec.cpp
  src/features/per_character.cpp
  src/features/final_fight.cpp
  src/features/boat.cpp
  src/features/raft.cpp
  src/features/solo_scenario.cpp
  src/features/knife_icon.cpp
  src/features/perfect_dodge.cpp
  src/features/shell_reload.cpp
  src/features/jill_device.cpp
  src/features/forced_death.cpp
  src/features/rocket_catch.cpp
  src/common/actors.cpp
  src/common/tracer.cpp
  src/common/crashlog.cpp
  src/common/wav.cpp
  src/common/arc.cpp)
target_include_directories(RE5Trainer PRIVATE src)
target_link_libraries(RE5Trainer PRIVATE imgui minhook d3d9 dinput8 dxguid version user32 gdi32 shell32 winmm)
# A map of the DLL (crash.log addresses -> functions: build/Release/RE5Trainer.map).
target_link_options(RE5Trainer PRIVATE /MAP)

add_executable(RE5TrainerLoader WIN32 src/loader/loader.cpp)
target_link_libraries(RE5TrainerLoader PRIVATE user32 shell32)
