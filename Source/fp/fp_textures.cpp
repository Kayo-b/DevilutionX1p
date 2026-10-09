/**
 * @file fp/fp_textures.cpp
 *
 * Wall and floor textures for the first-person view, extracted from the level's own iso tile art.
 */
#include "fp/fp_textures.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
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
#include "levels/dun_tile.hpp"
#include "levels/dun_tile_data.hpp"
#include "levels/gendung.h"
#include "levels/tile_properties.hpp"

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
	std::vector<int16_t> groundFloor;
	int wallRows = 0;
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
		cache.wallRows = 0;
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

/**
 * @brief Un-shears one of the two wall faces in a rendered column.
 *
 * The left column (x 0..31) holds the face along the edge from the diamond's left vertex up to its top
 * vertex; the right column (x 32..63) the edge from the top vertex down to the right vertex. Face texture
 * column u runs along the edge in world-axis order, row 0 is the top of the wall.
 */
std::vector<int16_t> ExtractFace(const std::vector<int16_t> &column, int height, bool leftEdge, bool &usable, int &rows, std::vector<uint8_t> &holes)
{
	const int faceHeight = FaceTextureHeight();
	const int bottom = height - 1;
	std::vector<int16_t> face(static_cast<size_t>(FaceTextureWidth) * faceHeight, NoPixel);
	int opaque = 0;
	for (int u = 0; u < FaceTextureWidth; u++) {
		// Left edge: world y runs from 1 (left vertex) to 0 (top vertex) as screen x increases, so u = 0
		// (world y = 0) is the rightmost screen column. Right edge: world x runs 0..1 with screen x.
		const int screenX = leftEdge ? FaceTextureWidth - 1 - u : FaceTextureWidth + u;
		const int base = leftEdge ? bottom - TILE_HEIGHT / 2 + 1 - screenX / 2 : bottom - TILE_HEIGHT + 1 + (screenX - FaceTextureWidth) / 2;
		for (int v = 0; v < faceHeight; v++) {
			const int screenY = base - v;
			if (screenY < 0)
				break;
			const int16_t pixel = column[static_cast<size_t>(screenY) * ColumnWidth + screenX];
			face[static_cast<size_t>(faceHeight - 1 - v) * FaceTextureWidth + u] = pixel;
			if (pixel != NoPixel)
				opaque++;
		}
	}
	usable = static_cast<float>(opaque) >= UsableOpaqueShare * static_cast<float>(face.size());
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
	textures->faceX = ExtractFace(column, height, true, textures->faceXUsable, textures->faceXRows, textures->holesX);
	textures->faceY = ExtractFace(column, height, false, textures->faceYUsable, textures->faceYRows, textures->holesY);
	textures->hasTallArt = false;
	textures->isStructure = false;
	textures->isPillar = false;
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
	// Only floor pieces have floor triangles; other pieces' bottom micros must not be decoded as triangles.
	// In town, solid pieces without tall art (water, the void outside the map) are flat ground as well.
	if (leveltype == DTYPE_TOWN && textures->hasTallArt) {
		// A town building or tree stands on its own ground: take it from the bottom of the drawn column.
		const std::vector<int16_t> ground(column.end() - static_cast<ptrdiff_t>(FloorHeight) * ColumnWidth, column.end());
		textures->floor = ExtractFloor(ground);
	} else if (!HasAnyOf(SOLData[pieceId], TileProperties::Solid | TileProperties::BlockMissile) || leveltype == DTYPE_TOWN)
		textures->floor = ExtractFloor(RenderColumnWithAlpha(pieceId, FloorHeight, true));
	else
		textures->floor.assign(static_cast<size_t>(FloorTextureSize) * FloorTextureSize, NoPixel);
	return textures;
}

} // namespace

int FaceTextureHeight()
{
	return ColumnHeight() - TILE_HEIGHT;
}

const PieceTextures &GetPieceTextures(uint16_t pieceId)
{
	Cache &cache = GetCache();
	std::unique_ptr<PieceTextures> &textures = cache.pieces[pieceId];
	if (textures == nullptr)
		textures = BuildPieceTextures(pieceId);
	return *textures;
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
			if (IsTileSolid({ x, y }))
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

} // namespace devilution::fp
