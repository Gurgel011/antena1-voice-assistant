#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "esp_ae_rate_cvt.h"
#include "esp_audio_dec.h"
#include "esp_aac_dec.h"

class AudioService;

class RadioStreamPlayer {
public:
    explicit RadioStreamPlayer(AudioService* audio_service);
    ~RadioStreamPlayer();

    void Start();
    void Stop();
    bool IsRunning() const { return running_; }

private:
    static constexpr const char* kStreamUrl = "https://antenaone.crossradio.com.br/stream/1;";
    static constexpr int kStreamSampleRate = 48000;
    static constexpr int kOutputSampleRate = 24000;
    static constexpr size_t kHttpReadSize = 8192;
    static constexpr size_t kMaxBufferedBytes = 256 * 1024;
    static constexpr size_t kMaxQueuedPcmChunks = 256;
    static constexpr size_t kStartBufferedChunks = 128;
    static constexpr int kHttpConnectTimeoutMs = 15000;
    static constexpr int kHttpReadTimeoutMs = 5000;
    static constexpr int kMaxConsecutiveReadTimeouts = 3;

    AudioService* audio_service_ = nullptr;
    TaskHandle_t task_handle_ = nullptr;
    TaskHandle_t playback_task_handle_ = nullptr;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    void* aac_decoder_ = nullptr;
    esp_ae_rate_cvt_handle_t resampler_ = nullptr;
    std::vector<uint8_t> input_buffer_;
    std::vector<uint8_t> decode_buffer_;
    std::mutex pcm_queue_mutex_;
    std::condition_variable pcm_queue_cv_;
    std::deque<std::vector<int16_t>> pcm_queue_;
    uint32_t bytes_received_ = 0;
    uint32_t frames_decoded_ = 0;
    uint32_t chunks_dropped_ = 0;
    bool logged_first_sync_ = false;
    bool playback_started_ = false;

    static void TaskEntry(void* arg);
    static void PlaybackTaskEntry(void* arg);
    void TaskLoop();
    void PlaybackLoop();
    void StreamOnce();
    bool EnsureDecoder();
    bool EnsureResampler();
    void ResetStreamState();
    void ProcessBytes(const uint8_t* data, size_t len);
    bool DecodeAdtsFrame(uint8_t* frame, size_t frame_len);
    void OutputDecodedPcm(uint8_t* data, size_t bytes, uint8_t channels, uint32_t sample_rate);
    void QueuePcm(std::vector<int16_t>&& pcm);
    static int FindAdtsSync(const std::vector<uint8_t>& buffer);
    static int GetAdtsFrameLength(const uint8_t* data, size_t len);
};
