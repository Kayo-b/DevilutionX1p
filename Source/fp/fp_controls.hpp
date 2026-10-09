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

/** @brief Called when leaving first person to release the mouse. */
void ReleaseInput();

} // namespace devilution::fp
