/**
 * @file fp/fp_world.cpp
 *
 * The things that stand in the first-person world as billboards, shared by rendering and picking.
 */
#include "fp/fp_world.hpp"

#include <algorithm>
#include <cmath>

#include "dead.h"
#include "engine/lighting_defs.hpp"
#include "engine/render/scrollrt.h"
#include "engine/trn.hpp"
#include "items.h"
#include "levels/dun_tile_data.hpp"
#include "levels/gendung.h"
#include "missiles.h"
#include "monster.h"
#include "objects.h"
#include "towners.h"
#include "utils/is_of.hpp"

namespace devilution::fp {

namespace {

/** Actor sprites have their feet a few pixels above the frame's bottom edge (2-11px in the original art). */
constexpr float FootOffsetPixels = 6.0F;
constexpr int CorpseSearchRadius = 24;
/** Direction the iso camera looks in (screen up = North). Sprites are drawn as seen from this direction. */
const float IsoViewYaw = DirectionToYaw(Direction::North);

Vec2 TileCenter(Point tile)
{
	return { static_cast<float>(tile.x) + 0.5F, static_cast<float>(tile.y) + 0.5F };
}

Vec2 Add(Vec2 a, Vec2 b)
{
	return { a.x + b.x, a.y + b.y };
}

/**
 * @brief Which of the 8 directional sprites shows an actor facing `facing` as seen from `viewerPos`.
 *
 * The sprite for direction D shows the actor as the iso camera sees it; rotate the facing by the
 * difference between the actual viewing direction and the iso one.
 */
Direction ViewRelativeDirection(Direction facing, Vec2 viewerPos, Vec2 actorPos)
{
	const float viewYaw = std::atan2(actorPos.y - viewerPos.y, actorPos.x - viewerPos.x);
	return YawToDirection(DirectionToYaw(facing) - (viewYaw - IsoViewYaw));
}

Entity MakeSpriteEntity(EntityKind kind, int id, Point tile, Vec2 pos, ClxSprite sprite)
{
	Entity entity {};
	entity.kind = kind;
	entity.id = id;
	entity.tile = tile;
	entity.pos = pos;
	entity.width = static_cast<float>(sprite.width()) / SpritePixelsPerTileX;
	entity.height = static_cast<float>(sprite.height()) / SpritePixelsPerTileY;
	entity.bottom = -FootOffsetPixels / SpritePixelsPerTileY;
	entity.sprite = sprite;
	entity.trn = nullptr;
	entity.lit = true;
	return entity;
}

/** @brief The monster's current animation frame, taken from the sprite sheet for the view-relative direction. */
ClxSprite MonsterSprite(const Monster &monster, Direction viewDirection)
{
	const ClxSprite current = monster.animInfo.currentSprite();
	const ClxSprite currentFirst = (*monster.animInfo.sprites)[0];
	// Find which animation is playing: its sheet for some direction is the one being shown.
	for (const AnimStruct &anim : monster.type().anims) {
		for (int dir = 0; dir < 8; dir++) {
			const OptionalClxSpriteList sprites = anim.spritesForDirection(static_cast<Direction>(dir));
			if (!sprites || (*sprites)[0] != currentFirst)
				continue;
			const OptionalClxSpriteList rotated = anim.spritesForDirection(viewDirection);
			if (!rotated)
				return current;
			return (*rotated)[monster.animInfo.getFrameToUseForRendering()];
		}
	}
	return current;
}

void CollectMonsters(Vec2 viewerPos, std::vector<Entity> &entities)
{
	for (size_t i = 0; i < ActiveMonsterCount; i++) {
		const int id = static_cast<int>(ActiveMonsters[i]);
		const Monster &monster = Monsters[id];
		if (!monster.animInfo.sprites || (monster.flags & MFLAG_HIDDEN) != 0 || !IsTileLit(monster.position.tile))
			continue;
		Vec2 pos = TileCenter(monster.position.tile);
		if (monster.isWalking())
			pos = Add(pos, ScreenToWorld(GetOffsetForWalking(monster.animInfo, monster.direction)));

		const ClxSprite sprite = MonsterSprite(monster, ViewRelativeDirection(monster.direction, viewerPos, pos));
		Entity entity = MakeSpriteEntity(EntityKind::Monster, id, monster.position.tile, pos, sprite);
		if (monster.isUnique())
			entity.trn = monster.uniqueMonsterTRN.get();
		if (monster.mode == MonsterMode::Petrified)
			entity.trn = GetStoneTRN();
		entities.push_back(entity);
	}
}

void CollectTowners(std::vector<Entity> &entities)
{
	if (leveltype != DTYPE_TOWN)
		return;
	for (size_t i = 0; i < Towners.size(); i++) {
		const Towner &towner = Towners[i];
		if (!towner.anim)
			continue;
		Entity entity = MakeSpriteEntity(EntityKind::Towner, static_cast<int>(i), towner.position, TileCenter(towner.position), towner.currentSprite());
		entity.lit = false; // the iso renderer draws towners unlit
		entities.push_back(entity);
	}
}

void CollectObjects(std::vector<Entity> &entities)
{
	for (int i = 0; i < ActiveObjectCount; i++) {
		const int id = ActiveObjects[i];
		const Object &object = Objects[id];
		// The object list is not cleared when entering town (its graphics are), so only trust objects the
		// current level's object map actually points at, like the iso renderer does.
		if (!InDungeonBounds(object.position) || dObject[object.position.x][object.position.y] != id + 1)
			continue;
		if (object.isDoor() || !object._oAnimData)
			continue; // doors are part of the walls
		if (LightAt(object.position) >= LightsMax)
			continue;
		Entity entity = MakeSpriteEntity(EntityKind::Object, id, object.position, TileCenter(object.position), object.currentSprite());
		entity.lit = object.applyLighting;
		entities.push_back(entity);
	}
}

void CollectItems(std::vector<Entity> &entities)
{
	for (size_t i = 0; i < ActiveItemCount; i++) {
		const int id = ActiveItems[i];
		const Item &item = Items[id];
		if (!item.AnimInfo.sprites)
			continue;
		entities.push_back(MakeSpriteEntity(EntityKind::Item, id, item.position, TileCenter(item.position), item.AnimInfo.currentSprite()));
	}
}

void CollectMissiles(std::vector<Entity> &entities)
{
	for (const Missile &missile : Missiles) {
		if (!missile._miDrawFlag || !missile._miAnimData)
			continue;
		const Point tile = missile.position.tileForRendering;
		if (!InDungeonBounds(tile))
			continue;
		const Vec2 pos = Add(TileCenter(tile), ScreenToWorld(missile.position.offsetForRendering));
		const ClxSprite sprite = (*missile._miAnimData)[missile._miAnimFrame - 1];
		Entity entity = MakeSpriteEntity(EntityKind::Missile, -1, tile, pos, sprite);
		entity.lit = missile._miLightFlag;
		if (missile._miUniqTrans != 0)
			entity.trn = Monsters[missile._misource].uniqueMonsterTRN.get();
		entities.push_back(entity);
	}
}

void CollectCorpses(Vec2 viewerPos, std::vector<Entity> &entities)
{
	const Point center { static_cast<int>(viewerPos.x), static_cast<int>(viewerPos.y) };
	for (int y = center.y - CorpseSearchRadius; y <= center.y + CorpseSearchRadius; y++) {
		for (int x = center.x - CorpseSearchRadius; x <= center.x + CorpseSearchRadius; x++) {
			const Point tile { x, y };
			if (!InDungeonBounds(tile) || dCorpse[x][y] == 0 || LightAt(tile) >= LightsMax)
				continue;
			const int8_t value = dCorpse[x][y];
			const Corpse &corpse = Corpses[(value & 0x1F) - 1];
			if (!corpse.sprites)
				continue;
			const Vec2 pos = TileCenter(tile);
			const auto facing = static_cast<Direction>((value >> 5) & 7);
			const ClxSprite sprite = corpse.spritesForDirection(ViewRelativeDirection(facing, viewerPos, pos))[corpse.frame];
			Entity entity = MakeSpriteEntity(EntityKind::Corpse, -1, tile, pos, sprite);
			if (corpse.translationPaletteIndex != 0)
				entity.trn = Monsters[corpse.translationPaletteIndex - 1].uniqueMonsterTRN.get();
			entities.push_back(entity);
		}
	}
}

} // namespace

uint8_t LightAt(Point tile)
{
	if (!InDungeonBounds(tile))
		return LightsMax;
	return dLight[tile.x][tile.y];
}

void CollectEntities(Vec2 viewerPos, std::vector<Entity> &entities)
{
	entities.clear();
	CollectTowners(entities);
	CollectMonsters(viewerPos, entities);
	CollectObjects(entities);
	CollectItems(entities);
	CollectMissiles(entities);
	CollectCorpses(viewerPos, entities);
}

bool IsSelectableKind(EntityKind kind)
{
	return IsNoneOf(kind, EntityKind::Missile, EntityKind::Corpse);
}

BillboardRect ProjectBillboard(const Projection &projection, const Entity &entity)
{
	const Vec2 rel { entity.pos.x - projection.pos.x, entity.pos.y - projection.pos.y };
	const float depth = rel.x * projection.dir.x + rel.y * projection.dir.y;
	if (depth < 0.1F)
		return { false, depth, 0, 0, 0, 0 };
	const float lateral = (rel.x * projection.plane.x + rel.y * projection.plane.y) / (PlaneLength * PlaneLength);
	const float scale = projection.focal / depth;
	const float centerX = (lateral / depth + 1.0F) * static_cast<float>(projection.width) / 2.0F;
	const float halfWidth = entity.width * scale / 2.0F;

	BillboardRect rect;
	rect.visible = true;
	rect.depth = depth;
	rect.left = static_cast<int>(std::floor(centerX - halfWidth));
	rect.right = static_cast<int>(std::ceil(centerX + halfWidth));
	rect.top = projection.horizon - static_cast<int>(std::lround((entity.bottom + entity.height - EyeHeight) * scale));
	rect.bottom = projection.horizon - static_cast<int>(std::lround((entity.bottom - EyeHeight) * scale));
	if (rect.right <= rect.left || rect.bottom <= rect.top)
		rect.visible = false;
	return rect;
}

} // namespace devilution::fp
