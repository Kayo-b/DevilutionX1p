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

void ToggleFirstPerson();

/**
 * @brief Renders the first-person view of the current level.
 * @param out Buffer covering the game viewport (excluding the bottom panel).
 */
void DrawFirstPersonView(const Surface &out);

} // namespace devilution::fp
