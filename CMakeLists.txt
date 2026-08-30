cmake_minimum_required(VERSION 3.21)

# Set the default toolchain to use the Vcpkg submodule.
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE
        "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg/scripts/buildsystems/vcpkg.cmake"
        CACHE STRING "Vcpkg toolchain file")
endif()

project(
    roguelike
    LANGUAGES C CXX
)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin")

file(
    GLOB_RECURSE SOURCE_FILES
    CONFIGURE_DEPENDS
    ${PROJECT_SOURCE_DIR}/src/*.cpp
    ${PROJECT_SOURCE_DIR}/src/*.hpp
)
add_executable(${PROJECT_NAME} ${SOURCE_FILES})

target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_17)

if (MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /utf-8 /W4)
else()
    target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra)
endif()

find_package(SDL3 CONFIG REQUIRED)
find_package(libtcod CONFIG REQUIRED)
target_link_libraries(${PROJECT_NAME} PRIVATE SDL3::SDL3 libtcod::libtcod)

# --- roguelike_tests: minimal, dependency-free unit tests for rules.hpp's pure math ---
#
# Deliberately its own executable rather than folded into src/'s glob: the main binary
# has exactly one main() (main.cpp), and this target needs its own. It links only
# rules.cpp + rng.cpp — the actual dependency closure of rules.hpp's declarations (see
# tests/rules_test.cpp) — not SDL3/libtcod, so it builds and runs with no display at all.
# `ctest` (or just running the binary directly) reports pass/fail; see tests/rules_test.cpp
# for what's covered and why this is the one layer that gets real unit tests today.
enable_testing()
add_executable(roguelike_tests
    tests/rules_test.cpp
    src/rules.cpp
    src/rng.cpp
)
target_include_directories(roguelike_tests PRIVATE src)
target_compile_features(roguelike_tests PRIVATE cxx_std_17)
# entity.hpp (transitively included via rules.hpp) uses tcod::ColorRGB, so this needs
# libtcod's headers even though none of its actual drawing code is ever called or linked.
target_link_libraries(roguelike_tests PRIVATE libtcod::libtcod)
if (MSVC)
    target_compile_options(roguelike_tests PRIVATE /utf-8 /W4)
else()
    target_compile_options(roguelike_tests PRIVATE -Wall -Wextra)
endif()
add_test(NAME rules_test COMMAND roguelike_tests)

# --- roguelike_game_tests: headless integration tests over GameState -----------------
#
# The testing overhaul's Phase 0+ (see CLAUDE.md's "Testing overhaul" section): combat
# resolution, AI behavior, movement/travel and world-generation invariants, all driven
# through the real GameState/end_turn()/resolve_attack() entry points via the
# tests/arena.* helpers instead of a parallel simulation. Links the full headless logic
# layer — everything except main.cpp and the render*.cpp/input*.cpp files, which are the
# only parts of src/ that touch SDL or a real console — the same closure --dump-loot
# already proves out with no window at all. Confirmed empirically before adding this:
# none of these .cpp files include anything SDL-related.
add_executable(roguelike_game_tests
    tests/game_test.cpp
    tests/arena.cpp
    src/entity.cpp
    src/rng.cpp
    src/rules.cpp
    src/content.cpp
    src/spells.cpp
    src/actors.cpp
    src/projectile.cpp
    src/map.cpp
    src/level.cpp
    src/game.cpp
    src/turn.cpp
    src/run_history.cpp
)
target_include_directories(roguelike_game_tests PRIVATE src tests)
target_compile_features(roguelike_game_tests PRIVATE cxx_std_17)
target_link_libraries(roguelike_game_tests PRIVATE libtcod::libtcod)
if (MSVC)
    target_compile_options(roguelike_game_tests PRIVATE /utf-8 /W4)
else()
    target_compile_options(roguelike_game_tests PRIVATE -Wall -Wextra)
endif()
add_test(NAME game_test COMMAND roguelike_game_tests)

# --- roguelike_ai_tests: Phase 2 of the testing overhaul — monster/minion AI behavior -
#
# Same headless closure as roguelike_game_tests (see its own comment above); split into
# its own binary/executable rather than folded into game_test.cpp so ctest reports each
# phase's suite separately, and so no single test file grows unmanageably long as more
# phases land.
add_executable(roguelike_ai_tests
    tests/ai_test.cpp
    tests/arena.cpp
    src/entity.cpp
    src/rng.cpp
    src/rules.cpp
    src/content.cpp
    src/spells.cpp
    src/actors.cpp
    src/projectile.cpp
    src/map.cpp
    src/level.cpp
    src/game.cpp
    src/turn.cpp
    src/run_history.cpp
)
target_include_directories(roguelike_ai_tests PRIVATE src tests)
target_compile_features(roguelike_ai_tests PRIVATE cxx_std_17)
target_link_libraries(roguelike_ai_tests PRIVATE libtcod::libtcod)
if (MSVC)
    target_compile_options(roguelike_ai_tests PRIVATE /utf-8 /W4)
else()
    target_compile_options(roguelike_ai_tests PRIVATE -Wall -Wextra)
endif()
add_test(NAME ai_test COMMAND roguelike_ai_tests)

# --- roguelike_movement_tests: Phase 3 of the testing overhaul — Shift+direction travel
#
# The addition here beyond roguelike_game_tests'/roguelike_ai_tests' own closure is the
# whole input.cpp/input_*.cpp group, for run_in_direction() itself (declared for tests in
# input.hpp — see its own comment); handle_event()'s switch in input.cpp references every
# other mode's handler, so all four satellites have to come along too, even though this
# suite never calls any of them. Every one of these files includes <SDL3/SDL.h> for
# SDLK_*/SDL_KMOD_* constants and the SDL_Event type, so this target needs SDL3 — but
# nothing this suite's tests actually exercise calls a real SDL runtime function (no
# SDL_Init(), no window), same headless spirit as every other test target here.
find_package(SDL3 CONFIG REQUIRED)
add_executable(roguelike_movement_tests
    tests/movement_test.cpp
    tests/arena.cpp
    src/entity.cpp
    src/rng.cpp
    src/rules.cpp
    src/content.cpp
    src/spells.cpp
    src/actors.cpp
    src/projectile.cpp
    src/map.cpp
    src/level.cpp
    src/game.cpp
    src/turn.cpp
    src/run_history.cpp
    src/input.cpp
    src/input_item_menus.cpp
    src/input_spell_minion_menus.cpp
    src/input_screens.cpp
    src/input_overlays.cpp
)
target_include_directories(roguelike_movement_tests PRIVATE src tests)
target_compile_features(roguelike_movement_tests PRIVATE cxx_std_17)
target_link_libraries(roguelike_movement_tests PRIVATE libtcod::libtcod SDL3::SDL3)
if (MSVC)
    target_compile_options(roguelike_movement_tests PRIVATE /utf-8 /W4)
else()
    target_compile_options(roguelike_movement_tests PRIVATE -Wall -Wextra)
endif()
add_test(NAME movement_test COMMAND roguelike_movement_tests)

# --- dump_loot_golden: automates what used to be a manual regression check -----------
#
# Phase 4 of the testing overhaul: --seed=42 --dump-loot across floors 1-10, diffed
# against a checked-in golden file (tests/golden/dump_loot_seed42.txt) instead of a human
# running it by hand and eyeballing the diff (see CLAUDE.md's Testing overhaul section).
# Runs the real ${PROJECT_NAME} binary (a shell script, not a CMake source list, since
# this is driving the actual game executable rather than linking against its logic
# layer) — $<TARGET_FILE:...> resolves to wherever it actually gets built, so this stays
# correct across platforms/generators without hardcoding bin/roguelike.
add_test(NAME dump_loot_golden
         COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_dump_loot_golden.sh
                 $<TARGET_FILE:${PROJECT_NAME}>
                 ${CMAKE_CURRENT_SOURCE_DIR}/tests/golden/dump_loot_seed42.txt)

# --- roguelike_generation_tests: Phase 4's other half — multi-seed generation invariants
#
# Same headless closure as roguelike_game_tests, plus generate_level() itself
# (level.cpp already in that closure) as the entry point under test.
add_executable(roguelike_generation_tests
    tests/generation_test.cpp
    src/entity.cpp
    src/rng.cpp
    src/rules.cpp
    src/content.cpp
    src/spells.cpp
    src/actors.cpp
    src/projectile.cpp
    src/map.cpp
    src/level.cpp
    src/game.cpp
    src/turn.cpp
    src/run_history.cpp
)
target_include_directories(roguelike_generation_tests PRIVATE src)
target_compile_features(roguelike_generation_tests PRIVATE cxx_std_17)
target_link_libraries(roguelike_generation_tests PRIVATE libtcod::libtcod)
if (MSVC)
    target_compile_options(roguelike_generation_tests PRIVATE /utf-8 /W4)
else()
    target_compile_options(roguelike_generation_tests PRIVATE -Wall -Wextra)
endif()
add_test(NAME generation_test COMMAND roguelike_generation_tests)
