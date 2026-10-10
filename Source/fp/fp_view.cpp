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
#include <limits>
#include <string>
#include <vector>

#include "engine/light_tables.hpp"
#include "engine/palette.h"
#include "engine/render/text_render.hpp"
#include "fp/fp_controls.hpp"
#include "fp/fp_buildings.hpp"
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

/** @brief The art and openings used for one wall face. */
struct FaceArt {
	const std::vector<int16_t> *face;
	const std::vector<uint8_t> *holes; // null if the face has no openings to look through
	int rows;                          // height of the wall this art shows, in face rows (0: the level's wall height)
};

/** @brief The face art for a wall hit: the piece's own art for that face, else the other face, else the level's plain wall. */
FaceArt FaceForHit(const RayHit &hit)
{
	if (!InDungeonBounds(hit.tile)) {
		// The edge of the map is a plain black wall.
		static const std::vector<int16_t> Black(static_cast<size_t>(FaceTextureWidth) * ColumnPixelHeight, 0);
		return { &Black, nullptr, 0 };
	}
	const PieceTextures &textures = GetPieceTextures(dPiece[hit.tile.x][hit.tile.y]);
	const bool wantX = hit.xSide;
	if (wantX ? textures.faceXUsable : textures.faceYUsable)
		return wantX ? FaceArt { &textures.faceX, &textures.holesX, textures.faceXRows } : FaceArt { &textures.faceY, &textures.holesY, textures.faceYRows };
	if (wantX ? textures.faceYUsable : textures.faceXUsable)
		return wantX ? FaceArt { &textures.faceY, &textures.holesY, textures.faceYRows } : FaceArt { &textures.faceX, &textures.holesX, textures.faceXRows };
	return { &GetFallbackFace(), nullptr, 0 };
}

/**
 * @brief Draws the walls. Openings in a wall (arches) are looked through to the walls behind them.
 * @param depthBuffer per-pixel camera depth (width * height); wall pixels write their depth, the rest stays infinite
 */
void DrawWalls(const Surface &out, const Projection &proj, std::vector<float> &zBuffer, std::vector<float> &depthBuffer)
{
	const float wallHeight = WallHeight();
	const int wallRows = LevelWallRows();
	const int faceHeight = FaceTextureHeight();
	const std::vector<int16_t> &fallback = GetFallbackFace();
	static std::vector<uint8_t> filled;
	filled.resize(static_cast<size_t>(proj.height));

	for (int x = 0; x < proj.width; x++) {
		const Vec2 ray = proj.columnRay(x);
		std::fill(filled.begin(), filled.end(), uint8_t { 0 });
		zBuffer[x] = MaxRayDistance;
		for (int layer = 0; layer < MaxWallLayers; layer++) {
			const RayHit hit = CastRay(proj.pos, ray, layer);
			if (!hit.hit)
				break;
			if (layer == 0)
				zBuffer[x] = hit.distance;

			const FaceArt art = FaceForHit(hit);
			// Each wall is as tall as its art: low walls (balustrades, tomb boxes) stay low, and the walls behind them show above.
			const int hitRows = art.rows > 0 ? std::clamp(art.rows, 1, faceHeight) : wallRows;
			const float hitHeight = static_cast<float>(hitRows) / SpritePixelsPerTileY;
			const bool lowWall = hitHeight < wallHeight - 0.2F;
			const float scale = proj.focal / std::max(hit.distance, 0.01F);
			const float topF = static_cast<float>(proj.horizon) - (hitHeight - EyeHeight) * scale;
			const float bottomF = static_cast<float>(proj.horizon) + EyeHeight * scale;
			const int top = std::max(0, static_cast<int>(topF));
			const int bottom = std::min(proj.height - 1, static_cast<int>(bottomF));

			// Position along the face in world-axis order; the hidden back faces show the art mirrored.
			float along = hit.xSide ? hit.point.y - static_cast<float>(hit.tile.y) : hit.point.x - static_cast<float>(hit.tile.x);
			if (hit.backFace)
				along = 1.0F - along;
			const int u = std::clamp(static_cast<int>(along * FaceTextureWidth), 0, FaceTextureWidth - 1);

			const uint8_t *light = LightTables[LightAt(hit.previous)].data();
			// The wall covers the bottom `hitRows` rows of the face texture.
			const float rowsPerPixel = static_cast<float>(hitRows) / (bottomF - topF);
			bool sawOpening = lowWall;
			for (int y = top; y <= bottom; y++) {
				if (filled[y] != 0)
					continue;
				const int row = std::clamp(faceHeight - hitRows + static_cast<int>((static_cast<float>(y) - topF) * rowsPerPixel), 0, faceHeight - 1);
				const size_t at = static_cast<size_t>(row) * FaceTextureWidth + u;
				int16_t texel = (*art.face)[at];
				if (texel == NoPixel) {
					if (art.holes != nullptr && (*art.holes)[at] != 0) {
						sawOpening = true; // look through to the next wall
						continue;
					}
					texel = fallback[at];
				}
				*out.at(x, y) = light[texel];
				filled[y] = 1;
				depthBuffer[static_cast<size_t>(y) * proj.width + x] = hit.distance;
			}
			if (!sawOpening)
				break;
		}
	}
}

void DrawSpriteBillboard(const Surface &out, const Projection &proj, const std::vector<float> &zBuffer, const std::vector<float> *depthBuffer, const Entity &entity, const BillboardRect &rect)
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
		if (depthBuffer == nullptr && zBuffer[x] < rect.depth)
			continue;
		const int u = (x - rect.left) * spriteWidth / screenWidth;
		for (int y = y0; y < y1; y++) {
			const int v = (y - rect.top) * spriteHeight / screenHeight;
			const int16_t pixel = pixels[v * spriteWidth + u];
			// Colour 0 is the baked-in ground shadow, which would stand upright on a billboard.
			if (pixel <= 0)
				continue;
			if (depthBuffer != nullptr && rect.depth > (*depthBuffer)[static_cast<size_t>(y) * proj.width + x])
				continue; // behind a wall or building
			uint8_t color = static_cast<uint8_t>(pixel);
			if (entity.trn != nullptr)
				color = entity.trn[color];
			else if (light != nullptr)
				color = light[color];
			*out.at(x, y) = color;
		}
	}
}

/**
 * @brief Draws an iso sprite lying on the floor (a corpse) with the exact ground mapping of the original view.
 *
 * The sprite is anchored at the bottom vertex of its tile's diamond. A floor point q (in tiles, relative to that vertex)
 * appeared on screen at ((qx - qy) * 32, (qx + qy) * 16), so each floor pixel of the first-person view looks its colour up
 * through that mapping. Perspective, foreshortening and walls then come out right from any viewpoint.
 */
void DrawFlatSprite(const Surface &out, const Projection &proj, const std::vector<float> &depthBuffer, const Entity &entity)
{
	static std::vector<int16_t> pixels;
	const ClxSprite sprite = *entity.sprite;
	DecodeClx(sprite, pixels);
	const int width = sprite.width();
	const int height = sprite.height();
	const Vec2 anchor { static_cast<float>(entity.tile.x) + 1.0F, static_cast<float>(entity.tile.y) + 1.0F };

	// Screen rectangle that can contain the footprint.
	float minX = 1e30F;
	float maxX = -1e30F;
	float minY = 1e30F;
	float maxY = -1e30F;
	for (const int cornerX : { 0, width }) {
		for (const int cornerY : { 0, height }) {
			const float sx = static_cast<float>(cornerX) - static_cast<float>(width) / 2.0F;
			const float sy = static_cast<float>(cornerY - height);
			const Vec2 world { anchor.x + (sx / 32.0F + sy / 16.0F) / 2.0F, anchor.y + (sy / 16.0F - sx / 32.0F) / 2.0F };
			const float rx = world.x - proj.pos.x;
			const float ry = world.y - proj.pos.y;
			const float depth = std::max(rx * proj.dir.x + ry * proj.dir.y, 0.1F);
			const float lateral = (rx * proj.plane.x + ry * proj.plane.y) / (PlaneLength * PlaneLength);
			minX = std::min(minX, (lateral / depth + 1.0F) * static_cast<float>(proj.width) / 2.0F);
			maxX = std::max(maxX, (lateral / depth + 1.0F) * static_cast<float>(proj.width) / 2.0F);
			const float sy2 = static_cast<float>(proj.horizon) + EyeHeight / depth * proj.focal;
			minY = std::min(minY, sy2);
			maxY = std::max(maxY, sy2);
		}
	}
	const int x0 = std::max(0, static_cast<int>(std::floor(minX)));
	const int x1 = std::min(proj.width - 1, static_cast<int>(std::ceil(maxX)));
	const int y0 = std::max(proj.horizon + 1, static_cast<int>(std::floor(minY)));
	const int y1 = std::min(proj.height - 1, static_cast<int>(std::ceil(maxY)));
	const uint8_t *light = entity.lit ? LightTables[LightAt(entity.tile)].data() : nullptr;
	for (int y = y0; y <= y1; y++) {
		const float depth = EyeHeight / -proj.rowSlope(y); // floor distance for this row
		for (int x = x0; x <= x1; x++) {
			if (depth >= depthBuffer[static_cast<size_t>(y) * proj.width + x])
				continue; // behind a wall
			const Vec2 ray = proj.columnRay(x);
			const float qx = proj.pos.x + ray.x * depth - anchor.x;
			const float qy = proj.pos.y + ray.y * depth - anchor.y;
			const int px = static_cast<int>(std::floor((qx - qy) * 32.0F + static_cast<float>(width) / 2.0F));
			const int py = static_cast<int>(std::floor((qx + qy) * 16.0F + static_cast<float>(height)));
			if (px < 0 || py < 0 || px >= width || py >= height)
				continue;
			const int16_t pixel = pixels[static_cast<size_t>(py) * width + px];
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

void DrawEntities(const Surface &out, const Projection &proj, const std::vector<float> &zBuffer, const std::vector<float> *depthBuffer)
{
	static std::vector<Entity> entities;
	static std::vector<std::pair<BillboardRect, const Entity *>> sorted;
	CollectEntities(proj.pos, entities);

	sorted.clear();
	// Things lying on the floor go first; everything standing is drawn over them.
	if (depthBuffer != nullptr) {
		for (const Entity &entity : entities) {
			if (entity.flat && entity.sprite)
				DrawFlatSprite(out, proj, *depthBuffer, entity);
		}
	}
	for (const Entity &entity : entities) {
		if (entity.flat && depthBuffer != nullptr)
			continue;
		const BillboardRect rect = ProjectBillboard(proj, entity);
		if (rect.visible && (entity.sprite || entity.pixels != nullptr))
			sorted.emplace_back(rect, &entity);
	}
	// Far to near, so nearer sprites overwrite farther ones.
	std::sort(sorted.begin(), sorted.end(), [](const auto &a, const auto &b) { return a.first.depth > b.first.depth; });

	for (const auto &[rect, entity] : sorted)
		DrawSpriteBillboard(out, proj, zBuffer, depthBuffer, *entity, rect);
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
	static std::vector<float> depthBuffer;

	const Projection proj = GetProjection();
	zBuffer.resize(static_cast<size_t>(proj.width));

	DrawFloorAndCeiling(out, proj);
	depthBuffer.assign(static_cast<size_t>(proj.width) * proj.height, std::numeric_limits<float>::infinity());
	DrawWalls(out, proj, zBuffer, depthBuffer);
	if (leveltype == DTYPE_TOWN && BuildingsEnabled())
		DrawBuildings(out, proj, zBuffer, depthBuffer);
	DrawEntities(out, proj, zBuffer, &depthBuffer);
	if (IsMouseLookActive())
		DrawCrosshair(out);
	DrawDebugInfo(out, proj);
}

} // namespace devilution::fp
