// Host fonts behind ScePvf's "system" fonts (the Vita's own are firmware files, not in a game dump).
#pragma once
#include <string>
#include <vector>

namespace hle {

// Latin face first, then fallbacks (Japanese); a glyph comes from the first face that has it.
std::vector<std::string> default_system_fonts();
void pvf_set_system_fonts(const std::vector<std::string>& paths);
std::vector<std::string> pvf_system_fonts();  // the current list (--font or the defaults); host UI text uses it too

}  // namespace hle
