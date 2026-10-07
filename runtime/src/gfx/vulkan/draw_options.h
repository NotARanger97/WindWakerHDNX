#pragma once
#include <cstdlib>
#include <cstring>

namespace gfxvk {
// Call once per option. Desktop opt-in behaviour and explicit overrides stay
// unchanged; Switch defaults to the reviewed draw preparation shortcuts.
inline bool draw_option_enabled(const char* name) {
    const char* value = std::getenv(name);
#ifdef __SWITCH__
    if (!value) return true;
#endif
    return value && std::strcmp(value, "1") == 0;
}
} // namespace gfxvk
