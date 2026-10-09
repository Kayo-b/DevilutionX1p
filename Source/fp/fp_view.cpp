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
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "engine/light_tables.hpp"
#include "engine/palette.h"
#include "engine/render/text_render.hpp"
#include "fp/fp_controls.hpp"
#include "fp/fp_raycast.hpp"
#include "fp/fp_world.hpp"
#include "levels/dun_tile_data.hpp"
#include "levels/gendung.h"
#include "utils/str_cat.hpp"

namespace devilution::fp {

namespace {

bool FirstPersonActive = false;

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

void DrawFloorAndCeiling(const Surface &out, const Projection &proj)
{
	const Vec2 rayLeft { proj.dir.x - proj.plane.x, proj.dir.y - proj.plane.y };
	const Vec2 rayRight { proj.dir.x + proj.plane.x, proj.dir.y + proj.plane.y };

	for (int y = 0; y < proj.height; y++) {
		const bool isFloor = y > proj.horizon;
		const int rowsFromHorizon = isFloor ? y - proj.horizon : proj.horizon - y;
		if (rowsFromHorizon == 0)
			continue;
		const float heightAboveOrBelow = isFloor ? EyeHeight : WallHeight - EyeHeight;
		const float rowDistance = heightAboveOrBelow * proj.focal / static_cast<float>(rowsFromHorizon);

		const float stepX = rowDistance * (rayRight.x - rayLeft.x) / static_cast<float>(proj.width);
		const float stepY = rowDistance * (rayRight.y - rayLeft.y) / static_cast<float>(proj.width);
		float wx = proj.pos.x + rowDistance * rayLeft.x;
		float wy = proj.pos.y + rowDistance * rayLeft.y;

		uint8_t *dst = out.at(0, y);
		for (int x = 0; x < proj.width; x++, wx += stepX, wy += stepY) {
			const Point tile { static_cast<int>(std::floor(wx)), static_cast<int>(std::floor(wy)) };
			uint8_t color;
			if (isFloor) {
				color = ((tile.x + tile.y) & 1) != 0 ? PAL16_BEIGE + 6 : PAL16_BEIGE + 9;
			} else {
				color = PAL16_GRAY + 10;
			}
			dst[x] = Shade(color, LightAt(tile));
		}
	}
}

void DrawWalls(const Surface &out, const Projection &proj, std::vector<float> &zBuffer)
{
	for (int x = 0; x < proj.width; x++) {
		const RayHit hit = CastRay(proj.pos, proj.columnRay(x));
		zBuffer[x] = hit.distance;
		if (!hit.hit)
			continue;

		const float scale = proj.focal / std::max(hit.distance, 0.01F);
		const int top = std::max(0, proj.horizon - static_cast<int>((WallHeight - EyeHeight) * scale));
		const int bottom = std::min(proj.height - 1, proj.horizon + static_cast<int>(EyeHeight * scale));
		// Flat debug colours: x-faces and y-faces differ so corners read clearly.
		const uint8_t color = Shade(hit.xSide ? PAL16_BLUE + 4 : PAL16_BLUE + 8, LightAt(hit.previous));
		for (int y = top; y <= bottom; y++)
			*out.at(x, y) = color;
	}
}

uint8_t EntityColor(EntityKind kind)
{
	switch (kind) {
	case EntityKind::Monster:
		return PAL16_RED + 4;
	case EntityKind::Towner:
		return PAL16_ORANGE + 4;
	case EntityKind::Object:
		return PAL8_YELLOW + 2;
	case EntityKind::Item:
		return PAL16_YELLOW + 2;
	}
	return PAL16_GRAY;
}

/** @brief Placeholder billboards (flat coloured boxes) until real sprites land in M3. */
void DrawEntities(const Surface &out, const Projection &proj, const std::vector<float> &zBuffer)
{
	static std::vector<Entity> entities;
	static std::vector<std::pair<BillboardRect, const Entity *>> sorted;
	CollectEntities(entities);

	sorted.clear();
	for (const Entity &entity : entities) {
		const BillboardRect rect = ProjectBillboard(proj, entity.pos, entity.width, entity.height);
		if (rect.visible)
			sorted.emplace_back(rect, &entity);
	}
	// Far to near, so nearer boxes overwrite farther ones.
	std::sort(sorted.begin(), sorted.end(), [](const auto &a, const auto &b) { return a.first.depth > b.first.depth; });

	for (const auto &[rect, entity] : sorted) {
		const uint8_t color = Shade(EntityColor(entity->kind), LightAt(entity->tile));
		const int top = std::max(0, rect.top);
		const int bottom = std::min(proj.height - 1, rect.bottom);
		for (int x = std::max(0, rect.centerX - rect.halfWidth); x < std::min(proj.width, rect.centerX + rect.halfWidth); x++) {
			if (zBuffer[x] < rect.depth)
				continue;
			for (int y = top; y <= bottom; y++)
				*out.at(x, y) = color;
		}
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

/** @brief Temporary readout used while verifying coordinates; remove once M4 is confirmed. */
void DrawDebugInfo(const Surface &out, const Projection &proj)
{
	const Point here { static_cast<int>(std::floor(proj.pos.x)), static_cast<int>(std::floor(proj.pos.y)) };
	const std::string text = StrCat("tile ", here.x, ",", here.y, " light ", static_cast<int>(LightAt(here)), " lvl ", static_cast<int>(leveltype));
	DrawString(out, text, { { 8, 8 }, { out.w() - 16, 20 } });
}

} // namespace

bool IsFirstPersonActive()
{
	return FirstPersonActive;
}

void ToggleFirstPerson()
{
	FirstPersonActive = !FirstPersonActive;
	if (FirstPersonActive)
		ResetViewState();
	else
		ReleaseInput();
}

void DrawFirstPersonView(const Surface &out)
{
	static std::vector<float> zBuffer;

	const Projection proj = GetProjection();
	zBuffer.resize(static_cast<size_t>(proj.width));

	DrawFloorAndCeiling(out, proj);
	DrawWalls(out, proj, zBuffer);
	DrawEntities(out, proj, zBuffer);
	if (IsMouseLookActive())
		DrawCrosshair(out);
	DrawDebugInfo(out, proj);
}

} // namespace devilution::fp
