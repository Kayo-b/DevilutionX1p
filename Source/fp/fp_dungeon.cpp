/**
 * @file fp/fp_dungeon.cpp
 *
 * The clean dungeon (see fp_dungeon.hpp): tile kinds, texture sets, the floor, ceiling and wall passes, and the
 * geometry of pillars and arches (and the caves' crests, stalagmites, fences, gates and shoring) handed to the mesh
 * pass.
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
#include "fp/fp_hell.hpp"
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

/**
 * The caves' crest: the ragged rim of darker rock along the top of every rock face, jutting out over it (the original
 * draws its cliffs' tops so). Each point along a face (CrestSegments to a tile) takes its shape from where it is
 * (CaveNoise), so faces meeting at a corner share their corner point and the rim runs on round it unbroken: how far it
 * juts out, how far its lip hangs below the wall's top, how far it rises above it into the dark, and how much further
 * out its middle bulges. Its underside meets the face CrestUndercut (of how far it juts out) above its lip. Over a
 * stairwell's opening its lip hangs no lower than CrestOverOpening. Light levels added to its lower front, which the
 * bulge shades, and to its underside (besides the half level every underside gets).
 */
constexpr int CrestSegments = 3;
constexpr float CrestOutMin = 0.08F;
constexpr float CrestOutMax = 0.3F;
constexpr float CrestDropMin = 0.25F;
constexpr float CrestDropMax = 0.9F;
constexpr float CrestRiseMax = 0.45F;
constexpr float CrestBulgeMax = 0.1F;
constexpr float CrestUndercut = 0.6F;
constexpr float CrestOverOpening = 0.15F;
constexpr float CrestLowerDarkness = 0.5F;
constexpr float CrestUndersideDarkness = 0.5F;
/**
 * Roots hanging from the crest's lip, as the original draws some over its cliffs: the share of the crest's segments
 * that have them (one to three strands), how far down they hang (drawn tiles), their width at the top (they taper to
 * a third), how far apart the strands of one are, how many points along them, and the light levels added to their
 * wood.
 */
constexpr float RootChance = 0.07F;
constexpr float RootMin = 0.35F;
constexpr float RootMax = 1.1F;
constexpr float RootWidth = 0.016F;
constexpr float RootSpacing = 0.35F; // between the strands of one, a share of the crest's segment
constexpr int RootPoints = 6;
constexpr float RootDarkness = 0.0F;
/**
 * The caves' walls: how high they stand (drawn tiles; their art's 104 rows would make them four tiles high, and the
 * crest out of sight from a few tiles off), and the shade the crest casts on the face under it: light levels added at
 * the wall's top, fading out CaveTopShadowDrop below it.
 */
constexpr float CaveWallHeight = 2.5F;
/** The top of the caves' stairwells (flat), and their timber frame: how far its posts and lintel stand out of the face, their width, the lintel's height. */
constexpr float CaveStairsTop = 2.0F;
constexpr float StairsFrameOut = 0.06F;
constexpr float StairsFramePost = 0.1F;
constexpr float StairsFrameLintel = 0.14F;
/**
 * The caves' rock is laid on each way a face can look (towards +x, -x, +y, -y) this far along (tiles), so the two faces
 * of an inner corner, or the two sides of a passage, do not mirror each other.
 */
constexpr std::array<float, 4> CaveFaceShifts { 0.0F, 1.13F, 0.61F, 1.74F };
/** The same for the diagonal faces of cut corners; and the length of a diagonal's normal's components (1 / sqrt(2)). */
constexpr float CaveDiagonalShift = 0.37F;
constexpr float DiagonalNormal = 0.70710678F;
constexpr float CaveTopShadow = 1.5F;
constexpr float CaveTopShadowDrop = 1.0F;

/**
 * The caves' pools of lava (and water) are drawn round a skeleton (PoolCover): their tiles' centres and the lines
 * between neighbouring ones, `PoolRadius` (tiles) about them, the lines between diagonal neighbours (which carry a
 * river running diagonally across the grid on from tile to tile, as the game will not let anyone step between them)
 * `PoolJoinRadius`; the shore wanders `PoolRagged` either way. Then: the dark crust of cooled rock along it (tiles)
 * and how dark it is (light levels), the shore's grain (PoolNoise samples a tile), and how much of a piece's art must
 * be lava or water (the cycled colours) for its art to be laid on every pool (so the original's dark crust islands go).
 */
constexpr float PoolRadius = 0.5F;
/** Light levels the caves' pools are drawn brighter than the light where they are: lava glows, and a river shows. */
constexpr float PoolGlow = 3.0F;
constexpr float PoolJoinRadius = 0.3F;
constexpr float PoolRagged = 0.1F;
constexpr float PoolCrust = 0.04F;
constexpr float PoolCrustDarkness = 3.0F;
constexpr float PoolNoiseScale = 10.0F;
constexpr float CleanPoolShare = 0.97F;
/** How far (tiles) the pools' art is bent about by smooth noise as it is laid on, so its seams wave. */
constexpr float PoolWarp = 0.6F;

/** The caves' stalagmites (their lone rock tiles): the base radius, the height (a share of the walls' height, from Min to Max), sides, rings. */
constexpr float StalagmiteRadius = 0.34F;
constexpr float StalagmiteMin = 0.4F;
constexpr float StalagmiteMax = 0.7F;
constexpr int StalagmiteSides = 7;
constexpr int StalagmiteRings = 4;

/**
 * The caves' fences: posts (half width) on the line through their tiles' middles, one at each tile's middle and one at
 * its edge, two rails between them (heights, half thickness); the gates: the leaf's height, and a frame of two posts
 * and a lintel round it (CleanDoorOpening). Drawn tiles.
 */
constexpr float FencePostHalf = 0.05F;
constexpr float FenceHeight = 1.3F;
constexpr std::array<float, 2> FenceRails { 0.5F, 1.05F };
constexpr float FenceRailHalf = 0.035F;
constexpr float GateHeight = 1.6F;
constexpr float GateLintel = 0.12F;
/** How far the gates' lintel reaches down past the opening's top: the leaf comes out that much lower (its height is read back off the face's art rows). */
constexpr float GateLintelOverlap = 0.1F;
/** A solid piece of a fence's megatile whose art above the floor is drawn over less than this share is fence, not the rock a fence runs into (theirs 0-29%, the rock's 31-40%). */
constexpr float FenceArtDensity = 0.3F;
/**
 * The caves' shoring, timber propping a cliff: posts (two a tile) and two beams along the rock's face, standing this
 * far out from it (their middle line); the posts' height and the beams'.
 */
constexpr float ShoringOut = 0.08F;
constexpr float ShoringHeight = 1.9F;
constexpr std::array<float, 2> ShoringBeams { 0.9F, 1.78F };

// --- Texture sets --------------------------------------------------------------------------------------------------

using SourceTexture = CleanSourceTexture;

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
	case DTYPE_CAVES: return "caves";
	case DTYPE_CRYPT: return "crypt";
	case DTYPE_HELL: return "hell";
	default: return "cathedral";
	}
}

constexpr std::array<const char *, static_cast<size_t>(CleanSurface::Count)> SlotNames { "wall", "base", "top", "floor", "ceiling", "pillar", "stone", "arch", "door", "marble", "lid", "iron", "ivory", "marble2", "ivory2", "band", "crest", "wood" };

/** The palette colours textures may use: the level's ramps (not the near-blacks 112-127, nor colours the level cycles) and the shared ramps 160-254. */
bool IsAllowedColor(int index)
{
	if (index <= 0 || index >= 255)
		return false;
	if (index >= 112 && index < 160)
		return false; // near-blacks that light to red, then the bright UI ramps
	if (IsCycledColor(index) || (leveltype == DTYPE_HELL && index < 32))
		return false; // lava and water, colour cycled; hell's blood, cycled through its light tables
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

/**
 * @brief The hue of some of the level's art (RGB over luminance, fully lit, its cycled colours left out): the caves'
 * rock and floor are as red, green or purple as the palette the level drew (one of four, at random).
 */
std::array<float, 3> ArtHue(const std::vector<int16_t> &art)
{
	double r = 0;
	double g = 0;
	double b = 0;
	int count = 0;
	for (const int16_t texel : art) {
		if (texel <= 0 || IsCycledColor(texel))
			continue;
		const SDL_Color c = logical_palette[texel];
		r += c.r;
		g += c.g;
		b += c.b;
		count++;
	}
	const double luma = count > 0 ? (0.30 * r + 0.59 * g + 0.11 * b) / count : 0.0;
	if (luma < 4.0)
		return { 1.0F, 1.0F, 1.0F };
	return { static_cast<float>(r / count / luma), static_cast<float>(g / count / luma), static_cast<float>(b / count / luma) };
}

/** Of a texture's own colour, the share kept round the hue of the art it takes on (ArtHue). */
constexpr float OwnChroma = 0.25F;

void BuildTexture(CleanTexture &out, const SourceTexture &source, Quantizer &quantize, const std::array<float, NumLightingLevels> &factors, const std::array<float, 3> *hue = nullptr)
{
	out.texelsPerTileU = static_cast<float>(source.width) / source.tilesAcross;
	out.texelsPerTileV = static_cast<float>(source.height) / source.tilesDown;
	out.mips.clear();
	CleanTexture::Mip first { source.width, source.height, source.rgb, {} };
	if (hue != nullptr) {
		// Each texel as light as it was, in the art's hue, with a little of its own colour.
		for (size_t i = 0; i + 2 < first.rgb.size(); i += 3) {
			const float r = first.rgb[i];
			const float g = first.rgb[i + 1];
			const float b = first.rgb[i + 2];
			const float luma = 0.30F * r + 0.59F * g + 0.11F * b;
			for (int c = 0; c < 3; c++) {
				const float own = c == 0 ? r : (c == 1 ? g : b);
				first.rgb[i + c] = static_cast<uint8_t>(std::clamp(luma * (*hue)[c] + OwnChroma * (own - luma), 0.0F, 255.0F) + 0.5F);
			}
		}
	}
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
	// The caves take their rock's and floor's hue from the level's art (its palette is one of four).
	const bool caves = leveltype == DTYPE_CAVES;
	const std::array<float, 3> rockHue = caves ? ArtHue(GetFallbackFace()) : std::array<float, 3> {};
	const std::array<float, 3> floorHue = caves ? ArtHue(GetGroundFloor()) : std::array<float, 3> {};
	if (caves)
		LogInfo("fp: caves' hues: rock {:.2f} {:.2f} {:.2f}, floor {:.2f} {:.2f} {:.2f}", rockHue[0], rockHue[1], rockHue[2], floorHue[0], floorHue[1], floorHue[2]);
	for (size_t slot = 0; slot < SlotNames.size(); slot++) {
		const auto surface = static_cast<CleanSurface>(slot);
		const auto find = [&](const char *name) -> const SourceTexture * {
			const auto it = SourceTextures.find(StrCat(set, "/", name));
			return it != SourceTextures.end() ? &it->second : nullptr;
		};
		// Hell's flesh is cut out of its own art (fp_hell.cpp) when the level loads.
		SourceTexture art;
		const SourceTexture *source = HellArtTexture(surface, art) ? &art : find(SlotNames[slot]);
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
		if (source == nullptr && surface == CleanSurface::Wood)
			source = find("door");
		if (source == nullptr && (surface == CleanSurface::Stone || surface == CleanSurface::Marble || surface == CleanSurface::Lid || surface == CleanSurface::Ivory || surface == CleanSurface::MarbleAlt || surface == CleanSurface::IvoryAlt))
			source = find("pillar");
		if (source == nullptr && surface != CleanSurface::Base && surface != CleanSurface::Top)
			source = find("wall");
		const SourceTexture plain = PlainTexture(surface);
		if (IsHell() && source != nullptr && source != &art) {
			art = *source; // the bone, in the level's own palette's bone colours
			HellTintTexture(surface, art);
			source = &art;
		}
		const std::array<float, 3> *hue = nullptr;
		if (caves && IsAnyOf(surface, CleanSurface::Wall, CleanSurface::Arch, CleanSurface::Pillar, CleanSurface::Crest, CleanSurface::Stone))
			hue = &rockHue;
		else if (caves && surface == CleanSurface::Floor)
			hue = &floorHue;
		BuildTexture(Textures.surfaces[slot], source != nullptr ? *source : plain, quantize, factors, hue);
	}
	LogInfo("fp: clean dungeon textures ready ({} set)", set);
}

/** Points a sampler at the detail level of a texture for a pixel's footprint (up close, its sample point is dithered). */
void PrepareSampler(CleanSampler &s, const CleanTexture &tex, float worldPerPixel)
{
	const float footprint = worldPerPixel * tex.texelsPerTileU; // full-size texels per pixel
	int level = 0;
	if (footprint >= 1.5F)
		level = std::min(static_cast<int>(std::log2(footprint / 1.5F)) + 1, static_cast<int>(tex.mips.size()) - 1);
	const CleanTexture::Mip &mip = tex.mips[level];
	s.lit = &mip.lit;
	s.maskU = mip.width - 1;
	s.maskV = mip.height - 1;
	s.shift = 0;
	while ((1 << s.shift) < mip.width)
		s.shift++;
	const float down = static_cast<float>(1 << level);
	s.scaleU = tex.texelsPerTileU / down;
	s.scaleV = tex.texelsPerTileV / down;
	s.jitter = footprint < 1.0F;
}

struct Sampler : CleanSampler {
	void Prepare(const CleanTexture &tex, float worldPerPixel) { PrepareSampler(*this, tex, worldPerPixel); }
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

/** @brief A hash of a point of a grid and a salt, 0..1. */
float Hash01(int x, int y, uint32_t salt)
{
	uint32_t h = static_cast<uint32_t>(x) * 73856093U ^ static_cast<uint32_t>(y) * 19349663U ^ (salt + 1U) * 83492791U;
	h ^= h >> 13;
	h *= 0x5bd1e995U;
	h ^= h >> 15;
	return static_cast<float>(h & 0xFFFFU) / 65535.0F;
}

/**
 * @brief 0..1 over the floor plan, for the caves' uneven rock: smooth over whole tiles, plus a finer grain of a value
 * per CrestSegments of a tile. The same at the same point whatever asks (two faces meeting at a corner).
 */
float CaveNoise(float x, float y, uint32_t salt)
{
	const float fx = std::floor(x);
	const float fy = std::floor(y);
	const int ix = static_cast<int>(fx);
	const int iy = static_cast<int>(fy);
	auto smooth = [](float t) { return t * t * (3.0F - 2.0F * t); };
	const float tx = smooth(x - fx);
	const float ty = smooth(y - fy);
	const float a = Hash01(ix, iy, salt) + (Hash01(ix + 1, iy, salt) - Hash01(ix, iy, salt)) * tx;
	const float b = Hash01(ix, iy + 1, salt) + (Hash01(ix + 1, iy + 1, salt) - Hash01(ix, iy + 1, salt)) * tx;
	const float fine = Hash01(static_cast<int>(std::lround(x * CrestSegments)), static_cast<int>(std::lround(y * CrestSegments)), salt + 101U);
	return (a + (b - a) * ty) * 0.5F + fine * 0.5F;
}

/**
 * @brief Smooth noise 0..1 over the floor plan, two octaves (0.8 and 0.4 of a tile across at PoolNoiseScale), tiling
 * every 6.4 tiles: the caves' pools' ragged shores.
 */
float PoolNoise(float x, float y)
{
	constexpr int Size = 64;
	static const std::array<float, Size * Size> Table = [] {
		std::array<float, Size * Size> table {};
		auto octave = [&](int cell, uint32_t salt, float weight) {
			const int cells = Size / cell;
			auto at = [&](int i, int j) { return Hash01(((i % cells) + cells) % cells, ((j % cells) + cells) % cells, salt); };
			for (int j = 0; j < Size; j++) {
				for (int i = 0; i < Size; i++) {
					const int ci = i / cell;
					const int cj = j / cell;
					float fx = static_cast<float>(i % cell) / static_cast<float>(cell);
					float fy = static_cast<float>(j % cell) / static_cast<float>(cell);
					fx = fx * fx * (3.0F - 2.0F * fx);
					fy = fy * fy * (3.0F - 2.0F * fy);
					const float a = at(ci, cj) + (at(ci + 1, cj) - at(ci, cj)) * fx;
					const float b = at(ci, cj + 1) + (at(ci + 1, cj + 1) - at(ci, cj + 1)) * fx;
					table[static_cast<size_t>(j) * Size + i] += weight * (a + (b - a) * fy);
				}
			}
		};
		octave(8, 51, 0.65F);
		octave(4, 52, 0.35F);
		return table;
	}();
	const float sx = x * PoolNoiseScale;
	const float sy = y * PoolNoiseScale;
	const int ix = static_cast<int>(std::floor(sx));
	const int iy = static_cast<int>(std::floor(sy));
	const float tx = sx - static_cast<float>(ix);
	const float ty = sy - static_cast<float>(iy);
	auto at = [&](int i, int j) { return Table[static_cast<size_t>(j & (Size - 1)) * Size + (i & (Size - 1))]; };
	const float a = at(ix, iy) + (at(ix + 1, iy) - at(ix, iy)) * tx;
	const float b = at(ix, iy + 1) + (at(ix + 1, iy + 1) - at(ix, iy + 1)) * tx;
	return a + (b - a) * ty;
}

/** The art of the level's commonest piece of lava or water with nothing else in it (FindCleanPools): the caves' pools wear it (empty: their own). */
std::vector<std::vector<int16_t>> CleanPoolArt;

/** @brief Whether a piece's floor art is lava or water: a good part of it in the colours the level cycles. */
bool IsCycledFloor(const PieceTextures &textures)
{
	int drawn = 0;
	int cycled = 0;
	for (const int16_t texel : textures.floor) {
		if (texel <= 0)
			continue;
		drawn++;
		if (IsCycledColor(texel))
			cycled++;
	}
	return drawn > 0 && cycled * 6 > drawn;
}

/** @brief How much of a piece's art above its floor diamond is drawn (0..1): little for posts and rails, most for rock. */
float ArtDensity(const PieceTextures &textures)
{
	const size_t rows = textures.column.size() / ColumnWidth;
	if (rows <= static_cast<size_t>(FloorTextureSize))
		return 0.0F;
	const size_t upper = (rows - FloorTextureSize) * ColumnWidth;
	size_t drawn = 0;
	for (size_t i = 0; i < upper; i++)
		if (textures.column[i] > 0)
			drawn++;
	return static_cast<float>(drawn) / static_cast<float>(upper);
}

/** @brief The megatile (the generator's tile, 2 x 2 pieces) a tile is part of, 0 if none. */
uint8_t MegatileAt(Point tile)
{
	if (tile.x < 16 || tile.y < 16)
		return 0;
	const int mx = (tile.x - 16) / 2;
	const int my = (tile.y - 16) / 2;
	if (mx >= DMAXX || my >= DMAXY)
		return 0;
	return dungeon[mx][my];
}

/**
 * @brief Whether a tile is part of one of the caves' megatiles of timber shoring (drlg_l3.cpp, Fence: 121-129, and
 * 150-152): posts and beams against a cliff (its pieces alternate timber and rock along the cliff's line: all are
 * drawn as rock, the timber in front of it).
 */
bool IsShoringMegatile(Point tile)
{
	const uint8_t mega = MegatileAt(tile);
	return (mega >= 121 && mega <= 129) || (mega >= 150 && mega <= 152);
}

/**
 * @brief Whether a solid tile of the caves is part of a wooden fence: a piece of the megatiles their generator lays
 * fences with (drlg_l3.cpp, Fence: 130-140, and 146 and 147 round the gates) whose art is sparse, posts and rails with
 * the dark between them (a fence's end shares its megatile with the rock it runs into).
 */
bool IsFencePiece(Point tile)
{
	const uint8_t mega = MegatileAt(tile);
	if (!((mega >= 130 && mega <= 140) || mega == 146 || mega == 147))
		return false;
	return ArtDensity(GetPieceTextures(dPiece[tile.x][tile.y])) < FenceArtDensity;
}

std::vector<CleanStairs> Stairs;
std::vector<int16_t> StairsIndex; // per tile: index into Stairs of the stairs whose opening is in it, -1 if none
/** Per tile: the caves' rock tiles whose corner is cut (FindChamfers), 1 + (cut towards +x) + 2 * (towards +y); 0 if none. */
std::vector<uint8_t> Chamfers;

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

/**
 * @brief The caves' cut corners: a rock tile whose two neighbours on one side and the other (and the one between them,
 * diagonally) are open ground, the rock going on behind it both ways, has that corner cut off along the diagonal, so
 * cliffs stepping across the grid run diagonally and the corners of rock round off. Not where timber or a stairwell is.
 */
CleanKind KindAt(Point tile);

void FindChamfers()
{
	Chamfers.assign(static_cast<size_t>(MAXDUNX) * MAXDUNY, 0);
	if (leveltype != DTYPE_CAVES)
		return;
	auto rockAt = [](Point t) {
		const CleanKind kind = KindAt(t);
		return kind == CleanKind::Wall || kind == CleanKind::Stairs;
	};
	auto openAt = [](Point t) {
		if (!InDungeonBounds(t))
			return false;
		const CleanKind kind = KindAt(t);
		return kind == CleanKind::Open || kind == CleanKind::Flat;
	};
	for (int y = 0; y < MAXDUNY; y++) {
		for (int x = 0; x < MAXDUNX; x++) {
			const Point tile { x, y };
			if (KindAt(tile) != CleanKind::Wall || IsShoringMegatile(tile))
				continue;
			for (const int cy : { -1, 1 }) {
				for (const int cx : { -1, 1 }) {
					if (openAt(tile + Displacement { cx, 0 }) && openAt(tile + Displacement { 0, cy }) && openAt(tile + Displacement { cx, cy })
					    && rockAt(tile - Displacement { cx, 0 }) && rockAt(tile - Displacement { 0, cy })) {
						Chamfers[static_cast<size_t>(y) * MAXDUNX + x] = static_cast<uint8_t>(1 + (cx > 0 ? 1 : 0) + (cy > 0 ? 2 : 0));
					}
				}
			}
		}
	}
}

/** @brief Towards which corner a rock tile is cut (FindChamfers), {0, 0} if it is not. */
Displacement ChamferAt(Point tile)
{
	if (!InDungeonBounds(tile) || Chamfers.empty())
		return { 0, 0 };
	const uint8_t code = Chamfers[static_cast<size_t>(tile.y) * MAXDUNX + tile.x];
	if (code == 0)
		return { 0, 0 };
	return { (code - 1) & 1 ? 1 : -1, (code - 1) & 2 ? 1 : -1 };
}

/**
 * @brief The caves' pools wear the art of their commonest piece that is lava or water through and through
 * (CleanPoolShare) instead of their own: the original paints dark crust islands in its pools, which come out as flat
 * blotches, and its pieces do not run on into each other seamlessly. The texels its diamond leaves empty, and its few
 * specks of crust, are filled from their neighbours (wrapping round). The floor pass lays it on bent by smooth noise
 * (PoolWarp), so where it wraps round is a wavy line, not a straight seam.
 */
void FindCleanPools()
{
	CleanPoolArt.clear();
	if (leveltype != DTYPE_CAVES)
		return;
	constexpr int Size = FloorTextureSize;
	// The pieces by how many tiles of pool they make, the commonest first.
	std::vector<std::pair<int, uint16_t>> counts;
	for (int y = 0; y < MAXDUNY; y++) {
		for (int x = 0; x < MAXDUNX; x++) {
			if (Kinds[static_cast<size_t>(y) * MAXDUNX + x] != CleanKind::Flat)
				continue;
			const uint16_t piece = dPiece[x][y];
			const auto it = std::find_if(counts.begin(), counts.end(), [&](const auto &c) { return c.second == piece; });
			if (it != counts.end())
				it->first++;
			else
				counts.emplace_back(1, piece);
		}
	}
	std::sort(counts.begin(), counts.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
	for (const auto &[count, piece] : counts) {
		const std::vector<int16_t> &floor = GetPieceTextures(piece).floor;
		if (floor.size() != static_cast<size_t>(Size) * Size)
			continue;
		int drawn = 0;
		int cycled = 0;
		for (const int16_t texel : floor) {
			if (texel <= 0)
				continue;
			drawn++;
			cycled += IsCycledColor(texel) ? 1 : 0;
		}
		if (drawn < Size * Size * 3 / 4 || static_cast<float>(cycled) < CleanPoolShare * static_cast<float>(drawn))
			continue;
		std::vector<int16_t> art = floor;
		for (int pass = 0; pass < 8; pass++) {
			bool missing = false;
			for (int v = 0; v < Size; v++) {
				for (int u = 0; u < Size; u++) {
					int16_t &texel = art[static_cast<size_t>(v) * Size + u];
					if (texel > 0 && IsCycledColor(texel))
						continue;
					for (const auto &[du, dv] : { std::pair { 1, 0 }, std::pair { -1, 0 }, std::pair { 0, 1 }, std::pair { 0, -1 } }) {
						const int16_t other = art[static_cast<size_t>((v + dv + Size) % Size) * Size + (u + du + Size) % Size];
						if (other > 0 && IsCycledColor(other)) {
							texel = other;
							break;
						}
					}
					missing = missing || texel <= 0 || !IsCycledColor(texel);
				}
			}
			if (!missing)
				break;
		}
		CleanPoolArt.push_back(std::move(art));
		LogInfo("fp: caves: the pools wear piece {} (on {} of their tiles)", piece, count);
		return;
	}
	LogInfo("fp: caves: none of the pools' {} pieces is clean lava or water: they keep their own art", counts.size());
}

void ClassifyTiles()
{
	Kinds.assign(static_cast<size_t>(MAXDUNX) * MAXDUNY, CleanKind::Open);
	WallHeights.assign(Kinds.size(), CleanCeiling());
	if (IsHell()) {
		// Hell's tiles are told apart by the tiles of its map (fp_hell.cpp); its stairs are rib cages, not stairwells.
		ClassifyHellTiles(Kinds, WallHeights);
		Stairs.clear();
		StairsIndex.assign(Kinds.size(), -1);
		BlindArches.clear();
		BlindArchIndex.clear();
		Chamfers.clear();
		return;
	}
	const float ceiling = CleanCeiling();
	const int levelRows = LevelWallRows();
	auto solid = [](Point p) { return !InDungeonBounds(p) || IsTileSolid(p); };
	const bool caves = leveltype == DTYPE_CAVES;
	int counts[9] {};
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
				else if (caves && (textures.isFlatSolid || !textures.hasTallArt) && IsCycledFloor(textures))
					kind = CleanKind::Flat; // the caves' lava (and the Poisoned Water Supply's water), whole pools of it
				else if (caves && IsFencePiece(tile))
					kind = CleanKind::Fence;
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
	FindChamfers();
	FindCleanPools();
	LogInfo("fp: clean dungeon tiles: {} walls, {} pillars, {} torch stands, {} doorways, {} stairs, {} lava or water, {} fences, {} blind arches", counts[1], counts[2], counts[3], counts[4], Stairs.size(), counts[6], counts[7], BlindArches.size());
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

/** @brief What a tile is to the caves' pools: 2 pool (lava, water), 1 rock (or off the map), 0 ground. */
inline uint8_t PoolKind(int x, int y)
{
	if (x < 0 || y < 0 || x >= MAXDUNX || y >= MAXDUNY)
		return 1;
	const CleanKind kind = Kinds[static_cast<size_t>(y) * MAXDUNX + x];
	if (kind == CleanKind::Flat)
		return 2;
	return kind == CleanKind::Wall || kind == CleanKind::Stairs ? 1 : 0;
}

/**
 * @brief How far inside the caves' pools a point is (tiles; below 0 outside): within PoolRadius of the centre of a
 * pool tile or of the line to a neighbouring one along the grid (pool, or the rock it runs up to), within
 * PoolJoinRadius of the line to a diagonal one. The point is at (ax, ay) between four tile centres, at (0, 0), (1, 0),
 * (0, 1) and (1, 1), which are q00, q10, q01, q11 (PoolKind); lines to centres beyond them come no nearer than these.
 */
inline float PoolCover(uint8_t q00, uint8_t q10, uint8_t q01, uint8_t q11, float ax, float ay)
{
	if ((q00 & q10 & q01 & q11) == 2)
		return 1.0F; // amid a pool
	float d2 = 81.0F;
	auto centre = [&](uint8_t q, float cx, float cy) {
		if (q == 2)
			d2 = std::min(d2, (ax - cx) * (ax - cx) + (ay - cy) * (ay - cy));
	};
	centre(q00, 0.0F, 0.0F);
	centre(q10, 1.0F, 0.0F);
	centre(q01, 0.0F, 1.0F);
	centre(q11, 1.0F, 1.0F);
	float d = std::sqrt(d2);
	auto joined = [](uint8_t a, uint8_t b) { return (a == 2 && b != 0) || (b == 2 && a != 0); };
	if (joined(q00, q10))
		d = std::min(d, ay);
	if (joined(q01, q11))
		d = std::min(d, 1.0F - ay);
	if (joined(q00, q01))
		d = std::min(d, ax);
	if (joined(q10, q11))
		d = std::min(d, 1.0F - ax);
	float cover = PoolRadius - d;
	constexpr float InvSqrt2 = 0.70710678F;
	if (q00 == 2 && q11 == 2)
		cover = std::max(cover, PoolJoinRadius - std::abs(ax - ay) * InvSqrt2);
	if (q10 == 2 && q01 == 2)
		cover = std::max(cover, PoolJoinRadius - std::abs(ax + ay - 1.0F) * InvSqrt2);
	return cover;
}

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
	return IsAnyOf(leveltype, DTYPE_CATHEDRAL, DTYPE_CATACOMBS, DTYPE_CAVES, DTYPE_CRYPT, DTYPE_HELL);
}

const CleanKind *CleanKindMap()
{
	EnsureKindsForLevel();
	return Kinds.data();
}

const float *CleanWallHeightMap()
{
	EnsureKindsForLevel();
	return WallHeights.data();
}

void PrepareCleanSampler(CleanSampler &sampler, CleanSurface surface, float worldPerPixel)
{
	PrepareSampler(sampler, Textures.surfaces[static_cast<size_t>(surface)], worldPerPixel);
}

bool CleanHasCeiling()
{
	return !IsAnyOf(leveltype, DTYPE_CAVES, DTYPE_HELL);
}

bool CleanDoorwaysAreGates()
{
	return leveltype == DTYPE_CAVES;
}


float CleanCeiling()
{
	if (leveltype == DTYPE_CAVES)
		return CaveWallHeight;
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

/** The arched opening of a stairwell: wide, its crown under the ceiling (the caves': flat-topped, in a timber frame). */
CleanOpening StairsOpening()
{
	if (leveltype == DTYPE_CAVES)
		return { StairsHalfWidth, CaveStairsTop, true };
	return { StairsHalfWidth, CleanCeiling() - ArchLintel - StairsHalfWidth };
}

const std::vector<CleanStairs> &CleanStairsList()
{
	EnsureKindsForLevel();
	return Stairs;
}

CleanOpening CleanDoorOpening()
{
	if (CleanDoorwaysAreGates())
		return { DoorHalfWidth, GateHeight, true };
	const float crown = std::min(DoorCrown, CleanCeiling() - 0.45F);
	return { DoorHalfWidth, crown - DoorHalfWidth };
}

float CleanOpeningTop(const CleanOpening &opening, float along)
{
	const float a = along - 0.5F;
	if (std::abs(a) >= opening.halfWidth)
		return 0.0F;
	if (opening.flat)
		return opening.spring;
	return opening.spring + std::sqrt(opening.halfWidth * opening.halfWidth - a * a);
}

float CleanLightAt(float x, float y)
{
	const float fx = x - 0.5F;
	const float fy = y - 0.5F;
	const int ix = FloorToInt(fx);
	const int iy = FloorToInt(fy);
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
	if (light >= static_cast<float>(LightsMax))
		return 0; // the darkest light level leaves every colour black
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
	const bool hasCeiling = CleanHasCeiling();
	const bool raggedPools = leveltype == DTYPE_CAVES;
	for (int y = 0; y < proj.height; y++) {
		const bool isFloor = y > proj.horizon;
		uint8_t *dst = out.at(0, y);
		if (!isFloor && !hasCeiling) {
			std::fill_n(dst, proj.width, 0); // the dark over the caves
			continue;
		}
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
		bool cleanArt = false;
		// The caves' pools: what the tile centres around a point are to them (PoolKind), and whether any is pool.
		uint8_t q00 = 0;
		uint8_t q10 = 0;
		uint8_t q01 = 0;
		uint8_t q11 = 0;
		bool poolCell = false;
		// Up close, the pools' art is sampled at a dithered point, which blends its texels instead of showing squares.
		const bool jitterPool = across * FloorTextureSize < 1.0F;
		// The pools' warp (PoolWarp) changes slowly: worked out every WarpStep pixels along the row, interpolated between.
		constexpr int WarpStep = 8;
		int warpSegment = -1;
		std::array<float, 4> warp {}; // u and v at the segment's start, and at its end
		for (int x = 0; x < proj.width; x++, wx += stepX, wy += stepY) {
			const int tx = FloorToInt(wx);
			const int ty = FloorToInt(wy);
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
			const int cx = FloorToInt(fx);
			const int cy = FloorToInt(fy);
			if (cx != cellX || cy != cellY) {
				cellX = cx;
				cellY = cy;
				l00 = TileLight(cx, cy);
				l10 = TileLight(cx + 1, cy);
				l01 = TileLight(cx, cy + 1);
				l11 = TileLight(cx + 1, cy + 1);
				if (raggedPools && isFloor) {
					q00 = PoolKind(cx, cy);
					q10 = PoolKind(cx + 1, cy);
					q01 = PoolKind(cx, cy + 1);
					q11 = PoolKind(cx + 1, cy + 1);
					poolCell = q00 == 2 || q10 == 2 || q01 == 2 || q11 == 2;
				}
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
					// The caves' pools reach a little onto the ground (PoolCover): their art there is the clean pool art.
					cleanArt = raggedPools && !CleanPoolArt.empty();
					flatArt = cleanArt ? &CleanPoolArt.front() : (flat ? &GetPieceTextures(dPiece[tx][ty]).floor : nullptr);
				}
				depthRow[x] = depth;
				if ((raggedPools ? poolCell : flat) && flatArt != nullptr && flatArt->size() == static_cast<size_t>(FloorTextureSize) * FloorTextureSize) {
					// The caves' pools: round their skeleton (PoolCover), the shore ragged, a dark crust of cooled rock
					// along it.
					float inside = 1.0F;
					if (raggedPools) {
						inside = PoolCover(q00, q10, q01, q11, ax, ay);
						if (inside < PoolRagged + PoolCrust && inside > -PoolRagged)
							inside += PoolRagged * (2.0F * PoolNoise(wx, wy) - 1.0F);
					}
					if (inside <= 0.0F) {
						dst[x] = sampler.At(wx, wy, LightLevel(light, x, y), x, y);
						continue;
					}
					if (inside < PoolCrust) {
						dst[x] = sampler.At(wx, wy, LightLevel(light + PoolCrustDarkness, x, y), x, y);
						continue;
					}
					// Lava and water keep their art's colours (and its colour cycling).
					float fu = (wx - static_cast<float>(tx)) * FloorTextureSize + (jitterPool ? DitherU(x, y) : 0.0F);
					float fv = (wy - static_cast<float>(ty)) * FloorTextureSize + (jitterPool ? DitherV(x, y) : 0.0F);
					if (cleanArt) {
						if (x / WarpStep != warpSegment) {
							warpSegment = x / WarpStep;
							const int first = warpSegment * WarpStep;
							const float sx = wx - stepX * static_cast<float>(x - first);
							const float sy = wy - stepY * static_cast<float>(x - first);
							const float ex = sx + stepX * WarpStep;
							const float ey = sy + stepY * WarpStep;
							constexpr float Reach = PoolWarp * FloorTextureSize;
							warp = { Reach * (PoolNoise(sx + 17.3F, sy) - 0.5F), Reach * (PoolNoise(sx, sy + 29.1F) - 0.5F), Reach * (PoolNoise(ex + 17.3F, ey) - 0.5F), Reach * (PoolNoise(ex, ey + 29.1F) - 0.5F) };
						}
						const float t = static_cast<float>(x % WarpStep) / WarpStep;
						fu += warp[0] + (warp[2] - warp[0]) * t;
						fv += warp[1] + (warp[3] - warp[1]) * t;
					}
					const int u = cleanArt ? static_cast<int>(std::floor(fu)) & (FloorTextureSize - 1) : std::clamp(static_cast<int>(fu), 0, FloorTextureSize - 1);
					const int v = cleanArt ? static_cast<int>(std::floor(fv)) & (FloorTextureSize - 1) : std::clamp(static_cast<int>(fv), 0, FloorTextureSize - 1);
					int16_t texel = (*flatArt)[static_cast<size_t>(v) * FloorTextureSize + u];
					// The art's diamond leaves a texel or two empty along the tile's edges: the nearest drawn one.
					for (int r = 1; texel <= 0 && r <= 2; r++) {
						for (const auto &[du, dv] : { std::pair { r, 0 }, std::pair { -r, 0 }, std::pair { 0, r }, std::pair { 0, -r }, std::pair { r, r }, std::pair { -r, -r }, std::pair { r, -r }, std::pair { -r, r } }) {
							const int nu = u + du;
							const int nv = v + dv;
							if (nu < 0 || nv < 0 || nu >= FloorTextureSize || nv >= FloorTextureSize)
								continue;
							texel = (*flatArt)[static_cast<size_t>(nv) * FloorTextureSize + nu];
							if (texel > 0)
								break;
						}
					}
					if (texel <= 0) {
						dst[x] = 0;
						continue;
					}
					// The caves' lava glows (PoolGlow), and its colours never go black (a step past the end of their ramp).
					int level = LightLevel(raggedPools ? light - PoolGlow : light, x, y);
					uint8_t color = LightTables[level][texel];
					while (color == 0 && raggedPools && level > 0)
						color = LightTables[--level][texel];
					dst[x] = color;
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
	const bool caves = leveltype == DTYPE_CAVES;
	static std::vector<uint8_t> filled;
	filled.resize(static_cast<size_t>(proj.height));
	ColumnSamplers samplers;
	for (int x = 0; x < proj.width; x++) {
		const Vec2 ray = proj.columnRay(x);
		std::fill(filled.begin(), filled.end(), uint8_t { 0 });
		zBuffer[x] = MaxRayDistance;
		bool hitSomething = false;
		for (int layer = 0; layer < MaxWallLayers; layer++) {
			RayHit hit = CastRay(proj.pos, ray, layer);
			if (!hit.hit)
				break;
			// A cut corner (the caves'): the ray meets the diagonal face inside the tile, or passes through the corner
			// cut off and on.
			const Displacement cut = caves ? ChamferAt(hit.tile) : Displacement { 0, 0 };
			float diagonalAlong = 0.0F;
			if (cut != Displacement { 0, 0 }) {
				const float nx = static_cast<float>(cut.deltaX) * DiagonalNormal;
				const float ny = static_cast<float>(cut.deltaY) * DiagonalNormal;
				// A point of the diagonal: the corner beside the cut one, along x.
				const float qx = static_cast<float>(hit.tile.x + (cut.deltaX > 0 ? 1 : 0));
				const float qy = static_cast<float>(hit.tile.y + (cut.deltaY > 0 ? 0 : 1));
				const float before = (hit.point.x - qx) * nx + (hit.point.y - qy) * ny;
				const float rate = ray.x * nx + ray.y * ny;
				if (rate >= -1e-6F)
					continue;
				const float t = std::max(before, 0.0F) / -rate;
				const Vec2 c { hit.point.x + ray.x * t, hit.point.y + ray.y * t };
				constexpr float Slack = 1e-3F;
				if (c.x < static_cast<float>(hit.tile.x) - Slack || c.x > static_cast<float>(hit.tile.x) + 1.0F + Slack || c.y < static_cast<float>(hit.tile.y) - Slack || c.y > static_cast<float>(hit.tile.y) + 1.0F + Slack)
					continue;
				hit.distance += t;
				hit.point = c;
				// Along the diagonal, the way it runs with the rock on its left (as the crest does).
				diagonalAlong = c.x * -ny + c.y * nx;
			}
			if (!hitSomething)
				zBuffer[x] = hit.distance;
			hitSomething = true;
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
			// The caves' crest shades the rock under it; their rock is shifted along each way a face looks (CaveFaceShifts).
			const bool topShadow = caves && (kind == CleanKind::Wall || kind == CleanKind::Stairs);
			const int facing = hit.xSide ? (hit.previous.x > hit.tile.x ? 0 : 1) : (hit.previous.y > hit.tile.y ? 2 : 3);
			float textureAlong = caves ? worldAlong + CaveFaceShifts[facing] : worldAlong;
			if (cut != Displacement { 0, 0 })
				textureAlong = diagonalAlong + CaveDiagonalShift;
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
					} else if (slope > 0.0F && opening->flat) {
						// A flat top.
						const float t = (opening->spring - h) / slope;
						if (t < exit) {
							exit = t;
							how = 2;
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
				const float shade = topShadow ? CaveTopShadow * std::clamp(1.0F - (wallTop - h) / CaveTopShadowDrop, 0.0F, 1.0F) : 0.0F;
				*out.at(x, y) = samplers[band].At(textureAlong, v, LightLevel(light + shade, x, y), x, y);
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

namespace {

/**
 * @brief The caves' own geometry: the ragged crest along the top of every rock face (CrestSegments), the stalagmites
 * of their lone rock tiles, and their wooden fences and the gates in them. `addTri` adds a triangle (BuildCleanGeometry).
 */
template <typename AddTri>
void AddCaveGeometry(std::vector<CleanQuad> &quads, AddTri &addTri)
{
	using P3 = std::array<float, 3>;
	using UV = std::array<float, 2>;
	const float ceiling = CleanCeiling();
	auto rock = [](Point t) {
		const CleanKind kind = KindAt(t);
		return kind == CleanKind::Wall || kind == CleanKind::Stairs;
	};
	// A quad as two triangles, each facing `outward`.
	auto addQuad = [&](const P3 &a, const P3 &b, const P3 &c, const P3 &d, UV ua, UV ub, UV uc, UV ud, const P3 &outward, CleanSurface surface, float darkness) {
		addTri(a, b, c, ua, ub, uc, outward, surface, darkness);
		addTri(a, c, d, ua, uc, ud, outward, surface, darkness);
	};

	// --- The crest ---------------------------------------------------------------------------------------------------
	// Whether a point lies on the face of a stairwell's opening: the crest hangs no lower there than over its arch.
	auto overOpening = [](float px, float py) {
		for (const CleanStairs &stairs : Stairs) {
			const int fx = stairs.trigger.x - stairs.tile.x;
			const int fy = stairs.trigger.y - stairs.tile.y;
			const float lineAt = fx != 0 ? static_cast<float>(stairs.tile.x) + (fx > 0 ? 1.0F : 0.0F) : static_cast<float>(stairs.tile.y) + (fy > 0 ? 1.0F : 0.0F);
			const float across = fx != 0 ? px : py;
			const float along = fx != 0 ? py : px;
			const float from = static_cast<float>(fx != 0 ? stairs.tile.y : stairs.tile.x);
			if (std::abs(across - lineAt) < 1e-3F && along > from - 1e-3F && along < from + 1.0F + 1e-3F)
				return true;
		}
		return false;
	};
	// The outline of the rock: an edge along every face of it with open ground in front, a cut corner's diagonal
	// instead of the two faces it cuts off, each running with the rock on its left (from `a` to `b`, `n` its outward
	// normal).
	struct Edge {
		float ax, ay, bx, by;
		float nx, ny;
		float top;
	};
	std::vector<Edge> edges;
	constexpr std::array<Displacement, 4> Sides { { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } } };
	for (int y = 0; y < MAXDUNY; y++) {
		for (int x = 0; x < MAXDUNX; x++) {
			const Point tile { x, y };
			if (!rock(tile))
				continue;
			const float top = std::min(WallHeights[static_cast<size_t>(y) * MAXDUNX + x], ceiling);
			const Displacement cut = ChamferAt(tile);
			if (cut != Displacement { 0, 0 }) {
				// The corner towards `cut` is cut off: the diagonal between the two corners beside it.
				const float qx = static_cast<float>(x + (cut.deltaX > 0 ? 1 : 0));
				const float qy = static_cast<float>(y + (cut.deltaY > 0 ? 1 : 0));
				const float nx = static_cast<float>(cut.deltaX) * DiagonalNormal;
				const float ny = static_cast<float>(cut.deltaY) * DiagonalNormal;
				Edge edge { qx, qy - static_cast<float>(cut.deltaY), qx - static_cast<float>(cut.deltaX), qy, nx, ny, top };
				if ((edge.bx - edge.ax) * -ny + (edge.by - edge.ay) * nx < 0.0F) {
					std::swap(edge.ax, edge.bx);
					std::swap(edge.ay, edge.by);
				}
				edges.push_back(edge);
			}
			for (const Displacement d : Sides) {
				const Point front = tile + d;
				if (!InDungeonBounds(front) || rock(front))
					continue;
				if (cut != Displacement { 0, 0 } && (d == Displacement { cut.deltaX, 0 } || d == Displacement { 0, cut.deltaY }))
					continue; // cut off
				const float nx = static_cast<float>(d.deltaX);
				const float ny = static_cast<float>(d.deltaY);
				const float cx = static_cast<float>(x) + 0.5F + 0.5F * nx;
				const float cy = static_cast<float>(y) + 0.5F + 0.5F * ny;
				edges.push_back({ cx + 0.5F * ny, cy - 0.5F * nx, cx - 0.5F * ny, cy + 0.5F * nx, nx, ny, top });
			}
		}
	}
	// The edges meeting at each corner of the outline: the one leaving it and the one arriving.
	auto cornerKey = [](float px, float py) { return static_cast<int>(std::lround(px)) * 1024 + static_cast<int>(std::lround(py)); };
	std::unordered_map<int, size_t> leaving;
	std::unordered_map<int, size_t> arriving;
	for (size_t i = 0; i < edges.size(); i++) {
		leaving.emplace(cornerKey(edges[i].ax, edges[i].ay), i);
		arriving.emplace(cornerKey(edges[i].bx, edges[i].by), i);
	}
	// Where the rims of two edges meet at a corner: out along both normals, so that each is `out` from its own edge.
	auto mitre = [](float n1x, float n1y, float n2x, float n2y, float &dx, float &dy) {
		const float k = 1.0F + n1x * n2x + n1y * n2y;
		if (k < 0.3F) {
			dx = n1x;
			dy = n1y;
			return;
		}
		dx = (n1x + n2x) / k;
		dy = (n1y + n2y) / k;
	};
	// The crest's section at a point of an edge: where its underside meets the face, its lip, its bulge and its top
	// edge, pushed out along `dir` (the edge's normal, or at a corner the mitre of the two).
	struct Section {
		P3 under;
		P3 lip;
		P3 bulge;
		P3 top;
	};
	auto section = [&](float px, float py, float dirX, float dirY, float top) {
		const float out = CrestOutMin + (CrestOutMax - CrestOutMin) * CaveNoise(px, py, 1);
		float drop = CrestDropMin + (CrestDropMax - CrestDropMin) * CaveNoise(px, py, 2);
		if (overOpening(px, py))
			drop = std::min(drop, CrestOverOpening);
		const float r = CaveNoise(px, py, 3);
		const float rise = CrestRiseMax * r * r;
		const float bulge = CrestBulgeMax * CaveNoise(px, py, 4);
		auto at = [&](float o, float z) { return P3 { px + dirX * o, py + dirY * o, z }; };
		return Section { at(0.0F, top - drop + CrestUndercut * out), at(out, top - drop), at(out + bulge, top - drop * 0.35F), at(out * 0.5F, top + rise) };
	};
	// A root: a thin strand hanging `hang` from `from`, swaying `sway` out from the rock (along n) and back, wandering
	// `wander` along it (e), tapering; two ribbons crossed, seen from both sides.
	auto addRoot = [&](const P3 &from, float hang, float sway, float wander, float nx, float ny, float ex, float ey) {
		std::array<P3, RootPoints> strand;
		for (int j = 0; j < RootPoints; j++) {
			const float f = static_cast<float>(j) / (RootPoints - 1);
			const float o = sway * std::sin(f * 3.14159265F) + 0.03F * f;
			const float w = wander * std::sin(f * 4.5F);
			strand[j] = P3 { from[0] + nx * o + ex * w, from[1] + ny * o + ey * w, from[2] - hang * f };
		}
		for (const auto &[wx, wy] : { std::pair { ex, ey }, std::pair { nx, ny } }) {
			for (int j = 0; j + 1 < RootPoints; j++) {
				const float w0 = RootWidth * (1.0F - 0.7F * static_cast<float>(j) / (RootPoints - 1));
				const float w1 = RootWidth * (1.0F - 0.7F * static_cast<float>(j + 1) / (RootPoints - 1));
				const P3 &p = strand[j];
				const P3 &q = strand[j + 1];
				const P3 c0 { p[0] - wx * w0, p[1] - wy * w0, p[2] };
				const P3 c1 { p[0] + wx * w0, p[1] + wy * w0, p[2] };
				const P3 c2 { q[0] + wx * w1, q[1] + wy * w1, q[2] };
				const P3 c3 { q[0] - wx * w1, q[1] - wy * w1, q[2] };
				const size_t first = quads.size();
				addQuad(c0, c1, c2, c3, UV { -w0, -p[2] }, UV { w0, -p[2] }, UV { w1, -q[2] }, UV { -w1, -q[2] }, P3 { -wy, wx, 0.0F }, CleanSurface::Wood, RootDarkness);
				for (size_t i = first; i < quads.size(); i++)
					quads[i].twoSided = true;
			}
		}
	};
	int roots = 0;
	for (const Edge &edge : edges) {
		const float length = std::hypot(edge.bx - edge.ax, edge.by - edge.ay);
		const float ex = (edge.bx - edge.ax) / length;
		const float ey = (edge.by - edge.ay) / length;
		const int segments = std::max(1, static_cast<int>(std::lround(length * CrestSegments)));
		float d0x = edge.nx;
		float d0y = edge.ny;
		float d1x = edge.nx;
		float d1y = edge.ny;
		if (const auto it = arriving.find(cornerKey(edge.ax, edge.ay)); it != arriving.end())
			mitre(edges[it->second].nx, edges[it->second].ny, edge.nx, edge.ny, d0x, d0y);
		if (const auto it = leaving.find(cornerKey(edge.bx, edge.by)); it != leaving.end())
			mitre(edge.nx, edge.ny, edges[it->second].nx, edges[it->second].ny, d1x, d1y);
		std::vector<Section> sections(static_cast<size_t>(segments) + 1);
		for (int k = 0; k <= segments; k++) {
			const float t = static_cast<float>(k) / static_cast<float>(segments);
			const float dx = k == 0 ? d0x : (k == segments ? d1x : edge.nx);
			const float dy = k == 0 ? d0y : (k == segments ? d1y : edge.ny);
			sections[k] = section(edge.ax + (edge.bx - edge.ax) * t, edge.ay + (edge.by - edge.ay) * t, dx, dy, edge.top);
		}
		// Texture: along the edge, down from the top; the underside along and out from the face.
		auto upright = [&](const P3 &p) { return UV { p[0] * ex + p[1] * ey, -p[2] }; };
		auto flat = [&](const P3 &p) { return UV { p[0] * ex + p[1] * ey, (p[0] - edge.ax) * edge.nx + (p[1] - edge.ay) * edge.ny }; };
		const P3 out { edge.nx, edge.ny, 0.0F };
		const P3 down { edge.nx * 0.5F, edge.ny * 0.5F, -1.0F };
		const P3 up { edge.nx, edge.ny, 0.3F };
		for (int k = 0; k < segments; k++) {
			const Section &a = sections[k];
			const Section &b = sections[k + 1];
			addQuad(a.under, b.under, b.lip, a.lip, flat(a.under), flat(b.under), flat(b.lip), flat(a.lip), down, CleanSurface::Crest, CrestUndersideDarkness);
			addQuad(a.lip, b.lip, b.bulge, a.bulge, upright(a.lip), upright(b.lip), upright(b.bulge), upright(a.bulge), out, CleanSurface::Crest, CrestLowerDarkness);
			addQuad(a.bulge, b.bulge, b.top, a.top, upright(a.bulge), upright(b.bulge), upright(b.top), upright(a.top), up, CleanSurface::Crest, 0.0F);
			// Now and then roots hang from the lip.
			const float mx = (a.lip[0] + b.lip[0]) / 2.0F;
			const float my = (a.lip[1] + b.lip[1]) / 2.0F;
			const int hx = static_cast<int>(std::lround(mx * 16.0F));
			const int hy = static_cast<int>(std::lround(my * 16.0F));
			if (Hash01(hx, hy, 31) >= RootChance)
				continue;
			roots++;
			// One to three strands, side by side, of different lengths.
			const int strands = 1 + static_cast<int>(Hash01(hx, hy, 35) * 2.99F);
			for (int strand = 0; strand < strands; strand++) {
				const float t = std::clamp(0.25F + 0.5F * Hash01(hx, hy, 32) + (static_cast<float>(strand) - static_cast<float>(strands - 1) / 2.0F) * RootSpacing, 0.0F, 1.0F);
				const P3 from { a.lip[0] + (b.lip[0] - a.lip[0]) * t - edge.nx * 0.02F, a.lip[1] + (b.lip[1] - a.lip[1]) * t - edge.ny * 0.02F, a.lip[2] + (b.lip[2] - a.lip[2]) * t + 0.05F };
				const float hang = (RootMin + (RootMax - RootMin) * Hash01(hx, hy, 33)) * (strand == 0 ? 1.0F : 0.5F + 0.4F * Hash01(hx + strand, hy, 36));
				const float sway = (Hash01(hx, hy + strand, 34) - 0.3F) * 0.16F;
				const float wander = (Hash01(hx + strand, hy, 37) - 0.5F) * 0.12F;
				addRoot(from, hang, sway, wander, edge.nx, edge.ny, ex, ey);
			}
		}
	}
	int cuts = 0;
	for (const Edge &edge : edges)
		cuts += edge.nx != 0.0F && edge.ny != 0.0F ? 1 : 0;

	// --- Stalagmites -------------------------------------------------------------------------------------------------
	// A spike of rock: rings narrowing up to a leaning tip, each point pushed in or out a little.
	auto addSpike = [&](float cx, float cy, float radius, float height, uint32_t salt) {
		constexpr float Pi = 3.14159265F;
		const float leanX = (Hash01(static_cast<int>(salt), 1, 7) - 0.5F) * 0.25F * radius;
		const float leanY = (Hash01(static_cast<int>(salt), 2, 7) - 0.5F) * 0.25F * radius;
		std::array<std::array<P3, StalagmiteSides>, StalagmiteRings> rings;
		std::array<float, StalagmiteSides + 1> arc {};
		for (int j = 0; j < StalagmiteRings; j++) {
			const float t = static_cast<float>(j) / StalagmiteRings;
			const float r = radius * std::pow(1.0F - t, 0.8F);
			const float ox = cx + leanX * t * t;
			const float oy = cy + leanY * t * t;
			for (int i = 0; i < StalagmiteSides; i++) {
				const float angle = 2.0F * Pi * (static_cast<float>(i) + (Hash01(i, j, salt) - 0.5F) * 0.4F) / StalagmiteSides;
				const float rr = r * (0.78F + 0.44F * Hash01(i, j, salt + 1U));
				rings[j][i] = P3 { ox + rr * std::cos(angle), oy + rr * std::sin(angle), height * t };
			}
		}
		for (int i = 0; i < StalagmiteSides; i++)
			arc[i + 1] = arc[i] + 2.0F * Pi * radius / StalagmiteSides;
		const P3 tip { cx + leanX, cy + leanY, height };
		for (int i = 0; i < StalagmiteSides; i++) {
			const int i1 = (i + 1) % StalagmiteSides;
			const float am = 2.0F * Pi * (static_cast<float>(i) + 0.5F) / StalagmiteSides;
			const P3 outward { std::cos(am), std::sin(am), 0.3F };
			for (int j = 0; j + 1 < StalagmiteRings; j++) {
				const P3 &a = rings[j][i];
				const P3 &b = rings[j][i1];
				const P3 &c = rings[j + 1][i1];
				const P3 &d = rings[j + 1][i];
				addQuad(a, b, c, d, UV { arc[i], -a[2] }, UV { arc[i + 1], -b[2] }, UV { arc[i + 1], -c[2] }, UV { arc[i], -d[2] }, outward, CleanSurface::Pillar, 0.0F);
			}
			const P3 &a = rings[StalagmiteRings - 1][i];
			const P3 &b = rings[StalagmiteRings - 1][i1];
			addTri(a, b, tip, UV { arc[i], -a[2] }, UV { arc[i + 1], -b[2] }, UV { (arc[i] + arc[i + 1]) / 2.0F, -height }, outward, CleanSurface::Pillar, 0.0F);
		}
	};
	int stalagmites = 0;
	for (int y = 0; y < MAXDUNY; y++) {
		for (int x = 0; x < MAXDUNX; x++) {
			if (Kinds[static_cast<size_t>(y) * MAXDUNX + x] != CleanKind::Pillar)
				continue;
			stalagmites++;
			const uint32_t salt = static_cast<uint32_t>(y * MAXDUNX + x) * 16U;
			const float cx = static_cast<float>(x) + 0.5F + (Hash01(x, y, 11) - 0.5F) * 0.12F;
			const float cy = static_cast<float>(y) + 0.5F + (Hash01(x, y, 12) - 0.5F) * 0.12F;
			const float height = ceiling * (StalagmiteMin + (StalagmiteMax - StalagmiteMin) * Hash01(x, y, 13));
			const float radius = StalagmiteRadius * (0.85F + 0.3F * Hash01(x, y, 14));
			addSpike(cx, cy, radius, height, salt);
			// Most stand in a cluster: one or two smaller spikes beside the big one.
			for (int k = 0; k < 2; k++) {
				if (Hash01(x, y, 15 + k) < 0.4F)
					continue;
				const float angle = 6.2831853F * Hash01(x, y, 17 + k);
				const float share = 0.4F + 0.25F * Hash01(x, y, 19 + k);
				const float reach = radius * (0.75F + 0.35F * share);
				addSpike(cx + reach * std::cos(angle), cy + reach * std::sin(angle), radius * share, height * share, salt + 4U + static_cast<uint32_t>(k) * 4U);
			}
		}
	}

	// --- Fences and gates --------------------------------------------------------------------------------------------
	// A wooden beam, an axis-aligned box: its sides and top (and its underside if asked), the grain running up a post
	// (`upright`), else along the beam.
	auto addBeam = [&](float x0, float y0, float z0, float x1, float y1, float z1, bool upright, bool under) {
		auto side = [&](P3 a, P3 b, P3 outward, int axis) {
			const P3 p0 { a[0], a[1], z0 };
			const P3 p1 { b[0], b[1], z0 };
			const P3 p2 { b[0], b[1], z1 };
			const P3 p3 { a[0], a[1], z1 };
			if (upright)
				addQuad(p0, p1, p2, p3, UV { a[axis], -z0 }, UV { b[axis], -z0 }, UV { b[axis], -z1 }, UV { a[axis], -z1 }, outward, CleanSurface::Wood, 0.0F);
			else
				addQuad(p0, p1, p2, p3, UV { -z0, a[axis] }, UV { -z0, b[axis] }, UV { -z1, b[axis] }, UV { -z1, a[axis] }, outward, CleanSurface::Wood, 0.0F);
		};
		side({ x0, y1, 0 }, { x1, y1, 0 }, { 0, 1, 0 }, 0);
		side({ x1, y0, 0 }, { x0, y0, 0 }, { 0, -1, 0 }, 0);
		side({ x1, y1, 0 }, { x1, y0, 0 }, { 1, 0, 0 }, 1);
		side({ x0, y0, 0 }, { x0, y1, 0 }, { -1, 0, 0 }, 1);
		const bool alongX = x1 - x0 >= y1 - y0;
		auto flatUV = [&](float px, float py) { return alongX ? UV { py, px } : UV { px, py }; };
		addQuad(P3 { x0, y0, z1 }, P3 { x1, y0, z1 }, P3 { x1, y1, z1 }, P3 { x0, y1, z1 }, flatUV(x0, y0), flatUV(x1, y0), flatUV(x1, y1), flatUV(x0, y1), P3 { 0, 0, 1 }, CleanSurface::Wood, 0.0F);
		if (under)
			addQuad(P3 { x0, y0, z0 }, P3 { x1, y0, z0 }, P3 { x1, y1, z0 }, P3 { x0, y1, z0 }, flatUV(x0, y0), flatUV(x1, y0), flatUV(x1, y1), flatUV(x0, y1), P3 { 0, 0, -1 }, CleanSurface::Wood, 0.0F);
	};
	// A beam centred on the line `across` through a tile, from `from` to `to` along it.
	auto addRail = [&](bool alongX, float across, float from, float to, float z) {
		const float a0 = std::min(from, to);
		const float a1 = std::max(from, to);
		if (alongX)
			addBeam(a0, across - FenceRailHalf, z - FenceRailHalf * 1.4F, a1, across + FenceRailHalf, z + FenceRailHalf * 1.4F, false, false);
		else
			addBeam(across - FenceRailHalf, a0, z - FenceRailHalf * 1.4F, across + FenceRailHalf, a1, z + FenceRailHalf * 1.4F, false, false);
	};
	auto addPost = [&](float px, float py, float height) {
		addBeam(px - FencePostHalf, py - FencePostHalf, 0.0F, px + FencePostHalf, py + FencePostHalf, height, true, false);
	};
	auto fenceLike = [](Point t) {
		const CleanKind kind = KindAt(t);
		return kind == CleanKind::Fence || kind == CleanKind::Doorway;
	};
	auto meets = [&](Point t) { return fenceLike(t) || rock(t) || KindAt(t) == CleanKind::Pillar; };
	const CleanOpening gate = CleanDoorOpening();
	int fences = 0;
	for (int y = 0; y < MAXDUNY; y++) {
		for (int x = 0; x < MAXDUNX; x++) {
			const Point tile { x, y };
			const CleanKind kind = KindAt(tile);
			if (kind != CleanKind::Fence && kind != CleanKind::Doorway)
				continue;
			fences++;
			const float cx = static_cast<float>(x) + 0.5F;
			const float cy = static_cast<float>(y) + 0.5F;
			// Which way it runs: along its fence neighbours, else across the gap between the rock either side.
			bool runsX = fenceLike(tile + Displacement { 1, 0 }) || fenceLike(tile + Displacement { -1, 0 });
			bool runsY = fenceLike(tile + Displacement { 0, 1 }) || fenceLike(tile + Displacement { 0, -1 });
			if (kind == CleanKind::Doorway) {
				const DoorwaySpan span = DoorwaySpanAt(tile);
				runsX = span.door != nullptr ? span.door->alongX : runsX;
				runsY = !runsX;
			} else if (!runsX && !runsY) {
				runsX = meets(tile + Displacement { 1, 0 }) && meets(tile + Displacement { -1, 0 });
				runsY = !runsX;
			}
			for (const bool alongX : { true, false }) {
				if (alongX ? !runsX : !runsY)
					continue;
				const float across = alongX ? cy : cx;
				const float middle = alongX ? cx : cy;
				// A gate: a post either side of its leaf, a lintel over it; the rails run from the posts.
				const bool isGate = kind == CleanKind::Doorway;
				const float postAt = isGate ? gate.halfWidth + FencePostHalf : 0.0F;
				for (const int sign : { -1, 1 }) {
					const Point next = tile + (alongX ? Displacement { sign, 0 } : Displacement { 0, sign });
					const float post = middle + static_cast<float>(sign) * postAt;
					if (isGate)
						addPost(alongX ? post : across, alongX ? across : post, GateHeight + GateLintel);
					if (!meets(next))
						continue;
					const float edge = middle + 0.5F * static_cast<float>(sign);
					for (const float z : FenceRails)
						addRail(alongX, across, post + static_cast<float>(sign) * FencePostHalf, edge, z);
					if (sign > 0 && fenceLike(next))
						addPost(alongX ? edge : across, alongX ? across : edge, FenceHeight);
				}
				if (isGate) {
					const float half = gate.halfWidth + 2.0F * FencePostHalf;
					const float z0 = GateHeight - GateLintelOverlap;
					if (alongX)
						addBeam(middle - half, across - FencePostHalf, z0, middle + half, across + FencePostHalf, GateHeight + GateLintel, false, true);
					else
						addBeam(across - FencePostHalf, middle - half, z0, across + FencePostHalf, middle + half, GateHeight + GateLintel, false, true);
				}
			}
			if (kind == CleanKind::Fence)
				addPost(cx, cy, FenceHeight);
		}
	}
	// Shoring: posts and beams along the faces of the rock it props.
	int shored = 0;
	for (int y = 0; y < MAXDUNY; y++) {
		for (int x = 0; x < MAXDUNX; x++) {
			const Point tile { x, y };
			if (!rock(tile) || !IsShoringMegatile(tile))
				continue;
			for (const Displacement d : Sides) {
				const Point front = tile + d;
				if (!InDungeonBounds(front) || rock(front) || KindAt(front) != CleanKind::Open)
					continue;
				shored++;
				const bool alongX = d.deltaY != 0; // the face runs along x when it faces y
				const float face = alongX ? static_cast<float>(y) + (d.deltaY > 0 ? 1.0F : 0.0F) : static_cast<float>(x) + (d.deltaX > 0 ? 1.0F : 0.0F);
				const float across = face + static_cast<float>(alongX ? d.deltaY : d.deltaX) * ShoringOut;
				const float from = static_cast<float>(alongX ? x : y);
				for (const float z : ShoringBeams)
					addRail(alongX, across, from, from + 1.0F, z);
				for (const float at : { 0.25F, 0.75F })
					addPost(alongX ? from + at : across, alongX ? across : from + at, ShoringHeight);
			}
		}
	}
	// The stairwells' timber frames: a post inside each side of the opening and a lintel under its top, standing a
	// little out of the face.
	const CleanOpening stairsOpening = StairsOpening();
	for (const CleanStairs &stairs : Stairs) {
		const int fx = stairs.trigger.x - stairs.tile.x;
		const int fy = stairs.trigger.y - stairs.tile.y;
		const bool xFace = fx != 0; // the face looks towards x (and runs along y)
		const int sign = xFace ? fx : fy;
		const float face = static_cast<float>(xFace ? stairs.tile.x : stairs.tile.y) + (sign > 0 ? 1.0F : 0.0F);
		const float in0 = sign > 0 ? face - 0.02F : face - StairsFrameOut;
		const float in1 = sign > 0 ? face + StairsFrameOut : face + 0.02F;
		const float middle = static_cast<float>(xFace ? stairs.tile.y : stairs.tile.x) + 0.5F;
		const float a0 = middle - stairsOpening.halfWidth;
		const float a1 = middle + stairsOpening.halfWidth;
		const float top = stairsOpening.spring;
		auto box = [&](float along0, float along1, float z0, float z1, bool upright, bool under) {
			if (xFace)
				addBeam(in0, along0, z0, in1, along1, z1, upright, under);
			else
				addBeam(along0, in0, z0, along1, in1, z1, upright, under);
		};
		box(a0, a0 + StairsFramePost, 0.0F, top, true, false);
		box(a1 - StairsFramePost, a1, 0.0F, top, true, false);
		box(a0 - 0.05F, a1 + 0.05F, top - StairsFrameLintel, top + 0.03F, false, true);
	}
	LogInfo("fp: caves: crest on {} edges ({} cut corners), {} roots, {} stalagmites, {} fence and gate tiles, shoring on {} faces", edges.size(), cuts, roots, stalagmites, fences, shored);
}

} // namespace

void BuildCleanGeometry(std::vector<CleanQuad> &quads, std::vector<Point> &stands, std::vector<std::array<float, 4>> &holes)
{
	EnsureKindsForLevel();
	quads.clear();
	stands.clear();
	holes.clear();
	if (IsHell()) {
		BuildHellGeometry(quads);
		return;
	}
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

	// A triangle, its normal worked out from its corners and turned to face `outward`.
	using UV = std::array<float, 2>;
	auto addTri = [&](P3 a, P3 b, P3 c, UV ua, UV ub, UV uc, P3 outward, CleanSurface surface, float darkness) {
		const P3 e1 { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
		const P3 e2 { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
		P3 n { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
		const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
		if (len < 1e-7F)
			return;
		if (n[0] * outward[0] + n[1] * outward[1] + n[2] * outward[2] < 0.0F) {
			n = { -n[0], -n[1], -n[2] };
			std::swap(b, c);
			std::swap(ub, uc);
		}
		quads.push_back({ { a, b, c, c }, { n[0] / len, n[1] / len, n[2] / len }, { ua, ub, uc, uc }, surface, darkness, true });
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
			if (dSpecial[x][y] <= 0 || Kinds[static_cast<size_t>(y) * MAXDUNX + x] != CleanKind::Open || done[static_cast<size_t>(y) * MAXDUNX + x] != 0 || leveltype == DTYPE_CAVES)
				continue; // (the caves have no masonry)
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
	// The pillars, now that the arches resting on them are known (the caves' are stalagmites, below).
	for (int y = 0; y < MAXDUNY; y++)
		for (int x = 0; x < MAXDUNX; x++)
			if (Kinds[static_cast<size_t>(y) * MAXDUNX + x] == CleanKind::Pillar && leveltype != DTYPE_CAVES)
				addPillar(static_cast<float>(x) + 0.5F, static_cast<float>(y) + 0.5F, carrying[static_cast<size_t>(y) * MAXDUNX + x]);
	// Stairwells: steps inside the opening beside each stairs trigger, down into a pit or up into the dark.
	const bool woodenSteps = leveltype == DTYPE_CAVES;
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
			// Risers a shade darker than the treads, so the flight reads as steps (the caves': wooden).
			quads.push_back({ { r0, r1, r2, r3 }, facing, { { { -half, -riserBottom }, { half, -riserBottom }, { half, -tread }, { -half, -tread } } }, woodenSteps ? CleanSurface::Wood : CleanSurface::Arch, darkness + 1.5F });
			const P3 t0 = at(in0, -half, tread);
			const P3 t1 = at(in0, half, tread);
			const P3 t2 = at(in1, half, tread);
			const P3 t3 = at(in1, -half, tread);
			quads.push_back({ { t0, t1, t2, t3 }, P3 { 0, 0, 1 }, { { { t0[0], t0[1] }, { t1[0], t1[1] }, { t2[0], t2[1] }, { t3[0], t3[1] } } }, woodenSteps ? CleanSurface::Wood : CleanSurface::Stone, darkness });
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
	if (leveltype == DTYPE_CAVES)
		AddCaveGeometry(quads, addTri);
	LogInfo("fp: clean dungeon geometry: {} quads, {} arches ({} pillars carrying them), {} torch stands, {} stairs", quads.size(), arches, carriers, stands.size(), Stairs.size());
}

#ifdef _DEBUG
std::string CleanTileMap(int x0, int y0, int x1, int y1)
{
	PrepareCleanDungeon();
	// '#' wall, 'l' low wall, 'c' a wall with a corner cut (the caves'), 'o' pillar, 't' torch stand, 'D' doorway, 'S'
	// stairs, '~' lava or water, 'f' fence, '^' an arch over the tile, '.' floor.
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
			case CleanKind::Wall: c = IsHell() ? (IsHellBlood({ x, y }) ? '~' : '#') : (WallHeights[static_cast<size_t>(y) * MAXDUNX + x] < CleanCeiling() - 0.01F ? 'l' : (ChamferAt({ x, y }) != Displacement { 0, 0 } ? 'c' : '#')); break;
			case CleanKind::Pillar: c = 'o'; break;
			case CleanKind::Stand: c = 't'; break;
			case CleanKind::Doorway: c = 'D'; break;
			case CleanKind::Stairs: c = 'S'; break;
			case CleanKind::Flat: c = '~'; break;
			case CleanKind::Fence: c = 'f'; break;
			case CleanKind::Prop: c = 'p'; break;
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
