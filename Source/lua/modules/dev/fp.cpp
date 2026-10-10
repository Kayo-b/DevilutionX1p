#ifdef _DEBUG
#include "lua/modules/dev/fp.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include <sol/sol.hpp>

#include "fp/fp_buildings.hpp"
#include "fp/fp_controls.hpp"
#include "fp/fp_raycast.hpp"
#include "fp/fp_textures.hpp"
#include "fp/fp_view.hpp"
#include "fp/fp_world.hpp"
#include "engine/palette.h"
#include "lua/metadoc.hpp"
#include "monster.h"
#include "player.h"
#include "utils/log.hpp"
#include "levels/tile_properties.hpp"
#include "levels/gendung.h"
#include "levels/trigs.h"
#include "objects.h"
#include "utils/str_cat.hpp"

namespace devilution {
namespace {

constexpr float Pi = 3.14159265F;

std::string Fixed2(float value)
{
	char buffer[32];
	std::snprintf(buffer, sizeof(buffer), "%.2f", value);
	return buffer;
}

std::string DebugCmdFpView(float yawDegrees, std::optional<float> pitch)
{
	const fp::ViewState &view = fp::GetViewState();
	fp::SetViewAngles(yawDegrees * Pi / 180.0F, pitch.value_or(view.pitch));
	return "View set.";
}

std::string DebugCmdFpGoto(int x, int y)
{
	// Nearest free tile to the requested one, searching outwards in rings.
	for (int radius = 0; radius <= 4; radius++) {
		for (int dy = -radius; dy <= radius; dy++) {
			for (int dx = -radius; dx <= radius; dx++) {
				if (std::max(std::abs(dx), std::abs(dy)) != radius)
					continue;
				if (fp::TeleportPlayer({ x + dx, y + dy }))
					return StrCat("Moved to ", x + dx, ",", y + dy, ".");
			}
		}
	}
	return "No free tile nearby.";
}

const char *KindName(fp::EntityKind kind)
{
	switch (kind) {
	case fp::EntityKind::Monster: return "monster";
	case fp::EntityKind::Towner: return "towner";
	case fp::EntityKind::Object: return "object";
	case fp::EntityKind::Item: return "item";
	case fp::EntityKind::Missile: return "missile";
	case fp::EntityKind::Corpse: return "corpse";
	case fp::EntityKind::Prop: return "prop";
	}
	return "?";
}

std::string DebugCmdFpInfo(std::optional<int> radius)
{
	const fp::ViewState &view = fp::GetViewState();
	const fp::Projection proj = fp::GetProjection();
	std::string result = StrCat("pos ", Fixed2(view.x), ",", Fixed2(view.y), " yaw ", static_cast<int>(view.yaw * 180.0F / Pi), " pitch ", Fixed2(view.pitch));
	std::vector<fp::Entity> entities;
	fp::CollectEntities(proj.pos, entities);
	const float maxDistance = static_cast<float>(radius.value_or(8));
	for (const fp::Entity &entity : entities) {
		const float distance = std::hypot(entity.pos.x - view.x, entity.pos.y - view.y);
		if (distance > maxDistance)
			continue;
		const fp::BillboardRect rect = fp::ProjectBillboard(proj, entity);
		StrAppend(result, "\n", KindName(entity.kind), " #", entity.id, " tile ", entity.tile.x, ",", entity.tile.y,
		    " dist ", Fixed2(distance), " sprite ", entity.sprite ? entity.sprite->width() : 0, "x", entity.sprite ? entity.sprite->height() : 0,
		    " light ", static_cast<int>(fp::LightAt(entity.tile)), rect.visible ? StrCat(" screen ", rect.left, "..", rect.right, " x ", rect.top, "..", rect.bottom) : " offscreen");
	}
	return result;
}

std::string DebugCmdFpDumpPieces(std::string path, std::optional<int> perRow, std::optional<int> rows)
{
	fp::DumpSolidPieces(path.c_str(), perRow.value_or(8), rows.value_or(4));
	return "Dumped.";
}

std::string DebugCmdFpMap(std::optional<int> radius)
{
	const fp::ViewState &view = fp::GetViewState();
	const int r = radius.value_or(10);
	const Point center { static_cast<int>(view.x), static_cast<int>(view.y) };
	std::string header = StrCat("map around ", center.x, ",", center.y, " (x across, y down): S solid with usable face, R solid no face, p solid with other tall art, P solid pillar, t walkable with tall art, . walkable, @ player");
	LogInfo("{}", header);
	for (int y = center.y - r; y <= center.y + r; y++) {
		std::string row;
		for (int x = center.x - r; x <= center.x + r; x++) {
			const Point tile { x, y };
			if (!InDungeonBounds(tile)) {
				row += ' ';
				continue;
			}
			const fp::PieceTextures &t = fp::GetPieceTextures(dPiece[x][y]);
			const bool solid = IsTileSolid(tile);
			char c = '.';
			if (x == center.x && y == center.y)
				c = '@';
			else if (solid)
				c = t.isFreeStanding ? 'C' : t.isPillar ? 'P' : (t.faceXUsable || t.faceYUsable) ? 'S' : (t.hasTallArt ? 'p' : 'R');
			else if (t.hasTallArt)
				c = 't';
			row += c;
		}
		LogInfo("{:3d} {}", y, row);
	}
	// Art statistics of every distinct piece that is not plain floor, to calibrate the pillar rule.
	std::vector<uint16_t> seen;
	for (int y = center.y - r; y <= center.y + r; y++) {
		for (int x = center.x - r; x <= center.x + r; x++) {
			if (!InDungeonBounds({ x, y }))
				continue;
			const uint16_t piece = dPiece[x][y];
			const fp::PieceTextures &t = fp::GetPieceTextures(piece);
			if (!t.hasTallArt || std::find(seen.begin(), seen.end(), piece) != seen.end())
				continue;
			seen.push_back(piece);
			int minX = 64;
			int maxX = -1;
			int minY = 256;
			int opaque = 0;
			const int columnHeight = static_cast<int>(t.column.size() / 64);
			for (int cy = 0; cy < columnHeight - 32; cy++) {
				for (int cx = 0; cx < 64; cx++) {
					if (t.column[static_cast<size_t>(cy) * 64 + cx] != fp::NoPixel) {
						minX = std::min(minX, cx);
						maxX = std::max(maxX, cx);
						minY = std::min(minY, cy);
						opaque++;
					}
				}
			}
			LogInfo("piece {} at {},{} solid {} width {} ({}..{}) rows {} opaque {} faceX {} faceY {} pillar {} rowsX {} rowsY {}", piece, x, y, IsTileSolid({ x, y }) ? 1 : 0, maxX - minX + 1, minX, maxX, columnHeight - 32 - minY, opaque,
			    t.faceXUsable ? 1 : 0, t.faceYUsable ? 1 : 0, t.isPillar ? 1 : 0, t.faceXRows, t.faceYRows);
		}
	}
	return "Map logged.";
}

std::string DebugCmdFpKillMonsters()
{
	int killed = 0;
	for (size_t i = 0; i < ActiveMonsterCount; i++) {
		Monster &monster = Monsters[ActiveMonsters[i]];
		if (monster.hitPoints <= 0 || monster.isPlayerMinion())
			continue;
		monster.hitPoints = 0;
		StartMonsterDeath(monster, *MyPlayer, false);
		killed++;
	}
	return StrCat("Killed ", killed, " monsters.");
}

std::string DebugCmdFpMonsterState()
{
	const fp::ViewState &view = fp::GetViewState();
	std::string result;
	for (size_t i = 0; i < ActiveMonsterCount; i++) {
		const Monster &monster = Monsters[ActiveMonsters[i]];
		if (std::hypot(static_cast<float>(monster.position.tile.x) - view.x, static_cast<float>(monster.position.tile.y) - view.y) > 4.0F)
			continue;
		StrAppend(result, "\nmonster #", ActiveMonsters[i], " hp ", monster.hitPoints, " mode ", static_cast<int>(monster.mode), " frame ", monster.animInfo.currentFrame, "/", monster.animInfo.numberOfFrames);
	}
	int corpses = 0;
	for (int y = 0; y < MAXDUNY; y++)
		for (int x = 0; x < MAXDUNX; x++)
			if (dCorpse[x][y] != 0)
				corpses++;
	StrAppend(result, "\ncorpse tiles: ", corpses);
	return result;
}

std::string DebugCmdFpDumpPiece(int x, int y, std::string path)
{
	if (!InDungeonBounds({ x, y }))
		return "Out of bounds.";
	const fp::PieceTextures &t = fp::GetPieceTextures(dPiece[x][y]);
	const int h = static_cast<int>(t.column.size() / 64);
	std::vector<uint8_t> image(static_cast<size_t>(64) * h * 3, 40);
	for (int i = 0; i < 64 * h; i++) {
		if (t.column[i] == fp::NoPixel)
			continue;
		const SDL_Color c = logical_palette[t.column[i]];
		image[i * 3] = c.r;
		image[i * 3 + 1] = c.g;
		image[i * 3 + 2] = c.b;
	}
	if (FILE *f = std::fopen(path.c_str(), "wb")) {
		std::fprintf(f, "P6\n64 %d\n255\n", h);
		std::fwrite(image.data(), 1, image.size(), f);
		std::fclose(f);
	}
	return "Dumped.";
}

std::string DebugCmdFpLowWalls()
{
	int count = 0;
	std::string first;
	for (int y = 0; y < MAXDUNY; y++) {
		for (int x = 0; x < MAXDUNX; x++) {
			if (!IsTileSolid({ x, y }))
				continue;
			const fp::PieceTextures &t = fp::GetPieceTextures(dPiece[x][y]);
			const int rows = std::max(t.faceXUsable ? t.faceXRows : 0, t.faceYUsable ? t.faceYRows : 0);
			if (rows > 0 && rows < 80) {
				if (count < 6)
					StrAppend(first, " ", x, ",", y, "(", rows, ")");
				count++;
			}
		}
	}
	return StrCat("low wall tiles: ", count, first);
}

std::string DebugCmdFpOn(bool enabled)
{
	if (fp::IsFirstPersonActive() != enabled)
		fp::ToggleFirstPerson();
	return enabled ? "First person on." : "First person off.";
}

std::string DebugCmdFpBuildings(bool enabled)
{
	fp::SetBuildingsEnabled(enabled);
	return enabled ? "Reconstructed buildings on." : "Reconstructed buildings off (flat billboards).";
}

std::string DebugCmdFpHouseModels(bool enabled)
{
	fp::SetHouseModelsEnabled(enabled);
	return enabled ? "Voxel house models on." : "Voxel house models off (fitted boxes).";
}

std::string DebugCmdFpHouseTextures(bool enabled)
{
	fp::SetHouseTexturesEnabled(enabled);
	return enabled ? "House textures from the iso art on." : "House textures off (ranked palette colours).";
}

std::string DebugCmdFpHouseMeshes(bool enabled)
{
	fp::SetHouseMeshesEnabled(enabled);
	return enabled ? "House meshes on." : "House meshes off (voxels).";
}

std::string DebugCmdFpDumpIso(int x0, int y0, int x1, int y1, std::string path)
{
	return fp::DumpIsoRegion(x0, y0, x1, y1, path) ? "Iso region dumped." : "Could not dump the iso region.";
}

std::string DebugCmdFpTileMap(int x0, int y0, int x1, int y1)
{
	return fp::TileMap(x0, y0, x1, y1);
}

std::string DebugCmdFpFeatures()
{
	std::string out;
	for (int i = 0; i < numtrigs; i++)
		out += StrCat("trigger ", trigs[i].position.x, ",", trigs[i].position.y, " msg ", static_cast<int>(trigs[i]._tmsg), "\n");
	int columns = 0;
	for (int y = 0; y < MAXDUNY && columns < 40; y++)
		for (int x = 0; x < MAXDUNX && columns < 40; x++)
			if (fp::GetPieceTextures(dPiece[x][y]).isFreeStanding) {
				out += StrCat("column ", x, ",", y, " piece ", dPiece[x][y], "\n");
				columns++;
			}
	for (int i = 0; i < ActiveObjectCount; i++) {
		const Object &object = Objects[ActiveObjects[i]];
		if (!object._oAnimData)
			continue;
		const ClxSprite sprite = object.currentSprite();
		out += StrCat("object ", object.position.x, ",", object.position.y, " type ", static_cast<int>(object._otype), " ", sprite.width(), "x", sprite.height(), " padding ",
		    fp::SpriteBottomPadding(sprite), "\n");
	}
	return out;
}

} // namespace

sol::table LuaDevFirstPersonModule(sol::state_view &lua)
{
	sol::table table = lua.create_table();
	LuaSetDocFn(table, "view", "(yawDegrees: number, pitch: number = nil)", "Set first-person view angles (0 = +x, 90 = +y).", &DebugCmdFpView);
	LuaSetDocFn(table, "teleport", "(x: number, y: number)", "Move the player to a free tile.", &DebugCmdFpGoto);
	LuaSetDocFn(table, "info", "(radius: number = 8)", "Describe the camera and nearby billboard entities.", &DebugCmdFpInfo);
	LuaSetDocFn(table, "dumppieces", "(path: string, perRow: number = 8, rows: number = 4)", "Dump a contact sheet of solid pieces as PPM.", &DebugCmdFpDumpPieces);
	LuaSetDocFn(table, "map", "(radius: number = 10)", "Log a tile-type map around the player.", &DebugCmdFpMap);
	LuaSetDocFn(table, "killmonsters", "()", "Kill every active monster (to look at corpses).", &DebugCmdFpKillMonsters);
	LuaSetDocFn(table, "monsterstate", "()", "Describe monsters near the player and count corpse tiles.", &DebugCmdFpMonsterState);
	LuaSetDocFn(table, "dumppiece", "(x: number, y: number, path: string)", "Dump the iso column art of the piece at a tile (PPM).", &DebugCmdFpDumpPiece);
	LuaSetDocFn(table, "lowwalls", "()", "Count solid tiles whose wall art is lower than a full wall.", &DebugCmdFpLowWalls);
	LuaSetDocFn(table, "housemodels", "(enabled: boolean)", "Draw town houses with the voxel models from the Tristram plaza reference (default) or as fitted boxes.", &DebugCmdFpHouseModels);
	LuaSetDocFn(table, "housetextures", "(enabled: boolean)", "Texture the voxel houses with walls and thatch cut out of their own iso art (default) or ranked palette colours.", &DebugCmdFpHouseTextures);
	LuaSetDocFn(table, "housemeshes", "(enabled: boolean)", "Draw the house models as textured triangle meshes (default) or as voxels.", &DebugCmdFpHouseMeshes);
	LuaSetDocFn(table, "dumpiso", "(x0: number, y0: number, x1: number, y1: number, path: string)", "Dump the original iso art of a tile rectangle (PPM).", &DebugCmdFpDumpIso);
	LuaSetDocFn(table, "tilemap", "(x0: number, y0: number, x1: number, y1: number)", "ASCII map of solid (#, s), tall-art (a) and building (B) tiles.", &DebugCmdFpTileMap);
	LuaSetDocFn(table, "features", "()", "List stairs triggers, free-standing column pieces and objects (with sprite size and bottom padding).", &DebugCmdFpFeatures);
	LuaSetDocFn(table, "on", "(enabled: boolean)", "Switch between first-person and the original view.", &DebugCmdFpOn);
	LuaSetDocFn(table, "buildings", "(enabled: boolean)", "Draw town houses as reconstructed 3D models (default) or flat billboards.", &DebugCmdFpBuildings);
	return table;
}

} // namespace devilution
#endif // _DEBUG
