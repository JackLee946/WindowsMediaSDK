#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "UIlib.h"

#include <rtmp/EasyRTMPAPI.h>
#include <rtmp/EasyTypes.h>

#include "audio_engine.h"
#include "capture/audio_capture.h"
#include "video_capture_engine.h"
#include "video_encoder_factory.h"
#include "video_render_factory.h"
#include "video_device_choose_window.h"

class MainWindow : public DuiLib::CWindowWnd,
                   public DuiLib::INotifyUI,
                   DuiLib::IDialogBuilderCallback {
public:
    MainWindow();
    ~MainWindow();

    void Init();
    void CreateDuiWindow();
    void Show();

private:
    LPCTSTR GetWindowClassName() const override;
    void Notify(DuiLib::TNotifyUI& msg) override;
    LRESULT HandleMessage(UINT uMsg, WPARAM wParam, LPARAM lParam) override;
    LRESULT OnCreate(UINT uMsg, WPARAM wParam, LPARAM lParam);
    LRESULT OnClose(UINT uMsg, WPARAM wParam, LPARAM lParam);
    void OnClick(DuiLib::TNotifyUI& msg);
    DuiLib::CControlUI* CreateControl(LPCTSTR pstrClass) override;

    void StartPush();
    void StopPush();
    void SetStatus(const std::string& status);
    void CreateVideoDeviceChooseWindow();

private:
    // UI
    DuiLib::CPaintManagerUI paint_manager_{};
    HINSTANCE hinstance_{};

    // pipeline state
    std::atomic<bool> pushing_{false};
    std::string url_{};
    int width_{1280};
    int height_{720};
    int fps_{25};

    std::shared_ptr<VideoCaptureEngine> video_capture_engine_{};
    std::shared_ptr<VideoRender> video_render_{};
    std::shared_ptr<VideoEncoder> video_encoder_{};

    std::vector<VideoDeviceInfo> video_devices_{};
    std::string current_device_id_{};
    std::shared_ptr<VideoDeviceWindow> video_device_window_{};
    HWND video_device_hwnd_{};
    // Keep the video frame observer alive. The capture stack stores only weak_ptr internally.
    std::shared_ptr<IVideoFrameObserver> video_frame_observer_{};

    std::thread render_thread_{};
    std::mutex render_mu_{};
    std::condition_variable render_cv_{};
    std::deque<std::shared_ptr<VideoFrame>> render_queue_{};
    std::atomic<bool> render_running_{false};

    // audio
    std::shared_ptr<AudioEngine> audio_engine_{};
    AudioCapture mic_{};

    // RTMP
    Easy_Handle rtmp_handle_{nullptr};
    std::atomic<bool> rtmp_metadata_inited_{false};
    std::thread rtmp_thread_{};
    std::mutex rtmp_mu_{};
    std::condition_variable rtmp_cv_{};
    struct QueuedFrame {
        EASY_AV_Frame frame{};
        std::vector<uint8_t> buffer{};
    };
    std::deque<QueuedFrame> rtmp_queue_{};

    // cached codec config
    std::vector<uint8_t> sps_{};
    std::vector<uint8_t> pps_{};
    std::atomic<bool> have_audio_params_{false};
    std::mutex mi_mu_{};
    EASY_MEDIA_INFO_T media_info_{};

    // audio encoder state
    struct AacEncoderState;
    std::unique_ptr<AacEncoderState> aac_{};
};


