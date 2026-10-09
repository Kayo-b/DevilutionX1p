/**
 * @file fp/fp_world.cpp
 *
 * The things that stand in the first-person world as billboards, shared by rendering and picking.
 */
#include "fp/fp_world.hpp"

#include <algorithm>

#include "engine/render/scrollrt.h"
#include "items.h"
#include "levels/dun_tile_data.hpp"
#include "levels/gendung.h"
#include "monster.h"
#include "objects.h"
#include "towners.h"

namespace devilution::fp {

namespace {

// Placeholder billboard sizes in tiles; M3 replaces these with real sprite dimensions.
constexpr float MonsterWidth = 0.5F;
constexpr float MonsterHeight = 1.1F;
constexpr float ObjectWidth = 0.6F;
constexpr float ObjectHeight = 0.9F;
constexpr float ItemWidth = 0.3F;
constexpr float ItemHeight = 0.25F;

Vec2 TileCenter(Point tile)
{
	return { static_cast<float>(tile.x) + 0.5F, static_cast<float>(tile.y) + 0.5F };
}

} // namespace

void CollectEntities(std::vector<Entity> &entities)
{
	entities.clear();

	if (leveltype == DTYPE_TOWN) {
		for (size_t i = 0; i < Towners.size(); i++) {
			const Towner &towner = Towners[i];
			entities.push_back({ EntityKind::Towner, static_cast<int>(i), towner.position, TileCenter(towner.position), MonsterWidth, MonsterHeight });
		}
	}

	for (size_t i = 0; i < ActiveMonsterCount; i++) {
		const int id = static_cast<int>(ActiveMonsters[i]);
		const Monster &monster = Monsters[id];
		if (!IsTileVisible(monster.position.tile) || (monster.flags & MFLAG_HIDDEN) != 0)
			continue;
		Vec2 pos = TileCenter(monster.position.tile);
		if (monster.isWalking()) {
			const Vec2 walk = ScreenToWorld(GetOffsetForWalking(monster.animInfo, monster.direction));
			pos.x += walk.x;
			pos.y += walk.y;
		}
		entities.push_back({ EntityKind::Monster, id, monster.position.tile, pos, MonsterWidth, MonsterHeight });
	}

	for (int i = 0; i < ActiveObjectCount; i++) {
		const int id = ActiveObjects[i];
		const Object &object = Objects[id];
		if (object.isDoor())
			continue; // doors are part of the walls
		entities.push_back({ EntityKind::Object, id, object.position, TileCenter(object.position), ObjectWidth, ObjectHeight });
	}

	for (size_t i = 0; i < ActiveItemCount; i++) {
		const int id = ActiveItems[i];
		const Item &item = Items[id];
		entities.push_back({ EntityKind::Item, id, item.position, TileCenter(item.position), ItemWidth, ItemHeight });
	}
}

BillboardRect ProjectBillboard(const Projection &projection, Vec2 pos, float width, float height)
{
	const Vec2 rel { pos.x - projection.pos.x, pos.y - projection.pos.y };
	const float depth = rel.x * projection.dir.x + rel.y * projection.dir.y;
	if (depth < 0.1F)
		return { false, depth, 0, 0, 0, 0 };
	const float lateral = (rel.x * projection.plane.x + rel.y * projection.plane.y) / (PlaneLength * PlaneLength);
	const float scale = projection.focal / depth;
	BillboardRect rect;
	rect.visible = true;
	rect.depth = depth;
	rect.centerX = static_cast<int>((lateral / depth + 1.0F) * static_cast<float>(projection.width) / 2.0F);
	rect.halfWidth = std::max(1, static_cast<int>(width * scale / 2.0F));
	rect.top = projection.horizon - static_cast<int>((height - EyeHeight) * scale);
	rect.bottom = projection.horizon + static_cast<int>(EyeHeight * scale);
	return rect;
}

} // namespace devilution::fp
