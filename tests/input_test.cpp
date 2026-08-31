// Full input-layer coverage (see CLAUDE.md's "Testing overhaul" section): synthesized
// SDL_Events driving handle_event() through every one of input.cpp's satellite files —
// input_item_menus.cpp, input_spell_minion_menus.cpp, input_screens.cpp,
// input_overlays.cpp — not just the open/close roundtrip shape this file started with.
// SDL_Event is a plain struct — no SDL_Init(), no window, no real display needed to
// construct one and hand it to handle_event() directly, the same headless spirit as
// tests/movement_test.cpp (which already proved input.cpp/its satellites link and run
// fine with nothing but SDL's headers).
//
// Still not literally exhaustive — this doesn't attempt every keycode/modifier
// combination a real keyboard could send, and rendering/visual feedback stays out of
// scope by design (see tests/render_test.cpp) — but every distinct branch in every
// handler function gets exercised at least once.

#include <SDL3/SDL.h>

#include <cstdio>
#include <string>
#include <vector>

#include "actors.hpp"
#include "arena.hpp"
#include "content.hpp"
#include "game.hpp"
#include "input.hpp"
#include "level.hpp"
#include "rules.hpp"
#include "spells.hpp"

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool condition, const std::string& description) {
  ++g_checks;
  if (!condition) {
    ++g_failures;
    std::fprintf(stderr, "FAIL: %s\n", description.c_str());
  }
}

SDL_Event key_down(SDL_Keycode key, SDL_Keymod mod = SDL_KMOD_NONE) {
  SDL_Event event{};
  event.type = SDL_EVENT_KEY_DOWN;
  event.key.key = key;
  event.key.mod = mod;
  return event;
}

int monster_index_named(const std::string& name) {
  for (size_t i = 0; i < kMonsterTable.size(); ++i) {
    if (kMonsterTable[i].name == name) return static_cast<int>(i);
  }
  return -1;
}

int spell_index_named(const std::string& name) {
  for (size_t i = 0; i < kSpellTable.size(); ++i) {
    if (kSpellTable[i].name == name) return static_cast<int>(i);
  }
  return -1;
}

int weapon_index_named(const std::string& name) {
  for (size_t i = 0; i < kWeaponTable.size(); ++i) {
    if (kWeaponTable[i].name == name) return static_cast<int>(i);
  }
  return -1;
}

// Which letter key opens `spell_name` from the SpellMenu, given gs.player's current
// intelligence/chosen_school — looked up through the real known_spell_indices() rather
// than a hardcoded letter, so this can't silently start testing the wrong row if the
// spell table is ever reordered.
SDL_Keycode letter_for_known_spell(GameState& gs, const std::string& spell_name) {
  auto known = known_spell_indices(gs.player.intelligence, gs.player.chosen_school);
  int target = spell_index_named(spell_name);
  for (size_t i = 0; i < known.size(); ++i) {
    if (known[i] == target) return static_cast<SDL_Keycode>(SDLK_A + i);
  }
  check(false, "test setup: \"" + spell_name + "\" should be in known_spell_indices() for this test's stats");
  return SDLK_ESCAPE;
}

std::vector<std::string> make_wide_room(int width) {
  return {
      std::string(static_cast<size_t>(width), '#'),
      "#" + std::string(static_cast<size_t>(width - 2), '.') + "#",
      std::string(static_cast<size_t>(width), '#'),
  };
}

const std::vector<std::string> kRoom = {
    "##########",
    "#........#",
    "#........#",
    "##########",
};

const std::vector<std::string> kBigRoom = {
    "#######",
    "#.....#",
    "#.....#",
    "#.....#",
    "#.....#",
    "#######",
};

// --- Menu open/close roundtrips (pre-existing coverage) ---------------------------------

// Trigger `key` from Mode::Playing, confirm it opens `expected_mode`, then send Escape
// and confirm it lands back on Mode::Playing — the open/close roundtrip every full-screen
// menu is supposed to support (see CLAUDE.md's "Adding a full-screen menu" note).
void check_menu_roundtrip(GameState& gs, SDL_Keycode trigger_key, Mode expected_mode, const std::string& menu_name) {
  gs.mode = Mode::Playing;
  handle_event(gs, key_down(trigger_key));
  check(gs.mode == expected_mode, menu_name + ": trigger key opens it from Mode::Playing");
  handle_event(gs, key_down(SDLK_ESCAPE));
  check(gs.mode == Mode::Playing, menu_name + ": Escape closes it back to Mode::Playing");
}

void test_menu_open_close_roundtrips() {
  GameState gs = arena::make_gamestate(kRoom, 2, 1);
  gs.player.weapons.push_back(kWeaponTable[0]);
  gs.player.armors.push_back(kArmorTable[0]);
  gs.player.potions.push_back(kPotionTable[0]);
  gs.player.intelligence = 10;
  gs.player.chosen_school = SpellSchool::Summoner;
  arena::place_minion(gs, kMinionTable[0], 4, 1);  // so 'm' has a roster to open

  check_menu_roundtrip(gs, SDLK_W, Mode::WeaponMenu, "WeaponMenu ('w')");
  check_menu_roundtrip(gs, SDLK_A, Mode::ArmorMenu, "ArmorMenu ('a')");
  check_menu_roundtrip(gs, SDLK_Q, Mode::PotionMenu, "PotionMenu ('q')");
  check_menu_roundtrip(gs, SDLK_D, Mode::Drop, "Drop ('d')");
  check_menu_roundtrip(gs, SDLK_Z, Mode::SpellMenu, "SpellMenu ('z')");
  check_menu_roundtrip(gs, SDLK_M, Mode::MinionRoster, "MinionRoster ('m')");
  check_menu_roundtrip(gs, SDLK_RIGHTBRACKET, Mode::MessageLog, "MessageLog (']')");
  check_menu_roundtrip(gs, SDLK_X, Mode::Look, "Look ('x')");

  // '?' is Shift+/ on a US layout — handle_help_input() accepts either the dedicated
  // keycode or the unshifted one with the modifier, so exercise the trigger the same way
  // (see handle_playing_input()'s own comment on this).
  gs.mode = Mode::Playing;
  handle_event(gs, key_down(SDLK_QUESTION));
  check(gs.mode == Mode::Help, "Help ('?'): trigger key opens it from Mode::Playing");
  handle_event(gs, key_down(SDLK_ESCAPE));
  check(gs.mode == Mode::Playing, "Help ('?'): Escape closes it back to Mode::Playing");
}

// 'm' with no minions at all doesn't open the roster — it's rejected with a message and
// stays in Mode::Playing (see handle_playing_input()'s own guard). A regression here
// would mean either an empty roster silently opens (nothing to show) or a real roster
// silently gets refused.
void test_minion_roster_requires_a_minion() {
  GameState gs = arena::make_gamestate(kRoom, 1, 1);  // no minions placed
  handle_event(gs, key_down(SDLK_M));
  check(gs.mode == Mode::Playing,
        "'m' with no minions on the floor stays in Mode::Playing rather than opening an empty roster");
}

// --- Item menus (input_item_menus.cpp) ---------------------------------------------------

void test_weapon_menu_equip_swaps_old_into_inventory() {
  GameState gs = arena::make_gamestate(kRoom, 1, 1);
  gs.player.weapons.push_back(kWeaponTable[weapon_index_named("Dagger")]);
  gs.player.weapons.push_back(kWeaponTable[weapon_index_named("Short Sword")]);

  gs.mode = Mode::WeaponMenu;
  handle_event(gs, key_down(SDLK_B));  // slot b = weapons[0] = Dagger
  check(gs.player.weapon.name == "Dagger", "equipping slot b picks up the first carried weapon");
  check(gs.player.weapons.size() == 1 && gs.player.weapons[0].name == "Short Sword",
        "the Dagger left the inventory; the old (intrinsic) Fists aren't added back");
  check(gs.mode == Mode::Playing, "equipping closes the menu");

  gs.mode = Mode::WeaponMenu;
  handle_event(gs, key_down(SDLK_B));  // now slot b = weapons[0] = Short Sword
  check(gs.player.weapon.name == "Short Sword", "equipping slot b again picks up the next carried weapon");
  check(gs.player.weapons.size() == 1 && gs.player.weapons[0].name == "Dagger",
        "the previously-equipped Dagger returns to inventory since it's not intrinsic");

  gs.mode = Mode::WeaponMenu;
  handle_event(gs, key_down(SDLK_A));  // slot a = Fists
  check(gs.player.weapon.name == "Fists", "equipping slot a always means Fists");
  check(gs.player.weapons.size() == 2, "the Short Sword also returns to inventory");

  gs.mode = Mode::WeaponMenu;
  handle_event(gs, key_down(SDLK_Z));  // no such slot
  check(gs.mode == Mode::WeaponMenu, "an out-of-range slot letter does nothing");

  gs.mode = Mode::WeaponMenu;
  handle_event(gs, key_down(SDLK_ESCAPE));
  check(gs.mode == Mode::Playing, "Escape closes without equipping anything");
}

void test_armor_menu_equip_swaps_old_into_inventory() {
  GameState gs = arena::make_gamestate(kRoom, 1, 1);
  gs.player.armors.push_back(kArmorTable[0]);
  gs.player.armors.push_back(kArmorTable[1]);

  gs.mode = Mode::ArmorMenu;
  handle_event(gs, key_down(SDLK_B));
  check(gs.player.armor.name == kArmorTable[0].name, "equipping slot b picks up the first carried armor");
  check(gs.player.armors.size() == 1 && gs.player.armors[0].name == kArmorTable[1].name,
        "the equipped armor left the inventory; the old (intrinsic) Nothing isn't added back");
  check(gs.mode == Mode::Playing, "equipping closes the menu");

  gs.mode = Mode::ArmorMenu;
  handle_event(gs, key_down(SDLK_A));  // slot a = Nothing
  check(gs.player.armor.name == "Nothing", "equipping slot a always means Nothing (no armor)");
  check(gs.player.armors.size() == 2, "the previously-equipped armor returns to inventory");

  gs.mode = Mode::ArmorMenu;
  handle_event(gs, key_down(SDLK_ESCAPE));
  check(gs.mode == Mode::Playing, "Escape closes without equipping anything");
}

void test_potion_menu_drinks_and_removes() {
  GameState gs = arena::make_gamestate(kRoom, 1, 1);
  gs.player.potions.push_back(kPotionTable[0]);  // Heal Potion
  gs.player.max_hp = 100;
  gs.player.hp = 40;

  gs.mode = Mode::PotionMenu;
  handle_event(gs, key_down(SDLK_A));
  check(gs.player.hp > 40, "drinking the Heal Potion raised HP");
  check(gs.player.potions.empty(), "the potion is consumed on drinking");
  check(gs.mode == Mode::Playing, "drinking closes the menu");

  gs.mode = Mode::PotionMenu;
  handle_event(gs, key_down(SDLK_A));  // now nothing in that slot
  check(gs.mode == Mode::PotionMenu, "an empty potion slot does nothing");

  gs.mode = Mode::PotionMenu;
  handle_event(gs, key_down(SDLK_ESCAPE));
  check(gs.mode == Mode::Playing, "Escape closes without drinking anything");
}

void test_drop_each_item_kind() {
  GameState gs = arena::make_gamestate(kRoom, 1, 1);
  gs.player.weapon = kWeaponTable[weapon_index_named("Dagger")];
  gs.player.armor = kArmorTable[0];
  gs.player.potions.push_back(kPotionTable[0]);

  // drop_slots() order: equipped weapon (non-intrinsic), weapons[], equipped armor
  // (non-intrinsic), armors[], potions[] — with none carried beyond the equipped pair,
  // slot 'a' is the weapon first.
  gs.mode = Mode::Drop;
  handle_event(gs, key_down(SDLK_A));
  check(gs.player.weapon.name == "Fists", "dropping the equipped weapon reverts to Fists");
  check(gs.level().items.size() == 1 && gs.level().items[0].weapon.name == "Dagger",
        "the dropped weapon lands on the player's own tile");
  check(gs.mode == Mode::Playing, "dropping the weapon closes the menu");

  gs.mode = Mode::Drop;
  handle_event(gs, key_down(SDLK_A));  // now slot a = the equipped armor
  check(gs.player.armor.name == "Nothing", "dropping the equipped armor reverts to Nothing");
  check(gs.level().armor_items.size() == 1, "the dropped armor lands on the ground");

  gs.mode = Mode::Drop;
  handle_event(gs, key_down(SDLK_A));  // now slot a = the potion
  check(gs.player.potions.empty(), "dropping the potion removes it from inventory");
  check(gs.level().potions.size() == 1, "the dropped potion lands on the ground");

  gs.mode = Mode::Drop;
  handle_event(gs, key_down(SDLK_A));  // nothing left to drop
  check(gs.mode == Mode::Drop, "an empty drop slot does nothing");

  gs.mode = Mode::Drop;
  handle_event(gs, key_down(SDLK_ESCAPE));
  check(gs.mode == Mode::Playing, "Escape closes without dropping anything");
}

void test_pickup_toggle_shift_a_and_enter() {
  GameState gs = arena::make_gamestate(kRoom, 1, 1);
  gs.level().items.push_back(GroundItem{1, 1, kWeaponTable[0]});
  gs.level().armor_items.push_back(GroundArmor{1, 1, kArmorTable[0]});
  auto slots = ground_slots_at(gs.level(), 1, 1);
  check(slots.size() == 2, "test setup: two items on the ground");

  gs.pickup_selected.assign(slots.size(), true);
  gs.mode = Mode::Pickup;
  handle_event(gs, key_down(SDLK_A));
  check(!gs.pickup_selected[0], "a letter toggles that slot off");
  handle_event(gs, key_down(SDLK_A));
  check(gs.pickup_selected[0], "pressing it again toggles it back on");

  handle_event(gs, key_down(SDLK_A, SDL_KMOD_SHIFT));
  check(!gs.pickup_selected[0] && !gs.pickup_selected[1], "Shift+A with everything checked unchecks everything");
  handle_event(gs, key_down(SDLK_A, SDL_KMOD_SHIFT));
  check(gs.pickup_selected[0] && gs.pickup_selected[1], "Shift+A again re-checks everything");

  gs.pickup_selected.assign(slots.size(), false);
  gs.mode = Mode::Pickup;
  handle_event(gs, key_down(SDLK_RETURN));
  check(gs.mode == Mode::Playing, "Enter with nothing checked is a free cancel");
  check(gs.level().items.size() == 1, "nothing was actually taken");

  gs.pickup_selected.assign(slots.size(), true);
  gs.mode = Mode::Pickup;
  handle_event(gs, key_down(SDLK_RETURN));
  check(gs.mode == Mode::Playing, "Enter with everything checked closes the menu");
  check(gs.level().items.empty() && gs.level().armor_items.empty(), "everything checked was picked up");
  check(gs.player.weapons.size() == 1 && gs.player.armors.size() == 1, "both picked-up items land in the right inventory list");

  gs.mode = Mode::Pickup;
  handle_event(gs, key_down(SDLK_ESCAPE));
  check(gs.mode == Mode::Playing, "Escape cancels for free — nothing left to take anyway here");
}

// --- LevelUp / SchoolChoice (input_spell_minion_menus.cpp) --------------------------------

void test_level_up_shift_letters_and_lowercase_rejected() {
  GameState gs = arena::make_gamestate(kRoom, 1, 1);
  gs.player.intelligence = 10;
  // A school must already be chosen, or Shift+I below re-triggers the school-choice
  // interrupt every time regardless of intelligence — the guard is chosen_school==None,
  // not "just crossed 4" (see test_level_up_int_crossing_4_forces_school_choice() for
  // that actual crossing case).
  gs.player.chosen_school = SpellSchool::CombatMage;
  gs.mode = Mode::LevelUp;
  gs.pending_attribute_points = 3;
  int str_before = gs.player.strength;

  handle_event(gs, key_down(SDLK_S));  // plain lowercase, no shift
  check(gs.player.strength == str_before, "plain 's' (no shift) does nothing");
  check(gs.pending_attribute_points == 3, "test setup: three points queued");

  handle_event(gs, key_down(SDLK_S, SDL_KMOD_SHIFT));
  check(gs.player.strength == str_before + 1, "Shift+S raises Strength");
  check(gs.player.max_hp > 0 && gs.pending_attribute_points == 2, "Strength's max HP ceiling rose too, and one point was spent");
  check(gs.mode == Mode::LevelUp, "more points pending: stays open");

  int dex_before = gs.player.dexterity;
  int evasion_before = gs.player.evasion;
  handle_event(gs, key_down(SDLK_D, SDL_KMOD_SHIFT));
  check(gs.player.dexterity == dex_before + 1, "Shift+D raises Dexterity");
  check(gs.player.evasion == evasion_before + kDodgePerDexPoint, "Dexterity's evasion ceiling rises by the per-point rate");
  check(gs.pending_attribute_points == 1, "one point spent, one left");

  handle_event(gs, key_down(SDLK_I, SDL_KMOD_SHIFT));
  check(gs.pending_attribute_points == 0, "the last point is spent");
  check(gs.mode == Mode::Playing, "0 points left and no school-choice trigger closes back to Playing");
}

void test_level_up_int_crossing_4_forces_school_choice() {
  GameState gs = arena::make_gamestate(kRoom, 1, 1);
  gs.player.intelligence = 3;
  gs.player.chosen_school = SpellSchool::None;
  gs.pending_attribute_points = 1;
  gs.mode = Mode::LevelUp;
  handle_event(gs, key_down(SDLK_I, SDL_KMOD_SHIFT));
  check(gs.player.intelligence == 4, "the level-up itself still applied before the interrupt");
  check(gs.pending_attribute_points == 0, "the interrupting school choice consumes the point too");
  check(gs.mode == Mode::SchoolChoice, "crossing INT 4 interrupts with the school choice, even with 0 points left");
}

void test_school_choice_sets_school_and_resumes_queued_levelup() {
  GameState gs = arena::make_gamestate(kRoom, 1, 1);
  gs.player.intelligence = 4;
  gs.mode = Mode::SchoolChoice;
  gs.pending_attribute_points = 2;  // still queued, e.g. mid --level=N drain
  handle_event(gs, key_down(SDLK_U, SDL_KMOD_SHIFT));
  check(gs.player.chosen_school == SpellSchool::Summoner, "Shift+U picks Summoner");
  check(gs.mode == Mode::LevelUp, "resumes the queued level-up points rather than dropping to Playing");

  GameState gs2 = arena::make_gamestate(kRoom, 1, 1);
  gs2.player.intelligence = 4;
  gs2.mode = Mode::SchoolChoice;
  gs2.pending_attribute_points = 0;
  handle_event(gs2, key_down(SDLK_C, SDL_KMOD_SHIFT));
  check(gs2.player.chosen_school == SpellSchool::Caster, "Shift+C picks Caster");
  check(gs2.mode == Mode::Playing, "no points queued: drops back to Playing");

  GameState gs3 = arena::make_gamestate(kRoom, 1, 1);
  gs3.player.intelligence = 4;
  gs3.mode = Mode::SchoolChoice;
  handle_event(gs3, key_down(SDLK_M, SDL_KMOD_SHIFT));
  check(gs3.player.chosen_school == SpellSchool::CombatMage, "Shift+M picks Combat Mage");

  GameState gs4 = arena::make_gamestate(kRoom, 1, 1);
  gs4.mode = Mode::SchoolChoice;
  handle_event(gs4, key_down(SDLK_ESCAPE));
  check(gs4.running == false, "Escape here quits rather than leaving chosen_school stuck at None");
}

// --- SpellMenu (input_spell_minion_menus.cpp) ---------------------------------------------

void test_spell_menu_ordinary_cast_enters_targeting() {
  GameState gs = arena::make_gamestate(kRoom, 1, 1);
  gs.player.intelligence = 10;
  gs.player.chosen_school = SpellSchool::None;
  SDL_Keycode dart_key = letter_for_known_spell(gs, "Magic Dart");
  gs.mode = Mode::SpellMenu;
  handle_event(gs, key_down(dart_key));
  check(gs.mode == Mode::Targeting, "an ordinary fired spell opens Targeting");
  check(gs.casting_spell_index == spell_index_named("Magic Dart"), "casting_spell_index is set to the chosen spell");
  check(gs.casting_actor_id == -1, "the player is casting, not a minion");
}

void test_spell_menu_toggle_on_off_and_unaffordable() {
  GameState gs = arena::make_gamestate(kRoom, 1, 1);
  gs.player.intelligence = 10;
  gs.player.chosen_school = SpellSchool::Caster;
  gs.player.mana = 100;
  SDL_Keycode sandstorm_key = letter_for_known_spell(gs, "Sandstorm");

  gs.mode = Mode::SpellMenu;
  handle_event(gs, key_down(sandstorm_key));
  check(gs.active_toggle_spell == spell_index_named("Sandstorm"), "toggling on sets active_toggle_spell");
  check(gs.player.mana == 100 - kSpellTable[static_cast<size_t>(spell_index_named("Sandstorm"))].mana_cost,
        "the flat activation cost is deducted");
  check(gs.mode == Mode::Playing, "an unaffordable toggle is still a free cancel");
  int mana_after_on = gs.player.mana;

  gs.mode = Mode::SpellMenu;
  handle_event(gs, key_down(sandstorm_key));
  check(gs.active_toggle_spell == -1, "toggling off clears active_toggle_spell");
  check(gs.player.mana == mana_after_on, "turning off is free");
  check(gs.mode == Mode::Playing, "toggling off closes the menu");

  GameState gs2 = arena::make_gamestate(kRoom, 1, 1);
  gs2.player.intelligence = 10;
  gs2.player.chosen_school = SpellSchool::Caster;
  gs2.player.mana = 0;
  SDL_Keycode sandstorm_key2 = letter_for_known_spell(gs2, "Sandstorm");
  gs2.mode = Mode::SpellMenu;
  handle_event(gs2, key_down(sandstorm_key2));
  check(gs2.active_toggle_spell == -1, "can't afford: stays off");
  check(gs2.mode == Mode::Playing, "still a free cancel");
}

void test_spell_menu_summon_success_cap_and_no_room() {
  GameState gs = arena::make_gamestate(kBigRoom, 3, 2);
  gs.player.intelligence = 10;
  gs.player.chosen_school = SpellSchool::Summoner;
  gs.player.mana = 1000;
  SDL_Keycode imp_key = letter_for_known_spell(gs, "Summon Imp");

  gs.mode = Mode::SpellMenu;
  handle_event(gs, key_down(imp_key));
  check(gs.mode == Mode::Playing, "a successful summon returns to Playing");
  check(count_minions_named(gs.level().monsters, "Imp") == 1, "an Imp was actually added");

  gs.mode = Mode::SpellMenu;
  handle_event(gs, key_down(imp_key));
  gs.mode = Mode::SpellMenu;
  handle_event(gs, key_down(imp_key));
  check(count_minions_named(gs.level().monsters, "Imp") == 3, "test setup: three Imps out, at Summon Imp's own cap");

  int mana_before_capped = gs.player.mana;
  gs.mode = Mode::SpellMenu;
  handle_event(gs, key_down(imp_key));
  check(count_minions_named(gs.level().monsters, "Imp") == 3, "a fourth Imp is refused at the cap");
  check(gs.player.mana == mana_before_capped, "no mana spent on a refused summon");
  check(gs.mode == Mode::Playing, "still a free cancel");

  GameState gs2 = arena::make_gamestate(kRoom, 1, 1);
  gs2.player.intelligence = 10;
  gs2.player.chosen_school = SpellSchool::Summoner;
  gs2.player.mana = 0;
  SDL_Keycode imp_key2 = letter_for_known_spell(gs2, "Summon Imp");
  gs2.mode = Mode::SpellMenu;
  handle_event(gs2, key_down(imp_key2));
  check(count_minions_named(gs2.level().monsters, "Imp") == 0, "can't afford: nothing summoned");
  check(gs2.mode == Mode::Playing, "can't afford: still a free cancel");

  const std::vector<std::string> kTinyCell = {"###", "#.#", "###"};
  GameState gs3 = arena::make_gamestate(kTinyCell, 1, 1);
  gs3.player.intelligence = 10;
  gs3.player.chosen_school = SpellSchool::Summoner;
  gs3.player.mana = 1000;
  SDL_Keycode imp_key3 = letter_for_known_spell(gs3, "Summon Imp");
  gs3.mode = Mode::SpellMenu;
  handle_event(gs3, key_down(imp_key3));
  check(gs3.mode == Mode::Playing, "no-room summon is still a free cancel");
  check(count_minions_named(gs3.level().monsters, "Imp") == 0, "nowhere to put it: nothing spawned");
  check(gs3.player.mana == 1000, "no mana spent when there's no room");
}

void test_spell_menu_combat_mage_buffs() {
  GameState gs = arena::make_gamestate(kRoom, 1, 1);
  gs.player.intelligence = 10;
  gs.player.chosen_school = SpellSchool::CombatMage;
  gs.player.mana = 100;

  SDL_Keycode fury_key = letter_for_known_spell(gs, "Battle Fury");
  gs.mode = Mode::SpellMenu;
  handle_event(gs, key_down(fury_key));
  const Spell& fury = kSpellTable[static_cast<size_t>(spell_index_named("Battle Fury"))];
  // buff_turns - 1, not buff_turns: the cast sets the timer *before* end_turn(), and
  // end_turn()'s own upkeep phase ticks every temp buff timer down by one turn later in
  // that same call (see tick_upkeep() in turn.cpp) — Haste below is the one buff that
  // applies *after* end_turn() specifically to avoid this same-turn decrement.
  check(gs.player.temp_melee_damage_bonus == fury.buff_amount && gs.player.temp_melee_damage_turns == fury.buff_turns - 1,
        "Battle Fury applies its flat melee-damage buff");
  check(gs.mode == Mode::Playing, "casting closes the menu");

  SDL_Keycode skin_key = letter_for_known_spell(gs, "Iron Skin");
  gs.mode = Mode::SpellMenu;
  handle_event(gs, key_down(skin_key));
  const Spell& skin = kSpellTable[static_cast<size_t>(spell_index_named("Iron Skin"))];
  check(gs.player.temp_armor_bonus == skin.buff_amount && gs.player.temp_armor_turns == skin.buff_turns - 1,
        "Iron Skin applies its flat armor buff (buff_turns - 1, same same-turn-tick reasoning as Battle Fury above)");

  SDL_Keycode haste_key = letter_for_known_spell(gs, "Haste");
  gs.mode = Mode::SpellMenu;
  handle_event(gs, key_down(haste_key));
  const Spell& haste = kSpellTable[static_cast<size_t>(spell_index_named("Haste"))];
  check(gs.player.temp_extra_actions_bonus == haste.buff_amount && gs.player.temp_extra_actions_turns == haste.buff_turns,
        "Haste applies its extra-actions buff after end_turn()");

  GameState gs2 = arena::make_gamestate(kRoom, 1, 1);
  gs2.player.intelligence = 10;
  gs2.player.chosen_school = SpellSchool::CombatMage;
  gs2.player.mana = 0;
  SDL_Keycode fury_key2 = letter_for_known_spell(gs2, "Battle Fury");
  gs2.mode = Mode::SpellMenu;
  handle_event(gs2, key_down(fury_key2));
  check(gs2.player.temp_melee_damage_turns == 0, "can't afford: no buff applied");
  check(gs2.mode == Mode::Playing, "still a free cancel");
}

void test_spell_menu_raise_dead_auto_aims_at_corpse_or_player() {
  GameState gs = arena::make_gamestate(kBigRoom, 3, 2);
  gs.player.intelligence = 10;
  gs.player.chosen_school = SpellSchool::Summoner;
  gs.player.mana = 100;
  gs.level().corpses.push_back(Corpse{4, 2, monster_index_named("Rat")});
  SDL_Keycode raise_key = letter_for_known_spell(gs, "Raise Dead");
  gs.mode = Mode::SpellMenu;
  handle_event(gs, key_down(raise_key));
  check(gs.mode == Mode::Targeting, "a successful raise setup opens Targeting");
  check(gs.casting_spell_index == spell_index_named("Raise Dead"), "casting_spell_index is set to Raise Dead");
  check(gs.target_x == 4 && gs.target_y == 2, "auto-aims at the corpse in range");

  GameState gs2 = arena::make_gamestate(kBigRoom, 3, 2);
  gs2.player.intelligence = 10;
  gs2.player.chosen_school = SpellSchool::Summoner;
  gs2.player.mana = 100;
  SDL_Keycode raise_key2 = letter_for_known_spell(gs2, "Raise Dead");
  gs2.mode = Mode::SpellMenu;
  handle_event(gs2, key_down(raise_key2));
  check(gs2.target_x == gs2.player.x && gs2.target_y == gs2.player.y,
        "no corpse anywhere: falls back to the player's own tile");
}

void test_spell_menu_place_swap_auto_aims_at_minion_or_player() {
  GameState gs = arena::make_gamestate(kBigRoom, 3, 2);
  gs.player.intelligence = 10;
  gs.player.chosen_school = SpellSchool::Summoner;
  gs.player.mana = 100;
  arena::place_minion(gs, kMinionTable[0], 4, 2);
  SDL_Keycode swap_key = letter_for_known_spell(gs, "Place Swap");
  gs.mode = Mode::SpellMenu;
  handle_event(gs, key_down(swap_key));
  check(gs.mode == Mode::Targeting, "a successful swap setup opens Targeting");
  check(gs.target_x == 4 && gs.target_y == 2, "auto-aims at the closest swappable minion");

  GameState gs2 = arena::make_gamestate(kBigRoom, 3, 2);
  gs2.player.intelligence = 10;
  gs2.player.chosen_school = SpellSchool::Summoner;
  gs2.player.mana = 100;
  SDL_Keycode swap_key2 = letter_for_known_spell(gs2, "Place Swap");
  gs2.mode = Mode::SpellMenu;
  handle_event(gs2, key_down(swap_key2));
  check(gs2.target_x == gs2.player.x && gs2.target_y == gs2.player.y,
        "no minion anywhere: falls back to the player's own tile");
}

// --- MinionRoster / MinionAbilityMenu (input_spell_minion_menus.cpp) ---------------------

void test_minion_roster_select_and_shift_a() {
  GameState gs = arena::make_gamestate(kBigRoom, 3, 2);
  int id1 = arena::place_minion(gs, kMinionTable[0], 1, 1);
  int id2 = arena::place_minion(gs, kMinionTable[0], 5, 1);
  (void)id2;

  gs.mode = Mode::MinionRoster;
  handle_event(gs, key_down(SDLK_A));  // first living minion, letter a
  check(gs.focused_minion_id == id1, "selecting the first letter focuses the first living minion");
  check(gs.commanding_all_minions == false, "selecting a letter focuses one minion, not the whole pack");
  check(gs.mode == Mode::MinionFocus, "selecting a minion opens MinionFocus");

  gs.mode = Mode::MinionRoster;
  handle_event(gs, key_down(SDLK_A, SDL_KMOD_SHIFT));
  check(gs.commanding_all_minions == true, "Shift+A commands every minion at once");
  check(gs.target_x == gs.player.x && gs.target_y == gs.player.y, "the cursor starts on the player's own tile");
  check(gs.mode == Mode::MinionFocus, "Shift+A also opens MinionFocus");

  gs.mode = Mode::MinionRoster;
  handle_event(gs, key_down(SDLK_Z));  // no third minion
  check(gs.mode == Mode::MinionRoster, "an out-of-range letter does nothing");

  gs.mode = Mode::MinionRoster;
  handle_event(gs, key_down(SDLK_ESCAPE));
  check(gs.mode == Mode::Playing, "Escape closes the roster");
}

void test_minion_ability_menu_affordability_and_targeting() {
  GameState gs = arena::make_gamestate(kRoom, 1, 1);
  int demon_id = arena::place_minion(gs, kMinionTable[1], 3, 1);  // Demon, has Wither Curse
  Actor* demon = arena::find_actor(gs, demon_id);
  demon->mana = 100;
  gs.focused_minion_id = demon_id;

  gs.mode = Mode::MinionAbilityMenu;
  handle_event(gs, key_down(SDLK_A));
  check(gs.mode == Mode::Targeting, "an affordable ability opens Targeting");
  check(gs.casting_actor_id == demon_id, "the minion itself is the caster, not the player");
  check(gs.casting_spell_index == spell_index_named("Wither Curse"), "casting_spell_index is set to the minion's own ability");

  demon->mana = 0;
  gs.focused_minion_id = demon_id;
  gs.mode = Mode::MinionAbilityMenu;
  handle_event(gs, key_down(SDLK_A));
  check(gs.mode == Mode::MinionFocus, "an unaffordable ability rejects back to MinionFocus");

  gs.mode = Mode::MinionAbilityMenu;
  handle_event(gs, key_down(SDLK_Z));  // no such ability
  check(gs.mode == Mode::MinionAbilityMenu, "an out-of-range letter does nothing");

  gs.mode = Mode::MinionAbilityMenu;
  handle_event(gs, key_down(SDLK_ESCAPE));
  check(gs.mode == Mode::MinionFocus, "Escape backs out to the cursor, not all the way to Playing");
}

// --- Start menu / Set Seed / Run History / Dead / Win / MessageLog / Help ----------------
// (input_screens.cpp)

void test_start_menu_navigation_and_options() {
  GameState gs = arena::make_gamestate(kRoom, 1, 1);
  gs.mode = Mode::StartMenu;
  gs.start_menu_selection = 0;
  handle_event(gs, key_down(SDLK_DOWN));
  check(gs.start_menu_selection == 1, "Down moves the selection forward");
  handle_event(gs, key_down(SDLK_UP));
  check(gs.start_menu_selection == 0, "Up moves it back");
  handle_event(gs, key_down(SDLK_UP));
  check(gs.start_menu_selection == 3, "Up wraps from the top to the bottom");

  gs.mode = Mode::StartMenu;
  handle_event(gs, key_down(SDLK_2));
  check(gs.mode == Mode::SetSeed, "'2' jumps straight to Set Seed");

  gs.mode = Mode::StartMenu;
  handle_event(gs, key_down(SDLK_3));
  check(gs.mode == Mode::RunHistory && gs.run_history_scroll == 0, "'3' jumps straight to Run History, scrolled to the top");

  gs.mode = Mode::StartMenu;
  handle_event(gs, key_down(SDLK_1));
  check(gs.mode == Mode::Playing, "'1' (Start Game) begins a fresh game");

  gs.mode = Mode::StartMenu;
  handle_event(gs, key_down(SDLK_4));
  check(gs.running == false, "'4' (Exit) stops the run loop");

  GameState gs2 = arena::make_gamestate(kRoom, 1, 1);
  gs2.mode = Mode::StartMenu;
  handle_event(gs2, key_down(SDLK_ESCAPE));
  check(gs2.running == false, "Escape from the start menu also quits");

  GameState gs3 = arena::make_gamestate(kRoom, 1, 1);
  gs3.mode = Mode::StartMenu;
  gs3.start_menu_selection = 0;
  handle_event(gs3, key_down(SDLK_RETURN));
  check(gs3.mode == Mode::Playing, "Enter confirms whatever is currently selected");
}

void test_set_seed_digit_entry_confirm_backspace_cancel() {
  GameState gs = arena::make_gamestate(kRoom, 1, 1);
  gs.mode = Mode::SetSeed;
  gs.seed_input.clear();
  handle_event(gs, key_down(SDLK_4));
  handle_event(gs, key_down(SDLK_2));
  check(gs.seed_input == "42", "digit keys accumulate");
  handle_event(gs, key_down(SDLK_BACKSPACE));
  check(gs.seed_input == "4", "backspace removes the last digit");
  handle_event(gs, key_down(SDLK_2));
  handle_event(gs, key_down(SDLK_RETURN));
  check(gs.mode == Mode::StartMenu, "Enter confirms and returns to the start menu");
  check(gs.current_seed_display == "42", "the confirmed value is recorded for display");

  GameState gs2 = arena::make_gamestate(kRoom, 1, 1);
  gs2.mode = Mode::SetSeed;
  gs2.seed_input.clear();
  gs2.current_seed_display = "random";
  handle_event(gs2, key_down(SDLK_RETURN));
  check(gs2.mode == Mode::StartMenu, "confirming with nothing typed still returns to the start menu");
  check(gs2.current_seed_display == "random", "confirming with nothing typed doesn't touch the RNG or the display");

  GameState gs3 = arena::make_gamestate(kRoom, 1, 1);
  gs3.mode = Mode::SetSeed;
  handle_event(gs3, key_down(SDLK_ESCAPE));
  check(gs3.mode == Mode::StartMenu, "Escape cancels back to the start menu");

  GameState gs4 = arena::make_gamestate(kRoom, 1, 1);
  gs4.mode = Mode::SetSeed;
  gs4.seed_input.clear();
  handle_event(gs4, key_down(SDLK_BACKSPACE));
  check(gs4.seed_input.empty(), "backspace on empty input does nothing");
}

void test_run_history_scroll_and_escape() {
  GameState gs = arena::make_gamestate(kRoom, 1, 1);
  gs.mode = Mode::RunHistory;
  gs.run_history_scroll = 0;
  handle_event(gs, key_down(SDLK_J));  // scroll down first: already at 0, clamps
  check(gs.run_history_scroll == 0, "scrolling down from 0 clamps at 0");
  handle_event(gs, key_down(SDLK_K));  // scroll up (older) — bounded by however many
                                        // real entries load_run_history() finds
  check(gs.run_history_scroll >= 0, "scrolling up never goes negative");
  handle_event(gs, key_down(SDLK_DOWN));
  check(gs.run_history_scroll >= 0, "scrolling back down never goes negative either");
  handle_event(gs, key_down(SDLK_ESCAPE));
  check(gs.mode == Mode::StartMenu, "Escape returns to the start menu");
}

void test_dead_and_win_restart_and_escape() {
  GameState gs = arena::make_gamestate(kRoom, 1, 1);
  gs.mode = Mode::Dead;
  handle_event(gs, key_down(SDLK_A));
  check(gs.mode == Mode::Playing, "any key restarts from the death screen");

  GameState gs2 = arena::make_gamestate(kRoom, 1, 1);
  gs2.mode = Mode::Dead;
  handle_event(gs2, key_down(SDLK_ESCAPE));
  check(gs2.running == false, "Escape from the death screen quits instead");

  GameState gs3 = arena::make_gamestate(kRoom, 1, 1);
  gs3.mode = Mode::Win;
  handle_event(gs3, key_down(SDLK_A));
  check(gs3.mode == Mode::Playing, "any key restarts from the win screen");

  GameState gs4 = arena::make_gamestate(kRoom, 1, 1);
  gs4.mode = Mode::Win;
  handle_event(gs4, key_down(SDLK_ESCAPE));
  check(gs4.running == false, "Escape from the win screen quits instead");
}

void test_message_log_scroll() {
  GameState gs = arena::make_gamestate(kRoom, 1, 1);
  for (int i = 0; i < 5; ++i) add_message(gs, "message " + std::to_string(i));
  gs.mode = Mode::MessageLog;
  gs.log_scroll = 0;
  handle_event(gs, key_down(SDLK_K));
  check(gs.log_scroll == 1, "scrolling up (toward older messages) increases log_scroll");
  handle_event(gs, key_down(SDLK_J));
  check(gs.log_scroll == 0, "scrolling back down returns to 0");
  handle_event(gs, key_down(SDLK_J));
  check(gs.log_scroll == 0, "scrolling down past 0 clamps at 0");
}

// --- Targeting / RangedAttack / MinionFocus / Look (input_overlays.cpp) ------------------

void test_minion_focus_escape_and_shift_p() {
  GameState gs = arena::make_gamestate(kRoom, 1, 1);
  int id = arena::place_minion(gs, kMinionTable[0], 2, 1);
  gs.focused_minion_id = id;
  gs.mode = Mode::MinionFocus;
  handle_event(gs, key_down(SDLK_ESCAPE));
  check(gs.mode == Mode::Playing, "Escape backs out to Playing");
  check(gs.focused_minion_id == id, "plain Escape backs out without resetting focus");

  gs.focused_minion_id = id;
  gs.mode = Mode::MinionFocus;
  handle_event(gs, key_down(SDLK_P, SDL_KMOD_SHIFT));
  check(gs.mode == Mode::Playing, "Shift+P also backs out to Playing");
  check(gs.focused_minion_id == -1, "Shift+P also resets focus, so the next o/p starts from the top");
}

void test_minion_focus_cycle_o_p() {
  GameState gs = arena::make_gamestate(kBigRoom, 3, 2);
  int id1 = arena::place_minion(gs, kMinionTable[0], 1, 1);
  int id2 = arena::place_minion(gs, kMinionTable[0], 5, 1);
  gs.focused_minion_id = id1;
  gs.mode = Mode::MinionFocus;
  handle_event(gs, key_down(SDLK_O));
  check(gs.focused_minion_id == id2, "'o' tabs to the next minion without leaving the mode");
  check(gs.mode == Mode::MinionFocus, "still in this mode after tabbing");
  handle_event(gs, key_down(SDLK_P));
  check(gs.focused_minion_id == id1, "'p' tabs back to the previous minion");
}

void test_minion_focus_z_ability_menu() {
  GameState gs = arena::make_gamestate(kRoom, 1, 1);
  int imp_id = arena::place_minion(gs, kMinionTable[0], 2, 1);  // Imp, no abilities

  gs.focused_minion_id = -1;
  gs.mode = Mode::MinionFocus;
  handle_event(gs, key_down(SDLK_Z));
  check(gs.mode == Mode::MinionFocus, "nothing focused: 'z' does nothing");
  check(gs.message_log.back() == "Focus a single minion first.", "the no-focus phrasing is used");

  gs.focused_minion_id = imp_id;
  gs.mode = Mode::MinionFocus;
  handle_event(gs, key_down(SDLK_Z));
  check(gs.mode == Mode::MinionFocus, "a minion with no abilities: 'z' does nothing");
  check(gs.message_log.back() == "That minion has no abilities.", "the has-no-abilities phrasing is used");

  int demon_id = arena::place_minion(gs, kMinionTable[1], 3, 1);  // Demon, has Wither Curse
  gs.focused_minion_id = demon_id;
  gs.mode = Mode::MinionFocus;
  handle_event(gs, key_down(SDLK_Z));
  check(gs.mode == Mode::MinionAbilityMenu, "a minion with an ability: 'z' opens its ability menu");
}

void test_minion_focus_f_and_g_orders_single_and_all() {
  GameState gs = arena::make_gamestate(kBigRoom, 3, 2);
  int id1 = arena::place_minion(gs, kMinionTable[0], 1, 1);
  int id2 = arena::place_minion(gs, kMinionTable[0], 5, 1);
  Actor* m1 = arena::find_actor(gs, id1);
  Actor* m2 = arena::find_actor(gs, id2);
  m2->order = MinionOrder::Hold;

  gs.focused_minion_id = id1;
  gs.commanding_all_minions = false;
  gs.mode = Mode::MinionFocus;
  handle_event(gs, key_down(SDLK_G));  // Aggressive, single minion only
  check(m1->order == MinionOrder::Aggressive, "'g' orders just the focused minion Aggressive");
  check(m2->order == MinionOrder::Hold, "the other (non-focused) minion is untouched");
  check(gs.mode == Mode::Playing, "'g' on a single minion closes the mode");

  m1->order = MinionOrder::Hold;
  m2->order = MinionOrder::Hold;
  gs.commanding_all_minions = true;
  gs.mode = Mode::MinionFocus;
  handle_event(gs, key_down(SDLK_F));
  check(m1->order == MinionOrder::Follow && m2->order == MinionOrder::Follow,
        "'f' with commanding_all_minions orders every living minion");
  check(gs.commanding_all_minions == false, "the All session ends once the order is given");
  check(gs.mode == Mode::Playing, "'f' on the whole pack closes the mode");
}

void test_minion_focus_enter_branches() {
  GameState gs = arena::make_gamestate(kBigRoom, 3, 2);
  int minion_id = arena::place_minion(gs, kMinionTable[0], 1, 1);
  int rat_id = arena::place_monster(gs, monster_index_named("Rat"), 5, 1);
  Actor* minion = arena::find_actor(gs, minion_id);
  gs.focused_minion_id = minion_id;
  gs.commanding_all_minions = false;

  gs.target_x = 5;
  gs.target_y = 1;
  gs.mode = Mode::MinionFocus;
  handle_event(gs, key_down(SDLK_RETURN));
  check(minion->order == MinionOrder::AttackTarget && minion->attack_target_id == rat_id,
        "Enter over a hostile assigns AttackTarget");
  check(gs.mode == Mode::Playing, "Enter over a hostile closes the mode");

  gs.focused_minion_id = minion_id;
  gs.target_x = gs.player.x;
  gs.target_y = gs.player.y;
  gs.mode = Mode::MinionFocus;
  handle_event(gs, key_down(SDLK_RETURN));
  check(minion->order == MinionOrder::Follow, "Enter over the player's own tile assigns Follow");

  gs.focused_minion_id = minion_id;
  gs.target_x = 2;
  gs.target_y = 2;
  gs.mode = Mode::MinionFocus;
  handle_event(gs, key_down(SDLK_RETURN));
  check(minion->order == MinionOrder::Hold && minion->hold_x == 2 && minion->hold_y == 2,
        "Enter over a free walkable tile assigns Hold there");

  gs.focused_minion_id = minion_id;
  gs.target_x = 0;
  gs.target_y = 0;  // a wall tile
  gs.mode = Mode::MinionFocus;
  handle_event(gs, key_down(SDLK_RETURN));
  check(gs.mode == Mode::MinionFocus, "Enter over a wall is rejected, staying in the mode to try again");
  check(minion->order == MinionOrder::Hold, "the rejected attempt left the existing order alone");
}

void test_minion_focus_cursor_bounds() {
  GameState gs = arena::make_gamestate(kBigRoom, 3, 2);
  gs.target_x = 3;
  gs.target_y = 2;
  gs.mode = Mode::MinionFocus;
  handle_event(gs, key_down(SDLK_L));
  check(gs.target_x == 4, "movement keys move the cursor, no range limit for a minion order");
}

void test_targeting_minion_caster_gone() {
  GameState gs = arena::make_gamestate(kRoom, 1, 1);
  gs.casting_spell_index = spell_index_named("Magic Dart");
  gs.casting_actor_id = 999999;  // resolves to nothing
  gs.mode = Mode::Targeting;
  handle_event(gs, key_down(SDLK_RETURN));
  check(gs.mode == Mode::Playing, "a gone minion caster cancels the cast");
  check(gs.casting_actor_id == -1, "the stale minion id is cleared");
}

void test_targeting_escape_and_insufficient_mana() {
  GameState gs = arena::make_gamestate(kRoom, 1, 1);
  gs.casting_spell_index = spell_index_named("Magic Dart");
  gs.casting_actor_id = -1;
  gs.mode = Mode::Targeting;
  handle_event(gs, key_down(SDLK_ESCAPE));
  check(gs.mode == Mode::Playing, "Escape cancels the cast");

  gs.player.mana = 0;
  gs.mode = Mode::Targeting;
  handle_event(gs, key_down(SDLK_RETURN));
  check(gs.mode == Mode::Playing, "can't afford: free cancel");
}

void test_targeting_raise_dead_branches() {
  GameState gs = arena::make_gamestate(kBigRoom, 3, 2);
  gs.player.mana = 100;
  int rat_index = monster_index_named("Rat");
  gs.level().corpses.push_back(Corpse{4, 2, rat_index});
  gs.casting_spell_index = spell_index_named("Raise Dead");
  gs.casting_actor_id = -1;
  gs.target_x = 4;
  gs.target_y = 2;
  gs.mode = Mode::Targeting;
  handle_event(gs, key_down(SDLK_RETURN));
  check(gs.mode == Mode::Playing, "a successful raise closes back to Playing");
  check(gs.level().corpses.empty(), "a successful raise consumes the corpse");
  bool found_raised = false;
  for (auto& m : gs.level().monsters) {
    if (m.allegiance == Allegiance::Player && m.monster_template_index == rat_index) found_raised = true;
  }
  check(found_raised, "a raised Rat minion was actually added");

  GameState gs2 = arena::make_gamestate(kBigRoom, 3, 2);
  gs2.player.mana = 100;
  gs2.casting_spell_index = spell_index_named("Raise Dead");
  gs2.casting_actor_id = -1;
  gs2.target_x = 4;
  gs2.target_y = 2;  // nothing there
  gs2.mode = Mode::Targeting;
  handle_event(gs2, key_down(SDLK_RETURN));
  check(gs2.mode == Mode::Playing, "no corpse under the cursor: still closes back to Playing");
  check(gs2.level().monsters.empty(), "no corpse under the cursor: nothing raised");

  GameState gs3 = arena::make_gamestate(kBigRoom, 3, 2);
  gs3.player.mana = 100;
  gs3.level().corpses.push_back(Corpse{4, 2, rat_index});
  gs3.level().monsters.push_back(spawn_reanimated(rat_index, 1, 1));
  gs3.level().monsters.push_back(spawn_reanimated(rat_index, 1, 2));
  gs3.level().monsters.push_back(spawn_reanimated(rat_index, 1, 3));
  check(count_raised_minions(gs3.level().monsters) == 3, "test setup: three raised minions already out, at the cap");
  gs3.casting_spell_index = spell_index_named("Raise Dead");
  gs3.casting_actor_id = -1;
  gs3.target_x = 4;
  gs3.target_y = 2;
  gs3.mode = Mode::Targeting;
  handle_event(gs3, key_down(SDLK_RETURN));
  check(gs3.mode == Mode::Playing, "at the cap: still closes back to Playing");
  check(!gs3.level().corpses.empty(), "at the cap: the corpse is left alone, not consumed");
  check(count_raised_minions(gs3.level().monsters) == 3, "no fourth raised minion added");

  const std::vector<std::string> kCorner = {
      "#######",
      "#.....#",
      "#.....#",
      "#.....#",
      "#######",
  };
  GameState gs4 = arena::make_gamestate(kCorner, 4, 1);
  gs4.player.mana = 100;
  gs4.level().corpses.push_back(Corpse{1, 1, rat_index});
  arena::place_monster(gs4, rat_index, 2, 1);
  arena::place_monster(gs4, rat_index, 1, 2);
  arena::place_monster(gs4, rat_index, 2, 2);
  gs4.casting_spell_index = spell_index_named("Raise Dead");
  gs4.casting_actor_id = -1;
  gs4.target_x = 1;
  gs4.target_y = 1;
  gs4.mode = Mode::Targeting;
  size_t monster_count_before = gs4.level().monsters.size();
  handle_event(gs4, key_down(SDLK_RETURN));
  check(gs4.mode == Mode::Playing, "no room beside the corpse: still closes back to Playing");
  check(!gs4.level().corpses.empty(), "no free tile beside the corpse: it's left alone");
  check(gs4.level().monsters.size() == monster_count_before, "nothing raised");
}

void test_targeting_debuff_branches() {
  GameState gs = arena::make_gamestate(kRoom, 1, 1);
  // The Demon sits right next to the player, far from the Rat — not adjacent to
  // whichever tile the Rat's own chase step lands on this turn. Placing it any closer
  // to the Rat risks a genuine, incidental kill: end_turn() (called below) also runs
  // the Rat's own hostile-AI turn, which could step it into the Demon's melee range,
  // and the Demon auto-defending would kill a 4-HP Rat outright — a real interaction,
  // just not the one this test is about.
  int rat_id = arena::place_monster(gs, monster_index_named("Rat"), 7, 1);
  int demon_id = arena::place_minion(gs, kMinionTable[1], 1, 2);
  Actor* demon = arena::find_actor(gs, demon_id);
  demon->mana = 100;
  gs.casting_spell_index = spell_index_named("Wither Curse");
  gs.casting_actor_id = demon_id;
  gs.target_x = 7;
  gs.target_y = 1;
  gs.mode = Mode::Targeting;
  handle_event(gs, key_down(SDLK_RETURN));
  check(gs.mode == Mode::Playing, "a successful curse closes back to Playing");
  Actor* rat = arena::find_actor(gs, rat_id);
  check(rat != nullptr, "test setup: the Rat survives this turn, far from the Demon");
  if (rat) check(rat->temp_melee_damage_bonus < 0, "the curse actually landed (a negative melee-damage delta)");
  check(demon->mana == 100 - kSpellTable[static_cast<size_t>(spell_index_named("Wither Curse"))].mana_cost,
        "the caster (the Demon, not the player) pays the mana");

  GameState gs2 = arena::make_gamestate(kRoom, 1, 1);
  int demon_id2 = arena::place_minion(gs2, kMinionTable[1], 1, 2);
  Actor* demon2 = arena::find_actor(gs2, demon_id2);
  demon2->mana = 100;
  gs2.casting_spell_index = spell_index_named("Wither Curse");
  gs2.casting_actor_id = demon_id2;
  gs2.target_x = 3;
  gs2.target_y = 1;  // nothing there
  gs2.mode = Mode::Targeting;
  handle_event(gs2, key_down(SDLK_RETURN));
  check(gs2.mode == Mode::Playing, "no hostile under the cursor: still closes back to Playing");
  check(demon2->mana == 100, "no hostile under the cursor: no mana spent");
}

void test_targeting_swap_branches() {
  GameState gs = arena::make_gamestate(kBigRoom, 3, 2);
  gs.player.mana = 100;
  int minion_id = arena::place_minion(gs, kMinionTable[0], 4, 2);
  gs.casting_spell_index = spell_index_named("Place Swap");
  gs.casting_actor_id = -1;
  gs.target_x = 4;
  gs.target_y = 2;
  gs.mode = Mode::Targeting;
  handle_event(gs, key_down(SDLK_RETURN));
  check(gs.mode == Mode::Playing, "a successful swap closes back to Playing");
  check(gs.player.x == 4 && gs.player.y == 2, "the player swapped into the minion's old tile");
  Actor* minion = arena::find_actor(gs, minion_id);
  check(minion->x == 3 && minion->y == 2, "the minion swapped into the player's old tile");

  GameState gs2 = arena::make_gamestate(kBigRoom, 3, 2);
  gs2.player.mana = 100;
  gs2.casting_spell_index = spell_index_named("Place Swap");
  gs2.casting_actor_id = -1;
  gs2.target_x = 4;
  gs2.target_y = 2;  // no minion there
  gs2.mode = Mode::Targeting;
  handle_event(gs2, key_down(SDLK_RETURN));
  check(gs2.mode == Mode::Playing, "no minion under the cursor: still closes back to Playing");
  check(gs2.player.x == 3 && gs2.player.y == 2, "no minion there: the player doesn't move");

  // A minion behind a wall is a real, valid minion (own_minion_at() finds it — the
  // cursor already knows where it is, since minions are drawn out of sight), but the
  // swap itself is still refused: a guaranteed, undodgeable teleport through solid rock
  // is exactly what line_clear() here is priced against.
  const std::vector<std::string> kSplitRoom = {
      "#######",
      "#.#...#",
      "#.#...#",
      "#######",
  };
  GameState gs3 = arena::make_gamestate(kSplitRoom, 1, 1);
  gs3.player.mana = 100;
  int minion_id3 = arena::place_minion(gs3, kMinionTable[0], 4, 1);
  gs3.casting_spell_index = spell_index_named("Place Swap");
  gs3.casting_actor_id = -1;
  gs3.target_x = 4;
  gs3.target_y = 1;
  gs3.mode = Mode::Targeting;
  handle_event(gs3, key_down(SDLK_RETURN));
  check(gs3.mode == Mode::Playing, "blocked by a wall: still closes back to Playing");
  check(gs3.player.x == 1 && gs3.player.y == 1, "a minion behind a wall: the swap is refused, the player doesn't move");
  check(gs3.player.mana == 100, "and no mana is spent on a refused swap");
  Actor* minion3 = arena::find_actor(gs3, minion_id3);
  check(minion3->x == 4 && minion3->y == 1, "the minion doesn't move either");
}

void test_targeting_ordinary_cast_fires_and_updates_last_target() {
  GameState gs = arena::make_gamestate(kRoom, 1, 1);
  gs.player.mana = 100;
  int rat_id = arena::place_monster(gs, monster_index_named("Rat"), 3, 1);
  gs.casting_spell_index = spell_index_named("Magic Dart");
  gs.casting_actor_id = -1;
  gs.target_x = 3;
  gs.target_y = 1;
  int mana_before = gs.player.mana;
  gs.mode = Mode::Targeting;
  handle_event(gs, key_down(SDLK_RETURN));
  check(gs.mode == Mode::Playing, "a successful ordinary cast closes back to Playing");
  check(gs.player.mana < mana_before, "casting deducted the spell's mana cost");
  check(gs.last_target_id == rat_id, "hitting a hostile under the cursor updates last_target_id");
}

void test_targeting_cursor_range_clamp() {
  GameState gs = arena::make_gamestate(make_wide_room(32), 10, 1);
  gs.casting_spell_index = spell_index_named("Magic Dart");  // range 8
  gs.casting_actor_id = -1;
  gs.target_x = 17;
  gs.target_y = 1;  // dx=7 from the caster: within range (49 <= 64)
  gs.mode = Mode::Targeting;
  handle_event(gs, key_down(SDLK_L));
  check(gs.target_x == 18, "cursor moves right up to exactly the spell's range (dx=8, 64<=64)");
  handle_event(gs, key_down(SDLK_L));
  check(gs.target_x == 18, "one more step would exceed the spell's range — the cursor doesn't move");
}

void test_ranged_attack_fires_and_updates_last_target() {
  GameState gs = arena::make_gamestate(kRoom, 1, 1);
  gs.player.weapon = kWeaponTable[weapon_index_named("Bow")];
  int rat_id = arena::place_monster(gs, monster_index_named("Rat"), 3, 1);
  gs.target_x = 3;
  gs.target_y = 1;
  gs.mode = Mode::RangedAttack;
  handle_event(gs, key_down(SDLK_RETURN));
  check(gs.mode == Mode::Playing, "a successful ranged shot closes back to Playing");
  check(gs.last_target_id == rat_id, "firing at a hostile under the cursor updates last_target_id");
}

void test_ranged_attack_escape_and_cursor_range_clamp() {
  GameState gs = arena::make_gamestate(make_wide_room(32), 10, 1);
  gs.player.weapon = kWeaponTable[weapon_index_named("Bow")];  // range 8
  gs.target_x = 17;
  gs.target_y = 1;
  gs.mode = Mode::RangedAttack;
  handle_event(gs, key_down(SDLK_L));
  check(gs.target_x == 18, "cursor moves up to exactly the weapon's range");
  handle_event(gs, key_down(SDLK_L));
  check(gs.target_x == 18, "one more step exceeds the weapon's range — cursor doesn't move");
  gs.mode = Mode::RangedAttack;
  handle_event(gs, key_down(SDLK_ESCAPE));
  check(gs.mode == Mode::Playing, "Escape closes RangedAttack");
}

void test_look_toggle_and_cursor_movement() {
  GameState gs = arena::make_gamestate(kBigRoom, 3, 2);
  gs.target_x = 3;
  gs.target_y = 2;
  gs.mode = Mode::Look;
  handle_event(gs, key_down(SDLK_L));
  check(gs.target_x == 4, "movement keys move the look cursor, no range limit");
  handle_event(gs, key_down(SDLK_X));
  check(gs.mode == Mode::Playing, "'x' closes Look the same as Escape");

  gs.mode = Mode::Look;
  handle_event(gs, key_down(SDLK_ESCAPE));
  check(gs.mode == Mode::Playing, "Escape also closes Look");
}

// --- Mode::Playing itself: movement, bump-attack/swap, stairs, wait, g/f/o/p ------------
// (input.cpp's own handle_playing_input(), as opposed to the modes it opens)

void test_playing_plain_movement_and_wall_block() {
  GameState gs = arena::make_gamestate(kRoom, 2, 1);
  handle_event(gs, key_down(SDLK_L));
  check(gs.player.x == 3 && gs.player.y == 1, "a plain movement key steps onto open floor");

  gs.player.x = 1;
  gs.player.y = 1;  // right next to the west wall
  handle_event(gs, key_down(SDLK_H));
  check(gs.player.x == 1 && gs.player.y == 1, "a wall blocks the step; the player doesn't move into it");
}

void test_playing_diagonal_movement() {
  GameState gs = arena::make_gamestate(kBigRoom, 3, 2);
  handle_event(gs, key_down(SDLK_N));  // down-right
  check(gs.player.x == 4 && gs.player.y == 3, "'n' moves down-right");
  handle_event(gs, key_down(SDLK_Y));  // up-left, back to start
  check(gs.player.x == 3 && gs.player.y == 2, "'y' moves up-left");
}

void test_playing_shift_movement_dispatches_to_run_in_direction() {
  // The dispatch itself (handle_playing_input() -> run_in_direction()) is the one thing
  // worth confirming here — run_in_direction()'s own stop rules are tests/movement_test.cpp's
  // job, exercised there by calling it directly.
  GameState gs = arena::make_gamestate(kBigRoom, 1, 2);
  handle_event(gs, key_down(SDLK_L, SDL_KMOD_SHIFT));
  check(gs.player.x > 1, "Shift+movement runs rather than taking a single step");
}

void test_playing_bump_attack_and_minion_swap() {
  GameState gs = arena::make_gamestate(kRoom, 1, 1);
  int rat_id = arena::place_monster(gs, monster_index_named("Rat"), 2, 1);
  size_t log_before = gs.message_log.size();
  handle_event(gs, key_down(SDLK_L));
  check(gs.player.x == 1 && gs.player.y == 1, "bumping into a hostile attacks in place rather than moving");
  check(gs.message_log.size() > log_before, "the attack logged a message");
  (void)rat_id;

  GameState gs2 = arena::make_gamestate(kRoom, 1, 1);
  int minion_id = arena::place_minion(gs2, kMinionTable[0], 2, 1);
  handle_event(gs2, key_down(SDLK_L));
  check(gs2.player.x == 2 && gs2.player.y == 1, "bumping into your own minion swaps places with it");
  Actor* minion = arena::find_actor(gs2, minion_id);
  check(minion->x == 1 && minion->y == 1, "the minion ends up where the player was");
}

void test_playing_wait() {
  GameState gs = arena::make_gamestate(kRoom, 1, 1);
  size_t log_before = gs.message_log.size();
  handle_event(gs, key_down(SDLK_PERIOD));
  check(gs.message_log.back() == "You wait.", "'.' (no shift) waits and logs it");
  check(gs.message_log.size() > log_before, "a turn was actually spent");
}

void test_playing_stairs_down() {
  GameState gs = arena::make_gamestate(kRoom, 2, 1);
  gs.level().has_stairs_down = true;
  gs.level().stairs_down_x = 2;
  gs.level().stairs_down_y = 1;
  int floor_before = gs.current_level;
  handle_event(gs, key_down(SDLK_GREATER));
  check(gs.current_level == floor_before + 1, "standing on real stairs down descends a floor");

  GameState gs2 = arena::make_gamestate(kRoom, 2, 1);
  gs2.level().has_stairs_down = true;
  gs2.level().stairs_down_x = 5;
  gs2.level().stairs_down_y = 1;  // not where the player is standing
  handle_event(gs2, key_down(SDLK_GREATER));
  check(gs2.current_level == 0, "not standing on the stairs: nothing happens");

  GameState gs3 = arena::make_gamestate(kRoom, 2, 1);
  gs3.level().has_stairs_down = false;  // kFinalFloor's own case
  gs3.level().stairs_down_x = 2;
  gs3.level().stairs_down_y = 1;
  handle_event(gs3, key_down(SDLK_GREATER));
  check(gs3.current_level == 0, "no real stairs down here (kFinalFloor): nothing happens either");
}

void test_playing_stairs_up() {
  GameState gs = arena::make_gamestate(kRoom, 2, 1);
  // ascend() needs a floor above to return to — build a second Level the same way
  // arena::make_gamestate() built the first (Level isn't copyable: it holds a TCODMap),
  // land the player on floor 1 with real stairs up, then ascend back to floor 0.
  gs.levels.push_back(Level{Map(MAP_WIDTH, MAP_HEIGHT), {}, {}, {}, {}, {}, {}, {}});
  gs.levels[1].map.paint_ascii(kRoom);
  gs.levels[1].has_stairs_up = true;
  gs.levels[1].entry_x = 2;
  gs.levels[1].entry_y = 1;
  gs.current_level = 1;
  gs.player.x = 2;
  gs.player.y = 1;
  handle_event(gs, key_down(SDLK_LESS));
  check(gs.current_level == 0, "standing on the entry tile with real stairs up ascends a floor");
}

void test_playing_pickup_key() {
  GameState gs = arena::make_gamestate(kRoom, 1, 1);
  handle_event(gs, key_down(SDLK_G));
  check(gs.message_log.back() == "There's nothing here to pick up.", "'g' on an empty tile just says so");

  GameState gs2 = arena::make_gamestate(kRoom, 1, 1);
  gs2.level().items.push_back(GroundItem{1, 1, kWeaponTable[0]});
  handle_event(gs2, key_down(SDLK_G));
  check(gs2.player.weapons.size() == 1, "a single item is picked up immediately");
  check(gs2.mode == Mode::Playing, "no menu needed for just one item");

  GameState gs3 = arena::make_gamestate(kRoom, 1, 1);
  gs3.level().items.push_back(GroundItem{1, 1, kWeaponTable[0]});
  gs3.level().armor_items.push_back(GroundArmor{1, 1, kArmorTable[0]});
  handle_event(gs3, key_down(SDLK_G));
  check(gs3.mode == Mode::Pickup, "two or more items open the Pickup menu instead");
  check(gs3.pickup_selected.size() == 2 && gs3.pickup_selected[0] && gs3.pickup_selected[1],
        "everything starts checked");
}

void test_playing_fire_key() {
  GameState gs = arena::make_gamestate(kRoom, 1, 1);
  gs.player.weapon = kWeaponTable[weapon_index_named("Dagger")];  // melee
  handle_event(gs, key_down(SDLK_F));
  check(gs.mode == Mode::Playing, "'f' with a melee weapon doesn't open RangedAttack");
  check(gs.message_log.back() == "Your Dagger isn't a ranged weapon.", "...and says so");

  GameState gs2 = arena::make_gamestate(kRoom, 1, 1);
  gs2.player.weapon = kWeaponTable[weapon_index_named("Bow")];
  handle_event(gs2, key_down(SDLK_F));
  check(gs2.mode == Mode::RangedAttack, "'f' with a ranged weapon opens RangedAttack");
}

void test_playing_minion_focus_cycle_keys() {
  GameState gs = arena::make_gamestate(kRoom, 1, 1);
  handle_event(gs, key_down(SDLK_O));
  check(gs.message_log.back() == "You have no minions to command.", "'o' with no minions says so");
  check(gs.mode == Mode::Playing, "and doesn't open MinionFocus");

  GameState gs2 = arena::make_gamestate(kBigRoom, 3, 2);
  int id = arena::place_minion(gs2, kMinionTable[0], 1, 1);
  handle_event(gs2, key_down(SDLK_O));
  check(gs2.mode == Mode::MinionFocus, "'o' with a minion present opens MinionFocus");
  check(gs2.focused_minion_id == id, "...focused on that minion");

  gs2.mode = Mode::Playing;
  handle_event(gs2, key_down(SDLK_P, SDL_KMOD_SHIFT));
  check(gs2.focused_minion_id == -1, "Shift+P resets focus from Mode::Playing too");
  check(gs2.mode == Mode::Playing, "...without opening anything");
}

}  // namespace

int main() {
  test_menu_open_close_roundtrips();
  test_minion_roster_requires_a_minion();
  test_weapon_menu_equip_swaps_old_into_inventory();
  test_armor_menu_equip_swaps_old_into_inventory();
  test_potion_menu_drinks_and_removes();
  test_drop_each_item_kind();
  test_pickup_toggle_shift_a_and_enter();
  test_level_up_shift_letters_and_lowercase_rejected();
  test_level_up_int_crossing_4_forces_school_choice();
  test_school_choice_sets_school_and_resumes_queued_levelup();
  test_spell_menu_ordinary_cast_enters_targeting();
  test_spell_menu_toggle_on_off_and_unaffordable();
  test_spell_menu_summon_success_cap_and_no_room();
  test_spell_menu_combat_mage_buffs();
  test_spell_menu_raise_dead_auto_aims_at_corpse_or_player();
  test_spell_menu_place_swap_auto_aims_at_minion_or_player();
  test_minion_roster_select_and_shift_a();
  test_minion_ability_menu_affordability_and_targeting();
  test_start_menu_navigation_and_options();
  test_set_seed_digit_entry_confirm_backspace_cancel();
  test_run_history_scroll_and_escape();
  test_dead_and_win_restart_and_escape();
  test_message_log_scroll();
  test_minion_focus_escape_and_shift_p();
  test_minion_focus_cycle_o_p();
  test_minion_focus_z_ability_menu();
  test_minion_focus_f_and_g_orders_single_and_all();
  test_minion_focus_enter_branches();
  test_minion_focus_cursor_bounds();
  test_targeting_minion_caster_gone();
  test_targeting_escape_and_insufficient_mana();
  test_targeting_raise_dead_branches();
  test_targeting_debuff_branches();
  test_targeting_swap_branches();
  test_targeting_ordinary_cast_fires_and_updates_last_target();
  test_targeting_cursor_range_clamp();
  test_ranged_attack_fires_and_updates_last_target();
  test_ranged_attack_escape_and_cursor_range_clamp();
  test_look_toggle_and_cursor_movement();

  test_playing_plain_movement_and_wall_block();
  test_playing_diagonal_movement();
  test_playing_shift_movement_dispatches_to_run_in_direction();
  test_playing_bump_attack_and_minion_swap();
  test_playing_wait();
  test_playing_stairs_down();
  test_playing_stairs_up();
  test_playing_pickup_key();
  test_playing_fire_key();
  test_playing_minion_focus_cycle_keys();

  std::printf("%d/%d checks passed\n", g_checks - g_failures, g_checks);
  return g_failures == 0 ? 0 : 1;
}
