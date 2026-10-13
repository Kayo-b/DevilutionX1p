/**
 * @file fp/fp_pick.cpp
 *
 * Finds what is under a screen position in the first-person view (crosshair or free cursor).
 */
#include "fp/fp_pick.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

#include "engine/render/clx_render.hpp"
#include "fp/fp_doors.hpp"
#include "fp/fp_dungeon.hpp"
#include "fp/fp_raycast.hpp"
#include "levels/dun_tile_data.hpp"
#include "monster.h"
#include "objects.h"

namespace devilution::fp {

namespace {

constexpr float MaxPickDistance = 24.0F;
// Aiming tolerance around opaque sprite pixels, in screen pixels.
constexpr int AimTolerance = 6;
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

/** @brief Whether the screen position lands on (or within AimTolerance of) an opaque pixel of the billboard. */
bool HitsSprite(const Entity &entity, const BillboardRect &rect, int x, int y)
{
	if (x < rect.left - AimTolerance || x >= rect.right + AimTolerance || y < rect.top - AimTolerance || y >= rect.bottom + AimTolerance)
		return false;
	if (!entity.sprite)
		return true;
	const ClxSprite sprite = *entity.sprite;
	const int screenWidth = rect.right - rect.left;
	const int screenHeight = rect.bottom - rect.top;
	for (const auto [dx, dy] : { std::pair { 0, 0 }, { -AimTolerance, 0 }, { AimTolerance, 0 }, { 0, -AimTolerance }, { 0, AimTolerance } }) {
		const int sx = x + dx - rect.left;
		const int sy = y + dy - rect.top;
		if (sx < 0 || sx >= screenWidth || sy < 0 || sy >= screenHeight)
			continue;
		const Point pixel { sx * sprite.width() / screenWidth, sy * sprite.height() / screenHeight };
		if (IsPointWithinClx(pixel, sprite))
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
	const float slope = proj.rowSlope(y);
	const RayHit wall = CastRayThroughHoles(proj.pos, ray, slope);
	const float wallDepth = wall.hit ? wall.distance : std::numeric_limits<float>::infinity();
	const float floorDepth = slope < 0 ? EyeHeight / -slope : std::numeric_limits<float>::infinity();
	const float ceilingDepth = slope > 0 ? (WallHeight() * VerticalScale() - EyeHeight) / slope : std::numeric_limits<float>::infinity();
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
	// The caves' gates stand in their fences with no wall round them: aiming at a gate (the line through its tile's
	// middle, up to its top) selects its door. An open one only if nothing else is aimed at, as what is seen through it
	// comes first.
	Pick openGate {};
	openGate.hasTarget = false;
	float openGateDepth = std::numeric_limits<float>::infinity();
	if (IsCleanDungeon() && CleanDoorwaysAreGates()) {
		const float gateTop = CleanDoorOpening().spring;
		for (int i = 0; i < ActiveObjectCount; i++) {
			Object &object = Objects[ActiveObjects[i]];
			if (!object.isDoor() || !object.canInteractWith())
				continue;
			const DoorwaySpan span = DoorwaySpanAt(object.position);
			if (span.door == nullptr || span.offset != 0)
				continue;
			const Point tile = object.position;
			const bool alongX = span.door->alongX;
			const float across = alongX ? ray.y : ray.x;
			if (std::abs(across) < 1e-6F)
				continue;
			const float line = static_cast<float>(alongX ? tile.y : tile.x) + 0.5F;
			const float depth = (line - (alongX ? proj.pos.y : proj.pos.x)) / across;
			if (depth <= 0.0F || depth >= bestDepth)
				continue;
			const float along = alongX ? proj.pos.x + ray.x * depth : proj.pos.y + ray.y * depth;
			const float from = static_cast<float>(alongX ? tile.x : tile.y);
			const float height = EyeHeight + slope * depth;
			if (along < from || along >= from + 1.0F || height < 0.0F || height > gateTop)
				continue;
			const Displacement back = alongX ? Displacement { 0, across > 0 ? -1 : 1 } : Displacement { across > 0 ? -1 : 1, 0 };
			const Pick gate { true, EntityKind::Object, static_cast<int>(object.GetId()), tile, tile + back };
			if (object._oVar4 != 0) { // DOOR_CLOSED is 0
				if (depth < openGateDepth) {
					openGate = gate;
					openGateDepth = depth;
				}
				continue;
			}
			pick = gate;
			bestDepth = depth;
		}
	}

	static std::vector<Entity> entities;
	CollectEntities(proj.pos, entities);
	const Vec2 floorPoint { proj.pos.x + ray.x * floorDepth, proj.pos.y + ray.y * floorDepth };

	for (const Entity &entity : entities) {
		if (!IsSelectableKind(entity.kind) || !IsSelectable(entity, ignoreMonsters))
			continue;
		const BillboardRect rect = ProjectBillboard(proj, entity);
		if (!rect.visible || rect.depth >= wallDepth || rect.depth >= bestDepth + 0.25F)
			continue;
		bool hit = HitsSprite(entity, rect, x, y);
		if (!hit && entity.kind == EntityKind::Item && floorDepth < wallDepth)
			hit = std::hypot(floorPoint.x - entity.pos.x, floorPoint.y - entity.pos.y) <= ItemFloorRadius;
		if (!hit)
			continue;
		pick = { true, entity.kind, entity.id, entity.tile, entity.tile };
		bestDepth = rect.depth;
	}

	if (!pick.hasTarget && openGate.hasTarget)
		pick = openGate;
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
