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
#include "lua/metadoc.hpp"
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

std::string DebugCmdFpDumpPieces(std::string path)
{
	fp::DumpSolidPieces(path.c_str(), 8, 4);
	return "Dumped.";
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

} // namespace

sol::table LuaDevFirstPersonModule(sol::state_view &lua)
{
	sol::table table = lua.create_table();
	LuaSetDocFn(table, "view", "(yawDegrees: number, pitch: number = nil)", "Set first-person view angles (0 = +x, 90 = +y).", &DebugCmdFpView);
	LuaSetDocFn(table, "teleport", "(x: number, y: number)", "Move the player to a free tile.", &DebugCmdFpGoto);
	LuaSetDocFn(table, "info", "(radius: number = 8)", "Describe the camera and nearby billboard entities.", &DebugCmdFpInfo);
	LuaSetDocFn(table, "dumppieces", "(path: string)", "Dump a contact sheet of solid pieces as PPM.", &DebugCmdFpDumpPieces);
	LuaSetDocFn(table, "on", "(enabled: boolean)", "Switch between first-person and the original view.", &DebugCmdFpOn);
	LuaSetDocFn(table, "buildings", "(enabled: boolean)", "Draw town houses as reconstructed 3D models (default) or flat billboards.", &DebugCmdFpBuildings);
	return table;
}

} // namespace devilution
#endif // _DEBUG
