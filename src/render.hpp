#pragma once

// Everything that draws. One entry point, plus the screen layout and the font/panel
// helpers main() needs to open a window in the first place.
//
// Rendering reads GameState and never writes it — if a render function needs to change
// something, that something belongs in input.cpp or turn.cpp instead.

#include <libtcod.hpp>

#include <string>

#include "game.hpp"

// --- Screen layout ---
//
// A context-prompt row across the top, then a map panel (left) and a status sidebar
// (right) side by side, then a message-log panel spanning the full width along the
// bottom. Each panel is a hand-drawn ASCII box (see draw_panel()) — libtcod's own
// tcod::print_frame() is deprecated upstream ("print your own banners for frames"), so
// this matches the rest of the drawing code rather than pulling in a discouraged helper.
//
// The dungeon (MAP_WIDTH x MAP_HEIGHT in game.hpp) is bigger than what's shown at once:
// MAP_VIEW_W x MAP_VIEW_H is the map panel's viewport, a scrolling window that follows
// the player or the aim cursor (see Camera), so the whole window fits on a normal screen
// instead of rendering the entire floor at 1:1.
constexpr int MAP_VIEW_W = 50;
constexpr int MAP_VIEW_H = 24;
constexpr int MESSAGE_ROWS = 5;  // message-log panel's visible rows, oldest on top

constexpr int CONTEXT_ROW = 0;  // transient LevelUp/Targeting/MinionFocus prompt

constexpr int MAP_PANEL_X = 0;
constexpr int MAP_PANEL_Y = CONTEXT_ROW + 1;
constexpr int MAP_PANEL_W = MAP_VIEW_W + 2;  // +2 for left/right border
constexpr int MAP_PANEL_H = MAP_VIEW_H + 2;  // +2 for top/bottom border
constexpr int MAP_ORIGIN_X = MAP_PANEL_X + 1;
constexpr int MAP_ORIGIN_Y = MAP_PANEL_Y + 1;

constexpr int SIDEBAR_X = MAP_PANEL_X + MAP_PANEL_W;
constexpr int SIDEBAR_Y = MAP_PANEL_Y;
constexpr int SIDEBAR_W = 28;
constexpr int SIDEBAR_H = MAP_PANEL_H;

constexpr int LOG_PANEL_X = MAP_PANEL_X;
constexpr int LOG_PANEL_Y = MAP_PANEL_Y + MAP_PANEL_H;
constexpr int LOG_PANEL_W = MAP_PANEL_W + SIDEBAR_W;
constexpr int LOG_PANEL_H = MESSAGE_ROWS + 2;  // +2 for top/bottom border

constexpr int SCREEN_WIDTH = LOG_PANEL_W;
constexpr int SCREEN_HEIGHT = LOG_PANEL_Y + LOG_PANEL_H;

constexpr int TILE_SIZE = 18;  // pixels per cell; square, so tiles aren't stretched

// The scrolled viewport onto the dungeon. Centers on the player, except while aiming or
// looking around (Targeting/RangedAttack/MinionFocus/Look), where it centers on the
// cursor instead so a cursor that has wandered off (MinionFocus and Look have no range
// limit, unlike a spell's Targeting) never drifts off-screen. Clamped so the viewport
// never scrolls past the map's edge.
struct Camera {
  int x = 0;
  int y = 0;

  // Whether a dungeon tile is currently inside the viewport. Every entity draw skips
  // anything failing this, rather than writing outside the map panel and into the
  // sidebar or log next to it.
  bool in_view(int map_x, int map_y) const {
    return map_x >= x && map_x < x + MAP_VIEW_W && map_y >= y && map_y < y + MAP_VIEW_H;
  }

  // Dungeon coordinates to console cell. Only meaningful when in_view() is true.
  int screen_x(int map_x) const { return MAP_ORIGIN_X + map_x - x; }
  int screen_y(int map_y) const { return MAP_ORIGIN_Y + map_y - y; }
};

Camera camera_for(const GameState& gs);

// Darkens a color for the "remembered, but not currently visible" rendering tier.
tcod::ColorRGB dim_color(tcod::ColorRGB c);

// Hand-drawn ASCII box: corners '+', horizontal '-', vertical '|', with an optional
// title inlaid into the top edge.
void draw_panel(tcod::Console& console, int x, int y, int w, int h, const std::string& title,
                tcod::ColorRGB color = tcod::ColorRGB{120, 120, 120});

// Loads the first font from a per-distro list of common monospace paths that exists on
// disk, rendered at tile_size x tile_size pixels per cell. Falls back to libtcod's
// built-in font if none of them exist. This approximates "use the font your terminal
// uses" without a fontconfig dependency or bundling a font file.
tcod::TilesetPtr load_best_tileset(int tile_size);

// Draws one complete frame for whatever mode the game is currently in. Clears the
// console itself; the caller only has to present it.
void render_frame(GameState& gs, tcod::Console& console);
