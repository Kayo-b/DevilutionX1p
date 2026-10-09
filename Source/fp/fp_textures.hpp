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
/** @brief Size of a floor texture (a tile is a 64x32 diamond in iso). */
constexpr int FloorTextureSize = 32;
/** @brief Texture pixel that has no art (shows the fallback texture instead). */
constexpr int16_t NoPixel = -1;

struct PieceTextures {
	/** Face along the tile's x edge (iso left column), FaceTextureWidth x FaceTextureHeight(), row 0 = top. */
	std::vector<int16_t> faceX;
	/** Face along the tile's y edge (iso right column). */
	std::vector<int16_t> faceY;
	std::vector<int16_t> floor;
	bool faceXUsable;
	bool faceYUsable;
};

/** @brief Height in pixels of wall face textures on the current level (depends on the tileset). */
[[nodiscard]] int FaceTextureHeight();

/** @brief Textures for a dPiece id, built on first use and cached until the level changes. */
[[nodiscard]] const PieceTextures &GetPieceTextures(uint16_t pieceId);

/** @brief A wall face to use where a piece has no usable art for the face being drawn. */
[[nodiscard]] const std::vector<int16_t> &GetFallbackFace();

} // namespace devilution::fp
