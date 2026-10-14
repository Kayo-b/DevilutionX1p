/**
 * @file fp/fp_hell.hpp
 *
 * Hell (levels 13-16) in first person, drawn as part of the clean dungeon (fp_dungeon.hpp) with its own kit.
 *
 * Hell's iso art is sculpture rather than masonry: raised troughs of blood rimmed by horned bone beams, their sides
 * panels of flesh between spine posts; flesh walls in the same frame; lone spine columns; bone spikes growing out of
 * the ash; rib cages over the stairs; and outside the rooms a lake of blood. Its tiles are told apart by the tiles of
 * the level's map (`dungeon`), not by their art. Everything is drawn low (HellWallTop, under the eye), so the blood
 * on top of the troughs shows, under a dark red sky (fp_sky.hpp).
 *
 * What is cut out of the level's own art when it loads: the flesh of the walls' sides (a texture of the level's set,
 * slot `wall`), the blood (kept in the art's palette colours, which hell cycles), the floors (each tile its own art,
 * the painted shadows of walls taken out), the crowns of the wall posts (cards) and of the spine columns
 * (billboards), and the shapes of the spikes' horns. The bone of the beams, posts, horns and spikes is the stairs to
 * hell's bone texture graded to the art's bone (`hell/pillar` and `hell/stone` in dungeontex.bin, tinted to the
 * level's palette). The stairs' rib cages are the stairs to hell model (fp_buildings.cpp's BuildHellStairs).
 */
#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "engine/point.hpp"
#include "engine/surface.hpp"
#include "fp/fp_dungeon.hpp"
#include "fp/fp_raycast.hpp"
#include "fp/fp_world.hpp"

namespace devilution::fp {

[[nodiscard]] bool IsHell();

/** @brief Height (drawn tiles) of hell's walls: the troughs, the flesh walls and the blood lake round the rooms. */
[[nodiscard]] float HellWallTop();
/** @brief How big hell's tile-art billboards (spikes, rib cages) are drawn, as a share of VerticalScale(): as low as its walls. */
[[nodiscard]] float HellArtScale();

/** @brief How far (tiles) hell's bone is drawn: beyond, it stands in the dark, and only its outline against the sky would show. */
constexpr float HellMeshReach = 26.0F;

/** @brief Tells hell's tiles apart (fp_dungeon.cpp's ClassifyTiles): walls, columns, props; the heights of its walls. */
void ClassifyHellTiles(std::vector<CleanKind> &kinds, std::vector<float> &heights);
/** @brief Whether a hell wall tile has blood on top (a trough, or the lake). */
[[nodiscard]] bool IsHellBlood(Point tile);
/** @brief Whether a tile belongs to one of hell's flights of stairs (their rib cages: fp_buildings.cpp's BuildHellStairs). */
[[nodiscard]] bool IsHellStairsTile(Point tile);
/** @brief Whether a prop tile's bone is a model (BuildHellGeometry: the spikes), so it is not a billboard. */
[[nodiscard]] bool IsHellModelledProp(Point tile);

/**
 * @brief The texture of one of hell's surfaces that is cut out of the level's art (the flesh: CleanSurface::Wall),
 * in the colours of the current palette. False for the others (and outside hell).
 */
bool HellArtTexture(CleanSurface surface, CleanSourceTexture &out);

/**
 * @brief Brings a texture of hell's set from dungeontex.bin to the level's palette: its bone (`pillar`, `stone`) was
 * graded to the bone of levels/l4data/l4_1.pal, and each hell level has one of four palettes, whose bone is tan, pink,
 * grey-blue or olive. Its colours are scaled by the level's bone over l4_1's. Nothing for the others.
 */
void HellTintTexture(CleanSurface surface, CleanSourceTexture &texture);

/**
 * @brief A prop's billboard art (its piece's iso column, ColumnWidth wide) without the ground it stands on: the ash of
 * its floor diamond is taken out, so only the bone stands up (the floor pass draws the ground).
 */
[[nodiscard]] const std::vector<int16_t> &HellPropArt(uint16_t piece);

/** @brief An art cut-out drawn on a card in the mesh pass (CleanQuad::sprite): palette pixels, row 0 the top, NoPixel clear. */
struct HellSprite {
	std::vector<int16_t> pixels;
	int width = 0;
	int height = 0;
	/** Its UpscaledTexture, for drawing up close (made with it). */
	const std::vector<int16_t> *fine = nullptr;
	/**
	 * How much of each texel is kept, 0..255, where it was cut out of art that goes on (empty: all of it): drawn through
	 * an ordered dither, so a cut fades out instead of showing as a straight line. Its own outline stays sharp.
	 */
	std::vector<uint8_t> alpha;
	/**
	 * Where it has art: rectangles (x0, y0, x1, y1, ends exclusive) of texels covering every drawn one with a texel to
	 * spare round it. Its cards are made of these alone, so the clear texels between them cost nothing to draw.
	 */
	std::vector<std::array<int, 4>> rects;
};
/** @brief One of the level's cut-outs (BuildHellGeometry makes them), null if there is no such one. */
[[nodiscard]] const HellSprite *HellSpriteAt(int index);

/** @brief The crowns of hell's spine columns near the viewer: billboards of their art, on top of their mesh spines. */
void CollectHellCrowns(Vec2 viewerPos, std::vector<Entity> &entities);

/**
 * @brief Draws hell's floor (each tile its own art, smoothly lit) where nothing has been drawn yet (DrawHellWalls goes
 * first); above the horizon is left for the sky.
 */
void DrawHellFloor(const Surface &out, const Projection &proj, std::vector<float> &depthBuffer);
/** @brief Draws hell's walls: their flesh sides and their tops (blood, or flesh), front to back over the floor. */
void DrawHellWalls(const Surface &out, const Projection &proj, std::vector<float> &zBuffer, std::vector<float> &depthBuffer);
/** @brief Hell's bone: the beams round the tops of the walls, the spine posts along them, the columns, their horns. */
void BuildHellGeometry(std::vector<CleanQuad> &quads);

} // namespace devilution::fp
