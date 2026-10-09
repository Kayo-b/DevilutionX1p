/**
 * @file fp/fp_world.hpp
 *
 * The things that stand in the first-person world as billboards, shared by rendering and picking.
 */
#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "engine/clx_sprite.hpp"
#include "engine/point.hpp"
#include "fp/fp_math.hpp"
#include "fp/fp_raycast.hpp"

namespace devilution::fp {

enum class EntityKind : uint8_t {
	Monster,
	Towner,
	Object,
	Item,
	Missile,
	Corpse,
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
	/** Colour translation used instead of lighting (unique monsters, petrified), as the iso renderer does. */
	const uint8_t *trn;
	bool lit; // apply the light table of the entity's tile
};

/** @brief Light table index of a tile (LightsMax outside the dungeon). */
[[nodiscard]] uint8_t LightAt(Point tile);

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
