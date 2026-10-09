/**
 * @file fp/fp_controls.cpp
 *
 * First-person camera state, free movement and look input.
 *
 * The camera owns a continuous world position. The player's logical tile follows it: whenever the camera
 * centre crosses into a new tile, the player is moved there directly (no walk animation), so monster AI,
 * triggers, lighting and vision keep working on the tile grid exactly as before.
 */
#include "fp/fp_controls.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

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
#include "doom.h"
#include "effects.h"
#include "engine/render/scrollrt.h"
#include "fp/fp_math.hpp"
#include "fp/fp_view.hpp"
#include "levels/dun_tile_data.hpp"
#include "levels/tile_properties.hpp"
#include "lighting.h"
#include "options.h"
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
constexpr float MaxPitch = 0.6F;
constexpr float StepSoundDistance = 0.9F; // tiles between footstep sounds
constexpr float Pi = 3.14159265F;

ViewState View {};
Point SyncedTile { -1, -1 };
bool MouseCaptured = false;
uint32_t LastFrameTicks = 0;
float PendingMouseX = 0;
float PendingMouseY = 0;
float StepDistance = 0;

Vec2 TileCenter(Point tile)
{
	return { static_cast<float>(tile.x) + 0.5F, static_cast<float>(tile.y) + 0.5F };
}

Point TileAt(float x, float y)
{
	return { static_cast<int>(std::floor(x)), static_cast<int>(std::floor(y)) };
}

/** @brief UI states that need the regular mouse cursor (panels, menus, item on cursor, ...). */
bool NeedsCursor()
{
	return InGameMenu()
	    || IsLeftPanelOpen() || IsRightPanelOpen()
	    || SpellSelectFlag || DropGoldFlag || IsWithdrawGoldOpen || DoomFlag || IsChatActive()
	    || pcurs >= CURSOR_FIRSTITEM || (pcurs != CURSOR_HAND && pcurs != CURSOR_NONE)
	    || MyPlayerIsDead;
}

/** @brief Keyboard movement is allowed unless text is being typed or a modal menu is up. */
bool KeyboardMovementAllowed()
{
	return !InGameMenu() && !IsChatActive() && !DropGoldFlag && !IsWithdrawGoldOpen;
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
#else
	SDL_SetRelativeMouseMode(capture ? SDL_TRUE : SDL_FALSE);
#endif
	if (!capture) {
		// Hand the cursor back in the middle of the view rather than wherever it was left.
		const Point center { gnScreenWidth / 2, gnViewportHeight / 2 };
		SDL_WarpMouseInWindow(ghMainWnd, center.x, center.y);
		MousePosition = center;
	}
}

/** @brief Clears what the iso cursor picking found; in mouse look the cursor position is meaningless. */
void ClearCursorTargets()
{
	pcursmonst = -1;
	pcursitem = -1;
	ObjectUnderCursor = nullptr;
	PlayerUnderCursor = nullptr;
	cursPosition = { -1, -1 };
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
	const auto ox = static_cast<int8_t>(std::lround((View.x - center.x) * 8.0F));
	const auto oy = static_cast<int8_t>(std::lround((View.y - center.y) * 8.0F));
	ChangeLightOffset(player.lightId, { ox, oy });
}

void ApplyLook(float dt)
{
	View.yaw += PendingMouseX * MouseSensitivity;
	View.pitch -= PendingMouseY * MouseSensitivity;
	PendingMouseX = 0;
	PendingMouseY = 0;

	if (!NeedsCursor()) {
		View.yaw += rightStickX * StickTurnSpeed * dt;
		View.pitch += rightStickY * StickPitchSpeed * dt;
	}

	View.yaw = std::remainder(View.yaw, 2.0F * Pi);
	View.pitch = std::clamp(View.pitch, -MaxPitch, MaxPitch);
}

/** @brief Desired movement in camera space: x = strafe right, y = forward, magnitude <= 1. */
Vec2 ReadMoveInput()
{
	Vec2 move { 0, 0 };

	if (KeyboardMovementAllowed()) {
#ifdef USE_SDL3
		const bool *keys = SDL_GetKeyboardState(nullptr);
#else
		const Uint8 *keys = SDL_GetKeyboardState(nullptr);
#endif
		if (keys[SDL_SCANCODE_W]) move.y += 1;
		if (keys[SDL_SCANCODE_S]) move.y -= 1;
		if (keys[SDL_SCANCODE_D]) move.x += 1;
		if (keys[SDL_SCANCODE_A]) move.x -= 1;
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

void ApplyFacing(Player &player)
{
	if (player._pmode != PM_STAND)
		return;
	const Direction facing = YawToDirection(View.yaw);
	if (facing != player._pdir)
		StartStand(player, facing);
}

} // namespace

const ViewState &GetViewState()
{
	return View;
}

void ResetViewState()
{
	const Player &player = *MyPlayer;
	const Vec2 center = TileCenter(player.position.tile);
	View = { center.x, center.y, DirectionToYaw(player._pdir), 0 };
	SyncedTile = player.position.tile;
	LastFrameTicks = 0;
	StepDistance = 0;
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
		// Crosshair actions arrive in M4; until then don't let iso click-to-walk fire.
		return MouseCaptured && (event.button.button == SDL_BUTTON_LEFT || event.button.button == SDL_BUTTON_RIGHT);
	case SDL_EVENT_KEY_DOWN:
	case SDL_EVENT_KEY_UP: {
		if (!KeyboardMovementAllowed())
			return false;
#ifdef USE_SDL3
		const SDL_Scancode scancode = event.key.scancode;
		const bool repeat = event.key.repeat;
#else
		const SDL_Scancode scancode = event.key.keysym.scancode;
		const bool repeat = event.key.repeat != 0;
#endif
		if (IsAnyOf(scancode, SDL_SCANCODE_W, SDL_SCANCODE_A, SDL_SCANCODE_S, SDL_SCANCODE_D))
			return true; // polled in ProcessFrame
		if (scancode == SDL_SCANCODE_T) {
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
	if (!IsFirstPersonActive()) {
		SetMouseCapture(false);
		return;
	}

	const uint32_t now = SDL_GetTicks();
	const float dt = LastFrameTicks == 0 ? 0.0F : std::min(static_cast<float>(now - LastFrameTicks) / 1000.0F, 0.1F);
	LastFrameTicks = now;

	SetMouseCapture(!NeedsCursor());
	ApplyLook(dt);
	if (MouseCaptured)
		ClearCursorTargets();

	Player &player = *MyPlayer;
	if (player.isWalking()) {
		// Movement started by the game itself (e.g. walking to an item): follow it.
		const Vec2 center = TileCenter(player.position.tile);
		const Vec2 walk = ScreenToWorld(GetOffsetForWalking(player.AnimInfo, player._pdir));
		View.x = center.x + walk.x;
		View.y = center.y + walk.y;
		SyncedTile = player.position.tile;
		return;
	}
	if (player.position.tile != SyncedTile) {
		// Teleported, changed level, knocked back, ...
		const Vec2 center = TileCenter(player.position.tile);
		View.x = center.x;
		View.y = center.y;
		SyncedTile = player.position.tile;
	}

	ApplyMovement(player, dt);
	ApplyFacing(player);
	UpdateLightOffset(player);
}

void ReleaseInput()
{
	SetMouseCapture(false);
}

} // namespace devilution::fp
