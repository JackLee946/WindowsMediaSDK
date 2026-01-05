#include "video_device_choose_window.h"

#include "string_utils.h"

VideoDeviceWindow::VideoDeviceWindow() {}

VideoDeviceWindow::~VideoDeviceWindow() {
    DestroyWindow(GetParent(this->GetHWND()));
}

LPCTSTR VideoDeviceWindow::GetWindowClassName() const {
    return _T("DUIVideoDeviceFrame");
}

UINT VideoDeviceWindow::GetClassStyle() const {
    return UI_CLASSSTYLE_DIALOG;
}

void VideoDeviceWindow::Notify(DuiLib::TNotifyUI& msg) {
    auto name = msg.pSender->GetName();
    if (msg.sType == _T("click")) {
        if (name == "btnOk") {
            for (size_t i = 0; i < option_vec_.size(); ++i) {
                auto option = dynamic_cast<DuiLib::COptionUI*>(
                    paint_manager_.FindControl(option_vec_[i].c_str()));
                if (option && option->IsSelected()) {
                    select_id_ = video_devices_[i].device_id;
                    if (callback_) {
                        callback_(select_id_);
                    }
                    break;
                }
            }
            Close();
        }
    }
}

LRESULT VideoDeviceWindow::HandleMessage(UINT uMsg, WPARAM wParam, LPARAM lParam) {
    LRESULT lRes = 0;
    switch (uMsg) {
    case WM_CREATE:
        lRes = OnCreate(uMsg, wParam, lParam);
        break;
    case WM_CLOSE:
        lRes = OnClose(uMsg, wParam, lParam);
        break;
    }
    if (paint_manager_.MessageHandler(uMsg, wParam, lParam, lRes)) {
        return lRes;
    }
    return __super::HandleMessage(uMsg, wParam, lParam);
}

LRESULT VideoDeviceWindow::MessageHandler(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, bool& /*bHandled*/) {
    return 0;
}

LRESULT VideoDeviceWindow::OnCreate(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/) {
    paint_manager_.Init(m_hWnd);
    paint_manager_.AddPreMessageFilter(this);
    DuiLib::CDialogBuilder builder;
    DuiLib::CControlUI* pRoot =
        builder.Create(_T("video_device_window.xml"), (UINT)0, NULL, &paint_manager_);
    paint_manager_.AttachDialog(pRoot);
    paint_manager_.AddNotifier(this);
    InitWindow();
    return 0;
}

LRESULT VideoDeviceWindow::OnClose(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/) {
    return 0;
}

void VideoDeviceWindow::InitWindow() {
    if (video_devices_.empty()) {
        return;
    }

    auto device_window =
        dynamic_cast<DuiLib::CVerticalLayoutUI*>(paint_manager_.FindControl(_T("deviceWindow")));
    if (!device_window) {
        return;
    }
    // Clear previous items (if any) to avoid overlap when reopening.
    device_window->RemoveAll();

    auto to_ui_text_from_utf8 = [](const std::string& s) -> DuiLib::CDuiString {
#ifdef _UNICODE
        // Prefer UTF-8 decoding; fallback to ANSI->Unicode if input isn't valid UTF-8.
        std::wstring ws = utils::Utf8ToUnicode(s);
        if (ws.empty() && !s.empty()) {
            ws = utils::AnsiToUnicode(s);
        }
        return ws.c_str();
#else
        // ANSI build: best-effort convert UTF-8 to ANSI.
        return utils::Utf8ToAnsi(s).c_str();
#endif
    };

    option_vec_.clear();
    for (size_t i = 0; i < video_devices_.size(); ++i) {
        std::string option_name = "camera_" + std::to_string(i);

        // Build a row layout so controls participate in layout (avoid float+fixed XY overlap).
        auto row = new DuiLib::CHorizontalLayoutUI();
        row->SetFixedHeight(34);
        row->SetChildPadding(8);
        row->SetChildVAlign(DT_VCENTER);
        RECT inset = {10, 0, 10, 0};
        row->SetInset(inset);

        DuiLib::COptionUI* option = new DuiLib::COptionUI;
        DuiLib::CLabelUI* label = new DuiLib::CLabelUI;

        // device_name is UTF-8 in mediasdk; DuiLib expects TCHAR (Unicode build).
        label->SetText(to_ui_text_from_utf8(video_devices_[i].device_name));
        option->SetName(option_name.c_str());
        option_vec_.push_back(option_name);

        option->SetFont(0);
        label->SetFont(0);

        option->SetFixedWidth(20);
        option->SetFixedHeight(20);
        label->SetFixedHeight(24);

        option->SetNormalImage(_T("..\\..\\resources\\common\\radio_un.png"));
        option->SetSelectedImage(_T("..\\..\\resources\\common\\radio_sel.png"));
        option->SetGroup(_T("cameraGroup"));

        if ((select_id_.empty() || select_id_ == "auto") && i == 0) {
            option->Selected(true);
        } else if (select_id_ == video_devices_[i].device_id) {
            option->Selected(true);
        }

        row->Add(option);
        row->Add(label);
        device_window->Add(row);
    }
}

void VideoDeviceWindow::SetCurrentVideoDevice(const std::string& device_id) {
    select_id_ = device_id;
}

void VideoDeviceWindow::SetVideoDevies(const std::vector<VideoDeviceInfo>& video_devices) {
    video_devices_.assign(video_devices.begin(), video_devices.end());
}

void VideoDeviceWindow::SetVideoDeviceCallback(VideoDeviceCallback callback) {
    callback_ = std::move(callback);
}


