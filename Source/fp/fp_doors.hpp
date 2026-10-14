/**
 * @file fp/fp_doors.hpp
 *
 * Doors in first person: the doorway is a wall with an opening through it, open or shut, and the door is a leaf
 * hinged in it that swings open when the door is opened and stays open.
 *
 * In the clean dungeon (the cathedral, the catacombs, the crypt: fp_dungeon.hpp) the doorway is a plain round-arched
 * opening through the door tile (CleanDoorOpening); the caves' doors are gates in their fences, a lattice of beams
 * in a flat-topped frame, no wall round it (CleanDoorwaysAreGates, BuildGateLattice). Elsewhere (hell) it is the closed door's art with
 * the opening read off it; the walls are drawn taller than the art (VerticalScale, and the wall cap cut off), so a doorway one tile wide
 * would look narrow beside its iso picture: the art and its opening are widened by DoorwayWidthScale around the tile's
 * middle, reaching a little into the wall tiles either side of it.
 */
#pragma once

#include <cstdint>
#include <vector>

#include "engine/point.hpp"
#include "fp/fp_raycast.hpp"

namespace devilution::fp {

struct Doorway {
	int object;   // index into Objects
	Point tile;   // the door tile
	bool alongX;  // the wall runs along x (its faces are hit from +y and -y)
	/** The closed art of the doorway's face, FaceTextureWidth x FaceTextureHeight(), centred on the opening. */
	std::vector<int16_t> face;
	/** Per column of `face`: the first face row inside the opening (FaceTextureHeight() where it is wall). */
	std::vector<int> opening;
	/**
	 * Positions along the wall are "door-relative": 0..1 across the door tile, below 0 and above 1 on its neighbours.
	 * `face` is laid over [artFrom, artTo); the opening is within [openFrom, openTo).
	 */
	float artFrom;
	float artTo;
	float openFrom;
	float openTo;
};

/** @brief A doorway's art reaching into a tile's wall faces: the doorway and the tile's offset from the door tile along the wall. */
struct DoorwaySpan {
	const Doorway *door = nullptr;
	int offset = 0; // -1, 0 (the door tile) or 1
};

/** @brief Brings the doorways up to date with the current level; call once per frame before drawing. */
void PrepareDoorways();
/** @brief The doorway whose art covers part of this tile's faces along the wall, if any. */
[[nodiscard]] DoorwaySpan DoorwaySpanAt(Point tile);
/** @brief Whether the tile is a door tile drawn as a doorway (so it stays a wall when the door is open). */
[[nodiscard]] bool IsDoorwayTile(Point tile);
/** @brief The column of the doorway's face art at a door-relative position along the wall, -1 outside it. */
[[nodiscard]] int DoorFaceColumn(const Doorway &door, float along);
/** @brief Whether a ray hit on a doorway's face passes through its opening at `height` (tiles, as drawn). */
[[nodiscard]] bool IsDoorwayHole(const RayHit &hit, float height);

/**
 * @brief How far the door's leaf is swung open, in radians (0 shut), and to which side of the wall: +1 towards +y
 * (+x for a wall along y), -1 the other way. Animates when the door's state changes.
 */
float DoorSwing(const Doorway &door, int &side);

} // namespace devilution::fp
