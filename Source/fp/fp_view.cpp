/**
 * @file fp/fp_view.cpp
 *
 * First-person view of the live level (software raycaster).
 *
 * World space is the dPiece grid: one unit per tile, the tile (x, y) covering [x, x+1) x [y, y+1).
 * The iso projection is a rotation of this grid (screen x = (x - y) * 32, screen y = (x + y) * 16),
 * so the handedness matches a y-down top-down map.
 */
#include "fp/fp_view.hpp"

#include <algorithm>
#include <unordered_map>
#include <cstdlib>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "engine/light_tables.hpp"
#include "engine/palette.h"
#include "engine/render/text_render.hpp"
#include "fp/fp_controls.hpp"
#include "fp/fp_buildings.hpp"
#include "fp/fp_doors.hpp"
#include "objects.h"
#include "utils/log.hpp"
#include "fp/fp_raycast.hpp"
#include "fp/fp_textures.hpp"
#include "fp/fp_weapon.hpp"
#include "fp/fp_world.hpp"
#include "levels/dun_tile_data.hpp"
#include "levels/gendung.h"
#include "utils/clx_decode.hpp"
#include "utils/str_cat.hpp"

namespace devilution::fp {

namespace {

bool FirstPersonActive = false;

uint8_t Shade(uint8_t color, uint8_t light)
{
	return LightTables[std::min<uint8_t>(light, LightsMax)][color];
}

/** @brief Colour of the open sky above town, which has no ceiling. */
constexpr uint8_t TownSkyColor = PAL16_BLUE + 14;
/** @brief Light levels added to ceilings so they read as darker than the floor they mirror. */
constexpr uint8_t CeilingExtraDarkness = 4;

/** Floor depth is stored a little farther than it is, so sprites lying on the floor (corpses) still win. */
constexpr float FloorDepthBias = 1.03F;

/** Town fog: things fade into the sky colour from FogStart tiles away, gone at FogEnd. */
constexpr float FogStart = 20.0F;
constexpr float FogEnd = 46.0F;
constexpr int FogSteps = 16;

/** @brief fog[step][colour]: the palette colour nearest to the colour mixed step / FogSteps of the way to the sky. */
const std::array<std::array<uint8_t, 256>, FogSteps + 1> &FogTables()
{
	static std::array<std::array<uint8_t, 256>, FogSteps + 1> tables {};
	static uint32_t readyFor = 0;
	if (readyFor != PaletteVersion()) {
		readyFor = PaletteVersion();
		const SDL_Color sky = logical_palette[TownSkyColor];
		for (int step = 0; step <= FogSteps; step++) {
			const float t = static_cast<float>(step) / FogSteps;
			for (int i = 0; i < 256; i++) {
				const SDL_Color c = logical_palette[i];
				const float r = c.r + (sky.r - c.r) * t;
				const float g = c.g + (sky.g - c.g) * t;
				const float b = c.b + (sky.b - c.b) * t;
				float best = std::numeric_limits<float>::max();
				tables[step][i] = static_cast<uint8_t>(i);
				for (int j = 1; j < 256; j++) {
					const SDL_Color o = logical_palette[j];
					const float d = (o.r - r) * (o.r - r) * 3 + (o.g - g) * (o.g - g) * 6 + (o.b - b) * (o.b - b);
					if (d < best) {
						best = d;
						tables[step][i] = static_cast<uint8_t>(j);
					}
				}
			}
			tables[step][TownSkyColor] = TownSkyColor;
		}
	}
	return tables;
}

/**
 * @brief Palette colours nearest to RGB colours (5 bits a channel), looked up as they are asked for and forgotten
 * when the palette changes. Colours 1-31 are left out: the caves and hell cycle them (lava), which would make
 * blended sprites flicker.
 */
class NearestColors {
public:
	void Prepare()
	{
		if (ready_ && readyFor_ == PaletteVersion())
			return;
		ready_ = true;
		readyFor_ = PaletteVersion();
		cache_.fill(Unknown);
	}

	uint8_t operator()(int r, int g, int b)
	{
		const size_t key = static_cast<size_t>((r >> 3) << 10 | (g >> 3) << 5 | (b >> 3));
		uint16_t &entry = cache_[key];
		if (entry == Unknown) {
			const int cr = (r & ~7) + 4;
			const int cg = (g & ~7) + 4;
			const int cb = (b & ~7) + 4;
			int best = std::numeric_limits<int>::max();
			entry = 0;
			for (int j = 32; j < 256; j++) {
				const SDL_Color o = logical_palette[j];
				const int d = (o.r - cr) * (o.r - cr) * 3 + (o.g - cg) * (o.g - cg) * 6 + (o.b - cb) * (o.b - cb);
				if (d < best) {
					best = d;
					entry = static_cast<uint16_t>(j);
				}
			}
		}
		return static_cast<uint8_t>(entry);
	}

private:
	static constexpr uint16_t Unknown = 0xFFFF;
	std::array<uint16_t, 32768> cache_ {};
	bool ready_ = false;
	uint32_t readyFor_ = 0;
};

NearestColors NearestColor;

/** @returns the fog step (0: clear) at a camera depth; 0 outside town. */
int FogStep(float depth)
{
	if (leveltype != DTYPE_TOWN || depth <= FogStart)
		return 0;
	if (depth >= FogEnd)
		return FogSteps;
	return static_cast<int>((depth - FogStart) / (FogEnd - FogStart) * FogSteps + 0.5F);
}

/** @brief Fogs everything drawn so far (the ground, walls and houses) by its depth; sprites fog themselves. */
void DrawFog(const Surface &out, const Projection &proj, const std::vector<float> &depthBuffer)
{
	if (leveltype != DTYPE_TOWN)
		return;
	const auto &fog = FogTables();
	for (int y = 0; y < proj.height; y++) {
		uint8_t *dst = out.at(0, y);
		const float *depth = &depthBuffer[static_cast<size_t>(y) * proj.width];
		for (int x = 0; x < proj.width; x++) {
			// The ground stores its depth a little farther (FloorDepthBias): a few percent is no matter here.
			const int step = FogStep(depth[x]);
			if (step > 0)
				dst[x] = fog[step][dst[x]];
		}
	}
}

/**
 * @param depthBuffer per-pixel camera depth; floor pixels write theirs, so things sunk below the floor (the
 * stairs' pit, the feet of sprites) are hidden by it
 */
void DrawFloorAndCeiling(const Surface &out, const Projection &proj, std::vector<float> &depthBuffer)
{
	const Vec2 rayLeft { proj.dir.x - proj.plane.x, proj.dir.y - proj.plane.y };
	const Vec2 rayRight { proj.dir.x + proj.plane.x, proj.dir.y + proj.plane.y };
	const float wallHeight = WallHeight();
	const bool isTown = leveltype == DTYPE_TOWN;
	// Hell's walls are low bone and spine work with the dark above them, as in the original.
	const bool hasCeiling = !isTown && leveltype != DTYPE_HELL;

	const uint8_t *structures = StructureTileMap();
	const uint8_t *buildings = BuildingTileMap();
	const bool kit = KitSurfacesActive();
	const bool holes = HasFloorHoles();
	const std::vector<int16_t> &ground = GetGroundFloor();
	for (int y = 0; y < proj.height; y++) {
		const bool isFloor = y > proj.horizon;
		uint8_t *dst = out.at(0, y);
		if (!isFloor && !hasCeiling) {
			std::fill_n(dst, proj.width, isTown ? TownSkyColor : uint8_t { 0 });
			continue;
		}
		const int rowsFromHorizon = isFloor ? y - proj.horizon : proj.horizon - y;
		if (rowsFromHorizon == 0) {
			std::fill_n(dst, proj.width, 0);
			continue;
		}
		const float heightAboveOrBelow = isFloor ? EyeHeight : wallHeight * VerticalScale() - EyeHeight;
		const float rowDistance = heightAboveOrBelow * proj.focal / static_cast<float>(rowsFromHorizon);

		const float stepX = rowDistance * (rayRight.x - rayLeft.x) / static_cast<float>(proj.width);
		const float stepY = rowDistance * (rayRight.y - rayLeft.y) / static_cast<float>(proj.width);
		float wx = proj.pos.x + rowDistance * rayLeft.x;
		float wy = proj.pos.y + rowDistance * rayLeft.y;
		// The tile changes every few pixels at most: look its art and light up once per tile.
		Point cachedTile { -1, -1 };
		// Up close, the floor art at TextureUpscale times its resolution (edges smoothed, see UpscaledTexture).
		const bool fineRow = std::hypot(stepX, stepY) * FloorTextureSize < UpscaleBelow;
		const std::vector<int16_t> *fineFloor = nullptr;
		const std::vector<int16_t> *floorArt = nullptr;
		const std::vector<int16_t> *floorFill = nullptr;
		uint8_t tileLight = 0;
		bool kitTile = false;

		for (int x = 0; x < proj.width; x++, wx += stepX, wy += stepY) {
			const float fx = std::floor(wx);
			const float fy = std::floor(wy);
			const Point tile { static_cast<int>(fx), static_cast<int>(fy) };
			if (!InDungeonBounds(tile)) {
				dst[x] = 0;
				continue;
			}
			if (holes && isFloor && IsFloorHole(wx, wy)) {
				dst[x] = 0; // open: whatever is down there is drawn into it
				continue;
			}
			if (tile != cachedTile) {
				cachedTile = tile;
				const uint8_t structure = structures != nullptr ? structures[static_cast<size_t>(tile.y) * MAXDUNX + tile.x] : 0;
				// Under a structure the piece's own art is the structure, drawn as a mesh: plain ground instead.
				const PieceTextures &piece = GetPieceTextures(dPiece[tile.x][tile.y]);
				// The ceiling is the plain ground: mirroring each tile's floor would hang lava, water and blood overhead.
				floorArt = (structure != 0 && structure != 3) || !isFloor || (buildings != nullptr && buildings[static_cast<size_t>(tile.y) * MAXDUNX + tile.x] != 0) ? &ground : &piece.floor;
				floorFill = isTown ? &ground : nullptr; // baked shadows and holes in town ground
				fineFloor = fineRow && floorArt->size() == static_cast<size_t>(FloorTextureSize) * FloorTextureSize ? &UpscaledTexture(*floorArt, FloorTextureSize) : nullptr;
				kitTile = kit && !piece.isFlatSolid; // lava and water keep their art
				tileLight = LightAt(tile);
				if (!isFloor)
					tileLight = std::min<uint8_t>(tileLight + CeilingExtraDarkness, LightsMax);
			}
			const int u = std::min(static_cast<int>((wx - fx) * FloorTextureSize), FloorTextureSize - 1);
			const int v = std::min(static_cast<int>((wy - fy) * FloorTextureSize), FloorTextureSize - 1);
			int16_t texel = (*floorArt)[v * FloorTextureSize + u];
			if (floorFill != nullptr && texel <= 0)
				texel = (*floorFill)[v * FloorTextureSize + u];
			else if (texel == NoPixel)
				texel = ground[v * FloorTextureSize + u]; // a solid piece seen through a doorway
			else if (fineFloor != nullptr) {
				constexpr int FineSize = FloorTextureSize * TextureUpscale;
				const int fu = std::min(static_cast<int>((wx - fx) * FineSize), FineSize - 1);
				const int fv = std::min(static_cast<int>((wy - fy) * FineSize), FineSize - 1);
				if (const int16_t f = (*fineFloor)[fv * FineSize + fu]; f != NoPixel)
					texel = f;
			}

			const uint8_t light = tileLight;
			if (kitTile)
				dst[x] = Shade(isFloor ? KitFloorColor(wx, wy, texel) : KitCeilingColor(wx, wy), light);
			else
				dst[x] = texel == NoPixel ? 0 : Shade(static_cast<uint8_t>(texel), light);
			if (isFloor)
				depthBuffer[static_cast<size_t>(y) * proj.width + x] = rowDistance * FloorDepthBias;
		}
	}
}

/** @brief The art and openings used for one wall face. */
struct FaceArt {
	const std::vector<int16_t> *face;
	const std::vector<uint8_t> *holes; // null if the face has no openings to look through
	int rows;                          // height of the wall this art shows, in face rows (0: the level's wall height)
	bool emptyIsOpen = false;          // every texel without art is an opening (arch pillars)
	const std::vector<int> *recess = nullptr; // first recessed row per face column (see PieceTextures::recessX)
	float recessDepth = 0;
	const Doorway *door = nullptr; // a doorway's art covers (part of) the face: its opening goes through the wall
	int doorOffset = 0;            // the tile's offset from the door tile along the wall
	bool passThrough = false;      // inside a doorway's opening, between two of its tiles: not drawn
	// The base texels whose art the next tile along the wall draws (see PieceTextures::cutX), and that tile's spill.
	const std::vector<uint8_t> *cut = nullptr;
	const std::vector<int16_t> *spill = nullptr;
};

/** @brief The next tile along a wall face (towards its far end, u = 1). */
Point NextAlongFace(Point tile, bool faceX)
{
	return faceX ? Point { tile.x, tile.y + 1 } : Point { tile.x + 1, tile.y };
}

/** @brief The level's plain filler wall for a tile at `parity` along its wall, with its blind arch carved in. */
FaceArt PlainWallArt(int parity)
{
	FaceArt art { &GetFallbackFace(parity), nullptr, 0 };
	uint16_t first;
	uint16_t second;
	bool useX;
	if (GetPlainWallPair(first, second, useX)) {
		// The pair alternates along the wall: the other one is the next tile.
		const bool odd = (parity & 1) != 0;
		const PieceTextures &own = GetPieceTextures(odd ? second : first);
		const PieceTextures &next = GetPieceTextures(odd ? first : second);
		art.cut = useX ? &own.cutX : &own.cutY;
		art.spill = useX ? &next.spillX : &next.spillY;
	}
	float depth;
	if (const std::vector<int> *recess = GetFallbackRecess(parity, depth); recess != nullptr && WallRecessesEnabled()) {
		art.recess = recess;
		art.recessDepth = depth;
	}
	return art;
}

/** @brief The face art of a wall hit from the tile's own art: the face hit, else the other face, else the level's plain wall. */
FaceArt TileFaceArt(const RayHit &hit)
{
	const PieceTextures &textures = GetPieceTextures(dPiece[hit.tile.x][hit.tile.y]);
	const bool wantX = hit.xSide;
	if (IsArchPillarTile(hit.tile)) {
		// The pillar and its half arch stand on the faces the art draws them on; the rest is open.
		FaceArt art = wantX ? FaceArt { &textures.faceX, &textures.holesX, 0 } : FaceArt { &textures.faceY, &textures.holesY, 0 };
		art.emptyIsOpen = true;
		return art;
	}
	auto withRecess = [&](bool useX) {
		FaceArt art = useX ? FaceArt { &textures.faceX, &textures.holesX, textures.faceXRows } : FaceArt { &textures.faceY, &textures.holesY, textures.faceYRows };
		if (const Point next = NextAlongFace(hit.tile, useX); useX == wantX && InDungeonBounds(next)) {
			const PieceTextures &nextTextures = GetPieceTextures(dPiece[next.x][next.y]);
			art.cut = useX ? &textures.cutX : &textures.cutY;
			art.spill = useX ? &nextTextures.spillX : &nextTextures.spillY;
		}
		const std::vector<int> &recess = useX ? textures.recessX : textures.recessY;
		if (!recess.empty() && WallRecessesEnabled()) {
			art.recess = &recess;
			art.recessDepth = useX ? textures.recessDepthX : textures.recessDepthY;
		}
		return art;
	};
	if (wantX ? textures.faceXUsable : textures.faceYUsable)
		return withRecess(wantX);
	if (wantX ? textures.faceYUsable : textures.faceXUsable)
		return withRecess(!wantX);
	// A wall whose face the iso camera never saw: the level's plain wall, alternating along the wall so its arches
	// come out whole.
	if (HasPlainWall())
		return PlainWallArt(wantX ? hit.tile.y : hit.tile.x);
	// No plain wall on the level (hell's bone walls): the piece's own sparse art, open between the bones.
	auto artCount = [](const std::vector<int16_t> &face) { return std::count_if(face.begin(), face.end(), [](int16_t t) { return t > 0; }); };
	const bool useX = artCount(textures.faceX) >= artCount(textures.faceY);
	FaceArt art = useX ? FaceArt { &textures.faceX, &textures.holesX, 0 } : FaceArt { &textures.faceY, &textures.holesY, 0 };
	art.emptyIsOpen = true;
	return art;
}

/** @brief The face art for a wall hit, a doorway's art over the tiles it covers. */
FaceArt FaceForHit(const RayHit &hit)
{
	if (!InDungeonBounds(hit.tile)) {
		// The edge of the map is a plain black wall.
		static const std::vector<int16_t> Black(static_cast<size_t>(FaceTextureWidth) * ColumnPixelHeight, 0);
		return { &Black, nullptr, 0 };
	}
	const DoorwaySpan span = DoorwaySpanAt(hit.tile);
	if (span.door == nullptr)
		return TileFaceArt(hit);
	const Doorway &door = *span.door;
	if (hit.xSide == door.alongX) {
		// A face across the wall between two tiles of the doorway: inside its opening, seen only through it.
		const float along = door.alongX ? hit.point.x - static_cast<float>(door.tile.x) : hit.point.y - static_cast<float>(door.tile.y);
		if (along > door.openFrom + 0.01F && along < door.openTo - 0.01F) {
			FaceArt art { nullptr, nullptr, 0 };
			art.passThrough = true;
			return art;
		}
		return TileFaceArt(hit);
	}
	FaceArt art { &door.face, nullptr, 0 };
	if (span.offset != 0) {
		// A neighbour keeps its own art beside the doorway; the partner, painted with the other half of the door, is
		// plain wall.
		art = span.offset < 0 && door.hasPartner && HasPlainWall() ? PlainWallArt(hit.xSide ? hit.tile.y : hit.tile.x) : TileFaceArt(hit);
	}
	art.door = &door;
	art.doorOffset = span.offset;
	return art;
}

/** Wall faces nearer than this (tiles) are the kit's wall models; farther ones are drawn from the art. */
constexpr float KitWallModelRange = 6.0F;
/** Steps a ray through a doorway is followed to find where it meets the doorway's sides, then refined by halving. */
constexpr int DoorwaySteps = 8;
constexpr int DoorwayRefinements = 4;

/** @brief Whether a face's opening mask has any opening in it (cached per mask). */
bool HasOpenings(const std::vector<uint8_t> *holes)
{
	if (holes == nullptr)
		return false;
	static std::unordered_map<const std::vector<uint8_t> *, bool> cache;
	const auto found = cache.find(holes);
	if (found != cache.end())
		return found->second;
	const bool any = std::any_of(holes->begin(), holes->end(), [](uint8_t h) { return h != 0; });
	cache.emplace(holes, any);
	return any;
}

/**
 * @brief Draws the walls. Openings in a wall (arches) are looked through to the walls behind them.
 * @param depthBuffer per-pixel camera depth (width * height); wall pixels write their depth, the rest stays infinite
 */
void DrawWalls(const Surface &out, const Projection &proj, std::vector<float> &zBuffer, std::vector<float> &depthBuffer)
{
	const float wallHeight = WallHeight();
	const float stretch = VerticalScale();
	const bool kit = KitSurfacesActive();
	static const bool kitWalls = std::getenv("FP_NO_KITWALLS") == nullptr;
	const int wallRows = LevelWallRows();
	const int faceHeight = FaceTextureHeight();
	const std::vector<int16_t> &fallback = GetFallbackFace();
	static std::vector<uint8_t> filled;
	filled.resize(static_cast<size_t>(proj.height));
	// Levels without a plain wall (hell's bones) are open nearly everywhere: fewer layers keep them affordable.
	const int maxLayers = HasPlainWall() ? MaxWallLayers : 4;

	for (int x = 0; x < proj.width; x++) {
		const Vec2 ray = proj.columnRay(x);
		std::fill(filled.begin(), filled.end(), uint8_t { 0 });
		zBuffer[x] = MaxRayDistance;
		for (int layer = 0; layer < maxLayers; layer++) {
			const RayHit hit = CastRay(proj.pos, ray, layer);
			if (!hit.hit)
				break;
			if (layer == 0)
				zBuffer[x] = hit.distance;

			const FaceArt art = FaceForHit(hit);
			if (art.passThrough)
				continue; // inside a doorway: on to whatever is beyond it
			// Each wall is as tall as its art: low walls (balustrades, tomb boxes) stay low, and the walls behind them show above.
			const int hitRows = art.rows > 0 ? std::clamp(art.rows, 1, faceHeight) : wallRows;
			const float hitHeight = static_cast<float>(WallShownRows(hitRows)) / SpritePixelsPerTileY;
			const bool lowWall = hitHeight < wallHeight - 0.2F;
			const float scale = proj.focal / std::max(hit.distance, 0.01F);
			const float topF = static_cast<float>(proj.horizon) - (hitHeight * stretch - EyeHeight) * scale;
			const float bottomF = static_cast<float>(proj.horizon) + EyeHeight * scale;
			const int top = std::max(0, static_cast<int>(topF));
			const int bottom = std::min(proj.height - 1, static_cast<int>(bottomF));

			// Position along the face in world-axis order. The hidden back faces show the same art unmirrored: mirroring
			// each tile on its own breaks arches that span neighbouring tiles into repeated halves.
			float along = hit.xSide ? hit.point.y - static_cast<float>(hit.tile.y) : hit.point.x - static_cast<float>(hit.tile.x);
			const int u = std::clamp(static_cast<int>(along * FaceTextureWidth), 0, FaceTextureWidth - 1);

			const uint8_t lightLevel = LightAt(hit.previous);
			const uint8_t *light = LightTables[lightLevel].data();
			// In the mod-kit dungeon, faces with a niche or a doorway are the kit's wall models (drawn with the
			// meshes): here they are looked through to whatever is behind them.
			bool kitModel = false;
			if (kit && kitWalls && art.door == nullptr && !lowWall && !art.emptyIsOpen && hit.distance < KitWallModelRange) {
				// Openings are doorway walls; every other face (recessed or plain) a niche wall.
				const bool opening = art.recess == nullptr && HasOpenings(art.holes);
				kitModel = KitWallModel(hit.tile, hit.previous, !opening, hitHeight);
			}
			// A recess (blind arch, niche) is a second plane behind the face; the rays carry on to it.
			const float rayAcross = std::abs(hit.xSide ? ray.x : ray.y);
			const float recessDistance = art.recess != nullptr && rayAcross > 1e-4F ? hit.distance + art.recessDepth / rayAcross : 0.0F;
			auto alongAt = [&](float distance) {
				const Vec2 p { proj.pos.x + ray.x * distance, proj.pos.y + ray.y * distance };
				return hit.xSide ? p.y - static_cast<float>(hit.tile.y) : p.x - static_cast<float>(hit.tile.x);
			};
			int recessU = -1;
			float recessAlong = 0;
			if (recessDistance > 0) {
				recessAlong = alongAt(recessDistance);
				if (recessAlong >= 0.0F && recessAlong < 1.0F)
					recessU = std::clamp(static_cast<int>(recessAlong * FaceTextureWidth), 0, FaceTextureWidth - 1);
			}
			// A doorway goes through the whole tile: the depths the ray is followed at through it, and the face column
			// there (-1 outside the face).
			const float doorwayExit = art.door != nullptr && rayAcross > 1e-4F ? hit.distance + 1.0F / rayAcross : hit.distance;
			std::array<float, DoorwaySteps + 1> stepDepth;
			std::array<int, DoorwaySteps + 1> stepU;
			const float doorRowsPerTile = SpritePixelsPerTileY * WallArtScale(wallRows);
			// The doorway's art column at a door-relative position, -1 outside its opening's span.
			auto openColumn = [&](float a) {
				return a >= art.door->openFrom && a < art.door->openTo ? DoorFaceColumn(*art.door, a) : -1;
			};
			auto doorColumn = [&](float distance) { return openColumn(alongAt(distance) + static_cast<float>(art.doorOffset)); };
			// This screen column's place in the doorway's art (-1: the tile's own art), and in its opening.
			const int doorArtU = art.door != nullptr ? DoorFaceColumn(*art.door, along + static_cast<float>(art.doorOffset)) : -1;
			const int doorOpenU = art.door != nullptr ? openColumn(along + static_cast<float>(art.doorOffset)) : -1;
			if (art.door != nullptr) {
				for (int step = 0; step <= DoorwaySteps; step++) {
					stepDepth[step] = hit.distance + (doorwayExit - hit.distance) * static_cast<float>(step) / DoorwaySteps;
					stepU[step] = doorColumn(stepDepth[step]);
				}
			}
			const uint8_t *recessLight = LightTables[std::min<int>(lightLevel + 1, LightsMax)].data();
			const uint8_t *revealLight = LightTables[std::min<int>(lightLevel + 3, LightsMax)].data();
			// The wall covers the bottom `hitRows` rows of the face texture; the cap at their top is cut off.
			const float artScale = WallArtScale(hitRows);
			const float rowsPerTile = SpritePixelsPerTileY * artScale; // face rows per tile of height (see WallArtRow)
			const int shownRows = static_cast<int>(static_cast<float>(hitRows) * artScale + 0.5F);
			const float rowsPerPixel = static_cast<float>(shownRows) / (bottomF - topF);
			// Up close, the face's art at TextureUpscale times its resolution (edges smoothed, see UpscaledTexture).
			const std::vector<int16_t> *fine = rowsPerPixel < UpscaleBelow && art.face != nullptr ? &UpscaledTexture(*art.face, FaceTextureWidth) : nullptr;
			const int fineU = std::clamp(static_cast<int>(along * FaceTextureWidth * TextureUpscale), 0, FaceTextureWidth * TextureUpscale - 1);
			bool sawOpening = lowWall || kitModel;
			for (int y = top; y <= bottom && !kitModel; y++) {
				if (filled[y] != 0)
					continue;
				const int row = std::clamp(faceHeight - shownRows + static_cast<int>((static_cast<float>(y) - topF) * rowsPerPixel), 0, faceHeight - 1);
				const size_t at = static_cast<size_t>(row) * FaceTextureWidth + u;
				// Height above the floor (art tiles) of this screen row at a depth along the ray.
				auto heightAt = [&](float distance) { return (EyeHeight + static_cast<float>(proj.horizon - y) * distance / proj.focal) / stretch; };
				if (doorOpenU >= 0 && row >= art.door->opening[doorOpenU]) {
					// Through the doorway: follow the ray through the wall's thickness. If it stays inside the opening it
					// looks through (to the room behind, and the leaf); else it meets the doorway's side or top there.
					const std::vector<int> &opening = art.door->opening;
					auto open = [&](int column, float distance) {
						if (column < 0)
							return false;
						const float height = heightAt(distance);
						const int r = faceHeight - 1 - static_cast<int>(height * doorRowsPerTile);
						return height >= 0.0F && r >= opening[column] && r < faceHeight;
					};
					float inside = hit.distance;
					float outside = -1.0F;
					for (int step = 1; step <= DoorwaySteps; step++) {
						if (!open(stepU[step], stepDepth[step])) {
							outside = stepDepth[step];
							break;
						}
						inside = stepDepth[step];
					}
					if (outside < 0) {
						sawOpening = true;
						continue;
					}
					for (int k = 0; k < DoorwayRefinements; k++) {
						const float d = (inside + outside) / 2.0F;
						if (open(doorColumn(d), d))
							inside = d;
						else
							outside = d;
					}
					// The side or top of the opening where the ray meets it, faced with the plain wall's stone: across the
					// wall's thickness, and up the side (or along the top).
					const Vec2 exit { proj.pos.x + ray.x * outside, proj.pos.y + ray.y * outside };
					const float into = hit.xSide ? exit.x - std::floor(exit.x) : exit.y - std::floor(exit.y);
					const int revealU = std::clamp(static_cast<int>(into * FaceTextureWidth), 0, FaceTextureWidth - 1);
					const bool atTop = doorColumn(outside) >= 0; // still inside the opening's span: it met the top
					const float alongExit = alongAt(outside);
					const float up = atTop ? (alongExit - std::floor(alongExit)) : heightAt(outside);
					const int revealRow = std::clamp(faceHeight - 1 - static_cast<int>(up * rowsPerTile), faceHeight - shownRows, faceHeight - 1);
					int16_t reveal = fallback[static_cast<size_t>(revealRow) * FaceTextureWidth + revealU];
					if (reveal <= 0)
						reveal = fallback[at] > 0 ? fallback[at] : 0;
					*out.at(x, y) = (atTop ? recessLight : revealLight)[reveal];
					filled[y] = 1;
					depthBuffer[static_cast<size_t>(y) * proj.width + x] = outside;
					continue;
				}
				if (doorArtU >= 0) {
					// The doorway's frame around its opening.
					int16_t texel = art.door->face[static_cast<size_t>(row) * FaceTextureWidth + doorArtU];
					if (texel == NoPixel)
						texel = fallback[at];
					*out.at(x, y) = light[texel];
					filled[y] = 1;
					depthBuffer[static_cast<size_t>(y) * proj.width + x] = hit.distance;
					continue;
				}
				if (art.recess != nullptr && row >= (*art.recess)[u] && (*art.face)[at] != NoPixel) {
					// Inside the recess outline: the back of the recess if the ray reaches it inside the outline,
					// else the recess's side (its reveal).
					const float height = heightAt(recessDistance);
					const int row2 = std::clamp(faceHeight - 1 - static_cast<int>(height * rowsPerTile), 0, faceHeight - 1);
					uint8_t color;
					float depth = hit.distance;
					int16_t back = recessU >= 0 ? (*art.face)[static_cast<size_t>(row2) * FaceTextureWidth + recessU] : NoPixel;
					if (fine != nullptr && back != NoPixel) {
						const int fineBackU = std::clamp(static_cast<int>(recessAlong * FaceTextureWidth * TextureUpscale), 0, FaceTextureWidth * TextureUpscale - 1);
						const int fineBackRow = std::clamp(static_cast<int>((static_cast<float>(faceHeight) - height * rowsPerTile) * TextureUpscale), 0, faceHeight * TextureUpscale - 1);
						if (const int16_t f = (*fine)[static_cast<size_t>(fineBackRow) * FaceTextureWidth * TextureUpscale + fineBackU]; f != NoPixel)
							back = f;
					}
					if (recessU >= 0 && row2 >= (*art.recess)[recessU] && back != NoPixel) {
						const uint8_t base = kit ? KitWallColor(recessAlong, height, back) : static_cast<uint8_t>(back);
						color = recessLight[base];
						depth = recessDistance;
					} else {
						color = revealLight[kit ? KitWallColor(along, static_cast<float>(faceHeight - 1 - row) / rowsPerTile, (*art.face)[at]) : (*art.face)[at]];
					}
					*out.at(x, y) = color;
					filled[y] = 1;
					depthBuffer[static_cast<size_t>(y) * proj.width + x] = depth;
					continue;
				}
				int16_t texel = (*art.face)[at];
				bool spilt = false;
				if (const int base = row - (faceHeight - FaceBaseRows); base >= 0 && art.cut != nullptr) {
					// The foot of the face's far end, drawn by the next tile.
					const size_t baseAt = static_cast<size_t>(base) * FaceTextureWidth + u;
					spilt = (*art.cut)[baseAt] != 0 && (*art.spill)[baseAt] != NoPixel;
					if (spilt)
						texel = (*art.spill)[baseAt];
				}
				if (fine != nullptr && texel != NoPixel && !spilt) {
					const int fineRow = std::clamp((faceHeight - shownRows) * TextureUpscale + static_cast<int>((static_cast<float>(y) - topF) * rowsPerPixel * TextureUpscale), 0, faceHeight * TextureUpscale - 1);
					texel = (*fine)[static_cast<size_t>(fineRow) * FaceTextureWidth * TextureUpscale + fineU];
				}
				if (texel == NoPixel) {
					if (art.emptyIsOpen || (art.holes != nullptr && (*art.holes)[at] != 0)) {
						sawOpening = true; // look through to the next wall
						continue;
					}
					texel = fallback[at];
				}
				if (kit)
					texel = KitWallColor(along, static_cast<float>(faceHeight - 1 - row) / rowsPerTile, texel);
				*out.at(x, y) = light[texel];
				filled[y] = 1;
				depthBuffer[static_cast<size_t>(y) * proj.width + x] = hit.distance;
			}
			if (!sawOpening)
				break;
		}
	}
}

void DrawSpriteBillboard(const Surface &out, const Projection &proj, const std::vector<float> &zBuffer, const std::vector<float> *depthBuffer, const Entity &entity, const BillboardRect &rect)
{
	static std::vector<int16_t> decoded;
	const std::vector<int16_t> *source = entity.pixels;
	int spriteWidth = entity.pixelsWidth;
	int spriteHeight = entity.pixelsHeight;
	if (source == nullptr) {
		const ClxSprite sprite = *entity.sprite;
		DecodeClx(sprite, decoded);
		source = &decoded;
		spriteWidth = sprite.width();
		spriteHeight = sprite.height();
	}
	const std::vector<int16_t> &pixels = *source;
	const uint8_t *light = entity.lit ? LightTables[LightAt(entity.tile)].data() : nullptr;
	const int screenWidth = rect.right - rect.left;
	const int screenHeight = rect.bottom - rect.top;

	const int x0 = std::max(0, rect.left);
	const int x1 = std::min(proj.width, rect.right);
	const int y0 = std::max(0, rect.top);
	const int y1 = std::min(proj.height, rect.bottom);
	const int fogStep = FogStep(rect.depth);
	const uint8_t *fog = fogStep > 0 ? FogTables()[fogStep].data() : nullptr;
	auto shade = [&](int16_t pixel) {
		uint8_t color = static_cast<uint8_t>(pixel);
		if (entity.trn != nullptr)
			color = entity.trn[color];
		else if (light != nullptr)
			color = light[color];
		return color;
	};

	if (screenHeight >= spriteHeight * 3 / 2) {
		// Blown up (near): sampled bilinearly, so outlines come out smooth instead of in big steps. The outline is
		// where the sprite's coverage crosses one half; colours blend between the opaque neighbours and go back to
		// the palette through NearestColor.
		NearestColor.Prepare();
		// The blend between neighbouring texels only spans the middle half of the way across (sharp bilinear): each
		// texel stays solid over the rest, so the sprite is half as soft as with plain bilinear filtering.
		auto Sharpen = [](int t) { return std::clamp((t - 64) * 2, 0, 256); };
		// Each colour's lit RGB, once per sprite; the sample positions of each column, once per sprite.
		std::array<uint8_t, 256> lit;
		std::array<SDL_Color, 256> rgb;
		for (int i = 0; i < 256; i++) {
			lit[i] = shade(static_cast<int16_t>(i));
			rgb[i] = logical_palette[lit[i]];
		}
		static std::vector<int> columnU;
		static std::vector<int> columnT;
		columnU.resize(static_cast<size_t>(std::max(0, x1 - x0)));
		columnT.resize(columnU.size());
		const float stepU = static_cast<float>(spriteWidth) / static_cast<float>(screenWidth);
		const float stepV = static_cast<float>(spriteHeight) / static_cast<float>(screenHeight);
		for (int x = x0; x < x1; x++) {
			const float fu = (static_cast<float>(x - rect.left) + 0.5F) * stepU - 0.5F;
			const int u = static_cast<int>(std::floor(fu));
			columnU[x - x0] = u;
			columnT[x - x0] = Sharpen(static_cast<int>((fu - static_cast<float>(u)) * 256.0F));
		}
		// The sprite with a clear border, so the four samples need no bounds checks; shadow (0) counts as clear.
		static std::vector<uint8_t> padded;
		const int paddedWidth = spriteWidth + 2;
		padded.assign(static_cast<size_t>(paddedWidth) * (spriteHeight + 2), 0);
		for (int v = 0; v < spriteHeight; v++)
			for (int u = 0; u < spriteWidth; u++)
				padded[static_cast<size_t>(v + 1) * paddedWidth + u + 1] = static_cast<uint8_t>(std::max<int16_t>(pixels[static_cast<size_t>(v) * spriteWidth + u], 0));
		for (int y = y0; y < y1; y++) {
			const float fv = (static_cast<float>(y - rect.top) + 0.5F) * stepV - 0.5F;
			const int v0 = static_cast<int>(std::floor(fv));
			const int tv = Sharpen(static_cast<int>((fv - static_cast<float>(v0)) * 256.0F));
			if (v0 < -1 || v0 >= spriteHeight)
				continue;
			const uint8_t *row0 = &padded[static_cast<size_t>(v0 + 1) * paddedWidth + 1];
			const uint8_t *row1 = row0 + paddedWidth;
			const float *depthRow = depthBuffer != nullptr ? &(*depthBuffer)[static_cast<size_t>(y) * proj.width] : nullptr;
			uint8_t *dst = out.at(0, y);
			for (int x = x0; x < x1; x++) {
				if (depthRow != nullptr ? rect.depth > depthRow[x] : zBuffer[x] < rect.depth)
					continue; // behind a wall or building
				const int u0 = columnU[x - x0];
				if (u0 < -1 || u0 >= spriteWidth)
					continue;
				const int s00 = row0[u0];
				const int s10 = row0[u0 + 1];
				const int s01 = row1[u0];
				const int s11 = row1[u0 + 1];
				uint8_t color;
				if (s00 == s10 && s00 == s01 && s00 == s11) {
					if (s00 == 0)
						continue;
					color = lit[s00];
				} else {
					const int tu = columnT[x - x0];
					const std::array<int, 4> samples { s00, s10, s01, s11 };
					const std::array<int, 4> weights { (256 - tu) * (256 - tv), tu * (256 - tv), (256 - tu) * tv, tu * tv };
					int coverage = 0;
					int r = 0;
					int g = 0;
					int b = 0;
					for (size_t k = 0; k < 4; k++) {
						if (samples[k] == 0)
							continue;
						const SDL_Color c = rgb[samples[k]];
						coverage += weights[k];
						r += weights[k] * c.r;
						g += weights[k] * c.g;
						b += weights[k] * c.b;
					}
					if (coverage < 256 * 256 / 2)
						continue;
					color = NearestColor(r / coverage, g / coverage, b / coverage);
				}
				if (fog != nullptr)
					color = fog[color];
				dst[x] = color;
			}
		}
		return;
	}

	for (int x = x0; x < x1; x++) {
		if (depthBuffer == nullptr && zBuffer[x] < rect.depth)
			continue;
		const int u = (x - rect.left) * spriteWidth / screenWidth;
		for (int y = y0; y < y1; y++) {
			const int v = (y - rect.top) * spriteHeight / screenHeight;
			const int16_t pixel = pixels[v * spriteWidth + u];
			// Colour 0 is the baked-in ground shadow, which would stand upright on a billboard.
			if (pixel <= 0)
				continue;
			if (depthBuffer != nullptr && rect.depth > (*depthBuffer)[static_cast<size_t>(y) * proj.width + x])
				continue; // behind a wall or building
			uint8_t color = shade(pixel);
			if (fog != nullptr)
				color = fog[color];
			*out.at(x, y) = color;
		}
	}
}

/**
 * @brief Draws an iso sprite lying on the floor (a corpse) with the exact ground mapping of the original view.
 *
 * The sprite is anchored at the bottom vertex of its tile's diamond. A floor point q (in tiles, relative to that vertex)
 * appeared on screen at ((qx - qy) * 32, (qx + qy) * 16), so each floor pixel of the first-person view looks its colour up
 * through that mapping. Perspective, foreshortening and walls then come out right from any viewpoint.
 */
void DrawFlatSprite(const Surface &out, const Projection &proj, const std::vector<float> &depthBuffer, const Entity &entity)
{
	static std::vector<int16_t> pixels;
	const ClxSprite sprite = *entity.sprite;
	DecodeClx(sprite, pixels);
	const int width = sprite.width();
	const int height = sprite.height();
	const Vec2 anchor { static_cast<float>(entity.tile.x) + 1.0F, static_cast<float>(entity.tile.y) + 1.0F };

	// Screen rectangle that can contain the footprint.
	float minX = 1e30F;
	float maxX = -1e30F;
	float minY = 1e30F;
	float maxY = -1e30F;
	for (const int cornerX : { 0, width }) {
		for (const int cornerY : { 0, height }) {
			const float sx = static_cast<float>(cornerX) - static_cast<float>(width) / 2.0F;
			const float sy = static_cast<float>(cornerY - height);
			const Vec2 world { anchor.x + (sx / 32.0F + sy / 16.0F) / 2.0F, anchor.y + (sy / 16.0F - sx / 32.0F) / 2.0F };
			const float rx = world.x - proj.pos.x;
			const float ry = world.y - proj.pos.y;
			const float depth = std::max(rx * proj.dir.x + ry * proj.dir.y, 0.1F);
			const float lateral = (rx * proj.plane.x + ry * proj.plane.y) / (PlaneLength * PlaneLength);
			minX = std::min(minX, (lateral / depth + 1.0F) * static_cast<float>(proj.width) / 2.0F);
			maxX = std::max(maxX, (lateral / depth + 1.0F) * static_cast<float>(proj.width) / 2.0F);
			const float sy2 = static_cast<float>(proj.horizon) + EyeHeight / depth * proj.focal;
			minY = std::min(minY, sy2);
			maxY = std::max(maxY, sy2);
		}
	}
	const int x0 = std::max(0, static_cast<int>(std::floor(minX)));
	const int x1 = std::min(proj.width - 1, static_cast<int>(std::ceil(maxX)));
	// Looking down hard puts the horizon above the top of the view: the rows still start at 0.
	const int y0 = std::max({ 0, proj.horizon + 1, static_cast<int>(std::floor(minY)) });
	const int y1 = std::min(proj.height - 1, static_cast<int>(std::ceil(maxY)));
	const uint8_t *light = entity.lit ? LightTables[LightAt(entity.tile)].data() : nullptr;
	for (int y = y0; y <= y1; y++) {
		const float depth = EyeHeight / -proj.rowSlope(y); // floor distance for this row
		const int fogStep = FogStep(depth);
		const uint8_t *fog = fogStep > 0 ? FogTables()[fogStep].data() : nullptr;
		for (int x = x0; x <= x1; x++) {
			if (depth >= depthBuffer[static_cast<size_t>(y) * proj.width + x])
				continue; // behind a wall
			const Vec2 ray = proj.columnRay(x);
			const float qx = proj.pos.x + ray.x * depth - anchor.x;
			const float qy = proj.pos.y + ray.y * depth - anchor.y;
			const int px = static_cast<int>(std::floor((qx - qy) * 32.0F + static_cast<float>(width) / 2.0F));
			const int py = static_cast<int>(std::floor((qx + qy) * 16.0F + static_cast<float>(height)));
			if (px < 0 || py < 0 || px >= width || py >= height)
				continue;
			const int16_t pixel = pixels[static_cast<size_t>(py) * width + px];
			if (pixel <= 0)
				continue;
			uint8_t color = static_cast<uint8_t>(pixel);
			if (entity.trn != nullptr)
				color = entity.trn[color];
			else if (light != nullptr)
				color = light[color];
			if (fog != nullptr)
				color = fog[color];
			*out.at(x, y) = color;
		}
	}
}

void DrawEntities(const Surface &out, const Projection &proj, const std::vector<float> &zBuffer, const std::vector<float> *depthBuffer)
{
	static std::vector<Entity> entities;
	static std::vector<std::pair<BillboardRect, const Entity *>> sorted;
	CollectEntities(proj.pos, entities);

	sorted.clear();
	// Things lying on the floor go first; everything standing is drawn over them.
	if (depthBuffer != nullptr) {
		for (const Entity &entity : entities) {
			if (entity.flat && entity.sprite)
				DrawFlatSprite(out, proj, *depthBuffer, entity);
		}
	}
	for (const Entity &entity : entities) {
		if ((entity.flat && depthBuffer != nullptr) || entity.hidden)
			continue;
		const BillboardRect rect = ProjectBillboard(proj, entity);
		if (rect.visible && (entity.sprite || entity.pixels != nullptr))
			sorted.emplace_back(rect, &entity);
	}
	// Far to near, so nearer sprites overwrite farther ones.
	std::sort(sorted.begin(), sorted.end(), [](const auto &a, const auto &b) { return a.first.depth > b.first.depth; });

	for (const auto &[rect, entity] : sorted)
		DrawSpriteBillboard(out, proj, zBuffer, depthBuffer, *entity, rect);
}

void DrawCrosshair(const Surface &out)
{
	const int cx = out.w() / 2;
	const int cy = out.h() / 2;
	for (int i = 3; i <= 7; i++) {
		out.SetPixel({ cx - i, cy }, PAL16_GRAY);
		out.SetPixel({ cx + i, cy }, PAL16_GRAY);
		out.SetPixel({ cx, cy - i }, PAL16_GRAY);
		out.SetPixel({ cx, cy + i }, PAL16_GRAY);
	}
}

/** @brief Temporary readout used while verifying coordinates; remove once M4 is confirmed. */
void DrawDebugInfo(const Surface &out, const Projection &proj)
{
	const Point here { static_cast<int>(std::floor(proj.pos.x)), static_cast<int>(std::floor(proj.pos.y)) };
	const std::string text = StrCat("tile ", here.x, ",", here.y, " light ", static_cast<int>(LightAt(here)), " lvl ", static_cast<int>(leveltype));
	DrawString(out, text, { { 8, 8 }, { out.w() - 16, 20 } });
}

} // namespace

bool IsFirstPersonActive()
{
	return FirstPersonActive;
}

void ToggleFirstPerson()
{
	FirstPersonActive = !FirstPersonActive;
	if (FirstPersonActive)
		ResetViewState();
	else
		ReleaseInput();
}

void DrawFirstPersonView(const Surface &out)
{
	static std::vector<float> zBuffer;
	static std::vector<float> depthBuffer;

	NotePaletteChange();
	PrepareDoorways();
	const Projection proj = GetProjection();
	zBuffer.resize(static_cast<size_t>(proj.width));

	// Debug: FP_PHASE_TIMES logs the average time of each drawing phase every 120 frames.
	static const bool timePhases = std::getenv("FP_PHASE_TIMES") != nullptr;
	static std::array<double, 5> phaseTotals {};
	static int timedFrames = 0;
	auto clock = [] { return std::chrono::steady_clock::now(); };
	auto t0 = clock();
	std::array<double, 5> phase {};
	auto lap = [&](int i) {
		const auto t = clock();
		phase[i] = std::chrono::duration<double, std::milli>(t - t0).count();
		t0 = t;
	};
	depthBuffer.assign(static_cast<size_t>(proj.width) * proj.height, std::numeric_limits<float>::infinity());
	DrawFloorAndCeiling(out, proj, depthBuffer);
	lap(0);
	DrawWalls(out, proj, zBuffer, depthBuffer);
	lap(1);
	if (leveltype == DTYPE_TOWN && BuildingsEnabled())
		DrawBuildings(out, proj, zBuffer, depthBuffer);
	else
		DrawStructures(out, proj, depthBuffer);
	DrawFog(out, proj, depthBuffer);
	lap(2);
	DrawEntities(out, proj, zBuffer, &depthBuffer);
	DrawFirstPersonWeapon(out);
	lap(3);
	if (timePhases) {
		for (size_t i = 0; i < phase.size(); i++)
			phaseTotals[i] += phase[i];
		if (++timedFrames == 120) {
			LogInfo("fp phases (ms): floor {:.2f}, walls {:.2f}, meshes {:.2f}, sprites {:.2f}; {}", phaseTotals[0] / 120, phaseTotals[1] / 120, phaseTotals[2] / 120, phaseTotals[3] / 120, TakeRasterStats(120));
			phaseTotals = {};
			timedFrames = 0;
		}
	}
	if (IsMouseLookActive())
		DrawCrosshair(out);
	DrawDebugInfo(out, proj);
}

} // namespace devilution::fp
