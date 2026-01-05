#include "audio_capture.h"

#include <Windows.h>

#include <Audioclient.h>
#include <Mmdeviceapi.h>

#include <chrono>
#include <cstring>

namespace {

template <class T>
static inline void SafeRelease(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

static bool IsS16Pcm(const WAVEFORMATEX* wf) {
    if (!wf) return false;
    if (wf->wFormatTag == WAVE_FORMAT_PCM) {
        return wf->wBitsPerSample == 16;
    }
    if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE && wf->cbSize >= (sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX))) {
        const auto* wfe = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wf);
        return (wfe->SubFormat == KSDATAFORMAT_SUBTYPE_PCM) && (wf->wBitsPerSample == 16);
    }
    return false;
}

static bool IsF32Float(const WAVEFORMATEX* wf) {
    if (!wf) return false;
    if (wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        return wf->wBitsPerSample == 32;
    }
    if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE && wf->cbSize >= (sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX))) {
        const auto* wfe = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wf);
        return (wfe->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) && (wf->wBitsPerSample == 32);
    }
    return false;
}

static void FillDesiredPcm16(WAVEFORMATEXTENSIBLE& out, int sample_rate, int channels) {
    std::memset(&out, 0, sizeof(out));
    out.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    out.Format.nChannels = (WORD)channels;
    out.Format.nSamplesPerSec = (DWORD)sample_rate;
    out.Format.wBitsPerSample = 16;
    out.Format.nBlockAlign = (WORD)((out.Format.nChannels * out.Format.wBitsPerSample) / 8);
    out.Format.nAvgBytesPerSec = out.Format.nSamplesPerSec * out.Format.nBlockAlign;
    out.Format.cbSize = (WORD)(sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX));
    out.Samples.wValidBitsPerSample = 16;
    out.dwChannelMask = (channels == 1) ? SPEAKER_FRONT_CENTER : (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT);
    out.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
}

static uint64_t NowSteadyUs() {
    using namespace std::chrono;
    return (uint64_t)duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

static std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (len <= 0) return std::wstring();
    std::wstring ws(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &ws[0], len);
    return ws;
}

} // namespace

AudioCapture::AudioCapture() = default;

AudioCapture::~AudioCapture() {
    Stop();
}

bool AudioCapture::Start(const std::string& device_id, PcmCallback cb) {
    if (!cb) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(cb_mu_);
        cb_ = std::move(cb);
    }
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        return true; // already running
    }
    thread_ = std::thread(&AudioCapture::CaptureThread, this, device_id);
    return true;
}

void AudioCapture::Stop() {
    if (!running_.exchange(false)) {
        return;
    }
    if (thread_.joinable()) {
        thread_.join();
    }
    std::lock_guard<std::mutex> lock(cb_mu_);
    cb_ = nullptr;
}

void AudioCapture::CaptureThread(std::string device_id) {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool com_inited = SUCCEEDED(hr);

    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    IAudioClient* audio_client = nullptr;
    IAudioCaptureClient* capture_client = nullptr;
    HANDLE hEvent = nullptr;
    WAVEFORMATEX* mix = nullptr;
    WAVEFORMATEX* fmt = nullptr; // points to selected format (mix/closest/desired)
    WAVEFORMATEX* closest = nullptr;

    auto cleanup = [&]() {
        if (audio_client) {
            audio_client->Stop();
        }
        if (hEvent) {
            CloseHandle(hEvent);
            hEvent = nullptr;
        }
        SafeRelease(capture_client);
        SafeRelease(audio_client);
        SafeRelease(device);
        SafeRelease(enumerator);
        if (closest) {
            CoTaskMemFree(closest);
            closest = nullptr;
        }
        if (mix) {
            CoTaskMemFree(mix);
            mix = nullptr;
        }
        if (com_inited) {
            CoUninitialize();
        }
    };

    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                          (void**)&enumerator);
    if (FAILED(hr) || !enumerator) {
        cleanup();
        return;
    }

    if (device_id.empty()) {
        hr = enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &device);
    } else {
        // device_id expected to be IMMDevice::GetId() string. Some callers may pass UTF-8.
        std::wstring wid = Utf8ToWide(device_id);
        if (wid.empty()) {
            hr = E_INVALIDARG;
        } else {
            hr = enumerator->GetDevice(wid.c_str(), &device);
        }
    }
    if (FAILED(hr) || !device) {
        cleanup();
        return;
    }

    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&audio_client);
    if (FAILED(hr) || !audio_client) {
        cleanup();
        return;
    }

    hr = audio_client->GetMixFormat(&mix);
    if (FAILED(hr) || !mix) {
        cleanup();
        return;
    }

    // Choose input format: prefer mix format if it's f32/s16; otherwise try to request PCM16.
    WAVEFORMATEXTENSIBLE desired16{};
    bool use_desired = false;
    if (IsS16Pcm(mix) || IsF32Float(mix)) {
        fmt = mix;
    } else {
        FillDesiredPcm16(desired16, (int)mix->nSamplesPerSec, (int)mix->nChannels);
        hr = audio_client->IsFormatSupported(AUDCLNT_SHAREMODE_SHARED, (WAVEFORMATEX*)&desired16, &closest);
        if (hr == S_OK) {
            fmt = (WAVEFORMATEX*)&desired16;
            use_desired = true;
        } else if (hr == S_FALSE && closest) {
            fmt = closest;
        } else {
            // Fall back to mix even if it's not ideal; the pipeline may ignore unsupported formats.
            fmt = mix;
        }
    }

    hEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!hEvent) {
        cleanup();
        return;
    }

    const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
    // 100ms buffer in shared mode.
    const REFERENCE_TIME buffer_duration = 1000000; // 100ms in 100-ns units
    hr = audio_client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, buffer_duration, 0, fmt, nullptr);
    if (FAILED(hr)) {
        cleanup();
        return;
    }

    hr = audio_client->SetEventHandle(hEvent);
    if (FAILED(hr)) {
        cleanup();
        return;
    }

    hr = audio_client->GetService(__uuidof(IAudioCaptureClient), (void**)&capture_client);
    if (FAILED(hr) || !capture_client) {
        cleanup();
        return;
    }

    hr = audio_client->Start();
    if (FAILED(hr)) {
        cleanup();
        return;
    }

    const int sample_rate = (int)fmt->nSamplesPerSec;
    const int channels = (int)fmt->nChannels;
    const int bits = (int)fmt->wBitsPerSample;
    const int block_align = (int)fmt->nBlockAlign;
    AudioSampleFormat sample_fmt = AudioSampleFormat::kS16;
    if (IsF32Float(fmt)) {
        sample_fmt = AudioSampleFormat::kF32;
    } else if (IsS16Pcm(fmt)) {
        sample_fmt = AudioSampleFormat::kS16;
    } else {
        // best-effort: keep kS16 and provide raw bytes; encoder path may ignore.
        sample_fmt = AudioSampleFormat::kS16;
    }

    while (running_.load()) {
        DWORD wait = WaitForSingleObject(hEvent, 2000);
        if (!running_.load()) break;
        if (wait != WAIT_OBJECT_0) {
            continue;
        }

        UINT32 packet_len = 0;
        hr = capture_client->GetNextPacketSize(&packet_len);
        if (FAILED(hr)) {
            break;
        }

        while (packet_len > 0 && running_.load()) {
            BYTE* data = nullptr;
            UINT32 num_frames = 0;
            DWORD buf_flags = 0;
            hr = capture_client->GetBuffer(&data, &num_frames, &buf_flags, nullptr, nullptr);
            if (FAILED(hr)) {
                running_.store(false);
                break;
            }

            AudioPcmFrame frame{};
            frame.sample_rate = sample_rate;
            frame.channels = channels;
            frame.bits_per_sample = bits;
            frame.format = sample_fmt;
            frame.timestamp_us = NowSteadyUs();

            const size_t bytes = (size_t)num_frames * (size_t)block_align;
            frame.data.resize(bytes);
            if (buf_flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                std::memset(frame.data.data(), 0, bytes);
            } else if (data && bytes > 0) {
                std::memcpy(frame.data.data(), data, bytes);
            }

            capture_client->ReleaseBuffer(num_frames);

            PcmCallback cb;
            {
                std::lock_guard<std::mutex> lock(cb_mu_);
                cb = cb_;
            }
            if (cb) {
                cb(frame);
            }

            hr = capture_client->GetNextPacketSize(&packet_len);
            if (FAILED(hr)) {
                running_.store(false);
                break;
            }
        }
    }

    cleanup();
}
