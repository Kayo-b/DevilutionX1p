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

#include <array>
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
/**
 * @brief Face rows below the edge line of the tile: a wall stands on the middle of its tile, half a tile in front of
 * the edge (16 rows lower in iso, the depth of the wall's lit top, its cap), with the plinths and column bases at its
 * foot. Faces carry them as their bottom rows; the bottom row is on the floor.
 */
constexpr int FaceBaseRows = 16;
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
	/**
	 * Per base texel (FaceBaseRows x FaceTextureWidth, row 0 = just below the foot): 1 where the foot of the face lies
	 * outside the piece's diamond, so the next tile along the wall draws it (in its spill).
	 */
	std::vector<uint8_t> cutX;
	std::vector<uint8_t> cutY;
	/** The cut base texels of the previous tile along the wall, as this piece draws them (NoPixel: floor). */
	std::vector<int16_t> spillX;
	std::vector<int16_t> spillY;
	std::vector<int16_t> floor;
	bool faceXUsable;
	bool faceYUsable;
	/**
	 * Recessed part of each face (the inside of a blind arch or niche), read off the art: per face texture column,
	 * the first row that is recessed (the face height if none). Empty if the face has no recess.
	 */
	std::vector<int> recessX;
	std::vector<int> recessY;
	float recessDepthX = 0; // how deep the recess goes, in tiles
	float recessDepthY = 0;
	/** Rows from the bottom of the face (FaceBaseRows below the foot of the wall) to its highest opaque pixel, 0 if empty. */
	int faceXRows;
	int faceYRows;
	/** The whole iso column (ColumnWidth x ColumnHeight, row 0 = top), for drawing the piece as a billboard. */
	std::vector<int16_t> column;
	/**
	 * Town pieces with tall art: the column without its ground (the pixels the floor alone draws), and the floor
	 * without what stands on it. For the props rebuilt as meshes (trees, rocks, walls), whose ground stays below them.
	 */
	std::vector<int16_t> upright;
	std::vector<int16_t> groundFloor;
	bool uprightGroundRemoved = false; // see RemoveUprightGround
	/** Whether the piece draws art above its floor diamond (a tree, house, rock) rather than a flat floor. */
	bool hasTallArt;
	/** Dense tall art (walls, roofs) that is better rebuilt as part of a 3D building than drawn as a flat billboard. */
	bool isStructure;
	/** A narrow upright piece of art (round pillar, torch stand) that looks the same from every side, so it can be a billboard. */
	bool isPillar;
	/**
	 * A solid piece that is really a free-standing column (thin shaft under a wider capital). It has no usable wall
	 * faces, so it is drawn as an upright billboard of its own art instead of as a wall cube.
	 */
	bool isFreeStanding;
	/** A solid piece that is only floor art (lava, water): blocks walking but is seen as floor, not as a wall. */
	bool isFlatSolid = false;
};

/** @brief Whether wall recesses (blind arches, niches) are carved into the walls (debug toggle, on by default). */
[[nodiscard]] bool WallRecessesEnabled();
void SetWallRecessesEnabled(bool enabled);

/** @brief Height in pixels of wall face textures on the current level (depends on the tileset). */
[[nodiscard]] int FaceTextureHeight();

/** @brief Textures for a dPiece id, built on first use and cached until the level changes. */
[[nodiscard]] const PieceTextures &GetPieceTextures(uint16_t pieceId);

/**
 * @brief For a town piece whose floor alone draws nothing (a solid piece: a wall, a boulder), takes the ground out of
 * its upright art by colour: the pixels of its ground diamond in colours the town's ground uses (`ground`). Once
 * per piece and level.
 */
void RemoveUprightGround(uint16_t pieceId, const std::array<bool, 256> &ground);

/** @brief A wall face to use where a piece has no usable art for the face being drawn. */
[[nodiscard]] const std::vector<int16_t> &GetFallbackFace();
/** @brief Filler wall face for a tile in a run: two neighbouring faces of the level's commonest straight wall, alternating with the parity of the tile's position along the wall. */
[[nodiscard]] const std::vector<int16_t> &GetFallbackFace(int parity);
/** @brief Whether the level has a plain straight wall to use as filler (hell's bone walls do not). */
[[nodiscard]] bool HasPlainWall();
/**
 * @brief A number that changes whenever the game's logical palette does (checked once per frame by
 * NotePaletteChange). Colour tables built from the palette compare against it and rebuild.
 */
[[nodiscard]] uint32_t PaletteVersion();
void NotePaletteChange();
/** @brief Debug: only the floor part of a piece's iso column (its bottom 32 rows), NoPixel elsewhere. */
[[nodiscard]] std::vector<int16_t> RenderPieceFloor(uint16_t pieceId);
/** @brief The pieces of that pair and which of their faces runs along the wall; false if there is none. */
bool GetPlainWallPair(uint16_t &first, uint16_t &second, bool &useX);

/**
 * @brief Height in face rows (pixels) of the level's walls, taken from the art of its wall pieces.
 *
 * The iso art of a wall is only this tall; scaling walls beyond it leaves a band of filler at the top.
 */
[[nodiscard]] int LevelWallRows();

/**
 * @brief Face rows at the top of the level's wall art that show the top of the wall (lit, seen from above in iso), 0
 * if it has none. First person cuts them off.
 */
[[nodiscard]] int WallCapRows();
/** @brief Face rows a wall whose art is `rows` rows tall shows: its art less the cap (tall walls only). */
[[nodiscard]] int WallShownRows(int rows);
/** @brief Face rows shown per row of the wall's own height for a wall whose art is `rows` rows tall (1: unscaled). */
[[nodiscard]] float WallArtScale(int rows);
/** @brief Face row at a height (tiles, before VerticalScale) on a wall whose art is `rows` rows tall, without its cap. */
[[nodiscard]] int WallArtRow(float height, int rows = LevelWallRows());
/** @brief The height (tiles, before VerticalScale) of a face row on such a wall: the inverse of WallArtRow. */
[[nodiscard]] float WallArtHeight(int row, int rows = LevelWallRows());

/**
 * @brief Finds the outline of an arch (blind arch, niche, doorway) in a face `width` texels wide whose wall art is
 * `rows` rows tall: per column, the first face row inside it (FaceTextureHeight() if the column is not), shaped to
 * rise to the arch's crown and fall to both sides. False if there is none.
 */
bool FindArchOutline(const std::vector<int16_t> &face, int width, int rows, std::vector<int> &top);

/** @brief How many times finer UpscaledTexture is than the art, each way. */
constexpr int TextureUpscale = 4;
/** @brief Art texels per screen pixel below which the art is drawn from its UpscaledTexture (it is blown up). */
constexpr float UpscaleBelow = 0.6F;
/**
 * @brief A texture (`width` texels wide, row 0 = top) at TextureUpscale times the resolution, its edges smoothed by xBR
 * (no blur), for drawing up close. Cached per level by the texture's storage, which must outlive the level.
 */
[[nodiscard]] const std::vector<int16_t> &UpscaledTexture(const std::vector<int16_t> &texture, int width);
/** @brief Drops a texture's UpscaledTexture (call before its storage is freed or changed). */
void ForgetUpscaledTexture(const std::vector<int16_t> &texture);

/** @brief The recess (see PieceTextures::recessX) of the filler face GetFallbackFace(parity), null if it has none. */
[[nodiscard]] const std::vector<int> *GetFallbackRecess(int parity, float &depth);

/** @brief Width of an arch face: from the middle of the tile before the arch's tile to the middle of the one after. */
constexpr int ArchFaceWidth = 2 * FaceTextureWidth;

/**
 * @brief An arch over a walkable tile (Diablo's special arch cels, dSpecial): its art read on the plane the arch
 * stands on (the middle of the tile, like the walls), ArchFaceWidth x FaceTextureHeight(), row 0 = top, NoPixel where
 * it is open. Column u is at (u + 0.5) / FaceTextureWidth - 0.5 tiles along the arch from the tile's start.
 */
struct SpecialArch {
	std::vector<int16_t> face;
	std::vector<int16_t> column; // the cel as drawn (ColumnWidth x ColumnHeight()), for resampling (SpecialArchFace)
	bool alongX = true; // runs along world x (the iso right edge), else along y
	bool ok = false;
};

/**
 * @brief The arch drawn by special cel `special` (dSpecial's 1-based value), cached per level. The cel draws the half
 * of the arch rising from the pillar after the tile; the pillar before it draws the other half.
 */
[[nodiscard]] const SpecialArch &GetSpecialArch(int special);
/**
 * @brief The cel's art on a plane `shiftPx` screen pixels right of the one SpecialArch::face is read on: where the
 * arch is when its pillars stand off their tiles' middles (an axis that far right in the art).
 */
[[nodiscard]] std::vector<int16_t> SpecialArchFace(const SpecialArch &arch, float shiftPx);
/**
 * @brief A piece's art on an arch's plane (see SpecialArch), shifted `sOffset` tiles (1: the piece of the tile before,
 * -1: the one after) and `shiftPx` screen pixels right (see SpecialArchFace).
 */
[[nodiscard]] std::vector<int16_t> PieceArchFace(uint16_t pieceId, bool alongX, float sOffset, float shiftPx = 0);
/** @brief Per arch face column: the row of the arch's underside (the first art above the opening), -1 where the art reaches the floor or is empty. */
[[nodiscard]] std::vector<int> ArchUnderside(const std::vector<int16_t> &face);

/** @brief Floor of the level's most common walkable piece, used where a piece has no ground art of its own. */
[[nodiscard]] const std::vector<int16_t> &GetGroundFloor();

#ifdef _DEBUG
/** @brief Debug: writes a PPM contact sheet (iso column, face X, face Y) of the most common solid pieces. */
void DumpSolidPieces(const char *path, int perRow, int rows);
#endif

} // namespace devilution::fp
