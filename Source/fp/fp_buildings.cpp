/**
 * @file fp/fp_buildings.cpp
 *
 * Town buildings rebuilt as a box with a gable roof, textured by projecting the model into the original iso art.
 */
#include "fp/fp_buildings.hpp"

#include <algorithm>
#include <array>
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
#include <unordered_map>
#include <vector>

#include "engine/light_tables.hpp"
#include "engine/palette.h"
#include "fp/fp_doors.hpp"
#include "fp/fp_dungeon.hpp"
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
	bool faceX;         // MeshWallFace: the x face texture (else the y face)
	bool cull = false;  // one-sided: skipped when turned away from the camera (closed shapes, slabs)
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
	uint8_t fallback;   // colour where the art has no pixel
	CleanSurface cleanSurface = CleanSurface::Wall; // MeshClean: the texture, laid on by uv (tiles)
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

/** @brief A face texel up close: from the face's UpscaledTexture at (u, v) in texels of the face, `width` wide. */
int16_t FineFaceTexel(const std::vector<int16_t> &face, int width, float u, float v)
{
	const std::vector<int16_t> &fine = UpscaledTexture(face, width);
	const int fineWidth = width * TextureUpscale;
	const int fineHeight = static_cast<int>(fine.size()) / fineWidth;
	const int x = std::clamp(static_cast<int>(u * TextureUpscale), 0, fineWidth - 1);
	const int y = std::clamp(static_cast<int>(v * TextureUpscale), 0, fineHeight - 1);
	return fine[static_cast<size_t>(y) * fineWidth + x];
}

/** @brief Iso art pixel at the screen position of a world point, searching a few pixels around for opaque art. */
int16_t SampleArt(const Building &b, float wx, float wy, float wz)
{
	if (FineCanvas() && ShadeFine(32.0F) && !b.canvas.empty()) {
		const float fx = (wx - wy) * 32.0F - static_cast<float>(b.originX);
		const float fy = (wx + wy) * 16.0F - wz * HeightPixels - static_cast<float>(b.originY);
		if (fx >= 0 && fy >= 0 && fx < static_cast<float>(b.canvasWidth) && fy < static_cast<float>(b.canvasHeight)) {
			const int16_t pixel = FineFaceTexel(b.canvas, b.canvasWidth, fx, fy);
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
	std::vector<std::array<float, 6>> uvs;       // texture coordinates per vertex (FPL2 files)
	std::vector<int16_t> textures;               // index into MeshTextures, -1 for none
};

/** @brief A model texture (RGB, rows top to bottom) and its palette version for the current palette. */
struct MeshTexture {
	int width = 0;
	int height = 0;
	std::vector<uint8_t> rgb;
	std::vector<uint8_t> indexed;
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
	std::vector<int16_t> lut(32768, -1);
	auto quantize = [&](const std::vector<uint8_t> &rgb, std::vector<uint8_t> &indexed) {
		indexed.resize(rgb.size() / 3);
		for (size_t i = 0; i < indexed.size(); i++) {
			// The textures are daylight photographs; Tristram is drawn at dusk, dim and cool. Darken and pull the
			// colours a little towards grey-blue so the buildings sit with the original art.
			const float r0 = rgb[i * 3];
			const float g0 = rgb[i * 3 + 1];
			const float b0 = rgb[i * 3 + 2];
			const float grey = (r0 + g0 + b0) / 3.0F;
			const float sat = dusk ? 0.75F : 0.85F;
			const int r = std::clamp(static_cast<int>((r0 * sat + grey * (1.0F - sat)) * dim * (dusk ? 0.92F : 1.0F) * tint[0]), 0, 255);
			const int g = std::clamp(static_cast<int>((g0 * sat + grey * (1.0F - sat)) * dim * (dusk ? 0.95F : 1.0F) * tint[1]), 0, 255);
			const int bl = std::clamp(static_cast<int>((b0 * sat + grey * (1.0F - sat)) * dim * (dusk ? 1.08F : 1.0F) * tint[2]), 0, 255);
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

const std::array<LandmarkPlacement, 10> Landmarks { {
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
	// Griswold's smithy: the long hall, and over the forge where he stands an open porch roof on two posts. The plaza
	// porch eaves are low; they are lifted to the hall's wall height, high enough to stand under.
	{ "smithy", { 60, 56 }, { 70, 64 }, 60.17F, 70.87F, 56.78F, 64.08F, 1.59F, 1.0F, 0.60F, true, { Vec2 { 61.69F, 63.85F }, Vec2 { 64.44F, 63.85F } }, true }, // 0.59
	// The fountain's model texture is a coarse rock that reads as a blob at its size: it keeps its art.
	{ "fountain", { 59, 69 }, { 61, 71 }, 59.65F, 61.46F, 69.75F, 71.53F, 0.643F, 100.0F, 0.643F, true, { Vec2 { 0, 0 }, Vec2 { 0, 0 } }, true, false }, // 0.52
	// Tristram's church (research/church): tower, nave and round apse, fitted to the iso art by silhouette overlap.
	{ "church", { 19, 14 }, { 31, 29 }, 19.78F, 32.22F, 14.31F, 30.19F, 0.85F, 100.0F, 0.85F, true, { Vec2 { 0, 0 }, Vec2 { 0, 0 } }, true },
} };

std::vector<LandmarkMesh> LandmarkMeshes;
bool LandmarksLoaded = false;

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
			} else if (ok) {
				ok = readAll(&mesh.classes[t], 1);
			}
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
		AddMeshTriangle(b, p, mesh.classes[t] == 1, middle);
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
	NotePaletteChange();
	const LevelKey key { pDungeonCels.get(), currlevel, static_cast<int>(leveltype), setlevel, PaletteVersion() };
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

/** The eye of the view being rasterized, and the world size of a screen pixel at the pixel being shaded. */
V3 ShadeEye { 0, 0, 0 };
/** MeshColor's texels per world unit for the triangle it last shaded, taking its slant into account. */
const WorldTriangle *SlantFor = nullptr;
float SlantTexelsPerWorld = 0;

/** @brief Palette colour of a mesh surface point: the house's own art patches tiled at the art's pixel density. */
uint8_t MeshColor(const Building &b, const WorldTriangle &tri, float wx, float wy, float wz);

/** @brief The two coordinates of a point kept when dropping an axis. */
inline std::pair<float, float> DropAxis(uint8_t axis, float x, float y, float z)
{
	return axis == 0 ? std::pair { y, z } : (axis == 1 ? std::pair { x, z } : std::pair { x, y });
}

/** @brief Bakes the colours of a projective landmark's triangles, at about the art's 32 pixels per tile. */
void BakeColors(Building &b)
{
	const bool texturesWere = ModelTexturesOn;
	ModelTexturesOn = false; // bake the art projection, for when model textures are switched off
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
		const uint16_t n0 = tri.bakeN;
		tri.bakeN = 0; // sample unbaked
		for (int j = 0; j < size; j++) {
			for (int i = 0; i + j < size; i++) {
				const float u = (static_cast<float>(i) + 0.33F) / static_cast<float>(size);
				const float v = (static_cast<float>(j) + 0.33F) / static_cast<float>(size);
				b.baked[tri.bakeOffset + static_cast<size_t>(j) * size + i] = MeshColor(b, tri, p0.x + e1.x * u + e2.x * v, p0.y + e1.y * u + e2.y * v, p0.z + e1.z * u + e2.z * v);
			}
		}
		tri.bakeN = n0;
	}
	ModelTexturesOn = texturesWere;
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
				const int16_t art = SampleArt(b, x, y, wz);
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
	// Big models: front to back, so the depth test turns hidden pixels away before their colour is looked up.
	static std::vector<uint32_t> sorted;
	const std::vector<uint32_t> *order = nullptr;
	if (b.mesh.size() > 2000) {
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
	RasterizeTriangles(out, proj, depthBuffer, b.mesh, [&](const WorldTriangle &tri, float wx, float wy, float wz) {
		const uint8_t color = MeshColor(b, tri, wx, wy, wz);
		const int tx = std::clamp(static_cast<int>(std::floor(wx)), lightMin.x, lightMax.x);
		const int ty = std::clamp(static_cast<int>(std::floor(wy)), lightMin.y, lightMax.y);
		const uint8_t level = light[static_cast<size_t>(ty - lightMin.y) * lightWidth + tx - lightMin.x];
		const bool textured = ModelTexturesOn && tri.texture >= 0 && tri.bakeN > 0;
		return static_cast<int>(LightTables[level][ShadeColor(color, textured ? tri.textureDarkness : tri.darkness)]);
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

/**
 * @brief The clean dungeon's structures (fp_dungeon.hpp): the cathedral's stairs, the torch stands (their 3D model
 * read off the art, as before), and the pillars and arches, gathered in chunks of tiles so they are culled together.
 */
void BuildCleanStructures()
{
	if (leveltype == DTYPE_CATHEDRAL)
		BuildCathedralStairs();
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
	const float stretch = VerticalScale();
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
		Building &b = Structures[it->second].art;
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
			tri.cull = !quad.twoSided;
			PrepareUvLookup(tri);
		}
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
	// Hell (and the caves, before they were drawn clean): free-standing columns (and torch stands) read off their art,
	// or else arcade pillars joined by arch walls.
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
	if (!ScreenBounds(st.art, proj, left, top, right, bottom))
		return;
	RasterizeTriangles(out, proj, depthBuffer, st.art.mesh, [&](const WorldTriangle &tri, float wx, float wy, float wz) {
		if (tri.mapping == MeshClean) {
			const auto [u, v] = TriangleUv(tri, wx, wy, wz);
			const float shade = tri.darkness * 0.5F + std::max(0.0F, tri.occlusion[0].at0 + tri.occlusion[0].perZ * wz)
			    + std::max(0.0F, tri.occlusion[1].at0 + tri.occlusion[1].perZ * wz);
			return static_cast<int>(CleanColor(tri.cleanSurface, u, v, ShadePixelWorld, CleanLightAt(wx, wy) + shade, ShadeX, ShadeY));
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
			return static_cast<int>(LightTables[std::min<uint8_t>(level, LightsMax)][ShadeColor(PatchColor(texel), tri.darkness)]);
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
			return static_cast<int>(LightTables[std::min<uint8_t>(level, LightsMax)][ShadeColor(PatchColor(art), tri.darkness)]);
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
			return static_cast<int>(LightTables[std::min<uint8_t>(level, LightsMax)][ShadeColor(PatchColor(texel), tri.darkness)]);
		}
		const uint8_t color = StructureColor(st, tri, wx, wy, wz);
		uint8_t darkness = tri.darkness;
		if (wz < -0.02F)
			darkness = std::max<uint8_t>(darkness, static_cast<uint8_t>(std::min(1.0F + -wz * 3.5F, static_cast<float>(SurfaceShade.size() - 1))));
		const Point tile { static_cast<int>(std::floor(wx)), static_cast<int>(std::floor(wy)) };
		const uint8_t level = InDungeonBounds(tile) ? LightAt(tile) : LightsMax;
		return static_cast<int>(LightTables[std::min<uint8_t>(level, LightsMax)][ShadeColor(color, darkness)]);
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
			return static_cast<int>(LightTables[std::min<uint8_t>(level, LightsMax)][ShadeColor(static_cast<uint8_t>(color), darkness)]);
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
	for (const Structure &st : Structures)
		DrawStructure(out, proj, depthBuffer, st);
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
