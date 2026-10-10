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

#include <string>
#include <vector>

#include "engine/point.hpp"
#include "engine/surface.hpp"
#include "fp/fp_raycast.hpp"

namespace devilution::fp {

/** @brief Whether town buildings are drawn as reconstructed 3D models (on by default) or as flat billboards. */
[[nodiscard]] bool BuildingsEnabled();
void SetBuildingsEnabled(bool enabled);

/** @brief Whether reconstructed houses use the voxel models from the Tristram plaza reference (on by default if the data is present). */
[[nodiscard]] bool HouseModelsEnabled();
void SetHouseModelsEnabled(bool enabled);
/** @brief Voxel houses wear the walls and thatch cut out of their own iso art (default) or brightness-ranked palette colours. */
void SetHouseTexturesEnabled(bool enabled);
/** @brief Voxel-model houses are drawn as the reference triangle mesh with full-resolution art textures (default) or as voxels. */
void SetHouseMeshesEnabled(bool enabled);

/** @brief Whether the tile belongs to a reconstructed building (and so is not drawn as a billboard). */
[[nodiscard]] bool IsBuildingTile(Point tile);

/**
 * @brief Draws all reconstructed buildings in view.
 * @param depthBuffer per-pixel camera depth (width * height), updated for every pixel drawn; sprites use it to
 * be hidden behind buildings.
 */
void DrawBuildings(const Surface &out, const Projection &proj, const std::vector<float> &zBuffer, std::vector<float> &depthBuffer);

#ifdef _DEBUG
/** @brief Debug: stitches the original iso art of a tile rectangle into a PPM, with the tile grid's own layout. */
bool DumpIsoRegion(int x0, int y0, int x1, int y1, const std::string &path);
/** @brief Debug: an ASCII map of solid, tall-art and building tiles in a rectangle. */
std::string TileMap(int x0, int y0, int x1, int y1);
#endif

/** @brief Whether a dungeon tile is drawn as part of a mesh structure (a free-standing column, the stairs down) instead of a wall cube or billboard. */
[[nodiscard]] bool IsStructureTile(Point tile);
/** @brief Whether the floor is open at a point (the pit of the stairs down), so the floor is not drawn there. */
[[nodiscard]] bool IsFloorHole(float x, float y);
/** @brief Draws the dungeon's mesh structures, depth tested against everything drawn so far. */
void DrawStructures(const Surface &out, const Projection &proj, std::vector<float> &depthBuffer);

/** @brief Number of reconstructed buildings in the current level (debug). */
[[nodiscard]] int BuildingCount();

} // namespace devilution::fp
