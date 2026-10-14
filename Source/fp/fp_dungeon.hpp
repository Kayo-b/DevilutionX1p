/**
 * @file fp/fp_dungeon.hpp
 *
 * The clean dungeon: how the cathedral (levels 1-4), the catacombs (5-8) and Hellfire's crypt (21-24) look in first
 * person. Hell (13-16) is drawn within it with a kit of its own (fp_hell.hpp): its kinds, texture set and geometry
 * come from there.
 *
 * These levels are not drawn from their iso art. A solid tile is a plain block of stone as tall as the ceiling (or a
 * low block where the art shows a low wall), a lone solid tile a plain pillar, a door tile a block with a round-arched
 * doorway through it, and an arch over a walkable tile (Diablo's special arch cels) a plain round arch between its
 * supports. Every surface wears its tileset's texture set (dungeontex.bin, made by fp-tools/export/
 * export_dungeon_textures.py from CC0 textures: see assets/fp/CREDITS.md), converted to the level's palette once per
 * light level so it darkens smoothly, and lit by the tile light interpolated between tile centres. The art is still
 * read to tell what a tile is (a pillar, a torch stand, a low wall), never to draw it. Stairs without a 3D model (the
 * catacombs' and the crypt's, the quest levels' entrances and exits) are an arched opening in the wall beside their
 * trigger with plain steps inside, going down or up.
 *
 * Kept as they were: the cathedral's stairs, the torch stands and the sarcophagus (3D models, fp_buildings.cpp),
 * every sprite, lava and water (drawn from their own floor art), and the door leaves (fp_doors.cpp,
 * fp_buildings.cpp), which now hang in the clean doorways.
 */
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "engine/lighting_defs.hpp"
#include "engine/point.hpp"
#include "engine/surface.hpp"
#include "fp/fp_raycast.hpp"

namespace devilution::fp {

/**
 * @brief Whether the current level is drawn as the clean dungeon (cathedral, catacombs, crypt, and hell: its own walls,
 * floor and bone structures, fp_hell.hpp).
 */
[[nodiscard]] bool IsCleanDungeon();
/** @brief Brings the tile kinds and the textures up to date with the level, its map and its palette: once a frame. */
void PrepareCleanDungeon();

/** @brief What a tile of the clean dungeon is. */
enum class CleanKind : uint8_t {
	Open,     // walkable floor (perhaps under an arch)
	Wall,     // a block of stone, floor to ceiling, or lower (CleanWallHeight)
	Pillar,   // a lone solid tile: a pillar, floor to ceiling (a mesh: rays pass it)
	Stand,    // a lone solid tile carrying a torch's flame: the kept 3D torch stand (a mesh)
	Doorway,  // a door tile: a block with a doorway through it
	Stairs,   // the wall beside a stairs trigger: a block with an arched opening and steps inside (see CleanStairs)
	Flat,     // lava or water: solid, drawn as its own floor art
	Prop,     // solid, drawn as an upright billboard of its own art (hell's bone spikes and rib cages)
};

[[nodiscard]] CleanKind CleanKindAt(Point tile);
/** @brief Height (drawn tiles) of a wall tile: the ceiling, or less for a low wall. */
[[nodiscard]] float CleanWallHeight(Point tile);
/** @brief Height (drawn tiles) of the ceiling: WallHeight() * VerticalScale(). */
[[nodiscard]] float CleanCeiling();

/** @brief A round-arched opening centred on its tile: straight sides up to the springing, then a half circle. */
struct CleanOpening {
	float halfWidth; // tiles
	float spring;    // drawn tiles above the floor
};
/** @brief The doorway through a door tile. */
[[nodiscard]] CleanOpening CleanDoorOpening();
/** @brief Height (drawn tiles) of the top of an opening at `along` (0..1 across its tile), 0 outside it. */
[[nodiscard]] float CleanOpeningTop(const CleanOpening &opening, float along);

/** @brief The surfaces of a texture set. */
enum class CleanSurface : uint8_t {
	Wall,    // a wall's body, tiled
	Base,    // the plinth band at the foot of a wall (falls back to Wall)
	Top,     // the band under the ceiling (falls back to Wall)
	Floor,
	Ceiling,
	Pillar,  // a pillar's shaft
	Stone,   // dressed stone: pillar plinths and capitals, the tops of low walls, the treads of steps (falls back to Pillar)
	Arch,    // the inside of doorways and arches
	Door,    // a door leaf: u, v 0..1 over the leaf
	Marble,  // the sarcophagus's stone (falls back to Pillar)
	Lid,     // the sarcophagus's lid: the carving at u, v 0..1 over its top (v along it), a plain margin round it for its edges (falls back to Marble)
	Iron,    // iron fittings: the sarcophagus's handles
	Ivory,   // the bases of the torch stands (falls back to Pillar)
	MarbleAlt, // a second version of Marble, for every other sarcophagus (falls back to Marble)
	IvoryAlt,  // a second version of Ivory, for every other torch stand (falls back to Ivory)
	Band,      // the cathedral's blind arches' ring: four bricks stood on end across, one brick long down (falls back to Wall)
	Count
};

/**
 * @brief Colour, lit, of a surface at (u, v) tiles across and down it.
 * @param worldPerPixel tiles covered by a screen pixel there (picks the texture's detail level, and below one texel a
 * pixel jitters its sample point by an ordered dither, which blends the texels instead of drawing them as squares)
 * @param light fractional light level (0 lit, LightsMax dark), dithered to the nearest levels
 * @param x, y screen position, for the dither
 */
[[nodiscard]] uint8_t CleanColor(CleanSurface surface, float u, float v, float worldPerPixel, float light, int x, int y);
/** @brief The light level at a point of the floor plan, interpolated between the tile centres (0 lit, LightsMax dark). */
[[nodiscard]] float CleanLightAt(float x, float y);

/** @brief A 4 x 4 Bayer matrix: thresholds 0..15 in a pattern without clumps. */
constexpr std::array<std::array<uint8_t, 4>, 4> CleanBayer { { { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 } } };

/** @brief Dither offsets in [-0.5, 0.5): one for the light, two (decorrelated) for the texture position. */
inline float DitherLight(int x, int y) { return (static_cast<float>(CleanBayer[y & 3][x & 3]) + 0.5F) / 16.0F; }
inline float DitherU(int x, int y) { return (static_cast<float>(CleanBayer[(y + 1) & 3][(x + 2) & 3]) + 0.5F) / 16.0F - 0.5F; }
inline float DitherV(int x, int y) { return (static_cast<float>(CleanBayer[(y + 3) & 3][(x + 1) & 3]) + 0.5F) / 16.0F - 0.5F; }

/** @brief A fractional light level dithered to the nearest whole one at a screen pixel. */
inline int LightLevel(float light, int x, int y)
{
	return std::clamp(static_cast<int>(light + DitherLight(x, y)), 0, static_cast<int>(LightsMax));
}

/** @brief Samples one texture of the level's set with the detail level for a pixel's footprint; up close, the sample point is dithered. */
struct CleanSampler {
	const std::array<std::vector<uint8_t>, NumLightingLevels> *lit = nullptr;
	int maskU = 0;
	int maskV = 0;
	int shift = 0; // log2 of the width
	float scaleU = 1; // texels per tile at this detail level
	float scaleV = 1;
	bool jitter = false;

	[[nodiscard]] uint8_t At(float u, float v, int level, int x, int y) const
	{
		float tu = u * scaleU;
		float tv = v * scaleV;
		if (jitter) {
			tu += DitherU(x, y);
			tv += DitherV(x, y);
		}
		const int iu = FloorToInt(tu) & maskU;
		const int iv = FloorToInt(tv) & maskV;
		return (*lit)[level][static_cast<size_t>(iv << shift | iu)];
	}
};
/** @brief Prepares a sampler of one of the level's surfaces for pixels covering `worldPerPixel` tiles. */
void PrepareCleanSampler(CleanSampler &sampler, CleanSurface surface, float worldPerPixel);

/** @brief An RGB texture for a slot of the level's set, before it is converted to the palette (rows top to bottom). */
struct CleanSourceTexture {
	int width = 0;  // powers of two
	int height = 0;
	float tilesAcross = 1; // the world size one repeat covers
	float tilesDown = 1;
	std::vector<uint8_t> rgb;
};

/** @brief The tile kinds of the level (MAXDUNX * MAXDUNY, index y * MAXDUNX + x), as of the last PrepareCleanDungeon. */
[[nodiscard]] const CleanKind *CleanKindMap();
/** @brief The wall heights of the level's tiles (see CleanWallHeight), as of the last PrepareCleanDungeon. */
[[nodiscard]] const float *CleanWallHeightMap();

/** @brief A flat quad of the clean geometry (pillars, arches, steps): corners in world tiles, z in drawn tiles. */
struct CleanQuad {
	std::array<std::array<float, 3>, 4> p;
	std::array<float, 3> normal; // outward
	std::array<std::array<float, 2>, 4> uv; // texture position (tiles) of each corner
	CleanSurface surface;
	float darkness = 0; // light levels added (the inside of a stairwell)
	int16_t sprite = -1; // instead of the surface: an art cut-out (fp_hell.hpp's HellSpriteAt), u and v 0..1 over it, clear where it has no art
	float spriteWidth = 0; // the cut-out's whole width on its card (tiles), for its detail level
};
/**
 * @brief The level's pillars, arches and stairs as quads; `stands` gets the tiles to give the kept torch stand, and
 * `holes` the floor cut open over stairs going down (x0, y0, x1, y1).
 */
void BuildCleanGeometry(std::vector<CleanQuad> &quads, std::vector<Point> &stands, std::vector<std::array<float, 4>> &holes);
/** @brief A number that changes whenever the clean dungeon has to be rebuilt (the level, its palette or its map), as of the last PrepareCleanDungeon. */
[[nodiscard]] uint32_t CleanDungeonVersion();

/** @brief Draws the floor and the ceiling (the floor writes its depth to `depthBuffer`). */
void DrawCleanFloorAndCeiling(const Surface &out, const Projection &proj, std::vector<float> &depthBuffer);
/** @brief Draws the walls, looking over low walls and through doorways. */
void DrawCleanWalls(const Surface &out, const Projection &proj, std::vector<float> &zBuffer, std::vector<float> &depthBuffer);
/** @brief Clean stairs: a trigger without a 3D model, and the wall tile beside it the opening goes into. */
struct CleanStairs {
	Point trigger;
	Point tile;
	bool down;
};
[[nodiscard]] const std::vector<CleanStairs> &CleanStairsList();

#ifdef _DEBUG
/** @brief Debug: an ASCII map of the clean tile kinds in a rectangle. */
[[nodiscard]] std::string CleanTileMap(int x0, int y0, int x1, int y1);
#endif

} // namespace devilution::fp
