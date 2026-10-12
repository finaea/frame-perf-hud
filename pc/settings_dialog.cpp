#include "settings_dialog.h"

#include "link_wire.h"
#include "resource.h"
#include "win_text.h"

namespace fph {
namespace pc {

namespace {

std::string dialogText(HWND dlg, int id) {
    wchar_t buffer[256] = {};
    GetDlgItemTextW(dlg, id, buffer, 255);
    return narrow(buffer);
}

std::string trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t");
    if (a == std::string::npos) return "";
    const size_t b = s.find_last_not_of(" \t");
    return s.substr(a, b - a + 1);
}

INT_PTR CALLBACK settingsProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    auto* data = reinterpret_cast<SettingsDialogData*>(GetWindowLongPtrW(dlg, DWLP_USER));
    switch (msg) {
        case WM_INITDIALOG:
            data = reinterpret_cast<SettingsDialogData*>(lp);
            SetWindowLongPtrW(dlg, DWLP_USER, lp);
            SetDlgItemTextW(dlg, IDC_MANUAL_IP, widen(data->manualIp).c_str());
            CheckDlgButton(dlg, IDC_AUTOSTART, data->autostart ? BST_CHECKED : BST_UNCHECKED);
            SetDlgItemTextW(dlg, IDC_INFO, widen(data->info).c_str());
            return TRUE;
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case IDC_FORGET:
                    data->forgetHeadsets = true;
                    EnableWindow(GetDlgItem(dlg, IDC_FORGET), FALSE);
                    SetDlgItemTextW(dlg, IDC_FORGET, L"Forgotten (on OK)");
                    return TRUE;
                case IDOK: {
                    const std::string ip = trim(dialogText(dlg, IDC_MANUAL_IP));
                    uint32_t parsed = 0;
                    if (!ip.empty() && !link::ipv4FromText(ip, parsed)) {
                        MessageBoxW(dlg, L"That isn't an IPv4 address like 192.168.1.47. Leave it empty to search.",
                                    L"frame-perf-hud", MB_OK | MB_ICONWARNING);
                        SetFocus(GetDlgItem(dlg, IDC_MANUAL_IP));
                        return TRUE;
                    }
                    data->manualIp = ip;
                    data->autostart = IsDlgButtonChecked(dlg, IDC_AUTOSTART) == BST_CHECKED;
                    EndDialog(dlg, IDOK);
                    return TRUE;
                }
                case IDCANCEL:
                    data->forgetHeadsets = false;
                    EndDialog(dlg, IDCANCEL);
                    return TRUE;
            }
            break;
    }
    return FALSE;
}

}  // namespace

bool showSettingsDialog(HINSTANCE instance, HWND parent, SettingsDialogData& data) {
    return DialogBoxParamW(instance, MAKEINTRESOURCEW(IDD_SETTINGS), parent, settingsProc,
                           reinterpret_cast<LPARAM>(&data)) == IDOK;
}

}  // namespace pc
}  // namespace fph
