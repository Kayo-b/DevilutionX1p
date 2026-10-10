/**
 * @file fp/fp_buildings.cpp
 *
 * Town buildings rebuilt as a box with a gable roof, textured by projecting the model into the original iso art.
 */
#include "fp/fp_buildings.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "engine/light_tables.hpp"
#include "engine/palette.h"
#include "fp/fp_textures.hpp"
#include "fp/fp_world.hpp"
#include "levels/gendung.h"
#include "levels/tile_properties.hpp"
#include "levels/trigs.h"
#include "interfac.h"
#include "utils/log.hpp"
#include "utils/paths.h"
#include "utils/str_cat.hpp"

namespace devilution::fp {

namespace {

/** Iso projection: screen x = (x - y) * 32, screen y = (x + y) * 16 - z * HeightPixels. */
constexpr float HeightPixels = 39.2F;
constexpr int MinBuildingTiles = 4;
/** Larger solid areas are cliffs or town walls, not houses. */
constexpr int MaxBuildingSpan = 9;
constexpr float MinFillShare = 0.45F;
/** Below this fill the flooded ground block is trimmed to its largest rectangle. */
constexpr float SpilledFillShare = 0.5F;
constexpr float MinFitScore = 0.5F;
constexpr int FitStep = 3;
/** Marks a colour as being in shade (underside of the roof overhang). */
constexpr int16_t UndersideFlag = 0x100;
constexpr uint8_t UndersideDarkness = 5;

struct V3 {
	float x;
	float y;
	float z;
};

/** @brief Half-space n . p <= d. */
struct Plane {
	float nx;
	float ny;
	float nz;
	float d;
};

struct Body {
	std::array<Plane, 6> planes;
	int count;
};

struct Params {
	bool ridgeAlongX;
	float eave;     // wall height
	float rise;     // height of the ridge above the eave
	float overhang; // how far the roof sticks out over the eaves
};

/** Side limits of the texture patches cut from a house's art, in art pixels. */
constexpr int MinPatchSize = 8;
constexpr int MaxPatchSize = 96;
/** Patches with a side shorter than this repeat too visibly; the house borrows another's instead. */
constexpr int BorrowPatchBelow = 16;
/** Art darker than this (outlines, window and door openings) is kept out of the patches. */
constexpr int MinPatchBrightness = 20;
/** Share of the fitted eave that the wall patch is taken from, counted from the ground. */
constexpr float WallPatchHeight = 0.75F;

/** @brief A picture of a house surface taken from the iso art; palette index 0 marks a missing texel. */
struct SurfaceTexture {
	int width = 0;
	int height = 0;
	std::vector<uint8_t> pixels;
	bool valid = false;
};

/** @brief A triangle of a house mesh in world space, with how its texture is laid on. */
struct WorldTriangle {
	std::array<V3, 3> p;
	uint8_t mapping;    // MeshWallAlongX / MeshWallAlongY / MeshFlat / MeshRoof / MeshRoofFlat
	uint8_t darkness;   // SurfaceShade step for surfaces turned away from the light
	bool roof;          // takes the thatch patch (else the wall patch)
	float slopeScale;   // roof: length along the slope per unit of horizontal distance
	float downX;        // roof: horizontal direction straight down the slope
	float downY;
	// Projectively textured structures (MeshProjective, MeshPit):
	float mirrorX;      // surfaces turned away from the iso camera show the art of the point mirrored through
	float mirrorY;      // this vertical plane (through the part's middle, across the surface's normal)
	float normalX;      // horizontal outward normal, unit length, or 0 for a horizontal surface
	float normalY;
	bool mirrored;
	uint8_t fallback;   // colour where the art has no pixel
};

enum : uint8_t {
	MeshWallAlongX, // a wall running along x: u follows world x
	MeshWallAlongY,
	MeshFlat, // horizontal: u, v follow world x, y
	MeshRoof, // a roof slope: u along it, v up it
	MeshProjective, // the structure's own iso art, projected (mirrored on the far side)
	MeshPit,        // below the floor, where the art shows nothing: the structure's wall art repeated, darkening with depth
};

struct Building {
	float x0;
	float y0;
	float x1; // exclusive end of the footprint, in tiles
	float y1;
	Params params;
	float score;
	int originX; // iso screen position of canvas pixel (0, 0)
	int originY;
	int canvasWidth;
	int canvasHeight;
	std::vector<int16_t> canvas;    // building art only; -1 / 0 transparent
	std::vector<int16_t> fitCanvas; // the same plus the ground under the building, for matching the silhouette
	Body walls;
	Body roof;
	float lowestZ; // for screen culling
	float highestZ;
	int model = -1;                // index into the house models, -1 to draw the fitted box
	bool longAlongX = true;        // the model's long axis runs along world x
	std::vector<uint8_t> wallRamp; // palette indices of the house's own wall art, sorted dark to light
	std::vector<uint8_t> roofRamp; // the same for its thatch
	SurfaceTexture wallPatch; // clean rectangle of the house's own wall art, tiled over the model's walls
	SurfaceTexture roofPatch; // the same for its thatch
	SurfaceTexture wallPatchVoxel; // the patches blurred to the size of one voxel, for the voxel model
	SurfaceTexture roofPatchVoxel;
	std::vector<WorldTriangle> mesh; // the model's surface in world space, when the model has a mesh
	float boundsOverhang = 0;        // how far the model reaches past the footprint, for screen culling
	int landmark = -1;               // index into Landmarks for the hand-placed buildings
};

struct LevelKey {
	const void *cels;
	int level;
	int type;
	bool isSetLevel;
	bool operator==(const LevelKey &) const = default;
};

bool Enabled = true;
std::vector<Building> Buildings;
std::vector<uint8_t> BuildingTiles;
LevelKey BuiltFor { nullptr, -1, -1, false };

Body MakeWalls(const Building &b, const Params &p)
{
	Body body;
	body.count = 6;
	body.planes = { { { 1, 0, 0, b.x1 }, { -1, 0, 0, -b.x0 }, { 0, 1, 0, b.y1 }, { 0, -1, 0, -b.y0 }, { 0, 0, 1, p.eave }, { 0, 0, -1, 0 } } };
	return body;
}

Body MakeRoof(const Building &b, const Params &p)
{
	Body body;
	body.count = 5;
	body.planes = {};
	body.planes[0] = { 0, 0, -1, -p.eave }; // underside
	if (p.ridgeAlongX) {
		const float ym = (b.y0 + b.y1) / 2.0F;
		const float s = p.rise / ((b.y1 - b.y0) / 2.0F + p.overhang);
		body.planes[1] = { 1, 0, 0, b.x1 + p.overhang };
		body.planes[2] = { -1, 0, 0, -(b.x0 - p.overhang) };
		body.planes[3] = { 0, s, 1, p.eave + p.rise + s * ym };  // slope facing +y (towards the iso camera)
		body.planes[4] = { 0, -s, 1, p.eave + p.rise - s * ym }; // slope facing -y
	} else {
		const float xm = (b.x0 + b.x1) / 2.0F;
		const float s = p.rise / ((b.x1 - b.x0) / 2.0F + p.overhang);
		body.planes[1] = { 0, 1, 0, b.y1 + p.overhang };
		body.planes[2] = { 0, -1, 0, -(b.y0 - p.overhang) };
		body.planes[3] = { s, 0, 1, p.eave + p.rise + s * xm };
		body.planes[4] = { -s, 0, 1, p.eave + p.rise - s * xm };
	}
	return body;
}

/** @brief Entry and exit of the line o + d * t through a convex body; face is the plane entered through. */
bool RayBody(const Body &body, V3 o, V3 d, float &tIn, float &tOut, int &face)
{
	tIn = -std::numeric_limits<float>::infinity();
	tOut = std::numeric_limits<float>::infinity();
	face = -1;
	for (int i = 0; i < body.count; i++) {
		const Plane &p = body.planes[i];
		const float denom = p.nx * d.x + p.ny * d.y + p.nz * d.z;
		const float num = p.d - (p.nx * o.x + p.ny * o.y + p.nz * o.z);
		if (std::abs(denom) < 1e-9F) {
			if (num < 0)
				return false;
			continue;
		}
		const float t = num / denom;
		if (denom > 0) {
			tOut = std::min(tOut, t);
		} else if (t > tIn) {
			tIn = t;
			face = i;
		}
	}
	return face >= 0 && tIn <= tOut;
}

/**
 * @brief The art colour to store in a texture patch. Palette entries 112 - 127 are a block of near-black shades
 * that the light tables do not darken like the ordinary ramps (they come out red), so they are swapped for the
 * nearest ordinary colour.
 */
uint8_t PatchColor(int16_t art)
{
	static std::array<uint8_t, 256> safe {};
	static bool ready = false;
	if (!ready) {
		for (int i = 0; i < 256; i++) {
			safe[i] = static_cast<uint8_t>(i);
			if (i < 112 || i > 127)
				continue;
			const SDL_Color c = logical_palette[i];
			int best = std::numeric_limits<int>::max();
			for (int j = 1; j < 256; j++) {
				if (j >= 112 && j <= 127)
					continue;
				const SDL_Color o = logical_palette[j];
				const int distance = (o.r - c.r) * (o.r - c.r) + (o.g - c.g) * (o.g - c.g) + (o.b - c.b) * (o.b - c.b);
				if (distance < best) {
					best = distance;
					safe[i] = static_cast<uint8_t>(j);
				}
			}
		}
		ready = true;
	}
	return safe[static_cast<uint8_t>(art)];
}

/** Brightness kept by each step of a mesh surface's darkness (see WorldTriangle::darkness). */
constexpr std::array<float, 6> SurfaceShade { 1.0F, 0.85F, 0.7F, 0.55F, 0.4F, 0.25F };

/**
 * @brief A colour dimmed by one of the SurfaceShade steps: the closest colour of its own 16-colour ramp (the
 * palette is laid out in ramps of one hue from light to dark), so the hue stays. The light tables are not used for
 * this: stepping some of the town's thatch colours along them ends in reds.
 */
uint8_t ShadeColor(uint8_t color, int darkness)
{
	static std::array<std::array<uint8_t, 256>, SurfaceShade.size()> table {};
	static bool ready = false;
	if (!ready) {
		for (size_t level = 0; level < SurfaceShade.size(); level++) {
			for (int i = 0; i < 256; i++) {
				const SDL_Color c = logical_palette[i];
				const float r = static_cast<float>(c.r) * SurfaceShade[level];
				const float g = static_cast<float>(c.g) * SurfaceShade[level];
				const float bl = static_cast<float>(c.b) * SurfaceShade[level];
				float best = std::numeric_limits<float>::max();
				table[level][i] = static_cast<uint8_t>(i);
				const int ramp = i & ~15;
				for (int j = ramp; j < ramp + 16; j++) {
					if (j == 0 || (j >= 112 && j <= 127 && (i < 112 || i > 127)))
						continue; // see PatchColor
					const SDL_Color o = logical_palette[j];
					const float distance = (o.r - r) * (o.r - r) * 3 + (o.g - g) * (o.g - g) * 6 + (o.b - bl) * (o.b - bl);
					if (distance < best) {
						best = distance;
						table[level][i] = static_cast<uint8_t>(j);
					}
				}
			}
		}
		ready = true;
	}
	return table[std::min<size_t>(darkness, SurfaceShade.size() - 1)][color];
}

/** @brief Iso art pixel at the screen position of a world point, searching a few pixels around for opaque art. */
int16_t SampleArt(const Building &b, float wx, float wy, float wz)
{
	const int px = static_cast<int>(std::floor((wx - wy) * 32.0F)) - b.originX;
	const int py = static_cast<int>(std::floor((wx + wy) * 16.0F - wz * HeightPixels)) - b.originY;
	for (int radius = 0; radius <= 2; radius++) {
		for (int dy = -radius; dy <= radius; dy++) {
			for (int dx = -radius; dx <= radius; dx++) {
				if (std::max(std::abs(dx), std::abs(dy)) != radius)
					continue;
				const int x = px + dx;
				const int y = py + dy;
				if (x < 0 || y < 0 || x >= b.canvasWidth || y >= b.canvasHeight)
					continue;
				const int16_t pixel = b.canvas[static_cast<size_t>(y) * b.canvasWidth + x];
				if (pixel > 0)
					return pixel;
			}
		}
	}
	return -1;
}

/** @brief Colour of a model surface point; surfaces facing away from the iso camera show their mirror image. */
int16_t SurfaceColor(const Building &b, const Plane &plane, V3 hit)
{
	const float facing = plane.nx + plane.ny;
	if (std::abs(facing) < 1e-4F) {
		// Underside of the roof: the roof's own colour in shade.
		const int16_t roofColor = SampleArt(b, hit.x, hit.y, b.params.eave + 0.3F);
		return roofColor < 0 ? int16_t { -1 } : static_cast<int16_t>(roofColor | UndersideFlag);
	}
	if (facing < 0) {
		if (std::abs(plane.nx) > std::abs(plane.ny))
			hit.x = b.x0 + b.x1 - hit.x;
		else
			hit.y = b.y0 + b.y1 - hit.y;
	}
	return SampleArt(b, hit.x, hit.y, hit.z);
}

/** @brief How well the model's iso silhouette matches the stitched art (intersection over union). */
float FitScore(const Building &b, const Params &p)
{
	const Body walls = MakeWalls(b, p);
	const Body roof = MakeRoof(b, p);
	constexpr V3 IsoRay { -1.0F, -1.0F, -32.0F / HeightPixels }; // away from the camera: same screen position
	int inside = 0;
	int outside = 0;
	for (int y = 0; y < b.canvasHeight; y += FitStep) {
		for (int x = 0; x < b.canvasWidth; x += FitStep) {
			const bool art = b.fitCanvas[static_cast<size_t>(y) * b.canvasWidth + x] > 0;
			const float sx = static_cast<float>(x + b.originX);
			const float sy = static_cast<float>(y + b.originY);
			const float sum = sy / 16.0F;  // x + y at z = 0
			const float diff = sx / 32.0F; // x - y
			const V3 origin { (sum + diff) / 2.0F, (sum - diff) / 2.0F, 0.0F };
			float tIn;
			float tOut;
			int face;
			const bool model = RayBody(walls, origin, IsoRay, tIn, tOut, face) || RayBody(roof, origin, IsoRay, tIn, tOut, face);
			if (art && model)
				inside++;
			else if (art || model)
				outside++;
		}
	}
	return inside + outside == 0 ? 0.0F : static_cast<float>(inside) / static_cast<float>(inside + outside);
}

/**
 * @brief Fits the roof to the art, searching around how Tristram cottages are really built.
 *
 * Measured on the Tristram plaza reference models (all relative to the short side W of the walls): the thatch
 * starts very low (eave 0.2 - 0.4 W above the ground) and climbs steeply to a ridge 1.0 - 1.25 W high (rise of
 * 1.0 - 1.6 per unit of half width, 45 - 58 degrees), overhanging about 0.09 W all round. The ridge runs along the
 * long side of an oblong house.
 */
void FitBuilding(Building &b)
{
	const float width = b.x1 - b.x0;
	const float depth = b.y1 - b.y0;
	const float shortSide = std::min(width, depth);
	const float longSide = std::max(width, depth);
	std::vector<bool> ridgeChoices;
	if (longSide >= 1.25F * shortSide)
		ridgeChoices.push_back(width >= depth); // along the long side
	else
		ridgeChoices = { true, false };

	Params best {};
	float bestScore = -1.0F;
	for (const bool ridgeAlongX : ridgeChoices) {
		// The roof spans the side across the ridge.
		const float span = ridgeAlongX ? depth : width;
		for (const float eaveRatio : { 0.15F, 0.22F, 0.30F, 0.38F }) {
			for (const float ridgeRatio : { 0.8F, 0.95F, 1.1F, 1.25F, 1.4F }) {
				for (const float overhangRatio : { 0.06F, 0.10F, 0.14F }) {
					const float eave = eaveRatio * span;
					const float ridge = ridgeRatio * span;
					const Params p { ridgeAlongX, eave, ridge - eave, overhangRatio * span };
					const float score = FitScore(b, p);
					if (score > bestScore) {
						bestScore = score;
						best = p;
					}
				}
			}
		}
	}
	b.params = best;
	b.score = bestScore;
	b.walls = MakeWalls(b, best);
	b.roof = MakeRoof(b, best);
	b.lowestZ = 0.0F;
	b.highestZ = best.eave + best.rise;
}

/**
 * @brief Stitches the iso columns of the tiles into pictures, nearest tiles last: `canvas` from the building's
 * art tiles, `fitCanvas` from those plus the ground tiles under it.
 */
void BuildCanvas(Building &b, const std::vector<Point> &artTiles, const std::vector<Point> &groundTiles)
{
	std::vector<std::pair<Point, bool>> tiles; // tile, isGround
	for (const Point t : artTiles)
		tiles.emplace_back(t, false);
	for (const Point t : groundTiles)
		tiles.emplace_back(t, true);
	std::sort(tiles.begin(), tiles.end(), [](const auto &a, const auto &c) {
		const Point p = a.first;
		const Point q = c.first;
		return p.x + p.y != q.x + q.y ? p.x + p.y < q.x + q.y : p.x < q.x;
	});
	if (tiles.empty())
		return;
	// Columns are 256 rows in town and fewer in the dungeons (160 in the cathedral).
	const int rows = static_cast<int>(GetPieceTextures(dPiece[tiles[0].first.x][tiles[0].first.y]).column.size() / ColumnWidth);
	int minX = std::numeric_limits<int>::max();
	int maxX = std::numeric_limits<int>::min();
	int minY = std::numeric_limits<int>::max();
	int maxY = std::numeric_limits<int>::min();
	for (const auto &[t, isGround] : tiles) {
		// Column pixel (cx, cy) sits at screen ((x - y) * 32 + cx - 32, (x + y + 2) * 16 + cy - (rows - 1)).
		minX = std::min(minX, (t.x - t.y) * 32 - 32);
		maxX = std::max(maxX, (t.x - t.y) * 32 + 31);
		minY = std::min(minY, (t.x + t.y + 2) * 16 - (rows - 1));
		maxY = std::max(maxY, (t.x + t.y + 2) * 16);
	}
	b.originX = minX;
	b.originY = minY;
	b.canvasWidth = maxX - minX + 1;
	b.canvasHeight = maxY - minY + 1;
	b.canvas.assign(static_cast<size_t>(b.canvasWidth) * b.canvasHeight, -1);
	b.fitCanvas.assign(b.canvas.size(), -1);
	for (const auto &[t, isGround] : tiles) {
		const PieceTextures &textures = GetPieceTextures(dPiece[t.x][t.y]);
		const int left = (t.x - t.y) * 32 - 32 - minX;
		const int top = (t.x + t.y + 2) * 16 - (rows - 1) - minY;
		for (int cy = 0; cy < rows && static_cast<size_t>(cy + 1) * ColumnWidth <= textures.column.size(); cy++) {
			for (int cx = 0; cx < ColumnWidth; cx++) {
				const int16_t pixel = textures.column[static_cast<size_t>(cy) * ColumnWidth + cx];
				if (pixel <= 0)
					continue;
				const size_t at = static_cast<size_t>(top + cy) * b.canvasWidth + left + cx;
				b.fitCanvas[at] = pixel;
				if (!isGround)
					b.canvas[at] = pixel;
			}
		}
	}
}

#ifdef _DEBUG
/** @brief Debug: the stitched art in colour, with the fitted silhouette outlined (white = model edge). */
void DumpBuilding(const Building &b, const std::string &path)
{
	constexpr V3 IsoRay { -1.0F, -1.0F, -32.0F / HeightPixels };
	std::vector<uint8_t> image(static_cast<size_t>(b.canvasWidth) * b.canvasHeight * 3, 0);
	for (int y = 0; y < b.canvasHeight; y++) {
		for (int x = 0; x < b.canvasWidth; x++) {
			const int16_t art = b.fitCanvas[static_cast<size_t>(y) * b.canvasWidth + x];
			const float sum = static_cast<float>(y + b.originY) / 16.0F;
			const float diff = static_cast<float>(x + b.originX) / 32.0F;
			const V3 origin { (sum + diff) / 2.0F, (sum - diff) / 2.0F, 0.0F };
			float tIn;
			float tOut;
			int face;
			const bool model = RayBody(b.walls, origin, IsoRay, tIn, tOut, face) || RayBody(b.roof, origin, IsoRay, tIn, tOut, face);
			uint8_t *px = &image[(static_cast<size_t>(y) * b.canvasWidth + x) * 3];
			if (art > 0) {
				const SDL_Color c = logical_palette[art];
				px[0] = c.r;
				px[1] = c.g;
				px[2] = c.b;
			}
			if (model && art <= 0) {
				px[0] = 255; // model without art
			} else if (!model && art > 0) {
				px[2] = std::min(255, px[2] + 90); // art outside the model
			}
		}
	}
	if (FILE *f = std::fopen(path.c_str(), "wb")) {
		std::fprintf(f, "P6\n%d %d\n255\n", b.canvasWidth, b.canvasHeight);
		std::fwrite(image.data(), 1, image.size(), f);
		std::fclose(f);
	}
}
#endif


/** @brief A house voxel model exported from the Tristram plaza reference (see research/tristram_plaza/export_houses.py). */
struct HouseModel {
	std::string name;
	float shortUnits = 0;
	float longUnits = 0;
	float voxelSize = 0;
	int nx = 0;
	int ny = 0;
	int nz = 0;
	std::vector<uint8_t> voxels; // 5 bytes each: i, j, k, class (0 wall, 1 roof), shade
	std::vector<std::array<float, 9>> triangles; // the reference mesh in the same voxel frame (see export_meshes.py)
	std::vector<uint8_t> triangleClasses;        // 0 wall, 1 roof, 2 door
};

std::vector<HouseModel> HouseModels;
bool HouseModelsLoaded = false;
bool HouseModelsOn = true;
bool TexturedHouses = true;
bool HouseMeshesOn = true;

FILE *OpenHouseAsset(const char *envName, const char *fileName)
{
	std::vector<std::string> candidates;
	if (const char *env = std::getenv(envName))
		candidates.emplace_back(env);
	candidates.push_back(StrCat(paths::AssetsPath(), "fp/", fileName));
	candidates.push_back(StrCat(paths::BasePath(), "assets/fp/", fileName));
	for (const std::string &path : candidates)
		if (FILE *f = std::fopen(path.c_str(), "rb"))
			return f;
	return nullptr;
}

/** @brief Adds the triangle meshes of housemesh.bin to the loaded voxel models, matched by name. */
void LoadHouseMeshes()
{
	FILE *f = OpenHouseAsset("FP_HOUSEMESH", "housemesh.bin");
	if (f == nullptr) {
		LogInfo("fp: house meshes not found (housemesh.bin); drawing voxels");
		return;
	}
	auto readAll = [&](void *dst, size_t size) { return std::fread(dst, 1, size, f) == size; };
	char magic[4];
	uint32_t count = 0;
	if (!readAll(magic, 4) || std::memcmp(magic, "FPM1", 4) != 0 || !readAll(&count, 4)) {
		std::fclose(f);
		return;
	}
	int loaded = 0;
	for (uint32_t m = 0; m < count; m++) {
		uint8_t nameLength = 0;
		std::string name;
		uint32_t triangleCount = 0;
		if (!readAll(&nameLength, 1))
			break;
		name.resize(nameLength);
		if (!readAll(name.data(), nameLength) || !readAll(&triangleCount, 4))
			break;
		std::vector<std::array<float, 9>> triangles(triangleCount);
		std::vector<uint8_t> classes(triangleCount);
		bool ok = true;
		for (uint32_t t = 0; t < triangleCount && ok; t++)
			ok = readAll(triangles[t].data(), sizeof(float) * 9) && readAll(&classes[t], 1);
		if (!ok)
			break;
		for (HouseModel &model : HouseModels) {
			if (model.name == name) {
				model.triangles = std::move(triangles);
				model.triangleClasses = std::move(classes);
				loaded++;
				break;
			}
		}
	}
	std::fclose(f);
	LogInfo("fp: loaded {} house meshes", loaded);
}

void LoadHouseModels()
{
	if (HouseModelsLoaded)
		return;
	HouseModelsLoaded = true;
	FILE *f = OpenHouseAsset("FP_HOUSES", "houses.bin");
	if (f == nullptr) {
		LogInfo("fp: house models not found (houses.bin); using fitted boxes");
		return;
	}
	auto readAll = [&](void *dst, size_t size) { return std::fread(dst, 1, size, f) == size; };
	char magic[4];
	uint32_t count = 0;
	if (!readAll(magic, 4) || std::memcmp(magic, "FPH1", 4) != 0 || !readAll(&count, 4)) {
		std::fclose(f);
		return;
	}
	for (uint32_t m = 0; m < count; m++) {
		HouseModel model;
		uint8_t nameLength = 0;
		if (!readAll(&nameLength, 1))
			break;
		model.name.resize(nameLength);
		float sizes[3];
		uint16_t dims[3];
		uint32_t voxelCount = 0;
		if (!readAll(model.name.data(), nameLength) || !readAll(sizes, sizeof(sizes)) || !readAll(dims, sizeof(dims)) || !readAll(&voxelCount, 4))
			break;
		model.shortUnits = sizes[0];
		model.longUnits = sizes[1];
		model.voxelSize = sizes[2];
		model.nx = dims[0];
		model.ny = dims[1];
		model.nz = dims[2];
		model.voxels.resize(static_cast<size_t>(voxelCount) * 5);
		if (!readAll(model.voxels.data(), model.voxels.size()))
			break;
		HouseModels.push_back(std::move(model));
	}
	std::fclose(f);
	LogInfo("fp: loaded {} house models", HouseModels.size());
	LoadHouseMeshes();
}

/** @brief A landmark mesh from landmarks.bin (see research/tristram_plaza/export_landmarks.py). */
struct LandmarkMesh {
	std::string name;
	std::vector<std::array<float, 9>> triangles; // u, w (0..1 over the plaza extent), z (plaza units) per vertex
	std::vector<uint8_t> classes;                // 0 wall, 1 roof, 2 door
};

/**
 * @brief Where a landmark's plaza mesh goes in town. These buildings are too complex for the footprint detection
 * (an L-shaped tavern, a smithy hall with an open porch), so they are placed by hand over the town map, measured
 * against the iso art (research/tristram_plaza: fitted by silhouette overlap, then checked by eye).
 */
struct LandmarkPlacement {
	const char *name;
	Point claimMin; // tiles that belong to the landmark: detected houses overlapping them are dropped
	Point claimMax;
	float x0;       // world position of the plaza extent: u = 0 .. 1 maps to x0 .. x1
	float x1;
	float y0;       // w = 0 .. 1 maps to y0 .. y1 (after flipW)
	float y1;
	float zScale;   // world height per plaza unit, up to eaveIn
	float eaveIn;   // plaza height where the scale changes (the eaves)
	float zScaleAbove; // world height per plaza unit above it, so a low eave can be lifted without a towering ridge
	bool flipW;     // the plaza's y axis runs the other way round to the town's
	std::array<Vec2, 2> posts; // thin wooden posts holding up an open roof; { 0, 0 } for none
};

const std::array<LandmarkPlacement, 2> Landmarks { {
	// Griswold's smithy: the long hall along x, and over the forge where he stands an open porch roof on two posts.
	// The plaza porch eaves are low; the art has them at the hall's wall height, high enough to stand under.
	{ "smithy", { 60, 56 }, { 71, 65 }, 60.6F, 72.25F, 55.75F, 65.5F, 1.45F, 1.0F, 0.55F, true, { Vec2 { 62.25F, 65.2F }, Vec2 { 65.25F, 65.2F } } },
	// Ogden's tavern: two front gables with the door in the corner between them, where Ogden stands.
	{ "ogden", { 46, 53 }, { 57, 63 }, 46.75F, 57.0F, 55.0F, 64.0F, 0.76F, 100.0F, 0.76F, true, { Vec2 { 0, 0 }, Vec2 { 0, 0 } } },
} };

std::vector<LandmarkMesh> LandmarkMeshes;
bool LandmarksLoaded = false;

void LoadLandmarks()
{
	if (LandmarksLoaded)
		return;
	LandmarksLoaded = true;
	FILE *f = OpenHouseAsset("FP_LANDMARKS", "landmarks.bin");
	if (f == nullptr) {
		LogInfo("fp: landmarks not found (landmarks.bin)");
		return;
	}
	auto readAll = [&](void *dst, size_t size) { return std::fread(dst, 1, size, f) == size; };
	char magic[4];
	uint32_t count = 0;
	if (!readAll(magic, 4) || std::memcmp(magic, "FPL1", 4) != 0 || !readAll(&count, 4)) {
		std::fclose(f);
		return;
	}
	for (uint32_t m = 0; m < count; m++) {
		LandmarkMesh mesh;
		uint8_t nameLength = 0;
		float height = 0;
		uint32_t triangleCount = 0;
		if (!readAll(&nameLength, 1))
			break;
		mesh.name.resize(nameLength);
		if (!readAll(mesh.name.data(), nameLength) || !readAll(&height, 4) || !readAll(&triangleCount, 4))
			break;
		mesh.triangles.resize(triangleCount);
		mesh.classes.resize(triangleCount);
		bool ok = true;
		for (uint32_t t = 0; t < triangleCount && ok; t++)
			ok = readAll(mesh.triangles[t].data(), sizeof(float) * 9) && readAll(&mesh.classes[t], 1);
		if (!ok)
			break;
		LandmarkMeshes.push_back(std::move(mesh));
	}
	std::fclose(f);
	LogInfo("fp: loaded {} landmark meshes", LandmarkMeshes.size());
}

int PaletteBrightness(int index)
{
	const SDL_Color c = logical_palette[index];
	return c.r * 3 + c.g * 6 + c.b;
}

/** @brief Palette indices of the house's wall and thatch art, one entry per art pixel, sorted by brightness. */
void BuildRamps(Building &b)
{
	std::vector<std::pair<int, uint8_t>> wall;
	std::vector<std::pair<int, uint8_t>> roof;
	for (const int16_t pixel : b.canvas) {
		if (pixel <= 0)
			continue;
		const SDL_Color c = logical_palette[pixel];
		const int brightness = PaletteBrightness(pixel);
		if (brightness < 60)
			continue; // black outlines and windows
		(c.r > c.b + 10 ? roof : wall).emplace_back(brightness, static_cast<uint8_t>(pixel));
	}
	auto toRamp = [](std::vector<std::pair<int, uint8_t>> &pixels, std::vector<uint8_t> &ramp) {
		std::sort(pixels.begin(), pixels.end());
		ramp.clear();
		for (const auto &p : pixels)
			ramp.push_back(p.second);
	};
	toRamp(wall, b.wallRamp);
	toRamp(roof, b.roofRamp);
}

/** @brief Fills texels missed by rounding: unknown ones between two known neighbours in a row or column. */
void CloseGaps(SurfaceTexture &tex)
{
	std::vector<uint8_t> closed = tex.pixels;
	for (int y = 0; y < tex.height; y++) {
		for (int x = 0; x < tex.width; x++) {
			const size_t at = static_cast<size_t>(y) * tex.width + x;
			if (tex.pixels[at] != 0)
				continue;
			if (x > 0 && x + 1 < tex.width && tex.pixels[at - 1] != 0 && tex.pixels[at + 1] != 0)
				closed[at] = tex.pixels[at - 1];
			else if (y > 0 && y + 1 < tex.height && tex.pixels[at - tex.width] != 0 && tex.pixels[at + tex.width] != 0)
				closed[at] = tex.pixels[at - tex.width];
		}
	}
	tex.pixels.swap(closed);
}

/** @brief The largest rectangle of the picture that is all known texels (palette index != 0), sides capped. */
SurfaceTexture LargestCleanRectangle(SurfaceTexture tex)
{
	SurfaceTexture patch;
	if (tex.width == 0)
		return patch;
	CloseGaps(tex);
	std::vector<int> run(tex.width, 0); // known texels in each column ending at the current row
	int bestArea = 0;
	int bestX = 0;
	int bestY = 0;
	int bestWidth = 0;
	int bestHeight = 0;
	for (int y = 0; y < tex.height; y++) {
		for (int x = 0; x < tex.width; x++)
			run[x] = tex.pixels[static_cast<size_t>(y) * tex.width + x] != 0 ? std::min(run[x] + 1, MaxPatchSize) : 0;
		for (int x = 0; x < tex.width; x++) {
			int height = MaxPatchSize;
			for (int right = x; right < tex.width && right - x < MaxPatchSize; right++) {
				height = std::min(height, run[right]);
				if (height < MinPatchSize)
					break;
				const int width = right - x + 1;
				if (width >= MinPatchSize && width * height > bestArea) {
					bestArea = width * height;
					bestX = x;
					bestY = y - height + 1;
					bestWidth = width;
					bestHeight = height;
				}
			}
		}
	}
	if (bestArea == 0)
		return patch;
	patch.width = bestWidth;
	patch.height = bestHeight;
	patch.pixels.resize(static_cast<size_t>(bestWidth) * bestHeight);
	for (int y = 0; y < bestHeight; y++)
		for (int x = 0; x < bestWidth; x++)
			patch.pixels[static_cast<size_t>(y) * bestWidth + x] = tex.pixels[static_cast<size_t>(bestY + y) * tex.width + bestX + x];
	patch.valid = true;
	return patch;
}

/** @brief Texel of a patch tiled over a surface, optionally mirrored every other repeat so no seams show. Coordinates in art pixels. */
uint8_t TiledTexel(const SurfaceTexture &patch, float u, float v, bool mirror = true)
{
	auto wrap = [mirror](float t, int size) {
		if (!mirror) {
			const int i = static_cast<int>(std::floor(t)) % size;
			return i < 0 ? i + size : i;
		}
		const int period = 2 * size;
		int i = static_cast<int>(std::floor(t)) % period;
		if (i < 0)
			i += period;
		return i < size ? i : period - 1 - i;
	};
	return patch.pixels[static_cast<size_t>(wrap(-v, patch.height)) * patch.width + wrap(u, patch.width)];
}

/**
 * @brief Cuts a clean patch of wall and of thatch out of the house's own iso art. Which surface a pixel shows is
 * told by casting it back onto the fitted model. Wall art is unskewed into surface space that way; the thatch faces
 * the iso camera nearly head on, so its patch is cut straight from the picture. Near-black outlines, windows,
 * doors and (for the walls) warm thatch colours are left out; the voxel model has its own openings.
 */
void BuildTextures(Building &b)
{
	const float eave = b.params.eave;
	const int wallHeight = std::max(4, static_cast<int>(std::ceil(eave * HeightPixels)));
	std::array<SurfaceTexture, 2> walls; // the +x and +y faces, the two the iso camera sees
	for (int face = 0; face < 2; face++) {
		walls[face].width = std::max(8, static_cast<int>(std::ceil((face == 0 ? b.y1 - b.y0 : b.x1 - b.x0) * 32.0F)));
		walls[face].height = wallHeight;
		walls[face].pixels.assign(static_cast<size_t>(walls[face].width) * wallHeight, 0);
	}
	SurfaceTexture roof;
	roof.width = b.canvasWidth;
	roof.height = b.canvasHeight;
	roof.pixels.assign(b.canvas.size(), 0);

	constexpr V3 IsoRay { -1.0F, -1.0F, -32.0F / HeightPixels };
	for (int y = 0; y < b.canvasHeight; y++) {
		for (int x = 0; x < b.canvasWidth; x++) {
			const size_t at = static_cast<size_t>(y) * b.canvasWidth + x;
			const int16_t art = b.canvas[at];
			if (art <= 0 || PaletteBrightness(art) < MinPatchBrightness)
				continue;
			const SDL_Color c = logical_palette[art];
			if (c.r > 2 * c.g + 20)
				continue; // red doors, signs and lamps
			const float sum = static_cast<float>(y + b.originY) / 16.0F;
			const float diff = static_cast<float>(x + b.originX) / 32.0F;
			const V3 origin { (sum + diff) / 2.0F, (sum - diff) / 2.0F, 0.0F };
			float wallIn;
			float roofIn;
			float tOut;
			int wallFace;
			int roofFace;
			const bool onWall = RayBody(b.walls, origin, IsoRay, wallIn, tOut, wallFace);
			const bool onRoof = RayBody(b.roof, origin, IsoRay, roofIn, tOut, roofFace);
			// The iso ray runs away from the camera, so the smaller entry is the surface the camera sees.
			if (onRoof && (!onWall || roofIn < wallIn)) {
				if (roofFace == 3 || roofFace == 4)
					roof.pixels[at] = PatchColor(art);
				continue;
			}
			if (!onWall || (wallFace != 0 && wallFace != 2))
				continue;
			const V3 hit { origin.x + IsoRay.x * wallIn, origin.y + IsoRay.y * wallIn, origin.z + IsoRay.z * wallIn };
			// Only the lower part of the wall: where the fitted eave is too high, thatch shows above it.
			if (hit.z > WallPatchHeight * eave)
				continue;
			// Wall stone and timber are grey to blue; warm pixels on a wall are thatch hanging low, or doors.
			if (c.r > c.b + 10)
				continue;
			SurfaceTexture &tex = walls[wallFace == 0 ? 0 : 1];
			const float u = wallFace == 0 ? (hit.y - b.y0) / (b.y1 - b.y0) : (hit.x - b.x0) / (b.x1 - b.x0);
			const int tx = std::clamp(static_cast<int>(u * static_cast<float>(tex.width)), 0, tex.width - 1);
			const int ty = std::clamp(tex.height - 1 - static_cast<int>(hit.z * HeightPixels), 0, tex.height - 1);
			tex.pixels[static_cast<size_t>(ty) * tex.width + tx] = PatchColor(art);
		}
	}
	b.wallPatch = LargestCleanRectangle(walls[0]);
	const SurfaceTexture other = LargestCleanRectangle(walls[1]);
	if (other.width * other.height > b.wallPatch.width * b.wallPatch.height)
		b.wallPatch = other;
	b.roofPatch = LargestCleanRectangle(roof);
#ifdef _DEBUG
	if (const char *dumpDir = std::getenv("FP_DUMP_BUILDINGS")) {
		for (const auto &[patch, kind] : { std::pair { &b.wallPatch, "wall" }, std::pair { &b.roofPatch, "roof" } }) {
			if (!patch->valid)
				continue;
			if (FILE *f = std::fopen(StrCat(dumpDir, "/p_", kind, "_", static_cast<int>(b.x0), "_", static_cast<int>(b.y0), ".ppm").c_str(), "wb")) {
				std::fprintf(f, "P6\n%d %d\n255\n", patch->width, patch->height);
				for (const uint8_t pixel : patch->pixels) {
					const SDL_Color c = logical_palette[pixel];
					std::fputc(c.r, f);
					std::fputc(c.g, f);
					std::fputc(c.b, f);
				}
				std::fclose(f);
			}
		}
	}
#endif
	LogInfo("fp: house {}..{} x {}..{} art patches: wall {}x{}, thatch {}x{}", static_cast<int>(b.x0), static_cast<int>(b.x1) - 1, static_cast<int>(b.y0), static_cast<int>(b.y1) - 1,
	    b.wallPatch.width, b.wallPatch.height, b.roofPatch.width, b.roofPatch.height);
}

/** @brief Palette colour of the house's own art at a model point; 0 if the art gives no patch for that surface. */
uint8_t ArtColor(const Building &b, bool roof, float wx, float wy, float wz)
{
	if (roof) {
		if (!b.roofPatchVoxel.valid)
			return 0;
		// Thatch: along the model's ridge and down the slope, at the art's 32 pixels per tile.
		const bool ridgeAlongX = b.longAlongX;
		const float halfSpan = (ridgeAlongX ? b.y1 - b.y0 : b.x1 - b.x0) / 2.0F + b.params.overhang;
		const float slopeScale = std::sqrt(halfSpan * halfSpan + b.params.rise * b.params.rise) / halfSpan;
		const float along = ridgeAlongX ? wx : wy;
		const float across = ridgeAlongX ? wy - (b.y0 + b.y1) / 2.0F : wx - (b.x0 + b.x1) / 2.0F;
		return TiledTexel(b.roofPatchVoxel, along * 32.0F, (halfSpan - std::abs(across)) * slopeScale * 32.0F);
	}
	if (!b.wallPatchVoxel.valid)
		return 0;
	// Stone: along the nearest face and up, at the art's density (32 pixels per tile across, HeightPixels up).
	const float fx = std::clamp((wx - b.x0) / (b.x1 - b.x0), 0.0F, 1.0F);
	const float fy = std::clamp((wy - b.y0) / (b.y1 - b.y0), 0.0F, 1.0F);
	const bool onXFace = std::min(fx, 1.0F - fx) * (b.x1 - b.x0) < std::min(fy, 1.0F - fy) * (b.y1 - b.y0);
	return TiledTexel(b.wallPatchVoxel, (onXFace ? wy : wx) * 32.0F, wz * HeightPixels);
}

/** @brief World size of one voxel of the house's model once stretched over its footprint. */
float VoxelSize(const Building &b)
{
	const HouseModel &model = HouseModels[b.model];
	const float shortSide = std::min(b.x1 - b.x0, b.y1 - b.y0);
	const float longSide = std::max(b.x1 - b.x0, b.y1 - b.y0);
	const float overhang = 0.12F * shortSide;
	return std::max((longSide + 2.0F * overhang) / static_cast<float>(model.nx), (shortSide + 2.0F * overhang) / static_cast<float>(model.ny));
}

/**
 * @brief Texel of a patch laid over a surface in patch-sized cells, each cell taking the patch from its own hashed
 * offset and mirroring, so the repeats do not line up into a visible grid. Coordinates in art pixels.
 */
uint8_t ScatteredTexel(const SurfaceTexture &patch, float u, float v)
{
	const int cellU = static_cast<int>(std::floor(u / static_cast<float>(patch.width)));
	const int cellV = static_cast<int>(std::floor(v / static_cast<float>(patch.height)));
	uint32_t hash = static_cast<uint32_t>(cellU) * 73856093U ^ static_cast<uint32_t>(cellV) * 19349663U;
	hash ^= hash >> 13;
	hash *= 0x5bd1e995U;
	hash ^= hash >> 15;
	int x = static_cast<int>(std::floor(u)) - cellU * patch.width;
	int y = static_cast<int>(std::floor(-v)) + (cellV + 1) * patch.height;
	x = std::clamp(x, 0, patch.width - 1);
	y = std::clamp(y, 0, patch.height - 1);
	if ((hash & 1) != 0)
		x = patch.width - 1 - x;
	x = (x + static_cast<int>((hash >> 8) % static_cast<uint32_t>(patch.width))) % patch.width;
	y = (y + static_cast<int>((hash >> 20) % static_cast<uint32_t>(patch.height))) % patch.height;
	return patch.pixels[static_cast<size_t>(y) * patch.width + x];
}

/**
 * @brief Blurs a patch over the size one voxel covers, so a voxel shows the average of the art under it instead of
 * one point of it (which beats into stripes against fine thatch strands). Results stay in the patch's own colours.
 */
void FilterPatch(SurfaceTexture &patch, int sizeX, int sizeY)
{
	if (!patch.valid || (sizeX <= 1 && sizeY <= 1))
		return;
	std::vector<uint8_t> colors;
	std::array<bool, 256> used {};
	for (const uint8_t p : patch.pixels) {
		if (!used[p])
			colors.push_back(p);
		used[p] = true;
	}
	std::vector<uint8_t> filtered(patch.pixels.size());
	for (int y = 0; y < patch.height; y++) {
		for (int x = 0; x < patch.width; x++) {
			int r = 0;
			int g = 0;
			int bl = 0;
			int n = 0;
			for (int dy = -sizeY / 2; dy < sizeY - sizeY / 2; dy++) {
				for (int dx = -sizeX / 2; dx < sizeX - sizeX / 2; dx++) {
					const SDL_Color c = logical_palette[TiledTexel(patch, static_cast<float>(x + dx), -static_cast<float>(y + dy))];
					r += c.r;
					g += c.g;
					bl += c.b;
					n++;
				}
			}
			r /= n;
			g /= n;
			bl /= n;
			int best = std::numeric_limits<int>::max();
			for (const uint8_t index : colors) {
				const SDL_Color c = logical_palette[index];
				const int distance = (c.r - r) * (c.r - r) * 3 + (c.g - g) * (c.g - g) * 6 + (c.b - bl) * (c.b - bl);
				if (distance < best) {
					best = distance;
					filtered[static_cast<size_t>(y) * patch.width + x] = index;
				}
			}
		}
	}
	patch.pixels.swap(filtered);
}

void AddMeshTriangle(Building &b, const std::array<V3, 3> &p, bool roof, V3 middle);

/** @brief Places the model's mesh over the footprint, the same way DrawHouseModel places its voxels. */
void BuildMesh(Building &b)
{
	b.mesh.clear();
	const HouseModel &model = HouseModels[b.model];
	if (model.triangles.empty())
		return;
	const float width = b.x1 - b.x0;
	const float depth = b.y1 - b.y0;
	const float cx = (b.x0 + b.x1) / 2.0F;
	const float cy = (b.y0 + b.y1) / 2.0F;
	const float shortSide = std::min(width, depth);
	const float longSide = std::max(width, depth);
	const float overhang = 0.12F * shortSide;
	const float spanLong = longSide + 2.0F * overhang;
	const float spanShort = shortSide + 2.0F * overhang;
	const float stepLong = spanLong / static_cast<float>(model.nx);
	const float stepShort = spanShort / static_cast<float>(model.ny);
	const float stepUp = stepShort;
	b.boundsOverhang = std::max(b.params.overhang, overhang);
	b.highestZ = std::max(b.highestZ, static_cast<float>(model.nz) * stepUp);
	auto toWorld = [&](const float *q) {
		const float along = q[0] * stepLong - spanLong / 2.0F;
		const float across = q[1] * stepShort - spanShort / 2.0F;
		return V3 { cx + (b.longAlongX ? along : across), cy + (b.longAlongX ? across : along), q[2] * stepUp };
	};
	for (size_t t = 0; t < model.triangles.size(); t++) {
		std::array<V3, 3> p;
		for (int k = 0; k < 3; k++)
			p[k] = toWorld(&model.triangles[t][static_cast<size_t>(k) * 3]);
		AddMeshTriangle(b, p, model.triangleClasses[t] == 1, { cx, cy, b.highestZ * 0.3F });
	}
}

/** @brief Adds a world triangle to the building's mesh, working out how it is textured and shaded. */
void AddMeshTriangle(Building &b, const std::array<V3, 3> &p, bool roof, V3 middle)
{
	WorldTriangle tri;
	tri.p = p;
	{
		const float cx = middle.x;
		const float cy = middle.y;
		const V3 e1 { tri.p[1].x - tri.p[0].x, tri.p[1].y - tri.p[0].y, tri.p[1].z - tri.p[0].z };
		const V3 e2 { tri.p[2].x - tri.p[0].x, tri.p[2].y - tri.p[0].y, tri.p[2].z - tri.p[0].z };
		V3 n { e1.y * e2.z - e1.z * e2.y, e1.z * e2.x - e1.x * e2.z, e1.x * e2.y - e1.y * e2.x };
		const float length = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
		if (length < 1e-9F)
			return;
		n = { n.x / length, n.y / length, n.z / length };
		// The mesh's winding is not reliable: orient the normal away from the middle of the house.
		const V3 centroid { (tri.p[0].x + tri.p[1].x + tri.p[2].x) / 3.0F, (tri.p[0].y + tri.p[1].y + tri.p[2].y) / 3.0F, (tri.p[0].z + tri.p[1].z + tri.p[2].z) / 3.0F };
		if (n.x * (centroid.x - cx) + n.y * (centroid.y - cy) + n.z * (centroid.z - middle.z) < 0)
			n = { -n.x, -n.y, -n.z };
		tri.roof = roof;
		tri.slopeScale = 1.0F;
		tri.downX = 0;
		tri.downY = 0;
		const float horizontal = std::sqrt(n.x * n.x + n.y * n.y);
		if (roof && horizontal > 0.15F && std::abs(n.z) > 0.25F) {
			// A slope: the strands of the thatch run straight down it, as they do in the art.
			tri.mapping = MeshRoof;
			tri.downX = n.x / horizontal;
			tri.downY = n.y / horizontal;
			tri.slopeScale = 1.0F / std::abs(n.z);
		} else if (std::abs(n.z) > 0.7F) {
			tri.mapping = MeshFlat;
		} else {
			tri.mapping = std::abs(n.x) > std::abs(n.y) ? MeshWallAlongY : MeshWallAlongX;
		}
		// The iso art is lit from the camera side (+x, +y); surfaces facing away and undersides are darker.
		tri.darkness = 0;
		if (n.z < -0.3F)
			tri.darkness = 3;
		else if (n.x + n.y < -0.5F)
			tri.darkness = 2;
		else if (n.x + n.y < 0.0F)
			tri.darkness = 1;
		b.mesh.push_back(tri);
	}
}

/** @brief Lowest roof surface above a point (the underside a post holds up); -1 if no roof covers it. */
float RoofHeightAt(const Building &b, float x, float y)
{
	float lowest = -1.0F;
	for (const WorldTriangle &tri : b.mesh) {
		if (!tri.roof)
			continue;
		const V3 &a = tri.p[0];
		const V3 &c = tri.p[1];
		const V3 &d = tri.p[2];
		const float area = (c.x - a.x) * (d.y - a.y) - (c.y - a.y) * (d.x - a.x);
		if (std::abs(area) < 1e-6F)
			continue;
		const float w1 = ((x - a.x) * (d.y - a.y) - (y - a.y) * (d.x - a.x)) / area;
		const float w2 = ((c.x - a.x) * (y - a.y) - (c.y - a.y) * (x - a.x)) / area;
		if (w1 < 0 || w2 < 0 || w1 + w2 > 1)
			continue;
		const float z = a.z + w1 * (c.z - a.z) + w2 * (d.z - a.z);
		if (lowest < 0 || z < lowest)
			lowest = z;
	}
	return lowest;
}

/** @brief Builds a landmark's mesh from its plaza model and placement, adding the posts of open roofs. */
void BuildLandmarkMesh(Building &b, const LandmarkPlacement &place, const LandmarkMesh &mesh)
{
	b.mesh.clear();
	float top = 0;
	auto height = [&](float z) {
		return z <= place.eaveIn ? z * place.zScale : place.eaveIn * place.zScale + (z - place.eaveIn) * place.zScaleAbove;
	};
	auto toWorld = [&](const float *q) {
		const float w = place.flipW ? 1.0F - q[1] : q[1];
		return V3 { place.x0 + q[0] * (place.x1 - place.x0), place.y0 + w * (place.y1 - place.y0), height(q[2]) };
	};
	for (const auto &t : mesh.triangles)
		for (int k = 0; k < 3; k++)
			top = std::max(top, height(t[static_cast<size_t>(k) * 3 + 2]));
	const V3 middle { (place.x0 + place.x1) / 2.0F, (place.y0 + place.y1) / 2.0F, top * 0.3F };
	for (size_t t = 0; t < mesh.triangles.size(); t++) {
		std::array<V3, 3> p;
		for (int k = 0; k < 3; k++)
			p[k] = toWorld(&mesh.triangles[t][static_cast<size_t>(k) * 3]);
		AddMeshTriangle(b, p, mesh.classes[t] == 1, middle);
	}
	// Posts: square timbers from the ground to the roof above them, shaded a step darker than the thatch.
	constexpr float PostHalfWidth = 0.07F;
	for (const Vec2 post : place.posts) {
		if (post.x == 0 && post.y == 0)
			continue;
		float postHeight = RoofHeightAt(b, post.x, post.y);
		if (postHeight <= 0)
			postHeight = 1.5F;
		const std::array<Vec2, 4> corners { { { post.x - PostHalfWidth, post.y - PostHalfWidth }, { post.x + PostHalfWidth, post.y - PostHalfWidth },
			{ post.x + PostHalfWidth, post.y + PostHalfWidth }, { post.x - PostHalfWidth, post.y + PostHalfWidth } } };
		for (int side = 0; side < 4; side++) {
			const Vec2 a = corners[side];
			const Vec2 c = corners[(side + 1) % 4];
			const V3 low0 { a.x, a.y, 0 };
			const V3 low1 { c.x, c.y, 0 };
			const V3 high0 { a.x, a.y, postHeight };
			const V3 high1 { c.x, c.y, postHeight };
			const V3 inside { post.x, post.y, postHeight / 2.0F };
			// Timber: the brown of the thatch suits it better than the wall stone.
			const size_t first = b.mesh.size();
			AddMeshTriangle(b, { low0, low1, high1 }, true, inside);
			AddMeshTriangle(b, { low0, high1, high0 }, true, inside);
			for (size_t i = first; i < b.mesh.size(); i++)
				b.mesh[i].darkness = std::min<uint8_t>(b.mesh[i].darkness + 1, SurfaceShade.size() - 1);
		}
	}
	b.highestZ = std::max(b.highestZ, top);
	b.boundsOverhang = std::max({ b.params.overhang, b.x0 - place.x0, place.x1 - b.x1, b.y0 - place.y0, place.y1 - b.y1, 0.0F }) + 0.1F;
}

/**
 * @brief Adds the hand-placed landmark buildings, replacing whatever the footprint detection made of their tiles.
 * Their art is stitched and fitted like any house, for cutting the wall and thatch patches.
 */
void AddLandmarks()
{
	LoadLandmarks();
	for (size_t l = 0; l < Landmarks.size(); l++) {
		const LandmarkPlacement &place = Landmarks[l];
		const auto mesh = std::find_if(LandmarkMeshes.begin(), LandmarkMeshes.end(), [&](const LandmarkMesh &m) { return m.name == place.name; });
		if (mesh == LandmarkMeshes.end())
			continue;
		// Only where the town map has the building (a modded town may not).
		int solid = 0;
		int tiles = 0;
		for (int y = place.claimMin.y; y <= place.claimMax.y; y++)
			for (int x = place.claimMin.x; x <= place.claimMax.x; x++, tiles++)
				solid += IsTileSolid({ x, y }) || GetPieceTextures(dPiece[x][y]).hasTallArt ? 1 : 0;
		if (solid * 2 < tiles) {
			LogInfo("fp: landmark {} not found on this town map", place.name);
			continue;
		}
		std::erase_if(Buildings, [&](const Building &b) {
			return b.x0 <= static_cast<float>(place.claimMax.x) && b.x1 > static_cast<float>(place.claimMin.x) && b.y0 <= static_cast<float>(place.claimMax.y)
			    && b.y1 > static_cast<float>(place.claimMin.y);
		});
		Building b {};
		b.x0 = static_cast<float>(place.claimMin.x);
		b.y0 = static_cast<float>(place.claimMin.y);
		b.x1 = static_cast<float>(place.claimMax.x + 1);
		b.y1 = static_cast<float>(place.claimMax.y + 1);
		b.landmark = static_cast<int>(l);
		std::vector<Point> artTiles;
		std::vector<Point> groundTiles;
		for (int y = place.claimMin.y - 2; y <= place.claimMax.y + 2; y++) {
			for (int x = place.claimMin.x - 2; x <= place.claimMax.x + 2; x++) {
				if (!InDungeonBounds({ x, y }))
					continue;
				const bool inside = x >= place.claimMin.x && x <= place.claimMax.x && y >= place.claimMin.y && y <= place.claimMax.y;
				if (GetPieceTextures(dPiece[x][y]).hasTallArt && (inside || IsTileSolid({ x, y })))
					artTiles.push_back({ x, y });
				else if (inside)
					groundTiles.push_back({ x, y });
			}
		}
		BuildCanvas(b, artTiles, groundTiles);
		FitBuilding(b);
		BuildRamps(b);
		BuildTextures(b);
		for (const Point t : artTiles)
			BuildingTiles[static_cast<size_t>(t.y) * MAXDUNX + t.x] = 1;
		BuildLandmarkMesh(b, place, *mesh);
		LogInfo("fp: landmark {} placed with {} triangles", place.name, b.mesh.size());
		Buildings.push_back(std::move(b));
	}
}

/** @brief Picks the voxel model whose proportions best match the footprint of the building. */
void ChooseModel(Building &b)
{
	LoadHouseModels();
	if (HouseModels.empty())
		return;
	BuildRamps(b);
	BuildTextures(b);
	if (b.wallRamp.size() < 20 || b.roofRamp.size() < 20)
		return; // not enough art to take colours from
	const float width = b.x1 - b.x0;
	const float depth = b.y1 - b.y0;
	const float ratio = std::max(width, depth) / std::min(width, depth);
	float best = 1e9F;
	for (size_t i = 0; i < HouseModels.size(); i++) {
		const float modelRatio = HouseModels[i].longUnits / HouseModels[i].shortUnits;
		const float score = std::abs(std::log(ratio / modelRatio));
		if (score < best) {
			best = score;
			b.model = static_cast<int>(i);
		}
	}
	// Long axis of the model along the longer footprint side; for a near-square one, along the fitted ridge.
	b.longAlongX = ratio < 1.15F ? b.params.ridgeAlongX : width >= depth;
}

/** @brief A footprint candidate with its fitted model. */
struct Candidate {
	Building building;
	std::vector<Point> block;
	std::vector<Point> artTiles;
	bool valid = false;
};

/**
 * @brief Builds the model for one house given a way to tell its ground tiles apart.
 * @param isGround whether a tile belongs to the footprint block that the wall-art tiles stand in front of
 */
template <typename Predicate>
Candidate MakeCandidate(const std::vector<Point> &art, int artMinX, int artMaxX, int artMinY, int artMaxY, Predicate isGround)
{
	Candidate result;
	auto index = [](int x, int y) { return static_cast<size_t>(y) * MAXDUNX + x; };
	std::vector<uint8_t> inBlock(static_cast<size_t>(MAXDUNX) * MAXDUNY, 0);
	std::vector<Point> block;
	std::vector<Point> stack;
	auto inWindow = [&](Point p) { return p.x >= artMinX - 8 && p.x <= artMaxX + 1 && p.y >= artMinY - 8 && p.y <= artMaxY + 1; };
	auto tryAdd = [&](Point n) {
		if (InDungeonBounds(n) && inWindow(n) && inBlock[index(n.x, n.y)] == 0 && isGround(n.x, n.y)) {
			inBlock[index(n.x, n.y)] = 1;
			stack.push_back(n);
		}
	};
	// The ground block lies behind the wall art (towards -x / -y), so start from the neighbours on those sides.
	for (const Point p : art) {
		for (const Point d : { Point { 1, 0 }, Point { -1, 0 }, Point { 0, 1 }, Point { 0, -1 } })
			tryAdd({ p.x + d.x, p.y + d.y });
	}
	while (!stack.empty()) {
		const Point p = stack.back();
		stack.pop_back();
		block.push_back(p);
		for (const Point d : { Point { 1, 0 }, Point { -1, 0 }, Point { 0, 1 }, Point { 0, -1 } })
			tryAdd({ p.x + d.x, p.y + d.y });
	}
	if (static_cast<int>(block.size()) < MinBuildingTiles)
		return result;

	int minX = MAXDUNX;
	int maxX = 0;
	int minY = MAXDUNY;
	int maxY = 0;
	for (const Point p : block) {
		minX = std::min(minX, p.x);
		maxX = std::max(maxX, p.x);
		minY = std::min(minY, p.y);
		maxY = std::max(maxY, p.y);
	}
	// A house is a rectangle. If the flooded area is clearly not one (it spilled into neighbouring ground), keep
	// the largest fully solid rectangle of it instead.
	if (static_cast<float>(block.size()) < SpilledFillShare * static_cast<float>((maxX - minX + 1) * (maxY - minY + 1))) {
		const int w = maxX - minX + 1;
		const int h = maxY - minY + 1;
		int bestArea = 0;
		int bx0 = 0;
		int by0 = 0;
		int bx1 = 0;
		int by1 = 0;
		for (int ry0 = 0; ry0 < h; ry0++) {
			for (int rx0 = 0; rx0 < w; rx0++) {
				for (int ry1 = ry0; ry1 < h; ry1++) {
					for (int rx1 = rx0; rx1 < w; rx1++) {
						bool full = true;
						for (int ry = ry0; ry <= ry1 && full; ry++)
							for (int rx = rx0; rx <= rx1 && full; rx++)
								full = inBlock[index(minX + rx, minY + ry)] != 0;
						if (!full)
							break;
						const int rectArea = (rx1 - rx0 + 1) * (ry1 - ry0 + 1);
						if (rectArea > bestArea) {
							bestArea = rectArea;
							bx0 = rx0;
							by0 = ry0;
							bx1 = rx1;
							by1 = ry1;
						}
					}
				}
			}
		}
		if (bestArea < MinBuildingTiles)
			return result;
		block.clear();
		for (int ry = by0; ry <= by1; ry++)
			for (int rx = bx0; rx <= bx1; rx++)
				block.push_back({ minX + rx, minY + ry });
		maxX = minX + bx1;
		maxY = minY + by1;
		minX += bx0;
		minY += by0;
	}
	const int area = (maxX - minX + 1) * (maxY - minY + 1);
	if (static_cast<float>(block.size()) < MinFillShare * static_cast<float>(area) || maxX - minX >= MaxBuildingSpan || maxY - minY >= MaxBuildingSpan)
		return result;

	Building& building = result.building;
	building.x0 = static_cast<float>(minX);
	building.y0 = static_cast<float>(minY);
	building.x1 = static_cast<float>(maxX + 1);
	building.y1 = static_cast<float>(maxY + 1);
	// All tall art around the block (front walls, back edge, roof pieces) makes up the picture.
	for (int ty = minY - 2; ty <= maxY + 2; ty++)
		for (int tx = minX - 2; tx <= maxX + 2; tx++)
			if (InDungeonBounds({ tx, ty }) && GetPieceTextures(dPiece[tx][ty]).hasTallArt)
				result.artTiles.push_back({ tx, ty });
	if (result.artTiles.empty())
		return result;
	// The ground under the building belongs in the picture too, so the model's footprint has art.
	std::vector<Point> groundTiles;
	for (const Point t : block)
		if (!GetPieceTextures(dPiece[t.x][t.y]).hasTallArt)
			groundTiles.push_back(t);
	BuildCanvas(building, result.artTiles, groundTiles);
	FitBuilding(building);
	result.block = std::move(block);
	result.valid = true;
	return result;
}

void BuildAll()
{
	Buildings.clear();
	BuildingTiles.assign(static_cast<size_t>(MAXDUNX) * MAXDUNY, 0);
	if (leveltype != DTYPE_TOWN)
		return;

	auto index = [](int x, int y) { return static_cast<size_t>(y) * MAXDUNX + x; };
	auto isStructure = [](int x, int y) { return GetPieceTextures(dPiece[x][y]).isStructure; };
	// Candidate 1: only the flat solid ground (the wall art sits in the tiles in front of it, not on it).
	auto isFlatGround = [](int x, int y) { return IsTileSolid({ x, y }) && !GetPieceTextures(dPiece[x][y]).hasTallArt; };
	// Candidate 2: every solid tile that is not itself front wall art, so back-wall pieces count as footprint.
	auto isAnySolid = [&](int x, int y) { return IsTileSolid({ x, y }) && !isStructure(x, y); };

	std::vector<uint8_t> seen(static_cast<size_t>(MAXDUNX) * MAXDUNY, 0);
	std::vector<uint8_t> blockUsed(static_cast<size_t>(MAXDUNX) * MAXDUNY, 0);
	for (int y = 0; y < MAXDUNY; y++) {
		for (int x = 0; x < MAXDUNX; x++) {
			if (seen[index(x, y)] != 0 || !isStructure(x, y))
				continue;
			// Group the wall-art tiles of one building.
			std::vector<Point> art;
			std::vector<Point> stack { { x, y } };
			seen[index(x, y)] = 1;
			while (!stack.empty()) {
				const Point p = stack.back();
				stack.pop_back();
				art.push_back(p);
				for (int dy = -1; dy <= 1; dy++) {
					for (int dx = -1; dx <= 1; dx++) {
						const Point n { p.x + dx, p.y + dy };
						if (!InDungeonBounds(n) || seen[index(n.x, n.y)] != 0 || !isStructure(n.x, n.y))
							continue;
						seen[index(n.x, n.y)] = 1;
						stack.push_back(n);
					}
				}
			}
			int artMinX = MAXDUNX;
			int artMaxX = 0;
			int artMinY = MAXDUNY;
			int artMaxY = 0;
			for (const Point p : art) {
				artMinX = std::min(artMinX, p.x);
				artMaxX = std::max(artMaxX, p.x);
				artMinY = std::min(artMinY, p.y);
				artMaxY = std::max(artMaxY, p.y);
			}

			Candidate flat = MakeCandidate(art, artMinX, artMaxX, artMinY, artMaxY, isFlatGround);
			Candidate solid = MakeCandidate(art, artMinX, artMaxX, artMinY, artMaxY, isAnySolid);
			Candidate *chosen = nullptr;
			for (Candidate *candidate : { &flat, &solid }) {
				if (!candidate->valid)
					continue;
				bool taken = false; // several wall-art groups can belong to the same house
				for (const Point p : candidate->block)
					taken = taken || blockUsed[index(p.x, p.y)] != 0;
				if (!taken && (chosen == nullptr || candidate->building.score > chosen->building.score))
					chosen = candidate;
			}
			if (chosen == nullptr)
				continue;
			const Building &b = chosen->building;
			LogInfo("building {}..{} x {}..{} ({}): ridge along {}, eave {:.2f}, rise {:.2f}, overhang {:.2f}, fit {:.2f}", static_cast<int>(b.x0), static_cast<int>(b.x1) - 1,
			    static_cast<int>(b.y0), static_cast<int>(b.y1) - 1, chosen == &flat ? "flat ground" : "any solid", b.params.ridgeAlongX ? "x" : "y", b.params.eave, b.params.rise,
			    b.params.overhang, b.score);
#ifdef _DEBUG
			if (const char *dumpDir = std::getenv("FP_DUMP_BUILDINGS"))
				DumpBuilding(b, StrCat(dumpDir, "/b_", static_cast<int>(b.x0), "_", static_cast<int>(b.y0), ".ppm"));
#endif
			if (b.score < MinFitScore)
				continue;
			for (const Point p : chosen->block)
				blockUsed[index(p.x, p.y)] = 1;
			for (const Point t : chosen->artTiles)
				BuildingTiles[index(t.x, t.y)] = 1;
			ChooseModel(chosen->building);
			Buildings.push_back(std::move(chosen->building));
		}
	}
	AddLandmarks();
	// A house whose art shows too little wall or thatch borrows the patch of the nearest house that has one:
	// neighbours are built alike, while the biggest patch in town may be the church's.
	auto usable = [](const SurfaceTexture &patch) { return patch.valid && patch.width >= BorrowPatchBelow && patch.height >= BorrowPatchBelow; };
	auto nearest = [&](const Building &to, SurfaceTexture Building::*patch) {
		const Building *best = nullptr;
		float bestDistance = 0;
		for (const Building &b : Buildings) {
			if (&b == &to || !usable(b.*patch))
				continue;
			const float dx = (b.x0 + b.x1 - to.x0 - to.x1) / 2.0F;
			const float dy = (b.y0 + b.y1 - to.y0 - to.y1) / 2.0F;
			const float distance = dx * dx + dy * dy;
			if (best == nullptr || distance < bestDistance) {
				best = &b;
				bestDistance = distance;
			}
		}
		return best != nullptr ? best->*patch : SurfaceTexture {};
	};
	std::vector<std::pair<SurfaceTexture, SurfaceTexture>> borrowed(Buildings.size());
	for (size_t i = 0; i < Buildings.size(); i++) {
		const Building &b = Buildings[i];
		borrowed[i] = { usable(b.wallPatch) ? b.wallPatch : nearest(b, &Building::wallPatch), usable(b.roofPatch) ? b.roofPatch : nearest(b, &Building::roofPatch) };
	}
	for (size_t i = 0; i < Buildings.size(); i++) {
		Building &b = Buildings[i];
		if (b.model < 0 && b.landmark < 0)
			continue;
		b.wallPatch = borrowed[i].first;
		b.roofPatch = borrowed[i].second;
		if (b.landmark >= 0)
			continue; // the mesh is built already, and there is no voxel model
		const float voxel = VoxelSize(b);
		b.wallPatchVoxel = b.wallPatch;
		b.roofPatchVoxel = b.roofPatch;
		FilterPatch(b.wallPatchVoxel, static_cast<int>(std::lround(voxel * 32.0F)), static_cast<int>(std::lround(voxel * HeightPixels)));
		FilterPatch(b.roofPatchVoxel, static_cast<int>(std::lround(voxel * 32.0F)), static_cast<int>(std::lround(voxel * 32.0F)));
		BuildMesh(b);
	}
}

void EnsureBuilt()
{
	const LevelKey key { pDungeonCels.get(), currlevel, static_cast<int>(leveltype), setlevel };
	if (key == BuiltFor)
		return;
	BuiltFor = key;
	BuildAll();
}

/** @brief Screen rectangle that can contain the building; false if it is entirely off screen. */
bool ScreenBounds(const Building &b, const Projection &proj, int &left, int &top, int &right, int &bottom)
{
	float minX = 1e30F;
	float maxX = -1e30F;
	float minY = 1e30F;
	float maxY = -1e30F;
	bool allBehind = true;
	const float ov = std::max(b.params.overhang, b.boundsOverhang);
	for (const float wx : { b.x0 - ov, b.x1 + ov }) {
		for (const float wy : { b.y0 - ov, b.y1 + ov }) {
			for (const float wz : { b.lowestZ, b.highestZ }) {
				const float rx = wx - proj.pos.x;
				const float ry = wy - proj.pos.y;
				float depth = rx * proj.dir.x + ry * proj.dir.y;
				allBehind = allBehind && depth < 0.05F;
				// Corners at or behind the camera plane project far off screen: clamp them to a small depth.
				depth = std::max(depth, 0.05F);
				const float lateral = (rx * proj.plane.x + ry * proj.plane.y) / (PlaneLength * PlaneLength);
				const float sx = (lateral / depth + 1.0F) * static_cast<float>(proj.width) / 2.0F;
				const float sy = static_cast<float>(proj.horizon) - (wz - EyeHeight) / depth * proj.focal;
				minX = std::min(minX, sx);
				maxX = std::max(maxX, sx);
				minY = std::min(minY, sy);
				maxY = std::max(maxY, sy);
			}
		}
	}
	if (allBehind)
		return false;
	left = std::max(0, static_cast<int>(std::floor(std::max(minX, -1.0F))));
	right = std::min(proj.width - 1, static_cast<int>(std::ceil(std::min(maxX, static_cast<float>(proj.width)))));
	top = std::max(0, static_cast<int>(std::floor(std::max(minY, -1.0F))));
	bottom = std::min(proj.height - 1, static_cast<int>(std::ceil(std::min(maxY, static_cast<float>(proj.height)))));
	return left <= right && top <= bottom;
}

/** @brief Draws the house as its voxel model: each voxel is projected and splatted as a small square with a per-pixel depth test. */
void DrawHouseModel(const Surface &out, const Projection &proj, std::vector<float> &depthBuffer, const Building &b)
{
	const HouseModel &model = HouseModels[b.model];
	const float width = b.x1 - b.x0;
	const float depth = b.y1 - b.y0;
	const float cx = (b.x0 + b.x1) / 2.0F;
	const float cy = (b.y0 + b.y1) / 2.0F;
	const float shortSide = std::min(width, depth);
	const float longSide = std::max(width, depth);
	const float overhang = 0.12F * shortSide;
	const float spanLong = longSide + 2.0F * overhang;
	const float spanShort = shortSide + 2.0F * overhang;
	const float stepLong = spanLong / static_cast<float>(model.nx);
	const float stepShort = spanShort / static_cast<float>(model.ny);
	const float stepUp = stepShort; // the same scale as across the house keeps the roof pitch of the reference
	const float voxelWorld = std::max({ stepLong, stepShort, stepUp });
	const float baseHeight = 0.0F;

	const size_t count = model.voxels.size() / 5;
	for (size_t n = 0; n < count; n++) {
		const uint8_t *v = &model.voxels[n * 5];
		const float along = (static_cast<float>(v[0]) + 0.5F) * stepLong - spanLong / 2.0F;
		const float across = (static_cast<float>(v[1]) + 0.5F) * stepShort - spanShort / 2.0F;
		const float wx = cx + (b.longAlongX ? along : across);
		const float wy = cy + (b.longAlongX ? across : along);
		const float wz = baseHeight + (static_cast<float>(v[2]) + 0.5F) * stepUp;

		const float rx = wx - proj.pos.x;
		const float ry = wy - proj.pos.y;
		const float distance = rx * proj.dir.x + ry * proj.dir.y;
		if (distance < 0.15F)
			continue;
		const float lateral = (rx * proj.plane.x + ry * proj.plane.y) / (PlaneLength * PlaneLength);
		const float sx = (lateral / distance + 1.0F) * static_cast<float>(proj.width) / 2.0F;
		const float sy = static_cast<float>(proj.horizon) - (wz - EyeHeight) / distance * proj.focal;
		const int size = std::max(1, static_cast<int>(std::ceil(voxelWorld * proj.focal / distance)));
		const int x0 = static_cast<int>(std::floor(sx - static_cast<float>(size) / 2.0F));
		const int y0 = static_cast<int>(std::floor(sy - static_cast<float>(size) / 2.0F));
		if (x0 + size <= 0 || y0 + size <= 0 || x0 >= proj.width || y0 >= proj.height)
			continue;

		uint8_t color = TexturedHouses ? ArtColor(b, v[3] != 0, wx, wy, wz) : 0;
		if (color == 0) {
			const std::vector<uint8_t> &ramp = v[3] == 0 ? b.wallRamp : b.roofRamp;
			// Walls use the lighter 70% of their art (the darkest pixels are mortar and shadow), thatch the darker 80%.
			const float low = v[3] == 0 ? 0.30F : 0.0F;
			const float high = v[3] == 0 ? 1.0F : 0.80F;
			const float percentile = low + (high - low) * static_cast<float>(v[4]) / 255.0F;
			color = ramp[static_cast<size_t>(percentile * static_cast<float>(ramp.size() - 1))];
		}
		const Point tile { std::clamp(static_cast<int>(std::floor(wx)), static_cast<int>(b.x0), static_cast<int>(b.x1) - 1),
			std::clamp(static_cast<int>(std::floor(wy)), static_cast<int>(b.y0), static_cast<int>(b.y1) - 1) };
		const uint8_t shaded = LightTables[LightAt(tile)][color];
		for (int y = std::max(0, y0); y < std::min(proj.height, y0 + size); y++) {
			for (int x = std::max(0, x0); x < std::min(proj.width, x0 + size); x++) {
				float &stored = depthBuffer[static_cast<size_t>(y) * proj.width + x];
				if (distance >= stored)
					continue;
				stored = distance;
				*out.at(x, y) = shaded;
			}
		}
	}
}

/** @brief Palette colour of a mesh surface point: the house's own art patches tiled at the art's pixel density. */
uint8_t MeshColor(const Building &b, const WorldTriangle &tri, float wx, float wy, float wz)
{
	const SurfaceTexture &patch = tri.roof ? b.roofPatch : b.wallPatch;
	const float up = tri.roof ? 32.0F : HeightPixels;
	switch (tri.mapping) {
	case MeshRoof:
		return ScatteredTexel(patch, (tri.downX * wy - tri.downY * wx) * 32.0F, -(tri.downX * wx + tri.downY * wy) * tri.slopeScale * 32.0F);
	case MeshFlat:
		return ScatteredTexel(patch, wx * 32.0F, wy * 32.0F);
	case MeshWallAlongX:
		return ScatteredTexel(patch, wx * 32.0F, wz * up);
	default:
		return ScatteredTexel(patch, wy * 32.0F, wz * up);
	}
}

/** @brief A mesh vertex in camera space: depth along the view, screen position, and the world point. */
struct CameraVertex {
	float depth;
	float lateral; // screen x offset at depth 1
	float height;  // world height above the eye
	V3 world;
};

/**
 * @brief Fills world triangles into the view: each is clipped at the near plane and filled with
 * perspective-correct world positions, sharing the depth buffer with the rest of the scene.
 * @param shade palette colour (lit) of a surface point of a triangle, or -1 to leave the pixel alone
 */
template <typename ShadeFn>
void RasterizeTriangles(const Surface &out, const Projection &proj, std::vector<float> &depthBuffer, const std::vector<WorldTriangle> &triangles, ShadeFn shade)
{
	constexpr float Near = 0.05F;
	const float halfWidth = static_cast<float>(proj.width) / 2.0F;
	auto toCamera = [&](const V3 &w) {
		const float rx = w.x - proj.pos.x;
		const float ry = w.y - proj.pos.y;
		return CameraVertex { rx * proj.dir.x + ry * proj.dir.y, (rx * proj.plane.x + ry * proj.plane.y) / (PlaneLength * PlaneLength), w.z - EyeHeight, w };
	};
	auto lerp = [](const CameraVertex &a, const CameraVertex &c, float t) {
		return CameraVertex { a.depth + (c.depth - a.depth) * t, a.lateral + (c.lateral - a.lateral) * t, a.height + (c.height - a.height) * t,
			{ a.world.x + (c.world.x - a.world.x) * t, a.world.y + (c.world.y - a.world.y) * t, a.world.z + (c.world.z - a.world.z) * t } };
	};
	for (const WorldTriangle &tri : triangles) {
		std::array<CameraVertex, 3> in { toCamera(tri.p[0]), toCamera(tri.p[1]), toCamera(tri.p[2]) };
		if (in[0].depth < Near && in[1].depth < Near && in[2].depth < Near)
			continue;
		// Clip at the near plane: up to four vertices remain.
		std::array<CameraVertex, 4> poly;
		int count = 0;
		for (int i = 0; i < 3; i++) {
			const CameraVertex &a = in[i];
			const CameraVertex &c = in[(i + 1) % 3];
			if (a.depth >= Near)
				poly[count++] = a;
			if ((a.depth >= Near) != (c.depth >= Near))
				poly[count++] = lerp(a, c, (Near - a.depth) / (c.depth - a.depth));
		}
		struct ScreenVertex {
			float x;
			float y;
			float invDepth;
			V3 worldOverDepth;
		};
		std::array<ScreenVertex, 4> sv;
		for (int i = 0; i < count; i++) {
			const float inv = 1.0F / poly[i].depth;
			sv[i] = { (poly[i].lateral * inv + 1.0F) * halfWidth, static_cast<float>(proj.horizon) - poly[i].height * inv * proj.focal, inv,
				{ poly[i].world.x * inv, poly[i].world.y * inv, poly[i].world.z * inv } };
		}
		for (int f = 1; f + 1 < count; f++) {
			const ScreenVertex &v0 = sv[0];
			const ScreenVertex &v1 = sv[f];
			const ScreenVertex &v2 = sv[f + 1];
			const float area = (v1.x - v0.x) * (v2.y - v0.y) - (v1.y - v0.y) * (v2.x - v0.x);
			if (std::abs(area) < 1e-6F)
				continue;
			const int left = std::max(0, static_cast<int>(std::floor(std::min({ v0.x, v1.x, v2.x }))));
			const int right = std::min(proj.width - 1, static_cast<int>(std::ceil(std::max({ v0.x, v1.x, v2.x }))));
			const int top = std::max(0, static_cast<int>(std::floor(std::min({ v0.y, v1.y, v2.y }))));
			const int bottom = std::min(proj.height - 1, static_cast<int>(std::ceil(std::max({ v0.y, v1.y, v2.y }))));
			if (left > right || top > bottom)
				continue;
			const float invArea = 1.0F / area;
			for (int y = top; y <= bottom; y++) {
				const float py = static_cast<float>(y) + 0.5F;
				for (int x = left; x <= right; x++) {
					const float px = static_cast<float>(x) + 0.5F;
					const float w0 = ((v1.x - px) * (v2.y - py) - (v1.y - py) * (v2.x - px)) * invArea;
					const float w1 = ((v2.x - px) * (v0.y - py) - (v2.y - py) * (v0.x - px)) * invArea;
					const float w2 = 1.0F - w0 - w1;
					if (w0 < 0.0F || w1 < 0.0F || w2 < 0.0F)
						continue;
					const float inv = w0 * v0.invDepth + w1 * v1.invDepth + w2 * v2.invDepth;
					const float distance = 1.0F / inv;
					float &stored = depthBuffer[static_cast<size_t>(y) * proj.width + x];
					if (distance >= stored)
						continue;
					const float wx = (w0 * v0.worldOverDepth.x + w1 * v1.worldOverDepth.x + w2 * v2.worldOverDepth.x) * distance;
					const float wy = (w0 * v0.worldOverDepth.y + w1 * v1.worldOverDepth.y + w2 * v2.worldOverDepth.y) * distance;
					const float wz = (w0 * v0.worldOverDepth.z + w1 * v1.worldOverDepth.z + w2 * v2.worldOverDepth.z) * distance;
					const int color = shade(tri, wx, wy, wz);
					if (color < 0)
						continue;
					stored = distance;
					*out.at(x, y) = static_cast<uint8_t>(color);
				}
			}
		}
	}
}

/** @brief Draws the house as its triangle mesh, textured from the house's art patches. */
void DrawHouseMesh(const Surface &out, const Projection &proj, std::vector<float> &depthBuffer, const Building &b)
{
	const Point lightMin { static_cast<int>(b.x0), static_cast<int>(b.y0) };
	const Point lightMax { static_cast<int>(b.x1) - 1, static_cast<int>(b.y1) - 1 };
	// Light per tile, looked up once per frame for the footprint.
	const int lightWidth = lightMax.x - lightMin.x + 1;
	std::vector<uint8_t> light(static_cast<size_t>(lightWidth) * (lightMax.y - lightMin.y + 1));
	for (int y = lightMin.y; y <= lightMax.y; y++)
		for (int x = lightMin.x; x <= lightMax.x; x++)
			light[static_cast<size_t>(y - lightMin.y) * lightWidth + x - lightMin.x] = LightAt({ x, y });
	RasterizeTriangles(out, proj, depthBuffer, b.mesh, [&](const WorldTriangle &tri, float wx, float wy, float wz) {
		const uint8_t color = MeshColor(b, tri, wx, wy, wz);
		const int tx = std::clamp(static_cast<int>(std::floor(wx)), lightMin.x, lightMax.x);
		const int ty = std::clamp(static_cast<int>(std::floor(wy)), lightMin.y, lightMax.y);
		const uint8_t level = light[static_cast<size_t>(ty - lightMin.y) * lightWidth + tx - lightMin.x];
		return static_cast<int>(LightTables[level][ShadeColor(color, tri.darkness)]);
	});
}

void DrawBuilding(const Surface &out, const Projection &proj, const std::vector<float> &zBuffer, std::vector<float> &depthBuffer, const Building &b)
{
	int left;
	int top;
	int right;
	int bottom;
	if (!ScreenBounds(b, proj, left, top, right, bottom))
		return;
	constexpr float MinDepth = 0.05F;
	constexpr int MaxPlanes = 11;
	const V3 origin { proj.pos.x, proj.pos.y, EyeHeight };

	// All planes of both bodies; the ray tests only need a few terms per pixel.
	std::array<const Plane *, MaxPlanes> planes;
	std::array<float, MaxPlanes> constant; // d - n . origin
	int count = 0;
	const int wallCount = b.walls.count;
	for (const Body *body : { &b.walls, &b.roof }) {
		for (int i = 0; i < body->count; i++) {
			planes[count] = &body->planes[i];
			constant[count] = body->planes[i].d - (body->planes[i].nx * origin.x + body->planes[i].ny * origin.y + body->planes[i].nz * origin.z);
			count++;
		}
	}

	std::array<float, MaxPlanes> horizontal; // n . (rx, ry) for the current column
	for (int x = left; x <= right; x++) {
		const Vec2 ray = proj.columnRay(x);
		for (int i = 0; i < count; i++)
			horizontal[i] = planes[i]->nx * ray.x + planes[i]->ny * ray.y;
		const float columnLimit = zBuffer[x];
		for (int y = top; y <= bottom; y++) {
			const float v = proj.rowSlope(y);
			float bestT = std::numeric_limits<float>::infinity();
			int bestPlane = -1;
			for (int first = 0, end = wallCount; first < count; first = end, end = count) {
				float tIn = -std::numeric_limits<float>::infinity();
				float tOut = std::numeric_limits<float>::infinity();
				int face = -1;
				bool miss = false;
				for (int i = first; i < end; i++) {
					const float denom = horizontal[i] + planes[i]->nz * v;
					if (std::abs(denom) < 1e-9F) {
						if (constant[i] < 0) {
							miss = true;
							break;
						}
						continue;
					}
					const float t = constant[i] / denom;
					if (denom > 0) {
						if (t < tOut)
							tOut = t;
					} else if (t > tIn) {
						tIn = t;
						face = i;
					}
					if (tIn > tOut) {
						miss = true;
						break;
					}
				}
				if (!miss && face >= 0 && tIn >= MinDepth && tIn < bestT) {
					bestT = tIn;
					bestPlane = face;
				}
			}
			if (bestPlane < 0)
				continue;
			const size_t index = static_cast<size_t>(y) * proj.width + x;
			if (bestT >= columnLimit || bestT >= depthBuffer[index])
				continue;
			const V3 hit { origin.x + ray.x * bestT, origin.y + ray.y * bestT, origin.z + v * bestT };
			const int16_t color = SurfaceColor(b, *planes[bestPlane], hit);
			if (color < 0)
				continue;
			const Point tile { std::clamp(static_cast<int>(std::floor(hit.x)), static_cast<int>(b.x0), static_cast<int>(b.x1) - 1),
				std::clamp(static_cast<int>(std::floor(hit.y)), static_cast<int>(b.y0), static_cast<int>(b.y1) - 1) };
			uint8_t light = LightAt(tile);
			if ((color & UndersideFlag) != 0)
				light = std::min<uint8_t>(light + UndersideDarkness, LightsMax);
			*out.at(x, y) = LightTables[light][color & 0xFF];
			depthBuffer[index] = bestT;
		}
	}
}


// ---------------------------------------------------------------------------------------------------------------
// Dungeon structures drawn as meshes: free-standing columns (and torch stands) and the cathedral's stairs down.

/** @brief A piece of a dungeon drawn as a mesh, with the iso art it is textured from. */
struct Structure {
	Building art; // canvas (and origin) of the art; the mesh is art.mesh
	// For MeshPit surfaces: where the art shows a stretch of plain wall to repeat.
	V3 pitSource { 0, 0, 0 };
	float pitSpanY = 1.0F;
	float pitSpanZ = 1.0F;
};

std::vector<Structure> Structures;
std::vector<uint8_t> StructureTiles; // 1: drawn by a structure (no wall cube, no billboard); 2: also has a floor hole
std::vector<std::array<float, 4>> FloorHoles; // x0, y0, x1, y1
LevelKey StructuresFor { nullptr, -1, -1, false };

/** @brief Adds a flat polygon (3 or 4 corners) of a structure with its outward normal. */
void AddStructureFace(Building &b, std::initializer_list<V3> corners, V3 normal, Vec2 mirror, uint8_t mapping, uint8_t darkness = 0)
{
	const std::vector<V3> p(corners);
	const float length = std::sqrt(normal.x * normal.x + normal.y * normal.y + normal.z * normal.z);
	if (length > 1e-6F)
		normal = { normal.x / length, normal.y / length, normal.z / length };
	WorldTriangle tri {};
	tri.mapping = mapping;
	tri.mirrorX = mirror.x;
	tri.mirrorY = mirror.y;
	const float horizontal = std::sqrt(normal.x * normal.x + normal.y * normal.y);
	tri.normalX = horizontal > 0.25F ? normal.x / horizontal : 0.0F;
	tri.normalY = horizontal > 0.25F ? normal.y / horizontal : 0.0F;
	// The iso camera looks from +x, +y: it saw the surfaces facing that way, and the tops.
	tri.mirrored = horizontal > 0.25F && tri.normalX + tri.normalY < -0.05F;
	tri.darkness = darkness;
	if (normal.z < -0.5F)
		tri.darkness = std::max<uint8_t>(darkness, 3);
	else if (horizontal > 0.25F && tri.normalX + tri.normalY < -0.5F)
		tri.darkness = std::max<uint8_t>(darkness, 1);
	tri.fallback = 0;
	tri.slopeScale = 1.0F;
	for (size_t k = 1; k + 1 < p.size(); k++) {
		tri.p = { p[0], p[k], p[k + 1] };
		b.mesh.push_back(tri);
	}
}

/** @brief An axis-aligned box: four sides and the top (and the bottom if asked). */
void AddStructureBox(Building &b, float x0, float y0, float z0, float x1, float y1, float z1, uint8_t mapping = MeshProjective, bool bottom = false)
{
	const Vec2 mid { (x0 + x1) / 2.0F, (y0 + y1) / 2.0F };
	AddStructureFace(b, { { x1, y0, z0 }, { x1, y1, z0 }, { x1, y1, z1 }, { x1, y0, z1 } }, { 1, 0, 0 }, mid, mapping);
	AddStructureFace(b, { { x0, y0, z0 }, { x0, y1, z0 }, { x0, y1, z1 }, { x0, y0, z1 } }, { -1, 0, 0 }, mid, mapping);
	AddStructureFace(b, { { x0, y1, z0 }, { x1, y1, z0 }, { x1, y1, z1 }, { x0, y1, z1 } }, { 0, 1, 0 }, mid, mapping);
	AddStructureFace(b, { { x0, y0, z0 }, { x1, y0, z0 }, { x1, y0, z1 }, { x0, y0, z1 } }, { 0, -1, 0 }, mid, mapping);
	AddStructureFace(b, { { x0, y0, z1 }, { x1, y0, z1 }, { x1, y1, z1 }, { x0, y1, z1 } }, { 0, 0, 1 }, mid, mapping);
	if (bottom)
		AddStructureFace(b, { { x0, y0, z0 }, { x1, y0, z0 }, { x1, y1, z0 }, { x0, y1, z0 } }, { 0, 0, -1 }, mid, mapping);
}

/** @brief An upright many-sided prism (a round shaft), with its top if asked. */
void AddStructureShaft(Building &b, Vec2 c, float radius, float z0, float z1, bool top, int sides = 12)
{
	constexpr float Pi = 3.14159265F;
	for (int i = 0; i < sides; i++) {
		const float a0 = 2.0F * Pi * static_cast<float>(i) / static_cast<float>(sides);
		const float a1 = 2.0F * Pi * static_cast<float>(i + 1) / static_cast<float>(sides);
		const float am = (a0 + a1) / 2.0F;
		const V3 p0 { c.x + radius * std::cos(a0), c.y + radius * std::sin(a0), z0 };
		const V3 p1 { c.x + radius * std::cos(a1), c.y + radius * std::sin(a1), z0 };
		AddStructureFace(b, { p0, p1, { p1.x, p1.y, z1 }, { p0.x, p0.y, z1 } }, { std::cos(am), std::sin(am), 0 }, c, MeshProjective);
		if (top)
			AddStructureFace(b, { { c.x, c.y, z1 }, { p0.x, p0.y, z1 }, { p1.x, p1.y, z1 } }, { 0, 0, 1 }, c, MeshProjective);
	}
}

/** @brief A slice of a column's profile: square or round, from z0 up to z1, its size changing linearly. */
struct ColumnSlice {
	float z0;
	float z1;
	float size0; // half side (square) or radius (round) at z0, in tiles
	float size1; // the same at z1
	bool square;
};

/**
 * @brief The profile of a free-standing column, read off its iso art row by row. A point at the left or right
 * extreme of an upright square or round shape has x + y equal to the axis's, so the widest pixels of a row are at
 * the height that row shows: z = (rows above the tile centre) / 39.2. Squares (plinths, capitals) look 64 px wide
 * per tile of half side, round shafts 45 px per tile of radius; parts much wider than a stout shaft are taken as
 * square. Empty if the art is not one symmetric upright piece (the arcade pillars carry half a wall).
 */
std::vector<ColumnSlice> ColumnProfile(const PieceTextures &textures, int &axisTwice)
{
	const int rows = static_cast<int>(textures.column.size() / ColumnWidth);
	const int centerRow = rows - 1 - 16; // the middle of the tile's floor diamond
	std::vector<std::pair<float, float>> widths; // z, half width in px
	int asymmetric = 0;
	// The floor diamond takes up the rows just above the centre too: its pixels are told apart from the base's
	// by colour, the floor using the level's ground colours.
	std::array<bool, 256> groundColor {};
	for (const int16_t texel : GetGroundFloor())
		if (texel > 0)
			groundColor[static_cast<uint8_t>(texel)] = true;
	struct RowSpan {
		float z;
		int first;
		int last;
	};
	std::vector<RowSpan> spans;
	int gap = 0;
	for (int row = centerRow - 1; row >= 0; row--) {
		const bool inFloor = row >= centerRow - 16;
		int first = ColumnWidth;
		int last = -1;
		for (int x = 0; x < ColumnWidth; x++) {
			const int16_t pixel = textures.column[static_cast<size_t>(row) * ColumnWidth + x];
			if (pixel > 0 && !(inFloor && groundColor[static_cast<uint8_t>(pixel)])) {
				first = std::min(first, x);
				last = x;
			}
		}
		if (last < first) {
			// Thin poles are dithered, with empty rows: only a long run of them is above the top.
			if (!spans.empty() && ++gap > 12)
				break;
			continue;
		}
		gap = 0;
		spans.push_back({ static_cast<float>(centerRow - row) / SpritePixelsPerTileY, first, last });
	}
	if (spans.size() < 6)
		return {};
	// The axis: the middle of most rows (twice, in px), which need not be the tile's (a torch stand's pole is a pixel off).
	std::vector<int> middles;
	for (const RowSpan &span : spans)
		middles.push_back(span.first + span.last + 1);
	std::nth_element(middles.begin(), middles.begin() + static_cast<ptrdiff_t>(middles.size() / 2), middles.end());
	axisTwice = middles[middles.size() / 2];
	for (const RowSpan &span : spans) {
		const int leftHalf = axisTwice - 2 * span.first;   // twice the half widths
		const int rightHalf = 2 * (span.last + 1) - axisTwice;
		if (std::abs(leftHalf - rightHalf) > 12)
			asymmetric++;
		widths.emplace_back(span.z, static_cast<float>(std::max(leftHalf, rightHalf)) / 2.0F);
	}
#ifdef _DEBUG
	if (std::getenv("FP_DUMP_PROFILES") != nullptr) {
		std::string line;
		for (const RowSpan &span : spans)
			line += StrCat(span.first, "-", span.last, " ");
		LogInfo("fp: column profile (bottom up, asymmetric {}): {}", asymmetric, line);
	}
#endif
	std::vector<ColumnSlice> slices;
	if (widths.size() < 6 || asymmetric * 5 > static_cast<int>(widths.size()))
		return slices;
	// Median of three rows against single stray pixels.
	std::vector<float> smooth(widths.size());
	for (size_t i = 0; i < widths.size(); i++) {
		std::array<float, 3> near { widths[i > 0 ? i - 1 : i].second, widths[i].second, widths[i + 1 < widths.size() ? i + 1 : i].second };
		std::sort(near.begin(), near.end());
		smooth[i] = near[1];
	}
	// Rows where the width falls off fast away from a wider part show that part's top or underside (a flat face
	// seen from above lies across rows), not its sides: they take the width of the narrower part beyond, and at the
	// very top they are dropped.
	constexpr float FaceDrop = 1.2F;
	size_t count = smooth.size();
	for (size_t i = 1; i < count; i++) {
		if (smooth[i] >= smooth[i - 1] - FaceDrop)
			continue;
		size_t k = i;
		while (k < count && smooth[k] < smooth[k - 1] - FaceDrop)
			k++;
		if (k >= count) {
			count = i; // the top face of the topmost part
			break;
		}
		for (size_t j = i; j < k; j++)
			smooth[j] = smooth[k];
		i = k;
	}
	smooth.resize(count);
	widths.resize(count);
	for (size_t i = count; i-- > 1;) {
		if (smooth[i - 1] >= smooth[i] - FaceDrop)
			continue;
		size_t k = i - 1;
		while (k > 0 && smooth[k - 1] < smooth[k] - FaceDrop)
			k--;
		if (k == 0)
			break; // the plinth's front corner reaching the floor stays
		for (size_t j = k; j < i; j++)
			smooth[j] = smooth[k - 1];
		i = k;
	}
	if (count < 6)
		return slices;
	const size_t top = count - 1;
	std::vector<float> sorted(smooth.begin() + static_cast<ptrdiff_t>(count / 4), smooth.begin() + static_cast<ptrdiff_t>(count * 3 / 4));
	std::nth_element(sorted.begin(), sorted.begin() + static_cast<ptrdiff_t>(sorted.size() / 2), sorted.end());
	const float shaft = sorted[sorted.size() / 2];
	for (size_t i = 0; i <= top; i++) {
		const float half = smooth[i];
		const bool square = shaft >= 6.0F && half > 1.35F * shaft;
		const float size = square ? half / 64.0F : half / 45.25F;
		const float z = widths[i].first;
		const float pixelsPerTile = square ? 64.0F : 45.25F;
		if (!slices.empty() && slices.back().square == square) {
			ColumnSlice &last = slices.back();
			// Keep growing the slice while the row continues its straight taper.
			const float slope = last.z1 > last.z0 ? (last.size1 - last.size0) / (last.z1 - last.z0) : 0.0F;
			const float predicted = last.size1 + slope * (z - last.z1);
			if (std::abs(predicted - size) * pixelsPerTile < 0.75F || std::abs(last.size1 - size) * pixelsPerTile < 0.75F) {
				if (std::abs(predicted - size) * pixelsPerTile >= 0.75F)
					last.size0 = last.size1; // flat so far: restart the taper from here
				last.z1 = z;
				last.size1 = size;
				continue;
			}
		}
		slices.push_back({ slices.empty() ? 0.0F : slices.back().z1, z, size, size, square });
	}
	return slices;
}

/** @brief A square frustum (straight-sided, its half side changing from s0 at z0 to s1 at z1), with its top if asked. */
void AddStructureFrustum(Building &b, Vec2 c, float s0, float s1, float z0, float z1, bool top)
{
	const std::array<Vec2, 4> dirs { { { 1, -1 }, { 1, 1 }, { -1, 1 }, { -1, -1 } } };
	for (int i = 0; i < 4; i++) {
		const Vec2 a = dirs[i];
		const Vec2 d = dirs[(i + 1) % 4];
		const V3 p0 { c.x + a.x * s0, c.y + a.y * s0, z0 };
		const V3 p1 { c.x + d.x * s0, c.y + d.y * s0, z0 };
		const V3 p2 { c.x + d.x * s1, c.y + d.y * s1, z1 };
		const V3 p3 { c.x + a.x * s1, c.y + a.y * s1, z1 };
		const Vec2 n { (a.x + d.x) / 2.0F, (a.y + d.y) / 2.0F };
		const float lean = (s0 - s1) / std::max(z1 - z0, 1e-3F);
		AddStructureFace(b, { p0, p1, p2, p3 }, { n.x, n.y, lean }, c, MeshProjective);
	}
	if (top)
		AddStructureFace(b, { { c.x - s1, c.y - s1, z1 }, { c.x + s1, c.y - s1, z1 }, { c.x + s1, c.y + s1, z1 }, { c.x - s1, c.y + s1, z1 } }, { 0, 0, 1 }, c, MeshProjective);
}

/** @brief A round frustum, with its top if asked. */
void AddStructureCone(Building &b, Vec2 c, float r0, float r1, float z0, float z1, bool top, int sides = 12)
{
	constexpr float Pi = 3.14159265F;
	const float lean = (r0 - r1) / std::max(z1 - z0, 1e-3F);
	for (int i = 0; i < sides; i++) {
		const float a0 = 2.0F * Pi * static_cast<float>(i) / static_cast<float>(sides);
		const float a1 = 2.0F * Pi * static_cast<float>(i + 1) / static_cast<float>(sides);
		const float am = (a0 + a1) / 2.0F;
		const V3 p0 { c.x + r0 * std::cos(a0), c.y + r0 * std::sin(a0), z0 };
		const V3 p1 { c.x + r0 * std::cos(a1), c.y + r0 * std::sin(a1), z0 };
		const V3 p2 { c.x + r1 * std::cos(a1), c.y + r1 * std::sin(a1), z1 };
		const V3 p3 { c.x + r1 * std::cos(a0), c.y + r1 * std::sin(a0), z1 };
		AddStructureFace(b, { p0, p1, p2, p3 }, { std::cos(am), std::sin(am), lean }, c, MeshProjective);
		if (top)
			AddStructureFace(b, { { c.x, c.y, z1 }, p3, p2 }, { 0, 0, 1 }, c, MeshProjective);
	}
}

/** @brief Builds a column's mesh at its tile from the profile of its art. */
bool BuildColumn(Structure &st, Point tile)
{
	const PieceTextures &textures = GetPieceTextures(dPiece[tile.x][tile.y]);
	int axisTwice = ColumnWidth;
	const std::vector<ColumnSlice> slices = ColumnProfile(textures, axisTwice);
#ifdef _DEBUG
	if (std::getenv("FP_DUMP_PROFILES") != nullptr)
		LogInfo("fp: column piece {} at {},{}: {} slices, axis {}", dPiece[tile.x][tile.y], tile.x, tile.y, slices.size(), axisTwice);
#endif
	if (slices.empty())
		return false;
	BuildCanvas(st.art, { tile }, {});
	// An axis off the tile's middle by d px on screen is off by d / 32 in x - y.
	const float offset = static_cast<float>(axisTwice - ColumnWidth) / 2.0F / 64.0F;
	const Vec2 c { static_cast<float>(tile.x) + 0.5F + offset, static_cast<float>(tile.y) + 0.5F - offset };
	auto reach = [](const ColumnSlice &slice, bool atTop) { return (atTop ? slice.size1 : slice.size0) * (slice.square ? 1.41F : 1.0F); };
	for (size_t i = 0; i < slices.size(); i++) {
		const ColumnSlice &slice = slices[i];
		// A top wherever the next slice up starts smaller (a ledge) or there is none.
		const bool top = i + 1 == slices.size() || reach(slices[i + 1], false) < reach(slice, true);
		if (slice.square)
			AddStructureFrustum(st.art, c, slice.size0, slice.size1, slice.z0, slice.z1, top);
		else
			AddStructureCone(st.art, c, slice.size0, slice.size1, slice.z0, slice.z1, top);
	}
	return true;
}

/** @brief The cathedral's stairs down: a low walled enclosure with marble corner pillars around steps into a pit. */
bool BuildStairsDown(Structure &st, Point trigger)
{
	// The enclosure takes the 3 x 3 tiles behind the trigger, open towards +y where the trigger tile is.
	const Point min { trigger.x - 1, trigger.y - 2 };
	for (int dy = 0; dy < 3; dy++) {
		for (int dx = 0; dx < 3; dx++) {
			const Point t { min.x + dx, min.y + dy };
			const bool open = dx == 1 && dy == 2;
			if (!InDungeonBounds(t) || IsTileSolid(t) == open)
				return false;
		}
	}
	std::vector<Point> art;
	for (int dy = 0; dy < 3; dy++)
		for (int dx = 0; dx < 3; dx++)
			if (GetPieceTextures(dPiece[min.x + dx][min.y + dy]).hasTallArt)
				art.push_back({ min.x + dx, min.y + dy });
	BuildCanvas(st.art, art, {});
	Building &b = st.art;
	// Measured on the iso art (tile units; heights at 39.2 px per unit).
	const float x0 = static_cast<float>(min.x) + 0.5F; // pillar axes
	const float x1 = static_cast<float>(min.x) + 2.5F;
	const float y0 = static_cast<float>(min.y) + 0.5F;
	const float y1 = static_cast<float>(min.y) + 2.5F;
	constexpr float WallHalf = 0.15F;
	constexpr float WallTop = 1.05F;
	constexpr float CopingHalf = 0.28F;
	constexpr float CopingTop = 1.45F;
	for (const Vec2 c : { Vec2 { x0, y0 }, Vec2 { x1, y0 }, Vec2 { x0, y1 }, Vec2 { x1, y1 } }) {
		AddStructureBox(b, c.x - 0.22F, c.y - 0.22F, 0.0F, c.x + 0.22F, c.y + 0.22F, 0.15F);
		AddStructureShaft(b, c, 0.16F, 0.15F, 1.0F, false);
		AddStructureBox(b, c.x - 0.21F, c.y - 0.21F, 1.0F, c.x + 0.21F, c.y + 0.21F, WallTop);
	}
	// Brick walls on three sides, the coping on top of them.
	AddStructureBox(b, x0 - WallHalf, y0, 0.0F, x0 + WallHalf, y1, WallTop);
	AddStructureBox(b, x1 - WallHalf, y0, 0.0F, x1 + WallHalf, y1, WallTop);
	AddStructureBox(b, x0, y0 - WallHalf, 0.0F, x1, y0 + WallHalf, WallTop);
	AddStructureBox(b, x0 - CopingHalf, y0 - CopingHalf, WallTop, x0 + CopingHalf, y1 + CopingHalf, CopingTop);
	AddStructureBox(b, x1 - CopingHalf, y0 - CopingHalf, WallTop, x1 + CopingHalf, y1 + CopingHalf, CopingTop);
	AddStructureBox(b, x0 - CopingHalf, y0 - CopingHalf, WallTop, x1 + CopingHalf, y0 + CopingHalf, CopingTop);
	// The pit between the walls, and the steps down into it from the open side.
	const float px0 = x0 + WallHalf;
	const float px1 = x1 - WallHalf;
	const float py0 = y0 + WallHalf;
	const float py1 = y1 + 0.1F;
	constexpr int Steps = 6;
	constexpr float StepDrop = 0.2F;
	const float stepDepth = (py1 - py0) / static_cast<float>(Steps);
	const float bottom = -StepDrop * static_cast<float>(Steps) - 0.1F;
	const Vec2 mid { (px0 + px1) / 2.0F, (py0 + py1) / 2.0F };
	AddStructureFace(b, { { px0, py0, bottom }, { px0, py1, bottom }, { px0, py1, 0 }, { px0, py0, 0 } }, { 1, 0, 0 }, mid, MeshPit);
	AddStructureFace(b, { { px1, py0, bottom }, { px1, py1, bottom }, { px1, py1, 0 }, { px1, py0, 0 } }, { -1, 0, 0 }, mid, MeshPit);
	AddStructureFace(b, { { px0, py0, bottom }, { px1, py0, bottom }, { px1, py0, 0 }, { px0, py0, 0 } }, { 0, 1, 0 }, mid, MeshPit);
	for (int i = 0; i < Steps; i++) {
		const float front = py1 - stepDepth * static_cast<float>(i);
		const float back = front - stepDepth;
		const float top = -StepDrop * static_cast<float>(i + 1);
		const float above = -StepDrop * static_cast<float>(i);
		AddStructureFace(b, { { px0, front, top }, { px1, front, top }, { px1, front, above }, { px0, front, above } }, { 0, 1, 0 }, mid, MeshPit);
		AddStructureFace(b, { { px0, back, top }, { px1, back, top }, { px1, front, top }, { px0, front, top } }, { 0, 0, 1 }, mid, MeshPit);
	}
	FloorHoles.push_back({ px0, py0, px1, py1 });
	// Plain wall to repeat in the pit: the outer face of the +x wall (it faces the iso camera).
	st.pitSource = { x1 + WallHalf, y0 + 0.25F, 0.15F };
	st.pitSpanY = (y1 - y0) - 0.5F;
	st.pitSpanZ = 0.8F;
	for (int dy = 0; dy < 3; dy++)
		for (int dx = 0; dx < 3; dx++)
			StructureTiles[static_cast<size_t>(min.y + dy) * MAXDUNX + min.x + dx] = 2;
	return true;
}

/** @brief Fallback colour per triangle: the art at its middle, for where the art has no pixel. */
void SetStructureFallbacks(Structure &st)
{
	for (WorldTriangle &tri : st.art.mesh) {
		const V3 m { (tri.p[0].x + tri.p[1].x + tri.p[2].x) / 3.0F, (tri.p[0].y + tri.p[1].y + tri.p[2].y) / 3.0F, (tri.p[0].z + tri.p[1].z + tri.p[2].z) / 3.0F };
		int16_t color = -1;
		for (int k = 0; k < 4 && color <= 0; k++) {
			const V3 q = k < 3 ? tri.p[k] : m;
			color = SampleArt(st.art, q.x, q.y, std::max(q.z, 0.05F));
		}
		tri.fallback = color > 0 ? PatchColor(color) : uint8_t { 0 };
	}
}

void BuildStructures()
{
	Structures.clear();
	FloorHoles.clear();
	StructureTiles.assign(static_cast<size_t>(MAXDUNX) * MAXDUNY, 0);
	if (leveltype == DTYPE_TOWN)
		return;
	if (leveltype == DTYPE_CATHEDRAL) {
		for (int i = 0; i < numtrigs; i++) {
			if (trigs[i]._tmsg != WM_DIABNEXTLVL)
				continue;
			Structure st;
			if (BuildStairsDown(st, trigs[i].position)) {
				SetStructureFallbacks(st);
				Structures.push_back(std::move(st));
			}
		}
	}
	for (int y = 0; y < MAXDUNY; y++) {
		for (int x = 0; x < MAXDUNX; x++) {
			if (StructureTiles[static_cast<size_t>(y) * MAXDUNX + x] != 0 || !GetPieceTextures(dPiece[x][y]).isFreeStanding)
				continue;
			Structure st;
			if (!BuildColumn(st, { x, y }))
				continue;
			SetStructureFallbacks(st);
			StructureTiles[static_cast<size_t>(y) * MAXDUNX + x] = 1;
			Structures.push_back(std::move(st));
		}
	}
	for (Structure &st : Structures) {
		float lowest = 0;
		float highest = 0;
		float minX = 1e9F;
		float minY = 1e9F;
		float maxX = -1e9F;
		float maxY = -1e9F;
		for (const WorldTriangle &tri : st.art.mesh) {
			for (const V3 &p : tri.p) {
				lowest = std::min(lowest, p.z);
				highest = std::max(highest, p.z);
				minX = std::min(minX, p.x);
				minY = std::min(minY, p.y);
				maxX = std::max(maxX, p.x);
				maxY = std::max(maxY, p.y);
			}
		}
		st.art.x0 = minX;
		st.art.y0 = minY;
		st.art.x1 = maxX;
		st.art.y1 = maxY;
		st.art.lowestZ = lowest;
		st.art.highestZ = highest;
	}
	LogInfo("fp: {} dungeon structures drawn as meshes", Structures.size());
}

void EnsureStructures()
{
	const LevelKey key { pDungeonCels.get(), currlevel, static_cast<int>(leveltype), setlevel };
	if (key == StructuresFor)
		return;
	StructuresFor = key;
	BuildStructures();
}

/** @brief Palette colour (before lighting) of a structure's surface point. */
uint8_t StructureColor(const Structure &st, const WorldTriangle &tri, float wx, float wy, float wz)
{
	if (tri.mapping == MeshPit) {
		// Repeat a stretch of the wall art: along the surface and down, wrapped into the source stretch.
		const float along = std::abs(tri.normalX) > 0.5F ? wy : wx;
		const float u = along - std::floor(along / st.pitSpanY) * st.pitSpanY;
		const float down = tri.normalX == 0 && tri.normalY == 0 ? wy * 0.7F : -wz;
		const float v = down - std::floor(down / st.pitSpanZ) * st.pitSpanZ;
		const int16_t art = SampleArt(st.art, st.pitSource.x, st.pitSource.y + u, st.pitSource.z + v);
		return art > 0 ? PatchColor(art) : tri.fallback;
	}
	float x = wx;
	float y = wy;
	if (tri.mirrored) {
		const float d = (x - tri.mirrorX) * tri.normalX + (y - tri.mirrorY) * tri.normalY;
		x -= 2.0F * d * tri.normalX;
		y -= 2.0F * d * tri.normalY;
	}
	const int16_t art = SampleArt(st.art, x, y, wz);
	return art > 0 ? PatchColor(art) : tri.fallback;
}

void DrawStructure(const Surface &out, const Projection &proj, std::vector<float> &depthBuffer, const Structure &st)
{
	int left;
	int top;
	int right;
	int bottom;
	if (!ScreenBounds(st.art, proj, left, top, right, bottom))
		return;
	RasterizeTriangles(out, proj, depthBuffer, st.art.mesh, [&](const WorldTriangle &tri, float wx, float wy, float wz) {
		const uint8_t color = StructureColor(st, tri, wx, wy, wz);
		uint8_t darkness = tri.darkness;
		if (wz < -0.02F)
			darkness = std::max<uint8_t>(darkness, static_cast<uint8_t>(std::min(1.0F + -wz * 3.5F, static_cast<float>(SurfaceShade.size() - 1))));
		const Point tile { static_cast<int>(std::floor(wx)), static_cast<int>(std::floor(wy)) };
		const uint8_t level = InDungeonBounds(tile) ? LightAt(tile) : LightsMax;
		return static_cast<int>(LightTables[std::min<uint8_t>(level, LightsMax)][ShadeColor(color, darkness)]);
	});
}

} // namespace

#ifdef _DEBUG
std::string TileMap(int x0, int y0, int x1, int y1)
{
	// Rows are y, columns x. '#' solid with tall art, 's' solid flat, 'a' tall art only, 'B' a reconstructed
	// building's art tile, '.' open ground.
	std::string map = StrCat("x ", x0, "..", x1, ", y ", y0, "..", y1, "\n");
	for (int y = y0; y <= y1; y++) {
		map += StrCat(y < 10 ? " " : "", y, " ");
		for (int x = x0; x <= x1; x++) {
			if (!InDungeonBounds({ x, y })) {
				map += ' ';
				continue;
			}
			const bool solid = IsTileSolid({ x, y });
			const bool tall = GetPieceTextures(dPiece[x][y]).hasTallArt;
			char c = solid ? (tall ? '#' : 's') : (tall ? 'a' : '.');
			if (!BuildingTiles.empty() && BuildingTiles[static_cast<size_t>(y) * MAXDUNX + x] != 0)
				c = 'B';
			map += c;
		}
		map += '\n';
	}
	return map;
}

bool DumpIsoRegion(int x0, int y0, int x1, int y1, const std::string &path)
{
	std::vector<Point> tiles;
	for (int y = y0; y <= y1; y++)
		for (int x = x0; x <= x1; x++)
			if (InDungeonBounds({ x, y }))
				tiles.push_back({ x, y });
	if (tiles.empty())
		return false;
	Building region {};
	BuildCanvas(region, tiles, {});
	FILE *f = std::fopen(path.c_str(), "wb");
	if (f == nullptr)
		return false;
	std::fprintf(f, "P6\n%d %d\n255\n", region.canvasWidth, region.canvasHeight);
	for (const int16_t pixel : region.canvas) {
		const SDL_Color c = pixel > 0 ? logical_palette[pixel] : SDL_Color { 0, 0, 0, 255 };
		std::fputc(c.r, f);
		std::fputc(c.g, f);
		std::fputc(c.b, f);
	}
	std::fclose(f);
	return true;
}
#endif

bool BuildingsEnabled()
{
	return Enabled;
}

void SetBuildingsEnabled(bool enabled)
{
	Enabled = enabled;
}

bool HouseModelsEnabled()
{
	return HouseModelsOn;
}

void SetHouseModelsEnabled(bool enabled)
{
	HouseModelsOn = enabled;
}

void SetHouseTexturesEnabled(bool enabled)
{
	TexturedHouses = enabled;
}

void SetHouseMeshesEnabled(bool enabled)
{
	HouseMeshesOn = enabled;
}

bool IsBuildingTile(Point tile)
{
	EnsureBuilt();
	return InDungeonBounds(tile) && BuildingTiles[static_cast<size_t>(tile.y) * MAXDUNX + tile.x] != 0;
}

void DrawBuildings(const Surface &out, const Projection &proj, const std::vector<float> &zBuffer, std::vector<float> &depthBuffer)
{
	EnsureBuilt();
	for (const Building &building : Buildings) {
		int left;
		int top;
		int right;
		int bottom;
		if (!ScreenBounds(building, proj, left, top, right, bottom))
			continue;
		if (HouseModelsOn && (building.model >= 0 || building.landmark >= 0) && (HouseMeshesOn || building.landmark >= 0) && !building.mesh.empty() && building.wallPatch.valid
		    && building.roofPatch.valid)
			DrawHouseMesh(out, proj, depthBuffer, building);
		else if (HouseModelsOn && building.model >= 0)
			DrawHouseModel(out, proj, depthBuffer, building);
		else
			DrawBuilding(out, proj, zBuffer, depthBuffer, building);
	}
}

bool IsStructureTile(Point tile)
{
	if (leveltype == DTYPE_TOWN || !InDungeonBounds(tile))
		return false;
	EnsureStructures();
	return StructureTiles[static_cast<size_t>(tile.y) * MAXDUNX + tile.x] != 0;
}

bool IsFloorHole(float x, float y)
{
	if (FloorHoles.empty())
		return false;
	for (const auto &hole : FloorHoles)
		if (x >= hole[0] && x < hole[2] && y >= hole[1] && y < hole[3])
			return true;
	return false;
}

void DrawStructures(const Surface &out, const Projection &proj, std::vector<float> &depthBuffer)
{
	if (leveltype == DTYPE_TOWN)
		return;
	EnsureStructures();
	for (const Structure &st : Structures)
		DrawStructure(out, proj, depthBuffer, st);
}

int BuildingCount()
{
	EnsureBuilt();
	return static_cast<int>(Buildings.size());
}

} // namespace devilution::fp
