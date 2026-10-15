/**
 * @file fp/fp_weapon.cpp
 *
 * The weapon in the player's hand, drawn over the first-person view with the first-person weapon art of
 * The Elder Scrolls II: Daggerfall. The art is read at run time from the player's own Daggerfall install
 * (FP_DAGGERFALL_DIR, the ARENA2 folder, else the "daggerfall" folder next to the game, where the installers copy it
 * from that install) and never shipped; without it no weapon is drawn.
 *
 * WEAPONnn.CIF: a 320 x 200 screen's worth of sprites. The first record is a plain image (x, y, width, height,
 * 0, size, then the pixels); the rest are animations with a 76-byte header (width, height, width again, x, y,
 * a word, 31 frame offsets from the record start, the record size) and frames in a byte RLE (a count over 127:
 * that many minus 127 copies of the next byte, else that many plus one bytes as they are). Records: 0 at rest,
 * 1 strike down, 2 down-left, 3 left, 4 right, 5 down-right, 6 thrust. The bow has a single record.
 */
#include "fp/fp_weapon.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

#include "engine/palette.h"
#include "engine/light_tables.hpp"
#include "fp/fp_controls.hpp"
#include "fp/fp_textures.hpp"
#include "fp/fp_world.hpp"
#include "items.h"
#include "levels/dun_tile_data.hpp"
#include "player.h"
#include "utils/paths.h"
#include "utils/str_cat.hpp"

namespace devilution::fp {

namespace {

struct DfFrame {
	int x = 0;
	int y = 0;
	int width = 0;
	int height = 0;
	std::vector<uint8_t> pixels; // Daggerfall palette indices, 0 is clear
};

using DfRecord = std::vector<DfFrame>;

struct DfWeapon {
	bool loaded = false;
	std::vector<DfRecord> records;
};

/** The weapon files, by what they hold. */
enum DfWeaponFile : uint8_t {
	DfLongBladeGilt = 0,
	DfStaff = 1,
	DfShortBlade = 2,
	DfLongBlade = 4,
	DfMace = 5,
	DfFlail = 6,
	DfWarhammer = 7,
	DfAxe = 8,
	DfBow = 9,
	DfHands = 10,
	DfWeaponFiles = 12,
};

/** @brief FP_DAGGERFALL_DIR (set but empty: no weapon), else the "daggerfall" folder next to the game. */
const char *DaggerfallDir()
{
	static const std::string dir = [] {
		if (const char *env = std::getenv("FP_DAGGERFALL_DIR"))
			return std::string(env);
		return StrCat(paths::BasePath(), "daggerfall");
	}();
	return dir.empty() ? nullptr : dir.c_str();
}

std::vector<uint8_t> ReadFile(const std::string &path)
{
	std::ifstream file(path, std::ios::binary);
	if (!file)
		return {};
	return { std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
}

int Word(const std::vector<uint8_t> &data, size_t pos)
{
	return pos + 1 < data.size() ? data[pos] | (data[pos + 1] << 8) : 0;
}

bool DecodeRle(const std::vector<uint8_t> &data, size_t pos, DfFrame &frame)
{
	const size_t count = static_cast<size_t>(frame.width) * frame.height;
	frame.pixels.clear();
	frame.pixels.reserve(count);
	while (frame.pixels.size() < count) {
		if (pos >= data.size())
			return false;
		const uint8_t c = data[pos++];
		if (c > 127) {
			if (pos >= data.size())
				return false;
			frame.pixels.insert(frame.pixels.end(), c - 127, data[pos++]);
		} else {
			if (pos + c + 1 > data.size())
				return false;
			frame.pixels.insert(frame.pixels.end(), data.begin() + pos, data.begin() + pos + c + 1);
			pos += c + 1;
		}
	}
	frame.pixels.resize(count);
	return true;
}

std::vector<DfRecord> ReadCif(const std::vector<uint8_t> &data)
{
	std::vector<DfRecord> records;
	size_t pos = 0;
	while (pos + 76 <= data.size()) {
		const int f0 = Word(data, pos);
		const int f1 = Word(data, pos + 2);
		const int f2 = Word(data, pos + 4);
		const int f3 = Word(data, pos + 6);
		const int f4 = Word(data, pos + 8);
		const int size = Word(data, pos + 10);
		if (records.empty() && f4 == 0 && size == f2 * f3 && f2 > 0 && f3 > 0) {
			// The weapon at rest, a plain image.
			DfFrame frame { f0, f1, f2, f3, {} };
			if (pos + 12 + size > data.size())
				break;
			frame.pixels.assign(data.begin() + pos + 12, data.begin() + pos + 12 + size);
			records.push_back({ std::move(frame) });
			pos += 12 + size;
			continue;
		}
		const int width = f0;
		const int height = f1;
		if (width <= 0 || height <= 0 || width > 320 || height > 200)
			break;
		DfRecord record;
		for (int i = 0; i < 31; i++) {
			const int offset = Word(data, pos + 12 + 2 * i);
			if (offset == 0)
				continue;
			DfFrame frame { f3, f4, width, height, {} };
			if (!DecodeRle(data, pos + offset, frame))
				return records;
			record.push_back(std::move(frame));
		}
		const int total = Word(data, pos + 74);
		if (record.empty() || total == 0)
			break;
		records.push_back(std::move(record));
		pos += total;
	}
	return records;
}

const DfWeapon &GetWeapon(int file)
{
	static std::array<DfWeapon, DfWeaponFiles> weapons;
	DfWeapon &weapon = weapons[file];
	if (!weapon.loaded) {
		weapon.loaded = true;
		if (const char *dir = DaggerfallDir(); dir != nullptr) {
			char name[16];
			std::snprintf(name, sizeof(name), "/WEAPON%02d.CIF", file);
			weapon.records = ReadCif(ReadFile(std::string(dir) + name));
		}
	}
	return weapon;
}

/** @brief Daggerfall's palette index -> the nearest of the game's fixed colours (128 .. 255, the same on every level). */
const std::array<uint8_t, 256> &ColorMap()
{
	static std::array<uint8_t, 256> map {};
	static bool ready = false;
	static uint32_t readyFor = 0;
	if (ready && readyFor == StablePaletteVersion())
		return map;
	ready = true;
	readyFor = StablePaletteVersion();
	std::vector<uint8_t> pal;
	if (const char *dir = DaggerfallDir(); dir != nullptr)
		pal = ReadFile(std::string(dir) + "/ART_PAL.COL");
	const size_t base = pal.size() == 776 ? 8 : 0;
	for (int i = 0; i < 256; i++) {
		const size_t at = base + static_cast<size_t>(i) * 3;
		const int r = at + 2 < pal.size() ? pal[at] : i;
		const int g = at + 2 < pal.size() ? pal[at + 1] : i;
		const int b = at + 2 < pal.size() ? pal[at + 2] : i;
		int best = std::numeric_limits<int>::max();
		for (int j = 128; j < 256; j++) {
			const SDL_Color o = logical_palette[j];
			const int d = (o.r - r) * (o.r - r) * 3 + (o.g - g) * (o.g - g) * 6 + (o.b - b) * (o.b - b);
			if (d < best) {
				best = d;
				map[i] = static_cast<uint8_t>(j);
			}
		}
	}
	return map;
}

const Item *HeldWeapon(const Player &player)
{
	for (const inv_body_loc hand : { INVLOC_HAND_LEFT, INVLOC_HAND_RIGHT }) {
		const Item &item = player.InvBody[hand];
		if (!item.isEmpty() && item.isWeapon())
			return &item;
	}
	return nullptr;
}

/** @brief Which Daggerfall weapon looks most like a Diablo one. */
int WeaponFileFor(const Item *item)
{
	if (item == nullptr)
		return DfHands;
	switch (item->_itype) {
	case ItemType::Sword:
		if (item->_iCurs == ICURS_DAGGER || item->_iCurs == ICURS_SHORT_SWORD)
			return DfShortBlade;
		return item->_iMagical == ITEM_QUALITY_UNIQUE ? DfLongBladeGilt : DfLongBlade;
	case ItemType::Axe:
		return DfAxe;
	case ItemType::Mace:
		if (item->_iCurs == ICURS_FLAIL)
			return DfFlail;
		if (item->_iCurs == ICURS_WAR_HAMMER || item->_iCurs == ICURS_MAUL)
			return DfWarhammer;
		return DfMace;
	case ItemType::Staff:
		return DfStaff;
	case ItemType::Bow:
		return DfBow;
	default:
		return DfHands;
	}
}

void DrawFrame(const Surface &out, const DfFrame &frame, float scale, int left, int top, uint8_t light)
{
	const auto &colors = ColorMap();
	const int x0 = std::max(0, left + static_cast<int>(std::floor(frame.x * scale)));
	const int y0 = std::max(0, top + static_cast<int>(std::floor(frame.y * scale)));
	const int x1 = std::min(out.w(), left + static_cast<int>(std::ceil((frame.x + frame.width) * scale)));
	const int y1 = std::min(out.h(), top + static_cast<int>(std::ceil((frame.y + frame.height) * scale)));
	for (int y = y0; y < y1; y++) {
		const int sy = static_cast<int>((y - top) / scale) - frame.y;
		if (sy < 0 || sy >= frame.height)
			continue;
		const uint8_t *row = &frame.pixels[static_cast<size_t>(sy) * frame.width];
		uint8_t *dst = out.at(0, y);
		for (int x = x0; x < x1; x++) {
			const int sx = static_cast<int>((x - left) / scale) - frame.x;
			if (sx < 0 || sx >= frame.width || row[sx] == 0)
				continue;
			dst[x] = FpLightTable(light)[colors[row[sx]]];
		}
	}
}

struct Preview {
	int file = -1;
	int record = 0;
	int frame = 0;
} WeaponPreview;

} // namespace

void PreviewFirstPersonWeapon(int file, int record, int frame)
{
	WeaponPreview = { file, record, frame };
}

void DrawFirstPersonWeapon(const Surface &out)
{
	if (DaggerfallDir() == nullptr || MyPlayer == nullptr)
		return;
	const Player &player = *MyPlayer;
	const bool preview = WeaponPreview.file >= 0 && WeaponPreview.file < DfWeaponFiles;
	// No one fights in Tristram (the game lets no blow be struck there): the weapon is put away, but for a preview.
	if (leveltype == DTYPE_TOWN && !preview)
		return;
	const DfWeapon &weapon = GetWeapon(preview ? WeaponPreview.file : WeaponFileFor(HeldWeapon(player)));
	if (weapon.records.empty())
		return;

	// A new swing each attack, going round the strike directions (the thrust last, less often).
	static constexpr std::array<int, 6> Swings { 1, 4, 2, 3, 5, 6 };
	static size_t swing = 0;
	static PLR_MODE lastMode = PM_STAND;
	static int lastFrame = 0;
	const bool melee = player._pmode == PM_ATTACK;
	const bool ranged = player._pmode == PM_RATTACK;
	if ((melee || ranged) && (player._pmode != lastMode || player.AnimInfo.currentFrame < lastFrame))
		swing = (swing + 1) % Swings.size();
	lastMode = player._pmode;
	lastFrame = player.AnimInfo.currentFrame;

	const DfFrame *frame = &weapon.records[0][0];
	if (melee || ranged) {
		const int frames = std::max<int>(player.AnimInfo.numberOfFrames, 1);
		const float progress = std::clamp(static_cast<float>(player.AnimInfo.currentFrame) / static_cast<float>(frames), 0.0F, 1.0F);
		const DfRecord &record = weapon.records.size() == 1 ? weapon.records[0]
		                                                     : weapon.records[std::min<size_t>(Swings[swing], weapon.records.size() - 1)];
		const size_t index = std::min(record.size() - 1, static_cast<size_t>(progress * static_cast<float>(record.size())));
		frame = &record[index];
	}
	if (preview) {
		const DfRecord &record = weapon.records[std::min<size_t>(std::max(WeaponPreview.record, 0), weapon.records.size() - 1)];
		frame = &record[std::min<size_t>(std::max(WeaponPreview.frame, 0), record.size() - 1)];
	}

	// Bob with the steps: the art sways and dips as the camera moves.
	static float bob = 0;
	static float lastX = 0;
	static float lastY = 0;
	const ViewState &view = GetViewState();
	const float moved = std::hypot(view.x - lastX, view.y - lastY);
	lastX = view.x;
	lastY = view.y;
	if (moved < 1.0F)
		bob += moved * 3.14159265F;
	const float bobX = melee || ranged ? 0.0F : std::sin(bob) * 4.0F;
	const float bobY = melee || ranged ? 0.0F : std::abs(std::sin(bob)) * 4.0F;

	// The art is a 320 x 200 screen: scale it to the view's height. Weapons run off its right and bottom edges
	// (the hand holding a long blade is out of sight), so those edges go on the view's.
	const float scale = static_cast<float>(out.h()) / 200.0F;
	const int left = out.w() - static_cast<int>(320.0F * scale) + static_cast<int>(bobX * scale);
	const int top = static_cast<int>(bobY * scale);
	const Point tile { static_cast<int>(std::floor(view.x)), static_cast<int>(std::floor(view.y)) };
	DrawFrame(out, *frame, scale, left, top, std::min<uint8_t>(LightAt(tile), LightsMax));
}

} // namespace devilution::fp
