/**
 * @file fp/fp_raycast.hpp
 *
 * Camera projection and grid ray casting shared by the first-person renderer and crosshair picking.
 */
#pragma once

#include "engine/point.hpp"
#include "fp/fp_math.hpp"

namespace devilution::fp {

// World units are tiles.
constexpr float PlaneLength = 1.0F; // tan(FOV / 2), FOV = 90 degrees
// Matched to the original art (see SpritePixelsPerTileY): a warrior is ~1.8 tiles tall and cathedral
// walls are about twice that.
constexpr float WallHeight = 3.4F;
constexpr float EyeHeight = 1.6F;
constexpr float MaxRayDistance = 48.0F;

/** @brief Camera plus the screen mapping of the game viewport. */
struct Projection {
	Vec2 pos;
	Vec2 dir;   // unit length
	Vec2 plane; // perpendicular to dir, length PlaneLength
	int width;
	int height;
	float focal;
	int horizon;

	/** @brief Un-normalised ray direction through screen column x; points along it are pos + ray * depth. */
	[[nodiscard]] Vec2 columnRay(int x) const
	{
		const float cameraX = 2.0F * static_cast<float>(x) / static_cast<float>(width) - 1.0F;
		return { dir.x + plane.x * cameraX, dir.y + plane.y * cameraX };
	}

	/** @brief Height change per unit of depth for a ray through screen row y. */
	[[nodiscard]] float rowSlope(int y) const
	{
		return static_cast<float>(horizon - y) / focal;
	}
};

/** @brief Projection for the current view state and game viewport size. */
[[nodiscard]] Projection GetProjection();

struct RayHit {
	bool hit;          // false if MaxRayDistance was reached first
	float distance;    // perpendicular depth of the hit face
	Point tile;        // the opaque tile that was hit
	Point previous;    // the last open tile before it
	bool xSide;        // whether an x-facing (vertical in the grid) face was hit
};

[[nodiscard]] bool IsOpaque(Point tile);

/** @brief Casts a ray over the dPiece grid until it hits an opaque tile. */
[[nodiscard]] RayHit CastRay(Vec2 origin, Vec2 ray);

} // namespace devilution::fp
