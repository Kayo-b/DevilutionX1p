/**
 * @file fp/fp_view.cpp
 *
 * First-person view of the live level (software raycaster).
 *
 * World space is the dPiece grid: one unit per tile, the tile (x, y) covering [x, x+1) x [y, y+1).
 * The iso projection is a rotation of this grid (screen x = (x - y) * 32, screen y = (x + y) * 16),
 * so the handedness matches a y-down top-down map.
 */
#include "fp/fp_view.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

#ifdef USE_SDL3
#include <SDL3/SDL_timer.h>
#else
#include <SDL.h>
#endif

#include "engine/displacement.hpp"
#include "engine/light_tables.hpp"
#include "engine/palette.h"
#include "engine/render/scrollrt.h"
#include "items.h"
#include "levels/dun_tile_data.hpp"
#include "levels/tile_properties.hpp"
#include "monster.h"
#include "player.h"

namespace devilution::fp {

namespace {

bool FirstPersonActive = false;

// Tunables (world units = tiles).
constexpr float PlaneLength = 1.0F; // tan(FOV / 2), FOV = 90 degrees
constexpr float WallHeight = 1.5F;
constexpr float EyeHeight = 0.75F;
constexpr float MaxRayDistance = 48.0F;

struct Vec2 {
	float x;
	float y;
};

struct Camera {
	Vec2 pos;
	Vec2 dir;   // unit length
	Vec2 plane; // perpendicular to dir, length PlaneLength
	float yaw;
};

float CurrentYaw = 0.0F;
uint32_t LastYawTicks = 0;

/** @brief Converts an iso screen-space pixel displacement into world (tile) units. */
Vec2 ScreenToWorld(Displacement offset)
{
	const float sx = static_cast<float>(offset.deltaX) / 32.0F;
	const float sy = static_cast<float>(offset.deltaY) / 16.0F;
	return { (sx + sy) / 2.0F, (sy - sx) / 2.0F };
}

float DirectionToYaw(Direction dir)
{
	const Displacement d { dir };
	return std::atan2(static_cast<float>(d.deltaY), static_cast<float>(d.deltaX));
}

/** @brief Eases the camera yaw towards the player's facing so 45 degree turns don't snap. */
float UpdateYaw(float targetYaw)
{
	const uint32_t now = SDL_GetTicks();
	const float dt = LastYawTicks == 0 ? 1.0F : static_cast<float>(now - LastYawTicks) / 1000.0F;
	LastYawTicks = now;

	constexpr float Pi = 3.14159265F;
	float delta = std::remainder(targetYaw - CurrentYaw, 2.0F * Pi);
	const float t = 1.0F - std::exp(-dt * 12.0F);
	CurrentYaw = std::remainder(CurrentYaw + delta * t, 2.0F * Pi);
	return CurrentYaw;
}

Camera BuildCamera()
{
	const Player &player = *MyPlayer;
	Vec2 pos { static_cast<float>(player.position.tile.x) + 0.5F, static_cast<float>(player.position.tile.y) + 0.5F };
	if (player.isWalking()) {
		const Vec2 walk = ScreenToWorld(GetOffsetForWalking(player.AnimInfo, player._pdir));
		pos.x += walk.x;
		pos.y += walk.y;
	}

	const float yaw = UpdateYaw(DirectionToYaw(player._pdir));
	const Vec2 dir { std::cos(yaw), std::sin(yaw) };
	return { pos, dir, { -dir.y * PlaneLength, dir.x * PlaneLength }, yaw };
}

bool IsOpaque(Point tile)
{
	return !InDungeonBounds(tile) || IsTileSolid(tile);
}

uint8_t LightAt(Point tile)
{
	if (!InDungeonBounds(tile))
		return LightsMax;
	return dLight[tile.x][tile.y];
}

uint8_t Shade(uint8_t color, uint8_t light)
{
	return LightTables[std::min<uint8_t>(light, LightsMax)][color];
}

void DrawFloorAndCeiling(const Surface &out, const Camera &cam, int horizon, float focal)
{
	const int width = out.w();
	const int height = out.h();
	const Vec2 rayLeft { cam.dir.x - cam.plane.x, cam.dir.y - cam.plane.y };
	const Vec2 rayRight { cam.dir.x + cam.plane.x, cam.dir.y + cam.plane.y };

	for (int y = 0; y < height; y++) {
		const bool isFloor = y > horizon;
		const int rowsFromHorizon = isFloor ? y - horizon : horizon - y;
		if (rowsFromHorizon == 0)
			continue;
		const float heightAboveOrBelow = isFloor ? EyeHeight : WallHeight - EyeHeight;
		const float rowDistance = heightAboveOrBelow * focal / static_cast<float>(rowsFromHorizon);

		const float stepX = rowDistance * (rayRight.x - rayLeft.x) / static_cast<float>(width);
		const float stepY = rowDistance * (rayRight.y - rayLeft.y) / static_cast<float>(width);
		float wx = cam.pos.x + rowDistance * rayLeft.x;
		float wy = cam.pos.y + rowDistance * rayLeft.y;

		uint8_t *dst = out.at(0, y);
		for (int x = 0; x < width; x++, wx += stepX, wy += stepY) {
			const Point tile { static_cast<int>(std::floor(wx)), static_cast<int>(std::floor(wy)) };
			const uint8_t light = LightAt(tile);
			uint8_t color;
			if (isFloor) {
				color = ((tile.x + tile.y) & 1) != 0 ? PAL16_BEIGE + 6 : PAL16_BEIGE + 9;
			} else {
				color = PAL16_GRAY + 10;
			}
			dst[x] = Shade(color, light);
		}
	}
}

void DrawWalls(const Surface &out, const Camera &cam, int horizon, float focal, std::vector<float> &zBuffer)
{
	const int width = out.w();
	const int height = out.h();

	for (int x = 0; x < width; x++) {
		const float cameraX = 2.0F * static_cast<float>(x) / static_cast<float>(width) - 1.0F;
		const Vec2 ray { cam.dir.x + cam.plane.x * cameraX, cam.dir.y + cam.plane.y * cameraX };

		Point tile { static_cast<int>(std::floor(cam.pos.x)), static_cast<int>(std::floor(cam.pos.y)) };
		const float deltaX = ray.x == 0 ? 1e30F : std::abs(1.0F / ray.x);
		const float deltaY = ray.y == 0 ? 1e30F : std::abs(1.0F / ray.y);
		const int stepX = ray.x < 0 ? -1 : 1;
		const int stepY = ray.y < 0 ? -1 : 1;
		float sideX = ray.x < 0 ? (cam.pos.x - static_cast<float>(tile.x)) * deltaX : (static_cast<float>(tile.x) + 1.0F - cam.pos.x) * deltaX;
		float sideY = ray.y < 0 ? (cam.pos.y - static_cast<float>(tile.y)) * deltaY : (static_cast<float>(tile.y) + 1.0F - cam.pos.y) * deltaY;

		Point previous = tile;
		bool hitXSide = false;
		bool hit = false;
		float distance = MaxRayDistance;
		while (true) {
			previous = tile;
			if (sideX < sideY) {
				distance = sideX;
				sideX += deltaX;
				tile.x += stepX;
				hitXSide = true;
			} else {
				distance = sideY;
				sideY += deltaY;
				tile.y += stepY;
				hitXSide = false;
			}
			if (distance > MaxRayDistance)
				break;
			if (IsOpaque(tile)) {
				hit = true;
				break;
			}
		}

		zBuffer[x] = distance;
		if (!hit)
			continue;

		const float invDistance = focal / std::max(distance, 0.01F);
		const int top = std::max(0, horizon - static_cast<int>((WallHeight - EyeHeight) * invDistance));
		const int bottom = std::min(height - 1, horizon + static_cast<int>(EyeHeight * invDistance));
		// Flat debug colours: x-faces and y-faces differ so corners read clearly.
		const uint8_t color = Shade(hitXSide ? PAL16_BLUE + 4 : PAL16_BLUE + 8, LightAt(previous));
		for (int y = top; y <= bottom; y++)
			*out.at(x, y) = color;
	}
}

/** @brief Debug billboard: a flat coloured box standing on the floor at a world position. */
void DrawMarker(const Surface &out, const Camera &cam, int horizon, float focal, const std::vector<float> &zBuffer,
    Vec2 pos, float markerWidth, float markerHeight, uint8_t color, uint8_t light)
{
	const Vec2 rel { pos.x - cam.pos.x, pos.y - cam.pos.y };
	const float depth = rel.x * cam.dir.x + rel.y * cam.dir.y;
	if (depth < 0.1F)
		return;
	const float lateral = (rel.x * cam.plane.x + rel.y * cam.plane.y) / (PlaneLength * PlaneLength);
	const int width = out.w();
	const int height = out.h();
	const int centerX = static_cast<int>((lateral / depth + 1.0F) * static_cast<float>(width) / 2.0F);
	const float scale = focal / depth;
	const int halfWidth = std::max(1, static_cast<int>(markerWidth * scale / 2.0F));
	const int top = std::max(0, horizon - static_cast<int>((markerHeight - EyeHeight) * scale));
	const int bottom = std::min(height - 1, horizon + static_cast<int>(EyeHeight * scale));
	const uint8_t shaded = Shade(color, light);

	for (int x = std::max(0, centerX - halfWidth); x < std::min(width, centerX + halfWidth); x++) {
		if (zBuffer[x] < depth)
			continue;
		for (int y = top; y <= bottom; y++)
			*out.at(x, y) = shaded;
	}
}

void DrawMarkers(const Surface &out, const Camera &cam, int horizon, float focal, const std::vector<float> &zBuffer)
{
	for (size_t i = 0; i < ActiveItemCount; i++) {
		const Item &item = Items[ActiveItems[i]];
		if (!IsTileVisible(item.position))
			continue;
		DrawMarker(out, cam, horizon, focal, zBuffer,
		    { static_cast<float>(item.position.x) + 0.5F, static_cast<float>(item.position.y) + 0.5F },
		    0.25F, 0.2F, PAL16_YELLOW + 2, LightAt(item.position));
	}

	for (size_t i = 0; i < ActiveMonsterCount; i++) {
		const Monster &monster = Monsters[ActiveMonsters[i]];
		if (!IsTileVisible(monster.position.tile))
			continue;
		Vec2 pos { static_cast<float>(monster.position.tile.x) + 0.5F, static_cast<float>(monster.position.tile.y) + 0.5F };
		if (monster.isWalking()) {
			const Vec2 walk = ScreenToWorld(GetOffsetForWalking(monster.animInfo, monster.direction));
			pos.x += walk.x;
			pos.y += walk.y;
		}
		DrawMarker(out, cam, horizon, focal, zBuffer, pos, 0.5F, 1.1F, PAL16_RED + 4, LightAt(monster.position.tile));
	}
}

void DrawCrosshair(const Surface &out)
{
	const int cx = out.w() / 2;
	const int cy = out.h() / 2;
	for (int i = 3; i <= 7; i++) {
		out.SetPixel({ cx - i, cy }, PAL16_GRAY);
		out.SetPixel({ cx + i, cy }, PAL16_GRAY);
		out.SetPixel({ cx, cy - i }, PAL16_GRAY);
		out.SetPixel({ cx, cy + i }, PAL16_GRAY);
	}
}

} // namespace

bool IsFirstPersonActive()
{
	return FirstPersonActive;
}

void ToggleFirstPerson()
{
	FirstPersonActive = !FirstPersonActive;
	if (FirstPersonActive) {
		CurrentYaw = DirectionToYaw(MyPlayer->_pdir);
		LastYawTicks = 0;
	}
}

void DrawFirstPersonView(const Surface &out)
{
	static std::vector<float> zBuffer;
	zBuffer.resize(static_cast<size_t>(out.w()));

	const Camera cam = BuildCamera();
	const float focal = static_cast<float>(out.w()) / 2.0F / PlaneLength;
	const int horizon = out.h() / 2;

	DrawFloorAndCeiling(out, cam, horizon, focal);
	DrawWalls(out, cam, horizon, focal, zBuffer);
	DrawMarkers(out, cam, horizon, focal, zBuffer);
	DrawCrosshair(out);
}

} // namespace devilution::fp
