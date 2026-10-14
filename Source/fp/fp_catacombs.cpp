/**
 * @file fp/fp_catacombs.cpp
 *
 * The catacombs in first person (see fp_catacombs.hpp): their tiles, the art cut out for their walls, floors and
 * ceiling, and their floor and ceiling pass.
 */
#include "fp/fp_catacombs.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

#include "engine/palette.h"
#include "fp/fp_buildings.hpp"
#include "fp/fp_doors.hpp"
#include "fp/fp_textures.hpp"
#include "fp/fp_world.hpp"
#include "interfac.h"
#include "levels/trigs.h"
#include "objects.h"
#include "levels/dun_tile.hpp"
#include "levels/dun_tile_data.hpp"
#include "levels/gendung.h"
#include "levels/tile_properties.hpp"
#include "utils/endian_swap.hpp"
#include "utils/is_of.hpp"
#include "utils/log.hpp"

namespace devilution::fp {

namespace {

// --- The tiles of the catacombs' map (`dungeon`, ids from 1 as in levels/l2data/l2.til) ---------------------------

/** The plain floor: brown cobbles. */
constexpr int PlainFloorTile = 3;
/** The plain walls: along y (its top and left pieces, their faces looking towards +x), and along x (top and right). */
constexpr int WallAlongYTile = 1;
constexpr int WallAlongXTile = 2;
/** The rock between the rooms, and past the map's edge (DRLG_LPass3's fill). */
constexpr int SolidTile = 12;
/** A lone tower: the flagstones of its cap are the ceiling's. */
constexpr int LoneTowerTile = 6;

/**
 * Map tiles whose top piece is a round tower: at the end of a wall (7, 9), in an inside corner (8, 38, 87), alone (6),
 * carrying an arch (39-43), behind a wall where the iso shows only its cap (13-16, 146-149), at the stairs (77, 78,
 * 160), crumbled (83), joined to two walls (55).
 */
bool IsTowerTile(int id)
{
	switch (id) {
	case 6: case 7: case 8: case 9: case 13: case 14: case 15: case 16: case 38: case 39: case 40: case 41: case 42:
	case 43: case 55: case 77: case 78: case 83: case 87: case 146: case 147: case 148: case 149: case 160:
		return true;
	default:
		return false;
	}
}

/** Floor tiles with the shadow of a tower or an arch painted on (ApplyShadowsPatterns, the arch minisets), or a flight of stairs (meshes now). */
bool IsShadowTile(int id)
{
	return (id >= 45 && id <= 51) || id == 71 || id == 72 || id == 76 || id == 77 || id == 78 || (id >= 158 && id <= 160);
}

/** The stairs: up (and to town) a spiral of steps round a tower (USTAIRS, WARPSTAIRS: the towers' map tiles), down a trench beside a tower (DSTAIRS: the solid pieces of its two map tiles beside the tower). */
bool IsSpiralStairsTile(int id)
{
	return id == 77 || id == 160;
}
bool IsStairsDownTile(int id)
{
	return id == 71 || id == 78;
}

/** The wooden scaffolds against the walls (BIG3, BIG4): their walls' art and their front tiles (see AddCatacombsGeometry). */
bool IsScaffoldTile(int id)
{
	return id >= 116 && id <= 119;
}

/** The map tile (`dungeon`) a tile of the dPiece grid belongs to; the rock past the map's edge. */
int MegatileAt(Point tile)
{
	if (tile.x < 16 || tile.y < 16)
		return SolidTile;
	const int mx = (tile.x - 16) / 2;
	const int my = (tile.y - 16) / 2;
	if (mx >= DMAXX || my >= DMAXY)
		return SolidTile;
	return dungeon[mx][my];
}

/** The piece a map tile puts at a quadrant of its 2 x 2 (0 top, 1 right, 2 left, 3 bottom; see DRLG_LPass3). */
uint16_t MegatilePiece(int id, int quadrant)
{
	const MegaTile &mega = pMegaTiles[id - 1];
	const std::array<uint16_t, 4> micros { mega.micro1, mega.micro2, mega.micro3, mega.micro4 };
	return Swap16LE(micros[quadrant]);
}

int QuadrantOf(Point tile)
{
	return (tile.x & 1) + 2 * (tile.y & 1);
}

enum : uint8_t {
	TileOwnFaces = 1,    // a wall whose faces the iso drew wear its own art
	TileTower = 2,       // a tower's tile (fp_catacombs' geometry)
	TileTowerCorner = 4, // a tower in an inside corner: its tile stays a wall, the tower stands on its corner
};

std::vector<uint8_t> Flags;        // per tile, y * MAXDUNX + x
std::vector<uint16_t> FloorPieces; // per tile: the piece whose floor art it shows

std::vector<Tower> Towers;
/** The towers whose circle (with its cap's rim) reaches into each tile: TowerList[TowerStart[t] .. TowerStart[t + 1]). */
std::vector<uint32_t> TowerStart;
std::vector<uint16_t> TowerList;

// --- Art -----------------------------------------------------------------------------------------------------------

/**
 * The near-blacks 112-127 come out red through the light tables (fp_buildings.cpp's PatchColor): the art's are swapped
 * for the nearest ordinary colour when it is cut.
 */
uint8_t SafeColor(int16_t texel)
{
	static std::array<uint8_t, 256> safe {};
	static uint32_t readyFor = 0;
	static bool ready = false;
	if (!ready || readyFor != StablePaletteVersion()) {
		ready = true;
		readyFor = StablePaletteVersion();
		for (int i = 0; i < 256; i++) {
			safe[i] = static_cast<uint8_t>(i);
			if (i < 112 || i > 127)
				continue;
			const SDL_Color c = logical_palette[i];
			int best = std::numeric_limits<int>::max();
			for (int j = 1; j < 255; j++) {
				if (j >= 112 && j < 160)
					continue;
				const SDL_Color o = logical_palette[j];
				const int d = (o.r - c.r) * (o.r - c.r) + (o.g - c.g) * (o.g - c.g) + (o.b - c.b) * (o.b - c.b);
				if (d < best) {
					best = d;
					safe[i] = static_cast<uint8_t>(j);
				}
			}
		}
	}
	return texel <= 0 ? uint8_t { 0 } : safe[static_cast<uint8_t>(texel)];
}

/** Fills the texels of a piece of art that have none: from the nearest drawn one up or down its column, else black. */
void FillColumns(std::vector<int16_t> &art, int width)
{
	const int height = static_cast<int>(art.size()) / width;
	for (int u = 0; u < width; u++) {
		for (int v = 0; v < height; v++) {
			int16_t &texel = art[static_cast<size_t>(v) * width + u];
			if (texel != NoPixel)
				continue;
			for (int r = 1; r < height && texel == NoPixel; r++) {
				if (v - r >= 0 && art[static_cast<size_t>(v - r) * width + u] != NoPixel)
					texel = art[static_cast<size_t>(v - r) * width + u];
				else if (v + r < height && art[static_cast<size_t>(v + r) * width + u] != NoPixel)
					texel = art[static_cast<size_t>(v + r) * width + u];
			}
			if (texel == NoPixel)
				texel = 0;
		}
	}
}

/** Makes art TextureUpscale times finer with the game's xBR (UpscaledTexture), its near-blacks swapped. */
std::vector<int16_t> Finer(std::vector<int16_t> art, int width)
{
	for (int16_t &texel : art)
		texel = SafeColor(texel);
	std::vector<int16_t> fine = UpscaledTexture(art, width);
	ForgetUpscaledTexture(art);
	return fine;
}

/** An ArtTexture of palette art (`width` a power of two), repeating every `tilesAcross`, `tilesDown` tall. */
void BuildArt(ArtTexture &texture, const std::vector<int16_t> &art, int width, float tilesAcross, float tilesDown)
{
	const int height = static_cast<int>(art.size()) / width;
	texture.mips.clear();
	texture.texelsPerTileU = static_cast<float>(width) / tilesAcross;
	texture.texelsPerTileV = static_cast<float>(height) / tilesDown;
	ArtTexture::Mip first;
	first.width = width;
	first.height = height;
	while ((1 << first.shift) < width)
		first.shift++;
	first.texels.resize(art.size());
	std::vector<std::array<float, 3>> rgb(art.size());
	for (size_t i = 0; i < art.size(); i++) {
		first.texels[i] = art[i] <= 0 ? uint8_t { 0 } : static_cast<uint8_t>(art[i]);
		const SDL_Color c = logical_palette[first.texels[i]];
		rgb[i] = { static_cast<float>(c.r), static_cast<float>(c.g), static_cast<float>(c.b) };
	}
	texture.mips.push_back(std::move(first));
	// Detail levels: each 2 x 2 texels of the one before averaged, as the nearest palette colour, down to 4 a side.
	while (texture.mips.back().width > 4 && texture.mips.back().height > 4 && texture.mips.size() < 7) {
		const ArtTexture::Mip &above = texture.mips.back();
		ArtTexture::Mip mip;
		mip.width = above.width / 2;
		mip.height = above.height / 2;
		mip.shift = above.shift - 1;
		mip.texels.resize(static_cast<size_t>(mip.width) * mip.height);
		std::vector<std::array<float, 3>> next(mip.texels.size());
		for (int y = 0; y < mip.height; y++) {
			for (int x = 0; x < mip.width; x++) {
				std::array<float, 3> sum {};
				for (int dy = 0; dy < 2; dy++)
					for (int dx = 0; dx < 2; dx++)
						for (int c = 0; c < 3; c++)
							sum[c] += rgb[static_cast<size_t>(y * 2 + dy) * above.width + x * 2 + dx][c] * 0.25F;
				const size_t at = static_cast<size_t>(y) * mip.width + x;
				next[at] = sum;
				mip.texels[at] = CleanNearestColor(static_cast<int>(sum[0] + 0.5F), static_cast<int>(sum[1] + 0.5F), static_cast<int>(sum[2] + 0.5F));
			}
		}
		rgb = std::move(next);
		texture.mips.push_back(std::move(mip));
	}
}

/** The level's cut art, made anew when its graphics or its palette change. */
struct ArtCache {
	const std::byte *cels = nullptr;
	uint32_t palette = 0;
	std::unordered_map<uint32_t, ArtTexture> faces;  // by piece * 2 + (faceX ? 1 : 0)
	std::unordered_map<uint16_t, ArtTexture> floors; // by piece
};
ArtCache Art;

void EnsureArtCache()
{
	if (Art.cels == pDungeonCels.get() && Art.palette == StablePaletteVersion())
		return;
	Art.cels = pDungeonCels.get();
	Art.palette = StablePaletteVersion();
	Art.faces.clear();
	Art.floors.clear();
}

/**
 * A piece's wall face (faceX: the face along y, else along x) as a wall shows it: the rows from its foot on the floor
 * up to under its cap (WallShownRows), xBR'd.
 */
const ArtTexture &FaceTexture(uint16_t piece, bool faceX)
{
	EnsureArtCache();
	auto [it, added] = Art.faces.try_emplace(static_cast<uint32_t>(piece) * 2 + (faceX ? 1 : 0));
	if (!added)
		return it->second;
	const PieceTextures &textures = GetPieceTextures(piece);
	const std::vector<int16_t> &face = faceX ? textures.faceX : textures.faceY;
	const int faceHeight = FaceTextureHeight();
	const int shown = std::clamp(WallShownRows(LevelWallRows()), 1, faceHeight);
	std::vector<int16_t> rows(face.end() - static_cast<ptrdiff_t>(shown) * FaceTextureWidth, face.end());
	FillColumns(rows, FaceTextureWidth);
	BuildArt(it->second, Finer(std::move(rows), FaceTextureWidth), FaceTextureWidth * TextureUpscale, 1.0F, CleanCeiling());
	return it->second;
}

/** A piece's floor art (one tile), xBR'd. */
const ArtTexture &FloorTexture(uint16_t piece)
{
	EnsureArtCache();
	auto [it, added] = Art.floors.try_emplace(piece);
	if (!added)
		return it->second;
	std::vector<int16_t> floor = GetPieceTextures(piece).floor;
	if (floor.size() != static_cast<size_t>(FloorTextureSize) * FloorTextureSize)
		floor.assign(static_cast<size_t>(FloorTextureSize) * FloorTextureSize, 0);
	FillColumns(floor, FloorTextureSize);
	BuildArt(it->second, Finer(std::move(floor), FloorTextureSize), FloorTextureSize * TextureUpscale, 1.0F, 1.0F);
	return it->second;
}

/** The plain wall pair's piece at a place along a wall: along y (faces towards x) by y, else by x. */
uint16_t PlainWallPiece(Point tile, bool alongY)
{
	return alongY ? MegatilePiece(WallAlongYTile, (tile.y & 1) != 0 ? 2 : 0) : MegatilePiece(WallAlongXTile, (tile.x & 1) != 0 ? 1 : 0);
}

/** An RGB source texture of palette art (for the texture set's slots), its palette colours kept. */
void ToSource(const std::vector<int16_t> &art, int width, float tilesAcross, float tilesDown, CleanSourceTexture &out)
{
	out.width = width;
	out.height = static_cast<int>(art.size()) / width;
	out.tilesAcross = tilesAcross;
	out.tilesDown = tilesDown;
	out.indices.resize(art.size());
	out.rgb.resize(art.size() * 3);
	for (size_t i = 0; i < art.size(); i++) {
		const uint8_t index = art[i] <= 0 ? uint8_t { 0 } : static_cast<uint8_t>(art[i]);
		out.indices[i] = index;
		const SDL_Color c = logical_palette[index];
		out.rgb[i * 3] = c.r;
		out.rgb[i * 3 + 1] = c.g;
		out.rgb[i * 3 + 2] = c.b;
	}
}

/** Stretches art down to `rows` rows (nearest), so a slot's texture is a power of two tall. */
std::vector<int16_t> Rows(const std::vector<int16_t> &art, int width, int rows)
{
	const int height = static_cast<int>(art.size()) / width;
	std::vector<int16_t> out(static_cast<size_t>(width) * rows);
	for (int y = 0; y < rows; y++) {
		const int sy = std::min(height - 1, y * height / rows);
		std::copy_n(&art[static_cast<size_t>(sy) * width], width, &out[static_cast<size_t>(y) * width]);
	}
	return out;
}

/**
 * The flagstones of a lone tower's cap: the square inside its disc, unrotated (FlagstoneFrom .. 1 - FlagstoneFrom of a
 * tile each way, as 32 texels), laid four times, mirrored, so it repeats without a seam every two tiles.
 */
constexpr float FlagstoneFrom = 0.08F;

std::vector<int16_t> CutFlagstones()
{
	constexpr int N = FloorTextureSize;
	std::vector<int16_t> square(static_cast<size_t>(N) * N, NoPixel);
	const std::vector<int16_t> &column = GetPieceTextures(MegatilePiece(LoneTowerTile, 0)).column;
	const int rows = static_cast<int>(column.size() / ColumnWidth);
	int top = -1;
	for (int y = 0; y < rows && top < 0; y++)
		for (int x = 0; x < ColumnWidth; x++)
			if (column[static_cast<size_t>(y) * ColumnWidth + x] != NoPixel) {
				top = y - 1; // the row of the cap's diamond's top corner, just above its disc
				break;
			}
	if (top >= 0) {
		for (int v = 0; v < N; v++) {
			for (int u = 0; u < N; u++) {
				const float fx = FlagstoneFrom + (1.0F - 2.0F * FlagstoneFrom) * (static_cast<float>(u) + 0.5F) / N;
				const float fy = FlagstoneFrom + (1.0F - 2.0F * FlagstoneFrom) * (static_cast<float>(v) + 0.5F) / N;
				const int sx = std::clamp(static_cast<int>(ColumnWidth / 2 + (fx - fy) * ColumnWidth / 2), 0, ColumnWidth - 1);
				const int sy = std::clamp(static_cast<int>(static_cast<float>(top) + (fx + fy) * FloorTextureSize / 2), 0, rows - 1);
				square[static_cast<size_t>(v) * N + u] = column[static_cast<size_t>(sy) * ColumnWidth + sx];
			}
		}
	}
	FillColumns(square, N);
	std::vector<int16_t> tiled(static_cast<size_t>(4) * N * N);
	for (int v = 0; v < 2 * N; v++)
		for (int u = 0; u < 2 * N; u++)
			tiled[static_cast<size_t>(v) * 2 * N + u] = square[static_cast<size_t>(v < N ? v : 2 * N - 1 - v) * N + (u < N ? u : 2 * N - 1 - u)];
	return tiled;
}

/** The plain wall along y, two tiles of it (its pair), as the shown rows of their faces. */
std::vector<int16_t> PlainWallStrip()
{
	const int faceHeight = FaceTextureHeight();
	const int shown = std::clamp(WallShownRows(LevelWallRows()), 1, faceHeight);
	std::vector<int16_t> strip(static_cast<size_t>(2) * FaceTextureWidth * shown);
	for (int k = 0; k < 2; k++) {
		const std::vector<int16_t> &face = GetPieceTextures(MegatilePiece(WallAlongYTile, k == 0 ? 0 : 2)).faceX;
		for (int v = 0; v < shown; v++)
			for (int u = 0; u < FaceTextureWidth; u++)
				strip[static_cast<size_t>(v) * 2 * FaceTextureWidth + k * FaceTextureWidth + u] = face[static_cast<size_t>(faceHeight - shown + v) * FaceTextureWidth + u];
	}
	FillColumns(strip, 2 * FaceTextureWidth);
	return strip;
}

/** The plain floor's map tile put together from its four pieces' floor art: 64 x 64 texels over 2 x 2 tiles. */
std::vector<int16_t> PlainFloor()
{
	constexpr int N = FloorTextureSize;
	std::vector<int16_t> floor(static_cast<size_t>(4) * N * N, 0);
	for (int quadrant = 0; quadrant < 4; quadrant++) {
		std::vector<int16_t> art = GetPieceTextures(MegatilePiece(PlainFloorTile, quadrant)).floor;
		if (art.size() != static_cast<size_t>(N) * N)
			continue;
		FillColumns(art, N);
		const int ox = (quadrant & 1) * N;
		const int oy = (quadrant >> 1) * N;
		for (int v = 0; v < N; v++)
			for (int u = 0; u < N; u++)
				floor[static_cast<size_t>(oy + v) * 2 * N + ox + u] = art[static_cast<size_t>(v) * N + u];
	}
	return floor;
}

// --- Doors ---------------------------------------------------------------------------------------------------------

/**
 * The doorways: flat-topped, as wide and tall as the leaf the iso draws in them (OBJ_L2LDOOR's closed frame: 21 px of
 * a 32 px tile edge across, 56 rows of 39.2 a tile tall, stretched like the walls), its sides a little wider.
 */
constexpr float DoorHalfWidth = 0.35F;
constexpr float DoorHeight = 2.15F;
/**
 * The leaf's art: the closed left door's frame, unsheared, from below the shadow along its top (DoorShadowRows) down;
 * its iron from the open frame's hinge straps (DoorIronFrame: its bluish dark texels), laid on as two straps from the
 * hinge DoorStrapReach across, DoorStrapRows tall, at DoorStraps of its height.
 */
constexpr int DoorShadowRows = 8;
constexpr int DoorIronFrame = 2;
constexpr float DoorStrapReach = 0.55F;
constexpr int DoorStrapRows = 3;
constexpr std::array<float, 2> DoorStraps { 0.22F, 0.74F };

/** The left door's sprites (closed, and the open frame with its hinges), from a door of the level (none if it has none). */
std::optional<std::pair<ClxSprite, ClxSprite>> DoorSprites()
{
	for (int i = 0; i < ActiveObjectCount; i++) {
		const Object &object = Objects[ActiveObjects[i]];
		if (object._otype != OBJ_L2LDOOR || !object._oAnimData || object._oAnimData->numSprites() <= DoorIronFrame)
			continue;
		return std::pair { (*object._oAnimData)[0], (*object._oAnimData)[DoorIronFrame] };
	}
	return std::nullopt;
}

/** The leaf (width x height texels, row 0 its top), its straps on; false if the level has no doors. */
bool CutDoorLeaf(std::vector<int16_t> &leaf, int &width, int &height)
{
	const std::optional<std::pair<ClxSprite, ClxSprite>> sprites = DoorSprites();
	if (!sprites)
		return false;
	const auto [closed, open] = *sprites;
	std::vector<int16_t> pixels;
	DecodeClx(closed, pixels);
	const int w = closed.width();
	const int h = closed.height();
	// Its columns, and where each starts (the face is sheared up a row every two across).
	std::vector<int> columns;
	std::vector<int> tops;
	for (int x = 0; x < w; x++) {
		for (int y = 0; y < h; y++) {
			if (pixels[static_cast<size_t>(y) * w + x] != NoPixel) {
				columns.push_back(x);
				tops.push_back(y);
				break;
			}
		}
	}
	if (columns.size() < 8)
		return false;
	int rows = h;
	for (size_t k = 0; k < columns.size(); k++) {
		int last = tops[k];
		while (last + 1 < h && pixels[static_cast<size_t>(last + 1) * w + columns[k]] != NoPixel)
			last++;
		rows = std::min(rows, last - tops[k] + 1 - DoorShadowRows);
	}
	if (rows < 16)
		return false;
	width = static_cast<int>(columns.size());
	height = rows;
	leaf.assign(static_cast<size_t>(width) * height, NoPixel);
	for (int u = 0; u < width; u++)
		for (int v = 0; v < height; v++)
			leaf[static_cast<size_t>(v) * width + u] = pixels[static_cast<size_t>(tops[u] + DoorShadowRows + v) * w + columns[u]];
	// The hinges' iron: the open frame's bluish dark texels, lightest first.
	std::vector<int16_t> iron;
	DecodeClx(open, pixels);
	for (const int16_t texel : pixels) {
		if (texel <= 0)
			continue;
		const SDL_Color c = logical_palette[texel];
		if (c.b > c.r + 6 && c.r + c.g + c.b < 300)
			iron.push_back(texel);
	}
	if (!iron.empty()) {
		std::sort(iron.begin(), iron.end(), [](int16_t a, int16_t b) {
			const SDL_Color p = logical_palette[a];
			const SDL_Color q = logical_palette[b];
			return p.r + p.g + p.b > q.r + q.g + q.b;
		});
		for (const float at : DoorStraps) {
			const int top = std::clamp(static_cast<int>(at * static_cast<float>(height)), 0, height - DoorStrapRows);
			const int reach = std::max(1, static_cast<int>(DoorStrapReach * static_cast<float>(width)));
			for (int r = 0; r < DoorStrapRows; r++) {
				// Lit along its top, in shadow along its bottom; mottled along it as the art's iron is.
				const size_t band = iron.size() * static_cast<size_t>(r) / DoorStrapRows;
				const size_t bandSize = std::max<size_t>(1, iron.size() / DoorStrapRows);
				for (int u = 0; u < reach; u++)
					leaf[static_cast<size_t>(top + r) * width + u] = iron[std::min(iron.size() - 1, band + static_cast<size_t>(u * 7 + r * 3) % bandSize)];
			}
		}
	}
	FillColumns(leaf, width);
	return true;
}

/**
 * The insides of the doorways (their jambs and the underside of their lintel): the dressed stone of the left door
 * piece's jamb (map tile 4's top piece, its face along x), a patch of it RevealPatch texels square from
 * RevealFrom rows above its foot, laid four times mirrored.
 */
constexpr int RevealPatch = 16;
constexpr int RevealFrom = 6;

std::vector<int16_t> CutReveal()
{
	constexpr int N = RevealPatch;
	const std::vector<int16_t> &face = GetPieceTextures(MegatilePiece(4, 0)).faceY;
	const int faceHeight = FaceTextureHeight();
	std::vector<int16_t> patch(static_cast<size_t>(N) * N, NoPixel);
	for (int v = 0; v < N; v++) {
		const int row = faceHeight - 1 - FaceBaseRows - RevealFrom - (N - 1 - v);
		for (int u = 0; u < N; u++)
			if (row >= 0 && static_cast<size_t>(row) * FaceTextureWidth + u + 2 < face.size())
				patch[static_cast<size_t>(v) * N + u] = face[static_cast<size_t>(row) * FaceTextureWidth + u + 2];
	}
	FillColumns(patch, N);
	std::vector<int16_t> tiled(static_cast<size_t>(4) * N * N);
	for (int v = 0; v < 2 * N; v++)
		for (int u = 0; u < 2 * N; u++)
			tiled[static_cast<size_t>(v) * 2 * N + u] = patch[static_cast<size_t>(v < N ? v : 2 * N - 1 - v) * N + (u < N ? u : 2 * N - 1 - u)];
	return tiled;
}

// --- Arches --------------------------------------------------------------------------------------------------------

/**
 * The arches (the special arch cels, from a tower to a wall or another tower): as the art draws them, a flat beam
 * ArchBeamDepth deep under the ceiling, its underside curving down at both ends into its supports over ArchHaunch
 * (a quarter circle), ArchHalfThickness either side of the middle of their tiles.
 */
constexpr float ArchBeamDepth = 0.6F;
constexpr float ArchHaunch = 0.6F;
constexpr float ArchHalfThickness = 0.3F;

// --- Stairs --------------------------------------------------------------------------------------------------------

/**
 * The stairs up: StairsSteps solid steps of stone winding round their tower (out from its drum StairsWidth) from the
 * side of their trigger up to the ceiling over StairsTurn, the ceiling open above the last StairsOpenSteps of them.
 */
constexpr int StairsSteps = 16;
constexpr float StairsWidth = 0.42F;
constexpr float StairsTurn = 5.236F; // 300 degrees
constexpr int StairsOpenSteps = 5;
/**
 * The stairs down: steps going down the trench beside their tower from the end by their trigger, PitSteps of them
 * down to PitDepth (drawn tiles) at its far end, which goes on under the floor into the dark; the trench's sides of
 * the walls' stone, PitBands bands a light level darker each going down.
 */
constexpr int PitSteps = 8;
constexpr float PitDepth = 1.6F;
constexpr int PitBands = 4;
constexpr float PitInset = 0.05F; // the trench's sides, in from its tiles' edges

/** A part of the ceiling open over the stairs up: a ring between r0 and r1 round (cx, cy), from angle a0 over `span`. */
struct CeilingHole {
	float cx, cy, r0, r1, a0, span;
};
std::vector<CeilingHole> CeilingHoles;

/** Whether a point of the ceiling is open over the stairs. */
bool IsCeilingHole(float x, float y)
{
	for (const CeilingHole &hole : CeilingHoles) {
		const float dx = x - hole.cx;
		const float dy = y - hole.cy;
		if (std::abs(dx) > hole.r1 || std::abs(dy) > hole.r1)
			continue;
		const float d2 = dx * dx + dy * dy;
		if (d2 > hole.r1 * hole.r1)
			continue;
		float a = std::atan2(dy, dx) - hole.a0;
		a -= 6.2831853F * std::floor(a / 6.2831853F);
		if (a <= hole.span)
			return true;
	}
	return false;
}

// --- Scaffolds -----------------------------------------------------------------------------------------------------

/**
 * The wooden scaffolds against the walls (BIG3, BIG4: map tiles 116-119), their front pieces solid but seen past: on
 * 118 and 119 a deck of planks ScaffoldDeckOut out from the wall at ScaffoldDeckHeight on posts, braced; on 116 and 117
 * fallen to pieces, a broken post and beams leaning on the wall or lying on the floor. Their timbers ScaffoldBeam
 * thick, of the deck's planks cut from the art (map tile 118's top piece: WoodFrom, its planks running down a row every
 * two across), WoodTexels long, WoodRows across, laid twice, mirrored.
 */
constexpr float ScaffoldDeckOut = 0.8F;
constexpr float ScaffoldDeckHeight = 1.0F;
constexpr float ScaffoldDeck = 0.08F;
constexpr float ScaffoldBeam = 0.045F;
constexpr int WoodTile = 118;
constexpr std::array<int, 2> WoodFrom { 32, 98 };
constexpr int WoodTexels = 32;
constexpr int WoodRows = 16;

std::vector<int16_t> CutWood()
{
	std::vector<int16_t> patch(static_cast<size_t>(WoodTexels) * WoodRows, NoPixel);
	const std::vector<int16_t> &column = GetPieceTextures(MegatilePiece(WoodTile, 0)).column;
	const int rows = static_cast<int>(column.size() / ColumnWidth);
	for (int v = 0; v < WoodRows; v++) {
		for (int u = 0; u < WoodTexels; u++) {
			const int sx = WoodFrom[0] + u;
			const int sy = WoodFrom[1] + v + u / 2;
			if (sx < ColumnWidth && sy < rows)
				patch[static_cast<size_t>(v) * WoodTexels + u] = column[static_cast<size_t>(sy) * ColumnWidth + sx];
		}
	}
	FillColumns(patch, WoodTexels);
	std::vector<int16_t> tiled(static_cast<size_t>(WoodTexels) * WoodRows * 2);
	for (int v = 0; v < 2 * WoodRows; v++)
		for (int u = 0; u < WoodTexels; u++)
			tiled[static_cast<size_t>(v) * WoodTexels + u] = patch[static_cast<size_t>(v < WoodRows ? v : 2 * WoodRows - 1 - v) * WoodTexels + u];
	return tiled;
}

// --- Wall torches --------------------------------------------------------------------------------------------------

/**
 * The wall torches (OBJ_TORCHL, R, L2, R2), meshes in the design of their sprites (wtorch1-4), only the flame a sprite
 * (its animation, cut out of their frames: what changes between them). Each is built in the frame of its wall's face
 * (WallTorchPlacement): out of the wall (n), along it to the right of one facing it (a), up (z); drawn tiles.
 * L2 and R2, on the wall tile (wtorch1, 2): an iron plate on the wall (TorchPlate*), a handle from its middle out and up
 * to a cup holding the flame (TorchHandle*, TorchCup*). L and R, on the floor before their wall (wtorch3, 4): a pole
 * leaning along the wall as the art draws it, its top out from the wall (TorchPole*), held by an iron ring on an arm.
 */
constexpr float TorchPlateHalfWidth = 0.17F;
constexpr float TorchPlateHalfHeight = 0.21F;
constexpr float TorchPlateDepth = 0.03F;
constexpr float TorchPlateHeight = 1.65F;
constexpr std::array<float, 3> TorchHandleTip { 0.3F, 0.0F, 2.15F }; // n, a, z
constexpr float TorchHandleRadius = 0.032F;
constexpr float TorchCupRadius = 0.07F;
constexpr float TorchCupHeight = 0.08F;
constexpr std::array<float, 3> TorchPoleFoot { 0.07F, -0.25F, 1.2F }; // n, a (towards the side the art's top leans away from), z
constexpr std::array<float, 3> TorchPoleTop { 0.3F, 0.2F, 2.15F };
constexpr float TorchPoleRadius = 0.038F;
constexpr float TorchRingAt = 0.35F; // along the pole
constexpr int TorchSides = 6;
/** The flame: its art at FlameScale drawn tiles per tile of art (SpritePixelsPerTileX across, ...Y up), its foot that far into the cup. */
constexpr float FlameScale = 0.9F;
constexpr float FlameSink = 0.04F;
/**
 * Where the wood is cut from (the sprites' frames as drawn, x and row): the handle of OBJ_TORCHR2's (wtorch2) from the
 * plate to its tip, the pole of OBJ_TORCHR's (wtorch3) from its foot to its top; and the plate of OBJ_TORCHL2's
 * (wtorch1): its columns, its rows below the top of its outline.
 */
constexpr std::array<float, 4> HandleArt { 55.0F, 70.0F, 41.0F, 57.0F };
constexpr std::array<float, 4> PoleArt { 22.0F, 88.0F, 44.0F, 50.0F };
constexpr std::array<int, 2> PlateArtColumns { 35, 46 };
constexpr int PlateArtRows = 13;

bool IsWallTorch(int type)
{
	return IsAnyOf(type, OBJ_TORCHL, OBJ_TORCHR, OBJ_TORCHL2, OBJ_TORCHR2);
}

/** A torch's sprites, and which of their pixels the frames share (the iron and wood: the rest is the flame). */
struct TorchArt {
	int width = 0;
	int height = 0;
	std::vector<int16_t> still; // the shared pixels, NoPixel elsewhere
	std::vector<std::vector<int16_t>> flames; // per frame: the flame (all but the shared pixels), cropped to flameRect
	std::array<int, 4> flameRect {}; // x0, y0, x1, y1 (ends exclusive) of all the frames' flames
};

const TorchArt *TorchArtFor(const Object &object)
{
	static std::unordered_map<const void *, TorchArt> cache;
	static const std::byte *cacheCels = nullptr;
	static int cacheLevel = -1;
	if (cacheCels != pDungeonCels.get() || cacheLevel != currlevel) {
		cacheCels = pDungeonCels.get();
		cacheLevel = currlevel;
		cache.clear();
	}
	if (!object._oAnimData || object._oAnimData->numSprites() < 2)
		return nullptr;
	const OptionalClxSpriteList &list = object._oAnimData;
	auto [it, added] = cache.try_emplace(static_cast<const void *>(list->data()));
	TorchArt &art = it->second;
	if (!added)
		return art.width > 0 ? &art : nullptr;
	const int frames = static_cast<int>(list->numSprites());
	std::vector<std::vector<int16_t>> pixels(static_cast<size_t>(frames));
	art.width = (*list)[0].width();
	art.height = (*list)[0].height();
	for (int i = 0; i < frames; i++) {
		const ClxSprite frame = (*list)[i];
		if (frame.width() != art.width || frame.height() != art.height) {
			art.width = 0;
			return nullptr;
		}
		DecodeClx(frame, pixels[i]);
	}
	const size_t count = static_cast<size_t>(art.width) * art.height;
	art.still.assign(count, NoPixel);
	std::vector<uint8_t> moving(count, 0);
	for (size_t at = 0; at < count; at++) {
		bool same = true;
		for (int i = 1; i < frames && same; i++)
			same = pixels[i][at] == pixels[0][at];
		if (same)
			art.still[at] = pixels[0][at];
		else
			moving[at] = 1;
	}
	int x0 = art.width;
	int y0 = art.height;
	int x1 = 0;
	int y1 = 0;
	for (int y = 0; y < art.height; y++)
		for (int x = 0; x < art.width; x++)
			if (moving[static_cast<size_t>(y) * art.width + x] != 0) {
				x0 = std::min(x0, x);
				y0 = std::min(y0, y);
				x1 = std::max(x1, x + 1);
				y1 = std::max(y1, y + 1);
			}
	if (x1 <= x0 || y1 <= y0) {
		art.width = 0;
		return nullptr;
	}
	art.flameRect = { x0, y0, x1, y1 };
	for (int i = 0; i < frames; i++) {
		std::vector<int16_t> flame(static_cast<size_t>(x1 - x0) * (y1 - y0), NoPixel);
		for (int y = y0; y < y1; y++)
			for (int x = x0; x < x1; x++)
				if (moving[static_cast<size_t>(y) * art.width + x] != 0)
					flame[static_cast<size_t>(y - y0) * (x1 - x0) + x - x0] = pixels[i][static_cast<size_t>(y) * art.width + x];
		art.flames.push_back(std::move(flame));
	}
	return &art;
}

/** The frame of a torch's wall: the point of the face it hangs on, out of the wall, along it; false if it has no wall. */
bool TorchFrame(const Object &object, Vec2 &origin, Vec2 &out, Vec2 &along)
{
	Vec2 normal;
	const Vec2 pos = WallTorchPlacement(object, normal);
	if (normal.x == 0.0F && normal.y == 0.0F)
		return false;
	origin = { pos.x - normal.x * 0.06F, pos.y - normal.y * 0.06F };
	out = normal;
	along = { normal.y, -normal.x };
	return true;
}

/** Whether a torch is a pole (L, R) rather than a plate and handle (L2, R2), and which way the pole's top leans along its wall. */
bool IsPoleTorch(const Object &object, float &lean)
{
	lean = object._otype == OBJ_TORCHR ? 1.0F : -1.0F; // the art's: OBJ_TORCHR's (wtorch3) top to the right, OBJ_TORCHL's (wtorch4) to the left
	return IsAnyOf(object._otype, OBJ_TORCHL, OBJ_TORCHR);
}

/** The top of a torch's wood (in its wall's frame), where its flame stands. */
std::array<float, 3> TorchTop(const Object &object)
{
	float lean;
	if (IsPoleTorch(object, lean))
		return { TorchPoleTop[0], TorchPoleTop[1] * lean, TorchPoleTop[2] };
	return { TorchHandleTip[0], TorchHandleTip[1], TorchHandleTip[2] + TorchCupHeight };
}

/** The first static frame of a torch of a type on the level, its art (none if there is none). */
const TorchArt *TorchArtOfType(int type, const Object **found)
{
	for (int i = 0; i < ActiveObjectCount; i++) {
		const Object &object = Objects[ActiveObjects[i]];
		if (object._otype != type)
			continue;
		if (const TorchArt *art = TorchArtFor(object); art != nullptr) {
			if (found != nullptr)
				*found = &object;
			return art;
		}
	}
	return nullptr;
}

/**
 * The torches' wood (CleanSurface::Torch): 32 texels along a stick (one repeat over its length), 16 round it; the
 * handle of wtorch2 in rows 0-15, the pole of wtorch3 in 16-31. Each row a line along the stick in the art, from one
 * side of it to the other and back (StickHalfWidth texels either side of its middle).
 */
constexpr int WoodAlong = 32;
constexpr int WoodRound = 16;
constexpr float StickHalfWidth = 1.5F;

void CutStick(const TorchArt &art, const std::array<float, 4> &line, std::vector<int16_t> &texels, int firstRow)
{
	const float dx = line[2] - line[0];
	const float dy = line[3] - line[1];
	const float length = std::max(std::hypot(dx, dy), 1.0F);
	const float px = -dy / length;
	const float py = dx / length;
	for (int r = 0; r < WoodRound; r++) {
		const int side = r < WoodRound / 2 ? r : WoodRound - 1 - r; // across and back
		const float across = ((static_cast<float>(side) + 0.5F) / (WoodRound / 2) * 2.0F - 1.0F) * StickHalfWidth;
		for (int u = 0; u < WoodAlong; u++) {
			const float t = (static_cast<float>(u) + 0.5F) / WoodAlong;
			const int x = static_cast<int>(std::lround(line[0] + dx * t + px * across));
			const int y = static_cast<int>(std::lround(line[1] + dy * t + py * across));
			int16_t texel = NoPixel;
			if (x >= 0 && y >= 0 && x < art.width && y < art.height)
				texel = art.still[static_cast<size_t>(y) * art.width + x];
			texels[static_cast<size_t>(firstRow + r) * WoodAlong + u] = texel;
		}
	}
}

/** Light levels added to the ceiling (as the clean dungeon's). */
constexpr float CeilingDarkness = 3.0F;

// --- Towers --------------------------------------------------------------------------------------------------------

/**
 * The towers (drawn tiles). In the iso art a drum of 0.6 tiles' radius stands where the faces of half-tile-thick walls
 * meet; first person's walls fill their tiles, so a tower out in the open takes its tile's place a little wider
 * (TowerRadius, just wide enough to cover the corners of the wall blocks it joins, 0.29 from the middles of the tiles
 * about it), and one in an inside corner stands on the corner towards the room (TowerCornerRadius: as far from the
 * middle of the tile beyond). At the foot a plinth (PlinthOut wider, PlinthHeight tall, as the art's base course),
 * under the ceiling the overhanging rim of its cap (CapOut wider, CapDrop deep). Drawn by the wall pass as circles
 * (TowerColumn).
 */
constexpr float TowerRadius = 0.71F;
constexpr float TowerCornerRadius = 0.42F;
constexpr float PlinthOut = 0.04F;
constexpr float PlinthHeight = 0.7F;
constexpr float CapOut = 0.07F;
constexpr float CapDrop = 0.25F;
/** Light levels added to the underside of the caps' rims. */
constexpr float CapUndersideDarkness = 1.0F;

/** Lays out the towers (Towers, TowerStart, TowerList) once the tiles are classified (see TowerRadius). */
void FindTowers(const std::vector<CleanKind> &kinds)
{
	Towers.clear();
	auto solid = [](Point p) { return !InDungeonBounds(p) || IsTileSolid(p); };
	for (int y = 0; y < MAXDUNY; y++) {
		for (int x = 0; x < MAXDUNX; x++) {
			const size_t at = static_cast<size_t>(y) * MAXDUNX + x;
			if ((Flags[at] & TileTower) == 0)
				continue;
			const float cx = static_cast<float>(x) + 0.5F;
			const float cy = static_cast<float>(y) + 0.5F;
			if (kinds[at] == CleanKind::Pillar)
				Towers.push_back({ cx, cy, TowerRadius });
			if ((Flags[at] & TileTowerCorner) == 0)
				continue;
			// In an inside corner: on each corner of its tile with a room beyond it (and walls either side).
			for (const int dy : { -1, 1 })
				for (const int dx : { -1, 1 })
					if (!solid({ x + dx, y + dy }) && solid({ x + dx, y }) && solid({ x, y + dy }))
						Towers.push_back({ cx + 0.5F * static_cast<float>(dx), cy + 0.5F * static_cast<float>(dy), TowerCornerRadius });
		}
	}
	// Each tower in the lists of the tiles its widest ring reaches into.
	const size_t count = static_cast<size_t>(MAXDUNX) * MAXDUNY;
	std::vector<uint16_t> perTile(count, 0);
	auto eachTile = [&](const Tower &tower, auto &&visit) {
		const float reach = tower.radius + std::max(CapOut, PlinthOut);
		for (int y = std::max(0, FloorToInt(tower.cy - reach)); y <= std::min(MAXDUNY - 1, FloorToInt(tower.cy + reach)); y++)
			for (int x = std::max(0, FloorToInt(tower.cx - reach)); x <= std::min(MAXDUNX - 1, FloorToInt(tower.cx + reach)); x++)
				visit(static_cast<size_t>(y) * MAXDUNX + x);
	};
	for (const Tower &tower : Towers)
		eachTile(tower, [&](size_t at) { perTile[at]++; });
	TowerStart.assign(count + 1, 0);
	for (size_t i = 0; i < count; i++)
		TowerStart[i + 1] = TowerStart[i] + perTile[i];
	TowerList.assign(TowerStart[count], 0);
	std::vector<uint32_t> next(TowerStart.begin(), TowerStart.end() - 1);
	for (size_t i = 0; i < Towers.size(); i++)
		eachTile(Towers[i], [&](size_t at) { TowerList[next[at]++] = static_cast<uint16_t>(i); });
}

/** The towers' drums wear the plain wall's stone (the same courses of rubble), which repeats without a seam every two tiles. */
constexpr float DrumRepeat = 2.0F;

} // namespace

// --- Public --------------------------------------------------------------------------------------------------------

bool IsCatacombs()
{
	return leveltype == DTYPE_CATACOMBS;
}

void PrepareArtSampler(ArtSampler &s, const ArtTexture &texture, float worldPerPixel)
{
	const float footprint = worldPerPixel * texture.texelsPerTileU; // full-size texels per pixel
	int level = 0;
	if (footprint >= 1.5F)
		level = std::min(static_cast<int>(std::log2(footprint / 1.5F)) + 1, static_cast<int>(texture.mips.size()) - 1);
	const ArtTexture::Mip &mip = texture.mips[level];
	s.texels = mip.texels.data();
	s.maskU = mip.width - 1;
	s.lastRow = mip.height - 1;
	s.shift = mip.shift;
	const float down = static_cast<float>(1 << level);
	s.scaleU = texture.texelsPerTileU / down;
	s.scaleV = texture.texelsPerTileV / down;
	s.jitter = footprint < 1.0F;
}

void ClassifyCatacombsTiles(std::vector<CleanKind> &kinds, std::vector<float> &heights)
{
	const size_t count = static_cast<size_t>(MAXDUNX) * MAXDUNY;
	Flags.assign(count, 0);
	FloorPieces.assign(count, 0);
	if (pMegaTiles == nullptr)
		return;
	const float ceiling = CleanCeiling();
	const int levelRows = LevelWallRows();
	const uint16_t plainFloor = MegatilePiece(PlainFloorTile, 0);
	auto solid = [](Point p) { return !InDungeonBounds(p) || IsTileSolid(p); };
	int walls = 0;
	int lowWalls = 0;
	int towers = 0;
	int corners = 0;
	int doorways = 0;
	int props = 0;
	for (int y = 0; y < MAXDUNY; y++) {
		for (int x = 0; x < MAXDUNX; x++) {
			const Point tile { x, y };
			const size_t at = static_cast<size_t>(y) * MAXDUNX + x;
			const int id = MegatileAt(tile);
			const int quadrant = QuadrantOf(tile);
			const uint16_t piece = dPiece[x][y];
			// The painted shadows go with the iso light: the plain floor's piece at the same place instead.
			FloorPieces[at] = IsShadowTile(id) ? MegatilePiece(PlainFloorTile, quadrant) : piece;
			if (IsDoorwayTile(tile)) {
				kinds[at] = CleanKind::Doorway;
				doorways++;
				continue;
			}
			if (!IsTileSolid(tile)) {
				kinds[at] = CleanKind::Open;
				continue;
			}
			// Under what stands on the floor, and in the walls (never seen), the plain floor.
			FloorPieces[at] = MegatilePiece(PlainFloorTile, quadrant);
			if (!HasAnyOf(SOLData[piece], TileProperties::BlockLight)) {
				// Solid but seen past: the wooden scaffolds, the railing of the stairs down.
				kinds[at] = CleanKind::Prop;
				props++;
				continue;
			}
			if (IsTowerTile(id) && quadrant == 0) {
				Flags[at] |= TileTower;
				towers++;
				// Out in the open on a side, the tower takes its tile's place; walled in on all four, its tile stays a
				// wall. Towards a room only diagonally beyond (an inside corner) it stands on that corner too (see
				// AddCatacombsGeometry).
				const bool exposed = !solid(tile + Displacement { 1, 0 }) || !solid(tile + Displacement { -1, 0 }) || !solid(tile + Displacement { 0, 1 }) || !solid(tile + Displacement { 0, -1 });
				kinds[at] = exposed ? CleanKind::Pillar : CleanKind::Wall;
				for (const int dy : { -1, 1 })
					for (const int dx : { -1, 1 })
						if (!solid(tile + Displacement { dx, dy }) && solid(tile + Displacement { dx, 0 }) && solid(tile + Displacement { 0, dy }))
							Flags[at] |= TileTowerCorner;
				corners += (Flags[at] & TileTowerCorner) != 0 ? 1 : 0;
				continue;
			}
			kinds[at] = CleanKind::Wall;
			// (Behind a scaffold the art paints its timbers on the wall: meshes now, the plain wall's stone behind them.)
			if (!IsScaffoldTile(id))
				Flags[at] |= TileOwnFaces;
			walls++;
			// Low walls (a collapsed wall's breach) stay low: as tall as their art, to the nearest tenth.
			const PieceTextures &textures = GetPieceTextures(piece);
			int rows = 0;
			if (textures.faceXUsable)
				rows = std::max(rows, textures.faceXRows);
			if (textures.faceYUsable)
				rows = std::max(rows, textures.faceYRows);
			if (rows > 0 && static_cast<float>(rows) < static_cast<float>(levelRows) * 0.6F) {
				const float height = static_cast<float>(rows - FaceBaseRows) / SpritePixelsPerTileY * VerticalScale();
				heights[at] = std::clamp(std::round(height * 10.0F) / 10.0F, 0.3F, ceiling);
				lowWalls++;
			}
		}
	}
	FindTowers(kinds);
	// The art the level's walls and floors show, cut now rather than on the first frame that sees it.
	int faces = 0;
	for (int y = 0; y < MAXDUNY; y++) {
		for (int x = 0; x < MAXDUNX; x++) {
			const size_t at = static_cast<size_t>(y) * MAXDUNX + x;
			if (kinds[at] == CleanKind::Open || kinds[at] == CleanKind::Doorway)
				FloorTexture(FloorPieces[at]);
			if (kinds[at] != CleanKind::Wall)
				continue;
			for (int facing = 0; facing < 4; facing++) {
				const Displacement d = std::array<Displacement, 4> { { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } } }[facing];
				const Point next = Point { x, y } + d;
				if (InDungeonBounds(next) && !IsTileSolid(next) && CatacombsFaceArt({ x, y }, facing) != nullptr)
					faces++;
			}
		}
	}
	FloorTexture(plainFloor);
	LogInfo("fp: catacombs tiles: {} walls ({} low), {} tower tiles ({} with inside corners: {} drums in all), {} doorways, {} props; {} wall faces from {} pieces' art, {} floors", walls, lowWalls, towers, corners, Towers.size(), doorways, props, faces, Art.faces.size(), Art.floors.size());
}

float CatacombsTowerRadius()
{
	return TowerRadius;
}

float CatacombsArchHalfThickness()
{
	return ArchHalfThickness;
}

float CatacombsArchUnderside(float s, float a, float b)
{
	const float beam = CleanCeiling() - ArchBeamDepth;
	const float haunch = std::min(ArchHaunch, (b - a) * 0.5F);
	const float into = haunch - std::min(s - a, b - s); // how far into the nearer haunch
	if (into <= 0.0F)
		return beam;
	return beam - (haunch - std::sqrt(std::max(0.0F, haunch * haunch - into * into)));
}

CleanOpening CatacombsDoorOpening()
{
	return { DoorHalfWidth, DoorHeight, true };
}

const ArtTexture *CatacombsFaceArt(Point tile, int facing)
{
	if (!IsCatacombs() || !InDungeonBounds(tile) || Flags.empty() || pMegaTiles == nullptr)
		return nullptr;
	const size_t at = static_cast<size_t>(tile.y) * MAXDUNX + tile.x;
	const bool alongY = facing <= 1; // a face looking along x runs along y: the art of the iso's left faces (faceX)
	// The faces the iso drew (looking towards +x or +y) wear the tile's own art.
	if ((Flags[at] & TileOwnFaces) != 0 && (facing == 0 || facing == 2)) {
		const uint16_t piece = dPiece[tile.x][tile.y];
		const PieceTextures &textures = GetPieceTextures(piece);
		if (alongY ? textures.faceXUsable : textures.faceYUsable)
			return &FaceTexture(piece, alongY);
	}
	// The others, the plain wall's by their place along the wall.
	return &FaceTexture(PlainWallPiece(tile, alongY), alongY);
}

bool CatacombsArtTexture(CleanSurface surface, CleanSourceTexture &out)
{
	if (!IsCatacombs() || pMegaTiles == nullptr)
		return false;
	constexpr int Fine = TextureUpscale;
	switch (surface) {
	case CleanSurface::Ceiling:
	case CleanSurface::Stone: {
		// The flagstones of a tower's cap: 2 tiles a repeat.
		ToSource(Finer(CutFlagstones(), 2 * FloorTextureSize), 2 * FloorTextureSize * Fine, 2.0F, 2.0F, out);
		return true;
	}
	case CleanSurface::Floor:
		ToSource(Finer(PlainFloor(), 2 * FloorTextureSize), 2 * FloorTextureSize * Fine, 2.0F, 2.0F, out);
		return true;
	case CleanSurface::Door: {
		// The door leaf, u and v 0..1 over it, its hinge at u 0 (stretched to powers of two).
		std::vector<int16_t> leaf;
		int width;
		int height;
		if (!CutDoorLeaf(leaf, width, height))
			return false;
		const std::vector<int16_t> fine = Finer(leaf, width);
		std::vector<int16_t> across(static_cast<size_t>(128) * height * Fine);
		for (int y = 0; y < height * Fine; y++)
			for (int x = 0; x < 128; x++)
				across[static_cast<size_t>(y) * 128 + x] = fine[static_cast<size_t>(y) * width * Fine + x * width * Fine / 128];
		ToSource(Rows(across, 128, 256), 128, 1.0F, 1.0F, out);
		return true;
	}
	case CleanSurface::Torch: {
		// The wall torches' wood, cut out of their sprites (see WoodAlong).
		const TorchArt *handle = TorchArtOfType(OBJ_TORCHR2, nullptr);
		const TorchArt *pole = TorchArtOfType(OBJ_TORCHR, nullptr);
		if (handle == nullptr && pole == nullptr)
			return false;
		std::vector<int16_t> wood(static_cast<size_t>(WoodAlong) * WoodRound * 2, NoPixel);
		CutStick(handle != nullptr ? *handle : *pole, handle != nullptr ? HandleArt : PoleArt, wood, 0);
		CutStick(pole != nullptr ? *pole : *handle, pole != nullptr ? PoleArt : HandleArt, wood, WoodRound);
		FillColumns(wood, WoodAlong);
		ToSource(Finer(wood, WoodAlong), WoodAlong * Fine, 1.0F, 1.0F, out);
		return true;
	}
	case CleanSurface::Iron: {
		// The torches' iron: the plate of wtorch1, unsheared (its columns rise a row every two), laid four times,
		// mirrored, the plate itself in the first quarter (so its front shows it whole: TorchPlate*).
		const TorchArt *plate = TorchArtOfType(OBJ_TORCHL2, nullptr);
		if (plate == nullptr)
			return false;
		constexpr int N = 16;
		std::vector<int16_t> patch(static_cast<size_t>(N) * N, NoPixel);
		for (int u = 0; u < N; u++) {
			const int x = PlateArtColumns[0] + u * (PlateArtColumns[1] - PlateArtColumns[0]) / (N - 1);
			int top = -1;
			for (int y = 0; y < plate->height && top < 0; y++) {
				const int16_t t = plate->still[static_cast<size_t>(y) * plate->width + x];
				if (t > 0) {
					const SDL_Color c = logical_palette[t];
					if (std::abs(c.r - c.g) < 8 && std::abs(c.g - c.b) < 8)
						top = y; // its grey, not the handle's wood above it
				}
			}
			for (int v = 0; v < N && top >= 0; v++) {
				const int y = top + v * PlateArtRows / N;
				if (y < plate->height)
					patch[static_cast<size_t>(v) * N + u] = plate->still[static_cast<size_t>(y) * plate->width + x];
			}
		}
		FillColumns(patch, N);
		std::vector<int16_t> tiled(static_cast<size_t>(4) * N * N);
		for (int v = 0; v < 2 * N; v++)
			for (int u = 0; u < 2 * N; u++)
				tiled[static_cast<size_t>(v) * 2 * N + u] = patch[static_cast<size_t>(v < N ? v : 2 * N - 1 - v) * N + (u < N ? u : 2 * N - 1 - u)];
		ToSource(Finer(tiled, 2 * N), 2 * N * Fine, 4.0F * TorchPlateHalfWidth, 4.0F * TorchPlateHalfHeight, out);
		return true;
	}
	case CleanSurface::Wood:
		// The scaffolds' timbers: the deck's planks, a repeat a tile along them.
		ToSource(Finer(CutWood(), WoodTexels), WoodTexels * Fine, 1.0F, 1.0F, out);
		return true;
	case CleanSurface::Arch: {
		// The doorways' jambs and lintels: the art's dressed stone, a repeat a tile across and down.
		ToSource(Finer(CutReveal(), 2 * RevealPatch), 2 * RevealPatch * Fine, 2.0F * RevealPatch / FaceTextureWidth, 2.0F * RevealPatch / SpritePixelsPerTileY * VerticalScale(), out);
		return true;
	}
	case CleanSurface::Wall:
	case CleanSurface::Pillar: {
		// The plain wall, two tiles across, from the floor up to the ceiling (stretched to a power of two tall): the
		// walls' own (as a fallback), and the towers' drums.
		const int width = 2 * FaceTextureWidth;
		const std::vector<int16_t> fine = Finer(PlainWallStrip(), width);
		int rows = 1;
		while (rows < static_cast<int>(fine.size()) / (width * Fine))
			rows <<= 1;
		ToSource(Rows(fine, width * Fine, rows), width * Fine, 2.0F, CleanCeiling(), out);
		return true;
	}
	default:
		return false;
	}
}

void AddCatacombsGeometry(std::vector<CleanQuad> &quads, std::vector<std::array<float, 4>> &holes)
{
	using P3 = std::array<float, 3>;
	const float ceiling = CleanCeiling();
	const CleanKind *kinds = CleanKindMap();
	constexpr float Pi = 3.14159265F;
	const size_t first = quads.size();
	// The stairs up: steps winding round their towers, starting by their triggers.
	CeilingHoles.clear();
	int spirals = 0;
	int pits = 0;
	for (int y = 0; y < MAXDUNY; y++) {
		for (int x = 0; x < MAXDUNX; x++) {
			const Point tile { x, y };
			if ((Flags[static_cast<size_t>(y) * MAXDUNX + x] & TileTower) == 0 || !IsSpiralStairsTile(MegatileAt(tile)))
				continue;
			const float cx = static_cast<float>(x) + 0.5F;
			const float cy = static_cast<float>(y) + 0.5F;
			// From the side of the nearest trigger (the art's steps start at the tile in front of the tower's left: +y).
			float start = Pi * 0.5F;
			int best = 9;
			for (int i = 0; i < numtrigs; i++) {
				const int dx = static_cast<int>(trigs[i].position.x) - x;
				const int dy = static_cast<int>(trigs[i].position.y) - y;
				const int far = std::abs(dx) + std::abs(dy);
				if (far > 0 && far < best) {
					best = far;
					start = std::atan2(static_cast<float>(dy), static_cast<float>(dx));
				}
			}
			const float r0 = TowerRadius;
			const float r1 = TowerRadius + StairsWidth;
			const float step = StairsTurn / StairsSteps;
			const float rise = ceiling / StairsSteps;
			for (int k = 0; k < StairsSteps; k++) {
				const float a0 = start - step * 0.5F + step * static_cast<float>(k);
				const float a1 = a0 + step;
				const float h0 = rise * static_cast<float>(k);
				const float h1 = rise * static_cast<float>(k + 1);
				auto at = [&](float r, float a, float z) { return P3 { cx + r * std::cos(a), cy + r * std::sin(a), z }; };
				// The tread.
				CleanQuad tread;
				tread.p = { at(r0, a0, h1), at(r1, a0, h1), at(r1, a1, h1), at(r0, a1, h1) };
				tread.normal = { 0.0F, 0.0F, 1.0F };
				for (int c = 0; c < 4; c++)
					tread.uv[c] = { tread.p[c][0], tread.p[c][1] };
				tread.surface = CleanSurface::Stone;
				quads.push_back(tread);
				// The riser, facing back down the flight, a shade darker so the steps read.
				CleanQuad riser;
				riser.p = { at(r1, a0, h0), at(r0, a0, h0), at(r0, a0, h1), at(r1, a0, h1) };
				riser.normal = { std::sin(a0), -std::cos(a0), 0.0F };
				riser.uv = { { { r1, -h0 }, { r0, -h0 }, { r0, -h1 }, { r1, -h1 } } };
				riser.surface = CleanSurface::Stone;
				riser.darkness = 1.0F;
				quads.push_back(riser);
				// Its outer side, down to the floor: the flight is built solid against the tower.
				const float am = (a0 + a1) * 0.5F;
				CleanQuad side;
				side.p = { at(r1, a1, 0.0F), at(r1, a0, 0.0F), at(r1, a0, h1), at(r1, a1, h1) };
				side.normal = { std::cos(am), std::sin(am), 0.0F };
				side.uv = { { { -a1 * r1, 0.0F }, { -a0 * r1, 0.0F }, { -a0 * r1, -h1 }, { -a1 * r1, -h1 } } };
				side.surface = CleanSurface::Wall;
				quads.push_back(side);
			}
			{
				// The end of the flight after its top step, solid from the floor up: facing on round the tower.
				auto at = [&](float r, float a, float z) { return P3 { cx + r * std::cos(a), cy + r * std::sin(a), z }; };
				const float a = start - step * 0.5F + StairsTurn;
				CleanQuad end;
				end.p = { at(r0, a, 0.0F), at(r1, a, 0.0F), at(r1, a, ceiling), at(r0, a, ceiling) };
				end.normal = { -std::sin(a), std::cos(a), 0.0F };
				end.uv = { { { r0, 0.0F }, { r1, 0.0F }, { r1, -ceiling }, { r0, -ceiling } } };
				end.surface = CleanSurface::Wall;
				quads.push_back(end);
			}
			CeilingHoles.push_back({ cx, cy, r0, r1, start - step * 0.5F + step * static_cast<float>(StairsSteps - StairsOpenSteps), step * StairsOpenSteps });
			spirals++;
		}
	}
	// The stairs down: each trench (its tiles in a line beside its tower), steps down it from the end by its trigger.
	std::vector<uint8_t> seen(static_cast<size_t>(MAXDUNX) * MAXDUNY, 0);
	for (int y = 0; y < MAXDUNY; y++) {
		for (int x = 0; x < MAXDUNX; x++) {
			const size_t at = static_cast<size_t>(y) * MAXDUNX + x;
			if (seen[at] != 0 || kinds[at] != CleanKind::Prop || !IsStairsDownTile(MegatileAt({ x, y })))
				continue;
			// Its tiles: the run of them along x or along y.
			auto pitAt = [&](Point t) { return InDungeonBounds(t) && kinds[static_cast<size_t>(t.y) * MAXDUNX + t.x] == CleanKind::Prop && IsStairsDownTile(MegatileAt(t)); };
			int lengthX = 1;
			while (pitAt({ x + lengthX, y }))
				lengthX++;
			int lengthY = 1;
			while (pitAt({ x, y + lengthY }))
				lengthY++;
			const bool alongY = lengthY >= lengthX;
			const int length = alongY ? lengthY : lengthX;
			for (int k = 0; k < length; k++)
				seen[alongY ? static_cast<size_t>(y + k) * MAXDUNX + x : at + k] = 1;
			const float x0 = static_cast<float>(x) + PitInset;
			const float y0 = static_cast<float>(y) + PitInset;
			const float x1 = static_cast<float>(alongY ? x + 1 : x + length) - PitInset;
			const float y1 = static_cast<float>(alongY ? y + length : y + 1) - PitInset;
			// Which end the flight starts at: the one nearer the stairs' trigger.
			const Point first { x, y };
			const Point last = alongY ? Point { x, y + length - 1 } : Point { x + length - 1, y };
			bool fromLast = true;
			int best = std::numeric_limits<int>::max();
			for (int i = 0; i < numtrigs; i++) {
				if (trigs[i]._tmsg != WM_DIABNEXTLVL)
					continue;
				const int tx = static_cast<int>(trigs[i].position.x);
				const int ty = static_cast<int>(trigs[i].position.y);
				const int toFirst = std::abs(tx - first.x) + std::abs(ty - first.y);
				const int toLast = std::abs(tx - last.x) + std::abs(ty - last.y);
				if (std::min(toFirst, toLast) < best) {
					best = std::min(toFirst, toLast);
					fromLast = toLast <= toFirst;
				}
			}
			// Points by how far along the flight (0 at its top), across it (0..1), and z.
			const float run = alongY ? y1 - y0 : x1 - x0;
			auto point = [&](float along, float across, float z) {
				const float s = fromLast ? run - along : along;
				return alongY ? P3 { x0 + (x1 - x0) * across, y0 + s, z } : P3 { x0 + s, y0 + (y1 - y0) * across, z };
			};
			const P3 down { alongY ? 0.0F : (fromLast ? -1.0F : 1.0F), alongY ? (fromLast ? -1.0F : 1.0F) : 0.0F, 0.0F }; // the way the flight goes
			for (int k = 0; k < PitSteps; k++) {
				const float a0 = run * static_cast<float>(k) / PitSteps;
				const float a1 = run * static_cast<float>(k + 1) / PitSteps;
				const float top = -PitDepth * static_cast<float>(k) / PitSteps;
				const float tread = -PitDepth * static_cast<float>(k + 1) / PitSteps;
				const float darkness = PitBands * static_cast<float>(k) / PitSteps;
				CleanQuad riser;
				riser.p = { point(a0, 0.0F, tread), point(a0, 1.0F, tread), point(a0, 1.0F, top), point(a0, 0.0F, top) };
				riser.normal = { -down[0], -down[1], 0.0F };
				riser.uv = { { { 0.0F, -tread }, { 1.0F, -tread }, { 1.0F, -top }, { 0.0F, -top } } };
				riser.surface = CleanSurface::Stone;
				riser.darkness = darkness + 1.0F;
				quads.push_back(riser);
				CleanQuad flat;
				flat.p = { point(a0, 0.0F, tread), point(a1, 0.0F, tread), point(a1, 1.0F, tread), point(a0, 1.0F, tread) };
				flat.normal = { 0.0F, 0.0F, 1.0F };
				for (int c = 0; c < 4; c++)
					flat.uv[c] = { flat.p[c][0], flat.p[c][1] };
				flat.surface = CleanSurface::Stone;
				flat.darkness = darkness;
				quads.push_back(flat);
			}
			// The sides, in bands darker going down, and the far end, where the flight goes on under the floor: dark.
			const float bottom = -PitDepth - 0.05F;
			for (int band = 0; band < PitBands; band++) {
				const float z0 = bottom * static_cast<float>(band + 1) / PitBands;
				const float z1 = bottom * static_cast<float>(band) / PitBands;
				for (const float across : { 0.0F, 1.0F }) {
					CleanQuad side;
					side.p = { point(0.0F, across, z0), point(run, across, z0), point(run, across, z1), point(0.0F, across, z1) };
					const P3 inward = alongY ? P3 { across > 0.5F ? -1.0F : 1.0F, 0.0F, 0.0F } : P3 { 0.0F, across > 0.5F ? -1.0F : 1.0F, 0.0F };
					side.normal = inward;
					const float u0 = alongY ? side.p[0][1] : side.p[0][0];
					const float u1 = alongY ? side.p[1][1] : side.p[1][0];
					side.uv = { { { u0, -z0 }, { u1, -z0 }, { u1, -z1 }, { u0, -z1 } } };
					side.surface = CleanSurface::Wall;
					side.darkness = static_cast<float>(band) + 0.5F;
					quads.push_back(side);
				}
			}
			CleanQuad end;
			end.p = { point(run, 0.0F, bottom), point(run, 1.0F, bottom), point(run, 1.0F, 0.0F), point(run, 0.0F, 0.0F) };
			end.normal = { -down[0], -down[1], 0.0F };
			end.uv = { { { 0.0F, -bottom }, { 1.0F, -bottom }, { 1.0F, 0.0F }, { 0.0F, 0.0F } } };
			end.surface = CleanSurface::Wall;
			end.darkness = static_cast<float>(PitBands) + 4.0F;
			quads.push_back(end);
			holes.push_back({ x0, y0, x1, y1 });
			pits++;
		}
	}
	// The scaffolds: on each of their front tiles, against the wall behind it.
	auto add3 = [](P3 a, P3 b) { return P3 { a[0] + b[0], a[1] + b[1], a[2] + b[2] }; };
	auto mul3 = [](P3 a, float k) { return P3 { a[0] * k, a[1] * k, a[2] * k }; };
	/** A square timber from a to b, `half` thick each way, its sides of the wood (u along it). */
	auto addBeam = [&](P3 a, P3 b, float half, float darkness) {
		P3 d { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
		const float length = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
		if (length < 1e-4F)
			return;
		d = mul3(d, 1.0F / length);
		P3 side = std::abs(d[2]) > 0.9F ? P3 { 1.0F, 0.0F, 0.0F } : P3 { d[1], -d[0], 0.0F };
		const float sideLength = std::sqrt(side[0] * side[0] + side[1] * side[1] + side[2] * side[2]);
		side = mul3(side, 1.0F / sideLength);
		const P3 up { side[1] * d[2] - side[2] * d[1], side[2] * d[0] - side[0] * d[2], side[0] * d[1] - side[1] * d[0] };
		const std::array<P3, 4> around { add3(mul3(side, half), mul3(up, half)), add3(mul3(side, -half), mul3(up, half)), add3(mul3(side, -half), mul3(up, -half)), add3(mul3(side, half), mul3(up, -half)) };
		for (int k = 0; k < 4; k++) {
			const P3 &c0 = around[k];
			const P3 &c1 = around[(k + 1) % 4];
			CleanQuad quad;
			quad.p = { add3(a, c0), add3(b, c0), add3(b, c1), add3(a, c1) };
			const P3 n = add3(c0, c1);
			const float nl = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
			quad.normal = { n[0] / nl, n[1] / nl, n[2] / nl };
			const float v0 = static_cast<float>(k) * 2.0F * half;
			const float v1 = v0 + 2.0F * half;
			quad.uv = { { { 0.0F, v0 }, { length, v0 }, { length, v1 }, { 0.0F, v1 } } };
			quad.surface = CleanSurface::Wood;
			quad.darkness = darkness + (n[2] < -0.5F * nl ? 1.0F : 0.0F);
			quads.push_back(quad);
		}
	};
	int scaffolds = 0;
	for (int y = 0; y < MAXDUNY; y++) {
		for (int x = 0; x < MAXDUNX; x++) {
			const Point tile { x, y };
			const int id = MegatileAt(tile);
			if (kinds[static_cast<size_t>(y) * MAXDUNX + x] != CleanKind::Prop || !IsScaffoldTile(id))
				continue;
			// The wall it stands against: the neighbour that is a wall (a scaffold's tiles run along it).
			Displacement toWall { 0, 0 };
			for (const Displacement d : { Displacement { -1, 0 }, Displacement { 0, -1 }, Displacement { 1, 0 }, Displacement { 0, 1 } }) {
				const Point n = tile + d;
				if (InDungeonBounds(n) && kinds[static_cast<size_t>(n.y) * MAXDUNX + n.x] == CleanKind::Wall) {
					toWall = d;
					break;
				}
			}
			if (toWall == Displacement { 0, 0 })
				continue;
			// A point `along` the wall (0..1 across the tile, mirrored every other tile), `out` from its face, z up.
			const bool flip = ((x + y) & 1) != 0;
			const P3 outward { static_cast<float>(-toWall.deltaX), static_cast<float>(-toWall.deltaY), 0.0F };
			const P3 alongDir { -outward[1], outward[0], 0.0F };
			const P3 face { static_cast<float>(x) + 0.5F + 0.5F * static_cast<float>(toWall.deltaX), static_cast<float>(y) + 0.5F + 0.5F * static_cast<float>(toWall.deltaY), 0.0F };
			auto at = [&](float along, float out, float z) {
				const float a = (flip ? 1.0F - along : along) - 0.5F;
				return P3 { face[0] + alongDir[0] * a + outward[0] * out, face[1] + alongDir[1] * a + outward[1] * out, z };
			};
			const float b = ScaffoldBeam;
			if (id == 118 || id == 119) {
				// A deck on posts: its planks (top, front edge, underside), two posts, a brace across under it.
				const float z0 = ScaffoldDeckHeight - ScaffoldDeck;
				const float z1 = ScaffoldDeckHeight;
				CleanQuad top;
				top.p = { at(0.0F, 0.0F, z1), at(1.0F, 0.0F, z1), at(1.0F, ScaffoldDeckOut, z1), at(0.0F, ScaffoldDeckOut, z1) };
				top.normal = { 0.0F, 0.0F, 1.0F };
				top.uv = { { { 0.0F, 0.0F }, { 1.0F, 0.0F }, { 1.0F, ScaffoldDeckOut }, { 0.0F, ScaffoldDeckOut } } };
				top.surface = CleanSurface::Wood;
				top.twoSided = true;
				quads.push_back(top);
				CleanQuad under = top;
				for (P3 &c : under.p)
					c[2] = z0;
				under.normal = { 0.0F, 0.0F, -1.0F };
				under.darkness = 2.0F;
				quads.push_back(under);
				CleanQuad edge;
				edge.p = { at(0.0F, ScaffoldDeckOut, z0), at(1.0F, ScaffoldDeckOut, z0), at(1.0F, ScaffoldDeckOut, z1), at(0.0F, ScaffoldDeckOut, z1) };
				edge.normal = outward;
				edge.uv = { { { 0.0F, 0.0F }, { 1.0F, 0.0F }, { 1.0F, ScaffoldDeck }, { 0.0F, ScaffoldDeck } } };
				edge.surface = CleanSurface::Wood;
				edge.twoSided = true;
				quads.push_back(edge);
				for (const float along : { 0.12F, 0.88F })
					addBeam(at(along, ScaffoldDeckOut - b, 0.0F), at(along, ScaffoldDeckOut - b, z0), b, 0.0F);
				addBeam(at(0.12F, ScaffoldDeckOut - b, 0.05F), at(0.88F, ScaffoldDeckOut - b, z0 - 0.05F), b * 0.8F, 0.5F);
				addBeam(at(0.5F, 0.05F, z0 - 0.35F), at(0.5F, ScaffoldDeckOut - b, z0 - 0.02F), b * 0.8F, 1.0F);
			} else {
				// Fallen to pieces: a broken post, beams leaning on the wall, one lying on the floor.
				addBeam(at(0.3F, 0.7F, 0.0F), at(0.3F, 0.7F, id == 116 ? 1.15F : 0.8F), b, 0.0F);
				addBeam(at(0.15F, 0.85F, 0.0F), at(0.75F, 0.04F, 1.9F), b, 0.0F);
				addBeam(at(0.95F, 0.55F, 0.0F), at(0.35F, 0.04F, 1.35F), b, 0.0F);
				addBeam(at(0.05F, 0.35F, b), at(0.95F, 0.8F, b), b, 0.5F);
			}
			scaffolds++;
		}
	}
	// The wall torches (see TorchPlate*): meshes in the design of their sprites, their flames sprites (CatacombsTorchFlame).
	/** A tube from a to b, radii ra and rb, its wood laid on along it (u 0..1) and round it (v0..v1); ends closed if asked. */
	auto addTube = [&](P3 a, P3 b, float ra, float rb, CleanSurface surface, float v0, float v1, bool capA, bool capB) {
		const P3 d { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
		const float length = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
		if (length < 1e-4F)
			return;
		const P3 t { d[0] / length, d[1] / length, d[2] / length };
		P3 side = std::abs(t[2]) > 0.9F ? P3 { 1.0F, 0.0F, 0.0F } : P3 { -t[1], t[0], 0.0F };
		const float sl = std::sqrt(side[0] * side[0] + side[1] * side[1] + side[2] * side[2]);
		side = mul3(side, 1.0F / sl);
		const P3 up { t[1] * side[2] - t[2] * side[1], t[2] * side[0] - t[0] * side[2], t[0] * side[1] - t[1] * side[0] };
		auto ring = [&](float angle) { return add3(mul3(side, std::cos(angle)), mul3(up, std::sin(angle))); };
		for (int k = 0; k < TorchSides; k++) {
			const float a0 = 6.2831853F * static_cast<float>(k) / TorchSides;
			const float a1 = 6.2831853F * static_cast<float>(k + 1) / TorchSides;
			const P3 r0 = ring(a0);
			const P3 r1 = ring(a1);
			const P3 rm = ring((a0 + a1) * 0.5F);
			CleanQuad quad;
			quad.p = { add3(a, mul3(r0, ra)), add3(b, mul3(r0, rb)), add3(b, mul3(r1, rb)), add3(a, mul3(r1, ra)) };
			quad.normal = rm;
			const float va = v0 + (v1 - v0) * static_cast<float>(k) / TorchSides;
			const float vb = v0 + (v1 - v0) * static_cast<float>(k + 1) / TorchSides;
			quad.uv = { { { 0.0F, va }, { 0.999F, va }, { 0.999F, vb }, { 0.0F, vb } } };
			quad.surface = surface;
			quad.darkness = 0.75F * (1.0F - rm[2]); // lit from above
			quads.push_back(quad);
			for (const bool atB : { false, true }) {
				if (!(atB ? capB : capA))
					continue;
				const P3 centre = atB ? b : a;
				const float r = atB ? rb : ra;
				CleanQuad cap;
				const P3 c0 = add3(centre, mul3(r0, r));
				const P3 c1 = add3(centre, mul3(r1, r));
				cap.p = atB ? std::array<P3, 4> { centre, c0, c1, c1 } : std::array<P3, 4> { centre, c1, c0, c0 };
				cap.normal = atB ? t : mul3(t, -1.0F);
				cap.uv = { { { 0.5F, (v0 + v1) * 0.5F }, { 0.6F, va }, { 0.6F, vb }, { 0.6F, vb } } };
				cap.surface = surface;
				cap.triangle = true;
				cap.darkness = atB ? 0.0F : 1.0F;
				quads.push_back(cap);
			}
		}
	};
	int torches = 0;
	for (int i = 0; i < ActiveObjectCount; i++) {
		const Object &object = Objects[ActiveObjects[i]];
		if (!IsWallTorch(object._otype) || !InDungeonBounds(object.position) || dObject[object.position.x][object.position.y] != ActiveObjects[i] + 1)
			continue;
		Vec2 origin;
		Vec2 out;
		Vec2 along;
		if (!TorchFrame(object, origin, out, along))
			continue;
		auto at = [&](float n, float a, float z) { return P3 { origin.x + out.x * n + along.x * a, origin.y + out.y * n + along.y * a, z }; };
		float lean;
		if (IsPoleTorch(object, lean)) {
			const P3 foot = at(TorchPoleFoot[0], TorchPoleFoot[1] * lean, TorchPoleFoot[2]);
			const P3 top = at(TorchPoleTop[0], TorchPoleTop[1] * lean, TorchPoleTop[2]);
			addTube(foot, top, TorchPoleRadius * 0.85F, TorchPoleRadius, CleanSurface::Torch, 0.5F, 1.0F, true, true);
			// Its ring, and the arm holding it to the wall.
			const P3 ringAt = add3(foot, mul3(P3 { top[0] - foot[0], top[1] - foot[1], top[2] - foot[2] }, TorchRingAt));
			const P3 ringDir { top[0] - foot[0], top[1] - foot[1], top[2] - foot[2] };
			const float rl = std::sqrt(ringDir[0] * ringDir[0] + ringDir[1] * ringDir[1] + ringDir[2] * ringDir[2]);
			const P3 half = mul3(ringDir, 0.035F / rl);
			addTube(add3(ringAt, mul3(half, -1.0F)), add3(ringAt, half), TorchPoleRadius * 1.55F, TorchPoleRadius * 1.55F, CleanSurface::Iron, 0.0F, 0.25F, true, true);
			const P3 wall { ringAt[0] - out.x * (TorchPoleFoot[0] + (TorchPoleTop[0] - TorchPoleFoot[0]) * TorchRingAt), ringAt[1] - out.y * (TorchPoleFoot[0] + (TorchPoleTop[0] - TorchPoleFoot[0]) * TorchRingAt), ringAt[2] };
			addTube(wall, ringAt, 0.018F, 0.018F, CleanSurface::Iron, 0.0F, 0.25F, false, false);
		} else {
			// The plate: its front of the plate's art, its edges.
			const float w = TorchPlateHalfWidth;
			const float h = TorchPlateHalfHeight;
			const float z = TorchPlateHeight;
			const float n = TorchPlateDepth;
			CleanQuad front;
			front.p = { at(n, -w, z - h), at(n, w, z - h), at(n, w, z + h), at(n, -w, z + h) };
			front.normal = { out.x, out.y, 0.0F };
			front.uv = { { { 0.0F, 2.0F * h }, { 2.0F * w, 2.0F * h }, { 2.0F * w, 0.0F }, { 0.0F, 0.0F } } };
			front.surface = CleanSurface::Iron;
			quads.push_back(front);
			const std::array<std::array<P3, 4>, 4> edges { {
			    { at(0, -w, z - h), at(0, w, z - h), at(n, w, z - h), at(n, -w, z - h) },
			    { at(0, w, z + h), at(0, -w, z + h), at(n, -w, z + h), at(n, w, z + h) },
			    { at(0, w, z - h), at(0, w, z + h), at(n, w, z + h), at(n, w, z - h) },
			    { at(0, -w, z + h), at(0, -w, z - h), at(n, -w, z - h), at(n, -w, z + h) },
			} };
			const std::array<P3, 4> normals { P3 { 0, 0, -1 }, P3 { 0, 0, 1 }, P3 { along.x, along.y, 0 }, P3 { -along.x, -along.y, 0 } };
			for (size_t e = 0; e < edges.size(); e++) {
				CleanQuad edge;
				edge.p = edges[e];
				edge.normal = normals[e];
				edge.uv = { { { 0.0F, 0.0F }, { 2.0F * w, 0.0F }, { 2.0F * w, n }, { 0.0F, n } } };
				edge.surface = CleanSurface::Iron;
				edge.darkness = 1.0F;
				quads.push_back(edge);
			}
			// The handle, from the plate's middle out and up, and the cup at its tip.
			const P3 tip = at(TorchHandleTip[0], TorchHandleTip[1], TorchHandleTip[2]);
			addTube(at(n * 0.5F, 0.0F, z), tip, TorchHandleRadius, TorchHandleRadius * 1.25F, CleanSurface::Torch, 0.0F, 0.5F, false, false);
			addTube(P3 { tip[0], tip[1], tip[2] - 0.02F }, P3 { tip[0], tip[1], tip[2] + TorchCupHeight }, TorchCupRadius * 0.6F, TorchCupRadius, CleanSurface::Iron, 0.0F, 0.25F, true, true);
		}
		torches++;
	}
	LogInfo("fp: catacombs geometry: {} stairs up, {} stairs down, {} scaffolds, {} wall torches, {} quads", spirals, pits, scaffolds, torches, quads.size() - first);
}

bool CatacombsTorchFlame(const Object &object, ClxSprite frame, Entity &entity)
{
	const TorchArt *art = TorchArtFor(object);
	Vec2 origin;
	Vec2 out;
	Vec2 along;
	if (art == nullptr || !TorchFrame(object, origin, out, along))
		return false;
	// Which frame it is showing.
	const OptionalClxSpriteList &list = object._oAnimData;
	size_t index = 0;
	for (size_t i = 0; i < list->numSprites(); i++)
		if ((*list)[static_cast<uint32_t>(i)].pixelData() == frame.pixelData())
			index = i;
	if (index >= art->flames.size())
		return false;
	const auto [x0, y0, x1, y1] = art->flameRect;
	const std::array<float, 3> top = TorchTop(object);
	entity.pos = { origin.x + out.x * top[0] + along.x * top[1], origin.y + out.y * top[0] + along.y * top[1] };
	entity.pixels = &art->flames[index];
	entity.pixelsWidth = x1 - x0;
	entity.pixelsHeight = y1 - y0;
	entity.width = static_cast<float>(x1 - x0) / SpritePixelsPerTileX * FlameScale;
	entity.height = static_cast<float>(y1 - y0) / SpritePixelsPerTileY * FlameScale;
	entity.bottom = top[2] - FlameSink;
	entity.facing = true; // turned to the camera, each column at its own depth: it never goes into the wall
	entity.sprite.reset();
	return true;
}

void TowerColumn::Begin(const Projection &proj, int x)
{
	proj_ = &proj;
	x_ = x;
	pos_ = proj.pos;
	ray_ = proj.columnRay(x);
	tile_ = { FloorToInt(pos_.x), FloorToInt(pos_.y) };
	deltaX_ = ray_.x == 0 ? 1e30F : std::abs(1.0F / ray_.x);
	deltaY_ = ray_.y == 0 ? 1e30F : std::abs(1.0F / ray_.y);
	stepX_ = ray_.x < 0 ? -1 : 1;
	stepY_ = ray_.y < 0 ? -1 : 1;
	sideX_ = ray_.x < 0 ? (pos_.x - static_cast<float>(tile_.x)) * deltaX_ : (static_cast<float>(tile_.x) + 1.0F - pos_.x) * deltaX_;
	sideY_ = ray_.y < 0 ? (pos_.y - static_cast<float>(tile_.y)) * deltaY_ : (static_cast<float>(tile_.y) + 1.0F - pos_.y) * deltaY_;
	entered_ = 0.0F;
	walking_ = IsCatacombs() && !Towers.empty();
	seenCount_ = 0;
	pendingCount_ = 0;
}

namespace {

/** Where a ray from `pos` enters a circle (its parameter, perpendicular depth), or a negative number if it does not (or starts inside). */
float EnterCircle(Vec2 pos, Vec2 ray, float cx, float cy, float radius)
{
	const float ox = pos.x - cx;
	const float oy = pos.y - cy;
	const float a = ray.x * ray.x + ray.y * ray.y;
	const float b = ray.x * ox + ray.y * oy;
	const float c = ox * ox + oy * oy - radius * radius;
	if (c <= 0.0F)
		return -1.0F;
	const float disc = b * b - a * c;
	if (disc < 0.0F)
		return -1.0F;
	return (-b - std::sqrt(disc)) / a;
}

} // namespace

void TowerColumn::DrawUntil(float until, const Surface &out, std::vector<uint8_t> &filled, std::vector<float> &depthBuffer, float &nearest)
{
	if (!walking_ && pendingCount_ == 0)
		return;
	// Every tower whose ring meets the ray nearer than `until` is listed in a tile the ray enters before it: walk on
	// to there, gathering them.
	while (walking_ && entered_ <= until) {
		if (!InDungeonBounds(tile_) || entered_ > MaxRayDistance) {
			walking_ = false;
			break;
		}
		const size_t at = static_cast<size_t>(tile_.y) * MAXDUNX + tile_.x;
		for (uint32_t k = TowerStart[at]; k < TowerStart[at + 1]; k++) {
			const uint16_t id = TowerList[k];
			if (std::find(seen_.begin(), seen_.begin() + seenCount_, id) != seen_.begin() + seenCount_ || seenCount_ == seen_.size())
				continue;
			seen_[seenCount_++] = id;
			const Tower &tower = Towers[id];
			const float t = EnterCircle(pos_, ray_, tower.cx, tower.cy, tower.radius + std::max(CapOut, PlinthOut));
			if (t > 0.0F && pendingCount_ < pending_.size())
				pending_[pendingCount_++] = { t, id };
		}
		if (sideX_ < sideY_) {
			entered_ = sideX_;
			sideX_ += deltaX_;
			tile_.x += stepX_;
		} else {
			entered_ = sideY_;
			sideY_ += deltaY_;
			tile_.y += stepY_;
		}
	}
	// The nearest first: the nearer one's rows stay.
	std::sort(pending_.begin(), pending_.begin() + pendingCount_, [](const auto &a, const auto &b) { return a.first < b.first; });
	size_t kept = 0;
	for (size_t i = 0; i < pendingCount_; i++) {
		if (pending_[i].first < until)
			DrawTower(Towers[pending_[i].second], out, filled, depthBuffer, nearest);
		else
			pending_[kept++] = pending_[i];
	}
	pendingCount_ = kept;
}

void TowerColumn::DrawTower(const Tower &tower, const Surface &out, std::vector<uint8_t> &filled, std::vector<float> &depthBuffer, float &nearest)
{
	const Projection &proj = *proj_;
	const int x = x_;
	const float ceiling = CleanCeiling();
	const float capFrom = ceiling - CapDrop;
	// Its rings from the floor up: the plinth, the drum, the rim of its cap; each where the ray meets it.
	struct Ring {
		float radius;
		float z0;
		float z1;
		CleanSurface surface;
		float t;
	};
	std::array<Ring, 3> rings { {
	    { tower.radius + PlinthOut, 0.0F, PlinthHeight, CleanSurface::Pillar, 0.0F },
	    { tower.radius, PlinthHeight, capFrom, CleanSurface::Pillar, 0.0F },
	    { tower.radius + CapOut, capFrom, ceiling, CleanSurface::Stone, 0.0F },
	} };
	auto rowAt = [&](float h, float t) { return static_cast<float>(proj.horizon) + (EyeHeight - h) * proj.focal / t; };
	CleanSampler sampler;
	for (Ring &ring : rings) {
		ring.t = EnterCircle(pos_, ray_, tower.cx, tower.cy, ring.radius);
		if (ring.t <= 0.0F)
			continue;
		const float t = ring.t;
		const Vec2 p { pos_.x + ray_.x * t, pos_.y + ray_.y * t };
		const float ax = p.x - tower.cx;
		const float ay = p.y - tower.cy;
		// The stone round it a whole number of times (DrumRepeat a repeat), from its angle.
		const float circumference = 6.2831853F * ring.radius;
		const float uScale = std::max(1.0F, std::round(circumference / DrumRepeat)) * DrumRepeat / circumference;
		const float u = std::atan2(ay, ax) * ring.radius * uScale;
		const float length = std::max(std::hypot(ax, ay), 1e-4F);
		const float light = CleanLightAt(p.x + ax / length * 0.05F, p.y + ay / length * 0.05F);
		if (ring.surface == CleanSurface::Pillar && ring.z0 == PlinthHeight)
			nearest = std::min(nearest, t); // the drum, floor to ceiling: what is behind it in this column is hidden
		PrepareCleanSampler(sampler, ring.surface, t / proj.focal);
		const int top = std::max(0, static_cast<int>(std::ceil(rowAt(ring.z1, t) - 0.5F)));
		const int bottom = std::min(proj.height - 1, static_cast<int>(rowAt(ring.z0, t)));
		for (int y = top; y <= bottom; y++) {
			if (filled[y] != 0)
				continue;
			const float h = EyeHeight + static_cast<float>(proj.horizon - y) / proj.focal * t;
			*out.at(x, y) = light >= static_cast<float>(LightsMax) ? 0 : sampler.At(u, -h, LightLevel(light, x, y), x, y);
			filled[y] = 1;
			depthBuffer[static_cast<size_t>(y) * proj.width + x] = t;
		}
	}
	// Between the rings: the top of the plinth, seen from above, and the underside of the cap, seen from below.
	auto flat = [&](float z, float from, float to, float darkness) {
		if (from <= 0.0F || to <= from)
			return;
		const bool below = z < EyeHeight;
		const int y0 = std::max(0, static_cast<int>(std::ceil(std::min(rowAt(z, from), rowAt(z, to)))));
		const int y1 = std::min(proj.height - 1, static_cast<int>(std::max(rowAt(z, from), rowAt(z, to))));
		PrepareCleanSampler(sampler, CleanSurface::Stone, (from + to) * 0.5F / proj.focal);
		for (int y = y0; y <= y1; y++) {
			if (filled[y] != 0 || (below ? y <= proj.horizon : y >= proj.horizon))
				continue;
			const float t = (below ? EyeHeight - z : z - EyeHeight) * proj.focal / static_cast<float>(below ? y - proj.horizon : proj.horizon - y);
			if (t < from || t > to)
				continue;
			const float px = pos_.x + ray_.x * t;
			const float py = pos_.y + ray_.y * t;
			const float light = CleanLightAt(px, py) + darkness;
			*out.at(x, y) = light >= static_cast<float>(LightsMax) ? 0 : sampler.At(px, py, LightLevel(light, x, y), x, y);
			filled[y] = 1;
			depthBuffer[static_cast<size_t>(y) * proj.width + x] = t;
		}
	};
	if (rings[1].t > 0.0F) {
		flat(PlinthHeight, rings[0].t, rings[1].t, 0.0F);
		flat(capFrom, rings[2].t, rings[1].t, CapUndersideDarkness + 1.5F);
	}
}

void DrawCatacombsFloorAndCeiling(const Surface &out, const Projection &proj, std::vector<float> &depthBuffer)
{
	const Vec2 rayLeft { proj.dir.x - proj.plane.x, proj.dir.y - proj.plane.y };
	const Vec2 rayRight { proj.dir.x + proj.plane.x, proj.dir.y + proj.plane.y };
	const float ceiling = CleanCeiling();
	const bool holes = HasFloorHoles();
	EnsureArtCache();
	for (int y = 0; y < proj.height; y++) {
		const bool isFloor = y > proj.horizon;
		uint8_t *dst = out.at(0, y);
		const int rowsFromHorizon = isFloor ? y - proj.horizon : proj.horizon - y;
		if (rowsFromHorizon == 0) {
			std::fill_n(dst, proj.width, 0);
			continue;
		}
		const float heightAboveOrBelow = isFloor ? EyeHeight : ceiling - EyeHeight;
		const float rowDistance = heightAboveOrBelow * proj.focal / static_cast<float>(rowsFromHorizon);
		const float stepX = rowDistance * (rayRight.x - rayLeft.x) / static_cast<float>(proj.width);
		const float stepY = rowDistance * (rayRight.y - rayLeft.y) / static_cast<float>(proj.width);
		float wx = proj.pos.x + rowDistance * rayLeft.x;
		float wy = proj.pos.y + rowDistance * rayLeft.y;
		// The detail level: the footprint across (one pixel) and in depth (where the rows bunch up), averaged.
		const float across = rowDistance / proj.focal;
		const float deep = rowDistance * rowDistance / (heightAboveOrBelow * proj.focal);
		const float worldPerPixel = std::sqrt(across * deep);
		if (!isFloor) {
			CleanSampler sampler;
			PrepareCleanSampler(sampler, CleanSurface::Ceiling, worldPerPixel);
			const bool holed = !CeilingHoles.empty();
			for (int x = 0; x < proj.width; x++, wx += stepX, wy += stepY) {
				if (holed && IsCeilingHole(wx, wy)) {
					dst[x] = 0; // open over the stairs up
					continue;
				}
				const float light = CleanLightAt(wx, wy) + CeilingDarkness;
				dst[x] = light >= static_cast<float>(LightsMax) ? 0 : sampler.At(wx, wy, LightLevel(light, x, y), x, y);
			}
			continue;
		}
		float *depthRow = &depthBuffer[static_cast<size_t>(y) * proj.width];
		const float depth = rowDistance * 1.03F; // see FloorDepthBias in fp_view.cpp
		Point cachedTile { -1, -1 };
		ArtSampler sampler;
		for (int x = 0; x < proj.width; x++, wx += stepX, wy += stepY) {
			const int tx = FloorToInt(wx);
			const int ty = FloorToInt(wy);
			if (tx < 0 || ty < 0 || tx >= MAXDUNX || ty >= MAXDUNY) {
				dst[x] = 0;
				continue;
			}
			if (holes && IsFloorHole(wx, wy)) {
				dst[x] = 0; // the pit of the stairs down: drawn into by its mesh
				continue;
			}
			if (tx != cachedTile.x || ty != cachedTile.y) {
				cachedTile = { tx, ty };
				PrepareArtSampler(sampler, FloorTexture(FloorPieces[static_cast<size_t>(ty) * MAXDUNX + tx]), worldPerPixel);
			}
			depthRow[x] = depth;
			const float light = CleanLightAt(wx, wy);
			dst[x] = light >= static_cast<float>(LightsMax) ? 0 : sampler.At(wx - static_cast<float>(tx), wy - static_cast<float>(ty), LightLevel(light, x, y), x, y);
		}
	}
}

} // namespace devilution::fp
