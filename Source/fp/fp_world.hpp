/**
 * @file fp/fp_world.hpp
 *
 * The things that stand in the first-person world as billboards, shared by rendering and picking.
 */
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <vector>

#include "engine/clx_sprite.hpp"
#include "engine/point.hpp"
#include "fp/fp_math.hpp"
#include "fp/fp_raycast.hpp"

namespace devilution {
struct Object;
} // namespace devilution

namespace devilution::fp {

enum class EntityKind : uint8_t {
	Monster,
	Towner,
	Object,
	Item,
	Missile,
	Corpse,
	Prop, // level art drawn as a billboard (town buildings, trees, rocks)
	Hero, // the player's own hero, seen from behind in third person
};

struct Entity {
	EntityKind kind;
	int id;     // index into Monsters / Towners / Objects / Items (unused for missiles and corpses)
	Point tile; // logical tile (what the game's cursor targeting expects) and light sample
	Vec2 pos;   // world position the sprite is centred on
	float width;
	float height;
	float bottom; // height of the sprite's bottom edge above the floor (negative: extends below)
	std::optional<ClxSprite> sprite;
	/** Alternative to sprite: raw palette pixels (-1 transparent), row 0 = top. */
	const std::vector<int16_t> *pixels = nullptr;
	int pixelsWidth = 0;
	int pixelsHeight = 0;
	/** Colour translation used instead of lighting (unique monsters, petrified), as the iso renderer does. */
	const uint8_t *trn;
	bool lit; // apply the light table of the entity's tile
	/** Drawn lying on the floor from the original iso art (corpses), instead of standing up as a billboard. */
	bool flat = false;
	/** Drawn some other way (as a mesh): kept only so it can be clicked. */
	bool hidden = false;
	/**
	 * Stands as an upright quad on its foot (pos), turned to face the camera, each column at its own depth: wide
	 * art (a tree) beside the camera then recedes as it should instead of filling the view.
	 */
	bool facing = false;
	/** With `facing`: the quad runs this way (a unit vector along it) instead of across the view: a wall torch hangs flat on its wall. */
	bool fixedAlong = false;
	Vec2 along {};
};

/**
 * @brief Decodes a CLX frame into a top-to-bottom pixel buffer; -1 marks transparent pixels.
 *
 * CLX pixel data runs bottom-to-top, left-to-right, and runs may continue onto the next row.
 */
void DecodeClx(ClxSprite sprite, std::vector<int16_t> &pixels);

/** @brief Number of empty (or shadow-only, colour 0) pixel rows under the lowest visible pixel of a sprite. */
[[nodiscard]] int SpriteBottomPadding(ClxSprite sprite);

/**
 * @brief Where a wall torch (OBJ_TORCHL, R, L2, R2) hangs in the clean dungeon: a little in front of the face of its
 * wall, which looks towards `normal` (zero if the torch has no wall beside it).
 */
[[nodiscard]] Vec2 WallTorchPlacement(const Object &object, Vec2 &normal);

/** @brief Light table index of a tile (LightsMax outside the dungeon). */
[[nodiscard]] uint8_t LightAt(Point tile);

/**
 * @brief The light table for a light level (0: lit .. LightsMax: dark) in first person. The game's own, but in town
 * one made from the palette: each colour darkened to the nearest palette colour. The town palette is not laid out in
 * 16-colour shade ramps, so the game's tables (only ever used fully lit in town) turn its colours into unrelated
 * ones when darker: greys into reds, greens and blacks.
 */
inline const std::array<uint8_t, 256> *FpLightTablesNow = nullptr;

/** @brief Points FpLightTable at the tables for the current level and palette: once a frame, before drawing. */
void RefreshFpLightTables();

inline const std::array<uint8_t, 256> &FpLightTable(int level)
{
	if (FpLightTablesNow == nullptr)
		RefreshFpLightTables();
	return FpLightTablesNow[std::clamp(level, 0, 15)];
}

/** @brief Whether a tile's tall art is drawn as a billboard of its own iso column (a prop: what no mesh replaces). */
[[nodiscard]] bool IsPropTile(Point tile);

/** @brief Collects all billboard entities that can currently be seen (walls are not considered here). */
void CollectEntities(Vec2 viewerPos, std::vector<Entity> &entities);

[[nodiscard]] bool IsSelectableKind(EntityKind kind);

/** @brief Screen-space footprint of a billboard. */
struct BillboardRect {
	bool visible;
	float depth;
	int left;
	int right; // exclusive
	int top;
	int bottom; // exclusive
};

[[nodiscard]] BillboardRect ProjectBillboard(const Projection &projection, const Entity &entity);

} // namespace devilution::fp
