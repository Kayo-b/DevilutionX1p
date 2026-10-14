/**
 * @file fp/fp_world.cpp
 *
 * The things that stand in the first-person world as billboards, shared by rendering and picking.
 */
#include "fp/fp_world.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <unordered_map>

#include "dead.h"
#include "engine/light_tables.hpp"
#include "engine/palette.h"
#include "engine/lighting_defs.hpp"
#include "engine/render/scrollrt.h"
#include "engine/trn.hpp"
#include "items.h"
#include "levels/dun_tile_data.hpp"
#include "levels/gendung.h"
#include "levels/tile_properties.hpp"
#include "missiles.h"
#include "monster.h"
#include "fp/fp_buildings.hpp"
#include "fp/fp_catacombs.hpp"
#include "fp/fp_dungeon.hpp"
#include "fp/fp_hell.hpp"
#include "fp/fp_textures.hpp"
#include "utils/clx_decode.hpp"
#include "objects.h"
#include "towners.h"
#include "utils/is_of.hpp"

namespace devilution::fp {

/**
 * @brief Where a wall torch hangs in the clean dungeon, and which way the face of its wall looks (`normal`): against
 * that face, not in the middle of a tile (the iso art drew it over the face). Its type tells the face (AddL2Torches):
 * OBJ_TORCHL2 and OBJ_TORCHR2 stand on the wall tile itself, on its face towards +x or +y (the faces the iso camera
 * saw); OBJ_TORCHL and OBJ_TORCHR on the floor north-west or north-east of a wall, on its face towards -x or -y.
 * Elsewhere (a torch not where its type puts it), the first face it has beside it.
 */
Vec2 WallTorchPlacement(const Object &object, Vec2 &normal)
{
	const Point tile = object.position;
	const Vec2 centre { static_cast<float>(tile.x) + 0.5F, static_cast<float>(tile.y) + 0.5F };
	Displacement face { 0, 0 };
	bool onWall = false;
	switch (object._otype) {
	case OBJ_TORCHL2: face = { 1, 0 }, onWall = true; break;
	case OBJ_TORCHR2: face = { 0, 1 }, onWall = true; break;
	case OBJ_TORCHL: face = { -1, 0 }; break;
	case OBJ_TORCHR: face = { 0, -1 }; break;
	default: break;
	}
	// The wall must be where the type says: the tile itself, or the one behind the face.
	const Point wall = onWall ? tile : tile - face;
	const Point front = onWall ? tile + face : tile;
	if (face == Displacement { 0, 0 } || !InDungeonBounds(wall) || !InDungeonBounds(front) || !IsTileSolid(wall) || IsTileSolid(front)) {
		face = { 0, 0 };
		onWall = IsTileSolid(tile);
		for (const Displacement d : onWall ? std::array { Displacement { 1, 0 }, Displacement { 0, 1 }, Displacement { -1, 0 }, Displacement { 0, -1 } }
		                                   : std::array { Displacement { -1, 0 }, Displacement { 0, -1 }, Displacement { 1, 0 }, Displacement { 0, 1 } }) {
			const Point next = tile + d;
			if (InDungeonBounds(next) && IsTileSolid(next) != onWall) {
				face = onWall ? d : Displacement { -d.deltaX, -d.deltaY };
				break;
			}
		}
	}
	normal = { static_cast<float>(face.deltaX), static_cast<float>(face.deltaY) };
	if (face == Displacement { 0, 0 })
		return centre;
	// A little in front of the face: 0.56 out from the wall tile's middle, or 0.44 back from the floor tile's.
	const float shift = onWall ? 0.56F : -0.44F;
	return { centre.x + normal.x * shift, centre.y + normal.y * shift };
}

namespace {

/** Actor sprites have their feet a few pixels above the frame's bottom edge (2-11px in the original art). */
constexpr float FootOffsetPixels = 6.0F;
constexpr int CorpseSearchRadius = 24;
constexpr int PropSearchRadius = 26;
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

/** Monsters are drawn a little larger than the iso scale so they read at distance (1.0 would be true to the walls). */
constexpr float MonsterScale = 1.0F;
/** Townspeople are ordinary humans, so they stay near the player's own height. */
constexpr float TownerScale = 0.92F;

/** @brief Empty rows under the lowest visible pixel of a sprite (its feet), cached per frame of art. */
int FootPadding(ClxSprite sprite)
{
	static std::unordered_map<const uint8_t *, int> cache;
	const uint8_t *key = sprite.pixelData();
	const auto found = cache.find(key);
	if (found != cache.end())
		return found->second;
	if (cache.size() > 8000)
		cache.clear();
	const int padding = SpriteBottomPadding(sprite);
	cache.emplace(key, padding);
	return padding;
}

/**
 * @brief How far an object's base reaches in front of its middle in the art, in px: the iso picture of a base W
 * px wide is a diamond W / 2 px tall, whose front corner (the lowest pixel) is W / 4 below its middle. Cached.
 */
int BaseSinkPixels(ClxSprite sprite)
{
	static std::unordered_map<const uint8_t *, int> cache;
	const uint8_t *key = sprite.pixelData();
	const auto found = cache.find(key);
	if (found != cache.end())
		return found->second;
	if (cache.size() > 8000)
		cache.clear();
	std::vector<int16_t> pixels;
	DecodeClx(sprite, pixels);
	const int width = sprite.width();
	const int lowest = sprite.height() - 1 - FootPadding(sprite);
	int widest = 0;
	for (int row = lowest; row >= 0 && row > lowest - 16; row--) {
		int first = width;
		int last = -1;
		for (int x = 0; x < width; x++) {
			if (pixels[static_cast<size_t>(row) * width + x] > 0) {
				first = std::min(first, x);
				last = x;
			}
		}
		widest = std::max(widest, last - first + 1);
	}
	const int sink = widest / 4;
	cache.emplace(key, sink);
	return sink;
}

/**
 * @param grounded the feet (lowest visible pixel) stand on the floor, scaled with the sprite; otherwise the sprite's
 * bottom edge sits a fixed few pixels below the floor
 */
Entity MakeSpriteEntity(EntityKind kind, int id, Point tile, Vec2 pos, ClxSprite sprite, float scale = 1.0F, bool grounded = false)
{
	Entity entity {};
	entity.kind = kind;
	entity.id = id;
	entity.tile = tile;
	entity.pos = pos;
	entity.width = scale * static_cast<float>(sprite.width()) / SpritePixelsPerTileX;
	entity.height = scale * static_cast<float>(sprite.height()) / SpritePixelsPerTileY;
	entity.bottom = grounded ? -scale * static_cast<float>(FootPadding(sprite)) / SpritePixelsPerTileY : -FootOffsetPixels / SpritePixelsPerTileY;
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
		Entity entity = MakeSpriteEntity(EntityKind::Monster, id, monster.position.tile, pos, sprite, MonsterScale, true);
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
		Entity entity = MakeSpriteEntity(EntityKind::Towner, static_cast<int>(i), towner.position, TileCenter(towner.position), towner.currentSprite(), TownerScale, true);
		entity.lit = false; // the iso renderer draws towners unlit
		entities.push_back(entity);
	}
}

/**
 * @brief A wall torch's frame as it hangs on its wall: the iso art draws it on the face, sheared as the face is (a
 * face along y rises a row every two to the right, one along x falls), so its columns are shifted back to level. The
 * result is wider than tall by half its width; cached per frame.
 */
const std::vector<int16_t> &UnshearedTorch(ClxSprite sprite, bool alongY, int &width, int &height)
{
	static std::unordered_map<const uint8_t *, std::vector<int16_t>> cache[2];
	// Keyed by where the frame lies, which another level's sprites may reuse: forgotten with the level.
	static const std::byte *cacheCels = nullptr;
	static int cacheLevel = -1;
	if (cacheCels != pDungeonCels.get() || cacheLevel != currlevel) {
		cacheCels = pDungeonCels.get();
		cacheLevel = currlevel;
		cache[0].clear();
		cache[1].clear();
	}
	width = sprite.width();
	height = sprite.height() + sprite.width() / 2;
	auto [it, added] = cache[alongY ? 1 : 0].try_emplace(sprite.pixelData());
	if (!added)
		return it->second;
	std::vector<int16_t> pixels;
	DecodeClx(sprite, pixels);
	std::vector<int16_t> &out = it->second;
	out.assign(static_cast<size_t>(width) * height, NoPixel);
	for (int sy = 0; sy < sprite.height(); sy++) {
		for (int sx = 0; sx < width; sx++) {
			const int v = sy + (alongY ? sx : width - 1 - sx) / 2;
			out[static_cast<size_t>(v) * width + sx] = pixels[static_cast<size_t>(sy) * width + sx];
		}
	}
	return out;
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
		const ClxSprite sprite = object.currentSprite();
		// Sprites that are mostly empty below (the flame of a torch stand, which stands in the tile's own art) keep
		// the art's alignment: grounding them would drop the flame to the floor.
		const bool grounded = FootPadding(sprite) * 10 < sprite.height() * 3;
		Entity entity = MakeSpriteEntity(EntityKind::Object, id, object.position, TileCenter(object.position), sprite, 1.0F, grounded);
		// A grounded object (a sarcophagus, a barrel) stands on the middle of its base, not on its front corner;
		// the floor hides the part that sinks below it. Not a chest: a low box with a shallow V for a bottom edge (its
		// front corner lowest), all of which would go under the floor and leave it cut off straight; its front corner
		// stands on the floor instead.
		if (grounded) {
			if (!object.IsChest())
				entity.bottom -= static_cast<float>(BaseSinkPixels(sprite)) / SpritePixelsPerTileY;
		} else {
			// The iso renderer puts the sprite's bottom at the tile's front corner. These sprites belong to the tile's
			// art (a torch stand's flame), which is drawn stretched up: stretch them with it.
			entity.bottom = -static_cast<float>(TILE_HEIGHT / 2) / SpritePixelsPerTileY * VerticalScale();
			entity.height *= VerticalScale();
		}
		entity.lit = object.applyLighting;
		entity.hidden = IsObjectDrawnAsMesh(id);
		if (IsCatacombs() && IsAnyOf(object._otype, OBJ_TORCHL, OBJ_TORCHR, OBJ_TORCHL2, OBJ_TORCHR2)) {
			// The catacombs' wall torches are meshes (fp_catacombs.cpp): only their flame is a sprite.
			if (!CatacombsTorchFlame(object, sprite, entity))
				continue;
		} else if (IsCleanDungeon() && IsAnyOf(object._otype, OBJ_TORCHL, OBJ_TORCHR, OBJ_TORCHL2, OBJ_TORCHR2)) {
			// A wall torch hangs flat on its wall (not turned to the camera, which half of it went into the wall), its
			// art unsheared, as wide as the face's art is (FaceTextureWidth a tile), its middle as high as before.
			Vec2 normal;
			entity.pos = WallTorchPlacement(object, normal);
			if (normal.x != 0.0F || normal.y != 0.0F) {
				int width;
				int height;
				const std::vector<int16_t> &pixels = UnshearedTorch(sprite, normal.x != 0.0F, width, height);
				const float perRow = entity.height / static_cast<float>(sprite.height());
				entity.pixels = &pixels;
				entity.pixelsWidth = width;
				entity.pixelsHeight = height;
				entity.width = static_cast<float>(width) / FaceTextureWidth;
				entity.bottom -= static_cast<float>(width) / 4.0F * perRow;
				entity.height = static_cast<float>(height) * perRow;
				entity.facing = true;
				entity.fixedAlong = true;
				entity.along = { normal.y, -normal.x }; // to the right of one looking at the face
			}
		}
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
			// A lying body is a picture of the ground as the iso camera saw it, so it keeps the facing it has in the world
			// and is drawn flat on the floor (see DrawFlatSprite) rather than as an upright billboard.
			const ClxSprite sprite = corpse.spritesForDirection(facing)[corpse.frame];
			Entity entity = MakeSpriteEntity(EntityKind::Corpse, -1, tile, pos, sprite, 1.0F);
			entity.flat = true;
			if (corpse.translationPaletteIndex != 0)
				entity.trn = Monsters[corpse.translationPaletteIndex - 1].uniqueMonsterTRN.get();
			entities.push_back(entity);
		}
	}
}

} // namespace

bool IsPropTile(Point tile)
{
	if (!InDungeonBounds(tile))
		return false;
	const bool town = leveltype == DTYPE_TOWN;
	if (IsHell())
		return CleanKindAt(tile) == CleanKind::Prop && !IsStructureTile(tile) && !IsHellModelledProp(tile); // models now: spikes, rib cages
	const PieceTextures &textures = GetPieceTextures(dPiece[tile.x][tile.y]);
	if (!textures.hasTallArt || (!town && !textures.isFreeStanding))
		return false;
	if (!town && (IsCleanDungeon() || IsStructureTile(tile)))
		return false; // drawn as a mesh (the clean dungeon draws nothing from the tile art)
	if (town && BuildingsEnabled() && IsBuildingTile(tile))
		return false; // drawn as part of a reconstructed building
	return true;
}

namespace {

/** @brief Town buildings, trees and rocks: each tile with tall art becomes a billboard of its own iso column. */
void CollectProps(Vec2 viewerPos, std::vector<Entity> &entities)
{
	if (IsCleanDungeon() && !IsHell())
		return; // its pillars and torch stands are meshes, and nothing else is drawn from the tile art (but hell's props)
	const Point center { static_cast<int>(viewerPos.x), static_cast<int>(viewerPos.y) };
	for (int y = center.y - PropSearchRadius; y <= center.y + PropSearchRadius; y++) {
		for (int x = center.x - PropSearchRadius; x <= center.x + PropSearchRadius; x++) {
			const Point tile { x, y };
			if (!IsPropTile(tile))
				continue;
			const PieceTextures &textures = GetPieceTextures(dPiece[x][y]);
			Entity entity {};
			entity.kind = EntityKind::Prop;
			entity.id = -1;
			entity.tile = tile;
			entity.pos = TileCenter(tile);
			entity.width = static_cast<float>(ColumnWidth) / SpritePixelsPerTileX;
			const int columnRows = static_cast<int>(textures.column.size() / ColumnWidth);
			// Tile art: stretched like the walls (hell's, as low as its walls).
		entity.height = static_cast<float>(columnRows) / SpritePixelsPerTileY * (IsHell() ? HellArtScale() : VerticalScale());
			// Hell's stand on the middle of their floor diamond, half its height above the column's foot.
		entity.bottom = IsHell() ? -static_cast<float>(FloorTextureSize / 2) / SpritePixelsPerTileY * HellArtScale() : -FootOffsetPixels / SpritePixelsPerTileY;
			entity.pixels = IsHell() ? &HellPropArt(dPiece[x][y]) : &textures.column;
			entity.pixelsWidth = ColumnWidth;
			entity.pixelsHeight = columnRows;
			entity.lit = true;
			entities.push_back(entity);
		}
	}
}

} // namespace

/**
 * @brief Decodes a CLX frame into a top-to-bottom pixel buffer; -1 marks transparent pixels.
 *
 * CLX pixel data runs bottom-to-top, left-to-right, and runs may continue onto the next row.
 */
void DecodeClx(ClxSprite sprite, std::vector<int16_t> &pixels)
{
	const int width = sprite.width();
	const int height = sprite.height();
	pixels.assign(static_cast<size_t>(width * height), -1);
	const size_t total = pixels.size();

	const uint8_t *src = sprite.pixelData();
	const uint8_t *end = src + sprite.pixelDataSize();
	size_t index = 0; // linear position in bottom-to-top order
	auto put = [&](uint8_t color) {
		const size_t x = index % width;
		const size_t row = height - 1 - index / width;
		pixels[row * width + x] = color;
		index++;
	};
	while (src < end && index < total) {
		uint8_t control = *src++;
		if (!IsClxOpaque(control)) {
			index += control;
		} else if (IsClxOpaqueFill(control)) {
			const uint8_t count = GetClxOpaqueFillWidth(control);
			const uint8_t color = *src++;
			for (uint8_t i = 0; i < count && index < total; i++)
				put(color);
		} else {
			const uint8_t count = GetClxOpaquePixelsWidth(control);
			for (uint8_t i = 0; i < count && index < total; i++)
				put(*src++);
		}
	}
}

int SpriteBottomPadding(ClxSprite sprite)
{
	std::vector<int16_t> pixels;
	DecodeClx(sprite, pixels);
	const int width = sprite.width();
	for (int row = sprite.height() - 1; row >= 0; row--)
		for (int x = 0; x < width; x++)
			if (pixels[static_cast<size_t>(row) * width + x] > 0)
				return sprite.height() - 1 - row;
	return 0;
}

uint8_t LightAt(Point tile)
{
	if (!InDungeonBounds(tile))
		return LightsMax;
	return dLight[tile.x][tile.y];
}

void RefreshFpLightTables()
{
	if (leveltype != DTYPE_TOWN) {
		FpLightTablesNow = LightTables.data();
		return;
	}
	// How much of a colour's brightness each level keeps: what the game's tables keep on average of the town's
	// ground (measured), so the town is as bright as before, now without the wrong colours.
	constexpr std::array<float, 16> Keep { 1.0F, 0.94F, 0.9F, 0.78F, 0.7F, 0.66F, 0.63F, 0.58F, 0.54F, 0.49F, 0.39F, 0.31F, 0.25F, 0.12F, 0.04F, 0.0F };
	static std::array<std::array<uint8_t, 256>, 16> tables {};
	static uint32_t readyFor = 0;
	if (readyFor != PaletteVersion()) {
		readyFor = PaletteVersion();
		for (size_t l = 0; l < tables.size(); l++) {
			for (int i = 0; i < 256; i++) {
				if (l == 0 || i == 0) {
					tables[l][static_cast<size_t>(i)] = l == 0 ? static_cast<uint8_t>(i) : 0;
					continue;
				}
				const SDL_Color c = logical_palette[i];
				const float r = static_cast<float>(c.r) * Keep[l];
				const float g = static_cast<float>(c.g) * Keep[l];
				const float b = static_cast<float>(c.b) * Keep[l];
				float best = std::numeric_limits<float>::max();
				uint8_t found = 0;
				for (int j = 1; j < 256; j++) {
					const SDL_Color o = logical_palette[j];
					const float d = (o.r - r) * (o.r - r) * 3 + (o.g - g) * (o.g - g) * 6 + (o.b - b) * (o.b - b);
					if (d < best) {
						best = d;
						found = static_cast<uint8_t>(j);
					}
				}
				// Black where the colour is all but gone.
				tables[l][static_cast<size_t>(i)] = r + g + b < 6.0F ? 0 : found;
			}
		}
	}
	FpLightTablesNow = tables.data();
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
	CollectProps(viewerPos, entities);
	CollectHellCrowns(viewerPos, entities);
	CollectTreeSprites(viewerPos, entities);
}

bool IsSelectableKind(EntityKind kind)
{
	return IsNoneOf(kind, EntityKind::Missile, EntityKind::Corpse, EntityKind::Prop);
}

/** How much farther off monsters and townsfolk look up close (tiles); see ProjectBillboard. */
constexpr float CreatureNearDepth = 1.3F;

BillboardRect ProjectBillboard(const Projection &projection, const Entity &entity)
{
	const Vec2 rel { entity.pos.x - projection.pos.x, entity.pos.y - projection.pos.y };
	const float depth = rel.x * projection.dir.x + rel.y * projection.dir.y;
	if (entity.facing) {
		// The quad's two ends, across the line from the camera to the foot.
		// Standing on its foot, the quad would be all there is to see (a tree's; a torch's small flame overhead stays).
		const float distance = std::hypot(rel.x, rel.y);
		if (distance < std::min(0.4F, entity.width * 0.25F))
			return { false, depth, 0, 0, 0, 0 };
		const Vec2 across = entity.fixedAlong ? Vec2 { entity.along.x * entity.width / 2.0F, entity.along.y * entity.width / 2.0F }
		                                      : Vec2 { -rel.y / distance * entity.width / 2.0F, rel.x / distance * entity.width / 2.0F };
		BillboardRect rect { false, depth, projection.width, 0, projection.height, 0 };
		float nearest = std::numeric_limits<float>::max();
		for (const float side : { -1.0F, 1.0F }) {
			const Vec2 end { rel.x + across.x * side, rel.y + across.y * side };
			const float endDepth = end.x * projection.dir.x + end.y * projection.dir.y;
			const float endLateral = (end.x * projection.plane.x + end.y * projection.plane.y) / (PlaneLength * PlaneLength);
			if (endDepth < 0.1F) {
				// Behind the camera: the quad runs off the screen on that end's side.
				if (endLateral < 0)
					rect.left = 0;
				else
					rect.right = projection.width;
				continue;
			}
			rect.visible = true;
			nearest = std::min(nearest, endDepth);
			const float x = (endLateral / endDepth + 1.0F) * static_cast<float>(projection.width) / 2.0F;
			rect.left = std::min(rect.left, static_cast<int>(std::floor(x)));
			rect.right = std::max(rect.right, static_cast<int>(std::ceil(x)));
		}
		if (!rect.visible)
			return rect;
		const float scale = projection.focal / nearest;
		rect.top = projection.horizon - static_cast<int>(std::lround((entity.bottom + entity.height - EyeHeight) * scale));
		rect.bottom = projection.horizon - static_cast<int>(std::lround((entity.bottom - EyeHeight) * scale));
		rect.left = std::max(rect.left, 0);
		rect.right = std::min(rect.right, projection.width);
		rect.visible = rect.right > rect.left && rect.bottom > rect.top;
		return rect;
	}
	if (depth < 0.1F)
		return { false, depth, 0, 0, 0, 0 };
	const float lateral = (rel.x * projection.plane.x + rel.y * projection.plane.y) / (PlaneLength * PlaneLength);
	// Monsters and townsfolk are sized as if a little farther off than they are (sqrt(d^2 + near^2)): one right in
	// front no longer fills the screen, while far ones hardly change. Walls still hide them by their real depth.
	const bool creature = IsAnyOf(entity.kind, EntityKind::Monster, EntityKind::Towner);
	const float sizedAt = creature ? std::sqrt(depth * depth + CreatureNearDepth * CreatureNearDepth) : depth;
	const float scale = projection.focal / sizedAt;
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
