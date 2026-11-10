// The window's contents: a card per Razer device with its battery, DPI
// stages and lighting. Drawn with Dear ImGui each frame.
#pragma once

#include <windows.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "devices.h"
#include "imgui.h"

namespace sq {

class App {
public:
    App(HWND hwnd, std::function<void()> wake);

    void Frame();
    void Pump();  // picks up what the worker read; call every loop
    void DevicesChanged();  // Windows says something was plugged or unplugged

    bool InCaption(float x, float y) const;
    void SetScale(float scale);
    void SetDark(bool dark);
    bool FontsDirty() const { return fontsDirty_; }
    void ReloadFonts();

    bool Place(RECT& r) const;
    void SavePlace(const RECT& r);

private:
    void drawTitle(float width);
    void drawDevice(Device& d, float width);
    void drawZoneRow(Device& d, Zone* z, float labelW);
    bool effectButton(const char* label, bool on);
    bool iconButton(const char* icon, const char* tip, bool danger = false);
    void loadConfig();
    void saveConfig();

    HWND hwnd_;
    std::function<void()> wake_;
    std::unique_ptr<Devices> devices_;
    std::vector<Device> shown_;
    bool loaded_ = false;
    bool synapse_ = false;
    ULONGLONG nextRefresh_ = 0;   // GetTickCount64 of the next battery refresh
    ULONGLONG changedAt_ = 0;     // a plug or unplug, waiting to settle

    std::string dir_;
    bool placed_ = false;
    RECT place_{};

    float scale_ = 1;
    bool fontsDirty_ = true;
    ImFont* body_ = nullptr;
    ImFont* small_ = nullptr;
    ImFont* title_ = nullptr;
    ImFont* label_ = nullptr;

    ImVec2 barMax_;  // the title bar, for dragging
    std::vector<std::pair<ImVec2, ImVec2>> hot_;
};

}  // namespace sq
