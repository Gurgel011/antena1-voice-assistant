#pragma once

#include "display.h"
#include <memory>
#include <string>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include "expression_emote.h"
#include "core/gfx_obj.h"
#include "widget/gfx_label.h"
#include "widget/gfx_img.h"

namespace emote {

class EmoteDisplay : public Display {
public:
    EmoteDisplay(esp_lcd_panel_handle_t panel, esp_lcd_panel_io_handle_t panel_io, int width, int height);
    virtual ~EmoteDisplay();

    virtual void SetEmotion(const char* emotion) override;
    virtual void SetStatus(const char* status) override;
    virtual void SetChatMessage(const char* role, const char* content) override;
    virtual void SetTheme(Theme* theme) override;
    virtual void ShowNotification(const char* notification, int duration_ms = 3000) override;
    virtual void UpdateStatusBar(bool update_all = false) override;
    virtual void SetPowerSaveMode(bool on) override;
    virtual void SetPreviewImage(const void* image);
    virtual void SetupUI() override;

    bool StopAnimDialog();
    bool InsertAnimDialog(const char* emoji_name, uint32_t duration_ms);

    void RefreshAll();

    // Get emote handle for internal use
    emote_handle_t GetEmoteHandle() const { return emote_handle_; }

private:
    virtual bool Lock(int timeout_ms = 0) override;
    virtual void Unlock() override;

    void EnsureDashboard();
    void UpdateDashboardStatus();
    void CreateLogoImage();
    gfx_obj_t* CreateLabel(const char* name, const char* text, int x, int y, int w, int h, gfx_font_t font,
                           gfx_color_t color, gfx_color_t bg_color, bool bg_enable,
                           gfx_text_align_t text_align, gfx_label_long_mode_t long_mode);

    emote_handle_t emote_handle_ = nullptr;
    gfx_obj_t* logo_image_ = nullptr;
    gfx_obj_t* brand_label_ = nullptr;
    gfx_obj_t* frequency_label_ = nullptr;
    gfx_obj_t* name_label_ = nullptr;
    gfx_obj_t* message_label_ = nullptr;
    gfx_obj_t* wifi_label_ = nullptr;
    gfx_obj_t* ip_label_ = nullptr;
    gfx_obj_t* status_label_ = nullptr;
    std::string status_text_ = "Painel ativo";
    bool dashboard_ready_ = false;

};

} // namespace emote
