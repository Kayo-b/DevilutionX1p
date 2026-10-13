/**
 * @file fp/fp_dungeon.hpp
 *
 * The clean dungeon: how the cathedral (levels 1-4), the catacombs (5-8) and Hellfire's crypt (21-24) look in first
 * person.
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

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "engine/point.hpp"
#include "engine/surface.hpp"
#include "fp/fp_raycast.hpp"

namespace devilution::fp {

/** @brief Whether the current level is drawn as the clean dungeon (cathedral, catacombs, crypt). */
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
	Pillar,
	Arch,    // the inside of doorways and arches
	Door,    // a door leaf: u, v 0..1 over the leaf
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

/** @brief A flat quad of the clean geometry (pillars, arches, steps): corners in world tiles, z in drawn tiles. */
struct CleanQuad {
	std::array<std::array<float, 3>, 4> p;
	std::array<float, 3> normal; // outward
	std::array<std::array<float, 2>, 4> uv; // texture position (tiles) of each corner
	CleanSurface surface;
	float darkness = 0; // light levels added (the inside of a stairwell)
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
