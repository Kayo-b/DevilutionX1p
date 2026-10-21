/**
 * @file fp/fp_view.hpp
 *
 * First-person view of the live level (software raycaster).
 */
#pragma once

#include "engine/surface.hpp"

namespace devilution::fp {

/** @brief Whether the game view is rendered in first person instead of isometric. */
[[nodiscard]] bool IsFirstPersonActive();

/** @brief Switches first person on or off; in town, on as soon as its models are built (PrepareTown), the iso view playing meanwhile. */
void ToggleFirstPerson();
/** @brief Switches first person on once it was asked for and the town's models are built. Call once a frame. */
void UpdateFirstPersonActivation();
/** @brief Switches first person on as a game starts or loads, if the options say to (Start in First Person); before PrepareTown. */
void ApplyStartView();

/** @brief Whether the first-person view follows the hero from behind (the Third Person option) rather than looking through their eyes. */
[[nodiscard]] bool IsThirdPerson();
/** @brief Switches third person on or off; from the isometric view, first person comes on with it. */
void ToggleThirdPerson();

/**
 * @brief Zooms the view a step in (step < 0) or out (step > 0), and shows the new setting for a moment: the Field of
 * View option in first person, the Camera Distance option in third.
 */
void StepZoom(int step);

/**
 * @brief Renders the first-person view of the current level.
 * @param out Buffer covering the game viewport (excluding the bottom panel).
 */
void DrawFirstPersonView(const Surface &out);

} // namespace devilution::fp
