/**
 * @file fp/fp_sky.cpp
 *
 * The night sky over town, and hell's dark red haze over its open pits: cube maps drawn behind everything else,
 * looked up per pixel through the camera's ray.
 *
 * World axes: x and y along the tile grid (y down the map), z up. The faces are ft (+x), lf (+y), bk (-x), rt (-y)
 * and up (+z), as export_sky.py describes; the down face is never seen (the ground is in the way) and is not shipped.
 */
#include "fp/fp_sky.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>

#ifdef DEVILUTIONX_FP_ZLIB
#include <zlib.h>
#endif

#include "engine/palette.h"
#include "fp/fp_textures.hpp"
#include "levels/gendung.h"
#include "utils/log.hpp"
#include "utils/paths.h"
#include "utils/str_cat.hpp"

namespace devilution::fp {

namespace {

enum SkyFace : uint8_t {
	FaceFront, // +x
	FaceLeft,  // +y
	FaceBack,  // -x
	FaceRight, // -y
	FaceUp,    // +z
};

struct SkyBox {
	int size = 0;
	int sideRows = 0;
	std::array<SDL_Color, 256> palette {};
	std::array<std::vector<uint8_t>, 5> faces;
	std::array<uint8_t, 256> remap {}; // stored palette index -> nearest colour of the game's palette
	uint32_t remapFor = 0;
	bool loaded = false;
};

/** The town's night sky (sky.bin) and hell's dark red haze (hellsky.bin, made by export_hell_sky.py). */
SkyBox TownSky;
SkyBox HellSky;
bool TownSkyTried = false;
bool HellSkyTried = false;
bool SkyOn = true;

FILE *OpenSkyAsset(const char *envName, const char *fileName)
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

void LoadSky(SkyBox &sky, const char *envName, const char *fileName)
{
#ifdef DEVILUTIONX_FP_ZLIB
	FILE *f = OpenSkyAsset(envName, fileName);
	if (f == nullptr) {
		LogInfo("fp: {} not found; the sky is a flat colour", fileName);
		return;
	}
	auto readAll = [&](void *dst, size_t size) { return std::fread(dst, 1, size, f) == size; };
	char magic[4];
	uint16_t dims[2];
	std::array<uint8_t, 768> palette;
	uint32_t packedSize = 0;
	if (!readAll(magic, 4) || std::memcmp(magic, "FPS1", 4) != 0 || !readAll(dims, sizeof(dims)) || !readAll(palette.data(), palette.size()) || !readAll(&packedSize, 4)) {
		std::fclose(f);
		return;
	}
	std::vector<uint8_t> packed(packedSize);
	const bool ok = readAll(packed.data(), packed.size());
	std::fclose(f);
	if (!ok)
		return;
	const int size = dims[0];
	const int sideRows = dims[1];
	if (size <= 0 || sideRows <= 0 || sideRows > size)
		return;
	const size_t sideBytes = static_cast<size_t>(sideRows) * size;
	const size_t upBytes = static_cast<size_t>(size) * size;
	std::vector<uint8_t> data(sideBytes * 4 + upBytes);
	uLongf length = static_cast<uLongf>(data.size());
	if (uncompress(data.data(), &length, packed.data(), static_cast<uLong>(packed.size())) != Z_OK || length != data.size()) {
		LogInfo("fp: {} is damaged", fileName);
		return;
	}
	for (int i = 0; i < 256; i++)
		sky.palette[i] = SDL_Color { palette[i * 3], palette[i * 3 + 1], palette[i * 3 + 2], 255 };
	for (int face = 0; face < 4; face++)
		sky.faces[face].assign(data.begin() + static_cast<ptrdiff_t>(sideBytes * face), data.begin() + static_cast<ptrdiff_t>(sideBytes * (face + 1)));
	sky.faces[FaceUp].assign(data.begin() + static_cast<ptrdiff_t>(sideBytes * 4), data.end());
	sky.size = size;
	sky.sideRows = sideRows;
	sky.loaded = true;
	LogInfo("fp: loaded the {} px sky {}", size, fileName);
#else
	LogInfo("fp: built without zlib; the town sky is a flat colour");
#endif
}

/**
 * @brief Maps the sky's own palette onto the game's, once per palette (in town they are the same colours). Hell's
 * cycled blood (1-31) and the bright UI ramps (128-159) are left out.
 */
void PrepareRemap(SkyBox &sky)
{
	if (sky.remapFor == PaletteVersion())
		return;
	sky.remapFor = PaletteVersion();
	const bool hell = leveltype == DTYPE_HELL;
	for (int i = 0; i < 256; i++) {
		const SDL_Color c = sky.palette[i];
		int best = std::numeric_limits<int>::max();
		sky.remap[i] = static_cast<uint8_t>(i);
		for (int j = 1; j < 256; j++) {
			if (hell && (j < 32 || (j >= 128 && j < 160) || j == 255))
				continue;
			const SDL_Color o = logical_palette[j];
			const int d = (o.r - c.r) * (o.r - c.r) * 3 + (o.g - c.g) * (o.g - c.g) * 6 + (o.b - c.b) * (o.b - c.b);
			if (d < best) {
				best = d;
				sky.remap[i] = static_cast<uint8_t>(j);
				if (d == 0)
					break;
			}
		}
	}
}

/** @brief The face a direction (z >= 0) goes through and where, in texels of that face. */
struct SkyTexel {
	uint8_t face;
	float col;
	float row;
};

SkyTexel Lookup(const SkyBox &sky, float dx, float dy, float dz)
{
	const float ax = std::abs(dx);
	const float ay = std::abs(dy);
	const float half = static_cast<float>(sky.size) * 0.5F;
	float u;
	float v;
	uint8_t face;
	if (dz >= ax && dz >= ay) {
		face = FaceUp;
		u = dx / dz;
		v = dy / dz;
	} else if (ax >= ay) {
		// Seen from inside, looking along +x the right is +y; along -x it is -y.
		face = dx > 0 ? FaceFront : FaceBack;
		u = dy / dx;
		v = dx > 0 ? -dz / dx : dz / dx;
	} else {
		// Along +y the right is -x; along -y it is +x.
		face = dy > 0 ? FaceLeft : FaceRight;
		u = -dx / dy;
		v = dy > 0 ? -dz / dy : dz / dy;
	}
	return { face, (u + 1.0F) * half, (v + 1.0F) * half };
}

constexpr float Infinity = std::numeric_limits<float>::infinity();

/** Pixels between exact look-ups along a row; in between, the texel position is interpolated (Quake's sky did the same). */
constexpr int SkySpan = 16;

} // namespace

bool SkyEnabled()
{
	return SkyOn;
}

void SetSkyEnabled(bool enabled)
{
	SkyOn = enabled;
}

bool DrawSky(const Surface &out, const Projection &proj, const std::vector<float> &depthBuffer)
{
	if (!SkyOn || (leveltype != DTYPE_TOWN && leveltype != DTYPE_HELL))
		return false;
	const bool town = leveltype == DTYPE_TOWN;
	SkyBox &sky = town ? TownSky : HellSky;
	bool &tried = town ? TownSkyTried : HellSkyTried;
	if (!tried) {
		tried = true;
		if (town)
			LoadSky(sky, "FP_SKY", "sky.bin");
		else
			LoadSky(sky, "FP_HELL_SKY", "hellsky.bin");
	}
	if (!sky.loaded)
		return false;
	PrepareRemap(sky);
	const int size = sky.size;
	const int lastRow[5] = { sky.sideRows - 1, sky.sideRows - 1, sky.sideRows - 1, sky.sideRows - 1, size - 1 };
	const std::array<const uint8_t *, 5> faces { sky.faces[0].data(), sky.faces[1].data(), sky.faces[2].data(), sky.faces[3].data(), sky.faces[4].data() };
	const uint8_t *remap = sky.remap.data();
	auto texel = [&](uint8_t face, float col, float row) {
		const int c = std::clamp(static_cast<int>(col), 0, size - 1);
		const int r = std::clamp(static_cast<int>(row), 0, lastRow[face]);
		return remap[faces[face][static_cast<size_t>(r) * size + c]];
	};
	const int lastY = std::min(proj.horizon, proj.height - 1);
	for (int y = 0; y <= lastY; y++) {
		uint8_t *dst = out.at(0, y);
		const float *depth = &depthBuffer[static_cast<size_t>(y) * proj.width];
		const float dz = std::max(proj.rowSlope(y), 0.0F) + 1e-4F; // the horizon row looks just above the horizon
		for (int x0 = 0; x0 < proj.width; x0 += SkySpan) {
			const int x1 = std::min(x0 + SkySpan, proj.width);
			bool open = false;
			for (int x = x0; x < x1 && !open; x++)
				open = depth[x] == Infinity;
			if (!open)
				continue;
			const Vec2 r0 = proj.columnRay(x0);
			const Vec2 r1 = proj.columnRay(x1);
			const SkyTexel a = Lookup(sky, r0.x, r0.y, dz);
			const SkyTexel b = Lookup(sky, r1.x, r1.y, dz);
			if (a.face == b.face) {
				// Both ends clamped to the face: everything between them is inside it too. 16.16 fixed point.
				const float maxRow = static_cast<float>(lastRow[a.face]);
				const float maxCol = static_cast<float>(size - 1);
				const int32_t c0 = static_cast<int32_t>(std::clamp(a.col, 0.0F, maxCol) * 65536.0F);
				const int32_t c1 = static_cast<int32_t>(std::clamp(b.col, 0.0F, maxCol) * 65536.0F);
				const int32_t r0 = static_cast<int32_t>(std::clamp(a.row, 0.0F, maxRow) * 65536.0F);
				const int32_t r1 = static_cast<int32_t>(std::clamp(b.row, 0.0F, maxRow) * 65536.0F);
				const int32_t stepCol = (c1 - c0) / (x1 - x0);
				const int32_t stepRow = (r1 - r0) / (x1 - x0);
				const uint8_t *face = faces[a.face];
				int32_t col = c0;
				int32_t row = r0;
				for (int x = x0; x < x1; x++, col += stepCol, row += stepRow) {
					if (depth[x] == Infinity)
						dst[x] = remap[face[static_cast<size_t>(row >> 16) * size + (col >> 16)]];
				}
			} else {
				for (int x = x0; x < x1; x++) {
					if (depth[x] != Infinity)
						continue;
					const Vec2 ray = proj.columnRay(x);
					const SkyTexel t = Lookup(sky, ray.x, ray.y, dz);
					dst[x] = texel(t.face, t.col, t.row);
				}
			}
		}
	}
	return true;
}

} // namespace devilution::fp
