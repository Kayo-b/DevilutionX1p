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
#include "fp/fp_textures.hpp"
#include "fp/fp_world.hpp"
#include "levels/dun_tile_data.hpp"
#include "levels/gendung.h"
#include "utils/clx_decode.hpp"
#include "utils/str_cat.hpp"

namespace devilution::fp {

namespace {

bool FirstPersonActive = false;

uint8_t Shade(uint8_t color, uint8_t light)
{
	return LightTables[std::min<uint8_t>(light, LightsMax)][color];
}

/** @brief Colour of the open sky above town, which has no ceiling. */
constexpr uint8_t TownSkyColor = PAL16_BLUE + 14;
/** @brief Light levels added to ceilings so they read as darker than the floor they mirror. */
constexpr uint8_t CeilingExtraDarkness = 4;

void DrawFloorAndCeiling(const Surface &out, const Projection &proj)
{
	const Vec2 rayLeft { proj.dir.x - proj.plane.x, proj.dir.y - proj.plane.y };
	const Vec2 rayRight { proj.dir.x + proj.plane.x, proj.dir.y + proj.plane.y };
	const float wallHeight = WallHeight();
	const bool isTown = leveltype == DTYPE_TOWN;
	const bool hasCeiling = !isTown;

	for (int y = 0; y < proj.height; y++) {
		const bool isFloor = y > proj.horizon;
		uint8_t *dst = out.at(0, y);
		if (!isFloor && !hasCeiling) {
			std::fill_n(dst, proj.width, TownSkyColor);
			continue;
		}
		const int rowsFromHorizon = isFloor ? y - proj.horizon : proj.horizon - y;
		if (rowsFromHorizon == 0) {
			std::fill_n(dst, proj.width, 0);
			continue;
		}
		const float heightAboveOrBelow = isFloor ? EyeHeight : wallHeight - EyeHeight;
		const float rowDistance = heightAboveOrBelow * proj.focal / static_cast<float>(rowsFromHorizon);

		const float stepX = rowDistance * (rayRight.x - rayLeft.x) / static_cast<float>(proj.width);
		const float stepY = rowDistance * (rayRight.y - rayLeft.y) / static_cast<float>(proj.width);
		float wx = proj.pos.x + rowDistance * rayLeft.x;
		float wy = proj.pos.y + rowDistance * rayLeft.y;

		for (int x = 0; x < proj.width; x++, wx += stepX, wy += stepY) {
			const float fx = std::floor(wx);
			const float fy = std::floor(wy);
			const Point tile { static_cast<int>(fx), static_cast<int>(fy) };
			if (!InDungeonBounds(tile)) {
				dst[x] = 0;
				continue;
			}
			const std::vector<int16_t> &floor = GetPieceTextures(dPiece[tile.x][tile.y]).floor;
			const int u = std::min(static_cast<int>((wx - fx) * FloorTextureSize), FloorTextureSize - 1);
			const int v = std::min(static_cast<int>((wy - fy) * FloorTextureSize), FloorTextureSize - 1);
			int16_t texel = floor[v * FloorTextureSize + u];
			if (isTown && texel <= 0)
				texel = GetGroundFloor()[v * FloorTextureSize + u]; // baked shadows and holes in town ground
			uint8_t light = LightAt(tile);
			if (!isFloor)
				light = std::min<uint8_t>(light + CeilingExtraDarkness, LightsMax);
			dst[x] = texel == NoPixel ? 0 : Shade(static_cast<uint8_t>(texel), light);
		}
	}
}

/** @brief The face texture for a wall hit: the piece's own art for that face, else the other face, else the level's plain wall. */
const std::vector<int16_t> &FaceForHit(const RayHit &hit)
{
	if (!InDungeonBounds(hit.tile)) {
		// The edge of the map is a plain black wall.
		static const std::vector<int16_t> Black(static_cast<size_t>(FaceTextureWidth) * ColumnPixelHeight, 0);
		return Black;
	}
	const PieceTextures &textures = GetPieceTextures(dPiece[hit.tile.x][hit.tile.y]);
	const bool wantX = hit.xSide;
	if (wantX ? textures.faceXUsable : textures.faceYUsable)
		return wantX ? textures.faceX : textures.faceY;
	if (wantX ? textures.faceYUsable : textures.faceXUsable)
		return wantX ? textures.faceY : textures.faceX;
	return GetFallbackFace();
}

void DrawWalls(const Surface &out, const Projection &proj, std::vector<float> &zBuffer)
{
	const float wallHeight = WallHeight();
	const int faceHeight = FaceTextureHeight();
	const std::vector<int16_t> &fallback = GetFallbackFace();

	for (int x = 0; x < proj.width; x++) {
		const RayHit hit = CastRay(proj.pos, proj.columnRay(x));
		zBuffer[x] = hit.distance;
		if (!hit.hit)
			continue;

		const float scale = proj.focal / std::max(hit.distance, 0.01F);
		const float topF = static_cast<float>(proj.horizon) - (wallHeight - EyeHeight) * scale;
		const float bottomF = static_cast<float>(proj.horizon) + EyeHeight * scale;
		const int top = std::max(0, static_cast<int>(topF));
		const int bottom = std::min(proj.height - 1, static_cast<int>(bottomF));

		// Position along the face in world-axis order; the hidden back faces show the art mirrored.
		float along = hit.xSide ? hit.point.y - static_cast<float>(hit.tile.y) : hit.point.x - static_cast<float>(hit.tile.x);
		if (hit.backFace)
			along = 1.0F - along;
		const int u = std::clamp(static_cast<int>(along * FaceTextureWidth), 0, FaceTextureWidth - 1);

		const std::vector<int16_t> &face = FaceForHit(hit);
		const uint8_t *light = LightTables[LightAt(hit.previous)].data();
		const float rowsPerPixel = static_cast<float>(faceHeight) / (bottomF - topF);
		for (int y = top; y <= bottom; y++) {
			const int row = std::clamp(static_cast<int>((static_cast<float>(y) - topF) * rowsPerPixel), 0, faceHeight - 1);
			int16_t texel = face[row * FaceTextureWidth + u];
			if (texel == NoPixel)
				texel = fallback[row * FaceTextureWidth + u];
			*out.at(x, y) = light[texel];
		}
	}
}

void DrawSpriteBillboard(const Surface &out, const Projection &proj, const std::vector<float> &zBuffer, const Entity &entity, const BillboardRect &rect)
{
	static std::vector<int16_t> decoded;
	const std::vector<int16_t> *source = entity.pixels;
	int spriteWidth = entity.pixelsWidth;
	int spriteHeight = entity.pixelsHeight;
	if (source == nullptr) {
		const ClxSprite sprite = *entity.sprite;
		DecodeClx(sprite, decoded);
		source = &decoded;
		spriteWidth = sprite.width();
		spriteHeight = sprite.height();
	}
	const std::vector<int16_t> &pixels = *source;
	const uint8_t *light = entity.lit ? LightTables[LightAt(entity.tile)].data() : nullptr;
	const int screenWidth = rect.right - rect.left;
	const int screenHeight = rect.bottom - rect.top;

	const int x0 = std::max(0, rect.left);
	const int x1 = std::min(proj.width, rect.right);
	const int y0 = std::max(0, rect.top);
	const int y1 = std::min(proj.height, rect.bottom);
	for (int x = x0; x < x1; x++) {
		if (zBuffer[x] < rect.depth)
			continue;
		const int u = (x - rect.left) * spriteWidth / screenWidth;
		for (int y = y0; y < y1; y++) {
			const int v = (y - rect.top) * spriteHeight / screenHeight;
			const int16_t pixel = pixels[v * spriteWidth + u];
			// Colour 0 is the baked-in ground shadow, which would stand upright on a billboard.
			if (pixel <= 0)
				continue;
			uint8_t color = static_cast<uint8_t>(pixel);
			if (entity.trn != nullptr)
				color = entity.trn[color];
			else if (light != nullptr)
				color = light[color];
			*out.at(x, y) = color;
		}
	}
}

void DrawEntities(const Surface &out, const Projection &proj, const std::vector<float> &zBuffer)
{
	static std::vector<Entity> entities;
	static std::vector<std::pair<BillboardRect, const Entity *>> sorted;
	CollectEntities(proj.pos, entities);

	sorted.clear();
	for (const Entity &entity : entities) {
		const BillboardRect rect = ProjectBillboard(proj, entity);
		if (rect.visible && (entity.sprite || entity.pixels != nullptr))
			sorted.emplace_back(rect, &entity);
	}
	// Far to near, so nearer sprites overwrite farther ones.
	std::sort(sorted.begin(), sorted.end(), [](const auto &a, const auto &b) { return a.first.depth > b.first.depth; });

	for (const auto &[rect, entity] : sorted)
		DrawSpriteBillboard(out, proj, zBuffer, *entity, rect);
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
