/**
 * @file fp/fp_controls.hpp
 *
 * First-person camera state, free movement and look input.
 */
#pragma once

#ifdef USE_SDL3
#include <SDL3/SDL_events.h>
#else
#include <SDL.h>
#endif

#include "engine/point.hpp"
#include "fp/fp_math.hpp"

namespace devilution::fp {

struct ViewState {
	/** World position in dPiece tile units (tile (x, y) covers [x, x+1) x [y, y+1)). */
	float x;
	float y;
	/** Heading in radians, 0 = +x (SouthEast in game directions), increasing towards +y. */
	float yaw;
	/** Vertical look, as a fraction of the focal length (positive looks up). */
	float pitch;
};

[[nodiscard]] const ViewState &GetViewState();

/** @brief Where the view is seen from: the hero's eyes, or in third person behind and above them. */
struct Camera {
	Vec2 pos;
	/** Height above the floor. */
	float eye;
	/** Added to the look's pitch: the third-person camera looks down a little, over the hero's head. */
	float pitch;
};

[[nodiscard]] const Camera &GetCamera();

/** @brief How the hero is going, to draw them walking in third person (first person glides them, with no walk of the game's). */
struct Stride {
	bool moving;
	/** The way they go. */
	float yaw;
	/** How far they went since they set off, in tiles. */
	float distance;
};

[[nodiscard]] const Stride &GetStride();

/** @brief Re-initialises the camera from the player's tile and facing (used when entering first person). */
void ResetViewState();

/** @brief True when the mouse is captured for looking around (first person, no UI needing a cursor). */
[[nodiscard]] bool IsMouseLookActive();

/**
 * @brief Handles first-person input events.
 * @return true if the event was consumed and must not reach the regular game handlers.
 */
bool HandleEvent(const SDL_Event &event);

/** @brief Per-frame update: look, movement, collision and syncing the player's logical tile. */
void ProcessFrame();

/** @brief Debug helpers (console): set the view angles, or move the player to a free tile. */
void SetViewAngles(float yaw, float pitch);
bool TeleportPlayer(Point tile);

/** @brief While the mouse is captured, clicks act on the crosshair: call after taking the position from a mouse event. */
void PinMouseToCrosshair();

/** @brief Called when leaving first person to release the mouse. */
void ReleaseInput();

} // namespace devilution::fp
