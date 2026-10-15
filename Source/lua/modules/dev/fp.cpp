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
#include "fp/fp_dungeon.hpp"
#include "fp/fp_pick.hpp"
#include "fp/fp_raycast.hpp"
#include "fp/fp_sky.hpp"
#include "fp/fp_textures.hpp"
#include "fp/fp_view.hpp"
#include "fp/fp_weapon.hpp"
#include "fp/fp_world.hpp"
#include "engine/load_file.hpp"
#include "engine/palette.h"
#include "lua/metadoc.hpp"
#include "monster.h"
#include "player.h"
#include "utils/log.hpp"
#include "levels/tile_properties.hpp"
#include "levels/gendung.h"
#include "levels/trigs.h"
#include "objects.h"
#include "msg.h"
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

/** @brief Debug: the town's floor from above, each tile as the first-person floor draws it (x across, y down), with a grid. */
std::string DebugCmdFpFloorMap(int x0, int y0, int x1, int y1, std::string path)
{
	const uint8_t *buildings = fp::BuildingTileMap();
	const uint16_t *sources = fp::FloorSourceMap();
	const std::vector<int16_t> &ground = fp::GetGroundFloor();
	constexpr int Size = fp::FloorTextureSize;
	const int w = (x1 - x0 + 1) * Size;
	const int h = (y1 - y0 + 1) * Size;
	std::vector<uint8_t> image(static_cast<size_t>(w) * h * 3, 0);
	for (int y = y0; y <= y1; y++) {
		for (int x = x0; x <= x1; x++) {
			if (!InDungeonBounds({ x, y }))
				continue;
			const fp::PieceTextures &piece = fp::GetPieceTextures(dPiece[x][y]);
			const uint8_t building = buildings != nullptr ? buildings[static_cast<size_t>(y) * MAXDUNX + x] : 0;
			const std::vector<int16_t> *art = building != 0 && building != 3 ? &ground : &piece.floor;
			if (building == 3 && !piece.groundFloor.empty())
				art = &piece.groundFloor;
			if (const uint16_t source = sources != nullptr ? sources[static_cast<size_t>(y) * MAXDUNX + x] : 0; source != 0)
				art = &fp::GetPieceTextures(source).floor;
			for (int v = 0; v < Size; v++) {
				for (int u = 0; u < Size; u++) {
					int16_t texel = art->size() == static_cast<size_t>(Size) * Size ? (*art)[v * Size + u] : int16_t { -1 };
					if (texel <= 0 && ground.size() == static_cast<size_t>(Size) * Size)
						texel = ground[v * Size + u];
					const SDL_Color c = texel > 0 ? logical_palette[texel] : SDL_Color { 255, 0, 255, 255 };
					const bool line = u == 0 || v == 0;
					uint8_t *dst = &image[(static_cast<size_t>((y - y0) * Size + v) * w + (x - x0) * Size + u) * 3];
					dst[0] = line ? 255 : c.r;
					dst[1] = line ? 255 : c.g;
					dst[2] = line ? 255 : c.b;
				}
			}
		}
	}
	FILE *file = std::fopen(path.c_str(), "wb");
	if (file == nullptr)
		return "Could not write.";
	std::fprintf(file, "P6\n%d %d\n255\n", w, h);
	std::fwrite(image.data(), 1, image.size(), file);
	std::fclose(file);
	return StrCat("Floor map of ", x0, ",", y0, " .. ", x1, ",", y1, " written.");
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

std::string DebugCmdFpProps(std::optional<std::string> path)
{
	// Every tile still drawn as a billboard of its own art (a "cardboard"), with what its art looks like.
	FILE *f = path ? std::fopen(path->c_str(), "w") : nullptr;
	int count = 0;
	std::string first;
	for (int y = 0; y < MAXDUNY; y++) {
		for (int x = 0; x < MAXDUNX; x++) {
			if (!fp::IsPropTile({ x, y }))
				continue;
			const fp::PieceTextures &t = fp::GetPieceTextures(dPiece[x][y]);
			const int rows = static_cast<int>(t.column.size() / fp::ColumnWidth);
			int opaque = 0;
			int top = rows;
			long r = 0;
			long g = 0;
			long b = 0;
			for (int i = 0; i < (rows - 32) * fp::ColumnWidth; i++) {
				if (t.column[i] <= 0)
					continue;
				const SDL_Color c = logical_palette[t.column[i]];
				r += c.r;
				g += c.g;
				b += c.b;
				opaque++;
				top = std::min(top, i / fp::ColumnWidth);
			}
			if (f != nullptr)
				std::fprintf(f, "%d %d %d %d %d %d %d %ld %ld %ld\n", x, y, dPiece[x][y], IsTileSolid({ x, y }) ? 1 : 0, t.isStructure ? 1 : 0, opaque, rows - 32 - top,
				    opaque > 0 ? r / opaque : 0, opaque > 0 ? g / opaque : 0, opaque > 0 ? b / opaque : 0);
			if (count < 8)
				StrAppend(first, " ", x, ",", y);
			count++;
		}
	}
	if (f != nullptr)
		std::fclose(f);
	return StrCat("prop billboards: ", count, first);
}

std::string DebugCmdFpSky(bool enabled)
{
	fp::SetSkyEnabled(enabled);
	return enabled ? "Night sky on." : "Night sky off (flat colour).";
}

std::string DebugCmdFpWindowLight(bool enabled)
{
	fp::SetWindowLightEnabled(enabled);
	return enabled ? "Lamps lit." : "Lamps out.";
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

std::string DebugCmdFpOperate(int x, int y)
{
	Object *object = FindObjectAtPosition({ x, y });
	if (object == nullptr)
		return StrCat("No object at ", x, ",", y, ".");
	OperateObject(*MyPlayer, *object);
	return StrCat("Operated the object at ", x, ",", y, ".");
}

std::string DebugCmdFpTownWarps(int mask)
{
	MyPlayer->pTownWarps = static_cast<uint8_t>(mask);
	return StrCat("Town warps set to ", mask, " (1 catacombs, 2 caves, 4 hell); enter town again to see them.");
}

std::string DebugCmdFpWeapon(std::optional<int> file, std::optional<int> record, std::optional<int> frame)
{
	fp::PreviewFirstPersonWeapon(file.value_or(-1), record.value_or(0), frame.value_or(0));
	if (!file)
		return "The weapon follows the player again.";
	return StrCat("Showing WEAPON", *file, ".CIF record ", record.value_or(0), " frame ", frame.value_or(0), ".");
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

std::string DebugCmdFpDumpIso(int x0, int y0, int x1, int y1, std::string path, std::optional<bool> uprightOnly)
{
	return fp::DumpIsoRegion(x0, y0, x1, y1, path, uprightOnly.value_or(false)) ? "Iso region dumped." : "Could not dump the iso region.";
}

std::string DebugCmdFpTileMap(int x0, int y0, int x1, int y1)
{
	return fp::TileMap(x0, y0, x1, y1);
}

std::string DebugCmdFpCleanMap(int x0, int y0, int x1, int y1)
{
	return fp::CleanTileMap(x0, y0, x1, y1);
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
		if (object.isDoor()) {
			out += StrCat("door ", object.position.x, ",", object.position.y, object._oVar4 == 0 ? " closed" : " open", "\n");
			continue;
		}
		if (!object._oAnimData)
			continue;
		const ClxSprite sprite = object.currentSprite();
		out += StrCat("object ", object.position.x, ",", object.position.y, " type ", static_cast<int>(object._otype), " ", sprite.width(), "x", sprite.height(), " padding ",
		    fp::SpriteBottomPadding(sprite), "\n");
	}
	return out;
}

std::string DebugCmdFpDumpFaces(int x0, int y0, int x1, int y1, std::string path)
{
	// Both faces of each tile in the rectangle side by side (x face, then y face), tiles in a row; holes in red.
	const int w = fp::FaceTextureWidth;
	const int h = fp::FaceTextureHeight();
	const int cols = (x1 - x0 + 1) * (y1 - y0 + 1);
	std::vector<uint8_t> image(static_cast<size_t>(cols) * 2 * (w + 2) * h * 3, 40);
	const int width = cols * 2 * (w + 2);
	int n = 0;
	for (int y = y0; y <= y1; y++) {
		for (int x = x0; x <= x1; x++, n++) {
			const fp::PieceTextures &t = fp::GetPieceTextures(dPiece[x][y]);
			for (int side = 0; side < 2; side++) {
				const std::vector<int16_t> &face = side == 0 ? t.faceX : t.faceY;
				const std::vector<uint8_t> &holes = side == 0 ? t.holesX : t.holesY;
				const std::vector<int> &recess = side == 0 ? t.recessX : t.recessY;
				const int left = (n * 2 + side) * (w + 2);
				for (int v = 0; v < h; v++) {
					for (int u = 0; u < w; u++) {
						const size_t at = static_cast<size_t>(v) * w + u;
						uint8_t *px = &image[(static_cast<size_t>(v) * width + left + u) * 3];
						if (!recess.empty() && recess[u] == v) {
							// The recess outline in green.
							px[0] = 0;
							px[1] = 220;
							px[2] = 0;
						} else if (face[at] > 0) {
							const SDL_Color c = logical_palette[face[at]];
							px[0] = c.r;
							px[1] = c.g;
							px[2] = c.b;
						} else if (holes[at] != 0) {
							px[0] = 200;
							px[1] = 0;
							px[2] = 0;
						}
					}
				}
			}
		}
	}
	FILE *f = std::fopen(path.c_str(), "wb");
	if (f == nullptr)
		return "Could not write.";
	std::fprintf(f, "P6\n%d %d\n255\n", width, h);
	std::fwrite(image.data(), 1, image.size(), f);
	std::fclose(f);
	return StrCat("Faces dumped (", cols, " tiles).");
}

std::string DebugCmdFpRecesses(bool enabled)
{
	fp::SetWallRecessesEnabled(enabled);
	return enabled ? "Wall recesses on." : "Wall recesses off.";
}

std::string DebugCmdFpWalkTo(int x, int y)
{
	NetSendCmdLoc(MyPlayerId, true, CMD_WALKXY, { x, y });
	return StrCat("Walking to ", x, ",", y, ".");
}

std::string DebugCmdFpWhere()
{
	const fp::ViewState &view = fp::GetViewState();
	return StrCat("tile ", MyPlayer->position.tile.x, ",", MyPlayer->position.tile.y, " view ", static_cast<int>(view.x * 100), ",", static_cast<int>(view.y * 100));
}

std::string DebugCmdFpPick()
{
	// What the crosshair (the middle of the view) aims at, as a click there would take it.
	const fp::Projection proj = fp::GetProjection();
	const fp::Pick pick = fp::PickAt({ proj.width / 2, proj.horizon }, false);
	if (!pick.hasTarget)
		return StrCat("nothing (ground ", pick.groundTile.x, ",", pick.groundTile.y, ")");
	std::string what = "something";
	switch (pick.kind) {
	case fp::EntityKind::Monster: what = "monster"; break;
	case fp::EntityKind::Object: what = Objects[pick.id].isDoor() ? "door" : "object"; break;
	case fp::EntityKind::Item: what = "item"; break;
	default: break;
	}
	return StrCat(what, " ", pick.id, " at ", pick.tile.x, ",", pick.tile.y);
}

std::string DebugCmdFpModelTextures(bool enabled)
{
	fp::SetModelTexturesEnabled(enabled);
	return enabled ? "Model textures on." : "Model textures off (projected iso art).";
}

std::string DebugCmdFpDumpFile(std::string gamePath, std::string outPath)
{
	// Copies a file from the game data (e.g. a palette, levels\\l1data\\l1_1.pal) to disk, for offline tools.
	size_t size = 0;
	const std::unique_ptr<std::byte[]> data = LoadFileInMem<std::byte>(gamePath.c_str(), &size);
	if (data == nullptr)
		return "Not found.";
	FILE *f = std::fopen(outPath.c_str(), "wb");
	if (f == nullptr)
		return "Could not write.";
	std::fwrite(data.get(), 1, size, f);
	std::fclose(f);
	return StrCat("Dumped ", size, " bytes.");
}

std::string DebugCmdFpDumpGrid(std::string outPath)
{
	// The level's grid for offline tools: one line per tile, x y piece solid megatile special object (object type, -1).
	FILE *f = std::fopen(outPath.c_str(), "w");
	if (f == nullptr)
		return "Could not write.";
	std::fprintf(f, "x y piece solid megatile special object\n");
	for (int y = 0; y < MAXDUNY; y++) {
		for (int x = 0; x < MAXDUNX; x++) {
			const int mx = (x - 16) / 2;
			const int my = (y - 16) / 2;
			const int mega = x >= 16 && y >= 16 && mx < DMAXX && my < DMAXY ? dungeon[mx][my] : 0;
			const int id = dObject[x][y];
			const int object = id > 0 ? static_cast<int>(Objects[id - 1]._otype) : -1;
			std::fprintf(f, "%d %d %d %d %d %d %d\n", x, y, dPiece[x][y], IsTileSolid({ x, y }) ? 1 : 0, mega, static_cast<int>(dSpecial[x][y]), object);
		}
	}
	std::fclose(f);
	return StrCat("Dumped the grid to ", outPath);
}

std::string DebugCmdFpSarcophagus(bool enabled)
{
	fp::SetSarcophagusKitEnabled(enabled);
	return enabled ? "Mod-kit sarcophagus model on." : "Mod-kit sarcophagus model off.";
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
	LuaSetDocFn(table, "floormap", "(x0: number, y0: number, x1: number, y1: number, path: string)", "Dump the town's floor from above, as the first-person view draws it (PPM).", &DebugCmdFpFloorMap);
	LuaSetDocFn(table, "dumppiece", "(x: number, y: number, path: string)", "Dump the iso column art of the piece at a tile (PPM).", &DebugCmdFpDumpPiece);
	LuaSetDocFn(table, "props", "(path: string = nil)", "Count the tiles still drawn as billboards of their own art (props); with a path, list them (x y piece solid structure opaque rows r g b).", &DebugCmdFpProps);
	LuaSetDocFn(table, "sky", "(enabled: boolean)", "Draw the town's night skybox (default) or a flat sky colour.", &DebugCmdFpSky);
	LuaSetDocFn(table, "windowlight", "(enabled: boolean)", "Lamps inside the tavern, the smithy and the church, their light coming out of the windows and doorways (default), or out.", &DebugCmdFpWindowLight);
	LuaSetDocFn(table, "lowwalls", "()", "Count solid tiles whose wall art is lower than a full wall.", &DebugCmdFpLowWalls);
	LuaSetDocFn(table, "housemodels", "(enabled: boolean)", "Draw town houses with the voxel models from the Tristram plaza reference (default) or as fitted boxes.", &DebugCmdFpHouseModels);
	LuaSetDocFn(table, "housetextures", "(enabled: boolean)", "Texture the voxel houses with walls and thatch cut out of their own iso art (default) or ranked palette colours.", &DebugCmdFpHouseTextures);
	LuaSetDocFn(table, "housemeshes", "(enabled: boolean)", "Draw the house models as textured triangle meshes (default) or as voxels.", &DebugCmdFpHouseMeshes);
	LuaSetDocFn(table, "dumpiso", "(x0: number, y0: number, x1: number, y1: number, path: string, uprightOnly: boolean = false)", "Dump the original iso art of a tile rectangle (PPM); uprightOnly leaves the ground out.", &DebugCmdFpDumpIso);
	LuaSetDocFn(table, "tilemap", "(x0: number, y0: number, x1: number, y1: number)", "ASCII map of solid (#, s), tall-art (a) and building (B) tiles.", &DebugCmdFpTileMap);
	LuaSetDocFn(table, "cleanmap", "(x0: number, y0: number, x1: number, y1: number)", "ASCII map of the clean dungeon's tile kinds (# wall, l low wall, o pillar, t torch stand, D doorway, S stairs, ~ lava, ^ arch).", &DebugCmdFpCleanMap);
	LuaSetDocFn(table, "features", "()", "List stairs triggers, free-standing column pieces and objects (with sprite size and bottom padding).", &DebugCmdFpFeatures);
	LuaSetDocFn(table, "dumpfaces", "(x0: number, y0: number, x1: number, y1: number, path: string)", "Dump the wall face textures (x face, y face) of the tiles in a rectangle; openings red (PPM).", &DebugCmdFpDumpFaces);
	LuaSetDocFn(table, "recesses", "(enabled: boolean)", "Carve blind arches and niches into the walls (default) or draw them flat.", &DebugCmdFpRecesses);
	LuaSetDocFn(table, "walkto", "(x: number, y: number)", "Walk to a tile the way a click does (path finding).", &DebugCmdFpWalkTo);
	LuaSetDocFn(table, "where", "()", "Player tile and camera position (x100).", &DebugCmdFpWhere);
	LuaSetDocFn(table, "pick", "()", "What the crosshair aims at (a monster, a door, an object, an item).", &DebugCmdFpPick);
	LuaSetDocFn(table, "modeltextures", "(enabled: boolean)", "Texture town buildings with the 3D models' own textures (default) or their projected iso art.", &DebugCmdFpModelTextures);
	LuaSetDocFn(table, "dumpfile", "(gamePath: string, outPath: string)", "Copy a file from the game data (a palette, a .dun) to disk.", &DebugCmdFpDumpFile);
	LuaSetDocFn(table, "dumpgrid", "(path: string)", "Write the level's grid for offline tools: per tile its piece, solid, megatile, special and object type.", &DebugCmdFpDumpGrid);
	LuaSetDocFn(table, "sarcophagus", "(enabled: boolean)", "Use the dungeon kit's 3D sarcophagus model (default), or a box of its sprite.", &DebugCmdFpSarcophagus);
	LuaSetDocFn(table, "townwarps", "(mask: number)", "Open the town's warps to the catacombs (1), the caves (2), hell (4): the town shows their entrances when entered again.", &DebugCmdFpTownWarps);
	LuaSetDocFn(table, "operate", "(x: number, y: number)", "Operate the object at a tile, as a click on it does (open a sarcophagus, a door...).", &DebugCmdFpOperate);
	LuaSetDocFn(table, "weapon", "(file: number = nil, record: number = 0, frame: number = 0)", "Show a Daggerfall weapon frame instead of the held one; no file to go back.", &DebugCmdFpWeapon);
	LuaSetDocFn(table, "on","(enabled: boolean)", "Switch between first-person and the original view.", &DebugCmdFpOn);
	LuaSetDocFn(table, "buildings", "(enabled: boolean)", "Draw town houses as reconstructed 3D models (default) or flat billboards.", &DebugCmdFpBuildings);
	return table;
}

} // namespace devilution
#endif // _DEBUG
