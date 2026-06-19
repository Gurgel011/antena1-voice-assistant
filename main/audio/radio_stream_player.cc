#include "radio_stream_player.h"

#include <algorithm>
#include <cinttypes>
#include <cstring>

#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include "audio_service.h"

#define TAG "RadioStreamPlayer"

RadioStreamPlayer::RadioStreamPlayer(AudioService* audio_service) : audio_service_(audio_service) {
}

RadioStreamPlayer::~RadioStreamPlayer() {
    Stop();
    if (aac_decoder_) {
        esp_aac_dec_close(aac_decoder_);
        aac_decoder_ = nullptr;
    }
    if (resampler_) {
        esp_ae_rate_cvt_close(resampler_);
        resampler_ = nullptr;
    }
}

void RadioStreamPlayer::Start() {
    if (running_.exchange(true)) {
        return;
    }
    stop_requested_ = false;
    playback_started_ = false;
    chunks_dropped_ = 0;
    xTaskCreate([](void* arg) {
        PlaybackTaskEntry(arg);
    }, "radio_playback", 4096, this, 4, &playback_task_handle_);
    xTaskCreate([](void* arg) {
        TaskEntry(arg);
    }, "radio_stream", 8192, this, 3, &task_handle_);
}

void RadioStreamPlayer::Stop() {
    if (!running_) {
        return;
    }
    stop_requested_ = true;
    pcm_queue_cv_.notify_all();
    while (task_handle_ != nullptr) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    while (playback_task_handle_ != nullptr) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    {
        std::lock_guard<std::mutex> lock(pcm_queue_mutex_);
        pcm_queue_.clear();
    }
}

void RadioStreamPlayer::TaskEntry(void* arg) {
    auto* player = static_cast<RadioStreamPlayer*>(arg);
    player->TaskLoop();
    player->task_handle_ = nullptr;
    player->running_ = false;
    vTaskDelete(nullptr);
}

void RadioStreamPlayer::PlaybackTaskEntry(void* arg) {
    auto* player = static_cast<RadioStreamPlayer*>(arg);
    player->PlaybackLoop();
    player->playback_task_handle_ = nullptr;
    vTaskDelete(nullptr);
}

void RadioStreamPlayer::PlaybackLoop() {
    while (!stop_requested_) {
        std::vector<int16_t> pcm;
        {
            std::unique_lock<std::mutex> lock(pcm_queue_mutex_);
            pcm_queue_cv_.wait(lock, [this]() {
                return stop_requested_ || pcm_queue_.size() >= kStartBufferedChunks || playback_started_;
            });
            if (stop_requested_) {
                break;
            }
            if (pcm_queue_.empty()) {
                if (playback_started_) {
                    ESP_LOGW(TAG, "PCM buffer underrun, waiting for %u chunks",
                             static_cast<unsigned>(kStartBufferedChunks));
                }
                playback_started_ = false;
                continue;
            }
            playback_started_ = true;
            pcm = std::move(pcm_queue_.front());
            pcm_queue_.pop_front();
        }
        pcm_queue_cv_.notify_one();
        audio_service_->PlayPcm(pcm);
    }
}

void RadioStreamPlayer::TaskLoop() {
    while (!stop_requested_) {
        StreamOnce();
        ResetStreamState();
        if (!stop_requested_) {
            ESP_LOGW(TAG, "Stream stopped, reconnecting soon");
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
    }
}

void RadioStreamPlayer::StreamOnce() {
    if (!EnsureDecoder() || !EnsureResampler()) {
        return;
    }

    esp_http_client_config_t config = {
        .url = kStreamUrl,
        .method = HTTP_METHOD_GET,
        .timeout_ms = kHttpConnectTimeoutMs,
        .buffer_size = static_cast<int>(kHttpReadSize),
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to create HTTP client");
        return;
    }

    esp_http_client_set_header(client, "User-Agent", "ESP32 Antena1 Radio");
    esp_http_client_set_header(client, "Icy-MetaData", "0");
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open stream: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return;
    }

    int header_len = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "Stream connected, status=%d headers=%d", status, header_len);
    esp_http_client_set_timeout_ms(client, kHttpReadTimeoutMs);

    std::vector<uint8_t> read_buffer(kHttpReadSize);
    int consecutive_timeouts = 0;
    while (!stop_requested_) {
        int read = esp_http_client_read(client, reinterpret_cast<char*>(read_buffer.data()), read_buffer.size());
        if (read > 0) {
            consecutive_timeouts = 0;
            bytes_received_ += read;
            if (bytes_received_ <= kHttpReadSize || bytes_received_ % (32 * 1024) < static_cast<uint32_t>(read)) {
                ESP_LOGI(TAG, "Received %" PRIu32 " stream bytes", bytes_received_);
            }
            ProcessBytes(read_buffer.data(), read);
            continue;
        }
        if (read == 0) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (read == -ESP_ERR_HTTP_EAGAIN || read == -ESP_ERR_HTTP_READ_TIMEOUT) {
            if (++consecutive_timeouts < kMaxConsecutiveReadTimeouts) {
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }
        }
        ESP_LOGW(TAG, "Stream read failed: %d", read);
        break;
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
}

bool RadioStreamPlayer::EnsureDecoder() {
    if (aac_decoder_) {
        return true;
    }
    esp_aac_dec_cfg_t aac_cfg = ESP_AAC_DEC_CONFIG_DEFAULT();
    aac_cfg.channel = ESP_AUDIO_DUAL;
    aac_cfg.aac_plus_enable = true;
    auto ret = esp_aac_dec_open(&aac_cfg, sizeof(aac_cfg), &aac_decoder_);
    if (ret != ESP_AUDIO_ERR_OK || !aac_decoder_) {
        ESP_LOGE(TAG, "Failed to open AAC decoder: %d", ret);
        return false;
    }
    decode_buffer_.resize(8192);
    return true;
}

bool RadioStreamPlayer::EnsureResampler() {
    if (resampler_) {
        return true;
    }
    esp_ae_rate_cvt_cfg_t cfg = {
        .src_rate = kStreamSampleRate,
        .dest_rate = kOutputSampleRate,
        .channel = ESP_AUDIO_MONO,
        .bits_per_sample = ESP_AUDIO_BIT16,
        .complexity = 2,
        .perf_type = ESP_AE_RATE_CVT_PERF_TYPE_SPEED,
    };
    auto ret = esp_ae_rate_cvt_open(&cfg, &resampler_);
    if (ret != ESP_AE_ERR_OK || !resampler_) {
        ESP_LOGE(TAG, "Failed to open resampler: %d", ret);
        return false;
    }
    return true;
}

void RadioStreamPlayer::ResetStreamState() {
    input_buffer_.clear();
    bytes_received_ = 0;
    frames_decoded_ = 0;
    chunks_dropped_ = 0;
    logged_first_sync_ = false;
    playback_started_ = false;
    {
        std::lock_guard<std::mutex> lock(pcm_queue_mutex_);
        pcm_queue_.clear();
    }
    pcm_queue_cv_.notify_all();
    if (aac_decoder_) {
        esp_aac_dec_reset(aac_decoder_);
    }
    if (resampler_) {
        esp_ae_rate_cvt_reset(resampler_);
    }
}

void RadioStreamPlayer::ProcessBytes(const uint8_t* data, size_t len) {
    input_buffer_.insert(input_buffer_.end(), data, data + len);
    if (input_buffer_.size() > kMaxBufferedBytes) {
        ESP_LOGW(TAG, "Dropping buffered stream data");
        input_buffer_.erase(input_buffer_.begin(), input_buffer_.end() - kHttpReadSize);
    }

    while (!stop_requested_) {
        int sync = FindAdtsSync(input_buffer_);
        if (sync < 0) {
            if (input_buffer_.size() > 1) {
                input_buffer_.erase(input_buffer_.begin(), input_buffer_.end() - 1);
            }
            return;
        }
        if (sync > 0) {
            input_buffer_.erase(input_buffer_.begin(), input_buffer_.begin() + sync);
        }
        if (!logged_first_sync_) {
            ESP_LOGI(TAG, "Found ADTS sync after %" PRIu32 " stream bytes", bytes_received_);
            logged_first_sync_ = true;
        }
        int frame_len = GetAdtsFrameLength(input_buffer_.data(), input_buffer_.size());
        if (frame_len < 0) {
            return;
        }
        if (frame_len == 0) {
            input_buffer_.erase(input_buffer_.begin());
            continue;
        }
        DecodeAdtsFrame(input_buffer_.data(), frame_len);
        input_buffer_.erase(input_buffer_.begin(), input_buffer_.begin() + frame_len);
    }
}

bool RadioStreamPlayer::DecodeAdtsFrame(uint8_t* frame, size_t frame_len) {
    esp_audio_dec_in_raw_t raw = {
        .buffer = frame,
        .len = static_cast<uint32_t>(frame_len),
        .consumed = 0,
        .frame_recover = ESP_AUDIO_DEC_RECOVERY_NONE,
    };
    esp_audio_dec_out_frame_t out = {
        .buffer = decode_buffer_.data(),
        .len = static_cast<uint32_t>(decode_buffer_.size()),
        .needed_size = 0,
        .decoded_size = 0,
    };
    esp_audio_dec_info_t info = {};
    auto ret = esp_aac_dec_decode(aac_decoder_, &raw, &out, &info);
    if (ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH && out.needed_size > decode_buffer_.size()) {
        decode_buffer_.resize(out.needed_size);
        out.buffer = decode_buffer_.data();
        out.len = static_cast<uint32_t>(decode_buffer_.size());
        ret = esp_aac_dec_decode(aac_decoder_, &raw, &out, &info);
    }
    if (ret != ESP_AUDIO_ERR_OK) {
        ESP_LOGW(TAG, "AAC decode failed: %d", ret);
        return false;
    }
    if (out.decoded_size > 0) {
        frames_decoded_++;
        if (frames_decoded_ == 1 || frames_decoded_ % 200 == 0) {
            ESP_LOGI(TAG, "Decoded AAC frame %" PRIu32 ": %u bytes PCM, %u Hz, %u channels",
                     frames_decoded_, out.decoded_size, info.sample_rate, info.channel);
        }
        OutputDecodedPcm(out.buffer, out.decoded_size, info.channel, info.sample_rate);
    }
    return true;
}

void RadioStreamPlayer::OutputDecodedPcm(uint8_t* data, size_t bytes, uint8_t channels, uint32_t sample_rate) {
    if (!audio_service_ || bytes < sizeof(int16_t) || sample_rate == 0) {
        return;
    }

    auto* pcm = reinterpret_cast<int16_t*>(data);
    size_t samples = bytes / sizeof(int16_t);
    std::vector<int16_t> mono;
    if (channels == ESP_AUDIO_DUAL && samples >= 2) {
        size_t frames = samples / 2;
        mono.resize(frames);
        for (size_t i = 0; i < frames; ++i) {
            int32_t mixed = static_cast<int32_t>(pcm[i * 2]) + static_cast<int32_t>(pcm[i * 2 + 1]);
            mono[i] = static_cast<int16_t>(mixed / 2);
        }
    } else {
        mono.assign(pcm, pcm + samples);
    }

    if (sample_rate == kOutputSampleRate) {
        QueuePcm(std::move(mono));
        return;
    }
    if (sample_rate != kStreamSampleRate || !resampler_) {
        ESP_LOGW(TAG, "Unsupported AAC sample rate: %" PRIu32, sample_rate);
        return;
    }

    uint32_t out_samples = 0;
    esp_ae_rate_cvt_get_max_out_sample_num(resampler_, mono.size(), &out_samples);
    std::vector<int16_t> resampled(out_samples);
    uint32_t actual_output = out_samples;
    auto ret = esp_ae_rate_cvt_process(resampler_, reinterpret_cast<esp_ae_sample_t>(mono.data()), mono.size(),
                                       reinterpret_cast<esp_ae_sample_t>(resampled.data()), &actual_output);
    if (ret != ESP_AE_ERR_OK) {
        ESP_LOGW(TAG, "Resample failed: %d", ret);
        return;
    }
    resampled.resize(actual_output);
    QueuePcm(std::move(resampled));
}

void RadioStreamPlayer::QueuePcm(std::vector<int16_t>&& pcm) {
    if (pcm.empty()) {
        return;
    }
    {
        std::unique_lock<std::mutex> lock(pcm_queue_mutex_);
        pcm_queue_cv_.wait(lock, [this]() {
            return stop_requested_ || pcm_queue_.size() < kMaxQueuedPcmChunks;
        });
        if (stop_requested_) {
            return;
        }
        pcm_queue_.push_back(std::move(pcm));
    }
    pcm_queue_cv_.notify_one();
}

int RadioStreamPlayer::FindAdtsSync(const std::vector<uint8_t>& buffer) {
    if (buffer.size() < 2) {
        return -1;
    }
    for (size_t i = 0; i + 1 < buffer.size(); ++i) {
        if (buffer[i] == 0xFF && (buffer[i + 1] & 0xF0) == 0xF0) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int RadioStreamPlayer::GetAdtsFrameLength(const uint8_t* data, size_t len) {
    if (len < 7) {
        return -1;
    }
    if (data[0] != 0xFF || (data[1] & 0xF0) != 0xF0) {
        return 0;
    }
    bool protection_absent = data[1] & 0x01;
    int header_len = protection_absent ? 7 : 9;
    int frame_len = ((data[3] & 0x03) << 11) | (data[4] << 3) | ((data[5] & 0xE0) >> 5);
    if (frame_len < header_len || frame_len > 8192) {
        return 0;
    }
    if (len < static_cast<size_t>(frame_len)) {
        return -1;
    }
    return frame_len;
}
