#include "hid.h"

#include <windows.h>
// After windows.h.
#include <hidsdi.h>
#include <setupapi.h>
#include <tlhelp32.h>

#include <cwchar>

namespace sq {

std::vector<HidInfo> FindControlInterfaces() {
    std::vector<HidInfo> out;
    GUID guid;
    HidD_GetHidGuid(&guid);
    HDEVINFO set = SetupDiGetClassDevsW(&guid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return out;
    SP_DEVICE_INTERFACE_DATA data{sizeof(data)};
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(set, nullptr, &guid, i, &data); i++) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &data, nullptr, 0, &need, nullptr);
        std::vector<uint8_t> buf(need + sizeof(wchar_t));
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buf.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(set, &data, detail, need, nullptr, nullptr)) continue;
        std::wstring path = detail->DevicePath;
        std::wstring lower = path;
        for (auto& c : lower) c = towlower(c);
        if (lower.find(L"vid_1532") == std::wstring::npos) continue;
        // Mice and keyboards are opened by Windows itself, so ask for no
        // access: feature reports still work.
        HANDLE h = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        PHIDP_PREPARSED_DATA pre = nullptr;
        HIDP_CAPS caps{};
        bool ok = HidD_GetPreparsedData(h, &pre) && HidP_GetCaps(pre, &caps) == HIDP_STATUS_SUCCESS;
        if (pre) HidD_FreePreparsedData(pre);
        if (ok && caps.FeatureReportByteLength == 91) {
            HidInfo info;
            info.path = path;
            HIDD_ATTRIBUTES attr{sizeof(attr)};
            if (HidD_GetAttributes(h, &attr)) info.pid = attr.ProductID;
            wchar_t name[128] = {};
            if (HidD_GetProductString(h, name, sizeof(name))) {
                char utf8[256] = {};
                WideCharToMultiByte(CP_UTF8, 0, name, -1, utf8, sizeof(utf8), nullptr, nullptr);
                info.product = utf8;
            }
            out.push_back(info);
        }
        CloseHandle(h);
    }
    SetupDiDestroyDeviceInfoList(set);
    return out;
}

Hid::Hid(const std::wstring& path) {
    handle_ = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
}

Hid::~Hid() {
    if (Open()) CloseHandle(handle_);
}

bool Hid::Open() const { return handle_ != INVALID_HANDLE_VALUE && handle_ != nullptr; }

bool Hid::Send(const Report& request, Reply& reply) {
    if (!Open()) return false;
    Report out = request;
    if (!HidD_SetFeature(handle_, out.data(), DWORD(out.size()))) return false;
    // Poll for the answer: a reply to something else, or "busy", means not yet.
    for (int tries = 0; tries < 50; tries++) {
        Sleep(tries < 5 ? 2 : 10);
        Report in{};
        if (!HidD_GetFeature(handle_, in.data(), DWORD(in.size()))) return false;
        reply = ParseReply(in);
        if (reply.status == Busy || reply.status == New) continue;
        if (reply.cls != request[7] || reply.id != request[8]) continue;
        return true;
    }
    reply.status = Timeout;
    return true;
}

bool SynapseRunning() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{sizeof(pe)};
    bool found = false;
    for (BOOL ok = Process32FirstW(snap, &pe); ok && !found; ok = Process32NextW(snap, &pe))
        found = _wcsicmp(pe.szExeFile, L"RazerAppEngine.exe") == 0 || _wcsicmp(pe.szExeFile, L"Razer Synapse 3.exe") == 0;
    CloseHandle(snap);
    return found;
}

}  // namespace sq
