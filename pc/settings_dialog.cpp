#include "settings_dialog.h"

#include "layout.h"
#include "link_wire.h"
#include "resource.h"
#include "win_text.h"

namespace fph {
namespace pc {

namespace {

constexpr UINT_PTR kUpdateStatusTimer = 1;  // Release G

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
            SetDlgItemInt(dlg, IDC_CAMERA_FPS, static_cast<UINT>(data->cameraFps), FALSE);  // T10
            SetDlgItemTextW(dlg, IDC_INFO, widen(data->info).c_str());
            CheckDlgButton(dlg, IDC_UPDATE_CHECK, data->updateCheck ? BST_CHECKED : BST_UNCHECKED);  // Release G
            if (data->updateStatus) SetDlgItemTextW(dlg, IDC_UPDATE_STATUS, widen(data->updateStatus()).c_str());
            SetTimer(dlg, kUpdateStatusTimer, 250, nullptr);
            return TRUE;
        case WM_TIMER:  // Release G: Check now answers while the dialog is open
            if (wp == kUpdateStatusTimer && data != nullptr && data->updateStatus) {
                const std::wstring text = widen(data->updateStatus());
                wchar_t shown[512] = {};
                GetDlgItemTextW(dlg, IDC_UPDATE_STATUS, shown, 511);
                if (text != shown) SetDlgItemTextW(dlg, IDC_UPDATE_STATUS, text.c_str());
            }
            return TRUE;
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case IDC_UPDATE_NOW:
                    if (data->checkNow) data->checkNow();
                    return TRUE;
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
                    BOOL fpsOk = FALSE;  // T10
                    const UINT fps = GetDlgItemInt(dlg, IDC_CAMERA_FPS, &fpsOk, FALSE);
                    if (!fpsOk || fps < 1 || fps > 30) {
                        MessageBoxW(dlg, L"The camera frame rate is a whole number from 1 to 30.", L"frame-perf-hud",
                                    MB_OK | MB_ICONWARNING);
                        SetFocus(GetDlgItem(dlg, IDC_CAMERA_FPS));
                        return TRUE;
                    }
                    data->cameraFps = static_cast<int>(fps);
                    data->manualIp = ip;
                    data->autostart = IsDlgButtonChecked(dlg, IDC_AUTOSTART) == BST_CHECKED;
                    data->updateCheck = IsDlgButtonChecked(dlg, IDC_UPDATE_CHECK) == BST_CHECKED;
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

INT_PTR CALLBACK renameProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    auto* name = reinterpret_cast<std::string*>(GetWindowLongPtrW(dlg, DWLP_USER));
    switch (msg) {
        case WM_INITDIALOG: {
            SetWindowLongPtrW(dlg, DWLP_USER, lp);
            name = reinterpret_cast<std::string*>(lp);
            HWND edit = GetDlgItem(dlg, IDC_RENAME_EDIT);
            SendMessageW(edit, EM_LIMITTEXT, 31, 0);  // the headset keeps 31 bytes; it cuts at a UTF-8 boundary too
            SetWindowTextW(edit, widen(*name).c_str());
            SendMessageW(edit, EM_SETSEL, 0, -1);
            SetFocus(edit);
            return FALSE;  // focus set here
        }
        case WM_COMMAND:
            if (LOWORD(wp) == IDOK) {
                const std::string text = trim(dialogText(dlg, IDC_RENAME_EDIT));
                if (text.empty()) {
                    SetFocus(GetDlgItem(dlg, IDC_RENAME_EDIT));
                    return TRUE;
                }
                *name = text;
                EndDialog(dlg, IDOK);
                return TRUE;
            }
            if (LOWORD(wp) == IDCANCEL) {
                EndDialog(dlg, IDCANCEL);
                return TRUE;
            }
            break;
    }
    return FALSE;
}

INT_PTR CALLBACK httpCameraProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    auto* url = reinterpret_cast<std::string*>(GetWindowLongPtrW(dlg, DWLP_USER));
    switch (msg) {
        case WM_INITDIALOG: {
            SetWindowLongPtrW(dlg, DWLP_USER, lp);
            url = reinterpret_cast<std::string*>(lp);
            HWND edit = GetDlgItem(dlg, IDC_HTTP_CAMERA_URL);
            SendMessageW(edit, EM_LIMITTEXT, static_cast<WPARAM>(kMaxHttpCameraUrl), 0);
            SetWindowTextW(edit, widen(*url).c_str());
            SendMessageW(edit, EM_SETSEL, 0, -1);
            SetFocus(edit);
            return FALSE;  // focus set here
        }
        case WM_COMMAND:
            if (LOWORD(wp) == IDOK) {
                const std::string text = trim(dialogText(dlg, IDC_HTTP_CAMERA_URL));
                HttpUrl parsed;
                std::string why;
                if (!parseHttpUrl(text, parsed, &why)) {
                    MessageBoxW(dlg, widen(why + ".").c_str(), L"frame-perf-hud", MB_OK | MB_ICONWARNING);
                    SetFocus(GetDlgItem(dlg, IDC_HTTP_CAMERA_URL));
                    return TRUE;
                }
                *url = text;
                EndDialog(dlg, IDOK);
                return TRUE;
            }
            if (LOWORD(wp) == IDCANCEL) {
                EndDialog(dlg, IDCANCEL);
                return TRUE;
            }
            break;
    }
    return FALSE;
}

}  // namespace

bool showHttpCameraDialog(HINSTANCE instance, HWND parent, std::string& url) {
    std::string edited = url;
    if (DialogBoxParamW(instance, MAKEINTRESOURCEW(IDD_HTTP_CAMERA), parent, httpCameraProc,
                        reinterpret_cast<LPARAM>(&edited)) != IDOK ||
        edited == url) {
        return false;
    }
    url = edited;
    return true;
}

bool showRenameDialog(HINSTANCE instance, HWND parent, std::string& name) {
    std::string edited = name;
    if (DialogBoxParamW(instance, MAKEINTRESOURCEW(IDD_RENAME), parent, renameProc,
                        reinterpret_cast<LPARAM>(&edited)) != IDOK ||
        edited == name) {
        return false;
    }
    name = edited;
    return true;
}

bool showSettingsDialog(HINSTANCE instance, HWND parent, SettingsDialogData& data) {
    return DialogBoxParamW(instance, MAKEINTRESOURCEW(IDD_SETTINGS), parent, settingsProc,
                           reinterpret_cast<LPARAM>(&data)) == IDOK;
}

}  // namespace pc
}  // namespace fph
