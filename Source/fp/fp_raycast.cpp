/**
 * @file fp/fp_raycast.cpp
 *
 * Camera projection and grid ray casting shared by the first-person renderer and crosshair picking.
 */
#include "fp/fp_raycast.hpp"

#include <cmath>

#include "fp/fp_controls.hpp"
#include "levels/dun_tile_data.hpp"
#include "levels/tile_properties.hpp"
#include "utils/ui_fwd.h"

namespace devilution::fp {

Projection GetProjection()
{
	const ViewState &view = GetViewState();
	const Vec2 dir { std::cos(view.yaw), std::sin(view.yaw) };
	Projection projection;
	projection.pos = { view.x, view.y };
	projection.dir = dir;
	projection.plane = { -dir.y * PlaneLength, dir.x * PlaneLength };
	projection.width = gnScreenWidth;
	projection.height = gnViewportHeight;
	projection.focal = static_cast<float>(projection.width) / 2.0F / PlaneLength;
	projection.horizon = projection.height / 2 + static_cast<int>(view.pitch * projection.focal);
	return projection;
}

bool IsOpaque(Point tile)
{
	return !InDungeonBounds(tile) || IsTileSolid(tile);
}

RayHit CastRay(Vec2 origin, Vec2 ray)
{
	Point tile { static_cast<int>(std::floor(origin.x)), static_cast<int>(std::floor(origin.y)) };
	const float deltaX = ray.x == 0 ? 1e30F : std::abs(1.0F / ray.x);
	const float deltaY = ray.y == 0 ? 1e30F : std::abs(1.0F / ray.y);
	const int stepX = ray.x < 0 ? -1 : 1;
	const int stepY = ray.y < 0 ? -1 : 1;
	float sideX = ray.x < 0 ? (origin.x - static_cast<float>(tile.x)) * deltaX : (static_cast<float>(tile.x) + 1.0F - origin.x) * deltaX;
	float sideY = ray.y < 0 ? (origin.y - static_cast<float>(tile.y)) * deltaY : (static_cast<float>(tile.y) + 1.0F - origin.y) * deltaY;

	RayHit result { false, MaxRayDistance, tile, tile, false };
	while (true) {
		result.previous = tile;
		float distance;
		if (sideX < sideY) {
			distance = sideX;
			sideX += deltaX;
			tile.x += stepX;
			result.xSide = true;
		} else {
			distance = sideY;
			sideY += deltaY;
			tile.y += stepY;
			result.xSide = false;
		}
		if (distance > MaxRayDistance)
			return result;
		if (IsOpaque(tile)) {
			result.hit = true;
			result.distance = distance;
			result.tile = tile;
			return result;
		}
	}
}

} // namespace devilution::fp
