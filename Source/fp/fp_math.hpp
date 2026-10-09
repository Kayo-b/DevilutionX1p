/**
 * @file fp/fp_math.hpp
 *
 * Small shared helpers for first-person world <-> game conversions.
 */
#pragma once

#include <cmath>

#include "engine/direction.hpp"
#include "engine/displacement.hpp"

namespace devilution::fp {

struct Vec2 {
	float x;
	float y;
};

/**
 * @brief Converts an iso screen-space pixel displacement (as used for walk offsets) into world (tile) units.
 *
 * The iso projection is screen x = (x - y) * 32, screen y = (x + y) * 16.
 */
inline Vec2 ScreenToWorld(Displacement offset)
{
	const float sx = static_cast<float>(offset.deltaX) / 32.0F;
	const float sy = static_cast<float>(offset.deltaY) / 16.0F;
	return { (sx + sy) / 2.0F, (sy - sx) / 2.0F };
}

inline float DirectionToYaw(Direction dir)
{
	const Displacement d { dir };
	return std::atan2(static_cast<float>(d.deltaY), static_cast<float>(d.deltaX));
}

/** @brief Nearest of the 8 game directions for a world-space heading. */
inline Direction YawToDirection(float yaw)
{
	// Octant 0 is +x, counting towards +y; see the Direction <-> Displacement mapping.
	constexpr Direction Octants[8] = {
		Direction::SouthEast, Direction::South, Direction::SouthWest, Direction::West,
		Direction::NorthWest, Direction::North, Direction::NorthEast, Direction::East
	};
	constexpr float Pi = 3.14159265F;
	int octant = static_cast<int>(std::lround(yaw / (Pi / 4.0F))) % 8;
	if (octant < 0)
		octant += 8;
	return Octants[octant];
}

} // namespace devilution::fp
