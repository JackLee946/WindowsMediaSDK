#include <rtmp/EasyRTMPAPI.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>
#include <cstdarg>
#include "mediasdk/local_log/local_log.h"
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

extern "C" {
#include "amf.h"
#include "log.h"
#include "rtmp.h"
}

namespace {
static const char* kEasyRtmpLogTag = "EasyRtmp";

static void LibrtmpLogCb(int level, const char* fmt, va_list vl) {
    char msg[2048] = {0};
#if defined(_MSC_VER)
    _vsnprintf_s(msg, sizeof(msg), _TRUNCATE, fmt, vl);
#else
    vsnprintf(msg, sizeof(msg), fmt, vl);
#endif
    // librtmp already formats without trailing newline sometimes
    switch (level) {
    case RTMP_LOGCRIT:
    case RTMP_LOGERROR:
        LOGE(kEasyRtmpLogTag) << "[librtmp] " << msg;
        break;
    case RTMP_LOGWARNING:
        LOGW(kEasyRtmpLogTag) << "[librtmp] " << msg;
        break;
    default:
        LOGI(kEasyRtmpLogTag) << "[librtmp] " << msg;
        break;
    }
}

static inline uint32_t ToMs(uint32_t sec, uint32_t usec) {
    return sec * 1000u + (usec / 1000u);
}

static int SampleRateIndex(int sr) {
    // ADTS sampling_frequency_index
    static const int srs[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000,
                              22050, 16000, 12000, 11025, 8000,  7350};
    for (int i = 0; i < (int)(sizeof(srs) / sizeof(srs[0])); ++i) {
        if (srs[i] == sr)
            return i;
    }
    // fallback to 44100
    return 4;
}

static void BuildAudioSpecificConfig(int sample_rate, int channels, std::vector<uint8_t>& out) {
    // AAC-LC
    int profile = 2; // AAC LC (Audio Object Type)
    int sr_idx = SampleRateIndex(sample_rate);
    int ch = channels <= 0 ? 1 : channels;
    uint16_t cfg = (uint16_t)((profile << 11) | (sr_idx << 7) | (ch << 3));
    out.resize(2);
    out[0] = (uint8_t)((cfg >> 8) & 0xFF);
    out[1] = (uint8_t)(cfg & 0xFF);
}

static void PutBE32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)((v >> 24) & 0xFF);
    p[1] = (uint8_t)((v >> 16) & 0xFF);
    p[2] = (uint8_t)((v >> 8) & 0xFF);
    p[3] = (uint8_t)(v & 0xFF);
}

// Simple SPS parser to extract width/height (simplified version)
static bool ParseSpsForResolution(const uint8_t* sps, uint32_t sps_len, uint32_t* width, uint32_t* height) {
    if (!sps || sps_len < 4 || width == nullptr || height == nullptr) {
        return false;
    }
    
    // Skip NAL header if present (0x67 or 0x00 0x00 0x00 0x01 0x67)
    const uint8_t* data = sps;
    uint32_t len = sps_len;
    if (len > 4 && data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1) {
        data += 4;
        len -= 4;
    } else if (len > 3 && data[0] == 0 && data[1] == 0 && data[2] == 1) {
        data += 3;
        len -= 3;
    }
    
    if (len < 5 || (data[0] & 0x1F) != 7) { // SPS NAL type = 7
        return false;
    }
    
    // Simplified parsing: try to extract mb_width and mb_height from SPS
    // This is a very basic implementation - full SPS parsing is complex
    // For now, we'll use a heuristic approach or return false and use defaults
    
    // Try to find common resolution patterns in SPS
    // This is a simplified approach - for production, use a proper SPS parser
    
    // For simplicity, return false and let caller use defaults
    // A proper implementation would use exponential-Golomb decoding
    return false;
}

static bool IsStartCode3(const uint8_t* p) { return p[0] == 0 && p[1] == 0 && p[2] == 1; }
static bool IsStartCode4(const uint8_t* p) { return p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1; }

static void AnnexBToAvcc(const uint8_t* in, size_t in_len, std::vector<uint8_t>& out) {
    out.clear();
    if (!in || in_len < 4) {
        return;
    }
    size_t i = 0;
    while (i + 3 < in_len) {
        // find start code
        size_t sc = (size_t)-1;
        size_t sc_len = 0;
        for (size_t j = i; j + 3 < in_len; ++j) {
            if (j + 4 <= in_len && IsStartCode4(in + j)) {
                sc = j;
                sc_len = 4;
                break;
            }
            if (IsStartCode3(in + j)) {
                sc = j;
                sc_len = 3;
                break;
            }
        }
        if (sc == (size_t)-1) {
            break;
        }
        size_t nal_start = sc + sc_len;
        size_t nal_end = in_len;
        for (size_t j = nal_start; j + 3 < in_len; ++j) {
            if ((j + 4 <= in_len && IsStartCode4(in + j)) || IsStartCode3(in + j)) {
                nal_end = j;
                break;
            }
        }
        if (nal_end > nal_start) {
            uint32_t nalsz = (uint32_t)(nal_end - nal_start);
            size_t old = out.size();
            out.resize(old + 4 + nalsz);
            PutBE32(out.data() + old, nalsz);
            memcpy(out.data() + old + 4, in + nal_start, nalsz);
        }
        i = nal_end;
    }
}

static void StripAdtsIfPresent(std::vector<uint8_t>& aac) {
    if (aac.size() < 7) {
        return;
    }
    // ADTS syncword 0xFFF (12 bits)
    if (aac[0] == 0xFF && (aac[1] & 0xF0) == 0xF0) {
        int protection_absent = aac[1] & 0x01;
        int header_len = protection_absent ? 7 : 9;
        if ((int)aac.size() > header_len) {
            aac.erase(aac.begin(), aac.begin() + header_len);
        }
    }
}

// FLV tag write helpers
static bool RtmpWriteFlvTag(RTMP* r, uint8_t tag_type, uint32_t ts_ms, const uint8_t* payload, uint32_t payload_len) {
    // FLV tag header: TagType(1) DataSize(3) Timestamp(3) TimestampExt(1) StreamID(3=0)
    // IMPORTANT: librtmp's RTMP_Write expects a complete FLV tag (11 + payload + 4) in a single stream.
    // Splitting into multiple RTMP_Write calls breaks its internal FLV tag parsing/state machine.
    const uint32_t total = 11u + payload_len + 4u;
    std::vector<uint8_t> tag;
    tag.resize(total);
    uint8_t* p = tag.data();

    p[0] = tag_type;
    p[1] = (uint8_t)((payload_len >> 16) & 0xFF);
    p[2] = (uint8_t)((payload_len >> 8) & 0xFF);
    p[3] = (uint8_t)(payload_len & 0xFF);
    p[4] = (uint8_t)((ts_ms >> 16) & 0xFF);
    p[5] = (uint8_t)((ts_ms >> 8) & 0xFF);
    p[6] = (uint8_t)(ts_ms & 0xFF);
    p[7] = (uint8_t)((ts_ms >> 24) & 0xFF);
    p[8] = p[9] = p[10] = 0;

    if (payload_len > 0 && payload) {
        memcpy(p + 11, payload, payload_len);
    }

    const uint32_t prev_size = payload_len + 11;
    PutBE32(p + 11 + payload_len, prev_size);

    int ret = RTMP_Write(r, (const char*)tag.data(), (int)tag.size());
    if (ret != (int)tag.size()) {
        LOGE(kEasyRtmpLogTag) << "[RTMP_Write] failed tag_type=" << (int)tag_type
                              << " ts_ms=" << ts_ms
                              << " payload_len=" << payload_len
                              << " ret=" << ret;
        return false;
    }
    return true;
}

static double FlvVideoCodecId(const EASY_MEDIA_INFO_T& mi) {
    // FLV VideoCodecID: AVC(H.264)=7.
    // EasyTypes.h uses its own codec constants; map to FLV ids for metadata.
    if (mi.u32VideoCodec == EASY_SDK_VIDEO_CODEC_H264) return 7.0;
    // H.265 in FLV is non-standard; return 0 to avoid advertising an unknown value.
    return 0.0;
}

static double FlvAudioCodecId(const EASY_MEDIA_INFO_T& mi) {
    // FLV SoundFormat: AAC=10.
    if (mi.u32AudioCodec == EASY_SDK_AUDIO_CODEC_AAC) return 10.0;
    // Unknown/unsupported: 0
    return 0.0;
}

static bool SendOnMetaData(RTMP* r, const EASY_MEDIA_INFO_T& mi, uint32_t ts_ms) {
    // Script tag payload is AMF0 encoded
    uint8_t buf[2048]; // Increased buffer size for more fields
    char* p = (char*)buf;
    char* end = (char*)buf + sizeof(buf);

    AVal name = AVC("onMetaData");
    // String "onMetaData"
    *p++ = AMF_STRING;
    p = AMF_EncodeString(p, end, &name);
    if (!p) return false;

    // Try to parse width/height from SPS
    uint32_t width = 0, height = 0;
    if (mi.u32SpsLength > 0) {
        ParseSpsForResolution(mi.u8Sps, mi.u32SpsLength, &width, &height);
    }

    // ECMA array - count will be updated after adding all fields
    *p++ = AMF_ECMA_ARRAY;
    char* count_pos = p;
    p = AMF_EncodeInt32(p, end, 0); // Placeholder for count
    if (!p) return false;
    int field_count = 0;

    auto put_named_number = [&](const char* k, double v) {
        AVal key = {(char*)k, (int)strlen(k)};
        char* ret = AMF_EncodeNamedNumber(p, end, &key, v);
        if (ret) field_count++;
        return ret;
    };

    // Video fields: only advertise video if SPS/PPS present (avoid servers expecting video on audio-only push)
    const bool has_video_cfg = (mi.u32SpsLength > 0 && mi.u32PpsLength > 0);
    if (has_video_cfg) {
        if (width > 0 && height > 0) {
            p = put_named_number("width", (double)width);
            if (!p) return false;
            p = put_named_number("height", (double)height);
            if (!p) return false;
        } else {
            p = put_named_number("width", 0);
            if (!p) return false;
            p = put_named_number("height", 0);
            if (!p) return false;
        }
        p = put_named_number("framerate", (double)mi.u32VideoFps);
        if (!p) return false;
        const double vcc = FlvVideoCodecId(mi);
        if (vcc > 0.0) {
            p = put_named_number("videocodecid", vcc);
            if (!p) return false;
        }
        if (!p) return false;
    }
    
    // Audio fields
    if (mi.u32AudioCodec > 0) {
        const double acc = FlvAudioCodecId(mi);
        if (acc > 0.0) {
            p = put_named_number("audiocodecid", acc);
            if (!p) return false;
        }
        
        if (mi.u32AudioSamplerate > 0) {
            p = put_named_number("audiosamplerate", (double)mi.u32AudioSamplerate);
            if (!p) return false;
        }
        
        if (mi.u32AudioChannel > 0) {
            p = put_named_number("audiochannels", (double)mi.u32AudioChannel);
            if (!p) return false;
        }
        
        if (mi.u32AudioBitsPerSample > 0) {
            p = put_named_number("audiosamplesize", (double)mi.u32AudioBitsPerSample);
            if (!p) return false;
        }
    }

    // Additional standard FLV metadata fields
    p = put_named_number("videodatarate", 0.0); // Can be calculated if needed
    if (!p) return false;
    p = put_named_number("audiodatarate", 0.0); // Can be calculated if needed
    if (!p) return false;

    // Update count
    AMF_EncodeInt32(count_pos, end, field_count);

    // object end
    *p++ = 0;
    *p++ = 0;
    *p++ = AMF_OBJECT_END;

    uint32_t payload_len = (uint32_t)(p - (char*)buf);
    return RtmpWriteFlvTag(r, 0x12 /*script*/, ts_ms, buf, payload_len);
}

struct EasyRtmpSession {
    EasyRtmpSession() = default;
    ~EasyRtmpSession() = default;

    std::mutex mu;
    RTMP* rtmp{nullptr};
    std::string url{};
    // RTMP_SetupURL() parses in-place and stores pointers into the provided URL buffer.
    // Therefore, the buffer must outlive the RTMP session/connection.
    std::vector<char> url_buf{};

    EasyRTMPCallBack cb{nullptr};
    void* cb_user{nullptr};

    EASY_MEDIA_INFO_T mi{};
    bool mi_set{false};
    bool connected{false};
    bool sent_headers{false};

    std::vector<uint8_t> aac_asc{};
    // RTMP_Write() uses a fixed RTMP channel (0x04) for all FLV tags (audio/video/script),
    // so timestamps must be monotonic (non-decreasing) across ALL tags, not per-stream.
    // Use UINT32_MAX as "unset".
    uint32_t last_ts_ms{UINT32_MAX};
};

static void Notify(EasyRtmpSession* s, EASY_RTMP_STATE_T st) {
    if (s && s->cb) {
        s->cb(EASY_SDK_EVENT_FRAME_FLAG, nullptr, st, s->cb_user);
    }
}

static bool EnsureConnected(EasyRtmpSession* s) {
    if (!s || !s->rtmp) return false;
    if (s->connected) return true;

    Notify(s, EASY_RTMP_STATE_CONNECTING);
    if (!RTMP_Connect(s->rtmp, nullptr) || !RTMP_ConnectStream(s->rtmp, 0)) {
        Notify(s, EASY_RTMP_STATE_CONNECT_FAILED);
        return false;
    }
    s->connected = true;
    Notify(s, EASY_RTMP_STATE_CONNECTED);
    return true;
}

static bool SendHeadersIfNeeded(EasyRtmpSession* s) {
    if (!s || !s->rtmp) return false;
    if (s->sent_headers) return true;
    if (!s->mi_set) return false;

    // Do not send headers with a timestamp lower than already-sent media on the same RTMP channel.
    // Equal timestamps are OK (delta=0).
    uint32_t hdr_ts = (s->last_ts_ms == UINT32_MAX) ? 0u : s->last_ts_ms;

    // send metadata
    if (!SendOnMetaData(s->rtmp, s->mi, hdr_ts)) {
        return false;
    }
    if (s->last_ts_ms == UINT32_MAX || hdr_ts > s->last_ts_ms) s->last_ts_ms = hdr_ts;

    // Build and send H264 AVC sequence header from SPS/PPS (AnnexB NAL units without start codes expected)
    if (s->mi.u32SpsLength > 0 && s->mi.u32PpsLength > 0) {
        const uint8_t* sps = s->mi.u8Sps;
        const uint8_t* pps = s->mi.u8Pps;
        uint32_t sps_len = s->mi.u32SpsLength;
        uint32_t pps_len = s->mi.u32PpsLength;
        if (sps_len > 0 && pps_len > 0) {
            std::vector<uint8_t> payload;
            payload.reserve(5 + 11 + sps_len + pps_len);
            // FLV video header: FrameType(1:key)+CodecID(7:AVC) => 0x17
            payload.push_back(0x17);
            payload.push_back(0x00); // AVC sequence header
            payload.push_back(0x00);
            payload.push_back(0x00);
            payload.push_back(0x00); // composition time

            // AVCDecoderConfigurationRecord
            payload.push_back(0x01); // configurationVersion
            payload.push_back(sps[1]); // AVCProfileIndication
            payload.push_back(sps[2]); // profile_compatibility
            payload.push_back(sps[3]); // AVCLevelIndication
            payload.push_back(0xFF);   // 6 bits reserved + 2 bits lengthSizeMinusOne (3 => 4 bytes)
            payload.push_back(0xE1);   // 3 bits reserved + 5 bits numOfSPS (1)
            payload.push_back((uint8_t)((sps_len >> 8) & 0xFF));
            payload.push_back((uint8_t)(sps_len & 0xFF));
            payload.insert(payload.end(), sps, sps + sps_len);
            payload.push_back(0x01); // numOfPPS
            payload.push_back((uint8_t)((pps_len >> 8) & 0xFF));
            payload.push_back((uint8_t)(pps_len & 0xFF));
            payload.insert(payload.end(), pps, pps + pps_len);

            if (!RtmpWriteFlvTag(s->rtmp, 0x09 /*video*/, hdr_ts, payload.data(), (uint32_t)payload.size())) {
                return false;
            }
            if (s->last_ts_ms == UINT32_MAX || hdr_ts > s->last_ts_ms) s->last_ts_ms = hdr_ts;
        }
    }

    // AAC sequence header
    if (s->mi.u32AudioCodec == EASY_SDK_AUDIO_CODEC_AAC && s->mi.u32AudioSamplerate > 0) {
        BuildAudioSpecificConfig((int)s->mi.u32AudioSamplerate, (int)s->mi.u32AudioChannel, s->aac_asc);
        // FLV audio: SoundFormat(10=AAC)<<4 | SoundRate | SoundSize | SoundType
        uint8_t sound_rate = 3; // 44kHz
        if (s->mi.u32AudioSamplerate <= 11025) sound_rate = 1;
        else if (s->mi.u32AudioSamplerate <= 22050) sound_rate = 2;
        else sound_rate = 3;
        uint8_t sound_size = 1; // 16-bit
        uint8_t sound_type = (s->mi.u32AudioChannel >= 2) ? 1 : 0;
        uint8_t hdr = (uint8_t)((10 << 4) | (sound_rate << 2) | (sound_size << 1) | (sound_type));
        std::vector<uint8_t> payload;
        payload.push_back(hdr);
        payload.push_back(0x00); // AAC sequence header
        payload.insert(payload.end(), s->aac_asc.begin(), s->aac_asc.end());
        if (!RtmpWriteFlvTag(s->rtmp, 0x08 /*audio*/, hdr_ts, payload.data(), (uint32_t)payload.size())) {
            return false;
        }
        if (s->last_ts_ms == UINT32_MAX || hdr_ts > s->last_ts_ms) s->last_ts_ms = hdr_ts;
    }

    s->sent_headers = true;
    Notify(s, EASY_RTMP_STATE_PUSHING);
    return true;
}

} // namespace

extern "C" {

// Initialize Winsock on first use (thread-safe)
static void EnsureWinsockInitialized() {
    static std::once_flag init_flag;
    std::call_once(init_flag, []() {
        WSADATA wsaData;
        WSAStartup(MAKEWORD(2, 2), &wsaData);
    });
}

Easy_Handle Easy_APICALL EasyRTMP_Create(void) {
    EnsureWinsockInitialized();
    // Route librtmp logs into local_log (helps diagnose WriteN/10054 issues).
    RTMP_LogSetCallback(LibrtmpLogCb);
    // Use DEBUG to capture server onStatus / publish rejection reasons when the server closes early (10054/10053).
    RTMP_LogSetLevel(RTMP_LOGDEBUG);
    EasyRtmpSession* s = new EasyRtmpSession();
    memset(&s->mi, 0, sizeof(s->mi));
    return (Easy_Handle)s;
}

Easy_I32 Easy_APICALL EasyRTMP_SetCallback(Easy_Handle handle, EasyRTMPCallBack callback, void* userptr) {
    auto s = (EasyRtmpSession*)handle;
    if (!s) return Easy_BadArgument;
    std::lock_guard<std::mutex> lock(s->mu);
    s->cb = callback;
    s->cb_user = userptr;
    return Easy_NoErr;
}

Easy_I32 Easy_APICALL EasyRTMP_Init(Easy_Handle handle,
                                   const char* url,
                                   EASY_MEDIA_INFO_T* pstruStreamInfo,
                                   Easy_U32 /*bufferKSize*/) {
    auto s = (EasyRtmpSession*)handle;
    if (!s || !url) return Easy_BadArgument;
    std::lock_guard<std::mutex> lock(s->mu);
    s->url = url;
    if (pstruStreamInfo) {
        s->mi = *pstruStreamInfo;
        s->mi_set = true;
    }
    return Easy_NoErr;
}

Easy_I32 Easy_APICALL EasyRTMP_InitMetadata(Easy_Handle handle, EASY_MEDIA_INFO_T* pstruStreamInfo, Easy_U32 /*bufferKSize*/) {
    auto s = (EasyRtmpSession*)handle;
    if (!s || !pstruStreamInfo) return Easy_BadArgument;
    std::lock_guard<std::mutex> lock(s->mu);
    s->mi = *pstruStreamInfo;
    s->mi_set = true;
    // force resend headers (e.g., SPS/PPS updated)
    s->sent_headers = false;
    return Easy_NoErr;
}

Easy_Bool Easy_APICALL EasyRTMP_Connect(Easy_Handle handle, const char* url) {
    auto s = (EasyRtmpSession*)handle;
    if (!s || !url) return 0;
    std::lock_guard<std::mutex> lock(s->mu);
    s->url = url;

    if (s->rtmp) {
        RTMP_Close(s->rtmp);
        RTMP_Free(s->rtmp);
        s->rtmp = nullptr;
    }
    s->rtmp = RTMP_Alloc();
    if (!s->rtmp) return 0;
    RTMP_Init(s->rtmp);

    // RTMP_SetupURL mutates the passed-in URL buffer and stores pointers into it.
    // Keep the buffer on the session object to ensure it stays valid for the connection lifetime.
    s->url_buf.assign(s->url.begin(), s->url.end());
    s->url_buf.push_back('\0');
    if (!RTMP_SetupURL(s->rtmp, s->url_buf.data())) {
        RTMP_Free(s->rtmp);
        s->rtmp = nullptr;
        Notify(s, EASY_RTMP_STATE_CONNECT_FAILED);
        return 0;
    }
    // IMPORTANT: RTMP_SetupURL overwrites r->Link.protocol based on the URL scheme.
    // Call RTMP_EnableWrite AFTER SetupURL so publish mode (RTMP_FEATURE_WRITE) is not lost.
    RTMP_EnableWrite(s->rtmp);
    // Perform actual connect here so "connected" is real and we don't block the send thread
    // during the first media write (which causes queue backlog).
    s->connected = false;
    s->sent_headers = false;

    Notify(s, EASY_RTMP_STATE_CONNECTING);
    if (!RTMP_Connect(s->rtmp, nullptr) || !RTMP_ConnectStream(s->rtmp, 0)) {
        Notify(s, EASY_RTMP_STATE_CONNECT_FAILED);
        RTMP_Close(s->rtmp);
        RTMP_Free(s->rtmp);
        s->rtmp = nullptr;
        return 0;
    }
    s->connected = true;
    Notify(s, EASY_RTMP_STATE_CONNECTED);
    return 1;
}

Easy_U32 Easy_APICALL EasyRTMP_SendPacket(Easy_Handle handle, EASY_AV_Frame* frame) {
    auto s = (EasyRtmpSession*)handle;
    if (!s || !frame || !frame->pBuffer || frame->u32AVFrameLen == 0) return 0;

    std::lock_guard<std::mutex> lock(s->mu);
    if (!s->rtmp) return 0;
    if (!EnsureConnected(s)) return 0;
    if (!SendHeadersIfNeeded(s)) return 0;

    uint32_t ts = ToMs(frame->u32TimestampSec, frame->u32TimestampUsec);

    auto clamp_global_monotonic = [&](uint32_t in_ts) -> uint32_t {
        uint32_t out = in_ts;
        // RTMP_SendPacket uses (packet_ts - last_ts) on the same RTMP channel; avoid underflow.
        // Non-decreasing timestamps are OK (delta=0); only clamp on decreasing.
        if (s->last_ts_ms != UINT32_MAX && out < s->last_ts_ms) {
            out = s->last_ts_ms;
        }
        s->last_ts_ms = out;
        return out;
    };

    if (frame->u32AVFrameFlag == EASY_SDK_VIDEO_FRAME_FLAG) {
        ts = clamp_global_monotonic(ts);
        // Input is expected AnnexB H264. Convert to AVCC for FLV.
        std::vector<uint8_t> avcc;
        AnnexBToAvcc((const uint8_t*)frame->pBuffer, (size_t)frame->u32AVFrameLen, avcc);
        if (avcc.empty()) return 0;

        std::vector<uint8_t> payload;
        payload.reserve(5 + avcc.size());
        uint8_t frametype_codec = (frame->u32AVFrameType == EASY_SDK_VIDEO_FRAME_I) ? 0x17 : 0x27;
        payload.push_back(frametype_codec);
        payload.push_back(0x01); // AVC NALU
        payload.push_back(0x00);
        payload.push_back(0x00);
        payload.push_back(0x00); // composition time
        payload.insert(payload.end(), avcc.begin(), avcc.end());

        if (!RtmpWriteFlvTag(s->rtmp, 0x09, ts, payload.data(), (uint32_t)payload.size())) {
            Notify(s, EASY_RTMP_STATE_ERROR);
            // Stop further writes on a broken connection to avoid WSAENOTSOCK (10038)
            RTMP_Close(s->rtmp);
            s->connected = false;
            return 0;
        }
        return frame->u32AVFrameLen;
    } else if (frame->u32AVFrameFlag == EASY_SDK_AUDIO_FRAME_FLAG) {
        ts = clamp_global_monotonic(ts);
        // Expect AAC raw. If ADTS is present, strip it.
        std::vector<uint8_t> aac((const uint8_t*)frame->pBuffer,
                                 (const uint8_t*)frame->pBuffer + frame->u32AVFrameLen);
        StripAdtsIfPresent(aac);
        if (aac.empty()) return 0;

        uint8_t sound_rate = 3;
        if (s->mi.u32AudioSamplerate <= 11025) sound_rate = 1;
        else if (s->mi.u32AudioSamplerate <= 22050) sound_rate = 2;
        else sound_rate = 3;
        uint8_t sound_size = 1;
        uint8_t sound_type = (s->mi.u32AudioChannel >= 2) ? 1 : 0;
        uint8_t hdr = (uint8_t)((10 << 4) | (sound_rate << 2) | (sound_size << 1) | (sound_type));

        std::vector<uint8_t> payload;
        payload.reserve(2 + aac.size());
        payload.push_back(hdr);
        payload.push_back(0x01); // AAC raw
        payload.insert(payload.end(), aac.begin(), aac.end());

        if (!RtmpWriteFlvTag(s->rtmp, 0x08, ts, payload.data(), (uint32_t)payload.size())) {
            Notify(s, EASY_RTMP_STATE_ERROR);
            RTMP_Close(s->rtmp);
            s->connected = false;
            return 0;
        }
        return frame->u32AVFrameLen;
    }

    return 0;
}

Easy_I32 Easy_APICALL EasyRTMP_GetBufInfo(Easy_Handle /*handle*/, int* usedSize, int* totalSize) {
    if (usedSize) *usedSize = 0;
    if (totalSize) *totalSize = 0;
    return Easy_NoErr;
}

void Easy_APICALL EasyRTMP_Release(Easy_Handle handle) {
    auto s = (EasyRtmpSession*)handle;
    if (!s) return;
    {
        std::lock_guard<std::mutex> lock(s->mu);
        if (s->rtmp) {
            RTMP_Close(s->rtmp);
            RTMP_Free(s->rtmp);
            s->rtmp = nullptr;
        }
    }
    Notify(s, EASY_RTMP_STATE_DISCONNECTED);
    delete s;
}

} // extern "C"


