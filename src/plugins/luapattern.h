#pragma once
// Bounded replacements for string.find/match/gmatch/gsub (internal to hn_plugins).
// The stock matcher is a single C call, so the instruction hook cannot interrupt it and a pattern such as
// '(.-)(.-)(.-)(.-)(.-)(.-)c' on a long string can stall the UI thread. These versions run the same algorithm
// (a port of Lua's lstrlib matcher, ASCII classes, no locale) with a step budget and a wall-clock check.
#include <cstddef>
struct lua_State;

namespace hn::plugins {

inline constexpr size_t kPatSubjectMax = 1024 * 1024;  // longest subject accepted
inline constexpr size_t kPatPatternMax = 256;          // longest pattern accepted
inline constexpr int kPatMaxQuantifiers = 24;          // quantified items (* + - ?) allowed in one pattern
inline constexpr long kPatSteps = 4'000'000;           // matcher steps per library call (a gsub counts all its matches)

// expired(L) is polled every 1024 steps; it returns true when the callback's wall-clock budget is gone
// (and must arm the abort). Replaces find/match/gmatch/gsub in the global `string` table.
using PatternExpired = bool (*)(lua_State *);
void installSafePatterns(lua_State *L, PatternExpired expired);

}  // namespace hn::plugins
