/**
 * @file fp/fp_textures.cpp
 *
 * Wall and floor textures for the first-person view, extracted from the level's own iso tile art.
 */
#include "fp/fp_textures.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <memory>
#include <numeric>
#include <span>
#include <string>
#include <unordered_map>

#include <cstdio>

#include "engine/light_tables.hpp"
#include "engine/palette.h"
#include "engine/render/dun_render.hpp"
#include "engine/render/light_render.hpp"
#include "engine/surface.hpp"
#include "fp/fp_raycast.hpp"
#include "levels/dun_tile.hpp"
#include "levels/dun_tile_data.hpp"
#include "levels/gendung.h"
#include "levels/tile_properties.hpp"
#include "utils/is_of.hpp"
#include "utils/log.hpp"

namespace devilution::fp {

namespace {

constexpr int FloorHeight = 32;
/** A face counts as usable art if at least this share of its pixels is opaque. */
constexpr float UsableOpaqueShare = 0.6F;
/** Share of its bounding box that tall art must fill to count as a building rather than foliage. */
constexpr float StructureFillShare = 0.5F;
/** Art must rise at least this many pixels above the floor diamond (about 2 tiles) to count as part of a building. */
constexpr int StructureMinRows = 70;
/** Upright art up to this wide (of 64 px) and at least this tall (px above the floor diamond) is a pillar. */
constexpr int PillarMaxWidth = 34;
constexpr int PillarMinRows = 60;
/** A free-standing column: thin shaft (px wide, measured this many rows above the floor diamond) under a bigger capital. */
constexpr int FreeStandingMaxShaft = 30;
constexpr int FreeStandingShaftRow = 20;
constexpr int FreeStandingMinOpaque = 300;
/** The lit top of a wall (its cap, seen from above in iso) is between this many face rows tall, else there is none. */
constexpr int MinCapRows = 8;
constexpr int MaxCapRows = 26;

/** Light tables that leave colours untouched, so extracted textures hold raw palette indices. */
const std::array<std::array<uint8_t, LightTableSize>, NumLightingLevels> &IdentityLightTables()
{
	static const auto Tables = [] {
		std::array<std::array<uint8_t, LightTableSize>, NumLightingLevels> tables;
		for (auto &table : tables)
			std::iota(table.begin(), table.end(), uint8_t { 0 });
		return tables;
	}();
	return Tables;
}

/** @brief Identifies the loaded level art; textures are rebuilt whenever it changes. */
struct LevelKey {
	const std::byte *cels;
	int level;
	int type;
	bool isSetLevel;
	int setLevelNum;

	bool operator==(const LevelKey &) const = default;
};

LevelKey CurrentLevelKey()
{
	return { pDungeonCels.get(), currlevel, static_cast<int>(leveltype), setlevel, static_cast<int>(setlvlnum) };
}

struct Cache {
	LevelKey key {};
	std::vector<std::unique_ptr<PieceTextures>> pieces = std::vector<std::unique_ptr<PieceTextures>>(MAXTILES);
	std::vector<int16_t> fallbackFace;
	std::vector<int16_t> fallbackPair; // the second tile of a run of two (see GetFallbackFace(int))
	uint16_t pairA = 0;
	uint16_t pairB = 0;
	bool pairUseX = false;
	bool fallbackPairReady = false;
	std::vector<int16_t> groundFloor;
	int wallRows = 0;
	int capRows = -1; // -1: not measured yet
};

Cache &GetCache()
{
	static Cache cache;
	const LevelKey key = CurrentLevelKey();
	if (!(cache.key == key)) {
		cache.key = key;
		for (auto &piece : cache.pieces)
			piece.reset();
		cache.fallbackFace.clear();
		cache.fallbackPair.clear();
		cache.fallbackPairReady = false;
		cache.wallRows = 0;
		cache.capRows = -1;
		cache.groundFloor.clear();
	}
	return cache;
}

int ColumnHeight()
{
	return MicroTileLen / 2 * TILE_HEIGHT; // MicroTileLen / 2 rows of 32px micros
}

/**
 * @brief Renders a piece column with the given background colour into an 8-bit buffer.
 * @param floorOnly render only the bottom two micros as floor triangles (as DrawFloorTile does)
 */
std::vector<uint8_t> RenderColumn(uint16_t pieceId, int height, uint8_t background, bool floorOnly)
{
	// The tile blitter may run spans past the end of a row (harmless on the wide screen buffer), so render
	// into a padded surface and copy out the column.
	constexpr int Pad = TILE_WIDTH;
	OwnedSurface surface(ColumnWidth + 2 * Pad, height + 2 * Pad);
	const Surface &out = surface;
	for (int y = 0; y < out.h(); y++)
		std::fill_n(out.at(0, y), out.w(), background);

	const auto &tables = IdentityLightTables();
	std::vector<uint8_t> lightmapBuffer(static_cast<size_t>(out.pitch()) * (out.h() + 1), 0);
	const Lightmap lightmap(out.at(0, 0), lightmapBuffer, out.pitch(), tables, tables[0].data(), nullptr);
	const uint8_t *tbl = tables[0].data();

	const MICROS &micros = DPieceMicros[pieceId];
	Point position { Pad, Pad + height - 1 };
	if (floorOnly) {
		for (int side = 0; side < 2; side++) {
			const LevelCelBlock block { micros.mt[side] };
			if (!block.hasValue())
				continue;
			RenderTileFrame(out, lightmap, position + Displacement { side * TILE_WIDTH / 2, 0 }, side == 0 ? TileType::LeftTriangle : TileType::RightTriangle,
			    GetDunFrame(pDungeonCels.get(), block.frame()), DunFrameTriangleHeight, MaskType::Solid, tbl);
		}
	} else {
		// Like DrawCell: on floor pieces the bottom two micros are the floor, which is only valid when drawn
		// as forced triangles (DrawFloorTile); their stored type does not describe the data.
		const bool isFloorPiece = !HasAnyOf(SOLData[pieceId], TileProperties::Solid | TileProperties::BlockMissile);
		for (uint_fast8_t i = 0; i < MicroTileLen; i += 2) {
			for (int side = 0; side < 2; side++) {
				if (i == 0 && isFloorPiece)
					continue;
				const LevelCelBlock block { micros.mt[i + side] };
				if (block.hasValue())
					RenderTile(out, lightmap, position + Displacement { side * TILE_WIDTH / 2, 0 }, pDungeonCels.get(), block, MaskType::Solid, tbl);
			}
			position.y -= TILE_HEIGHT;
		}
	}

	std::vector<uint8_t> pixels(static_cast<size_t>(ColumnWidth) * height);
	for (int y = 0; y < height; y++)
		std::copy_n(out.at(Pad, Pad + y), ColumnWidth, &pixels[static_cast<size_t>(y) * ColumnWidth]);
	return pixels;
}

/** @brief Renders a column twice on different backgrounds; pixels that differ were not drawn. */
std::vector<int16_t> RenderColumnWithAlpha(uint16_t pieceId, int height, bool floorOnly)
{
	const std::vector<uint8_t> onBlack = RenderColumn(pieceId, height, 0, floorOnly);
	const std::vector<uint8_t> onWhite = RenderColumn(pieceId, height, 255, floorOnly);
	std::vector<int16_t> pixels(onBlack.size());
	for (size_t i = 0; i < pixels.size(); i++)
		pixels[i] = onBlack[i] == onWhite[i] ? onBlack[i] : NoPixel;
	return pixels;
}

/** @brief Share of the level's ground floor texels in each 16-colour palette ramp (cached per level). */
const std::array<float, 16> &FloorRampShares()
{
	static std::array<float, 16> shares {};
	static LevelKey builtFor { nullptr, -1, -1, false, -1 };
	if (!(builtFor == CurrentLevelKey())) {
		builtFor = CurrentLevelKey();
		shares.fill(0.0F);
		int count = 0;
		for (const int16_t t : GetGroundFloor()) {
			if (t > 0) {
				shares[t >> 4] += 1.0F;
				count++;
			}
		}
		for (float &share : shares)
			share /= static_cast<float>(std::max(count, 1));
	}
	return shares;
}

/**
 * @brief Un-shears one of the two wall faces in a rendered column.
 *
 * The left column (x 0..31) holds the face along the edge from the diamond's left vertex up to its top
 * vertex; the right column (x 32..63) the edge from the top vertex down to the right vertex. Face texture
 * column u runs along the edge in world-axis order, row 0 is the top of the wall.
 */
std::vector<int16_t> ExtractFace(const std::vector<int16_t> &column, int height, bool leftEdge, bool &usable, int &rows, std::vector<uint8_t> &holes,
    std::vector<uint8_t> &cut, std::vector<int16_t> &spill)
{
	const int faceHeight = FaceTextureHeight();
	const int bottom = height - 1;
	std::vector<int16_t> face(static_cast<size_t>(FaceTextureWidth) * faceHeight, NoPixel);
	int opaque = 0;
	std::array<int, 16> wallRamps {};
	int wallCount = 0;
	// Row of face texel v (0 at the foot of the wall, negative below it).
	auto rowOf = [&](int v) { return faceHeight - 1 - FaceBaseRows - v; };
	for (int u = 0; u < FaceTextureWidth; u++) {
		// Left edge: world y runs from 1 (left vertex) to 0 (top vertex) as screen x increases, so u = 0
		// (world y = 0) is the rightmost screen column. Right edge: world x runs 0..1 with screen x.
		const int screenX = leftEdge ? FaceTextureWidth - 1 - u : FaceTextureWidth + u;
		const int base = leftEdge ? bottom - TILE_HEIGHT / 2 + 1 - screenX / 2 : bottom - TILE_HEIGHT + 1 + (screenX - FaceTextureWidth) / 2;
		for (int v = 0; v < faceHeight - FaceBaseRows; v++) {
			const int screenY = base - v;
			if (screenY < 0)
				break;
			const int16_t pixel = column[static_cast<size_t>(screenY) * ColumnWidth + screenX];
			face[static_cast<size_t>(rowOf(v)) * FaceTextureWidth + u] = pixel;
			if (pixel != NoPixel) {
				opaque++;
				wallRamps[pixel >> 4]++;
				wallCount++;
			}
		}
	}
	// Below the foot of the wall the art shows the floor in front of it, and on it the plinths and column bases. Keep
	// what is not floor (its colour ramp is common in the level's ground and rare in this wall); elsewhere the wall
	// carries on down, mirrored. Openings that reach the floor stay open.
	const std::array<float, 16> &floorRamps = FloorRampShares();
	auto isFloor = [&](int16_t pixel) {
		return pixel == NoPixel || wallCount == 0
		    || floorRamps[pixel >> 4] > 2.0F * static_cast<float>(wallRamps[pixel >> 4]) / static_cast<float>(wallCount);
	};
	// The foot of the face's far end (u from about half way) lies outside the piece's diamond, so the next tile along
	// the wall draws it: `cut` marks those base texels. This piece in turn draws that part of the previous tile's
	// face, half a tile up (`spill`, FaceBaseRows x FaceTextureWidth, NoPixel where it is floor).
	cut.assign(static_cast<size_t>(FaceTextureWidth) * FaceBaseRows, 0);
	spill.assign(static_cast<size_t>(FaceTextureWidth) * FaceBaseRows, NoPixel);
	for (int u = 0; u < FaceTextureWidth; u++) {
		const int screenX = leftEdge ? FaceTextureWidth - 1 - u : FaceTextureWidth + u;
		const int base = leftEdge ? bottom - TILE_HEIGHT / 2 + 1 - screenX / 2 : bottom - TILE_HEIGHT + 1 + (screenX - FaceTextureWidth) / 2;
		// The bottom edge of the diamond under this face column.
		const int diamondBottom = leftEdge ? bottom - TILE_HEIGHT / 2 + screenX / 2 : bottom - (screenX - FaceTextureWidth) / 2;
		// The same face column of the previous tile, as this piece draws it.
		const int spillX = leftEdge ? screenX + FaceTextureWidth : screenX - FaceTextureWidth;
		for (int k = 1; k <= FaceBaseRows; k++) {
			const size_t at = static_cast<size_t>(k - 1) * FaceTextureWidth + u;
			cut[at] = base + k > diamondBottom ? 1 : 0;
			const int spillY = base + k - TILE_HEIGHT / 2;
			if (spillY >= 0 && spillY < height) {
				const int16_t pixel = column[static_cast<size_t>(spillY) * ColumnWidth + spillX];
				spill[at] = isFloor(pixel) ? NoPixel : pixel;
			}
		}
	}
	for (int u = 0; u < FaceTextureWidth; u++) {
		const int screenX = leftEdge ? FaceTextureWidth - 1 - u : FaceTextureWidth + u;
		const int base = leftEdge ? bottom - TILE_HEIGHT / 2 + 1 - screenX / 2 : bottom - TILE_HEIGHT + 1 + (screenX - FaceTextureWidth) / 2;
		const bool openAtFoot = face[static_cast<size_t>(rowOf(0)) * FaceTextureWidth + u] == NoPixel;
		for (int k = 1; k <= FaceBaseRows; k++) {
			int16_t &texel = face[static_cast<size_t>(rowOf(-k)) * FaceTextureWidth + u];
			if (openAtFoot)
				continue;
			const int screenY = base + k;
			const int16_t pixel = screenY < height ? column[static_cast<size_t>(screenY) * ColumnWidth + screenX] : NoPixel;
			texel = isFloor(pixel) ? face[static_cast<size_t>(rowOf(k - 1)) * FaceTextureWidth + u] : pixel;
		}
	}
	// Measured over the wall's own rows: the base rows below its foot hold little art.
	usable = static_cast<float>(opaque) >= UsableOpaqueShare * static_cast<float>(FaceTextureWidth * (faceHeight - FaceBaseRows));
	// A texel without art is an opening (arch, window) if there is art above it in the same column.
	holes.assign(face.size(), 0);
	for (int u = 0; u < FaceTextureWidth; u++) {
		bool artAbove = false;
		for (int row = 0; row < faceHeight; row++) {
			const size_t at = static_cast<size_t>(row) * FaceTextureWidth + u;
			if (face[at] != NoPixel)
				artAbove = true;
			else if (artAbove)
				holes[at] = 1;
		}
	}
	rows = 0;
	for (int row = 0; row < faceHeight && rows == 0; row++)
		for (int u = 0; u < FaceTextureWidth; u++)
			if (face[static_cast<size_t>(row) * FaceTextureWidth + u] != NoPixel) {
				rows = faceHeight - row;
				break;
			}
	return face;
}

bool RecessesOn = true;

int TexelBrightness(int16_t texel)
{
	const SDL_Color c = logical_palette[static_cast<uint8_t>(texel)];
	return c.r * 3 + c.g * 6 + c.b;
}

/** @brief Median brightness of the opaque texels in the bottom `rows` rows of a face `width` texels wide, 0 if too sparse. */
int MedianBrightness(const std::vector<int16_t> &face, int width, int rows)
{
	const int faceHeight = FaceTextureHeight();
	std::vector<int> values;
	for (int row = faceHeight - rows; row < faceHeight; row++)
		for (int u = 0; u < width; u++) {
			const int16_t t = face[static_cast<size_t>(row) * width + u];
			if (t > 0)
				values.push_back(TexelBrightness(t));
		}
	if (values.size() < static_cast<size_t>(width * rows / 2))
		return 0;
	std::nth_element(values.begin(), values.begin() + static_cast<ptrdiff_t>(values.size() / 2), values.end());
	return values[values.size() / 2];
}

/** Rows a recess or opening is searched below the top of the wall art, as a share of its height (the cap and cornice). */
constexpr float ArchSearchStart = 0.3F;
/** A column belongs to an arch only if it reaches at least this share of the wall's height above the floor. */
constexpr float ArchMinShare = 0.4F;
/** Texels darker than this share of the face's median brightness are in the arch's shadow. */
constexpr float ArchDarkShare = 0.8F;

} // namespace

bool FindArchOutline(const std::vector<int16_t> &face, int width, int rows, std::vector<int> &top)
{
	const int faceHeight = FaceTextureHeight();
	top.assign(static_cast<size_t>(width), faceHeight);
	if (rows < 40)
		return false;
	const int median = MedianBrightness(face, width, rows);
	if (median == 0)
		return false;
	const int dark = static_cast<int>(static_cast<float>(median) * ArchDarkShare);
	auto isDark = [&](int row, int u) {
		const int16_t t = face[static_cast<size_t>(row) * width + u];
		return t > 0 && TexelBrightness(t) <= dark;
	};
	// Per column, from the top down below the cornice: the first run of dark texels (at most one bright one in five)
	// is the shadow under the arch's ring. A run that starts too low (a shadow at the foot of a pilaster) is none.
	constexpr int Run = 5;
	const int start = faceHeight - rows + static_cast<int>(static_cast<float>(rows) * ArchSearchStart);
	const int lowest = faceHeight - static_cast<int>(static_cast<float>(rows) * ArchMinShare);
	for (int u = 0; u < width; u++) {
		for (int row = start; row <= lowest; row++) {
			if (!isDark(row, u))
				continue;
			int bright = 0;
			for (int k = 1; k < Run; k++)
				bright += isDark(row + k, u) ? 0 : 1;
			if (bright <= 1) {
				top[u] = row;
				break;
			}
		}
	}
	// Median of three across columns, then drop spans narrower than a quarter of a face.
	std::vector<int> smooth(top);
	for (int u = 1; u + 1 < width; u++) {
		std::array<int, 3> n { top[u - 1], top[u], top[u + 1] };
		std::sort(n.begin(), n.end());
		smooth[u] = n[1];
	}
	for (int u = 0; u < width;) {
		if (smooth[u] >= faceHeight) {
			u++;
			continue;
		}
		int end = u;
		while (end < width && smooth[end] < faceHeight)
			end++;
		if (end - u < FaceTextureWidth / 4)
			for (int k = u; k < end; k++)
				smooth[k] = faceHeight;
		u = end;
	}
	// An arch is highest in its middle and falls away to both sides: within each span, make the outline fall
	// steadily from its highest point outwards, and soften the steps.
	bool any = false;
	for (int u = 0; u < width;) {
		if (smooth[u] >= faceHeight) {
			u++;
			continue;
		}
		any = true;
		int end = u;
		while (end < width && smooth[end] < faceHeight)
			end++;
		int peak = u;
		for (int k = u; k < end; k++)
			if (smooth[k] < smooth[peak] || (smooth[k] == smooth[peak] && std::abs(k * 2 - (u + end)) < std::abs(peak * 2 - (u + end))))
				peak = k;
		for (int k = peak + 1; k < end; k++)
			smooth[k] = std::max(smooth[k], smooth[k - 1]);
		for (int k = peak - 1; k >= u; k--)
			smooth[k] = std::max(smooth[k], smooth[k + 1]);
		std::vector<int> soft(smooth.begin() + u, smooth.begin() + end);
		for (int k = u + 1; k + 1 < end; k++)
			soft[k - u] = (smooth[k - 1] + 2 * smooth[k] + smooth[k + 1] + 2) / 4;
		std::copy(soft.begin(), soft.end(), smooth.begin() + u);
		u = end;
	}
	top = std::move(smooth);
	return any;
}

namespace {

/**
 * @brief Finds the recess of a wall face (the inside of a blind arch or niche): see FindArchOutline. Very dark
 * recesses (deep niches with a figure or a pillar inside) go deeper.
 */
void FindRecess(const std::vector<int16_t> &face, int rows, std::vector<int> &recess, float &depth)
{
	constexpr float BlindArchDepth = 0.2F;
	constexpr float NicheDepth = 0.35F;
	recess.clear();
	depth = 0;
	const int faceHeight = FaceTextureHeight();
	if (!FindArchOutline(face, FaceTextureWidth, rows, recess))
		return;
	const int median = MedianBrightness(face, FaceTextureWidth, rows);
	int64_t sum = 0;
	int count = 0;
	for (int u = 0; u < FaceTextureWidth; u++)
		for (int row = recess[u]; row < faceHeight - 6; row++) {
			const int16_t t = face[static_cast<size_t>(row) * FaceTextureWidth + u];
			if (t > 0) {
				sum += TexelBrightness(t);
				count++;
			}
		}
	if (count == 0) {
		recess.clear();
		return;
	}
	depth = sum / count < median * 35 / 100 ? NicheDepth : BlindArchDepth;
}

/** @brief Un-rotates the floor diamond into a square: texel (u, v) is world (u, v) / FloorTextureSize. */
std::vector<int16_t> ExtractFloor(const std::vector<int16_t> &column)
{
	std::vector<int16_t> floor(static_cast<size_t>(FloorTextureSize) * FloorTextureSize);
	const int top = FloorHeight - TILE_HEIGHT; // row of the diamond's top vertex in the floor render
	for (int v = 0; v < FloorTextureSize; v++) {
		for (int u = 0; u < FloorTextureSize; u++) {
			const float fx = (static_cast<float>(u) + 0.5F) / FloorTextureSize;
			const float fy = (static_cast<float>(v) + 0.5F) / FloorTextureSize;
			const int sx = std::clamp(static_cast<int>(TILE_WIDTH / 2 + (fx - fy) * TILE_WIDTH / 2), 0, ColumnWidth - 1);
			const int sy = std::clamp(static_cast<int>(top + (fx + fy) * TILE_HEIGHT / 2), 0, FloorHeight - 1);
			floor[static_cast<size_t>(v) * FloorTextureSize + u] = column[static_cast<size_t>(sy) * ColumnWidth + sx];
		}
	}
	return floor;
}

std::unique_ptr<PieceTextures> BuildPieceTextures(uint16_t pieceId)
{
	auto textures = std::make_unique<PieceTextures>();
	const int height = ColumnHeight();
	const std::vector<int16_t> column = RenderColumnWithAlpha(pieceId, height, false);
	textures->faceX = ExtractFace(column, height, true, textures->faceXUsable, textures->faceXRows, textures->holesX, textures->cutX, textures->spillX);
	textures->faceY = ExtractFace(column, height, false, textures->faceYUsable, textures->faceYRows, textures->holesY, textures->cutY, textures->spillY);
	if (leveltype != DTYPE_TOWN) {
		if (textures->faceXUsable)
			FindRecess(textures->faceX, textures->faceXRows, textures->recessX, textures->recessDepthX);
		if (textures->faceYUsable)
			FindRecess(textures->faceY, textures->faceYRows, textures->recessY, textures->recessDepthY);
	}
	textures->hasTallArt = false;
	textures->isStructure = false;
	textures->isPillar = false;
	textures->isFreeStanding = false;
	int opaque = 0;
	int minRow = height;
	int minX = ColumnWidth;
	int maxX = -1;
	for (int y = 0; y < height - TILE_HEIGHT; y++) {
		for (int x = 0; x < ColumnWidth; x++) {
			if (column[static_cast<size_t>(y) * ColumnWidth + x] != NoPixel) {
				textures->hasTallArt = true;
				opaque++;
				minRow = std::min(minRow, y);
				minX = std::min(minX, x);
				maxX = std::max(maxX, x);
			}
		}
	}
	textures->isPillar = textures->hasTallArt && maxX - minX + 1 <= PillarMaxWidth && height - TILE_HEIGHT - minRow >= PillarMinRows;
	if (textures->hasTallArt) {
		// Trees are sparse; buildings fill their bounding box.
		const int box = (height - TILE_HEIGHT - minRow) * (maxX - minX + 1);
		const bool tallEnough = height - TILE_HEIGHT - minRow >= StructureMinRows;
		textures->isStructure = tallEnough && static_cast<float>(opaque) >= StructureFillShare * static_cast<float>(box);
	}
	textures->column = column;
	if (textures->hasTallArt && opaque >= FreeStandingMinOpaque && !textures->faceXUsable && !textures->faceYUsable
	    && HasAnyOf(SOLData[pieceId], TileProperties::Solid | TileProperties::BlockMissile)) {
		// The width of the shaft, a little above the base: thin for a column, wide for a wall piece.
		int shaft = 0;
		for (int row = height - TILE_HEIGHT - FreeStandingShaftRow - 3; row <= height - TILE_HEIGHT - FreeStandingShaftRow + 3; row++) {
			int first = ColumnWidth;
			int last = -1;
			for (int x = 0; x < ColumnWidth; x++) {
				if (column[static_cast<size_t>(row) * ColumnWidth + x] != NoPixel) {
					first = std::min(first, x);
					last = x;
				}
			}
			shaft = std::max(shaft, last >= first ? last - first + 1 : 0);
		}
		textures->isFreeStanding = shaft > 0 && shaft <= FreeStandingMaxShaft;
	}
	if (leveltype != DTYPE_TOWN && !textures->hasTallArt && HasAnyOf(SOLData[pieceId], TileProperties::Solid)) {
		// Lava and water: their ground is bright art; the inside of thick walls is black or empty.
		const std::vector<int16_t> ground(column.end() - static_cast<ptrdiff_t>(FloorHeight) * ColumnWidth, column.end());
		const std::vector<int16_t> floor = ExtractFloor(ground);
		int lit = 0;
		for (const int16_t t : floor) {
			if (t <= 0)
				continue;
			const SDL_Color c = logical_palette[static_cast<uint8_t>(t)];
			lit += c.r + c.g + c.b > 60 ? 1 : 0;
		}
		if (lit * 2 > static_cast<int>(floor.size())) {
			textures->isFlatSolid = true;
			textures->floor = floor;
		}
	}
	// Only floor pieces have floor triangles; other pieces' bottom micros must not be decoded as triangles.
	// In town, solid pieces without tall art (water, the void outside the map) are flat ground as well.
	if ((leveltype == DTYPE_TOWN && textures->hasTallArt) || textures->isFreeStanding) {
		// A town building or tree stands on its own ground: take it from the bottom of the drawn column.
		const std::vector<int16_t> ground(column.end() - static_cast<ptrdiff_t>(FloorHeight) * ColumnWidth, column.end());
		textures->floor = ExtractFloor(ground);
	} else if (!HasAnyOf(SOLData[pieceId], TileProperties::Solid | TileProperties::BlockMissile) || leveltype == DTYPE_TOWN)
		textures->floor = ExtractFloor(RenderColumnWithAlpha(pieceId, FloorHeight, true));
	else if (!textures->isFlatSolid)
		textures->floor.assign(static_cast<size_t>(FloorTextureSize) * FloorTextureSize, NoPixel);
	return textures;
}

} // namespace

int FaceTextureHeight()
{
	return ColumnHeight() - TILE_HEIGHT + FaceBaseRows;
}

const PieceTextures &GetPieceTextures(uint16_t pieceId)
{
	Cache &cache = GetCache();
	std::unique_ptr<PieceTextures> &textures = cache.pieces[pieceId];
	if (textures == nullptr)
		textures = BuildPieceTextures(pieceId);
	return *textures;
}

namespace {

/** @brief Perceptual distance between two palette colours (weighted YUV, as xBR measures it). */
int ComputeColorDistance(uint8_t a, uint8_t b)
{
	const SDL_Color p = logical_palette[a];
	const SDL_Color q = logical_palette[b];
	const int dr = p.r - q.r;
	const int dg = p.g - q.g;
	const int db = p.b - q.b;
	const int y = std::abs(dr * 299 + dg * 587 + db * 114) / 1000;
	const int u = std::abs(dr * -169 + dg * -331 + db * 500) / 1000;
	const int v = std::abs(dr * 500 + dg * -419 + db * -81) / 1000;
	return 48 * y + 7 * u + 6 * v;
}

/**
 * @brief A colour halfway between two: the middle of their ramp if they share one (the palette's ramps of 16 run
 * light to dark), else the colour of either's ramp nearest their mix, so the art keeps its hues.
 */
uint8_t ComputeMixColor(uint8_t a, uint8_t b)
{
	if (a / 16 == b / 16)
		return static_cast<uint8_t>((a + b + (a < b ? 0 : 1)) / 2);
	const SDL_Color p = logical_palette[a];
	const SDL_Color q = logical_palette[b];
	const int r = p.r + q.r;
	const int g = p.g + q.g;
	const int bl = p.b + q.b;
	int best = std::numeric_limits<int>::max();
	uint8_t result = a;
	for (const int ramp : { a / 16, b / 16 }) {
		for (int j = ramp * 16; j < ramp * 16 + 16; j++) {
			const SDL_Color o = logical_palette[j];
			const int d = (2 * o.r - r) * (2 * o.r - r) * 3 + (2 * o.g - g) * (2 * o.g - g) * 6 + (2 * o.b - bl) * (2 * o.b - bl);
			if (d < best) {
				best = d;
				result = static_cast<uint8_t>(j);
			}
		}
	}
	return result;
}

/** @brief ComputeColorDistance and ComputeMixColor for every pair of colours, for the palette the textures are built with. */
struct ColorPairs {
	std::vector<uint16_t> distance = std::vector<uint16_t>(256 * 256);
	std::vector<uint8_t> mix = std::vector<uint8_t>(256 * 256);
};

const ColorPairs &GetColorPairs()
{
	static ColorPairs pairs;
	static LevelKey builtFor { nullptr, -1, -1, false, -1 };
	if (!(builtFor == CurrentLevelKey())) {
		builtFor = CurrentLevelKey();
		for (int a = 0; a < 256; a++) {
			for (int b = 0; b < 256; b++) {
				pairs.distance[a * 256 + b] = static_cast<uint16_t>(std::min(ComputeColorDistance(static_cast<uint8_t>(a), static_cast<uint8_t>(b)), 65535));
				pairs.mix[a * 256 + b] = ComputeMixColor(static_cast<uint8_t>(a), static_cast<uint8_t>(b));
			}
		}
	}
	return pairs;
}

/**
 * @brief Doubles a texture with xBR (level 1): where an edge runs diagonally across a texel's corner, that corner takes
 * on half the colour across the edge, so outlines come out smooth at twice the resolution without blurring the
 * texels in between. NoPixel texels stay NoPixel and count as the texel itself for their neighbours.
 */
std::vector<int16_t> Xbr2x(const std::vector<int16_t> &src, int width, int height)
{
	const ColorPairs &pairs = GetColorPairs();
	auto ColorDistance = [&](uint8_t a, uint8_t b) { return static_cast<int>(pairs.distance[a * 256 + b]); };
	std::vector<int16_t> dst(static_cast<size_t>(width) * height * 4);
	const int outWidth = width * 2;
	for (int y = 0; y < height; y++) {
		for (int x = 0; x < width; x++) {
			const int16_t e = src[static_cast<size_t>(y) * width + x];
			std::array<int16_t, 4> out { e, e, e, e };
			if (e != NoPixel) {
				const auto ec = static_cast<uint8_t>(e);
				for (int corner = 0; corner < 4; corner++) {
					const int sx = (corner & 1) != 0 ? 1 : -1;
					const int sy = (corner & 2) != 0 ? 1 : -1;
					// The neighbourhood mirrored so that this corner is the bottom right one.
					auto at = [&](int a, int b) {
						const int px = std::clamp(x + sx * a, 0, width - 1);
						const int py = std::clamp(y + sy * b, 0, height - 1);
						const int16_t t = src[static_cast<size_t>(py) * width + px];
						return t == NoPixel ? ec : static_cast<uint8_t>(t);
					};
					const uint8_t f = at(1, 0);
					const uint8_t h = at(0, 1);
					if (f == ec || h == ec)
						continue;
					const uint8_t i = at(1, 1);
					const uint8_t c = at(1, -1);
					const uint8_t g = at(-1, 1);
					const uint8_t d = at(-1, 0);
					const uint8_t b = at(0, -1);
					const int across = ColorDistance(ec, c) + ColorDistance(ec, g) + ColorDistance(i, at(2, 0)) + ColorDistance(i, at(0, 2)) + 4 * ColorDistance(h, f);
					const int along = ColorDistance(h, d) + ColorDistance(h, at(1, 2)) + ColorDistance(f, at(2, 1)) + ColorDistance(f, b) + 4 * ColorDistance(ec, i);
					if (across >= along)
						continue; // no clear edge (the art's noise)
					const uint8_t other = ColorDistance(ec, f) <= ColorDistance(ec, h) ? f : h;
					out[static_cast<size_t>(corner)] = static_cast<int16_t>(pairs.mix[ec * 256 + other]);
				}
			}
			const size_t topLeft = static_cast<size_t>(y) * 2 * outWidth + static_cast<size_t>(x) * 2;
			dst[topLeft] = out[0];
			dst[topLeft + 1] = out[1];
			dst[topLeft + outWidth] = out[2];
			dst[topLeft + outWidth + 1] = out[3];
		}
	}
	return dst;
}

} // namespace

namespace {

struct UpscaleCache {
	LevelKey builtFor { nullptr, -1, -1, false, -1 };
	std::unordered_map<const int16_t *, std::vector<int16_t>> textures;
	// The last texture looked up, as callers ask for the same one many times in a row.
	const int16_t *lastKey = nullptr;
	const std::vector<int16_t> *last = nullptr;
};

UpscaleCache &GetUpscaleCache()
{
	static UpscaleCache cache;
	if (!(cache.builtFor == CurrentLevelKey())) {
		cache.builtFor = CurrentLevelKey();
		cache.textures.clear();
		cache.lastKey = nullptr;
	}
	return cache;
}

} // namespace

const std::vector<int16_t> &UpscaledTexture(const std::vector<int16_t> &texture, int width)
{
	UpscaleCache &cache = GetUpscaleCache();
	const size_t size = texture.size() * TextureUpscale * TextureUpscale;
	if (cache.lastKey == texture.data() && cache.last->size() == size)
		return *cache.last;
	std::vector<int16_t> &fine = cache.textures[texture.data()];
	if (fine.size() != size) {
		const int height = static_cast<int>(texture.size()) / width;
		fine = Xbr2x(Xbr2x(texture, width, height), width * 2, height * 2);
	}
	cache.lastKey = texture.data();
	cache.last = &fine;
	return fine;
}

void ForgetUpscaledTexture(const std::vector<int16_t> &texture)
{
	UpscaleCache &cache = GetUpscaleCache();
	cache.textures.erase(texture.data());
	cache.lastKey = nullptr;
}

const std::vector<int16_t> &GetGroundFloor()
{
	Cache &cache = GetCache();
	if (!cache.groundFloor.empty())
		return cache.groundFloor;
	std::unordered_map<uint16_t, int> counts;
	for (int y = 0; y < MAXDUNY; y++) {
		for (int x = 0; x < MAXDUNX; x++) {
			if (!IsTileSolid({ x, y }))
				counts[dPiece[x][y]]++;
		}
	}
	int best = 0;
	uint16_t bestPiece = 0;
	for (const auto &[piece, count] : counts) {
		if (count > best) {
			best = count;
			bestPiece = piece;
		}
	}
	cache.groundFloor = ExtractFloor(RenderColumnWithAlpha(bestPiece, FloorHeight, true));
	std::replace(cache.groundFloor.begin(), cache.groundFloor.end(), NoPixel, int16_t { 0 });
	return cache.groundFloor;
}

int LevelWallRows()
{
	Cache &cache = GetCache();
	if (cache.wallRows > 0)
		return cache.wallRows;
	// The art height of the usable wall faces, weighted by how often each piece occurs.
	std::unordered_map<int, int> votes;
	for (int y = 0; y < MAXDUNY; y++) {
		for (int x = 0; x < MAXDUNX; x++) {
			if (!IsTileSolid({ x, y }))
				continue;
			const PieceTextures &textures = GetPieceTextures(dPiece[x][y]);
			if (textures.faceXUsable && textures.faceXRows > 0)
				votes[textures.faceXRows]++;
			if (textures.faceYUsable && textures.faceYRows > 0)
				votes[textures.faceYRows]++;
		}
	}
	int best = FaceTextureHeight();
	int bestVotes = 0;
	for (const auto &[rows, count] : votes) {
		if (count > bestVotes) {
			bestVotes = count;
			best = rows;
		}
	}
	cache.wallRows = std::min(best, FaceTextureHeight());
	return cache.wallRows;
}

const std::vector<int16_t> &GetFallbackFace()
{
	Cache &cache = GetCache();
	if (!cache.fallbackFace.empty())
		return cache.fallbackFace;

	// The most common solid piece with a plain face (almost no openings) is the level's typical wall; arches
	// would show up as repeating arcs wherever the filler is used.
	std::unordered_map<uint16_t, int> counts;
	for (int y = 0; y < MAXDUNY; y++) {
		for (int x = 0; x < MAXDUNX; x++) {
			if (IsTileSolid({ x, y }))
				counts[dPiece[x][y]]++;
		}
	}
	std::vector<std::pair<int, uint16_t>> ranked;
	for (const auto &[piece, count] : counts)
		ranked.emplace_back(count, piece);
	std::sort(ranked.begin(), ranked.end(), std::greater<>());
	auto opaqueShare = [](const std::vector<int16_t> &face) {
		return static_cast<float>(std::count_if(face.begin(), face.end(), [](int16_t p) { return p != NoPixel; })) / static_cast<float>(face.size());
	};
	for (const float minShare : { 0.55F, 0.0F }) {
		for (const auto &[count, piece] : ranked) {
			const PieceTextures &textures = GetPieceTextures(piece);
			for (const bool useX : { true, false }) {
				if (!(useX ? textures.faceXUsable : textures.faceYUsable))
					continue;
				const std::vector<int16_t> &face = useX ? textures.faceX : textures.faceY;
				if (opaqueShare(face) < minShare)
					continue;
				cache.fallbackFace = face;
				break;
			}
			if (!cache.fallbackFace.empty())
				break;
		}
		if (!cache.fallbackFace.empty())
			break;
	}
	if (cache.fallbackFace.empty())
		cache.fallbackFace.assign(static_cast<size_t>(FaceTextureWidth) * FaceTextureHeight(), 0);
	// Holes in the fallback would show through to nothing; fill them with black.
	std::replace(cache.fallbackFace.begin(), cache.fallbackFace.end(), NoPixel, int16_t { 0 });
	return cache.fallbackFace;
}

#ifdef _DEBUG
void DumpSolidPieces(const char *path, int perRow, int rows)
{
	std::unordered_map<uint16_t, int> counts;
	for (int y = 0; y < MAXDUNY; y++)
		for (int x = 0; x < MAXDUNX; x++)
			if (IsTileSolid({ x, y }) || GetPieceTextures(dPiece[x][y]).hasTallArt)
				counts[dPiece[x][y]]++;
	std::vector<std::pair<int, uint16_t>> ranked;
	for (const auto &[piece, count] : counts)
		ranked.emplace_back(count, piece);
	std::sort(ranked.begin(), ranked.end(), std::greater<>());

	const int height = ColumnHeight();
	const int cellW = ColumnWidth + 2 * FaceTextureWidth + 8;
	const int width = cellW * perRow;
	const int totalHeight = (height + 4) * rows;
	std::vector<uint8_t> image(static_cast<size_t>(width) * totalHeight * 3, 40);
	auto put = [&](int x, int y, int color) {
		const SDL_Color c = logical_palette[color];
		uint8_t *px = &image[(static_cast<size_t>(y) * width + x) * 3];
		px[0] = c.r;
		px[1] = c.g;
		px[2] = c.b;
	};
	const std::string listPath = std::string(path) + ".txt";
	FILE *list = std::fopen(listPath.c_str(), "w");
	for (size_t i = 0; i < ranked.size() && i < static_cast<size_t>(perRow * rows); i++) {
		const uint16_t piece = ranked[i].second;
		const int ox = static_cast<int>(i % perRow) * cellW;
		const int oy = static_cast<int>(i / perRow) * (height + 4);
		const std::vector<int16_t> column = RenderColumnWithAlpha(piece, height, false);
		for (int y = 0; y < height; y++)
			for (int x = 0; x < ColumnWidth; x++)
				if (column[static_cast<size_t>(y) * ColumnWidth + x] != NoPixel)
					put(ox + x, oy + y, column[static_cast<size_t>(y) * ColumnWidth + x]);
		const PieceTextures &t = GetPieceTextures(piece);
		const std::vector<int16_t> *faces[2] = { &t.faceX, &t.faceY };
		for (int f = 0; f < 2; f++)
			for (int y = 0; y < FaceTextureHeight(); y++)
				for (int x = 0; x < FaceTextureWidth; x++) {
					const int16_t px = (*faces[f])[static_cast<size_t>(y) * FaceTextureWidth + x];
					if (px != NoPixel)
						put(ox + ColumnWidth + f * FaceTextureWidth + x, oy + y, px);
				}
		if (list != nullptr)
			std::fprintf(list, "%zu piece %d count %d usableX %d usableY %d\n", i, piece, ranked[i].first, t.faceXUsable, t.faceYUsable);
	}
	if (list != nullptr)
		std::fclose(list);
	if (FILE *f = std::fopen(path, "wb")) {
		std::fprintf(f, "P6\n%d %d\n255\n", width, totalHeight);
		std::fwrite(image.data(), 1, image.size(), f);
		std::fclose(f);
	}
}
#endif

const std::vector<int16_t> &GetFallbackFace(int parity)
{
	Cache &cache = GetCache();
	if (!cache.fallbackPairReady) {
		cache.fallbackPairReady = true;
		// The commonest pair of neighbouring wall tiles in a straight run: arches spanning tile edges come out whole
		// when filler walls alternate between the two.
		std::unordered_map<uint32_t, int> counts;
		for (int y = 0; y + 1 < MAXDUNY; y++) {
			for (int x = 0; x + 1 < MAXDUNX; x++) {
				if (!IsTileSolid({ x, y }))
					continue;
				const PieceTextures &a = GetPieceTextures(dPiece[x][y]);
				if (a.faceYUsable && IsTileSolid({ x + 1, y }) && GetPieceTextures(dPiece[x + 1][y]).faceYUsable && (x & 1) == 0)
					counts[(static_cast<uint32_t>(dPiece[x][y]) << 16 | dPiece[x + 1][y]) << 1 | 0]++;
				if (a.faceXUsable && IsTileSolid({ x, y + 1 }) && GetPieceTextures(dPiece[x][y + 1]).faceXUsable && (y & 1) == 0)
					counts[(static_cast<uint32_t>(dPiece[x][y]) << 16 | dPiece[x][y + 1]) << 1 | 1]++;
			}
		}
		uint32_t best = 0;
		int bestCount = 0;
		for (const auto &[key, count] : counts) {
			const PieceTextures &a = GetPieceTextures(static_cast<uint16_t>(key >> 17));
			const PieceTextures &b = GetPieceTextures(static_cast<uint16_t>((key >> 1) & 0xFFFF));
			const bool useX = (key & 1) != 0;
			const std::vector<int16_t> &fa = useX ? a.faceX : a.faceY;
			const std::vector<int16_t> &fb = useX ? b.faceX : b.faceY;
			// Mostly opaque over the wall's own rows above the floor (the face texture is taller than the wall; the base
			// rows below its foot are open wherever the art shows only floor there).
			const auto full = [](const std::vector<int16_t> &f) {
				const auto start = f.begin() + static_cast<std::ptrdiff_t>(f.size() - static_cast<size_t>(LevelWallRows()) * FaceTextureWidth);
				const auto end = f.end() - static_cast<std::ptrdiff_t>(FaceBaseRows) * FaceTextureWidth;
				return std::count(start, end, NoPixel) * 10 < end - start;
			};
			if (count > bestCount && full(fa) && full(fb)) {
				best = key;
				bestCount = count;
			}
		}
		LogInfo("fp: filler wall pair: pieces {} and {} ({} runs)", best >> 17, (best >> 1) & 0xFFFF, bestCount);
		if (bestCount >= 3) {
			const bool useX = (best & 1) != 0;
			const PieceTextures &a = GetPieceTextures(static_cast<uint16_t>(best >> 17));
			const PieceTextures &b = GetPieceTextures(static_cast<uint16_t>((best >> 1) & 0xFFFF));
			cache.fallbackFace = useX ? a.faceX : a.faceY;
			cache.fallbackPair = useX ? b.faceX : b.faceY;
			cache.pairA = static_cast<uint16_t>(best >> 17);
			cache.pairB = static_cast<uint16_t>((best >> 1) & 0xFFFF);
			cache.pairUseX = useX;
			std::replace(cache.fallbackFace.begin(), cache.fallbackFace.end(), NoPixel, int16_t { 0 });
			std::replace(cache.fallbackPair.begin(), cache.fallbackPair.end(), NoPixel, int16_t { 0 });
		}
	}
	if (cache.fallbackPair.empty())
		return GetFallbackFace();
	return (parity & 1) == 0 ? cache.fallbackFace : cache.fallbackPair;
}

bool GetPlainWallPair(uint16_t &first, uint16_t &second, bool &useX)
{
	GetFallbackFace(0);
	const Cache &cache = GetCache();
	if (cache.fallbackPair.empty())
		return false;
	first = cache.pairA;
	second = cache.pairB;
	useX = cache.pairUseX;
	return true;
}

std::vector<int16_t> RenderPieceFloor(uint16_t pieceId)
{
	return RenderColumnWithAlpha(pieceId, FloorHeight, true);
}

namespace {
uint32_t PaletteHash = 0;
uint32_t PaletteVersionNumber = 1;
uint32_t UncycledPaletteHash = 0;
uint32_t StablePaletteVersionNumber = 1;
} // namespace

uint32_t PaletteVersion()
{
	return PaletteVersionNumber;
}

uint32_t StablePaletteVersion()
{
	return StablePaletteVersionNumber;
}

bool IsCycledColor(int index)
{
	// palette_update_caves turns 1-31 (the Poisoned Water Supply's level also sets 0-31 as its water clears),
	// palette_update_crypt 1-15 and 16-31, palette_update_hive 1-8 and 9-15.
	return index >= 0 && index < 32 && IsAnyOf(leveltype, DTYPE_CAVES, DTYPE_CRYPT, DTYPE_NEST);
}

void NotePaletteChange()
{
	uint32_t hash = 2166136261U;
	uint32_t uncycled = 2166136261U;
	for (int i = 0; i < 256; i++) {
		const SDL_Color c = logical_palette[i];
		hash = (hash ^ c.r) * 16777619U;
		hash = (hash ^ c.g) * 16777619U;
		hash = (hash ^ c.b) * 16777619U;
		if (IsCycledColor(i))
			continue;
		uncycled = (uncycled ^ c.r) * 16777619U;
		uncycled = (uncycled ^ c.g) * 16777619U;
		uncycled = (uncycled ^ c.b) * 16777619U;
	}
	if (hash != PaletteHash) {
		PaletteHash = hash;
		PaletteVersionNumber++;
	}
	if (uncycled != UncycledPaletteHash) {
		UncycledPaletteHash = uncycled;
		StablePaletteVersionNumber++;
	}
}

int WallCapRows()
{
	Cache &cache = GetCache();
	if (cache.capRows >= 0)
		return cache.capRows;
	cache.capRows = 0;
	uint16_t first;
	uint16_t second;
	bool useX;
	if (!GetPlainWallPair(first, second, useX))
		return 0;
	// The top of a wall seen from above is lit: a bright band at the top of the face art, ending in the shadow line
	// under it. Measured on the plain wall pair, row by row.
	const int faceHeight = FaceTextureHeight();
	const int rows = LevelWallRows();
	const int top = faceHeight - rows;
	auto rowBrightness = [&](int row) {
		int sum = 0;
		int count = 0;
		for (const uint16_t piece : { first, second }) {
			const PieceTextures &textures = GetPieceTextures(piece);
			const std::vector<int16_t> &face = useX ? textures.faceX : textures.faceY;
			for (int u = 0; u < FaceTextureWidth; u++) {
				const int16_t t = face[static_cast<size_t>(row) * FaceTextureWidth + u];
				if (t > 0) {
					sum += TexelBrightness(t);
					count++;
				}
			}
		}
		return count > FaceTextureWidth ? sum / count : -1;
	};
	int capSum = 0;
	int capCount = 0;
	for (int row = top + 1; row <= top + 6; row++) {
		const int b = rowBrightness(row);
		if (b >= 0) {
			capSum += b;
			capCount++;
		}
	}
	if (capCount == 0)
		return 0;
	const int capBrightness = capSum / capCount;
	for (int row = top + MinCapRows; row <= top + MaxCapRows && row < faceHeight; row++) {
		const int b = rowBrightness(row);
		if (b >= 0 && b * 100 < capBrightness * 72) {
			cache.capRows = row - top;
			break;
		}
	}
	LogInfo("fp: wall cap {} of {} rows", cache.capRows, rows);
	return cache.capRows;
}

int WallShownRows(int rows)
{
	const int cap = WallCapRows();
	return cap > 0 && rows > 3 * cap ? rows - cap : rows;
}

float WallArtScale(int /*rows*/)
{
	// The face's bottom row is the foot of the wall on the floor: its art keeps its own proportions.
	return 1.0F;
}

int WallArtRow(float height, int rows)
{
	return FaceTextureHeight() - 1 - static_cast<int>(height * SpritePixelsPerTileY * WallArtScale(rows));
}

float WallArtHeight(int row, int rows)
{
	return static_cast<float>(FaceTextureHeight() - 1 - row) / (SpritePixelsPerTileY * WallArtScale(rows));
}

const std::vector<int> *GetFallbackRecess(int parity, float &depth)
{
	uint16_t first;
	uint16_t second;
	bool useX;
	depth = 0;
	if (!GetPlainWallPair(first, second, useX))
		return nullptr;
	const PieceTextures &textures = GetPieceTextures((parity & 1) == 0 ? first : second);
	const std::vector<int> &recess = useX ? textures.recessX : textures.recessY;
	if (recess.empty())
		return nullptr;
	depth = useX ? textures.recessDepthX : textures.recessDepthY;
	return &recess;
}

bool HasPlainWall()
{
	GetFallbackFace(0);
	return !GetCache().fallbackPair.empty();
}

bool WallRecessesEnabled()
{
	return RecessesOn;
}

void SetWallRecessesEnabled(bool enabled)
{
	RecessesOn = enabled;
}

} // namespace devilution::fp
