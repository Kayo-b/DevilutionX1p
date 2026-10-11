/**
 * @file fp/fp_weapon.hpp
 *
 * The weapon in the player's hand, drawn over the first-person view.
 */
#pragma once

#include "engine/surface.hpp"

namespace devilution::fp {

/** @brief Draws the equipped weapon (or the bare hands) at the bottom of the view, swinging with the attacks. */
void DrawFirstPersonWeapon(const Surface &out);

/** @brief Debug: shows WEAPON<file>.CIF's record and frame instead of the held weapon; a negative file stops. */
void PreviewFirstPersonWeapon(int file, int record, int frame);

} // namespace devilution::fp
