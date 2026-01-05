#define _CRTDBG_MAP_ALLOC

#include "main_window.h"

#include <VersionHelpers.h>
#include <shellscalingapi.h>

#include <chrono>
#include <functional>
#include <algorithm>
#include <cstring>
#include <iostream>
#include <cwchar>

#include "my_window.h"
#include "string_utils.h"
#include "local_log.h"

namespace {
const char* kRtmpPushLogTag = "RtmpPush";
static std::atomic<uint64_t> g_audio_cb_count{0};
static std::atomic<uint64_t> g_rtmp_send_count{0};
constexpr UINT WM_APP_RTMP_SEND_FAILED = WM_APP + 100;
}

extern "C" {
#include "libavcodec/avcodec.h"
#include "libavutil/audio_fifo.h"
#include "libavutil/channel_layout.h"
#include "libavutil/opt.h"
#include "libavutil/samplefmt.h"
#include "libswresample/swresample.h"
}

#pragma comment(lib, "Shcore.lib")

namespace {

uint64_t NowUsSince(const std::chrono::steady_clock::time_point& start) {
    auto now = std::chrono::steady_clock::now();
    return (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(now - start).count();
}

struct AnnexBNal {
    const uint8_t* data{nullptr};
    size_t size{0};
};

std::vector<AnnexBNal> SplitAnnexB(const uint8_t* data, size_t len) {
    std::vector<AnnexBNal> out;
    if (!data || len < 4) {
        return out;
    }
    auto is_start3 = [&](size_t i) {
        return i + 3 <= len && data[i] == 0x00 && data[i + 1] == 0x00 && data[i + 2] == 0x01;
    };
    auto is_start4 = [&](size_t i) {
        return i + 4 <= len && data[i] == 0x00 && data[i + 1] == 0x00 && data[i + 2] == 0x00 &&
               data[i + 3] == 0x01;
    };

    size_t i = 0;
    while (i + 3 < len) {
        size_t sc = (size_t)-1;
        size_t sc_len = 0;
        for (size_t j = i; j + 3 < len; ++j) {
            if (is_start4(j)) {
                sc = j;
                sc_len = 4;
                break;
            }
            if (is_start3(j)) {
                sc = j;
                sc_len = 3;
                break;
            }
        }
        if (sc == (size_t)-1) {
            break;
        }
        size_t nal_start = sc + sc_len;
        size_t nal_end = len;
        for (size_t j = nal_start; j + 3 < len; ++j) {
            if (is_start4(j) || is_start3(j)) {
                nal_end = j;
                break;
            }
        }
        if (nal_end > nal_start) {
            out.push_back({data + nal_start, nal_end - nal_start});
        }
        i = nal_end;
    }
    return out;
}

bool ExtractH264SpsPps(const uint8_t* data,
                       size_t len,
                       std::vector<uint8_t>& sps,
                       std::vector<uint8_t>& pps,
                       bool& has_idr) {
    has_idr = false;
    auto nals = SplitAnnexB(data, len);
    bool updated = false;
    for (auto& nal : nals) {
        if (nal.size < 1) {
            continue;
        }
        uint8_t nal_type = nal.data[0] & 0x1F;
        if (nal_type == 5) {
            has_idr = true;
        } else if (nal_type == 7) {
            std::vector<uint8_t> cur(nal.data, nal.data + nal.size);
            if (sps != cur) {
                sps = std::move(cur);
                updated = true;
            }
        } else if (nal_type == 8) {
            std::vector<uint8_t> cur(nal.data, nal.data + nal.size);
            if (pps != cur) {
                pps = std::move(cur);
                updated = true;
            }
        }
    }
    return updated;
}

int RtmpStateCallback(int /*_frameType*/, char* /*pBuf*/, EASY_RTMP_STATE_T state, void* /*_userPtr*/) {
    switch (state) {
    case EASY_RTMP_STATE_CONNECTING:
        LOGI(kRtmpPushLogTag) << "[rtmp] connecting...";
        break;
    case EASY_RTMP_STATE_CONNECTED:
        LOGI(kRtmpPushLogTag) << "[rtmp] connected";
        break;
    case EASY_RTMP_STATE_CONNECT_FAILED:
        LOGI(kRtmpPushLogTag) << "[rtmp] connect failed";
        break;
    case EASY_RTMP_STATE_CONNECT_ABORT:
        LOGI(kRtmpPushLogTag) << "[rtmp] connect abort";
        break;
    case EASY_RTMP_STATE_DISCONNECTED:
        LOGI(kRtmpPushLogTag) << "[rtmp] disconnected";
        break;
    default:
        break;
    }
    return 0;
}

template <typename T>
static T* Find(DuiLib::CPaintManagerUI& pm, const char* name) {
    return dynamic_cast<T*>(pm.FindControl(name));
}

struct AacEncoderFFmpeg {
    using EncodedCallback =
        std::function<void(const uint8_t* data, size_t len, uint32_t pts_ms, uint64_t pts_us)>;

    ~AacEncoderFFmpeg() { Uninit(); }

    bool Init(int sample_rate, int channels) {
        if (inited_) {
            return true;
        }
        avcodec_register_all();
        codec_ = avcodec_find_encoder(AV_CODEC_ID_AAC);
        if (!codec_) {
            std::cout << "[aac] encoder not found" << std::endl;
            return false;
        }
        ctx_ = avcodec_alloc_context3(codec_);
        if (!ctx_) {
            return false;
        }
        ctx_->sample_rate = sample_rate;
        ctx_->channels = channels;
        ctx_->channel_layout = channels == 1 ? AV_CH_LAYOUT_MONO : AV_CH_LAYOUT_STEREO;
        ctx_->bit_rate = 64000;
        ctx_->sample_fmt = AV_SAMPLE_FMT_FLTP;
        if (codec_->sample_fmts) {
            ctx_->sample_fmt = codec_->sample_fmts[0];
            for (int i = 0; codec_->sample_fmts[i] != AV_SAMPLE_FMT_NONE; ++i) {
                if (codec_->sample_fmts[i] == AV_SAMPLE_FMT_FLTP) {
                    ctx_->sample_fmt = AV_SAMPLE_FMT_FLTP;
                    break;
                }
                if (codec_->sample_fmts[i] == AV_SAMPLE_FMT_S16) {
                    ctx_->sample_fmt = AV_SAMPLE_FMT_S16;
                }
            }
        }

        AVDictionary* opts = nullptr;
        av_dict_set(&opts, "profile", "aac_low", 0);
        int ret = avcodec_open2(ctx_, codec_, &opts);
        av_dict_free(&opts);
        if (ret < 0) {
            std::cout << "[aac] avcodec_open2 failed: " << ret << std::endl;
            return false;
        }

        frame_size_ = ctx_->frame_size > 0 ? ctx_->frame_size : 1024;
        fifo_ = av_audio_fifo_alloc(ctx_->sample_fmt, ctx_->channels, frame_size_ * 4);
        if (!fifo_) {
            return false;
        }
        inited_ = true;
        return true;
    }

    void RegisterCallback(EncodedCallback cb) { cb_ = std::move(cb); }

    bool PushPcm(const AudioPcmFrame& pcm) {
        if (!inited_) {
            if (!Init(pcm.sample_rate, pcm.channels)) {
                return false;
            }
        }
        if (!ctx_ || !fifo_) {
            return false;
        }

        AVSampleFormat in_fmt = AV_SAMPLE_FMT_NONE;
        if (pcm.format == AudioSampleFormat::kS16) {
            in_fmt = AV_SAMPLE_FMT_S16;
        } else if (pcm.format == AudioSampleFormat::kF32) {
            in_fmt = AV_SAMPLE_FMT_FLT;
        }
        if (in_fmt == AV_SAMPLE_FMT_NONE) {
            return true;
        }

        if (!swr_ || in_fmt != swr_in_fmt_ || pcm.channels != swr_in_channels_ ||
            pcm.sample_rate != swr_in_rate_) {
            if (swr_) {
                swr_free(&swr_);
            }
            swr_in_fmt_ = in_fmt;
            swr_in_channels_ = pcm.channels;
            swr_in_rate_ = pcm.sample_rate;
            swr_ = swr_alloc_set_opts(
                nullptr,
                (int64_t)ctx_->channel_layout,
                ctx_->sample_fmt,
                ctx_->sample_rate,
                (int64_t)(pcm.channels == 1 ? AV_CH_LAYOUT_MONO : AV_CH_LAYOUT_STEREO),
                in_fmt,
                pcm.sample_rate,
                0,
                nullptr);
            if (!swr_ || swr_init(swr_) < 0) {
                std::cout << "[aac] swr_init failed" << std::endl;
                return false;
            }
        }

        const uint8_t* in_data[1] = {pcm.data.data()};
        int bytes_per_frame = (pcm.channels * pcm.bits_per_sample) / 8;
        if (bytes_per_frame <= 0) {
            return true;
        }
        int in_samples = (int)(pcm.data.size() / (size_t)bytes_per_frame);
        if (in_samples <= 0) {
            return true;
        }

        int max_out =
            av_rescale_rnd(swr_get_delay(swr_, pcm.sample_rate) + in_samples, ctx_->sample_rate,
                           pcm.sample_rate, AV_ROUND_UP);
        uint8_t** converted = nullptr;
        int ret = av_samples_alloc_array_and_samples(&converted, nullptr, ctx_->channels, max_out,
                                                     ctx_->sample_fmt, 0);
        if (ret < 0) {
            return false;
        }
        int out_samples = swr_convert(swr_, converted, max_out, in_data, in_samples);
        if (out_samples < 0) {
            av_freep(&converted[0]);
            av_freep(&converted);
            return false;
        }

        if (av_audio_fifo_realloc(fifo_, av_audio_fifo_size(fifo_) + out_samples) < 0) {
            av_freep(&converted[0]);
            av_freep(&converted);
            return false;
        }
        if (av_audio_fifo_write(fifo_, (void**)converted, out_samples) < out_samples) {
            av_freep(&converted[0]);
            av_freep(&converted);
            return false;
        }

        av_freep(&converted[0]);
        av_freep(&converted);

        while (av_audio_fifo_size(fifo_) >= frame_size_) {
            AVFrame* frame = av_frame_alloc();
            if (!frame) {
                return false;
            }
            frame->nb_samples = frame_size_;
            frame->channel_layout = ctx_->channel_layout;
            frame->format = ctx_->sample_fmt;
            frame->sample_rate = ctx_->sample_rate;
            if (av_frame_get_buffer(frame, 0) < 0) {
                av_frame_free(&frame);
                return false;
            }
            if (av_audio_fifo_read(fifo_, (void**)frame->data, frame_size_) < frame_size_) {
                av_frame_free(&frame);
                break;
            }

            AVPacket pkt;
            av_init_packet(&pkt);
            pkt.data = nullptr;
            pkt.size = 0;
            int got = 0;
            int enc_ret = avcodec_encode_audio2(ctx_, &pkt, frame, &got);
            av_frame_free(&frame);
            if (enc_ret < 0) {
                av_packet_unref(&pkt);
                return false;
            }
            if (got && cb_) {
                uint64_t pts_us =
                    (audio_samples_sent_ * 1000000ULL) / (uint64_t)ctx_->sample_rate;
                uint32_t pts_ms = (uint32_t)(pts_us / 1000ULL);
                cb_(pkt.data, (size_t)pkt.size, pts_ms, pts_us);
                audio_samples_sent_ += (uint64_t)frame_size_;
            }
            av_packet_unref(&pkt);
        }

        return true;
    }

    void Uninit() {
        if (fifo_) {
            av_audio_fifo_free(fifo_);
            fifo_ = nullptr;
        }
        if (swr_) {
            swr_free(&swr_);
        }
        if (ctx_) {
            avcodec_close(ctx_);
            av_free(ctx_);
            ctx_ = nullptr;
        }
        codec_ = nullptr;
        inited_ = false;
    }

    bool inited_{false};
    const AVCodec* codec_{nullptr};
    AVCodecContext* ctx_{nullptr};
    SwrContext* swr_{nullptr};
    AVAudioFifo* fifo_{nullptr};
    AVSampleFormat swr_in_fmt_{AV_SAMPLE_FMT_NONE};
    int swr_in_channels_{0};
    int swr_in_rate_{0};
    int frame_size_{1024};
    uint64_t audio_samples_sent_{0};
    EncodedCallback cb_{};
};

} // namespace

struct MainWindow::AacEncoderState {
    AacEncoderFFmpeg enc;
};

MainWindow::MainWindow() {
    video_capture_engine_.reset(new VideoCaptureEngine());
    audio_engine_.reset(new AudioEngine());
    video_encoder_ = VideoEnocderFcatory::Instance().CreateEncoder(kEncodeTypeX264);
    if (video_encoder_) {
        video_encoder_->SetOutputSize((uint32_t)width_, (uint32_t)height_);
    }

    memset(&media_info_, 0, sizeof(media_info_));
    media_info_.u32VideoCodec = EASY_SDK_VIDEO_CODEC_H264;
    media_info_.u32VideoFps = (Easy_U32)fps_;
    media_info_.u32AudioCodec = EASY_SDK_AUDIO_CODEC_AAC;

    aac_.reset(new AacEncoderState());
}

MainWindow::~MainWindow() {
    StopPush();
}

void MainWindow::Init() {
    if (IsWindows8OrGreater()) {
        SetProcessDpiAwareness(PROCESS_PER_MONITOR_DPI_AWARE);
    }
    hinstance_ = GetModuleHandle(0);
    DuiLib::CPaintManagerUI::SetInstance(hinstance_);
    // Use path relative to exe directory (xml folder should be copied there during build)
    DuiLib::CDuiString resource_path = DuiLib::CPaintManagerUI::GetInstancePath();
    resource_path += _T("xml");
    DuiLib::CPaintManagerUI::SetResourcePath(resource_path.GetData());
    
    // Setup logging to exe directory
    char exe_path[MAX_PATH] = {0};
    GetModuleFileNameA(NULL, exe_path, MAX_PATH);
    std::string exe_dir = exe_path;
    size_t last_slash = exe_dir.find_last_of("\\/");
    if (last_slash != std::string::npos) {
        exe_dir = exe_dir.substr(0, last_slash + 1);
    }
    SetLocalLogDir(exe_dir);
    SetLocalLogLevel(kLocalLogLevelInfo);
}

void MainWindow::CreateDuiWindow() {
    Create(NULL, _T("RTMP Push Demo"), UI_WNDSTYLE_FRAME, WS_EX_WINDOWEDGE);
}

void MainWindow::Show() {
    ShowModal();
}

LPCTSTR MainWindow::GetWindowClassName() const {
    return _T("DUIRtmpPushFrame");
}

void MainWindow::Notify(DuiLib::TNotifyUI& msg) {
    if (msg.sType == _T("click")) {
        OnClick(msg);
    }
}

LRESULT MainWindow::HandleMessage(UINT uMsg, WPARAM wParam, LPARAM lParam) {
    LRESULT lRes = 0;
    switch (uMsg) {
    case WM_CREATE:
        lRes = OnCreate(uMsg, wParam, lParam);
        break;
    case WM_CLOSE:
        lRes = OnClose(uMsg, wParam, lParam);
        break;
    case WM_APP_RTMP_SEND_FAILED: {
        // Stop push on UI thread to avoid reconnect storms and further socket errors.
        // Keep ASCII here to avoid source-encoding issues in this file.
        SetStatus("RTMP send failed, stopped.");
        StopPush();
        lRes = 0;
        break;
    }
    default:
        break;
    }
    if (paint_manager_.MessageHandler(uMsg, wParam, lParam, lRes)) {
        return lRes;
    }
    return __super::HandleMessage(uMsg, wParam, lParam);
}

LRESULT MainWindow::OnCreate(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/) {
    paint_manager_.Init(m_hWnd);
    DuiLib::CDialogBuilder builder;
    DuiLib::CControlUI* pRoot =
        builder.Create(_T("main_window.xml"), (UINT)0, this, &paint_manager_);
    paint_manager_.AttachDialog(pRoot);
    paint_manager_.AddNotifier(this);
    CenterWindow();

    auto edit = Find<DuiLib::CEditUI>(paint_manager_, "editUrl");
    if (edit) {
        edit->SetText(_T("rtmp://127.0.0.1/live/stream"));
    }
    SetStatus("就绪");
    
    // Update button states
    auto btnStart = Find<DuiLib::CButtonUI>(paint_manager_, "btnStart");
    if (btnStart) {
        btnStart->SetEnabled(true);
    }
    auto btnStop = Find<DuiLib::CButtonUI>(paint_manager_, "btnStop");
    if (btnStop) {
        btnStop->SetEnabled(false);
    }
    return 0;
}

LRESULT MainWindow::OnClose(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/) {
    StopPush();
    return 0;
}

void MainWindow::OnClick(DuiLib::TNotifyUI& msg) {
    auto name = msg.pSender->GetName();
    if (name == "btnStart") {
        StartPush();
    } else if (name == "btnStop") {
        StopPush();
    } else if (name == "btnChooseCamera") {
        CreateVideoDeviceChooseWindow();
    } else if (name == "btnQuit") {
        Close();
    }
}

DuiLib::CControlUI* MainWindow::CreateControl(LPCTSTR pstrClass) {
    if (_tcscmp(pstrClass, _T("CWndUI")) == 0) {
        CWndUI* wndui = new CWndUI();
        HWND wnd = CreateWindow(_T("STATIC"), _T(""),
                                WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN, 0, 0, 0,
                                0, paint_manager_.GetPaintWindow(), (HMENU)0, NULL, NULL);
        EnableWindow(wnd, FALSE);
        wndui->Attach(wnd);
        wndui->SetEnabled(false);
        return wndui;
    }
    return NULL;
}

void MainWindow::SetStatus(const std::string& status) {
    auto lbl = Find<DuiLib::CLabelUI>(paint_manager_, "lblStatus");
    if (lbl) {
        // Treat status as UTF-8 to avoid codepage-dependent mojibake.
#ifdef _UNICODE
        std::wstring wstatus = utils::Utf8ToUnicode(status);
        lbl->SetText(wstatus.empty() ? _T("") : (LPCTSTR)wstatus.c_str());
#else
        lbl->SetText(status.c_str());
#endif
    }
}

void MainWindow::StartPush() {
    LOGI(kRtmpPushLogTag) << "[StartPush] Entry";
    try {
    LOGI(kRtmpPushLogTag) << "[StartPush] Check pushing flag";
    if (pushing_.exchange(true)) {
        LOGI(kRtmpPushLogTag) << "[StartPush] Already pushing, return";
        return;
    }

    LOGI(kRtmpPushLogTag) << "[StartPush] Find edit control";
    auto edit = Find<DuiLib::CEditUI>(paint_manager_, "editUrl");
    if (!edit) {
        LOGI(kRtmpPushLogTag) << "[StartPush] Edit control not found";
        pushing_ = false;
        return;
    }
    LOGI(kRtmpPushLogTag) << "[StartPush] Get URL text";
    DuiLib::CDuiString url_str = edit->GetText();
#ifdef _UNICODE
    url_ = utils::UnicodeToAnsi(url_str.GetData());
#else
    url_ = std::string(url_str.GetData());
#endif
    LOGI(kRtmpPushLogTag) << "[StartPush] URL: " << url_;
    if (url_.empty()) {
        LOGI(kRtmpPushLogTag) << "[StartPush] URL is empty";
        SetStatus(u8"RTMP地址为空");
        pushing_ = false;
        return;
    }

    LOGI(kRtmpPushLogTag) << "[StartPush] Check video_encoder_";
    if (!video_encoder_) {
        LOGI(kRtmpPushLogTag) << "[StartPush] Video encoder is null";
        SetStatus("Video encoder init failed");
        pushing_ = false;
        return;
    }
    LOGI(kRtmpPushLogTag) << "[StartPush] Check aac_";
    if (!aac_) {
        LOGI(kRtmpPushLogTag) << "[StartPush] AAC encoder is null";
        SetStatus("Audio encoder init failed");
        pushing_ = false;
        return;
    }

    LOGI(kRtmpPushLogTag) << "[StartPush] Set status: starting";
    SetStatus(u8"正在启动...");
    
    // Update button states
    {
        auto btnStart = Find<DuiLib::CButtonUI>(paint_manager_, "btnStart");
        if (btnStart) {
            btnStart->SetEnabled(false);
        }
        auto btnStop = Find<DuiLib::CButtonUI>(paint_manager_, "btnStop");
        if (btnStop) {
            btnStop->SetEnabled(false);
        }
    }

    // RTMP connect
    LOGI(kRtmpPushLogTag) << "[StartPush] Create RTMP handle";
    Easy_Handle h = EasyRTMP_Create();
    if (!h) {
        LOGI(kRtmpPushLogTag) << "[StartPush] EasyRTMP_Create failed";
        SetStatus(u8"EasyRTMP_Create 失败");
        pushing_ = false;
        // Re-enable start button
        {
            auto btnStart = Find<DuiLib::CButtonUI>(paint_manager_, "btnStart");
            if (btnStart) {
                btnStart->SetEnabled(true);
            }
        }
        return;
    }
    LOGI(kRtmpPushLogTag) << "[StartPush] Store RTMP handle";
    {
        std::lock_guard<std::mutex> lock(rtmp_mu_);
        rtmp_handle_ = h;
    }
    LOGI(kRtmpPushLogTag) << "[StartPush] Set RTMP callback";
    EasyRTMP_SetCallback(h, RtmpStateCallback, nullptr);
    LOGI(kRtmpPushLogTag) << "[StartPush] Connect RTMP";
    if (!EasyRTMP_Connect(h, url_.c_str())) {
        LOGI(kRtmpPushLogTag) << "[StartPush] EasyRTMP_Connect failed";
        SetStatus(u8"RTMP连接失败");
        {
            std::lock_guard<std::mutex> lock(rtmp_mu_);
            rtmp_handle_ = nullptr;
        }
        EasyRTMP_Release(h);
        pushing_ = false;
        // Re-enable start button
        {
            auto btnStart = Find<DuiLib::CButtonUI>(paint_manager_, "btnStart");
            if (btnStart) {
                btnStart->SetEnabled(true);
            }
        }
        return;
    }

    LOGI(kRtmpPushLogTag) << "[StartPush] RTMP connected";
    rtmp_metadata_inited_ = false;

    // send thread
    LOGI(kRtmpPushLogTag) << "[StartPush] Create RTMP send thread";
    rtmp_thread_ = std::thread([this]() {
        LOGI(kRtmpPushLogTag) << "[RTMP Thread] Started";
        while (pushing_.load()) {
            // Wait until metadata is initialized (SPS/PPS ready) before draining the queue.
            // This prevents silently dropping frames and makes startup behavior deterministic.
            if (!rtmp_metadata_inited_.load()) {
                std::unique_lock<std::mutex> lock(rtmp_mu_);
                rtmp_cv_.wait_for(lock, std::chrono::milliseconds(50),
                                  [&]() { return !pushing_.load() || rtmp_metadata_inited_.load(); });
                continue;
            }
            QueuedFrame q;
            {
                std::unique_lock<std::mutex> lock(rtmp_mu_);
                rtmp_cv_.wait(lock, [&]() { return !pushing_.load() || !rtmp_queue_.empty(); });
                if (!pushing_.load() && rtmp_queue_.empty()) {
                    break;
                }
                q = std::move(rtmp_queue_.front());
                rtmp_queue_.pop_front();
            }
            Easy_Handle h = nullptr;
            {
                std::lock_guard<std::mutex> lock(rtmp_mu_);
                h = rtmp_handle_;
            }
            if (!h) {
                LOGI(kRtmpPushLogTag) << "[RTMP Thread] Handle is null, skipping";
                continue;
            }
            q.frame.pBuffer = q.buffer.data();
            q.frame.u32AVFrameLen = (Easy_U32)q.buffer.size();
            const uint64_t sn = ++g_rtmp_send_count;
            if (sn <= 3 || (sn % 200) == 0) {
                LOGI(kRtmpPushLogTag) << "[RTMP Thread] Sending packet (throttled), type="
                                      << (q.frame.u32AVFrameFlag == EASY_SDK_VIDEO_FRAME_FLAG ? "VIDEO" : "AUDIO")
                                      << ", len=" << q.buffer.size() << ", send_count=" << sn;
            }
            Easy_U32 sent = EasyRTMP_SendPacket(h, &q.frame);
            if (sent == 0) {
                LOGE(kRtmpPushLogTag) << "[RTMP Thread] SendPacket failed; stopping push to avoid reconnect storm";
                // Notify UI thread to StopPush() (do NOT call StopPush here to avoid deadlock).
                if (m_hWnd) {
                    ::PostMessage(m_hWnd, WM_APP_RTMP_SEND_FAILED, 0, 0);
                }
                break;
            }
        }
    });

    // preview render thread (optional)
    LOGI(kRtmpPushLogTag) << "[StartPush] Create render thread";
    render_running_ = true;
    render_thread_ = std::thread([this]() {
        LOGI(kRtmpPushLogTag) << "[Render Thread] Started";
        while (render_running_.load()) {
            std::shared_ptr<VideoFrame> frame;
            {
                std::unique_lock<std::mutex> lock(render_mu_);
                if (render_queue_.empty()) {
                    render_cv_.wait(lock);
                }
                if (!render_running_.load()) {
                    return;
                }
                if (render_queue_.empty()) {
                    continue;
                }
                frame = render_queue_.front();
                render_queue_.pop_front();
            }
            if (!frame || !video_render_) {
                continue;
            }
            uint32_t w = frame->GetWidth();
            uint32_t h = frame->GetHeight();
            uint8_t* y = frame->GetData();
            uint8_t* u = y + w * h;
            uint8_t* v = y + w * h * 5 / 4;
            video_render_->RendFrameI420(y, w, u, w / 2, v, w / 2, w, h);
        }
    });

    // init render window handle (temporarily disabled to isolate crash)
    // auto wnd = (CWndUI*)(paint_manager_.FindControl("renderWindow"));
    // if (wnd) {
    //     wnd->SetEnabled(false);
    //     wnd->SetVisible(true);
    //     wnd->SetPos({0, 0, 960, 540});
    //     ::ShowWindow(wnd->GetHwnd(), true);
    //     try {
    //         video_render_ = VideoRenderFactory::CreateInstance()->CreateVideoRender(kRenderTypeOpenGL);
    //         if (video_render_) {
    //             video_render_->SetWindow(wnd->GetHwnd());
    //         }
    //     } catch (...) {
    //         // Render creation failed, continue without preview
    //         video_render_.reset();
    //     }
    // }

    // init encoder callback
    LOGI(kRtmpPushLogTag) << "[StartPush] Setup video encoder callback";
    auto start_ts = std::chrono::steady_clock::now();
    auto frame_idx = std::make_shared<std::atomic<uint64_t>>(0);
    // Video timestamp base (set once when metadata becomes ready) so first sent video frame starts at 0ms.
    auto video_base_us = std::make_shared<std::atomic<uint64_t>>(UINT64_MAX);
    LOGI(kRtmpPushLogTag) << "[StartPush] Set video encoder output size";
    video_encoder_->SetOutputSize((uint32_t)width_, (uint32_t)height_);
    LOGI(kRtmpPushLogTag) << "[StartPush] Register video encoder callback";
    video_encoder_->RegisterEncodeCalback([this, start_ts, frame_idx, video_base_us](uint8_t* data,
                                                                      uint32_t len) mutable {
        LOGI(kRtmpPushLogTag) << "[Video Encoder Callback] Entry";
        if (!pushing_.load()) {
            return;
        }
        if (!data || len == 0) {
            return;
        }
        bool has_idr = false;
        bool updated = ExtractH264SpsPps(data, len, sps_, pps_, has_idr);
        if (updated) {
            std::lock_guard<std::mutex> lock(mi_mu_);
            media_info_.u32SpsLength = (Easy_U32)std::min<size_t>(sps_.size(), sizeof(media_info_.u8Sps));
            media_info_.u32PpsLength = (Easy_U32)std::min<size_t>(pps_.size(), sizeof(media_info_.u8Pps));
            memset(media_info_.u8Sps, 0, sizeof(media_info_.u8Sps));
            memset(media_info_.u8Pps, 0, sizeof(media_info_.u8Pps));
            memcpy(media_info_.u8Sps, sps_.data(), media_info_.u32SpsLength);
            memcpy(media_info_.u8Pps, pps_.data(), media_info_.u32PpsLength);
        }

        if (!rtmp_metadata_inited_.load() && updated && have_audio_params_.load()) {
            EASY_MEDIA_INFO_T mi_copy{};
            {
                std::lock_guard<std::mutex> lock(mi_mu_);
                mi_copy = media_info_;
            }
            if (mi_copy.u32SpsLength > 0 && mi_copy.u32PpsLength > 0) {
                Easy_Handle h = nullptr;
                {
                    std::lock_guard<std::mutex> lock(rtmp_mu_);
                    h = rtmp_handle_;
                }
                if (!h) {
                    return;
                }
                LOGI(kRtmpPushLogTag) << "[Video Encoder Callback] Initializing metadata, SPS len=" << mi_copy.u32SpsLength << ", PPS len=" << mi_copy.u32PpsLength;
                EasyRTMP_InitMetadata(h, &mi_copy, 1024);
                rtmp_metadata_inited_ = true;
                LOGI(kRtmpPushLogTag) << "[Video Encoder Callback] Metadata initialized";
                // Wake RTMP thread that is waiting for metadata.
                rtmp_cv_.notify_all();
            }
        }

        // Do not enqueue video frames until metadata is ready; otherwise we may send pre-metadata frames
        // with timestamps that don't match the audio timeline, causing RTMP server disconnects.
        if (!rtmp_metadata_inited_.load()) {
            return;
        }

        // IMPORTANT: Use wall-clock elapsed time since StartPush for VIDEO timestamps.
        // Also apply a base so the first *sent* video frame starts at 0ms.
        (void)(*frame_idx)++; // keep counter (debug/metrics), but don't base timestamps on it.
        uint64_t pts_us_raw = NowUsSince(start_ts);
        uint64_t base = video_base_us->load();
        if (base == UINT64_MAX) {
            video_base_us->store(pts_us_raw);
            base = pts_us_raw;
        }
        uint64_t pts_us = (pts_us_raw >= base) ? (pts_us_raw - base) : 0;
        uint32_t pts_ms = (uint32_t)(pts_us / 1000ULL);

        EASY_AV_Frame f{};
        memset(&f, 0, sizeof(f));
        f.u32AVFrameFlag = EASY_SDK_VIDEO_FRAME_FLAG;
        f.u32AVFrameType = has_idr ? EASY_SDK_VIDEO_FRAME_I : EASY_SDK_VIDEO_FRAME_P;
        f.u32PTS = pts_ms;
        f.u32TimestampSec = (Easy_U32)(pts_us / 1000000ULL);
        f.u32TimestampUsec = (Easy_U32)(pts_us % 1000000ULL);

        QueuedFrame q;
        q.frame = f;
        q.buffer.assign(data, data + len);
        {
            std::lock_guard<std::mutex> lock(rtmp_mu_);
            rtmp_queue_.push_back(std::move(q));
            LOGI(kRtmpPushLogTag) << "[Video Encoder Callback] Queued video frame, len=" << len << ", queue size=" << rtmp_queue_.size();
        }
        rtmp_cv_.notify_one();
    });

    // audio encoder callback
    LOGI(kRtmpPushLogTag) << "[StartPush] Register audio encoder callback";
    aac_->enc.RegisterCallback([this](const uint8_t* data, size_t len, uint32_t pts_ms, uint64_t pts_us) {
        const uint64_t n = ++g_audio_cb_count;
        if (n <= 3 || (n % 200) == 0) {
            LOGI(kRtmpPushLogTag) << "[Audio Encoder Callback] Entry (throttled), count=" << n;
        }
        if (!pushing_.load()) {
            return;
        }
        if (!data || len == 0) {
            return;
        }
        // Don't enqueue audio until RTMP metadata (and headers) are initialized from SPS/PPS.
        if (!rtmp_metadata_inited_.load()) {
            return;
        }
        EASY_AV_Frame f{};
        memset(&f, 0, sizeof(f));
        f.u32AVFrameFlag = EASY_SDK_AUDIO_FRAME_FLAG;
        f.u32AVFrameType = EASY_SDK_AUDIO_CODEC_AAC;
        f.u32PTS = pts_ms;
        f.u32TimestampSec = (Easy_U32)(pts_us / 1000000ULL);
        f.u32TimestampUsec = (Easy_U32)(pts_us % 1000000ULL);

        QueuedFrame q;
        q.frame = f;
        q.buffer.assign(data, data + len);
        {
            std::lock_guard<std::mutex> lock(rtmp_mu_);
            rtmp_queue_.push_back(std::move(q));
            if (n <= 3 || (n % 200) == 0) {
                LOGI(kRtmpPushLogTag) << "[Audio Encoder Callback] Queued audio frame (throttled), len=" << len
                                      << ", queue size=" << rtmp_queue_.size();
            }
        }
        rtmp_cv_.notify_one();
    });

    // start capture
    LOGI(kRtmpPushLogTag) << "[StartPush] Enumerate video devices";
    if (video_devices_.empty()) {
        video_devices_ = video_capture_engine_->EnumVideoDevices();
    }
    if (video_devices_.empty()) {
        LOGI(kRtmpPushLogTag) << "[StartPush] No video devices found";
        SetStatus("未找到摄像头设备");
        StopPush();
        // Re-enable start button
        {
            auto btnStart = Find<DuiLib::CButtonUI>(paint_manager_, "btnStart");
            if (btnStart) {
                btnStart->SetEnabled(true);
            }
        }
        return;
    }
    LOGI(kRtmpPushLogTag) << "[StartPush] Found " << video_devices_.size() << " video devices";
    if (current_device_id_.empty()) {
        current_device_id_ = video_devices_[0].device_id;
    }
    std::string device_id = current_device_id_;
    LOGI(kRtmpPushLogTag) << "[StartPush] Using device: " << device_id;

    // inline observer to get frames
    LOGI(kRtmpPushLogTag) << "[StartPush] Create FrameObserver";
    struct FrameObserver : public IVideoFrameObserver {
        MainWindow* self;
        explicit FrameObserver(MainWindow* s) : self(s) {}
        void OnVideoError(int error_code, const std::string& device_name) override {
            LOGI(kRtmpPushLogTag) << "[FrameObserver] OnVideoError: error_code=" << error_code << ", device=" << utils::Utf8ToAscii(device_name);
        }
        void OnVideoFrame(std::shared_ptr<VideoFrame> vf) override {
            if (!vf) {
                LOGI(kRtmpPushLogTag) << "[FrameObserver] OnVideoFrame received null frame";
                return;
            }
            static std::atomic<uint64_t> frame_count{0};
            uint64_t count = frame_count++;
            if (count < 5 || count % 30 == 0) { // Log first 5 frames and then every 30 frames
                LOGI(kRtmpPushLogTag) << "[FrameObserver] OnVideoFrame received, frame_count=" << count << ", width=" << vf->GetWidth() << ", height=" << vf->GetHeight();
            }
            if (!self || !self->pushing_.load()) {
                if (count < 5) {
                    LOGI(kRtmpPushLogTag) << "[FrameObserver] OnVideoFrame: self is null or not pushing, skipping";
                }
                return;
            }
            // preview
            if (self->video_render_) {
                std::lock_guard<std::mutex> lock(self->render_mu_);
                self->render_queue_.push_back(vf);
                self->render_cv_.notify_all();
            }
            // encode (request keyframe about every 2s)
            if (!self->video_encoder_) {
                if (count < 5) {
                    LOGI(kRtmpPushLogTag) << "[FrameObserver] OnVideoFrame: video_encoder_ is null, skipping encode";
                }
                return;
            }
            static std::atomic<uint64_t> idx{0};
            uint64_t i = idx++;
            bool key = (self->fps_ > 0) ? (i % (uint64_t)(self->fps_ * 2) == 0) : (i % 50 == 0);
            if (count < 5 || key) {
                LOGI(kRtmpPushLogTag) << "[FrameObserver] OnVideoFrame: calling EncodeFrame, key=" << (key ? "true" : "false") << ", encode_idx=" << i;
            }
            self->video_encoder_->EncodeFrame(vf, key);
        }
    };

    LOGI(kRtmpPushLogTag) << "[StartPush] Create frame observer shared_ptr";
    video_frame_observer_ = std::make_shared<FrameObserver>(this);
    LOGI(kRtmpPushLogTag) << "[StartPush] Register video frame observer";
    video_capture_engine_->RegisteVideoFrameObserver(video_frame_observer_);
    LOGI(kRtmpPushLogTag) << "[StartPush] Set video profile";
    video_capture_engine_->SetVideoProfile({(uint32_t)width_, (uint32_t)height_, (uint32_t)fps_});
    LOGI(kRtmpPushLogTag) << "[StartPush] Start video capture";
    video_capture_engine_->StartCapture(device_id);
    LOGI(kRtmpPushLogTag) << "[StartPush] Video capture started";

    // start mic
    LOGI(kRtmpPushLogTag) << "[StartPush] Start microphone capture";
    if (!mic_.Start("", [this](const AudioPcmFrame& pcm) {
        LOGI(kRtmpPushLogTag) << "[Audio Capture Callback] Entry";
        if (!pushing_.load()) {
            return;
        }
        if (!have_audio_params_.load()) {
            have_audio_params_ = true;
            {
                std::lock_guard<std::mutex> lock(mi_mu_);
                media_info_.u32AudioSamplerate = (Easy_U32)pcm.sample_rate;
                media_info_.u32AudioChannel = (Easy_U32)pcm.channels;
                media_info_.u32AudioBitsPerSample = (Easy_U32)pcm.bits_per_sample;
            }
            // if SPS/PPS already ready, init metadata now
            EASY_MEDIA_INFO_T mi_copy{};
            {
                std::lock_guard<std::mutex> lock(mi_mu_);
                mi_copy = media_info_;
            }
            // Allow metadata init with audio params only (audio-only stream is supported)
                // Wait for video SPS/PPS before initializing metadata to satisfy servers that require video.
                if (mi_copy.u32AudioSamplerate > 0 && mi_copy.u32SpsLength > 0 && mi_copy.u32PpsLength > 0 &&
                    !rtmp_metadata_inited_.load()) {
                Easy_Handle h = nullptr;
                {
                    std::lock_guard<std::mutex> lock(rtmp_mu_);
                    h = rtmp_handle_;
                }
                if (h) {
                    LOGI(kRtmpPushLogTag) << "[Audio Capture Callback] Initializing metadata, SPS len=" << mi_copy.u32SpsLength << ", PPS len=" << mi_copy.u32PpsLength << ", audio sample rate=" << mi_copy.u32AudioSamplerate;
                    EasyRTMP_InitMetadata(h, &mi_copy, 1024);
                    rtmp_metadata_inited_ = true;
                    LOGI(kRtmpPushLogTag) << "[Audio Capture Callback] Metadata initialized";
                }
            }
        }
        // IMPORTANT: Do not feed PCM into AAC encoder until metadata is ready.
        // Otherwise AAC PTS accumulates while packets are dropped, and the first sent audio timestamp
        // becomes much larger than video, triggering RTMP timestamp underflow and server disconnect.
        if (rtmp_metadata_inited_.load() && aac_) {
            (void)aac_->enc.PushPcm(pcm);
        }
    })) {
        LOGI(kRtmpPushLogTag) << "[StartPush] Microphone start failed";
        SetStatus("Microphone start failed");
        // Clean up before calling StopPush
        video_capture_engine_->StopCapture();
        render_running_ = false;
        render_cv_.notify_all();
        if (render_thread_.joinable()) {
            render_thread_.join();
        }
        video_render_.reset();
        rtmp_cv_.notify_all();
        if (rtmp_thread_.joinable()) {
            rtmp_thread_.join();
        }
        {
            std::lock_guard<std::mutex> lock(rtmp_mu_);
            rtmp_queue_.clear();
        }
        Easy_Handle h = nullptr;
        {
            std::lock_guard<std::mutex> lock(rtmp_mu_);
            h = rtmp_handle_;
            rtmp_handle_ = nullptr;
        }
        if (h) {
            EasyRTMP_Release(h);
        }
        pushing_ = false;
        // Re-enable start button
        {
            auto btnStart = Find<DuiLib::CButtonUI>(paint_manager_, "btnStart");
            if (btnStart) {
                btnStart->SetEnabled(true);
            }
        }
        return;
    }

    LOGI(kRtmpPushLogTag) << "[StartPush] Set status: pushing";
    SetStatus(u8"正在推流...");
    
    // Update button states
    LOGI(kRtmpPushLogTag) << "[StartPush] Update button states";
    {
        auto btnStart = Find<DuiLib::CButtonUI>(paint_manager_, "btnStart");
        if (btnStart) {
            btnStart->SetEnabled(false);
        }
        auto btnStop = Find<DuiLib::CButtonUI>(paint_manager_, "btnStop");
        if (btnStop) {
            btnStop->SetEnabled(true);
        }
    }
    LOGI(kRtmpPushLogTag) << "[StartPush] Successfully completed";
    } catch (const std::exception& e) {
        LOGI(kRtmpPushLogTag) << "[StartPush] Exception caught: " << e.what();
        SetStatus("Start push exception");
        StopPush();
    } catch (...) {
        LOGI(kRtmpPushLogTag) << "[StartPush] Unknown exception caught";
        // never crash the UI thread
        SetStatus("Start push exception");
        StopPush();
    }
}

void MainWindow::CreateVideoDeviceChooseWindow() {
    if (pushing_.load()) {
        SetStatus(u8"请先停止推流再切换摄像头");
        return;
    }
    if (video_devices_.empty()) {
        video_devices_ = video_capture_engine_->EnumVideoDevices();
    }
    if (video_devices_.empty()) {
        SetStatus(u8"未找到摄像头设备");
        return;
    }
    if (video_device_window_ && IsWindow(video_device_hwnd_)) {
        return;
    }
    int count = (int)video_devices_.size();
    int width = 600;
    int height = count * 50 + 70;
    video_device_window_.reset(new VideoDeviceWindow());
    video_device_window_->SetVideoDeviceCallback(
        [&](const std::string& device_id) { current_device_id_ = device_id; });
    video_device_window_->SetCurrentVideoDevice(current_device_id_);
    video_device_window_->SetVideoDevies(video_devices_);
    video_device_hwnd_ = video_device_window_->Create(m_hWnd, _T("VideoDeviceWindow"),
                                                      UI_WNDSTYLE_DIALOG, 0, 0, 0, 0, 0, NULL);
    video_device_window_->ResizeClient(width, height);
    video_device_window_->CenterWindow();
    this->ShowWindow(true);
}

void MainWindow::StopPush() {
    if (!pushing_.exchange(false)) {
        return;
    }

    SetStatus(u8"正在停止...");

    // stop capture/audio
    video_capture_engine_->StopCapture();
    mic_.Stop();
    video_frame_observer_.reset();

    // stop render
    render_running_ = false;
    render_cv_.notify_all();
    if (render_thread_.joinable()) {
        render_thread_.join();
    }
    video_render_.reset();

    // stop rtmp send thread
    rtmp_cv_.notify_all();
    if (rtmp_thread_.joinable()) {
        rtmp_thread_.join();
    }
    {
        std::lock_guard<std::mutex> lock(rtmp_mu_);
        rtmp_queue_.clear();
    }

    Easy_Handle h = nullptr;
    {
        std::lock_guard<std::mutex> lock(rtmp_mu_);
        h = rtmp_handle_;
        rtmp_handle_ = nullptr;
    }
    if (h) {
        EasyRTMP_Release(h);
    }
    rtmp_metadata_inited_ = false;
    have_audio_params_ = false;
    sps_.clear();
    pps_.clear();

    const char* status_stopped = "\xe5\xb7\xb2\xe5\x81\x9c\xe6\xad\xa2"; // UTF-8 encoded "已停止"
    SetStatus(status_stopped);
    
    // Update button states
    {
        auto btnStart = Find<DuiLib::CButtonUI>(paint_manager_, "btnStart");
        if (btnStart) {
            btnStart->SetEnabled(true);
        }
        auto btnStop = Find<DuiLib::CButtonUI>(paint_manager_, "btnStop");
        if (btnStop) {
            btnStop->SetEnabled(false);
        }
    }
}



