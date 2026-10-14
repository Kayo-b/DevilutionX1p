/**
 * @file fp/fp_hell.cpp
 *
 * Hell in first person (see fp_hell.hpp): its tiles, the textures cut out of its art, its floor and wall passes, and
 * the bone geometry handed to the mesh pass.
 */
#include "fp/fp_hell.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <utility>

#include "engine/light_tables.hpp"
#include "engine/palette.h"
#include "fp/fp_buildings.hpp"
#include "fp/fp_textures.hpp"
#include "levels/dun_tile_data.hpp"
#include "levels/gendung.h"
#include "levels/tile_properties.hpp"
#include "utils/endian_swap.hpp"
#include "utils/log.hpp"

namespace devilution::fp {

namespace {

// --- Proportions (drawn tiles) -----------------------------------------------------------------------------------

/**
 * The height of every wall (the art's are about 1.8 tiles of art, above the eye once stretched like the other levels':
 * so low, the blood on the troughs shows), and how big the tile-art billboards are drawn, as a share of their art.
 */
constexpr float WallTop = 0.92F;
constexpr float ArtScale = 0.6F;
/** Light levels the blood is brighter than its light: it glows a little in the dark. */
constexpr float BloodGlow = 4.0F;
/** Light levels added to the wall sides facing along y (the other axis' faces stay as lit), so corners read. */
constexpr float FaceYShade = 0.6F;
/** The beams round the tops of the walls: their radius, how far into the wall their axis lies, how far above the top, how far they run on past a wall's end. */
constexpr float BeamRadius = 0.085F;
constexpr float BeamInset = 0.07F;
constexpr float BeamLift = 0.02F;
constexpr float BeamOverhang = 0.12F;
/** Spine posts along the walls: about how far apart, how far out of the face, their radius (body, joints), how high they go above the beams. */
constexpr float PostSpacing = 3.0F;
constexpr float PostOut = 0.03F;
constexpr float PostBody = 0.1F;
constexpr float PostJoint = 0.068F;
constexpr float PostAbove = 0.1F;
/** Lone spine columns: their height to the skull, their radius (body, joints). */
constexpr float ColumnHeight = 1.3F;
constexpr float ColumnBody = 0.13F;
constexpr float ColumnJoint = 0.09F;

/** The tiles of hell's map (`dungeon`, ids from 1) by what they are, from the iso art of levels/l4data/l4.til. */
enum class Megatile : uint8_t {
	Other,  // floor, and flesh walls (a solid piece of any other tile)
	Shadow, // floor with the shadow of a wall's corner painted on (ApplyShadowsPatterns)
	Blood,  // a trough of blood rimmed with bone, or the lake round the rooms (30)
	Spike,  // bone spikes growing out of the ash
	Stairs, // the rib cages over the stairs up (31-38), down (39-46) and to town (129-136)
};

constexpr int LakeTile = 30;
constexpr int PlainFloorTile = 6;
constexpr int FleshWallTile = 1;

Megatile ClassOf(int id)
{
	if (id == 47 || id == 48)
		return Megatile::Shadow;
	if ((id >= 18 && id <= 30) || (id >= 116 && id <= 128) || id == 137)
		return Megatile::Blood;
	if ((id >= 31 && id <= 46) || (id >= 129 && id <= 136))
		return Megatile::Stairs;
	if (id >= 95 && id <= 97)
		return Megatile::Spike;
	return Megatile::Other;
}

/** The map tile (`dungeon`) a tile of the dPiece grid belongs to; the lake past the map's edge. */
int MegatileAt(Point tile)
{
	if (tile.x < 16 || tile.y < 16)
		return LakeTile;
	const int mx = (tile.x - 16) / 2;
	const int my = (tile.y - 16) / 2;
	if (mx >= DMAXX || my >= DMAXY)
		return LakeTile;
	return dungeon[mx][my];
}

/** The piece a map tile puts at a quadrant of its 2 x 2 (0 top, 1 right, 2 left, 3 bottom; see DRLG_LPass3). */
uint16_t MegatilePiece(int id, int quadrant)
{
	const MegaTile &mega = pMegaTiles[id - 1];
	const std::array<uint16_t, 4> micros { mega.micro1, mega.micro2, mega.micro3, mega.micro4 };
	return Swap16LE(micros[quadrant]);
}

enum : uint8_t {
	TileBlood = 1,    // a tile of a trough or the lake
	TileBloodTop = 2, // a wall with blood on top: blood tiles, and the flesh walls beside them (the troughs' fronts)
};

std::vector<uint8_t> Flags;           // per tile, y * MAXDUNX + x
std::vector<uint16_t> FloorPieces;    // per tile: the piece whose floor art it shows

// --- Art -----------------------------------------------------------------------------------------------------------

/** The blood: BloodSize square over BloodTiles x BloodTiles tiles, in the art's colours (hell cycles them); cut once per level graphics. */
constexpr int BloodTiles = 2;
constexpr int BloodSize = FloorTextureSize * BloodTiles;
struct BloodArt {
	const std::byte *cels = nullptr;
	std::vector<int16_t> texels;
};
BloodArt Blood;

/**
 * A map tile's art as the game draws it (MegatileWidth wide, `height` rows): its four pieces, each a column ColumnWidth
 * wide, the top one in the middle, half a column higher than the left and right ones, the bottom one lowest.
 */
constexpr int MegatileWidth = 2 * ColumnWidth;
constexpr std::array<std::pair<int, int>, 4> QuadrantOffsets { { { ColumnWidth / 2, 0 }, { ColumnWidth, ColumnWidth / 4 }, { 0, ColumnWidth / 4 }, { ColumnWidth / 2, ColumnWidth / 2 } } };

std::vector<int16_t> ComposeMegatile(int id, int &height)
{
	const int rows = static_cast<int>(GetPieceTextures(MegatilePiece(id, 0)).column.size() / ColumnWidth);
	height = rows + ColumnWidth / 2;
	std::vector<int16_t> canvas(static_cast<size_t>(MegatileWidth) * height, NoPixel);
	for (int quadrant = 0; quadrant < 4; quadrant++) {
		const std::vector<int16_t> &column = GetPieceTextures(MegatilePiece(id, quadrant)).column;
		const auto [ox, oy] = QuadrantOffsets[quadrant];
		for (int y = 0; y < rows && y + oy < height; y++)
			for (int x = 0; x < ColumnWidth; x++)
				if (const int16_t texel = column[static_cast<size_t>(y) * ColumnWidth + x]; texel != NoPixel)
					canvas[static_cast<size_t>(y + oy) * MegatileWidth + x + ox] = texel;
	}
	return canvas;
}

/**
 * The top of the lake's map tile, a block of blood two tiles square, as a square: the block's top diamond in its art
 * (ComposeMegatile), from the highest row drawn, unrotated. One stretch of the art's blood two tiles wide: repeated,
 * it shows far less of a grid than a tile's.
 */
std::vector<int16_t> CutBlood()
{
	constexpr int N = BloodSize;
	std::vector<int16_t> texels(static_cast<size_t>(N) * N, 1);
	if (pMegaTiles == nullptr)
		return texels;
	constexpr int Width = MegatileWidth;
	int height;
	const std::vector<int16_t> canvas = ComposeMegatile(LakeTile, height);
	int top = -1;
	for (int y = 0; y < height && top < 0; y++)
		if (canvas[static_cast<size_t>(y) * Width + Width / 2] > 0 || canvas[static_cast<size_t>(y) * Width + Width / 2 - 1] > 0)
			top = y;
	if (top < 0)
		return texels;
	for (int v = 0; v < N; v++) {
		int16_t last = 1;
		for (int u = 0; u < N; u++) {
			const float fx = (static_cast<float>(u) + 0.5F) / N;
			const float fy = (static_cast<float>(v) + 0.5F) / N;
			const int sx = std::clamp(static_cast<int>(Width / 2 + (fx - fy) * Width / 2), 0, Width - 1);
			const int sy = std::clamp(static_cast<int>(static_cast<float>(top) + (fx + fy) * N / 2), 0, height - 1);
			const int16_t texel = canvas[static_cast<size_t>(sy) * Width + sx];
			// Only blood (the cycled colours): the diamond's outline falls just outside the art here and there.
			if (texel >= 1 && texel < 32)
				last = texel;
			texels[static_cast<size_t>(v) * N + u] = last;
		}
	}
	return texels;
}

void EnsureBlood()
{
	if (Blood.cels == pDungeonCels.get() && !Blood.texels.empty())
		return;
	ForgetUpscaledTexture(Blood.texels);
	Blood.cels = pDungeonCels.get();
	Blood.texels = CutBlood();
}

/**
 * The crown of a spine column (its art's crossed bones and horns, above the narrow neck of its spine), cut out of its
 * map tile's art to be drawn as a billboard on the column's mesh spine. `pixels` is `width` x `height` (the column's
 * axis in the middle), its bottom `neck` art rows above the middle of the column's floor diamond.
 */
struct Crown {
	std::vector<int16_t> pixels;
	int width = 0;
	int height = 0;
	int neck = 0;
};
constexpr float ColumnArtScale = 0.75F; // drawn tiles per tile of the columns' art (as tall as SpritePixelsPerTileY)

const Crown &CrownFor(int megatile, int quadrant)
{
	static std::map<int, Crown> cache;
	static const std::byte *cacheFor = nullptr;
	if (cacheFor != pDungeonCels.get()) {
		cache.clear();
		cacheFor = pDungeonCels.get();
	}
	auto [it, added] = cache.try_emplace(megatile * 4 + quadrant);
	Crown &crown = it->second;
	if (!added || pMegaTiles == nullptr)
		return crown;
	int height;
	const std::vector<int16_t> canvas = ComposeMegatile(megatile, height);
	// The column's axis and the middle of its floor diamond in the map tile's art.
	const auto [ox, oy] = QuadrantOffsets[quadrant];
	const int axis = ox + ColumnWidth / 2;
	const int floorRow = oy + height - ColumnWidth / 2 - FloorTextureSize / 2;
	// From the top of the art down: the crown is wide, the spine under it narrow. Its neck: the first row, past the
	// crown's widest, where the art round the axis is narrower than NeckWidth.
	constexpr int NeckWidth = 30;
	auto widthAt = [&](int y) {
		int left = MegatileWidth;
		int right = -1;
		for (int x = 0; x < MegatileWidth; x++) {
			if (canvas[static_cast<size_t>(y) * MegatileWidth + x] > 0) {
				left = std::min(left, x);
				right = std::max(right, x);
			}
		}
		return right >= left ? right - left + 1 : 0;
	};
	int top = -1;
	int neck = -1;
	bool wide = false;
	for (int y = 0; y < floorRow && neck < 0; y++) {
		const int w = widthAt(y);
		if (top < 0 && w > 0)
			top = y;
		if (w > NeckWidth * 2)
			wide = true;
		else if (wide && w < NeckWidth)
			neck = y;
	}
	if (top < 0 || neck < 0)
		return crown;
	crown.width = MegatileWidth;
	crown.height = neck - top;
	crown.neck = floorRow - neck;
	crown.pixels.assign(static_cast<size_t>(crown.width) * crown.height, NoPixel);
	for (int y = 0; y < crown.height; y++) {
		for (int x = 0; x < crown.width; x++) {
			const int sx = axis - crown.width / 2 + x;
			if (sx >= 0 && sx < MegatileWidth)
				crown.pixels[static_cast<size_t>(y) * crown.width + x] = canvas[static_cast<size_t>(top + y) * MegatileWidth + sx];
		}
	}
	return crown;
}

/** The crown of the spine column at a tile (empty if its art has none). */
const Crown &CrownAt(Point tile)
{
	return CrownFor(MegatileAt(tile), (tile.x & 1) + 2 * (tile.y & 1));
}

/**
 * The flesh of the walls' sides: the panel on the left face of a straight flesh wall's top piece, between its posts
 * and under its beam, with one of the braided bone veins running across it (FleshWidth x FleshRows texels of art,
 * from FleshFrom rows above the face's foot). Mirrored, it repeats every two widths without a seam.
 */
constexpr int FleshWidth = 20;
constexpr int FleshFrom = 26;
constexpr int FleshRows = 35;
constexpr int FleshSize = 64;

bool CutFlesh(CleanSourceTexture &out)
{
	if (pMegaTiles == nullptr)
		return false;
	const std::vector<int16_t> &column = GetPieceTextures(MegatilePiece(FleshWallTile, 0)).column;
	const int rows = static_cast<int>(column.size() / ColumnWidth);
	if (rows < 64)
		return false;
	// The face's foot runs from the diamond's left corner (16 rows above the column's bottom) up to its top vertex;
	// a face row h is h rows above FaceBaseRows below it.
	std::vector<std::array<float, 3>> art(static_cast<size_t>(FleshWidth) * FleshRows);
	int drawn = 0;
	for (int r = 0; r < FleshRows; r++) {
		const int h = FleshFrom + FleshRows - 1 - r;
		for (int u = 0; u < FleshWidth; u++) {
			const int y = static_cast<int>(static_cast<float>(rows - 16) - static_cast<float>(u) / 2.0F + FaceBaseRows) - h;
			const int16_t texel = y >= 0 && y < rows ? column[static_cast<size_t>(y) * ColumnWidth + u] : NoPixel;
			const SDL_Color c = logical_palette[texel > 0 ? texel : 0];
			art[static_cast<size_t>(r) * FleshWidth + u] = { static_cast<float>(c.r), static_cast<float>(c.g), static_cast<float>(c.b) };
			drawn += texel > 0 ? 1 : 0;
		}
	}
	if (drawn < FleshWidth * FleshRows / 2)
		return false;
	// Resampled (bilinear) to FleshSize square: across, the panel and its mirror image.
	out.width = FleshSize;
	out.height = FleshSize;
	out.tilesAcross = 2.0F * FleshWidth / FaceTextureWidth;
	out.tilesDown = WallTop;
	out.rgb.resize(static_cast<size_t>(FleshSize) * FleshSize * 3);
	const auto at = [&](int u, int r) -> const std::array<float, 3> & {
		u = ((u % (2 * FleshWidth)) + 2 * FleshWidth) % (2 * FleshWidth);
		if (u >= FleshWidth)
			u = 2 * FleshWidth - 1 - u;
		return art[static_cast<size_t>(std::clamp(r, 0, FleshRows - 1)) * FleshWidth + u];
	};
	for (int y = 0; y < FleshSize; y++) {
		const float sr = (static_cast<float>(y) + 0.5F) / FleshSize * FleshRows - 0.5F;
		const int r0 = static_cast<int>(std::floor(sr));
		const float fr = sr - static_cast<float>(r0);
		for (int x = 0; x < FleshSize; x++) {
			const float su = (static_cast<float>(x) + 0.5F) / FleshSize * 2.0F * FleshWidth - 0.5F;
			const int u0 = static_cast<int>(std::floor(su));
			const float fu = su - static_cast<float>(u0);
			for (int c = 0; c < 3; c++) {
				const float top = at(u0, r0)[c] * (1 - fu) + at(u0 + 1, r0)[c] * fu;
				const float bottom = at(u0, r0 + 1)[c] * (1 - fu) + at(u0 + 1, r0 + 1)[c] * fu;
				out.rgb[(static_cast<size_t>(y) * FleshSize + x) * 3 + c] = static_cast<uint8_t>(std::clamp(top + (bottom - top) * fr + 0.5F, 0.0F, 255.0F));
			}
		}
	}
	return true;
}

// --- Lighting ------------------------------------------------------------------------------------------------------

inline float TileLight(int x, int y)
{
	if (x < 0 || y < 0 || x >= MAXDUNX || y >= MAXDUNY)
		return LightsMax;
	return static_cast<float>(dLight[x][y]);
}

/** An art texel (palette index) lit by the game's light tables (which cycle hell's blood), 0 for none. */
inline uint8_t LitTexel(int16_t texel, float light, int x, int y)
{
	return texel <= 0 ? 0 : LightTables[LightLevel(light, x, y)][static_cast<uint8_t>(texel)];
}

} // namespace

// --- Public --------------------------------------------------------------------------------------------------------

bool IsHell()
{
	return leveltype == DTYPE_HELL;
}

float HellWallTop()
{
	return WallTop;
}

float HellArtScale()
{
	return ArtScale;
}

void ClassifyHellTiles(std::vector<CleanKind> &kinds, std::vector<float> &heights)
{
	const size_t count = static_cast<size_t>(MAXDUNX) * MAXDUNY;
	Flags.assign(count, 0);
	FloorPieces.assign(count, 0);
	auto solid = [](Point p) { return !InDungeonBounds(p) || IsTileSolid(p); };
	int walls = 0;
	int columns = 0;
	int props = 0;
	for (int y = 0; y < MAXDUNY; y++) {
		for (int x = 0; x < MAXDUNX; x++) {
			const Point tile { x, y };
			const size_t at = static_cast<size_t>(y) * MAXDUNX + x;
			const int id = MegatileAt(tile);
			const Megatile what = ClassOf(id);
			FloorPieces[at] = dPiece[x][y];
			// The painted shadows of walls go with the iso light, and the stairs' rib cages are models standing on plain
			// ground: the plain floor's piece at the same place instead.
			if ((what == Megatile::Shadow || what == Megatile::Stairs) && pMegaTiles != nullptr)
				FloorPieces[at] = MegatilePiece(PlainFloorTile, (x & 1) + 2 * (y & 1));
			if (!IsTileSolid(tile)) {
				kinds[at] = CleanKind::Open;
				continue;
			}
			heights[at] = WallTop;
			switch (what) {
			case Megatile::Blood:
				kinds[at] = CleanKind::Wall;
				Flags[at] = TileBlood;
				walls++;
				break;
			case Megatile::Spike:
			case Megatile::Stairs:
				kinds[at] = CleanKind::Prop;
				props++;
				break;
			default: {
				const bool lone = !solid(tile + Displacement { 1, 0 }) && !solid(tile + Displacement { -1, 0 }) && !solid(tile + Displacement { 0, 1 }) && !solid(tile + Displacement { 0, -1 });
				kinds[at] = lone ? CleanKind::Pillar : CleanKind::Wall;
				(lone ? columns : walls)++;
				// A column's model stands on plain ground (its art paints its foot on the floor).
				if (lone && pMegaTiles != nullptr)
					FloorPieces[at] = MegatilePiece(PlainFloorTile, (x & 1) + 2 * (y & 1));
				break;
			}
			}
		}
	}
	// Blood on top: the troughs and the lake, and the flesh walls along them (the fronts of the troughs).
	for (int y = 0; y < MAXDUNY; y++) {
		for (int x = 0; x < MAXDUNX; x++) {
			const size_t at = static_cast<size_t>(y) * MAXDUNX + x;
			if (kinds[at] != CleanKind::Wall)
				continue;
			bool blood = (Flags[at] & TileBlood) != 0;
			for (const Displacement d : { Displacement { 1, 0 }, Displacement { -1, 0 }, Displacement { 0, 1 }, Displacement { 0, -1 } }) {
				const Point n = Point { x, y } + d;
				if (InDungeonBounds(n) && (Flags[static_cast<size_t>(n.y) * MAXDUNX + n.x] & TileBlood) != 0)
					blood = true;
			}
			if (blood)
				Flags[at] |= TileBloodTop;
		}
	}
	LogInfo("fp: hell tiles: {} walls, {} spine columns, {} spikes and rib cages", walls, columns, props);
}

bool IsHellBlood(Point tile)
{
	if (!InDungeonBounds(tile) || Flags.empty())
		return false;
	return (Flags[static_cast<size_t>(tile.y) * MAXDUNX + tile.x] & TileBloodTop) != 0;
}

bool IsHellStairsTile(Point tile)
{
	return IsHell() && InDungeonBounds(tile) && ClassOf(MegatileAt(tile)) == Megatile::Stairs;
}

void HellTintTexture(CleanSurface surface, CleanSourceTexture &texture)
{
	if (!IsHell() || (surface != CleanSurface::Pillar && surface != CleanSurface::Stone))
		return;
	// The mean of the lighter halves of the bone ramps (80-89, 112-121), in l4_1.pal and in the level's palette.
	constexpr std::array<float, 3> L4Bone { 99.55F, 80.55F, 58.9F };
	std::array<float, 3> bone {};
	for (const int base : { 80, 112 }) {
		for (int i = base; i < base + 10; i++) {
			const SDL_Color c = logical_palette[i];
			bone[0] += static_cast<float>(c.r) / 20.0F;
			bone[1] += static_cast<float>(c.g) / 20.0F;
			bone[2] += static_cast<float>(c.b) / 20.0F;
		}
	}
	for (size_t i = 0; i < texture.rgb.size(); i++) {
		const float scaled = static_cast<float>(texture.rgb[i]) * bone[i % 3] / L4Bone[i % 3];
		texture.rgb[i] = static_cast<uint8_t>(std::clamp(scaled + 0.5F, 0.0F, 255.0F));
	}
}

bool HellArtTexture(CleanSurface surface, CleanSourceTexture &out)
{
	if (!IsHell() || surface != CleanSurface::Wall)
		return false;
	return CutFlesh(out);
}

const std::vector<int16_t> &HellPropArt(uint16_t piece)
{
	static std::map<uint16_t, std::vector<int16_t>> cache;
	static const std::byte *cacheFor = nullptr;
	if (cacheFor != pDungeonCels.get()) {
		cache.clear();
		cacheFor = pDungeonCels.get();
	}
	auto [it, added] = cache.try_emplace(piece);
	if (!added)
		return it->second;
	std::vector<int16_t> &art = it->second;
	art = GetPieceTextures(piece).column;
	const int rows = static_cast<int>(art.size() / ColumnWidth);
	// The floor diamond: its bottom FloorTextureSize rows. The ash and mauve ramps there are ground.
	for (int y = std::max(0, rows - FloorTextureSize); y < rows; y++) {
		for (int x = 0; x < ColumnWidth; x++) {
			int16_t &texel = art[static_cast<size_t>(y) * ColumnWidth + x];
			if (texel >= 32 && texel < 64)
				texel = NoPixel;
		}
	}
	return art;
}

void CollectHellCrowns(Vec2 viewerPos, std::vector<Entity> &entities)
{
	if (!IsHell())
		return;
	const CleanKind *kinds = CleanKindMap();
	constexpr int Radius = 24;
	const Point centre { static_cast<int>(viewerPos.x), static_cast<int>(viewerPos.y) };
	for (int y = centre.y - Radius; y <= centre.y + Radius; y++) {
		for (int x = centre.x - Radius; x <= centre.x + Radius; x++) {
			const Point tile { x, y };
			if (!InDungeonBounds(tile) || kinds[static_cast<size_t>(y) * MAXDUNX + x] != CleanKind::Pillar)
				continue;
			const Crown &crown = CrownAt(tile);
			if (crown.pixels.empty())
				continue;
			Entity entity {};
			entity.kind = EntityKind::Prop;
			entity.id = -1;
			entity.tile = tile;
			entity.pos = { static_cast<float>(x) + 0.5F, static_cast<float>(y) + 0.5F };
			entity.width = static_cast<float>(crown.width) / SpritePixelsPerTileX * ColumnArtScale;
			entity.height = static_cast<float>(crown.height) / SpritePixelsPerTileY * ColumnArtScale;
			entity.bottom = static_cast<float>(crown.neck) / SpritePixelsPerTileY * ColumnArtScale;
			entity.pixels = &crown.pixels;
			entity.pixelsWidth = crown.width;
			entity.pixelsHeight = crown.height;
			entity.lit = true;
			entities.push_back(entity);
		}
	}
}

void DrawHellFloor(const Surface &out, const Projection &proj, std::vector<float> &depthBuffer)
{
	EnsureBlood();
	const bool holes = HasFloorHoles();
	const Vec2 rayLeft { proj.dir.x - proj.plane.x, proj.dir.y - proj.plane.y };
	const Vec2 rayRight { proj.dir.x + proj.plane.x, proj.dir.y + proj.plane.y };
	constexpr int FineSize = FloorTextureSize * TextureUpscale;
	for (int y = 0; y < proj.height; y++) {
		uint8_t *dst = out.at(0, y);
		if (y <= proj.horizon) {
			std::fill_n(dst, proj.width, 0); // the sky's (fp_sky.cpp), where nothing stands in front of it
			continue;
		}
		const float rowDistance = EyeHeight * proj.focal / static_cast<float>(y - proj.horizon);
		const float stepX = rowDistance * (rayRight.x - rayLeft.x) / static_cast<float>(proj.width);
		const float stepY = rowDistance * (rayRight.y - rayLeft.y) / static_cast<float>(proj.width);
		float wx = proj.pos.x + rowDistance * rayLeft.x;
		float wy = proj.pos.y + rowDistance * rayLeft.y;
		// Up close, the floor art at TextureUpscale times its resolution (see UpscaledTexture).
		const bool fineRow = std::hypot(stepX, stepY) * FloorTextureSize < UpscaleBelow;
		float *depthRow = &depthBuffer[static_cast<size_t>(y) * proj.width];
		const float depth = rowDistance * 1.03F; // see FloorDepthBias in fp_view.cpp
		Point cachedTile { -1, -1 };
		const std::vector<int16_t> *floor = nullptr;
		const std::vector<int16_t> *fine = nullptr;
		int cellX = std::numeric_limits<int>::min();
		int cellY = 0;
		float l00 = 0;
		float l10 = 0;
		float l01 = 0;
		float l11 = 0;
		for (int x = 0; x < proj.width; x++, wx += stepX, wy += stepY) {
			const int tx = static_cast<int>(std::floor(wx));
			const int ty = static_cast<int>(std::floor(wy));
			if (tx < 0 || ty < 0 || tx >= MAXDUNX || ty >= MAXDUNY) {
				dst[x] = 0;
				continue;
			}
			if (holes && IsFloorHole(wx, wy)) {
				dst[x] = 0; // the pit of the stairs down: drawn into by its model
				continue;
			}
			if (tx != cachedTile.x || ty != cachedTile.y) {
				cachedTile = { tx, ty };
				floor = &GetPieceTextures(FloorPieces[static_cast<size_t>(ty) * MAXDUNX + tx]).floor;
				const bool sized = floor->size() == static_cast<size_t>(FloorTextureSize) * FloorTextureSize;
				if (!sized)
					floor = nullptr;
				fine = fineRow && floor != nullptr ? &UpscaledTexture(*floor, FloorTextureSize) : nullptr;
			}
			// The light, interpolated between tile centres.
			const float fx = wx - 0.5F;
			const float fy = wy - 0.5F;
			const int cx = static_cast<int>(std::floor(fx));
			const int cy = static_cast<int>(std::floor(fy));
			if (cx != cellX || cy != cellY) {
				cellX = cx;
				cellY = cy;
				l00 = TileLight(cx, cy);
				l10 = TileLight(cx + 1, cy);
				l01 = TileLight(cx, cy + 1);
				l11 = TileLight(cx + 1, cy + 1);
			}
			depthRow[x] = depth;
			if (l00 >= LightsMax && l10 >= LightsMax && l01 >= LightsMax && l11 >= LightsMax) {
				dst[x] = 0; // in the dark
				continue;
			}
			const float ax = fx - static_cast<float>(cx);
			const float ay = fy - static_cast<float>(cy);
			const float top = l00 + (l10 - l00) * ax;
			const float bottom = l01 + (l11 - l01) * ax;
			const float light = top + (bottom - top) * ay;
			if (floor == nullptr) {
				dst[x] = 0;
				continue;
			}
			const float lu = wx - static_cast<float>(tx);
			const float lv = wy - static_cast<float>(ty);
			int16_t texel = (*floor)[static_cast<size_t>(std::min(static_cast<int>(lv * FloorTextureSize), FloorTextureSize - 1)) * FloorTextureSize + std::min(static_cast<int>(lu * FloorTextureSize), FloorTextureSize - 1)];
			if (fine != nullptr) {
				const int16_t f = (*fine)[static_cast<size_t>(std::min(static_cast<int>(lv * FineSize), FineSize - 1)) * FineSize + std::min(static_cast<int>(lu * FineSize), FineSize - 1)];
				if (f != NoPixel)
					texel = f;
			}
			dst[x] = LitTexel(texel, light, x, y);
		}
	}
}

void DrawHellWalls(const Surface &out, const Projection &proj, std::vector<float> &zBuffer, std::vector<float> &depthBuffer)
{
	EnsureBlood();
	const CleanKind *kinds = CleanKindMap();
	const std::vector<int16_t> &blood = Blood.texels;
	const std::vector<int16_t> &bloodFine = UpscaledTexture(blood, BloodSize);
	constexpr int FineSize = BloodSize * TextureUpscale;
	const float drop = EyeHeight - WallTop; // how far the tops are below the eye
	// Each column is walked once through the grid, front to back: every wall is under the eye, so what comes into view
	// farther on lies above (on screen) what was drawn nearer. `yTop` is the highest row the walls have drawn yet.
	for (int x = 0; x < proj.width; x++) {
		zBuffer[x] = MaxRayDistance;
		const Vec2 ray = proj.columnRay(x);
		const float rayLength = std::hypot(ray.x, ray.y);
		Point tile { static_cast<int>(std::floor(proj.pos.x)), static_cast<int>(std::floor(proj.pos.y)) };
		const float deltaX = ray.x == 0 ? 1e30F : std::abs(1.0F / ray.x);
		const float deltaY = ray.y == 0 ? 1e30F : std::abs(1.0F / ray.y);
		const int stepX = ray.x < 0 ? -1 : 1;
		const int stepY = ray.y < 0 ? -1 : 1;
		float sideX = ray.x < 0 ? (proj.pos.x - static_cast<float>(tile.x)) * deltaX : (static_cast<float>(tile.x) + 1.0F - proj.pos.x) * deltaX;
		float sideY = ray.y < 0 ? (proj.pos.y - static_cast<float>(tile.y)) * deltaY : (static_cast<float>(tile.y) + 1.0F - proj.pos.y) * deltaY;
		auto isWall = [&](Point t) { return !InDungeonBounds(t) || kinds[static_cast<size_t>(t.y) * MAXDUNX + t.x] == CleanKind::Wall; };
		bool previousWall = isWall(tile); // the camera's own tile: no face to draw
		bool enteredX = false;
		float enter = 0;
		int yTop = proj.height;
		CleanSampler flesh;
		// Looking down, the horizon lies above the screen (pitch up to MaxPitch): rows stop at the top of the screen.
		const int firstRow = std::max(0, proj.horizon + 1);
		while (enter < MaxRayDistance && yTop > firstRow && InDungeonBounds(tile)) {
			const float leave = std::min(sideX, sideY);
			const bool wall = isWall(tile);
			if (wall && !previousWall && enter > 0.01F) {
				// The side of the wall the ray comes in by, from the floor up to the top: flesh.
				if (zBuffer[x] == MaxRayDistance)
					zBuffer[x] = enter;
				const float scale = proj.focal / enter;
				const int top = std::max(firstRow, static_cast<int>(std::ceil(static_cast<float>(proj.horizon) + drop * scale - 0.5F)));
				const int bottom = std::min({ proj.height - 1, static_cast<int>(static_cast<float>(proj.horizon) + EyeHeight * scale), yTop - 1 });
				const Vec2 p { proj.pos.x + ray.x * enter, proj.pos.y + ray.y * enter };
				const float along = enteredX ? p.y : p.x;
				const float light = CleanLightAt(p.x - ray.x / rayLength * 0.05F, p.y - ray.y / rayLength * 0.05F) + (enteredX ? 0.0F : FaceYShade);
				PrepareCleanSampler(flesh, CleanSurface::Wall, enter / proj.focal);
				const bool dark = light >= LightsMax;
				for (int y = top; y <= bottom; y++) {
					const float h = EyeHeight + static_cast<float>(proj.horizon - y) / proj.focal * enter;
					const float v = std::clamp(WallTop - h, 0.0F, WallTop * 0.999F);
					*out.at(x, y) = dark ? 0 : flesh.At(along, v, LightLevel(light, x, y), x, y);
					depthBuffer[static_cast<size_t>(y) * proj.width + x] = enter;
				}
				yTop = std::min(yTop, top);
			}
			if (wall && leave > 0.01F) {
				// Its top, from where the ray comes in (or is first seen over the wall in front) to where it leaves.
				const int from = std::max(firstRow, static_cast<int>(std::ceil(static_cast<float>(proj.horizon) + drop * proj.focal / leave - 0.5F)));
				const bool bloodTop = InDungeonBounds(tile) && IsHellBlood(tile);
				if (!bloodTop)
					PrepareCleanSampler(flesh, CleanSurface::Wall, (enter + leave) * 0.5F / proj.focal);
				for (int y = from; y < yTop; y++) {
					const float t = drop * proj.focal / static_cast<float>(y - proj.horizon);
					const Vec2 p { proj.pos.x + ray.x * t, proj.pos.y + ray.y * t };
					const float light = CleanLightAt(p.x, p.y);
					uint8_t color;
					if (bloodTop) {
						// Across the blood's stretch of BloodTiles tiles, wherever it lies.
						const float lu = std::clamp(p.x / BloodTiles - std::floor(p.x / BloodTiles), 0.0F, 0.9999F);
						const float lv = std::clamp(p.y / BloodTiles - std::floor(p.y / BloodTiles), 0.0F, 0.9999F);
						const int16_t texel = t / proj.focal * FloorTextureSize < UpscaleBelow
						    ? bloodFine[static_cast<size_t>(lv * FineSize) * FineSize + static_cast<size_t>(lu * FineSize)]
						    : blood[static_cast<size_t>(lv * BloodSize) * BloodSize + static_cast<size_t>(lu * BloodSize)];
						color = LitTexel(texel, std::max(light - BloodGlow, 0.0F), x, y);
					} else {
						color = light + 1.0F >= LightsMax ? 0 : flesh.At(p.x, p.y, LightLevel(light + 1.0F, x, y), x, y);
					}
					*out.at(x, y) = color;
					depthBuffer[static_cast<size_t>(y) * proj.width + x] = t;
				}
				yTop = std::min(yTop, from);
			}
			previousWall = wall;
			if (sideX < sideY) {
				sideX += deltaX;
				tile.x += stepX;
				enteredX = true;
			} else {
				sideY += deltaY;
				tile.y += stepY;
				enteredX = false;
			}
			enter = leave;
		}
	}
}

void BuildHellGeometry(std::vector<CleanQuad> &quads)
{
	using P3 = std::array<float, 3>;
	const CleanKind *kinds = CleanKindMap();
	auto kindAt = [&](Point t) { return InDungeonBounds(t) ? kinds[static_cast<size_t>(t.y) * MAXDUNX + t.x] : CleanKind::Wall; };
	auto isWall = [&](Point t) { return kindAt(t) == CleanKind::Wall; };

	auto add = [](P3 a, P3 b) { return P3 { a[0] + b[0], a[1] + b[1], a[2] + b[2] }; };
	auto sub = [](P3 a, P3 b) { return P3 { a[0] - b[0], a[1] - b[1], a[2] - b[2] }; };
	auto mul = [](P3 a, float k) { return P3 { a[0] * k, a[1] * k, a[2] * k }; };
	auto dot = [](P3 a, P3 b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
	auto cross = [](P3 a, P3 b) { return P3 { a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0] }; };
	auto unit = [&](P3 a) {
		const float len = std::sqrt(dot(a, a));
		return len > 1e-6F ? mul(a, 1.0F / len) : P3 { 0, 0, 1 };
	};

	/**
	 * A tube along a path, `sides` round, of a radius at each point of it (0 closes it to a point); its texture runs
	 * along it (u, tiles) and round it (v). Shaded by the way each quad faces: up lit, down in shadow.
	 */
	auto addTube = [&](const std::vector<P3> &path, const std::vector<float> &radius, int sides, CleanSurface surface, bool capStart, bool capEnd) {
		const size_t n = path.size();
		if (n < 2)
			return;
		std::vector<P3> tangents(n);
		for (size_t i = 0; i < n; i++)
			tangents[i] = unit(sub(path[std::min(i + 1, n - 1)], path[i == 0 ? 0 : i - 1]));
		// The frame round the path: carried along it (parallel transport), so the tube does not twist.
		std::vector<P3> normals(n);
		const P3 reference = std::abs(tangents[0][2]) < 0.9F ? P3 { 0, 0, 1 } : P3 { 1, 0, 0 };
		normals[0] = unit(cross(cross(tangents[0], reference), tangents[0]));
		for (size_t i = 1; i < n; i++)
			normals[i] = unit(sub(normals[i - 1], mul(tangents[i], dot(normals[i - 1], tangents[i]))));
		std::vector<std::vector<P3>> rings(n, std::vector<P3>(static_cast<size_t>(sides) + 1));
		std::vector<std::vector<P3>> directions(n, std::vector<P3>(static_cast<size_t>(sides) + 1));
		std::vector<float> lengths(n, 0);
		for (size_t i = 0; i < n; i++) {
			if (i > 0)
				lengths[i] = lengths[i - 1] + std::sqrt(dot(sub(path[i], path[i - 1]), sub(path[i], path[i - 1])));
			const P3 binormal = cross(tangents[i], normals[i]);
			for (int k = 0; k <= sides; k++) {
				const float a = 6.2831853F * static_cast<float>(k) / static_cast<float>(sides);
				const P3 d = add(mul(normals[i], std::cos(a)), mul(binormal, std::sin(a)));
				directions[i][k] = d;
				rings[i][k] = add(path[i], mul(d, radius[i]));
			}
		}
		for (size_t i = 0; i + 1 < n; i++) {
			const float around = (radius[i] + radius[i + 1]) * 0.5F * 6.2831853F / static_cast<float>(sides);
			for (int k = 0; k < sides; k++) {
				const P3 normal = unit(add(add(directions[i][k], directions[i][k + 1]), add(directions[i + 1][k], directions[i + 1][k + 1])));
				CleanQuad quad;
				quad.p = { rings[i][k], rings[i + 1][k], rings[i + 1][k + 1], rings[i][k + 1] };
				quad.normal = normal;
				const float v0 = around * static_cast<float>(k);
				const float v1 = around * static_cast<float>(k + 1);
				quad.uv = { { { lengths[i], v0 }, { lengths[i + 1], v0 }, { lengths[i + 1], v1 }, { lengths[i], v1 } } };
				quad.surface = surface;
				quad.darkness = 0.5F * (1.0F - normal[2]);
				quads.push_back(quad);
			}
		}
		// Flat ends: a fan of quads (each a triangle, its last corner repeated) round the end's middle.
		auto addCap = [&](size_t i, float facing) {
			if (radius[i] <= 0.0F)
				return;
			const P3 normal = mul(tangents[i], facing);
			for (int k = 0; k < sides; k++) {
				CleanQuad quad;
				const P3 &a = rings[i][facing > 0 ? k : k + 1];
				const P3 &b = rings[i][facing > 0 ? k + 1 : k];
				quad.p = { path[i], a, b, b };
				quad.normal = normal;
				quad.uv = { { { 0, 0 }, { (a[0] - path[i][0]), (a[1] - path[i][1]) }, { (b[0] - path[i][0]), (b[1] - path[i][1]) }, { (b[0] - path[i][0]), (b[1] - path[i][1]) } } };
				quad.surface = CleanSurface::Stone;
				quad.darkness = 0.5F * (1.0F - normal[2]);
				quads.push_back(quad);
			}
		};
		if (capStart)
			addCap(0, -1.0F);
		if (capEnd)
			addCap(n - 1, 1.0F);
	};

	/** A horn from `base`, leaning out along `out` and curving up to its point, `length` long. */
	auto addHorn = [&](P3 base, P3 out, P3 side, float length, float thickness) {
		constexpr int Segments = 5;
		std::vector<P3> path;
		std::vector<float> radius;
		for (int i = 0; i <= Segments; i++) {
			const float t = static_cast<float>(i) / Segments;
			// Out first, then up, its point turning back in a little: a lyre's arm.
			const float o = length * (0.42F * std::sin(t * 2.4F));
			const float u = length * (0.95F * t * t * (1.6F - 0.6F * t));
			const float s = length * 0.06F * t * t;
			path.push_back(add(add(add(base, mul(out, o)), P3 { 0, 0, u }), mul(side, s)));
			radius.push_back(thickness * (1.0F - t) * (1.0F - 0.25F * t));
		}
		addTube(path, radius, 5, CleanSurface::Pillar, false, false);
	};

	/** A spine of vertebrae from the floor up to `height`, at (x, y), `body` and `joint` radii; with a skull and horns on top, its horns spread along `along`. */
	auto addSpine = [&](float cx, float cy, float height, float body, float joint, P3 along, P3 facing, float hornLength, bool withSkull) {
		constexpr int Vertebrae = 3;
		std::vector<P3> path;
		std::vector<float> radius;
		path.push_back({ cx, cy, 0.0F });
		radius.push_back(body * 1.35F); // its foot spreads on the floor
		const float segment = height / Vertebrae;
		for (int i = 0; i < Vertebrae; i++) {
			const float z = segment * static_cast<float>(i);
			path.push_back({ cx, cy, z + segment * 0.5F });
			radius.push_back(body);
			path.push_back({ cx, cy, z + segment });
			radius.push_back(joint);
		}
		addTube(path, radius, 6, CleanSurface::Pillar, false, false);
		if (!withSkull)
			return;
		// The skull: a knob a little wider than the spine, its horns from its sides.
		const float skull = body * 1.35F;
		std::vector<P3> head;
		std::vector<float> headRadius;
		const std::array<std::pair<float, float>, 4> profile { { { 0.0F, 0.55F }, { 0.35F, 1.0F }, { 0.75F, 0.85F }, { 1.0F, 0.0F } } };
		for (const auto &[at, r] : profile) {
			head.push_back({ cx + facing[0] * skull * 0.25F * at, cy + facing[1] * skull * 0.25F * at, height + skull * 1.8F * at });
			headRadius.push_back(skull * r);
		}
		addTube(head, headRadius, 6, CleanSurface::Stone, false, false);
		const float hornZ = height + skull * 0.9F;
		for (const float s : { -1.0F, 1.0F }) {
			const P3 out = mul(along, s);
			addHorn(add(P3 { cx, cy, hornZ }, mul(out, skull * 0.7F)), out, facing, hornLength, skull * 0.45F);
		}
	};

	// The edges round the tops of the walls, where a wall meets anything that is not a wall: runs along each face.
	struct Run {
		int face;    // 0 +x, 1 -x, 2 +y, 3 -y
		int line;    // the tile the face belongs to, across
		int from;    // first tile along
		int to;      // last tile along
	};
	std::vector<Run> runs;
	constexpr std::array<Displacement, 4> Fronts { { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } } };
	for (int face = 0; face < 4; face++) {
		const Displacement front = Fronts[face];
		const bool xFace = front.deltaX != 0; // a face towards x runs along y
		for (int line = 0; line < (xFace ? MAXDUNX : MAXDUNY); line++) {
			auto edge = [&](int step) {
				const Point tile = xFace ? Point { line, step } : Point { step, line };
				return isWall(tile) && InDungeonBounds(tile) && !isWall(tile + front);
			};
			const int steps = xFace ? MAXDUNY : MAXDUNX;
			for (int step = 0; step < steps;) {
				if (!edge(step)) {
					step++;
					continue;
				}
				const int start = step;
				while (step < steps && edge(step))
					step++;
				runs.push_back({ face, line, start, step - 1 });
			}
		}
	}
	// Posts: at both ends of every run and evenly between, each corner once.
	std::map<std::pair<int, int>, bool> posts;
	int beams = 0;
	for (const Run &run : runs) {
		const Displacement front = Fronts[run.face];
		const bool xFace = front.deltaX != 0;
		const float normalAcross = static_cast<float>(xFace ? front.deltaX : front.deltaY);
		// The face's plane, across, and the run's ends along it.
		const float plane = static_cast<float>(run.line) + (normalAcross > 0 ? 1.0F : 0.0F);
		float a = static_cast<float>(run.from);
		float b = static_cast<float>(run.to) + 1.0F;
		// At a wall's end (convex) the beam runs on past it a little, like crossed logs; at an inside corner it reaches
		// the other face's beam.
		auto tileAlong = [&](int step) { return xFace ? Point { run.line, step } : Point { step, run.line }; };
		const bool startOpen = !isWall(tileAlong(run.from - 1));
		const bool endOpen = !isWall(tileAlong(run.to + 1));
		const float beamA = a - (startOpen ? BeamOverhang : -BeamInset);
		const float beamB = b + (endOpen ? BeamOverhang : -BeamInset);
		const float acrossAxis = plane - normalAcross * BeamInset;
		auto point = [&](float along, float across, float z) { return xFace ? P3 { across, along, z } : P3 { along, across, z }; };
		const float z = WallTop + BeamLift;
		addTube({ point(beamA, acrossAxis, z), point(beamB, acrossAxis, z) }, { BeamRadius, BeamRadius }, 6, CleanSurface::Pillar, startOpen, endOpen);
		beams++;
		const float length = b - a;
		const int count = std::max(1, static_cast<int>(std::lround(length / PostSpacing)));
		const P3 alongDir = xFace ? P3 { 0, 1, 0 } : P3 { 1, 0, 0 };
		const P3 facing = xFace ? P3 { normalAcross, 0, 0 } : P3 { 0, normalAcross, 0 };
		for (int i = 0; i <= count; i++) {
			const float s = a + length * static_cast<float>(i) / static_cast<float>(count);
			// Corners are shared by two runs: posts keyed by their place (in 1/20 tiles) are put up once.
			const P3 p = point(s, plane + normalAcross * PostOut, 0);
			const std::pair<int, int> key { static_cast<int>(std::lround(p[0] * 20.0F)), static_cast<int>(std::lround(p[1] * 20.0F)) };
			if (posts.count(key) != 0)
				continue;
			// Inside corners lie in the angle of two faces: the post there is set back into it.
			posts[key] = true;
			addSpine(p[0], p[1], WallTop + PostAbove, PostBody, PostJoint, alongDir, facing, 0.62F, true);
		}
	}
	// The lone spine columns.
	int columns = 0;
	for (int y = 0; y < MAXDUNY; y++) {
		for (int x = 0; x < MAXDUNX; x++) {
			if (kinds[static_cast<size_t>(y) * MAXDUNX + x] != CleanKind::Pillar)
				continue;
			// The spine up to the crown cut out of its art (a billboard, CollectHellCrowns), or a skull and horns of its own.
			const Crown &crown = CrownAt({ x, y });
			const bool crowned = !crown.pixels.empty();
			const float height = crowned ? static_cast<float>(crown.neck) / SpritePixelsPerTileY * ColumnArtScale : ColumnHeight;
			const bool alongX = ((x + y) & 1) == 0;
			addSpine(static_cast<float>(x) + 0.5F, static_cast<float>(y) + 0.5F, height, ColumnBody, ColumnJoint, alongX ? P3 { 1, 0, 0 } : P3 { 0, 1, 0 },
			    alongX ? P3 { 0, 1, 0 } : P3 { 1, 0, 0 }, 0.95F, !crowned);
			columns++;
		}
	}
	LogInfo("fp: hell geometry: {} quads, {} beams, {} posts, {} spine columns", quads.size(), beams, posts.size(), columns);
}

} // namespace devilution::fp
