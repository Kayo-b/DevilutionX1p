/**
 * @file fp/fp_catacombs.hpp
 *
 * The catacombs (levels 5-8) in first person, drawn as part of the clean dungeon (fp_dungeon.hpp) from their own art.
 *
 * The catacombs' iso art is dry-laid rubble: walls of rough stone in uneven courses under a cap of flagstones, round
 * towers of the same stone at the ends and the inside corners of the walls (and alone in rooms, and under the
 * arches), flat arches of a beam with curved ends from a tower to a wall, square door frames, and spiral stairs round
 * a tower. Its tiles are told apart by the tiles of the level's map (`dungeon`, ids from 1 as in l2.til), not by their
 * art.
 *
 * Cut out of the level's art when it loads (nothing of it is shipped): every wall face wears its own tile's face art
 * (the decorated ones keep their niches, blood and cracks), the faces the iso camera never drew (and those round the
 * doorways) the plain wall pair's by their place along the wall; the door leaves the closed door's frame, its hinges'
 * iron on; the insides of the doorways the dressed stone of a door's jamb; each tile's floor its own floor art (the painted shadows of towers and arches taken
 * out); the ceiling the flagstones of a tower's cap. All of it is made four times finer by the game's xBR
 * (UpscaledTexture), keeps its palette colours (lit through the game's light tables, as the original) and has detail
 * levels for the distance.
 */
#pragma once

#include <array>
#include <cstdint>
#include <utility>
#include <vector>

#include "engine/light_tables.hpp"
#include "engine/point.hpp"
#include "engine/surface.hpp"
#include "fp/fp_dungeon.hpp"
#include "fp/fp_math.hpp"
#include "fp/fp_raycast.hpp"
#include "fp/fp_world.hpp"

namespace devilution::fp {

[[nodiscard]] bool IsCatacombs();

/**
 * @brief A texture of palette art (its colours kept, lit through the light tables when drawn), row 0 the top, with
 * detail levels (each 2 x 2 texels averaged and mapped back to the nearest palette colour). Its width is a power of
 * two (it repeats across); its rows are clamped.
 */
struct ArtTexture {
	struct Mip {
		int width = 0;
		int height = 0;
		int shift = 0; // log2 of the width
		std::vector<uint8_t> texels;
	};
	std::vector<Mip> mips;
	float texelsPerTileU = 0;
	float texelsPerTileV = 0;
};

/** @brief Samples an ArtTexture with the detail level for a pixel's footprint; up close, the sample point is dithered. */
struct ArtSampler {
	const uint8_t *texels = nullptr;
	int maskU = 0;
	int lastRow = 0;
	int shift = 0;
	float scaleU = 1;
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
		const int iv = std::clamp(FloorToInt(tv), 0, lastRow);
		return LightTables[level][texels[static_cast<size_t>(iv) << shift | static_cast<size_t>(iu)]];
	}
};
void PrepareArtSampler(ArtSampler &sampler, const ArtTexture &texture, float worldPerPixel);

/** @brief A tower's drum: its middle and its radius (its plinth and the rim of its cap are a little wider). */
struct Tower {
	float cx;
	float cy;
	float radius;
};

/**
 * @brief The catacombs' towers in one column of the wall pass (DrawCleanWalls), drawn there rather than as meshes: the
 * ray is walked through the grid, and each tower whose circle it meets (its plinth, its drum, the rim of its cap, each
 * at its own radius; the top of the plinth and the underside of the cap between them) is drawn nearest first into the
 * rows not drawn yet, before the walls behind it. Begin it for each column, then DrawUntil the distance of each wall
 * the pass draws (MaxRayDistance after the last).
 */
class TowerColumn {
public:
	void Begin(const Projection &proj, int x);
	/** Draws the towers the ray meets nearer than `until` (perpendicular depth); a drum's depth into `nearest`. */
	void DrawUntil(float until, const Surface &out, std::vector<uint8_t> &filled, std::vector<float> &depthBuffer, float &nearest);

private:
	void DrawTower(const Tower &tower, const Surface &out, std::vector<uint8_t> &filled, std::vector<float> &depthBuffer, float &nearest);

	const Projection *proj_ = nullptr;
	int x_ = 0;
	Vec2 pos_ {};
	Vec2 ray_ {};
	Point tile_ {};
	float deltaX_ = 0;
	float deltaY_ = 0;
	int stepX_ = 0;
	int stepY_ = 0;
	float sideX_ = 0;
	float sideY_ = 0;
	float entered_ = 0;
	bool walking_ = false;
	std::array<uint16_t, 24> seen_ {};
	size_t seenCount_ = 0;
	std::array<std::pair<float, uint16_t>, 12> pending_ {};
	size_t pendingCount_ = 0;
};

/** @brief Tells the catacombs' tiles apart (fp_dungeon.cpp's ClassifyTiles): walls, towers, doorways, props. */
void ClassifyCatacombsTiles(std::vector<CleanKind> &kinds, std::vector<float> &heights);

/**
 * @brief The art a wall face wears: the face of a wall tile looking towards +x (0), -x (1), +y (2) or -y (3), one tile
 * across, from the floor (its last row) up to the ceiling (row 0). Null if the tile is not a catacombs wall.
 */
[[nodiscard]] const ArtTexture *CatacombsFaceArt(Point tile, int facing);

/** @brief The radius of the catacombs' towers out in the open (the arches resting on them start at their drums). */
[[nodiscard]] float CatacombsTowerRadius();
/** @brief Half the thickness of the catacombs' arches. */
[[nodiscard]] float CatacombsArchHalfThickness();
/**
 * @brief The height (drawn tiles) of the underside of a catacombs arch at `s` along it, between its supports at `a`
 * and `b`: a flat beam under the ceiling, curving down into the supports at its ends.
 */
[[nodiscard]] float CatacombsArchUnderside(float s, float a, float b);

/** @brief The catacombs' doorways: flat-topped, the size of the leaf the iso draws in them. */
[[nodiscard]] CleanOpening CatacombsDoorOpening();

/**
 * @brief The texture of one of the catacombs' surfaces cut out of the level's art (the ceiling's and stone's
 * flagstones, the wall, the floor), with its palette colours. False for the others (and outside the catacombs).
 */
bool CatacombsArtTexture(CleanSurface surface, CleanSourceTexture &out);

/**
 * @brief The catacombs' own geometry for the mesh pass (fp_dungeon.cpp's BuildCleanGeometry): the stairs up winding
 * round their towers, the stairs down in their trenches (the floor cut open over them: `holes`, x0, y0, x1, y1), the
 * scaffolds. (Their towers are drawn by the wall pass: TowerColumn.)
 */
void AddCatacombsGeometry(std::vector<CleanQuad> &quads, std::vector<std::array<float, 4>> &holes);

/**
 * @brief The flame of a wall torch (its sprite's animation without the iron and wood the frames share), on top of its
 * mesh, as an upright sprite turned to the camera; false if it is not one of the catacombs' wall torches.
 */
bool CatacombsTorchFlame(const Object &object, ClxSprite frame, Entity &entity);

/** @brief Draws the catacombs' floor (each tile its own art) and their flagstone ceiling. */
void DrawCatacombsFloorAndCeiling(const Surface &out, const Projection &proj, std::vector<float> &depthBuffer);

} // namespace devilution::fp
