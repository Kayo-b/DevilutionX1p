/**
 * @file fp/fp_buildings.hpp
 *
 * Town buildings rebuilt as simple 3D models (a box with a gable roof) textured from the original iso art.
 *
 * The tiles of a building are grouped, their iso columns are stitched into one picture, and a box-plus-roof model
 * is fitted so its iso silhouette matches the picture. Each surface of the model is then coloured by projecting
 * it back into that picture, so windows and doors land where the art has them, and the far sides (which the iso
 * camera never saw) mirror the visible ones.
 */
#pragma once

#include <vector>

#include "engine/point.hpp"
#include "engine/surface.hpp"
#include "fp/fp_raycast.hpp"

namespace devilution::fp {

/** @brief Whether town buildings are drawn as reconstructed 3D models (on by default) or as flat billboards. */
[[nodiscard]] bool BuildingsEnabled();
void SetBuildingsEnabled(bool enabled);

/** @brief Whether the tile belongs to a reconstructed building (and so is not drawn as a billboard). */
[[nodiscard]] bool IsBuildingTile(Point tile);

/**
 * @brief Draws all reconstructed buildings in view.
 * @param depthBuffer per-pixel camera depth (width * height), updated for every pixel drawn; sprites use it to
 * be hidden behind buildings.
 */
void DrawBuildings(const Surface &out, const Projection &proj, const std::vector<float> &zBuffer, std::vector<float> &depthBuffer);

/** @brief Number of reconstructed buildings in the current level (debug). */
[[nodiscard]] int BuildingCount();

} // namespace devilution::fp
