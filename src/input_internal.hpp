#pragma once

// Private contract between input.cpp and its per-mode-group satellite files
// (input_item_menus.cpp, input_spell_minion_menus.cpp, input_screens.cpp,
// input_overlays.cpp) — NOT part of the public input API (that's input.hpp; nothing
// outside this small file group includes this header). Each mode still gets exactly one
// handler function, chosen by exactly one switch in handle_event() (input.cpp) —
// splitting the ~20 of them across files by mode-group is purely a file-size cut (see
// CLAUDE.md's "Known limitations" note this addresses), not a change to that
// architecture. A function declared here has external linkage instead of living in its
// file's own anonymous namespace purely so another translation unit can reach it;
// nothing else outside these five files should call one directly.

#include <SDL3/SDL.h>

#include <vector>

#include "game.hpp"

// Defined in input.cpp, next to its sibling would_see_hostile_from()/run_in_direction()
// (all three private to that file) — shared here because both handle_playing_input()
// ('o'/'p' from Mode::Playing) and input_overlays.cpp's handle_minion_focus_input()
// (the same keys working *inside* Mode::MinionFocus) need it. See its doc comment in
// input.cpp for what it actually does.
bool cycle_minion_focus(GameState& gs, int direction);

// Defined in input_spell_minion_menus.cpp, next to the ability menu it serves — shared
// here because input_overlays.cpp's handle_minion_focus_input() also needs it, to decide
// whether 'z' has anything to open.
std::vector<int> focused_minion_abilities(GameState& gs);

// --- input_item_menus.cpp: WeaponMenu, ArmorMenu, PotionMenu, Drop, Pickup ---
void handle_weapon_menu_input(GameState& gs, const SDL_Event& event);
void handle_armor_menu_input(GameState& gs, const SDL_Event& event);
void handle_potion_menu_input(GameState& gs, const SDL_Event& event);
void handle_drop_input(GameState& gs, const SDL_Event& event);
void handle_pickup_input(GameState& gs, const SDL_Event& event);

// --- input_spell_minion_menus.cpp: LevelUp, SchoolChoice, SpellMenu, MinionRoster,
//     MinionAbilityMenu ---
void handle_level_up_input(GameState& gs, const SDL_Event& event);
void handle_school_choice_input(GameState& gs, const SDL_Event& event);
void handle_spell_menu_input(GameState& gs, const SDL_Event& event);
void handle_minion_roster_input(GameState& gs, const SDL_Event& event);
void handle_minion_ability_menu_input(GameState& gs, const SDL_Event& event);

// --- input_screens.cpp: StartMenu, SetSeed, RunHistory, Dead, Win, MessageLog, Help ---
void handle_start_menu_input(GameState& gs, const SDL_Event& event);
void handle_set_seed_input(GameState& gs, const SDL_Event& event);
void handle_run_history_input(GameState& gs, const SDL_Event& event);
void handle_dead_input(GameState& gs, const SDL_Event& event);
void handle_win_input(GameState& gs, const SDL_Event& event);
void handle_message_log_input(GameState& gs, const SDL_Event& event);
void handle_help_input(GameState& gs, const SDL_Event& event);

// --- input_overlays.cpp: Targeting, RangedAttack, MinionFocus, Look (the modes that
//     overlay the Playing HUD instead of taking over the whole screen) ---
void handle_targeting_input(GameState& gs, const SDL_Event& event);
void handle_ranged_input(GameState& gs, const SDL_Event& event);
void handle_minion_focus_input(GameState& gs, const SDL_Event& event);
void handle_look_input(GameState& gs, const SDL_Event& event);
