// Controller setup: input tokens, mapping lines and gamepad_mappings.txt edits (tools/common/pad_mapping.h).
#include <cstdio>
#include <string>

#include "../tools/common/pad_mapping.h"

using namespace dcp::padmap;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

int main() {
    CHECK(button_axis_token(5, -32768, 32767) == "a5");    // trigger resting at the bottom
    CHECK(button_axis_token(5, 32767, -32768) == "a5~");   // trigger resting at the top
    CHECK(button_axis_token(2, 0, 30000) == "+a2");
    CHECK(button_axis_token(2, 0, -30000) == "-a2");
    CHECK(stick_axis_token(1, 30000) == "a1");
    CHECK(stick_axis_token(1, -30000) == "a1~");
    CHECK(hat_token(0, 4) == "h0.4");

    const std::string line = build_line("guid1", "Pad, Wired", "Windows", {{"a", "b1"}, {"leftx", "a0"}});
    CHECK(line == "guid1,Pad  Wired,a:b1,leftx:a0,platform:Windows,");
    CHECK(build_line("g", "", "Linux", {}) == "g,Controller,platform:Linux,");

    std::string file = merge_line("", "guid1", line);
    CHECK(file == line + "\n");
    CHECK(has_line(file, "guid1"));
    CHECK(!has_line(file, "guid"));
    file = merge_line(file, "guid2", "guid2,Other,platform:Windows,");
    CHECK(has_line(file, "guid2"));
    file = merge_line(file, "guid1", "guid1,New,platform:Windows,");  // replaced, not doubled
    CHECK(file == "guid2,Other,platform:Windows,\nguid1,New,platform:Windows,\n");
    file = merge_line(file, "guid2", "");
    CHECK(file == "guid1,New,platform:Windows,\n");
    CHECK(merge_line("a,1\r\n\r\nb,2", "b", "") == "a,1\n");  // CRLF and blank lines from a hand edit

    size_t buttons = 0;
    for (const Step& s : kSteps) buttons += s.kind == Kind::Button;
    CHECK(kStepCount == 20 && buttons == 10);

    std::printf(g_fail ? "pad_mapping: %d failed\n" : "pad_mapping: ok\n", g_fail);
    return g_fail != 0;
}
