/**
 * @file fp/fp_autotest.cpp
 *
 * Debug-only scripted run of the first-person view for visual checks that need no window focus or input.
 *
 * Set FP_TEST_STEPS to a ';'-separated list and start the game (SDL_VIDEODRIVER=offscreen works). The menu is
 * clicked through by a helper thread (FP_TEST_MENU_KEYS = number of Return presses). One step runs per frame, after
 * the game has been running for a moment:
 *   wait:N            do nothing for N frames
 *   lua:CODE          run a Lua console line (CODE may contain ':' but not ';')
 *   shot:PATH         render the first-person view into a PPM file
 *   iso:PATH          save the previous frame as drawn by the original view (switch it on a few frames earlier)
 *   bench:N           log the average frame time over the next N frames
 *   click:X,Y         move the mouse to a screen position and click the left button there
 *   key:CODE          press and release a key (its SDL keycode, such as 45 for '-')
 *   quit              leave the game
 */
#ifdef _DEBUG
#include "fp/fp_autotest.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include <SDL.h>

#include "diablo.h"
#include "engine/dx.h"
#include "engine/palette.h"
#include "engine/surface.hpp"
#include "fp/fp_view.hpp"
#include "lua/repl.hpp"
#include "utils/log.hpp"
#include "utils/ui_fwd.h"

namespace devilution::fp {

namespace {

constexpr int WarmupFrames = 45;

std::vector<std::string> Steps;
size_t NextStep = 0;
int Frame = 0;
int WaitFrames = 0;
int BenchFrames = 0;
int BenchTotal = 0;
Uint32 BenchStart = 0;

void PushKey(SDL_Keycode key)
{
	for (const SDL_EventType type : { SDL_KEYDOWN, SDL_KEYUP }) {
		SDL_Event event {};
		event.type = type;
		event.key.keysym.sym = key;
		event.key.keysym.scancode = SDL_GetScancodeFromKey(key);
		event.key.state = type == SDL_KEYDOWN ? SDL_PRESSED : SDL_RELEASED;
		SDL_PushEvent(&event);
	}
}

void PushReturn()
{
	for (const SDL_EventType type : { SDL_KEYDOWN, SDL_KEYUP }) {
		SDL_Event event {};
		event.type = type;
		event.key.keysym.sym = SDLK_RETURN;
		event.key.keysym.scancode = SDL_SCANCODE_RETURN;
		event.key.state = type == SDL_KEYDOWN ? SDL_PRESSED : SDL_RELEASED;
		SDL_PushEvent(&event);
		std::this_thread::sleep_for(std::chrono::milliseconds(60));
	}
}

void PushClick(int x, int y)
{
	SDL_Event motion {};
	motion.type = SDL_MOUSEMOTION;
	motion.motion.x = x;
	motion.motion.y = y;
	SDL_PushEvent(&motion);
	for (const SDL_EventType type : { SDL_MOUSEBUTTONDOWN, SDL_MOUSEBUTTONUP }) {
		SDL_Event event {};
		event.type = type;
		event.button.button = SDL_BUTTON_LEFT;
		event.button.state = type == SDL_MOUSEBUTTONDOWN ? SDL_PRESSED : SDL_RELEASED;
		event.button.x = x;
		event.button.y = y;
		SDL_PushEvent(&event);
	}
}

void MenuKeyThread(int presses)
{
	std::this_thread::sleep_for(std::chrono::seconds(4));
	for (int i = 0; i < presses; i++) {
		PushReturn();
		std::this_thread::sleep_for(std::chrono::milliseconds(2500));
	}
}

void WriteShot(const std::string &path)
{
	OwnedSurface surface(gnScreenWidth, gnViewportHeight);
	DrawFirstPersonView(surface);
	if (FILE *f = std::fopen(path.c_str(), "wb")) {
		std::fprintf(f, "P6\n%d %d\n255\n", surface.w(), surface.h());
		for (int y = 0; y < surface.h(); y++) {
			for (int x = 0; x < surface.w(); x++) {
				const SDL_Color c = logical_palette[*surface.at(x, y)];
				const uint8_t rgb[3] = { c.r, c.g, c.b };
				std::fwrite(rgb, 1, 3, f);
			}
		}
		std::fclose(f);
	}
}

/** @brief Writes the screen buffer of the previous frame (the original view when first person is off), without the panel. */
void WriteIsoShot(const std::string &path)
{
	const Surface screen = GlobalBackBuffer();
	const int height = gnViewportHeight;
	if (FILE *f = std::fopen(path.c_str(), "wb")) {
		std::fprintf(f, "P6\n%d %d\n255\n", screen.w(), height);
		for (int y = 0; y < height; y++) {
			for (int x = 0; x < screen.w(); x++) {
				const SDL_Color c = system_palette[*screen.at(x, y)];
				const uint8_t rgb[3] = { c.r, c.g, c.b };
				std::fwrite(rgb, 1, 3, f);
			}
		}
		std::fclose(f);
	}
}

struct Starter {
	Starter()
	{
		const char *steps = std::getenv("FP_TEST_STEPS");
		if (steps == nullptr)
			return;
		std::string rest = steps;
		size_t pos = 0;
		while (pos <= rest.size()) {
			const size_t end = rest.find(';', pos);
			Steps.push_back(rest.substr(pos, end == std::string::npos ? std::string::npos : end - pos));
			if (end == std::string::npos)
				break;
			pos = end + 1;
		}
		const char *keys = std::getenv("FP_TEST_MENU_KEYS");
		std::thread(MenuKeyThread, keys != nullptr ? std::atoi(keys) : 3).detach();
	}
};
const Starter StarterInstance;

} // namespace

void AutotestTick()
{
	if (NextStep < Steps.size() || BenchFrames > 0)
		PauseMode = 0; // the offscreen window never has focus, which would pause the game (once the steps are done, a
		               // player at a window may pause again)
	if (BenchFrames > 0) {
		if (--BenchFrames == 0)
			LogInfo("autotest bench: {:.1f} ms per frame over {} frames", static_cast<double>(SDL_GetTicks() - BenchStart) / BenchTotal, BenchTotal);
		return;
	}
	if (NextStep >= Steps.size())
		return;
	if (++Frame < WarmupFrames)
		return;
	if (WaitFrames > 0) {
		WaitFrames--;
		return;
	}
	const std::string step = Steps[NextStep++];
	if (step.rfind("wait:", 0) == 0) {
		WaitFrames = std::atoi(step.c_str() + 5);
	} else if (step.rfind("lua:", 0) == 0) {
		const auto result = RunLuaReplLine(step.substr(4));
		LogInfo("autotest lua: {}", result.has_value() ? *result : "error: " + result.error());
	} else if (step.rfind("click:", 0) == 0) {
		const size_t comma = step.find(',');
		PushClick(std::atoi(step.c_str() + 6), comma != std::string::npos ? std::atoi(step.c_str() + comma + 1) : 0);
	} else if (step.rfind("key:", 0) == 0) {
		PushKey(static_cast<SDL_Keycode>(std::atoi(step.c_str() + 4)));
	} else if (step.rfind("bench:", 0) == 0) {
		BenchTotal = BenchFrames = std::atoi(step.c_str() + 6);
		BenchStart = SDL_GetTicks();
	} else if (step.rfind("iso:", 0) == 0) {
		WriteIsoShot(step.substr(4));
	} else if (step.rfind("shot:", 0) == 0) {
		WriteShot(step.substr(5));
	} else if (step == "quit") {
		SDL_Event event {};
		event.type = SDL_QUIT;
		SDL_PushEvent(&event);
	}
}

} // namespace devilution::fp
#endif // _DEBUG
