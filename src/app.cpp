#include "app.h"

#include <shlobj.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>

#include "hid.h"
#include "nlohmann/json.hpp"

namespace sq {
namespace {

ImVec4 hex(unsigned rgb, float a = 1.0f) {
    return ImVec4(((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, a);
}

// kitchensink's colours.
struct Palette {
    ImVec4 bg, card, border, text, muted, accent, hover, frame, accentText, warn, good;
};
const Palette kDark = {hex(0x15171C), hex(0x1E2128), hex(0x2B2F38), hex(0xE6E8EC), hex(0x8C929D), hex(0x5AA8FF),
                       hex(0x30353F), hex(0x262A33), hex(0xFFFFFF), hex(0xE3A73B), hex(0x4CC38A)};
const Palette kLight = {hex(0xF2F3F5), hex(0xFFFFFF), hex(0xD5D9DF), hex(0x1F2328), hex(0x656D76), hex(0x0969DA),
                        hex(0xE4E7EB), hex(0xEEF0F3), hex(0xFFFFFF), hex(0x9A6700), hex(0x1A7F37)};
const Palette* pal = &kDark;

ImU32 col(const ImVec4& v, float a = 1) { return ImGui::ColorConvertFloat4ToU32(ImVec4(v.x, v.y, v.z, v.w * a)); }

namespace icon {
const char* Refresh = "\xEE\x9C\xAC";   // U+E72C
const char* Minimise = "\xEE\xA4\xA1";  // U+E921
const char* Close = "\xEE\xA2\xBB";     // U+E8BB
}  // namespace icon

std::string fontPath(const char* file) {
    char dir[MAX_PATH];
    GetWindowsDirectoryA(dir, MAX_PATH);
    return std::string(dir) + "\\Fonts\\" + file;
}

bool exists(const std::string& path) { return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

std::string dpiText(std::pair<int, int> d) {
    char buf[32];
    if (d.first == d.second) std::snprintf(buf, sizeof(buf), "%d", d.first);
    else std::snprintf(buf, sizeof(buf), "%d\xC3\x97%d", d.first, d.second);
    return buf;
}

const char* kEffects[] = {"Off", "Static", "Breathing", "Spectrum"};
const Effect kEffectIds[] = {EffectOff, EffectStatic, EffectBreathing, EffectSpectrum};

}  // namespace

App::App(HWND hwnd, std::function<void()> wake) : hwnd_(hwnd), wake_(std::move(wake)) {
    PWSTR p = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p)))
        dir_ = (std::filesystem::path(p) / "squeak").u8string();
    CoTaskMemFree(p);
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::u8path(dir_), ec);
    loadConfig();
    synapse_ = SynapseRunning();
    devices_ = std::make_unique<Devices>(wake_);
    nextRefresh_ = GetTickCount64() + 60000;
}

void App::loadConfig() {
    std::ifstream in(std::filesystem::u8path(dir_) / "config.json");
    if (!in) return;
    try {
        nlohmann::json j = nlohmann::json::parse(in);
        if (j.contains("window")) {
            auto w = j["window"];
            place_ = RECT{w.value("x", 0), w.value("y", 0), w.value("x", 0) + w.value("w", 0), w.value("y", 0) + w.value("h", 0)};
            placed_ = place_.right > place_.left && place_.bottom > place_.top;
        }
    } catch (...) {
    }
}

void App::saveConfig() {
    nlohmann::json j;
    if (placed_)
        j["window"] = {{"x", place_.left}, {"y", place_.top}, {"w", place_.right - place_.left}, {"h", place_.bottom - place_.top}};
    std::ofstream out(std::filesystem::u8path(dir_) / "config.json");
    out << j.dump(2) << "\n";
}

bool App::Place(RECT& r) const {
    if (!placed_) return false;
    r = place_;
    return true;
}

void App::SavePlace(const RECT& r) {
    placed_ = true;
    place_ = r;
    saveConfig();
}

void App::Pump() {
    if (devices_->Changed(shown_)) loaded_ = true;
    ULONGLONG now = GetTickCount64();
    // Plugging in a dongle sends a burst of notices: wait for it to settle.
    if (changedAt_ && now - changedAt_ > 1500) {
        changedAt_ = 0;
        devices_->Refresh();
    }
    if (now >= nextRefresh_) {
        nextRefresh_ = now + 60000;
        synapse_ = SynapseRunning();
        if (IsWindowVisible(hwnd_) && !IsIconic(hwnd_)) devices_->Refresh();
    }
}

void App::DevicesChanged() { changedAt_ = GetTickCount64(); }

bool App::InCaption(float x, float y) const {
    if (x >= barMax_.x || y >= barMax_.y) return false;
    for (const auto& [a, b] : hot_)
        if (x >= a.x && y >= a.y && x < b.x && y < b.y) return false;
    return true;
}

void App::SetScale(float scale) {
    scale_ = scale;
    fontsDirty_ = true;
}

void App::SetDark(bool dark) { pal = dark ? &kDark : &kLight; }

void App::ReloadFonts() {
    fontsDirty_ = false;
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    static const ImWchar text[] = {0x0020, 0x024F, 0x2000, 0x206F, 0x2190, 0x21FF, 0x25A0, 0x25FF, 0};
    static const ImWchar icons[] = {0xE700, 0xF8FF, 0};
    std::string regular = fontPath("segoeui.ttf"), semibold = fontPath("seguisb.ttf");
    std::string iconFont = fontPath("SegoeIcons.ttf");
    if (!exists(iconFont)) iconFont = fontPath("segmdl2.ttf");
    if (!exists(semibold)) semibold = regular;
    auto add = [&](const std::string& path, float px, bool withIcons) -> ImFont* {
        ImFontConfig cfg;
        cfg.OversampleH = 2;
        ImFont* f = exists(path) ? io.Fonts->AddFontFromFileTTF(path.c_str(), px * scale_, &cfg, text) : nullptr;
        if (!f) {
            ImFontConfig def;
            def.SizePixels = px * scale_;
            return io.Fonts->AddFontDefault(&def);
        }
        if (withIcons && exists(iconFont)) {
            ImFontConfig merge;
            merge.MergeMode = true;
            merge.GlyphOffset = ImVec2(0, 1.5f * scale_);
            io.Fonts->AddFontFromFileTTF(iconFont.c_str(), px * scale_ * 0.9f, &merge, icons);
        }
        return f;
    };
    body_ = add(regular, 15, true);
    small_ = add(regular, 13, true);
    title_ = add(semibold, 17, false);
    label_ = add(semibold, 12, false);
    io.FontDefault = body_;

    ImGuiStyle& s = ImGui::GetStyle();
    s = ImGuiStyle();
    s.WindowPadding = ImVec2(0, 0);
    s.FramePadding = ImVec2(8, 4);
    s.ItemSpacing = ImVec2(6, 6);
    s.FrameRounding = 0;
    s.GrabRounding = 0;
    s.PopupRounding = 0;
    s.WindowBorderSize = 0;
    s.ChildBorderSize = 0;
    s.PopupBorderSize = 1;
    s.ScrollbarSize = 8;
    s.ScrollbarRounding = 0;
    s.GrabMinSize = 10;
    s.ScaleAllSizes(scale_);
}

bool App::iconButton(const char* glyph, const char* tip, bool danger) {
    const Palette& p = *pal;
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, danger ? hex(0xC42B1C) : p.hover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, danger ? hex(0xC42B1C) : p.hover);
    ImGui::PushStyleColor(ImGuiCol_Text, p.muted);
    ImGui::PushFont(small_);
    bool pressed = ImGui::Button(glyph, ImVec2(30 * scale_, 26 * scale_));
    ImGui::PopFont();
    ImGui::PopStyleColor(4);
    hot_.push_back({ImGui::GetItemRectMin(), ImGui::GetItemRectMax()});
    if (tip && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) ImGui::SetTooltip("%s", tip);
    return pressed;
}

bool App::effectButton(const char* label, bool on) {
    const Palette& p = *pal;
    ImGui::PushStyleColor(ImGuiCol_Button, on ? p.accent : p.frame);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, on ? p.accent : p.hover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, on ? p.accent : p.hover);
    ImGui::PushStyleColor(ImGuiCol_Text, on ? p.accentText : p.text);
    ImGui::PushFont(small_);
    bool pressed = ImGui::Button(label, ImVec2(ImGui::CalcTextSize("Breathing").x + 16 * scale_, 0));
    ImGui::PopFont();
    ImGui::PopStyleColor(4);
    return pressed;
}

void App::Frame() {
    hot_.clear();
    const Palette& p = *pal;
    ImGuiIO& io = ImGui::GetIO();
    ImGuiStyle& st = ImGui::GetStyle();
    st.Colors[ImGuiCol_WindowBg] = p.bg;
    st.Colors[ImGuiCol_Text] = p.text;
    st.Colors[ImGuiCol_PopupBg] = p.card;
    st.Colors[ImGuiCol_Border] = p.border;
    st.Colors[ImGuiCol_FrameBg] = p.frame;
    st.Colors[ImGuiCol_FrameBgHovered] = p.hover;
    st.Colors[ImGuiCol_FrameBgActive] = p.hover;
    st.Colors[ImGuiCol_SliderGrab] = p.accent;
    st.Colors[ImGuiCol_SliderGrabActive] = p.accent;
    st.Colors[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    st.Colors[ImGuiCol_ScrollbarGrab] = p.border;
    st.Colors[ImGuiCol_ScrollbarGrabHovered] = p.muted;
    st.Colors[ImGuiCol_ScrollbarGrabActive] = p.muted;

    if (ImGui::IsKeyPressed(ImGuiKey_F5, false)) devices_->Refresh();

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("squeak", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);
    drawTitle(io.DisplaySize.x);

    const float k = scale_;
    const float pad = 14 * k;
    ImGui::SetCursorPos(ImVec2(0, barMax_.y));
    ImGui::BeginChild("list", ImVec2(io.DisplaySize.x, io.DisplaySize.y - barMax_.y), ImGuiChildFlags_None);
    ImGui::SetCursorPos(ImVec2(pad, 4 * k));
    ImGui::BeginGroup();
    const float width = io.DisplaySize.x - 2 * pad - 8 * k;
    if (synapse_) {
        ImGui::PushFont(small_);
        ImGui::PushStyleColor(ImGuiCol_Text, p.warn);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
        ImGui::TextWrapped("Razer Synapse is running, so it may put its own lighting back over what's set here.");
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        ImGui::PopFont();
        ImGui::Dummy(ImVec2(0, 2 * k));
    }
    if (!loaded_) {
        ImGui::TextColored(p.muted, "Looking for devices\xE2\x80\xA6");
    } else if (shown_.empty()) {
        ImGui::TextColored(p.muted, "No Razer devices found. Plug one in and it'll show here.");
    }
    for (auto& d : shown_) {
        ImGui::PushID(int(d.pid));
        drawDevice(d, width);
        ImGui::PopID();
        ImGui::Dummy(ImVec2(0, 4 * k));
    }
    ImGui::EndGroup();
    ImGui::Dummy(ImVec2(0, 8 * k));
    ImGui::EndChild();
    ImGui::End();
}

void App::drawTitle(float width) {
    const Palette& p = *pal;
    const float k = scale_;
    const float h = 38 * k;
    barMax_ = ImVec2(width, h);
    ImGui::SetCursorPos(ImVec2(14 * k, 11 * k));
    ImGui::PushFont(label_);
    ImGui::TextColored(p.accent, "SQUEAK");
    ImGui::PopFont();
    ImGui::SameLine(0, 8 * k);
    ImGui::PushFont(small_);
    ImGui::SetCursorPosY(10 * k);
    ImGui::TextColored(p.muted, "%s", devices_->Busy() ? "reading\xE2\x80\xA6" : "Razer mice and keyboards, without Synapse");
    ImGui::PopFont();

    ImGui::SetCursorPos(ImVec2(width - 3 * 30 * k - 6 * k, 6 * k));
    if (iconButton(icon::Refresh, "Read the devices again (F5)")) {
        synapse_ = SynapseRunning();
        devices_->Refresh();
    }
    ImGui::SameLine(0, 0);
    if (iconButton(icon::Minimise, "Minimise")) ShowWindow(hwnd_, SW_MINIMIZE);
    ImGui::SameLine(0, 0);
    if (iconButton(icon::Close, "Close", true)) PostMessageW(hwnd_, WM_CLOSE, 0, 0);
}

void App::drawDevice(Device& d, float width) {
    const Palette& p = *pal;
    const float k = scale_;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImDrawListSplitter split;
    split.Split(dl, 2);
    split.SetCurrentChannel(dl, 1);
    const ImVec2 top = ImGui::GetCursorScreenPos();
    const float pad = 14 * k;
    ImGui::SetCursorScreenPos(ImVec2(top.x + pad, top.y + 10 * k));
    ImGui::BeginGroup();
    const float inner = width - 2 * pad;

    // Name, then how it's connected and its battery on the right.
    ImGui::PushFont(title_);
    ImGui::TextUnformatted(d.name.c_str());
    ImGui::PopFont();
    {
        std::string right;
        ImVec4 colour = p.muted;
        if (d.asleep) {
            right = "Asleep: move it to wake it";
        } else {
            right = d.connection;
            if (d.battery >= 0) {
                if (!right.empty()) right += "  \xC2\xB7  ";
                right += std::to_string(d.battery) + "%";
                if (d.charging) right += ", charging";
                if (d.battery <= 15 && !d.charging) colour = p.warn;
            }
        }
        ImGui::PushFont(small_);
        ImVec2 sz = ImGui::CalcTextSize(right.c_str());
        ImGui::SameLine();
        ImGui::SetCursorScreenPos(ImVec2(top.x + pad + inner - sz.x, top.y + 10 * k + (title_->FontSize - small_->FontSize) / 2 + 1 * k));
        ImGui::TextColored(colour, "%s", right.c_str());
        ImGui::PopFont();
    }

    if (!d.asleep) {
        // DPI stages as chips, the one in use filled.
        ImGui::Dummy(ImVec2(0, 2 * k));
        if (d.stages && !d.stages->dpi.empty()) {
            ImGui::PushFont(label_);
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(p.muted, "DPI");
            ImGui::PopFont();
            ImGui::PushFont(small_);
            for (size_t i = 0; i < d.stages->dpi.size(); i++) {
                ImGui::SameLine(0, i == 0 ? 10 * k : 4 * k);
                bool on = int(i) + 1 == d.stages->active;
                std::string t = dpiText(d.stages->dpi[i]);
                ImVec2 sz = ImGui::CalcTextSize(t.c_str());
                ImVec2 a = ImGui::GetCursorScreenPos();
                ImVec2 b(a.x + sz.x + 16 * k, a.y + sz.y + 8 * k);
                ImGui::GetWindowDrawList()->AddRectFilled(a, b, col(on ? p.accent : p.frame));
                ImGui::GetWindowDrawList()->AddText(ImVec2(a.x + 8 * k, a.y + 4 * k), col(on ? p.accentText : p.text), t.c_str());
                ImGui::Dummy(ImVec2(b.x - a.x, b.y - a.y));
                if (ImGui::IsItemHovered()) ImGui::SetTooltip(on ? "Stage %d, in use" : "Stage %d", int(i) + 1);
            }
            ImGui::PopFont();
            // If it was changed on the fly, the current DPI may be none of them.
            bool matches = d.stages->active >= 1 && d.stages->active <= int(d.stages->dpi.size()) &&
                           d.stages->dpi[d.stages->active - 1] == d.dpi;
            if (d.dpi.first > 0 && !matches) {
                ImGui::SameLine(0, 10 * k);
                ImGui::PushFont(small_);
                ImGui::AlignTextToFramePadding();
                ImGui::TextColored(p.muted, "now %s", dpiText(d.dpi).c_str());
                ImGui::PopFont();
            }
        } else if (d.dpi.first > 0) {
            ImGui::PushFont(small_);
            ImGui::TextColored(p.muted, "DPI %s", dpiText(d.dpi).c_str());
            ImGui::PopFont();
        }

        // Smaller facts on one line.
        {
            std::string facts;
            auto add = [&](const std::string& s) {
                if (s.empty()) return;
                if (!facts.empty()) facts += "   \xC2\xB7   ";
                facts += s;
            };
            if (d.pollHz) add(std::to_string(d.pollHz) + " Hz");
            if (!d.firmware.empty()) add("firmware " + d.firmware);
            ImGui::PushFont(small_);
            ImGui::TextColored(p.muted, "%s", facts.c_str());
            ImGui::PopFont();
        }

        if (!d.zones.empty()) {
            ImGui::Dummy(ImVec2(0, 4 * k));
            ImGui::PushFont(label_);
            ImGui::TextColored(p.muted, "LIGHTING");
            ImGui::PopFont();
            float labelW = 0;
            ImGui::PushFont(small_);
            for (const auto& z : d.zones) labelW = std::max(labelW, ImGui::CalcTextSize(z.name.c_str()).x);
            labelW = std::max(labelW, ImGui::CalcTextSize("All zones").x) + 14 * k;
            ImGui::PopFont();
            if (d.zones.size() > 1) drawZoneRow(d, nullptr, labelW);
            for (auto& z : d.zones) {
                ImGui::PushID(z.id);
                drawZoneRow(d, &z, labelW);
                ImGui::PopID();
            }
        }
    }
    ImGui::EndGroup();
    const ImVec2 end = ImGui::GetItemRectMax();
    const ImVec2 bottom(top.x + width, end.y + 12 * k);
    split.SetCurrentChannel(dl, 0);
    dl->AddRectFilled(top, bottom, col(p.card));
    split.Merge(dl);
    ImGui::SetCursorScreenPos(ImVec2(top.x, bottom.y));
    ImGui::Dummy(ImVec2(width, 0));
}

// drawZoneRow draws one zone's controls, or (z null) a row that sets every
// zone at once.
void App::drawZoneRow(Device& d, Zone* z, float labelW) {
    const Palette& p = *pal;
    const float k = scale_;
    const bool all = z == nullptr;
    // The "all" row shows a choice only when every zone agrees.
    Light shownLight = all ? d.zones[0].light : z->light;
    bool agree = true;
    if (all)
        for (const auto& o : d.zones)
            agree &= o.light.effect == shownLight.effect;

    ImGui::PushFont(small_);
    ImGui::AlignTextToFramePadding();
    float x = ImGui::GetCursorPosX();
    ImGui::TextColored(all ? p.muted : p.text, "%s", all ? "All zones" : z->name.c_str());
    ImGui::SameLine(x + labelW);

    auto apply = [&](Light l) {
        for (auto& o : d.zones) {
            if (!all && &o != z) continue;
            o.light = l;
            devices_->SetLight(d, o.id, l);
        }
    };

    for (int i = 0; i < 4; i++) {
        if (i) ImGui::SameLine(0, 2 * k);
        bool on = (!all || agree) && shownLight.effect == kEffectIds[i];
        if (effectButton(kEffects[i], on)) {
            Light l = shownLight;
            l.effect = kEffectIds[i];
            apply(l);
        }
    }
    if (shownLight.effect == EffectOther && !all) {
        ImGui::SameLine(0, 8 * k);
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(p.muted, "(another effect)");
    }

    const bool coloured = (!all || agree) && (shownLight.effect == EffectStatic || shownLight.effect == EffectBreathing);
    if (coloured) {
        ImGui::SameLine(0, 10 * k);
        float c[3] = {shownLight.r / 255.0f, shownLight.g / 255.0f, shownLight.b / 255.0f};
        if (ImGui::ColorEdit3("##colour", c,
                              ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel | ImGuiColorEditFlags_PickerHueWheel |
                                  ImGuiColorEditFlags_NoSidePreview | ImGuiColorEditFlags_NoAlpha)) {
            Light l = shownLight;
            l.r = uint8_t(c[0] * 255 + 0.5f);
            l.g = uint8_t(c[1] * 255 + 0.5f);
            l.b = uint8_t(c[2] * 255 + 0.5f);
            apply(l);
        }
    }
    const bool lit = (!all || agree) && shownLight.effect != EffectOff;
    if (lit) {
        ImGui::SameLine(0, 10 * k);
        int pct = ((all ? d.zones[0].brightness : z->brightness) * 100 + 127) / 255;
        ImGui::SetNextItemWidth(130 * k);
        if (ImGui::SliderInt("##brightness", &pct, 0, 100, "%d%%", ImGuiSliderFlags_AlwaysClamp)) {
            int v = (pct * 255 + 50) / 100;
            for (auto& o : d.zones) {
                if (!all && &o != z) continue;
                o.brightness = v;
                devices_->SetBrightness(d, o.id, v);
            }
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) ImGui::SetTooltip("Brightness");
    }
    ImGui::PopFont();
}

}  // namespace sq
