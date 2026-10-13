/**
 * @file itemlabels.h
 *
 * Adds item labels QoL feature
 */
#pragma once

#include "engine/point.hpp"
#include "engine/surface.hpp"

namespace devilution {

void ToggleItemLabelHighlight();
void HighlightKeyPressed(bool pressed);
bool IsItemLabelHighlighted();
void ResetItemlabelHighlighted();
bool IsHighlightingLabelsEnabled();
void AddItemToLabelQueue(int id, Point position);
/** @brief Queues an item's label with its bottom middle at a screen position (the first-person view, which projects its items itself). */
void AddItemToLabelQueueAt(int id, Point bottomMiddle);
void DrawItemNameLabels(const Surface &out);

} // namespace devilution
