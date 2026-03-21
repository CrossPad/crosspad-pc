#if USE_LVGL

#include "crosspad/app/AppRegistrar.hpp"
#include "crosspad-gui/platform/IGuiPlatform.h"
#include "lvgl.h"

#include <cstdio>
#include <string>
#include <vector>
#include <fstream>
#include <filesystem>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#undef ERROR
#endif

/* ── Markdown file locator ───────────────────────────────────────────── */

static std::string findInstructionsFile()
{
    namespace fs = std::filesystem;

    // Try paths relative to exe location (bin/../docs/instructions.md)
    const char* candidates[] = {
        "docs/instructions.md",
        "../docs/instructions.md",
    };

    // From current working directory
    for (auto& c : candidates) {
        std::error_code ec;
        if (fs::exists(c, ec)) return fs::canonical(c, ec).string();
    }

    // From exe directory
#ifdef _WIN32
    char exePath[MAX_PATH] = {};
    GetModuleFileNameA(NULL, exePath, MAX_PATH);
    fs::path exeDir = fs::path(exePath).parent_path();
    {
        auto p = exeDir / ".." / "docs" / "instructions.md";
        std::error_code ec;
        if (fs::exists(p, ec)) return fs::canonical(p, ec).string();
    }
#endif

    return {};
}

/* ── Simple markdown line parser ─────────────────────────────────────── */

enum class LineType { H1, H2, BULLET, TABLE_ROW, BLANK, TEXT };

struct ParsedLine {
    LineType type;
    std::string text;
};

static std::vector<ParsedLine> parseMarkdown(const std::string& path)
{
    std::vector<ParsedLine> lines;
    std::ifstream file(path);
    if (!file.is_open()) {
        lines.push_back({LineType::TEXT, "(Could not open " + path + ")"});
        return lines;
    }

    std::string line;
    while (std::getline(file, line)) {
        // Strip trailing \r
        if (!line.empty() && line.back() == '\r')
            line.pop_back();

        if (line.empty()) {
            lines.push_back({LineType::BLANK, ""});
        } else if (line.rfind("## ", 0) == 0) {
            lines.push_back({LineType::H2, line.substr(3)});
        } else if (line.rfind("# ", 0) == 0) {
            lines.push_back({LineType::H1, line.substr(2)});
        } else if (line.rfind("- ", 0) == 0) {
            lines.push_back({LineType::BULLET, line.substr(2)});
        } else if (line.front() == '|') {
            // Skip separator rows like |---|---|
            bool isSeparator = true;
            for (char c : line) {
                if (c != '|' && c != '-' && c != ' ' && c != ':') {
                    isSeparator = false;
                    break;
                }
            }
            if (!isSeparator) {
                // Extract cells, join with " \xE2\x80\x94 " (em dash)
                std::vector<std::string> cells;
                size_t pos = 1; // skip leading |
                while (pos < line.size()) {
                    size_t next = line.find('|', pos);
                    if (next == std::string::npos) break;
                    std::string cell = line.substr(pos, next - pos);
                    size_t start = cell.find_first_not_of(' ');
                    size_t end = cell.find_last_not_of(' ');
                    if (start != std::string::npos)
                        cell = cell.substr(start, end - start + 1);
                    else
                        cell = "";
                    cells.push_back(cell);
                    pos = next + 1;
                }
                // For 2-column tables (key — action), use arrow separator
                // For grid tables (single-char cells), use spaces
                std::string formatted;
                bool isGrid = (cells.size() >= 4 && cells[0].size() <= 2);
                for (size_t i = 0; i < cells.size(); i++) {
                    if (i > 0) {
                        if (isGrid)
                            formatted += "   ";
                        else
                            formatted += " - ";
                    }
                    formatted += cells[i];
                }
                lines.push_back({LineType::TABLE_ROW, formatted});
            }
        } else {
            lines.push_back({LineType::TEXT, line});
        }
    }
    return lines;
}

/* ── LVGL rendering ──────────────────────────────────────────────────── */

static const lv_color_t COLOR_GREEN  = lv_color_hex(0x00E676);
static const lv_color_t COLOR_GREEN2 = lv_color_hex(0x66BB6A);
static const lv_color_t COLOR_BULLET = lv_color_hex(0xAAAAAA);
static const lv_color_t COLOR_TABLE  = lv_color_hex(0xCCCCCC);
static const lv_color_t COLOR_TEXT   = lv_color_hex(0xDDDDDD);

static void renderLine(lv_obj_t* parent, const ParsedLine& line)
{
    if (line.type == LineType::BLANK) {
        // Small spacer
        lv_obj_t* spacer = lv_obj_create(parent);
        lv_obj_set_size(spacer, LV_PCT(100), 4);
        lv_obj_set_style_bg_opa(spacer, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(spacer, 0, 0);
        lv_obj_set_style_pad_all(spacer, 0, 0);
        lv_obj_remove_flag(spacer, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
        return;
    }

    lv_obj_t* lbl = lv_label_create(parent);
    lv_obj_set_width(lbl, LV_PCT(100));
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);

    switch (line.type) {
        case LineType::H1:
            lv_label_set_text(lbl, line.text.c_str());
            lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
            lv_obj_set_style_text_color(lbl, COLOR_GREEN, 0);
            break;
        case LineType::H2:
            lv_label_set_text(lbl, line.text.c_str());
            lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
            lv_obj_set_style_text_color(lbl, COLOR_GREEN2, 0);
            break;
        case LineType::BULLET: {
            std::string bullet = std::string("\xE2\x80\xA2 ") + line.text;  // UTF-8 bullet •
            lv_label_set_text(lbl, bullet.c_str());
            lv_obj_set_style_text_font(lbl, &lv_font_montserrat_10, 0);
            lv_obj_set_style_text_color(lbl, COLOR_BULLET, 0);
            break;
        }
        case LineType::TABLE_ROW:
            lv_label_set_text(lbl, line.text.c_str());
            lv_obj_set_style_text_font(lbl, &lv_font_montserrat_10, 0);
            lv_obj_set_style_text_color(lbl, COLOR_TABLE, 0);
            break;
        case LineType::TEXT:
            lv_label_set_text(lbl, line.text.c_str());
            lv_obj_set_style_text_font(lbl, &lv_font_montserrat_10, 0);
            lv_obj_set_style_text_color(lbl, COLOR_TEXT, 0);
            break;
        default:
            break;
    }
}

/* ── App create / destroy ────────────────────────────────────────────── */

static lv_obj_t* lv_CreateInstructions(lv_obj_t* parent, App* a)
{
    (void)a;

    // Root container
    lv_obj_t* container = lv_obj_create(parent);
    lv_obj_set_size(container, lv_obj_get_content_width(parent),
                    lv_obj_get_content_height(parent));
    lv_obj_set_style_bg_color(container, lv_color_black(), 0);
    lv_obj_set_style_border_width(container, 0, 0);
    lv_obj_set_style_pad_all(container, 0, 0);
    lv_obj_set_style_radius(container, 0, 0);
    lv_obj_center(container);

    // Scrollable content
    lv_obj_t* content = lv_obj_create(container);
    lv_obj_set_size(content, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(content, lv_color_black(), 0);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_style_pad_all(content, 8, 0);
    lv_obj_set_style_pad_row(content, 2, 0);
    lv_obj_set_style_radius(content, 0, 0);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_AUTO);

    // Load and render markdown
    std::string path = findInstructionsFile();
    if (path.empty()) {
        lv_obj_t* lbl = lv_label_create(content);
        lv_label_set_text(lbl, "instructions.md not found");
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xFF4444), 0);
    } else {
        auto lines = parseMarkdown(path);
        for (auto& line : lines) {
            renderLine(content, line);
        }
        printf("[Instructions] Loaded %zu lines from %s\n", lines.size(), path.c_str());
    }

    return container;
}

static void lv_DestroyInstructions(lv_obj_t* obj)
{
    (void)obj;
}

void _register_Instructions_app() {
    static char icon_path[256];
    snprintf(icon_path, sizeof(icon_path), "%sinfo.png",
             crosspad_gui::getGuiPlatform().assetPathPrefix());
    static const crosspad::AppEntry entry = {
        "Help", icon_path,
        lv_CreateInstructions, lv_DestroyInstructions,
        nullptr, nullptr, nullptr, nullptr, 0
    };
    crosspad::AppRegistry::getInstance().registerApp(entry);
}

#endif // USE_LVGL
