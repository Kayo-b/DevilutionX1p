/**
 * @file control/control_hud.hpp
 *
 * The HUD Size option: the main panel with its flasks, belt and buttons drawn at another size than its art.
 *
 * The panel keeps its 640 x 128 layout, so every position in it (buttons, belt slots, flasks) stays as it is: it is
 * drawn at its size on a canvas of its own and scaled onto the screen from there. While it is scaled, its layout is
 * moved below the bottom of the screen (GetMainPanel()), so a position never means both a spot in the view and a spot
 * on the panel: mouse positions over the panel on the screen are mapped onto the layout (ScreenToHud), and the
 * cursor drawn back where it is on the screen (HudToScreen). What stands over the panel (the speedbook, messages,
 * the level-up button) goes by where the panel shows instead, GetMainPanelOnScreen().
 */
#pragma once

#include "engine/point.hpp"
#include "engine/rectangle.hpp"
#include "engine/size.hpp"
#include "engine/surface.hpp"

namespace devilution {

/**
 * @brief Lays out the main panel for the HUD Size option and this screen (called by CalculatePanelAreas).
 * @return the rectangle the panel is laid out in, GetMainPanel()
 */
Rectangle LayOutMainPanel(Size panelSize);

/** @brief Whether the main panel is drawn at another size than its art. */
[[nodiscard]] bool IsHudScaled();

/** @brief Where the main panel shows on the screen: GetMainPanel() unless the HUD Size option scales it. */
[[nodiscard]] const Rectangle &GetMainPanelOnScreen();

/** @brief The layout position under a screen position over the main panel; any other position is returned as it is. */
[[nodiscard]] Point ScreenToHud(Point position);

/** @brief Where a position in the main panel's layout (or just over it) shows on the screen; any other is returned as it is. */
[[nodiscard]] Point HudToScreen(Point position);

/**
 * @brief Starts this frame's HUD: the panel, its flasks, belt, buttons and info box are drawn on the returned surface.
 * @param out the screen, with everything under the HUD drawn on it
 */
Surface BeginHudDrawing(const Surface &out);

/** @brief Puts the HUD drawn since BeginHudDrawing on the screen, at its size. */
void EndHudDrawing(const Surface &out);

} // namespace devilution
