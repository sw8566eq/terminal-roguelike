#pragma once

// Shared random-number utility used by map generation, combat, and item/monster
// placement, so everything draws from one engine instead of each file seeding
// its own.
//
// Returns an integer in [lo, hi], inclusive.
int random_int(int lo, int hi);

// Rolls `count` dice of `sides` each and sums them (e.g. roll_dice(2, 6) is 2d6).
int roll_dice(int count, int sides);
