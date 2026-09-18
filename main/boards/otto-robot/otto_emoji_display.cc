#include "otto_emoji_display.h"
#include "lvgl_theme.h"

#include <esp_log.h>

#include <cstring>

#include "display/lcd_display.h"

#define TAG "OttoEmojiDisplay"

// 保持小智协议的21种表情名称不变，并为Otto动作增加中央贴图别名。
const OttoEmojiDisplay::ImageMap OttoEmojiDisplay::image_maps_[] = {
    {"neutral", &emotion_neutral},
    {"happy", &emotion_happy},
    {"laughing", &emotion_laughing},
    {"funny", &emotion_funny},
    {"sad", &emotion_sad},
    {"angry", &emotion_angry},
    {"crying", &emotion_crying},
    {"loving", &emotion_loving},
    {"embarrassed", &emotion_embarrassed},
    {"surprised", &emotion_surprised},
    {"shocked", &emotion_shocked},
    {"thinking", &emotion_thinking},
    {"winking", &emotion_winking},
    {"cool", &emotion_cool},
    {"relaxed", &emotion_relaxed},
    {"delicious", &emotion_delicious},
    {"kissy", &emotion_kissy},
    {"confident", &emotion_confident},
    {"sleepy", &emotion_sleepy},
    {"silly", &emotion_silly},
    {"confused", &emotion_confused},

    {"action_walk", &action_walk},
    {"action_turn", &action_turn},
    {"action_jump", &action_jump},
    {"action_swing", &action_swing},
    {"action_moonwalk", &action_moonwalk},
    {"action_bend", &action_bend},
    {"action_shake_leg", &action_shake_leg},
    {"action_updown", &action_updown},
    {"action_tiptoe_swing", &action_swing},
    {"action_jitter", &action_fitness},
    {"action_ascending_turn", &action_turn},
    {"action_crusaito", &action_whirlwind_leg},
    {"action_flapping", &action_windmill},
    {"action_hands_up", &action_hands_up},
    {"action_hands_down", &action_hands_down},
    {"action_hand_wave", &action_hand_wave},
    {"action_home", &action_home},
    {"action_laugh", &emotion_laughing},

    {nullptr, nullptr}  // 结束标记
};

OttoEmojiDisplay::OttoEmojiDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                                   int width, int height, int offset_x, int offset_y, bool mirror_x,
                                   bool mirror_y, bool swap_xy)
    : SpiLcdDisplay(panel_io, panel, width, height, offset_x, offset_y, mirror_x, mirror_y, swap_xy),
      emotion_image_(nullptr),
      base_image_(&emotion_neutral),
      action_active_(false) {
    SetupImageContainer();
};

void OttoEmojiDisplay::SetupImageContainer() {
    DisplayLockGuard lock(this);

    if (emoji_label_) {
        lv_obj_del(emoji_label_);
    }

    if (chat_message_label_) {
        lv_obj_del(chat_message_label_);
    }
    if (content_) {
        lv_obj_del(content_);
    }

    content_ = lv_obj_create(container_);
    lv_obj_set_scrollbar_mode(content_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_size(content_, LV_HOR_RES, LV_HOR_RES);
    lv_obj_set_style_bg_opa(content_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(content_, 0, 0);
    lv_obj_set_flex_grow(content_, 1);
    lv_obj_center(content_);

    emoji_label_ = lv_label_create(content_);
    lv_label_set_text(emoji_label_, "");
    lv_obj_set_width(emoji_label_, 0);
    lv_obj_set_style_border_width(emoji_label_, 0, 0);
    lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);

    emotion_image_ = lv_image_create(content_);
    lv_obj_set_size(emotion_image_, LV_HOR_RES, LV_HOR_RES);
    lv_obj_set_style_border_width(emotion_image_, 0, 0);
    lv_obj_set_style_bg_opa(emotion_image_, LV_OPA_TRANSP, 0);
    lv_obj_center(emotion_image_);
    lv_image_set_src(emotion_image_, &emotion_neutral);

    chat_message_label_ = lv_label_create(content_);
    lv_label_set_text(chat_message_label_, "");
    lv_obj_set_width(chat_message_label_, LV_HOR_RES * 0.9);
    lv_label_set_long_mode(chat_message_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(chat_message_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(chat_message_label_, lv_color_white(), 0);
    lv_obj_set_style_border_width(chat_message_label_, 0, 0);

    lv_obj_set_style_bg_opa(chat_message_label_, LV_OPA_70, 0);
    lv_obj_set_style_bg_color(chat_message_label_, lv_color_black(), 0);
    lv_obj_set_style_pad_ver(chat_message_label_, 5, 0);

    lv_obj_align(chat_message_label_, LV_ALIGN_BOTTOM_MID, 0, 0);

    auto& theme_manager = LvglThemeManager::GetInstance();
    auto theme = theme_manager.GetTheme("dark");
    if (theme != nullptr) {
        LcdDisplay::SetTheme(theme);
    }
}

void OttoEmojiDisplay::SetEmotion(const char* emotion) {
    if (!emotion || !emotion_image_) {
        return;
    }

    const lv_image_dsc_t* image = FindImage(emotion);
    if (image == nullptr) {
        image = &emotion_neutral;
        ESP_LOGI(TAG, "未知表情'%s'，使用默认", emotion);
    }

    DisplayLockGuard lock(this);
    base_image_ = image;
    if (!action_active_) {
        lv_image_set_src(emotion_image_, image);
    }
    ESP_LOGI(TAG, "设置表情: %s", emotion);
}

const lv_image_dsc_t* OttoEmojiDisplay::FindImage(const char* name) {
    for (const auto& map : image_maps_) {
        if (map.name && strcmp(map.name, name) == 0) {
            return map.image;
        }
    }
    return nullptr;
}

bool OttoEmojiDisplay::ShowAction(const char* action) {
    if (!action || !emotion_image_) {
        return false;
    }
    const lv_image_dsc_t* image = FindImage(action);
    if (image == nullptr) {
        ESP_LOGW(TAG, "动作贴图不存在: %s", action);
        return false;
    }

    DisplayLockGuard lock(this);
    action_active_ = true;
    lv_image_set_src(emotion_image_, image);
    ESP_LOGI(TAG, "显示动作贴图: %s", action);
    return true;
}

void OttoEmojiDisplay::ClearAction() {
    if (!emotion_image_) {
        return;
    }

    DisplayLockGuard lock(this);
    action_active_ = false;
    lv_image_set_src(emotion_image_, base_image_ != nullptr ? base_image_ : &emotion_neutral);
}

void OttoEmojiDisplay::SetChatMessage(const char* role, const char* content) {
    DisplayLockGuard lock(this);
    if (chat_message_label_ == nullptr) {
        return;
    }

    if (content == nullptr || strlen(content) == 0) {
        lv_obj_add_flag(chat_message_label_, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_label_set_text(chat_message_label_, content);
    lv_obj_remove_flag(chat_message_label_, LV_OBJ_FLAG_HIDDEN);

    ESP_LOGI(TAG, "设置聊天消息 [%s]: %s", role, content);
}
