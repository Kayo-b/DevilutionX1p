/**
 * @file fp/fp_sky.hpp
 *
 * The night sky over town in first person: a cube map (assets/fp/sky.bin, made by fp-tools/export/export_sky.py).
 */
#pragma once

#include <vector>

#include "engine/surface.hpp"
#include "fp/fp_raycast.hpp"

namespace devilution::fp {

/** @brief Whether the town's sky is the skybox (default, if sky.bin is there) or a flat colour. */
[[nodiscard]] bool SkyEnabled();
void SetSkyEnabled(bool enabled);

/**
 * @brief Draws the sky into the pixels at or above the horizon that nothing has been drawn on yet (their depth is
 * still infinite). False if there is no sky to draw (outside town, switched off, or sky.bin missing).
 */
bool DrawSky(const Surface &out, const Projection &proj, const std::vector<float> &depthBuffer);

} // namespace devilution::fp
