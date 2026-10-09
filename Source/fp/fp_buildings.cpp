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
#include "utils/log.hpp"
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
	int minX = std::numeric_limits<int>::max();
	int maxX = std::numeric_limits<int>::min();
	int minY = std::numeric_limits<int>::max();
	int maxY = std::numeric_limits<int>::min();
	for (const auto &[t, isGround] : tiles) {
		// Column pixel (cx, cy) sits at screen ((x - y) * 32 + cx - 32, (x + y + 2) * 16 + cy - 255).
		minX = std::min(minX, (t.x - t.y) * 32 - 32);
		maxX = std::max(maxX, (t.x - t.y) * 32 + 31);
		minY = std::min(minY, (t.x + t.y + 2) * 16 - 255);
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
		const int top = (t.x + t.y + 2) * 16 - 255 - minY;
		for (int cy = 0; cy < ColumnPixelHeight; cy++) {
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
			Buildings.push_back(std::move(chosen->building));
		}
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
	const float ov = b.params.overhang;
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

} // namespace

bool BuildingsEnabled()
{
	return Enabled;
}

void SetBuildingsEnabled(bool enabled)
{
	Enabled = enabled;
}

bool IsBuildingTile(Point tile)
{
	EnsureBuilt();
	return InDungeonBounds(tile) && BuildingTiles[static_cast<size_t>(tile.y) * MAXDUNX + tile.x] != 0;
}

void DrawBuildings(const Surface &out, const Projection &proj, const std::vector<float> &zBuffer, std::vector<float> &depthBuffer)
{
	EnsureBuilt();
	for (const Building &building : Buildings)
		DrawBuilding(out, proj, zBuffer, depthBuffer, building);
}

int BuildingCount()
{
	EnsureBuilt();
	return static_cast<int>(Buildings.size());
}

} // namespace devilution::fp
