/**
 * @file fp/fp_autotest.hpp
 *
 * Debug-only scripted first-person run (see fp_autotest.cpp).
 */
#pragma once

namespace devilution::fp {

#ifdef _DEBUG
/** @brief Runs the next scripted step, if FP_TEST_STEPS is set. Call once per game frame. */
void AutotestTick();
#else
inline void AutotestTick() { }
#endif

} // namespace devilution::fp
