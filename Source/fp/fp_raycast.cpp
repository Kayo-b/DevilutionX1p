/**
 * @file fp/fp_raycast.cpp
 *
 * Camera projection and grid ray casting shared by the first-person renderer and crosshair picking.
 */
#include "fp/fp_raycast.hpp"

#include "fp/fp_buildings.hpp"

#include <algorithm>
#include <cmath>

#include "fp/fp_controls.hpp"
#include "fp/fp_doors.hpp"
#include "fp/fp_dungeon.hpp"
#include "fp/fp_hell.hpp"
#include "fp/fp_textures.hpp"
#include "levels/dun_tile_data.hpp"
#include "levels/gendung.h"
#include "levels/tile_properties.hpp"
#include "options.h"
#include "utils/ui_fwd.h"

namespace devilution::fp {

float VerticalScale()
{
	return leveltype == DTYPE_TOWN ? 1.0F : 1.5F;
}

float WallHeight()
{
	return static_cast<float>(WallShownRows(LevelWallRows())) / SpritePixelsPerTileY;
}

float PlaneLength()
{
	constexpr float DegreesToRadians = 3.14159265F / 180.0F;
	const int fov = std::clamp(*GetOptions().Graphics.fieldOfView, 30, 150);
	return std::tan(static_cast<float>(fov) * 0.5F * DegreesToRadians);
}

Projection GetProjection()
{
	const ViewState &view = GetViewState();
	const Camera &camera = GetCamera();
	const Vec2 dir { std::cos(view.yaw), std::sin(view.yaw) };
	const float planeLength = PlaneLength();
	Projection projection;
	projection.pos = camera.pos;
	projection.eye = camera.eye;
	projection.dir = dir;
	projection.plane = { -dir.y * planeLength, dir.x * planeLength };
	projection.planeLength = planeLength;
	projection.width = gnScreenWidth;
	projection.height = gnViewportHeight;
	projection.focal = static_cast<float>(projection.width) / 2.0F / planeLength;
	projection.horizon = projection.height / 2 + static_cast<int>((view.pitch + camera.pitch) * projection.focal);
	return projection;
}

bool IsOpaque(Point tile)
{
	// Town has no walls: its buildings and trees are meshes and the rest is ground. Past the map's edge the sky
	// shows (the ground there fades into the fog), not a black wall.
	if (leveltype == DTYPE_TOWN)
		return false;
	if (!InDungeonBounds(tile))
		return true;
	// A doorway stays a wall with an opening when its door is open (and walkable); the caves' gates stand in their
	// fences, meshes.
	if (IsDoorwayTile(tile))
		return !CleanDoorwaysAreGates();
	if (!IsTileSolid(tile))
		return false;
	if (IsCleanDungeon()) {
		// Pillars and torch stands are meshes; lava and water are floor; the stairs' tiles are their models.
		const CleanKind kind = CleanKindAt(tile);
		return (kind == CleanKind::Wall || kind == CleanKind::Stairs) && !IsStructureTile(tile);
	}
	// Free-standing columns and the stairs down are drawn as meshes (or billboards), so rays pass them.
	if (GetPieceTextures(dPiece[tile.x][tile.y]).isFlatSolid)
		return false; // lava, water: seen as floor
	return !GetPieceTextures(dPiece[tile.x][tile.y]).isFreeStanding && !IsStructureTile(tile);
}

RayHit CastRay(Vec2 origin, Vec2 ray, int skip)
{
	Point tile { static_cast<int>(std::floor(origin.x)), static_cast<int>(std::floor(origin.y)) };
	const float deltaX = ray.x == 0 ? 1e30F : std::abs(1.0F / ray.x);
	const float deltaY = ray.y == 0 ? 1e30F : std::abs(1.0F / ray.y);
	const int stepX = ray.x < 0 ? -1 : 1;
	const int stepY = ray.y < 0 ? -1 : 1;
	float sideX = ray.x < 0 ? (origin.x - static_cast<float>(tile.x)) * deltaX : (static_cast<float>(tile.x) + 1.0F - origin.x) * deltaX;
	float sideY = ray.y < 0 ? (origin.y - static_cast<float>(tile.y)) * deltaY : (static_cast<float>(tile.y) + 1.0F - origin.y) * deltaY;

	RayHit result { false, MaxRayDistance, tile, tile, false, false, origin };
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
			if (skip > 0) {
				skip--;
				continue;
			}
			result.hit = true;
			result.distance = distance;
			result.tile = tile;
			// Moving towards +x hits the tile's -x face, which faces away from the iso camera.
			result.backFace = result.xSide ? stepX > 0 : stepY > 0;
			result.point = { origin.x + ray.x * distance, origin.y + ray.y * distance };
			return result;
		}
	}
}

bool IsHoleAt(const RayHit &hit, float height)
{
	if (!InDungeonBounds(hit.tile) || height < 0.0F)
		return false;
	if (DoorwaySpanAt(hit.tile).door != nullptr && IsDoorwayHole(hit, height))
		return true;
	if (IsHell())
		return height > CleanWallHeight(hit.tile); // its walls are low: aimed over, the ray goes on
	if (IsCleanDungeon())
		return false; // clean walls are solid: aiming at a stairwell walks to its trigger
	const PieceTextures &textures = GetPieceTextures(dPiece[hit.tile.x][hit.tile.y]);
	const bool wantX = hit.xSide;
	const bool ownUsable = wantX ? textures.faceXUsable : textures.faceYUsable;
	const bool otherUsable = wantX ? textures.faceYUsable : textures.faceXUsable;
	if (!ownUsable && !otherUsable)
		return false;
	const bool useX = ownUsable ? wantX : !wantX;
	const std::vector<uint8_t> &holes = useX ? textures.holesX : textures.holesY;
	float along = hit.xSide ? hit.point.y - static_cast<float>(hit.tile.y) : hit.point.x - static_cast<float>(hit.tile.x);
	const int u = std::clamp(static_cast<int>(along * FaceTextureWidth), 0, FaceTextureWidth - 1);
	const int faceHeight = FaceTextureHeight();
	const int row = WallArtRow(height / VerticalScale());
	if (row < 0 || row >= faceHeight)
		return false;
	return holes[static_cast<size_t>(row) * FaceTextureWidth + u] != 0;
}

RayHit CastRayThroughHoles(Vec2 origin, float eye, Vec2 ray, float slope)
{
	RayHit hit = CastRay(origin, ray);
	for (int skip = 1; hit.hit && skip < MaxWallLayers; skip++) {
		if (!IsHoleAt(hit, eye + slope * hit.distance))
			break;
		hit = CastRay(origin, ray, skip);
	}
	return hit;
}

} // namespace devilution::fp
