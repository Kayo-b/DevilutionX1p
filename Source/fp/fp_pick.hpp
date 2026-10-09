/**
 * @file fp/fp_pick.hpp
 *
 * Finds what is under a screen position in the first-person view (crosshair or free cursor).
 */
#pragma once

#include "engine/point.hpp"
#include "fp/fp_world.hpp"

namespace devilution::fp {

struct Pick {
	bool hasTarget;
	EntityKind kind; // valid if hasTarget
	int id;          // index into Monsters / Towners / Objects / Items
	Point tile;      // target tile if hasTarget
	Point groundTile; // where the ray meets the floor or the last open tile before a wall
};

/**
 * @brief Picks the nearest selectable thing under the given screen position of the game viewport.
 * @param ignoreMonsters set while a player-only spell (heal other, resurrect) is being targeted
 */
[[nodiscard]] Pick PickAt(Point screenPosition, bool ignoreMonsters);

} // namespace devilution::fp
