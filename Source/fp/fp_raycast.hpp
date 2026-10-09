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
/**
 * @brief Original art pixels per world unit (tile).
 *
 * The iso projection is dimetric with a 30 degree camera elevation (hence 2:1 tiles): one tile across the
 * view is 32 * sqrt(2) ~ 45.25px, and heights are foreshortened by cos(30) to ~39.2px per tile.
 */
constexpr float SpritePixelsPerTileX = 45.25F;
constexpr float SpritePixelsPerTileY = 39.2F;
/** A warrior sprite is ~1.8 tiles tall at this scale. */
constexpr float EyeHeight = 1.6F;

/** @brief Wall (and ceiling) height of the current level, from the height of its wall art. */
[[nodiscard]] float WallHeight();
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
	bool backFace;     // the face points away from the iso camera (-x / -y side), so the art never showed it
	Vec2 point;        // where the ray met the face
};

[[nodiscard]] bool IsOpaque(Point tile);

/** @brief Casts a ray over the dPiece grid until it hits an opaque tile. */
[[nodiscard]] RayHit CastRay(Vec2 origin, Vec2 ray);

} // namespace devilution::fp
