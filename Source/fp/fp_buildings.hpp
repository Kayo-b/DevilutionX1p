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
/** @brief Town buildings wear the 3D models' own textures (default) or their projected iso art. */
void SetModelTexturesEnabled(bool enabled);

/** @brief Per-tile building marks in town (index y * MAXDUNX + x): 0 none, 1 art drawn by a building, 2 also under a model whose ground art must give way to plain ground. Null outside town. */
[[nodiscard]] const uint8_t *BuildingTileMap();
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
bool DumpIsoRegion(int x0, int y0, int x1, int y1, const std::string &path, bool uprightOnly = false);
/** @brief Debug: an ASCII map of solid, tall-art and building tiles in a rectangle. */
std::string TileMap(int x0, int y0, int x1, int y1);
#endif

/**
 * @brief The mod-kit dungeon (dev.fp.modkit): walls, floors and ceilings in the dungeon kit's stone, shaded by the
 * original art's light and dark so arches, niches and doors still read; columns, sarcophagi, torch stands as the
 * kit's models. Whether it is on and ready for this level.
 */
[[nodiscard]] bool KitSurfacesActive();
/** @brief Whether closed doors are drawn as the kit's door model (the wall pass then leaves their panel open). */
[[nodiscard]] bool KitDoorsActive();
/** @param u along the wall face (tiles), v height (tiles); art the original texel, for its light and dark */
[[nodiscard]] uint8_t KitWallColor(float u, float v, int16_t art);
[[nodiscard]] uint8_t KitFloorColor(float wx, float wy, int16_t art);
[[nodiscard]] uint8_t KitCeilingColor(float wx, float wy);
void SetModKitEnabled(bool enabled);

/** @brief Whether a dungeon tile is drawn as part of a mesh structure (a free-standing column, the stairs down) instead of a wall cube or billboard. */
[[nodiscard]] bool IsStructureTile(Point tile);
/**
 * @brief Whether a dungeon tile is a pillar carrying the end of an arch: drawn as a wall from its own (partial)
 * face art, the parts without art left open to look through.
 */
[[nodiscard]] bool IsArchPillarTile(Point tile);
/** @brief The per-tile structure kinds (MAXDUNX * MAXDUNY, index y * MAXDUNX + x; 0 none, 3 arch pillar, else a mesh), null in town. For per-pixel loops. */
[[nodiscard]] const uint8_t *StructureTileMap();
[[nodiscard]] bool HasFloorHoles();
/** @brief Whether the floor is open at a point (the pit of the stairs down), so the floor is not drawn there. */
[[nodiscard]] bool IsFloorHole(float x, float y);
/** @brief Whether an object is drawn as a 3D box of its own sprite (a sarcophagus) rather than a billboard. */
[[nodiscard]] bool IsObjectDrawnAsMesh(int id);
/** @brief Draws the dungeon's mesh structures, depth tested against everything drawn so far. */
void DrawStructures(const Surface &out, const Projection &proj, std::vector<float> &depthBuffer);

/** @brief Debug: mesh rasterizer counters per frame since the last call, as text. */
std::string TakeRasterStats(int frames);

/** @brief Number of reconstructed buildings in the current level (debug). */
[[nodiscard]] int BuildingCount();

} // namespace devilution::fp
