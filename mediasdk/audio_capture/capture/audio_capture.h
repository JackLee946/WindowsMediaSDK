#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Raw microphone PCM sample format.
enum class AudioSampleFormat {
    kS16 = 0, // signed 16-bit little-endian interleaved
    kF32 = 1, // float32 little-endian interleaved
};

struct AudioPcmFrame {
    int sample_rate{0};
    int channels{0};
    int bits_per_sample{0};
    AudioSampleFormat format{AudioSampleFormat::kS16};

    // Interleaved PCM bytes.
    std::vector<uint8_t> data{};

    // Timestamp in microseconds (steady clock).
    uint64_t timestamp_us{0};
};

class AudioCapture {
public:
    using PcmCallback = std::function<void(const AudioPcmFrame&)>;

    AudioCapture();
    ~AudioCapture();

    // device_id: WASAPI device id (IMMDevice::GetId). If empty, uses default capture device.
    bool Start(const std::string& device_id, PcmCallback cb);
    void Stop();

    bool IsRunning() const { return running_.load(); }

private:
    void CaptureThread(std::string device_id);

private:
    std::atomic<bool> running_{false};
    std::thread thread_{};

    std::mutex cb_mu_{};
    PcmCallback cb_{};
};
