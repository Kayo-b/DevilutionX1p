/**
 * @file fp/fp_dungeon.cpp
 *
 * The clean dungeon (see fp_dungeon.hpp): tile kinds, texture sets, the floor, ceiling and wall passes, and the
 * geometry of pillars and arches handed to the mesh pass.
 */
#include "fp/fp_dungeon.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <unordered_map>
#include <utility>

#include "engine/light_tables.hpp"
#include "engine/palette.h"
#include "fp/fp_buildings.hpp"
#include "fp/fp_doors.hpp"
#include "fp/fp_textures.hpp"
#include "fp/fp_world.hpp"
#include "interfac.h"
#include "levels/dun_tile_data.hpp"
#include "levels/gendung.h"
#include "levels/tile_properties.hpp"
#include "levels/trigs.h"
#include "objects.h"
#include "utils/is_of.hpp"
#include "utils/log.hpp"
#include "utils/str_cat.hpp"

namespace devilution::fp {

namespace {

// --- Proportions (drawn tiles) -----------------------------------------------------------------------------------

/** Doorways: half their width, and the room left above their crown. */
constexpr float DoorHalfWidth = 0.36F;
constexpr float DoorCrown = 2.25F;
/** The lowest an arch's springing goes, and the wall left above an arch's crown. */
constexpr float MinArchSpring = 1.5F;
constexpr float ArchLintel = 0.28F;
/** Walls whose art is lower than this share of the level's walls are drawn as low walls. */
constexpr float LowWallShare = 0.6F;
/** Light levels added to the ceiling, to the undersides of arches and to the inside of doorways. */
constexpr float CeilingDarkness = 3.0F;
constexpr float UndersideDarkness = 1.5F;
constexpr float RevealDarkness = 1.0F;
/** Arches are looked for this many tiles along each axis from their tile. */
constexpr int MaxArchRun = 3;
/** Stairwells: half the opening's width, how far its steps go down or up (drawn tiles), how many, and how much darker the stairwell gets towards its back (light levels). */
constexpr float StairsHalfWidth = 0.42F;
constexpr float StairsDrop = 1.2F;
constexpr float StairsRise = 1.4F;
constexpr int StairsSteps = 6;
constexpr float StairwellDarkness = 2.5F;

/**
 * How a tileset's pillars are shaped: a base block, the shaft, and either a capital block under the ceiling or (the
 * cathedral's, as its art draws them) a thin collar and a block flaring out on top of a shorter shaft, nearly to the
 * tile's width. Arches resting on such a pillar spring from the top of its block, which is of their bricks; over the
 * block a pier goes on up to the ceiling (of the arches' bricks under arches, else of stone).
 */
struct PillarStyle {
	bool round;
	float shaft;   // half width (square) or radius (round)
	float plinth;  // half width of the base block, the capital and the collar
	float base;    // height of the base block
	float capital; // height of the capital block (under the ceiling)
	float top = 0;       // the flared block: the height of its top (0: none, a capital instead)
	float blockFrom = 0; // where it starts
	float blockFoot = 0; // its half width there
	float blockHalf = 0; // its half width at the top, and the pier's over it
	float collar = 0;    // the height of the collar under it
};

PillarStyle PillarStyleFor(dungeon_type type)
{
	switch (type) {
	case DTYPE_CATACOMBS: return { true, 0.22F, 0.27F, 0.24F, 0.22F };
	case DTYPE_CRYPT: return { false, 0.22F, 0.29F, 0.3F, 0.3F };
	default: return { true, 0.2F, 0.27F, 0.3F, 0.0F, 2.0F, 1.42F, 0.23F, 0.46F, 0.1F };
	}
}

/**
 * Blind arches: in the cathedral every stretch of full-height wall with open floor in front has a row of them, as its
 * art draws them along every wall: round-topped, from the top of the base band up, ringed with bricks set on end like
 * voussoirs (the set's `band`), spread evenly along the stretch about `BlindArchSpacing` apart, so they come out whole
 * (FindBlindArches). Drawn flat on the face (cheap), shaded to look sunk in: a rim of arch stone just inside the
 * outline (the sides and the underside of the top one would see), a stone sill, and a back a light level darker, its
 * bricks shifted off the wall's, darker still in the shadow under the top. The spacing; the half width (less where a
 * short stretch leaves too little wall between them: `BlindArchMinPier`); the wall left between the ring and the
 * frieze; the ring's thickness; the rim's and the sill's width; the back's darkness, the shadow's extra darkness at the
 * top and how far down it fades (light levels, drawn tiles); the shift of the back's bricks (tiles).
 */
constexpr float BlindArchSpacing = 1.7F;
constexpr float BlindArchHalfWidth = 0.51F;
constexpr float BlindArchMinPier = 0.2F;
constexpr float BlindArchLintel = 0.1F;
constexpr float BlindArchRing = 0.25F;
constexpr float BlindArchRim = 0.05F;
constexpr float BlindArchBackDarkness = 1.0F;
constexpr float BlindArchShadow = 2.0F;
constexpr float BlindArchShadowDrop = 0.4F;
constexpr float BlindArchBackShift = 0.0625F;

/** Light levels added to the sloping sides of a pillar's stone block (they face a little down; brick ones go on into their arches unshaded). */
constexpr float FlareDarkness = 0.5F;

// --- Ordered dither ------------------------------------------------------------------------------------------------

/** A 4 x 4 Bayer matrix: thresholds 0..15 in a pattern without clumps. */
constexpr std::array<std::array<uint8_t, 4>, 4> Bayer { { { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 } } };

/** Dither offsets in [-0.5, 0.5): one for the light, two (decorrelated) for the texture position. */
inline float DitherLight(int x, int y) { return (static_cast<float>(Bayer[y & 3][x & 3]) + 0.5F) / 16.0F; }
inline float DitherU(int x, int y) { return (static_cast<float>(Bayer[(y + 1) & 3][(x + 2) & 3]) + 0.5F) / 16.0F - 0.5F; }
inline float DitherV(int x, int y) { return (static_cast<float>(Bayer[(y + 3) & 3][(x + 1) & 3]) + 0.5F) / 16.0F - 0.5F; }

inline int LightLevel(float light, int x, int y)
{
	return std::clamp(static_cast<int>(light + DitherLight(x, y)), 0, static_cast<int>(LightsMax));
}

// --- Texture sets --------------------------------------------------------------------------------------------------

struct SourceTexture {
	int width = 0;
	int height = 0;
	float tilesAcross = 1;
	float tilesDown = 1;
	std::vector<uint8_t> rgb;
};

std::unordered_map<std::string, SourceTexture> SourceTextures;
bool SourcesLoaded = false;

void LoadSourceTextures()
{
	if (SourcesLoaded)
		return;
	SourcesLoaded = true;
	FILE *f = OpenFpAsset("FP_DUNGEONTEX", "dungeontex.bin");
	if (f == nullptr) {
		LogInfo("fp: dungeontex.bin not found, the clean dungeon uses plain colours");
		return;
	}
	auto readAll = [&](void *dst, size_t size) { return std::fread(dst, 1, size, f) == size; };
	char magic[4];
	uint32_t count = 0;
	if (!readAll(magic, 4) || std::memcmp(magic, "FPT1", 4) != 0 || !readAll(&count, 4)) {
		std::fclose(f);
		return;
	}
	for (uint32_t i = 0; i < count; i++) {
		uint8_t nameLength;
		if (!readAll(&nameLength, 1))
			break;
		std::string name(nameLength, '\0');
		uint16_t size[2];
		float tiles[2];
		if (!readAll(name.data(), nameLength) || !readAll(size, sizeof(size)) || !readAll(tiles, sizeof(tiles)))
			break;
		SourceTexture tex;
		tex.width = size[0];
		tex.height = size[1];
		tex.tilesAcross = tiles[0];
		tex.tilesDown = tiles[1];
		tex.rgb.resize(static_cast<size_t>(tex.width) * tex.height * 3);
		if (!readAll(tex.rgb.data(), tex.rgb.size()))
			break;
		// The sampler wraps with masks: sizes must be powers of two.
		if ((tex.width & (tex.width - 1)) != 0 || (tex.height & (tex.height - 1)) != 0) {
			LogInfo("fp: texture {} is {}x{}, not a power of two: skipped", name, tex.width, tex.height);
			continue;
		}
		SourceTextures.emplace(std::move(name), std::move(tex));
	}
	std::fclose(f);
	LogInfo("fp: loaded {} dungeon textures", SourceTextures.size());
}

/** A texture of the level's set: its detail levels (mips), each converted to the palette at every light level. */
struct CleanTexture {
	float texelsPerTileU = 64;
	float texelsPerTileV = 64;
	struct Mip {
		int width;
		int height;
		std::vector<uint8_t> rgb;
		std::array<std::vector<uint8_t>, NumLightingLevels> lit;
	};
	std::vector<Mip> mips;
};

struct TextureSet {
	std::array<CleanTexture, static_cast<size_t>(CleanSurface::Count)> surfaces;
	// The bands at the foot (plinth) and the top (frieze) of a wall, as tall as their textures (0: none).
	float baseBand = 0;
	float topBand = 0;
	float bandAcross = 0; // the size of the blind arches' ring texture (`band`: four bricks across, one long down; 0: none)
	float bandDown = 0;
};

TextureSet Textures;
dungeon_type TexturesFor = DTYPE_NONE;
uint32_t TexturesPalette = 0;
uint32_t StablePaletteHash = 0;

const char *SetName(dungeon_type type)
{
	switch (type) {
	case DTYPE_CATACOMBS: return "catacombs";
	case DTYPE_CRYPT: return "crypt";
	default: return "cathedral";
	}
}

constexpr std::array<const char *, static_cast<size_t>(CleanSurface::Count)> SlotNames { "wall", "base", "top", "floor", "ceiling", "pillar", "stone", "arch", "door", "marble", "lid", "iron", "ivory", "marble2", "ivory2", "band" };

/** The palette colours textures may use: the level's ramps (not the near-blacks 112-127, nor colours the level cycles) and the shared ramps 160-254. */
bool IsAllowedColor(int index)
{
	if (index <= 0 || index >= 255)
		return false;
	if (index >= 112 && index < 160)
		return false; // near-blacks that light to red, then the bright UI ramps
	if (leveltype == DTYPE_CRYPT && index < 32)
		return false; // lava, colour cycled
	return true;
}

/** A hash of the palette colours the textures use (the cycled ones left out, so lava does not rebuild them). */
uint32_t HashStablePalette()
{
	uint32_t hash = 2166136261U;
	for (int i = 0; i < 256; i++) {
		if (!IsAllowedColor(i))
			continue;
		const SDL_Color c = logical_palette[i];
		hash = (hash ^ c.r) * 16777619U;
		hash = (hash ^ c.g) * 16777619U;
		hash = (hash ^ c.b) * 16777619U;
	}
	// The light tables too: they decide how dark each level is.
	for (size_t level = 0; level < NumLightingLevels; level += 5)
		for (int i = 160; i < 176; i++)
			hash = (hash ^ LightTables[level][i]) * 16777619U;
	return hash;
}

/** Nearest allowed palette colour of an RGB colour (6 bits a channel), cached. */
class Quantizer {
public:
	void Prepare()
	{
		cache_.assign(1 << 18, Unknown);
		allowed_.clear();
		for (int i = 0; i < 256; i++)
			if (IsAllowedColor(i))
				allowed_.push_back(static_cast<uint8_t>(i));
	}

	uint8_t operator()(int r, int g, int b)
	{
		r = std::clamp(r, 0, 255);
		g = std::clamp(g, 0, 255);
		b = std::clamp(b, 0, 255);
		uint16_t &entry = cache_[static_cast<size_t>((r >> 2) << 12 | (g >> 2) << 6 | (b >> 2))];
		if (entry == Unknown) {
			const int cr = (r & ~3) + 2;
			const int cg = (g & ~3) + 2;
			const int cb = (b & ~3) + 2;
			int best = std::numeric_limits<int>::max();
			entry = 0;
			for (const uint8_t j : allowed_) {
				const SDL_Color o = logical_palette[j];
				const int d = (o.r - cr) * (o.r - cr) * 3 + (o.g - cg) * (o.g - cg) * 6 + (o.b - cb) * (o.b - cb);
				if (d < best) {
					best = d;
					entry = j;
				}
			}
			// Pure black: the darkest colour of all is still black.
			if (r < 4 && g < 4 && b < 4)
				entry = 0;
		}
		return static_cast<uint8_t>(entry);
	}

private:
	static constexpr uint16_t Unknown = 0xFFFF;
	std::vector<uint16_t> cache_;
	std::vector<uint8_t> allowed_;
};

float Luma(const SDL_Color &c)
{
	return 0.30F * static_cast<float>(c.r) + 0.59F * static_cast<float>(c.g) + 0.11F * static_cast<float>(c.b);
}

/**
 * @brief How bright each light level leaves a colour, measured on the game's own light tables: the mean brightness
 * kept by the colours of the level's ramps that its art uses (the brighter half). Textures are darkened by this, so
 * they fade into the dark as the original art and the sprites do.
 */
std::array<float, NumLightingLevels> LightFactors()
{
	std::array<float, NumLightingLevels> factors {};
	for (size_t level = 0; level < NumLightingLevels; level++) {
		double sum = 0;
		int count = 0;
		for (int i = 32; i < 112; i++) {
			if (i % 16 > 8)
				continue; // the part of each ramp the art uses
			const float before = Luma(logical_palette[i]);
			if (before < 30.0F)
				continue;
			sum += Luma(logical_palette[LightTables[level][i]]) / before;
			count++;
		}
		factors[level] = count > 0 ? static_cast<float>(sum / count) : 1.0F - static_cast<float>(level) / LightsMax;
	}
	factors[LightsMax] = 0.0F;
	return factors;
}

/** A plain stand-in texture (no dungeontex.bin): soft noise around a colour. */
SourceTexture PlainTexture(CleanSurface surface)
{
	SourceTexture tex;
	tex.width = 64;
	tex.height = 64;
	tex.tilesAcross = 1;
	tex.tilesDown = 1;
	tex.rgb.resize(64 * 64 * 3);
	const int base = surface == CleanSurface::Ceiling ? 40 : (surface == CleanSurface::Floor ? 55 : 70);
	uint32_t seed = 12345U + static_cast<uint32_t>(surface) * 7919U;
	for (size_t i = 0; i < 64 * 64; i++) {
		seed = seed * 1664525U + 1013904223U;
		const int v = base + static_cast<int>((seed >> 24) % 12);
		tex.rgb[i * 3] = static_cast<uint8_t>(v + 4);
		tex.rgb[i * 3 + 1] = static_cast<uint8_t>(v);
		tex.rgb[i * 3 + 2] = static_cast<uint8_t>(v - 4);
	}
	return tex;
}

void BuildTexture(CleanTexture &out, const SourceTexture &source, Quantizer &quantize, const std::array<float, NumLightingLevels> &factors)
{
	out.texelsPerTileU = static_cast<float>(source.width) / source.tilesAcross;
	out.texelsPerTileV = static_cast<float>(source.height) / source.tilesDown;
	out.mips.clear();
	CleanTexture::Mip first { source.width, source.height, source.rgb, {} };
	out.mips.push_back(std::move(first));
	// Detail levels: each the average of 2 x 2 texels of the one before, down to 4 texels a side.
	while (out.mips.back().width > 4 && out.mips.back().height > 4 && out.mips.size() < 7) {
		const CleanTexture::Mip &above = out.mips.back();
		CleanTexture::Mip mip { above.width / 2, above.height / 2, {}, {} };
		mip.rgb.resize(static_cast<size_t>(mip.width) * mip.height * 3);
		for (int y = 0; y < mip.height; y++)
			for (int x = 0; x < mip.width; x++)
				for (int c = 0; c < 3; c++) {
					int sum = 0;
					for (int dy = 0; dy < 2; dy++)
						for (int dx = 0; dx < 2; dx++)
							sum += above.rgb[(static_cast<size_t>(y * 2 + dy) * above.width + x * 2 + dx) * 3 + c];
					mip.rgb[(static_cast<size_t>(y) * mip.width + x) * 3 + c] = static_cast<uint8_t>((sum + 2) / 4);
				}
		out.mips.push_back(std::move(mip));
	}
	for (CleanTexture::Mip &mip : out.mips) {
		const size_t count = static_cast<size_t>(mip.width) * mip.height;
		for (size_t level = 0; level < NumLightingLevels; level++) {
			std::vector<uint8_t> &lit = mip.lit[level];
			lit.resize(count);
			const float f = factors[level];
			for (size_t i = 0; i < count; i++) {
				lit[i] = f <= 0.0F ? uint8_t { 0 }
				                   : quantize(static_cast<int>(mip.rgb[i * 3] * f + 0.5F), static_cast<int>(mip.rgb[i * 3 + 1] * f + 0.5F), static_cast<int>(mip.rgb[i * 3 + 2] * f + 0.5F));
			}
		}
		mip.rgb.clear();
		mip.rgb.shrink_to_fit();
	}
}

/** Makes sure the level's texture set is built for the current palette. */
void EnsureTextures()
{
	NotePaletteChange();
	StablePaletteHash = HashStablePalette();
	if (TexturesFor == leveltype && TexturesPalette == StablePaletteHash)
		return;
	TexturesFor = leveltype;
	TexturesPalette = StablePaletteHash;
	LoadSourceTextures();
	Quantizer quantize;
	quantize.Prepare();
	const std::array<float, NumLightingLevels> factors = LightFactors();
	LogInfo("fp: light factors {:.2f} {:.2f} {:.2f} {:.2f} {:.2f} {:.2f} {:.2f} {:.2f} {:.2f} {:.2f} {:.2f} {:.2f} {:.2f} {:.2f} {:.2f}", factors[0], factors[1], factors[2], factors[3], factors[4], factors[5], factors[6], factors[7], factors[8], factors[9], factors[10], factors[11], factors[12], factors[13], factors[14]);
	const std::string set = SetName(leveltype);
	for (size_t slot = 0; slot < SlotNames.size(); slot++) {
		const auto surface = static_cast<CleanSurface>(slot);
		const auto find = [&](const char *name) -> const SourceTexture * {
			const auto it = SourceTextures.find(StrCat(set, "/", name));
			return it != SourceTextures.end() ? &it->second : nullptr;
		};
		const SourceTexture *source = find(SlotNames[slot]);
		if (surface == CleanSurface::Base)
			Textures.baseBand = source != nullptr ? source->tilesDown : 0.0F;
		if (surface == CleanSurface::Top)
			Textures.topBand = source != nullptr ? source->tilesDown : 0.0F;
		if (surface == CleanSurface::Band) {
			Textures.bandAcross = source != nullptr ? source->tilesAcross : 0.0F;
			Textures.bandDown = source != nullptr ? source->tilesDown : 0.0F;
		}
		if (source == nullptr && (surface == CleanSurface::Lid || surface == CleanSurface::MarbleAlt))
			source = find("marble");
		if (source == nullptr && surface == CleanSurface::IvoryAlt)
			source = find("ivory");
		if (source == nullptr && (surface == CleanSurface::Stone || surface == CleanSurface::Marble || surface == CleanSurface::Lid || surface == CleanSurface::Ivory || surface == CleanSurface::MarbleAlt || surface == CleanSurface::IvoryAlt))
			source = find("pillar");
		if (source == nullptr && surface != CleanSurface::Base && surface != CleanSurface::Top)
			source = find("wall");
		const SourceTexture plain = PlainTexture(surface);
		BuildTexture(Textures.surfaces[slot], source != nullptr ? *source : plain, quantize, factors);
	}
	LogInfo("fp: clean dungeon textures ready ({} set)", set);
}

/** Samples one texture with the detail level for a pixel's footprint; up close, the sample point is dithered. */
struct Sampler {
	const std::array<std::vector<uint8_t>, NumLightingLevels> *lit = nullptr;
	int maskU = 0;
	int maskV = 0;
	int shift = 0; // log2 of the width
	float scaleU = 1; // texels per tile at this detail level
	float scaleV = 1;
	bool jitter = false;

	void Prepare(const CleanTexture &tex, float worldPerPixel)
	{
		const float footprint = worldPerPixel * tex.texelsPerTileU; // full-size texels per pixel
		int level = 0;
		if (footprint >= 1.5F)
			level = std::min(static_cast<int>(std::log2(footprint / 1.5F)) + 1, static_cast<int>(tex.mips.size()) - 1);
		const CleanTexture::Mip &mip = tex.mips[level];
		lit = &mip.lit;
		maskU = mip.width - 1;
		maskV = mip.height - 1;
		shift = 0;
		while ((1 << shift) < mip.width)
			shift++;
		const float down = static_cast<float>(1 << level);
		scaleU = tex.texelsPerTileU / down;
		scaleV = tex.texelsPerTileV / down;
		jitter = footprint < 1.0F;
	}

	[[nodiscard]] uint8_t At(float u, float v, int level, int x, int y) const
	{
		float tu = u * scaleU;
		float tv = v * scaleV;
		if (jitter) {
			tu += DitherU(x, y);
			tv += DitherV(x, y);
		}
		const int iu = static_cast<int>(std::floor(tu)) & maskU;
		const int iv = static_cast<int>(std::floor(tv)) & maskV;
		return (*lit)[level][static_cast<size_t>(iv << shift | iu)];
	}
};

// --- Tile kinds ----------------------------------------------------------------------------------------------------

struct LevelKey {
	const std::byte *cels;
	int level;
	int type;
	bool isSetLevel;
	int setLevelNum;
	bool operator==(const LevelKey &) const = default;
};

std::vector<CleanKind> Kinds;    // per tile, y * MAXDUNX + x
std::vector<float> WallHeights;  // per tile: drawn height of a wall

/** A blind arch on a stretch of wall (FindBlindArches): its middle along the face (world x or y) and its half width. */
struct BlindArch {
	float middle;
	float halfWidth;
};
std::vector<BlindArch> BlindArches;
/** Per tile and face (its +x, -x, +y, -y side), the (up to two) blind arches it carries some of: into BlindArches, -1 if none. */
std::vector<std::array<int16_t, 2>> BlindArchIndex;

LevelKey KindsFor { nullptr, -1, -1, false, -1 };
uint32_t KindsMap = 0;
uint32_t Version = 1;

uint32_t HashMap()
{
	uint32_t hash = 2166136261U;
	for (int x = 0; x < MAXDUNX; x++) {
		for (int y = 0; y < MAXDUNY; y++) {
			hash = (hash ^ dPiece[x][y]) * 16777619U;
			hash = (hash ^ static_cast<uint8_t>(dSpecial[x][y])) * 16777619U;
		}
	}
	return hash;
}

bool HasLightObject(Point tile)
{
	const int id = dObject[tile.x][tile.y];
	if (id <= 0)
		return false;
	return Objects[id - 1]._otype == OBJ_L1LIGHT;
}

std::vector<CleanStairs> Stairs;
std::vector<int16_t> StairsIndex; // per tile: index into Stairs of the stairs whose opening is in it, -1 if none

/** Whether a trigger is a flight of stairs without a 3D model, and which way it goes. */
bool IsUnmodelledStairs(interface_mode msg, bool &down)
{
	switch (msg) {
	case WM_DIABNEXTLVL:
	case WM_DIABPREVLVL:
		down = msg == WM_DIABNEXTLVL;
		return leveltype != DTYPE_CATHEDRAL; // the cathedral's are 3D models (fp_buildings.cpp)
	case WM_DIABSETLVL:
		down = true;
		return true;
	case WM_DIABRTNLVL:
	case WM_DIABTWARPUP:
		down = false;
		return true;
	default:
		return false;
	}
}

/**
 * @brief Finds the wall each stairs trigger leads into: the solid neighbour of the trigger with the most solid tiles
 * around it (the iso art paints the flight on a block of wall tiles there), opposite an open tile if it can.
 */
void FindStairs()
{
	Stairs.clear();
	StairsIndex.assign(static_cast<size_t>(MAXDUNX) * MAXDUNY, -1);
	for (int i = 0; i < numtrigs; i++) {
		bool down;
		if (!IsUnmodelledStairs(trigs[i]._tmsg, down))
			continue;
		const Point trigger = trigs[i].position;
		Point best { -1, -1 };
		int bestScore = 0;
		for (const Displacement d : { Displacement { 0, -1 }, Displacement { -1, 0 }, Displacement { 1, 0 }, Displacement { 0, 1 } }) {
			const Point tile = trigger + d;
			// A wall, or a lone block in a room (the catacombs' stairs up stand free).
			const CleanKind kind = InDungeonBounds(tile) ? Kinds[static_cast<size_t>(tile.y) * MAXDUNX + tile.x] : CleanKind::Open;
			if (kind != CleanKind::Wall && kind != CleanKind::Pillar)
				continue;
			int score = 1;
			for (int dy = -1; dy <= 1; dy++)
				for (int dx = -1; dx <= 1; dx++)
					if (IsTileSolid(tile + Displacement { dx, dy }))
						score++;
			if (InDungeonBounds(trigger - d) && !IsTileSolid(trigger - d))
				score += 2;
			if (score > bestScore) {
				bestScore = score;
				best = tile;
			}
		}
		if (bestScore == 0)
			continue;
		const size_t at = static_cast<size_t>(best.y) * MAXDUNX + best.x;
		Kinds[at] = CleanKind::Stairs;
		WallHeights[at] = CleanCeiling();
		StairsIndex[at] = static_cast<int16_t>(Stairs.size());
		Stairs.push_back({ trigger, best, down });
	}
}

/** The height (art rows above the floor) of a piece's tallest wall face, 0 if it has no usable face. */
int ArtWallRows(const PieceTextures &textures)
{
	int rows = 0;
	if (textures.faceXUsable)
		rows = std::max(rows, textures.faceXRows);
	if (textures.faceYUsable)
		rows = std::max(rows, textures.faceYRows);
	return rows;
}

/** @brief Lays out the cathedral's blind arches (see BlindArchSpacing) along every stretch of wall face with open floor in front. */
void FindBlindArches()
{
	BlindArches.clear();
	BlindArchIndex.assign(static_cast<size_t>(MAXDUNX) * MAXDUNY * 4, { -1, -1 });
	if (leveltype != DTYPE_CATHEDRAL)
		return;
	const float ceiling = CleanCeiling();
	constexpr std::array<Displacement, 4> Fronts { { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } } };
	for (int face = 0; face < 4; face++) {
		const Displacement front = Fronts[face];
		const bool xFace = front.deltaX != 0; // a face towards x runs along y
		const int lines = xFace ? MAXDUNX : MAXDUNY;
		const int steps = xFace ? MAXDUNY : MAXDUNX;
		for (int line = 0; line < lines; line++) {
			auto tileAt = [&](int step) { return xFace ? Point { line, step } : Point { step, line }; };
			// A full-height wall with open floor in front of this face.
			auto walled = [&](int step) {
				const Point tile = tileAt(step);
				const Point ahead = tile + front;
				if (!InDungeonBounds(ahead))
					return false;
				const size_t at = static_cast<size_t>(tile.y) * MAXDUNX + tile.x;
				return Kinds[at] == CleanKind::Wall && WallHeights[at] >= ceiling - 0.01F && Kinds[static_cast<size_t>(ahead.y) * MAXDUNX + ahead.x] == CleanKind::Open;
			};
			for (int step = 0; step < steps;) {
				if (!walled(step)) {
					step++;
					continue;
				}
				const int start = step;
				while (step < steps && walled(step))
					step++;
				const float length = static_cast<float>(step - start);
				const int count = std::max(1, static_cast<int>(std::lround(length / BlindArchSpacing)));
				const float spacing = length / static_cast<float>(count);
				const float halfWidth = std::min(BlindArchHalfWidth, (spacing - BlindArchMinPier) / 2.0F);
				if (halfWidth < 0.2F)
					continue;
				for (int i = 0; i < count; i++) {
					const float middle = static_cast<float>(start) + (static_cast<float>(i) + 0.5F) * spacing;
					const float reach = halfWidth + BlindArchRing;
					const int first = std::max(start, static_cast<int>(std::floor(middle - reach)));
					const int last = std::min(step - 1, static_cast<int>(std::ceil(middle + reach)) - 1);
					for (int at = first; at <= last; at++) {
						const Point tile = tileAt(at);
						std::array<int16_t, 2> &slots = BlindArchIndex[(static_cast<size_t>(tile.y) * MAXDUNX + tile.x) * 4 + face];
						(slots[0] < 0 ? slots[0] : slots[1]) = static_cast<int16_t>(BlindArches.size());
					}
					BlindArches.push_back({ middle, halfWidth });
				}
			}
		}
	}
}

void ClassifyTiles()
{
	Kinds.assign(static_cast<size_t>(MAXDUNX) * MAXDUNY, CleanKind::Open);
	WallHeights.assign(Kinds.size(), CleanCeiling());
	const float ceiling = CleanCeiling();
	const int levelRows = LevelWallRows();
	auto solid = [](Point p) { return !InDungeonBounds(p) || IsTileSolid(p); };
	int counts[8] {};
	for (int y = 0; y < MAXDUNY; y++) {
		for (int x = 0; x < MAXDUNX; x++) {
			const Point tile { x, y };
			const size_t at = static_cast<size_t>(y) * MAXDUNX + x;
			CleanKind kind = CleanKind::Open;
			if (IsDoorwayTile(tile)) {
				kind = CleanKind::Doorway;
			} else if (IsTileSolid(tile)) {
				const PieceTextures &textures = GetPieceTextures(dPiece[x][y]);
				const bool lone = !solid(tile + Displacement { 1, 0 }) && !solid(tile + Displacement { -1, 0 }) && !solid(tile + Displacement { 0, 1 }) && !solid(tile + Displacement { 0, -1 });
				// The crypt's lava lies beside the floor; flat solid pieces walled in are the inside of thick walls. The
				// cathedral and the catacombs have no lava or water: their flat solid pieces are walls too.
				const bool besideFloor = !solid(tile + Displacement { 1, 0 }) || !solid(tile + Displacement { -1, 0 }) || !solid(tile + Displacement { 0, 1 }) || !solid(tile + Displacement { 0, -1 });
				if (textures.isFlatSolid && besideFloor && leveltype == DTYPE_CRYPT)
					kind = CleanKind::Flat;
				else if (lone && HasLightObject(tile))
					kind = CleanKind::Stand;
				else if (lone)
					kind = CleanKind::Pillar;
				else
					kind = CleanKind::Wall;
				if (kind == CleanKind::Wall) {
					// Low walls (balustrades, tomb boxes) stay low: as tall as their art, to the nearest tenth.
					const int rows = ArtWallRows(textures);
					if (rows > 0 && rows < levelRows * LowWallShare) {
						const float height = static_cast<float>(rows - FaceBaseRows) / SpritePixelsPerTileY * VerticalScale();
						WallHeights[at] = std::clamp(std::round(height * 10.0F) / 10.0F, 0.3F, ceiling);
					}
				}
			}
			Kinds[at] = kind;
			counts[static_cast<int>(kind)]++;
		}
	}
	FindStairs();
	FindBlindArches();
	LogInfo("fp: clean dungeon tiles: {} walls, {} pillars, {} torch stands, {} doorways, {} stairs, {} blind arches", counts[1], counts[2], counts[3], counts[4], Stairs.size(), BlindArches.size());
}

void EnsureKinds()
{
	const LevelKey key { pDungeonCels.get(), currlevel, static_cast<int>(leveltype), setlevel, static_cast<int>(setlvlnum) };
	PrepareDoorways();
	const uint32_t map = HashMap();
	if (key == KindsFor && map == KindsMap && !Kinds.empty())
		return;
	KindsFor = key;
	KindsMap = map;
	Version++;
	ClassifyTiles();
}

/** Brings the kinds up to date with the level (not its map: PrepareCleanDungeon does that once a frame). */
void EnsureKindsForLevel()
{
	const LevelKey key { pDungeonCels.get(), currlevel, static_cast<int>(leveltype), setlevel, static_cast<int>(setlvlnum) };
	if (Kinds.empty() || !(key == KindsFor))
		EnsureKinds();
}

CleanKind KindAt(Point tile)
{
	if (!InDungeonBounds(tile))
		return CleanKind::Wall;
	return Kinds[static_cast<size_t>(tile.y) * MAXDUNX + tile.x];
}

bool IsSupport(CleanKind kind)
{
	return kind == CleanKind::Wall || kind == CleanKind::Doorway || kind == CleanKind::Stairs || kind == CleanKind::Pillar || kind == CleanKind::Stand;
}

// --- Lighting ------------------------------------------------------------------------------------------------------

inline float TileLight(int x, int y)
{
	if (x < 0 || y < 0 || x >= MAXDUNX || y >= MAXDUNY)
		return LightsMax;
	return static_cast<float>(dLight[x][y]);
}

/**
 * The light at the four tile centres around the last point CleanLightAt looked up: kept while the points stay between
 * them (they are looked up pixel by pixel), forgotten every frame.
 */
struct LightCell {
	int x = std::numeric_limits<int>::min();
	int y = 0;
	float l00 = 0;
	float l10 = 0;
	float l01 = 0;
	float l11 = 0;
};
LightCell LastLightCell;

/** The light of a wall face at a point along it: interpolated along the open tiles in front of it. */
float FaceLight(const RayHit &hit, const Vec2 &ray)
{
	// A little in front of the face, so the solid tile's own (similar) light counts less.
	const float back = 0.05F;
	const float len = std::sqrt(ray.x * ray.x + ray.y * ray.y);
	return CleanLightAt(hit.point.x - ray.x / len * back, hit.point.y - ray.y / len * back);
}

// --- Walls ---------------------------------------------------------------------------------------------------------

/** The texture and its position for a point of a wall's face at height h (drawn tiles) on a wall `top` high. */
inline CleanSurface WallBand(float h, float top, float ceiling, float &v)
{
	// Within a band, v runs down from its top edge (kept off its bottom edge, which would wrap to the top row).
	if (h < Textures.baseBand) {
		v = std::max(Textures.baseBand - h, 0.0F) * 0.999F;
		return CleanSurface::Base;
	}
	if (top >= ceiling - 0.01F && h > ceiling - Textures.topBand) {
		v = std::max(ceiling - h, 0.0F);
		return CleanSurface::Top;
	}
	v = -h;
	return CleanSurface::Wall;
}

struct ColumnSamplers {
	std::array<Sampler, static_cast<size_t>(CleanSurface::Count)> s;
	void Prepare(float worldPerPixel, std::initializer_list<CleanSurface> which)
	{
		for (const CleanSurface surface : which)
			s[static_cast<size_t>(surface)].Prepare(Textures.surfaces[static_cast<size_t>(surface)], worldPerPixel);
	}
	const Sampler &operator[](CleanSurface surface) const { return s[static_cast<size_t>(surface)]; }
};

} // namespace

// --- Public --------------------------------------------------------------------------------------------------------

bool IsCleanDungeon()
{
	return IsAnyOf(leveltype, DTYPE_CATHEDRAL, DTYPE_CATACOMBS, DTYPE_CRYPT);
}

float CleanCeiling()
{
	return WallHeight() * VerticalScale();
}

void PrepareCleanDungeon()
{
	EnsureKinds();
	EnsureTextures();
	LastLightCell.x = std::numeric_limits<int>::min(); // the light moves between frames
}

CleanKind CleanKindAt(Point tile)
{
	EnsureKindsForLevel();
	return KindAt(tile);
}

float CleanWallHeight(Point tile)
{
	EnsureKindsForLevel();
	if (!InDungeonBounds(tile))
		return CleanCeiling();
	return WallHeights[static_cast<size_t>(tile.y) * MAXDUNX + tile.x];
}

uint32_t CleanDungeonVersion()
{
	return Version * 2654435761U ^ TexturesPalette;
}

/** The arched opening of a stairwell: wide, its crown under the ceiling. */
CleanOpening StairsOpening()
{
	return { StairsHalfWidth, CleanCeiling() - ArchLintel - StairsHalfWidth };
}

const std::vector<CleanStairs> &CleanStairsList()
{
	EnsureKindsForLevel();
	return Stairs;
}

CleanOpening CleanDoorOpening()
{
	const float crown = std::min(DoorCrown, CleanCeiling() - 0.45F);
	return { DoorHalfWidth, crown - DoorHalfWidth };
}

float CleanOpeningTop(const CleanOpening &opening, float along)
{
	const float a = along - 0.5F;
	if (std::abs(a) >= opening.halfWidth)
		return 0.0F;
	return opening.spring + std::sqrt(opening.halfWidth * opening.halfWidth - a * a);
}

float CleanLightAt(float x, float y)
{
	const float fx = x - 0.5F;
	const float fy = y - 0.5F;
	const int ix = static_cast<int>(std::floor(fx));
	const int iy = static_cast<int>(std::floor(fy));
	if (ix != LastLightCell.x || iy != LastLightCell.y)
		LastLightCell = { ix, iy, TileLight(ix, iy), TileLight(ix + 1, iy), TileLight(ix, iy + 1), TileLight(ix + 1, iy + 1) };
	const LightCell &c = LastLightCell;
	const float tx = fx - static_cast<float>(ix);
	const float ty = fy - static_cast<float>(iy);
	const float top = c.l00 + (c.l10 - c.l00) * tx;
	const float bottom = c.l01 + (c.l11 - c.l01) * tx;
	return top + (bottom - top) * ty;
}

uint8_t CleanColor(CleanSurface surface, float u, float v, float worldPerPixel, float light, int x, int y)
{
	// Called for every pixel of a mesh: the sampler is kept while the surface and its detail level stay the same.
	static Sampler sampler;
	static CleanSurface lastSurface = CleanSurface::Count;
	static int lastBucket = -1;
	static uint32_t lastVersion = 0;
	const CleanTexture &tex = Textures.surfaces[static_cast<size_t>(surface)];
	const float footprint = worldPerPixel * tex.texelsPerTileU;
	// Detail level, and on the finest one whether the sample point is jittered: the buckets Sampler::Prepare tells apart.
	const int bucket = footprint < 1.0F ? 0 : (footprint < 1.5F ? 1 : 2 + std::ilogb(footprint / 1.5F));
	if (surface != lastSurface || bucket != lastBucket || lastVersion != TexturesPalette) {
		lastSurface = surface;
		lastBucket = bucket;
		lastVersion = TexturesPalette;
		sampler.Prepare(tex, worldPerPixel);
	}
	return sampler.At(u, v, LightLevel(light, x, y), x, y);
}

void DrawCleanFloorAndCeiling(const Surface &out, const Projection &proj, std::vector<float> &depthBuffer)
{
	const Vec2 rayLeft { proj.dir.x - proj.plane.x, proj.dir.y - proj.plane.y };
	const Vec2 rayRight { proj.dir.x + proj.plane.x, proj.dir.y + proj.plane.y };
	const float ceiling = CleanCeiling();
	const bool holes = HasFloorHoles();
	const CleanTexture &floorTex = Textures.surfaces[static_cast<size_t>(CleanSurface::Floor)];
	const CleanTexture &ceilingTex = Textures.surfaces[static_cast<size_t>(CleanSurface::Ceiling)];
	for (int y = 0; y < proj.height; y++) {
		const bool isFloor = y > proj.horizon;
		uint8_t *dst = out.at(0, y);
		const int rowsFromHorizon = isFloor ? y - proj.horizon : proj.horizon - y;
		if (rowsFromHorizon == 0) {
			std::fill_n(dst, proj.width, 0);
			continue;
		}
		const float heightAboveOrBelow = isFloor ? EyeHeight : ceiling - EyeHeight;
		const float rowDistance = heightAboveOrBelow * proj.focal / static_cast<float>(rowsFromHorizon);
		const float stepX = rowDistance * (rayRight.x - rayLeft.x) / static_cast<float>(proj.width);
		const float stepY = rowDistance * (rayRight.y - rayLeft.y) / static_cast<float>(proj.width);
		float wx = proj.pos.x + rowDistance * rayLeft.x;
		float wy = proj.pos.y + rowDistance * rayLeft.y;
		// The detail level: the footprint across (one pixel) and in depth (where the rows bunch up), averaged.
		const float across = rowDistance / proj.focal;
		const float deep = rowDistance * rowDistance / (heightAboveOrBelow * proj.focal);
		Sampler sampler;
		sampler.Prepare(isFloor ? floorTex : ceilingTex, std::sqrt(across * deep));
		const float extraDark = isFloor ? 0.0F : CeilingDarkness;
		float *depthRow = &depthBuffer[static_cast<size_t>(y) * proj.width];
		const float depth = rowDistance * 1.03F; // see FloorDepthBias in fp_view.cpp
		// The light is interpolated between tile centres: the four around a point change only every so often.
		int cellX = std::numeric_limits<int>::min();
		int cellY = 0;
		float l00 = 0;
		float l10 = 0;
		float l01 = 0;
		float l11 = 0;
		Point cachedTile { -1, -1 };
		bool flat = false;
		const std::vector<int16_t> *flatArt = nullptr;
		for (int x = 0; x < proj.width; x++, wx += stepX, wy += stepY) {
			const int tx = static_cast<int>(std::floor(wx));
			const int ty = static_cast<int>(std::floor(wy));
			if (tx < 0 || ty < 0 || tx >= MAXDUNX || ty >= MAXDUNY) {
				dst[x] = 0;
				continue;
			}
			if (holes && isFloor && IsFloorHole(wx, wy)) {
				dst[x] = 0; // the pit of the stairs down: drawn into by its mesh
				continue;
			}
			const float fx = wx - 0.5F;
			const float fy = wy - 0.5F;
			const int cx = static_cast<int>(std::floor(fx));
			const int cy = static_cast<int>(std::floor(fy));
			if (cx != cellX || cy != cellY) {
				cellX = cx;
				cellY = cy;
				l00 = TileLight(cx, cy);
				l10 = TileLight(cx + 1, cy);
				l01 = TileLight(cx, cy + 1);
				l11 = TileLight(cx + 1, cy + 1);
			}
			const float ax = fx - static_cast<float>(cx);
			const float ay = fy - static_cast<float>(cy);
			const float top = l00 + (l10 - l00) * ax;
			const float bottom = l01 + (l11 - l01) * ax;
			const float light = top + (bottom - top) * ay + extraDark;
			if (isFloor) {
				if (tx != cachedTile.x || ty != cachedTile.y) {
					cachedTile = { tx, ty };
					flat = Kinds[static_cast<size_t>(ty) * MAXDUNX + tx] == CleanKind::Flat;
					flatArt = flat ? &GetPieceTextures(dPiece[tx][ty]).floor : nullptr;
				}
				depthRow[x] = depth;
				if (flat && flatArt != nullptr && flatArt->size() == static_cast<size_t>(FloorTextureSize) * FloorTextureSize) {
					// Lava and water keep their art (and its colour cycling).
					const int u = std::min(static_cast<int>((wx - static_cast<float>(tx)) * FloorTextureSize), FloorTextureSize - 1);
					const int v = std::min(static_cast<int>((wy - static_cast<float>(ty)) * FloorTextureSize), FloorTextureSize - 1);
					const int16_t texel = (*flatArt)[static_cast<size_t>(v) * FloorTextureSize + u];
					dst[x] = texel <= 0 ? 0 : LightTables[LightLevel(light, x, y)][texel];
					continue;
				}
			}
			dst[x] = sampler.At(wx, wy, LightLevel(light, x, y), x, y);
		}
	}
}

/** @brief atan2(y, x) for y >= 0, to within about 0.005 rad: cheap enough per pixel. */
inline float HalfTurnAngle(float x, float y)
{
	const float ax = std::abs(x);
	const float t = std::min(ax, y) / std::max({ ax, y, 1e-12F });
	const float t2 = t * t;
	float a = t * (0.99997726F + t2 * (-0.33262347F + t2 * (0.19354346F + t2 * (-0.11643287F + t2 * (0.05265332F - t2 * 0.01172120F)))));
	if (y > ax)
		a = 1.57079633F - a;
	return x < 0.0F ? 3.14159265F - a : a;
}

void DrawCleanWalls(const Surface &out, const Projection &proj, std::vector<float> &zBuffer, std::vector<float> &depthBuffer)
{
	const float ceiling = CleanCeiling();
	const CleanOpening doorShape = CleanDoorOpening();
	const CleanOpening stairsShape = StairsOpening();
	static std::vector<uint8_t> filled;
	filled.resize(static_cast<size_t>(proj.height));
	ColumnSamplers samplers;
	for (int x = 0; x < proj.width; x++) {
		const Vec2 ray = proj.columnRay(x);
		std::fill(filled.begin(), filled.end(), uint8_t { 0 });
		zBuffer[x] = MaxRayDistance;
		for (int layer = 0; layer < MaxWallLayers; layer++) {
			const RayHit hit = CastRay(proj.pos, ray, layer);
			if (!hit.hit)
				break;
			if (layer == 0)
				zBuffer[x] = hit.distance;
			const CleanKind kind = KindAt(hit.tile);
			const float d = std::max(hit.distance, 0.01F);
			const float wallTop = kind == CleanKind::Wall ? WallHeights[static_cast<size_t>(hit.tile.y) * MAXDUNX + hit.tile.x] : ceiling;
			const bool low = wallTop < ceiling - 0.01F;
			const float scale = proj.focal / d;
			const float topF = static_cast<float>(proj.horizon) - (wallTop - EyeHeight) * scale;
			const float bottomF = static_cast<float>(proj.horizon) + EyeHeight * scale;
			const int top = std::max(0, static_cast<int>(std::ceil(topF - 0.5F)));
			const int bottom = std::min(proj.height - 1, static_cast<int>(bottomF));
			// Along the face in world order (so textures run on from tile to tile), and within the tile.
			const float worldAlong = hit.xSide ? hit.point.y : hit.point.x;
			const float along = worldAlong - std::floor(worldAlong);
			const float light = InDungeonBounds(hit.tile) ? FaceLight(hit, ray) : static_cast<float>(LightsMax);
			const float worldPerPixel = d / proj.focal;
			samplers.Prepare(worldPerPixel, { CleanSurface::Wall, CleanSurface::Base, CleanSurface::Top });
			// An opening through this face: a doorway (through both broad faces of a door tile), or the stairwell
			// beside a stairs trigger (through the face towards it, closed at the back).
			const CleanOpening *opening = nullptr;
			bool closedBack = false;
			if (kind == CleanKind::Doorway) {
				const DoorwaySpan span = DoorwaySpanAt(hit.tile);
				if (span.door != nullptr && span.offset == 0 && hit.xSide != span.door->alongX)
					opening = &doorShape;
			} else if (kind == CleanKind::Stairs) {
				const int16_t index = StairsIndex[static_cast<size_t>(hit.tile.y) * MAXDUNX + hit.tile.x];
				if (index >= 0 && Stairs[index].trigger == hit.previous) {
					opening = &stairsShape;
					closedBack = true;
				}
			}
			const float openingTop = opening != nullptr ? CleanOpeningTop(*opening, along) : 0.0F;
			const float rayAlong = hit.xSide ? ray.y : ray.x;
			const float rayAcross = std::abs(hit.xSide ? ray.x : ray.y);
			bool lookOn = low;
			// The blind arch on this face nearest this column, if any (see BlindArchSpacing), from the base band up.
			const BlindArch *arch = nullptr;
			if (kind == CleanKind::Wall && !low && WallRecessesEnabled() && InDungeonBounds(hit.tile) && !BlindArchIndex.empty()) {
				const int face = hit.xSide ? (hit.previous.x > hit.tile.x ? 0 : 1) : (hit.previous.y > hit.tile.y ? 2 : 3);
				for (const int16_t slot : BlindArchIndex[(static_cast<size_t>(hit.tile.y) * MAXDUNX + hit.tile.x) * 4 + face])
					if (slot >= 0 && (arch == nullptr || std::abs(worldAlong - BlindArches[slot].middle) < std::abs(worldAlong - arch->middle)))
						arch = &BlindArches[slot];
			}
			const bool archFace = arch != nullptr;
			const float archHalf = archFace ? arch->halfWidth : 0.0F;
			const float archBottom = Textures.baseBand;
			const float archSpring = ceiling - Textures.topBand - BlindArchLintel - BlindArchRing - archHalf;
			const float archA = archFace ? worldAlong - arch->middle : 0.0F; // across the arch, from its middle
			const float archR2 = archHalf * archHalf;
			const float innerR2 = (archHalf - BlindArchRim) * (archHalf - BlindArchRim);
			const float ringR2 = (archHalf + BlindArchRing) * (archHalf + BlindArchRing);
			// This column's part of it, by height: the arch up to `archTopH`, its rim from `rimFromH` (a column in a side's
			// rim is rim all the way down), the ring from `ringFromH` to `ringToH`.
			const float aa = archA * archA;
			const bool archColumn = archFace && aa < archR2;
			const float archTopH = archColumn ? archSpring + std::sqrt(archR2 - aa) : 0.0F;
			const float rimFromH = archColumn && aa < innerR2 ? archSpring + std::sqrt(innerR2 - aa) : archBottom;
			const bool ringColumn = archFace && aa < ringR2;
			const float ringFromH = archColumn ? archTopH : archSpring;
			const float ringToH = ringColumn ? archSpring + std::sqrt(ringR2 - aa) : 0.0F;
			// The ring's bricks: as many whole ones as fit round its middle line, each as wide as the band's.
			const float bandBrick = Textures.bandAcross / 4.0F;
			const float voussoirs = bandBrick > 0.0F ? std::max(1.0F, std::round(3.14159265F * (archHalf + BlindArchRing / 2.0F) / bandBrick)) : 0.0F;
			// Where a point `ah` above the springing in this column is on the ring's band: along it (tiles of the band
			// texture), and down it (0 its outer edge, 1 its inner).
			auto ringAt = [&](float ah) {
				const float angle = HalfTurnAngle(archA, std::max(ah, 0.0F)); // 0 at the right springing, pi at the left
				const float r = std::sqrt(aa + ah * ah);
				return std::pair { angle / 3.14159265F * voussoirs * bandBrick, (archHalf + BlindArchRing - r) / BlindArchRing };
			};
			constexpr int RingStep = 8;
			int ringNext = -1;
			int ringFrom = 0;
			float ringU = 0;
			float ringV = 0;
			float ringDu = 0;
			float ringDv = 0;
			if (archFace)
				samplers.Prepare(worldPerPixel, { CleanSurface::Arch, CleanSurface::Stone, CleanSurface::Band });
			for (int y = top; y <= bottom; y++) {
				if (filled[y] != 0)
					continue;
				const float slope = static_cast<float>(proj.horizon - y) / proj.focal;
				const float h = EyeHeight + slope * d;
				const float ah = h - archSpring; // above the springing
				if (archColumn && h > archBottom && h < archTopH) {
					// Inside the arch: its rim, its sill, or its back in the shadow under the top.
					uint8_t color;
					if (h >= rimFromH)
						color = samplers[CleanSurface::Arch].At(worldAlong, -h, LightLevel(light + (ah > 0.0F ? UndersideDarkness : RevealDarkness), x, y), x, y);
					else if (h < archBottom + BlindArchRim)
						color = samplers[CleanSurface::Stone].At(worldAlong, -h, LightLevel(light, x, y), x, y);
					else {
						const float shadow = BlindArchShadow * std::max(0.0F, 1.0F - (rimFromH - h) / BlindArchShadowDrop);
						color = samplers[CleanSurface::Wall].At(worldAlong + BlindArchBackShift, -h, LightLevel(light + BlindArchBackDarkness + shadow, x, y), x, y);
					}
					*out.at(x, y) = color;
					filled[y] = 1;
					depthBuffer[static_cast<size_t>(y) * proj.width + x] = d;
					continue;
				}
				if (ringColumn && h >= ringFromH && h < ringToH && voussoirs > 0.0F) {
					// The ring: bricks on end set round the top, each pointing at its middle. Where they are on the band
					// is worked out every RingStep rows and drawn straight in between (over a few rows the ring is).
					if (y >= ringNext) {
						const float h1 = EyeHeight + static_cast<float>(proj.horizon - y - RingStep) / proj.focal * d;
						const auto [u0, v0] = ringAt(ah);
						const auto [u1, v1] = ringAt(h1 - archSpring);
						ringU = u0;
						ringV = v0;
						ringDu = (u1 - u0) / RingStep;
						ringDv = (v1 - v0) / RingStep;
						ringFrom = y;
						ringNext = y + RingStep;
					}
					const float step = static_cast<float>(y - ringFrom);
					const float v = std::clamp(ringV + ringDv * step, 0.0F, 0.999F) * Textures.bandDown;
					*out.at(x, y) = samplers[CleanSurface::Band].At(ringU + ringDu * step, v, LightLevel(light, x, y), x, y);
					filled[y] = 1;
					depthBuffer[static_cast<size_t>(y) * proj.width + x] = d;
					continue;
				}
				if (opening != nullptr && h < openingTop && h > 0.0F) {
					// Inside the opening: follow the ray through the wall's thickness to where it leaves the opening.
					const float back = rayAcross > 1e-5F ? 1.0F / rayAcross : 1e9F;
					float exit = back;
					int how = 0; // 0 out at the back, 1 a side, 2 the top, 3 the floor
					if (rayAlong > 1e-6F) {
						const float t = (0.5F + opening->halfWidth - along) / rayAlong;
						if (t < exit) {
							exit = t;
							how = 1;
						}
					} else if (rayAlong < -1e-6F) {
						const float t = (0.5F - opening->halfWidth - along) / rayAlong;
						if (t < exit) {
							exit = t;
							how = 1;
						}
					}
					if (slope < 0.0F) {
						const float t = h / -slope;
						if (t < exit) {
							exit = t;
							how = 3;
						}
					} else if (slope > 0.0F) {
						// The arched top: where the ray, above the springing, leaves the half circle.
						const float a0 = along - 0.5F;
						const float h0 = h - opening->spring;
						const float qa = rayAlong * rayAlong + slope * slope;
						const float qb = 2.0F * (a0 * rayAlong + h0 * slope);
						const float qc = a0 * a0 + h0 * h0 - opening->halfWidth * opening->halfWidth;
						const float disc = qb * qb - 4.0F * qa * qc;
						if (disc >= 0.0F) {
							const float t = (-qb + std::sqrt(disc)) / (2.0F * qa);
							const float springAt = std::max(0.0F, -h0 / slope);
							if (t >= springAt && t < exit) {
								exit = t;
								how = 2;
							}
						}
					}
					if (how == 3 || (how == 0 && !closedBack)) {
						lookOn = true; // through it, or down onto the floor in it (drawn already, or a stairwell's pit)
						continue;
					}
					const float t = d + exit;
					const Vec2 p { proj.pos.x + ray.x * t, proj.pos.y + ray.y * t };
					const float hh = EyeHeight + slope * t;
					uint8_t color;
					if (how == 0) {
						// The back of a stairwell: the wall, in the dark.
						Sampler sampler;
						sampler.Prepare(Textures.surfaces[static_cast<size_t>(CleanSurface::Wall)], t / proj.focal);
						color = sampler.At(hit.xSide ? p.y : p.x, -hh, LightLevel(CleanLightAt(p.x, p.y) + StairwellDarkness, x, y), x, y);
					} else {
						// The sides and the top of the opening, faced with the arch stone across the wall's thickness.
						const float into = hit.xSide ? p.x : p.y;
						const float v = how == 1 ? -hh : (hit.xSide ? p.y : p.x);
						const float revealLight = CleanLightAt(p.x, p.y) + (how == 2 ? UndersideDarkness : RevealDarkness) + (closedBack ? StairwellDarkness * exit / back : 0.0F);
						Sampler sampler;
						sampler.Prepare(Textures.surfaces[static_cast<size_t>(CleanSurface::Arch)], t / proj.focal);
						color = sampler.At(into, v, LightLevel(revealLight, x, y), x, y);
					}
					*out.at(x, y) = color;
					filled[y] = 1;
					depthBuffer[static_cast<size_t>(y) * proj.width + x] = t;
					continue;
				}
				float v;
				const CleanSurface band = WallBand(h, wallTop, ceiling, v);
				*out.at(x, y) = samplers[band].At(worldAlong, v, LightLevel(light, x, y), x, y);
				filled[y] = 1;
				depthBuffer[static_cast<size_t>(y) * proj.width + x] = d;
			}
			if (low && wallTop < EyeHeight) {
				// The top of a low wall, seen from above, between its face and where the ray leaves the tile.
				const float ex = ray.x > 0 ? (static_cast<float>(hit.tile.x + 1) - proj.pos.x) / ray.x : (ray.x < 0 ? (static_cast<float>(hit.tile.x) - proj.pos.x) / ray.x : 1e9F);
				const float ey = ray.y > 0 ? (static_cast<float>(hit.tile.y + 1) - proj.pos.y) / ray.y : (ray.y < 0 ? (static_cast<float>(hit.tile.y) - proj.pos.y) / ray.y : 1e9F);
				const float leave = std::min(ex, ey);
				const int from = std::max(0, static_cast<int>(std::ceil(static_cast<float>(proj.horizon) + (EyeHeight - wallTop) * proj.focal / std::max(leave, 0.01F) - 0.5F)));
				const int to = std::min(proj.height - 1, top - 1);
				Sampler capSampler;
				capSampler.Prepare(Textures.surfaces[static_cast<size_t>(CleanSurface::Stone)], (d + leave) * 0.5F / proj.focal);
				for (int y = from; y <= to; y++) {
					if (filled[y] != 0)
						continue;
					const float slope = static_cast<float>(proj.horizon - y) / proj.focal;
					if (slope >= 0.0F)
						continue;
					const float t = (wallTop - EyeHeight) / slope;
					const Vec2 p { proj.pos.x + ray.x * t, proj.pos.y + ray.y * t };
					*out.at(x, y) = capSampler.At(p.x, p.y, LightLevel(CleanLightAt(p.x, p.y), x, y), x, y);
					filled[y] = 1;
					depthBuffer[static_cast<size_t>(y) * proj.width + x] = t;
				}
			}
			if (!lookOn)
				break;
		}
	}
}

void BuildCleanGeometry(std::vector<CleanQuad> &quads, std::vector<Point> &stands, std::vector<std::array<float, 4>> &holes)
{
	EnsureKindsForLevel();
	quads.clear();
	stands.clear();
	holes.clear();
	const float ceiling = CleanCeiling();
	const PillarStyle style = PillarStyleFor(leveltype);
	using P3 = std::array<float, 3>;
	// A vertical quad from (a, z0) to (b, z1), facing `normal`; its texture runs along the horizontal axis `axis`
	// (0: x, 1: y; -1: along the quad, for slanted sides) and down from the top.
	auto addWall = [&](P3 a, P3 b, float z0, float z1, P3 normal, CleanSurface surface, int axis, float uStart = 0) {
		const P3 p0 { a[0], a[1], z0 };
		const P3 p1 { b[0], b[1], z0 };
		const P3 p2 { b[0], b[1], z1 };
		const P3 p3 { a[0], a[1], z1 };
		float ua;
		float ub;
		if (axis >= 0) {
			ua = a[axis];
			ub = b[axis];
		} else {
			ua = uStart;
			ub = uStart + std::hypot(b[0] - a[0], b[1] - a[1]);
		}
		quads.push_back({ { p0, p1, p2, p3 }, normal, { { { ua, -z0 }, { ub, -z0 }, { ub, -z1 }, { ua, -z1 } } }, surface });
	};
	auto addFlat = [&](float x0, float y0, float x1, float y1, float z, bool up, CleanSurface surface) {
		if (up)
			quads.push_back({ { P3 { x0, y0, z }, P3 { x1, y0, z }, P3 { x1, y1, z }, P3 { x0, y1, z } }, P3 { 0, 0, 1 }, { { { x0, y0 }, { x1, y0 }, { x1, y1 }, { x0, y1 } } }, surface });
		else
			quads.push_back({ { P3 { x0, y0, z }, P3 { x0, y1, z }, P3 { x1, y1, z }, P3 { x1, y0, z } }, P3 { 0, 0, -1 }, { { { x0, y0 }, { x0, y1 }, { x1, y1 }, { x1, y0 } } }, surface });
	};
	// A square block around (cx, cy) widening from half width h0 at z0 to h1 at z1: its four sloping sides, its top if
	// `top` and its underside. The texture runs along each side's horizontal axis and down from the top.
	auto addFlare = [&](float cx, float cy, float h0, float h1, float z0, float z1, CleanSurface surface, bool top, float darkness) {
		const float slope = (h1 - h0) / (z1 - z0);
		const float len = std::hypot(1.0F, slope);
		const float nz = -slope / len;
		// Each side: its outward direction (x, y) and the corner it starts from, turning counter-clockwise.
		for (const auto &[dx, dy] : { std::pair { 0.0F, 1.0F }, std::pair { 0.0F, -1.0F }, std::pair { 1.0F, 0.0F }, std::pair { -1.0F, 0.0F } }) {
			const int axis = dx == 0 ? 0 : 1; // the horizontal axis along the side
			// Along the side: from -h to +h (counter-clockwise seen from outside).
			const float sx = -dy;
			const float sy = dx;
			auto corner = [&](float h, float along, float z) { return P3 { cx + dx * h + sx * along * h, cy + dy * h + sy * along * h, z }; };
			const P3 p0 = corner(h0, -1, z0);
			const P3 p1 = corner(h0, 1, z0);
			const P3 p2 = corner(h1, 1, z1);
			const P3 p3 = corner(h1, -1, z1);
			quads.push_back({ { p0, p1, p2, p3 }, P3 { dx / len, dy / len, nz }, { { { p0[axis], -z0 }, { p1[axis], -z0 }, { p2[axis], -z1 }, { p3[axis], -z1 } } }, surface, darkness });
		}
		if (top)
			addFlat(cx - h1, cy - h1, cx + h1, cy + h1, z1, true, surface);
		addFlat(cx - h0, cy - h0, cx + h0, cy + h0, z0, false, surface);
	};
	// An axis-aligned box's four sides, its top if `top`, its underside if `under`.
	auto addBox = [&](float cx, float cy, float half, float z0, float z1, CleanSurface surface, bool top, bool under) {
		const float x0 = cx - half;
		const float x1 = cx + half;
		const float y0 = cy - half;
		const float y1 = cy + half;
		addWall({ x0, y1, 0 }, { x1, y1, 0 }, z0, z1, { 0, 1, 0 }, surface, 0);
		addWall({ x1, y0, 0 }, { x0, y0, 0 }, z0, z1, { 0, -1, 0 }, surface, 0);
		addWall({ x1, y1, 0 }, { x1, y0, 0 }, z0, z1, { 1, 0, 0 }, surface, 1);
		addWall({ x0, y0, 0 }, { x0, y1, 0 }, z0, z1, { -1, 0, 0 }, surface, 1);
		if (top)
			addFlat(x0, y0, x1, y1, z1, true, surface);
		if (under)
			addFlat(x0, y0, x1, y1, z0, false, surface);
	};

	for (int y = 0; y < MAXDUNY; y++)
		for (int x = 0; x < MAXDUNX; x++)
			if (Kinds[static_cast<size_t>(y) * MAXDUNX + x] == CleanKind::Stand)
				stands.push_back({ x, y });
	// A pillar, given the sides arches rest on it (1 +x, 2 -x, 4 +y, 8 -y): a plinth, the shaft, and a capital under
	// the ceiling, or a collar, a flared block and a pier up to the ceiling (but its sides inside an arch's masonry).
	auto addPillar = [&](float cx, float cy, uint8_t arches) {
		addBox(cx, cy, style.plinth, 0.0F, style.base, CleanSurface::Stone, true, false);
		float shaftTop = ceiling - style.capital;
		if (style.top > 0) {
			shaftTop = style.blockFrom - style.collar;
			addBox(cx, cy, style.plinth, shaftTop, style.blockFrom, CleanSurface::Stone, false, true);
			const CleanSurface block = arches != 0 ? CleanSurface::Wall : CleanSurface::Stone;
			addFlare(cx, cy, style.blockFoot, style.blockHalf, style.blockFrom, style.top, block, false, arches != 0 ? 0.0F : FlareDarkness);
			const float h = style.blockHalf;
			const CleanSurface pier = CleanSurface::Wall;
			if ((arches & 4) == 0)
				addWall({ cx - h, cy + h, 0 }, { cx + h, cy + h, 0 }, style.top, ceiling, { 0, 1, 0 }, pier, 0);
			if ((arches & 8) == 0)
				addWall({ cx + h, cy - h, 0 }, { cx - h, cy - h, 0 }, style.top, ceiling, { 0, -1, 0 }, pier, 0);
			if ((arches & 1) == 0)
				addWall({ cx + h, cy + h, 0 }, { cx + h, cy - h, 0 }, style.top, ceiling, { 1, 0, 0 }, pier, 1);
			if ((arches & 2) == 0)
				addWall({ cx - h, cy - h, 0 }, { cx - h, cy + h, 0 }, style.top, ceiling, { -1, 0, 0 }, pier, 1);
		} else {
			addBox(cx, cy, style.plinth, ceiling - style.capital, ceiling, CleanSurface::Stone, false, true);
		}
		if (style.round) {
			constexpr int Sides = 10;
			constexpr float Pi = 3.14159265F;
			float u = 0;
			for (int i = 0; i < Sides; i++) {
				const float a0 = 2.0F * Pi * static_cast<float>(i) / Sides;
				const float a1 = 2.0F * Pi * static_cast<float>(i + 1) / Sides;
				const float am = (a0 + a1) / 2.0F;
				const P3 p0 { cx + style.shaft * std::cos(a0), cy + style.shaft * std::sin(a0), 0 };
				const P3 p1 { cx + style.shaft * std::cos(a1), cy + style.shaft * std::sin(a1), 0 };
				addWall(p0, p1, style.base, shaftTop, { std::cos(am), std::sin(am), 0 }, CleanSurface::Pillar, -1, u);
				u += std::hypot(p1[0] - p0[0], p1[1] - p0[1]);
			}
		} else {
			addBox(cx, cy, style.shaft, style.base, shaftTop, CleanSurface::Pillar, false, false);
		}
	};

	// Arches over walkable tiles (special arch cels): one per run of walkable tiles between two supports.
	std::vector<uint8_t> done(static_cast<size_t>(MAXDUNX) * MAXDUNY, 0);
	// Pillars with flared blocks that arches rest on: on which sides (1 +x, 2 -x, 4 +y, 8 -y).
	std::vector<uint8_t> carrying(static_cast<size_t>(MAXDUNX) * MAXDUNY, 0);
	int carriers = 0;
	int arches = 0;
	for (int y = 0; y < MAXDUNY; y++) {
		for (int x = 0; x < MAXDUNX; x++) {
			const Point tile { x, y };
			if (dSpecial[x][y] <= 0 || Kinds[static_cast<size_t>(y) * MAXDUNX + x] != CleanKind::Open || done[static_cast<size_t>(y) * MAXDUNX + x] != 0)
				continue;
			// The run along each axis: walkable tiles up to a support on both sides.
			struct Run {
				int from = 0;
				int to = 0;
				bool bounded = false;
			};
			auto runAlong = [&](bool alongX) {
				Run run;
				const Displacement step = alongX ? Displacement { 1, 0 } : Displacement { 0, 1 };
				int before = 0;
				while (before < MaxArchRun && !IsSupport(KindAt(tile - step * (before + 1))) && KindAt(tile - step * (before + 1)) == CleanKind::Open)
					before++;
				int after = 0;
				while (after < MaxArchRun && !IsSupport(KindAt(tile + step * (after + 1))) && KindAt(tile + step * (after + 1)) == CleanKind::Open)
					after++;
				run.from = -before;
				run.to = after;
				run.bounded = IsSupport(KindAt(tile - step * (before + 1))) && IsSupport(KindAt(tile + step * (after + 1)));
				return run;
			};
			const Run rx = runAlong(true);
			const Run ry = runAlong(false);
			// The axis bounded on both sides, the shorter span if both are, else the one resting on pillars (an arcade).
			bool alongX;
			const auto onPillars = [&](bool x) {
				const Displacement step = x ? Displacement { 1, 0 } : Displacement { 0, 1 };
				const Run &run = x ? rx : ry;
				return KindAt(tile + step * (run.from - 1)) == CleanKind::Pillar || KindAt(tile + step * (run.to + 1)) == CleanKind::Pillar;
			};
			if (rx.bounded != ry.bounded)
				alongX = rx.bounded;
			else if (rx.bounded && rx.to - rx.from != ry.to - ry.from)
				alongX = rx.to - rx.from < ry.to - ry.from;
			else if (rx.bounded)
				alongX = onPillars(true) || !onPillars(false);
			else
				continue;
			const Run run = alongX ? rx : ry;
			const Displacement step = alongX ? Displacement { 1, 0 } : Displacement { 0, 1 };
			for (int k = run.from; k <= run.to; k++) {
				const Point t = tile + step * k;
				done[static_cast<size_t>(t.y) * MAXDUNX + t.x] = 1;
			}
			const Point first = tile + step * run.from;
			const Point last = tile + step * run.to;
			const CleanKind supportBefore = KindAt(first - step);
			const CleanKind supportAfter = KindAt(last + step);
			const auto isPost = [](CleanKind k) { return k == CleanKind::Pillar || k == CleanKind::Stand; };
			// A pillar's flared block, if it has one, carries the arch.
			const auto isBlock = [&](CleanKind k) { return k == CleanKind::Pillar && style.top > 0; };
			const auto postHalf = [&](CleanKind k) { return isBlock(k) ? style.blockHalf : style.shaft; };
			const bool onBlock = isBlock(supportBefore) || isBlock(supportAfter);
			// Along the axis: the opening runs between the supports' faces (a pillar's shaft or block, or a wall's face).
			const float start = static_cast<float>(alongX ? first.x : first.y);
			const float end = static_cast<float>(alongX ? last.x : last.y) + 1.0F;
			const float a = isPost(supportBefore) ? start - 0.5F + postHalf(supportBefore) : start;
			const float b = isPost(supportAfter) ? end + 0.5F - postHalf(supportAfter) : end;
			// Across: as thick as the walls between two walls, else as the pillars (their blocks).
			const float acrossMid = static_cast<float>(alongX ? y : x) + 0.5F;
			const float half = onBlock ? style.blockHalf : (isPost(supportBefore) || isPost(supportAfter) ? style.shaft : 0.5F);
			for (const auto &[support, side] : { std::pair { first - step, alongX ? 1 : 4 }, std::pair { last + step, alongX ? 2 : 8 } }) {
				if (!isBlock(KindAt(support)))
					continue;
				uint8_t &sides = carrying[static_cast<size_t>(support.y) * MAXDUNX + support.x];
				carriers += sides == 0 ? 1 : 0;
				sides |= side;
			}
			const float c0 = acrossMid - half;
			const float c1 = acrossMid + half;
			const float radius = (b - a) / 2.0F;
			const float middle = (a + b) / 2.0F;
			// A half circle where it fits under the ceiling, else flattened to fit; from the top of the pillars' blocks.
			const float rise = std::min(radius, ceiling - ArchLintel - (onBlock ? style.top : MinArchSpring));
			const float spring = onBlock ? style.top : ceiling - ArchLintel - rise;
			auto archTop = [&](float s) {
				const float q = std::clamp((s - middle) / radius, -1.0F, 1.0F);
				return spring + rise * std::sqrt(1.0F - q * q);
			};
			auto point = [&](float along, float across, float z) { return alongX ? P3 { along, across, z } : P3 { across, along, z }; };
			constexpr int Segments = 12;
			for (int i = 0; i < Segments; i++) {
				const float s0 = a + (b - a) * static_cast<float>(i) / Segments;
				const float s1 = a + (b - a) * static_cast<float>(i + 1) / Segments;
				const float z0 = archTop(s0);
				const float z1 = archTop(s1);
				for (const float side : { -1.0F, 1.0F }) {
					const float c = side > 0 ? c1 : c0;
					const P3 n = alongX ? P3 { 0, side, 0 } : P3 { side, 0, 0 };
					// The face above the arch: a strip from the curve up to the ceiling (two triangles' worth as a quad).
					const P3 q0 = point(s0, c, z0);
					const P3 q1 = point(s1, c, z1);
					const P3 q2 = point(s1, c, ceiling);
					const P3 q3 = point(s0, c, ceiling);
					quads.push_back({ { q0, q1, q2, q3 }, n, { { { s0, -z0 }, { s1, -z1 }, { s1, -ceiling }, { s0, -ceiling } } }, CleanSurface::Wall });
				}
				// The underside, following the curve across the arch's thickness.
				const float ds = s1 - s0;
				const float dz = z1 - z0;
				const float len = std::hypot(ds, dz);
				P3 n = alongX ? P3 { dz / len, 0, -ds / len } : P3 { 0, dz / len, -ds / len };
				const P3 u0 = point(s0, c0, z0);
				const P3 u1 = point(s1, c0, z1);
				const P3 u2 = point(s1, c1, z1);
				const P3 u3 = point(s0, c1, z0);
				quads.push_back({ { u0, u3, u2, u1 }, n, { { { s0, c0 }, { s0, c1 }, { s1, c1 }, { s1, c0 } } }, CleanSurface::Arch });
			}
			arches++;
		}
	}
	// The pillars, now that the arches resting on them are known.
	for (int y = 0; y < MAXDUNY; y++)
		for (int x = 0; x < MAXDUNX; x++)
			if (Kinds[static_cast<size_t>(y) * MAXDUNX + x] == CleanKind::Pillar)
				addPillar(static_cast<float>(x) + 0.5F, static_cast<float>(y) + 0.5F, carrying[static_cast<size_t>(y) * MAXDUNX + x]);
	// Stairwells: steps inside the opening beside each stairs trigger, down into a pit or up into the dark.
	for (const CleanStairs &stairs : Stairs) {
		const Vec2 forward { static_cast<float>(stairs.tile.x - stairs.trigger.x), static_cast<float>(stairs.tile.y - stairs.trigger.y) };
		const Vec2 side { -forward.y, forward.x };
		const Vec2 centre { static_cast<float>(stairs.tile.x) + 0.5F, static_cast<float>(stairs.tile.y) + 0.5F };
		// A point `in` tiles into the tile from its front face, `across` from its middle line, z up.
		auto at = [&](float in, float across, float z) {
			return P3 { centre.x + forward.x * (in - 0.5F) + side.x * across, centre.y + forward.y * (in - 0.5F) + side.y * across, z };
		};
		const float half = StairsHalfWidth;
		const int n = StairsSteps;
		auto addStep = [&](float in0, float in1, float riserBottom, float tread, float darkness) {
			// The riser (facing back out to the trigger) and the tread on top of it.
			const P3 facing { -forward.x, -forward.y, 0 };
			const P3 r0 = at(in0, -half, riserBottom);
			const P3 r1 = at(in0, half, riserBottom);
			const P3 r2 = at(in0, half, tread);
			const P3 r3 = at(in0, -half, tread);
			// Risers a shade darker than the treads, so the flight reads as steps.
			quads.push_back({ { r0, r1, r2, r3 }, facing, { { { -half, -riserBottom }, { half, -riserBottom }, { half, -tread }, { -half, -tread } } }, CleanSurface::Arch, darkness + 1.5F });
			const P3 t0 = at(in0, -half, tread);
			const P3 t1 = at(in0, half, tread);
			const P3 t2 = at(in1, half, tread);
			const P3 t3 = at(in1, -half, tread);
			quads.push_back({ { t0, t1, t2, t3 }, P3 { 0, 0, 1 }, { { { t0[0], t0[1] }, { t1[0], t1[1] }, { t2[0], t2[1] }, { t3[0], t3[1] } } }, CleanSurface::Stone, darkness });
		};
		for (int k = 0; k < n; k++) {
			const float in0 = static_cast<float>(k) / static_cast<float>(n);
			const float in1 = static_cast<float>(k + 1) / static_cast<float>(n);
			const float darkness = StairwellDarkness * in0;
			if (stairs.down)
				addStep(in0, in1, -StairsDrop * static_cast<float>(k + 1) / static_cast<float>(n), -StairsDrop * static_cast<float>(k + 1) / static_cast<float>(n), darkness);
			else
				addStep(in0, in1, StairsRise * static_cast<float>(k) / static_cast<float>(n), StairsRise * static_cast<float>(k + 1) / static_cast<float>(n), darkness);
		}
		if (stairs.down) {
			// Each riser of a flight going down runs from the tread above it: redo them over the whole drop.
			for (size_t q = quads.size() - static_cast<size_t>(2 * n); q < quads.size(); q += 2) {
				const int k = static_cast<int>((q - (quads.size() - static_cast<size_t>(2 * n))) / 2);
				const float above = -StairsDrop * static_cast<float>(k) / static_cast<float>(n);
				CleanQuad &riser = quads[q];
				riser.p[2][2] = above;
				riser.p[3][2] = above;
				riser.uv[2][1] = -above;
				riser.uv[3][1] = -above;
			}
			// The pit's sides and back, below the floor, and the floor cut open over it.
			const float bottom = -StairsDrop - 0.05F;
			for (const float s2 : { -1.0F, 1.0F }) {
				const P3 inward { -side.x * s2, -side.y * s2, 0 };
				const P3 a = at(0, half * s2, 0);
				const P3 b = at(1, half * s2, 0);
				quads.push_back({ { at(0, half * s2, bottom), at(1, half * s2, bottom), b, a }, inward, { { { 0, -bottom }, { 1, -bottom }, { 1, 0 }, { 0, 0 } } }, CleanSurface::Wall, StairwellDarkness * 0.5F });
			}
			quads.push_back({ { at(1, -half, bottom), at(1, half, bottom), at(1, half, 0), at(1, -half, 0) }, P3 { -forward.x, -forward.y, 0 },
			    { { { -half, -bottom }, { half, -bottom }, { half, 0 }, { -half, 0 } } }, CleanSurface::Wall, StairwellDarkness });
			const P3 c0 = at(0, -half, 0);
			const P3 c1 = at(1, half, 0);
			holes.push_back({ std::min(c0[0], c1[0]), std::min(c0[1], c1[1]), std::max(c0[0], c1[0]), std::max(c0[1], c1[1]) });
		}
	}
	LogInfo("fp: clean dungeon geometry: {} quads, {} arches ({} pillars carrying them), {} torch stands, {} stairs", quads.size(), arches, carriers, stands.size(), Stairs.size());
}

#ifdef _DEBUG
std::string CleanTileMap(int x0, int y0, int x1, int y1)
{
	PrepareCleanDungeon();
	// '#' wall, 'l' low wall, 'o' pillar, 't' torch stand, 'D' doorway, 'S' stairs, '~' lava or water, '^' an
	// arch over the tile, '.' floor.
	std::string map = StrCat("x ", x0, "..", x1, ", y ", y0, "..", y1, "\n");
	for (int y = y0; y <= y1; y++) {
		map += StrCat(y < 10 ? " " : "", y, " ");
		for (int x = x0; x <= x1; x++) {
			if (!InDungeonBounds({ x, y })) {
				map += ' ';
				continue;
			}
			const CleanKind kind = KindAt({ x, y });
			char c = '.';
			switch (kind) {
			case CleanKind::Wall: c = WallHeights[static_cast<size_t>(y) * MAXDUNX + x] < CleanCeiling() - 0.01F ? 'l' : '#'; break;
			case CleanKind::Pillar: c = 'o'; break;
			case CleanKind::Stand: c = 't'; break;
			case CleanKind::Doorway: c = 'D'; break;
			case CleanKind::Stairs: c = 'S'; break;
			case CleanKind::Flat: c = '~'; break;
			default: c = dSpecial[x][y] > 0 ? '^' : '.'; break;
			}
			map += c;
		}
		map += '\n';
	}
	return map;
}
#endif

} // namespace devilution::fp
