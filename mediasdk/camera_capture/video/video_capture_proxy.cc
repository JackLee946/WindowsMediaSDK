#include "video_capture_proxy.h"

VideoCaptureProxy::VideoCaptureProxy() {}

VideoCaptureProxy::~VideoCaptureProxy() {
    task_thread_.Wait();
}

void VideoCaptureProxy::SetVideoProfile(const VideoProfile& video_profile) {
    // Capture by value: work runs asynchronously; references may dangle after this function returns.
    const VideoProfile video_profile_copy = video_profile;
    task_thread_.PostWork([this, video_profile_copy]() { video_capture_manager_.SetVideoProfile(video_profile_copy); });
}

void VideoCaptureProxy::StartCapture(const std::string& video_device_id) {
    // Capture by value: work runs asynchronously; references may dangle after this function returns.
    const std::string video_device_id_copy = video_device_id;
    task_thread_.PostWork([this, video_device_id_copy]() { video_capture_manager_.StartCapture(video_device_id_copy); });
}

void VideoCaptureProxy::StopCapture() {
    task_thread_.PostWork([this]() { video_capture_manager_.StopCapture(); });
}

void VideoCaptureProxy::StartPreview(void* hwnd) {
    const HWND hwnd_copy = (HWND)hwnd;
    task_thread_.PostWork([this, hwnd_copy]() { video_capture_manager_.StartPreview(hwnd_copy); });
}

void VideoCaptureProxy::StopPreview() {
    task_thread_.PostWork([this]() { video_capture_manager_.StopPreview(); });
}

void VideoCaptureProxy::RegisteVideoFrameObserver(std::shared_ptr<IVideoFrameObserver> observer) {
    video_capture_manager_.RegisteVideoFrameObserver(observer);
}