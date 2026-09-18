#pragma once

#include "display/lcd_display.h"
#include "kanshan_emotions.h"

/**
 * @brief Otto机器人看山表情显示类
 * 继承LcdDisplay，仅替换中央表情，保留状态栏和聊天文字。
 */
class OttoEmojiDisplay : public SpiLcdDisplay {
public:
    /**
     * @brief 构造函数，参数与SpiLcdDisplay相同
     */
    OttoEmojiDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                     int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y,
                     bool swap_xy);

    virtual ~OttoEmojiDisplay() = default;

    // 重写表情设置方法
    virtual void SetEmotion(const char* emotion) override;

    // 重写聊天消息设置方法
    virtual void SetChatMessage(const char* role, const char* content) override;

    // 动作期间临时覆盖中央表情；结束后恢复最近一次对话表情。
    bool ShowAction(const char* action);
    void ClearAction();

private:
    void SetupImageContainer();
    static const lv_image_dsc_t* FindImage(const char* name);

    lv_obj_t* emotion_image_;  ///< 中央看山表情组件
    const lv_image_dsc_t* base_image_;  ///< 动作结束后恢复的对话表情
    bool action_active_;

    // 对话表情和动作贴图映射
    struct ImageMap {
        const char* name;
        const lv_image_dsc_t* image;
    };

    static const ImageMap image_maps_[];
};
