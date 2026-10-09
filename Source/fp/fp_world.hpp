/**
 * @file fp/fp_world.hpp
 *
 * The things that stand in the first-person world as billboards, shared by rendering and picking.
 */
#pragma once

#include <vector>

#include "engine/point.hpp"
#include "fp/fp_math.hpp"
#include "fp/fp_raycast.hpp"

namespace devilution::fp {

enum class EntityKind : uint8_t {
	Monster,
	Towner,
	Object,
	Item,
};

struct Entity {
	EntityKind kind;
	int id;     // index into Monsters / Towners / Objects / Items
	Point tile; // logical tile (what the game's cursor targeting expects)
	Vec2 pos;   // world position of the billboard's foot
	float width;
	float height;
};

/** @brief Collects all billboard entities that can currently be seen (walls are not considered here). */
void CollectEntities(std::vector<Entity> &entities);

/** @brief Screen-space footprint of a billboard. */
struct BillboardRect {
	bool visible;
	float depth;
	int centerX;
	int halfWidth;
	int top;
	int bottom;
};

[[nodiscard]] BillboardRect ProjectBillboard(const Projection &projection, Vec2 pos, float width, float height);

} // namespace devilution::fp
