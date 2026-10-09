/**
 * @file fp/fp_textures.hpp
 *
 * Wall and floor textures for the first-person view, extracted from the level's own iso tile art.
 *
 * Each dPiece column is rendered once with the game's tile renderer. The wall art along the tile's two
 * back edges is sheared 2:1 in iso; un-shearing it gives a flat face texture per edge. The floor diamond
 * is un-rotated into a square.
 */
#pragma once

#include <cstdint>
#include <vector>

namespace devilution::fp {

/** @brief Width of a wall face texture (one tile edge spans 32px in iso). */
constexpr int FaceTextureWidth = 32;
/** @brief Size of a piece's whole iso column in pixels. */
constexpr int ColumnWidth = 64;
constexpr int ColumnPixelHeight = 256; // MicroTileLen / 2 rows of 32px micros
/** @brief Size of a floor texture (a tile is a 64x32 diamond in iso). */
constexpr int FloorTextureSize = 32;
/** @brief Texture pixel that has no art (shows the fallback texture instead). */
constexpr int16_t NoPixel = -1;

struct PieceTextures {
	/** Face along the tile's x edge (iso left column), FaceTextureWidth x FaceTextureHeight(), row 0 = top. */
	std::vector<int16_t> faceX;
	/** Face along the tile's y edge (iso right column). */
	std::vector<int16_t> faceY;
	/** Per face texel: 1 where the art has an opening (no pixel, but art above it), e.g. inside an arch. */
	std::vector<uint8_t> holesX;
	std::vector<uint8_t> holesY;
	std::vector<int16_t> floor;
	bool faceXUsable;
	bool faceYUsable;
	/** Rows from the base of the face to its highest opaque pixel (the wall height the art shows), 0 if empty. */
	int faceXRows;
	int faceYRows;
	/** The whole iso column (ColumnWidth x ColumnHeight, row 0 = top), for drawing the piece as a billboard. */
	std::vector<int16_t> column;
	/** Whether the piece draws art above its floor diamond (a tree, house, rock) rather than a flat floor. */
	bool hasTallArt;
	/** Dense tall art (walls, roofs) that is better rebuilt as part of a 3D building than drawn as a flat billboard. */
	bool isStructure;
	/** A narrow upright piece of art (round pillar, torch stand) that looks the same from every side, so it can be a billboard. */
	bool isPillar;
};

/** @brief Height in pixels of wall face textures on the current level (depends on the tileset). */
[[nodiscard]] int FaceTextureHeight();

/** @brief Textures for a dPiece id, built on first use and cached until the level changes. */
[[nodiscard]] const PieceTextures &GetPieceTextures(uint16_t pieceId);

/** @brief A wall face to use where a piece has no usable art for the face being drawn. */
[[nodiscard]] const std::vector<int16_t> &GetFallbackFace();

/**
 * @brief Height in face rows (pixels) of the level's walls, taken from the art of its wall pieces.
 *
 * The iso art of a wall is only this tall; scaling walls beyond it leaves a band of filler at the top.
 */
[[nodiscard]] int LevelWallRows();

/** @brief Floor of the level's most common walkable piece, used where a piece has no ground art of its own. */
[[nodiscard]] const std::vector<int16_t> &GetGroundFloor();

#ifdef _DEBUG
/** @brief Debug: writes a PPM contact sheet (iso column, face X, face Y) of the most common solid pieces. */
void DumpSolidPieces(const char *path, int perRow, int rows);
#endif

} // namespace devilution::fp
