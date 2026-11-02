#include "app_gui.h"

#include "video_view.h"

#include <algorithm>
#include <cfloat>
#include <string>
#include <utility>
#include <vector>

namespace app_gui {

namespace {

constexpr float kTitleBar = 36.0f;
constexpr float kSidebar = 300.0f;
constexpr float kBorder = 2.0f;
constexpr float kPad = 14.0f;
constexpr float kLabelWidth = 100.0f;
constexpr float kCaptionButtonsWidth = 122.0f; // minimize, maximize and close, at the title bar's right

const ImVec4 kAccentText(0.76f, 0.48f, 1.00f, 1.0f);
const ImVec4 kMutedText(0.70f, 0.62f, 0.82f, 1.0f);
const ImVec4 kErrorText(1.0f, 0.45f, 0.45f, 1.0f);

ImU32 u32(int r, int g, int b) {
    return IM_COL32(r, g, b, 255);
}

// The display's scale, for sizes given at 100%.
float g_scale = 1.0f;
float px(float size) {
    return size * g_scale;
}

// Header with the section's status at the right edge.
void section_header(const char* text, const char* status, bool on) {
    ImGui::Dummy(ImVec2(0.0f, px(2.0f)));
    ImGui::PushStyleColor(ImGuiCol_Text, kAccentText);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
    const float kDotSpace = px(14.0f);
    ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::CalcTextSize(status).x - kDotSpace);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddCircleFilled(
        ImVec2(p.x + px(4.0f), p.y + ImGui::GetTextLineHeight() * 0.5f), px(4.0f),
        on ? u32(74, 222, 128) : u32(190, 70, 90));
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + kDotSpace);
    ImGui::TextUnformatted(status);
    ImGui::Separator();
}

// Label on the left; the next control fills the rest of the row.
void row_label(const char* text) {
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, kMutedText);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
    ImGui::SameLine(px(kLabelWidth) + ImGui::GetStyle().WindowPadding.x);
    ImGui::SetNextItemWidth(-FLT_MIN);
}

// `text` cut to fit `width`, ending in an ellipsis if anything was removed.
std::string fit_text(std::string text, float width) {
    constexpr const char* kEllipsis = "\xE2\x80\xA6";
    if (ImGui::CalcTextSize(text.c_str()).x <= width) return text;
    while (!text.empty()) {
        // One whole UTF-8 character at a time.
        while (text.size() > 1 && (static_cast<unsigned char>(text.back()) & 0xC0) == 0x80) text.pop_back();
        text.pop_back();
        if (ImGui::CalcTextSize((text + kEllipsis).c_str()).x <= width) break;
    }
    return text + kEllipsis;
}

// Returns true when the user picked a different item. The open list marks the
// device's default, if there is one.
bool combo(const char* id, const std::vector<std::string>& names, int& index, const char* empty_text,
           int default_index = -1) {
    if (names.empty()) {
        ImGui::TextDisabled("%s", empty_text);
        return false;
    }
    const int selected = std::clamp(index, 0, static_cast<int>(names.size()) - 1);
    const std::string& name = names[static_cast<size_t>(selected)];
    const float text_width =
        ImGui::CalcItemWidth() - ImGui::GetFrameHeight() - ImGui::GetStyle().FramePadding.x * 2.0f;
    const std::string preview = fit_text(name, text_width);
    const int previous = index;
    if (ImGui::BeginCombo(id, preview.c_str())) {
        for (int i = 0; i < static_cast<int>(names.size()); ++i) {
            ImGui::PushID(i);
            const std::string& item = names[static_cast<size_t>(i)];
            if (ImGui::Selectable(i == default_index ? (item + "  (device default)").c_str() : item.c_str(), i == selected)) {
                index = i;
            }
            if (i == selected) ImGui::SetItemDefaultFocus();
            ImGui::PopID();
        }
        ImGui::EndCombo();
    } else if (preview != name && ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", name.c_str());
    }
    return index != previous;
}

void error_text(const std::string& text) {
    if (text.empty()) return;
    ImGui::PushStyleColor(ImGuiCol_Text, kErrorText);
    ImGui::TextWrapped("%s", text.c_str());
    ImGui::PopStyleColor();
}

bool refresh_button() {
    const float size = ImGui::GetFrameHeight();
    const bool pressed = ImGui::Button("##refresh", ImVec2(size, size));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Refresh Devices");
    const ImVec2 a = ImGui::GetItemRectMin();
    const ImVec2 c(a.x + size * 0.5f, a.y + size * 0.5f);
    const float r = size * 0.27f;
    auto* dl = ImGui::GetWindowDrawList();
    const ImU32 col = u32(240, 230, 255);
    dl->PathArcTo(c, r, 0.0f, 4.9f, 16);
    dl->PathStroke(col, 0, px(1.6f));
    const ImVec2 tip(c.x + r * 0.19f, c.y - r * 0.98f);
    dl->AddTriangleFilled(ImVec2(tip.x + px(4.5f), tip.y), ImVec2(tip.x - px(1.0f), tip.y - px(4.0f)),
                          ImVec2(tip.x - px(1.0f), tip.y + px(4.0f)), col);
    return pressed;
}

std::string format_text(const char* format, double value) {
    char text[48];
    std::snprintf(text, sizeof(text), format, value);
    return text;
}

std::string count_text(uint64_t value) {
    return std::to_string(value);
}

// Semi-transparent statistics box in the corner of the 4:3 picture. Drawn under
// the ImGui windows and above the video, so it also shows in fullscreen.
void draw_stats_overlay(const Frame& frame) {
    const Stats& s = frame.stats;
    std::vector<std::pair<const char*, std::string>> rows;
    rows.emplace_back("Capture", format_text("%.1f fps", s.fps));
    rows.emplace_back("Gaps", count_text(s.source_gaps));
    rows.emplace_back("Preview Drops", count_text(s.latest_drops));
    rows.emplace_back("Fields Sent", count_text(s.mister_sent));
    rows.emplace_back("Samples Skipped", count_text(s.mister_replaced));
    rows.emplace_back("Send", format_text("%.2f ms", s.mister_ms));
    if (s.decode_ms > 0.0) rows.emplace_back("JPEG Decode", format_text("%.2f ms", s.decode_ms));
    rows.emplace_back("App", format_text("%.2f ms", s.app_ms));
    rows.emplace_back("Preview Skips", count_text(s.present_skips));
    rows.emplace_back("Audio", s.audio_rate_hz ? std::to_string(s.audio_rate_hz) + " Hz" : "Off");
    if (s.allocator_buffers >= 0) rows.emplace_back("Buffers", std::to_string(s.allocator_buffers));
    if (s.mister_errors) rows.emplace_back("Errors", count_text(s.mister_errors));

    float label_w = 0.0f, value_w = 0.0f;
    for (const auto& [label, value] : rows) {
        label_w = std::max(label_w, ImGui::CalcTextSize(label).x);
        value_w = std::max(value_w, ImGui::CalcTextSize(value.c_str()).x);
    }
    const float kInset = px(10.0f), kBoxPad = px(8.0f), kGap = px(16.0f);
    const float line = ImGui::GetTextLineHeight() + px(2.0f);

    const float picture_w = std::min(frame.video_w, frame.video_h * 4.0f / 3.0f);
    const float picture_h = picture_w * 3.0f / 4.0f;
    const ImVec2 box(frame.video_x + (frame.video_w - picture_w) * 0.5f + kInset,
                     frame.video_y + (frame.video_h - picture_h) * 0.5f + kInset);
    const ImVec2 box_end(box.x + kBoxPad * 2.0f + label_w + kGap + value_w,
                         box.y + kBoxPad * 2.0f + line * static_cast<float>(rows.size()));

    auto* dl = ImGui::GetBackgroundDrawList();
    dl->AddRectFilled(box, box_end, IM_COL32(12, 6, 22, 190));
    dl->AddRect(box, box_end, IM_COL32(168, 85, 247, 150));
    float y = box.y + kBoxPad;
    for (const auto& [label, value] : rows) {
        dl->AddText(ImVec2(box.x + kBoxPad, y), u32(178, 158, 209), label);
        dl->AddText(ImVec2(box_end.x - kBoxPad - ImGui::CalcTextSize(value.c_str()).x, y),
                    u32(242, 232, 255), value.c_str());
        y += line;
    }
}

enum class CaptionButton { Minimize, Maximize, Close };

bool caption_button(const char* id, CaptionButton kind) {
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(px(32.0f), px(26.0f)));
    const ImVec2 a = ImGui::GetItemRectMin();
    const ImVec2 b = ImGui::GetItemRectMax();
    auto* dl = ImGui::GetWindowDrawList();
    if (ImGui::IsItemHovered()) {
        dl->AddRectFilled(a, b, kind == CaptionButton::Close ? u32(184, 46, 72) : u32(72, 32, 112));
    }
    const float cx = (a.x + b.x) * 0.5f;
    const float cy = (a.y + b.y) * 0.5f;
    const ImU32 col = u32(240, 230, 255);
    const float w = px(1.5f), l = px(6.0f), h = px(5.0f);
    switch (kind) {
    case CaptionButton::Minimize:
        dl->AddLine(ImVec2(cx - l, cy), ImVec2(cx + l, cy), col, w);
        break;
    case CaptionButton::Maximize:
        dl->AddRect(ImVec2(cx - l, cy - h), ImVec2(cx + l, cy + h), col, 0.0f, 0, w);
        break;
    case CaptionButton::Close:
        dl->AddLine(ImVec2(cx - h, cy - h), ImVec2(cx + h, cy + h), col, w);
        dl->AddLine(ImVec2(cx + h, cy - h), ImVec2(cx - h, cy + h), col, w);
        break;
    }
    return pressed;
}

} // namespace

void apply_theme(float scale) {
    ImGuiStyle& style = ImGui::GetStyle();
    style = ImGuiStyle();
    style.WindowRounding = 0.0f;
    style.ChildRounding = 0.0f;
    style.FrameRounding = 0.0f;
    style.PopupRounding = 0.0f;
    style.ScrollbarRounding = 0.0f;
    style.GrabRounding = 0.0f;
    style.TabRounding = 0.0f;
    style.WindowBorderSize = 0.0f;
    style.ChildBorderSize = 0.0f;
    style.PopupBorderSize = 1.0f;
    style.FrameBorderSize = 1.0f;
    style.WindowPadding = ImVec2(kPad, 8.0f);
    style.FramePadding = ImVec2(8.0f, 4.0f);
    style.ItemSpacing = ImVec2(8.0f, 6.0f);
    style.ItemInnerSpacing = ImVec2(6.0f, 4.0f);
    style.IndentSpacing = 16.0f;
    style.ScrollbarSize = 14.0f;
    style.GrabMinSize = 12.0f;

    ImVec4* c = style.Colors;
    const ImVec4 bg(0.07f, 0.04f, 0.11f, 1.0f);
    const ImVec4 panel(0.11f, 0.07f, 0.18f, 1.0f);
    const ImVec4 frame(0.16f, 0.09f, 0.26f, 1.0f);
    const ImVec4 accent(0.66f, 0.33f, 0.97f, 1.0f);
    const ImVec4 accent_hi(0.76f, 0.48f, 1.00f, 1.0f);
    const ImVec4 text(0.95f, 0.91f, 1.00f, 1.0f);
    const ImVec4 muted(0.70f, 0.62f, 0.82f, 1.0f);
    c[ImGuiCol_Text] = text;
    c[ImGuiCol_TextDisabled] = muted;
    c[ImGuiCol_WindowBg] = bg;
    c[ImGuiCol_ChildBg] = panel;
    c[ImGuiCol_PopupBg] = panel;
    c[ImGuiCol_Border] = ImVec4(0.45f, 0.22f, 0.72f, 1.0f);
    c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg] = frame;
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.24f, 0.12f, 0.40f, 1.0f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.30f, 0.14f, 0.50f, 1.0f);
    c[ImGuiCol_TitleBg] = bg;
    c[ImGuiCol_TitleBgActive] = bg;
    c[ImGuiCol_TitleBgCollapsed] = bg;
    c[ImGuiCol_MenuBarBg] = panel;
    c[ImGuiCol_ScrollbarBg] = bg;
    c[ImGuiCol_ScrollbarGrab] = frame;
    c[ImGuiCol_ScrollbarGrabHovered] = accent;
    c[ImGuiCol_ScrollbarGrabActive] = accent_hi;
    c[ImGuiCol_CheckMark] = accent_hi;
    c[ImGuiCol_SliderGrab] = accent;
    c[ImGuiCol_SliderGrabActive] = accent_hi;
    c[ImGuiCol_Button] = ImVec4(0.28f, 0.12f, 0.46f, 1.0f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.42f, 0.18f, 0.70f, 1.0f);
    c[ImGuiCol_ButtonActive] = accent;
    c[ImGuiCol_Header] = ImVec4(0.28f, 0.12f, 0.46f, 1.0f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.42f, 0.18f, 0.70f, 1.0f);
    c[ImGuiCol_HeaderActive] = accent;
    c[ImGuiCol_Separator] = ImVec4(0.32f, 0.16f, 0.50f, 1.0f);
    c[ImGuiCol_SeparatorHovered] = accent;
    c[ImGuiCol_SeparatorActive] = accent_hi;
    c[ImGuiCol_ResizeGrip] = frame;
    c[ImGuiCol_ResizeGripHovered] = accent;
    c[ImGuiCol_ResizeGripActive] = accent_hi;
    c[ImGuiCol_Tab] = panel;
    c[ImGuiCol_TabHovered] = accent;
    c[ImGuiCol_TabSelected] = ImVec4(0.28f, 0.12f, 0.46f, 1.0f);
    c[ImGuiCol_PlotLines] = accent_hi;
    c[ImGuiCol_PlotHistogram] = accent;
    c[ImGuiCol_TextSelectedBg] = ImVec4(0.66f, 0.33f, 0.97f, 0.35f);
    c[ImGuiCol_NavHighlight] = accent;
    style.ScaleAllSizes(scale);
}

void draw(Frame& frame) {
    g_scale = frame.scale;
    const ImVec2 size = ImGui::GetIO().DisplaySize;
    const float border = frame.fullscreen ? 0.0f : px(kBorder);
    const float title = frame.fullscreen ? 0.0f : px(kTitleBar);
    const float sidebar = frame.fullscreen ? 0.0f : px(kSidebar);

    frame.video_x = border + sidebar;
    frame.video_y = border + title;
    frame.video_w = std::max(1.0f, size.x - border * 2.0f - sidebar);
    frame.video_h = std::max(1.0f, size.y - border * 2.0f - title);
    frame.caption_min = ImVec2(border, border);
    frame.caption_max = ImVec2(size.x - px(kCaptionButtonsWidth + 4.0f), border + title);

    if (frame.show_stats) draw_stats_overlay(frame);
    if (frame.fullscreen) return;

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(size);
    ImGui::Begin("##chrome", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBringToFrontOnFocus |
                     ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoBackground);

    auto* draw_list = ImGui::GetWindowDrawList();
    draw_list->AddRectFilled(ImVec2(0, 0), ImVec2(size.x, title + border), u32(22, 12, 36));
    draw_list->AddRectFilled(ImVec2(0, title + border), ImVec2(sidebar + border, size.y), u32(18, 10, 28));
    draw_list->AddRect(ImVec2(0.5f, 0.5f), ImVec2(size.x - 0.5f, size.y - 0.5f), u32(168, 85, 247), 0.0f, 0,
                       border);

    ImGui::SetCursorPos(ImVec2(px(kPad), (title + border - ImGui::GetTextLineHeight()) * 0.5f));
    ImGui::TextUnformatted("GroovyGrab");

    ImGui::SetCursorPos(ImVec2(size.x - px(kCaptionButtonsWidth), px(5.0f)));
    if (caption_button("##min", CaptionButton::Minimize)) frame.minimize = true;
    ImGui::SameLine(0.0f, px(6.0f));
    if (caption_button("##max", CaptionButton::Maximize)) frame.toggle_maximize = true;
    ImGui::SameLine(0.0f, px(6.0f));
    if (caption_button("##close", CaptionButton::Close)) frame.close = true;

    ImGui::SetCursorPos(ImVec2(border, title + border));
    ImGui::BeginChild("##sidebar", ImVec2(sidebar - border, size.y - title - border * 2.0f),
                      ImGuiChildFlags_AlwaysUseWindowPadding);

    section_header("Capture", frame.has_video ? "Receiving Video" : "No Video", frame.has_video);
    row_label("Device");
    ImGui::SetNextItemWidth(-(ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x));
    if (combo("##device", frame.devices, frame.device_index, "None Found")) frame.device_changed = true;
    ImGui::SameLine();
    if (refresh_button()) frame.refresh_devices = true;
    row_label("Audio");
    if (combo("##audio", frame.audio_devices, frame.audio_index, "None Found")) frame.audio_changed = true;
    row_label("Colour Format");
    if (combo("##pixelformat", frame.pixel_formats, frame.pixel_format_index, "None")) frame.format_changed = true;
    row_label("Resolution");
    if (combo("##size", frame.sizes, frame.size_index, "None", frame.default_size_index)) frame.format_changed = true;
    row_label("Frame Rate");
    if (combo("##rate", frame.rates, frame.rate_index, "None", frame.default_rate_index)) frame.format_changed = true;

    const bool mister_up = frame.stats.mister_connected;
    const char* mister_state = mister_up                  ? "Connected"
                               : !frame.stream              ? "Idle"
                               : frame.stats.mister_calibrating ? "Calibrating"
                                                            : "Connecting";
    section_header("MiSTer", mister_state, mister_up);
    row_label("Address");
    ImGui::InputTextWithHint("##ip", "MiSTer's IP address", frame.ip, sizeof(frame.ip));
    if (ImGui::IsItemDeactivatedAfterEdit()) frame.ip_changed = true;
    row_label("CRT Output");
    const char* crt_i15 = frame.pal ? "576i / 15 kHz" : "480i / 15 kHz";
    const char* crt_p15 = frame.pal ? "288p / 15 kHz" : "240p / 15 kHz";
    if (ImGui::BeginCombo("##mode", frame.progressive ? crt_p15 : crt_i15)) {
        if (ImGui::Selectable(crt_i15, !frame.progressive)) frame.progressive = false;
        if (ImGui::Selectable(crt_p15, frame.progressive)) frame.progressive = true;
        ImGui::EndCombo();
    }
    row_label("Crop / Pad H");
    // AlwaysClamp also covers a value typed after Ctrl+click.
    ImGui::SliderInt("##croph", &frame.crop_h, -video_view::kMaxCropPercent, video_view::kMaxCropPercent, "%d%%",
                     ImGuiSliderFlags_AlwaysClamp);
    row_label("Crop / Pad V");
    ImGui::SliderInt("##cropv", &frame.crop_v, -video_view::kMaxCropPercent, video_view::kMaxCropPercent, "%d%%",
                     ImGuiSliderFlags_AlwaysClamp);
    ImGui::Checkbox("16:9 Letterbox", &frame.letterbox);
    ImGui::SameLine(0.0f, px(24.0f));
    ImGui::Checkbox("Stats", &frame.show_stats);

    ImGui::Dummy(ImVec2(0.0f, px(2.0f)));
    if (frame.stream) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.42f, 0.12f, 0.22f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.58f, 0.16f, 0.28f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.70f, 0.20f, 0.32f, 1.0f));
        if (ImGui::Button("Stop Stream", ImVec2(-FLT_MIN, px(32.0f)))) frame.stream = false;
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.42f, 0.18f, 0.70f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.54f, 0.28f, 0.86f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.66f, 0.33f, 0.97f, 1.0f));
        ImGui::BeginDisabled(!frame.has_capture);
        if (ImGui::Button("Start Stream", ImVec2(-FLT_MIN, px(32.0f)))) frame.stream = true;
        ImGui::EndDisabled();
    }
    ImGui::PopStyleColor(3);
    // Every error goes here, below the last control, so none of them moves the panel.
    error_text(frame.stats.capture_error);
    error_text(frame.stats.audio_error);
    error_text(frame.stats.settings_error);
    if (frame.stream && !mister_up && !frame.stats.mister_calibrating) error_text(frame.stats.mister_error);
    if (mister_up) error_text(frame.stats.capture_warning);

    ImGui::EndChild();

    if (!frame.has_video) {
        const char* msg = !frame.stats.capture_error.empty() ? "Capture Unavailable" : "Waiting For Capture";
        const ImVec2 ts = ImGui::CalcTextSize(msg);
        draw_list->AddText(ImVec2(frame.video_x + (frame.video_w - ts.x) * 0.5f,
                                  frame.video_y + (frame.video_h - ts.y) * 0.5f),
                           u32(168, 140, 200), msg);
    }

    ImGui::End();
}

} // namespace app_gui
