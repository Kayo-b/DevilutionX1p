/**
 * @file fp/fp_controls.cpp
 *
 * First-person camera state, free movement and look input.
 *
 * The camera owns a continuous world position. The player's logical tile follows it: whenever the camera
 * centre crosses into a new tile, the player is moved there directly (no walk animation), so monster AI,
 * triggers, lighting and vision keep working on the tile grid exactly as before.
 *
 * In third person the view is seen from behind that position instead (GetCamera), which stays the hero's.
 */
#include "fp/fp_controls.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

#ifdef USE_SDL3
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_timer.h>
#endif

#include "control/control.hpp"
#include "controls/controller_motion.h"
#include "controls/control_mode.hpp"
#include "controls/game_controls.h"
#include "controls/keymapper.hpp"
#include "controls/plrctrls.h"
#include "cursor.h"
#include "diablo.h"
#include "automap.h"
#include "doom.h"
#include "effects.h"
#include "engine/render/scrollrt.h"
#include "fp/fp_buildings.hpp"
#include "fp/fp_dungeon.hpp"
#include "fp/fp_hell.hpp"
#include "fp/fp_math.hpp"
#include "fp/fp_raycast.hpp"
#include "fp/fp_view.hpp"
#include "levels/dun_tile_data.hpp"
#include "levels/tile_properties.hpp"
#include "lighting.h"
#include "options.h"
#include "panels/console.hpp"
#include "player.h"
#include "qol/autopickup.h"
#include "qol/stash.h"
#include "utils/display.h"
#include "utils/is_of.hpp"
#include "utils/sdl_compat.h"
#include "utils/ui_fwd.h"

namespace devilution::fp {

namespace {

// Tunables.
constexpr float MoveSpeed = 2.8F;           // tiles per second (original walk is ~2.5 straight, ~3.5 diagonal)
constexpr float CollisionRadius = 0.2F;     // half-size of the camera's collision box, in tiles
constexpr float MouseSensitivity = 0.0025F; // radians per mouse count
constexpr float StickTurnSpeed = 3.0F;      // radians per second at full deflection
constexpr float StickPitchSpeed = 1.5F;     // focal fractions per second at full deflection
constexpr float KeyTurnSpeed = 2.5F;        // radians per second (arrow keys)
constexpr float KeyPitchSpeed = 1.0F;       // focal fractions per second (arrow keys)
constexpr float MaxPitch = 1.25F;        // tan of about 51 degrees up or down
constexpr float StepSoundDistance = 0.9F; // tiles between footstep sounds
constexpr float Pi = 3.14159265F;
// Third person: the camera stands behind the hero and a little to their right (over the shoulder, so the crosshair
// is clear of them), above their head, looking down a little so the crosshair's line passes just over it.
constexpr float CameraBackBase = 0.9F;     // tiles behind the hero at no Camera Distance step
constexpr float CameraBackPerStep = 0.6F;  // and further back a step
constexpr float ShoulderShift = 0.25F;     // tiles to the right per tile behind
constexpr float CameraRise = 0.28F;        // tiles above the eye per tile behind
constexpr float CeilingClearance = 0.45F;  // how far under the ceiling it stays
constexpr float HeadHeight = 1.6F;         // where the crosshair's line passes the hero
constexpr float CameraBackOffSpeed = 3.0F; // tiles per second it goes back out once a wall let it come in
constexpr float CameraProbeStep = 0.05F;   // tiles between the wall probes

ViewState View {};
Point SyncedTile { -1, -1 };
bool MouseCaptured = false;
uint32_t LastFrameTicks = 0;
float PendingMouseX = 0;
float PendingMouseY = 0;
float StepDistance = 0;
Camera Cam {};
/** How far the third-person camera stands back from the hero now, and to their right. */
float CameraBack = 0;
float CameraShift = 0;
/** Whether the camera was put behind the hero since the view was reset: until then it goes there at once. */
bool CameraPlaced = false;
Stride HeroStride {};
Vec2 LastHeroPos {};
float StillFor = 0;

Vec2 TileCenter(Point tile)
{
	return { static_cast<float>(tile.x) + 0.5F, static_cast<float>(tile.y) + 0.5F };
}

Point TileAt(float x, float y)
{
	return { static_cast<int>(std::floor(x)), static_cast<int>(std::floor(y)) };
}

// The keys polled each frame: by scancode, or in SDL 1.2 (which has none) by key symbol.
#ifdef USE_SDL1
using HeldKey = SDLKey;
constexpr HeldKey KeyForward = SDLK_w, KeyBack = SDLK_s, KeyStrafeLeft = SDLK_a, KeyStrafeRight = SDLK_d, KeySpells = SDLK_t;
constexpr HeldKey KeyTurnLeft = SDLK_LEFT, KeyTurnRight = SDLK_RIGHT, KeyLookUp = SDLK_UP, KeyLookDown = SDLK_DOWN;
#else
using HeldKey = SDL_Scancode;
constexpr HeldKey KeyForward = SDL_SCANCODE_W, KeyBack = SDL_SCANCODE_S, KeyStrafeLeft = SDL_SCANCODE_A, KeyStrafeRight = SDL_SCANCODE_D,
                  KeySpells = SDL_SCANCODE_T;
constexpr HeldKey KeyTurnLeft = SDL_SCANCODE_LEFT, KeyTurnRight = SDL_SCANCODE_RIGHT, KeyLookUp = SDL_SCANCODE_UP, KeyLookDown = SDL_SCANCODE_DOWN;
#endif

bool KeyHeld(HeldKey key)
{
#ifdef USE_SDL1
	return SDL_GetKeyState(nullptr)[key] != 0;
#else
	return SDL_GetKeyboardState(nullptr)[key] != 0;
#endif
}

/** @brief The developer console (debug builds only) is open: it takes the keyboard and the cursor. */
bool ConsoleOpen()
{
#ifdef _DEBUG
	return IsConsoleOpen();
#else
	return false;
#endif
}

/** @brief UI states that need the regular mouse cursor (panels, menus, item on cursor, ...). */
bool NeedsCursor()
{
	return InGameMenu()
	    || IsLeftPanelOpen() || IsRightPanelOpen()
	    || SpellSelectFlag || DropGoldFlag || IsWithdrawGoldOpen || DoomFlag || IsChatActive() || ConsoleOpen()
	    || pcurs >= CURSOR_FIRSTITEM || IsAnyOf(pcurs, CURSOR_IDENTIFY, CURSOR_REPAIR, CURSOR_RECHARGE, CURSOR_OIL, CURSOR_HOURGLASS)
	    || MyPlayerIsDead;
}

/** @brief Keyboard movement is allowed unless text is being typed or a modal menu is up. */
bool KeyboardMovementAllowed()
{
	return !InGameMenu() && !IsChatActive() && !DropGoldFlag && !IsWithdrawGoldOpen && !ConsoleOpen();
}

void SetMouseCapture(bool capture)
{
	if (capture == MouseCaptured)
		return;
	MouseCaptured = capture;
	PendingMouseX = 0;
	PendingMouseY = 0;
#ifdef USE_SDL3
	SDL_SetWindowRelativeMouseMode(ghMainWnd, capture);
#elif defined(USE_SDL1)
	// Grabbed with the cursor hidden, SDL 1.2 reports relative motion past the window's edges.
	SDL_WM_GrabInput(capture ? SDL_GRAB_ON : SDL_GRAB_OFF);
	SDL_ShowCursor(capture ? SDL_DISABLE : SDL_ENABLE);
#else
	SDL_SetRelativeMouseMode(capture ? SDL_TRUE : SDL_FALSE);
#endif
	// The game's cursor logic reads MousePosition; while looking it is pinned to the crosshair.
	const Point center { gnScreenWidth / 2, gnViewportHeight / 2 };
	MousePosition = center;
	if (!capture) {
		// Hand the cursor back in the middle of the view rather than wherever it was left.
		SDL_WarpMouseInWindow(ghMainWnd, center.x, center.y);
	}
}

/** @brief Static obstacles for the camera box: solid tiles and solid objects (closed doors, barrels, ...). */
bool IsBlockedForCamera(Point tile)
{
	return !InDungeonBounds(tile) || !IsTileWalkable(tile);
}

/** @brief Whether a tile can become the player's logical tile. */
bool CanEnterTile(Point from, Point to)
{
	if (to == from)
		return true;
	if (IsBlockedForCamera(to))
		return false;
	if (dMonster[to.x][to.y] != 0)
		return false;
	if (dPlayer[to.x][to.y] != 0 && PlayerAtPosition(to) != MyPlayer)
		return false;
	// Diagonal steps must not cut corners, same rule as the original pathing.
	if (to.x != from.x && to.y != from.y && !CanStep(from, to))
		return false;
	return true;
}

/** @brief Whether the camera's collision box centred at (x, y) overlaps static obstacles. */
bool BoxHitsWall(float x, float y)
{
	const Point minTile = TileAt(x - CollisionRadius, y - CollisionRadius);
	const Point maxTile = TileAt(x + CollisionRadius, y + CollisionRadius);
	for (int ty = minTile.y; ty <= maxTile.y; ty++) {
		for (int tx = minTile.x; tx <= maxTile.x; tx++) {
			if (IsBlockedForCamera({ tx, ty }))
				return true;
		}
	}
	return false;
}

bool CanOccupy(Point currentTile, float x, float y)
{
	return !BoxHitsWall(x, y) && CanEnterTile(currentTile, TileAt(x, y));
}

/** @brief Moves the player's logical tile without a walk animation (mirrors the end of DoWalk). */
void MovePlayerToTile(Player &player, Point tile)
{
	FixPlrWalkTags(player);
	player.position.tile = tile;
	FixPlayerLocation(player, player._pdir);
	player.occupyTile(tile, false);
	SetPlayerOld(player);
	SyncedTile = tile;
	AutoPickup(player);
}

void UpdateLightOffset(const Player &player)
{
	if (player.lightId == NO_LIGHT || leveltype == DTYPE_TOWN)
		return;
	const Vec2 center = TileCenter(player.position.tile);
	// DoLighting indexes its tables with the offset and only accepts -8 .. 7 (eighths of a tile). The camera can be
	// more than a tile from the player's tile (gliding to a walk step the game took at once), so clamp it.
	auto eighths = [](float d) { return static_cast<int8_t>(std::clamp<long>(std::lround(d * 8.0F), -7, 7)); };
	ChangeLightOffset(player.lightId, { eighths(View.x - center.x), eighths(View.y - center.y) });
}

/** @brief How far behind the hero the third-person camera stands, in tiles, at the Camera Distance option. */
float WantedCameraBack()
{
	return CameraBackBase + CameraBackPerStep * static_cast<float>(std::clamp(*GetOptions().Graphics.cameraDistance, 1, 10));
}

/** @brief The highest the camera may be: under the ceiling (the caves' wall tops). None in town, nor in hell, whose walls are low. */
float CameraCeiling()
{
	if (leveltype == DTYPE_TOWN || IsHell())
		return std::numeric_limits<float>::infinity();
	if (IsCleanDungeon())
		return CleanCeiling();
	return WallHeight() * VerticalScale();
}

/**
 * @brief What the third-person camera does not go into: what the hero cannot, but in town only the buildings (and the
 * map's edge): it goes over trees, fences and water. While the town's models are built, what the hero cannot.
 */
bool StopsCamera(Point tile)
{
	if (leveltype == DTYPE_TOWN && InDungeonBounds(tile) && BuildingsEnabled() && !IsTownBuilding())
		return IsBuildingTile(tile);
	return IsBlockedForCamera(tile);
}

/** StopsCamera of the tiles asked this frame: a town tile asks the buildings, which is slow for the many probes. */
std::vector<std::pair<Point, bool>> CameraTiles;

bool CameraBoxBlocked(float x, float y)
{
	const Point minTile = TileAt(x - CollisionRadius, y - CollisionRadius);
	const Point maxTile = TileAt(x + CollisionRadius, y + CollisionRadius);
	for (int ty = minTile.y; ty <= maxTile.y; ty++) {
		for (int tx = minTile.x; tx <= maxTile.x; tx++) {
			const Point tile { tx, ty };
			auto known = std::find_if(CameraTiles.begin(), CameraTiles.end(), [&](const auto &entry) { return entry.first == tile; });
			if (known == CameraTiles.end())
				known = CameraTiles.insert(CameraTiles.end(), { tile, StopsCamera(tile) });
			if (known->second)
				return true;
		}
	}
	return false;
}

/** @brief How far the camera's box goes from `from` along `way` (a unit vector), up to `wanted`, before it meets a wall. */
float RoomAlong(Vec2 from, Vec2 way, float wanted)
{
	float room = 0;
	for (float t = CameraProbeStep; t <= wanted + CameraProbeStep / 2; t += CameraProbeStep) {
		const float at = std::min(t, wanted);
		if (CameraBoxBlocked(from.x + way.x * at, from.y + way.y * at))
			break;
		room = at;
	}
	return room;
}

/** @brief Comes in at once to `room`, or goes back out towards it at CameraBackOffSpeed (at once if `snap`). */
float Approach(float current, float room, float dt, bool snap)
{
	if (snap || room <= current)
		return room;
	return std::min(room, current + CameraBackOffSpeed * dt);
}

void UpdateStride(float dt)
{
	const Vec2 step { View.x - LastHeroPos.x, View.y - LastHeroPos.y };
	LastHeroPos = { View.x, View.y };
	const float moved = std::hypot(step.x, step.y);
	if (moved > 0.0005F && moved < 1.0F) {
		HeroStride.moving = true;
		HeroStride.yaw = std::atan2(step.y, step.x);
		HeroStride.distance += moved;
		StillFor = 0;
		return;
	}
	// A frame without a step (the game's tick) does not stop the walk; a tenth of a second does.
	StillFor += dt;
	if (StillFor > 0.1F) {
		HeroStride.moving = false;
		HeroStride.distance = 0;
	}
}

/** @brief Puts the camera where it sees the hero from this frame, at once if the view was just reset. */
void UpdateCamera(float dt)
{
	const Vec2 hero { View.x, View.y };
	if (!IsThirdPerson()) {
		// At the eyes: switched to third person, the camera glides back from here.
		Cam = { hero, EyeHeight, 0 };
		CameraBack = 0;
		CameraShift = 0;
		CameraPlaced = true;
		return;
	}
	const bool snap = !CameraPlaced;
	CameraPlaced = true;
	const float wanted = WantedCameraBack();
	CameraTiles.clear();
	const Vec2 dir { std::cos(View.yaw), std::sin(View.yaw) };
	const Vec2 right { -dir.y, dir.x };
	// To the right first, then back from there, each as far as the walls let it.
	CameraShift = Approach(CameraShift, RoomAlong(hero, right, wanted * ShoulderShift), dt, snap);
	const Vec2 shoulder { hero.x + right.x * CameraShift, hero.y + right.y * CameraShift };
	CameraBack = Approach(CameraBack, RoomAlong(shoulder, { -dir.x, -dir.y }, wanted), dt, snap);
	Cam.pos = { shoulder.x - dir.x * CameraBack, shoulder.y - dir.y * CameraBack };
	// Its height and its look down go by where it wants to be, so a wall bringing it in does not tilt the view.
	Cam.eye = std::max(EyeHeight, std::min(EyeHeight + CameraRise * wanted, CameraCeiling() - CeilingClearance));
	Cam.pitch = -std::max(Cam.eye - HeadHeight, 0.0F) / wanted;
}

void ApplyLook(float dt)
{
	// Zoomed in (a narrow field of view), the mouse turns the view slower so it moves as far on the screen.
	const float sensitivity = MouseSensitivity * std::min(PlaneLength(), 1.0F);
	View.yaw += PendingMouseX * sensitivity;
	View.pitch -= PendingMouseY * sensitivity;
	PendingMouseX = 0;
	PendingMouseY = 0;

	if (!NeedsCursor()) {
		View.yaw += rightStickX * StickTurnSpeed * dt;
		View.pitch += rightStickY * StickPitchSpeed * dt;
	}

	// Arrow keys turn and look for keyboard-only play; the automap uses them for scrolling.
	if (!NeedsCursor() && KeyboardMovementAllowed() && !AutomapActive) {
		if (KeyHeld(KeyTurnLeft)) View.yaw -= KeyTurnSpeed * dt;
		if (KeyHeld(KeyTurnRight)) View.yaw += KeyTurnSpeed * dt;
		if (KeyHeld(KeyLookUp)) View.pitch += KeyPitchSpeed * dt;
		if (KeyHeld(KeyLookDown)) View.pitch -= KeyPitchSpeed * dt;
	}

	View.yaw = std::remainder(View.yaw, 2.0F * Pi);
	View.pitch = std::clamp(View.pitch, -MaxPitch, MaxPitch);
}

/** @brief Desired movement in camera space: x = strafe right, y = forward, magnitude <= 1. */
Vec2 ReadMoveInput()
{
	Vec2 move { 0, 0 };

	if (KeyboardMovementAllowed()) {
		if (KeyHeld(KeyForward)) move.y += 1;
		if (KeyHeld(KeyBack)) move.y -= 1;
		if (KeyHeld(KeyStrafeRight)) move.x += 1;
		if (KeyHeld(KeyStrafeLeft)) move.x -= 1;
	}

	if (!IsMovementHandlerActive() && !InGameMenu() && !PadMenuNavigatorActive && !PadHotspellMenuActive) {
		move.x += leftStickX;
		move.y += leftStickY;
		const AxisDirection dpad = GetLeftStickOrDpadDirection(true);
		if (leftStickX == 0 && leftStickY == 0) {
			if (dpad.x == AxisDirectionX_RIGHT) move.x += 1;
			if (dpad.x == AxisDirectionX_LEFT) move.x -= 1;
			if (dpad.y == AxisDirectionY_UP) move.y += 1;
			if (dpad.y == AxisDirectionY_DOWN) move.y -= 1;
		}
	}

	const float length = std::hypot(move.x, move.y);
	if (length > 1.0F) {
		move.x /= length;
		move.y /= length;
	}
	return move;
}

void ApplyMovement(Player &player, float dt)
{
	const Vec2 input = ReadMoveInput();
	if (input.x == 0 && input.y == 0)
		return;
	if (player._pmode != PM_STAND)
		return; // attacking, casting, getting hit, ... lock movement like the original

	const float cosYaw = std::cos(View.yaw);
	const float sinYaw = std::sin(View.yaw);
	// Forward is (cos, sin); right is the forward vector rotated +90 degrees in a y-down world.
	const float dx = (cosYaw * input.y - sinYaw * input.x) * MoveSpeed * dt;
	const float dy = (sinYaw * input.y + cosYaw * input.x) * MoveSpeed * dt;

	const Point currentTile = player.position.tile;
	const float startX = View.x;
	const float startY = View.y;
	// Resolve each axis separately so the camera slides along walls.
	if (CanOccupy(currentTile, View.x + dx, View.y))
		View.x += dx;
	if (CanOccupy(currentTile, View.x, View.y + dy))
		View.y += dy;

	const Point newTile = TileAt(View.x, View.y);
	if (newTile != currentTile && !CanEnterTile(currentTile, newTile)) {
		View.x = startX;
		View.y = startY;
		return;
	}
	if (newTile != currentTile)
		MovePlayerToTile(player, newTile);

	StepDistance += std::hypot(View.x - startX, View.y - startY);
	if (StepDistance >= StepSoundDistance) {
		StepDistance = 0;
		if (*GetOptions().Audio.walkingSound)
			PlaySfxLoc(SfxID::Walk, player.position.tile);
	}
}

/**
 * Walks the game starts itself (clicking an item or a monster) are taken over so they go at the first-person speed:
 * each step's tile is taken at once, the camera glides to it at MoveSpeed, and the rest of the path and the action
 * at its end are held back until it arrives.
 */
struct HeldWalk {
	bool active = false;
	std::array<int8_t, MaxPathLengthPlayer> path {};
	action_id action = ACTION_NONE;
	std::array<int, 4> params {};
};
HeldWalk Held;

void TakeOverWalk(Player &player)
{
	const Point target = player.position.future;
	const Direction dir = player._pdir;
	std::copy(std::begin(player.walkpath), std::end(player.walkpath), Held.path.begin());
	Held.action = player.destAction;
	Held.params = { player.destParam1, player.destParam2, player.destParam3, player.destParam4 };
	Held.active = true;
	player.walkpath[0] = WALK_NONE;
	player.destAction = ACTION_NONE;
	StartStand(player, dir);
	if (InDungeonBounds(target) && target != player.position.tile)
		MovePlayerToTile(player, target);
}

/** @returns whether the camera is still on its way to the player's tile */
bool GlideToHeldStep(Player &player, float dt)
{
	const Vec2 goal = TileCenter(player.position.tile);
	const float dx = goal.x - View.x;
	const float dy = goal.y - View.y;
	const float distance = std::hypot(dx, dy);
	const float step = MoveSpeed * dt;
	if (distance > step) {
		View.x += dx / distance * step;
		View.y += dy / distance * step;
		StepDistance += step;
		if (StepDistance >= StepSoundDistance) {
			StepDistance = 0;
			if (*GetOptions().Audio.walkingSound)
				PlaySfxLoc(SfxID::Walk, player.position.tile);
		}
		return true;
	}
	View.x = goal.x;
	View.y = goal.y;
	// Arrived: hand the rest of the path and the action back to the game.
	std::copy(Held.path.begin(), Held.path.end(), std::begin(player.walkpath));
	player.destAction = Held.action;
	player.destParam1 = Held.params[0];
	player.destParam2 = Held.params[1];
	player.destParam3 = Held.params[2];
	player.destParam4 = Held.params[3];
	Held.active = false;
	return false;
}

void ApplyFacing(Player &player)
{
	if (player._pmode != PM_STAND)
		return;
	const Direction facing = YawToDirection(View.yaw);
	if (facing != player._pdir)
		StartStand(player, facing);
}

/** @brief What follows where the hero now is: their facing, their light, their stride and the camera. */
void FinishFrame(Player &player, float dt)
{
	ApplyFacing(player);
	UpdateLightOffset(player);
	UpdateStride(dt);
	UpdateCamera(dt);
}

} // namespace

const ViewState &GetViewState()
{
	return View;
}

const Camera &GetCamera()
{
	return Cam;
}

const Stride &GetStride()
{
	return HeroStride;
}

void ResetViewState()
{
	const Player &player = *MyPlayer;
	const Vec2 center = TileCenter(player.position.tile);
	View = { center.x, center.y, DirectionToYaw(player._pdir), 0 };
	SyncedTile = player.position.tile;
	LastFrameTicks = 0;
	StepDistance = 0;
	LastHeroPos = center;
	HeroStride = {};
	// At the hero's eyes until the first frame puts it behind them (the level may not be loaded yet).
	Cam = { center, EyeHeight, 0 };
	CameraBack = 0;
	CameraShift = 0;
	CameraPlaced = false;
}

bool IsMouseLookActive()
{
	return IsFirstPersonActive() && !NeedsCursor();
}

bool HandleEvent(const SDL_Event &event)
{
	if (!IsFirstPersonActive())
		return false;

	switch (event.type) {
	case SDL_EVENT_MOUSE_MOTION:
		if (!MouseCaptured)
			return false;
		PendingMouseX += static_cast<float>(event.motion.xrel);
		PendingMouseY += static_cast<float>(event.motion.yrel);
		return true;
	case SDL_EVENT_MOUSE_BUTTON_DOWN:
	case SDL_EVENT_MOUSE_BUTTON_UP:
		// Clicks go through the regular handlers; see PinMouseToCrosshair().
		return false;
	case SDL_EVENT_KEY_DOWN:
	case SDL_EVENT_KEY_UP: {
		if (!KeyboardMovementAllowed())
			return false;
#ifdef USE_SDL3
		const HeldKey key = event.key.scancode;
		const bool repeat = event.key.repeat;
#elif defined(USE_SDL1)
		const HeldKey key = event.key.keysym.sym;
		const bool repeat = false; // the game turns no key repeat on in SDL 1.2
#else
		const HeldKey key = event.key.keysym.scancode;
		const bool repeat = event.key.repeat != 0;
#endif
		if (IsAnyOf(key, KeyForward, KeyStrafeLeft, KeyBack, KeyStrafeRight))
			return true; // polled in ProcessFrame
		if (key == KeySpells) {
			// The Speedbook lives on S in iso; give it a first-person home.
			if (event.type == SDL_EVENT_KEY_DOWN && !repeat) {
				const uint32_t speedbookKey = GetOptions().Keymapper.KeyForAction("DisplaySpells");
				if (speedbookKey != SDLK_UNKNOWN)
					KeymapperPress(static_cast<SDL_Keycode>(speedbookKey));
			}
			return true;
		}
		return false;
	}
	default:
		return false;
	}
}

void ProcessFrame()
{
	UpdateFirstPersonActivation();
	if (!IsFirstPersonActive()) {
		SetMouseCapture(false);
		return;
	}

	const uint32_t now = SDL_GetTicks();
	const float dt = LastFrameTicks == 0 ? 0.0F : std::min(static_cast<float>(now - LastFrameTicks) / 1000.0F, 0.1F);
	LastFrameTicks = now;

	SetMouseCapture(!NeedsCursor());
	ApplyLook(dt);

	Player &player = *MyPlayer;
	if (player.isWalking() && player._pmode != PM_DEATH) {
		// Movement started by the game itself (walking to an item or a monster): at the first-person speed.
		TakeOverWalk(player);
	}
	if (Held.active) {
		const Vec2 input = ReadMoveInput();
		if (input.x != 0 || input.y != 0) {
			Held.active = false; // the player took over: drop the rest of the path
		} else {
			SyncedTile = player.position.tile;
			if (GlideToHeldStep(player, dt)) {
				FinishFrame(player, dt);
				return;
			}
		}
	}
	if (player.position.tile != SyncedTile) {
		// Teleported, changed level, knocked back, ...
		const Vec2 center = TileCenter(player.position.tile);
		View.x = center.x;
		View.y = center.y;
		SyncedTile = player.position.tile;
	}

	ApplyMovement(player, dt);
	FinishFrame(player, dt);
}

void SetViewAngles(float yaw, float pitch)
{
	View.yaw = std::remainder(yaw, 2.0F * Pi);
	View.pitch = std::clamp(pitch, -MaxPitch, MaxPitch);
}

bool TeleportPlayer(Point tile)
{
	Player &player = *MyPlayer;
	if (!InDungeonBounds(tile) || !IsTileWalkable(tile) || dMonster[tile.x][tile.y] != 0 || player._pmode != PM_STAND)
		return false;
	MovePlayerToTile(player, tile);
	const Vec2 center = TileCenter(tile);
	View.x = center.x;
	View.y = center.y;
	return true;
}

void PinMouseToCrosshair()
{
	if (MouseCaptured)
		MousePosition = { gnScreenWidth / 2, gnViewportHeight / 2 };
}

void ReleaseInput()
{
	SetMouseCapture(false);
}

} // namespace devilution::fp
