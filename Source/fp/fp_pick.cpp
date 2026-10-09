/**
 * @file fp/fp_pick.cpp
 *
 * Finds what is under a screen position in the first-person view (crosshair or free cursor).
 */
#include "fp/fp_pick.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

#include "fp/fp_raycast.hpp"
#include "levels/dun_tile_data.hpp"
#include "monster.h"
#include "objects.h"

namespace devilution::fp {

namespace {

constexpr float MaxPickDistance = 24.0F;
// Hit boxes are a little wider than the drawn billboards so aiming isn't fiddly.
constexpr float TargetWidthScale = 1.3F;
// Items lie on the floor; also accept the crosshair landing on the floor near them.
constexpr float ItemFloorRadius = 0.6F;

bool IsSelectable(const Entity &entity, bool ignoreMonsters)
{
	switch (entity.kind) {
	case EntityKind::Monster: {
		if (ignoreMonsters)
			return false;
		const Monster &monster = Monsters[entity.id];
		return IsTileLit(entity.tile) && !monster.hasNoLife() && (monster.flags & MFLAG_HIDDEN) == 0 && !monster.isPlayerMinion();
	}
	case EntityKind::Towner:
		return !ignoreMonsters;
	case EntityKind::Object:
		return Objects[entity.id].canInteractWith();
	case EntityKind::Item:
		return true;
	}
	return false;
}

Point TileAt(Vec2 pos)
{
	return { static_cast<int>(std::floor(pos.x)), static_cast<int>(std::floor(pos.y)) };
}

} // namespace

Pick PickAt(Point screenPosition, bool ignoreMonsters)
{
	const Projection proj = GetProjection();
	const int x = std::clamp(screenPosition.x, 0, proj.width - 1);
	const int y = std::clamp(screenPosition.y, 0, proj.height - 1);

	const Vec2 ray = proj.columnRay(x);
	const RayHit wall = CastRay(proj.pos, ray);
	const float wallDepth = wall.hit ? wall.distance : std::numeric_limits<float>::infinity();
	const float slope = proj.rowSlope(y);
	const float floorDepth = slope < 0 ? EyeHeight / -slope : std::numeric_limits<float>::infinity();
	const float ceilingDepth = slope > 0 ? (WallHeight - EyeHeight) / slope : std::numeric_limits<float>::infinity();
	const float surfaceDepth = std::min({ wallDepth, floorDepth, ceilingDepth, MaxPickDistance });

	Pick pick {};
	pick.hasTarget = false;
	float bestDepth = surfaceDepth;

	// Doors are wall tiles; aiming at the wall face selects them.
	if (wall.hit && wallDepth <= surfaceDepth) {
		Object *object = FindObjectAtPosition(wall.tile);
		if (object != nullptr && object->isDoor() && object->canInteractWith()) {
			pick = { true, EntityKind::Object, static_cast<int>(object->GetId()), wall.tile, wall.previous };
			bestDepth = wallDepth;
		}
	}

	static std::vector<Entity> entities;
	CollectEntities(entities);
	const Vec2 floorPoint { proj.pos.x + ray.x * floorDepth, proj.pos.y + ray.y * floorDepth };

	for (const Entity &entity : entities) {
		if (!IsSelectable(entity, ignoreMonsters))
			continue;
		const BillboardRect rect = ProjectBillboard(proj, entity.pos, entity.width * TargetWidthScale, entity.height);
		if (!rect.visible || rect.depth >= wallDepth)
			continue;
		bool hit = x >= rect.centerX - rect.halfWidth && x <= rect.centerX + rect.halfWidth && y >= rect.top && y <= rect.bottom;
		if (!hit && entity.kind == EntityKind::Item && floorDepth < wallDepth) {
			hit = std::hypot(floorPoint.x - entity.pos.x, floorPoint.y - entity.pos.y) <= ItemFloorRadius;
		}
		if (!hit || rect.depth >= bestDepth + 0.25F)
			continue;
		pick = { true, entity.kind, entity.id, entity.tile, entity.tile };
		bestDepth = rect.depth;
	}

	if (!pick.hasTarget) {
		if (floorDepth <= surfaceDepth) {
			pick.groundTile = TileAt(floorPoint);
		} else if (wall.hit && wallDepth <= surfaceDepth) {
			pick.groundTile = wall.previous;
		} else {
			pick.groundTile = TileAt({ proj.pos.x + ray.x * surfaceDepth, proj.pos.y + ray.y * surfaceDepth });
		}
		if (!InDungeonBounds(pick.groundTile))
			pick.groundTile = TileAt(proj.pos);
	}
	return pick;
}

} // namespace devilution::fp
