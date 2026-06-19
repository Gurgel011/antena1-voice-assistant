#include "application.h"
#include "board.h"
#include "display.h"
#include "system_info.h"
#include "audio_codec.h"
#include "mqtt_protocol.h"
#include "websocket_protocol.h"
#include "assets/lang_config.h"
#include "mcp_server.h"
#include "assets.h"
#include "settings.h"

#include <cerrno>
#include <cctype>
#include <cstring>
#include <fcntl.h>
#include <initializer_list>
#include <string>
#include <vector>
#include <esp_log.h>
#include <cJSON.h>
#include <driver/gpio.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#include <font_awesome.h>

#define TAG "Application"

namespace {
constexpr const char* kIdleDisplayMessage =
    "Toque para falar - Antena 1 com Paulo Gurgel";
constexpr const char* kLocalVoiceServerHost = "192.168.2.160";
constexpr int kLocalVoiceServerPort = 8088;
constexpr const char* kLocalVoiceServerPath = "/voice-ogg";
constexpr const char* kLocalTtsServerPath = "/tts";
constexpr int kLocalVoiceSampleRate = 16000;
constexpr int kLocalVoiceRecordMs = 5000;
constexpr size_t kMaxLocalVoiceResponseBytes = 256 * 1024;

std::string LowerAscii(std::string text) {
    for (char& ch : text) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return text;
}

bool ContainsAny(const std::string& text, const std::initializer_list<const char*> words) {
    for (const auto* word : words) {
        if (text.find(word) != std::string::npos) {
            return true;
        }
    }
    return false;
}

void AppendLe16(std::string& out, uint16_t value) {
    out.push_back(static_cast<char>(value & 0xff));
    out.push_back(static_cast<char>((value >> 8) & 0xff));
}

void AppendLe32(std::string& out, uint32_t value) {
    out.push_back(static_cast<char>(value & 0xff));
    out.push_back(static_cast<char>((value >> 8) & 0xff));
    out.push_back(static_cast<char>((value >> 16) & 0xff));
    out.push_back(static_cast<char>((value >> 24) & 0xff));
}

std::string BuildWav16Mono(const std::vector<int16_t>& pcm, int sample_rate) {
    const uint32_t data_bytes = pcm.size() * sizeof(int16_t);
    std::string wav;
    wav.reserve(44 + data_bytes);

    wav.append("RIFF", 4);
    AppendLe32(wav, 36 + data_bytes);
    wav.append("WAVE", 4);
    wav.append("fmt ", 4);
    AppendLe32(wav, 16);
    AppendLe16(wav, 1);
    AppendLe16(wav, 1);
    AppendLe32(wav, sample_rate);
    AppendLe32(wav, sample_rate * 2);
    AppendLe16(wav, 2);
    AppendLe16(wav, 16);
    wav.append("data", 4);
    AppendLe32(wav, data_bytes);
    wav.append(reinterpret_cast<const char*>(pcm.data()), data_bytes);
    return wav;
}

bool ConnectTcpWithTimeout(int sock, const char* host, int port, int timeout_ms) {
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        ESP_LOGE(TAG, "Invalid local voice server IP: %s", host);
        return false;
    }

    int flags = fcntl(sock, F_GETFL, 0);
    if (flags < 0) {
        flags = 0;
    }
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    int ret = connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (ret < 0 && errno != EINPROGRESS) {
        ESP_LOGE(TAG, "Local voice connect failed: errno=%d", errno);
        return false;
    }

    if (ret < 0) {
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(sock, &wfds);
        timeval tv = {
            .tv_sec = timeout_ms / 1000,
            .tv_usec = (timeout_ms % 1000) * 1000,
        };
        ret = select(sock + 1, nullptr, &wfds, nullptr, &tv);
        if (ret <= 0) {
            ESP_LOGE(TAG, "Local voice connect timeout/select failed: ret=%d errno=%d", ret, errno);
            return false;
        }

        int err = 0;
        socklen_t err_len = sizeof(err);
        if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &err_len) != 0 || err != 0) {
            ESP_LOGE(TAG, "Local voice connect socket error: %d", err);
            return false;
        }
    }

    fcntl(sock, F_SETFL, flags);
    return true;
}

bool SendAll(int sock, const char* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        int ret = send(sock, data + sent, len - sent, 0);
        if (ret < 0 && errno == EINTR) {
            continue;
        }
        if (ret <= 0) {
            ESP_LOGE(TAG, "Local voice send failed: ret=%d errno=%d", ret, errno);
            return false;
        }
        sent += ret;
    }
    return true;
}

std::string HeaderValue(const std::string& headers, const char* name) {
    std::string prefix = name;
    prefix += ":";
    size_t start = 0;
    while (start < headers.size()) {
        size_t end = headers.find("\r\n", start);
        if (end == std::string::npos) {
            end = headers.size();
        }
        std::string line = headers.substr(start, end - start);
        if (line.size() >= prefix.size() && line.compare(0, prefix.size(), prefix) == 0) {
            size_t value_start = prefix.size();
            while (value_start < line.size() && line[value_start] == ' ') {
                value_start++;
            }
            return line.substr(value_start);
        }
        start = end + 2;
    }
    return "";
}

std::string PercentDecode(std::string text) {
    std::string decoded;
    decoded.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size()) {
            char hex[3] = {text[i + 1], text[i + 2], '\0'};
            char* end = nullptr;
            long value = strtol(hex, &end, 16);
            if (end && *end == '\0') {
                decoded.push_back(static_cast<char>(value));
                i += 2;
                continue;
            }
        }
        decoded.push_back(text[i] == '+' ? ' ' : text[i]);
    }
    return decoded;
}

std::string RouteLabel(const std::string& route) {
    if (route == "wikipedia") return "Wikipedia";
    if (route == "ollama") return "LLM";
    if (route == "ollama_fallback") return "LLM indisponivel";
    if (route == "weather") return "Clima";
    if (route == "time") return "Hora";
    if (route == "date") return "Data";
    if (route == "context") return "Contexto";
    if (route == "heloisa" || route == "caio") return "Resposta personalizada";
    if (route == "no_source") return "Sem fonte confiavel";
    if (route == "internet") return "Internet";
    if (route == "radio_start" || route == "radio_stop" || route == "radio_unsupported") return "Radio";
    if (route == "empty_transcription") return "Transcricao";
    return route.empty() ? "Servidor local" : route;
}

std::string ListeningCountdownMessage(int remaining_seconds) {
    return "Ouvindo... " + std::to_string(remaining_seconds) + "s restantes";
}

bool IsLegacyCloudMessage(const char* message) {
    static constexpr char blocked_domain[] = {
        'x', 'i', 'a', 'o', 'z', 'h', 'i', '.', 'm', 'e', '\0'
    };
    return message && std::strstr(message, blocked_domain);
}

bool PostLocalVoiceRawHttp(const std::string& wav, std::string& response_audio, std::string& command, std::string& transcript, std::string& route, std::string& answer) {
    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Local voice socket failed: errno=%d", errno);
        return false;
    }

    timeval tv = {
        .tv_sec = 18,
        .tv_usec = 0,
    };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    bool ok = false;
    if (ConnectTcpWithTimeout(sock, kLocalVoiceServerHost, kLocalVoiceServerPort, 4000)) {
        std::string request =
            std::string("POST ") + kLocalVoiceServerPath + " HTTP/1.1\r\n" +
            "Host: " + kLocalVoiceServerHost + ":" + std::to_string(kLocalVoiceServerPort) + "\r\n" +
            "Content-Type: audio/wav\r\n" +
            "Content-Length: " + std::to_string(wav.size()) + "\r\n" +
            "Connection: close\r\n\r\n";

        if (SendAll(sock, request.data(), request.size()) && SendAll(sock, wav.data(), wav.size())) {
            std::string response;
            response.reserve(16 * 1024);
            char buffer[2048];
            while (response.size() < kMaxLocalVoiceResponseBytes + 8192) {
                int ret = recv(sock, buffer, sizeof(buffer), 0);
                if (ret < 0 && errno == EINTR) {
                    continue;
                }
                if (ret <= 0) {
                    break;
                }
                response.append(buffer, ret);
            }

            size_t header_end = response.find("\r\n\r\n");
            if (header_end != std::string::npos) {
                std::string headers = response.substr(0, header_end);
                command = HeaderValue(headers, "X-Command");
                transcript = PercentDecode(HeaderValue(headers, "X-Transcript"));
                route = HeaderValue(headers, "X-Route");
                answer = PercentDecode(HeaderValue(headers, "X-Answer"));
                int status = headers.find(" 200 ") != std::string::npos ? 200 : 0;
                response_audio.assign(response.data() + header_end + 4, response.size() - header_end - 4);
                ESP_LOGI(TAG, "Local voice raw HTTP status=%d command=%s route=%s transcript=%s answer=%s bytes=%u",
                         status, command.c_str(), route.c_str(), transcript.c_str(), answer.c_str(), response_audio.size());
                ok = status == 200;
            } else {
                ESP_LOGE(TAG, "Local voice raw HTTP missing headers, bytes=%u", response.size());
            }
        }
    }

    close(sock);
    return ok;
}

std::string JsonEscape(const std::string& text) {
    std::string escaped;
    escaped.reserve(text.size() + 8);
    for (char ch : text) {
        switch (ch) {
            case '\\':
                escaped += "\\\\";
                break;
            case '"':
                escaped += "\\\"";
                break;
            case '\n':
                escaped += "\\n";
                break;
            case '\r':
                escaped += "\\r";
                break;
            case '\t':
                escaped += "\\t";
                break;
            default:
                escaped.push_back(ch);
                break;
        }
    }
    return escaped;
}

bool PostLocalTtsRawHttp(const std::string& text, std::string& response_audio) {
    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Local TTS socket failed: errno=%d", errno);
        return false;
    }

    timeval tv = {
        .tv_sec = 6,
        .tv_usec = 0,
    };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    bool ok = false;
    if (ConnectTcpWithTimeout(sock, kLocalVoiceServerHost, kLocalVoiceServerPort, 3000)) {
        std::string body = "{\"text\":\"" + JsonEscape(text) + "\"}";
        std::string request =
            std::string("POST ") + kLocalTtsServerPath + " HTTP/1.1\r\n" +
            "Host: " + kLocalVoiceServerHost + ":" + std::to_string(kLocalVoiceServerPort) + "\r\n" +
            "Content-Type: application/json\r\n" +
            "Content-Length: " + std::to_string(body.size()) + "\r\n" +
            "Connection: close\r\n\r\n" +
            body;

        if (SendAll(sock, request.data(), request.size())) {
            std::string response;
            response.reserve(16 * 1024);
            char buffer[2048];
            while (response.size() < kMaxLocalVoiceResponseBytes + 8192) {
                int ret = recv(sock, buffer, sizeof(buffer), 0);
                if (ret < 0 && errno == EINTR) {
                    continue;
                }
                if (ret <= 0) {
                    break;
                }
                response.append(buffer, ret);
            }

            size_t header_end = response.find("\r\n\r\n");
            if (header_end != std::string::npos) {
                std::string headers = response.substr(0, header_end);
                int status = headers.find(" 200 ") != std::string::npos ? 200 : 0;
                response_audio.assign(response.data() + header_end + 4, response.size() - header_end - 4);
                ESP_LOGI(TAG, "Local TTS raw HTTP status=%d bytes=%u", status, response_audio.size());
                ok = status == 200 && !response_audio.empty();
            } else {
                ESP_LOGE(TAG, "Local TTS raw HTTP missing headers, bytes=%u", response.size());
            }
        }
    }

    close(sock);
    return ok;
}
}


Application::Application() {
    event_group_ = xEventGroupCreate();

#if CONFIG_USE_DEVICE_AEC && CONFIG_USE_SERVER_AEC
#error "CONFIG_USE_DEVICE_AEC and CONFIG_USE_SERVER_AEC cannot be enabled at the same time"
#elif CONFIG_USE_DEVICE_AEC
    aec_mode_ = kAecOnDeviceSide;
#elif CONFIG_USE_SERVER_AEC
    aec_mode_ = kAecOnServerSide;
#else
    aec_mode_ = kAecOff;
#endif

    esp_timer_create_args_t clock_timer_args = {
        .callback = [](void* arg) {
            Application* app = (Application*)arg;
            xEventGroupSetBits(app->event_group_, MAIN_EVENT_CLOCK_TICK);
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "clock_timer",
        .skip_unhandled_events = true
    };
    esp_timer_create(&clock_timer_args, &clock_timer_handle_);
}

Application::~Application() {
    if (clock_timer_handle_ != nullptr) {
        esp_timer_stop(clock_timer_handle_);
        esp_timer_delete(clock_timer_handle_);
    }
    vEventGroupDelete(event_group_);
}

bool Application::SetDeviceState(DeviceState state) {
    return state_machine_.TransitionTo(state);
}

void Application::Initialize() {
    auto& board = Board::GetInstance();
    SetDeviceState(kDeviceStateStarting);

    // Setup the display
    auto display = board.GetDisplay();
    display->SetupUI();
    display->SetEmotion("neutral");
    display->SetChatMessage("system", kIdleDisplayMessage);

    // Setup the audio service
    auto codec = board.GetAudioCodec();
    audio_service_.Initialize(codec);
    audio_service_.Start();

    AudioServiceCallbacks callbacks;
    callbacks.on_send_queue_available = [this]() {
        xEventGroupSetBits(event_group_, MAIN_EVENT_SEND_AUDIO);
    };
    callbacks.on_wake_word_detected = [this](const std::string& wake_word) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_WAKE_WORD_DETECTED);
    };
    callbacks.on_vad_change = [this](bool speaking) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_VAD_CHANGE);
    };
    audio_service_.SetCallbacks(callbacks);

    // Add state change listeners
    state_machine_.AddStateChangeListener([this](DeviceState old_state, DeviceState new_state) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_STATE_CHANGED);
    });

    // Start the clock timer to update the status bar
    esp_timer_start_periodic(clock_timer_handle_, 1000000);

    // Add MCP common tools (only once during initialization)
    auto& mcp_server = McpServer::GetInstance();
    mcp_server.AddCommonTools();
    mcp_server.AddUserOnlyTools();

    // Set network event callback for UI updates and network state handling
    board.SetNetworkEventCallback([this](NetworkEvent event, const std::string& data) {
        auto display = Board::GetInstance().GetDisplay();
        
        switch (event) {
            case NetworkEvent::Scanning:
                display->ShowNotification(Lang::Strings::SCANNING_WIFI, 30000);
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_DISCONNECTED);
                break;
            case NetworkEvent::Connecting: {
                if (data.empty()) {
                    // Cellular network - registering without carrier info yet
                    display->SetStatus(Lang::Strings::REGISTERING_NETWORK);
                } else {
                    // WiFi or cellular with carrier info
                    std::string msg = Lang::Strings::CONNECT_TO;
                    msg += data;
                    msg += "...";
                    display->ShowNotification(msg.c_str(), 30000);
                }
                break;
            }
            case NetworkEvent::Connected: {
                std::string msg = Lang::Strings::CONNECTED_TO;
                msg += data;
                display->ShowNotification(msg.c_str(), 30000);
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_CONNECTED);
                break;
            }
            case NetworkEvent::Disconnected:
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_DISCONNECTED);
                break;
            case NetworkEvent::WifiConfigModeEnter:
                // WiFi config mode enter is handled by WifiBoard internally
                break;
            case NetworkEvent::WifiConfigModeExit:
                // WiFi config mode exit is handled by WifiBoard internally
                break;
            // Cellular modem specific events
            case NetworkEvent::ModemDetecting:
                display->SetStatus(Lang::Strings::DETECTING_MODULE);
                break;
            case NetworkEvent::ModemErrorNoSim:
                Alert(Lang::Strings::ERROR, Lang::Strings::PIN_ERROR, "triangle_exclamation", Lang::Sounds::OGG_ERR_PIN);
                break;
            case NetworkEvent::ModemErrorRegDenied:
                Alert(Lang::Strings::ERROR, Lang::Strings::REG_ERROR, "triangle_exclamation", Lang::Sounds::OGG_ERR_REG);
                break;
            case NetworkEvent::ModemErrorInitFailed:
                Alert(Lang::Strings::ERROR, Lang::Strings::MODEM_INIT_ERROR, "triangle_exclamation", Lang::Sounds::OGG_EXCLAMATION);
                break;
            case NetworkEvent::ModemErrorTimeout:
                display->SetStatus(Lang::Strings::REGISTERING_NETWORK);
                break;
        }
    });

    // Start network asynchronously
    board.StartNetwork();

    // Update the status bar immediately to show the network state
    display->UpdateStatusBar(true);
}

void Application::Run() {
    // Set the priority of the main task to 10
    vTaskPrioritySet(nullptr, 10);

    const EventBits_t ALL_EVENTS = 
        MAIN_EVENT_SCHEDULE |
        MAIN_EVENT_SEND_AUDIO |
        MAIN_EVENT_WAKE_WORD_DETECTED |
        MAIN_EVENT_VAD_CHANGE |
        MAIN_EVENT_CLOCK_TICK |
        MAIN_EVENT_ERROR |
        MAIN_EVENT_NETWORK_CONNECTED |
        MAIN_EVENT_NETWORK_DISCONNECTED |
        MAIN_EVENT_TOGGLE_CHAT |
        MAIN_EVENT_START_LISTENING |
        MAIN_EVENT_STOP_LISTENING |
        MAIN_EVENT_ACTIVATION_DONE |
        MAIN_EVENT_STATE_CHANGED;

    while (true) {
        auto bits = xEventGroupWaitBits(event_group_, ALL_EVENTS, pdTRUE, pdFALSE, portMAX_DELAY);

        if (bits & MAIN_EVENT_ERROR) {
            SetDeviceState(kDeviceStateIdle);
            Alert(Lang::Strings::ERROR, last_error_message_.c_str(), "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
        }

        if (bits & MAIN_EVENT_NETWORK_CONNECTED) {
            HandleNetworkConnectedEvent();
        }

        if (bits & MAIN_EVENT_NETWORK_DISCONNECTED) {
            HandleNetworkDisconnectedEvent();
        }

        if (bits & MAIN_EVENT_ACTIVATION_DONE) {
            HandleActivationDoneEvent();
        }

        if (bits & MAIN_EVENT_STATE_CHANGED) {
            HandleStateChangedEvent();
        }

        if (bits & MAIN_EVENT_TOGGLE_CHAT) {
            HandleToggleChatEvent();
        }

        if (bits & MAIN_EVENT_START_LISTENING) {
            HandleStartListeningEvent();
        }

        if (bits & MAIN_EVENT_STOP_LISTENING) {
            HandleStopListeningEvent();
        }

        if (bits & MAIN_EVENT_SEND_AUDIO) {
            while (auto packet = audio_service_.PopPacketFromSendQueue()) {
                if (protocol_ && !protocol_->SendAudio(std::move(packet))) {
                    break;
                }
            }
        }

        if (bits & MAIN_EVENT_WAKE_WORD_DETECTED) {
            HandleWakeWordDetectedEvent();
        }

        if (bits & MAIN_EVENT_VAD_CHANGE) {
            if (GetDeviceState() == kDeviceStateListening) {
                auto led = Board::GetInstance().GetLed();
                led->OnStateChanged();
            }
        }

        if (bits & MAIN_EVENT_SCHEDULE) {
            std::unique_lock<std::mutex> lock(mutex_);
            auto tasks = std::move(main_tasks_);
            lock.unlock();
            for (auto& task : tasks) {
                task();
            }
        }

        if (bits & MAIN_EVENT_CLOCK_TICK) {
            clock_ticks_++;
            auto display = Board::GetInstance().GetDisplay();
            display->UpdateStatusBar();
        
            // Print debug info every 10 seconds
            if (clock_ticks_ % 10 == 0) {
                SystemInfo::PrintHeapStats();
            }
        }
    }
}

void Application::HandleNetworkConnectedEvent() {
    ESP_LOGI(TAG, "Network connected");
    auto state = GetDeviceState();

    if (state == kDeviceStateStarting || state == kDeviceStateWifiConfiguring) {
        // Network is ready, start activation
        SetDeviceState(kDeviceStateActivating);
        if (activation_task_handle_ != nullptr) {
            ESP_LOGW(TAG, "Activation task already running");
            return;
        }

        xTaskCreate([](void* arg) {
            Application* app = static_cast<Application*>(arg);
            app->ActivationTask();
            app->activation_task_handle_ = nullptr;
            vTaskDelete(NULL);
        }, "activation", 4096 * 2, this, 2, &activation_task_handle_);
    }

    // Update the status bar immediately to show the network state
    auto display = Board::GetInstance().GetDisplay();
    display->UpdateStatusBar(true);
}

void Application::HandleNetworkDisconnectedEvent() {
    // Close current conversation when network disconnected
    auto state = GetDeviceState();
    if (state == kDeviceStateConnecting || state == kDeviceStateListening || state == kDeviceStateSpeaking) {
        ESP_LOGI(TAG, "Closing audio channel due to network disconnection");
        protocol_->CloseAudioChannel();
    }

    // Update the status bar immediately to show the network state
    auto display = Board::GetInstance().GetDisplay();
    display->UpdateStatusBar(true);
}

void Application::HandleActivationDoneEvent() {
    ESP_LOGI(TAG, "Activation done");

    SystemInfo::PrintHeapStats();
    SetDeviceState(kDeviceStateIdle);

    has_server_time_ = ota_ && ota_->HasServerTime();

    auto display = Board::GetInstance().GetDisplay();
    if (ota_ && !ota_->GetCurrentVersion().empty()) {
        std::string message = std::string(Lang::Strings::VERSION) + ota_->GetCurrentVersion();
        display->ShowNotification(message.c_str());
    }
    display->SetChatMessage("system", kIdleDisplayMessage);

    // Release OTA object after activation is complete
    ota_.reset();
    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);

    display->SetChatMessage("system", kIdleDisplayMessage);
}

void Application::StartRadioStream() {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    if (!radio_stream_player_) {
        radio_stream_player_ = std::make_unique<RadioStreamPlayer>(&audio_service_);
    }
    radio_stream_player_->Start();
    display->SetChatMessage("system", "Antena 1 ao vivo - Paulo Gurgel");
}

void Application::StopRadioStream() {
    if (radio_stream_player_ && radio_stream_player_->IsRunning()) {
        radio_stream_player_->Stop();
    }
    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
    auto display = board.GetDisplay();
    display->SetChatMessage("system", "Radio pausada - Paulo Gurgel");
}

void Application::StartLocalVoiceAssistantTask() {
    if (local_voice_assistant_task_handle_ != nullptr) {
        ESP_LOGW(TAG, "Local voice assistant task already running");
        return;
    }

    ESP_LOGI(TAG, "Starting local voice assistant task");
    xTaskCreate([](void* arg) {
        Application* app = static_cast<Application*>(arg);
        app->LocalVoiceAssistantTask();
        app->local_voice_assistant_task_handle_ = nullptr;
        vTaskDelete(NULL);
    }, "local_voice_ai", 4096 * 4, this, 3, &local_voice_assistant_task_handle_);
}

void Application::LocalVoiceAssistantTask() {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto codec = board.GetAudioCodec();
    ESP_LOGI(TAG, "Local voice task started");
    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    StopRadioStream();
    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    audio_service_.ResetDecoder();

    local_voice_playback_active_ = true;
    Schedule([this, display]() {
        SetDeviceState(kDeviceStateSpeaking);
        display->SetChatMessage("system", "Antena 1 pronta. Como posso te ajudar?");
    });
    std::string prompt_audio;
    if (PostLocalTtsRawHttp("Como posso te ajudar?", prompt_audio)) {
        vTaskDelay(pdMS_TO_TICKS(200));
        audio_service_.ResetDecoder();
        audio_service_.PlaySound(std::string_view(prompt_audio.data(), prompt_audio.size()));
        vTaskDelay(pdMS_TO_TICKS(2600));
        local_voice_playback_active_ = false;
    } else {
        local_voice_playback_active_ = false;
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    Schedule([this, display]() {
        SetDeviceState(kDeviceStateListening);
        display->SetChatMessage("system", "Ouvindo... fale agora");
    });
    vTaskDelay(pdMS_TO_TICKS(500));

    const int chunk_ms = 60;
    const int chunk_samples = kLocalVoiceSampleRate * chunk_ms / 1000;
    const int chunks = kLocalVoiceRecordMs / chunk_ms;
    const int input_channels = codec ? std::max(1, codec->input_channels()) : 1;
    ESP_LOGI(TAG, "Recording local voice: chunks=%d chunk_samples=%d input_channels=%d", chunks, chunk_samples, input_channels);
    std::vector<int16_t> pcm;
    pcm.reserve(kLocalVoiceSampleRate * kLocalVoiceRecordMs / 1000);

    int read_ok = 0;
    int read_fail = 0;
    for (int i = 0; i < chunks; ++i) {
        if (i % (1000 / chunk_ms) == 0) {
            const int elapsed_ms = i * chunk_ms;
            const int remaining = std::max(1, (kLocalVoiceRecordMs - elapsed_ms + 999) / 1000);
            Schedule([display, remaining]() {
                auto message = ListeningCountdownMessage(remaining);
                display->SetChatMessage("system", message.c_str());
            });
        }
        std::vector<int16_t> chunk;
        if (audio_service_.ReadAudioData(chunk, kLocalVoiceSampleRate, chunk_samples)) {
            read_ok++;
            if (input_channels > 1 && chunk.size() >= static_cast<size_t>(chunk_samples * input_channels)) {
                std::vector<int16_t> mono(chunk_samples);
                for (int j = 0; j < chunk_samples; ++j) {
                    mono[j] = chunk[j * input_channels];
                }
                chunk = std::move(mono);
            } else if (chunk.size() > static_cast<size_t>(chunk_samples)) {
                chunk.resize(chunk_samples);
            }
            pcm.insert(pcm.end(), chunk.begin(), chunk.end());
        } else {
            read_fail++;
        }
    }
    ESP_LOGI(TAG, "Local voice capture done: read_ok=%d read_fail=%d pcm_samples=%u", read_ok, read_fail, pcm.size());

    if (pcm.empty()) {
        Schedule([this, display]() {
            display->SetChatMessage("system", "Nao consegui ouvir o microfone.");
            SetDeviceState(kDeviceStateIdle);
        });
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        return;
    }

    Schedule([display]() {
        display->SetChatMessage("system", "Processando sua pergunta...");
    });

    std::string wav = BuildWav16Mono(pcm, kLocalVoiceSampleRate);
    std::string response_audio;
    std::string command;
    std::string transcript;
    std::string route;
    std::string answer;
    ESP_LOGI(TAG, "Posting local voice WAV: %u bytes to %s:%d%s",
             wav.size(), kLocalVoiceServerHost, kLocalVoiceServerPort, kLocalVoiceServerPath);

    Schedule([display]() {
        display->SetChatMessage("system", "Consultando fontes...");
    });
    PostLocalVoiceRawHttp(wav, response_audio, command, transcript, route, answer);
    ESP_LOGI(TAG, "Local voice response: command=%s route=%s transcript=%s answer=%s bytes=%u",
             command.c_str(), route.c_str(), transcript.c_str(), answer.c_str(), response_audio.size());

    if (response_audio.empty() && command.empty()) {
        Schedule([this, display]() {
            display->SetChatMessage("system", "Servidor local sem resposta.");
            SetDeviceState(kDeviceStateIdle);
        });
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        return;
    }

    if (!transcript.empty()) {
        Schedule([display, transcript]() {
            std::string message = "Voce: " + transcript;
            display->SetChatMessage("system", message.c_str());
        });
        vTaskDelay(pdMS_TO_TICKS(1500));
    }

    if (!route.empty()) {
        std::string route_label = RouteLabel(route);
        Schedule([display, route_label]() {
            std::string message = "Fonte: " + route_label;
            display->SetChatMessage("system", message.c_str());
        });
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    auto play_response_audio = [this, display, &response_audio, &answer](int playback_ms) {
        if (response_audio.empty()) {
            return;
        }
        local_voice_playback_active_ = true;
        Schedule([this, display, answer]() {
            SetDeviceState(kDeviceStateSpeaking);
            if (!answer.empty()) {
                std::string message = "Resposta: " + answer;
                display->SetChatMessage("system", message.c_str());
            } else {
                display->SetChatMessage("system", "Respondendo...");
            }
        });
        vTaskDelay(pdMS_TO_TICKS(100));
        audio_service_.ResetDecoder();
        audio_service_.PlaySound(std::string_view(response_audio.data(), response_audio.size()));
        vTaskDelay(pdMS_TO_TICKS(playback_ms));
        local_voice_playback_active_ = false;
    };

    if (command == "radio_start") {
        play_response_audio(3000);
        Schedule([this]() {
            StartRadioStream();
            SetDeviceState(kDeviceStateIdle);
        });
        return;
    }

    if (command == "radio_stop") {
        play_response_audio(3000);
        Schedule([this, display]() {
            StopRadioStream();
            display->SetChatMessage("system", "Radio pausada.");
            SetDeviceState(kDeviceStateIdle);
        });
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        return;
    }

    if (command == "radio_unsupported") {
        play_response_audio(5000);
        Schedule([this, display]() {
            display->SetChatMessage("system", "Por enquanto so tenho a Antena 1 configurada.");
            SetDeviceState(kDeviceStateIdle);
        });
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        return;
    }

    if (transcript.empty()) {
        Schedule([this, display]() {
            display->SetChatMessage("system", "Nao entendi o audio. Fale de novo.");
            SetDeviceState(kDeviceStateIdle);
        });
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        return;
    }

    play_response_audio(7000);

    Schedule([this, display]() {
        display->SetChatMessage("system", kIdleDisplayMessage);
        SetDeviceState(kDeviceStateIdle);
    });
    board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
}

bool Application::HandleRadioVoiceCommand(const std::string& text) {
    auto command = LowerAscii(text);
    bool mentions_radio = ContainsAny(command, {"radio", "rádio", "antena", "stream"});
    if (!mentions_radio) {
        return false;
    }

    if (ContainsAny(command, {"parar", "pare", "pausar", "pause", "desligar", "desliga", "silencio", "silêncio"})) {
        Schedule([this]() {
            if (protocol_ && protocol_->IsAudioChannelOpened()) {
                protocol_->CloseAudioChannel();
            }
            StopRadioStream();
        });
        return true;
    }

    if (ContainsAny(command, {"tocar", "toque", "escutar", "ouvir", "liga", "ligar", "iniciar"})) {
        Schedule([this]() {
            if (protocol_ && protocol_->IsAudioChannelOpened()) {
                protocol_->CloseAudioChannel();
            }
            StartRadioStream();
        });
        return true;
    }

    return false;
}

void Application::ActivationTask() {
    // Create OTA object for activation process
    ota_ = std::make_unique<Ota>();

    // Check for new assets version
    CheckAssetsVersion();

    ESP_LOGI(TAG, "Skipping remote OTA/version check for local radio dashboard");

    // Local radio dashboard mode: keep the network free for the stream.
    protocol_.reset();

    // Signal completion to main loop
    xEventGroupSetBits(event_group_, MAIN_EVENT_ACTIVATION_DONE);
}

void Application::CheckAssetsVersion() {
    // Only allow CheckAssetsVersion to be called once
    if (assets_version_checked_) {
        return;
    }
    assets_version_checked_ = true;

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto& assets = Assets::GetInstance();

    if (!assets.partition_valid()) {
        ESP_LOGW(TAG, "Assets partition is disabled for board %s", BOARD_NAME);
        return;
    }
    
    Settings settings("assets", true);
    // Check if there is a new assets need to be downloaded
    std::string download_url = settings.GetString("download_url");

    if (!download_url.empty()) {
        settings.EraseKey("download_url");

        char message[256];
        snprintf(message, sizeof(message), Lang::Strings::FOUND_NEW_ASSETS, download_url.c_str());
        Alert(Lang::Strings::LOADING_ASSETS, message, "cloud_arrow_down", Lang::Sounds::OGG_UPGRADE);
        
        // Wait for the audio service to be idle for 3 seconds
        vTaskDelay(pdMS_TO_TICKS(3000));
        SetDeviceState(kDeviceStateUpgrading);
        board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        display->SetChatMessage("system", Lang::Strings::PLEASE_WAIT);

        bool success = assets.Download(download_url, [this, display](int progress, size_t speed) -> void {
            char buffer[32];
            snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
            Schedule([display, message = std::string(buffer)]() {
                display->SetChatMessage("system", message.c_str());
            });
        });

        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        vTaskDelay(pdMS_TO_TICKS(1000));

        if (!success) {
            Alert(Lang::Strings::ERROR, Lang::Strings::DOWNLOAD_ASSETS_FAILED, "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
            vTaskDelay(pdMS_TO_TICKS(2000));
            SetDeviceState(kDeviceStateActivating);
            return;
        }
    }

    // Apply assets
    assets.Apply();
    display->SetChatMessage("system", "");
    display->SetEmotion("microchip_ai");
}

void Application::CheckNewVersion() {
    const int MAX_RETRY = 10;
    int retry_count = 0;
    int retry_delay = 10; // Initial retry delay in seconds

    auto& board = Board::GetInstance();
    while (true) {
        auto display = board.GetDisplay();
        display->SetStatus(Lang::Strings::CHECKING_NEW_VERSION);

        esp_err_t err = ota_->CheckVersion();
        if (err != ESP_OK) {
            retry_count++;
            if (retry_count >= MAX_RETRY) {
                ESP_LOGE(TAG, "Too many retries, exit version check");
                return;
            }

            char error_message[128];
            snprintf(error_message, sizeof(error_message), "code=%d, url=%s", err, ota_->GetCheckVersionUrl().c_str());
            char buffer[256];
            snprintf(buffer, sizeof(buffer), Lang::Strings::CHECK_NEW_VERSION_FAILED, retry_delay, error_message);
            Alert(Lang::Strings::ERROR, buffer, "cloud_slash", Lang::Sounds::OGG_EXCLAMATION);

            ESP_LOGW(TAG, "Check new version failed, retry in %d seconds (%d/%d)", retry_delay, retry_count, MAX_RETRY);
            for (int i = 0; i < retry_delay; i++) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                if (GetDeviceState() == kDeviceStateIdle) {
                    break;
                }
            }
            retry_delay *= 2; // Double the retry delay
            continue;
        }
        retry_count = 0;
        retry_delay = 10; // Reset retry delay

        if (ota_->HasNewVersion()) {
            if (UpgradeFirmware(ota_->GetFirmwareUrl(), ota_->GetFirmwareVersion())) {
                return; // This line will never be reached after reboot
            }
            // If upgrade failed, continue to normal operation
        }

        // No new version, mark the current version as valid
        ota_->MarkCurrentVersionValid();
        if (!ota_->HasActivationCode() && !ota_->HasActivationChallenge()) {
            // Exit the loop if done checking new version
            break;
        }

        display->SetStatus(Lang::Strings::ACTIVATION);
        // Activation code is shown to the user and waiting for the user to input
        if (ota_->HasActivationCode()) {
            ShowActivationCode(ota_->GetActivationCode(), ota_->GetActivationMessage());
            break;
        }

        // This will block the loop until the activation is done or timeout
        for (int i = 0; i < 10; ++i) {
            ESP_LOGI(TAG, "Activating... %d/%d", i + 1, 10);
            esp_err_t err = ota_->Activate();
            if (err == ESP_OK) {
                break;
            } else if (err == ESP_ERR_TIMEOUT) {
                vTaskDelay(pdMS_TO_TICKS(3000));
            } else {
                vTaskDelay(pdMS_TO_TICKS(10000));
            }
            if (GetDeviceState() == kDeviceStateIdle) {
                break;
            }
        }
    }
}

void Application::InitializeProtocol() {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto codec = board.GetAudioCodec();

    display->SetStatus(Lang::Strings::LOADING_PROTOCOL);

    if (ota_->HasMqttConfig()) {
        protocol_ = std::make_unique<MqttProtocol>();
    } else if (ota_->HasWebsocketConfig()) {
        protocol_ = std::make_unique<WebsocketProtocol>();
    } else {
        ESP_LOGW(TAG, "No protocol specified in the OTA config, using MQTT");
        protocol_ = std::make_unique<MqttProtocol>();
    }

    protocol_->OnConnected([this]() {
        DismissAlert();
    });

    protocol_->OnNetworkError([this](const std::string& message) {
        last_error_message_ = message;
        xEventGroupSetBits(event_group_, MAIN_EVENT_ERROR);
    });
    
    protocol_->OnIncomingAudio([this](std::unique_ptr<AudioStreamPacket> packet) {
        if (GetDeviceState() == kDeviceStateSpeaking) {
            audio_service_.PushPacketToDecodeQueue(std::move(packet));
        }
    });
    
    protocol_->OnAudioChannelOpened([this, codec, &board]() {
        board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        if (protocol_->server_sample_rate() != codec->output_sample_rate()) {
            ESP_LOGW(TAG, "Server sample rate %d does not match device output sample rate %d, resampling may cause distortion",
                protocol_->server_sample_rate(), codec->output_sample_rate());
        }
    });
    
    protocol_->OnAudioChannelClosed([this, &board]() {
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        Schedule([this]() {
            auto display = Board::GetInstance().GetDisplay();
            display->SetChatMessage("system", "");
            SetDeviceState(kDeviceStateIdle);
        });
    });
    
    protocol_->OnIncomingJson([this, display](const cJSON* root) {
        // Parse JSON data
        auto type = cJSON_GetObjectItem(root, "type");
        if (strcmp(type->valuestring, "tts") == 0) {
            auto state = cJSON_GetObjectItem(root, "state");
            if (strcmp(state->valuestring, "start") == 0) {
                Schedule([this]() {
                    aborted_ = false;
                    SetDeviceState(kDeviceStateSpeaking);
                });
            } else if (strcmp(state->valuestring, "stop") == 0) {
                Schedule([this]() {
                    if (GetDeviceState() == kDeviceStateSpeaking) {
                        if (listening_mode_ == kListeningModeManualStop) {
                            SetDeviceState(kDeviceStateIdle);
                        } else {
                            SetDeviceState(kDeviceStateListening);
                        }
                    }
                });
            } else if (strcmp(state->valuestring, "sentence_start") == 0) {
                auto text = cJSON_GetObjectItem(root, "text");
                if (cJSON_IsString(text)) {
                    ESP_LOGI(TAG, "<< %s", text->valuestring);
                    Schedule([display, message = std::string(text->valuestring)]() {
                        display->SetChatMessage("assistant", message.c_str());
                    });
                }
            }
        } else if (strcmp(type->valuestring, "stt") == 0) {
            auto text = cJSON_GetObjectItem(root, "text");
            if (cJSON_IsString(text)) {
                ESP_LOGI(TAG, ">> %s", text->valuestring);
                if (HandleRadioVoiceCommand(text->valuestring)) {
                    ESP_LOGI(TAG, "Handled local radio voice command: %s", text->valuestring);
                    return;
                }
                Schedule([display, message = std::string(text->valuestring)]() {
                    display->SetChatMessage("user", message.c_str());
                });
            }
        } else if (strcmp(type->valuestring, "llm") == 0) {
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(emotion)) {
                Schedule([display, emotion_str = std::string(emotion->valuestring)]() {
                    display->SetEmotion(emotion_str.c_str());
                });
            }
        } else if (strcmp(type->valuestring, "mcp") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            if (cJSON_IsObject(payload)) {
                McpServer::GetInstance().ParseMessage(payload);
            }
        } else if (strcmp(type->valuestring, "system") == 0) {
            auto command = cJSON_GetObjectItem(root, "command");
            if (cJSON_IsString(command)) {
                ESP_LOGI(TAG, "System command: %s", command->valuestring);
                if (strcmp(command->valuestring, "reboot") == 0) {
                    // Do a reboot if user requests a OTA update
                    Schedule([this]() {
                        Reboot();
                    });
                } else {
                    ESP_LOGW(TAG, "Unknown system command: %s", command->valuestring);
                }
            }
        } else if (strcmp(type->valuestring, "alert") == 0) {
            auto status = cJSON_GetObjectItem(root, "status");
            auto message = cJSON_GetObjectItem(root, "message");
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(status) && cJSON_IsString(message) && cJSON_IsString(emotion)) {
                Alert(status->valuestring, message->valuestring, emotion->valuestring, Lang::Sounds::OGG_VIBRATION);
            } else {
                ESP_LOGW(TAG, "Alert command requires status, message and emotion");
            }
#if CONFIG_RECEIVE_CUSTOM_MESSAGE
        } else if (strcmp(type->valuestring, "custom") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            ESP_LOGI(TAG, "Received custom message: %s", cJSON_PrintUnformatted(root));
            if (cJSON_IsObject(payload)) {
                Schedule([this, display, payload_str = std::string(cJSON_PrintUnformatted(payload))]() {
                    display->SetChatMessage("system", payload_str.c_str());
                });
            } else {
                ESP_LOGW(TAG, "Invalid custom message format: missing payload");
            }
#endif
        } else {
            ESP_LOGW(TAG, "Unknown message type: %s", type->valuestring);
        }
    });
    
    protocol_->Start();
}

void Application::ShowActivationCode(const std::string& code, const std::string& message) {
    struct digit_sound {
        char digit;
        const std::string_view& sound;
    };
    static const std::array<digit_sound, 10> digit_sounds{{
        digit_sound{'0', Lang::Sounds::OGG_0},
        digit_sound{'1', Lang::Sounds::OGG_1}, 
        digit_sound{'2', Lang::Sounds::OGG_2},
        digit_sound{'3', Lang::Sounds::OGG_3},
        digit_sound{'4', Lang::Sounds::OGG_4},
        digit_sound{'5', Lang::Sounds::OGG_5},
        digit_sound{'6', Lang::Sounds::OGG_6},
        digit_sound{'7', Lang::Sounds::OGG_7},
        digit_sound{'8', Lang::Sounds::OGG_8},
        digit_sound{'9', Lang::Sounds::OGG_9}
    }};

    // Keep the display on the local custom message instead of the server activation URL.
    Alert(Lang::Strings::STANDBY, kIdleDisplayMessage, "neutral", "");

    for (const auto& digit : code) {
        auto it = std::find_if(digit_sounds.begin(), digit_sounds.end(),
            [digit](const digit_sound& ds) { return ds.digit == digit; });
        if (it != digit_sounds.end()) {
            audio_service_.PlaySound(it->sound);
        }
    }
}

void Application::Alert(const char* status, const char* message, const char* emotion, const std::string_view& sound) {
    if (IsLegacyCloudMessage(message)) {
        status = Lang::Strings::STANDBY;
        message = kIdleDisplayMessage;
        emotion = "neutral";
    }

    ESP_LOGW(TAG, "Alert [%s] %s: %s", emotion, status, message);
    auto display = Board::GetInstance().GetDisplay();
    display->SetStatus(status);
    display->SetEmotion(emotion);
    display->SetChatMessage("system", message);
    if (!sound.empty()) {
        audio_service_.PlaySound(sound);
    }
}

void Application::DismissAlert() {
    if (GetDeviceState() == kDeviceStateIdle) {
        auto display = Board::GetInstance().GetDisplay();
        display->SetStatus(Lang::Strings::STANDBY);
        display->SetEmotion("neutral");
        display->SetChatMessage("system", kIdleDisplayMessage);
    }
}

void Application::ToggleChatState() {
    xEventGroupSetBits(event_group_, MAIN_EVENT_TOGGLE_CHAT);
}

void Application::StartListening() {
    xEventGroupSetBits(event_group_, MAIN_EVENT_START_LISTENING);
}

void Application::StopListening() {
    xEventGroupSetBits(event_group_, MAIN_EVENT_STOP_LISTENING);
}

void Application::ToggleRadioStream() {
    Schedule([this]() {
        if (radio_stream_player_ && radio_stream_player_->IsRunning()) {
            StopRadioStream();
        } else {
            if (protocol_ && protocol_->IsAudioChannelOpened()) {
                protocol_->CloseAudioChannel();
            }
            StartRadioStream();
        }
    });
}

void Application::HandleToggleChatEvent() {
    auto state = GetDeviceState();
    
    if (state == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (state == kDeviceStateWifiConfiguring) {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    } else if (state == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    }

    if (!protocol_) {
        if (state == kDeviceStateIdle) {
            StartLocalVoiceAssistantTask();
        } else if (state == kDeviceStateSpeaking) {
            audio_service_.ResetDecoder();
            SetDeviceState(kDeviceStateIdle);
        }
        return;
    }

    if (state == kDeviceStateIdle) {
        ListeningMode mode = GetDefaultListeningMode();
        if (!protocol_->IsAudioChannelOpened()) {
            StopRadioStream();
            SetDeviceState(kDeviceStateConnecting);
            // Schedule to let the state change be processed first (UI update)
            Schedule([this, mode]() {
                ContinueOpenAudioChannel(mode);
            });
            return;
        }
        SetListeningMode(mode);
    } else if (state == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonNone);
    } else if (state == kDeviceStateListening) {
        protocol_->CloseAudioChannel();
    }
}

void Application::ContinueOpenAudioChannel(ListeningMode mode) {
    // Check state again in case it was changed during scheduling
    if (GetDeviceState() != kDeviceStateConnecting) {
        return;
    }

    // Switch to performance mode before connecting to reduce latency
    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);

    if (!protocol_->IsAudioChannelOpened()) {
        if (!protocol_->OpenAudioChannel()) {
            return;
        }
    }

    SetListeningMode(mode);
}

void Application::HandleStartListeningEvent() {
    auto state = GetDeviceState();
    
    if (state == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (state == kDeviceStateWifiConfiguring) {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }
    
    if (state == kDeviceStateIdle) {
        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            // Schedule to let the state change be processed first (UI update)
            Schedule([this]() {
                ContinueOpenAudioChannel(kListeningModeManualStop);
            });
            return;
        }
        SetListeningMode(kListeningModeManualStop);
    } else if (state == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonNone);
        SetListeningMode(kListeningModeManualStop);
    }
}

void Application::HandleStopListeningEvent() {
    auto state = GetDeviceState();
    
    if (state == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    } else if (state == kDeviceStateListening) {
        if (protocol_) {
            protocol_->SendStopListening();
        }
        SetDeviceState(kDeviceStateIdle);
    }
}

void Application::HandleWakeWordDetectedEvent() {
    if (!protocol_) {
        return;
    }

    auto state = GetDeviceState();
    auto wake_word = audio_service_.GetLastWakeWord();
    ESP_LOGI(TAG, "Wake word detected: %s (state: %d)", wake_word.c_str(), (int)state);

    if (state == kDeviceStateIdle) {
        audio_service_.EncodeWakeWord();
        auto wake_word = audio_service_.GetLastWakeWord();

        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            // Schedule to let the state change be processed first (UI update),
            // then continue with OpenAudioChannel which may block for ~1 second
            Schedule([this, wake_word]() {
                ContinueWakeWordInvoke(wake_word);
            });
            return;
        }
        // Channel already opened, continue directly
        ContinueWakeWordInvoke(wake_word);
    } else if (state == kDeviceStateSpeaking || state == kDeviceStateListening) {
        AbortSpeaking(kAbortReasonWakeWordDetected);
        // Clear send queue to avoid sending residues to server
        while (audio_service_.PopPacketFromSendQueue());

        if (state == kDeviceStateListening) {
            protocol_->SendStartListening(GetDefaultListeningMode());
            audio_service_.ResetDecoder();
            audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
            // Re-enable wake word detection as it was stopped by the detection itself
            audio_service_.EnableWakeWordDetection(true);
        } else {
            // Play popup sound and start listening again
            play_popup_on_listening_ = true;
            SetListeningMode(GetDefaultListeningMode());
        }
    } else if (state == kDeviceStateActivating) {
        // Restart the activation check if the wake word is detected during activation
        SetDeviceState(kDeviceStateIdle);
    }
}

void Application::ContinueWakeWordInvoke(const std::string& wake_word) {
    // Check state again in case it was changed during scheduling
    if (GetDeviceState() != kDeviceStateConnecting) {
        return;
    }

    // Switch to performance mode before connecting to reduce latency
    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);

    if (!protocol_->IsAudioChannelOpened()) {
        if (!protocol_->OpenAudioChannel()) {
            audio_service_.EnableWakeWordDetection(true);
            return;
        }
    }

    ESP_LOGI(TAG, "Wake word detected: %s", wake_word.c_str());
#if CONFIG_SEND_WAKE_WORD_DATA
    // Encode and send the wake word data to the server
    while (auto packet = audio_service_.PopWakeWordPacket()) {
        protocol_->SendAudio(std::move(packet));
    }
    // Set the chat state to wake word detected
    protocol_->SendWakeWordDetected(wake_word);
    SetListeningMode(GetDefaultListeningMode());
#else
    // Set flag to play popup sound after state changes to listening
    // (PlaySound here would be cleared by ResetDecoder in EnableVoiceProcessing)
    play_popup_on_listening_ = true;
    SetListeningMode(GetDefaultListeningMode());
#endif
}

void Application::HandleStateChangedEvent() {
    DeviceState new_state = state_machine_.GetState();
    clock_ticks_ = 0;

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto led = board.GetLed();
    led->OnStateChanged();
    
    switch (new_state) {
        case kDeviceStateUnknown:
        case kDeviceStateIdle:
            display->SetStatus(Lang::Strings::STANDBY);
            display->ClearChatMessages();  // Clear messages first
            display->SetEmotion("neutral"); // Then set emotion (wechat mode checks child count)
            display->SetChatMessage("system", kIdleDisplayMessage);
            audio_service_.EnableVoiceProcessing(false);
            audio_service_.EnableWakeWordDetection(false);
            break;
        case kDeviceStateConnecting:
            display->SetStatus(Lang::Strings::CONNECTING);
            display->SetEmotion("neutral");
            display->SetChatMessage("system", "");
            break;
        case kDeviceStateListening:
            display->SetStatus(Lang::Strings::LISTENING);
            display->SetEmotion("neutral");

            // Make sure the audio processor is running
            if (play_popup_on_listening_ || !audio_service_.IsAudioProcessorRunning()) {
                // For auto mode, wait for playback queue to be empty before enabling voice processing
                // This prevents audio truncation when STOP arrives late due to network jitter
                if (listening_mode_ == kListeningModeAutoStop) {
                    audio_service_.WaitForPlaybackQueueEmpty();
                }
                
                if (protocol_) {
                    // Send the start listening command
                    protocol_->SendStartListening(listening_mode_);
                    audio_service_.EnableVoiceProcessing(true);
                } else {
                    audio_service_.EnableVoiceProcessing(false);
                }
            }

#ifdef CONFIG_WAKE_WORD_DETECTION_IN_LISTENING
            // Enable wake word detection in listening mode (configured via Kconfig)
            audio_service_.EnableWakeWordDetection(audio_service_.IsAfeWakeWord());
#else
            // Disable wake word detection in listening mode
            audio_service_.EnableWakeWordDetection(false);
#endif
            
            // Play popup sound after ResetDecoder (in EnableVoiceProcessing) has been called
            if (play_popup_on_listening_) {
                play_popup_on_listening_ = false;
                audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
            }
            break;
        case kDeviceStateSpeaking:
            display->SetStatus(Lang::Strings::SPEAKING);

            if (listening_mode_ != kListeningModeRealtime) {
                audio_service_.EnableVoiceProcessing(false);
                // Only AFE wake word can be detected in speaking mode
                audio_service_.EnableWakeWordDetection(audio_service_.IsAfeWakeWord());
            }
            if (!local_voice_playback_active_) {
                audio_service_.ResetDecoder();
            }
            break;
        case kDeviceStateWifiConfiguring:
            audio_service_.EnableVoiceProcessing(false);
            audio_service_.EnableWakeWordDetection(false);
            break;
        default:
            // Do nothing
            break;
    }
}

void Application::Schedule(std::function<void()>&& callback) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        main_tasks_.push_back(std::move(callback));
    }
    xEventGroupSetBits(event_group_, MAIN_EVENT_SCHEDULE);
}

void Application::AbortSpeaking(AbortReason reason) {
    ESP_LOGI(TAG, "Abort speaking");
    aborted_ = true;
    if (protocol_) {
        protocol_->SendAbortSpeaking(reason);
    }
}

void Application::SetListeningMode(ListeningMode mode) {
    listening_mode_ = mode;
    SetDeviceState(kDeviceStateListening);
}

ListeningMode Application::GetDefaultListeningMode() const {
    return aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime;
}

void Application::Reboot() {
    ESP_LOGI(TAG, "Rebooting...");
    // Disconnect the audio channel
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        protocol_->CloseAudioChannel();
    }
    protocol_.reset();
    audio_service_.Stop();

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

bool Application::UpgradeFirmware(const std::string& url, const std::string& version) {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();

    std::string upgrade_url = url;
    std::string version_info = version.empty() ? "(Manual upgrade)" : version;

    // Close audio channel if it's open
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        ESP_LOGI(TAG, "Closing audio channel before firmware upgrade");
        protocol_->CloseAudioChannel();
    }
    ESP_LOGI(TAG, "Starting firmware upgrade from URL: %s", upgrade_url.c_str());

    Alert(Lang::Strings::OTA_UPGRADE, Lang::Strings::UPGRADING, "download", Lang::Sounds::OGG_UPGRADE);
    vTaskDelay(pdMS_TO_TICKS(3000));

    SetDeviceState(kDeviceStateUpgrading);

    std::string message = std::string(Lang::Strings::NEW_VERSION) + version_info;
    display->SetChatMessage("system", message.c_str());

    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    audio_service_.Stop();
    vTaskDelay(pdMS_TO_TICKS(1000));

    bool upgrade_success = Ota::Upgrade(upgrade_url, [this, display](int progress, size_t speed) {
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
        Schedule([display, message = std::string(buffer)]() {
            display->SetChatMessage("system", message.c_str());
        });
    });

    if (!upgrade_success) {
        // Upgrade failed, restart audio service and continue running
        ESP_LOGE(TAG, "Firmware upgrade failed, restarting audio service and continuing operation...");
        audio_service_.Start(); // Restart audio service
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER); // Restore power save level
        Alert(Lang::Strings::ERROR, Lang::Strings::UPGRADE_FAILED, "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
        vTaskDelay(pdMS_TO_TICKS(3000));
        return false;
    } else {
        // Upgrade success, reboot immediately
        ESP_LOGI(TAG, "Firmware upgrade successful, rebooting...");
        display->SetChatMessage("system", "Upgrade successful, rebooting...");
        vTaskDelay(pdMS_TO_TICKS(1000)); // Brief pause to show message
        Reboot();
        return true;
    }
}

void Application::WakeWordInvoke(const std::string& wake_word) {
    if (!protocol_) {
        return;
    }

    auto state = GetDeviceState();
    
    if (state == kDeviceStateIdle) {
        audio_service_.EncodeWakeWord();

        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            // Schedule to let the state change be processed first (UI update)
            Schedule([this, wake_word]() {
                ContinueWakeWordInvoke(wake_word);
            });
            return;
        }
        // Channel already opened, continue directly
        ContinueWakeWordInvoke(wake_word);
    } else if (state == kDeviceStateSpeaking) {
        Schedule([this]() {
            AbortSpeaking(kAbortReasonNone);
        });
    } else if (state == kDeviceStateListening) {   
        Schedule([this]() {
            if (protocol_) {
                protocol_->CloseAudioChannel();
            }
        });
    }
}

bool Application::CanEnterSleepMode() {
    if (GetDeviceState() != kDeviceStateIdle) {
        return false;
    }

    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        return false;
    }

    if (!audio_service_.IsIdle()) {
        return false;
    }

    // Now it is safe to enter sleep mode
    return true;
}

void Application::RegisterMcpBroadcastCallback(std::function<void(const std::string&)> callback) {
    mcp_broadcast_callback_ = std::move(callback);
}

void Application::SendMcpMessage(const std::string& payload) {
    // Always schedule to run in main task for thread safety
    Schedule([this, payload](){ 
        if (protocol_) {
            protocol_->SendMcpMessage(payload);
        }
        if (mcp_broadcast_callback_) {
            mcp_broadcast_callback_(payload);
        }
    });
}

void Application::SetAecMode(AecMode mode) {
    aec_mode_ = mode;
    Schedule([this]() {
        auto& board = Board::GetInstance();
        auto display = board.GetDisplay();
        switch (aec_mode_) {
        case kAecOff:
            audio_service_.EnableDeviceAec(false);
            display->ShowNotification(Lang::Strings::RTC_MODE_OFF);
            break;
        case kAecOnServerSide:
            audio_service_.EnableDeviceAec(false);
            display->ShowNotification(Lang::Strings::RTC_MODE_ON);
            break;
        case kAecOnDeviceSide:
            audio_service_.EnableDeviceAec(true);
            display->ShowNotification(Lang::Strings::RTC_MODE_ON);
            break;
        }

        // If the AEC mode is changed, close the audio channel
        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
    });
}

void Application::PlaySound(const std::string_view& sound) {
    audio_service_.PlaySound(sound);
}

void Application::ResetProtocol() {
    Schedule([this]() {
        // Close audio channel if opened
        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
        // Reset protocol
        protocol_.reset();
    });
}
