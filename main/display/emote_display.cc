#include "emote_display.h"

// Standard C++ headers
#include <cstring>
#include <memory>
#include <unordered_map>
#include <tuple>
#include <algorithm>
#include <cinttypes>

// Standard C headers
#include <sys/time.h>
#include <time.h>

// ESP-IDF headers
#include <esp_log.h>
#include <esp_lcd_panel_io.h>
#include <esp_timer.h>
#include <lvgl.h>

// FreeRTOS headers
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// Project headers
#include "assets/lang_config.h"
#include "assets.h"
#include "board.h"
#include "gfx.h"
#include "expression_emote.h"
#include <wifi_manager.h>

LV_FONT_DECLARE(font_puhui_basic_14_1);
LV_FONT_DECLARE(font_puhui_basic_16_4);
LV_FONT_DECLARE(font_puhui_basic_20_4);
LV_FONT_DECLARE(font_puhui_basic_30_4);

extern "C" const gfx_image_dsc_t antena1_logo;

namespace emote {

namespace {
bool IsLegacyCloudMessage(const char* message)
{
    static constexpr char blocked_domain[] = {
        'x', 'i', 'a', 'o', 'z', 'h', 'i', '.', 'm', 'e', '\0'
    };
    return message && std::strstr(message, blocked_domain);
}
} // namespace

// ============================================================================
// Constants and Type Definitions
// ============================================================================

static const char* TAG = "EmoteDisplay";

// ============================================================================
// Forward Declarations
// ============================================================================

class EmoteDisplay;

// ============================================================================
// Helper Functions
// ============================================================================

static bool OnFlushIoReady(const esp_lcd_panel_io_handle_t panel_io,
    esp_lcd_panel_io_event_data_t* const edata, void* user_ctx)
{
    emote_handle_t handle = static_cast<emote_handle_t>(user_ctx);
    if (handle) {
        emote_notify_flush_finished(handle);
    }
    return true;
}

// Flush callback for emote
static void OnFlushCallback(int x_start, int y_start, int x_end, int y_end, const void* data, emote_handle_t handle)
{
    esp_lcd_panel_handle_t panel = (esp_lcd_panel_handle_t)emote_get_user_data(handle);
    if (panel != nullptr) {
        esp_lcd_panel_draw_bitmap(panel, x_start, y_start, x_end, y_end, data);
    }
}

// ============================================================================
// Graphics Initialization Functions
// ============================================================================

static emote_handle_t InitializeEmote(const esp_lcd_panel_handle_t panel, const int width, const int height)
{
    if (!panel) {
        ESP_LOGE(TAG, "Invalid panel");
        return nullptr;
    }

    emote_config_t emote_cfg = {
        .flags = {
            .swap = true,
            .double_buffer = true,
            .buff_dma = false,
        },
        .gfx_emote = {
            .h_res = width,
            .v_res = height,
            .fps = 30,
        },
        .buffers = {
            .buf_pixels = static_cast<size_t>(width * 16),
        },
        .task = {
            .task_priority = 5,
            .task_stack = 6 * 1024,
            .task_affinity = 0,
            .task_stack_in_ext = false,
        },
        .flush_cb = OnFlushCallback,
        .user_data = (void*)panel,
    };

    emote_handle_t emote_handle = emote_init(&emote_cfg);
    if (!emote_handle) {
        ESP_LOGE(TAG, "Failed to initialize emote");
        return nullptr;
    }

    return emote_handle;
}

// ============================================================================
// EmoteDisplay Class Implementation
// ============================================================================

EmoteDisplay::EmoteDisplay(const esp_lcd_panel_handle_t panel, const esp_lcd_panel_io_handle_t panel_io,
                           const int width, const int height)
{
    emote_handle_ = InitializeEmote(panel, width, height);

    const esp_lcd_panel_io_callbacks_t cbs = {
        .on_color_trans_done = OnFlushIoReady,
    };
    esp_lcd_panel_io_register_event_callbacks(panel_io, &cbs, emote_handle_);
}

EmoteDisplay::~EmoteDisplay()
{
    if (emote_handle_) {
        emote_deinit(emote_handle_);
        emote_handle_ = nullptr;
    }
}

void EmoteDisplay::SetEmotion(const char* const emotion)
{
    ESP_LOGI(TAG, "SetEmotion: %s", emotion);
    if (dashboard_ready_ || (emotion && std::strcmp(emotion, "microchip_ai") == 0)) {
        EnsureDashboard();
    }
}

void EmoteDisplay::SetChatMessage(const char* const role, const char* const content)
{
    ESP_LOGI(TAG, "SetChatMessage: %s, %s", role, content);
    if (dashboard_ready_ && message_label_ && content && strlen(content) > 0 && !IsLegacyCloudMessage(content)) {
        emote_lock(emote_handle_);
        gfx_label_set_text(message_label_, content);
        emote_unlock(emote_handle_);
        RefreshAll();
    }
}

void EmoteDisplay::SetStatus(const char* const status)
{
    ESP_LOGI(TAG, "SetStatus: %s", status);
    if (!status || strlen(status) == 0) {
        return;
    }
    EnsureDashboard();
    if (std::strcmp(status, Lang::Strings::ERROR) == 0) {
        status_text_ = "Atencao";
    } else if (std::strcmp(status, Lang::Strings::LISTENING) == 0) {
        status_text_ = "Microfone ativo";
    } else if (std::strcmp(status, Lang::Strings::SPEAKING) == 0) {
        status_text_ = "Audio ativo";
    } else {
        status_text_ = "Painel ativo";
    }
    UpdateDashboardStatus();
}

void EmoteDisplay::ShowNotification(const char* notification, int duration_ms)
{
    (void)duration_ms;
    ESP_LOGI(TAG, "ShowNotification: %s", notification);
    if (dashboard_ready_ && notification && std::strstr(notification, "WiFi")) {
        UpdateDashboardStatus();
    }
}

void EmoteDisplay::UpdateStatusBar(bool update_all)
{
    ESP_LOGD(TAG, "UpdateStatusBar: %s", update_all ? "true" : "false");
    if (!emote_handle_) {
        return;
    }
    if (dashboard_ready_) {
        UpdateDashboardStatus();
    }
}

void EmoteDisplay::SetPowerSaveMode(bool on)
{
    ESP_LOGI(TAG, "SetPowerSaveMode: %s", on ? "ON" : "OFF");
    if (!emote_handle_) {
        return;
    }
}

void EmoteDisplay::SetPreviewImage(const void* image)
{
    if (image) {
        ESP_LOGI(TAG, "SetPreviewImage: Preview image not supported, using default icon");
    }
}

void EmoteDisplay::SetupUI()
{
    if (IsSetupUICalled()) {
        ESP_LOGW(TAG, "SetupUI() called multiple times, skipping duplicate call");
        return;
    }
    Display::SetupUI();
}

void EmoteDisplay::SetTheme(Theme* const theme)
{
    ESP_LOGI(TAG, "SetTheme: %p", theme);
}

gfx_obj_t* EmoteDisplay::CreateLabel(const char* name, const char* text, int x, int y, int w, int h, gfx_font_t font,
                                     gfx_color_t color, gfx_color_t bg_color, bool bg_enable,
                                     gfx_text_align_t text_align, gfx_label_long_mode_t long_mode)
{
    gfx_obj_t* label = emote_create_obj_by_type(emote_handle_, EMOTE_OBJ_TYPE_LABEL, name);
    if (!label) {
        ESP_LOGE(TAG, "Failed to create dashboard label");
        return nullptr;
    }

    emote_lock(emote_handle_);
    gfx_obj_set_pos(label, x, y);
    gfx_obj_set_size(label, w, h);
    gfx_label_set_font(label, font);
    gfx_label_set_text(label, text);
    gfx_label_set_color(label, color);
    gfx_label_set_bg_enable(label, bg_enable);
    if (bg_enable) {
        gfx_label_set_bg_color(label, bg_color);
    }
    gfx_label_set_text_align(label, text_align);
    gfx_label_set_long_mode(label, long_mode);
    emote_unlock(emote_handle_);
    return label;
}

void EmoteDisplay::CreateLogoImage()
{
    logo_image_ = emote_create_obj_by_type(emote_handle_, EMOTE_OBJ_TYPE_IMAGE, "dash_logo");
    if (!logo_image_) {
        ESP_LOGE(TAG, "Failed to create Antena 1 logo image");
        return;
    }

    emote_lock(emote_handle_);
    gfx_img_set_src(logo_image_, (void*)&antena1_logo);
    gfx_obj_set_pos(logo_image_, 130, 10);
    gfx_obj_set_visible(logo_image_, true);
    emote_unlock(emote_handle_);
}

void EmoteDisplay::EnsureDashboard()
{
    if (!emote_handle_ || dashboard_ready_) {
        return;
    }

    emote_lock(emote_handle_);
    const char* default_names[] = {
        EMT_DEF_ELEM_DEFAULT_LABEL,
        EMT_DEF_ELEM_EYE_ANIM,
        EMT_DEF_ELEM_EMERG_DLG,
        EMT_DEF_ELEM_TOAST_LABEL,
        EMT_DEF_ELEM_CLOCK_LABEL,
        EMT_DEF_ELEM_LISTEN_ANIM,
        EMT_DEF_ELEM_STATUS_ICON,
        EMT_DEF_ELEM_CHARGE_ICON,
        EMT_DEF_ELEM_BAT_LEFT_LABEL,
        EMT_DEF_ELEM_QRCODE,
    };
    for (const auto* name : default_names) {
        if (auto* obj = emote_get_obj_by_name(emote_handle_, name)) {
            gfx_obj_set_visible(obj, false);
        }
    }
    emote_unlock(emote_handle_);

    CreateLogoImage();

    brand_label_ = CreateLabel("dash_brand", "ANTENA 1", 28, 112, 304, 34, (gfx_font_t)&font_puhui_basic_30_4,
                               GFX_COLOR_HEX(0xFF2B2B), GFX_COLOR_HEX(0x050505), false,
                               GFX_TEXT_ALIGN_CENTER, GFX_LABEL_LONG_CLIP);
    frequency_label_ = CreateLabel("dash_frequency", "94.7 FM", 92, 150, 176, 28, (gfx_font_t)&font_puhui_basic_20_4,
                                   GFX_COLOR_HEX(0xF7D046), GFX_COLOR_HEX(0x1A1A1A), true,
                                   GFX_TEXT_ALIGN_CENTER, GFX_LABEL_LONG_CLIP);
    name_label_ = CreateLabel("dash_name", "Paulo Gurgel", 28, 188, 304, 36, (gfx_font_t)&font_puhui_basic_30_4,
                              GFX_COLOR_HEX(0xFFFFFF), GFX_COLOR_HEX(0x050505), false,
                              GFX_TEXT_ALIGN_CENTER, GFX_LABEL_LONG_CLIP);
    message_label_ = CreateLabel("dash_message", "Antena 1, a radio mais ouvida do Brasil", 20, 238, 320, 32,
                                 (gfx_font_t)&font_puhui_basic_20_4, GFX_COLOR_HEX(0xFFFFFF),
                                 GFX_COLOR_HEX(0x101010), true, GFX_TEXT_ALIGN_CENTER,
                                 GFX_LABEL_LONG_SCROLL);
    wifi_label_ = CreateLabel("dash_wifi", "Wi-Fi: conectando", 24, 286, 312, 22, (gfx_font_t)&font_puhui_basic_16_4,
                              GFX_COLOR_HEX(0x84D65A), GFX_COLOR_HEX(0x050505), false,
                              GFX_TEXT_ALIGN_CENTER, GFX_LABEL_LONG_CLIP);
    ip_label_ = CreateLabel("dash_ip", "IP: aguardando", 24, 312, 312, 20, (gfx_font_t)&font_puhui_basic_14_1,
                            GFX_COLOR_HEX(0xA7A7A7), GFX_COLOR_HEX(0x050505), false,
                            GFX_TEXT_ALIGN_CENTER, GFX_LABEL_LONG_CLIP);
    status_label_ = CreateLabel("dash_status", "Painel ativo", 24, 336, 312, 20, (gfx_font_t)&font_puhui_basic_14_1,
                                GFX_COLOR_HEX(0x6CB7FF), GFX_COLOR_HEX(0x050505), false,
                                GFX_TEXT_ALIGN_CENTER, GFX_LABEL_LONG_CLIP);

    if (message_label_) {
        emote_lock(emote_handle_);
        gfx_label_set_scroll_step(message_label_, 1);
        gfx_label_set_scroll_speed(message_label_, 24);
        gfx_label_set_scroll_loop(message_label_, true);
        emote_unlock(emote_handle_);
    }

    dashboard_ready_ = true;

    UpdateDashboardStatus();
    RefreshAll();
}

void EmoteDisplay::UpdateDashboardStatus()
{
    if (!emote_handle_ || !dashboard_ready_) {
        return;
    }

    auto& wifi = WifiManager::GetInstance();
    std::string wifi_text;
    std::string ip_text;
    if (wifi.IsConnected()) {
        wifi_text = "Wi-Fi: " + wifi.GetSsid();
        ip_text = "IP: " + wifi.GetIpAddress() + "  RSSI: " + std::to_string(wifi.GetRssi()) + " dBm";
    } else if (wifi.IsConfigMode()) {
        wifi_text = "Wi-Fi: modo configuracao";
        ip_text = "Conecte ao hotspot do aparelho";
    } else {
        wifi_text = "Wi-Fi: conectando";
        ip_text = "IP: aguardando";
    }

    emote_lock(emote_handle_);
    if (wifi_label_) {
        gfx_label_set_text(wifi_label_, wifi_text.c_str());
    }
    if (ip_label_) {
        gfx_label_set_text(ip_label_, ip_text.c_str());
    }
    if (status_label_) {
        gfx_label_set_text(status_label_, status_text_.c_str());
    }
    emote_unlock(emote_handle_);
    RefreshAll();
}

bool EmoteDisplay::Lock(const int timeout_ms)
{
    (void)timeout_ms;
    return emote_handle_ && emote_lock(emote_handle_) == ESP_OK;
}

void EmoteDisplay::Unlock()
{
    if (emote_handle_) {
        emote_unlock(emote_handle_);
    }
}

bool EmoteDisplay::StopAnimDialog()
{
    ESP_LOGI(TAG, "StopAnimDialog");
    if (emote_handle_) {
        return emote_stop_anim_dialog(emote_handle_);
    }
    return false;
}

bool EmoteDisplay::InsertAnimDialog(const char* emoji_name, uint32_t duration_ms)
{
    ESP_LOGI(TAG, "InsertAnimDialog: %s, %" PRIu32, emoji_name, duration_ms);
    if (emote_handle_ && emoji_name) {
        return emote_insert_anim_dialog(emote_handle_, emoji_name, duration_ms);
    }
    return false;
}

void EmoteDisplay::RefreshAll()
{
    if (emote_handle_) {
        emote_notify_all_refresh(emote_handle_);
        return;
    }
}

} // namespace emote
