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

#include <cstdio>
#include <string>
#include <vector>

#include "engine/point.hpp"
#include "engine/surface.hpp"
#include "fp/fp_raycast.hpp"
#include "fp/fp_world.hpp"

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

/**
 * @brief Before the level goes (or the game ends): stops working out the town lamps' light (its thread must not
 * outlive the town's models); the next level builds its own.
 */
void StopTownBuild();

/**
 * @brief Per-tile building marks in town (index y * MAXDUNX + x): 0 none, 1 art drawn by a building, 2 also under a
 * model whose ground art must give way to plain ground, 3 a prop drawn as a mesh (a tree, a rock: its own ground
 * without the prop stays). Null outside town.
 */
[[nodiscard]] const uint8_t *BuildingTileMap();
/**
 * @brief Per town tile (index y * MAXDUNX + x): the piece whose floor the tile shows instead of its own, or 0 for its
 * own (the open ground near buildings, props and uncovered house footprints). Null outside town.
 */
[[nodiscard]] const uint16_t *FloorSourceMap();
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

/** @brief Enables or disables the dungeon kit's sarcophagus model (on by default; off: a box of its sprite). */
void SetSarcophagusKitEnabled(bool enabled);

/**
 * @brief Whether a dungeon tile is drawn as part of a mesh structure (a pillar, a torch stand, the stairs) instead of a
 * wall cube or billboard.
 */
[[nodiscard]] bool IsStructureTile(Point tile);
/** @brief The per-tile structure kinds (MAXDUNX * MAXDUNY, index y * MAXDUNX + x; 0 none, 1 a mesh, 2 a mesh over a floor hole), null in town. For per-pixel loops. */
[[nodiscard]] const uint8_t *StructureTileMap();
[[nodiscard]] bool HasFloorHoles();
/** @brief Whether the floor is open at a point (the pit of the stairs down), so the floor is not drawn there. */
[[nodiscard]] bool IsFloorHole(float x, float y);
/** @brief Whether an object is drawn as a 3D box of its own sprite (a sarcophagus) rather than a billboard. */
[[nodiscard]] bool IsObjectDrawnAsMesh(int id);
/** @brief Draws the dungeon's mesh structures, depth tested against everything drawn so far. */
void DrawStructures(const Surface &out, const Projection &proj, std::vector<float> &depthBuffer);

/**
 * @brief Lights what has been drawn (the ground, walls, buildings) with the light of the lamps inside the tavern, the
 * smithy and the church, where it comes out of their windows and doorways, from the depth of each pixel. Town only.
 */
void ApplyWindowGlow(const Surface &out, const Projection &proj, const std::vector<float> &depthBuffer);
/** @brief The town's trees: each a billboard of its own art, facing the camera, on its trunk's foot. */
void CollectTreeSprites(Vec2 viewerPos, std::vector<Entity> &entities);

/** @brief Whether the lamps inside the tavern, the smithy and the church are lit (default) or out. */
void SetWindowLightEnabled(bool enabled);

/** @brief Debug: mesh rasterizer counters per frame since the last call, as text. */
std::string TakeRasterStats(int frames);

/**
 * @brief Opens one of the mod's asset files (assets/fp/<fileName>, or the path in the environment variable `envName`)
 * for reading; null if it is missing.
 */
FILE *OpenFpAsset(const char *envName, const char *fileName);

/** @brief Number of reconstructed buildings in the current level (debug). */
[[nodiscard]] int BuildingCount();

} // namespace devilution::fp
