/**
 * @file fp/fp_buildings.cpp
 *
 * Town buildings rebuilt as a box with a gable roof, textured by projecting the model into the original iso art.
 */
#include "fp/fp_buildings.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <functional>
#include <limits>
#include <optional>
#include <tuple>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "engine/light_tables.hpp"
#include "engine/palette.h"
#include "fp/fp_doors.hpp"
#include "fp/fp_dungeon.hpp"
#include "fp/fp_hell.hpp"
#include "fp/fp_textures.hpp"
#include "fp/fp_world.hpp"
#include "levels/gendung.h"
#include "levels/tile_properties.hpp"
#include "levels/trigs.h"
#include "objects.h"
#include "tables/objdat.h"
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
	bool ridgeAlongX = false;
	float eave = 0;     // wall height
	float rise = 0;     // height of the ridge above the eave
	float overhang = 0; // how far the roof sticks out over the eaves (screen culling reads it for every structure)
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
	bool faceX;         // MeshWallFace: the x face texture (else the y face)
	bool cull = false;  // one-sided: skipped when turned away from the camera (closed shapes, slabs)
	bool inside = false; // inside a lit landmark (the church's entrance hall, through its open door): lit by its lamps
	bool rim = false;    // a side of the church's cross windows: lit by the lamp behind it (CrossLamps)
	bool slit = false;   // a gap between the planks of a wall (Adria's): glows with the light inside (GlowSlits)
	V3 normal { 0, 0, 0 }; // outward, for culling
	// Baked colours (projective landmarks): an N x N grid over the triangle's (u, v) = weights of p1 and p2.
	uint32_t bakeOffset = 0;
	uint16_t bakeN = 0;
	uint8_t bakeAxis = 0;        // the axis dropped to solve for (u, v)
	std::array<float, 4> bakeInv {}; // inverse of the 2 x 2 matrix of the edges in the other two axes
	int16_t texture = -1;            // model texture (MeshTextures), sampled at uv when model textures are on
	std::array<float, 6> uv {};
	uint8_t textureDarkness = 0;     // shading of the textured surface by the sun
	float texelDensity = 0;          // full-size texels of the model texture per world unit (for the mip level)
	V3 unitNormal { 0, 0, 0 };       // for the mip level: surfaces seen at a slant cover more texels per pixel
	float arcSpring = 0; // MeshArcade: the arch wall's springing height
	int16_t arch = -1;   // MeshInterior: index into LitRooms; MeshPane: into LitLandmarks
	int16_t artX = -1;   // town prop hulls: the tile whose own column of art they are textured from
	int16_t artY = -1;
	uint8_t fallback;   // colour where the art has no pixel
	CleanSurface cleanSurface = CleanSurface::Wall; // MeshClean: the texture, laid on by uv (tiles)
	int16_t sprite = -1;                            // MeshSprite: the cut-out (HellSpriteAt)
	bool smallLight = false; // MeshClean, MeshSprite: small enough to be lit as a whole (CleanTriangleLight)
	// MeshClean: extra shade (light levels) at a point z high, the sum of two ramps max(0, at0 + perZ * z): a
	// sarcophagus's shadow under its rim and towards its foot.
	struct Ramp {
		float at0 = 0;
		float perZ = 0;
	};
	std::array<Ramp, 2> occlusion {};
};

enum : uint8_t {
	MeshWallAlongX, // a wall running along x: u follows world x
	MeshWallAlongY,
	MeshFlat, // horizontal: u, v follow world x, y
	MeshRoof, // a roof slope: u along it, v up it
	MeshProjective, // the structure's own iso art, projected (mirrored on the far side)
	MeshPit,        // below the floor, where the art shows nothing: the structure's wall art repeated, darkening with depth
	MeshArcade,     // an arch wall between pillars: the level's plain wall pair along it, its painted arch cut open
	MeshProjectiveCut, // like MeshProjective, but see-through where the art has no pixel
	MeshWallFace,   // a wall face texture of the tile at (mirrorX, mirrorY) (faceX picks which), open where it has no art
	MeshFloorArt,   // the floor art under the point (what the art paints on the floor, raised into 3D: the stairs' fan)
	MeshSolid,      // shaded by its own draw call (the door leaves)
	MeshClean,      // the clean dungeon's pillars and arches (fp_dungeon.cpp): a surface of its texture set, by uv
	MeshInterior,   // the inside of a lit landmark's room (LitRooms[arch]), seen through its windows, lit by its lamps
	MeshTiled,      // a model texture (WorldTriangle::texture) tiled over the surface in world space (the town's low walls)
	MeshPane,       // window glass, glowing with its house's light (LitLandmarks[arch]); see AddWindowPane
	MeshFrame,      // a window's wooden frame
	MeshSprite,     // an art cut-out on a card (hell's posts: HellSpriteAt(sprite)), by uv 0..1, clear where it has no art
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
	std::vector<uint8_t> baked;      // projective landmarks: baked triangle colours (see WorldTriangle::bakeN)
	std::vector<float> isoNear;      // projective landmarks: nearest surface to the iso camera per 2 x 2 canvas pixels
	float boundsOverhang = 0;        // how far the model reaches past the footprint, for screen culling
	int landmark = -1;               // index into Landmarks for the hand-placed buildings
	std::vector<Point> claimed;      // detected houses: the art tiles they draw (BuildingTiles 1)
	std::vector<std::array<float, 6>> doors; // landmarks: their doors' bounds (x0, x1, y0, y1, z0, z1)
};

struct LevelKey {
	const void *cels;
	int level;
	int type;
	bool isSetLevel;
	uint32_t palette = 0; // textures and colour tables are built for the palette: a new one rebuilds them
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
	static uint32_t readyFor = 0;
	if (readyFor != StablePaletteVersion()) {
		readyFor = StablePaletteVersion();
		for (int i = 0; i < 256; i++) {
			safe[i] = static_cast<uint8_t>(i);
			if (i < 112 || i > 127)
				continue;
			const SDL_Color c = logical_palette[i];
			int best = std::numeric_limits<int>::max();
			for (int j = 1; j < 256; j++) {
				if ((j >= 112 && j <= 127) || IsCycledColor(j))
					continue;
				const SDL_Color o = logical_palette[j];
				const int distance = (o.r - c.r) * (o.r - c.r) + (o.g - c.g) * (o.g - c.g) + (o.b - c.b) * (o.b - c.b);
				if (distance < best) {
					best = distance;
					safe[i] = static_cast<uint8_t>(j);
				}
			}
		}
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
	static uint32_t readyFor = 0;
	if (readyFor != StablePaletteVersion()) {
		readyFor = StablePaletteVersion();
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
	}
	return table[std::min<size_t>(darkness, SurfaceShade.size() - 1)][color];
}

/** World size of a screen pixel at the point being shaded, and its screen position (set by RasterizeTriangles). */
float ShadePixelWorld = 0;
int ShadeX = 0;
int ShadeY = 0;
/** Counts DrawStructures calls: what is worked out once a frame per triangle is kept with it (CleanTriangleLight). */
uint32_t StructureFrame = 0;
/**
 * @brief Whether SampleArt may read the art's UpscaledTexture up close: not in the caves, whose art cycles its lava
 * colours (the smoothing would mix them as they stand at one moment), nor in hell.
 */
bool FineCanvas()
{
	return leveltype == DTYPE_TOWN || leveltype == DTYPE_CATHEDRAL || leveltype == DTYPE_CATACOMBS;
}

/** @brief Whether art with `texelsPerWorld` texels per tile is blown up enough at the point being shaded to use its UpscaledTexture. */
bool ShadeFine(float texelsPerWorld)
{
	return ShadePixelWorld * texelsPerWorld < UpscaleBelow;
}

/** @brief A texel of a face's UpscaledTexture (`fine`) at (u, v) in texels of the face, `width` wide. */
int16_t FineTexel(const std::vector<int16_t> &fine, int width, float u, float v)
{
	const int fineWidth = width * TextureUpscale;
	const int fineHeight = static_cast<int>(fine.size()) / fineWidth;
	const int x = std::clamp(static_cast<int>(u * TextureUpscale), 0, fineWidth - 1);
	const int y = std::clamp(static_cast<int>(v * TextureUpscale), 0, fineHeight - 1);
	return fine[static_cast<size_t>(y) * fineWidth + x];
}

/** @brief A face texel up close: from the face's UpscaledTexture at (u, v) in texels of the face, `width` wide. */
int16_t FineFaceTexel(const std::vector<int16_t> &face, int width, float u, float v)
{
	return FineTexel(UpscaledTexture(face, width), width, u, v);
}

/**
 * @brief Iso art pixel at the screen position of a world point, searching a few pixels around for opaque art. Up close,
 * from the canvas's UpscaledTexture: `fineCanvas` if given (the threads of a bake, which must not fill the cache).
 */
int16_t SampleArt(const Building &b, float wx, float wy, float wz, const std::vector<int16_t> *fineCanvas = nullptr)
{
	if (FineCanvas() && ShadeFine(32.0F) && !b.canvas.empty()) {
		const float fx = (wx - wy) * 32.0F - static_cast<float>(b.originX);
		const float fy = (wx + wy) * 16.0F - wz * HeightPixels - static_cast<float>(b.originY);
		if (fx >= 0 && fy >= 0 && fx < static_cast<float>(b.canvasWidth) && fy < static_cast<float>(b.canvasHeight)) {
			const int16_t pixel = fineCanvas != nullptr ? FineTexel(*fineCanvas, b.canvasWidth, fx, fy) : FineFaceTexel(b.canvas, b.canvasWidth, fx, fy);
			if (pixel > 0)
				return pixel;
		}
	}
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

/** @brief Runs `work(i)` for every i below `count`, spread over the processor's threads (in no particular order). */
template <typename Work>
void ParallelFor(size_t count, const Work &work)
{
	std::atomic<size_t> next { 0 };
	auto run = [&]() {
		for (size_t i = next++; i < count; i = next++)
			work(i);
	};
	const size_t threads = std::min<size_t>(std::clamp(std::thread::hardware_concurrency(), 1U, 16U), count);
	if (threads <= 1) {
		run();
		return;
	}
	std::vector<std::thread> pool;
	for (size_t t = 1; t < threads; t++)
		pool.emplace_back(run);
	run();
	for (std::thread &thread : pool)
		thread.join();
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

	std::vector<Params> tried;
	for (const bool ridgeAlongX : ridgeChoices) {
		// The roof spans the side across the ridge.
		const float span = ridgeAlongX ? depth : width;
		for (const float eaveRatio : { 0.15F, 0.22F, 0.30F, 0.38F }) {
			for (const float ridgeRatio : { 0.8F, 0.95F, 1.1F, 1.25F, 1.4F }) {
				for (const float overhangRatio : { 0.06F, 0.10F, 0.14F }) {
					const float eave = eaveRatio * span;
					const float ridge = ridgeRatio * span;
					tried.push_back({ ridgeAlongX, eave, ridge - eave, overhangRatio * span });
				}
			}
		}
	}
	std::vector<float> scores(tried.size());
	ParallelFor(tried.size(), [&](size_t i) { scores[i] = FitScore(b, tried[i]); });
	Params best {};
	float bestScore = -1.0F;
	for (size_t i = 0; i < tried.size(); i++) {
		if (scores[i] > bestScore) {
			bestScore = scores[i];
			best = tried[i];
		}
	}
	b.params = best;
	b.score = bestScore;
	b.walls = MakeWalls(b, best);
	b.roof = MakeRoof(b, best);
	b.lowestZ = 0.0F;
	b.highestZ = best.eave + best.rise;
}

bool IsTownRockPiece(uint16_t piece);
bool IsTreePiece(uint16_t piece);

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
	std::vector<uint8_t> classes;                // 0 wall, 1 roof, 2 door, 3 inside, 4 a cross window's rim
	std::vector<uint8_t> wound;                  // 1 where the winding gives the normal (the file's class bit WoundClass)
	std::vector<std::array<float, 6>> uvs;       // texture coordinates per vertex (FPL2 files)
	std::vector<int16_t> textures;               // index into MeshTextures, -1 for none
};

/** @brief A model texture (RGB, rows top to bottom) and its palette version for the current palette. */
struct MeshTexture {
	int width = 0;
	int height = 0;
	std::vector<uint8_t> rgb;
	std::vector<uint8_t> indexed;
	bool artColors = false; // the original art's colours already (Adria's shack and wares): not dimmed for dusk
	/** Smaller copies (mip levels 1, 2, ...: each half the size of the one before, averaged), for far surfaces. */
	struct Level {
		int width;
		int height;
		std::vector<uint8_t> indexed;
	};
	std::vector<Level> mips;
};
std::vector<MeshTexture> MeshTextures;
bool ModelTexturesOn = true;
/** Brightness the model textures are brought down to, to match the town art. */
constexpr float TextureDim = 0.42F;

/**
 * @brief Converts the model textures to the current palette: nearest colour by eye weights, leaving out colour 0 and
 * the near-blacks 112 - 127 (the light tables turn them red, see PatchColor).
 */
void QuantizeMeshTextures(float dim = 0.42F, bool dusk = true, std::array<float, 3> tint = { 1.0F, 1.0F, 1.0F })
{
	std::vector<int16_t> lut(32768, -1); // by the colour after darkening: shared by every texture
	// asIs: the art's colours already (MeshTexture::artColors), only matched to the palette.
	auto quantizeWith = [&](const std::vector<uint8_t> &rgb, std::vector<uint8_t> &indexed, bool asIs) {
		indexed.resize(rgb.size() / 3);
		for (size_t i = 0; i < indexed.size(); i++) {
			// The textures are daylight photographs; Tristram is drawn at dusk, dim and cool. Darken and pull the
			// colours a little towards grey-blue so the buildings sit with the original art.
			const float r0 = rgb[i * 3];
			const float g0 = rgb[i * 3 + 1];
			const float b0 = rgb[i * 3 + 2];
			const float grey = (r0 + g0 + b0) / 3.0F;
			const float sat = asIs ? 1.0F : (dusk ? 0.75F : 0.85F);
			const float scale = asIs ? 1.0F : dim;
			const std::array<float, 3> cast = asIs ? std::array<float, 3> { 1.0F, 1.0F, 1.0F }
			                                       : std::array<float, 3> { (dusk ? 0.92F : 1.0F) * tint[0], (dusk ? 0.95F : 1.0F) * tint[1], (dusk ? 1.08F : 1.0F) * tint[2] };
			const int r = std::clamp(static_cast<int>((r0 * sat + grey * (1.0F - sat)) * scale * cast[0]), 0, 255);
			const int g = std::clamp(static_cast<int>((g0 * sat + grey * (1.0F - sat)) * scale * cast[1]), 0, 255);
			const int bl = std::clamp(static_cast<int>((b0 * sat + grey * (1.0F - sat)) * scale * cast[2]), 0, 255);
			const int key = (r >> 3) << 10 | (g >> 3) << 5 | (bl >> 3);
			if (lut[key] < 0) {
				const int qr = (r & ~7) + 4;
				const int qg = (g & ~7) + 4;
				const int qb = (bl & ~7) + 4;
				int best = std::numeric_limits<int>::max();
				for (int j = 1; j < 256; j++) {
					if (j >= 112 && j <= 127)
						continue;
					const SDL_Color c = logical_palette[j];
					const int d = (c.r - qr) * (c.r - qr) * 3 + (c.g - qg) * (c.g - qg) * 6 + (c.b - qb) * (c.b - qb);
					if (d < best) {
						best = d;
						lut[key] = static_cast<int16_t>(j);
					}
				}
			}
			indexed[i] = static_cast<uint8_t>(lut[key]);
		}
	};
	for (MeshTexture &tex : MeshTextures) {
		auto quantize = [&](const std::vector<uint8_t> &rgb, std::vector<uint8_t> &indexed) { quantizeWith(rgb, indexed, tex.artColors); };
		quantize(tex.rgb, tex.indexed);
		// Mip levels: each texel the average of 2 x 2 of the level above, averaged before quantizing so far
		// surfaces show the texture's mean colours instead of whichever texel a screen pixel happens to hit.
		tex.mips.clear();
		const std::vector<uint8_t> *rgb = &tex.rgb;
		std::vector<uint8_t> above;
		std::vector<uint8_t> level;
		int w = tex.width;
		int h = tex.height;
		while (w > 1 || h > 1) {
			const int nw = std::max(1, w / 2);
			const int nh = std::max(1, h / 2);
			level.assign(static_cast<size_t>(nw) * nh * 3, 0);
			for (int y = 0; y < nh; y++) {
				for (int x = 0; x < nw; x++) {
					for (int c = 0; c < 3; c++) {
						int sum = 0;
						for (int dy = 0; dy < 2; dy++)
							for (int dx = 0; dx < 2; dx++)
								sum += (*rgb)[(static_cast<size_t>(std::min(y * 2 + dy, h - 1)) * w + std::min(x * 2 + dx, w - 1)) * 3 + c];
						level[(static_cast<size_t>(y) * nw + x) * 3 + c] = static_cast<uint8_t>((sum + 2) / 4);
					}
				}
			}
			MeshTexture::Level &mip = tex.mips.emplace_back(MeshTexture::Level { nw, nh, {} });
			quantize(level, mip.indexed);
			above.swap(level);
			rgb = &above;
			w = nw;
			h = nh;
		}
	}
}

/**
 * @brief Texel of a model texture at (u, v), from the mip level that fits a screen pixel covering `footprint` texels
 * of the full-size texture (1 or less: the full-size texture).
 */
uint8_t MeshTexel(const MeshTexture &tex, float u, float v, float footprint)
{
	const int level = footprint >= 2.0F ? std::min(std::ilogb(footprint), static_cast<int>(tex.mips.size())) : 0;
	const int width = level == 0 ? tex.width : tex.mips[level - 1].width;
	const int height = level == 0 ? tex.height : tex.mips[level - 1].height;
	const std::vector<uint8_t> &indexed = level == 0 ? tex.indexed : tex.mips[level - 1].indexed;
	const int x = static_cast<int>(std::floor((u - std::floor(u)) * static_cast<float>(width))) % width;
	const int y = static_cast<int>(std::floor((std::ceil(v) - v) * static_cast<float>(height))) % height;
	return indexed[static_cast<size_t>(y) * width + x];
}

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
	bool projective = false;   // textured by projecting its own iso art (its model was fitted to it), not with patches
	bool modelTextures = true; // may wear the model's own textures (dev.fp.modeltextures); else always its art
};

const std::array<LandmarkPlacement, 13> Landmarks { {
	// The buildings of the Tristram plaza model (Nexus Mods 67266), each fitted to the town's iso art by silhouette
	// overlap (fp-tools/export/fit_landmark.py; the plaza is laid out like the town, at its scale, y flipped) and
	// textured by projecting that art back on. The comment gives each fit's overlap with the art.
	{ "cottage_long", { 45, 42 }, { 55, 46 }, 45.64F, 55.34F, 42.09F, 46.45F, 0.931F, 100.0F, 0.931F, true, { Vec2 { 0, 0 }, Vec2 { 0, 0 } }, true },  // 0.77
	{ "farnham", { 67, 78 }, { 73, 82 }, 67.59F, 73.30F, 78.04F, 82.41F, 0.919F, 100.0F, 0.919F, true, { Vec2 { 0, 0 }, Vec2 { 0, 0 } }, true },       // 0.65
	{ "gillian", { 37, 64 }, { 43, 68 }, 37.64F, 43.34F, 64.09F, 68.45F, 0.950F, 100.0F, 0.950F, true, { Vec2 { 0, 0 }, Vec2 { 0, 0 } }, true },       // 0.63
	// Ogden's tavern: two front gables with the door in the corner between them, where Ogden stands.
	{ "ogden", { 48, 56 }, { 56, 64 }, 48.95F, 56.60F, 56.61F, 64.50F, 0.934F, 100.0F, 0.934F, true, { Vec2 { 0, 0 }, Vec2 { 0, 0 } }, true },         // 0.70
	{ "pepin", { 48, 74 }, { 55, 81 }, 48.31F, 55.01F, 74.38F, 81.02F, 0.687F, 100.0F, 0.687F, true, { Vec2 { 0, 0 }, Vec2 { 0, 0 } }, true },         // 0.66
	{ "cottage_small", { 25, 48 }, { 30, 53 }, 25.78F, 30.90F, 48.48F, 53.39F, 0.875F, 100.0F, 0.875F, true, { Vec2 { 0, 0 }, Vec2 { 0, 0 } }, true }, // 0.65
	{ "cottage_tall", { 69, 66 }, { 74, 73 }, 69.74F, 74.86F, 66.35F, 73.25F, 0.881F, 100.0F, 0.881F, true, { Vec2 { 0, 0 }, Vec2 { 0, 0 } }, true },  // 0.66
	// Griswold's smithy: the long hall, and over the forge where he stands an open porch roof on the model's own two
	// posts. The plaza porch eaves are low; they are lifted to the hall's wall height, high enough to stand under.
	{ "smithy", { 60, 56 }, { 70, 64 }, 60.17F, 70.87F, 56.78F, 64.08F, 1.59F, 1.0F, 0.60F, true, { Vec2 { 0, 0 }, Vec2 { 0, 0 } }, true }, // 0.59
	// The fountain wears its parts' colours from the original art (its art projected on it came out a patchwork: the
	// water's amber on its walls) and the art's amber water in its basin (export_landmarks.py, fountain_triangles).
	{ "fountain", { 59, 69 }, { 61, 71 }, 59.65F, 61.46F, 69.75F, 71.53F, 0.643F, 100.0F, 0.643F, true, { Vec2 { 0, 0 }, Vec2 { 0, 0 } }, true }, // 0.52
	// Tristram's church (research/church): tower, nave and round apse, fitted to the iso art by silhouette overlap.
	{ "church", { 19, 14 }, { 31, 29 }, 19.78F, 32.22F, 14.31F, 30.19F, 0.85F, 100.0F, 0.85F, true, { Vec2 { 0, 0 }, Vec2 { 0, 0 } }, true },
	// The mausoleum by the cows: not in Nexus 67266, built by export_landmarks.py after the art (mausoleum_triangles),
	// in tiles of its platform (x 48-50, y 18-21) and art heights.
	{ "mausoleum", { 48, 18 }, { 50, 21 }, 48.0F, 51.0F, 18.0F, 22.0F, 1.0F, 100.0F, 1.0F, false, { Vec2 { 0, 0 }, Vec2 { 0, 0 } }, true },
	// Adria's shack (research/adria): planks, a roof sloping down to the back, her wares on shelves beside the door.
	// Fitted to its art by eye (the roof's corners and its slope): the model at 0.94 of the plaza's scale. Her cauldron
	// on its frame, from the same file, fits the art where it is (fit_landmark.py, at the plaza's scale), a tile from
	// where the shack's placement would put it.
	{ "adria", { 74, 16 }, { 79, 21 }, 74.519F, 79.041F, 16.571F, 20.951F, 0.875F, 100.0F, 0.875F, true, { Vec2 { 0, 0 }, Vec2 { 0, 0 } }, true },
	{ "cauldron", { 81, 18 }, { 83, 19 }, 81.537F, 83.367F, 18.464F, 19.434F, 0.956F, 100.0F, 0.956F, true, { Vec2 { 0, 0 }, Vec2 { 0, 0 } }, true }, // 0.51
} };

std::vector<LandmarkMesh> LandmarkMeshes;
bool LandmarksLoaded = false;

/** The top bit of a triangle's class in a mesh file: its texture is in the original art's colours already. */
constexpr uint8_t ArtColorsClass = 0x80;
/** The next bit: its winding is reliable (counter-clockwise seen from outside), the game takes its normal from it. */
constexpr uint8_t WoundClass = 0x40;

/** @brief Reads a mesh file in the landmarks.bin format (see research/tristram_plaza/export_landmarks.py). */
void LoadMeshFile(const char *envName, const char *fileName, std::vector<LandmarkMesh> &meshes)
{
	FILE *f = OpenHouseAsset(envName, fileName);
	if (f == nullptr) {
		LogInfo("fp: {} not found", fileName);
		return;
	}
	auto readAll = [&](void *dst, size_t size) { return std::fread(dst, 1, size, f) == size; };
	char magic[4];
	uint32_t count = 0;
	if (!readAll(magic, 4) || (std::memcmp(magic, "FPL1", 4) != 0 && std::memcmp(magic, "FPL2", 4) != 0) || !readAll(&count, 4)) {
		std::fclose(f);
		return;
	}
	const bool textured = magic[3] == '2';
	int textureBase = static_cast<int>(MeshTextures.size());
	if (textured) {
		// The texture table comes first; then the model count.
		for (uint32_t i = 0; i < count; i++) {
			uint16_t size[2];
			MeshTexture tex;
			if (!readAll(size, sizeof(size)))
				break;
			tex.width = size[0];
			tex.height = size[1];
			tex.rgb.resize(static_cast<size_t>(tex.width) * tex.height * 3);
			if (!readAll(tex.rgb.data(), tex.rgb.size()))
				break;
			MeshTextures.push_back(std::move(tex));
		}
		if (!readAll(&count, 4)) {
			std::fclose(f);
			return;
		}
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
		mesh.wound.resize(triangleCount);
		if (textured) {
			mesh.uvs.resize(triangleCount);
			mesh.textures.resize(triangleCount);
		}
		bool ok = true;
		for (uint32_t t = 0; t < triangleCount && ok; t++) {
			ok = readAll(mesh.triangles[t].data(), sizeof(float) * 9);
			if (ok && textured) {
				int16_t texture;
				ok = readAll(mesh.uvs[t].data(), sizeof(float) * 6) && readAll(&mesh.classes[t], 1) && readAll(&texture, 2);
				mesh.textures[t] = texture < 0 ? int16_t { -1 } : static_cast<int16_t>(texture + textureBase);
				// The class's top bit: its texture is in the original art's colours already (ArtColorsClass).
				if (ok && (mesh.classes[t] & ArtColorsClass) != 0) {
					mesh.classes[t] &= ~ArtColorsClass;
					if (mesh.textures[t] >= 0 && static_cast<size_t>(mesh.textures[t]) < MeshTextures.size())
						MeshTextures[mesh.textures[t]].artColors = true;
				}
			} else if (ok) {
				ok = readAll(&mesh.classes[t], 1);
			}
			// Its winding is reliable (counter-clockwise seen from outside): the normal comes from it.
			mesh.wound[t] = (mesh.classes[t] & WoundClass) != 0 ? 1 : 0;
			mesh.classes[t] &= ~WoundClass;
		}
		if (!ok)
			break;
		meshes.push_back(std::move(mesh));
	}
	std::fclose(f);
	LogInfo("fp: loaded {} meshes from {}", meshes.size(), fileName);
}

void LoadLandmarks()
{
	if (LandmarksLoaded)
		return;
	LandmarksLoaded = true;
	LoadMeshFile("FP_LANDMARKS", "landmarks.bin", LandmarkMeshes);
}

/** @brief Pieces of the dungeon kit (research/dungeon_pieces/export_kit.py), or null if missing. */
const LandmarkMesh *KitMesh(std::string_view name)
{
	static std::vector<LandmarkMesh> kit;
	static bool loaded = false;
	if (!loaded) {
		loaded = true;
		LoadMeshFile("FP_DUNGEONKIT", "dungeonkit.bin", kit);
	}
	for (const LandmarkMesh &mesh : kit)
		if (mesh.name == name)
			return &mesh;
	return nullptr;
}

bool SarcophagusKitOn = true;
/** Brightness the dungeon kit's textures are brought to (the dungeon's light tables darken them further). */
constexpr float KitDim = 0.45F;

/**
 * @brief Converts the kit's textures (the door leaves' wood outside the clean dungeon) to the level's palette, tinted
 * by the level's own wall colour (warm in the cathedral, cold in the catacombs, ...).
 */
void PrepareKitTextures()
{
	if (leveltype == DTYPE_TOWN || IsCleanDungeon())
		return; // the clean dungeon's doors wear its own texture set
	KitMesh("door"); // loads the kit, and its textures with it
	std::array<double, 3> artSum {};
	int artCount = 0;
	for (const int16_t t : GetFallbackFace()) {
		if (t <= 0)
			continue;
		const SDL_Color c = logical_palette[t];
		artSum[0] += c.r;
		artSum[1] += c.g;
		artSum[2] += c.b;
		artCount++;
	}
	std::array<float, 3> tint { 1.0F, 1.0F, 1.0F };
	if (artCount > 0) {
		const double grey = (artSum[0] + artSum[1] + artSum[2]) / 3.0;
		for (int i = 0; i < 3; i++)
			tint[i] = static_cast<float>(std::clamp(artSum[i] / std::max(grey, 1.0), 0.6, 1.5));
	}
	QuantizeMeshTextures(KitDim, false, tint);
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

void AddMeshTriangle(Building &b, const std::array<V3, 3> &p, bool roof, V3 middle, bool wound = false);

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
void AddMeshTriangle(Building &b, const std::array<V3, 3> &p, bool roof, V3 middle, bool wound)
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
		// The kit's winding is not reliable: orient the normal away from the middle of the house (unless it is: the
		// models built for the mod, whose recesses and roof undersides face in towards the middle).
		const V3 centroid { (tri.p[0].x + tri.p[1].x + tri.p[2].x) / 3.0F, (tri.p[0].y + tri.p[1].y + tri.p[2].y) / 3.0F, (tri.p[0].z + tri.p[1].z + tri.p[2].z) / 3.0F };
		if (!wound && n.x * (centroid.x - cx) + n.y * (centroid.y - cy) + n.z * (centroid.z - middle.z) < 0)
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
		tri.normal = n;
		b.mesh.push_back(tri);
	}
}

/** Nearness to the iso camera: larger is nearer (the iso rays run along -(1, 1, 32 / HeightPixels)). */
float IsoNearness(float x, float y, float z)
{
	return x + y + z * (32.0F / HeightPixels);
}

/**
 * @brief Switches a landmark to being textured with its own iso art, and records which surface the iso camera saw
 * at each canvas pixel, so surfaces it did not see (hidden behind other parts) do not take another part's pixels.
 */
void MakeProjective(Building &b, bool mirroredPlacement)
{
	const float cx = (b.x0 + b.x1) / 2.0F;
	const float cy = (b.y0 + b.y1) / 2.0F;
	for (WorldTriangle &tri : b.mesh) {
		tri.mapping = MeshProjective;
		// The model's own winding is outward (the reoriented normal can be wrong on a complex shape): one-sided.
		const V3 e1 { tri.p[1].x - tri.p[0].x, tri.p[1].y - tri.p[0].y, tri.p[1].z - tri.p[0].z };
		const V3 e2 { tri.p[2].x - tri.p[0].x, tri.p[2].y - tri.p[0].y, tri.p[2].z - tri.p[0].z };
		const V3 wound { e1.y * e2.z - e1.z * e2.y, e1.z * e2.x - e1.x * e2.z, e1.x * e2.y - e1.y * e2.x };
		// Placing the model with one axis flipped turns its winding inside out. One-sided by the model's own winding:
		// inner surfaces (the church's inner roof and walls) face inwards and are skipped from outside.
		tri.normal = mirroredPlacement ? V3 { -wound.x, -wound.y, -wound.z } : wound;
		tri.cull = std::getenv("FP_TWO_SIDED") == nullptr;
		{
			// Sun from high up on the iso camera's side, like the art's light.
			const float length = std::sqrt(tri.normal.x * tri.normal.x + tri.normal.y * tri.normal.y + tri.normal.z * tri.normal.z);
			const float lit = length > 0 ? (0.35F * tri.normal.x + 0.45F * tri.normal.y + 0.82F * tri.normal.z) / length : 0.0F;
			tri.textureDarkness = static_cast<uint8_t>(std::clamp(static_cast<int>(std::lround((0.85F - lit) * 2.0F)), 0, 2));
		}
		const float horizontal = std::hypot(tri.normal.x, tri.normal.y);
		tri.normalX = horizontal > 0.25F ? tri.normal.x / horizontal : 0.0F;
		tri.normalY = horizontal > 0.25F ? tri.normal.y / horizontal : 0.0F;
		tri.mirrored = horizontal > 0.25F && tri.normalX + tri.normalY < -0.05F;
		tri.mirrorX = cx;
		tri.mirrorY = cy;
		// The art carries its own light; only undersides and the sides it never saw get darker.
		tri.darkness = tri.normal.z < -0.3F ? 3 : (tri.mirrored ? 1 : 0);
	}
	const int w = (b.canvasWidth + 1) / 2;
	const int h = (b.canvasHeight + 1) / 2;
	b.isoNear.assign(static_cast<size_t>(w) * h, -1e9F);
	for (const WorldTriangle &tri : b.mesh) {
		std::array<float, 3> sx;
		std::array<float, 3> sy;
		std::array<float, 3> n;
		for (int k = 0; k < 3; k++) {
			const V3 &p = tri.p[k];
			sx[k] = ((p.x - p.y) * 32.0F - static_cast<float>(b.originX)) / 2.0F;
			sy[k] = ((p.x + p.y) * 16.0F - p.z * HeightPixels - static_cast<float>(b.originY)) / 2.0F;
			n[k] = IsoNearness(p.x, p.y, p.z);
		}
		const float area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sy[1] - sy[0]) * (sx[2] - sx[0]);
		if (std::abs(area) < 1e-6F)
			continue;
		const int left = std::max(0, static_cast<int>(std::floor(std::min({ sx[0], sx[1], sx[2] }))));
		const int right = std::min(w - 1, static_cast<int>(std::ceil(std::max({ sx[0], sx[1], sx[2] }))));
		const int top = std::max(0, static_cast<int>(std::floor(std::min({ sy[0], sy[1], sy[2] }))));
		const int bottom = std::min(h - 1, static_cast<int>(std::ceil(std::max({ sy[0], sy[1], sy[2] }))));
		for (int y = top; y <= bottom; y++) {
			for (int x = left; x <= right; x++) {
				const float px = static_cast<float>(x) + 0.5F;
				const float py = static_cast<float>(y) + 0.5F;
				const float w0 = ((sx[1] - px) * (sy[2] - py) - (sy[1] - py) * (sx[2] - px)) / area;
				const float w1 = ((sx[2] - px) * (sy[0] - py) - (sy[2] - py) * (sx[0] - px)) / area;
				const float w2 = 1.0F - w0 - w1;
				if (w0 < 0 || w1 < 0 || w2 < 0)
					continue;
				float &near = b.isoNear[static_cast<size_t>(y) * w + x];
				near = std::max(near, w0 * n[0] + w1 * n[1] + w2 * n[2]);
			}
		}
	}
}


/**
 * @brief Whether most of a tile's upright art shows in the original iso picture: not drawn over by the building's
 * art tiles drawn after it (those in front of it). A boulder beside a building the original shows stays a prop of
 * its own; one the building's art hides (behind the smithy) stays hidden.
 */
bool ShowsInArt(Point t, const std::vector<Point> &artTiles)
{
	auto drawnAfter = [&](Point u) { return u.x + u.y != t.x + t.y ? u.x + u.y > t.x + t.y : u.x > t.x; };
	const PieceTextures &textures = GetPieceTextures(dPiece[t.x][t.y]);
	const int rows = static_cast<int>(textures.upright.size() / ColumnWidth);
	const int left = (t.x - t.y) * 32 - 32;
	const int top = (t.x + t.y + 2) * 16 - (rows - 1);
	int covered = 0;
	int total = 0;
	for (int cy = 0; cy < rows; cy++) {
		for (int cx = 0; cx < ColumnWidth; cx++) {
			if (textures.upright[static_cast<size_t>(cy) * ColumnWidth + cx] <= 0)
				continue;
			total++;
			const int sx = left + cx;
			const int sy = top + cy;
			for (const Point u : artTiles) {
				if (u == t || !drawnAfter(u) || IsTownRockPiece(dPiece[u.x][u.y]))
					continue;
				const PieceTextures &other = GetPieceTextures(dPiece[u.x][u.y]);
				const int otherRows = static_cast<int>(other.column.size() / ColumnWidth);
				const int ox = sx - ((u.x - u.y) * 32 - 32);
				const int oy = sy - ((u.x + u.y + 2) * 16 - (otherRows - 1));
				if (ox >= 0 && oy >= 0 && ox < ColumnWidth && oy < otherRows && other.column[static_cast<size_t>(oy) * ColumnWidth + ox] > 0) {
					covered++;
					break;
				}
			}
		}
	}
	return total > 0 && covered * 2 < total;
}

/**
 * @brief Marks the tiles a projective landmark replaces: those under the model (2: their ground art, which shows
 * the building, gives way to plain ground) and art tiles around it whose picture is mostly the building (1). Trees,
 * graves and walls next to it keep their billboards.
 */
void ClaimProjectiveTiles(const Building &b, const std::vector<Point> &artTiles)
{
	// Footprint: tiles whose middle lies under some triangle of the model.
	std::vector<uint8_t> under(static_cast<size_t>(MAXDUNX) * MAXDUNY, 0);
	for (const WorldTriangle &tri : b.mesh) {
		const float minX = std::min({ tri.p[0].x, tri.p[1].x, tri.p[2].x });
		const float maxX = std::max({ tri.p[0].x, tri.p[1].x, tri.p[2].x });
		const float minY = std::min({ tri.p[0].y, tri.p[1].y, tri.p[2].y });
		const float maxY = std::max({ tri.p[0].y, tri.p[1].y, tri.p[2].y });
		const float area = (tri.p[1].x - tri.p[0].x) * (tri.p[2].y - tri.p[0].y) - (tri.p[1].y - tri.p[0].y) * (tri.p[2].x - tri.p[0].x);
		if (std::abs(area) < 1e-6F)
			continue;
		for (int y = static_cast<int>(std::floor(minY)); y <= static_cast<int>(std::floor(maxY)); y++) {
			for (int x = static_cast<int>(std::floor(minX)); x <= static_cast<int>(std::floor(maxX)); x++) {
				if (!InDungeonBounds({ x, y }))
					continue;
				const float px = static_cast<float>(x) + 0.5F;
				const float py = static_cast<float>(y) + 0.5F;
				const float w1 = ((px - tri.p[0].x) * (tri.p[2].y - tri.p[0].y) - (py - tri.p[0].y) * (tri.p[2].x - tri.p[0].x)) / area;
				const float w2 = ((tri.p[1].x - tri.p[0].x) * (py - tri.p[0].y) - (tri.p[1].y - tri.p[0].y) * (px - tri.p[0].x)) / area;
				if (w1 >= 0 && w2 >= 0 && w1 + w2 <= 1)
					under[static_cast<size_t>(y) * MAXDUNX + x] = 1;
			}
		}
	}
	for (size_t i = 0; i < under.size(); i++)
		if (under[i] != 0)
			BuildingTiles[i] = 2;
	// Art tiles outside it: hidden when a good part (35%) of their upright art lies on the model's iso silhouette.
	const int w = (b.canvasWidth + 1) / 2;
	for (const Point t : artTiles) {
		const size_t at = static_cast<size_t>(t.y) * MAXDUNX + t.x;
		if (under[at] != 0)
			continue;
		if (IsTownRockPiece(dPiece[t.x][t.y]) && ShowsInArt(t, artTiles))
			continue; // a boulder beside the building, not part of it: a prop of its own
		const PieceTextures &textures = GetPieceTextures(dPiece[t.x][t.y]);
		const int rows = static_cast<int>(textures.column.size() / ColumnWidth);
		const int left = (t.x - t.y) * 32 - 32 - b.originX;
		const int top = (t.x + t.y + 2) * 16 - (rows - 1) - b.originY;
		int inside = 0;
		int total = 0;
		for (int cy = 0; cy < rows - 32; cy++) {
			for (int cx = 0; cx < ColumnWidth; cx++) {
				if (textures.column[static_cast<size_t>(cy) * ColumnWidth + cx] <= 0)
					continue;
				total++;
				const int px = (left + cx) / 2;
				const int py = (top + cy) / 2;
				if (px >= 0 && py >= 0 && px < w && py < (b.canvasHeight + 1) / 2 && b.isoNear[static_cast<size_t>(py) * w + px] > -1e8F)
					inside++;
			}
		}
		// Right next to the footprint, art is the building's own edge (an apse roof drawn on the tile in front)
		// unless very little of it is on the model.
		bool besideFootprint = false;
		for (int dy = -1; dy <= 1 && !besideFootprint; dy++)
			for (int dx = -1; dx <= 1 && !besideFootprint; dx++)
				besideFootprint = InDungeonBounds({ t.x + dx, t.y + dy }) && under[static_cast<size_t>(t.y + dy) * MAXDUNX + t.x + dx] != 0;
#ifdef _DEBUG
		if (std::getenv("FP_DUMP_PROFILES") != nullptr)
			LogInfo("fp: claim {},{}: {} of {} art pixels on the model, beside {}", t.x, t.y, inside, total, besideFootprint);
#endif
		// Low pieces (the fountain) have almost no art above their floor rows: beside the model they are its own.
		if ((total > 0 && inside * 100 >= total * (besideFootprint ? 12 : 35)) || (besideFootprint && total < 120))
			BuildingTiles[at] = 1;
	}
}

void BakeColors(Building &b);

/** @brief Whether the iso camera saw a landmark's surface at a point (nothing of the landmark in front of it). */
bool IsoVisible(const Building &b, float x, float y, float z)
{
	if (b.isoNear.empty())
		return true;
	const int w = (b.canvasWidth + 1) / 2;
	const int px = static_cast<int>(std::floor(((x - y) * 32.0F - static_cast<float>(b.originX)) / 2.0F));
	const int py = static_cast<int>(std::floor(((x + y) * 16.0F - z * HeightPixels - static_cast<float>(b.originY)) / 2.0F));
	if (px < 0 || py < 0 || px >= w || py >= (b.canvasHeight + 1) / 2)
		return false;
	return IsoNearness(x, y, z) >= b.isoNear[static_cast<size_t>(py) * w + px] - 0.25F;
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
		const size_t before = b.mesh.size();
		AddMeshTriangle(b, p, mesh.classes[t] == 1, middle, !mesh.wound.empty() && mesh.wound[t] != 0);
		if (b.mesh.size() > before) {
			b.mesh.back().inside = mesh.classes[t] == 3;
			b.mesh.back().rim = mesh.classes[t] == 4;
			b.mesh.back().slit = mesh.classes[t] == 5;
		}
		if (mesh.classes[t] == 2) {
			// A door: its bounds, grown by each of its triangles (the leaf and its parts are one door where they touch).
			std::array<float, 6> box { std::min({ p[0].x, p[1].x, p[2].x }), std::max({ p[0].x, p[1].x, p[2].x }), std::min({ p[0].y, p[1].y, p[2].y }),
				std::max({ p[0].y, p[1].y, p[2].y }), std::min({ p[0].z, p[1].z, p[2].z }), std::max({ p[0].z, p[1].z, p[2].z }) };
			bool merged = false;
			for (std::array<float, 6> &door : b.doors) {
				if (box[0] <= door[1] + 0.1F && door[0] <= box[1] + 0.1F && box[2] <= door[3] + 0.1F && door[2] <= box[3] + 0.1F) {
					door = { std::min(door[0], box[0]), std::max(door[1], box[1]), std::min(door[2], box[2]), std::max(door[3], box[3]), std::min(door[4], box[4]), std::max(door[5], box[5]) };
					merged = true;
					break;
				}
			}
			if (!merged)
				b.doors.push_back(box);
		}
		if (b.mesh.size() > before && !mesh.textures.empty() && place.modelTextures) {
			b.mesh.back().texture = mesh.textures[t];
			b.mesh.back().uv = mesh.uvs[t];
		}
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

void AddStructureFace(Building &b, std::initializer_list<V3> corners, V3 normal, Vec2 mirror, uint8_t mapping, uint8_t darkness);

// ---------------------------------------------------------------------------------------------------------------
// Light from inside the tavern, the smithy and the church. Lamps hang under each one's ceiling, and their light
// leaves the building only where its model is open (windows and doorways; in the church through the stained glass,
// dimmed), so it falls out of the windows in patches on the ground and on the walls across from them, and never on
// the building's own walls or roof. Seen through a window, the room inside is lit by the same lamps.

/**
 * @brief The landmarks with light inside: which of Landmarks, and its rooms as boxes a little inside its walls (in
 * the model's own frame: u, w 0..1 over its extent, z model units above its foot, as landmarks.bin has them; the
 * walls and the roof are found from the model). The church has no room of its own: its stained glass box is the room.
 */
struct LitLandmark {
	const char *name;
	int rooms;
	std::array<std::array<float, 6>, 2> room; // u0, u1, w0, w1, z0, z1
	float strength;                           // of each of its lamps
	std::array<float, 3> color;               // of its light: fire and candles, the church's through red glass
	bool glass = false;                       // also lit inside its stained glass box (the church's nave)
	bool autoRoom = false;                    // its room fitted to its walls from inside (AddAutoRoom), not measured
	bool unglazed = false;                    // its openings stay open, no glass in them (the gaps of Adria's planks)
};
constexpr std::array<LitLandmark, 10> LitLandmarks { {
	// The tavern's two wings (the plaza model's x 73.8 - 79.9, y 250.2 - 254.6 and x 73.8 - 77.1, y 248.2 - 250.3).
	{ "ogden", 2, { { { 0.0803F, 0.8780F, 0.3438F, 0.9012F, 0.088F, 5.588F }, { 0.0803F, 0.5118F, 0.0904F, 0.3564F, 0.088F, 5.588F } } }, 1.0F, { 255.0F, 112.0F, 40.0F } },
	// The smithy's hall (x 86.6 - 96.0, y 251.35 - 254.5), not the open porch.
	{ "smithy", 1, { { { 0.0595F, 0.9381F, 0.4966F, 0.9277F, 0.102F, 3.602F }, {} } }, 0.9F, { 255.0F, 100.0F, 32.0F } },
	// The church: its nave inside the stained glass box, and its tower (x 24.2 - 26.9, y 23.4 - 25.9, from above the
	// nave's roof up to under its spire: its openings are at 7.3 - 8, 10.3 - 11 and 12.3 - 13.3 tiles up).
	{ "church", 1, { { { 0.36F, 0.56F, 0.27F, 0.43F, 7.06F, 15.76F }, {} } }, 1.45F, { 255.0F, 56.0F, 22.0F }, true },
	// The houses: a hearth and a candle or two, dimmer than the tavern.
	{ "cottage_long", 0, {}, 0.75F, { 255.0F, 118.0F, 44.0F }, false, true },
	{ "farnham", 0, {}, 0.75F, { 255.0F, 118.0F, 44.0F }, false, true },
	{ "gillian", 0, {}, 0.75F, { 255.0F, 118.0F, 44.0F }, false, true },
	{ "pepin", 0, {}, 0.75F, { 255.0F, 118.0F, 44.0F }, false, true },
	{ "cottage_small", 0, {}, 0.75F, { 255.0F, 118.0F, 44.0F }, false, true },
	{ "cottage_tall", 0, {}, 0.75F, { 255.0F, 118.0F, 44.0F }, false, true },
	// Adria's shack, inside its planks (x 100.2 - 104.18, y 290.92 - 294.9 of the model): the art's light in its
	// doorway and the gaps between its planks is a warm yellow. No glass: its openings are gaps and slits.
	{ "adria", 1, { { { 0.0882F, 0.9156F, 0.0790F, 0.9330F, 0.02F, 2.7F }, {} } }, 0.8F, { 255.0F, 150.0F, 50.0F }, false, false, true },
} };

/** The lamps' light outside, falling from the windows, per unit of strength. */
constexpr float LampPower = 6.5F;
/** How far a lamp's light reaches (tiles), and the distance at which it is down to half. */
constexpr float LampReach = 7.0F;
constexpr float LampHalf = 3.0F;
/** How far below the ceiling a lamp hangs, and the radius of its flame (its shadows' soft edges). */
constexpr float LampDrop = 0.35F;
constexpr float LampRadius = 0.12F;
/** How much of the light the church's stained glass lets through, and a house's window glass. */
constexpr float GlassTransmission = 0.45F;
constexpr float WindowGlassTransmission = 0.8F;
/** How much the lamps flicker (share of their light), and the glass's panes with them. */
constexpr float LampFlicker = 0.07F;
constexpr float PaneFlicker = 0.06F;
constexpr int PaneLevels = 9;
/** Strengths of light falling on a colour (GlowColors). */
constexpr int GlowLevels = 24;

/** @brief A lamp inside a lit landmark. */
struct Lamp {
	V3 position;
	float strength;
	int lit;  // which of LitLandmarks: its flicker
	int room; // which of LitRooms it hangs in; -1 for a window's own light
	V3 facing { 0, 0, 0 }; // a window's light shines out this way only (Lambertian); zero: all around
	float half = LampHalf;   // the distance at which its light is down to half
	float reach = LampReach; // and where it ends
};

/**
 * @brief The room inside a lit landmark: the space between its walls, up to under its roof (a ceiling height per
 * cell, so the room follows the roof's slope). Drawn as seen through the windows, but for the church's (its glass).
 */
struct LitRoom {
	int lit = 0;
	float x0 = 0;
	float x1 = 0;
	float y0 = 0;
	float y1 = 0;
	float z0 = 0;
	float top = 0; // its highest ceiling
	int nx = 0;
	int ny = 0;
	std::vector<float> ceiling; // [y][x]
};
constexpr float RoomCell = 0.25F;

/**
 * @brief The light of a building's lamps around it, on a grid: at each point, the sum over the lamps seen from there
 * of their light's strength times the direction towards them. A surface takes its share by its normal (N . E): all of
 * a building's lamps are on the same side of anything outside it, so this is their light, summed.
 */
struct LightGrid {
	int lit = 0;
	float x0 = 0;
	float y0 = 0;
	int nx = 0;
	int ny = 0;
	int nz = 0;
	std::vector<std::array<int16_t, 3>> light; // [z][y][x], LightUnit per unit of light
	std::vector<uint8_t> lit2d;                 // [y][x]: some light in the column of cells from (x, y) to (x + 1, y + 1)
};
constexpr float LightUnit = 2048.0F;
/** The highest lamp a light grid reaches up to (tiles; the church tower's go higher). */
constexpr float LightTop = 5.0F;
constexpr float LampCell = 0.125F;
constexpr float LampLayer = 0.25F;

std::vector<Lamp> Lamps;
std::vector<Lamp> CrossLamps; // behind the church's cross windows (in Lamps too): what lights the windows' sides
std::vector<LitRoom> LitRooms;
std::vector<LightGrid> LightGrids;
/**
 * The light grids are worked out on a thread of their own while the town is already shown (it takes a good part of a
 * second); LightGrids is the thread's until LightReady.
 */
struct LightThread {
	std::thread thread;
	std::atomic<bool> stop { false }; // asked to give up (the town is left, or the game quits)
	~LightThread()
	{
		Join();
	}
	void Join()
	{
		stop = true;
		if (thread.joinable())
			thread.join();
		stop = false;
	}
};
LightThread LightBuilder;
std::atomic<bool> LightReady { false };
int GlassTexture = -1; // the church's stained glass (MeshTextures), lit from inside
bool WindowLightOn = true;

/** @brief The flicker of a building's light at a time (seconds), smooth, in -1..1; each building has its own. */
float WindowFlicker(int lit, float t)
{
	constexpr float Tau = 6.2831853F;
	const float k = 1.0F + 0.13F * static_cast<float>(lit);
	const float p = 1.7F * static_cast<float>(lit);
	return 0.5F * std::sin(t * Tau * 0.47F * k + p) + 0.3F * std::sin(t * Tau * 1.13F * k + 2.1F * p + 1.0F) + 0.2F * std::sin(t * Tau * 2.29F * k + 0.7F * p + 2.0F);
}

float FlickerNow[LitLandmarks.size()] {};

/** @brief The nearest palette colour to a colour (leaving out the cycling colours, see PatchColor). */
uint8_t NearestColor(float r, float g, float b)
{
	float best = std::numeric_limits<float>::max();
	uint8_t found = 0;
	for (int j = 1; j < 256; j++) {
		if (j >= 112 && j <= 127)
			continue;
		const SDL_Color o = logical_palette[j];
		const float d = (o.r - r) * (o.r - r) * 3 + (o.g - g) * (o.g - g) * 6 + (o.b - b) * (o.b - b);
		if (d < best) {
			best = d;
			found = static_cast<uint8_t>(j);
		}
	}
	return found;
}

/**
 * @brief The nearest of the palette's warm colours (red over green over blue) to a warm colour: dim firelight, where
 * the nearest of all can be an olive or a grey.
 */
uint8_t NearestWarmColor(float r, float g, float b)
{
	float best = std::numeric_limits<float>::max();
	uint8_t found = NearestColor(r, g, b);
	for (int j = 1; j < 256; j++) {
		if (j >= 112 && j <= 127)
			continue;
		const SDL_Color o = logical_palette[j];
		if (o.r < o.g || o.g < o.b || o.r < o.b + 24)
			continue;
		const float d = (o.r - r) * (o.r - r) * 3 + (o.g - g) * (o.g - g) * 6 + (o.b - b) * (o.b - b);
		if (d < best) {
			best = d;
			found = static_cast<uint8_t>(j);
		}
	}
	return found;
}

/**
 * @brief The church's stained glass lit from inside, PaneLevels strengths of its light: the glass's brightness on a
 * ramp from deep red through orange to a hot yellow, as the original art draws its lit windows.
 */
const std::array<uint8_t, 256> &GlassGlow(int level)
{
	static std::array<std::array<uint8_t, 256>, PaneLevels> tables {};
	static uint32_t readyFor = 0;
	if (readyFor != PaletteVersion()) {
		readyFor = PaletteVersion();
		constexpr std::array<std::array<float, 4>, 6> Ramp { { { 0.0F, 30, 2, 2 }, { 0.35F, 128, 8, 6 }, { 0.6F, 200, 18, 10 }, { 0.8F, 236, 70, 18 }, { 0.92F, 250, 126, 38 }, { 1.0F, 255, 186, 88 } } };
		for (int l = 0; l < PaneLevels; l++) {
			// The glass texture is darkened like the walls (TextureDim): its brightest is about a third of white.
			const float gain = 2.9F + 0.7F * static_cast<float>(l) / static_cast<float>(PaneLevels - 1);
			for (int i = 0; i < 256; i++) {
				const SDL_Color c = logical_palette[i];
				const float t = std::clamp((0.3F * c.r + 0.59F * c.g + 0.11F * c.b) / 255.0F * gain, 0.0F, 1.0F);
				size_t k = 1;
				while (k + 1 < Ramp.size() && Ramp[k][0] < t)
					k++;
				const float f = std::clamp((t - Ramp[k - 1][0]) / (Ramp[k][0] - Ramp[k - 1][0]), 0.0F, 1.0F);
				tables[l][i] = NearestColor(Ramp[k - 1][1] + (Ramp[k][1] - Ramp[k - 1][1]) * f, Ramp[k - 1][2] + (Ramp[k][2] - Ramp[k - 1][2]) * f, Ramp[k - 1][3] + (Ramp[k][3] - Ramp[k - 1][3]) * f);
			}
		}
	}
	return tables[std::clamp(level, 0, PaneLevels - 1)];
}

/** @brief The table step for a flicker value (-1..1), dithered by a hash so the steps between do not band. */
int FlickerStep(float flicker, float amplitude, uint32_t hash)
{
	const float t = 0.5F + 0.5F * flicker * amplitude / 0.14F; // 0..1 over the table's range
	const float dither = static_cast<float>(hash & 0xFF) / 256.0F;
	return std::clamp(static_cast<int>(t * static_cast<float>(PaneLevels - 1) + dither), 0, PaneLevels - 1);
}

/**
 * @brief The palette colours of the lamps' light falling on a colour, GlowLevels strengths of it: the colour lit
 * (as a surface reflects light: brighter, warmer) and a little of the light itself, so dark ground shows it too.
 */
const std::array<uint8_t, 256> &GlowColors(int lit, int level)
{
	static std::array<std::array<std::array<uint8_t, 256>, GlowLevels>, LitLandmarks.size()> tables {};
	static uint32_t readyFor = 0;
	if (readyFor != PaletteVersion()) {
		readyFor = PaletteVersion();
		for (size_t f = 0; f < LitLandmarks.size(); f++)
		for (int l = 0; l < GlowLevels; l++) {
			const float s = static_cast<float>(l) / static_cast<float>(GlowLevels - 1);
			for (int i = 0; i < 256; i++) {
				const SDL_Color c = logical_palette[i];
				if (i == 0 || l == 0) {
					tables[f][l][i] = static_cast<uint8_t>(i);
					continue;
				}
				const std::array<float, 3> &color = LitLandmarks[f].color;
				auto shine = [&](float channel, float light) { return std::min(255.0F, channel * (1.0F + 2.6F * s * light / 255.0F) + s * light * 0.07F); };
				tables[f][l][i] = NearestColor(shine(c.r, color[0]), shine(c.g, color[1]), shine(c.b, color[2]));
			}
		}
	}
	return tables[std::clamp<size_t>(lit, 0, LitLandmarks.size() - 1)][std::clamp(level, 0, GlowLevels - 1)];
}

/** @brief The nearest of a building's surfaces (not its lit rooms) hit by the segment o + d * t, t in (0, maxT). */
bool SegmentHitsBuilding(const Building &b, V3 o, V3 d, float maxT, float &hitT)
{
	bool hit = false;
	hitT = maxT;
	for (const WorldTriangle &tri : b.mesh) {
		if (tri.mapping == MeshInterior)
			continue;
		const V3 e1 { tri.p[1].x - tri.p[0].x, tri.p[1].y - tri.p[0].y, tri.p[1].z - tri.p[0].z };
		const V3 e2 { tri.p[2].x - tri.p[0].x, tri.p[2].y - tri.p[0].y, tri.p[2].z - tri.p[0].z };
		const V3 pv { d.y * e2.z - d.z * e2.y, d.z * e2.x - d.x * e2.z, d.x * e2.y - d.y * e2.x };
		const float det = e1.x * pv.x + e1.y * pv.y + e1.z * pv.z;
		if (std::abs(det) < 1e-9F)
			continue;
		const float inv = 1.0F / det;
		const V3 tv { o.x - tri.p[0].x, o.y - tri.p[0].y, o.z - tri.p[0].z };
		const float u = (tv.x * pv.x + tv.y * pv.y + tv.z * pv.z) * inv;
		if (u < 0 || u > 1)
			continue;
		const V3 qv { tv.y * e1.z - tv.z * e1.y, tv.z * e1.x - tv.x * e1.z, tv.x * e1.y - tv.y * e1.x };
		const float v = (d.x * qv.x + d.y * qv.y + d.z * qv.z) * inv;
		if (v < 0 || u + v > 1)
			continue;
		const float t = (e2.x * qv.x + e2.y * qv.y + e2.z * qv.z) * inv;
		if (t > 1e-4F && t < hitT) {
			hitT = t;
			hit = true;
		}
	}
	return hit;
}

/** @brief The ceiling of a lit room at a point (its cell's). */
float RoomCeiling(const LitRoom &room, float x, float y)
{
	const int i = std::clamp(static_cast<int>((x - room.x0) / RoomCell), 0, room.nx - 1);
	const int j = std::clamp(static_cast<int>((y - room.y0) / RoomCell), 0, room.ny - 1);
	return room.ceiling[static_cast<size_t>(j) * room.nx + i];
}

/**
 * @brief Fits a lit room to its building: each side moved out to the wall the model has there (the median of rays
 * cast out from the room's middle, so a window does not count), its ceiling a little under the roof cell by cell.
 */
void FitRoom(const Building &b, LitRoom &room)
{
	const float midZ = room.z0 + 0.8F;
	for (int side = 0; side < 4; side++) {
		const int axis = side / 2; // 0: a side facing x, 1: facing y
		const float sign = side % 2 == 0 ? -1.0F : 1.0F;
		const V3 d = axis == 0 ? V3 { sign, 0, 0 } : V3 { 0, sign, 0 };
		const float alongLo = axis == 0 ? room.y0 : room.x0;
		const float alongHi = axis == 0 ? room.y1 : room.x1;
		const float from = axis == 0 ? (room.x0 + room.x1) / 2.0F : (room.y0 + room.y1) / 2.0F;
		float &edge = axis == 0 ? (sign < 0 ? room.x0 : room.x1) : (sign < 0 ? room.y0 : room.y1);
		std::vector<float> walls;
		for (int i = 0; i < 9; i++) {
			for (int k = 0; k < 3; k++) {
				const float along = alongLo + (alongHi - alongLo) * (static_cast<float>(i) + 0.5F) / 9.0F;
				const float z = midZ + 0.3F * static_cast<float>(k - 1);
				const V3 o = axis == 0 ? V3 { from, along, z } : V3 { along, from, z };
				float t;
				if (SegmentHitsBuilding(b, o, d, std::abs(edge - from) + 1.0F, t))
					walls.push_back(from + sign * t);
			}
		}
		if (walls.size() < 6)
			continue;
		std::nth_element(walls.begin(), walls.begin() + static_cast<ptrdiff_t>(walls.size() / 2), walls.end());
		const float wall = walls[walls.size() / 2];
		if (std::abs(wall - edge) < 0.6F)
			edge = wall - sign * 0.03F;
	}
	room.nx = std::max(1, static_cast<int>(std::ceil((room.x1 - room.x0) / RoomCell)));
	room.ny = std::max(1, static_cast<int>(std::ceil((room.y1 - room.y0) / RoomCell)));
	room.ceiling.assign(static_cast<size_t>(room.nx) * room.ny, room.top);
	float highest = room.z0;
	for (int j = 0; j < room.ny; j++) {
		for (int i = 0; i < room.nx; i++) {
			const float cx0 = room.x0 + (room.x1 - room.x0) * static_cast<float>(i) / static_cast<float>(room.nx);
			const float cx1 = room.x0 + (room.x1 - room.x0) * static_cast<float>(i + 1) / static_cast<float>(room.nx);
			const float cy0 = room.y0 + (room.y1 - room.y0) * static_cast<float>(j) / static_cast<float>(room.ny);
			const float cy1 = room.y0 + (room.y1 - room.y0) * static_cast<float>(j + 1) / static_cast<float>(room.ny);
			float h = room.top;
			for (int sj = 0; sj <= 2; sj++)
				for (int si = 0; si <= 2; si++) {
					const float roof = RoofHeightAt(b, cx0 + (cx1 - cx0) * static_cast<float>(si) / 2.0F, cy0 + (cy1 - cy0) * static_cast<float>(sj) / 2.0F);
					if (roof > 0)
						h = std::min(h, roof - 0.12F);
				}
			h = std::max(h, room.z0 + 0.6F);
			room.ceiling[static_cast<size_t>(j) * room.nx + i] = h;
			highest = std::max(highest, h);
		}
	}
	room.top = highest;
}

/**
 * @brief Adds a lit room's inside to its building's mesh (MeshInterior, WorldTriangle::arch its index in LitRooms):
 * its floor, its ceiling cell by cell with the steps between them, and its walls, all facing in, so that through a
 * window one sees the far side of the room.
 */
void AddRoomInside(Building &b, int roomIndex)
{
	const LitRoom &room = LitRooms[roomIndex];
	const size_t first = b.mesh.size();
	auto cellX = [&](int i) { return room.x0 + (room.x1 - room.x0) * static_cast<float>(i) / static_cast<float>(room.nx); };
	auto cellY = [&](int j) { return room.y0 + (room.y1 - room.y0) * static_cast<float>(j) / static_cast<float>(room.ny); };
	auto ceilingAt = [&](int i, int j) { return i < 0 || j < 0 || i >= room.nx || j >= room.ny ? -1.0F : room.ceiling[static_cast<size_t>(j) * room.nx + i]; };
	const Vec2 middle { (room.x0 + room.x1) / 2.0F, (room.y0 + room.y1) / 2.0F };
	const float z0 = room.z0;
	for (int j = 0; j < room.ny; j++) {
		for (int i = 0; i < room.nx; i++) {
			const float ax = cellX(i);
			const float bx = cellX(i + 1);
			const float ay = cellY(j);
			const float by = cellY(j + 1);
			const float h = ceilingAt(i, j);
			AddStructureFace(b, { { ax, ay, z0 }, { bx, ay, z0 }, { bx, by, z0 }, { ax, by, z0 } }, { 0, 0, 1 }, middle, MeshInterior, 0);
			AddStructureFace(b, { { ax, ay, h }, { ax, by, h }, { bx, by, h }, { bx, ay, h } }, { 0, 0, -1 }, middle, MeshInterior, 0);
			// Each side: a wall where the room ends, else the step up to this cell's ceiling from a lower neighbour's.
			const std::array<std::pair<int, int>, 4> next { { { i - 1, j }, { i + 1, j }, { i, j - 1 }, { i, j + 1 } } };
			for (int s = 0; s < 4; s++) {
				const float other = ceilingAt(next[s].first, next[s].second);
				const float low = other < 0 ? z0 : other;
				if (low >= h - 1e-4F)
					continue;
				const float x = s == 0 ? ax : bx;
				const float y = s == 2 ? ay : by;
				// Facing into this cell when it is a wall, into the neighbour (the lower ceiling) when it is a step.
				const float facing = other < 0 ? 1.0F : -1.0F;
				if (s < 2) {
					const V3 normal { (s == 0 ? 1.0F : -1.0F) * facing, 0, 0 };
					AddStructureFace(b, { { x, ay, low }, { x, by, low }, { x, by, h }, { x, ay, h } }, normal, middle, MeshInterior, 0);
				} else {
					const V3 normal { 0, (s == 2 ? 1.0F : -1.0F) * facing, 0 };
					AddStructureFace(b, { { ax, y, low }, { bx, y, low }, { bx, y, h }, { ax, y, h } }, normal, middle, MeshInterior, 0);
				}
			}
		}
	}
	for (size_t i = first; i < b.mesh.size(); i++)
		b.mesh[i].arch = static_cast<int16_t>(roomIndex);
}

/** How high above its floor a lamp hangs at most (tiles), and how far apart lamps hang up a tall room. */
constexpr float LampHang = 1.6F;
constexpr float LampStack = 2.5F;

/** @brief Hangs a room's lamps along its length, one about every three tiles, under its ceiling. */
void AddRoomLamps(int roomIndex)
{
	const LitRoom &room = LitRooms[roomIndex];
	const bool alongX = room.x1 - room.x0 >= room.y1 - room.y0;
	const float length = alongX ? room.x1 - room.x0 : room.y1 - room.y0;
	const int count = std::max(1, static_cast<int>(std::lround(length / 3.2F)));
	for (int k = 0; k < count; k++) {
		const float t = (static_cast<float>(k) + 0.5F) / static_cast<float>(count);
		const float x = alongX ? room.x0 + (room.x1 - room.x0) * t : (room.x0 + room.x1) / 2.0F;
		const float y = alongX ? (room.y0 + room.y1) / 2.0F : room.y0 + (room.y1 - room.y0) * t;
		// One more every LampStack up a tall room (the church's tower).
		const float under = RoomCeiling(room, x, y) - LampDrop;
		for (float z = std::max(std::min(under, room.z0 + LampHang), room.z0 + 0.5F); z <= std::max(under, room.z0 + 0.5F); z += LampStack) {
			Lamps.push_back({ { x, y, z }, LitLandmarks[room.lit].strength, room.lit, roomIndex });
			LogInfo("fp: {}: lamp at {:.2f}, {:.2f}, {:.2f}", LitLandmarks[room.lit].name, x, y, z);
		}
	}
}

/**
 * @brief The floor of an upper storey in a room, if the model has one: the lowest level, well above the room's floor,
 * where flat surfaces cover a good part of the room. Its underside and top.
 */
bool FindStoreyFloor(const Building &b, const LitRoom &room, float &bottom, float &top)
{
	constexpr float Bin = 0.1F;
	const int bins = static_cast<int>((room.top - room.z0) / Bin) + 1;
	std::vector<float> area(static_cast<size_t>(bins), 0.0F);
	for (const WorldTriangle &tri : b.mesh) {
		if (tri.mapping == MeshInterior)
			continue;
		const float cx = (tri.p[0].x + tri.p[1].x + tri.p[2].x) / 3.0F;
		const float cy = (tri.p[0].y + tri.p[1].y + tri.p[2].y) / 3.0F;
		const float cz = (tri.p[0].z + tri.p[1].z + tri.p[2].z) / 3.0F;
		if (cx < room.x0 + 0.2F || cx > room.x1 - 0.2F || cy < room.y0 + 0.2F || cy > room.y1 - 0.2F || cz < room.z0 + 1.2F || cz > room.top - 0.4F)
			continue;
		const V3 u { tri.p[1].x - tri.p[0].x, tri.p[1].y - tri.p[0].y, tri.p[1].z - tri.p[0].z };
		const V3 v { tri.p[2].x - tri.p[0].x, tri.p[2].y - tri.p[0].y, tri.p[2].z - tri.p[0].z };
		const V3 n { u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z, u.x * v.y - u.y * v.x };
		const float twice = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
		if (std::abs(n.z) > 0.9F * twice)
			area[static_cast<size_t>((cz - room.z0) / Bin)] += twice / 2.0F;
	}
	const float needed = 0.3F * (room.x1 - room.x0) * (room.y1 - room.y0);
	for (int i = 0; i + 1 < bins; i++) {
		if (area[i] + area[i + 1] < needed)
			continue;
		bottom = std::numeric_limits<float>::max();
		top = std::numeric_limits<float>::lowest();
		const float from = room.z0 + static_cast<float>(i) * Bin;
		for (const WorldTriangle &tri : b.mesh) {
			if (tri.mapping == MeshInterior)
				continue;
			const float cx = (tri.p[0].x + tri.p[1].x + tri.p[2].x) / 3.0F;
			const float cy = (tri.p[0].y + tri.p[1].y + tri.p[2].y) / 3.0F;
			const float cz = (tri.p[0].z + tri.p[1].z + tri.p[2].z) / 3.0F;
			if (cx < room.x0 + 0.2F || cx > room.x1 - 0.2F || cy < room.y0 + 0.2F || cy > room.y1 - 0.2F || cz < from || cz > from + 0.4F)
				continue;
			for (const V3 &q : tri.p) {
				bottom = std::min(bottom, q.z);
				top = std::max(top, q.z);
			}
		}
		return bottom < top + 1e-3F;
	}
	return false;
}

/** @brief The church's lit room: inside its stained glass box (the nave behind the windows), a little inside it. */
void AddGlassRoom(const Building &b, int lit, const LandmarkPlacement &place)
{
	if (GlassTexture < 0)
		return;
	LitRoom room;
	room.lit = lit;
	room.x0 = room.y0 = room.z0 = std::numeric_limits<float>::max();
	room.x1 = room.y1 = room.top = std::numeric_limits<float>::lowest();
	for (const WorldTriangle &tri : b.mesh) {
		if (tri.texture != GlassTexture)
			continue;
		for (const V3 &p : tri.p) {
			room.x0 = std::min(room.x0, p.x);
			room.x1 = std::max(room.x1, p.x);
			room.y0 = std::min(room.y0, p.y);
			room.y1 = std::max(room.y1, p.y);
			room.z0 = std::min(room.z0, p.z);
			room.top = std::max(room.top, p.z);
		}
	}
	if (room.x0 > room.x1)
		return;
	room.x0 += 0.1F;
	room.x1 -= 0.1F;
	room.y0 += 0.1F;
	room.y1 -= 0.1F;
	room.z0 = std::max(room.z0, 0.0F);
	room.nx = room.ny = 1;
	room.ceiling.assign(1, room.top + 0.2F);
	LogInfo("fp: {}: glass room {:.2f}..{:.2f} x {:.2f}..{:.2f}, z {:.2f}..{:.2f}", place.name, room.x0, room.x1, room.y0, room.y1, room.z0, room.top);
	LitRooms.push_back(room);
	AddRoomLamps(static_cast<int>(LitRooms.size() - 1));
}

/**
 * @brief A lamp in the middle of a landmark's own inside (WorldTriangle::inside: the church's entrance hall behind its
 * open door), under its ceiling: its light comes out of the doorway.
 */
void AddInsideLamp(const Building &b, int lit)
{
	V3 lo { std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max() };
	V3 hi { std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest() };
	for (const WorldTriangle &tri : b.mesh) {
		if (!tri.inside)
			continue;
		for (const V3 &p : tri.p) {
			lo = { std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z) };
			hi = { std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z) };
		}
	}
	if (lo.x > hi.x)
		return;
	const V3 at { (lo.x + hi.x) / 2.0F, (lo.y + hi.y) / 2.0F, std::max(hi.z - LampDrop, lo.z + 0.5F) };
	Lamps.push_back({ at, LitLandmarks[lit].strength, lit, -1 });
	LogInfo("fp: {}: lamp in its inside at {:.2f}, {:.2f}, {:.2f}", LitLandmarks[lit].name, at.x, at.y, at.z);
}

/** @brief The nearest of some triangles hit by the segment o + d * t, t in (0, maxT). */
bool RayHitsTriangles(const std::vector<const WorldTriangle *> &triangles, V3 o, V3 d, float maxT, float &hitT)
{
	bool hit = false;
	hitT = maxT;
	for (const WorldTriangle *tri : triangles) {
		const V3 e1 { tri->p[1].x - tri->p[0].x, tri->p[1].y - tri->p[0].y, tri->p[1].z - tri->p[0].z };
		const V3 e2 { tri->p[2].x - tri->p[0].x, tri->p[2].y - tri->p[0].y, tri->p[2].z - tri->p[0].z };
		const V3 pv { d.y * e2.z - d.z * e2.y, d.z * e2.x - d.x * e2.z, d.x * e2.y - d.y * e2.x };
		const float det = e1.x * pv.x + e1.y * pv.y + e1.z * pv.z;
		if (std::abs(det) < 1e-9F)
			continue;
		const float inv = 1.0F / det;
		const V3 tv { o.x - tri->p[0].x, o.y - tri->p[0].y, o.z - tri->p[0].z };
		const float u = (tv.x * pv.x + tv.y * pv.y + tv.z * pv.z) * inv;
		if (u < 0 || u > 1)
			continue;
		const V3 qv { tv.y * e1.z - tv.z * e1.y, tv.z * e1.x - tv.x * e1.z, tv.x * e1.y - tv.y * e1.x };
		const float v = (d.x * qv.x + d.y * qv.y + d.z * qv.z) * inv;
		if (v < 0 || u + v > 1)
			continue;
		const float t = (e2.x * qv.x + e2.y * qv.y + e2.z * qv.z) * inv;
		if (t > 1e-4F && t < hitT) {
			hitT = t;
			hit = true;
		}
	}
	return hit;
}

/** @brief A window in a building's wall: the rectangle of its opening in the plane through the middle of the wall. */
struct WindowOpening {
	int axis;    // 0: in a wall facing x (along y), 1: facing y (along x)
	float plane; // the wall's middle
	float along0;
	float along1;
	float z0;
	float z1;
};

/**
 * @brief The windows in the walls around a lit room: looking out from its sides, the places where nothing of the
 * building is in the way, each joined into a rectangle. Openings down to the floor are doors (left open), and very
 * big ones are open fronts (the smithy's forge).
 */
std::vector<WindowOpening> FindWindows(const Building &b, const LitRoom &room)
{
	std::vector<WindowOpening> windows;
	constexpr float Step = 0.05F;
	constexpr float Reach = 1.0F;
	constexpr float FitReach = 0.15F; // how far a window's edges may be moved out to the hole's (past the room's ceiling)
	for (int side = 0; side < 4; side++) {
		const int axis = side / 2;
		const float sign = side % 2 == 0 ? -1.0F : 1.0F;
		const V3 out = axis == 0 ? V3 { sign, 0, 0 } : V3 { 0, sign, 0 };
		const float at = axis == 0 ? (sign < 0 ? room.x0 : room.x1) : (sign < 0 ? room.y0 : room.y1);
		const float lo = axis == 0 ? room.y0 : room.x0;
		const float hi = axis == 0 ? room.y1 : room.x1;
		const float zLo = room.z0 + 0.1F;
		const float zHi = room.top;
		// The triangles that can be in the way: in the slab just outside this side.
		std::vector<const WorldTriangle *> near;
		for (const WorldTriangle &tri : b.mesh) {
			if (tri.mapping == MeshInterior)
				continue;
			float a0 = std::numeric_limits<float>::max();
			float a1 = std::numeric_limits<float>::lowest();
			float l0 = a0;
			float l1 = a1;
			float z0 = a0;
			float z1 = a1;
			for (const V3 &p : tri.p) {
				const float across = (axis == 0 ? p.x : p.y) - at;
				a0 = std::min(a0, across * sign);
				a1 = std::max(a1, across * sign);
				l0 = std::min(l0, axis == 0 ? p.y : p.x);
				l1 = std::max(l1, axis == 0 ? p.y : p.x);
				z0 = std::min(z0, p.z);
				z1 = std::max(z1, p.z);
			}
			if (a1 >= -0.05F && a0 <= Reach && l1 >= lo - FitReach && l0 <= hi + FitReach && z1 >= zLo - FitReach && z0 <= zHi + FitReach)
				near.push_back(&tri);
		}
		const int na = static_cast<int>((hi - lo) / Step);
		const int nz = static_cast<int>((zHi - zLo) / Step);
		if (na <= 0 || nz <= 0)
			continue;
		std::vector<uint8_t> open(static_cast<size_t>(na) * nz, 0);
		std::vector<float> walls;
		std::vector<std::pair<float, float>> closed; // (along, z) of some samples with wall, for its thickness
		for (int k = 0; k < nz; k++) {
			for (int i = 0; i < na; i++) {
				const float along = lo + (static_cast<float>(i) + 0.5F) * Step;
				const float z = zLo + (static_cast<float>(k) + 0.5F) * Step;
				const float x = axis == 0 ? at : along;
				const float y = axis == 0 ? along : at;
				if (z > RoomCeiling(room, x, y) - 0.05F)
					continue;
				float t;
				if (RayHitsTriangles(near, { x - out.x * 0.02F, y - out.y * 0.02F, z }, out, Reach, t)) {
					walls.push_back(t - 0.02F);
					if ((i + k) % 7 == 0)
						closed.emplace_back(along, z);
				} else {
					open[static_cast<size_t>(k) * na + i] = 1;
				}
			}
		}
		if (walls.size() < 20)
			continue;
		std::nth_element(walls.begin(), walls.begin() + static_cast<ptrdiff_t>(walls.size() / 2), walls.end());
		const float inner = walls[walls.size() / 2];
		// The wall's outer face: looking back in from outside.
		std::vector<float> outers;
		for (const auto &[along, z] : closed) {
			const float x = axis == 0 ? at + sign * (inner + 0.8F) : along;
			const float y = axis == 0 ? along : at + sign * (inner + 0.8F);
			float t;
			if (RayHitsTriangles(near, { x, y, z }, { -out.x, -out.y, 0 }, 0.8F, t))
				outers.push_back(inner + 0.8F - t);
		}
		float outer = inner + 0.3F;
		if (!outers.empty()) {
			std::nth_element(outers.begin(), outers.begin() + static_cast<ptrdiff_t>(outers.size() / 2), outers.end());
			outer = std::max(outers[outers.size() / 2], inner + 0.05F);
		}
		const float plane = at + sign * (inner + outer) / 2.0F;
		// Each opening: the open samples joined to each other.
		std::vector<uint8_t> seen(open.size(), 0);
		for (size_t start = 0; start < open.size(); start++) {
			if (open[start] == 0 || seen[start] != 0)
				continue;
			std::vector<size_t> stack { start };
			seen[start] = 1;
			int i0 = na;
			int i1 = -1;
			int k0 = nz;
			int k1 = -1;
			int cells = 0;
			while (!stack.empty()) {
				const size_t n = stack.back();
				stack.pop_back();
				const int i = static_cast<int>(n % na);
				const int k = static_cast<int>(n / na);
				i0 = std::min(i0, i);
				i1 = std::max(i1, i);
				k0 = std::min(k0, k);
				k1 = std::max(k1, k);
				cells++;
				for (const auto &[di, dk] : { std::pair { 1, 0 }, std::pair { -1, 0 }, std::pair { 0, 1 }, std::pair { 0, -1 } }) {
					const int ni = i + di;
					const int nk = k + dk;
					if (ni < 0 || nk < 0 || ni >= na || nk >= nz)
						continue;
					const size_t m = static_cast<size_t>(nk) * na + ni;
					if (open[m] != 0 && seen[m] == 0) {
						seen[m] = 1;
						stack.push_back(m);
					}
				}
			}
			WindowOpening window;
			window.axis = axis;
			window.plane = plane;
			window.along0 = lo + static_cast<float>(i0) * Step;
			window.along1 = lo + static_cast<float>(i1 + 1) * Step;
			window.z0 = zLo + static_cast<float>(k0) * Step;
			window.z1 = zLo + static_cast<float>(k1 + 1) * Step;
			const float width = window.along1 - window.along0;
			const float height = window.z1 - window.z0;
			const bool door = window.z0 < room.z0 + 0.35F;
			const bool square = static_cast<float>(cells) >= 0.6F * static_cast<float>((i1 - i0 + 1) * (k1 - k0 + 1));
			if (door || width < 0.2F || height < 0.25F || width > 2.0F || height > 2.0F || !square)
				continue;
			// Each edge fitted to the hole's, finer than the samples (and past the room's ceiling, which can be lower
			// than the top of a window under the eaves): out from just inside it while most rays across get through.
			auto through = [&](float along, float z) {
				const float x = axis == 0 ? at : along;
				const float y = axis == 0 ? along : at;
				float t;
				return !RayHitsTriangles(near, { x - out.x * 0.02F, y - out.y * 0.02F, z }, out, Reach, t);
			};
			auto openRow = [&](bool alongEdge, float edge) {
				int got = 0;
				for (int k = 1; k <= 5; k++) {
					const float f = 0.25F + 0.5F * static_cast<float>(k) / 6.0F;
					got += alongEdge ? through(edge, window.z0 + (window.z1 - window.z0) * f) : through(window.along0 + (window.along1 - window.along0) * f, edge);
				}
				return got >= 3;
			};
			constexpr float Fine = 0.01F;
			auto fit = [&](bool alongEdge, float &edge, float dir) {
				float last = edge - dir * Step;
				for (float e = last + dir * Fine; std::abs(e - edge) <= FitReach; e += dir * Fine) {
					if (!openRow(alongEdge, e))
						break;
					last = e;
				}
				edge = last + dir * (Fine / 2.0F + 0.01F); // a little into the wall, so no crack shows round the frame
			};
			fit(false, window.z1, 1.0F);
			fit(false, window.z0, -1.0F);
			fit(true, window.along1, 1.0F);
			fit(true, window.along0, -1.0F);
			windows.push_back(window);
		}
	}
	return windows;
}

/** Window frames: the border's width, the cross's, and how deep they are (tiles). */
constexpr float FrameBorder = 0.05F;
constexpr float FrameCross = 0.035F;
constexpr float FrameDepth = 0.06F;

/**
 * @brief Glazes a window: a pane of glass in the middle of the wall (MeshPane: glowing with its building's light,
 * WorldTriangle::arch the lit landmark) in a wooden frame of four lights, a border and a cross (MeshFrame), unless it
 * has its own (a door's grille).
 */
void AddWindowPane(Building &b, const WindowOpening &w, int lit, bool frame = true)
{
	auto at = [&](float along, float across, float z) {
		return w.axis == 0 ? V3 { w.plane + across, along, z } : V3 { along, w.plane + across, z };
	};
	const V3 facing = w.axis == 0 ? V3 { 1, 0, 0 } : V3 { 0, 1, 0 };
	const Vec2 middle = w.axis == 0 ? Vec2 { w.plane, (w.along0 + w.along1) / 2.0F } : Vec2 { (w.along0 + w.along1) / 2.0F, w.plane };
	// The glass, both sides.
	const size_t glass = b.mesh.size();
	AddStructureFace(b, { at(w.along0, 0, w.z0), at(w.along1, 0, w.z0), at(w.along1, 0, w.z1), at(w.along0, 0, w.z1) }, facing, middle, MeshPane, 0);
	for (size_t i = glass; i < b.mesh.size(); i++) {
		WorldTriangle &tri = b.mesh[i];
		tri.cull = false;
		tri.arch = static_cast<int16_t>(lit);
		tri.mirrorX = middle.x; // the pane's middle and half its size, for its glow (brightest in the middle)
		tri.mirrorY = middle.y;
		tri.downX = (w.z0 + w.z1) / 2.0F;
		tri.slopeScale = (w.along1 - w.along0) / 2.0F;
		tri.downY = (w.z1 - w.z0) / 2.0F;
	}
	if (!frame)
		return;
	// The frame: boxes standing out of the glass on both sides.
	auto bar = [&](float a0, float a1, float z0, float z1) {
		const float d = FrameDepth / 2.0F;
		const V3 p000 = at(a0, -d, z0), p100 = at(a1, -d, z0), p110 = at(a1, d, z0), p010 = at(a0, d, z0);
		const V3 p001 = at(a0, -d, z1), p101 = at(a1, -d, z1), p111 = at(a1, d, z1), p011 = at(a0, d, z1);
		const V3 alongDir = w.axis == 0 ? V3 { 0, 1, 0 } : V3 { 1, 0, 0 };
		AddStructureFace(b, { p010, p110, p111, p011 }, facing, middle, MeshFrame, 0);
		AddStructureFace(b, { p000, p100, p101, p001 }, { -facing.x, -facing.y, 0 }, middle, MeshFrame, 0);
		AddStructureFace(b, { p001, p101, p111, p011 }, { 0, 0, 1 }, middle, MeshFrame, 0);
		AddStructureFace(b, { p000, p100, p110, p010 }, { 0, 0, -1 }, middle, MeshFrame, 0);
		AddStructureFace(b, { p100, p110, p111, p101 }, alongDir, middle, MeshFrame, 0);
		AddStructureFace(b, { p000, p010, p011, p001 }, { -alongDir.x, -alongDir.y, 0 }, middle, MeshFrame, 0);
	};
	const float am = (w.along0 + w.along1) / 2.0F;
	const float zm = (w.z0 + w.z1) / 2.0F;
	bar(w.along0, w.along0 + FrameBorder, w.z0, w.z1);
	bar(w.along1 - FrameBorder, w.along1, w.z0, w.z1);
	bar(w.along0, w.along1, w.z0, w.z0 + FrameBorder);
	bar(w.along0, w.along1, w.z1 - FrameBorder, w.z1);
	bar(am - FrameCross / 2.0F, am + FrameCross / 2.0F, w.z0, w.z1);
	bar(w.along0, w.along1, zm - FrameCross / 2.0F, zm + FrameCross / 2.0F);
}

/**
 * @brief The faces of the wall beyond one side of a room (0 -x, 1 +x, 2 -y, 3 +y): how far out from the side its inner
 * and its outer face are (the middle of a few rays, so an opening does not count). False if there is no wall.
 */
bool WallFaces(const Building &b, const LitRoom &room, int side, float &inner, float &outer)
{
	const int axis = side / 2;
	const float sign = side % 2 == 0 ? -1.0F : 1.0F;
	const V3 out = axis == 0 ? V3 { sign, 0, 0 } : V3 { 0, sign, 0 };
	const float at = axis == 0 ? (sign < 0 ? room.x0 : room.x1) : (sign < 0 ? room.y0 : room.y1);
	const float lo = axis == 0 ? room.y0 : room.x0;
	const float hi = axis == 0 ? room.y1 : room.x1;
	std::vector<float> inners;
	std::vector<float> outers;
	for (int k = 1; k <= 7; k++) {
		const float along = lo + (hi - lo) * static_cast<float>(k) / 8.0F;
		const float x = axis == 0 ? at : along;
		const float y = axis == 0 ? along : at;
		const float z = room.z0 + std::min(0.9F, (RoomCeiling(room, x, y) - room.z0) * 0.5F); // under low eaves too
		float t;
		if (!SegmentHitsBuilding(b, { x - out.x * 0.02F, y - out.y * 0.02F, z }, out, 1.0F, t))
			continue;
		inners.push_back(t - 0.02F);
		const V3 back { x + out.x * (t + 0.8F), y + out.y * (t + 0.8F), z };
		float u;
		if (SegmentHitsBuilding(b, back, { -out.x, -out.y, 0 }, 0.8F, u))
			outers.push_back(t + 0.8F - u);
	}
	if (inners.size() < 3)
		return false;
	std::nth_element(inners.begin(), inners.begin() + static_cast<ptrdiff_t>(inners.size() / 2), inners.end());
	inner = inners[inners.size() / 2];
	outer = inner + 0.25F;
	if (!outers.empty()) {
		std::nth_element(outers.begin(), outers.begin() + static_cast<ptrdiff_t>(outers.size() / 2), outers.end());
		outer = std::max(outers[outers.size() / 2], inner + 0.05F);
	}
	return true;
}

/** Windows set into houses whose models have none: their size, and their sill above the room's floor (tiles). */
constexpr float SetWindowWidth = 0.5F;
constexpr float SetWindowHeight = 0.62F;
constexpr float SetWindowSill = 0.75F;

/**
 * @brief Windows for a house whose model has none in its walls (some of the plaza cottages): one in each wall of a
 * room (two in a long one), clear of its doors, glazed on the wall's outer face, with a light just outside each
 * shining out.
 */
void AddSetWindows(Building &b, int lit, const LitRoom &room, std::vector<WindowOpening> &glazed)
{
	int added = 0;
	for (int side = 0; side < 4; side++) {
		const int axis = side / 2;
		const float sign = side % 2 == 0 ? -1.0F : 1.0F;
		const V3 out = axis == 0 ? V3 { sign, 0, 0 } : V3 { 0, sign, 0 };
		const float at = axis == 0 ? (sign < 0 ? room.x0 : room.x1) : (sign < 0 ? room.y0 : room.y1);
		const float lo = axis == 0 ? room.y0 : room.x0;
		const float hi = axis == 0 ? room.y1 : room.x1;
		const float length = hi - lo;
		const int count = length >= 5.0F ? 2 : (length >= 1.2F ? 1 : 0);
		float inner;
		float outer;
		if (count == 0 || !WallFaces(b, room, side, inner, outer))
			continue;
		const float plane = at + sign * outer;
		for (int k = 0; k < count; k++) {
			const float middle = lo + length * (static_cast<float>(k) + 0.5F) / static_cast<float>(count);
			WindowOpening w { axis, plane, middle - SetWindowWidth / 2.0F, middle + SetWindowWidth / 2.0F, room.z0 + SetWindowSill, room.z0 + SetWindowSill + SetWindowHeight };
			// Under the ceiling there: under low eaves smaller and lower, between the floor and the eaves.
			const float x = axis == 0 ? at : middle;
			const float y = axis == 0 ? middle : at;
			const float ceiling = RoomCeiling(room, x, y);
			if (w.z1 > ceiling - 0.12F) {
				const float room0 = room.z0 + 0.12F;
				const float space = ceiling - 0.12F - room0;
				if (space < 0.32F)
					continue;
				const float height = std::min(SetWindowHeight, space * 0.8F);
				w.z0 = room0 + (space - height) * 0.6F;
				w.z1 = w.z0 + height;
				const float width = std::min(SetWindowWidth, height * 0.85F);
				w.along0 = middle - width / 2.0F;
				w.along1 = middle + width / 2.0F;
			}
			bool clear = true;
			for (const std::array<float, 6> &door : b.doors) {
				const float doorPlane = axis == 0 ? (door[0] + door[1]) / 2.0F : (door[2] + door[3]) / 2.0F;
				const float d0 = axis == 0 ? door[2] : door[0];
				const float d1 = axis == 0 ? door[3] : door[1];
				const bool sameWall = std::abs(doorPlane - plane) < 0.5F && (axis == 0 ? door[1] - door[0] : door[3] - door[2]) < 0.4F;
				clear = clear && !(sameWall && d0 < w.along1 + 0.3F && w.along0 - 0.3F < d1);
			}
			for (const WindowOpening &o : glazed)
				clear = clear && !(o.axis == axis && std::abs(o.plane - plane) < 0.5F && o.along0 < w.along1 + 0.3F && w.along0 - 0.3F < o.along1);
			// The wall whole there (no opening already).
			float t;
			clear = clear && SegmentHitsBuilding(b, { x - out.x * 0.02F, y - out.y * 0.02F, (w.z0 + w.z1) / 2.0F }, out, 1.0F, t) && std::abs(t - 0.02F - inner) < 0.15F;
			if (!clear)
				continue;
			glazed.push_back(w);
			w.plane += sign * 0.02F; // on the wall's face
			AddWindowPane(b, w, lit);
			const float area = (w.along1 - w.along0) * (w.z1 - w.z0);
			const V3 lamp = axis == 0 ? V3 { w.plane + sign * 0.15F, middle, (w.z0 + w.z1) / 2.0F } : V3 { middle, w.plane + sign * 0.15F, (w.z0 + w.z1) / 2.0F };
			Lamps.push_back({ lamp, 1.6F * area * LitLandmarks[lit].strength, lit, -1, out, 1.0F, 3.5F });
			added++;
		}
	}
	LogInfo("fp: {}: {} windows set in a room's walls", LitLandmarks[lit].name, added);
}

/** @brief Glazes the windows of a lit landmark's rooms (each window once, where two rooms share a wall). */
std::vector<WindowOpening> GlazeWindows(Building &b, int lit, size_t firstRoom)
{
	std::vector<WindowOpening> done;
	for (size_t r = firstRoom; r < LitRooms.size(); r++) {
		for (const WindowOpening &w : FindWindows(b, LitRooms[r])) {
			bool same = false;
			for (const WindowOpening &o : done)
				same = same || (o.axis == w.axis && std::abs(o.plane - w.plane) < 0.3F && o.along0 < w.along1 && w.along0 < o.along1 && o.z0 < w.z1 && w.z0 < o.z1);
			if (same)
				continue;
			done.push_back(w);
			AddWindowPane(b, w, lit);
		}
	}
	LogInfo("fp: {}: {} windows glazed", LitLandmarks[lit].name, done.size());
	return done;
}

/**
 * @brief Glazes the little windows in a lit house's closed doors (a grille of four lights): a pane of its light just
 * behind the leaf, over the holes in it, so the house's emptied inside does not show through them.
 */
void GlazeDoors(Building &b, int lit, const LitRoom &room)
{
	constexpr float Step = 0.02F;
	int glazed = 0;
	for (const std::array<float, 6> &door : b.doors) {
		const int axis = door[1] - door[0] < door[3] - door[2] ? 0 : 1; // across the leaf: 0 x, 1 y
		const float thin0 = axis == 0 ? door[0] : door[2];
		const float thin1 = axis == 0 ? door[1] : door[3];
		const float lo = axis == 0 ? door[2] : door[0];
		const float hi = axis == 0 ? door[3] : door[1];
		if (thin1 - thin0 > 0.3F || hi - lo < 0.4F || door[4] > 0.4F)
			continue; // not a leaf standing on the ground
		const float plane = (thin0 + thin1) / 2.0F;
		const V3 across = axis == 0 ? V3 { 1, 0, 0 } : V3 { 0, 1, 0 };
		auto from = [&](float along, float z, float back) {
			return axis == 0 ? V3 { plane - back, along, z } : V3 { along, plane - back, z };
		};
		// Closed: in its wall, with the wall on both sides of it (an open leaf stands out of its wall).
		const float zWall = door[4] + (door[5] - door[4]) * 0.4F;
		float t;
		if (!SegmentHitsBuilding(b, from(lo - 0.15F, zWall, 0.5F), across, 1.0F, t) || !SegmentHitsBuilding(b, from(hi + 0.15F, zWall, 0.5F), across, 1.0F, t))
			continue;
		// The leaf: the surfaces in the door's bounds; the holes, where rays across get through it with the leaf all
		// round (not round its edge, an arch's corners).
		std::vector<const WorldTriangle *> leaf;
		for (const WorldTriangle &tri : b.mesh) {
			if (tri.mapping == MeshInterior || tri.mapping == MeshPane || tri.mapping == MeshFrame)
				continue;
			bool in = true;
			for (const V3 &p : tri.p)
				in = in && p.x >= door[0] - 0.02F && p.x <= door[1] + 0.02F && p.y >= door[2] - 0.02F && p.y <= door[3] + 0.02F && p.z >= door[4] - 0.02F && p.z <= door[5] + 0.02F;
			if (in)
				leaf.push_back(&tri);
		}
		const int na = static_cast<int>((hi - lo) / Step);
		const int nz = static_cast<int>((door[5] - door[4]) / Step);
		if (na < 3 || nz < 3)
			continue;
		std::vector<uint8_t> open(static_cast<size_t>(na) * nz, 0);
		for (int k = 0; k < nz; k++)
			for (int i = 0; i < na; i++)
				open[static_cast<size_t>(k) * na + i] = RayHitsTriangles(leaf, from(lo + (static_cast<float>(i) + 0.5F) * Step, door[4] + (static_cast<float>(k) + 0.5F) * Step, 0.4F), across, 0.8F, t) ? 0 : 1;
		// Round the edge: open from the bounds' border in.
		std::vector<size_t> stack;
		for (int k = 0; k < nz; k++)
			for (int i = 0; i < na; i++)
				if ((i == 0 || k == 0 || i == na - 1 || k == nz - 1) && open[static_cast<size_t>(k) * na + i] == 1) {
					open[static_cast<size_t>(k) * na + i] = 2;
					stack.push_back(static_cast<size_t>(k) * na + i);
				}
		while (!stack.empty()) {
			const size_t n = stack.back();
			stack.pop_back();
			const int i = static_cast<int>(n % na);
			const int k = static_cast<int>(n / na);
			for (const auto &[di, dk] : { std::pair { 1, 0 }, std::pair { -1, 0 }, std::pair { 0, 1 }, std::pair { 0, -1 } }) {
				const int ni = i + di;
				const int nk = k + dk;
				if (ni < 0 || nk < 0 || ni >= na || nk >= nz || open[static_cast<size_t>(nk) * na + ni] != 1)
					continue;
				open[static_cast<size_t>(nk) * na + ni] = 2;
				stack.push_back(static_cast<size_t>(nk) * na + ni);
			}
		}
		int i0 = na;
		int i1 = -1;
		int k0 = nz;
		int k1 = -1;
		int holes = 0;
		for (int k = 0; k < nz; k++)
			for (int i = 0; i < na; i++)
				if (open[static_cast<size_t>(k) * na + i] == 1) {
					i0 = std::min(i0, i);
					i1 = std::max(i1, i);
					k0 = std::min(k0, k);
					k1 = std::max(k1, k);
					holes++;
				}
		if (holes < 4)
			continue;
		// Behind the leaf, on the house's side.
		const float inside = axis == 0 ? (room.x0 + room.x1) / 2.0F : (room.y0 + room.y1) / 2.0F;
		const float sign = inside > plane ? 1.0F : -1.0F;
		const WindowOpening w { axis, (sign > 0 ? thin1 : thin0) + sign * 0.01F, lo + static_cast<float>(i0 - 1) * Step, lo + static_cast<float>(i1 + 2) * Step,
			door[4] + static_cast<float>(k0 - 1) * Step, door[4] + static_cast<float>(k1 + 2) * Step };
		AddWindowPane(b, w, lit, false);
		glazed++;
	}
	LogInfo("fp: {}: {} door windows glazed", LitLandmarks[lit].name, glazed);
}

/** The lamps behind the church's cross windows: how bright, how far their light carries, and how far behind. */
constexpr float CrossLampStrength = 0.6F;
constexpr float CrossLampHalf = 1.1F;
constexpr float CrossLampReach = 3.0F;
constexpr float CrossLampBehind = 0.06F;

/**
 * @brief A red lamp just behind each of the church's cross windows (the sides of its windows, WorldTriangle::rim,
 * grouped), a little above its middle, shining out through it: it lights the window's sides and the ground before it.
 */
void AddCrossLamps(const Building &b, int lit)
{
	std::vector<std::array<float, 6>> crosses; // x0, x1, y0, y1, z0, z1
	for (const WorldTriangle &tri : b.mesh) {
		if (!tri.rim)
			continue;
		std::array<float, 6> box { std::numeric_limits<float>::max(), std::numeric_limits<float>::lowest(), std::numeric_limits<float>::max(), std::numeric_limits<float>::lowest(),
			std::numeric_limits<float>::max(), std::numeric_limits<float>::lowest() };
		for (const V3 &p : tri.p)
			box = { std::min(box[0], p.x), std::max(box[1], p.x), std::min(box[2], p.y), std::max(box[3], p.y), std::min(box[4], p.z), std::max(box[5], p.z) };
		bool joined = false;
		for (std::array<float, 6> &cross : crosses) {
			if (box[0] <= cross[1] + 0.3F && cross[0] <= box[1] + 0.3F && box[2] <= cross[3] + 0.3F && cross[2] <= box[3] + 0.3F && box[4] <= cross[5] + 0.3F && cross[4] <= box[5] + 0.3F) {
				cross = { std::min(cross[0], box[0]), std::max(cross[1], box[1]), std::min(cross[2], box[2]), std::max(cross[3], box[3]), std::min(cross[4], box[4]), std::max(cross[5], box[5]) };
				joined = true;
				break;
			}
		}
		if (!joined)
			crosses.push_back(box);
	}
	// Groups a side bridged after they were started are one window.
	for (bool merged = true; merged;) {
		merged = false;
		for (size_t i = 0; i < crosses.size() && !merged; i++)
			for (size_t j = i + 1; j < crosses.size() && !merged; j++) {
				std::array<float, 6> &a = crosses[i];
				const std::array<float, 6> &c = crosses[j];
				if (c[0] <= a[1] + 0.3F && a[0] <= c[1] + 0.3F && c[2] <= a[3] + 0.3F && a[2] <= c[3] + 0.3F && c[4] <= a[5] + 0.3F && a[4] <= c[5] + 0.3F) {
					a = { std::min(a[0], c[0]), std::max(a[1], c[1]), std::min(a[2], c[2]), std::max(a[3], c[3]), std::min(a[4], c[4]), std::max(a[5], c[5]) };
					crosses.erase(crosses.begin() + static_cast<ptrdiff_t>(j));
					merged = true;
				}
			}
	}
	if (crosses.empty())
		return;
	// The nave's middle, between its two walls of windows.
	float midX = 0;
	float midY = 0;
	for (const std::array<float, 6> &c : crosses) {
		midX += (c[0] + c[1]) / 2.0F;
		midY += (c[2] + c[3]) / 2.0F;
	}
	midX /= static_cast<float>(crosses.size());
	midY /= static_cast<float>(crosses.size());
	for (const std::array<float, 6> &c : crosses) {
		const V3 middle { (c[0] + c[1]) / 2.0F, (c[2] + c[3]) / 2.0F, (c[4] + c[5]) / 2.0F };
		// Across its wall: the way the window is thinnest.
		const bool acrossX = c[1] - c[0] < c[3] - c[2];
		const float thickness = acrossX ? c[1] - c[0] : c[3] - c[2];
		const float in = acrossX ? (midX > middle.x ? 1.0F : -1.0F) : (midY > middle.y ? 1.0F : -1.0F);
		const float back = thickness / 2.0F + CrossLampBehind;
		const V3 at { middle.x + (acrossX ? in * back : 0.0F), middle.y + (acrossX ? 0.0F : in * back), middle.z + 0.2F * (c[5] - c[4]) };
		const V3 out { acrossX ? -in : 0.0F, acrossX ? 0.0F : -in, 0.0F };
		const Lamp lamp { at, CrossLampStrength * LitLandmarks[lit].strength, lit, -1, out, CrossLampHalf, CrossLampReach };
		Lamps.push_back(lamp);
		CrossLamps.push_back(lamp);
	}
	LogInfo("fp: {}: {} lamps behind its cross windows", LitLandmarks[lit].name, crosses.size());
}

/** @brief The light of the lamps behind the church's cross windows on a side of a window, facing `normal` (0..). */
float CrossLight(V3 p, V3 normal)
{
	const float length = std::sqrt(normal.x * normal.x + normal.y * normal.y + normal.z * normal.z);
	if (length > 0)
		normal = { normal.x / length, normal.y / length, normal.z / length };
	float light = 0.04F;
	for (const Lamp &lamp : CrossLamps) {
		const V3 d { lamp.position.x - p.x, lamp.position.y - p.y, lamp.position.z - p.z };
		const float d2 = d.x * d.x + d.y * d.y + d.z * d.z;
		if (d2 > lamp.reach * lamp.reach)
			continue;
		const float distance = std::sqrt(d2) + 1e-4F;
		// Mostly from the lamp (the sides turned to it brightest), a little bounced round the window. Every side faces
		// into the opening, where the lamp is (whichever way the model wound it).
		const float facing = std::abs(d.x * normal.x + d.y * normal.y + d.z * normal.z) / distance;
		light += 2.2F * lamp.strength * (0.2F + 0.8F * facing) / (1.0F + d2 / (lamp.half * lamp.half * 0.25F));
	}
	return light * (1.0F + LampFlicker * FlickerNow[2]);
}

/**
 * @brief A room fitted to a house from the inside: from a point under its roof, out to its walls each way (the
 * middle of rays at a few heights, so windows do not count), up to under its roof; split at an upper floor if it has
 * one. For the houses without hand-measured rooms.
 */
void AddAutoRoom(Building &b, int lit, const LandmarkPlacement &place)
{
	Vec2 middle { (place.x0 + place.x1) / 2.0F, (place.y0 + place.y1) / 2.0F };
	if (RoofHeightAt(b, middle.x, middle.y) <= 0) {
		float best = std::numeric_limits<float>::max();
		for (float y = place.y0; y <= place.y1; y += 0.25F)
			for (float x = place.x0; x <= place.x1; x += 0.25F)
				if (RoofHeightAt(b, x, y) > 1.0F && std::hypot(x - middle.x, y - middle.y) < best) {
					best = std::hypot(x - middle.x, y - middle.y);
					middle = { x, y };
				}
	}
	std::array<float, 4> wall {};
	for (int side = 0; side < 4; side++) {
		const int axis = side / 2;
		const float sign = side % 2 == 0 ? -1.0F : 1.0F;
		const V3 d = axis == 0 ? V3 { sign, 0, 0 } : V3 { 0, sign, 0 };
		std::vector<float> hits;
		for (const float z : { 0.5F, 0.9F, 1.3F })
			for (const float off : { -0.5F, -0.25F, 0.0F, 0.25F, 0.5F }) {
				const V3 o = axis == 0 ? V3 { middle.x, middle.y + off, z } : V3 { middle.x + off, middle.y, z };
				float t;
				if (SegmentHitsBuilding(b, o, d, 8.0F, t))
					hits.push_back(t);
			}
		if (hits.empty())
			return;
		std::nth_element(hits.begin(), hits.begin() + static_cast<ptrdiff_t>(hits.size() / 2), hits.end());
		wall[side] = hits[hits.size() / 2];
	}
	LitRoom room;
	room.lit = lit;
	room.x0 = middle.x - wall[0] + 0.03F;
	room.x1 = middle.x + wall[1] - 0.03F;
	room.y0 = middle.y - wall[2] + 0.03F;
	room.y1 = middle.y + wall[3] - 0.03F;
	room.z0 = 0.05F;
	room.top = 6.0F;
	FitRoom(b, room);
	std::vector<LitRoom> storeys;
	float floorBottom;
	float floorTop;
	if (FindStoreyFloor(b, room, floorBottom, floorTop)) {
		LitRoom upper = room;
		upper.z0 = floorTop + 0.02F;
		upper.top = 6.0F;
		FitRoom(b, upper);
		room.top = floorBottom - 0.03F;
		FitRoom(b, room);
		storeys.push_back(std::move(room));
		storeys.push_back(std::move(upper));
	} else {
		storeys.push_back(std::move(room));
	}
	for (LitRoom &storey : storeys) {
		LogInfo("fp: {}: room {:.2f}..{:.2f} x {:.2f}..{:.2f}, z {:.2f}..{:.2f}", place.name, storey.x0, storey.x1, storey.y0, storey.y1, storey.z0, storey.top);
		LitRooms.push_back(std::move(storey));
		AddRoomLamps(static_cast<int>(LitRooms.size() - 1));
	}
}

/**
 * @brief The gaps between the planks of a lit landmark's walls (WorldTriangle::slit: Adria's shack) glow with its
 * light, as a window's glass does: the art shows the lamplight inside between them.
 */
void GlowSlits(Building &b, int lit)
{
	for (WorldTriangle &tri : b.mesh) {
		if (!tri.slit)
			continue;
		// Each strip is a quad: both its triangles span its bounds, the middle and half the size of its glow.
		const float x0 = std::min({ tri.p[0].x, tri.p[1].x, tri.p[2].x });
		const float x1 = std::max({ tri.p[0].x, tri.p[1].x, tri.p[2].x });
		const float y0 = std::min({ tri.p[0].y, tri.p[1].y, tri.p[2].y });
		const float y1 = std::max({ tri.p[0].y, tri.p[1].y, tri.p[2].y });
		const float z0 = std::min({ tri.p[0].z, tri.p[1].z, tri.p[2].z });
		const float z1 = std::max({ tri.p[0].z, tri.p[1].z, tri.p[2].z });
		tri.mapping = MeshPane;
		tri.texture = -1;
		tri.cull = false;
		tri.arch = static_cast<int16_t>(lit);
		tri.mirrorX = (x0 + x1) / 2.0F;
		tri.mirrorY = (y0 + y1) / 2.0F;
		tri.downX = (z0 + z1) / 2.0F;
		tri.slopeScale = std::max(x1 - x0, y1 - y0) / 2.0F;
		tri.downY = (z1 - z0) / 2.0F;
	}
}

/** @brief A lit landmark's rooms (or the church's glass box), their insides and their lamps. */
void AddLitRooms(Building &b, int lit, const LandmarkPlacement &place)
{
	GlowSlits(b, lit);
	auto height = [&](float z) {
		return z <= place.eaveIn ? z * place.zScale : place.eaveIn * place.zScale + (z - place.eaveIn) * place.zScaleAbove;
	};
	const size_t firstRoom = LitRooms.size();
	if (LitLandmarks[lit].glass) {
		AddGlassRoom(b, lit, place);
		AddCrossLamps(b, lit);
	}
	if (LitLandmarks[lit].autoRoom)
		AddAutoRoom(b, lit, place);
	for (int r = 0; r < LitLandmarks[lit].rooms; r++) {
		const auto &box = LitLandmarks[lit].room[r];
		auto at = [&](float u, float w) {
			const float ww = place.flipW ? 1.0F - w : w;
			return Vec2 { place.x0 + u * (place.x1 - place.x0), place.y0 + ww * (place.y1 - place.y0) };
		};
		const Vec2 a = at(box[0], box[2]);
		const Vec2 c = at(box[1], box[3]);
		LitRoom room;
		room.lit = lit;
		room.x0 = std::min(a.x, c.x);
		room.x1 = std::max(a.x, c.x);
		room.y0 = std::min(a.y, c.y);
		room.y1 = std::max(a.y, c.y);
		room.z0 = height(box[4]);
		room.top = height(box[5]);
		FitRoom(b, room);
		// Two storeys: a room under the upper floor, and one over it up to the roof.
		std::vector<LitRoom> storeys;
		float floorBottom;
		float floorTop;
		if (FindStoreyFloor(b, room, floorBottom, floorTop)) {
			LitRoom upper = room;
			upper.z0 = floorTop + 0.02F;
			upper.top = height(box[5]);
			FitRoom(b, upper);
			room.top = floorBottom - 0.03F;
			FitRoom(b, room);
			storeys.push_back(std::move(room));
			storeys.push_back(std::move(upper));
		} else {
			storeys.push_back(std::move(room));
		}
		for (LitRoom &storey : storeys) {
			LogInfo("fp: {}: room {:.2f}..{:.2f} x {:.2f}..{:.2f}, z {:.2f}..{:.2f}", place.name, storey.x0, storey.x1, storey.y0, storey.y1, storey.z0, storey.top);
			LitRooms.push_back(std::move(storey));
			AddRoomInside(b, static_cast<int>(LitRooms.size() - 1));
			AddRoomLamps(static_cast<int>(LitRooms.size() - 1));
		}
	}
	if (!LitLandmarks[lit].glass && !LitLandmarks[lit].unglazed) {
		// The windows the model has; a house whose model has none gets a few set into its ground floor's walls.
		std::vector<WindowOpening> glazed = GlazeWindows(b, lit, firstRoom);
		if (LitLandmarks[lit].autoRoom && glazed.empty())
			for (size_t r = firstRoom; r < LitRooms.size(); r++)
				if (LitRooms[r].z0 < 0.5F)
					AddSetWindows(b, lit, LitRooms[r], glazed);
		if (firstRoom < LitRooms.size())
			GlazeDoors(b, lit, LitRooms[firstRoom]);
	}
}

/** @brief The light of a lit room's lamps on a point of its inside, with its inward normal (0: none, ~1: bright). */
float RoomLight(int room, V3 p, V3 normal)
{
	float light = 0.06F;
	for (const Lamp &lamp : Lamps) {
		if (lamp.room != room)
			continue;
		const V3 d { lamp.position.x - p.x, lamp.position.y - p.y, lamp.position.z - p.z };
		const float d2 = d.x * d.x + d.y * d.y + d.z * d.z;
		const float distance = std::sqrt(d2) + 1e-4F;
		const float facing = std::max(0.0F, (d.x * normal.x + d.y * normal.y + d.z * normal.z) / distance);
		// Straight from the lamp, and a little bounced off the room's other surfaces.
		light += lamp.strength * (0.85F * facing + 0.15F) / (1.0F + d2 / (LampHalf * LampHalf * 0.5F));
	}
	// Dim: a few candles and a fire, not daylight.
	return 0.75F * light * (1.0F + LampFlicker * FlickerNow[LitRooms[room].lit]);
}

/** @brief The light of a lit landmark's lamps at a point inside it (its model's own inside: the church's nave). */
float LandmarkInsideLight(int lit, V3 p)
{
	float light = 0.05F;
	for (const Lamp &lamp : Lamps) {
		if (lamp.lit != lit)
			continue;
		const V3 d { lamp.position.x - p.x, lamp.position.y - p.y, lamp.position.z - p.z };
		const float d2 = d.x * d.x + d.y * d.y + d.z * d.z;
		light += 0.45F * lamp.strength / (1.0F + d2 / (LampHalf * LampHalf * 0.35F));
	}
	return light * (1.0F + LampFlicker * FlickerNow[lit]);
}

uint32_t PointHash(float x, float y, float z, float perUnit);

constexpr int PaneLevels24 = 24;
/** How bright the houses' window glass is, against their light's colour (dim, as candle light through old glass). */
constexpr float PaneBrightness = 0.5F;

/**
 * @brief The palette colours of a lit landmark's window glass, PaneLevels24 strengths of its light: dark glass with
 * a glow, its light's colour, and at the brightest a warm white, as the original art's lit windows.
 */
const std::array<uint8_t, PaneLevels24> &PaneRamp(int lit)
{
	static std::array<std::array<uint8_t, PaneLevels24>, LitLandmarks.size()> ramps {};
	static uint32_t readyFor = 0;
	if (readyFor != PaletteVersion()) {
		readyFor = PaletteVersion();
		for (size_t f = 0; f < LitLandmarks.size(); f++) {
			const std::array<float, 3> &c = LitLandmarks[f].color;
			for (int l = 0; l < PaneLevels24; l++) {
				const float t = static_cast<float>(l) / static_cast<float>(PaneLevels24 - 1);
				std::array<float, 3> rgb;
				for (int k = 0; k < 3; k++) {
					constexpr std::array<float, 3> Hot { 255.0F, 214.0F, 140.0F };
					rgb[k] = t < 0.6F ? c[k] * (0.2F + 0.8F * t / 0.6F) : c[k] + (Hot[k] - c[k]) * (t - 0.6F) / 0.4F;
				}
				ramps[f][l] = NearestWarmColor(rgb[0] * PaneBrightness, rgb[1] * PaneBrightness, rgb[2] * PaneBrightness);
			}
		}
	}
	return ramps[std::clamp<size_t>(lit, 0, LitLandmarks.size() - 1)];
}

/** @brief The colour of window glass at a point: its house's light through it, brightest in the middle of each light. */
int PaneColor(const WorldTriangle &tri, float wx, float wy, float wz)
{
	if (!WindowLightOn)
		return NearestColor(22.0F, 24.0F, 34.0F); // dark glass
	const bool alongY = std::abs(tri.normal.x) > std::abs(tri.normal.y);
	const float u = ((alongY ? wy : wx) - (alongY ? tri.mirrorY : tri.mirrorX)) / std::max(tri.slopeScale, 0.01F);
	const float v = (wz - tri.downX) / std::max(tri.downY, 0.01F);
	// Each of the four lights: its own middle, and its own shade (old glass is uneven).
	const float lu = std::abs(u) * 2.0F - 1.0F;
	const float lv = std::abs(v) * 2.0F - 1.0F;
	const uint32_t quarter = PointHash(tri.mirrorX + (u < 0 ? 0.0F : 3.1F), tri.mirrorY + (v < 0 ? 0.0F : 5.3F), tri.downX, 13.0F);
	const float own = (static_cast<float>(quarter & 0xFF) / 255.0F - 0.5F) * 0.14F;
	const float glow = 0.9F - 0.18F * (lu * lu + lv * lv) - 0.12F * (u * u + v * v) + own + 0.08F * FlickerNow[tri.arch];
	const float dither = static_cast<float>(PointHash(wx, wy, wz, 60.0F) & 0xFF) / 256.0F;
	const int level = std::clamp(static_cast<int>(glow * static_cast<float>(PaneLevels24 - 1) + dither), 0, PaneLevels24 - 1);
	return PaneRamp(tri.arch)[static_cast<size_t>(level)];
}

/** @brief Which of LitLandmarks a building is, or -1. */
int LitIndexOf(const Building &b)
{
	if (b.landmark < 0)
		return -1;
	for (size_t i = 0; i < LitLandmarks.size(); i++)
		if (std::string_view(Landmarks[b.landmark].name) == LitLandmarks[i].name)
			return static_cast<int>(i);
	return -1;
}

/** Surfaces inside a lit room: plank floor, timber ceiling, wood panelling low on the walls and plaster above. */
enum InteriorMaterial : uint8_t {
	InteriorFloor,
	InteriorCeiling,
	InteriorPanel,
	InteriorPlaster,
};
constexpr int InteriorLevels = 32;

/** @brief Palette colours of an inside material lit by the lamps, InteriorLevels strengths of their light. */
const std::array<uint8_t, InteriorLevels> &InteriorRamp(int lit, InteriorMaterial material)
{
	static std::array<std::array<std::array<uint8_t, InteriorLevels>, 4>, LitLandmarks.size()> ramps {};
	static uint32_t readyFor = 0;
	if (readyFor != PaletteVersion()) {
		readyFor = PaletteVersion();
		constexpr std::array<std::array<float, 3>, 4> Albedo { { { 0.42F, 0.32F, 0.23F }, { 0.30F, 0.23F, 0.17F }, { 0.36F, 0.27F, 0.19F }, { 0.60F, 0.58F, 0.55F } } };
		for (size_t f = 0; f < LitLandmarks.size(); f++)
			for (size_t m = 0; m < Albedo.size(); m++)
				for (int l = 0; l < InteriorLevels; l++) {
					const float light = 1.6F * static_cast<float>(l) / static_cast<float>(InteriorLevels - 1);
					const std::array<float, 3> &color = LitLandmarks[f].color;
					ramps[f][m][l] = NearestColor(std::min(255.0F, Albedo[m][0] * color[0] * light), std::min(255.0F, Albedo[m][1] * color[1] * light), std::min(255.0F, Albedo[m][2] * color[2] * light));
				}
	}
	return ramps[lit][material];
}

/** @brief A small hash of a point in space, for grain and dithering. */
uint32_t PointHash(float x, float y, float z, float perUnit)
{
	uint32_t h = static_cast<uint32_t>(x * perUnit) * 73856093U ^ static_cast<uint32_t>(y * perUnit) * 19349663U ^ static_cast<uint32_t>(std::max(z, 0.0F) * perUnit) * 83492791U;
	h ^= h >> 13;
	h *= 0x5bd1e995U;
	h ^= h >> 15;
	return h;
}

/** @brief The colour of a lit room's inside at a point: its material, lit by its lamps. */
int InteriorColor(const WorldTriangle &tri, float wx, float wy, float wz)
{
	const LitRoom &room = LitRooms[tri.arch];
	float light = WindowLightOn ? RoomLight(tri.arch, { wx, wy, wz }, tri.normal) : 0.02F;
	InteriorMaterial material;
	if (tri.normal.z > 0.5F) {
		// Planks along the room's length, each a little lighter or darker.
		material = InteriorFloor;
		const bool alongX = room.x1 - room.x0 >= room.y1 - room.y0;
		const float across = alongX ? wy : wx;
		const float along = alongX ? wx : wy;
		const uint32_t plank = PointHash(across * 5.0F, std::floor(along * 0.8F + std::floor(across * 5.0F) * 0.37F), 0, 1.0F);
		light *= 0.85F + 0.3F * static_cast<float>(plank & 0xFF) / 255.0F;
		if (std::abs(across * 5.0F - std::round(across * 5.0F)) < 0.06F)
			light *= 0.6F; // the gaps between the planks
	} else if (tri.normal.z < -0.5F) {
		// Beams across the ceiling.
		material = InteriorCeiling;
		const float along = room.x1 - room.x0 >= room.y1 - room.y0 ? wx : wy;
		if (std::abs(along * 1.6F - std::round(along * 1.6F)) < 0.12F)
			light *= 0.55F;
	} else if (wz - room.z0 < 0.85F) {
		material = InteriorPanel;
		const float along = std::abs(tri.normal.x) > 0.5F ? wy : wx;
		if (std::abs(along * 4.0F - std::round(along * 4.0F)) < 0.05F || std::abs(wz - room.z0 - 0.82F) < 0.03F)
			light *= 0.6F;
	} else {
		material = InteriorPlaster;
		light *= 0.92F + 0.08F * static_cast<float>(PointHash(wx, wy, wz, 9.0F) & 0xFF) / 255.0F;
	}
	const float dither = static_cast<float>(PointHash(wx, wy, wz, 40.0F) & 0xFF) / 256.0F;
	const int level = std::clamp(static_cast<int>(light / 1.6F * static_cast<float>(InteriorLevels - 1) + dither), 0, InteriorLevels - 1);
	return InteriorRamp(room.lit, material)[level];
}

/** @brief The flicker of each lit building now. */
void UpdateWindowFlicker()
{
	const float t = static_cast<float>(SDL_GetTicks()) / 1000.0F;
	for (size_t i = 0; i < LitLandmarks.size(); i++)
		FlickerNow[i] = WindowFlicker(static_cast<int>(i), t);
}

void CullHiddenSurfaces(Building &b);

/**
 * @brief Adds the hand-placed landmark buildings, replacing whatever the footprint detection made of their tiles.
 * Their art is stitched and fitted like any house, for cutting the wall and thatch patches.
 */
void AddLandmarks()
{
	LoadLandmarks();
	QuantizeMeshTextures(TextureDim, true); // the palette is the level's
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
		if (solid * 4 < tiles) { // a quarter: small pieces like the fountain stand on mostly walkable tiles
			LogInfo("fp: landmark {} not found on this town map", place.name);
			continue;
		}
		std::erase_if(Buildings, [&](const Building &b) {
			const bool replaced = b.x0 <= static_cast<float>(place.claimMax.x) && b.x1 > static_cast<float>(place.claimMin.x) && b.y0 <= static_cast<float>(place.claimMax.y)
			    && b.y1 > static_cast<float>(place.claimMin.y);
			// The boulders a replaced house took for its art are props again (unless the landmark claims them).
			if (replaced)
				for (const Point t : b.claimed)
					if (IsTownRockPiece(dPiece[t.x][t.y]))
						BuildingTiles[static_cast<size_t>(t.y) * MAXDUNX + t.x] = 0;
			return replaced;
		});
		Building b {};
		b.x0 = static_cast<float>(place.claimMin.x);
		b.y0 = static_cast<float>(place.claimMin.y);
		b.x1 = static_cast<float>(place.claimMax.x + 1);
		b.y1 = static_cast<float>(place.claimMax.y + 1);
		b.landmark = static_cast<int>(l);
		std::vector<Point> artTiles;
		std::vector<Point> groundTiles;
		std::vector<Point> nearbyArt; // every upright art tile in the margin, for which ones the model replaces
		for (int y = place.claimMin.y - 2; y <= place.claimMax.y + 2; y++) {
			for (int x = place.claimMin.x - 2; x <= place.claimMax.x + 2; x++) {
				if (!InDungeonBounds({ x, y }))
					continue;
				const bool inside = x >= place.claimMin.x && x <= place.claimMax.x && y >= place.claimMin.y && y <= place.claimMax.y;
				if (GetPieceTextures(dPiece[x][y]).hasTallArt)
					nearbyArt.push_back({ x, y });
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
		BuildLandmarkMesh(b, place, *mesh);
		if (place.projective) {
			MakeProjective(b, place.flipW);
			BakeColors(b);
			ClaimProjectiveTiles(b, nearbyArt);
			for (size_t lit = 0; lit < LitLandmarks.size(); lit++) {
				if (std::string_view(place.name) != LitLandmarks[lit].name)
					continue;
				if (LitLandmarks[lit].glass && !mesh->textures.empty())
					GlassTexture = mesh->textures.back(); // the stained glass box behind the nave's windows comes last
				AddLitRooms(b, static_cast<int>(lit), place);
				AddInsideLamp(b, static_cast<int>(lit));
			}
			// Not the church: its stained glass shows through slits finer than the grid, and little else is hidden.
			if (std::getenv("FP_KEEP_INSIDES") == nullptr && std::string_view(place.name) != "church") // debug: keep every surface
				CullHiddenSurfaces(b);
		} else {
			for (const Point t : artTiles)
				BuildingTiles[static_cast<size_t>(t.y) * MAXDUNX + t.x] = 1;
		}
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

void BuildTownProps();
void BuildLampGrids();

/** @brief Whether a colour is one of the art's painted lamplight's: orange and yellow before the tavern, red (down to a dark red) before the church. */
bool IsPaintedLightColor(SDL_Color c)
{
	const bool orange = c.r >= 60 && c.r * 10 >= c.b * 20 && c.g * 10 >= c.b * 12 && c.r >= c.g + 15;
	const bool red = c.r >= 50 && c.r >= 2 * c.g && c.r >= 2 * c.b;
	const bool yellow = c.r >= 70 && c.g >= 50 && 2 * c.b <= c.g && c.r >= c.b + 40;
	return orange || red || yellow;
}

/** @brief The share of a floor's texels in the art's painted lamplight. */
float PaintedLightShare(const std::vector<int16_t> &floor)
{
	int lit = 0;
	int count = 0;
	for (const int16_t texel : floor) {
		if (texel <= 0)
			continue;
		count++;
		if (IsPaintedLightColor(logical_palette[texel]))
			lit++;
	}
	return count > 0 ? static_cast<float>(lit) / static_cast<float>(count) : 0.0F;
}

/** Per town tile: the piece whose floor is drawn there, or 0 for its own (FloorSourceMap). */
std::vector<uint16_t> FloorSources;

/**
 * @brief Picks the floor each town tile shows where its own would be wrong in first person: the tiles a building's
 * art is drawn on or its model stands on, the props' tiles (trees, rocks, walls: their art painted the foot of what
 * stood there into the floor), and the footprints of houses the models do not cover (whose floor art is the foot of
 * the house's walls), and the boulders' feet painted on the ground of the tiles they block. Each takes the floor of
 * the nearest tile of plain open ground, so the ground runs on.
 */
void ChooseFloorSources()
{
	FloorSources.assign(static_cast<size_t>(MAXDUNX) * MAXDUNY, 0);
	auto index = [](int x, int y) { return static_cast<size_t>(y) * MAXDUNX + x; };
	// How bright a floor is: the art paints the houses' shadows on the ground, which with no sun to cast them are
	// only dark stains in first person.
	auto luma = [](int16_t texel) {
		const SDL_Color c = logical_palette[texel];
		return 0.3F * c.r + 0.59F * c.g + 0.11F * c.b;
	};
	std::vector<float> levels;
	for (int y = 0; y < MAXDUNY; y += 3)
		for (int x = 0; x < MAXDUNX; x += 3)
			if (!IsTileSolid({ x, y }) && BuildingTiles[index(x, y)] == 0 && !GetPieceTextures(dPiece[x][y]).hasTallArt)
				for (const int16_t texel : GetPieceTextures(dPiece[x][y]).floor)
					if (texel > 0)
						levels.push_back(luma(texel));
	std::nth_element(levels.begin(), levels.begin() + static_cast<ptrdiff_t>(levels.size() / 2), levels.end());
	const float darkBelow = levels.empty() ? 0.0F : 0.55F * levels[levels.size() / 2];
	// The share of a floor in shadow: its texels much darker than the ground's usual.
	auto shaded = [&](const std::vector<int16_t> &floor) {
		int dark = 0;
		int count = 0;
		for (const int16_t texel : floor) {
			if (texel <= 0)
				continue;
			count++;
			dark += luma(texel) < darkBelow ? 1 : 0;
		}
		return count > 0 ? static_cast<float>(dark) / static_cast<float>(count) : 0.0F;
	};
	constexpr float ShadowShare = 0.3F;
	constexpr float NearBuildingShadowShare = 0.1F; // beside a house: the foot of its walls the art paints there too
	auto open = [&](int x, int y) {
		if (!InDungeonBounds({ x, y }) || IsTileSolid({ x, y }) || BuildingTiles[index(x, y)] != 0)
			return false;
		const PieceTextures &t = GetPieceTextures(dPiece[x][y]);
		return !t.hasTallArt && !t.floor.empty() && PaintedLightShare(t.floor) < 0.02F && shaded(t.floor) < ShadowShare;
	};
	// Near a building (its art or its model's tiles): where a house's footprint can show past its model.
	std::vector<uint8_t> nearBuilding(FloorSources.size(), 0);
	for (int y = 0; y < MAXDUNY; y++)
		for (int x = 0; x < MAXDUNX; x++) {
			const uint8_t mark = BuildingTiles[index(x, y)];
			if (mark != 1 && mark != 2)
				continue;
			for (int dy = -2; dy <= 2; dy++)
				for (int dx = -2; dx <= 2; dx++)
					if (InDungeonBounds({ x + dx, y + dy }))
						nearBuilding[index(x + dx, y + dy)] = 1;
		}
	// The boulders' feet: solid tiles of floor art only, in small groups beside a boulder's tiles. The river's tiles
	// are solid floor art too (some of its pieces the same), but run on in one long chain.
	std::vector<uint8_t> stoneFeet(FloorSources.size(), 0);
	{
		auto flatSolid = [&](int x, int y) {
			return InDungeonBounds({ x, y }) && BuildingTiles[index(x, y)] == 0 && IsTileSolid({ x, y }) && !GetPieceTextures(dPiece[x][y]).hasTallArt;
		};
		auto rock = [&](int x, int y) {
			return InDungeonBounds({ x, y }) && (IsTownRockPiece(dPiece[x][y]) || (BuildingTiles[index(x, y)] == 3 && GetPieceTextures(dPiece[x][y]).hasTallArt && IsTileSolid({ x, y })));
		};
		constexpr size_t LongestFeet = 10;
		std::vector<uint8_t> seen(FloorSources.size(), 0);
		for (int y = 0; y < MAXDUNY; y++)
			for (int x = 0; x < MAXDUNX; x++) {
				if (seen[index(x, y)] != 0 || !flatSolid(x, y))
					continue;
				std::vector<Point> group;
				std::vector<Point> stack { { x, y } };
				seen[index(x, y)] = 1;
				bool besideRock = false;
				while (!stack.empty()) {
					const Point p = stack.back();
					stack.pop_back();
					group.push_back(p);
					for (int dy = -1; dy <= 1; dy++)
						for (int dx = -1; dx <= 1; dx++) {
							besideRock = besideRock || rock(p.x + dx, p.y + dy);
							if ((dx == 0) == (dy == 0) || !flatSolid(p.x + dx, p.y + dy) || seen[index(p.x + dx, p.y + dy)] != 0)
								continue;
							seen[index(p.x + dx, p.y + dy)] = 1;
							stack.push_back({ p.x + dx, p.y + dy });
						}
				}
				if (besideRock && group.size() <= LongestFeet)
					for (const Point p : group)
						stoneFeet[index(p.x, p.y)] = 1;
			}
	}
	int borrowed = 0;
	for (int y = 0; y < MAXDUNY; y++) {
		for (int x = 0; x < MAXDUNX; x++) {
			const PieceTextures &t = GetPieceTextures(dPiece[x][y]);
			const uint8_t mark = BuildingTiles[index(x, y)];
			const bool footprint = nearBuilding[index(x, y)] != 0 && IsTileSolid({ x, y }) && !t.hasTallArt && !t.isFlatSolid;
			const bool shadow = nearBuilding[index(x, y)] != 0 && !t.hasTallArt && !t.isFlatSolid && shaded(t.floor) >= NearBuildingShadowShare;
			// A boulder's foot painted on the ground of a tile it blocks (now a mesh stands there).
			const bool stoneFoot = stoneFeet[index(x, y)] != 0;
			if (mark == 0 && !footprint && !shadow && !stoneFoot)
				continue;
			// The nearest open ground, ring by ring.
			for (int r = 1; r <= 8 && FloorSources[index(x, y)] == 0; r++) {
				for (int dy = -r; dy <= r && FloorSources[index(x, y)] == 0; dy++)
					for (int dx = -r; dx <= r; dx++) {
						if (std::max(std::abs(dx), std::abs(dy)) != r || !open(x + dx, y + dy))
							continue;
						FloorSources[index(x, y)] = dPiece[x + dx][y + dy];
						borrowed++;
						break;
					}
			}
		}
	}
	LogInfo("fp: {} town tiles show the floor of the open ground near them", borrowed);
}

/**
 * @brief Paints out what does not belong on the town's walkable floor: the feet of boulders the art paints into the
 * floor diamonds of the tiles around them (blue stone, now meshes standing on the ground), and the art's painted
 * lamplight and window light (red and orange; the lamps light the ground instead). Small specks stay: the ground's
 * own leaves and stones are specks of these colours too. The river (solid, not a boulder's) keeps its blue.
 */
void CleanTownFloors()
{
	std::array<uint8_t, 256> kind {};
	for (int i = 1; i < 256; i++) {
		const SDL_Color c = logical_palette[i];
		const bool stone = c.b >= c.r + 20 && c.b >= c.g + 15; // (the ground's own blue-grey specks are paler)
		kind[static_cast<size_t>(i)] = stone ? 1 : (IsPaintedLightColor(c) ? 2 : 0);
	}
	constexpr std::array<int, 4> MinCluster { 0, 3, 5, 0 }; // texels: stone, painted light
	CleanGroundFloor(kind, MinCluster); // first: the others' cleaned texels take its
	std::vector<uint8_t> done(65536, 0);
	int pieces = 0;
	auto clean = [&](uint16_t piece) {
		if (piece == 0 || done[piece] != 0)
			return;
		done[piece] = 1;
		CleanTownFloor(piece, kind, MinCluster, GetGroundFloor());
		pieces++;
	};
	for (int y = 0; y < MAXDUNY; y++)
		for (int x = 0; x < MAXDUNX; x++) {
			const uint8_t mark = BuildingTiles[static_cast<size_t>(y) * MAXDUNX + x];
			if (!FloorSources.empty())
				clean(FloorSources[static_cast<size_t>(y) * MAXDUNX + x]);
			// Walkable ground, and the boulders' own tiles (their floor the boulder's foot); not the river's.
			if ((mark == 0 || mark == 3) && (!IsTileSolid({ x, y }) || IsTownRockPiece(dPiece[x][y])))
				clean(dPiece[x][y]);
		}
	LogInfo("fp: the floors of {} town pieces cleaned of stones and painted light", pieces);
}

/**
 * @brief The ground the town's art paints lit, in front of the tavern's door and the church's, is plain ground: the lamps' light
 * falls there instead, from the openings it really comes out of. A piece of ground is painted lit when a good share
 * of its floor is the art's warm lamplight colours.
 */
void ClearPaintedLight()
{
	int cleared = 0;
	for (int y = 0; y < MAXDUNY; y++) {
		for (int x = 0; x < MAXDUNX; x++) {
			uint8_t &mark = BuildingTiles[static_cast<size_t>(y) * MAXDUNX + x];
			const PieceTextures &t = GetPieceTextures(dPiece[x][y]);
			if (mark != 0 || t.hasTallArt || IsTileSolid({ x, y }))
				continue;
			if (PaintedLightShare(t.floor) >= 0.04F) {
				mark = 2;
				cleared++;
			}
		}
	}
	LogInfo("fp: {} tiles of painted lamplight on the ground made plain ground", cleared);
}

void BuildAll()
{
	LightBuilder.Join(); // it reads the buildings
	LightReady = false;
	Buildings.clear();
	Lamps.clear();
	CrossLamps.clear();
	LitRooms.clear();
	LightGrids.clear();
	GlassTexture = -1;
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
			// Only trees (a dense crown passes for wall art): no house, though solid ground beside it (the stream by
			// the tree south-east of the town) would make it a footprint.
			if (std::all_of(art.begin(), art.end(), [](Point p) { return IsTreePiece(dPiece[p.x][p.y]); }))
				continue;
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
			chosen->building.claimed = chosen->artTiles;
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
	BuildTownProps();
	LightBuilder.thread = std::thread([] {
		BuildLampGrids();
		LightReady = true;
	});
	ClearPaintedLight();
	ChooseFloorSources();
	CleanTownFloors();

}

void EnsureBuilt()
{
	NotePaletteChange();
	const LevelKey key { pDungeonCels.get(), currlevel, static_cast<int>(leveltype), setlevel, PaletteVersion() };
	if (key == BuiltFor)
		return;
	BuiltFor = key;
	BuildAll();
}

/** @brief Screen rectangle that can contain the building; false if it is entirely off screen. */
bool ScreenBounds(const Building &b, const Projection &proj, int &left, int &top, int &right, int &bottom, float stretch = 1.0F)
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
				// The structure's heights are in art units, stretched when drawn (DrawStructure's `stretch`).
				const float sy = static_cast<float>(proj.horizon) - (wz * stretch - EyeHeight) / depth * proj.focal;
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
		const uint8_t shaded = FpLightTable(LightAt(tile))[color];
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

/** The eye of the view being rasterized, and the world size of a screen pixel at the pixel being shaded. */
V3 ShadeEye { 0, 0, 0 };
/** MeshColor's texels per world unit for the triangle it last shaded, taking its slant into account. */
const WorldTriangle *SlantFor = nullptr;
float SlantTexelsPerWorld = 0;

/** @brief Palette colour of a mesh surface point: the house's own art patches tiled at the art's pixel density. */
uint8_t MeshColor(const Building &b, const WorldTriangle &tri, float wx, float wy, float wz);
/**
 * @brief MeshColor without the model's textures or the baked colours: the art projected onto the point, or its patches
 * (`fineCanvas`: see SampleArt).
 */
uint8_t ArtColor(const Building &b, const WorldTriangle &tri, float wx, float wy, float wz, const std::vector<int16_t> *fineCanvas = nullptr);

/** @brief The two coordinates of a point kept when dropping an axis. */
inline std::pair<float, float> DropAxis(uint8_t axis, float x, float y, float z)
{
	return axis == 0 ? std::pair { y, z } : (axis == 1 ? std::pair { x, z } : std::pair { x, y });
}

/** @brief Samples the baked colours of a landmark's triangles, spread over the threads. */
void SampleBakedColors(Building &b)
{
	const float pixelWas = ShadePixelWorld;
	ShadePixelWorld = 0; // the art at its finest, whatever the view drew last
	PatchColor(0); // its table is made on first use, not by the threads
	const std::vector<int16_t> *fineCanvas = b.canvas.empty() ? nullptr : &UpscaledTexture(b.canvas, b.canvasWidth); // nor the upscaled canvas
	ParallelFor(b.mesh.size(), [&](size_t t) {
		const WorldTriangle &tri = b.mesh[t];
		if (tri.bakeN == 0)
			return;
		const int size = tri.bakeN;
		const V3 &p0 = tri.p[0];
		const V3 e1 { tri.p[1].x - p0.x, tri.p[1].y - p0.y, tri.p[1].z - p0.z };
		const V3 e2 { tri.p[2].x - p0.x, tri.p[2].y - p0.y, tri.p[2].z - p0.z };
		for (int j = 0; j < size; j++) {
			for (int i = 0; i + j < size; i++) {
				const float u = (static_cast<float>(i) + 0.33F) / static_cast<float>(size);
				const float v = (static_cast<float>(j) + 0.33F) / static_cast<float>(size);
				b.baked[tri.bakeOffset + static_cast<size_t>(j) * size + i] = ArtColor(b, tri, p0.x + e1.x * u + e2.x * v, p0.y + e1.y * u + e2.y * v, p0.z + e1.z * u + e2.z * v, fineCanvas);
			}
		}
	});
	ShadePixelWorld = pixelWas;
}

/**
 * @brief Bakes the colours of a projective landmark's triangles, at about the art's 32 pixels per tile: the art
 * projected onto them (what those wearing a model texture show when model textures are switched off).
 */
void BakeColors(Building &b)
{
	b.baked.clear();
	for (WorldTriangle &tri : b.mesh) {
		const V3 &p0 = tri.p[0];
		const V3 e1 { tri.p[1].x - p0.x, tri.p[1].y - p0.y, tri.p[1].z - p0.z };
		const V3 e2 { tri.p[2].x - p0.x, tri.p[2].y - p0.y, tri.p[2].z - p0.z };
		const V3 n { e1.y * e2.z - e1.z * e2.y, e1.z * e2.x - e1.x * e2.z, e1.x * e2.y - e1.y * e2.x };
		const float ax = std::abs(n.x);
		const float ay = std::abs(n.y);
		const float az = std::abs(n.z);
		tri.bakeAxis = ax >= ay && ax >= az ? 0 : (ay >= az ? 1 : 2);
		const auto [a1, b1] = DropAxis(tri.bakeAxis, e1.x, e1.y, e1.z);
		const auto [a2, b2] = DropAxis(tri.bakeAxis, e2.x, e2.y, e2.z);
		const float det = a1 * b2 - a2 * b1;
		if (std::abs(det) < 1e-9F) {
			tri.bakeN = 0;
			continue;
		}
		tri.bakeInv = { b2 / det, -a2 / det, -b1 / det, a1 / det };
		const float worldArea = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
		if (tri.texture >= 0 && static_cast<size_t>(tri.texture) < MeshTextures.size() && worldArea > 1e-9F) {
			const MeshTexture &tex = MeshTextures[tri.texture];
			const float uvArea = std::abs((tri.uv[2] - tri.uv[0]) * (tri.uv[5] - tri.uv[1]) - (tri.uv[4] - tri.uv[0]) * (tri.uv[3] - tri.uv[1]));
			tri.texelDensity = std::sqrt(uvArea * static_cast<float>(tex.width) * static_cast<float>(tex.height) / worldArea);
			tri.unitNormal = { n.x / worldArea, n.y / worldArea, n.z / worldArea };
		}
		const float longest = std::max({ std::sqrt(e1.x * e1.x + e1.y * e1.y + e1.z * e1.z), std::sqrt(e2.x * e2.x + e2.y * e2.y + e2.z * e2.z),
		    std::hypot(e2.x - e1.x, e2.y - e1.y, e2.z - e1.z) });
		const int size = std::clamp(static_cast<int>(std::ceil(longest * 32.0F)), 2, 192);
		tri.bakeN = static_cast<uint16_t>(size);
		tri.bakeOffset = static_cast<uint32_t>(b.baked.size());
		b.baked.resize(b.baked.size() + static_cast<size_t>(size) * size, 0);
	}
	SampleBakedColors(b);
}

uint8_t MeshColor(const Building &b, const WorldTriangle &tri, float wx, float wy, float wz)
{
	if (ModelTexturesOn && tri.texture >= 0 && tri.bakeN > 0 && static_cast<size_t>(tri.texture) < MeshTextures.size()) {
		const auto [da, db] = DropAxis(tri.bakeAxis, wx - tri.p[0].x, wy - tri.p[0].y, wz - tri.p[0].z);
		const float w1 = tri.bakeInv[0] * da + tri.bakeInv[1] * db;
		const float w2 = tri.bakeInv[2] * da + tri.bakeInv[3] * db;
		const float w0 = 1.0F - w1 - w2;
		const float u = w0 * tri.uv[0] + w1 * tri.uv[2] + w2 * tri.uv[4];
		const float v = w0 * tri.uv[1] + w1 * tri.uv[3] + w2 * tri.uv[5];
		// Texels a screen pixel covers: the world size of a pixel at this distance, larger where the surface is seen
		// at a slant (by the square root: between the stretched and the unstretched direction, a little sharp). The
		// slant is taken once per triangle, at its middle.
		if (SlantFor != &tri) {
			SlantFor = &tri;
			const V3 view { (tri.p[0].x + tri.p[1].x + tri.p[2].x) / 3.0F - ShadeEye.x, (tri.p[0].y + tri.p[1].y + tri.p[2].y) / 3.0F - ShadeEye.y,
				(tri.p[0].z + tri.p[1].z + tri.p[2].z) / 3.0F - ShadeEye.z };
			const float viewLength = std::sqrt(view.x * view.x + view.y * view.y + view.z * view.z);
			const float facing = viewLength > 1e-6F ? std::abs(view.x * tri.unitNormal.x + view.y * tri.unitNormal.y + view.z * tri.unitNormal.z) / viewLength : 1.0F;
			SlantTexelsPerWorld = tri.texelDensity / std::sqrt(std::max(facing, 0.1F));
		}
		const float footprint = SlantTexelsPerWorld * ShadePixelWorld;
		return MeshTexel(MeshTextures[tri.texture], u, v, footprint);
	}
	if (tri.bakeN > 0) {
		const auto [da, db] = DropAxis(tri.bakeAxis, wx - tri.p[0].x, wy - tri.p[0].y, wz - tri.p[0].z);
		const float u = tri.bakeInv[0] * da + tri.bakeInv[1] * db;
		const float v = tri.bakeInv[2] * da + tri.bakeInv[3] * db;
		const int n = tri.bakeN;
		const int j = std::clamp(static_cast<int>(v * static_cast<float>(n)), 0, n - 1);
		const int i = std::clamp(static_cast<int>(u * static_cast<float>(n)), 0, n - 1 - j);
		return b.baked[tri.bakeOffset + static_cast<size_t>(j) * n + i];
	}
	return ArtColor(b, tri, wx, wy, wz);
}

uint8_t ArtColor(const Building &b, const WorldTriangle &tri, float wx, float wy, float wz, const std::vector<int16_t> *fineCanvas)
{
	const SurfaceTexture &patch = tri.roof ? b.roofPatch : b.wallPatch;
	const float up = tri.roof ? 32.0F : HeightPixels;
	static const bool flatShade = std::getenv("FP_FLAT_SHADE") != nullptr; // debug: time the rasterizing alone
	if (flatShade)
		return 100;
	if (tri.mapping == MeshProjective) {
		// The art where the iso camera saw this point; else at its mirror image; else the wall patch.
		float x = wx;
		float y = wy;
		for (int attempt = 0; attempt < 2; attempt++) {
			if (attempt == 1 || tri.mirrored) {
				if (tri.normalX == 0 && tri.normalY == 0)
					break;
				const float d = (wx - tri.mirrorX) * tri.normalX + (wy - tri.mirrorY) * tri.normalY;
				x = wx - 2.0F * d * tri.normalX;
				y = wy - 2.0F * d * tri.normalY;
			}
			if (IsoVisible(b, x, y, wz)) {
				const int16_t art = SampleArt(b, x, y, wz, fineCanvas);
				if (art > 0)
					return PatchColor(art);
			}
			if (tri.mirrored)
				break;
		}
		if (b.wallPatch.valid)
			return ScatteredTexel(b.wallPatch, (std::abs(tri.normalX) > std::abs(tri.normalY) ? wy : wx) * 32.0F, wz * HeightPixels);
		return 0;
	}
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
/** Debug counters: pixels inside triangles, pixels passing the depth test, triangles. */
std::array<uint64_t, 3> RasterStats {};

template <typename ShadeFn>
void RasterizeTriangles(const Surface &out, const Projection &proj, std::vector<float> &depthBuffer, const std::vector<WorldTriangle> &triangles, ShadeFn shade, float stretch = 1.0F,
    const std::vector<uint32_t> *order = nullptr)
{
	constexpr float Near = 0.05F;
	const float halfWidth = static_cast<float>(proj.width) / 2.0F;
	auto toCamera = [&](const V3 &w) {
		const float rx = w.x - proj.pos.x;
		const float ry = w.y - proj.pos.y;
		return CameraVertex { rx * proj.dir.x + ry * proj.dir.y, (rx * proj.plane.x + ry * proj.plane.y) / (PlaneLength * PlaneLength), w.z * stretch - EyeHeight, w };
	};
	auto lerp = [](const CameraVertex &a, const CameraVertex &c, float t) {
		return CameraVertex { a.depth + (c.depth - a.depth) * t, a.lateral + (c.lateral - a.lateral) * t, a.height + (c.height - a.height) * t,
			{ a.world.x + (c.world.x - a.world.x) * t, a.world.y + (c.world.y - a.world.y) * t, a.world.z + (c.world.z - a.world.z) * t } };
	};
	// Culling in drawn space: the eye stands lower relative to stretched geometry.
	const V3 eye { proj.pos.x, proj.pos.y, EyeHeight / stretch };
	ShadeEye = eye;
	SlantFor = nullptr;
	static const bool NoCull = std::getenv("FP_NO_CULL") != nullptr; // debug
	const size_t triangleCount = order != nullptr ? order->size() : triangles.size();
	RasterStats[2] += triangleCount;
	for (size_t n = 0; n < triangleCount; n++) {
		const WorldTriangle &tri = triangles[order != nullptr ? (*order)[n] : n];
		if (tri.cull && !NoCull && tri.normal.x * (tri.p[0].x - eye.x) + tri.normal.y * (tri.p[0].y - eye.y) + tri.normal.z * (tri.p[0].z - eye.z) > 0)
			continue;
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
			// The barycentric weights change linearly along a row: work out the span inside the triangle per row
			// instead of testing the whole bounding box (long thin triangles have huge ones).
			const float dw0 = (v1.y - v2.y) * invArea;
			const float dw1 = (v2.y - v0.y) * invArea;
			const float dw2 = -dw0 - dw1;
			for (int y = top; y <= bottom; y++) {
				const float py = static_cast<float>(y) + 0.5F;
				const float base = static_cast<float>(left) + 0.5F;
				const float b0 = ((v1.x - base) * (v2.y - py) - (v1.y - py) * (v2.x - base)) * invArea;
				const float b1 = ((v2.x - base) * (v0.y - py) - (v2.y - py) * (v0.x - base)) * invArea;
				const float b2 = 1.0F - b0 - b1;
				float from = 0.0F;
				float to = static_cast<float>(right - left);
				bool empty = false;
				for (const auto &[b, d] : { std::pair { b0, dw0 }, std::pair { b1, dw1 }, std::pair { b2, dw2 } }) {
					if (std::abs(d) < 1e-12F) {
						empty = empty || b < 0.0F;
						continue;
					}
					const float root = -b / d; // where this weight crosses zero
					if (d > 0)
						from = std::max(from, root);
					else
						to = std::min(to, root);
				}
				if (empty || from > to)
					continue;
				const int xStart = left + std::max(0, static_cast<int>(std::floor(from)));
				const int xEnd = std::min(right, left + static_cast<int>(std::ceil(to)));
				for (int x = xStart; x <= xEnd; x++) {
					const float k = static_cast<float>(x - left);
					const float w0 = b0 + dw0 * k;
					const float w1 = b1 + dw1 * k;
					const float w2 = 1.0F - w0 - w1;
					if (w0 < 0.0F || w1 < 0.0F || w2 < 0.0F)
						continue;
					RasterStats[0]++;
					const float inv = w0 * v0.invDepth + w1 * v1.invDepth + w2 * v2.invDepth;
					const float distance = 1.0F / inv;
					float &stored = depthBuffer[static_cast<size_t>(y) * proj.width + x];
					if (distance >= stored)
						continue;
					const float wx = (w0 * v0.worldOverDepth.x + w1 * v1.worldOverDepth.x + w2 * v2.worldOverDepth.x) * distance;
					const float wy = (w0 * v0.worldOverDepth.y + w1 * v1.worldOverDepth.y + w2 * v2.worldOverDepth.y) * distance;
					const float wz = (w0 * v0.worldOverDepth.z + w1 * v1.worldOverDepth.z + w2 * v2.worldOverDepth.z) * distance;
					RasterStats[1]++;
					ShadePixelWorld = distance / proj.focal;
					ShadeX = x;
					ShadeY = y;
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
	// Big models (most of the landmarks): front to back, so the depth test turns hidden pixels away before their
	// colour is looked up. Sorting a few hundred triangles costs far less than shading what hides behind them.
	static std::vector<uint32_t> sorted;
	const std::vector<uint32_t> *order = nullptr;
	if (b.mesh.size() > 600) {
		static std::vector<std::pair<float, uint32_t>> byDistance;
		byDistance.clear();
		for (uint32_t i = 0; i < b.mesh.size(); i++) {
			const WorldTriangle &tri = b.mesh[i];
			const float dx = (tri.p[0].x + tri.p[1].x + tri.p[2].x) / 3.0F - proj.pos.x;
			const float dy = (tri.p[0].y + tri.p[1].y + tri.p[2].y) / 3.0F - proj.pos.y;
			byDistance.emplace_back(dx * dx + dy * dy, i);
		}
		std::sort(byDistance.begin(), byDistance.end());
		sorted.clear();
		for (const auto &[distance, i] : byDistance)
			sorted.push_back(i);
		order = &sorted;
	}
	const int lit = LitIndexOf(b);
	static uint32_t woodFor = 0;
	static uint8_t wood = 0;
	if (woodFor != PaletteVersion()) {
		woodFor = PaletteVersion();
		wood = NearestColor(64.0F, 44.0F, 30.0F); // window frames: dark oak
	}
	RasterizeTriangles(out, proj, depthBuffer, b.mesh, [&](const WorldTriangle &tri, float wx, float wy, float wz) {
		if (tri.mapping == MeshInterior)
			return InteriorColor(tri, wx, wy, wz);
		if (tri.mapping == MeshPane)
			return PaneColor(tri, wx, wy, wz);
		if (tri.mapping == MeshFrame) {
			const int tx = std::clamp(static_cast<int>(std::floor(wx)), lightMin.x, lightMax.x);
			const int ty = std::clamp(static_cast<int>(std::floor(wy)), lightMin.y, lightMax.y);
			return static_cast<int>(FpLightTable(light[static_cast<size_t>(ty - lightMin.y) * lightWidth + tx - lightMin.x])[ShadeColor(wood, tri.darkness)]);
		}
		const uint8_t color = MeshColor(b, tri, wx, wy, wz);
		if (tri.inside && lit >= 0) {
			// Its inside, seen through the open door: its own colours in the light of its lamps, not the night's.
			const float light = WindowLightOn ? LandmarkInsideLight(lit, { wx, wy, wz }) : 0.03F;
			const int level = std::clamp(static_cast<int>(std::lround((1.0F - std::min(light, 1.0F)) * static_cast<float>(LightsMax))), 0, static_cast<int>(LightsMax));
			const bool textured = ModelTexturesOn && tri.texture >= 0 && tri.bakeN > 0;
			const uint8_t base = FpLightTable(level)[ShadeColor(color, textured ? tri.textureDarkness : tri.darkness)];
			return static_cast<int>(GlowColors(lit, std::clamp(static_cast<int>(light * 10.0F), 0, GlowLevels - 1))[base]);
		}
		if (tri.rim && WindowLightOn && lit >= 0) {
			// A side of a cross window: its stone in the light of the lamp behind it.
			const float light = CrossLight({ wx, wy, wz }, tri.normal);
			const int level = std::clamp(static_cast<int>(std::lround((1.0F - std::min(light, 1.0F)) * static_cast<float>(LightsMax))), 0, static_cast<int>(LightsMax));
			const bool textured = ModelTexturesOn && tri.texture >= 0 && tri.bakeN > 0;
			const uint8_t base = FpLightTable(level)[ShadeColor(color, textured ? tri.textureDarkness : tri.darkness)];
			return static_cast<int>(GlowColors(lit, std::clamp(static_cast<int>(light * 10.0F), 0, GlowLevels - 1))[base]);
		}
		if (tri.texture == GlassTexture && GlassTexture >= 0 && WindowLightOn && ModelTexturesOn) {
			// The church's stained glass, lit from inside: its own colours, brighter, flickering with the church's light.
			const uint32_t hash = static_cast<uint32_t>(wx * 97.0F) * 2654435761U ^ static_cast<uint32_t>(wz * 89.0F) * 40503U ^ static_cast<uint32_t>(wy * 101.0F);
			return static_cast<int>(GlassGlow(FlickerStep(FlickerNow[2], PaneFlicker, hash >> 8))[color]);
		}
		const int tx = std::clamp(static_cast<int>(std::floor(wx)), lightMin.x, lightMax.x);
		const int ty = std::clamp(static_cast<int>(std::floor(wy)), lightMin.y, lightMax.y);
		const uint8_t level = light[static_cast<size_t>(ty - lightMin.y) * lightWidth + tx - lightMin.x];
		const bool textured = ModelTexturesOn && tri.texture >= 0 && tri.bakeN > 0;
		return static_cast<int>(FpLightTable(level)[ShadeColor(color, textured ? tri.textureDarkness : tri.darkness)]);
	}, 1.0F, order);
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
			*out.at(x, y) = FpLightTable(light)[color & 0xFF];
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
int StructuresBuildCount = 0; // bumped on every rebuild, to refresh per-level caches

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
	tri.cull = true;
	tri.normal = normal;
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
/**
 * @param shaftTop if given, only the symmetric lower part is read (the shaft under an arch end), and the height
 * where it ends is returned here
 */
std::vector<ColumnSlice> ColumnProfile(const PieceTextures &textures, int &axisTwice, float *shaftTop = nullptr)
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
	if (shaftTop != nullptr)
		middles.resize(std::max<size_t>(1, middles.size() * 2 / 5)); // the shaft's rows, at the bottom
	std::nth_element(middles.begin(), middles.begin() + static_cast<ptrdiff_t>(middles.size() / 2), middles.end());
	axisTwice = middles[middles.size() / 2];
	int asymmetricRun = 0;
	for (const RowSpan &span : spans) {
		const int leftHalf = axisTwice - 2 * span.first;   // twice the half widths
		const int rightHalf = 2 * (span.last + 1) - axisTwice;
		const bool lopsided = std::abs(leftHalf - rightHalf) > 12;
		if (lopsided)
			asymmetric++;
		if (shaftTop != nullptr) {
			asymmetricRun = lopsided ? asymmetricRun + 1 : 0;
			if (asymmetricRun >= 3) {
				widths.resize(widths.size() >= 2 ? widths.size() - 2 : 0);
				asymmetric = 0;
				break;
			}
		}
		widths.emplace_back(span.z, static_cast<float>(std::max(leftHalf, rightHalf)) / 2.0F);
	}
	if (shaftTop != nullptr)
		*shaftTop = widths.empty() ? 0.0F : widths.back().first;
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
bool BuildColumnSlices(Structure &st, Point tile, const std::vector<ColumnSlice> &slices, int axisTwice);

/** @brief Sets up a triangle for looking up its texture coordinates at a point (barycentric weights from two axes). */
void PrepareUvLookup(WorldTriangle &tri)
{
	const V3 &p0 = tri.p[0];
	const V3 e1 { tri.p[1].x - p0.x, tri.p[1].y - p0.y, tri.p[1].z - p0.z };
	const V3 e2 { tri.p[2].x - p0.x, tri.p[2].y - p0.y, tri.p[2].z - p0.z };
	const V3 n { e1.y * e2.z - e1.z * e2.y, e1.z * e2.x - e1.x * e2.z, e1.x * e2.y - e1.y * e2.x };
	const float ax = std::abs(n.x);
	const float ay = std::abs(n.y);
	const float az = std::abs(n.z);
	tri.bakeAxis = ax >= ay && ax >= az ? 0 : (ay >= az ? 1 : 2);
	const auto [a1, b1] = DropAxis(tri.bakeAxis, e1.x, e1.y, e1.z);
	const auto [a2, b2] = DropAxis(tri.bakeAxis, e2.x, e2.y, e2.z);
	const float det = a1 * b2 - a2 * b1;
	if (std::abs(det) < 1e-12F) {
		tri.texture = -1;
		return;
	}
	tri.bakeInv = { b2 / det, -a2 / det, -b1 / det, a1 / det };
}

/** @brief A triangle's texture position at a point of it, from its corners' (`uv`, set up by PrepareUvLookup). */
inline std::pair<float, float> TriangleUv(const WorldTriangle &tri, float wx, float wy, float wz)
{
	const auto [da, db] = DropAxis(tri.bakeAxis, wx - tri.p[0].x, wy - tri.p[0].y, wz - tri.p[0].z);
	const float w1 = tri.bakeInv[0] * da + tri.bakeInv[1] * db;
	const float w2 = tri.bakeInv[2] * da + tri.bakeInv[3] * db;
	const float w0 = 1.0F - w1 - w2;
	return { w0 * tri.uv[0] + w1 * tri.uv[2] + w2 * tri.uv[4], w0 * tri.uv[1] + w1 * tri.uv[3] + w2 * tri.uv[5] };
}

/** @brief Texture colour of a kit-textured triangle at a point; -1 if it has none. */
int KitMeshColor(const WorldTriangle &tri, float wx, float wy, float wz)
{
	if (tri.texture < 0 || static_cast<size_t>(tri.texture) >= MeshTextures.size())
		return -1;
	const auto [da, db] = DropAxis(tri.bakeAxis, wx - tri.p[0].x, wy - tri.p[0].y, wz - tri.p[0].z);
	const float w1 = tri.bakeInv[0] * da + tri.bakeInv[1] * db;
	const float w2 = tri.bakeInv[2] * da + tri.bakeInv[3] * db;
	const float w0 = 1.0F - w1 - w2;
	const float u = w0 * tri.uv[0] + w1 * tri.uv[2] + w2 * tri.uv[4];
	const float v = w0 * tri.uv[1] + w1 * tri.uv[3] + w2 * tri.uv[5];
	const MeshTexture &tex = MeshTextures[tri.texture];
	const int x = static_cast<int>((u - std::floor(u)) * static_cast<float>(tex.width)) & (tex.width - 1);
	const int y = static_cast<int>((std::ceil(v) - v) * static_cast<float>(tex.height)) & (tex.height - 1);
	return tex.indexed[static_cast<size_t>(y) * tex.width + x];
}

/**
 * @brief Adds a kit model with its u and w axes along axisU and axisW, spanning sizeU by sizeW around c and height up
 * from z0. If given, move takes each world point to where it is drawn (a lid lifted off its box), and finish is called
 * with each triangle added and the index of the model's triangle it came from. The model must be closed and wound the
 * same way all over (the kit's sarcophagus pieces are): its triangles face out by their winding and are one-sided.
 */
void AddKitModelOriented(Building &b, const LandmarkMesh &mesh, Vec2 c, Vec2 axisU, Vec2 axisW, float sizeU, float sizeW, float z0, float height, bool ownTextures,
    const std::function<V3(V3)> &move = nullptr, const std::function<void(WorldTriangle &, size_t)> &finish = nullptr)
{
	float modelHeight = 0;
	for (const auto &t : mesh.triangles)
		for (int k = 0; k < 3; k++)
			modelHeight = std::max(modelHeight, t[static_cast<size_t>(k) * 3 + 2]);
	if (modelHeight <= 0)
		return;
	auto toWorld = [&](const float *q) {
		const float u = (q[0] - 0.5F) * sizeU;
		const float w = (q[1] - 0.5F) * sizeW;
		const V3 p { c.x + axisU.x * u + axisW.x * w, c.y + axisU.y * u + axisW.y * w, z0 + q[2] / modelHeight * height };
		return move ? move(p) : p;
	};
	std::vector<std::array<V3, 3>> world(mesh.triangles.size());
	// Which way the triangles wind seen from outside (laying the model down may mirror it): by the sign of the volume
	// they enclose, measured around the model's middle for precision.
	double volume = 0;
	for (size_t t = 0; t < mesh.triangles.size(); t++) {
		world[t] = { toWorld(&mesh.triangles[t][0]), toWorld(&mesh.triangles[t][3]), toWorld(&mesh.triangles[t][6]) };
		std::array<V3, 3> r = world[t];
		for (V3 &p : r)
			p = { p.x - c.x, p.y - c.y, p.z - z0 };
		volume += static_cast<double>(r[0].x) * (r[1].y * r[2].z - r[1].z * r[2].y) + static_cast<double>(r[0].y) * (r[1].z * r[2].x - r[1].x * r[2].z)
		    + static_cast<double>(r[0].z) * (r[1].x * r[2].y - r[1].y * r[2].x);
	}
	const float outward = volume < 0 ? -1.0F : 1.0F;
	for (size_t t = 0; t < mesh.triangles.size(); t++) {
		const auto &[p0, p1, p2] = world[t];
		const V3 e1 { p1.x - p0.x, p1.y - p0.y, p1.z - p0.z };
		const V3 e2 { p2.x - p0.x, p2.y - p0.y, p2.z - p0.z };
		const V3 n { (e1.y * e2.z - e1.z * e2.y) * outward, (e1.z * e2.x - e1.x * e2.z) * outward, (e1.x * e2.y - e1.y * e2.x) * outward };
		const size_t before = b.mesh.size();
		AddStructureFace(b, { p0, p1, p2 }, n, c, MeshProjective);
		if (b.mesh.size() == before)
			continue;
		WorldTriangle &tri = b.mesh.back();
		if (ownTextures && !mesh.textures.empty()) {
			tri.texture = mesh.textures[t];
			tri.uv = mesh.uvs[t];
			PrepareUvLookup(tri);
		}
		if (finish)
			finish(tri, t);
	}
}

/** @brief Where a column's axis stands, given its art's middle (twice, in px): off the tile's middle by d px on screen is off by d / 32 in x - y. */
Vec2 ColumnAxis(Point tile, int axisTwice)
{
	const float offset = static_cast<float>(axisTwice - ColumnWidth) / 2.0F / 64.0F;
	return { static_cast<float>(tile.x) + 0.5F + offset, static_cast<float>(tile.y) + 0.5F - offset };
}

/** @brief Whether the object at `tile` wears the second version of its texture (scattered, so neighbours differ). */
bool SecondVersionAt(Point tile)
{
	const uint32_t hash = (static_cast<uint32_t>(tile.x) * 73856093U) ^ (static_cast<uint32_t>(tile.y) * 19349663U);
	return ((hash >> 4) & 1) != 0;
}

/**
 * Torch stands' ivory bases: half their side and their height (art units), a broad low pyramid (its sides about 30
 * degrees from the floor as drawn), and how much darker (half light levels) the sides turned away from the light are,
 * the art's light coming from the left (-x, +y).
 */
constexpr float StandBaseHalf = 0.25F;
constexpr float StandBaseHeight = 0.08F;
constexpr uint8_t StandShadowSide = 2;

/**
 * @brief A low square pyramid of a clean texture (a torch stand's base): half side `half` on the floor, cut off at
 * `topHalf` (where the pole comes out, a flat top there) `height` up; lit from the left as the art is.
 */
void AddCleanPyramid(Building &b, Vec2 c, float half, float topHalf, float height, CleanSurface surface)
{
	const float slope = (half - topHalf) / height;
	const float len = std::hypot(1.0F, slope);
	// The faces added since `first`: in the surface, laid on from above (the sides are low), `darkness` darker.
	auto finish = [&](size_t first, uint8_t darkness) {
		for (size_t t = first; t < b.mesh.size(); t++) {
			WorldTriangle &tri = b.mesh[t];
			for (int k = 0; k < 3; k++) {
				tri.uv[k * 2] = tri.p[k].x;
				tri.uv[k * 2 + 1] = tri.p[k].y;
			}
			tri.cleanSurface = surface;
			tri.darkness = darkness;
			PrepareUvLookup(tri);
		}
	};
	for (const auto &[dx, dy] : { std::pair { 1.0F, 0.0F }, std::pair { 0.0F, 1.0F }, std::pair { -1.0F, 0.0F }, std::pair { 0.0F, -1.0F } }) {
		const float sx = -dy; // along the side
		const float sy = dx;
		auto corner = [&](float h, float along, float z) { return V3 { c.x + dx * h + sx * along * h, c.y + dy * h + sy * along * h, z }; };
		const std::array<V3, 4> p { corner(half, -1, 0), corner(half, 1, 0), corner(topHalf, 1, height), corner(topHalf, -1, height) };
		const size_t first = b.mesh.size();
		AddStructureFace(b, { p[0], p[1], p[2], p[3] }, { dx / len, dy / len, slope / len }, c, MeshClean);
		finish(first, dx > 0 || dy < 0 ? StandShadowSide : 0);
	}
	// The cut-off tip, round the pole's foot.
	const size_t first = b.mesh.size();
	AddStructureFace(b, { V3 { c.x - topHalf, c.y - topHalf, height }, V3 { c.x + topHalf, c.y - topHalf, height }, V3 { c.x + topHalf, c.y + topHalf, height }, V3 { c.x - topHalf, c.y + topHalf, height } }, { 0, 0, 1 }, c, MeshClean);
	finish(first, 0);
}

/**
 * @brief Builds a column's mesh at its tile from the profile of its art. With `ivoryBase` (the clean dungeon's torch
 * stands), the base the art shows under the pole (read as round, with the floor around it, it comes out a wide flared
 * blob) is a low square pyramid of the texture set's ivory instead, broad and flat, the pole going down into it.
 */
bool BuildColumn(Structure &st, Point tile, bool ivoryBase = false)
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
	// Nothing reaches above the ceiling: the art's upper rows there are the vault the column carries.
	std::vector<ColumnSlice> clamped;
	for (ColumnSlice slice : slices) {
		if (slice.z0 >= WallHeight())
			break;
		if (slice.z1 > WallHeight()) {
			slice.size1 = slice.size0 + (slice.size1 - slice.size0) * (WallHeight() - slice.z0) / (slice.z1 - slice.z0);
			slice.z1 = WallHeight();
		}
		clamped.push_back(slice);
	}
	if (!ivoryBase || clamped.empty())
		return BuildColumnSlices(st, tile, clamped, axisTwice);
	// The base: the slices at the foot wider than twice the pole (its narrowest).
	auto reach = [](const ColumnSlice &slice) { return std::max(slice.size0, slice.size1) * (slice.square ? 1.41F : 1.0F); };
	float pole = reach(clamped.front());
	for (const ColumnSlice &slice : clamped)
		pole = std::min({ pole, slice.size0, slice.size1 });
	size_t base = 0;
	while (base < clamped.size() && reach(clamped[base]) > 2.0F * pole)
		base++;
	if (base == 0 || base == clamped.size())
		return BuildColumnSlices(st, tile, clamped, axisTwice);
	std::vector<ColumnSlice> upper(clamped.begin() + static_cast<std::ptrdiff_t>(base), clamped.end());
	upper.front().z0 = std::min(upper.front().z0, StandBaseHeight);
	if (!BuildColumnSlices(st, tile, upper, axisTwice))
		return false;
	AddCleanPyramid(st.art, ColumnAxis(tile, axisTwice), StandBaseHalf, pole, StandBaseHeight, SecondVersionAt(tile) ? CleanSurface::IvoryAlt : CleanSurface::Ivory);
	return true;
}

bool BuildColumnSlices(Structure &st, Point tile, const std::vector<ColumnSlice> &slices, int axisTwice)
{
	if (slices.empty())
		return false;
	BuildCanvas(st.art, { tile }, {});
	const Vec2 c = ColumnAxis(tile, axisTwice);
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

/**
 * @brief A pillar carrying arch ends (an arcade): its shaft as a round column read off the symmetric lower art,
 * and above the spring of the arches a block wearing the wall face art, the arch openings cut out of it.
 */
bool BuildArcadePillar(Structure &st, Point tile, float &spring)
{
	const PieceTextures &textures = GetPieceTextures(dPiece[tile.x][tile.y]);
	int axisTwice = ColumnWidth;
	spring = 0;
	std::vector<ColumnSlice> slices = ColumnProfile(textures, axisTwice, &spring);
	BuildCanvas(st.art, { tile }, {});
	if (slices.empty() || spring < 0.4F) {
		// The art does not give a clean shaft: a plain one of the cathedral's proportions.
		axisTwice = ColumnWidth;
		spring = 1.0F;
		slices = { { 0.0F, 0.12F, 0.2F, 0.2F, true }, { 0.12F, 0.9F, 0.15F, 0.15F, false }, { 0.9F, 1.0F, 0.2F, 0.2F, true } };
	}
	const float offset = static_cast<float>(axisTwice - ColumnWidth) / 2.0F / 64.0F;
	const Vec2 c { static_cast<float>(tile.x) + 0.5F + offset, static_cast<float>(tile.y) + 0.5F - offset };
	for (size_t i = 0; i < slices.size(); i++) {
		const ColumnSlice &slice = slices[i];
		const bool top = i + 1 == slices.size();
		if (slice.square)
			AddStructureFrustum(st.art, c, slice.size0, slice.size1, slice.z0, slice.z1, top);
		else
			AddStructureCone(st.art, c, slice.size0, slice.size1, slice.z0, slice.z1, top);
	}
	return true;
}

/** @brief The level's plain wall pair and its painted arch, for arch walls (cached per level). */
struct ArcadeArt {
	bool ok = false;
	bool hasRecess = false;
	uint16_t pairA = 0;
	uint16_t pairB = 0;
	bool useX = false;
	float centre = 1.0F;    // tiles from the start of the first face
	float halfWidth = 0.5F; // of the painted arch's opening, tiles
	float crown = 1.5F;     // height of the painted arch's crown and of its springing (tiles, before VerticalScale)
	float spring = 1.0F;
};

/** Half the width of an arcade's pier, left standing beside each arch between pillars (tiles). */
constexpr float ArcadePierHalf = 0.12F;

const ArcadeArt &GetArcadeArt()
{
	static int builtFor = -1;
	static ArcadeArt art;
	if (builtFor == StructuresBuildCount)
		return art;
	builtFor = StructuresBuildCount;
	art = {};
	art.ok = GetPlainWallPair(art.pairA, art.pairB, art.useX);
	if (art.ok) {
		const PieceTextures &ta = GetPieceTextures(art.pairA);
		const PieceTextures &tb = GetPieceTextures(art.pairB);
		const std::vector<int> &ra = art.useX ? ta.recessX : ta.recessY;
		const std::vector<int> &rb = art.useX ? tb.recessX : tb.recessY;
		const int faceHeight = FaceTextureHeight();
		int sum = 0;
		int n = 0;
		int first = -1;
		int last = -1;
		int crownRow = faceHeight;
		for (int u = 0; u < 2 * FaceTextureWidth; u++) {
			const std::vector<int> &r = u < FaceTextureWidth ? ra : rb;
			if (!r.empty() && r[u % FaceTextureWidth] < faceHeight) {
				sum += u;
				n++;
				if (first < 0)
					first = u;
				last = u;
				crownRow = std::min(crownRow, r[u % FaceTextureWidth]);
			}
		}
		art.centre = n > 0 ? (static_cast<float>(sum) / static_cast<float>(n) + 0.5F) / FaceTextureWidth : 1.0F;
		art.hasRecess = n > 0;
		if (art.hasRecess) {
			auto rowAt = [&](int u) { return (u < FaceTextureWidth ? ra : rb)[u % FaceTextureWidth]; };
			art.halfWidth = static_cast<float>(last - first + 1) / 2.0F / FaceTextureWidth;
			art.crown = WallArtHeight(crownRow);
			art.spring = WallArtHeight(std::max(rowAt(first), rowAt(last)));
			art.hasRecess = art.crown > art.spring + 0.1F && art.halfWidth > 0.2F;
		}
	}
	return art;
}

/**
 * @brief How the painted arch is laid on an arch wall: stretched so it springs from the pillars' capitals and spans
 * the gap between them (less the piers).
 */
struct ArcadeFit {
	float span;
	float spring;  // the wall's springing height
	float scaleH;  // art tiles per tile along the wall
	float scaleV;  // art tiles per tile up the wall, from the springing
};

ArcadeFit FitArcade(const ArcadeArt &art, float span, float spring)
{
	ArcadeFit fit { span, spring, 1.0F, 1.0F };
	if (!art.hasRecess)
		return fit;
	const float radius = std::max(0.2F, span / 2.0F - ArcadePierHalf);
	fit.scaleH = art.halfWidth / radius;
	const float crown = std::min(spring + (art.crown - art.spring) / fit.scaleH, WallHeight() - 0.2F);
	fit.scaleV = (art.crown - art.spring) / std::max(crown - spring, 0.1F);
	return fit;
}

/** @brief The point of the art (along the plain wall pair, height) shown at a point of an arch wall. */
void ArcadeArtPoint(const ArcadeArt &art, const ArcadeFit &fit, float along, float z, float &artAlong, float &artZ)
{
	artAlong = art.centre + (along - fit.span / 2.0F) * fit.scaleH;
	artZ = art.hasRecess ? art.spring + (z - fit.spring) * fit.scaleV : z;
}

/** @brief Where an arch wall's texture is at a point of the art along the plain wall pair: which face, and its column. */
void ArcadeTexel(float artAlong, bool &second, int &u)
{
	const float wrapped = artAlong - 2.0F * std::floor(artAlong / 2.0F);
	second = wrapped >= 1.0F;
	u = std::clamp(static_cast<int>((wrapped - (second ? 1.0F : 0.0F)) * FaceTextureWidth), 0, FaceTextureWidth - 1);
}

/** @brief Height of the top of an arch wall's opening at a point along its span; 0 where it is closed. */
float ArcadeOpeningTop(const ArcadeArt &art, const ArcadeFit &fit, float along)
{
	if (art.ok && art.hasRecess) {
		float artAlong;
		float artZ;
		ArcadeArtPoint(art, fit, along, 0.0F, artAlong, artZ);
		bool second;
		int u;
		ArcadeTexel(artAlong, second, u);
		const PieceTextures &tex = GetPieceTextures(second ? art.pairB : art.pairA);
		const std::vector<int> &recess = art.useX ? tex.recessX : tex.recessY;
		const int faceHeight = FaceTextureHeight();
		if (recess.empty() || recess[u] >= faceHeight || std::abs(along - fit.span / 2.0F) >= fit.span / 2.0F - ArcadePierHalf)
			return 0.0F;
		return fit.spring + (WallArtHeight(recess[u]) - art.spring) / fit.scaleV;
	}
	const float radius = fit.span / 2.0F - 0.2F;
	const float offset = along - fit.span / 2.0F;
	return std::abs(offset) < radius ? 1.0F + std::sqrt(radius * radius - offset * offset) * 0.8F : 0.0F;
}

/** @brief A pillar of an arcade, for joining it to its neighbours with arch walls. */
struct ArcadePillar {
	Point tile;
	float spring;
	size_t structure;
};

/**
 * @brief An arch wall from a pillar towards a neighbour along one axis, from the springing of the arches up to the
 * wall top. Both sides wear the level's plain wall pair laid along the span with its painted arch centred between
 * the pillars; the inside of that painted arch is open.
 */
void AddArcadeWall(Building &b, Vec2 from, Vec2 to, float spring)
{
	constexpr float HalfThickness = 0.14F;
	const float length = std::hypot(to.x - from.x, to.y - from.y);
	if (length < 0.5F)
		return;
	const Vec2 dir { (to.x - from.x) / length, (to.y - from.y) / length };
	const Vec2 side { -dir.y, dir.x };
	const float z0 = std::max(0.0F, spring - 0.05F);
	const float z1 = WallHeight();
	auto corner = [&](float along, float across, float z) { return V3 { from.x + dir.x * along + side.x * across, from.y + dir.y * along + side.y * across, z }; };
	auto addFace = [&](std::initializer_list<V3> corners, V3 normal, uint8_t mapping) {
		const size_t first = b.mesh.size();
		AddStructureFace(b, corners, normal, { (from.x + to.x) / 2.0F, (from.y + to.y) / 2.0F }, mapping);
		if (mapping != MeshArcade)
			return;
		for (size_t t = first; t < b.mesh.size(); t++) {
			WorldTriangle &tri = b.mesh[t];
			tri.mirrorX = from.x;
			tri.mirrorY = from.y;
			tri.downX = dir.x;
			tri.downY = dir.y;
			tri.slopeScale = length; // the span, for centring the arch
			tri.arcSpring = spring;
		}
	};
	for (const float s : { 1.0F, -1.0F }) {
		const float a = s * HalfThickness;
		addFace({ corner(0, a, z0), corner(length, a, z0), corner(length, a, z1), corner(0, a, z1) }, { side.x * s, side.y * s, 0 }, MeshArcade);
	}
	// The underside of the arch (its intrados) between the two faces, following the opening, so the wall has its
	// thickness seen from below and from the side.
	const ArcadeArt &art = GetArcadeArt();
	const ArcadeFit fit = FitArcade(art, length, spring);
	const int samples = std::max(4, static_cast<int>(length * 16.0F));
	auto top = [&](int i) { return std::max(z0, ArcadeOpeningTop(art, fit, length * static_cast<float>(i) / static_cast<float>(samples))); };
	float prev = top(0);
	for (int i = 0; i < samples; i++) {
		const float next = top(i + 1);
		if (std::max(prev, next) > z0 + 0.01F) {
			const float s0 = length * static_cast<float>(i) / static_cast<float>(samples);
			const float s1 = length * static_cast<float>(i + 1) / static_cast<float>(samples);
			const float ds = s1 - s0;
			const float dz = next - prev;
			const size_t first = b.mesh.size();
			addFace({ corner(s0, -HalfThickness, prev), corner(s1, -HalfThickness, next), corner(s1, HalfThickness, next), corner(s0, HalfThickness, prev) },
			    { dir.x * dz, dir.y * dz, -ds }, MeshArcade);
			for (size_t t = first; t < b.mesh.size(); t++)
				b.mesh[t].faceX = true; // underside: not cut
		}
		prev = next;
	}
	addFace({ corner(0, -HalfThickness, z1), corner(length, -HalfThickness, z1), corner(length, HalfThickness, z1), corner(0, HalfThickness, z1) }, { 0, 0, 1 }, MeshProjective);
	// The ends, so a wall stopping at a pillar (the end of an arcade, a corner) is a slab from outside too and not
	// two sheets. Where it runs on into another arch wall or meets a wall the end is hidden inside that.
	addFace({ corner(0, -HalfThickness, z0), corner(0, HalfThickness, z0), corner(0, HalfThickness, z1), corner(0, -HalfThickness, z1) }, { -dir.x, -dir.y, 0 }, MeshProjective);
	addFace({ corner(length, -HalfThickness, z0), corner(length, HalfThickness, z0), corner(length, HalfThickness, z1), corner(length, -HalfThickness, z1) }, { dir.x, dir.y, 0 }, MeshProjective);
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

/**
 * @brief The cathedral's stairs up to town: the dungeon kit's flight with its round fan of steps (stairs_01),
 * laid over the tiles where the art draws them. The flight climbs from the trigger towards -y over 4 tiles to the
 * top of the walls; the fan, which the art paints on the floor in front, spreads 2 tiles towards +y.
 */
bool BuildStairsUp(Structure &st, Point trigger)
{
	const LandmarkMesh *kit = KitMesh("stairs_01");
	if (kit == nullptr)
		return false;
	for (int dx = 0; dx < 2; dx++) {
		if (!InDungeonBounds({ trigger.x + dx, trigger.y - 3 }) || IsTileSolid({ trigger.x + dx, trigger.y }))
			return false;
		for (int dy = 1; dy <= 3; dy++)
			if (!IsTileSolid({ trigger.x + dx, trigger.y - dy }))
				return false;
	}
	// The art: the flight's tiles and the floor in front with the painted fan.
	std::vector<Point> art;
	for (int y = trigger.y - 4; y <= trigger.y + 2; y++)
		for (int x = trigger.x - 1; x <= trigger.x + 2; x++)
			if (InDungeonBounds({ x, y }))
				art.push_back({ x, y });
	BuildCanvas(st.art, art, {});
	// stairs_01 (model units): the flight runs from x = 0 (top, 3.25 high) to x = 2.4, 1.82 wide around y = 2.02;
	// the fan from x = 2.4 to 4.78, 1.02 high, 4.02 across.
	constexpr float ModelLength = 4.78F;
	constexpr float ModelWidth = 4.02F;
	constexpr float FlightEnd = 2.4F;
	constexpr float FanTop = 1.02F;
	constexpr float ModelTop = 3.25F;
	constexpr float WorldFanTop = 0.45F;
	constexpr float WorldTop = 2.4F;
	const float top = static_cast<float>(trigger.y - 4);
	const float axis = static_cast<float>(trigger.x + 1);
	auto toWorld = [&](const float *q) {
		const float along = q[0] * ModelLength;
		const float across = q[1] * ModelWidth - ModelWidth / 2.0F;
		const float z = q[2];
		// The fan is smaller than the model's: the art paints it about 1.5 tiles out from the foot of the flight.
		const bool fan = along >= FlightEnd;
		const float wy = fan ? static_cast<float>(trigger.y) + (along - FlightEnd) * 0.63F : top + along * (4.0F / FlightEnd);
		const float wz = z <= FanTop ? z * (WorldFanTop / FanTop) : WorldFanTop + (z - FanTop) * ((WorldTop - WorldFanTop) / (ModelTop - FanTop));
		return V3 { axis + across * (fan ? 0.75F : 1.1F), wy, wz };
	};
	const Vec2 mid { axis, static_cast<float>(trigger.y) - 1.0F };
	for (const auto &t : kit->triangles) {
		const V3 p0 = toWorld(&t[0]);
		const V3 p1 = toWorld(&t[3]);
		const V3 p2 = toWorld(&t[6]);
		const V3 e1 { p1.x - p0.x, p1.y - p0.y, p1.z - p0.z };
		const V3 e2 { p2.x - p0.x, p2.y - p0.y, p2.z - p0.z };
		V3 n { e1.y * e2.z - e1.z * e2.y, e1.z * e2.x - e1.x * e2.z, e1.x * e2.y - e1.y * e2.x };
		// Outward: treads up; on the flight, risers towards its foot (+y) and sides away from its axis; on the fan,
		// away from its middle.
		const V3 c { (p0.x + p1.x + p2.x) / 3.0F, (p0.y + p1.y + p2.y) / 3.0F, (p0.z + p1.z + p2.z) / 3.0F };
		const float foot = static_cast<float>(trigger.y);
		bool flip;
		if (std::abs(n.z) > 0.5F * std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z))
			flip = n.z < 0;
		else if (c.y < foot)
			flip = std::abs(n.y) > std::abs(n.x) ? n.y < 0 : n.x * (c.x - axis) < 0;
		else
			flip = n.x * (c.x - axis) + n.y * (c.y - foot) < 0;
		if (flip)
			n = { -n.x, -n.y, -n.z };
		// The fan takes the floor art it stands on; the flight (down to its last riser, on the foot line) its own.
		const bool tread = std::abs(n.z) > 0.5F * std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
		const bool onFan = c.y > foot + 0.02F || (tread && c.y >= foot - 0.05F);
		const size_t before = st.art.mesh.size();
		AddStructureFace(st.art, { p0, p1, p2 }, n, mid, onFan ? MeshFloorArt : MeshProjective);
		if (st.art.mesh.size() > before && !kit->textures.empty()) {
			WorldTriangle &tri = st.art.mesh.back();
			const size_t index = static_cast<size_t>(&t - kit->triangles.data());
			tri.texture = kit->textures[index];
			tri.uv = kit->uvs[index];
			PrepareUvLookup(tri);
		}
	}
	for (int dx = 0; dx < 2; dx++)
		for (int dy = 1; dy <= 3; dy++)
			StructureTiles[static_cast<size_t>(trigger.y - dy) * MAXDUNX + trigger.x + dx] = 1;
	for (int y = trigger.y; y <= trigger.y + 2; y++)
		for (int x = trigger.x - 1; x <= trigger.x + 2; x++)
			if (InDungeonBounds({ x, y }) && !IsTileSolid({ x, y }))
				StructureTiles[static_cast<size_t>(y) * MAXDUNX + x] = 1;
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
		if (tri.mapping == MeshFloorArt) {
			const int16_t ground = GetGroundFloor()[static_cast<size_t>(FloorTextureSize / 2) * FloorTextureSize + FloorTextureSize / 2];
			tri.fallback = ground > 0 ? PatchColor(ground) : tri.fallback;
		}
	}
}

/** Where the edges of the sarcophagus's lid take their colour in its texture: the plain margin round the carving. */
constexpr float LidPlainU = 0.5F;
constexpr float LidPlainV = 0.015F;
/** How much darker (half light levels, besides their facing) the sarcophagus's sunken panels and its hollow are. */
constexpr uint8_t SunkenPanelShade = 3;
constexpr uint8_t HollowShade = 6;
/**
 * The shadow under the sarcophagus's rim: RimShade light levels right under it, gone RimShadeBand of its height lower
 * down; and towards its foot, FootShade light levels on the floor, gone FootShadeBand of its height up.
 */
constexpr float RimShade = 3.0F;
constexpr float RimShadeBand = 0.25F;
constexpr float FootShade = 2.0F;
constexpr float FootShadeBand = 0.4F;

/**
 * @brief In the clean dungeon, how a kit sarcophagus (or its body or lid, of the same frame) laid sizeU by sizeW and
 * height high wears its texture set instead of the sprite's art (null elsewhere): the kit's classes tell its parts
 * apart (1 the lid, 2 the iron handles, 3 the sunken panels, 4 the hollow); the rest is `stone` (Marble or its second
 * version, MarbleAlt). Stone and iron are laid on by facing, in
 * tiles of the closed box's frame (so they stay on a lid that moves); the lid's carving is stretched over its top, its
 * edges take the plain margin round it. A light from above shades the rounded edges (sides a light level darker than
 * the tops, undersides two), and the body is shaded where less light gets in: its sunken panels, its hollow, under
 * its rim and at its foot.
 */
std::function<void(WorldTriangle &, size_t)> CleanSarcophagus(const LandmarkMesh &mesh, float sizeU, float sizeW, float height, CleanSurface stone)
{
	if (!IsCleanDungeon() || mesh.classes.size() != mesh.triangles.size())
		return nullptr;
	float modelHeight = 0;
	for (const auto &t : mesh.triangles)
		modelHeight = std::max({ modelHeight, t[2], t[5], t[8] });
	if (modelHeight <= 0)
		return nullptr;
	// The bottom of the rim: of the body's outermost faces (at the edge of the model's u or w) in its upper half.
	float rimBottom = modelHeight;
	for (size_t t = 0; t < mesh.triangles.size(); t++) {
		const std::array<float, 9> &m = mesh.triangles[t];
		auto atEdge = [&](int axis) {
			return std::max({ m[axis], m[3 + axis], m[6 + axis] }) <= 1e-3F || std::min({ m[axis], m[3 + axis], m[6 + axis] }) >= 1.0F - 1e-3F;
		};
		if (mesh.classes[t] == 0 && (atEdge(0) || atEdge(1)) && std::min({ m[2], m[5], m[8] }) > modelHeight / 2)
			rimBottom = std::min({ rimBottom, m[2], m[5], m[8] });
	}
	// The lid's extent in the model's u and w (0..1): the carving spans it.
	float lidU0 = 1;
	float lidU1 = 0;
	float lidW0 = 1;
	float lidW1 = 0;
	for (size_t t = 0; t < mesh.triangles.size(); t++) {
		if (mesh.classes[t] != 1)
			continue;
		for (int k = 0; k < 3; k++) {
			lidU0 = std::min(lidU0, mesh.triangles[t][k * 3]);
			lidU1 = std::max(lidU1, mesh.triangles[t][k * 3]);
			lidW0 = std::min(lidW0, mesh.triangles[t][k * 3 + 1]);
			lidW1 = std::max(lidW1, mesh.triangles[t][k * 3 + 1]);
		}
	}
	return [&mesh, sizeU, sizeW, height, stone, modelHeight, rimBottom, lidU0, lidU1, lidW0, lidW1](WorldTriangle &tri, size_t t) {
		const std::array<float, 9> &m = mesh.triangles[t];
		std::array<V3, 3> q; // tiles in the model's frame: u, w across, z up
		for (int k = 0; k < 3; k++)
			q[k] = { (m[k * 3] - 0.5F) * sizeU, (m[k * 3 + 1] - 0.5F) * sizeW, m[k * 3 + 2] / modelHeight * height };
		const V3 e1 { q[1].x - q[0].x, q[1].y - q[0].y, q[1].z - q[0].z };
		const V3 e2 { q[2].x - q[0].x, q[2].y - q[0].y, q[2].z - q[0].z };
		const float nx = std::abs(e1.y * e2.z - e1.z * e2.y);
		const float ny = std::abs(e1.z * e2.x - e1.x * e2.z);
		const float nz = std::abs(e1.x * e2.y - e1.y * e2.x);
		const bool flat = nz >= nx && nz >= ny;
		const uint8_t part = mesh.classes[t];
		const bool carving = part == 1 && flat && tri.normal.z > 0 && lidU1 > lidU0 && lidW1 > lidW0;
		tri.cleanSurface = part == 2 ? CleanSurface::Iron : (part == 1 ? CleanSurface::Lid : stone);
		for (int k = 0; k < 3; k++) {
			float u;
			float v;
			if (carving) {
				u = (m[k * 3 + 1] - lidW0) / (lidW1 - lidW0);
				v = (m[k * 3] - lidU0) / (lidU1 - lidU0);
			} else if (part == 1) {
				u = LidPlainU;
				v = LidPlainV;
			} else if (flat) {
				u = q[k].x;
				v = q[k].y;
			} else {
				u = nx >= ny ? q[k].y : q[k].x;
				v = -q[k].z;
			}
			tri.uv[k * 2] = u;
			tri.uv[k * 2 + 1] = v;
		}
		tri.darkness = static_cast<uint8_t>(std::lround((1.0F - std::clamp(tri.normal.z, -1.0F, 1.0F)) * 2.0F));
		if (part == 3)
			tri.darkness += SunkenPanelShade;
		if (part == 4)
			tri.darkness += HollowShade;
		// The body's shadows: under the rim (on what lies below it), and towards the foot.
		if (part == 0 || part == 3 || part == 4) {
			const float rim = rimBottom * height / modelHeight;
			if (std::max({ m[2], m[5], m[8] }) * height / modelHeight <= rim + 1e-4F) {
				tri.occlusion[0].perZ = RimShade / (RimShadeBand * height);
				tri.occlusion[0].at0 = RimShade - tri.occlusion[0].perZ * rim;
			}
			tri.occlusion[1].perZ = -FootShade / (FootShadeBand * height);
			tri.occlusion[1].at0 = FootShade;
		}
		tri.mapping = MeshClean;
		tri.texture = 0;
		PrepareUvLookup(tri);
	};
}

/** @brief An object drawn as a box textured with its own sprite (a sarcophagus), rebuilt when its frame changes. */
struct ObjectBox {
	const uint8_t *frame = nullptr;
	Point tile { -1, -1 };
	Structure st;
};

std::unordered_map<int, ObjectBox> ObjectBoxes;

bool IsBoxObject(const Object &object)
{
	return object._otype == OBJ_SARC;
}

/**
 * @brief Measures the box an object's iso sprite shows and builds it. The sprite is drawn centred on its tile with
 * its bottom row at the tile's front corner. For a box Lx by Ly by H, the iso picture is (Lx + Ly) * 32 px wide;
 * its lowest pixel (the front corner) lies (Lx - Ly) * 16 px right of the middle, and its height is
 * (Lx + Ly) * 16 + H * 39.2 px.
 */
bool BuildObjectBox(ObjectBox &box, const Object &object, ClxSprite sprite)
{
	// The box is measured on the first frame (a sarcophagus closed), so it keeps its size while the lid moves; the
	// current frame is the art laid on it.
	const ClxSprite closed = (*object._oAnimData)[0];
	std::vector<int16_t> pixels;
	DecodeClx(closed, pixels);
	const int w = closed.width();
	const int h = closed.height();
	for (int16_t &p : pixels)
		if (p == 0)
			p = -1; // shadow
	int left = w;
	int right = -1;
	int top = h;
	int bottom = -1;
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			if (pixels[static_cast<size_t>(y) * w + x] <= 0)
				continue;
			left = std::min(left, x);
			right = std::max(right, x);
			top = std::min(top, y);
			bottom = std::max(bottom, y);
		}
	}
	if (right < left)
		return false;
	auto rowMiddle = [&](int y) {
		int sum = 0;
		int n = 0;
		for (int x = 0; x < w; x++)
			if (pixels[static_cast<size_t>(y) * w + x] > 0) {
				sum += x;
				n++;
			}
		return n > 0 ? static_cast<float>(sum) / static_cast<float>(n) : 0.0F;
	};
	const float middle = static_cast<float>(left + right) / 2.0F;
	const float sum = static_cast<float>(right - left + 1) / 32.0F;          // Lx + Ly
	const float difference = (rowMiddle(bottom) - middle) / 16.0F;           // Lx - Ly
	const float lx = std::max(0.15F, (sum + difference) / 2.0F);
	const float ly = std::max(0.15F, (sum - difference) / 2.0F);
	const float height = std::max(0.2F, (static_cast<float>(bottom - top + 1) - sum * 16.0F) / SpritePixelsPerTileY);
	const Point tile = object.position;
	// The box's middle: on screen, x at the picture's middle, the ground under it (sum * 8) px above the front corner.
	const int closedOriginX = (tile.x - tile.y) * 32 - w / 2;
	const int closedOriginY = (tile.x + tile.y + 2) * 16 - (h - 1);
	const float a = (static_cast<float>(closedOriginX) + middle) / 32.0F;                           // cx - cy
	const float c = (static_cast<float>(closedOriginY + bottom) - sum * 8.0F) / 16.0F;              // cx + cy
	const float cx = (a + c) / 2.0F;
	const float cy = (c - a) / 2.0F;
	Building &b = box.st.art;
	b = Building {};
	DecodeClx(sprite, b.canvas);
	for (int16_t &p : b.canvas)
		if (p == 0)
			p = -1;
	b.canvasWidth = sprite.width();
	b.canvasHeight = sprite.height();
	b.originX = (tile.x - tile.y) * 32 - b.canvasWidth / 2;
	b.originY = (tile.x + tile.y + 2) * 16 - (b.canvasHeight - 1);
	// How far the lid is off: the opening animation's progress.
	const float opened = object._oAnimLen > 1 ? std::clamp(static_cast<float>(object._oAnimFrame - 1) / static_cast<float>(object._oAnimLen - 1), 0.0F, 1.0F) : 0.0F;
	const LandmarkMesh *body = KitMesh("sarcophagus_body");
	const LandmarkMesh *lid = KitMesh("sarcophagus_lid");
	// The kit's sarcophagus is long along its x: laid along the box's longer side.
	const bool swap = lx < ly;
	const Vec2 axisU = swap ? Vec2 { 0, 1 } : Vec2 { 1, 0 };
	const Vec2 axisW = swap ? Vec2 { 1, 0 } : Vec2 { 0, 1 };
	const float sizeU = swap ? ly : lx;
	const float sizeW = swap ? lx : ly;
	const CleanSurface stone = SecondVersionAt(tile) ? CleanSurface::MarbleAlt : CleanSurface::Marble;
	if (const LandmarkMesh *mesh = SarcophagusKitOn ? KitMesh("sarcophagus") : nullptr; mesh != nullptr && (opened <= 0.0F || body == nullptr || lid == nullptr)) {
		// The kit's sarcophagus over the box the sprite shows: in the clean dungeon in its texture set's marble, with
		// the carving and the iron handles; elsewhere in the sprite's own stone.
		AddKitModelOriented(b, *mesh, { cx, cy }, axisU, axisW, sizeU, sizeW, 0.0F, height, false, nullptr, CleanSarcophagus(*mesh, sizeU, sizeW, height, stone));
	} else if (mesh != nullptr) {
		// Opening: the body, and the lid slid off sideways, turned a little and tipped over the edge it rests on.
		// Both pieces are placed in the closed sarcophagus's frame; the body stops below the lid.
		auto top = [](const LandmarkMesh &m) {
			float z = 0;
			for (const auto &t : m.triangles)
				z = std::max({ z, t[2], t[5], t[8] });
			return z;
		};
		const float full = std::max(top(*lid), 1e-3F);
		const float bodyHeight = height * top(*body) / full;
		AddKitModelOriented(b, *body, { cx, cy }, axisU, axisW, sizeU, sizeW, 0.0F, bodyHeight, false, nullptr, CleanSarcophagus(*body, sizeU, sizeW, bodyHeight, stone));
		const float yaw = 0.3F * opened;
		const float tilt = 0.18F * std::clamp(opened * 2.0F - 1.0F, 0.0F, 1.0F);
		const float slideU = 0.12F * sizeU * opened;
		const float slideW = 0.48F * sizeW * opened;
		auto moveLid = [=](V3 p) {
			// To the box's own axes, around its middle.
			float u = (p.x - cx) * axisU.x + (p.y - cy) * axisU.y;
			float w = (p.x - cx) * axisW.x + (p.y - cy) * axisW.y;
			float z = p.z;
			const float ru = u * std::cos(yaw) - w * std::sin(yaw);
			const float rw = u * std::sin(yaw) + w * std::cos(yaw);
			u = ru + slideU;
			w = rw + slideW;
			// Tip about the box's top edge on the side it slid to: the overhang goes down.
			const float dw = w - sizeW / 2.0F;
			const float dz = z - height;
			w = sizeW / 2.0F + dw * std::cos(tilt) + dz * std::sin(tilt);
			z = height + dz * std::cos(tilt) - dw * std::sin(tilt);
			return V3 { cx + axisU.x * u + axisW.x * w, cy + axisU.y * u + axisW.y * w, z };
		};
		AddKitModelOriented(b, *lid, { cx, cy }, axisU, axisW, sizeU, sizeW, 0.0F, height, false, moveLid, CleanSarcophagus(*lid, sizeU, sizeW, height, stone));
	} else {
		AddStructureBox(b, cx - lx / 2.0F, cy - ly / 2.0F, 0.0F, cx + lx / 2.0F, cy + ly / 2.0F, height);
	}
	SetStructureFallbacks(box.st);
	b.x0 = cx - lx / 2.0F;
	b.y0 = cy - ly / 2.0F;
	b.x1 = cx + lx / 2.0F;
	b.y1 = cy + ly / 2.0F;
	b.lowestZ = 0;
	b.highestZ = height;
	b.boundsOverhang = opened > 0.0F ? 0.7F * std::max(lx, ly) : 0.0F; // the lid slid off
	box.tile = tile;
	box.frame = sprite.pixelData();
	return true;
}

/** @brief A door's leaf as a low-poly slab hinged in its doorway, rebuilt whenever its swing changes. */
struct DoorLeaf {
	float angle = -1.0F;
	int side = 0;
	Vec2 hinge { 0, 0 };
	Vec2 direction { 0, 0 }; // from the hinge along the leaf
	float alongStart = 0;    // door-relative position along the wall where the leaf starts (see Doorway)
	float width = 0;
	float height = 0;        // of the leaf at its highest
	std::vector<WorldTriangle> mesh;
};

std::unordered_map<int, DoorLeaf> DoorLeaves; // by object id

/** Gap between the leaf and its frame, the leaf's thickness, and how many slices follow its arched top (tiles). */
constexpr float DoorLeafGap = 0.012F;
constexpr float DoorLeafThickness = 0.07F;
constexpr int DoorLeafSlices = 10;
/** The wood texture repeats every this many tiles across and up the leaf. */
constexpr float DoorWoodRepeat = 0.8F;

/** @brief The kit's door texture (wood planks), -1 if the kit is not loaded. */
int DoorWoodTexture()
{
	const LandmarkMesh *mesh = KitMesh("door");
	if (mesh == nullptr)
		return -1;
	for (const int16_t texture : mesh->textures)
		if (texture >= 0)
			return texture;
	return -1;
}

/**
 * The caves' gates: a see-through lattice of beams like their fences' (fp_dungeon.cpp), drawn tiles: the stiles at its
 * edges and the pickets between them (their width), the rails across (their height, and where they are: up from the
 * foot, a share of the way up, down from the top), the brace from the foot at the hinge to the top at the far stile,
 * and the beams' thickness.
 */
constexpr float GateStile = 0.08F;
constexpr float GatePicket = 0.05F;
constexpr int GatePickets = 3;
constexpr float GateRail = 0.09F;
constexpr std::array<float, 3> GateRails { 0.2F, 0.5F, 0.045F };
constexpr float GateBrace = 0.07F;
constexpr float GateThickness = 0.06F;

/**
 * @brief The leaf of one of the caves' gates (CleanDoorwaysAreGates) as a lattice of wooden beams wearing the clean
 * dungeon's `wood`: `d` runs along the leaf from its hinge, `m` through it, `point(offset, through, z)` places a point
 * (z in art units).
 */
template <typename PointFn>
void BuildGateLattice(DoorLeaf &leaf, Vec2 d, Vec2 m, PointFn point)
{
	const float stretch = VerticalScale();
	const float width = leaf.width;
	const float height = leaf.height * stretch; // drawn
	const float half = GateThickness / 2.0F;
	// A face from four corners (offset along the leaf, through it, drawn height), its normal from `along` (the leaf),
	// `through` and `up`, and each corner's texture position.
	auto face = [&](std::array<std::array<float, 3>, 4> c, float along, float through, float up, std::array<std::array<float, 2>, 4> uv) {
		Building b {};
		const V3 normal { d.x * along + m.x * through, d.y * along + m.y * through, up * stretch };
		AddStructureFace(b, { point(c[0][0], c[0][1], c[0][2] / stretch), point(c[1][0], c[1][1], c[1][2] / stretch), point(c[2][0], c[2][1], c[2][2] / stretch), point(c[3][0], c[3][1], c[3][2] / stretch) },
		    normal, leaf.hinge, MeshClean, 0);
		for (size_t t = 0; t < b.mesh.size(); t++) {
			WorldTriangle &tri = b.mesh[t];
			const std::array<size_t, 3> k { 0, t + 1, t + 2 }; // AddStructureFace fans from the first corner
			for (size_t v = 0; v < 3; v++) {
				tri.uv[v * 2] = uv[k[v]][0];
				tri.uv[v * 2 + 1] = uv[k[v]][1];
			}
			tri.cleanSurface = CleanSurface::Wood;
			tri.darkness = up < -0.5F ? 2 : (std::abs(along) > 0.5F ? 1 : 0); // undersides and ends a little darker
			PrepareUvLookup(tri);
			leaf.mesh.push_back(tri);
		}
	};
	// A straight beam in the leaf's plane from (o0, z0) to (o1, z1), `w` wide: its two broad faces and its two long
	// sides (its ends meet other beams). The grain runs along it.
	auto beam = [&](float o0, float z0, float o1, float z1, float w) {
		const float len = std::hypot(o1 - o0, z1 - z0);
		if (len < 1e-4F)
			return;
		const float uo = (o1 - o0) / len;
		const float uz = (z1 - z0) / len;
		const float po = -uz * w / 2.0F; // across the beam, in the leaf's plane
		const float pz = uo * w / 2.0F;
		const std::array<std::array<float, 2>, 4> grain { { { -w / 2.0F, 0.0F }, { w / 2.0F, 0.0F }, { w / 2.0F, len }, { -w / 2.0F, len } } };
		for (const float t : { half, -half }) {
			face({ { { o0 - po, t, z0 - pz }, { o0 + po, t, z0 + pz }, { o1 + po, t, z1 + pz }, { o1 - po, t, z1 - pz } } }, 0.0F, t > 0 ? 1.0F : -1.0F, 0.0F, grain);
		}
		for (const float s : { 1.0F, -1.0F }) {
			const std::array<std::array<float, 2>, 4> side { { { -half, 0.0F }, { half, 0.0F }, { half, len }, { -half, len } } };
			face({ { { o0 + s * po, -half, z0 + s * pz }, { o0 + s * po, half, z0 + s * pz }, { o1 + s * po, half, z1 + s * pz }, { o1 + s * po, -half, z1 + s * pz } } },
			    s * po / (w / 2.0F), 0.0F, s * pz / (w / 2.0F), side);
		}
	};
	// The stiles, the pickets between them, the rails across and the brace.
	const float railLow = GateRails[0];
	const float railMid = height * GateRails[1];
	const float railHigh = height - GateRails[2];
	beam(GateStile / 2.0F, 0.0F, GateStile / 2.0F, height, GateStile);
	beam(width - GateStile / 2.0F, 0.0F, width - GateStile / 2.0F, height, GateStile);
	for (int i = 1; i <= GatePickets; i++) {
		const float o = GateStile + (width - 2.0F * GateStile) * static_cast<float>(i) / (GatePickets + 1);
		beam(o, railLow, o, railHigh, GatePicket);
	}
	for (const float z : { railLow, railMid, railHigh })
		beam(GateStile, z, width - GateStile, z, GateRail);
	beam(GateStile, railLow, width - GateStile, railHigh, GateBrace);
}

void BuildDoorLeaf(DoorLeaf &leaf, const Doorway &door, float angle, int side)
{
	leaf.angle = angle;
	leaf.side = side;
	leaf.mesh.clear();
	const Vec2 along = door.alongX ? Vec2 { 1, 0 } : Vec2 { 0, 1 };
	const Vec2 across = door.alongX ? Vec2 { 0, 1 } : Vec2 { 1, 0 };
	const float s = static_cast<float>(side);
	const Vec2 d { along.x * std::cos(angle) + across.x * s * std::sin(angle), along.y * std::cos(angle) + across.y * s * std::sin(angle) };
	const Vec2 m { across.x * std::cos(angle) - along.x * s * std::sin(angle), across.y * std::cos(angle) - along.y * s * std::sin(angle) };
	leaf.alongStart = door.openFrom + DoorLeafGap;
	leaf.width = door.openTo - door.openFrom - 2.0F * DoorLeafGap;
	// The hinge stands on the doorway's jamb, in the middle of the wall's thickness.
	const Vec2 tile { static_cast<float>(door.tile.x), static_cast<float>(door.tile.y) };
	leaf.hinge = { tile.x + along.x * leaf.alongStart + across.x * 0.5F, tile.y + along.y * leaf.alongStart + across.y * 0.5F };
	leaf.direction = d;
	if (leaf.width <= 0.05F)
		return;
	// The top of the leaf follows the opening's arch, a little below it.
	std::array<float, DoorLeafSlices + 1> offsets;
	std::array<float, DoorLeafSlices + 1> heights;
	const int faceHeight = FaceTextureHeight();
	leaf.height = 0;
	for (int i = 0; i <= DoorLeafSlices; i++) {
		offsets[i] = leaf.width * static_cast<float>(i) / DoorLeafSlices;
		const float at = std::clamp(leaf.alongStart + offsets[i], door.openFrom, door.openTo - 0.001F);
		const int u = std::max(DoorFaceColumn(door, at), 0);
		int row = door.opening[u];
		// Columns at the very edge of the opening may be closed: use the nearest open one.
		for (int k = 1; row >= faceHeight && k < FaceTextureWidth; k++) {
			const int left = std::max(u - k, 0);
			const int right = std::min(u + k, FaceTextureWidth - 1);
			row = std::min(door.opening[left], door.opening[right]);
		}
		heights[i] = std::max(0.1F, WallArtHeight(row) - DoorLeafGap);
		leaf.height = std::max(leaf.height, heights[i]);
	}
	auto point = [&](float offset, float t, float z) {
		return V3 { leaf.hinge.x + d.x * offset + m.x * t, leaf.hinge.y + d.y * offset + m.y * t, z };
	};
	if (CleanDoorwaysAreGates()) {
		BuildGateLattice(leaf, d, m, point);
		return;
	}
	const int wood = DoorWoodTexture();
	// `corners` as (offset along the leaf, offset through it, height); the wood is laid on by offset along and height.
	auto add = [&](std::initializer_list<std::array<float, 3>> corners, V3 normal, uint8_t darkness) {
		std::vector<std::array<float, 3>> c(corners);
		Building b {};
		AddStructureFace(b, { point(c[0][0], c[0][1], c[0][2]), point(c[1][0], c[1][1], c[1][2]), point(c[2][0], c[2][1], c[2][2]), point(c[3][0], c[3][1], c[3][2]) },
		    normal, leaf.hinge, MeshSolid, darkness);
		for (size_t t = 0; t < b.mesh.size(); t++) {
			WorldTriangle &tri = b.mesh[t];
			tri.darkness = darkness; // no extra shade for faces turned from the iso camera
			if (wood >= 0) {
				// AddStructureFace fans the polygon from its first corner: (0, 1, 2), (0, 2, 3).
				const std::array<size_t, 3> k { 0, t + 1, t + 2 };
				for (size_t v = 0; v < 3; v++) {
					tri.uv[v * 2] = (c[k[v]][0] + c[k[v]][1]) / DoorWoodRepeat;
					tri.uv[v * 2 + 1] = c[k[v]][2] / DoorWoodRepeat;
				}
				tri.texture = static_cast<int16_t>(wood);
				PrepareUvLookup(tri);
			}
			leaf.mesh.push_back(tri);
		}
	};
	const float half = DoorLeafThickness / 2.0F;
	for (int i = 0; i < DoorLeafSlices; i++) {
		const float a = offsets[i];
		const float b = offsets[i + 1];
		const float ha = heights[i];
		const float hb = heights[i + 1];
		add({ { a, half, 0 }, { b, half, 0 }, { b, half, hb }, { a, half, ha } }, { m.x, m.y, 0 }, 0);
		add({ { a, -half, 0 }, { a, -half, ha }, { b, -half, hb }, { b, -half, 0 } }, { -m.x, -m.y, 0 }, 0);
		// The top edge, facing up and out along the slope of the arch.
		const float rise = hb - ha;
		const float run = b - a;
		add({ { a, -half, ha }, { a, half, ha }, { b, half, hb }, { b, -half, hb } }, { -d.x * rise, -d.y * rise, run }, 1);
	}
	const float w = leaf.width;
	add({ { 0, -half, 0 }, { 0, half, 0 }, { 0, half, heights[0] }, { 0, -half, heights[0] } }, { -d.x, -d.y, 0 }, 2);
	add({ { w, -half, 0 }, { w, -half, heights[DoorLeafSlices] }, { w, half, heights[DoorLeafSlices] }, { w, half, 0 } }, { d.x, d.y, 0 }, 2);
}

/**
 * @brief A door leaf's colour: the texel's brightness (lifted a little) mapped onto the wood ramp measured from
 * Daggerfall's own door textures (TEXTURE.074, "Door Set"), then matched to the nearest palette colour.
 */
uint8_t DoorLeafColor(uint8_t color)
{
	struct Stop {
		float luma, r, g, b;
	};
	static constexpr std::array<Stop, 8> Wood { {
	    { 18, 25, 20, 15 }, { 48, 51, 44, 34 }, { 70, 89, 64, 41 }, { 86, 123, 75, 42 },
	    { 100, 123, 92, 60 }, { 120, 151, 110, 69 }, { 160, 210, 148, 84 }, { 235, 251, 214, 150 } } };
	static std::array<uint8_t, 256> table {};
	static uint32_t readyFor = 0;
	if (readyFor != StablePaletteVersion()) {
		readyFor = StablePaletteVersion();
		// Matching colour by colour speckles (the palette has no true brown): use the one 16-colour ramp that fits the wood best.
		int woodRamp = 0;
		float woodFit = 1e30F;
		for (int ramp = 128; ramp < 256; ramp += 16) { // 128 and up are the same on every level
			float fit = 0;
			for (const Stop &stop : Wood) {
				float nearest = 1e30F;
				for (int j = ramp; j < ramp + 16; j++) {
					const SDL_Color o = logical_palette[j];
					nearest = std::min(nearest, (o.r - stop.r) * (o.r - stop.r) * 6 + (o.g - stop.g) * (o.g - stop.g) * 3 + (o.b - stop.b) * (o.b - stop.b) * 2);
				}
				fit += nearest;
			}
			if (fit < woodFit) {
				woodFit = fit;
				woodRamp = ramp;
			}
		}
		for (int i = 0; i < 256; i++) {
			const SDL_Color c = logical_palette[i];
			const float luma = std::min(235.0F, (0.3F * c.r + 0.59F * c.g + 0.11F * c.b) * 2.2F + 20.0F);
			size_t k = 1;
			while (k + 1 < Wood.size() && Wood[k].luma < luma)
				k++;
			const float t = std::clamp((luma - Wood[k - 1].luma) / (Wood[k].luma - Wood[k - 1].luma), 0.0F, 1.0F);
			constexpr float Dim = 0.85F; // the doors sit in the dark: Daggerfall's ramp, toned down
			const float r = Dim * (Wood[k - 1].r + (Wood[k].r - Wood[k - 1].r) * t);
			const float g = Dim * (Wood[k - 1].g + (Wood[k].g - Wood[k - 1].g) * t);
			const float b = Dim * (Wood[k - 1].b + (Wood[k].b - Wood[k - 1].b) * t);
			float best = 1e30F;
			table[i] = static_cast<uint8_t>(i);
			for (int j = woodRamp; j < woodRamp + 16; j++) {
				const SDL_Color o = logical_palette[j];
				const float dist = (o.r - r) * (o.r - r) * 3 + (o.g - g) * (o.g - g) * 6 + (o.b - b) * (o.b - b);
				if (dist < best) {
					best = dist;
					table[i] = static_cast<uint8_t>(j);
				}
			}
		}
	}
	return table[color];
}

void FinishStructureBounds();

/**
 * @brief Whether a triangle is small enough to be lit as a whole, at its middle: the light is the tiles' interpolated
 * between their centres (CleanLightAt), and changes little over a fifth of a tile. Looked up per pixel, it was a good
 * part of the bone's cost.
 */
bool SmallForLight(const WorldTriangle &tri)
{
	constexpr float Reach = 0.2F;
	for (int k = 0; k < 3; k++) {
		const V3 &a = tri.p[k];
		const V3 &b = tri.p[(k + 1) % 3];
		if (std::abs(a.x - b.x) > Reach || std::abs(a.y - b.y) > Reach)
			return false;
	}
	return true;
}

/** @brief The light (CleanLightAt) on a clean triangle at a point: once a frame at its middle for a small one. */
float CleanTriangleLight(const WorldTriangle &tri, float wx, float wy)
{
	if (!tri.smallLight)
		return CleanLightAt(wx, wy);
	static const WorldTriangle *last = nullptr;
	static uint32_t lastFrame = 0;
	static float light = 0;
	if (&tri != last || lastFrame != StructureFrame) {
		last = &tri;
		lastFrame = StructureFrame;
		light = CleanLightAt((tri.p[0].x + tri.p[1].x + tri.p[2].x) / 3.0F, (tri.p[0].y + tri.p[1].y + tri.p[2].y) / 3.0F);
	}
	return light;
}

/**
 * @brief Lays the clean dungeon's textures on a kept model (the cathedral's stairs) by the way each triangle faces, in
 * world coordinates: `vertical` on its sides (`pillar` on round parts), `top` on what faces up, `under` below; parts
 * under the floor (a pit) darken with their depth.
 */
void RetextureClean(Structure &st, CleanSurface vertical, CleanSurface pillar, CleanSurface top, CleanSurface under)
{
	const float stretch = VerticalScale();
	for (WorldTriangle &tri : st.art.mesh) {
		const V3 n = tri.normal;
		const bool up = n.z > 0.5F;
		const bool down = n.z < -0.5F;
		const bool round = !up && !down && std::abs(n.x) > 0.2F && std::abs(n.y) > 0.2F;
		tri.cleanSurface = up ? top : (down ? under : (round ? pillar : vertical));
		float lowest = 0;
		for (int k = 0; k < 3; k++) {
			const V3 &p = tri.p[k];
			lowest = std::min(lowest, p.z);
			if (up || down) {
				tri.uv[k * 2] = p.x;
				tri.uv[k * 2 + 1] = p.y;
			} else {
				tri.uv[k * 2] = std::abs(n.x) >= std::abs(n.y) ? p.y : p.x;
				tri.uv[k * 2 + 1] = -p.z * stretch;
			}
		}
		// Half light levels: the original's shading (undersides, faces turned away), and down in the pit.
		tri.darkness = static_cast<uint8_t>(std::min(tri.darkness + static_cast<int>(-lowest * stretch * 3.0F), 24));
		tri.mapping = MeshClean;
		tri.texture = 0;
		tri.smallLight = SmallForLight(tri);
		PrepareUvLookup(tri);
	}
}

/** @brief The cathedral's stairs, up and down, at its stairs triggers (in the clean dungeon, with its textures). */
void BuildCathedralStairs()
{
	const bool clean = IsCleanDungeon();
	for (int i = 0; i < numtrigs; i++) {
		Structure st;
		if (trigs[i]._tmsg == WM_DIABPREVLVL && BuildStairsUp(st, trigs[i].position)) {
			SetStructureFallbacks(st);
			if (clean)
				RetextureClean(st, CleanSurface::Arch, CleanSurface::Arch, CleanSurface::Stone, CleanSurface::Arch);
			Structures.push_back(std::move(st));
		} else if (trigs[i]._tmsg == WM_DIABNEXTLVL && BuildStairsDown(st, trigs[i].position)) {
			SetStructureFallbacks(st);
			if (clean)
				RetextureClean(st, CleanSurface::Wall, CleanSurface::Stone, CleanSurface::Stone, CleanSurface::Arch);
			Structures.push_back(std::move(st));
		}
	}
}

/** @brief Adds a quad of the clean dungeon's geometry (CleanQuad: drawn tiles, z stretched) to a structure's mesh. */
void AddCleanQuad(Building &b, const CleanQuad &quad)
{
	const float stretch = VerticalScale();
	// Meshes are kept in art units, stretched up when drawn: so are the normals (for culling).
	const auto at = [&](int k) { return V3 { quad.p[k][0], quad.p[k][1], quad.p[k][2] / stretch }; };
	const size_t first = b.mesh.size();
	const V3 normal { quad.normal[0], quad.normal[1], quad.normal[2] * stretch };
	if (quad.triangle)
		AddStructureFace(b, { at(0), at(1), at(2) }, normal, { 0, 0 }, MeshClean);
	else
		AddStructureFace(b, { at(0), at(1), at(2), at(3) }, normal, { 0, 0 }, MeshClean);
	for (size_t t = first; t < b.mesh.size(); t++) {
		WorldTriangle &tri = b.mesh[t];
		// AddStructureFace fans the quad from its first corner: (0, 1, 2), then (0, 2, 3).
		const std::array<int, 3> corners { 0, static_cast<int>(t - first) + 1, static_cast<int>(t - first) + 2 };
		for (int v = 0; v < 3; v++) {
			tri.uv[v * 2] = quad.uv[corners[v]][0];
			tri.uv[v * 2 + 1] = quad.uv[corners[v]][1];
		}
		tri.cleanSurface = quad.surface;
		// Half a light level per step: undersides, and the inside of stairwells.
		tri.darkness = static_cast<uint8_t>((quad.normal[2] < -0.5F ? 3 : 0) + std::lround(quad.darkness * 2.0F));
		tri.smallLight = SmallForLight(tri);
		tri.cull = !quad.twoSided;
		if (quad.sprite >= 0) {
			// A card: seen from both sides.
			tri.mapping = MeshSprite;
			tri.sprite = quad.sprite;
			tri.cull = false;
			tri.slopeScale = quad.spriteWidth; // the cut-out's width, for the detail level
		}
		PrepareUvLookup(tri);
	}
}

/**
 * @brief Hell's stairs (fp_hell.hpp): the hell stairs' rib cage over each flight, its rims round the pit and the spine
 * going down into it for the stairs down (the floor cut open over it); the cage alone for the stairs up and to town.
 * Laid on the middle of the flight's tiles, its entrance (where the spine comes up out of the pit) towards the
 * trigger, in the bone of hell's set. hell_stairs.obj (model units): x -1.347 .. 4.042, y -3.369 .. 3.07, the floor
 * at z 0 (3.66 above its lowest point, 2.852 below its highest); the pit's rims round (1.346, 0), open about 1.35
 * across x and 2 along y; the spine comes up at -y.
 */
void BuildHellStairs()
{
	const LandmarkMesh *whole = KitMesh("hell_stairs");
	const LandmarkMesh *cage = KitMesh("hell_ribs");
	if (whole == nullptr || cage == nullptr)
		return;
	constexpr float Scale = 0.6F; // tiles per model unit
	constexpr float ModelX0 = -1.347F;
	constexpr float ModelX1 = 4.042F;
	constexpr float ModelY0 = -3.369F;
	constexpr float ModelY1 = 3.07F;
	constexpr float ModelBelow = 3.66F;
	constexpr float ModelAbove = 2.852F;
	constexpr float PitX = 1.346F;
	constexpr float HoleHalfX = 0.95F;
	constexpr float HoleHalfY = 1.4F;
	const float stretch = VerticalScale();
	for (int i = 0; i < numtrigs; i++) {
		const interface_mode msg = trigs[i]._tmsg;
		const bool down = msg == WM_DIABNEXTLVL;
		if (!down && msg != WM_DIABPREVLVL && msg != WM_DIABTWARPUP)
			continue;
		const Point trigger = trigs[i].position;
		// The flight's tiles: those of its stairs next to the trigger, and all that touch them.
		std::vector<Point> tiles;
		for (int dy = -2; dy <= 2; dy++)
			for (int dx = -2; dx <= 2; dx++)
				if (const Point t = trigger + Displacement { dx, dy }; IsHellStairsTile(t))
					tiles.push_back(t);
		for (size_t k = 0; k < tiles.size() && tiles.size() < 64; k++) {
			for (int dy = -1; dy <= 1; dy++) {
				for (int dx = -1; dx <= 1; dx++) {
					const Point t = tiles[k] + Displacement { dx, dy };
					if (IsHellStairsTile(t) && std::find(tiles.begin(), tiles.end(), t) == tiles.end())
						tiles.push_back(t);
				}
			}
		}
		if (tiles.empty())
			continue;
		int minX = MAXDUNX;
		int minY = MAXDUNY;
		int maxX = -1;
		int maxY = -1;
		for (const Point t : tiles) {
			minX = std::min(minX, t.x);
			maxX = std::max(maxX, t.x);
			minY = std::min(minY, t.y);
			maxY = std::max(maxY, t.y);
		}
		const Vec2 centre { static_cast<float>(minX + maxX + 1) / 2.0F, static_cast<float>(minY + maxY + 1) / 2.0F };
		const Vec2 toTrigger { static_cast<float>(trigger.x) + 0.5F - centre.x, static_cast<float>(trigger.y) + 0.5F - centre.y };
		const Vec2 front = std::abs(toTrigger.x) > std::abs(toTrigger.y) ? Vec2 { toTrigger.x < 0 ? -1.0F : 1.0F, 0 } : Vec2 { 0, toTrigger.y < 0 ? -1.0F : 1.0F };
		// The model's y runs from its entrance (-y) into the pit: away from the trigger. Its x across.
		const Vec2 axisW { -front.x, -front.y };
		const Vec2 axisU { -axisW.y, axisW.x };
		// The pit's middle on the flight's middle; the model's frame is centred on its extent.
		const float frameX = (ModelX0 + ModelX1) / 2.0F;
		const float frameY = (ModelY0 + ModelY1) / 2.0F;
		auto toWorld = [&](float mx, float my) {
			return Vec2 { centre.x + (axisU.x * (mx - PitX) + axisW.x * my) * Scale, centre.y + (axisU.y * (mx - PitX) + axisW.y * my) * Scale };
		};
		const Vec2 c = toWorld(frameX, frameY);
		Structure st;
		// Meshes are kept in art units, stretched up when drawn.
		AddKitModelOriented(st.art, down ? *whole : *cage, c, axisU, axisW, (ModelX1 - ModelX0) * Scale, (ModelY1 - ModelY0) * Scale, -ModelBelow * Scale / stretch,
		    (ModelBelow + ModelAbove) * Scale / stretch, false);
		if (st.art.mesh.empty())
			continue;
		SetStructureFallbacks(st);
		RetextureClean(st, CleanSurface::Pillar, CleanSurface::Pillar, CleanSurface::Pillar, CleanSurface::Stone);
		if (down) {
			const Vec2 a = toWorld(PitX - HoleHalfX, -HoleHalfY);
			const Vec2 b = toWorld(PitX + HoleHalfX, HoleHalfY);
			const float x0 = std::min(a.x, b.x);
			const float y0 = std::min(a.y, b.y);
			const float x1 = std::max(a.x, b.x);
			const float y1 = std::max(a.y, b.y);
			FloorHoles.push_back({ x0, y0, x1, y1 });
			// The pit under the hole: flesh sides going down in bands, darker the deeper, and a floor at the bottom
			// (the spine goes down to it), all but black.
			const float bottom = -ModelBelow * Scale;
			constexpr int Bands = 3;
			using P3 = std::array<float, 3>;
			const std::array<std::array<float, 4>, 4> sides { { { x0, y0, x1, y0 }, { x1, y0, x1, y1 }, { x1, y1, x0, y1 }, { x0, y1, x0, y0 } } };
			for (const auto &[ax, ay, bx, by] : sides) {
				const float len = std::hypot(bx - ax, by - ay);
				// Inward: the hole's middle is on the left of each side, walked round counter-clockwise.
				const P3 inward { -(by - ay) / len, (bx - ax) / len, 0 };
				for (int band = 0; band < Bands; band++) {
					const float z0 = bottom * static_cast<float>(band + 1) / Bands;
					const float z1 = bottom * static_cast<float>(band) / Bands;
					CleanQuad side;
					side.p = { P3 { ax, ay, z0 }, P3 { bx, by, z0 }, P3 { bx, by, z1 }, P3 { ax, ay, z1 } };
					side.normal = inward;
					const float u0 = ax + ay;
					side.uv = { { { u0, -z0 }, { u0 + len, -z0 }, { u0 + len, -z1 }, { u0, -z1 } } };
					side.surface = CleanSurface::Wall;
					side.darkness = 1.5F + 2.5F * static_cast<float>(band);
					AddCleanQuad(st.art, side);
				}
			}
			CleanQuad floor;
			floor.p = { P3 { x0, y0, bottom }, P3 { x1, y0, bottom }, P3 { x1, y1, bottom }, P3 { x0, y1, bottom } };
			floor.normal = { 0, 0, 1 };
			floor.uv = { { { x0, y0 }, { x1, y0 }, { x1, y1 }, { x0, y1 } } };
			floor.surface = CleanSurface::Wall;
			floor.darkness = 8.0F;
			AddCleanQuad(st.art, floor);
		}
		for (const Point t : tiles)
			StructureTiles[static_cast<size_t>(t.y) * MAXDUNX + t.x] = 1;
		Structures.push_back(std::move(st));
	}
}

/**
 * @brief The clean dungeon's structures (fp_dungeon.hpp): the cathedral's stairs, the torch stands (their 3D model
 * read off the art, as before), and the pillars and arches, gathered in chunks of tiles so they are culled together.
 */
void BuildCleanStructures()
{
	if (leveltype == DTYPE_CATHEDRAL)
		BuildCathedralStairs();
	if (IsHell())
		BuildHellStairs();
	std::vector<CleanQuad> quads;
	std::vector<Point> stands;
	std::vector<std::array<float, 4>> holes;
	BuildCleanGeometry(quads, stands, holes);
	FloorHoles.insert(FloorHoles.end(), holes.begin(), holes.end());
	for (const Point tile : stands) {
		uint8_t &mark = StructureTiles[static_cast<size_t>(tile.y) * MAXDUNX + tile.x];
		Structure st;
		if (mark != 0 || !BuildColumn(st, tile, true))
			continue;
		SetStructureFallbacks(st);
		mark = 1;
		Structures.push_back(std::move(st));
	}
	constexpr int Chunk = 4;
	std::unordered_map<int, size_t> chunks;
	for (const CleanQuad &quad : quads) {
		const Point tile { static_cast<int>(std::floor((quad.p[0][0] + quad.p[2][0]) / 2.0F)), static_cast<int>(std::floor((quad.p[0][1] + quad.p[2][1]) / 2.0F)) };
		if (!InDungeonBounds(tile) || StructureTiles[static_cast<size_t>(tile.y) * MAXDUNX + tile.x] != 0)
			continue; // under a stairs model
		const int key = (tile.y / Chunk) * (MAXDUNX / Chunk + 1) + tile.x / Chunk;
		auto [it, added] = chunks.try_emplace(key, Structures.size());
		if (added)
			Structures.emplace_back();
		AddCleanQuad(Structures[it->second].art, quad);
	}
	for (int y = 0; y < MAXDUNY; y++)
		for (int x = 0; x < MAXDUNX; x++)
			if (IsAnyOf(CleanKindAt({ x, y }), CleanKind::Pillar, CleanKind::Fence))
				StructureTiles[static_cast<size_t>(y) * MAXDUNX + x] = 1;
}

void BuildStructures()
{
	PrepareKitTextures();
	for (const Structure &st : Structures)
		ForgetUpscaledTexture(st.art.canvas);
	Structures.clear();
	ObjectBoxes.clear();
	DoorLeaves.clear();
	FloorHoles.clear();
	StructureTiles.assign(static_cast<size_t>(MAXDUNX) * MAXDUNY, 0);
	if (leveltype == DTYPE_TOWN)
		return;
	if (IsCleanDungeon()) {
		BuildCleanStructures();
		FinishStructureBounds();
		LogInfo("fp: {} dungeon structures drawn as meshes (clean dungeon)", Structures.size());
		return;
	}
	// Levels not drawn clean (Hellfire's nest): free-standing columns (and torch stands) read off their art, or else
	// arcade pillars joined by arch walls.
	std::vector<ArcadePillar> pillars;
	for (int y = 0; y < MAXDUNY; y++) {
		for (int x = 0; x < MAXDUNX; x++) {
			if (StructureTiles[static_cast<size_t>(y) * MAXDUNX + x] != 0 || !GetPieceTextures(dPiece[x][y]).isFreeStanding)
				continue;
			Structure st;
			if (!BuildColumn(st, { x, y })) {
				st = Structure {};
				float spring;
				BuildArcadePillar(st, { x, y }, spring);
				pillars.push_back({ { x, y }, spring, Structures.size() });
			}
			SetStructureFallbacks(st);
			StructureTiles[static_cast<size_t>(y) * MAXDUNX + x] = 1;
			Structures.push_back(std::move(st));
		}
	}
	// Arch walls between neighbouring pillars of an arcade, and from end pillars to the wall they meet.
	auto pillarAt = [&](Point t) -> const ArcadePillar * {
		for (const ArcadePillar &p : pillars)
			if (p.tile == t)
				return &p;
		return nullptr;
	};
	for (const ArcadePillar &p : pillars) {
		for (const Point d : { Point { 1, 0 }, Point { 0, 1 }, Point { -1, 0 }, Point { 0, -1 } }) {
			for (int k = 1; k <= 3; k++) {
				const Point t { p.tile.x + d.x * k, p.tile.y + d.y * k };
				if (!InDungeonBounds(t))
					break;
				const ArcadePillar *other = pillarAt(t);
				const Vec2 from { static_cast<float>(p.tile.x) + 0.5F, static_cast<float>(p.tile.y) + 0.5F };
				const Vec2 to { static_cast<float>(t.x) + 0.5F, static_cast<float>(t.y) + 0.5F };
				if (other != nullptr) {
					if (k > 1 && (d.x > 0 || d.y > 0)) // each span once
						AddArcadeWall(Structures[p.structure].art, from, to, std::min(p.spring, other->spring));
					break;
				}
				if (IsTileSolid(t)) {
					if (k > 1 && IsOpaque(t))
						AddArcadeWall(Structures[p.structure].art, from, to, p.spring);
					break;
				}
			}
		}
	}
	FinishStructureBounds();
	LogInfo("fp: {} dungeon structures drawn as meshes ({} arcade pillars)", Structures.size(), pillars.size());
}

/** @brief Each structure's bounds, for culling it on screen. */
void FinishStructureBounds()
{
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
}

void EnsureStructures()
{
	// The clean dungeon is rebuilt when its map changes (doors, levers), the rest when the palette does, but neither with
	// the colours the level cycles (lava, water): the caves turn theirs every frame. (DrawFirstPersonView notes the
	// palette's changes once a frame.)
	const bool clean = IsCleanDungeon();
	const LevelKey key { pDungeonCels.get(), currlevel, static_cast<int>(leveltype), setlevel, clean ? CleanDungeonVersion() : StablePaletteVersion() };
	if (key == StructuresFor)
		return;
	StructuresFor = key;
	StructuresBuildCount++;
	BuildStructures();
}

/** @brief Palette colour (before lighting) of a structure's surface point. */
uint8_t StructureColor(const Structure &st, const WorldTriangle &tri, float wx, float wy, float wz)
{
	if (tri.mapping == MeshFloorArt) {
		const Point tile { static_cast<int>(std::floor(wx)), static_cast<int>(std::floor(wy)) };
		if (!InDungeonBounds(tile))
			return tri.fallback;
		const std::vector<int16_t> &floor = GetPieceTextures(dPiece[tile.x][tile.y]).floor;
		const int u = std::clamp(static_cast<int>((wx - static_cast<float>(tile.x)) * FloorTextureSize), 0, FloorTextureSize - 1);
		const int v = std::clamp(static_cast<int>((wy - static_cast<float>(tile.y)) * FloorTextureSize), 0, FloorTextureSize - 1);
		const int16_t texel = floor.empty() ? int16_t { -1 } : floor[static_cast<size_t>(v) * FloorTextureSize + u];
		return texel > 0 ? PatchColor(texel) : tri.fallback;
	}
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
	if (art > 0)
		return PatchColor(art);
	if (tri.normalX != 0 || tri.normalY != 0) {
		// A side the art does not show: the level's plain wall, laid along the side.
		const std::vector<int16_t> &face = GetFallbackFace();
		const int faceHeight = FaceTextureHeight();
		const float along = std::abs(tri.normalX) > std::abs(tri.normalY) ? wy : wx;
		const int u = std::clamp(static_cast<int>((along - std::floor(along)) * FaceTextureWidth), 0, FaceTextureWidth - 1);
		const int v = std::clamp(WallArtRow(wz), 0, faceHeight - 1);
		int16_t texel = face[static_cast<size_t>(v) * FaceTextureWidth + u];
		if (texel > 0 && ShadeFine(SpritePixelsPerTileY)) {
			const float fv = static_cast<float>(faceHeight) - wz * SpritePixelsPerTileY * WallArtScale(LevelWallRows());
			if (const int16_t fine = FineFaceTexel(face, FaceTextureWidth, (along - std::floor(along)) * FaceTextureWidth, fv); fine > 0)
				texel = fine;
		}
		if (texel > 0)
			return PatchColor(texel);
	}
	return tri.fallback;
}

void DrawStructure(const Surface &out, const Projection &proj, std::vector<float> &depthBuffer, const Structure &st, float stretch = VerticalScale(),
    const std::vector<uint32_t> *order = nullptr)
{
	const ArcadeArt &arcade = GetArcadeArt();
	int left;
	int top;
	int right;
	int bottom;
	if (!ScreenBounds(st.art, proj, left, top, right, bottom, stretch))
		return;
	RasterizeTriangles(out, proj, depthBuffer, st.art.mesh, [&](const WorldTriangle &tri, float wx, float wy, float wz) {
		if (tri.mapping == MeshSprite) {
			const HellSprite *sprite = HellSpriteAt(tri.sprite);
			if (sprite == nullptr)
				return -1;
			const auto [da, db] = DropAxis(tri.bakeAxis, wx - tri.p[0].x, wy - tri.p[0].y, wz - tri.p[0].z);
			const float w1 = tri.bakeInv[0] * da + tri.bakeInv[1] * db;
			const float w2 = tri.bakeInv[2] * da + tri.bakeInv[3] * db;
			const float w0 = 1.0F - w1 - w2;
			const float u = std::clamp(w0 * tri.uv[0] + w1 * tri.uv[2] + w2 * tri.uv[4], 0.0F, 0.9999F);
			const float v = std::clamp(w0 * tri.uv[1] + w1 * tri.uv[3] + w2 * tri.uv[5], 0.0F, 0.9999F);
			// Blown up (up close), from its smoothed copy at TextureUpscale times the resolution. Its outline is where the
			// coverage of the four texels round the point crosses one half (smooth, not in steps); the colour is the
			// nearest of them that is drawn.
			const float texelsPerWorld = static_cast<float>(sprite->width) / std::max(tri.slopeScale, 1e-3F);
			const bool fineLevel = ShadePixelWorld * texelsPerWorld < UpscaleBelow && sprite->fine != nullptr;
			const std::vector<int16_t> &pixels = fineLevel && sprite->fine != nullptr ? *sprite->fine : sprite->pixels;
			const int w = fineLevel ? sprite->width * TextureUpscale : sprite->width;
			const int h = fineLevel ? sprite->height * TextureUpscale : sprite->height;
			const float fu = u * static_cast<float>(w) - 0.5F;
			const float fv = v * static_cast<float>(h) - 0.5F;
			const int u0 = FloorToInt(fu);
			const int v0 = FloorToInt(fv);
			const float tu = fu - static_cast<float>(u0);
			const float tv = fv - static_cast<float>(v0);
			auto sample = [&](int su, int sv) -> int16_t {
				if (su < 0 || sv < 0 || su >= w || sv >= h)
					return NoPixel;
				return pixels[static_cast<size_t>(sv) * w + su];
			};
			const std::array<int16_t, 4> texels { sample(u0, v0), sample(u0 + 1, v0), sample(u0, v0 + 1), sample(u0 + 1, v0 + 1) };
			const std::array<float, 4> weights { (1 - tu) * (1 - tv), tu * (1 - tv), (1 - tu) * tv, tu * tv };
			float coverage = 0;
			int16_t texel = 0;
			float best = -1;
			for (size_t k = 0; k < 4; k++) {
				if (texels[k] <= 0)
					continue;
				coverage += weights[k];
				if (weights[k] > best) {
					best = weights[k];
					texel = texels[k];
				}
			}
			if (coverage < 0.5F || texel <= 0)
				return -1;
			// Where it was cut out of art that goes on, it fades out through an ordered dither (bilinear over its texels).
			if (!sprite->alpha.empty()) {
				const float cu = u * static_cast<float>(sprite->width) - 0.5F;
				const float cv = v * static_cast<float>(sprite->height) - 0.5F;
				const int au = FloorToInt(cu);
				const int av = FloorToInt(cv);
				const float su = cu - static_cast<float>(au);
				const float sv = cv - static_cast<float>(av);
				auto alphaAt = [&](int x, int y) -> float {
					x = std::clamp(x, 0, sprite->width - 1);
					y = std::clamp(y, 0, sprite->height - 1);
					return static_cast<float>(sprite->alpha[static_cast<size_t>(y) * sprite->width + x]);
				};
				const float top = alphaAt(au, av) * (1 - su) + alphaAt(au + 1, av) * su;
				const float bottom = alphaAt(au, av + 1) * (1 - su) + alphaAt(au + 1, av + 1) * su;
				if (top + (bottom - top) * sv < (static_cast<float>(CleanBayer[ShadeY & 3][ShadeX & 3]) + 0.5F) * 16.0F)
					return -1;
			}
			const float light = CleanTriangleLight(tri, wx, wy) + tri.darkness * 0.5F;
			return light >= LightsMax ? 0 : static_cast<int>(FpLightTable(LightLevel(light, ShadeX, ShadeY))[static_cast<uint8_t>(texel)]);
		}
		if (tri.mapping == MeshClean) {
			const auto [u, v] = TriangleUv(tri, wx, wy, wz);
			const float shade = tri.darkness * 0.5F + std::max(0.0F, tri.occlusion[0].at0 + tri.occlusion[0].perZ * wz)
			    + std::max(0.0F, tri.occlusion[1].at0 + tri.occlusion[1].perZ * wz);
			return static_cast<int>(CleanColor(tri.cleanSurface, u, v, ShadePixelWorld, CleanTriangleLight(tri, wx, wy) + shade, ShadeX, ShadeY));
		}
		if (tri.mapping == MeshArcade) {
			const float along = (wx - tri.mirrorX) * tri.downX + (wy - tri.mirrorY) * tri.downY;
			const ArcadeFit fit = FitArcade(arcade, tri.slopeScale, tri.arcSpring);
			float artAlong;
			float artZ;
			ArcadeArtPoint(arcade, fit, along, wz, artAlong, artZ);
			bool second;
			int u;
			ArcadeTexel(artAlong, second, u);
			const int faceHeight = FaceTextureHeight();
			const int row = std::clamp(WallArtRow(artZ), faceHeight - LevelWallRows() + WallCapRows(), faceHeight - 1);
			if (!tri.faceX && wz < ArcadeOpeningTop(arcade, fit, along))
				return -1; // inside the arch
			int16_t texel = 0;
			if (arcade.ok) {
				const PieceTextures &tex = GetPieceTextures(second ? arcade.pairB : arcade.pairA);
				texel = (arcade.useX ? tex.faceX : tex.faceY)[static_cast<size_t>(row) * FaceTextureWidth + u];
			}
			if (texel <= 0)
				texel = GetFallbackFace()[static_cast<size_t>(row) * FaceTextureWidth + u];
			else if (ShadeFine(SpritePixelsPerTileY)) {
				const PieceTextures &tex = GetPieceTextures(second ? arcade.pairB : arcade.pairA);
				const float fu = (artAlong - std::floor(artAlong)) * FaceTextureWidth;
				const float fv = static_cast<float>(faceHeight) - artZ * SpritePixelsPerTileY * WallArtScale(LevelWallRows());
				if (const int16_t fine = FineFaceTexel(arcade.useX ? tex.faceX : tex.faceY, FaceTextureWidth, fu, fv); fine > 0)
					texel = fine;
			}
			const Point lit { static_cast<int>(std::floor(wx)), static_cast<int>(std::floor(wy)) };
			const uint8_t level = InDungeonBounds(lit) ? LightAt(lit) : LightsMax;
			return static_cast<int>(FpLightTable(std::min<uint8_t>(level, LightsMax))[ShadeColor(PatchColor(texel), tri.darkness)]);
		}
		if (tri.mapping == MeshProjectiveCut) {
			float x = wx;
			float y = wy;
			if (tri.mirrored) {
				const float d = (x - tri.mirrorX) * tri.normalX + (y - tri.mirrorY) * tri.normalY;
				x -= 2.0F * d * tri.normalX;
				y -= 2.0F * d * tri.normalY;
			}
			const int px = static_cast<int>(std::floor((x - y) * 32.0F)) - st.art.originX;
			const int py = static_cast<int>(std::floor((x + y) * 16.0F - wz * HeightPixels)) - st.art.originY;
			if (px < 0 || py < 0 || px >= st.art.canvasWidth || py >= st.art.canvasHeight)
				return -1;
			int16_t art = st.art.canvas[static_cast<size_t>(py) * st.art.canvasWidth + px];
			if (art <= 0)
				return -1;
			if (FineCanvas() && ShadeFine(32.0F)) {
				const float fx = (x - y) * 32.0F - static_cast<float>(st.art.originX);
				const float fy = (x + y) * 16.0F - wz * HeightPixels - static_cast<float>(st.art.originY);
				if (const int16_t fine = FineFaceTexel(st.art.canvas, st.art.canvasWidth, fx, fy); fine > 0)
					art = fine;
			}
			const Point lit { static_cast<int>(std::floor(wx)), static_cast<int>(std::floor(wy)) };
			const uint8_t level = InDungeonBounds(lit) ? LightAt(lit) : LightsMax;
			return static_cast<int>(FpLightTable(std::min<uint8_t>(level, LightsMax))[ShadeColor(PatchColor(art), tri.darkness)]);
		}
		if (tri.mapping == MeshWallFace) {
			const PieceTextures &textures = GetPieceTextures(dPiece[static_cast<int>(tri.mirrorX)][static_cast<int>(tri.mirrorY)]);
			const std::vector<int16_t> &face = tri.faceX ? textures.faceX : textures.faceY;
			// Along the face in world-axis order, unmirrored on the far side, as the raycaster does.
			const float along = tri.faceX ? wy - tri.mirrorY : wx - tri.mirrorX;
			const int faceHeight = FaceTextureHeight();
			const int u = std::clamp(static_cast<int>(along * FaceTextureWidth), 0, FaceTextureWidth - 1);
			const int v = WallArtRow(wz);
			if (v < 0 || v >= faceHeight)
				return -1;
			const int16_t texel = face[static_cast<size_t>(v) * FaceTextureWidth + u];
			if (texel <= 0)
				return -1;
			const Point lit { static_cast<int>(std::floor(wx)), static_cast<int>(std::floor(wy)) };
			const uint8_t level = InDungeonBounds(lit) ? LightAt(lit) : LightsMax;
			return static_cast<int>(FpLightTable(std::min<uint8_t>(level, LightsMax))[ShadeColor(PatchColor(texel), tri.darkness)]);
		}
		const uint8_t color = StructureColor(st, tri, wx, wy, wz);
		uint8_t darkness = tri.darkness;
		if (wz < -0.02F)
			darkness = std::max<uint8_t>(darkness, static_cast<uint8_t>(std::min(1.0F + -wz * 3.5F, static_cast<float>(SurfaceShade.size() - 1))));
		const Point tile { static_cast<int>(std::floor(wx)), static_cast<int>(std::floor(wy)) };
		const uint8_t level = InDungeonBounds(tile) ? LightAt(tile) : LightsMax;
		return static_cast<int>(FpLightTable(std::min<uint8_t>(level, LightsMax))[ShadeColor(color, darkness)]);
	}, stretch, order);
}

/** Iron straps across a door leaf: their middles as shares of the leaf's height, and their half height (tiles). */
constexpr std::array<float, 2> DoorStraps { 0.22F, 0.72F };
constexpr float DoorStrapHalf = 0.035F;
constexpr uint8_t DoorStrapDarkness = 4;
/** Planks across a leaf, and the share of a plank's width its dark seam takes. */
constexpr int DoorPlanks = 5;
constexpr float DoorSeamShare = 0.08F;
constexpr uint8_t DoorSeamDarkness = 3;

/** @brief Draws every door's leaf in its doorway, shut, swinging or open. */
void DrawDoorLeaves(const Surface &out, const Projection &proj, std::vector<float> &depthBuffer)
{
	for (int i = 0; i < ActiveObjectCount; i++) {
		const int id = ActiveObjects[i];
		const DoorwaySpan span = DoorwaySpanAt(Objects[id].position);
		if (span.door == nullptr || span.offset != 0 || span.door->object != id)
			continue;
		const Doorway &door = *span.door;
		int side;
		const float angle = DoorSwing(door, side);
		if (LightAt(door.tile) >= LightsMax)
			continue;
		DoorLeaf &leaf = DoorLeaves[id];
		if (leaf.angle != angle || leaf.side != side)
			BuildDoorLeaf(leaf, door, angle, side);
		const int faceHeight = FaceTextureHeight();
		const bool clean = IsCleanDungeon();
		RasterizeTriangles(out, proj, depthBuffer, leaf.mesh, [&](const WorldTriangle &tri, float wx, float wy, float wz) {
			uint8_t darkness = tri.darkness;
			const float offset = (wx - leaf.hinge.x) * leaf.direction.x + (wy - leaf.hinge.y) * leaf.direction.y;
			if (clean && CleanDoorwaysAreGates()) {
				// The caves' gates: a lattice of beams, each wearing the wood by its own texture positions.
				const auto [u, v] = TriangleUv(tri, wx, wy, wz);
				return static_cast<int>(CleanColor(CleanSurface::Wood, u, v, ShadePixelWorld, CleanLightAt(wx, wy) + static_cast<float>(darkness) * 0.5F, ShadeX, ShadeY));
			}
			if (clean) {
				// The clean dungeon's door texture over the whole leaf (its planks and irons are painted on).
				const float u = std::clamp(offset / std::max(leaf.width, 0.01F), 0.0F, 0.999F);
				const float v = std::clamp(1.0F - wz / std::max(leaf.height, 0.01F), 0.0F, 0.999F);
				return static_cast<int>(CleanColor(CleanSurface::Door, u, v, ShadePixelWorld, CleanLightAt(wx, wy) + static_cast<float>(darkness) * 0.5F, ShadeX, ShadeY));
			}
			const float plank = offset / leaf.width * DoorPlanks;
			if (plank - std::floor(plank) < DoorSeamShare && tri.darkness == 0)
				darkness = DoorSeamDarkness;
			for (const float strap : DoorStraps)
				if (std::abs(wz - strap * leaf.height) < DoorStrapHalf)
					darkness = std::max(darkness, DoorStrapDarkness);
			int color = tri.texture >= 0 ? KitMeshColor(tri, wx, wy, wz) : -1;
			if (color >= 0) {
				color = DoorLeafColor(static_cast<uint8_t>(color)); // the kit's grey planks, in Daggerfall's wood colours
			} else {
				// No kit: the closed door's art over the leaf, wherever it has swung.
				const int u = std::max(DoorFaceColumn(door, leaf.alongStart + offset), 0);
				const int row = std::clamp(WallArtRow(wz), 0, faceHeight - 1);
				const int16_t texel = door.face[static_cast<size_t>(row) * FaceTextureWidth + u];
				color = DoorLeafColor(texel > 0 ? PatchColor(texel) : uint8_t { 0 });
			}
			const Point lit { static_cast<int>(std::floor(wx)), static_cast<int>(std::floor(wy)) };
			const uint8_t level = InDungeonBounds(lit) ? LightAt(lit) : LightsMax;
			return static_cast<int>(FpLightTable(std::min<uint8_t>(level, LightsMax))[ShadeColor(static_cast<uint8_t>(color), darkness)]);
		}, VerticalScale());
	}
}

void DrawObjectBoxes(const Surface &out, const Projection &proj, std::vector<float> &depthBuffer)
{
	for (int i = 0; i < ActiveObjectCount; i++) {
		const int id = ActiveObjects[i];
		const Object &object = Objects[id];
		if (!IsBoxObject(object) || !object._oAnimData || !InDungeonBounds(object.position) || dObject[object.position.x][object.position.y] != id + 1)
			continue;
		if (LightAt(object.position) >= LightsMax)
			continue;
		const ClxSprite sprite = object.currentSprite();
		ObjectBox &box = ObjectBoxes[id];
		if (box.frame != sprite.pixelData() || box.tile != object.position) {
			if (!BuildObjectBox(box, object, sprite))
				continue;
		}
		// Nearest first, so the depth test turns hidden pixels away before they are shaded: close up, a sarcophagus fills
		// much of the view, its lid over the top of its body and its handles over its sides.
		const std::vector<WorldTriangle> &mesh = box.st.art.mesh;
		static std::vector<std::pair<float, uint32_t>> byDistance;
		static std::vector<uint32_t> order;
		byDistance.clear();
		for (uint32_t t = 0; t < mesh.size(); t++) {
			const WorldTriangle &tri = mesh[t];
			const float dx = (tri.p[0].x + tri.p[1].x + tri.p[2].x) / 3.0F - proj.pos.x;
			const float dy = (tri.p[0].y + tri.p[1].y + tri.p[2].y) / 3.0F - proj.pos.y;
			const float dz = (tri.p[0].z + tri.p[1].z + tri.p[2].z) / 3.0F - EyeHeight;
			byDistance.emplace_back(dx * dx + dy * dy + dz * dz, t);
		}
		std::sort(byDistance.begin(), byDistance.end());
		order.clear();
		for (const auto &[distance, t] : byDistance)
			order.push_back(t);
		DrawStructure(out, proj, depthBuffer, box.st, 1.0F, &order); // objects keep their own size
	}
}

// ---------------------------------------------------------------------------------------------------------------
// Town props: the trees, boulders, low walls, gravestones and grass tufts that the iso art paints on single tiles.
// They were billboards of each tile's own column (cardboards turning with the camera); here they are meshes:
// trees a trunk and a canopy, the rest the visual hull of their art seen from the iso camera, a few tenths of a
// tile deep. Both are textured by projecting the art back on (the far sides mirror it), so from the iso camera's
// side they look as they did.

enum class PropKind : uint8_t {
	None,     // not a prop (no tall art)
	Tree,     // a trunk and a canopy
	Rock,     // a boulder: the hull of its art, as deep as a boulder
	Wall,     // a low stone wall: a wall's thickness deep
	Slab,     // a gravestone: a slab's thickness deep
	Grass,    // a tuft: shallow, see-through where the art is
	Leftover, // a piece of the church's art that its model stands in for: hidden
};

/** @brief Visual hull depth behind the art's front (tiles). */
float PropThickness(PropKind kind)
{
	switch (kind) {
	case PropKind::Rock: return 1.1F;
	case PropKind::Wall: return 0.36F;
	case PropKind::Slab: return 0.14F;
	default: return 0.3F;
	}
}

/** @brief The hull's grid cells per tile side: thin things need finer cells. */
int PropCells(PropKind kind)
{
	return kind == PropKind::Wall || kind == PropKind::Slab ? 6 : 4;
}

/** @brief Mean colour, opaque pixel count and height (rows above the column's bottom) of a piece's upright art. */
struct ArtStats {
	int opaque = 0;
	int rows = 0;
	float r = 0;
	float g = 0;
	float b = 0;
};

ArtStats UprightStats(const PieceTextures &t)
{
	ArtStats s;
	const int rows = static_cast<int>(t.upright.size() / ColumnWidth);
	int top = rows;
	for (int i = 0; i < static_cast<int>(t.upright.size()); i++) {
		if (t.upright[i] <= 0)
			continue;
		const SDL_Color c = logical_palette[t.upright[i]];
		s.r += c.r;
		s.g += c.g;
		s.b += c.b;
		s.opaque++;
		top = std::min(top, i / ColumnWidth);
	}
	if (s.opaque > 0) {
		s.r /= static_cast<float>(s.opaque);
		s.g /= static_cast<float>(s.opaque);
		s.b /= static_cast<float>(s.opaque);
	}
	s.rows = rows - top;
	return s;
}

/** @brief Whether a town piece is a boulder's (the known boulder pieces, or one its art shows is). */
bool IsTownRockPiece(uint16_t piece)
{
	// (556 and 558, once taken for boulders, are Adria's cauldron: a landmark of its own.)
	return (piece >= 220 && piece <= 232) || piece == 835 || piece == 836 || piece == 1166 || piece >= 1202;
}

/** @brief Whether a town piece is one of the trees' (its art is the tree alone, even beside a house). */
bool IsTreePiece(uint16_t piece)
{
	return (piece >= 116 && piece <= 216) || (piece >= 357 && piece <= 363);
}

/**
 * @brief What a town piece's tall art shows. The pieces of Diablo's town are known (dev.fp.props lists those still
 * drawn as billboards, with their colours); any other piece is told by its art: autumn foliage is warm, stone
 * blue-grey, and small art is a tuft.
 */
PropKind TownPieceKind(uint16_t piece)
{
	const PieceTextures &t = GetPieceTextures(piece);
	if (!t.hasTallArt || t.upright.empty())
		return PropKind::None;
	if (piece >= 104 && piece <= 114)
		return PropKind::Grass;
	if (IsTreePiece(piece))
		return PropKind::Tree;
	if (IsTownRockPiece(piece))
		return PropKind::Rock;
	if ((piece >= 837 && piece <= 847) || (piece >= 998 && piece <= 1029))
		return PropKind::Leftover;
	if (piece >= 1049 && piece <= 1112)
		return PropKind::Wall; // 1049 and 1050: a wall's crumbled end
	if (piece >= 957 && piece <= 1048)
		return PropKind::Slab;
	const ArtStats s = UprightStats(t);
	if (s.opaque == 0)
		return PropKind::None;
	if (s.opaque < 160 && s.rows < 24)
		return PropKind::Grass;
	return s.r > s.b + 4.0F ? PropKind::Tree : PropKind::Rock;
}

/** Screen pixels per tile of depth along the iso view (moving (1, 1) / sqrt 2 changes x + y by sqrt 2). */
constexpr float IsoDepthPixels = 16.0F * 1.41421356F;

/** @brief A prop mesh drawn as one piece: the hulls of the props in a block of tiles. */
struct TownProp {
	Building art {}; // bounds and mesh; each triangle textured from its tile's column of art (WorldTriangle::artX, artY)
	PropKind kind = PropKind::None;
	bool ownCanvas = false; // textured from art.canvas instead (rocks and gravestones: the art of their group of tiles)
	Vec2 middle { 0, 0 }; // for culling by distance
	float radius = 0;
	std::vector<WorldTriangle> farMesh; // fewer triangles, for far away (PropFarDistance)
};

/** Props farther than this (tiles) are drawn from their far mesh: coarser hulls. */
constexpr float PropFarDistance = 11.0F;

std::vector<TownProp> TownProps;

/** @brief A piece's hull in the tile's own frame (x, y in 0..1), with the point its far sides mirror through. */
struct PropHull {
	std::vector<WorldTriangle> triangles;
	Vec2 middle { 0.5F, 0.5F };
	float top = 0;
};
/** @brief A piece's hulls: for near, and coarser for far away. */
struct PropHullPair {
	PropHull near;
	PropHull far;
};
std::unordered_map<uint16_t, PropHullPair> PropHulls;

/** @brief Art pixel of a tile's upright column at the iso screen position of a world point (NoPixel if none). */
int16_t ColumnArtAt(const PieceTextures &t, Point tile, float wx, float wy, float wz)
{
	const int rows = static_cast<int>(t.upright.size() / ColumnWidth);
	const int cx = static_cast<int>(std::floor((wx - wy) * 32.0F)) - (tile.x - tile.y) * 32 + 32;
	const int cy = static_cast<int>(std::floor((wx + wy) * 16.0F - wz * HeightPixels)) - (tile.x + tile.y + 2) * 16 + rows - 1;
	if (cx < 0 || cy < 0 || cx >= ColumnWidth || cy >= rows)
		return NoPixel;
	return t.upright[static_cast<size_t>(cy) * ColumnWidth + cx];
}

/** @brief The commonest colour of some art pixels (0 if none). */
uint8_t CommonestColor(const std::vector<int16_t> &pixels)
{
	std::array<int, 256> counts {};
	for (const int16_t p : pixels)
		if (p > 0)
			counts[p]++;
	const auto best = std::max_element(counts.begin() + 1, counts.end());
	return *best > 0 ? static_cast<uint8_t>(best - counts.begin()) : uint8_t { 0 };
}

/**
 * @brief The visual hull of a piece's upright art, `kind`'s thickness deep, in the tile's own frame.
 *
 * The tile is cut into cells. A cell is as tall as its vertical stands inside the art: from the cell's point on
 * the ground up the column of art pixels above it in iso (gaps of a pixel or two bridged). The lowest art pixel in
 * each screen column is the prop's front, where it meets the ground; cells farther behind it than the thickness
 * stay empty, so a wall or a gravestone does not grow into a ramp behind it. The corners take the mean height of
 * the cells standing around them, and the sides go straight down to the ground.
 */
/** @brief The hull's cell heights (n x n, row j = v): see BuildPropHull. */
std::vector<float> PropHullHeights(const PieceTextures &t, float thickness, int n)
{
	std::vector<float> height(static_cast<size_t>(n) * n, 0.0F);
	const int rows = static_cast<int>(t.upright.size() / ColumnWidth);
	if (rows == 0)
		return height;
	auto opaque = [&](int cx, int cy) { return cx >= 0 && cy >= 0 && cx < ColumnWidth && cy < rows && t.upright[static_cast<size_t>(cy) * ColumnWidth + cx] > 0; };
	std::array<int, ColumnWidth> low;
	for (int cx = 0; cx < ColumnWidth; cx++) {
		low[cx] = -1;
		for (int cy = rows - 1; cy >= 0 && low[cx] < 0; cy--)
			if (opaque(cx, cy))
				low[cx] = cy;
	}
	for (int j = 0; j < n; j++) {
		for (int i = 0; i < n; i++) {
			const float u = (static_cast<float>(i) + 0.5F) / static_cast<float>(n);
			const float v = (static_cast<float>(j) + 0.5F) / static_cast<float>(n);
			const int cx = std::clamp(static_cast<int>(std::floor((u - v) * 32.0F)) + 32, 0, ColumnWidth - 1);
			// The cell's ground point in the column: tile corner (0, 0) is the diamond's top vertex, 32 rows above its bottom.
			const float base = (u + v - 2.0F) * 16.0F + static_cast<float>(rows - 1);
			if (low[cx] < 0 || base > static_cast<float>(low[cx]) + 3.0F)
				continue; // in front of the art: open ground
			if ((static_cast<float>(low[cx]) - base) / IsoDepthPixels > thickness)
				continue; // too far behind the art's front
			int top = -1;
			int gap = 0;
			for (int cy = std::min(static_cast<int>(base), low[cx]); cy >= 0; cy--) {
				if (opaque(cx, cy) || opaque(cx - 1, cy) || opaque(cx + 1, cy)) {
					top = cy;
					gap = 0;
				} else if (++gap > 2) {
					break;
				}
			}
			const float h = top >= 0 ? (base - static_cast<float>(top)) / HeightPixels : 0.0F;
			if (h >= 0.04F)
				height[static_cast<size_t>(j) * n + i] = h;
		}
	}
	return height;
}

PropHull BuildPropHull(uint16_t piece, PropKind kind, int n)
{
	const PieceTextures &t = GetPieceTextures(piece);
	PropHull hull;
	const std::vector<float> height = PropHullHeights(t, PropThickness(kind), n);
	float sumX = 0;
	float sumY = 0;
	int standing = 0;
	for (int j = 0; j < n; j++)
		for (int i = 0; i < n; i++)
			if (height[static_cast<size_t>(j) * n + i] > 0) {
				sumX += (static_cast<float>(i) + 0.5F) / static_cast<float>(n);
				sumY += (static_cast<float>(j) + 0.5F) / static_cast<float>(n);
				standing++;
			}

	if (standing == 0)
		return hull;
	hull.middle = { sumX / static_cast<float>(standing), sumY / static_cast<float>(standing) };
	auto cellHeight = [&](int i, int j) { return i < 0 || j < 0 || i >= n || j >= n ? 0.0F : height[static_cast<size_t>(j) * n + i]; };
	auto cornerHeight = [&](int i, int j) {
		float sum = 0;
		int count = 0;
		for (int dj = -1; dj <= 0; dj++)
			for (int di = -1; di <= 0; di++)
				if (const float h = cellHeight(i + di, j + dj); h > 0) {
					sum += h;
					count++;
				}
		return count > 0 ? sum / static_cast<float>(count) : 0.0F;
	};
	// Its own colour where the art has none (the sides it does not show).
	std::vector<int16_t> pixels(t.upright.begin(), t.upright.end());
	const uint8_t fallback = PatchColor(static_cast<int16_t>(std::max<uint8_t>(CommonestColor(pixels), 1)));
	const uint8_t mapping = kind == PropKind::Grass ? MeshProjectiveCut : MeshProjective;
	Building mesh {};
	const float cell = 1.0F / static_cast<float>(n);
	for (int j = 0; j < n; j++) {
		for (int i = 0; i < n; i++) {
			if (cellHeight(i, j) <= 0)
				continue;
			const float x0 = static_cast<float>(i) * cell;
			const float y0 = static_cast<float>(j) * cell;
			const float x1 = x0 + cell;
			const float y1 = y0 + cell;
			const V3 a { x0, y0, cornerHeight(i, j) };
			const V3 b { x1, y0, cornerHeight(i + 1, j) };
			const V3 c { x1, y1, cornerHeight(i + 1, j + 1) };
			const V3 d { x0, y1, cornerHeight(i, j + 1) };
			hull.top = std::max({ hull.top, a.z, b.z, c.z, d.z });
			auto topFace = [&](V3 p, V3 q, V3 r) {
				const V3 e1 { q.x - p.x, q.y - p.y, q.z - p.z };
				const V3 e2 { r.x - p.x, r.y - p.y, r.z - p.z };
				V3 normal { e1.y * e2.z - e1.z * e2.y, e1.z * e2.x - e1.x * e2.z, e1.x * e2.y - e1.y * e2.x };
				if (normal.z < 0)
					normal = { -normal.x, -normal.y, -normal.z };
				AddStructureFace(mesh, { p, q, r }, normal, hull.middle, mapping);
			};
			// Split along the diagonal whose ends differ less, which keeps the top nearer flat.
			if (std::abs(a.z - c.z) <= std::abs(b.z - d.z)) {
				topFace(a, b, c);
				topFace(a, c, d);
			} else {
				topFace(a, b, d);
				topFace(b, c, d);
			}
			// Sides where the next cell is open ground.
			auto side = [&](V3 p, V3 q, V3 normal) {
				if (p.z <= 0 && q.z <= 0)
					return;
				AddStructureFace(mesh, { V3 { p.x, p.y, 0 }, V3 { q.x, q.y, 0 }, q, p }, normal, hull.middle, mapping);
			};
			if (cellHeight(i, j - 1) <= 0)
				side(a, b, { 0, -1, 0 });
			if (cellHeight(i + 1, j) <= 0)
				side(b, c, { 1, 0, 0 });
			if (cellHeight(i, j + 1) <= 0)
				side(c, d, { 0, 1, 0 });
			if (cellHeight(i - 1, j) <= 0)
				side(d, a, { -1, 0, 0 });
		}
	}
	for (WorldTriangle &tri : mesh.mesh)
		tri.fallback = fallback;
	hull.triangles = std::move(mesh.mesh);
	return hull;
}

/** @brief Stitches the upright art of some tiles in iso, nearer tiles over farther ones, as the game draws them. */
void BuildUprightCanvas(Building &b, const std::vector<Point> &tiles, bool withGround = false)
{
	BuildCanvas(b, tiles, {});
	std::fill(b.canvas.begin(), b.canvas.end(), int16_t { -1 });
	std::vector<Point> sorted = tiles;
	std::sort(sorted.begin(), sorted.end(), [](Point p, Point q) { return p.x + p.y != q.x + q.y ? p.x + p.y < q.x + q.y : p.x < q.x; });
	for (const Point t : sorted) {
		const PieceTextures &textures = GetPieceTextures(dPiece[t.x][t.y]);
		const std::vector<int16_t> &art = withGround ? textures.column : textures.upright;
		const int rows = static_cast<int>(art.size() / ColumnWidth);
		const int left = (t.x - t.y) * 32 - 32 - b.originX;
		const int top = (t.x + t.y + 2) * 16 - (rows - 1) - b.originY;
		for (int cy = 0; cy < rows; cy++) {
			for (int cx = 0; cx < ColumnWidth; cx++) {
				const int16_t pixel = art[static_cast<size_t>(cy) * ColumnWidth + cx];
				if (pixel <= 0)
					continue;
				const int x = left + cx;
				const int y = top + cy;
				if (x >= 0 && y >= 0 && x < b.canvasWidth && y < b.canvasHeight)
					b.canvas[static_cast<size_t>(y) * b.canvasWidth + x] = pixel;
			}
		}
	}
	b.fitCanvas.clear();
}

/**
 * @brief A town tree: its own art, cut out of the iso picture, drawn as one billboard turned to face the camera
 * wherever it stands (as the cows are), on the foot of its trunk.
 */
struct TreeSprite {
	Vec2 foot;
	std::vector<int16_t> pixels; // -1 transparent, row 0 the top
	int width = 0;
	int height = 0;
	int belowFoot = 0; // rows of art under the foot (sunk into the ground)
};

std::vector<TreeSprite> TreeSprites;

/**
 * @brief Cuts the trees out of a group of neighbouring tree tiles. Each tile whose art comes down to the middle of
 * its own ground diamond holds a trunk's foot; every pixel of the group's art goes to the trunk nearest it across
 * the screen (of those whose foot is not above it), and each trunk's pixels become its sprite, centred on its foot.
 */
void BuildTrees(const std::vector<Point> &tiles, int &treeCount)
{
	Building b {};
	BuildUprightCanvas(b, tiles);
	if (b.canvas.empty())
		return;
	// The same art with its ground: the trunks' feet, which are drawn in the floor diamonds in colours the ground
	// uses too, went with the ground (RemoveUprightGround) and are traced back from it.
	Building raw {};
	BuildUprightCanvas(raw, tiles, true);
	if (raw.canvasWidth != b.canvasWidth || raw.canvasHeight != b.canvasHeight)
		return;
	const int w = b.canvasWidth;
	const int h = b.canvasHeight;
	auto at = [&](int x, int y) { return static_cast<size_t>(y) * w + x; };
	std::vector<uint8_t> inDiamond(b.canvas.size(), 0);
	std::vector<std::pair<float, float>> diamondMiddles;
	for (const Point t : tiles) {
		const float cx = static_cast<float>((t.x - t.y) * 32 - b.originX);
		const float cy = static_cast<float>((t.x + t.y + 2) * 16 - 16 - b.originY);
		diamondMiddles.emplace_back(cx, cy);
		for (int y = static_cast<int>(cy) - 16; y <= static_cast<int>(cy) + 16; y++)
			for (int x = static_cast<int>(cx) - 32; x <= static_cast<int>(cx) + 32; x++)
				if (x >= 0 && y >= 0 && x < w && y < h && std::abs(static_cast<float>(x) + 0.5F - cx) / 32.0F + std::abs(static_cast<float>(y) + 0.5F - cy) / 16.0F <= 1.0F)
					inDiamond[at(x, y)] = 1;
	}
	auto leafy = [](int16_t pixel) {
		const SDL_Color c = logical_palette[pixel];
		return c.r >= c.g + 6 || c.r >= c.b + 10;
	};
	struct Trunk {
		float sx; // iso screen position of its foot
		float sy;
	};
	// The bark above the floor diamonds is the trees' own (trunks and branches). In the diamonds each trunk goes on
	// down over the pale ground under its tree (earth and fallen leaves) to its foot, near the diamond's middle. It is
	// followed there row by row by its dark core, the bark's darkest shades, which that ground has next to none of,
	// and goes on as wide as it is above (moved a pixel to keep its core in it, where it leans): its lit side there is
	// as bright as the ground, and the art draws it thin and broken, so where the ground shows through, it is the bark
	// above.
	auto luminance = [](int16_t pixel) {
		const SDL_Color c = logical_palette[pixel];
		return (c.r * 3 + c.g * 6 + c.b) / 10;
	};
	constexpr int DarkBark = 10;   // the trunk's core, in the diamonds
	constexpr int ShadedBark = 22; // the trunk's lit side there (and some of the ground)
	constexpr int WidestTrunk = 12;
	struct TrunkRow {
		int from;
		int to;
		int shift; // from the trunk above
	};
	std::vector<uint8_t> trunkPixel(b.canvas.size(), 0);
	for (size_t i = 0; i < b.canvas.size(); i++)
		trunkPixel[i] = inDiamond[i] == 0 && b.canvas[i] > 0 && !leafy(b.canvas[i]) ? 1 : 0;
	// Bark is near black or blue; the shadows on the ground (a trunk's own among them) are mostly green or yellow-grey.
	auto bark = [&](int x, int y, int brightest) {
		const int16_t p = raw.canvas[at(x, y)];
		return p > 0 && !leafy(p) && luminance(p) <= brightest && logical_palette[p].b >= logical_palette[p].g;
	};
	for (int y = 1; y < h; y++) {
		std::vector<TrunkRow> runs; // this row's trunk, from each run of trunk in the row above
		for (int x = 0; x < w; x++) {
			if (trunkPixel[at(x, y - 1)] == 0)
				continue;
			int aboveFrom = x;
			while (x + 1 < w && trunkPixel[at(x + 1, y - 1)] != 0)
				x++;
			int aboveTo = x;
			// Its dark core here, in a diamond, a pixel either side of it at most.
			int from = w;
			int to = -1;
			for (int cx = std::max(0, aboveFrom - 1); cx <= std::min(w - 1, aboveTo + 1); cx++)
				if (inDiamond[at(cx, y)] != 0 && bark(cx, y, DarkBark)) {
					from = std::min(from, cx);
					to = std::max(to, cx);
				}
			if (to < 0)
				continue; // the trunk ends above (or is not over a diamond)
			if (aboveTo - aboveFrom + 1 > WidestTrunk) {
				// A trunk with a branch beside it above: the trunk round its core.
				aboveFrom = std::max(0, (from + to) / 2 - WidestTrunk / 2);
				aboveTo = std::min(w - 1, aboveFrom + WidestTrunk - 1);
			}
			const int shift = from < aboveFrom && to <= aboveTo ? -1 : (to > aboveTo && from >= aboveFrom ? 1 : 0);
			runs.push_back({ std::max(0, aboveFrom + shift), std::min(w - 1, aboveTo + shift), shift });
		}
		for (const TrunkRow &run : runs) {
			for (int cx = run.from; cx <= run.to; cx++) {
				if (inDiamond[at(cx, y)] == 0 || trunkPixel[at(cx, y)] != 0)
					continue;
				int16_t pixel = raw.canvas[at(cx, y)];
				if (!bark(cx, y, ShadedBark)) {
					// The ground showing through: the bark above (where the trunk was, before it leant a pixel).
					const int ax = std::clamp(cx - run.shift, 0, w - 1);
					pixel = trunkPixel[at(ax, y - 1)] != 0 ? b.canvas[at(ax, y - 1)] : -1;
					for (int d = 1; pixel <= 0 && d <= WidestTrunk; d++) {
						for (const int nx : { ax - d, ax + d })
							if (pixel <= 0 && nx >= 0 && nx < w && trunkPixel[at(nx, y - 1)] != 0)
								pixel = b.canvas[at(nx, y - 1)];
					}
				}
				if (pixel <= 0)
					continue;
				trunkPixel[at(cx, y)] = 1;
				b.canvas[at(cx, y)] = pixel;
			}
		}
	}
	// In the floor diamonds only the trunks and the leaves of the canopy stay: not the shadows on the ground, nor the
	// leaves fallen round the feet (bits of leaf on their own, wholly in a diamond).
	for (size_t i = 0; i < b.canvas.size(); i++)
		if (inDiamond[i] != 0 && trunkPixel[i] == 0 && b.canvas[i] > 0 && !leafy(b.canvas[i]))
			b.canvas[i] = -1;
	// The trees: each piece of bark big enough is a trunk and its branches, its lowest pixel the foot.
	std::vector<Trunk> trunks;
	{
		std::vector<uint8_t> seen(b.canvas.size(), 0);
		for (int y0 = 0; y0 < h; y0++) {
			for (int x0 = 0; x0 < w; x0++) {
				if (trunkPixel[at(x0, y0)] == 0 || seen[at(x0, y0)] != 0 || b.canvas[at(x0, y0)] <= 0)
					continue;
				std::vector<std::pair<int, int>> pixels;
				std::vector<std::pair<int, int>> stack { { x0, y0 } };
				seen[at(x0, y0)] = 1;
				while (!stack.empty()) {
					const auto [x, y] = stack.back();
					stack.pop_back();
					pixels.emplace_back(x, y);
					for (int dy = -1; dy <= 1; dy++)
						for (int dx = -1; dx <= 1; dx++) {
							const int nx = x + dx;
							const int ny = y + dy;
							if (nx < 0 || ny < 0 || nx >= w || ny >= h || seen[at(nx, ny)] != 0 || trunkPixel[at(nx, ny)] == 0 || b.canvas[at(nx, ny)] <= 0)
								continue;
							seen[at(nx, ny)] = 1;
							stack.emplace_back(nx, ny);
						}
				}
				if (pixels.size() < 80) {
					// A bit of bark on its own: what the art draws on the ground (a fallen twig, a trunk's shadow).
					bool low = true;
					for (const auto &[x, y] : pixels)
						low = low && (inDiamond[at(x, y)] != 0 || (y + 3 < h && inDiamond[at(x, y + 3)] != 0));
					if (low)
						for (const auto &[x, y] : pixels)
							b.canvas[at(x, y)] = -1;
					continue;
				}
				int footY = -1;
				for (const auto &[x, y] : pixels)
					footY = std::max(footY, y);
				bool grounded = false;
				for (int dy = 0; dy <= 4 && !grounded; dy++)
					for (const auto &[x, y] : pixels)
						if (y == footY && footY + dy < h && inDiamond[at(x, footY + dy)] != 0) {
							grounded = true;
							break;
						}
				if (!grounded)
					continue; // a branch, cut off from its trunk by the leaves in front of it
				// A trunk the art draws lit where it goes into the ground (its dark core gone a few rows) is lost
				// there: it goes on down to the middle of the diamond under it, its bark the widest of its lowest rows
				// (the runs of bark over its foot, not branches beside it) drawn on down, with those just as wide in
				// turn, leaning a little as they lean where it is one stem (not two), and darker as it nears the
				// ground (a lit tip drawn on would be a pale pipe).
				{
					// The runs of bark in a row, those that reach to within a few pixels of the foot's.
					auto runsAt = [&](int row, int bandFrom, int bandTo) {
						std::vector<int> xs;
						for (const auto &[x, y] : pixels)
							if (y == row)
								xs.push_back(x);
						std::sort(xs.begin(), xs.end());
						std::vector<std::pair<int, int>> runs;
						for (size_t i = 0; i < xs.size(); i++) {
							if (runs.empty() || xs[i] > runs.back().second + 1)
								runs.emplace_back(xs[i], xs[i]);
							else
								runs.back().second = xs[i];
						}
						std::erase_if(runs, [&](const std::pair<int, int> &r) { return r.second < bandFrom || r.first > bandTo; });
						return runs;
					};
					auto width = [](const std::vector<std::pair<int, int>> &runs) {
						int n = 0;
						for (const auto &[from, to] : runs)
							n += to - from + 1;
						return n;
					};
					constexpr int Band = 3;
					const std::vector<std::pair<int, int>> foot = runsAt(footY, 0, w);
					const int footFrom = foot.front().first;
					const int footTo = foot.back().second;
					float middleY = -1;
					const float px = static_cast<float>(footFrom + footTo) / 2.0F + 0.5F;
					for (int dy = 0; dy <= 8 && middleY < 0; dy++)
						for (const auto &[cx, cy] : diamondMiddles)
							if (std::abs(px - cx) / 32.0F + std::abs(static_cast<float>(footY + dy) + 0.5F - cy) / 16.0F <= 1.0F)
								middleY = cy;
					const int target = static_cast<int>(middleY) - 1;
					if (middleY >= 0 && footY < target - 3 && target < h) {
						constexpr int Lowest = 6;        // the rows looked at
						constexpr float Lean = 0.5F;     // pixels a row at most
						constexpr float GroundShade = 0.6F; // how much darker it is at the ground
						std::vector<std::vector<std::pair<int, int>>> rows;
						int widest = 0;
						for (int row = footY; row > footY - Lowest && row >= 0; row--) {
							rows.push_back(runsAt(row, footFrom - Band, footTo + Band));
							widest = std::max(widest, width(rows.back()));
						}
						std::vector<size_t> sources; // its widest lowest rows: those the same as the lowest of them
						for (size_t i = 0; i < rows.size(); i++)
							if (width(rows[i]) == widest && (sources.empty() || rows[i] == rows[sources.front()]))
								sources.push_back(i);
						const std::vector<std::pair<int, int>> &top = rows.back();
						const bool oneStem = foot.size() == 1 && top.size() == 1 && rows.size() == static_cast<size_t>(Lowest);
						const float lean = oneStem ? std::clamp(static_cast<float>(footFrom + footTo - top[0].first - top[0].second) / (2.0F * (Lowest - 1)), -Lean, Lean)
						                           : 0.0F;
						const std::vector<std::pair<int, int>> &runs = rows[sources.front()];
						for (int y = footY + 1; y <= target; y++) {
							const int from = footY - static_cast<int>(sources[static_cast<size_t>(y - footY - 1) % sources.size()]);
							// That row under the foot's middle, leaning on from there.
							const int offset = (footFrom + footTo - runs.front().first - runs.back().second) / 2 + static_cast<int>(std::lround(lean * static_cast<float>(y - footY)));
							const float shade = 1.0F - GroundShade * static_cast<float>(y - footY) / static_cast<float>(target - footY);
							for (const auto &[runFrom, runTo] : runs)
								for (int x = runFrom; x <= runTo; x++) {
									const int nx = x + offset;
									if (nx < 0 || nx >= w || trunkPixel[at(nx, y)] != 0)
										continue;
									const SDL_Color c = logical_palette[b.canvas[at(x, from)]];
									b.canvas[at(nx, y)] = NearestColor(c.r * shade, c.g * shade, c.b * shade);
									trunkPixel[at(nx, y)] = 1;
									seen[at(nx, y)] = 1;
									pixels.emplace_back(nx, y);
								}
						}
						footY = target;
					}
				}
				int sum = 0;
				int n = 0;
				for (const auto &[x, y] : pixels)
					if (y >= footY - 1) {
						sum += x;
						n++;
					}
				const float sx = static_cast<float>(sum) / static_cast<float>(n) + 0.5F + static_cast<float>(b.originX);
				bool near = false;
				for (const Trunk &k : trunks)
					near = near || std::abs(k.sx - sx) < 20.0F;
				if (!near)
					trunks.push_back({ sx, static_cast<float>(footY + b.originY) });
			}
		}
	}
	// Round each foot only the trunk: the rest there is leaves fallen on the ground and the trunk's shadow.
	for (const Trunk &k : trunks) {
		const int footX = static_cast<int>(k.sx) - b.originX;
		const int footY = static_cast<int>(k.sy) - b.originY;
		for (int y = std::max(0, footY - 8); y <= std::min(h - 1, footY + 16); y++)
			for (int x = std::max(0, footX - 45); x <= std::min(w - 1, footX + 45); x++)
				if (inDiamond[at(x, y)] != 0 && trunkPixel[at(x, y)] == 0)
					b.canvas[at(x, y)] = -1;
	}
	if (trunks.empty()) {
		// No trunk in the group's own tiles (it stands on a building's): the lowest art.
		int lowestY = -1;
		int atX = 0;
		for (int y = 0; y < h; y++)
			for (int x = 0; x < w; x++)
				if (b.canvas[at(x, y)] > 0 && y > lowestY) {
					lowestY = y;
					atX = x;
				}
		if (lowestY < 0)
			return;
		trunks.push_back({ static_cast<float>(atX + b.originX), static_cast<float>(lowestY + b.originY) });
	}
	{
		std::vector<int> component(b.canvas.size(), -1);
		int next = 0;
		for (int y0 = 0; y0 < h; y0++) {
			for (int x0 = 0; x0 < w; x0++) {
				if (b.canvas[at(x0, y0)] <= 0 || component[at(x0, y0)] >= 0)
					continue;
				std::vector<std::pair<int, int>> pixels;
				std::vector<std::pair<int, int>> stack { { x0, y0 } };
				component[at(x0, y0)] = next;
				bool outside = false;
				while (!stack.empty()) {
					const auto [x, y] = stack.back();
					stack.pop_back();
					pixels.emplace_back(x, y);
					outside = outside || inDiamond[at(x, y)] == 0;
					for (int dy = -1; dy <= 1; dy++)
						for (int dx = -1; dx <= 1; dx++) {
							const int nx = x + dx;
							const int ny = y + dy;
							if (nx < 0 || ny < 0 || nx >= w || ny >= h || b.canvas[at(nx, ny)] <= 0 || component[at(nx, ny)] >= 0)
								continue;
							component[at(nx, ny)] = next;
							stack.emplace_back(nx, ny);
						}
				}
				if (!outside)
					for (const auto &[x, y] : pixels)
						b.canvas[at(x, y)] = -1;
				next++;
			}
		}
	}
#ifdef _DEBUG
	if (const char *dir = std::getenv("FP_DUMP_TREES"); dir != nullptr) {
		static int group = 0;
		std::string feet;
		for (const Trunk &k : trunks)
			feet += StrCat(" ", static_cast<int>(k.sx) - b.originX, ",", static_cast<int>(k.sy) - b.originY);
		LogInfo("fp: tree group {} (trees from {}): feet{}", group, treeCount, feet);
		for (const auto &[name, canvas] : { std::pair { "raw", &raw.canvas }, std::pair { "group", &b.canvas } }) {
			if (FILE *f = std::fopen(StrCat(dir, "/", name, "_", group, ".ppm").c_str(), "wb"); f != nullptr) {
				std::fprintf(f, "P6\n%d %d\n255\n", w, h);
				for (size_t i = 0; i < canvas->size(); i++) {
					const int16_t p = (*canvas)[i];
					SDL_Color c = p > 0 ? logical_palette[p] : SDL_Color { 0, 120, 0, 255 };
					if (inDiamond[i] != 0 && p <= 0)
						c = SDL_Color { 0, 70, 0, 255 };
					const uint8_t rgb[3] { c.r, c.g, c.b };
					std::fwrite(rgb, 1, 3, f);
				}
				std::fclose(f);
			}
		}
		group++;
	}
#endif
	// Each art pixel to its trunk: the nearest across the screen (a canopy spreads sideways more than it hangs), of
	// the trunks whose foot is below it. Nothing below its tree's foot stays: what is there is the ground's (a twig,
	// fallen leaves, the edge of a diamond's art) or leaves the iso view shows lower than the foot because they hang
	// out towards it, which on the tree's upright card would be sunk into the ground, poking out of it.
	std::vector<std::vector<std::pair<int, int>>> owned(trunks.size());
	for (int y = 0; y < b.canvasHeight; y++) {
		for (int x = 0; x < b.canvasWidth; x++) {
			if (b.canvas[static_cast<size_t>(y) * b.canvasWidth + x] <= 0)
				continue;
			const float sx = static_cast<float>(x + b.originX);
			const float sy = static_cast<float>(y + b.originY);
			size_t best = trunks.size();
			float bestDistance = std::numeric_limits<float>::max();
			for (int pass = 0; pass < 2 && best == trunks.size(); pass++) {
				for (size_t k = 0; k < trunks.size(); k++) {
					if (pass == 0 && sy > trunks[k].sy + 2.0F)
						continue;
					const float d = std::abs(sx - trunks[k].sx) + 0.25F * std::abs(sy - (trunks[k].sy - 70.0F));
					if (d < bestDistance) {
						bestDistance = d;
						best = k;
					}
				}
			}
			if (sy > trunks[best].sy + 1.0F)
				continue;
			owned[best].emplace_back(x, y);
		}
	}
	for (size_t k = 0; k < trunks.size(); k++) {
		if (owned[k].size() < 20)
			continue;
		const int footX = static_cast<int>(std::lround(trunks[k].sx)) - b.originX;
		const int footY = static_cast<int>(std::lround(trunks[k].sy)) - b.originY;
		int halfWidth = 0;
		int top = footY;
		int bottom = footY;
		for (const auto &[x, y] : owned[k]) {
			halfWidth = std::max(halfWidth, std::abs(x - footX));
			top = std::min(top, y);
			bottom = std::max(bottom, y);
		}
		TreeSprite sprite;
		sprite.width = 2 * halfWidth + 1;
		sprite.height = bottom - top + 1;
		sprite.belowFoot = bottom - footY;
		sprite.pixels.assign(static_cast<size_t>(sprite.width) * sprite.height, -1);
		for (const auto &[x, y] : owned[k])
			sprite.pixels[static_cast<size_t>(y - top) * sprite.width + x - footX + halfWidth] = b.canvas[static_cast<size_t>(y) * b.canvasWidth + x];
		sprite.foot = { (trunks[k].sy / 16.0F + trunks[k].sx / 32.0F) / 2.0F, (trunks[k].sy / 16.0F - trunks[k].sx / 32.0F) / 2.0F };
#ifdef _DEBUG
		if (std::getenv("FP_LOG_TREES") != nullptr)
			LogInfo("fp: tree {} foot {:.2f},{:.2f} sprite {}x{} ({} under the foot)", treeCount, sprite.foot.x, sprite.foot.y, sprite.width, sprite.height, sprite.belowFoot);
#endif
#ifdef _DEBUG
		if (const char *dir = std::getenv("FP_DUMP_TREES"); dir != nullptr) {
			if (FILE *f = std::fopen(StrCat(dir, "/tree_", treeCount, ".ppm").c_str(), "wb"); f != nullptr) {
				std::fprintf(f, "P6\n%d %d\n255\n", sprite.width, sprite.height);
				for (const int16_t p : sprite.pixels) {
					const SDL_Color c = p > 0 ? logical_palette[p] : SDL_Color { 0, 160, 0, 255 };
					const uint8_t rgb[3] { c.r, c.g, c.b };
					std::fwrite(rgb, 1, 3, f);
				}
				std::fclose(f);
			}
		}
#endif
		TreeSprites.push_back(std::move(sprite));
		treeCount++;
	}
}

/** Thickness of the town's low stone walls (tiles), and their pieces along a tile (near and far away). */
constexpr float TownWallThickness = 0.42F;
constexpr int TownWallSegments = 8;
constexpr int TownWallFarSegments = 3;

/** Heights of the town's low stone wall tiles (index y * MAXDUNX + x; 0 elsewhere), for the walls to run on smoothly. */
std::vector<float> WallHeights;
std::vector<float> WallAcross; // per tile and axis (0: a wall along x, 1: along y): where across the tile it stands

/** @brief The plaza houses' stone wall texture (MeshTextures): the commonest on the plaza cottages' walls; -1 if none. */
int StoneTexture()
{
	static int texture = -2;
	if (texture != -2)
		return texture;
	std::unordered_map<int, int> counts;
	for (const LandmarkMesh &mesh : LandmarkMeshes) {
		if (mesh.name.rfind("cottage", 0) != 0)
			continue;
		for (size_t i = 0; i < mesh.classes.size() && i < mesh.textures.size(); i++)
			if (mesh.classes[i] == 0 && mesh.textures[i] >= 0)
				counts[mesh.textures[i]]++;
	}
	texture = -1;
	int best = 0;
	for (const auto &[index, count] : counts)
		if (count > best) {
			best = count;
			texture = index;
		}
	return texture;
}

/** How much darker the low walls' stone is than the cottages' (SurfaceShade steps): the art's walls are dark. */
constexpr int WallStoneDarkness = 3;
/** The low walls' stone in grey, against the texture's brightness (at SurfaceShade step WallStoneDarkness). */
constexpr float WallStoneGrey = 0.9F;

/**
 * @brief The low walls' stone: a texel of the cottages' (blue-grey) stone as a dark neutral grey of the same
 * brightness, dimmed by a SurfaceShade step: the palette's greys only, so no tint creeps in.
 */
uint8_t WallStoneColor(uint8_t texel, int darkness)
{
	static std::array<std::array<uint8_t, 256>, SurfaceShade.size()> table {};
	static uint32_t readyFor = 0;
	if (readyFor != PaletteVersion()) {
		readyFor = PaletteVersion();
		for (size_t level = 0; level < SurfaceShade.size(); level++) {
			for (int i = 0; i < 256; i++) {
				const SDL_Color c = logical_palette[i];
				const float grey = (0.3F * c.r + 0.59F * c.g + 0.11F * c.b) * SurfaceShade[level] * WallStoneGrey;
				float best = std::numeric_limits<float>::max();
				uint8_t found = 0;
				for (int j = 1; j < 256; j++) {
					if (j >= 112 && j <= 127)
						continue;
					const SDL_Color o = logical_palette[j];
					if (std::max({ o.r, o.g, o.b }) - std::min({ o.r, o.g, o.b }) > 2)
						continue; // (the near-greys are tinted green or purple)
					const float d = std::abs((o.r + o.g + o.b) / 3.0F - grey);
					if (d < best) {
						best = d;
						found = static_cast<uint8_t>(j);
					}
				}
				table[level][static_cast<size_t>(i)] = found;
			}
		}
	}
	return table[static_cast<size_t>(std::clamp(darkness, 0, static_cast<int>(SurfaceShade.size()) - 1))][texel];
}

/** The low walls' width at their foot and at their top (tiles): a dry stone wall narrows as it goes up. */
constexpr float WallFootHalf = 0.22F;
constexpr float WallTopHalf = 0.15F;

/** @brief Where across its tile a wall piece's art stands, for a wall along x (else along y): 0..1, kept clear of the edges. */
float WallArtAcross(const PieceTextures &t, bool alongX)
{
	constexpr int Cells = 8;
	const std::vector<float> grid = PropHullHeights(t, 0.7F, Cells);
	float acrossSum = 0;
	float weight = 0;
	for (int i = 0; i < Cells; i++)
		for (int j = 0; j < Cells; j++) {
			const float h = alongX ? grid[static_cast<size_t>(j) * Cells + i] : grid[static_cast<size_t>(i) * Cells + j];
			if (h <= 0)
				continue;
			acrossSum += h * (static_cast<float>(j) + 0.5F) / Cells;
			weight += h;
		}
	return weight > 0 ? std::clamp(acrossSum / weight, WallFootHalf, 1.0F - WallFootHalf) : 0.5F;
}

/**
 * @brief A low stone wall's tile, built as a straight dry stone wall along its run (towards the neighbours that are
 * wall too): narrowing towards its top, which runs on from tile to tile (halfway to each neighbour's height, lower
 * where the wall ends) with a gentle unevenness, in the plaza houses' stone (MeshTiled). Across the tile it stands
 * where its run's art has it (WallAcross), and at a corner it ends in the wall it meets.
 */
void BuildTownWallTile(Point tile, const std::array<bool, 4> &toward, int segments, std::vector<WorldTriangle> &out)
{
	const PieceTextures &t = GetPieceTextures(dPiece[tile.x][tile.y]);
	const std::vector<int16_t> pixels(t.upright.begin(), t.upright.end());
	const uint8_t fallback = PatchColor(static_cast<int16_t>(std::max<uint8_t>(CommonestColor(pixels), 1)));
	auto heightOf = [](Point p) { return InDungeonBounds(p) && !WallHeights.empty() ? WallHeights[static_cast<size_t>(p.y) * MAXDUNX + p.x] : 0.0F; };
	auto acrossOf = [&](Point p, int axis) {
		const size_t i = (static_cast<size_t>(p.y) * MAXDUNX + p.x) * 2 + static_cast<size_t>(axis);
		return i < WallAcross.size() && WallAcross[i] > 0 ? WallAcross[i] : WallArtAcross(t, axis == 0);
	};
	const float own = heightOf(tile);
	if (own <= 0)
		return;
	Building mesh {};
	for (int axis = 0; axis < 2; axis++) {
		const bool alongX = axis == 0;
		const bool before = toward[alongX ? 0 : 2]; // a wall neighbour at -x (-y)
		const bool after = toward[alongX ? 1 : 3];
		if (!before && !after)
			continue;
		const bool corner = toward[alongX ? 2 : 0] || toward[alongX ? 3 : 1];
		// Where across the tile it stands: where its whole run does (WallAcross), so the run's tiles line up.
		const float across = acrossOf(tile, axis);
		// Where the wall it meets at a corner stands, across that wall (along this one).
		const float meets = acrossOf(tile, 1 - axis);
		const Point previous = alongX ? Point { tile.x - 1, tile.y } : Point { tile.x, tile.y - 1 };
		const Point next = alongX ? Point { tile.x + 1, tile.y } : Point { tile.x, tile.y + 1 };
		const float h0 = before ? (own + heightOf(previous)) / 2.0F : own * 0.8F;
		const float h1 = after ? (own + heightOf(next)) / 2.0F : own * 0.8F;
		// Along the run: the whole tile, or (at a corner) from where the walls meet out to each wall neighbour.
		const float from = before || !corner ? 0.0F : meets - WallFootHalf;
		const float to = after || !corner ? 1.0F : meets + WallFootHalf;
		const float base = alongX ? static_cast<float>(tile.x) : static_cast<float>(tile.y);
		const float line = alongX ? static_cast<float>(tile.y) : static_cast<float>(tile.x);
		std::vector<float> heights(static_cast<size_t>(segments) + 1);
		for (int k = 0; k <= segments; k++) {
			const float a = from + (to - from) * static_cast<float>(k) / static_cast<float>(segments);
			const float h = a < 0.5F ? h0 + (own - h0) * a / 0.5F : own + (h1 - own) * (a - 0.5F) / 0.5F;
			// Uneven by the world position along the run, so it carries on over the tiles' edges.
			const float w = base + a;
			const float bump = 0.035F * std::sin(w * 4.1F + line * 1.7F) + 0.02F * std::sin(w * 11.3F + line * 2.3F + 1.3F);
			heights[static_cast<size_t>(k)] = std::max(0.15F, h + bump);
		}
		auto at = [&](float along, float acrossAt, float z) {
			return alongX ? V3 { static_cast<float>(tile.x) + along, static_cast<float>(tile.y) + acrossAt, z } : V3 { static_cast<float>(tile.x) + acrossAt, static_cast<float>(tile.y) + along, z };
		};
		const Vec2 middle = alongX ? Vec2 { static_cast<float>(tile.x) + 0.5F, static_cast<float>(tile.y) + across } : Vec2 { static_cast<float>(tile.x) + across, static_cast<float>(tile.y) + 0.5F };
		const V3 front = alongX ? V3 { 0, 1, 0 } : V3 { 1, 0, 0 };
		const V3 back { -front.x, -front.y, 0 };
		const V3 ahead = alongX ? V3 { 1, 0, 0 } : V3 { 0, 1, 0 };
		const V3 behind { -ahead.x, -ahead.y, 0 };
		for (int k = 0; k < segments; k++) {
			const float a = from + (to - from) * static_cast<float>(k) / static_cast<float>(segments);
			const float b = from + (to - from) * static_cast<float>(k + 1) / static_cast<float>(segments);
			const float ha = heights[static_cast<size_t>(k)];
			const float hb = heights[static_cast<size_t>(k) + 1];
			AddStructureFace(mesh, { at(a, across - WallTopHalf, ha), at(b, across - WallTopHalf, hb), at(b, across + WallTopHalf, hb), at(a, across + WallTopHalf, ha) }, { 0, 0, 1 }, middle, MeshTiled);
			AddStructureFace(mesh, { at(a, across + WallFootHalf, 0), at(b, across + WallFootHalf, 0), at(b, across + WallTopHalf, hb), at(a, across + WallTopHalf, ha) }, front, middle, MeshTiled);
			AddStructureFace(mesh, { at(a, across - WallFootHalf, 0), at(b, across - WallFootHalf, 0), at(b, across - WallTopHalf, hb), at(a, across - WallTopHalf, ha) }, back, middle, MeshTiled);
			if (k == 0 && (!before || corner))
				AddStructureFace(mesh, { at(a, across - WallFootHalf, 0), at(a, across + WallFootHalf, 0), at(a, across + WallTopHalf, ha), at(a, across - WallTopHalf, ha) }, behind, middle, MeshTiled);
			if (k + 1 == segments && (!after || corner))
				AddStructureFace(mesh, { at(b, across - WallFootHalf, 0), at(b, across + WallFootHalf, 0), at(b, across + WallTopHalf, hb), at(b, across - WallTopHalf, hb) }, ahead, middle, MeshTiled);
		}
	}
	const int texture = StoneTexture();
	for (WorldTriangle &tri : mesh.mesh) {
		tri.fallback = fallback;
		tri.texture = static_cast<int16_t>(texture);
		tri.cull = false;
		tri.artX = static_cast<int16_t>(tile.x);
		tri.artY = static_cast<int16_t>(tile.y);
		out.push_back(tri);
	}
}

/** Hull props are drawn in blocks of this many tiles a side (culled as one). */
constexpr int PropBlock = 6;

// ---------------------------------------------------------------------------------------------------------------
// Rocks and gravestones rebuilt from their art. The iso camera is orthographic, looking down at 30 degrees, with
// the same scale across and down the screen: a pixel of art is a ray through the world. A boulder is its art's
// outline blown up into a rounded shape around the rays (deepest where the outline is widest), so that every point
// of it lies on the ray of the pixel it shows; a gravestone is an upright slab cut to its outline.

/** World units per screen pixel across the screen and down it, and the direction towards the iso camera. */
constexpr V3 IsoAcross { 1.0F / 64.0F, -1.0F / 64.0F, 0.0F };
constexpr float IsoDownScale = 1.0F / (512.0F + HeightPixels * HeightPixels);
constexpr V3 IsoDown { 16.0F * IsoDownScale, 16.0F * IsoDownScale, -HeightPixels * IsoDownScale };
constexpr float IsoPixelWorld = 1.41421356F / 64.0F; // the length of IsoAcross (and of IsoDown)
const V3 IsoToward = [] {
	const V3 c { 32.0F * HeightPixels, 32.0F * HeightPixels, 1024.0F };
	const float length = std::sqrt(c.x * c.x + c.y * c.y + c.z * c.z);
	return V3 { c.x / length, c.y / length, c.z / length };
}();

/** @brief The point on the ground the iso camera shows at screen point (sx, sy). */
V3 IsoGround(float sx, float sy)
{
	return { (sy / 16.0F + sx / 32.0F) / 2.0F, (sy / 16.0F - sx / 32.0F) / 2.0F, 0.0F };
}

/** @brief The pixel of a prop's own art (its canvas) the iso camera saw at a point, or a neighbour's; NoPixel if none. */
int16_t CanvasArtAt(const Building &b, float wx, float wy, float wz)
{
	const int px = static_cast<int>(std::floor((wx - wy) * 32.0F)) - b.originX;
	const int py = static_cast<int>(std::floor((wx + wy) * 16.0F - wz * HeightPixels)) - b.originY;
	for (int r = 0; r <= 1; r++)
		for (int dy = -r; dy <= r; dy++)
			for (int dx = -r; dx <= r; dx++) {
				const int x = px + dx;
				const int y = py + dy;
				if (x < 0 || y < 0 || x >= b.canvasWidth || y >= b.canvasHeight)
					continue;
				const int16_t p = b.canvas[static_cast<size_t>(y) * b.canvasWidth + x];
				if (p > 0)
					return p;
			}
	return NoPixel;
}

/** @brief The groups of 8-connected opaque pixels of a canvas, those of `minSize` pixels or more. */
std::vector<std::vector<std::pair<int, int>>> CanvasComponents(const Building &b, size_t minSize)
{
	std::vector<std::vector<std::pair<int, int>>> components;
	const int w = b.canvasWidth;
	const int h = b.canvasHeight;
	std::vector<uint8_t> seen(b.canvas.size(), 0);
	for (int y0 = 0; y0 < h; y0++) {
		for (int x0 = 0; x0 < w; x0++) {
			if (b.canvas[static_cast<size_t>(y0) * w + x0] <= 0 || seen[static_cast<size_t>(y0) * w + x0] != 0)
				continue;
			std::vector<std::pair<int, int>> pixels;
			std::vector<std::pair<int, int>> stack { { x0, y0 } };
			seen[static_cast<size_t>(y0) * w + x0] = 1;
			while (!stack.empty()) {
				const auto [x, y] = stack.back();
				stack.pop_back();
				pixels.emplace_back(x, y);
				for (int dy = -1; dy <= 1; dy++)
					for (int dx = -1; dx <= 1; dx++) {
						const int nx = x + dx;
						const int ny = y + dy;
						if (nx < 0 || ny < 0 || nx >= w || ny >= h || seen[static_cast<size_t>(ny) * w + nx] != 0 || b.canvas[static_cast<size_t>(ny) * w + nx] <= 0)
							continue;
						seen[static_cast<size_t>(ny) * w + nx] = 1;
						stack.emplace_back(nx, ny);
					}
			}
			if (pixels.size() >= minSize)
				components.push_back(std::move(pixels));
		}
	}
	return components;
}

/** @brief A triangle of a prop rebuilt from its art: textured with the art the iso camera saw at each point. */
void AddArtTriangle(std::vector<WorldTriangle> &out, V3 a, V3 b, V3 c, uint8_t fallback)
{
	const V3 e1 { b.x - a.x, b.y - a.y, b.z - a.z };
	const V3 e2 { c.x - a.x, c.y - a.y, c.z - a.z };
	V3 normal { e1.y * e2.z - e1.z * e2.y, e1.z * e2.x - e1.x * e2.z, e1.x * e2.y - e1.y * e2.x };
	const float length = std::sqrt(normal.x * normal.x + normal.y * normal.y + normal.z * normal.z);
	if (length < 1e-9F)
		return;
	normal = { normal.x / length, normal.y / length, normal.z / length };
	WorldTriangle tri {};
	tri.p = { a, b, c };
	tri.mapping = MeshProjective;
	tri.normal = normal;
	tri.cull = false;
	tri.mirrored = false;
	tri.slopeScale = 1.0F;
	tri.fallback = fallback;
	// Turned away from the iso camera (the side it did not see), or facing down: a little darker.
	const float facing = normal.x * IsoToward.x + normal.y * IsoToward.y + normal.z * IsoToward.z;
	tri.darkness = std::abs(facing) < 0.35F ? 1 : 0;
	out.push_back(tri);
}

/** @brief A triangle of a closed prop shape, turned to face out from `inside`, drawn from the front only. */
void AddSolidTriangle(std::vector<WorldTriangle> &out, V3 a, V3 b, V3 c, V3 inside, uint8_t fallback)
{
	const V3 e1 { b.x - a.x, b.y - a.y, b.z - a.z };
	const V3 e2 { c.x - a.x, c.y - a.y, c.z - a.z };
	const V3 n { e1.y * e2.z - e1.z * e2.y, e1.z * e2.x - e1.x * e2.z, e1.x * e2.y - e1.y * e2.x };
	const V3 m { (a.x + b.x + c.x) / 3.0F - inside.x, (a.y + b.y + c.y) / 3.0F - inside.y, (a.z + b.z + c.z) / 3.0F - inside.z };
	const size_t before = out.size();
	if (n.x * m.x + n.y * m.y + n.z * m.z < 0)
		AddArtTriangle(out, a, c, b, fallback);
	else
		AddArtTriangle(out, a, b, c, fallback);
	if (out.size() > before) {
		WorldTriangle &tri = out.back();
		tri.cull = true;
		if (tri.normal.z < -0.3F)
			tri.darkness = 2; // its underside
	}
}

/** @brief The unit icosahedron, split `level` times (each triangle in four), its points on the sphere. */
const std::vector<std::array<V3, 3>> &UnitSphere(int level)
{
	static std::array<std::vector<std::array<V3, 3>>, 2> spheres;
	std::vector<std::array<V3, 3>> &sphere = spheres[static_cast<size_t>(std::clamp(level, 0, 1))];
	if (!sphere.empty())
		return sphere;
	const float t = (1.0F + std::sqrt(5.0F)) / 2.0F;
	std::array<V3, 12> v { { { -1, t, 0 }, { 1, t, 0 }, { -1, -t, 0 }, { 1, -t, 0 }, { 0, -1, t }, { 0, 1, t }, { 0, -1, -t }, { 0, 1, -t }, { t, 0, -1 }, { t, 0, 1 }, { -t, 0, -1 }, { -t, 0, 1 } } };
	auto unit = [](V3 p) {
		const float l = std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z);
		return V3 { p.x / l, p.y / l, p.z / l };
	};
	for (V3 &p : v)
		p = unit(p);
	constexpr std::array<std::array<int, 3>, 20> Faces { { { 0, 11, 5 }, { 0, 5, 1 }, { 0, 1, 7 }, { 0, 7, 10 }, { 0, 10, 11 }, { 1, 5, 9 }, { 5, 11, 4 }, { 11, 10, 2 }, { 10, 7, 6 }, { 7, 1, 8 },
		{ 3, 9, 4 }, { 3, 4, 2 }, { 3, 2, 6 }, { 3, 6, 8 }, { 3, 8, 9 }, { 4, 9, 5 }, { 2, 4, 11 }, { 6, 2, 10 }, { 8, 6, 7 }, { 9, 8, 1 } } };
	for (const auto &f : Faces) {
		const V3 a = v[f[0]], b = v[f[1]], c = v[f[2]];
		if (level <= 0) {
			sphere.push_back({ a, b, c });
			continue;
		}
		auto mid = [&](V3 p, V3 q) { return unit({ (p.x + q.x) / 2, (p.y + q.y) / 2, (p.z + q.z) / 2 }); };
		const V3 ab = mid(a, b), bc = mid(b, c), ca = mid(c, a);
		sphere.push_back({ a, ab, ca });
		sphere.push_back({ ab, b, bc });
		sphere.push_back({ ca, bc, c });
		sphere.push_back({ ab, bc, ca });
	}
	return sphere;
}

/**
 * @brief A tunnel carved through stones (the caves' entrance): in the frame of a point `origin` on the ground and the
 * way the tunnel runs out, `along`, whatever of a stone lies past `back` along it and inside its arch (half an
 * ellipse `half` across each way at the floor, `top` high, its outline wobbling by up to `ragged` of it) is pushed out
 * onto the arch, or back onto the tunnel's back if that is nearer, and drawn dark (`color`): darker the deeper.
 */
struct StoneCarve {
	Vec2 origin;
	Vec2 along;
	float back;
	float half;
	float top;
	float ragged;
	uint8_t color;
};

/**
 * @brief A lumpy stone: a squashed sphere (`radius` across, `height` up, turned by `turn`) with its surface pushed in
 * and out by a few waves, so no two are alike; what would be under the ground is flattened onto it. With `carve`, a
 * tunnel cut through it (StoneCarve).
 */
void AddStone(std::vector<WorldTriangle> &out, V3 middle, float radius, float height, float turn, uint32_t seed, int level, uint8_t fallback, const StoneCarve *carve = nullptr)
{
	auto random = [&](int k) { return static_cast<float>((seed * 2654435761U + static_cast<uint32_t>(k) * 40503U) >> 8 & 0xFFFF) / 65535.0F; };
	std::array<V3, 4> waves;
	std::array<float, 4> phase;
	for (int k = 0; k < 4; k++) {
		const float a = random(k * 3) * 6.2831853F;
		const float z = random(k * 3 + 1) * 2.0F - 1.0F;
		const float r = std::sqrt(1.0F - z * z);
		waves[k] = { r * std::cos(a), r * std::sin(a), z };
		phase[k] = random(k * 3 + 2) * 6.2831853F;
	}
	const float stretch = 0.85F + 0.3F * random(20);
	const float c = std::cos(turn);
	const float s = std::sin(turn);
	auto place = [&](V3 p) {
		float lump = 1.0F;
		for (int k = 0; k < 4; k++)
			lump += (k < 2 ? 0.13F : 0.06F) * std::sin((p.x * waves[k].x + p.y * waves[k].y + p.z * waves[k].z) * (k < 2 ? 2.6F : 5.3F) + phase[k]);
		const float x = p.x * lump * radius * stretch;
		const float y = p.y * lump * radius / stretch;
		const float z = std::max(p.z * lump * height, -0.35F * height);
		return V3 { middle.x + x * c - y * s, middle.y + x * s + y * c, std::max(middle.z + z, 0.0F) };
	};
	// Whatever of the stone is inside the tunnel, pushed out onto its nearest surface; whether it was.
	auto cut = [&](V3 q, bool &carved) {
		carved = false;
		if (carve == nullptr)
			return q;
		const float dx = q.x - carve->origin.x;
		const float dy = q.y - carve->origin.y;
		const float u = dx * carve->along.x + dy * carve->along.y;
		const float v = -dx * carve->along.y + dy * carve->along.x;
		if (u <= carve->back)
			return q;
		// The arch's outline round its foot's middle, wobbling with the angle and along the tunnel.
		const float angle = std::atan2(q.z / carve->top, v / carve->half);
		const float wobble = 1.0F + carve->ragged * (0.6F * std::sin(angle * 5.0F + u * 3.1F) + 0.4F * std::sin(angle * 11.0F - u * 5.3F));
		const float nv = v / (carve->half * wobble);
		const float nz = q.z / (carve->top * wobble);
		const float r = std::sqrt(nv * nv + nz * nz);
		if (r >= 1.0F)
			return q;
		carved = true;
		const float toBack = u - carve->back;
		const float toArch = (1.0F - r) * std::min(carve->half, carve->top);
		if (toBack <= toArch) {
			q.x -= carve->along.x * toBack;
			q.y -= carve->along.y * toBack;
		} else if (r < 1e-4F) {
			q.z = carve->top * wobble;
		} else {
			const float scale = 1.0F / r;
			const float nvOut = v * scale - v;
			q.x += -carve->along.y * nvOut;
			q.y += carve->along.x * nvOut;
			q.z *= scale;
		}
		return q;
	};
	for (const auto &tri : UnitSphere(level)) {
		std::array<bool, 3> carved {};
		const V3 a = cut(place(tri[0]), carved[0]);
		const V3 b = cut(place(tri[1]), carved[1]);
		const V3 d = cut(place(tri[2]), carved[2]);
		const size_t before = out.size();
		AddSolidTriangle(out, a, b, d, { middle.x, middle.y, std::max(middle.z, height * 0.3F) }, fallback);
		if (out.size() > before && carved[0] && carved[1] && carved[2]) {
			WorldTriangle &inside = out.back();
			if (inside.normal.x * carve->along.x + inside.normal.y * carve->along.y > 0.5F) {
				// A face the stone turned out of the tunnel, now stretched across its arch (or flat on its back, which
				// the tunnel's own back closes): gone.
				out.pop_back();
				continue;
			}
			// The tunnel's inside: dark rock, black deep in.
			const float mx = (a.x + b.x + d.x) / 3.0F - carve->origin.x;
			const float my = (a.y + b.y + d.y) / 3.0F - carve->origin.y;
			const float depth = mx * carve->along.x + my * carve->along.y - carve->back;
			inside.mapping = MeshSolid;
			inside.fallback = depth < 0.35F ? NearestColor(6.0F, 6.0F, 9.0F) : carve->color;
			inside.darkness = static_cast<uint8_t>(SurfaceShade.size() - (depth < 0.7F ? 1 : 2));
		}
	}
}

/** @brief A component of a canvas as a mask (a pixel of empty border round it), with its small specks closed. */
struct ArtMask {
	int x0 = 0; // the canvas pixel at mask (1, 1)
	int y0 = 0;
	int w = 0;
	int h = 0;
	std::vector<uint8_t> mask;

	ArtMask(const std::vector<std::pair<int, int>> &pixels, int close)
	{
		int x1 = std::numeric_limits<int>::lowest();
		int y1 = x1;
		x0 = y0 = std::numeric_limits<int>::max();
		for (const auto &[x, y] : pixels) {
			x0 = std::min(x0, x);
			y0 = std::min(y0, y);
			x1 = std::max(x1, x);
			y1 = std::max(y1, y);
		}
		w = x1 - x0 + 3;
		h = y1 - y0 + 3;
		mask.assign(static_cast<size_t>(w) * h, 0);
		for (const auto &[x, y] : pixels)
			mask[At(x, y)] = 1;
		auto grow = [&](uint8_t value) {
			std::vector<uint8_t> to = mask;
			for (int y = 0; y < h; y++)
				for (int x = 0; x < w; x++) {
					if (mask[static_cast<size_t>(y) * w + x] == value)
						continue;
					for (int dy = -close; dy <= close; dy++)
						for (int dx = -close; dx <= close; dx++) {
							const int nx = x + dx;
							const int ny = y + dy;
							if (dx * dx + dy * dy <= close * close && nx >= 0 && ny >= 0 && nx < w && ny < h && mask[static_cast<size_t>(ny) * w + nx] == value) {
								to[static_cast<size_t>(y) * w + x] = value;
								dy = close + 1;
								break;
							}
						}
				}
			mask = std::move(to);
		};
		if (close > 0) {
			grow(1);
			grow(0);
		}
		for (const auto &[x, y] : pixels)
			mask[At(x, y)] = 1;
		for (int y = 0; y < h; y++)
			for (int x = 0; x < w; x++)
				if (x == 0 || y == 0 || x == w - 1 || y == h - 1)
					mask[static_cast<size_t>(y) * w + x] = 0;
	}

	[[nodiscard]] size_t At(int x, int y) const { return static_cast<size_t>(y - y0 + 1) * w + (x - x0 + 1); }

	/** @brief Each pixel's distance to the outside of the mask (chamfer). */
	[[nodiscard]] std::vector<float> Distance() const
	{
		std::vector<float> d(mask.size(), 0.0F);
		for (size_t i = 0; i < mask.size(); i++)
			if (mask[i] != 0)
				d[i] = 1e6F;
		constexpr float Diagonal = 1.41421356F;
		for (int y = 1; y < h; y++)
			for (int x = 1; x < w - 1; x++) {
				float &v = d[static_cast<size_t>(y) * w + x];
				if (v != 0)
					v = std::min({ v, d[static_cast<size_t>(y - 1) * w + x] + 1, d[static_cast<size_t>(y) * w + x - 1] + 1, d[static_cast<size_t>(y - 1) * w + x - 1] + Diagonal, d[static_cast<size_t>(y - 1) * w + x + 1] + Diagonal });
			}
		for (int y = h - 2; y >= 0; y--)
			for (int x = w - 2; x >= 1; x--) {
				float &v = d[static_cast<size_t>(y) * w + x];
				if (v != 0)
					v = std::min({ v, d[static_cast<size_t>(y + 1) * w + x] + 1, d[static_cast<size_t>(y) * w + x + 1] + 1, d[static_cast<size_t>(y + 1) * w + x + 1] + Diagonal, d[static_cast<size_t>(y + 1) * w + x - 1] + Diagonal });
			}
		return d;
	}
};

/** Screen pixels per tile across the iso view, and down it (for a length along the ground towards the camera). */
constexpr float IsoPixelsPerTile = 45.254834F;
constexpr float IsoPixelsPerTileDeep = 22.627417F;

/**
 * @brief A heap of boulders from its art, as a few big stones: the original draws a boulder about every
 * BoulderArtArea pixels of its heap, and there is a stone for every two of them (one for up to three, two for four or
 * five, ...), each as big as its share of the heap. Where they lie: the biggest circles inside the outline (each where
 * it is widest), then a few rounds of k-means over the heap's pixels, so between them they cover it; each stone as big
 * as a disc of its pixels' area (StoneFill), a lumpy low-poly stone. The iso view has the same scale across and down,
 * so a disc of the art is a stone of that size, its middle on the ray of the disc's. Where a stone is above the heap's
 * foot it is half further back, half up on the stones in front (never higher than the biggest). `stones` counts.
 */
constexpr float BoulderArtArea = 640.0F; // art pixels a boulder of the original covers in its heap (measured on two heaps)
constexpr float StoneFill = 1.1F;        // a stone's radius over that of a disc of its pixels' area: they overlap a little
constexpr int StoneRounds = 6;           // k-means rounds

void PackStones(const Building &b, const std::vector<std::pair<int, int>> &pixels, uint8_t fallback, std::vector<WorldTriangle> &near, std::vector<WorldTriangle> &far, int &stones)
{
	ArtMask m(pixels, 2);
	// The heap's foot: the lowest pixel of each column, evened out over the columns around it.
	std::vector<int> bottom(static_cast<size_t>(m.w), -1);
	std::vector<std::pair<int, int>> inside;
	for (int y = 0; y < m.h; y++)
		for (int x = 0; x < m.w; x++)
			if (m.mask[static_cast<size_t>(y) * m.w + x] != 0) {
				bottom[static_cast<size_t>(x)] = y;
				inside.emplace_back(x, y);
			}
	if (inside.empty())
		return;
	std::vector<float> foot(static_cast<size_t>(m.w), static_cast<float>(m.h - 2));
	for (int c = 0; c < m.w; c++) {
		std::vector<int> around;
		for (int k = std::max(0, c - 4); k <= std::min(m.w - 1, c + 4); k++)
			if (bottom[static_cast<size_t>(k)] >= 0)
				around.push_back(bottom[static_cast<size_t>(k)]);
		if (!around.empty()) {
			std::nth_element(around.begin(), around.begin() + static_cast<ptrdiff_t>(around.size() / 2), around.end());
			foot[static_cast<size_t>(c)] = static_cast<float>(around[around.size() / 2]) + 1.0F;
		}
	}
	const int boulders = std::max(1, static_cast<int>(std::lround(static_cast<float>(inside.size()) / BoulderArtArea)));
	const int count = std::max(1, boulders / 2);
	// Seeds: the biggest circles inside the outline, each where it is widest.
	std::vector<std::array<float, 2>> centres;
	{
		ArtMask left = m;
		for (int k = 0; k < count; k++) {
			const std::vector<float> distance = left.Distance();
			const auto widest = std::max_element(distance.begin(), distance.end());
			if (*widest < 1.0F)
				break;
			const int mx = static_cast<int>((widest - distance.begin()) % m.w);
			const int my = static_cast<int>((widest - distance.begin()) / m.w);
			centres.push_back({ static_cast<float>(mx), static_cast<float>(my) });
			const float r = *widest + 0.5F;
			for (int y = 0; y < m.h; y++)
				for (int x = 0; x < m.w; x++)
					if (static_cast<float>((x - mx) * (x - mx) + (y - my) * (y - my)) <= r * r)
						left.mask[static_cast<size_t>(y) * m.w + x] = 0;
		}
	}
	// k-means over the heap's pixels: each stone takes the pixels nearest it.
	std::vector<int> owner(inside.size(), 0);
	std::vector<int> share(centres.size(), 0);
	for (int round = 0; round < StoneRounds; round++) {
		std::vector<std::array<double, 2>> sum(centres.size(), { 0.0, 0.0 });
		std::fill(share.begin(), share.end(), 0);
		for (size_t i = 0; i < inside.size(); i++) {
			const auto [x, y] = inside[i];
			float best = std::numeric_limits<float>::max();
			for (size_t c = 0; c < centres.size(); c++) {
				const float dx = static_cast<float>(x) - centres[c][0];
				const float dy = static_cast<float>(y) - centres[c][1];
				const float d = dx * dx + dy * dy;
				if (d < best) {
					best = d;
					owner[i] = static_cast<int>(c);
				}
			}
			sum[owner[i]][0] += x;
			sum[owner[i]][1] += y;
			share[owner[i]]++;
		}
		for (size_t c = 0; c < centres.size(); c++)
			if (share[c] > 0)
				centres[c] = { static_cast<float>(sum[c][0] / share[c]), static_cast<float>(sum[c][1] / share[c]) };
	}
	// The stones placed so far (middle, radius, height): what a stone above the heap's foot may rest on.
	struct Placed {
		V3 middle;
		float radius;
		float height;
	};
	std::vector<Placed> placed;
	auto supportAt = [&](float x, float y) {
		float top = 0;
		for (const Placed &p : placed) {
			const float reach = 0.85F * p.radius; // its lumps aside, how far out its top surely reaches
			const float d2 = (x - p.middle.x) * (x - p.middle.x) + (y - p.middle.y) * (y - p.middle.y);
			if (d2 < reach * reach)
				top = std::max(top, p.middle.z + p.height * std::sqrt(1.0F - d2 / (reach * reach)));
		}
		return top;
	};
	// Biggest first, so a smaller one can rest on it.
	std::vector<size_t> order(centres.size());
	for (size_t c = 0; c < order.size(); c++)
		order[c] = c;
	std::sort(order.begin(), order.end(), [&](size_t a, size_t c) { return share[a] > share[c]; });
	float tallest = 0; // the biggest stone's height: none rests higher than on top of it
	for (const size_t c : order) {
		if (share[c] == 0)
			continue;
		const float mx = centres[c][0];
		const float my = centres[c][1];
		const float r = std::sqrt(static_cast<float>(share[c]) / 3.14159265F) * StoneFill;
		// The stone: its middle on the ray of the disc's.
		const float radius = r / IsoPixelsPerTile;
		const float height = radius * 0.72F;
		const float sx = mx + static_cast<float>(m.x0 - 1 + b.originX) + 0.5F;
		const float sy = my + static_cast<float>(m.y0 - 1 + b.originY) + 0.5F;
		const int column = std::clamp(static_cast<int>(std::lround(mx)), 0, m.w - 1);
		const float gap = std::max(0.0F, foot[static_cast<size_t>(column)] - my - r);
		const float lift = std::min(0.5F * gap / HeightPixels, tallest * 0.8F);
		tallest = std::max(tallest, 2.0F * height);
		// On the ray of its disc's middle, its flattened underside (AddStone: 0.35 of its height below its middle) as
		// high as it would be up on the stones in front, if one of them is under it there; else lower, the highest it
		// can be with a stone under it, and at worst on the ground (further back along the ray).
		auto at = [&](float bottom) {
			const float z = bottom + height * 0.35F;
			const float sum = (sy + z * HeightPixels) / 16.0F; // x + y
			const float difference = sx / 32.0F;            // x - y
			return V3 { (sum + difference) / 2.0F, (sum - difference) / 2.0F, z };
		};
		constexpr int Steps = 8;
		V3 middle = at(0.0F);
		for (int k = Steps; k > 0; k--) {
			const float bottom = lift * static_cast<float>(k) / Steps;
			const V3 candidate = at(bottom);
			if (supportAt(candidate.x, candidate.y) >= bottom - 0.03F) {
				middle = candidate;
				break;
			}
		}
		placed.push_back({ middle, radius, height });
		const uint32_t seed = static_cast<uint32_t>(std::lround(middle.x * 97.0F)) * 73856093U ^ static_cast<uint32_t>(std::lround(middle.y * 97.0F)) * 19349663U;
		const float turn = static_cast<float>(seed % 628U) / 100.0F;
		AddStone(near, middle, radius, height, turn, seed, 1, fallback);
		AddStone(far, middle, radius, height, turn, seed, 0, fallback);
		stones++;
	}
}

/**
 * @brief A gravestone from its art, made as what its outline is: a headstone (round-topped where its top corners are
 * cut), a cross (arms much wider than the upright above them), a post (narrow) or a slab fallen flat (wide and low).
 * Its body is the art's wider rows: what the ground's removal leaves of its shaded foot is a thin stem, so it stands
 * on the ground under the art's foot as wide as its body. It faces the iso camera.
 */
void ShapeGrave(const Building &b, const std::vector<std::pair<int, int>> &pixels, uint8_t fallback, std::vector<WorldTriangle> &out)
{
	int y0 = std::numeric_limits<int>::max();
	int y1 = std::numeric_limits<int>::lowest();
	for (const auto &[x, y] : pixels) {
		y0 = std::min(y0, y);
		y1 = std::max(y1, y);
	}
	const int rows = y1 - y0 + 1;
	std::vector<int> left(static_cast<size_t>(rows), std::numeric_limits<int>::max());
	std::vector<int> right(static_cast<size_t>(rows), std::numeric_limits<int>::lowest());
	for (const auto &[x, y] : pixels) {
		left[static_cast<size_t>(y - y0)] = std::min(left[static_cast<size_t>(y - y0)], x);
		right[static_cast<size_t>(y - y0)] = std::max(right[static_cast<size_t>(y - y0)], x);
	}
	auto width = [&](int row) { return right[static_cast<size_t>(row)] >= left[static_cast<size_t>(row)] ? right[static_cast<size_t>(row)] - left[static_cast<size_t>(row)] + 1 : 0; };
	int widest = 0;
	int widestRow = 0;
	for (int row = 0; row < rows; row++)
		if (width(row) > widest) {
			widest = width(row);
			widestRow = row;
		}
	// The body: the rows at least half as wide as the widest; its width, and its middle across.
	std::vector<int> body;
	float middleSum = 0;
	for (int row = 0; row < rows; row++)
		if (2 * width(row) >= widest) {
			body.push_back(width(row));
			middleSum += static_cast<float>(left[static_cast<size_t>(row)] + right[static_cast<size_t>(row)] + 1) / 2.0F;
		}
	const float middlePx = middleSum / static_cast<float>(body.size());
	std::sort(body.begin(), body.end());
	const float bodyWidth = static_cast<float>(body[body.size() * 85 / 100]);
	const float tall = static_cast<float>(rows);
	const float footSx = middlePx + static_cast<float>(b.originX);
	const float footSy = static_cast<float>(y1 + b.originY) + 1.0F;
	const V3 foot = IsoGround(footSx, footSy);
	constexpr V3 Along { 0.70710678F, -0.70710678F, 0.0F };
	constexpr V3 Facing { 0.70710678F, 0.70710678F, 0.0F };
	auto at = [&](float along, float facing, float z) {
		return V3 { foot.x + along * Along.x + facing * Facing.x, foot.y + along * Along.y + facing * Facing.y, z };
	};
	auto zOf = [&](int row) { return std::max(0.0F, (footSy - static_cast<float>(y0 + row + b.originY)) / HeightPixels); };
	// A box from a0 to a1 along, f0 to f1 towards the camera, z0 to z1 up.
	auto box = [&](float a0, float a1, float f0, float f1, float z0, float z1) {
		const V3 inside = at((a0 + a1) / 2.0F, (f0 + f1) / 2.0F, (z0 + z1) / 2.0F);
		const std::array<V3, 8> p { at(a0, f0, z0), at(a1, f0, z0), at(a1, f1, z0), at(a0, f1, z0), at(a0, f0, z1), at(a1, f0, z1), at(a1, f1, z1), at(a0, f1, z1) };
		constexpr std::array<std::array<int, 4>, 6> Sides { { { 0, 1, 2, 3 }, { 4, 5, 6, 7 }, { 0, 1, 5, 4 }, { 1, 2, 6, 5 }, { 2, 3, 7, 6 }, { 3, 0, 4, 7 } } };
		for (const auto &q : Sides) {
			AddSolidTriangle(out, p[q[0]], p[q[1]], p[q[2]], inside, fallback);
			AddSolidTriangle(out, p[q[0]], p[q[2]], p[q[3]], inside, fallback);
		}
	};
	const float halfWidth = bodyWidth / 2.0F / IsoPixelsPerTile;
	const float top = zOf(0);
	constexpr float Thick = 0.09F;
	// A cross: a band of rows nearly as wide as the widest, with a narrow upright above it and narrower rows below.
	int band0 = widestRow;
	int band1 = widestRow;
	while (band0 > 0 && 20 * width(band0 - 1) >= 17 * widest)
		band0--;
	while (band1 + 1 < rows && 20 * width(band1 + 1) >= 17 * widest)
		band1++;
	float above = 0;
	for (int row = 0; row < band0; row++)
		above += static_cast<float>(width(row));
	above = band0 > 0 ? above / static_cast<float>(band0) : static_cast<float>(widest);
	float below = 0;
	int belowRows = 0;
	for (int row = band1 + 1; row < std::min(rows, band1 + 5); row++, belowRows++)
		below += static_cast<float>(width(row));
	below = belowRows > 0 ? below / static_cast<float>(belowRows) : static_cast<float>(widest);
	const bool cross = band0 >= 2 && above <= 0.55F * static_cast<float>(widest) && below <= 0.75F * static_cast<float>(widest) && band1 - band0 + 1 <= rows * 45 / 100;
	if (static_cast<float>(widest) >= 1.6F * tall) {
		// Fallen flat: as long towards the camera as the art is tall (less its own thickness).
		const float length = std::max(0.1F, (tall - 3.0F) / IsoPixelsPerTileDeep);
		const float half = static_cast<float>(widest) / 2.0F / IsoPixelsPerTile;
		box(-half, half, -length, 0.0F, 0.0F, 0.06F);
	} else if (cross) {
		// Its upright as wide as the art above the arms, its arms as wide as the art there, both about its middle.
		const float stem = std::max(above, 2.0F) / 2.0F / IsoPixelsPerTile;
		const float arm = static_cast<float>(widest) / 2.0F / IsoPixelsPerTile;
		box(-stem, stem, -Thick / 2.0F, Thick / 2.0F, 0.0F, top);
		box(-arm, arm, -Thick / 2.0F, Thick / 2.0F, zOf(band1 + 1), zOf(band0));
	} else if (bodyWidth <= 0.42F * tall) {
		// A post: as deep as it is wide.
		box(-halfWidth, halfWidth, -halfWidth, halfWidth, 0.0F, top);
	} else {
		// A headstone, round-topped where the corners of its top are cut.
		const int topWidth = std::max(width(0), width(std::min(1, rows - 1)));
		const bool round = static_cast<float>(topWidth) < 0.75F * bodyWidth;
		const float arc = round ? std::min(halfWidth, top * 0.5F) : 0.0F;
		std::vector<std::pair<float, float>> outline { { -halfWidth, 0.0F }, { halfWidth, 0.0F }, { halfWidth, top - arc } };
		constexpr int ArcSteps = 6;
		for (int k = 1; k < ArcSteps; k++) {
			const float a = 3.14159265F * static_cast<float>(k) / static_cast<float>(ArcSteps);
			outline.emplace_back(halfWidth * std::cos(a), top - arc + arc * std::sin(a));
		}
		outline.emplace_back(-halfWidth, top - arc);
		const V3 inside = at(0.0F, 0.0F, top * 0.45F);
		const size_t n = outline.size();
		for (size_t k = 1; k + 1 < n; k++) {
			for (const float f : { -Thick / 2.0F, Thick / 2.0F })
				AddSolidTriangle(out, at(outline[0].first, f, outline[0].second), at(outline[k].first, f, outline[k].second), at(outline[k + 1].first, f, outline[k + 1].second), inside, fallback);
		}
		for (size_t k = 0; k < n; k++) {
			const auto &[a0, z0] = outline[k];
			const auto &[a1, z1] = outline[(k + 1) % n];
			const V3 p0 = at(a0, -Thick / 2.0F, z0), p1 = at(a1, -Thick / 2.0F, z1);
			const V3 q0 = at(a0, Thick / 2.0F, z0), q1 = at(a1, Thick / 2.0F, z1);
			AddSolidTriangle(out, p0, p1, q1, inside, fallback);
			AddSolidTriangle(out, p0, q1, q0, inside, fallback);
		}
	}
}

/** @brief The commonest colour of a group of canvas pixels, for where the art shows nothing. */
uint8_t ComponentColor(const Building &b, const std::vector<std::pair<int, int>> &pixels)
{
	std::vector<int16_t> colors;
	colors.reserve(pixels.size());
	for (const auto &[x, y] : pixels)
		colors.push_back(b.canvas[static_cast<size_t>(y) * b.canvasWidth + x]);
	return PatchColor(static_cast<int16_t>(std::max<uint8_t>(CommonestColor(colors), 1)));
}

/** @brief The bounds of a prop from its mesh. */
void FitPropBounds(TownProp &prop)
{
	Building &b = prop.art;
	b.x0 = b.y0 = std::numeric_limits<float>::max();
	b.x1 = b.y1 = std::numeric_limits<float>::lowest();
	b.lowestZ = 0;
	b.highestZ = 0;
	for (const WorldTriangle &tri : b.mesh)
		for (const V3 &p : tri.p) {
			b.x0 = std::min(b.x0, p.x);
			b.y0 = std::min(b.y0, p.y);
			b.x1 = std::max(b.x1, p.x);
			b.y1 = std::max(b.y1, p.y);
			b.highestZ = std::max(b.highestZ, p.z);
		}
	prop.middle = { (b.x0 + b.x1) / 2.0F, (b.y0 + b.y1) / 2.0F };
	prop.radius = std::hypot(b.x1 - b.x0, b.y1 - b.y0) / 2.0F;
}

/**
 * @brief The art of a group of prop tiles without the ground it paints in their floor diamonds. The foot of a
 * boulder or a gravestone is drawn there with the ground, and the ground (with its shadows) is grey, brown and green
 * where the stones are blue: in the diamonds the stone pixels of the whole art are kept (where they are not a speck
 * on their own), the rest goes. Painted lamplight goes too.
 */
void BuildPropCanvas(Building &b, const std::vector<Point> &tiles)
{
	BuildUprightCanvas(b, tiles);
	Building raw {};
	BuildUprightCanvas(raw, tiles, true);
	if (raw.canvasWidth != b.canvasWidth || raw.canvasHeight != b.canvasHeight)
		return;
	const int w = b.canvasWidth;
	const int h = b.canvasHeight;
	std::vector<uint8_t> inDiamond(b.canvas.size(), 0);
	for (const Point t : tiles) {
		const float cx = static_cast<float>((t.x - t.y) * 32 - b.originX);
		const float cy = static_cast<float>((t.x + t.y + 2) * 16 - 16 - b.originY);
		for (int y = static_cast<int>(cy) - 17; y <= static_cast<int>(cy) + 17; y++)
			for (int x = static_cast<int>(cx) - 33; x <= static_cast<int>(cx) + 33; x++)
				if (x >= 0 && y >= 0 && x < w && y < h && std::abs(static_cast<float>(x) + 0.5F - cx) / 32.0F + std::abs(static_cast<float>(y) + 0.5F - cy) / 16.0F <= 1.05F)
					inDiamond[static_cast<size_t>(y) * w + x] = 1;
	}
	auto stone = [](int16_t p) {
		if (p <= 0)
			return false;
		const SDL_Color c = logical_palette[static_cast<uint8_t>(p)];
		return c.b >= c.r + 12 && c.b >= c.g + 8;
	};
	std::vector<int16_t> kept = b.canvas;
	for (size_t i = 0; i < kept.size(); i++)
		if (inDiamond[i] != 0)
			kept[i] = stone(raw.canvas[i]) ? raw.canvas[i] : int16_t { -1 };
	b.canvas = kept;
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			const size_t i = static_cast<size_t>(y) * w + x;
			if (inDiamond[i] == 0 || kept[i] <= 0)
				continue;
			int around = 0;
			for (int dy = -1; dy <= 1; dy++)
				for (int dx = -1; dx <= 1; dx++)
					if ((dx != 0 || dy != 0) && x + dx >= 0 && y + dy >= 0 && x + dx < w && y + dy < h && kept[static_cast<size_t>(y + dy) * w + x + dx] > 0)
						around++;
			if (around < 3)
				b.canvas[i] = -1;
		}
	for (int16_t &p : b.canvas) {
		if (p <= 0)
			continue;
		const SDL_Color c = logical_palette[static_cast<uint8_t>(p)];
		const bool orange = c.r >= 80 && c.r * 10 >= c.b * 20 && c.g * 10 >= c.b * 12;
		const bool red = c.r >= 80 && c.r >= 2 * c.g && c.r >= 2 * c.b;
		if (orange || red)
			p = -1;
	}
}

#ifdef _DEBUG
/** @brief Debug (FP_DUMP_PROPS=dir): a prop group's canvas as dir/name_N.ppm. */
void DumpPropCanvas(const Building &b, const char *name)
{
	const char *dir = std::getenv("FP_DUMP_PROPS");
	if (dir == nullptr)
		return;
	static int count = 0;
	if (FILE *f = std::fopen(StrCat(dir, "/", name, "_", count++, ".ppm").c_str(), "wb"); f != nullptr) {
		std::fprintf(f, "P6\n%d %d\n255\n", b.canvasWidth, b.canvasHeight);
		for (const int16_t p : b.canvas) {
			const SDL_Color c = p > 0 ? logical_palette[p] : SDL_Color { 0, 120, 0, 255 };
			const uint8_t rgb[3] { c.r, c.g, c.b };
			std::fwrite(rgb, 1, 3, f);
		}
		std::fclose(f);
	}
}
#endif

int StoneCount = 0; // the boulders made (logged)

/** @brief The boulders of a group of rock tiles. */
void BuildRocks(const std::vector<Point> &tiles)
{
	TownProp prop;
	prop.kind = PropKind::Rock;
	prop.ownCanvas = true;
	Building &b = prop.art;
	BuildPropCanvas(b, tiles);
#ifdef _DEBUG
	DumpPropCanvas(b, "rocks");
#endif
	if (b.canvas.empty())
		return;
	for (const auto &pixels : CanvasComponents(b, 40))
		PackStones(b, pixels, ComponentColor(b, pixels), b.mesh, prop.farMesh, StoneCount);
	if (b.mesh.empty())
		return;
	FitPropBounds(prop);
	TownProps.push_back(std::move(prop));
}

/**
 * The caves' entrance (the town's warp to level 9, its trigger at CaveMouthCorner's tile): a mound of big boulders
 * as the art draws it, about 2.6 tiles high, its mouth facing south-east (+x, +y) onto the trigger's tile, a tunnel
 * carved through the stones there, dark inside (StoneCarve). The mound's middle lies CaveMoundBack behind the corner
 * of the trigger's tile the mouth opens at; its stones are listed in that frame (CaveStones: along the mouth's axis,
 * across it, the height of their underside, their radius, their height). While the warp is closed, a boulder blocks
 * the mouth, as in the art. The mound's tiles are the boulder tiles within CaveMoundReach of its middle, and the
 * mouth's own pieces; its stones take their colours from their art (BuildPropCanvas), the rest of the field round it
 * is packed as before.
 */
constexpr Point CaveMouthCorner { 17, 69 };
constexpr float CaveMoundBack = 1.7F;
constexpr float CaveMoundReach = 2.6F;
constexpr std::array<std::array<float, 5>, 15> CaveStones { {
	{ -0.9F, 0.0F, 0.0F, 1.2F, 1.05F },    // the back, the biggest
	{ -0.15F, -1.4F, 0.0F, 0.95F, 0.85F }, // its flanks
	{ -0.15F, 1.4F, 0.0F, 0.95F, 0.85F },
	{ 1.05F, -0.72F, 0.0F, 0.65F, 0.8F },  // the mouth's jambs (the carve cuts the arch out of them)
	{ 1.05F, 0.72F, 0.0F, 0.65F, 0.8F },
	{ 0.55F, -0.55F, 0.0F, 0.6F, 0.75F },  // the tunnel's sides, behind them
	{ 0.55F, 0.55F, 0.0F, 0.6F, 0.75F },
	{ -1.3F, -1.3F, 0.0F, 0.8F, 0.7F },    // behind
	{ -1.3F, 1.3F, 0.0F, 0.8F, 0.7F },
	{ 1.4F, -1.75F, 0.0F, 0.5F, 0.45F },   // in front, either side
	{ 1.45F, 1.7F, 0.0F, 0.45F, 0.4F },
	{ 0.9F, 0.0F, 0.85F, 0.78F, 0.6F },    // the lintel over the mouth, on the jambs
	{ -0.45F, -0.65F, 0.95F, 0.9F, 0.75F }, // the second course
	{ -0.35F, 0.7F, 0.9F, 0.85F, 0.7F },
	{ -0.25F, 0.0F, 1.6F, 0.72F, 0.6F },   // the crown
} };
/** The mouth: from this far along its axis (the tunnel's dark back) out, this wide each side, this high. */
constexpr float CaveMouthBack = 0.45F;
constexpr float CaveMouthHalf = 0.5F;
constexpr float CaveMouthTop = 1.2F;
constexpr float CaveMouthRagged = 0.12F;

/** @brief Whether the town has the caves' entrance (its mouth's pieces, open or closed), and its stones' frame. */
bool FindCaveMound(Vec2 &middle, Vec2 &along)
{
	if (leveltype != DTYPE_TOWN || !InDungeonBounds({ 15, 70 }) || dPiece[14][70] != 1202 || dPiece[15][70] != 1199)
		return false;
	along = { 0.70710678F, 0.70710678F };
	middle = { static_cast<float>(CaveMouthCorner.x) - along.x * CaveMoundBack, static_cast<float>(CaveMouthCorner.y) - along.y * CaveMoundBack };
	return true;
}

void BuildCaveMound(const std::vector<Point> &tiles, Vec2 middle, Vec2 along)
{
	TownProp prop;
	prop.kind = PropKind::Rock;
	prop.ownCanvas = true;
	Building &b = prop.art;
	BuildPropCanvas(b, tiles);
	if (b.canvas.empty())
		return;
	std::vector<std::pair<int, int>> pixels;
	for (int y = 0; y < b.canvasHeight; y++)
		for (int x = 0; x < b.canvasWidth; x++)
			if (b.canvas[static_cast<size_t>(y) * b.canvasWidth + x] > 0)
				pixels.emplace_back(x, y);
	const uint8_t color = pixels.empty() ? uint8_t { 1 } : ComponentColor(b, pixels);
	const bool open = IsWarpOpen(DTYPE_CAVES);
	const StoneCarve carve { middle, along, CaveMouthBack, CaveMouthHalf, CaveMouthTop, CaveMouthRagged, color };
	const Vec2 across { -along.y, along.x };
	const float turn = std::atan2(along.y, along.x);
	auto stone = [&](float u, float v, float bottom, float radius, float height, uint32_t k) {
		const V3 at { middle.x + along.x * u + across.x * v, middle.y + along.y * u + across.y * v, bottom + height * 0.35F };
		const uint32_t seed = 0x5EEDU * (k + 1) ^ 0x2545F491U;
		AddStone(b.mesh, at, radius, height, turn + static_cast<float>(k) * 0.7F, seed, 1, color, open ? &carve : nullptr);
		AddStone(prop.farMesh, at, radius, height, turn + static_cast<float>(k) * 0.7F, seed, 0, color, open ? &carve : nullptr);
		StoneCount++;
	};
	for (size_t k = 0; k < CaveStones.size(); k++) {
		const auto &[u, v, bottom, radius, height] = CaveStones[k];
		stone(u, v, bottom, radius, height, static_cast<uint32_t>(k));
	}
	if (open) {
		// The tunnel's back and floor, dark, in case no stone closes them.
		auto point = [&](float u, float v, float z) { return V3 { middle.x + along.x * u + across.x * v, middle.y + along.y * u + across.y * v, z }; };
		const float h = CaveMouthHalf;
		auto dark = [&](std::array<V3, 4> q, V3 inside, int darkness) {
			for (const auto &[i, j, k] : { std::array { 0, 1, 2 }, std::array { 0, 2, 3 } }) {
				const size_t before = b.mesh.size();
				AddSolidTriangle(b.mesh, q[i], q[j], q[k], inside, 0);
				if (b.mesh.size() > before) {
					b.mesh.back().mapping = MeshSolid;
					b.mesh.back().fallback = darkness < 0 ? NearestColor(6.0F, 6.0F, 9.0F) : color;
					b.mesh.back().darkness = static_cast<uint8_t>(std::max(darkness, 0));
					prop.farMesh.push_back(b.mesh.back());
				}
			}
		};
		const V3 below = point(CaveMouthBack + 0.5F, 0.0F, -1.0F);
		const float t = CaveMouthTop * (1.0F + CaveMouthRagged);
		const float w = h * (1.0F + CaveMouthRagged);
		dark({ point(CaveMouthBack, -w, 0.0F), point(CaveMouthBack, w, 0.0F), point(CaveMouthBack, w, t), point(CaveMouthBack, -w, t) }, point(CaveMouthBack - 1.0F, 0.0F, 0.5F), -1);
		// The floor going in, black, under the arch's middle (the rock's foot either side of it is the stones').
		dark({ point(CaveMouthBack, -h * 0.8F, 0.004F), point(CaveMouthBack + 0.65F, -h * 0.8F, 0.004F), point(CaveMouthBack + 0.65F, h * 0.8F, 0.004F), point(CaveMouthBack, h * 0.8F, 0.004F) }, below, -1);
	} else {
		// The boulder the art blocks the mouth with.
		stone(1.15F, 0.0F, 0.0F, 0.58F, 0.8F, 100);
	}
	if (b.mesh.empty())
		return;
	FitPropBounds(prop);
	LogInfo("fp: the caves' entrance: a mound of {} stones on {} tiles, {}", CaveStones.size() + (open ? 0 : 1), tiles.size(), open ? "open" : "closed");
	TownProps.push_back(std::move(prop));
}

/** @brief The gravestones and crosses of a group of grave tiles. */
void BuildGraves(const std::vector<Point> &tiles)
{
	TownProp prop;
	prop.kind = PropKind::Slab;
	prop.ownCanvas = true;
	Building &b = prop.art;
	BuildPropCanvas(b, tiles);
#ifdef _DEBUG
	DumpPropCanvas(b, "graves");
#endif
	if (b.canvas.empty())
		return;
	// A stone the ground's removal cut in two (its dark foot taken out) is one again: pieces one above the other.
	std::vector<std::vector<std::pair<int, int>>> pieces = CanvasComponents(b, 3);
	auto bounds = [](const std::vector<std::pair<int, int>> &piece) {
		std::array<int, 4> r { std::numeric_limits<int>::max(), std::numeric_limits<int>::lowest(), std::numeric_limits<int>::max(), std::numeric_limits<int>::lowest() };
		for (const auto &[x, y] : piece)
			r = { std::min(r[0], x), std::max(r[1], x), std::min(r[2], y), std::max(r[3], y) };
		return r;
	};
	for (bool merged = true; merged;) {
		merged = false;
		for (size_t i = 0; i < pieces.size() && !merged; i++)
			for (size_t j = i + 1; j < pieces.size() && !merged; j++) {
				const std::array<int, 4> a = bounds(pieces[i]);
				const std::array<int, 4> c = bounds(pieces[j]);
				if (a[0] <= c[1] + 1 && c[0] <= a[1] + 1 && std::max(a[2], c[2]) - std::min(a[3], c[3]) <= 12) {
					pieces[i].insert(pieces[i].end(), pieces[j].begin(), pieces[j].end());
					pieces.erase(pieces.begin() + static_cast<ptrdiff_t>(j));
					merged = true;
				}
			}
	}
	for (const auto &pixels : pieces)
		if (pixels.size() >= 12)
			ShapeGrave(b, pixels, ComponentColor(b, pixels), b.mesh);
	if (b.mesh.empty())
		return;
	prop.farMesh = b.mesh;
	FitPropBounds(prop);
	TownProps.push_back(std::move(prop));
}

/**
 * @brief A tree the original art paints into a landmark's own tiles, which no tree tile holds (the autumn tree at the
 * corner of Adria's shack): the town's tree of the same drawing, the one whose foot is at `like`, stands in for it.
 */
struct PaintedTree {
	const char *landmark;
	Vec2 foot; // the foot of the art's trunk
	Vec2 like;
};
constexpr std::array<PaintedTree, 1> PaintedTrees { {
	{ "adria", { 77.15F, 21.45F }, { 79.6F, 4.35F } },
} };

void AddPaintedTrees()
{
	for (const PaintedTree &painted : PaintedTrees) {
		const bool placed = std::any_of(Buildings.begin(), Buildings.end(), [&](const Building &b) {
			return b.landmark >= 0 && std::string_view(Landmarks[b.landmark].name) == painted.landmark;
		});
		// The nearest tree to where it is expected (within a tile and a half: its foot moves as its cutting changes).
		const auto like = std::min_element(TreeSprites.begin(), TreeSprites.end(), [&](const TreeSprite &a, const TreeSprite &c) {
			return std::hypot(a.foot.x - painted.like.x, a.foot.y - painted.like.y) < std::hypot(c.foot.x - painted.like.x, c.foot.y - painted.like.y);
		});
		if (!placed || like == TreeSprites.end() || std::hypot(like->foot.x - painted.like.x, like->foot.y - painted.like.y) > 1.5F)
			continue;
		TreeSprite tree = *like;
		tree.foot = painted.foot;
		TreeSprites.push_back(std::move(tree));
	}
}

/**
 * @brief Replaces the town's remaining tall-art tiles (those no building draws) with meshes: trees, and the hulls of
 * the rest. Pieces of the church's art its model stands in for are hidden. Marks the tiles (BuildingTiles 3) so
 * they are no longer billboards and their floor is the ground under them.
 */
void BuildTownProps()
{
	TownProps.clear();
	StoneCount = 0;
	PropHulls.clear();
	TreeSprites.clear();
	if (leveltype != DTYPE_TOWN)
		return;
	auto index = [](int x, int y) { return static_cast<size_t>(y) * MAXDUNX + x; };
	// The town's ground colours: those its open ground's floors use (a tenth of a percent of their texels or more).
	{
		std::array<int, 256> counts {};
		int total = 0;
		for (int y = 0; y < MAXDUNY; y++)
			for (int x = 0; x < MAXDUNX; x++) {
				const PieceTextures &t = GetPieceTextures(dPiece[x][y]);
				if (t.hasTallArt || IsTileSolid({ x, y }))
					continue;
				for (const int16_t texel : t.floor)
					if (texel > 0) {
						counts[texel]++;
						total++;
					}
			}
		std::array<bool, 256> ground {};
		for (int c = 1; c < 256; c++)
			ground[c] = counts[c] * 1000 >= total;
		for (int y = 0; y < MAXDUNY; y++)
			for (int x = 0; x < MAXDUNX; x++)
				if (BuildingTiles[index(x, y)] == 0 && GetPieceTextures(dPiece[x][y]).hasTallArt)
					RemoveUprightGround(dPiece[x][y], ground);
	}
	std::vector<PropKind> kinds(static_cast<size_t>(MAXDUNX) * MAXDUNY, PropKind::None);
	for (int y = 0; y < MAXDUNY; y++)
		for (int x = 0; x < MAXDUNX; x++) {
			if (BuildingTiles[index(x, y)] == 0)
				kinds[index(x, y)] = TownPieceKind(dPiece[x][y]);
			else if (GetPieceTextures(dPiece[x][y]).hasTallArt && IsTreePiece(dPiece[x][y]))
				kinds[index(x, y)] = PropKind::Tree; // a tree beside a house, its tile claimed by the house's art: still the tree's
		}
	// The caves' entrance's mouth is boulders (BuildCaveMound), whatever its pieces' art is told as.
	if (Vec2 middle, along; FindCaveMound(middle, along))
		for (int y = 68; y <= 71; y++)
			for (int x = 14; x <= 17; x++)
				if (BuildingTiles[index(x, y)] == 0 && GetPieceTextures(dPiece[x][y]).hasTallArt)
					kinds[index(x, y)] = PropKind::Rock;
	// Trees: groups of neighbouring tree tiles.
	int treeCount = 0;
	std::vector<uint8_t> seen(kinds.size(), 0);
	for (int y = 0; y < MAXDUNY; y++) {
		for (int x = 0; x < MAXDUNX; x++) {
			if (kinds[index(x, y)] != PropKind::Tree || seen[index(x, y)] != 0)
				continue;
			std::vector<Point> group;
			std::vector<Point> stack { { x, y } };
			seen[index(x, y)] = 1;
			while (!stack.empty()) {
				const Point p = stack.back();
				stack.pop_back();
				group.push_back(p);
				for (int dy = -1; dy <= 1; dy++)
					for (int dx = -1; dx <= 1; dx++) {
						const Point q { p.x + dx, p.y + dy };
						if (InDungeonBounds(q) && seen[index(q.x, q.y)] == 0 && kinds[index(q.x, q.y)] == PropKind::Tree) {
							seen[index(q.x, q.y)] = 1;
							stack.push_back(q);
						}
					}
			}
			BuildTrees(group, treeCount);
		}
	}
	AddPaintedTrees();
	// The low stone walls' heights, from their art.
	WallHeights.assign(static_cast<size_t>(MAXDUNX) * MAXDUNY, 0.0F);
	for (int y = 0; y < MAXDUNY; y++)
		for (int x = 0; x < MAXDUNX; x++) {
			if (kinds[index(x, y)] != PropKind::Wall)
				continue;
			float top = 0;
			for (const float h : PropHullHeights(GetPieceTextures(dPiece[x][y]), 0.7F, 8))
				top = std::max(top, h);
			WallHeights[index(x, y)] = std::clamp(top, 0.35F, 1.1F);
		}
	{
		// Mostly the walls' common height: the art's tiles of a wall differ more than the wall does.
		std::vector<float> heights;
		for (const float h : WallHeights)
			if (h > 0)
				heights.push_back(h);
		if (!heights.empty()) {
			std::nth_element(heights.begin(), heights.begin() + static_cast<ptrdiff_t>(heights.size() / 2), heights.end());
			const float common = heights[heights.size() / 2];
			for (float &h : WallHeights)
				if (h > 0)
					h = 0.25F * h + 0.75F * common;
		}
	}
	// Corners the art draws across two tiles meeting at a diagonal (the walls ending on the tiles either side of an
	// empty one): the empty tile is the corner, so the two walls meet in it instead of leaving a gap at the corner.
	std::vector<Point> corners;
	for (int y = 1; y < MAXDUNY - 1; y++)
		for (int x = 1; x < MAXDUNX - 1; x++) {
			if (kinds[index(x, y)] != PropKind::None || BuildingTiles[index(x, y)] != 0)
				continue;
			auto wall = [&](int dx, int dy) { return kinds[index(x + dx, y + dy)] == PropKind::Wall; };
			for (const int sx : { -1, 1 })
				for (const int sy : { -1, 1 })
					if (wall(sx, 0) && wall(0, sy) && !wall(-sx, 0) && !wall(0, -sy) && wall(2 * sx, 0) && wall(0, 2 * sy))
						corners.push_back({ x, y });
		}
	for (const Point c : corners) {
		kinds[index(c.x, c.y)] = PropKind::Wall;
		float sum = 0;
		int count = 0;
		for (const Point n : { Point { c.x - 1, c.y }, Point { c.x + 1, c.y }, Point { c.x, c.y - 1 }, Point { c.x, c.y + 1 } })
			if (WallHeights[index(n.x, n.y)] > 0) {
				sum += WallHeights[index(n.x, n.y)];
				count++;
			}
		WallHeights[index(c.x, c.y)] = count > 0 ? sum / static_cast<float>(count) : 0.6F;
		LogInfo("fp: low wall corner closed at {}, {}", c.x, c.y);
	}
	// Where across their tiles the walls stand: each straight run of wall tiles where most of its art has it (the
	// art's tiles of a run differ, some with a buttress or a fallen stone), so they line up with no gaps between.
	WallAcross.assign(static_cast<size_t>(MAXDUNX) * MAXDUNY * 2, 0.0F);
	for (int axis = 0; axis < 2; axis++) {
		auto at = [&](int line, int k) { return axis == 0 ? Point { k, line } : Point { line, k }; };
		auto isWall = [&](Point p) { return InDungeonBounds(p) && kinds[index(p.x, p.y)] == PropKind::Wall; };
		for (int line = 0; line < (axis == 0 ? MAXDUNY : MAXDUNX); line++) {
			const int length = axis == 0 ? MAXDUNX : MAXDUNY;
			for (int k = 0; k < length;) {
				if (!isWall(at(line, k)) || !isWall(at(line, k + 1))) {
					k++;
					continue;
				}
				int end = k + 1;
				while (isWall(at(line, end + 1)))
					end++;
				std::vector<float> acrosses;
				for (int m = k; m <= end; m++) {
					const Point p = at(line, m);
					acrosses.push_back(WallArtAcross(GetPieceTextures(dPiece[p.x][p.y]), axis == 0));
				}
				std::nth_element(acrosses.begin(), acrosses.begin() + static_cast<ptrdiff_t>(acrosses.size() / 2), acrosses.end());
				const float across = acrosses[acrosses.size() / 2];
				for (int m = k; m <= end; m++) {
					const Point p = at(line, m);
					WallAcross[index(p.x, p.y) * 2 + static_cast<size_t>(axis)] = across;
				}
				k = end + 1;
			}
		}
	}
	// The rest: each tile's hull (one per piece, placed on every tile showing it), in blocks.
	std::unordered_map<int, size_t> blocks;
	int hullTiles = 0;
	for (int y = 0; y < MAXDUNY; y++) {
		for (int x = 0; x < MAXDUNX; x++) {
			const PropKind kind = kinds[index(x, y)];
			if (kind == PropKind::None)
				continue;
			if (kind == PropKind::Leftover) {
				BuildingTiles[index(x, y)] = 1;
				continue;
			}
			if (BuildingTiles[index(x, y)] != 0)
				continue; // a tree's tile a house claimed: its ground stays the house's
			BuildingTiles[index(x, y)] = 3;
			if (kind == PropKind::Tree || kind == PropKind::Rock || kind == PropKind::Slab)
				continue; // built by group, below (trees above)
			std::array<bool, 4> wallToward {};
			if (kind == PropKind::Wall) {
				const std::array<Point, 4> next { Point { x - 1, y }, Point { x + 1, y }, Point { x, y - 1 }, Point { x, y + 1 } };
				for (size_t k = 0; k < next.size(); k++)
					wallToward[k] = InDungeonBounds(next[k]) && kinds[index(next[k].x, next[k].y)] == PropKind::Wall;
			}
			const bool straightWall = std::any_of(wallToward.begin(), wallToward.end(), [](bool b) { return b; });
			const uint16_t piece = dPiece[x][y];
			auto found = PropHulls.find(piece);
			if (found == PropHulls.end()) {
				const int cells = PropCells(kind);
				found = PropHulls.emplace(piece, PropHullPair { BuildPropHull(piece, kind, cells), BuildPropHull(piece, kind, (cells + 1) / 2) }).first;
			}
			const PropHull &hull = found->second.near;
			if (hull.triangles.empty() && !straightWall)
				continue;
			const int key = (y / PropBlock) * 1000 + (x / PropBlock) * 10 + static_cast<int>(kind);
			auto block = blocks.find(key);
			if (block == blocks.end()) {
				TownProp prop;
				prop.kind = kind;
				prop.art.x0 = static_cast<float>(x);
				prop.art.y0 = static_cast<float>(y);
				prop.art.x1 = static_cast<float>(x + 1);
				prop.art.y1 = static_cast<float>(y + 1);
				prop.art.lowestZ = 0;
				prop.art.highestZ = 0;
				TownProps.push_back(std::move(prop));
				block = blocks.emplace(key, TownProps.size() - 1).first;
			}
			TownProp &prop = TownProps[block->second];
			Building &b = prop.art;
			b.x0 = std::min(b.x0, static_cast<float>(x));
			b.y0 = std::min(b.y0, static_cast<float>(y));
			b.x1 = std::max(b.x1, static_cast<float>(x + 1));
			b.y1 = std::max(b.y1, static_cast<float>(y + 1));
			b.highestZ = std::max(b.highestZ, hull.top);
			auto place = [&](const PropHull &from, std::vector<WorldTriangle> &to) {
				for (WorldTriangle tri : from.triangles) {
					for (V3 &p : tri.p) {
						p.x += static_cast<float>(x);
						p.y += static_cast<float>(y);
					}
					tri.mirrorX += static_cast<float>(x);
					tri.mirrorY += static_cast<float>(y);
					tri.artX = static_cast<int16_t>(x);
					tri.artY = static_cast<int16_t>(y);
					to.push_back(tri);
				}
			};
			if (straightWall) {
				BuildTownWallTile({ x, y }, wallToward, TownWallSegments, b.mesh);
				BuildTownWallTile({ x, y }, wallToward, TownWallFarSegments, prop.farMesh);
				b.highestZ = std::max(b.highestZ, 1.4F);
			} else {
				place(hull, b.mesh);
				place(found->second.far, prop.farMesh);
			}
			hullTiles++;
		}
	}
	for (TownProp &prop : TownProps) {
		prop.middle = { (prop.art.x0 + prop.art.x1) / 2.0F, (prop.art.y0 + prop.art.y1) / 2.0F };
		prop.radius = std::hypot(prop.art.x1 - prop.art.x0, prop.art.y1 - prop.art.y0) / 2.0F;
	}
	// The caves' entrance: its mound of boulders, built as one (BuildCaveMound); the field round it as before.
	std::vector<uint8_t> inMound(kinds.size(), 0);
	{
		Vec2 middle;
		Vec2 along;
		if (FindCaveMound(middle, along)) {
			std::vector<Point> tiles;
			for (int y = 0; y < MAXDUNY; y++)
				for (int x = 0; x < MAXDUNX; x++) {
					const float dx = static_cast<float>(x) + 0.5F - middle.x;
					const float dy = static_cast<float>(y) + 0.5F - middle.y;
					const bool mouth = x >= 14 && x <= 17 && y >= 68 && y <= 71 && GetPieceTextures(dPiece[x][y]).hasTallArt;
					if (!mouth && (kinds[index(x, y)] != PropKind::Rock || dx * dx + dy * dy > CaveMoundReach * CaveMoundReach))
						continue;
					inMound[index(x, y)] = 1;
					BuildingTiles[index(x, y)] = 3;
					tiles.push_back({ x, y });
				}
			BuildCaveMound(tiles, middle, along);
			hullTiles += static_cast<int>(tiles.size());
		}
	}
	// Boulders and gravestones: groups of neighbouring tiles of each, from their art together.
	for (const PropKind kind : { PropKind::Rock, PropKind::Slab }) {
		std::vector<uint8_t> grouped(inMound);
		for (int y = 0; y < MAXDUNY; y++) {
			for (int x = 0; x < MAXDUNX; x++) {
				if (kinds[index(x, y)] != kind || grouped[index(x, y)] != 0 || BuildingTiles[index(x, y)] != 3)
					continue;
				std::vector<Point> group;
				std::vector<Point> stack { { x, y } };
				grouped[index(x, y)] = 1;
				while (!stack.empty()) {
					const Point p = stack.back();
					stack.pop_back();
					group.push_back(p);
					for (int dy = -1; dy <= 1; dy++)
						for (int dx = -1; dx <= 1; dx++) {
							const Point q { p.x + dx, p.y + dy };
							if (InDungeonBounds(q) && grouped[index(q.x, q.y)] == 0 && kinds[index(q.x, q.y)] == kind && BuildingTiles[index(q.x, q.y)] == 3) {
								grouped[index(q.x, q.y)] = 1;
								stack.push_back(q);
							}
						}
				}
				if (kind == PropKind::Rock)
					BuildRocks(group);
				else
					BuildGraves(group);
				hullTiles += static_cast<int>(group.size());
			}
		}
	}
	size_t triangles = 0;
	std::array<size_t, 8> byKind {};
	for (const TownProp &prop : TownProps) {
		triangles += prop.art.mesh.size();
		byKind[static_cast<size_t>(prop.kind)] += prop.art.mesh.size();
	}
	LogInfo("fp: prop triangles: rocks {} ({} stones), walls {}, graves {}, grass {}", byKind[static_cast<size_t>(PropKind::Rock)], StoneCount, byKind[static_cast<size_t>(PropKind::Wall)], byKind[static_cast<size_t>(PropKind::Slab)], byKind[static_cast<size_t>(PropKind::Grass)]);
	LogInfo("fp: town props: {} tree sprites, {} hull tiles ({} pieces), {} meshes, {} triangles", treeCount, hullTiles, PropHulls.size(), TownProps.size(), triangles);
}

/** @brief Whether a point is inside a lit room (with its walls): no light from the grids there. */
bool InsideLitRoom(float x, float y, float z)
{
	constexpr float Wall = 0.15F;
	for (const LitRoom &room : LitRooms)
		if (x > room.x0 - Wall && x < room.x1 + Wall && y > room.y0 - Wall && y < room.y1 + Wall && z < room.top + 0.3F)
			return true;
	return false;
}

/**
 * @brief What blocks the lamps' light around a point: the town's buildings and prop hulls, as a grid of voxels
 * (1 solid, 2 the church's glass, 3 a window's glass) over every surface of them.
 */
struct Occluders {
	float x0 = 0;
	float y0 = 0;
	int nx = 0;
	int ny = 0;
	int nz = 0;
	std::vector<uint8_t> cells; // [z][y][x]
	static constexpr float Size = 0.0625F;

	void Mark(const WorldTriangle &tri, uint8_t value)
	{
		const V3 &a = tri.p[0];
		const V3 &b = tri.p[1];
		const V3 &c = tri.p[2];
		auto length = [](const V3 &p, const V3 &q) { return std::sqrt((p.x - q.x) * (p.x - q.x) + (p.y - q.y) * (p.y - q.y) + (p.z - q.z) * (p.z - q.z)); };
		const int n = std::max(1, static_cast<int>(std::ceil(std::max({ length(a, b), length(b, c), length(c, a) }) / (Size * 0.5F))));
		for (int i = 0; i <= n; i++) {
			for (int j = 0; i + j <= n; j++) {
				const float u = static_cast<float>(i) / static_cast<float>(n);
				const float v = static_cast<float>(j) / static_cast<float>(n);
				const int cx = static_cast<int>(std::floor((a.x + (b.x - a.x) * u + (c.x - a.x) * v - x0) / Size));
				const int cy = static_cast<int>(std::floor((a.y + (b.y - a.y) * u + (c.y - a.y) * v - y0) / Size));
				const int cz = static_cast<int>(std::floor((a.z + (b.z - a.z) * u + (c.z - a.z) * v) / Size));
				if (cx < 0 || cy < 0 || cz < 0 || cx >= nx || cy >= ny || cz >= nz)
					continue;
				uint8_t &cell = cells[(static_cast<size_t>(cz) * ny + cy) * nx + cx];
				if (cell != 1)
					cell = value;
			}
		}
	}

	void Add(const std::vector<WorldTriangle> &mesh, float bx0, float by0, float bx1, float by1, bool glass)
	{
		if (bx1 < x0 || by1 < y0 || bx0 > x0 + static_cast<float>(nx) * Size || by0 > y0 + static_cast<float>(ny) * Size)
			return;
		for (const WorldTriangle &tri : mesh) {
			if (tri.mapping == MeshInterior || tri.rim)
				continue; // (a cross window's sides line its opening: marked, they would close it to the light)
			Mark(tri, tri.mapping == MeshPane ? 3 : (glass && tri.texture == GlassTexture && GlassTexture >= 0 ? 2 : 1));
		}
	}

	/** @brief The cells a triangle passes through (as Mark marks them), each given to `visit` until it returns true. */
	template <typename Visit>
	bool AnyCell(const WorldTriangle &tri, Visit visit) const
	{
		const V3 &a = tri.p[0];
		const V3 &b = tri.p[1];
		const V3 &c = tri.p[2];
		auto length = [](const V3 &p, const V3 &q) { return std::sqrt((p.x - q.x) * (p.x - q.x) + (p.y - q.y) * (p.y - q.y) + (p.z - q.z) * (p.z - q.z)); };
		const int n = std::max(1, static_cast<int>(std::ceil(std::max({ length(a, b), length(b, c), length(c, a) }) / (Size * 0.5F))));
		for (int i = 0; i <= n; i++) {
			for (int j = 0; i + j <= n; j++) {
				const float u = static_cast<float>(i) / static_cast<float>(n);
				const float v = static_cast<float>(j) / static_cast<float>(n);
				const int cx = static_cast<int>(std::floor((a.x + (b.x - a.x) * u + (c.x - a.x) * v - x0) / Size));
				const int cy = static_cast<int>(std::floor((a.y + (b.y - a.y) * u + (c.y - a.y) * v - y0) / Size));
				const int cz = static_cast<int>(std::floor((a.z + (b.z - a.z) * u + (c.z - a.z) * v) / Size));
				if (cx >= 0 && cy >= 0 && cz >= 0 && cx < nx && cy < ny && cz < nz && visit((static_cast<size_t>(cz) * ny + cy) * nx + cx))
					return true;
			}
		}
		return false;
	}

	/** @brief The first marked cell a ray from `from` (outside the grid or in it) along a unit direction meets, or -1. */
	ptrdiff_t FirstMet(V3 from, V3 d) const
	{
		// Into the grid's box first.
		const std::array<float, 3> lo { x0, y0, 0 };
		const std::array<float, 3> hi { x0 + static_cast<float>(nx) * Size, y0 + static_cast<float>(ny) * Size, static_cast<float>(nz) * Size };
		const std::array<float, 3> o { from.x, from.y, from.z };
		const std::array<float, 3> dir { d.x, d.y, d.z };
		float enter = 0;
		float leave = std::numeric_limits<float>::max();
		for (int k = 0; k < 3; k++) {
			if (std::abs(dir[k]) < 1e-9F) {
				if (o[k] < lo[k] || o[k] > hi[k])
					return -1;
				continue;
			}
			float t0 = (lo[k] - o[k]) / dir[k];
			float t1 = (hi[k] - o[k]) / dir[k];
			if (t0 > t1)
				std::swap(t0, t1);
			enter = std::max(enter, t0);
			leave = std::min(leave, t1);
		}
		if (enter > leave)
			return -1;
		const float start = enter + 1e-4F;
		float px = (from.x + d.x * start - x0) / Size;
		float py = (from.y + d.y * start - y0) / Size;
		float pz = (from.z + d.z * start) / Size;
		int ix = std::clamp(static_cast<int>(std::floor(px)), 0, nx - 1);
		int iy = std::clamp(static_cast<int>(std::floor(py)), 0, ny - 1);
		int iz = std::clamp(static_cast<int>(std::floor(pz)), 0, nz - 1);
		const int sx = d.x > 0 ? 1 : -1;
		const int sy = d.y > 0 ? 1 : -1;
		const int sz = d.z > 0 ? 1 : -1;
		constexpr float Far = std::numeric_limits<float>::max();
		const float ddx = d.x != 0 ? std::abs(1.0F / d.x) : Far;
		const float ddy = d.y != 0 ? std::abs(1.0F / d.y) : Far;
		const float ddz = d.z != 0 ? std::abs(1.0F / d.z) : Far;
		float tx = d.x != 0 ? ((d.x > 0 ? static_cast<float>(ix + 1) - px : px - static_cast<float>(ix)) * ddx) : Far;
		float ty = d.y != 0 ? ((d.y > 0 ? static_cast<float>(iy + 1) - py : py - static_cast<float>(iy)) * ddy) : Far;
		float tz = d.z != 0 ? ((d.z > 0 ? static_cast<float>(iz + 1) - pz : pz - static_cast<float>(iz)) * ddz) : Far;
		while (ix >= 0 && iy >= 0 && iz >= 0 && ix < nx && iy < ny && iz < nz) {
			const size_t cell = (static_cast<size_t>(iz) * ny + iy) * nx + ix;
			if (cells[cell] != 0)
				return static_cast<ptrdiff_t>(cell);
			if (tx <= ty && tx <= tz) {
				tx += ddx;
				ix += sx;
			} else if (ty <= tz) {
				ty += ddy;
				iy += sy;
			} else {
				tz += ddz;
				iz += sz;
			}
		}
		return -1;
	}

	/** @brief Whether a ray from `from` along a unit direction gets `length` away without meeting anything (glass included). */
	bool Escapes(V3 from, V3 d, float length) const
	{
		float px = (from.x - x0) / Size;
		float py = (from.y - y0) / Size;
		float pz = from.z / Size;
		int ix = static_cast<int>(std::floor(px));
		int iy = static_cast<int>(std::floor(py));
		int iz = static_cast<int>(std::floor(pz));
		const int sx = d.x > 0 ? 1 : -1;
		const int sy = d.y > 0 ? 1 : -1;
		const int sz = d.z > 0 ? 1 : -1;
		constexpr float Far = std::numeric_limits<float>::max();
		const float ddx = d.x != 0 ? std::abs(Size / d.x) : Far;
		const float ddy = d.y != 0 ? std::abs(Size / d.y) : Far;
		const float ddz = d.z != 0 ? std::abs(Size / d.z) : Far;
		float tx = d.x != 0 ? ((d.x > 0 ? static_cast<float>(ix + 1) - px : px - static_cast<float>(ix)) * ddx) : Far;
		float ty = d.y != 0 ? ((d.y > 0 ? static_cast<float>(iy + 1) - py : py - static_cast<float>(iy)) * ddy) : Far;
		float tz = d.z != 0 ? ((d.z > 0 ? static_cast<float>(iz + 1) - pz : pz - static_cast<float>(iz)) * ddz) : Far;
		float t = 0;
		while (t < length) {
			if (iz < 0)
				return false; // into the ground
			if (ix < 0 || iy < 0 || ix >= nx || iy >= ny || iz >= nz)
				return true; // out of the grid: in the open
			if (cells[(static_cast<size_t>(iz) * ny + iy) * nx + ix] != 0)
				return false;
			if (tx <= ty && tx <= tz) {
				t = tx;
				tx += ddx;
				ix += sx;
			} else if (ty <= tz) {
				t = ty;
				ty += ddy;
				iy += sy;
			} else {
				t = tz;
				tz += ddz;
				iz += sz;
			}
		}
		return true;
	}

	/** @brief How far along a unit direction the first solid cell is (glass does not count), or -1 if none within `length`. */
	float FirstSolid(V3 from, V3 direction, float length) const
	{
		constexpr float Step = Size * 0.5F;
		for (float t = 0; t <= length; t += Step) {
			const int ix = static_cast<int>(std::floor((from.x + direction.x * t - x0) / Size));
			const int iy = static_cast<int>(std::floor((from.y + direction.y * t - y0) / Size));
			const int iz = static_cast<int>(std::floor((from.z + direction.z * t) / Size));
			if (ix >= 0 && iy >= 0 && iz >= 0 && ix < nx && iy < ny && iz < nz && cells[(static_cast<size_t>(iz) * ny + iy) * nx + ix] == 1)
				return t;
		}
		return -1.0F;
	}

	/**
	 * @brief How much light gets from `from` to `to` (1, the glass's share, or 0), leaving out the last `skip` of
	 * the way: the surface the light falls on is not in its own way.
	 */
	float Transmission(V3 from, V3 to, float skip) const
	{
		const float dx = to.x - from.x;
		const float dy = to.y - from.y;
		const float dz = to.z - from.z;
		const float length = std::sqrt(dx * dx + dy * dy + dz * dz);
		if (length <= skip)
			return 1.0F;
		const float end = (length - skip) / length; // in t along from .. to
		float px = (from.x - x0) / Size;
		float py = (from.y - y0) / Size;
		float pz = from.z / Size;
		int ix = static_cast<int>(std::floor(px));
		int iy = static_cast<int>(std::floor(py));
		int iz = static_cast<int>(std::floor(pz));
		const int sx = dx > 0 ? 1 : -1;
		const int sy = dy > 0 ? 1 : -1;
		const int sz = dz > 0 ? 1 : -1;
		constexpr float Far = std::numeric_limits<float>::max();
		const float ddx = dx != 0 ? std::abs(Size / dx) : Far; // t per cell along each axis
		const float ddy = dy != 0 ? std::abs(Size / dy) : Far;
		const float ddz = dz != 0 ? std::abs(Size / dz) : Far;
		float tx = dx != 0 ? ((dx > 0 ? static_cast<float>(ix + 1) - px : px - static_cast<float>(ix)) * ddx) : Far;
		float ty = dy != 0 ? ((dy > 0 ? static_cast<float>(iy + 1) - py : py - static_cast<float>(iy)) * ddy) : Far;
		float tz = dz != 0 ? ((dz > 0 ? static_cast<float>(iz + 1) - pz : pz - static_cast<float>(iz)) * ddz) : Far;
		float through = 1.0F;
		uint8_t inGlass = 0; // the glass the ray is in (2 the church's, 3 a window's), counted once
		float t = 0;
		while (t < end) {
			if (ix >= 0 && iy >= 0 && iz >= 0 && ix < nx && iy < ny && iz < nz) {
				const uint8_t cell = cells[(static_cast<size_t>(iz) * ny + iy) * nx + ix];
				if (cell == 1)
					return 0.0F;
				if (cell >= 2 && cell != inGlass)
					through *= cell == 2 ? GlassTransmission : WindowGlassTransmission;
				inGlass = cell;
			} else {
				inGlass = 0;
			}
			if (tx <= ty && tx <= tz) {
				t = tx;
				tx += ddx;
				ix += sx;
			} else if (ty <= tz) {
				t = ty;
				ty += ddy;
				iy += sy;
			} else {
				t = tz;
				tz += ddz;
				iz += sz;
			}
		}
		return through;
	}
};

/**
 * @brief Drops the surfaces of a landmark's model no one can see from outside: the insides of its walls and roof,
 * its floors, beams and stairs behind closed doors and glass. A surface stays if, from a few points just off its
 * front, a ray in one of a few dozen directions gets out of the building without meeting anything (through an open
 * door, past the eaves). Its lit rooms' insides (MeshInterior) and the reveals around its windows' glass stay.
 */
void CullHiddenSurfaces(Building &b)
{
	const auto start = std::chrono::steady_clock::now();
	float minX = std::numeric_limits<float>::max();
	float minY = minX;
	float maxX = std::numeric_limits<float>::lowest();
	float maxY = maxX;
	float maxZ = 0;
	for (const WorldTriangle &tri : b.mesh)
		for (const V3 &p : tri.p) {
			minX = std::min(minX, p.x);
			minY = std::min(minY, p.y);
			maxX = std::max(maxX, p.x);
			maxY = std::max(maxY, p.y);
			maxZ = std::max(maxZ, p.z);
		}
	Occluders grid;
	grid.x0 = minX - 0.5F;
	grid.y0 = minY - 0.5F;
	grid.nx = static_cast<int>((maxX - minX + 1.0F) / Occluders::Size) + 1;
	grid.ny = static_cast<int>((maxY - minY + 1.0F) / Occluders::Size) + 1;
	grid.nz = static_cast<int>((maxZ + 0.5F) / Occluders::Size) + 1;
	grid.cells.assign(static_cast<size_t>(grid.nx) * grid.ny * grid.nz, 0);
	for (const WorldTriangle &tri : b.mesh)
		if (tri.mapping != MeshInterior)
			grid.Mark(tri, 1);
	const float reach = std::hypot(maxX - minX + 1.0F, maxY - minY + 1.0F, maxZ + 1.0F);
	// Directions spread over the sphere (a Fibonacci lattice).
	std::vector<V3> directions;
	constexpr int Count = 96;
	for (int k = 0; k < Count; k++) {
		const float z = 1.0F - 2.0F * (static_cast<float>(k) + 0.5F) / Count;
		const float r = std::sqrt(std::max(0.0F, 1.0F - z * z));
		const float a = static_cast<float>(k) * 2.39996323F;
		directions.push_back({ r * std::cos(a), r * std::sin(a), z });
	}
	// The reveals of glazed windows show between the glass and the wall's face, a gap finer than the grid: what is
	// around a pane stays.
	std::vector<std::array<float, 6>> panes;
	for (const WorldTriangle &tri : b.mesh) {
		if (tri.mapping != MeshPane)
			continue;
		std::array<float, 6> box { std::numeric_limits<float>::max(), std::numeric_limits<float>::lowest(), std::numeric_limits<float>::max(), std::numeric_limits<float>::lowest(),
			std::numeric_limits<float>::max(), std::numeric_limits<float>::lowest() };
		for (const V3 &p : tri.p) {
			box = { std::min(box[0], p.x - 0.35F), std::max(box[1], p.x + 0.35F), std::min(box[2], p.y - 0.35F), std::max(box[3], p.y + 0.35F), std::min(box[4], p.z - 0.35F),
				std::max(box[5], p.z + 0.35F) };
		}
		panes.push_back(box);
	}
	// What is in sight from where one stands around it: from points round it at eye height, rays over all of it, a
	// tenth of a tile apart where they reach it; the first cell each meets is in sight. A surface in such a
	// cell stays whatever its own rays found: the insides seen through small holes (in a roof's thatch), which those
	// few directions can miss.
	std::unique_ptr<std::atomic<uint8_t>[]> inSight(new std::atomic<uint8_t>[grid.cells.size()]);
	for (size_t i = 0; i < grid.cells.size(); i++)
		inSight[i].store(0, std::memory_order_relaxed);
	constexpr float StandingEye = 1.3F; // the first-person eye's height (EyeHeight; town walls are not stretched)
	std::vector<V3> standing;
	for (const float off : { 0.5F, 1.5F, 4.0F }) {
		const float ax = minX - off;
		const float ay = minY - off;
		const float bx = maxX + off;
		const float by = maxY + off;
		const float perimeter = 2.0F * (bx - ax + by - ay);
		constexpr float Apart = 1.0F;
		for (float s = 0; s < perimeter; s += Apart) {
			float u = s;
			Vec2 at;
			if (u < bx - ax) {
				at = { ax + u, ay };
			} else if ((u -= bx - ax) < by - ay) {
				at = { bx, ay + u };
			} else if ((u -= by - ay) < bx - ax) {
				at = { bx - u, by };
			} else {
				at = { ax, by - (u - (bx - ax)) };
			}
			standing.push_back({ at.x, at.y, StandingEye });
		}
	}
	const V3 middle { (minX + maxX) / 2.0F, (minY + maxY) / 2.0F, maxZ / 2.0F };
	std::atomic<size_t> nextView { 0 };
	auto look = [&]() {
		for (size_t v = nextView++; v < standing.size(); v = nextView++) {
			const V3 eye = standing[v];
			V3 f { middle.x - eye.x, middle.y - eye.y, middle.z - eye.z };
			const float distance = std::sqrt(f.x * f.x + f.y * f.y + f.z * f.z);
			f = { f.x / distance, f.y / distance, f.z / distance };
			V3 r { f.y, -f.x, 0 };
			const float rl = std::sqrt(r.x * r.x + r.y * r.y);
			r = { r.x / rl, r.y / rl, 0 };
			const V3 u { r.y * f.z - r.z * f.y, r.z * f.x - r.x * f.z, r.x * f.y - r.y * f.x };
			// The building's bounds as seen from here.
			float a0 = std::numeric_limits<float>::max();
			float a1 = std::numeric_limits<float>::lowest();
			float e0 = a0;
			float e1 = a1;
			for (int corner = 0; corner < 8; corner++) {
				const V3 k { ((corner & 1) != 0 ? maxX : minX) - eye.x, ((corner & 2) != 0 ? maxY : minY) - eye.y, ((corner & 4) != 0 ? maxZ : 0.0F) - eye.z };
				const float kf = k.x * f.x + k.y * f.y + k.z * f.z;
				const float kr = k.x * r.x + k.y * r.y + k.z * r.z;
				const float ku = k.x * u.x + k.y * u.y + k.z * u.z;
				const float a = std::atan2(kr, kf);
				const float e = std::atan2(ku, std::hypot(kf, kr));
				a0 = std::min(a0, a);
				a1 = std::max(a1, a);
				e0 = std::min(e0, e);
				e1 = std::max(e1, e);
			}
			const float step = std::clamp(0.1F / distance, 0.006F, 0.05F);
			for (float e = e0; e <= e1; e += step) {
				for (float a = a0; a <= a1; a += step) {
					const float ce = std::cos(e);
					const V3 d { f.x * ce * std::cos(a) + r.x * ce * std::sin(a) + u.x * std::sin(e), f.y * ce * std::cos(a) + r.y * ce * std::sin(a) + u.y * std::sin(e),
						f.z * ce * std::cos(a) + r.z * ce * std::sin(a) + u.z * std::sin(e) };
					const ptrdiff_t cell = grid.FirstMet(eye, d);
					if (cell >= 0)
						inSight[static_cast<size_t>(cell)].store(1, std::memory_order_relaxed);
				}
			}
		}
	};
	{
		std::vector<std::thread> lookers;
		const unsigned count = std::clamp(std::thread::hardware_concurrency(), 1U, 16U);
		for (unsigned t = 0; t < count; t++)
			lookers.emplace_back(look);
		for (std::thread &thread : lookers)
			thread.join();
	}
	std::vector<uint8_t> keep(b.mesh.size(), 1);
	std::atomic<size_t> next { 0 };
	auto work = [&]() {
		for (size_t i = next++; i < b.mesh.size(); i = next++) {
			const WorldTriangle &tri = b.mesh[i];
			if (tri.mapping == MeshInterior || tri.mapping == MeshPane || tri.mapping == MeshFrame)
				continue;
			bool nearPane = false;
			for (const std::array<float, 6> &box : panes) {
				bool inside = true;
				for (const V3 &p : tri.p)
					inside = inside && p.x >= box[0] && p.x <= box[1] && p.y >= box[2] && p.y <= box[3] && p.z >= box[4] && p.z <= box[5];
				nearPane = nearPane || inside;
			}
			if (nearPane)
				continue;
			const V3 e1 { tri.p[1].x - tri.p[0].x, tri.p[1].y - tri.p[0].y, tri.p[1].z - tri.p[0].z };
			const V3 e2 { tri.p[2].x - tri.p[0].x, tri.p[2].y - tri.p[0].y, tri.p[2].z - tri.p[0].z };
			V3 n { e1.y * e2.z - e1.z * e2.y, e1.z * e2.x - e1.x * e2.z, e1.x * e2.y - e1.y * e2.x };
			const float length = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
			if (length < 1e-9F)
				continue;
			n = { n.x / length, n.y / length, n.z / length };
			if (std::abs(n.z) < 0.5F && std::min({ tri.p[0].z, tri.p[1].z, tri.p[2].z }) < 0.2F)
				continue; // a wall's foot: the ground beside it is in the grid's lowest cells
			// Its front: the side it is drawn from (both for two-sided surfaces).
			const float front = tri.normal.x * n.x + tri.normal.y * n.y + tri.normal.z * n.z;
			std::array<float, 2> sides { front >= 0 ? 1.0F : -1.0F, 0.0F };
			if (!tri.cull)
				sides[1] = -sides[0];
			const V3 c { (tri.p[0].x + tri.p[1].x + tri.p[2].x) / 3.0F, (tri.p[0].y + tri.p[1].y + tri.p[2].y) / 3.0F, (tri.p[0].z + tri.p[1].z + tri.p[2].z) / 3.0F };
			const std::array<V3, 4> points { c, V3 { (c.x + tri.p[0].x) / 2.0F, (c.y + tri.p[0].y) / 2.0F, (c.z + tri.p[0].z) / 2.0F },
				V3 { (c.x + tri.p[1].x) / 2.0F, (c.y + tri.p[1].y) / 2.0F, (c.z + tri.p[1].z) / 2.0F }, V3 { (c.x + tri.p[2].x) / 2.0F, (c.y + tri.p[2].y) / 2.0F, (c.z + tri.p[2].z) / 2.0F } };
			bool seen = false;
			for (const float side : sides) {
				if (side == 0 || seen)
					continue;
				const V3 m { n.x * side, n.y * side, n.z * side };
				for (const float off : { 0.1F, 0.22F }) { // farther off too: past a neighbour's cells
					for (const V3 &p : points) {
						const V3 from { p.x + m.x * off, p.y + m.y * off, p.z + m.z * off };
						for (const V3 &d : directions) {
							if (d.x * m.x + d.y * m.y + d.z * m.z < 0.05F)
								continue;
							if (grid.Escapes(from, d, reach)) {
								seen = true;
								break;
							}
						}
						if (seen)
							break;
					}
					if (seen)
						break;
				}
			}
			if (!seen && !grid.AnyCell(tri, [&](size_t cell) { return inSight[cell].load(std::memory_order_relaxed) != 0; }))
				keep[i] = 0;
		}
	};
	std::vector<std::thread> threads;
	const unsigned count = std::clamp(std::thread::hardware_concurrency(), 1U, 16U);
	for (unsigned t = 0; t < count; t++)
		threads.emplace_back(work);
	for (std::thread &thread : threads)
		thread.join();
	const size_t before = b.mesh.size();
	std::vector<WorldTriangle> kept;
	kept.reserve(before);
	for (size_t i = 0; i < before; i++)
		if (keep[i] != 0)
			kept.push_back(b.mesh[i]);
	b.mesh = std::move(kept);
	const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
	LogInfo("fp: {}: {} of {} surfaces hidden inside, dropped ({:.0f} ms)", Landmarks[b.landmark].name, before - b.mesh.size(), before, ms);
}

/** A window's own light: its strength per square tile of opening, and the distance at which it is down to half. */
constexpr float WindowLampStrength = 1.2F;
constexpr float WindowLampHalf = 1.0F;
constexpr float WindowLampReach = 3.5F;

/**
 * @brief The openings in the walls around a lit room (windows, doorways, the smithy's open front), found by looking
 * out from the room's sides through what blocks light, and a light of each just outside it, shining out: the glow
 * of the room's light in and around it.
 */
void AddWindowLamps(const LitRoom &room, const Occluders &occluders, std::vector<Lamp> &lamps)
{
	constexpr float Step = Occluders::Size;
	constexpr float Reach = 1.5F;
	int count = 0;
	for (int side = 0; side < 4; side++) {
		const int axis = side / 2; // 0: a side facing x, 1: facing y
		const float sign = side % 2 == 0 ? -1.0F : 1.0F;
		const V3 out = axis == 0 ? V3 { sign, 0, 0 } : V3 { 0, sign, 0 };
		const float at = axis == 0 ? (sign < 0 ? room.x0 : room.x1) : (sign < 0 ? room.y0 : room.y1);
		const float alongLo = axis == 0 ? room.y0 : room.x0;
		const float alongHi = axis == 0 ? room.y1 : room.x1;
		const int na = static_cast<int>((alongHi - alongLo) / Step);
		const int nz = static_cast<int>((room.top - room.z0) / Step);
		if (na <= 0 || nz <= 0)
			continue;
		std::vector<float> wall(static_cast<size_t>(na) * nz, -1.0F); // distance out to the wall; -1 open
		std::vector<uint8_t> open(wall.size(), 0);
		for (int k = 0; k < nz; k++) {
			for (int i = 0; i < na; i++) {
				const float along = alongLo + (static_cast<float>(i) + 0.5F) * Step;
				const float z = room.z0 + (static_cast<float>(k) + 0.5F) * Step;
				const float x = axis == 0 ? at : along;
				const float y = axis == 0 ? along : at;
				if (z > RoomCeiling(room, x, y) - 0.05F)
					continue;
				const V3 from { x - out.x * 0.05F, y - out.y * 0.05F, z };
				const float hit = occluders.FirstSolid(from, out, Reach);
				if (hit < 0)
					open[static_cast<size_t>(k) * na + i] = 1;
				else
					wall[static_cast<size_t>(k) * na + i] = hit;
			}
		}
		std::vector<float> walls;
		for (const float d : wall)
			if (d >= 0)
				walls.push_back(d);
		if (walls.empty())
			continue;
		std::nth_element(walls.begin(), walls.begin() + static_cast<ptrdiff_t>(walls.size() / 2), walls.end());
		const float wallOut = walls[walls.size() / 2] - 0.05F; // from the room's side to the wall's outer face, about
		std::vector<uint8_t> seen(open.size(), 0);
		for (size_t start = 0; start < open.size(); start++) {
			if (open[start] == 0 || seen[start] != 0)
				continue;
			std::vector<size_t> stack { start };
			seen[start] = 1;
			float sumA = 0;
			float sumZ = 0;
			int cells = 0;
			while (!stack.empty()) {
				const size_t n = stack.back();
				stack.pop_back();
				const int i = static_cast<int>(n % na);
				const int k = static_cast<int>(n / na);
				sumA += alongLo + (static_cast<float>(i) + 0.5F) * Step;
				sumZ += room.z0 + (static_cast<float>(k) + 0.5F) * Step;
				cells++;
				for (const auto &[di, dk] : { std::pair { 1, 0 }, std::pair { -1, 0 }, std::pair { 0, 1 }, std::pair { 0, -1 } }) {
					const int ni = i + di;
					const int nk = k + dk;
					if (ni < 0 || nk < 0 || ni >= na || nk >= nz)
						continue;
					const size_t m = static_cast<size_t>(nk) * na + ni;
					if (open[m] != 0 && seen[m] == 0) {
						seen[m] = 1;
						stack.push_back(m);
					}
				}
			}
			if (cells < 4)
				continue; // a chink
			const float area = static_cast<float>(cells) * Step * Step;
			const float along = sumA / static_cast<float>(cells);
			const float z = sumZ / static_cast<float>(cells);
			const float outAt = at + (axis == 0 ? out.x : out.y) * (wallOut + 0.12F);
			const V3 position = axis == 0 ? V3 { outAt, along, z } : V3 { along, outAt, z };
			lamps.push_back({ position, WindowLampStrength * std::min(area, 1.5F) * LitLandmarks[room.lit].strength, room.lit, -1, out, WindowLampHalf, WindowLampReach });
			count++;
		}
	}
	LogInfo("fp: {}: room at {:.1f}, {:.1f}: {} openings", LitLandmarks[room.lit].name, (room.x0 + room.x1) / 2.0F, (room.y0 + room.y1) / 2.0F, count);
}

/**
 * @brief Works out, for each lamp, how much of it can be seen from the points of a grid around it: from inside
 * its room, all of it; outside, through the openings in the walls only. The flame is a small ball: several points of
 * it are tried, so the light's edges on the ground are soft.
 */
void BuildLampGrids()
{
	LightGrids.clear();
	if (Lamps.empty())
		return;
	const auto start = std::chrono::steady_clock::now();
	static const std::array<V3, 5> Flame { { { 0, 0, 0 }, { LampRadius, 0, LampRadius * 0.5F }, { -LampRadius, 0, -LampRadius * 0.5F }, { 0, LampRadius, -LampRadius * 0.5F }, { 0, -LampRadius, LampRadius * 0.5F } } };
	for (size_t lit = 0; lit < LitLandmarks.size(); lit++) {
		if (LightBuilder.stop)
			return;
		std::vector<const Lamp *> lamps;
		float minX = std::numeric_limits<float>::max();
		float minY = minX;
		float maxX = std::numeric_limits<float>::lowest();
		float maxY = maxX;
		float maxZ = 0;
		for (const Lamp &lamp : Lamps) {
			if (lamp.lit != static_cast<int>(lit))
				continue;
			lamps.push_back(&lamp);
			minX = std::min(minX, lamp.position.x);
			minY = std::min(minY, lamp.position.y);
			maxX = std::max(maxX, lamp.position.x);
			maxY = std::max(maxY, lamp.position.y);
			maxZ = std::max(maxZ, lamp.position.z);
		}
		if (lamps.empty())
			continue;
		// What is in the way of the light: the buildings and prop hulls around.
		Occluders occluders;
		occluders.x0 = minX - LampReach - 0.5F;
		occluders.y0 = minY - LampReach - 0.5F;
		occluders.nx = static_cast<int>((maxX - minX + 2.0F * LampReach + 1.0F) / Occluders::Size) + 1;
		occluders.ny = static_cast<int>((maxY - minY + 2.0F * LampReach + 1.0F) / Occluders::Size) + 1;
		occluders.nz = static_cast<int>((maxZ + 1.5F) / Occluders::Size) + 1;
		occluders.cells.assign(static_cast<size_t>(occluders.nx) * occluders.ny * occluders.nz, 0);
		for (const Building &b : Buildings)
			occluders.Add(b.mesh, b.x0 - b.boundsOverhang - 1.0F, b.y0 - b.boundsOverhang - 1.0F, b.x1 + b.boundsOverhang + 1.0F, b.y1 + b.boundsOverhang + 1.0F, true);
		for (const TownProp &prop : TownProps)
			occluders.Add(prop.art.mesh, prop.art.x0, prop.art.y0, prop.art.x1, prop.art.y1, false);
		// The windows' own lights, shining out of them.
		std::vector<Lamp> windowLamps;
		for (const LitRoom &room : LitRooms)
			if (room.lit == static_cast<int>(lit) && !LightBuilder.stop)
				AddWindowLamps(room, occluders, windowLamps);
		for (const Lamp &lamp : windowLamps)
			lamps.push_back(&lamp);
		LightGrid grid;
		grid.lit = static_cast<int>(lit);
		grid.x0 = minX - LampReach;
		grid.y0 = minY - LampReach;
		grid.nx = static_cast<int>((maxX - minX + 2.0F * LampReach) / LampCell) + 2;
		grid.ny = static_cast<int>((maxY - minY + 2.0F * LampReach) / LampCell) + 2;
		// Up to a little over the eaves: higher up the light falls on nothing but the lit building itself.
		grid.nz = static_cast<int>((std::min(maxZ, LightTop) + 1.0F) / LampLayer) + 2;
		grid.light.assign(static_cast<size_t>(grid.nx) * grid.ny * grid.nz, { 0, 0, 0 });
		// Rows of the grid, spread over the threads.
		std::atomic<int> next { 0 };
		auto work = [&]() {
			for (int row = next++; row < grid.ny * grid.nz && !LightBuilder.stop; row = next++) {
				const int j = row % grid.ny;
				const int k = row / grid.ny;
				for (int i = 0; i < grid.nx; i++) {
					const V3 p { grid.x0 + static_cast<float>(i) * LampCell, grid.y0 + static_cast<float>(j) * LampCell, static_cast<float>(k) * LampLayer };
					if (InsideLitRoom(p.x, p.y, p.z))
						continue;
					V3 sum { 0, 0, 0 };
					for (const Lamp *lamp : lamps) {
						const V3 d { lamp->position.x - p.x, lamp->position.y - p.y, lamp->position.z - p.z };
						const float d2 = d.x * d.x + d.y * d.y + d.z * d.z;
						if (d2 >= lamp->reach * lamp->reach)
							continue;
						float seen = 0;
						for (const V3 &f : Flame)
							seen += occluders.Transmission({ lamp->position.x + f.x, lamp->position.y + f.y, lamp->position.z + f.z }, p, 0.1F);
						if (seen <= 0)
							continue;
						const float distance = std::sqrt(d2);
						// A window's light: out of it only.
						const float out = lamp->facing.x == 0 && lamp->facing.y == 0 && lamp->facing.z == 0 ? 1.0F : -(d.x * lamp->facing.x + d.y * lamp->facing.y + d.z * lamp->facing.z) / distance;
						if (out <= 0)
							continue;
						const float fade = 1.0F - d2 / (lamp->reach * lamp->reach);
						const float light = out * LampPower * lamp->strength * seen / static_cast<float>(Flame.size()) * fade * fade / (1.0F + d2 / (lamp->half * lamp->half)) / distance;
						sum = { sum.x + d.x * light, sum.y + d.y * light, sum.z + d.z * light };
					}
					auto unit = [](float v) { return static_cast<int16_t>(std::clamp(v * LightUnit, -32767.0F, 32767.0F)); };
					grid.light[(static_cast<size_t>(k) * grid.ny + j) * grid.nx + i] = { unit(sum.x), unit(sum.y), unit(sum.z) };
				}
			}
		};
		std::vector<std::thread> threads;
		const unsigned count = std::clamp(std::thread::hardware_concurrency(), 1U, 16U);
		for (unsigned t = 0; t < count; t++)
			threads.emplace_back(work);
		for (std::thread &thread : threads)
			thread.join();
		grid.lit2d.assign(static_cast<size_t>(grid.nx) * grid.ny, 0);
		for (int k = 0; k < grid.nz; k++)
			for (int j = 0; j < grid.ny; j++)
				for (int i = 0; i < grid.nx; i++) {
					const auto &c = grid.light[(static_cast<size_t>(k) * grid.ny + j) * grid.nx + i];
					if ((c[0] | c[1] | c[2]) == 0)
						continue;
					for (int dj = -1; dj <= 0; dj++)
						for (int di = -1; di <= 0; di++)
							if (i + di >= 0 && j + dj >= 0)
								grid.lit2d[static_cast<size_t>(j + dj) * grid.nx + i + di] = 1;
				}
		LightGrids.push_back(std::move(grid));
	}
	const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
	LogInfo("fp: {} lamps lit their surroundings in {:.0f} ms", Lamps.size(), ms);
}

/** Props farther than this (tiles) are lost in the fog. */
constexpr float PropDrawRange = 48.0F;

void DrawTownProps(const Surface &out, const Projection &proj, std::vector<float> &depthBuffer)
{
	static const bool noHulls = std::getenv("FP_NO_HULLS") != nullptr; // debug: time them
	if (noHulls)
		return;
	// Near to far, so the depth test turns hidden pixels away before they are shaded.
	static std::vector<std::pair<float, const TownProp *>> order;
	order.clear();
	for (const TownProp &prop : TownProps) {
		const float distance = std::hypot(prop.middle.x - proj.pos.x, prop.middle.y - proj.pos.y) - prop.radius;
		if (distance <= PropDrawRange)
			order.emplace_back(distance, &prop);
	}
	std::sort(order.begin(), order.end(), [](const auto &a, const auto &c) { return a.first < c.first; });
	for (const auto &[distance, propPointer] : order) {
		const TownProp &prop = *propPointer;
		const bool far = distance > PropFarDistance && !prop.farMesh.empty();
		int left;
		int top;
		int right;
		int bottom;
		if (!ScreenBounds(prop.art, proj, left, top, right, bottom))
			continue;
		const PieceTextures *columnFor = nullptr;
		Point columnTile { -1, -1 };
		const std::vector<WorldTriangle> &mesh = far ? prop.farMesh : prop.art.mesh;
		// Big ones (heaps of boulders, the caves' mound) front to back too, so hidden pixels are turned away unshaded.
		static std::vector<uint32_t> sorted;
		const std::vector<uint32_t> *order = nullptr;
		if (mesh.size() > 600) {
			static std::vector<std::pair<float, uint32_t>> byDistance;
			byDistance.clear();
			for (uint32_t i = 0; i < mesh.size(); i++) {
				const WorldTriangle &tri = mesh[i];
				const float dx = (tri.p[0].x + tri.p[1].x + tri.p[2].x) / 3.0F - proj.pos.x;
				const float dy = (tri.p[0].y + tri.p[1].y + tri.p[2].y) / 3.0F - proj.pos.y;
				byDistance.emplace_back(dx * dx + dy * dy, i);
			}
			std::sort(byDistance.begin(), byDistance.end());
			sorted.clear();
			for (const auto &[d, i] : byDistance)
				sorted.push_back(i);
			order = &sorted;
		}
		RasterizeTriangles(out, proj, depthBuffer, mesh, [&](const WorldTriangle &tri, float wx, float wy, float wz) {
			float x = wx;
			float y = wy;
			if (tri.mirrored) {
				const float d = (x - tri.mirrorX) * tri.normalX + (y - tri.mirrorY) * tri.normalY;
				x -= 2.0F * d * tri.normalX;
				y -= 2.0F * d * tri.normalY;
			}
			const Point lit { static_cast<int>(std::floor(wx)), static_cast<int>(std::floor(wy)) };
			const uint8_t level = InDungeonBounds(lit) ? std::min<uint8_t>(LightAt(lit), LightsMax) : LightsMax;
			if (tri.mapping == MeshSolid)
				return static_cast<int>(FpLightTable(level)[ShadeColor(tri.fallback, tri.darkness)]);
			if (tri.mapping == MeshTiled && tri.texture >= 0 && static_cast<size_t>(tri.texture) < MeshTextures.size()) {
				// The stone tiled over the wall: along it and up its sides, across its top.
				constexpr float Repeat = 0.7F; // texture repeats per tile
				float u;
				float v;
				if (std::abs(tri.normal.z) > 0.6F) {
					u = wx * Repeat;
					v = wy * Repeat;
				} else if (std::abs(tri.normal.x) > std::abs(tri.normal.y)) {
					u = wy * Repeat;
					v = wz * Repeat;
				} else {
					u = wx * Repeat;
					v = wz * Repeat;
				}
				const MeshTexture &tex = MeshTextures[tri.texture];
				const float distance = std::hypot(wx - proj.pos.x, wy - proj.pos.y, wz - EyeHeight);
				const float footprint = Repeat * static_cast<float>(tex.width) * distance / proj.focal;
				return static_cast<int>(FpLightTable(level)[WallStoneColor(MeshTexel(tex, u, v, footprint), tri.darkness + WallStoneDarkness)]);
			}
			if (prop.ownCanvas) {
				const int16_t art = CanvasArtAt(prop.art, wx, wy, wz);
				return static_cast<int>(FpLightTable(level)[ShadeColor(art > 0 ? PatchColor(art) : tri.fallback, tri.darkness)]);
			}
			const Point tile { tri.artX, tri.artY };
			if (tile != columnTile) {
				columnTile = tile;
				columnFor = &GetPieceTextures(dPiece[tile.x][tile.y]);
			}
			int16_t art = ColumnArtAt(*columnFor, tile, x, y, wz);
			if (art <= 0 && tri.mapping != MeshProjectiveCut) {
				// Just off the art's edge: the nearest art pixel above (the hull's cells overreach a little).
				for (int k = 1; k <= 3 && art <= 0; k++)
					art = ColumnArtAt(*columnFor, tile, x, y, wz + static_cast<float>(k) / HeightPixels);
			}
			if (art <= 0) {
				if (tri.mapping == MeshProjectiveCut)
					return -1;
				art = tri.fallback;
			}
			return static_cast<int>(FpLightTable(level)[ShadeColor(PatchColor(art), tri.darkness)]);
		}, 1.0F, order);
	}
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

bool DumpIsoRegion(int x0, int y0, int x1, int y1, const std::string &path, bool uprightOnly)
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
	if (uprightOnly) {
		// Leave the ground out: redraw each tile's floor part over the canvas as transparent, nearest last as in
		// BuildCanvas, wherever the floor is what the canvas shows.
		const int rows = static_cast<int>(GetPieceTextures(dPiece[tiles[0].x][tiles[0].y]).column.size() / ColumnWidth);
		for (const Point t : tiles) {
			const std::vector<int16_t> floor = RenderPieceFloor(dPiece[t.x][t.y]);
			const int left = (t.x - t.y) * 32 - 32 - region.originX;
			const int top = (t.x + t.y + 2) * 16 - (rows - 1) - region.originY + (rows - 32);
			for (int cy = 0; cy < 32; cy++) {
				for (int cx = 0; cx < ColumnWidth; cx++) {
					const int16_t p = floor[static_cast<size_t>(cy) * ColumnWidth + cx];
					if (p <= 0)
						continue;
					int16_t &c = region.canvas[static_cast<size_t>(top + cy) * region.canvasWidth + left + cx];
					if (c == p)
						c = -1;
				}
			}
		}
	}
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

void StopTownBuild()
{
	LightBuilder.Join();
	BuiltFor = { nullptr, -1, -1, false };
}

void SetModelTexturesEnabled(bool enabled)
{
	ModelTexturesOn = enabled;
}

void SetHouseMeshesEnabled(bool enabled)
{
	HouseMeshesOn = enabled;
}

const uint8_t *BuildingTileMap()
{
	if (leveltype != DTYPE_TOWN || !BuildingsEnabled())
		return nullptr;
	EnsureBuilt();
	return BuildingTiles.data();
}

const uint16_t *FloorSourceMap()
{
	if (leveltype != DTYPE_TOWN || !BuildingsEnabled())
		return nullptr;
	EnsureBuilt();
	return FloorSources.empty() ? nullptr : FloorSources.data();
}

bool IsBuildingTile(Point tile)
{
	EnsureBuilt();
	return InDungeonBounds(tile) && BuildingTiles[static_cast<size_t>(tile.y) * MAXDUNX + tile.x] != 0;
}

void DrawBuildings(const Surface &out, const Projection &proj, const std::vector<float> &zBuffer, std::vector<float> &depthBuffer)
{
	EnsureBuilt();
	UpdateWindowFlicker();
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
	DrawTownProps(out, proj, depthBuffer);
}

void ApplyWindowGlow(const Surface &out, const Projection &proj, const std::vector<float> &depthBuffer)
{
	if (leveltype != DTYPE_TOWN || !BuildingsEnabled() || !WindowLightOn)
		return;
	EnsureBuilt();
	if (!LightReady || LightGrids.empty())
		return;
	// The screen rectangle the grids can show in.
	int left = proj.width;
	int right = -1;
	int top = proj.height;
	int bottom = -1;
	for (const LightGrid &grid : LightGrids) {
		Building box {};
		box.x0 = grid.x0;
		box.y0 = grid.y0;
		box.x1 = grid.x0 + static_cast<float>(grid.nx - 1) * LampCell;
		box.y1 = grid.y0 + static_cast<float>(grid.ny - 1) * LampCell;
		box.lowestZ = 0;
		box.highestZ = static_cast<float>(grid.nz - 1) * LampLayer;
		int l;
		int t;
		int r;
		int b;
		if (!ScreenBounds(box, proj, l, t, r, b))
			continue;
		left = std::min(left, l);
		right = std::max(right, r);
		top = std::min(top, t);
		bottom = std::max(bottom, b);
	}
	left = std::max(left, 0);
	top = std::max(top, 0);
	right = std::min(right, proj.width - 1);
	bottom = std::min(bottom, proj.height - 1);
	if (left > right || top > bottom)
		return;
	std::array<std::array<const uint8_t *, GlowLevels>, LitLandmarks.size()> tables;
	for (size_t f = 0; f < LitLandmarks.size(); f++)
		for (int l = 0; l < GlowLevels; l++)
			tables[f][l] = GlowColors(static_cast<int>(f), l).data();
	std::array<float, LitLandmarks.size()> flicker;
	for (size_t i = 0; i < flicker.size(); i++)
		flicker[i] = (1.0F + LampFlicker * FlickerNow[i]) / LightUnit;
	static std::vector<Vec2> rays;
	rays.resize(static_cast<size_t>(proj.width));
	for (int x = 0; x < proj.width; x++)
		rays[x] = proj.columnRay(x);
	auto depthAt = [&](int x, int y) { return depthBuffer[static_cast<size_t>(y) * proj.width + x]; };
	auto worldAt = [&](int x, int y, float depth) {
		return V3 { proj.pos.x + rays[x].x * depth, proj.pos.y + rays[x].y * depth, EyeHeight + proj.rowSlope(y) * depth };
	};
	static const bool debugSeen = std::getenv("FP_LAMP_SEEN") != nullptr; // debug: the lamps' light, whichever way surfaces face
	// The light on the surface seen at a pixel.
	int litAt = 0; // the building whose light is the most at the last point lightAt worked out
	auto lightAt = [&](int x, int y, float depth) {
		const V3 p = worldAt(x, y, depth);
		const float pz = std::max(p.z, 0.0F);
		float light = 0;
		for (const LightGrid &grid : LightGrids) {
			const float fx = (p.x - grid.x0) * (1.0F / LampCell);
			const float fy = (p.y - grid.y0) * (1.0F / LampCell);
			const float fz = pz * (1.0F / LampLayer);
			if (!(fx >= 0 && fy >= 0 && fx < static_cast<float>(grid.nx - 1) && fy < static_cast<float>(grid.ny - 1) && fz < static_cast<float>(grid.nz - 1)))
				continue;
			const int i = static_cast<int>(fx);
			const int j = static_cast<int>(fy);
			if (grid.lit2d[static_cast<size_t>(j) * grid.nx + i] == 0)
				continue;
			const int k = static_cast<int>(fz);
			const size_t row = static_cast<size_t>(grid.nx);
			const size_t layer = row * grid.ny;
			const std::array<int16_t, 3> *cell = &grid.light[(static_cast<size_t>(k) * grid.ny + j) * grid.nx + i];
			const std::array<const std::array<int16_t, 3> *, 8> corners { cell, cell + 1, cell + row, cell + row + 1, cell + layer, cell + layer + 1, cell + layer + row, cell + layer + row + 1 };
			bool any = false;
			for (const auto *c : corners)
				any = any || ((*c)[0] | (*c)[1] | (*c)[2]) != 0;
			if (!any)
				continue;
			const float tx = fx - static_cast<float>(i);
			const float ty = fy - static_cast<float>(j);
			const float tz = fz - static_cast<float>(k);
			const std::array<float, 8> weight { (1 - tx) * (1 - ty) * (1 - tz), tx * (1 - ty) * (1 - tz), (1 - tx) * ty * (1 - tz), tx * ty * (1 - tz), (1 - tx) * (1 - ty) * tz, tx * (1 - ty) * tz, (1 - tx) * ty * tz, tx * ty * tz };
			V3 e { 0, 0, 0 };
			for (size_t c = 0; c < corners.size(); c++)
				e = { e.x + weight[c] * (*corners[c])[0], e.y + weight[c] * (*corners[c])[1], e.z + weight[c] * (*corners[c])[2] };
			if (debugSeen) {
				light += std::sqrt(e.x * e.x + e.y * e.y + e.z * e.z) * flicker[grid.lit];
				litAt = grid.lit;
				continue;
			}
			// The surface's normal, from the neighbouring pixels' points (on the side whose depth is nearer this one's).
			V3 n { 0, 0, 1 };
			const int ax = x + 1 < proj.width && (x == 0 || std::abs(depthAt(x + 1, y) - depth) <= std::abs(depthAt(x - 1, y) - depth)) ? x + 1 : x - 1;
			const int ay = y + 1 < proj.height && (y == 0 || std::abs(depthAt(x, y + 1) - depth) <= std::abs(depthAt(x, y - 1) - depth)) ? y + 1 : y - 1;
			if (ax >= 0 && ay >= 0) {
				const float da = depthAt(ax, y);
				const float db = depthAt(x, ay);
				if (da < MaxRayDistance && db < MaxRayDistance && std::abs(da - depth) < 0.05F * depth && std::abs(db - depth) < 0.05F * depth) {
					const V3 a = worldAt(ax, y, da);
					const V3 b = worldAt(x, ay, db);
					const V3 u { a.x - p.x, a.y - p.y, a.z - p.z };
					const V3 v { b.x - p.x, b.y - p.y, b.z - p.z };
					const V3 c { u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z, u.x * v.y - u.y * v.x };
					const float length = std::sqrt(c.x * c.x + c.y * c.y + c.z * c.z);
					if (length > 1e-9F) {
						const float towards = c.x * (proj.pos.x - p.x) + c.y * (proj.pos.y - p.y) + c.z * (EyeHeight - p.z) < 0 ? -1.0F : 1.0F;
						n = { c.x * towards / length, c.y * towards / length, c.z * towards / length };
					}
				}
			}
			const float here = std::max(0.0F, n.x * e.x + n.y * e.y + n.z * e.z) * flicker[grid.lit];
			if (here > light)
				litAt = grid.lit;
			light += here;
		}
		return light;
	};
	static constexpr std::array<std::array<float, 4>, 4> Bayer { { { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 } } };
	// Brighter light saturates: the pools under the windows keep their texture instead of going flat.
	constexpr int ToneSteps = 256;
	constexpr float ToneTop = 4.0F;
	static const std::array<float, ToneSteps + 1> Tone = [] {
		std::array<float, ToneSteps + 1> tone {};
		for (int i = 0; i <= ToneSteps; i++)
			tone[i] = (1.0F - std::exp(-1.6F * ToneTop * static_cast<float>(i) / ToneSteps)) * static_cast<float>(GlowLevels - 1);
		return tone;
	}();
	auto apply = [&](int x, int y, float light, int lit) {
		if (light <= 0)
			return;
		const float level = Tone[std::min(static_cast<int>(light * (ToneSteps / ToneTop)), ToneSteps)];
		const int step = std::min(static_cast<int>(level + Bayer[y & 3][x & 3] / 16.0F), GlowLevels - 1);
		if (step > 0) {
			uint8_t *dst = out.at(x, y);
			*dst = tables[lit][step][*dst];
		}
	};
	// Worked out every Step pixels and blended in between, where the pixels between are on the same surface as
	// those around them (the light changes slowly over a surface); exactly at the edges of surfaces. Cells whose
	// corners are all dark on one surface are left alone.
	constexpr int Step = 4;
	const int x0 = left - left % Step;
	const int y0 = top - top % Step;
	const int columns = (right - x0) / Step + 2;
	const int rows = (bottom - y0) / Step + 2;
	static std::vector<float> sampleLight;
	static std::vector<float> sampleDepth;
	static std::vector<uint8_t> sampleLit;
	sampleLight.assign(static_cast<size_t>(columns) * rows, 0.0F);
	sampleLit.assign(sampleLight.size(), 0);
	sampleDepth.assign(static_cast<size_t>(columns) * rows, std::numeric_limits<float>::infinity());
	auto sampleX = [&](int c) { return std::min(x0 + c * Step, proj.width - 1); };
	auto sampleY = [&](int r) { return std::min(y0 + r * Step, proj.height - 1); };
	for (int r = 0; r < rows; r++) {
		for (int c = 0; c < columns; c++) {
			const int x = sampleX(c);
			const int y = sampleY(r);
			const float depth = depthAt(x, y);
			if (!(depth < MaxRayDistance))
				continue;
			sampleDepth[static_cast<size_t>(r) * columns + c] = depth;
			sampleLight[static_cast<size_t>(r) * columns + c] = lightAt(x, y, depth);
			sampleLit[static_cast<size_t>(r) * columns + c] = static_cast<uint8_t>(litAt);
		}
	}
	for (int r = 0; r + 1 < rows; r++) {
		const int ya = sampleY(r);
		const int yb = sampleY(r + 1);
		for (int c = 0; c + 1 < columns; c++) {
			const int xa = sampleX(c);
			const int xb = sampleX(c + 1);
			const size_t at = static_cast<size_t>(r) * columns + c;
			const std::array<float, 4> depth { sampleDepth[at], sampleDepth[at + 1], sampleDepth[at + columns], sampleDepth[at + columns + 1] };
			const std::array<float, 4> light { sampleLight[at], sampleLight[at + 1], sampleLight[at + columns], sampleLight[at + columns + 1] };
			const std::array<size_t, 4> corner { at, at + 1, at + columns, at + columns + 1 };
			const int cellLit = sampleLit[corner[std::max_element(light.begin(), light.end()) - light.begin()]];
			const float nearest = std::min({ depth[0], depth[1], depth[2], depth[3] });
			const float farthest = std::max({ depth[0], depth[1], depth[2], depth[3] });
			const bool smooth = farthest < MaxRayDistance && farthest - nearest < 0.1F * nearest;
			const bool dark = light[0] <= 0 && light[1] <= 0 && light[2] <= 0 && light[3] <= 0;
			if (smooth && dark)
				continue;
			for (int y = ya; y < (r + 2 == rows ? yb + 1 : yb) && y <= bottom; y++) {
				const float ty = yb > ya ? static_cast<float>(y - ya) / static_cast<float>(yb - ya) : 0.0F;
				for (int x = xa; x < (c + 2 == columns ? xb + 1 : xb) && x <= right; x++) {
					if (x < left || y < top)
						continue;
					const float d = depthAt(x, y);
					if (!(d < MaxRayDistance))
						continue;
					const float tx = xb > xa ? static_cast<float>(x - xa) / static_cast<float>(xb - xa) : 0.0F;
					if (smooth) {
						const float da = depth[0] + (depth[1] - depth[0]) * tx;
						const float db = depth[2] + (depth[3] - depth[2]) * tx;
						if (std::abs(da + (db - da) * ty - d) < 0.015F * d) {
							const float la = light[0] + (light[1] - light[0]) * tx;
							const float lb = light[2] + (light[3] - light[2]) * tx;
							apply(x, y, la + (lb - la) * ty, cellLit);
							continue;
						}
					}
					const float exact = lightAt(x, y, d);
					apply(x, y, exact, litAt);
				}
			}
		}
	}
}

void CollectTreeSprites(Vec2 viewerPos, std::vector<Entity> &entities)
{
	if (leveltype != DTYPE_TOWN || !BuildingsEnabled())
		return;
	EnsureBuilt();
	for (const TreeSprite &tree : TreeSprites) {
		if (std::hypot(tree.foot.x - viewerPos.x, tree.foot.y - viewerPos.y) > PropDrawRange)
			continue;
		Entity entity {};
		entity.kind = EntityKind::Prop;
		entity.id = -1;
		entity.tile = { static_cast<int>(tree.foot.x), static_cast<int>(tree.foot.y) };
		entity.pos = tree.foot;
		entity.width = static_cast<float>(tree.width) / SpritePixelsPerTileX;
		entity.height = static_cast<float>(tree.height) / SpritePixelsPerTileY;
		entity.bottom = -static_cast<float>(tree.belowFoot) / SpritePixelsPerTileY;
		entity.pixels = &tree.pixels;
		entity.pixelsWidth = tree.width;
		entity.pixelsHeight = tree.height;
		entity.lit = true;
		entity.facing = true;
		entities.push_back(entity);
	}
}

void SetWindowLightEnabled(bool enabled)
{
	WindowLightOn = enabled;
}

std::string TakeRasterStats(int frames)
{
	const std::string text = StrCat("per frame: ", RasterStats[0] / frames, " pixels in triangles, ", RasterStats[1] / frames, " pass depth, ", RasterStats[2] / frames, " triangles");
	RasterStats = {};
	return text;
}

void SetSarcophagusKitEnabled(bool enabled)
{
	SarcophagusKitOn = enabled;
	StructuresFor = LevelKey { nullptr, -1, -1, false }; // rebuild cached object boxes
}

bool IsStructureTile(Point tile)
{
	if (leveltype == DTYPE_TOWN || !InDungeonBounds(tile))
		return false;
	EnsureStructures();
	return StructureTiles[static_cast<size_t>(tile.y) * MAXDUNX + tile.x] != 0;
}

const uint8_t *StructureTileMap()
{
	if (leveltype == DTYPE_TOWN)
		return nullptr;
	EnsureStructures();
	return StructureTiles.data();
}

bool HasFloorHoles()
{
	return !FloorHoles.empty();
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
	StructureFrame++;
	// Nearest first, so the depth test turns hidden pixels away before their colour is looked up. Hell's bone stands
	// in the dark beyond HellMeshReach (only the sky would show its outline), where it is left out.
	static std::vector<std::pair<float, const Structure *>> order;
	order.clear();
	const float reach = IsHell() ? HellMeshReach : std::numeric_limits<float>::infinity();
	for (const Structure &st : Structures) {
		const float dx = std::max({ st.art.x0 - proj.pos.x, 0.0F, proj.pos.x - st.art.x1 });
		const float dy = std::max({ st.art.y0 - proj.pos.y, 0.0F, proj.pos.y - st.art.y1 });
		const float distance = std::sqrt(dx * dx + dy * dy);
		if (distance <= reach)
			order.emplace_back(distance, &st);
	}
	std::sort(order.begin(), order.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
	for (const auto &[distance, st] : order) {
		// Big models (hell's stairs) front to back within themselves too.
		if (st->art.mesh.size() > 2000) {
			static std::vector<std::pair<float, uint32_t>> byDistance;
			static std::vector<uint32_t> sorted;
			byDistance.clear();
			for (uint32_t i = 0; i < st->art.mesh.size(); i++) {
				const WorldTriangle &tri = st->art.mesh[i];
				const float dx = (tri.p[0].x + tri.p[1].x + tri.p[2].x) / 3.0F - proj.pos.x;
				const float dy = (tri.p[0].y + tri.p[1].y + tri.p[2].y) / 3.0F - proj.pos.y;
				byDistance.emplace_back(dx * dx + dy * dy, i);
			}
			std::sort(byDistance.begin(), byDistance.end());
			sorted.clear();
			for (const auto &[d, i] : byDistance)
				sorted.push_back(i);
			DrawStructure(out, proj, depthBuffer, *st, VerticalScale(), &sorted);
			continue;
		}
		DrawStructure(out, proj, depthBuffer, *st);
	}
	DrawObjectBoxes(out, proj, depthBuffer);
	DrawDoorLeaves(out, proj, depthBuffer);
}

bool IsObjectDrawnAsMesh(int id)
{
	return leveltype != DTYPE_TOWN && id >= 0 && id < MAXOBJECTS && IsBoxObject(Objects[id]);
}

FILE *OpenFpAsset(const char *envName, const char *fileName)
{
	return OpenHouseAsset(envName, fileName);
}

int BuildingCount()
{
	EnsureBuilt();
	return static_cast<int>(Buildings.size());
}

} // namespace devilution::fp
