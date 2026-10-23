/**
 * @file control/control_hud.cpp
 *
 * The HUD Size option: the main panel with its flasks, belt and buttons drawn at another size than its art.
 */
#include "control/control_hud.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

#include "control/control.hpp"
#include "engine/displacement.hpp"
#include "engine/palette.h"
#include "options.h"
#include "utils/ui_fwd.h"

namespace devilution {

namespace {

/** Rows over the laid-out panel that are part of the HUD: the tops of the flasks stand 13 rows above it. */
constexpr int MarginRows = 16;

/** The HUD Size this screen has room for, as a scale of the panel's art; 1 when it is drawn as it is. */
float Scale = 1.0F;
/** Where the main panel shows on the screen. */
Rectangle OnScreen;

/** The HUD is drawn here at its art size, at its layout positions (GetMainPanel()), then scaled onto the screen. */
std::optional<OwnedSurface> Canvas;
/** The margin rows of the canvas before the HUD was drawn: a pixel left as it was is not part of the HUD. */
std::vector<uint8_t> MarginBackground;

int FloorDiv(int a, int b)
{
	const int quotient = a / b;
	return (a % b != 0 && (a < 0) != (b < 0)) ? quotient - 1 : quotient;
}

/**
 * @brief Palette colours nearest to RGB colours (5 bits a channel), looked up as they are asked for and forgotten
 * when the palette changes. Colours 1-31 are left out: the caves and hell cycle them.
 */
class NearestColors {
public:
	void Prepare()
	{
		const auto sameColor = [](const SDL_Color &a, const SDL_Color &b) { return a.r == b.r && a.g == b.g && a.b == b.b; };
		if (ready_ && std::equal(logical_palette.begin() + 32, logical_palette.end(), palette_.begin() + 32, sameColor))
			return;
		ready_ = true;
		palette_ = logical_palette;
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
				const SDL_Color o = palette_[j];
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
	std::array<SDL_Color, 256> palette_ {};
	std::array<uint16_t, 32768> cache_ {};
	bool ready_ = false;
};

NearestColors NearestColor;

float ScaleForScreen(Size panelSize)
{
	// On a screen no wider than the panel it has rows of its own below the view: it stays as it is.
	if (gnScreenWidth <= panelSize.width)
		return 1.0F;
	const float wanted = static_cast<float>(*GetOptions().Graphics.hudSize) / 100.0F;
	// No wider than the screen, and leaving the side panels (inventory, character) their height above it.
	const float roomAcross = static_cast<float>(gnScreenWidth) / static_cast<float>(panelSize.width);
	const float roomUp = static_cast<float>(gnScreenHeight - SidePanelSize.height) / static_cast<float>(panelSize.height);
	return std::clamp(wanted, 0.25F, std::max(1.0F, std::min(roomAcross, roomUp)));
}

const auto OptionChangeHandlerHudSize = (GetOptions().Graphics.hudSize.SetValueChangedCallback(CalculatePanelAreas), true);

} // namespace

Rectangle LayOutMainPanel(Size panelSize)
{
	Scale = ScaleForScreen(panelSize);
	const Size size {
		static_cast<int>(std::lround(static_cast<float>(panelSize.width) * Scale)),
		static_cast<int>(std::lround(static_cast<float>(panelSize.height) * Scale)),
	};
	OnScreen = { { (gnScreenWidth - size.width) / 2, gnScreenHeight - size.height }, size };
	if (size == panelSize) {
		Scale = 1.0F;
		return OnScreen;
	}
	// Below the screen, with the margin rows between.
	return { { (gnScreenWidth - panelSize.width) / 2, gnScreenHeight + MarginRows }, panelSize };
}

bool IsHudScaled()
{
	return Scale != 1.0F;
}

const Rectangle &GetMainPanelOnScreen()
{
	return OnScreen;
}

Point ScreenToHud(Point position)
{
	if (!IsHudScaled() || !OnScreen.contains(position))
		return position;
	const Rectangle &panel = GetMainPanel();
	return panel.position + Displacement {
		(position.x - OnScreen.position.x) * panel.size.width / OnScreen.size.width,
		(position.y - OnScreen.position.y) * panel.size.height / OnScreen.size.height,
	};
}

Point HudToScreen(Point position)
{
	if (!IsHudScaled())
		return position;
	const Rectangle &panel = GetMainPanel();
	const int x = position.x - panel.position.x;
	const int y = position.y - panel.position.y;
	if (x < 0 || x >= panel.size.width || y < -MarginRows || y >= panel.size.height)
		return position;
	// The middle of the pixel, which ScreenToHud maps back to it.
	return OnScreen.position + Displacement {
		FloorDiv((2 * x + 1) * OnScreen.size.width, 2 * panel.size.width),
		FloorDiv((2 * y + 1) * OnScreen.size.height, 2 * panel.size.height),
	};
}

Surface BeginHudDrawing(const Surface &out)
{
	if (!IsHudScaled())
		return out;
	const Rectangle &panel = GetMainPanel();
	const Size canvasSize { gnScreenWidth, panel.position.y + panel.size.height };
	if (!Canvas || Canvas->w() != canvasSize.width || Canvas->h() != canvasSize.height)
		Canvas.emplace(canvasSize);

	// The margin rows start as the screen beneath them: the flasks' tops blend over it, and what they leave as it was
	// is not drawn. The panel itself is drawn whole (DrawMainPanel).
	MarginBackground.resize(static_cast<size_t>(panel.size.width) * MarginRows);
	for (int y = 0; y < MarginRows; y++) {
		const int layoutY = panel.position.y - MarginRows + y;
		uint8_t *row = Canvas->at(panel.position.x, layoutY);
		uint8_t *background = &MarginBackground[static_cast<size_t>(y) * panel.size.width];
		for (int x = 0; x < panel.size.width; x++) {
			const Point screen = HudToScreen({ panel.position.x + x, layoutY });
			row[x] = background[x] = out.InBounds(screen) ? out[screen] : 0;
		}
	}
	return *Canvas;
}

void EndHudDrawing(const Surface &out)
{
	if (!IsHudScaled())
		return;
	const Rectangle &panel = GetMainPanel();
	const int panelWidth = panel.size.width;
	const int panelHeight = panel.size.height;
	const int width = OnScreen.size.width;
	const int height = OnScreen.size.height;
	const bool growing = Scale > 1.0F;
	if (!growing)
		NearestColor.Prepare();

	auto pixel = [&](int x, int y) { return *Canvas->at(panel.position.x + x, panel.position.y + y); };
	// The panel is opaque; over it, only what the HUD drew.
	auto isHud = [&](int x, int y, uint8_t color) {
		return y >= 0 || color != MarginBackground[static_cast<size_t>(y + MarginRows) * panelWidth + x];
	};

	const int firstRow = std::max(OnScreen.position.y - (MarginRows * height + panelHeight - 1) / panelHeight, 0);
	const int endRow = std::min(OnScreen.position.y + height, out.h());
	const int firstColumn = std::max(OnScreen.position.x, 0);
	const int endColumn = std::min(OnScreen.position.x + width, out.w());
	for (int sy = firstRow; sy < endRow; sy++) {
		// The panel's rows under this screen row.
		const int y0 = std::max(FloorDiv((sy - OnScreen.position.y) * panelHeight, height), -MarginRows);
		const int y1 = std::min(std::max(y0 + 1, FloorDiv((sy + 1 - OnScreen.position.y) * panelHeight, height)), panelHeight);
		if (y0 >= y1)
			continue;
		uint8_t *dst = out.at(0, sy);
		for (int sx = firstColumn; sx < endColumn; sx++) {
			const int x0 = (sx - OnScreen.position.x) * panelWidth / width;
			if (growing) {
				const uint8_t color = pixel(x0, y0);
				if (isHud(x0, y0, color))
					dst[sx] = color;
				continue;
			}
			// Shrinking: the average of the panel's pixels under this one, if the HUD covers half of them.
			const int x1 = std::min(std::max(x0 + 1, (sx + 1 - OnScreen.position.x) * panelWidth / width), panelWidth);
			int covered = 0;
			int total = 0;
			int r = 0;
			int g = 0;
			int b = 0;
			int first = -1;
			bool uniform = true;
			for (int y = y0; y < y1; y++) {
				for (int x = x0; x < x1; x++) {
					total++;
					const uint8_t color = pixel(x, y);
					if (!isHud(x, y, color))
						continue;
					covered++;
					if (first < 0)
						first = color;
					else if (color != first)
						uniform = false;
					const SDL_Color &rgb = logical_palette[color];
					r += rgb.r;
					g += rgb.g;
					b += rgb.b;
				}
			}
			if (covered == 0 || covered * 2 < total)
				continue;
			dst[sx] = uniform ? static_cast<uint8_t>(first) : NearestColor(r / covered, g / covered, b / covered);
		}
	}
}

} // namespace devilution
