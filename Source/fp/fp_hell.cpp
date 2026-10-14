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
#include <cstdio>
#include <cstdlib>
#include <string>
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
#include "utils/str_cat.hpp"

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
/**
 * The posts' crowns, cut out of the corner post of map tile 9 (rows of its art as ComposeMegatile puts it together,
 * counted up from its bottom row, its axis in the middle): from the tips of its horns (PostCrownTop) to under the knob
 * where its beams cross (PostCrownBottom), at most PostCrownHalf to each side. The art's beams (meshes here) run out
 * from the knob, their tops at PostBeamTop beside it (out to PostBeamFlat) and PostBeamSlope rows lower per texel
 * further out: what lies above them is kept (the horns, the side spikes), and below them only the knob, narrowing
 * from PostKnobHalf at the beams' top to PostChinHalf at its chin. Those cuts fade out over PostCutFeather texels. The
 * knob's middle (PostKnobRow) sits on the beams. Drawn at PostCrownScale of the art's size, on a card facing out of
 * the wall. (Counted from the bottom: the game draws 12 of hell's 16 micros a piece, MicroTileLen, so its columns are
 * shorter than the files'.)
 */
constexpr int PostCrownTile = 9;
constexpr int PostCrownTop = 184;
constexpr int PostCrownBottom = 107;
constexpr int PostKnobRow = 113;
constexpr int PostCrownHalf = 48;
constexpr float PostBeamTop = 129.0F;
constexpr float PostBeamFlat = 20.0F;
constexpr float PostBeamSlope = 0.5F;
constexpr float PostKnobHalf = 20.0F;
constexpr float PostChinHalf = 7.0F;
constexpr float PostCutFeather = 3.0F;
constexpr float PostCrownScale = 0.55F;
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

/** The level's art cut-outs drawn on cards (HellSpriteAt), made by BuildHellGeometry. */
std::vector<HellSprite> Sprites;
enum : int16_t {
	PostCrownSprite = 0,
};

/** The crown of the wall posts (see PostCrownTile), or an empty sprite if the art has not got it. */
HellSprite CutPostCrown()
{
	HellSprite sprite;
	if (pMegaTiles == nullptr)
		return sprite;
	int height;
	const std::vector<int16_t> canvas = ComposeMegatile(PostCrownTile, height);
	const int axis = QuadrantOffsets[0].first + ColumnWidth / 2;
	const int top = height - PostCrownTop;
	if (top < 0)
		return sprite;
	sprite.width = 2 * PostCrownHalf;
	sprite.height = PostCrownTop - PostCrownBottom;
	sprite.pixels.assign(static_cast<size_t>(sprite.width) * sprite.height, NoPixel);
	sprite.alpha.assign(sprite.pixels.size(), 0);
	int drawn = 0;
	for (int y = 0; y < sprite.height; y++) {
		const int row = top + y;
		const float up = static_cast<float>(height - row); // rows above the art's bottom
		for (int x = 0; x < sprite.width; x++) {
			const int sx = axis - PostCrownHalf + x;
			if (sx < 0 || sx >= MegatileWidth)
				continue;
			const float dx = std::abs(static_cast<float>(sx - axis)) + 0.5F;
			// How far (texels) inside what is kept: above the beams' top, or within the knob under it.
			const float aboveBeams = up - (PostBeamTop - std::max(0.0F, dx - PostBeamFlat) * PostBeamSlope);
			const float down = std::clamp((PostBeamTop - up) / (PostBeamTop - static_cast<float>(PostCrownBottom)), 0.0F, 1.0F);
			const float knobHalf = PostChinHalf + (PostKnobHalf - PostChinHalf) * std::pow(1.0F - down, 0.8F);
			const float inside = std::max(aboveBeams, knobHalf - dx);
			const float keep = std::clamp((inside + 0.5F) / PostCutFeather, 0.0F, 1.0F);
			if (keep <= 0.0F)
				continue;
			const size_t at = static_cast<size_t>(y) * sprite.width + x;
			const int16_t texel = canvas[static_cast<size_t>(row) * MegatileWidth + sx];
			sprite.pixels[at] = texel;
			sprite.alpha[at] = static_cast<uint8_t>(keep * 255.0F + 0.5F);
			drawn += texel > 0 ? 1 : 0;
		}
	}
	if (drawn == 0)
		sprite = {};
	return sprite;
}

/**
 * Finds where a cut-out has art (HellSprite::rects): in bands SpriteBand texels wide, each run of rows with art in
 * the band (gaps under SpriteGap rows bridged), grown by a texel all round for its smoothed outline.
 */
constexpr int SpriteBand = 6;
constexpr int SpriteGap = 3;

void FindSpriteRects(HellSprite &sprite)
{
	sprite.rects.clear();
	for (int x0 = 0; x0 < sprite.width; x0 += SpriteBand) {
		const int x1 = std::min(sprite.width, x0 + SpriteBand);
		auto rowDrawn = [&](int y) {
			for (int x = std::max(0, x0 - 1); x < std::min(sprite.width, x1 + 1); x++)
				if (sprite.pixels[static_cast<size_t>(y) * sprite.width + x] > 0)
					return true;
			return false;
		};
		int y = 0;
		while (y < sprite.height) {
			if (!rowDrawn(y)) {
				y++;
				continue;
			}
			const int from = y;
			int last = y;
			while (y < sprite.height && (rowDrawn(y) || y - last < SpriteGap)) {
				if (rowDrawn(y))
					last = y;
				y++;
			}
			sprite.rects.push_back({ x0, std::max(0, from - 1), x1, std::min(sprite.height, last + 2) });
			y = last + 1;
		}
	}
}

/**
 * The flesh of the walls' sides: panels on the left faces of two flesh walls' top pieces (map tiles FleshPanelTiles),
 * between their posts and under their beams, each with braided bone veins running across it (FleshWidth x FleshRows
 * texels of art, from FleshFrom rows above the face's foot). Laid side by side as the first, it mirrored, the second,
 * it mirrored (FleshPeriod texels), cross-faded over FleshBlend texels where the two meet, they repeat every five
 * tiles without a seam.
 */
constexpr std::array<int, 2> FleshPanelTiles { FleshWallTile, 63 };
constexpr int FleshWidth = 20;
constexpr int FleshFrom = 26;
constexpr int FleshRows = 35;
constexpr int FleshPeriod = 4 * FleshWidth;
constexpr int FleshBlend = 3;
constexpr int FleshTextureWidth = 128;
constexpr int FleshTextureHeight = 64;

using Rgb = std::array<float, 3>;

/** A panel of flesh (FleshWidth x FleshRows, row 0 the top) cut out of a map tile's top piece; false if it is not drawn there. */
bool CutFleshPanel(int megatile, std::vector<Rgb> &art)
{
	const std::vector<int16_t> &column = GetPieceTextures(MegatilePiece(megatile, 0)).column;
	const int rows = static_cast<int>(column.size() / ColumnWidth);
	if (rows < 64)
		return false;
	// The face's foot runs from the diamond's left corner (16 rows above the column's bottom) up to its top vertex;
	// a face row h is h rows above FaceBaseRows below it.
	art.assign(static_cast<size_t>(FleshWidth) * FleshRows, Rgb {});
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
	return drawn >= FleshWidth * FleshRows / 2;
}

bool CutFlesh(CleanSourceTexture &out)
{
	if (pMegaTiles == nullptr)
		return false;
	std::array<std::vector<Rgb>, 2> panels;
	if (!CutFleshPanel(FleshPanelTiles[0], panels[0]))
		return false;
	if (!CutFleshPanel(FleshPanelTiles[1], panels[1]))
		panels[1] = panels[0];
	// One period across: a texel `u` of it (any integer), row r.
	const auto at = [&](int u, int r) {
		u = ((u % FleshPeriod) + FleshPeriod) % FleshPeriod;
		r = std::clamp(r, 0, FleshRows - 1);
		const int segment = u / FleshWidth;
		const int k = u % FleshWidth;
		const std::vector<Rgb> &panel = panels[segment / 2];
		const int column = (segment & 1) == 0 ? k : FleshWidth - 1 - k;
		Rgb c = panel[static_cast<size_t>(r) * FleshWidth + column];
		// The panels meet where both are at their first column (the first's mirror image and the second, the second's
		// and the first): near there each fades into the other, half and half at the join.
		if (column < FleshBlend) {
			const Rgb &o = panels[1 - segment / 2][static_cast<size_t>(r) * FleshWidth + column];
			const float t = 0.5F * (1.0F - static_cast<float>(column) / FleshBlend);
			for (int ch = 0; ch < 3; ch++)
				c[ch] = c[ch] * (1 - t) + o[ch] * t;
		}
		return c;
	};
	// Resampled (bilinear) to FleshTextureWidth x FleshTextureHeight.
	out.width = FleshTextureWidth;
	out.height = FleshTextureHeight;
	out.tilesAcross = static_cast<float>(FleshPeriod) / FaceTextureWidth;
	out.tilesDown = WallTop;
	out.rgb.resize(static_cast<size_t>(out.width) * out.height * 3);
	for (int y = 0; y < out.height; y++) {
		const float sr = (static_cast<float>(y) + 0.5F) / static_cast<float>(out.height) * FleshRows - 0.5F;
		const int r0 = static_cast<int>(std::floor(sr));
		const float fr = sr - static_cast<float>(r0);
		for (int x = 0; x < out.width; x++) {
			const float su = (static_cast<float>(x) + 0.5F) / static_cast<float>(out.width) * FleshPeriod - 0.5F;
			const int u0 = static_cast<int>(std::floor(su));
			const float fu = su - static_cast<float>(u0);
			const Rgb a = at(u0, r0);
			const Rgb b = at(u0 + 1, r0);
			const Rgb c = at(u0, r0 + 1);
			const Rgb d = at(u0 + 1, r0 + 1);
			for (int ch = 0; ch < 3; ch++) {
				const float top = a[ch] * (1 - fu) + b[ch] * fu;
				const float bottom = c[ch] * (1 - fu) + d[ch] * fu;
				out.rgb[(static_cast<size_t>(y) * out.width + x) * 3 + ch] = static_cast<uint8_t>(std::clamp(top + (bottom - top) * fr + 0.5F, 0.0F, 255.0F));
			}
		}
	}
	return true;
}

// --- Spikes --------------------------------------------------------------------------------------------------------

/**
 * A horn of a cluster of spikes as its art draws it: from where it leaves the cluster up to its point, per art row its
 * middle across (px from the column's left) and its half width (px), from the bottom up.
 */
struct SpikeHorn {
	std::vector<std::array<float, 3>> points; // x middle, row, half width
	bool growing = true;
};

/**
 * Follows the horns of a cluster of bone spikes up its piece's art: the bone (not the ash and mauve ground, 32-63)
 * from the middle of the floor diamond up, row by row, in runs across. Each horn goes on with the run nearest its
 * middle that overlaps it; a run no horn goes on into is a horn of its own, branching off the nearest horn below (or,
 * at the foot, rising from the ground). Horns shorter than SpikeMinRows are left out.
 */
constexpr int SpikeMinRows = 14;
/** The spikes' horns as tubes: as thick as this share of their art's lower third, tapering (radius ~ (1 - t)^SpikeTaper) to a point SpikeTipRows art rows past their last; those rising within SpikeRootRows of the foot are rooted under the floor. */
constexpr float SpikeThickness = 0.9F;
constexpr float SpikeTaper = 0.9F;
constexpr float SpikeTipRows = 4.0F;
constexpr float SpikeRootRows = 10.0F;
/** Their base radius at most (tiles); a branch's (one that leaves another above the ground) at most SpikeBranchShare of it. */
constexpr float SpikeMaxRadius = 0.07F;
constexpr float SpikeBranchShare = 0.7F;

std::vector<SpikeHorn> TraceSpikes(const std::vector<int16_t> &column)
{
	const int rows = static_cast<int>(column.size() / ColumnWidth);
	const int base = rows - FloorTextureSize / 2;
	auto bone = [&](int x, int y) {
		const int16_t t = column[static_cast<size_t>(y) * ColumnWidth + x];
		return t > 0 && (t < 32 || t >= 64);
	};
	std::vector<SpikeHorn> horns;
	std::vector<std::pair<int, int>> runs;
	for (int y = base; y >= 0; y--) {
		runs.clear();
		for (int x = 0; x < ColumnWidth;) {
			if (!bone(x, y)) {
				x++;
				continue;
			}
			const int from = x;
			while (x < ColumnWidth && bone(x, y))
				x++;
			runs.emplace_back(from, x - 1);
		}
		std::vector<bool> taken(runs.size(), false);
		for (SpikeHorn &horn : horns) {
			if (!horn.growing)
				continue;
			const auto &[cx, row, half] = horn.points.back();
			int best = -1;
			float bestGap = 1e9F;
			for (size_t r = 0; r < runs.size(); r++) {
				const float mid = (static_cast<float>(runs[r].first + runs[r].second)) / 2.0F;
				if (taken[r] || static_cast<float>(runs[r].second) < cx - half - 1.5F || static_cast<float>(runs[r].first) > cx + half + 1.5F)
					continue;
				if (std::abs(mid - cx) < bestGap) {
					bestGap = std::abs(mid - cx);
					best = static_cast<int>(r);
				}
			}
			if (best < 0) {
				horn.growing = false;
				continue;
			}
			taken[best] = true;
			horn.points.push_back({ (static_cast<float>(runs[best].first + runs[best].second)) / 2.0F, static_cast<float>(y), (static_cast<float>(runs[best].second - runs[best].first) + 1.0F) / 2.0F });
		}
		for (size_t r = 0; r < runs.size(); r++) {
			if (taken[r])
				continue;
			const std::array<float, 3> here { (static_cast<float>(runs[r].first + runs[r].second)) / 2.0F, static_cast<float>(y), (static_cast<float>(runs[r].second - runs[r].first) + 1.0F) / 2.0F };
			SpikeHorn branch;
			// Off the nearest horn just below, where it was a row ago.
			const SpikeHorn *parent = nullptr;
			float parentGap = 8.0F;
			for (const SpikeHorn &horn : horns) {
				if (horn.points.size() < 2 || horn.points.back()[1] != static_cast<float>(y))
					continue;
				const std::array<float, 3> &below = horn.points[horn.points.size() - 2];
				const float gap = std::abs(below[0] - here[0]);
				if (gap < parentGap) {
					parentGap = gap;
					parent = &horn;
				}
			}
			if (parent != nullptr)
				branch.points.push_back(parent->points[parent->points.size() - 2]);
			branch.points.push_back(here);
			horns.push_back(std::move(branch));
		}
	}
	std::vector<SpikeHorn> kept;
	for (SpikeHorn &horn : horns)
		if (horn.points.front()[1] - horn.points.back()[1] >= SpikeMinRows)
			kept.push_back(std::move(horn));
	return kept;
}

const std::vector<SpikeHorn> &SpikeHornsOf(uint16_t piece)
{
	static std::map<uint16_t, std::vector<SpikeHorn>> cache;
	static const std::byte *cacheFor = nullptr;
	if (cacheFor != pDungeonCels.get()) {
		cache.clear();
		cacheFor = pDungeonCels.get();
	}
	auto [it, added] = cache.try_emplace(piece);
	if (added)
		it->second = TraceSpikes(GetPieceTextures(piece).column);
	return it->second;
}

/** Per tile: 1 where a prop's bone is a model (the spikes), not a billboard. */
std::vector<uint8_t> ModelledProps;

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
				// A spike's solid piece has no floor art of its own: the plain floor's under its model.
				if (pMegaTiles != nullptr)
					FloorPieces[at] = MegatilePiece(PlainFloorTile, (x & 1) + 2 * (y & 1));
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

bool IsHellModelledProp(Point tile)
{
	return IsHell() && InDungeonBounds(tile) && !ModelledProps.empty() && ModelledProps[static_cast<size_t>(tile.y) * MAXDUNX + tile.x] != 0;
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

const HellSprite *HellSpriteAt(int index)
{
	if (index < 0 || static_cast<size_t>(index) >= Sprites.size() || Sprites[index].pixels.empty())
		return nullptr;
	return &Sprites[index];
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
	constexpr float Unseen = std::numeric_limits<float>::infinity(); // what the depth buffer starts as
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
			if (depthRow[x] != Unseen)
				continue; // a wall stands in front (DrawHellWalls draws first)
			const int tx = FloorToInt(wx);
			const int ty = FloorToInt(wy);
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
			const int cx = FloorToInt(fx);
			const int cy = FloorToInt(fy);
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
						const float lu = std::clamp(p.x / BloodTiles - static_cast<float>(FloorToInt(p.x / BloodTiles)), 0.0F, 0.9999F);
						const float lv = std::clamp(p.y / BloodTiles - static_cast<float>(FloorToInt(p.y / BloodTiles)), 0.0F, 0.9999F);
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
	// The art's cut-outs drawn on cards, made anew with the geometry.
	for (const HellSprite &sprite : Sprites)
		ForgetUpscaledTexture(sprite.pixels);
	Sprites.clear();
	Sprites.push_back(CutPostCrown()); // PostCrownSprite
	for (HellSprite &sprite : Sprites) {
		if (sprite.pixels.empty())
			continue;
		FindSpriteRects(sprite);
		sprite.fine = &UpscaledTexture(sprite.pixels, sprite.width);
	}
	// Debug: FP_DUMP_HELL_SPRITES=dir writes each cut-out as a PPM (clear: magenta).
	if (const char *dir = std::getenv("FP_DUMP_HELL_SPRITES"); dir != nullptr) {
		for (size_t i = 0; i < Sprites.size(); i++) {
			const HellSprite &sprite = Sprites[i];
			const std::string path = StrCat(dir, "/hell_sprite_", i, ".ppm");
			if (FILE *f = std::fopen(path.c_str(), "wb"); f != nullptr) {
				std::fprintf(f, "P6\n%d %d\n255\n", sprite.width, sprite.height);
				for (const int16_t texel : sprite.pixels) {
					const SDL_Color c = texel > 0 ? logical_palette[texel] : SDL_Color { 255, 0, 255, 255 };
					const uint8_t rgb[3] { c.r, c.g, c.b };
					std::fwrite(rgb, 1, 3, f);
				}
				std::fclose(f);
			}
		}
	}
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
	// Posts: at both ends of every run and evenly between, each corner once, facing out of the faces they stand on
	// (a corner's between its two). Keyed by their place (in 1/20 tiles).
	struct Post {
		P3 at;
		P3 facing;
	};
	std::map<std::pair<int, int>, Post> posts;
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
		const P3 facing = xFace ? P3 { normalAcross, 0, 0 } : P3 { 0, normalAcross, 0 };
		for (int i = 0; i <= count; i++) {
			const float s = a + length * static_cast<float>(i) / static_cast<float>(count);
			const P3 p = point(s, plane, 0);
			const std::pair<int, int> key { static_cast<int>(std::lround(p[0] * 20.0F)), static_cast<int>(std::lround(p[1] * 20.0F)) };
			auto [it, added] = posts.try_emplace(key, Post { p, { 0, 0, 0 } });
			it->second.facing = add(it->second.facing, facing);
		}
	}
	// The posts: a spine standing just out of the face, and on it the crown cut out of the art (PostCrownTile), on a
	// card facing out (or a skull knob and horns of its own, if the art has none).
	const HellSprite *crown = HellSpriteAt(PostCrownSprite);
	for (const auto &[key, post] : posts) {
		P3 facing = post.facing;
		facing[2] = 0;
		facing = unit(std::abs(facing[0]) + std::abs(facing[1]) > 1e-3F ? facing : P3 { 1, 0, 0 });
		const P3 alongDir { -facing[1], facing[0], 0 };
		const P3 at = add(post.at, mul(facing, PostOut));
		addSpine(at[0], at[1], WallTop + PostAbove, PostBody, PostJoint, alongDir, facing, 0.62F, crown == nullptr);
		if (crown == nullptr)
			continue;
		const float scale = PostCrownScale / SpritePixelsPerTileY;
		const float halfWidth = static_cast<float>(crown->width) * PostCrownScale / SpritePixelsPerTileX / 2.0F;
		const float bottom = WallTop + BeamLift - static_cast<float>(PostKnobRow - PostCrownBottom) * scale;
		const float top = bottom + static_cast<float>(crown->height) * scale;
		const P3 middle = add(at, mul(facing, PostBody + 0.02F));
		// The card: a quad for each of the crown's rectangles with art, laid where they lie on it.
		for (const auto &[rx0, ry0, rx1, ry1] : crown->rects) {
			const float u0 = static_cast<float>(rx0) / static_cast<float>(crown->width);
			const float u1 = static_cast<float>(rx1) / static_cast<float>(crown->width);
			const float v0 = static_cast<float>(ry0) / static_cast<float>(crown->height);
			const float v1 = static_cast<float>(ry1) / static_cast<float>(crown->height);
			const P3 left = add(middle, mul(alongDir, halfWidth * (2 * u0 - 1)));
			const P3 right = add(middle, mul(alongDir, halfWidth * (2 * u1 - 1)));
			const float zTop = top - (top - bottom) * v0;
			const float zBottom = top - (top - bottom) * v1;
			CleanQuad card;
			card.p = { P3 { left[0], left[1], zBottom }, P3 { right[0], right[1], zBottom }, P3 { right[0], right[1], zTop }, P3 { left[0], left[1], zTop } };
			card.normal = facing;
			card.uv = { { { u0, v1 }, { u1, v1 }, { u1, v0 }, { u0, v0 } } };
			card.surface = CleanSurface::Pillar;
			card.sprite = PostCrownSprite;
			card.spriteWidth = 2.0F * halfWidth;
			quads.push_back(card);
		}
	}
	// The spikes: each horn its art draws a tapering tube, standing where it stands in the art across the view (the
	// iso camera's screen runs along x - y) and leaning a little towards it and away by turns.
	ModelledProps.assign(static_cast<size_t>(MAXDUNX) * MAXDUNY, 0);
	int spikes = 0;
	for (int y = 0; y < MAXDUNY; y++) {
		for (int x = 0; x < MAXDUNX; x++) {
			if (kinds[static_cast<size_t>(y) * MAXDUNX + x] != CleanKind::Prop || ClassOf(MegatileAt({ x, y })) != Megatile::Spike)
				continue;
			const std::vector<SpikeHorn> &horns = SpikeHornsOf(dPiece[x][y]);
			if (horns.empty())
				continue;
			const int rows = static_cast<int>(GetPieceTextures(dPiece[x][y]).column.size() / ColumnWidth);
			const float base = static_cast<float>(rows - FloorTextureSize / 2);
			const P3 across { 0.70710678F, -0.70710678F, 0 }; // screen x
			const P3 toward { 0.70710678F, 0.70710678F, 0 };  // towards the iso camera
			const P3 centre { static_cast<float>(x) + 0.5F, static_cast<float>(y) + 0.5F, 0 };
			int k = 0;
			for (const SpikeHorn &horn : horns) {
				const float lean = ((k++ & 1) == 0 ? 0.25F : -0.25F);
				auto at = [&](float px, float row) {
					const float z = (base - row) / SpritePixelsPerTileY * ArtScale;
					const float side = (px - ColumnWidth / 2.0F) / SpritePixelsPerTileX * ArtScale;
					return add(add(centre, mul(across, side)), add(mul(toward, lean * z), P3 { 0, 0, z }));
				};
				// Its middle line smoothed (the art's runs wobble), its thickness that of its lower third, tapering to a
				// point SpikeTipRows past its last row, on along its last stretch.
				const size_t n = horn.points.size();
				std::vector<float> middle(n);
				for (size_t i = 0; i < n; i++) {
					float sum = 0;
					int count = 0;
					for (size_t j = i >= 2 ? i - 2 : 0; j <= std::min(n - 1, i + 2); j++, count++)
						sum += horn.points[j][0];
					middle[i] = sum / static_cast<float>(count);
				}
				float thick = 0;
				const size_t lower = std::max<size_t>(1, n / 3);
				for (size_t i = 0; i < lower; i++)
					thick += horn.points[i][2];
				// Rooted in the ground (it rises from the foot of the cluster), or a branch, thinner.
				const bool rooted = base - horn.points.front()[1] <= SpikeRootRows;
				const float baseRadius = std::clamp(thick / static_cast<float>(lower) * SpikeThickness / SpritePixelsPerTileX * ArtScale, 0.02F, SpikeMaxRadius * (rooted ? 1.0F : SpikeBranchShare));
				const float length = static_cast<float>(n - 1) + SpikeTipRows;
				std::vector<P3> path;
				std::vector<float> radius;
				// A rooted one starts a little under the floor.
				if (rooted) {
					P3 root = at(middle.front(), horn.points.front()[1]);
					root[2] = -0.06F;
					path.push_back(root);
					radius.push_back(baseRadius * 1.15F);
				}
				const size_t step = std::max<size_t>(1, n / 7);
				for (size_t i = 0; i + 1 < n; i += step) {
					path.push_back(at(middle[i], horn.points[i][1]));
					radius.push_back(baseRadius * std::pow(1.0F - static_cast<float>(i) / length, SpikeTaper));
				}
				path.push_back(at(middle[n - 1], horn.points[n - 1][1]));
				radius.push_back(baseRadius * std::pow(SpikeTipRows / length, SpikeTaper));
				const size_t back = std::min<size_t>(4, n - 1);
				const float dxPerRow = back > 0 ? (middle[n - 1] - middle[n - 1 - back]) / static_cast<float>(back) : 0.0F;
				path.push_back(at(middle[n - 1] + dxPerRow * SpikeTipRows, horn.points[n - 1][1] - SpikeTipRows));
				radius.push_back(0.0F);
				addTube(path, radius, 6, CleanSurface::Stone, false, false);
			}
			ModelledProps[static_cast<size_t>(y) * MAXDUNX + x] = 1;
			spikes++;
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
	LogInfo("fp: hell geometry: {} quads, {} beams, {} posts, {} spine columns, {} spikes", quads.size(), beams, posts.size(), columns, spikes);
}

} // namespace devilution::fp
