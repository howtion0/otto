/*
    Otto机器人控制器 - MCP协议版本
*/

#include <cJSON.h>
#include <esp_log.h>
#include <esp_timer.h>

#include <cstring>
#include <atomic>
#include <string_view>

#include "application.h"
#include "assets/lang_config.h"
#include "board.h"
#include "config.h"
#include "mcp_server.h"
#include "otto_emoji_display.h"
#include "otto_movements.h"
#include "sdkconfig.h"
#include "settings.h"

#define TAG "OttoController"

class OttoController {
private:
    enum ActionType {
        ACTION_WALK = 1,
        ACTION_TURN = 2,
        ACTION_JUMP = 3,
        ACTION_SWING = 4,
        ACTION_MOONWALK = 5,
        ACTION_BEND = 6,
        ACTION_SHAKE_LEG = 7,
        ACTION_UPDOWN = 8,
        ACTION_TIPTOE_SWING = 9,
        ACTION_JITTER = 10,
        ACTION_ASCENDING_TURN = 11,
        ACTION_CRUSAITO = 12,
        ACTION_FLAPPING = 13,
        ACTION_HANDS_UP = 14,
        ACTION_HANDS_DOWN = 15,
        ACTION_HAND_WAVE = 16,
        ACTION_HOME = 17,
        ACTION_LAUGH = 18
    };

    Otto otto_;
    TaskHandle_t action_task_handle_ = nullptr;
    QueueHandle_t action_queue_;
    bool has_hands_ = false;
    std::atomic<bool> is_action_in_progress_{false};
    QueueHandle_t sound_queue_ = nullptr;
    std::atomic<bool> sound_busy_{false};

    std::atomic<int> current_action_type_{0};
    std::atomic<int> current_action_steps_{0};
    std::atomic<int> current_action_speed_{0};
    std::atomic<int> current_action_direction_{0};
    std::atomic<int> current_action_amount_{0};
    std::atomic<int> last_action_type_{ACTION_HOME};
    std::atomic<int> last_action_steps_{1};
    std::atomic<int> last_action_speed_{1000};
    std::atomic<int> last_action_direction_{1};
    std::atomic<int> last_action_amount_{0};
    std::atomic<int64_t> action_started_at_ms_{0};
    std::atomic<int64_t> last_action_finished_at_ms_{0};
    std::atomic<int> current_sound_action_type_{0};
    std::atomic<bool> display_action_active_{false};
    std::atomic<int> display_action_type_{0};

    static const char* ActionName(int action_type) {
        switch (action_type) {
            case ACTION_WALK: return "walk";
            case ACTION_TURN: return "turn";
            case ACTION_JUMP: return "jump";
            case ACTION_SWING: return "swing";
            case ACTION_MOONWALK: return "moonwalk";
            case ACTION_BEND: return "bend";
            case ACTION_SHAKE_LEG: return "shake_leg";
            case ACTION_UPDOWN: return "updown";
            case ACTION_TIPTOE_SWING: return "tiptoe_swing";
            case ACTION_JITTER: return "jitter";
            case ACTION_ASCENDING_TURN: return "ascending_turn";
            case ACTION_CRUSAITO: return "crusaito";
            case ACTION_FLAPPING: return "flapping";
            case ACTION_HANDS_UP: return "hands_up";
            case ACTION_HANDS_DOWN: return "hands_down";
            case ACTION_HAND_WAVE: return "hand_wave";
            case ACTION_HOME: return "home";
            case ACTION_LAUGH: return "laugh";
            default: return "idle";
        }
    }

    static int ActionTypeFromName(const std::string& name) {
        if (name == "walk" || name == "walk_forward") return ACTION_WALK;
        if (name == "turn" || name == "turn_left") return ACTION_TURN;
        if (name == "jump") return ACTION_JUMP;
        if (name == "swing") return ACTION_SWING;
        if (name == "moonwalk") return ACTION_MOONWALK;
        if (name == "bend") return ACTION_BEND;
        if (name == "shake_leg") return ACTION_SHAKE_LEG;
        if (name == "updown") return ACTION_UPDOWN;
        if (name == "tiptoe_swing") return ACTION_TIPTOE_SWING;
        if (name == "jitter") return ACTION_JITTER;
        if (name == "ascending_turn") return ACTION_ASCENDING_TURN;
        if (name == "crusaito") return ACTION_CRUSAITO;
        if (name == "flapping") return ACTION_FLAPPING;
        if (name == "hands_up") return ACTION_HANDS_UP;
        if (name == "hands_down") return ACTION_HANDS_DOWN;
        if (name == "hand_wave") return ACTION_HAND_WAVE;
        if (name == "home") return ACTION_HOME;
        if (name == "laugh") return ACTION_LAUGH;
        return 0;
    }

    static const char* ActionImageName(int action_type) {
        switch (action_type) {
            case ACTION_WALK: return "action_walk";
            case ACTION_TURN: return "action_turn";
            case ACTION_JUMP: return "action_jump";
            case ACTION_SWING: return "action_swing";
            case ACTION_MOONWALK: return "action_moonwalk";
            case ACTION_BEND: return "action_bend";
            case ACTION_SHAKE_LEG: return "action_shake_leg";
            case ACTION_UPDOWN: return "action_updown";
            case ACTION_TIPTOE_SWING: return "action_tiptoe_swing";
            case ACTION_JITTER: return "action_jitter";
            case ACTION_ASCENDING_TURN: return "action_ascending_turn";
            case ACTION_CRUSAITO: return "action_crusaito";
            case ACTION_FLAPPING: return "action_flapping";
            case ACTION_HANDS_UP: return "action_hands_up";
            case ACTION_HANDS_DOWN: return "action_hands_down";
            case ACTION_HAND_WAVE: return "action_hand_wave";
            case ACTION_HOME: return "action_home";
            case ACTION_LAUGH: return "action_laugh";
            default: return nullptr;
        }
    }

    static OttoEmojiDisplay* GetOttoDisplay() {
        return static_cast<OttoEmojiDisplay*>(Board::GetInstance().GetDisplay());
    }

    static void SoundTask(void* arg) {
        auto* controller = static_cast<OttoController*>(arg);
        std::string_view sound;
        while (true) {
            if (xQueueReceive(controller->sound_queue_, &sound, portMAX_DELAY) == pdTRUE) {
                Application::GetInstance().PlaySound(sound);
                controller->sound_busy_.store(false);
                controller->current_sound_action_type_.store(0);
            }
        }
    }

    // All callers pass embedded assets, whose data remains valid for this task.
    bool QueueSound(std::string_view sound, int sound_action_type = 0) {
        if (sound_queue_ == nullptr || sound.empty() || sound_busy_.exchange(true)) {
            return false;
        }
        current_sound_action_type_.store(sound_action_type);
        if (xQueueSend(sound_queue_, &sound, 0) != pdTRUE) {
            sound_busy_.store(false);
            current_sound_action_type_.store(0);
            return false;
        }
        return true;
    }

    struct OttoActionParams {
        int action_type;
        int steps;
        int speed;
        int direction;
        int amount;
    };

    std::string_view GetActionSound(int action_type) const {
        switch (action_type) {
            case ACTION_WALK:
                return Lang::Sounds::OGG_ACTION_WALK;
            case ACTION_SWING:
                return Lang::Sounds::OGG_ACTION_SWING;
            case ACTION_LAUGH:
                return Lang::Sounds::OGG_NAILONG_LAUGH;
            case ACTION_TURN:
            case ACTION_JUMP:
                return Lang::Sounds::OGG_ACTION_TURN_JUMP;
            case ACTION_BEND:
            case ACTION_SHAKE_LEG:
                return Lang::Sounds::OGG_ACTION_BEND_SHAKE_LEG;
            case ACTION_MOONWALK:
            case ACTION_UPDOWN:
            case ACTION_TIPTOE_SWING:
            case ACTION_JITTER:
            case ACTION_ASCENDING_TURN:
            case ACTION_CRUSAITO:
            case ACTION_FLAPPING:
            case ACTION_HANDS_UP:
            case ACTION_HANDS_DOWN:
            case ACTION_HAND_WAVE:
                return Lang::Sounds::OGG_ACTION_OTHER;
            case ACTION_HOME:
            default:
                return {};
        }
    }

    static void ActionTask(void* arg) {
        OttoController* controller = static_cast<OttoController*>(arg);
        OttoActionParams params;
        controller->otto_.AttachServos();

        while (true) {
            if (xQueueReceive(controller->action_queue_, &params, pdMS_TO_TICKS(1000)) == pdTRUE) {
                ESP_LOGI(TAG, "执行动作: %d", params.action_type);
                controller->current_action_type_.store(params.action_type);
                controller->current_action_steps_.store(params.steps);
                controller->current_action_speed_.store(params.speed);
                controller->current_action_direction_.store(params.direction);
                controller->current_action_amount_.store(params.amount);
                controller->action_started_at_ms_.store(esp_timer_get_time() / 1000);
                controller->is_action_in_progress_.store(true);
                if (auto* display = GetOttoDisplay()) {
                    const bool shown = display->ShowAction(ActionImageName(params.action_type));
                    controller->display_action_active_.store(shown);
                    controller->display_action_type_.store(shown ? params.action_type : 0);
                }

                // Enqueue without waiting for audio decoding so motion starts immediately.
                // A laugh has no servo movement, so keep its action lifecycle active until
                // the local sound has really drained. This gives the controller a real
                // moving -> idle edge and prevents the dispatcher from holding a phantom
                // command until its safety timeout.
                auto action_sound = controller->GetActionSound(params.action_type);
                bool sound_queued = false;
                if (!action_sound.empty()) {
                    if (params.action_type == ACTION_LAUGH) {
                        constexpr int kSoundRetryDelayMs = 20;
                        constexpr int kSoundRetryAttempts = 250;
                        for (int attempt = 0; attempt < kSoundRetryAttempts; ++attempt) {
                            if (controller->QueueSound(action_sound, params.action_type)) {
                                sound_queued = true;
                                break;
                            }
                            vTaskDelay(pdMS_TO_TICKS(kSoundRetryDelayMs));
                        }
                    } else {
                        sound_queued = controller->QueueSound(action_sound, params.action_type);
                    }
                }

                switch (params.action_type) {
                    case ACTION_WALK:
                        controller->otto_.Walk(params.steps, params.speed, params.direction,
                                               params.amount);
                        break;
                    case ACTION_TURN:
                        controller->otto_.Turn(params.steps, params.speed, params.direction,
                                               params.amount);
                        break;
                    case ACTION_JUMP:
                        controller->otto_.Jump(params.steps, params.speed);
                        break;
                    case ACTION_SWING:
                        controller->otto_.Swing(params.steps, params.speed, params.amount);
                        break;
                    case ACTION_MOONWALK:
                        controller->otto_.Moonwalker(params.steps, params.speed, params.amount,
                                                     params.direction);
                        break;
                    case ACTION_BEND:
                        controller->otto_.Bend(params.steps, params.speed, params.direction);
                        break;
                    case ACTION_SHAKE_LEG:
                        controller->otto_.ShakeLeg(params.steps, params.speed, params.direction);
                        break;
                    case ACTION_UPDOWN:
                        controller->otto_.UpDown(params.steps, params.speed, params.amount);
                        break;
                    case ACTION_TIPTOE_SWING:
                        controller->otto_.TiptoeSwing(params.steps, params.speed, params.amount);
                        break;
                    case ACTION_JITTER:
                        controller->otto_.Jitter(params.steps, params.speed, params.amount);
                        break;
                    case ACTION_ASCENDING_TURN:
                        controller->otto_.AscendingTurn(params.steps, params.speed, params.amount);
                        break;
                    case ACTION_CRUSAITO:
                        controller->otto_.Crusaito(params.steps, params.speed, params.amount,
                                                   params.direction);
                        break;
                    case ACTION_FLAPPING:
                        controller->otto_.Flapping(params.steps, params.speed, params.amount,
                                                   params.direction);
                        break;
                    case ACTION_HANDS_UP:
                        if (controller->has_hands_) {
                            controller->otto_.HandsUp(params.speed, params.direction);
                        }
                        break;
                    case ACTION_HANDS_DOWN:
                        if (controller->has_hands_) {
                            controller->otto_.HandsDown(params.speed, params.direction);
                        }
                        break;
                    case ACTION_HAND_WAVE:
                        if (controller->has_hands_) {
                            controller->otto_.HandWave(params.speed, params.direction);
                        }
                        break;
                    case ACTION_HOME:
                        controller->otto_.Home(params.direction == 1);
                        break;
                    case ACTION_LAUGH:
                        break;
                }
                if (params.action_type == ACTION_LAUGH) {
                    if (!sound_queued) {
                        ESP_LOGW(TAG, "奶龙笑声音频在等待5秒后仍未能入队");
                    } else {
                        while (controller->sound_busy_.load()) {
                            vTaskDelay(pdMS_TO_TICKS(10));
                        }
                    }
                }
                if (params.action_type != ACTION_HOME && params.action_type != ACTION_LAUGH) {
                    controller->otto_.Home(params.action_type < ACTION_HANDS_UP);
                }
                controller->last_action_type_.store(params.action_type);
                controller->last_action_steps_.store(params.steps);
                controller->last_action_speed_.store(params.speed);
                controller->last_action_direction_.store(params.direction);
                controller->last_action_amount_.store(params.amount);
                controller->last_action_finished_at_ms_.store(esp_timer_get_time() / 1000);
                controller->current_action_type_.store(0);
                controller->is_action_in_progress_.store(false);
                if (auto* display = GetOttoDisplay()) {
                    display->ClearAction();
                }
                controller->display_action_active_.store(false);
                controller->display_action_type_.store(0);
                vTaskDelay(pdMS_TO_TICKS(20));
            }
        }
    }

    void StartActionTaskIfNeeded() {
        if (action_task_handle_ == nullptr) {
            // Audio input/output must preempt servo interpolation so speech and
            // local action sounds do not stutter while a movement is running.
            xTaskCreate(ActionTask, "otto_action", 1024 * 3, this, 3, &action_task_handle_);
        }
    }

    void QueueAction(int action_type, int steps, int speed, int direction, int amount) {
        // 检查手部动作
        if ((action_type >= ACTION_HANDS_UP && action_type <= ACTION_HAND_WAVE) && !has_hands_) {
            ESP_LOGW(TAG, "尝试执行手部动作，但机器人没有配置手部舵机");
            return;
        }

        ESP_LOGI(TAG, "动作控制: 类型=%d, 步数=%d, 速度=%d, 方向=%d, 幅度=%d", action_type, steps,
                 speed, direction, amount);

        OttoActionParams params = {action_type, steps, speed, direction, amount};
        xQueueSend(action_queue_, &params, portMAX_DELAY);
        StartActionTaskIfNeeded();
    }

    void LoadTrimsFromNVS() {
        Settings settings("otto_trims", false);

        int left_leg = settings.GetInt("left_leg", 0);
        int right_leg = settings.GetInt("right_leg", 0);
        int left_foot = settings.GetInt("left_foot", 0);
        int right_foot = settings.GetInt("right_foot", 0);
        int left_hand = settings.GetInt("left_hand", 0);
        int right_hand = settings.GetInt("right_hand", 0);

        ESP_LOGI(TAG, "从NVS加载微调设置: 左腿=%d, 右腿=%d, 左脚=%d, 右脚=%d, 左手=%d, 右手=%d",
                 left_leg, right_leg, left_foot, right_foot, left_hand, right_hand);

        otto_.SetTrims(left_leg, right_leg, left_foot, right_foot, left_hand, right_hand);
    }

public:
    bool IsIdleForConversation() const {
        return !is_action_in_progress_.load() && !sound_busy_.load() &&
               (action_queue_ == nullptr || uxQueueMessagesWaiting(action_queue_) == 0);
    }

    bool QueueActionByName(const std::string& action_name, int steps, int speed, int direction,
                           int amount, std::string& error) {
        const int action_type = ActionTypeFromName(action_name);
        if (action_type == 0) {
            error = "未知动作: " + action_name;
            return false;
        }
        if (steps < 1 || steps > 100) {
            error = "steps 必须在 1-100 之间";
            return false;
        }
        if (speed < 500 || speed > 1500) {
            error = "speed 必须在 500-1500 之间";
            return false;
        }
        if (direction < -1 || direction > 1) {
            error = "direction 必须是 -1、0 或 1";
            return false;
        }
        if (amount < 0 || amount > 170) {
            error = "amount 必须在 0-170 之间";
            return false;
        }
        if (action_type >= ACTION_HANDS_UP && action_type <= ACTION_HAND_WAVE && !has_hands_) {
            error = "当前 Otto 没有配置手部舵机";
            return false;
        }
        QueueAction(action_type, steps, speed, direction, amount);
        return true;
    }

    void StopAction() {
        if (action_task_handle_ != nullptr) {
            vTaskDelete(action_task_handle_);
            action_task_handle_ = nullptr;
        }
        is_action_in_progress_.store(false);
        current_action_type_.store(0);
        if (auto* display = GetOttoDisplay()) {
            display->ClearAction();
        }
        display_action_active_.store(false);
        display_action_type_.store(0);
        if (action_queue_ != nullptr) {
            xQueueReset(action_queue_);
        }
        QueueAction(ACTION_HOME, 1, 1000, 1, 0);
    }

    std::string GetAvailableActionsJson() const {
        cJSON* root = cJSON_CreateObject();
        cJSON* actions = cJSON_CreateArray();
        auto add_action = [actions](const char* name) {
            cJSON* action = cJSON_CreateObject();
            cJSON_AddStringToObject(action, "name", name);
            cJSON* parameters = cJSON_CreateObject();
            auto add_integer = [parameters](const char* parameter_name, int minimum,
                                            int maximum, int default_value) {
                cJSON* descriptor = cJSON_CreateObject();
                cJSON_AddStringToObject(descriptor, "type", "integer");
                cJSON_AddNumberToObject(descriptor, "minimum", minimum);
                cJSON_AddNumberToObject(descriptor, "maximum", maximum);
                cJSON_AddNumberToObject(descriptor, "default", default_value);
                cJSON_AddItemToObject(parameters, parameter_name, descriptor);
            };
            add_integer("steps", 1, 100, 3);
            add_integer("speed", 500, 1500, 1000);
            add_integer("direction", -1, 1, 1);
            add_integer("amount", 0, 170, 50);
            cJSON_AddItemToObject(action, "parameters", parameters);
            cJSON_AddItemToArray(actions, action);
        };
        const int action_types[] = {
            ACTION_WALK, ACTION_TURN, ACTION_JUMP, ACTION_SWING, ACTION_MOONWALK,
            ACTION_BEND, ACTION_SHAKE_LEG, ACTION_UPDOWN, ACTION_TIPTOE_SWING, ACTION_JITTER,
            ACTION_ASCENDING_TURN, ACTION_CRUSAITO, ACTION_FLAPPING, ACTION_LAUGH,
        };
        for (int action_type : action_types) {
            add_action(ActionName(action_type));
        }
        if (has_hands_) {
            add_action(ActionName(ACTION_HANDS_UP));
            add_action(ActionName(ACTION_HANDS_DOWN));
            add_action(ActionName(ACTION_HAND_WAVE));
        }
        add_action(ActionName(ACTION_HOME));
        cJSON_AddItemToObject(root, "actions", actions);
        char* text = cJSON_PrintUnformatted(root);
        std::string result = text == nullptr ? "{\"actions\":[]}" : text;
        if (text != nullptr) {
            cJSON_free(text);
        }
        cJSON_Delete(root);
        return result;
    }

    std::string GetRuntimeStatusJson() const {
        cJSON* root = cJSON_CreateObject();
        const bool moving = is_action_in_progress_.load();
        const int current_type = current_action_type_.load();
        const int last_type = last_action_type_.load();

        cJSON* action = cJSON_CreateObject();
        cJSON_AddStringToObject(action, "state", moving ? "moving" : "idle");
        cJSON_AddStringToObject(action, "name", ActionName(current_type));
        cJSON_AddNumberToObject(action, "type", current_type);
        cJSON_AddNumberToObject(action, "steps", current_action_steps_.load());
        cJSON_AddNumberToObject(action, "speed", current_action_speed_.load());
        cJSON_AddNumberToObject(action, "direction", current_action_direction_.load());
        cJSON_AddNumberToObject(action, "amount", current_action_amount_.load());
        cJSON_AddNumberToObject(action, "started_at_ms",
                                static_cast<double>(action_started_at_ms_.load()));
        cJSON_AddItemToObject(root, "action", action);

        cJSON* last_action = cJSON_CreateObject();
        cJSON_AddStringToObject(last_action, "name", ActionName(last_type));
        cJSON_AddNumberToObject(last_action, "type", last_type);
        cJSON_AddNumberToObject(last_action, "steps", last_action_steps_.load());
        cJSON_AddNumberToObject(last_action, "speed", last_action_speed_.load());
        cJSON_AddNumberToObject(last_action, "direction", last_action_direction_.load());
        cJSON_AddNumberToObject(last_action, "amount", last_action_amount_.load());
        cJSON_AddNumberToObject(last_action, "finished_at_ms",
                                static_cast<double>(last_action_finished_at_ms_.load()));
        cJSON_AddItemToObject(root, "last_action", last_action);

        cJSON_AddNumberToObject(root, "queue_depth",
                                action_queue_ == nullptr ? 0 : uxQueueMessagesWaiting(action_queue_));
        cJSON* sound = cJSON_CreateObject();
        cJSON_AddBoolToObject(sound, "busy", sound_busy_.load());
        cJSON_AddStringToObject(sound, "action_sound",
                                ActionName(current_sound_action_type_.load()));
        cJSON_AddItemToObject(root, "sound", sound);

        cJSON* display = cJSON_CreateObject();
        const bool display_action_active = display_action_active_.load();
        const int display_action_type = display_action_type_.load();
        cJSON_AddBoolToObject(display, "action_image_active", display_action_active);
        cJSON_AddStringToObject(
            display, "image_alias",
            display_action_active ? ActionImageName(display_action_type) : "base_emotion");
        cJSON_AddItemToObject(root, "display", display);

        cJSON* audio = cJSON_CreateObject();
        cJSON_AddNumberToObject(
            audio, "output_volume", Board::GetInstance().GetAudioCodec()->output_volume());
        cJSON_AddItemToObject(root, "audio", audio);

        char* text = cJSON_PrintUnformatted(root);
        std::string result = text == nullptr ? "{}" : text;
        if (text != nullptr) {
            cJSON_free(text);
        }
        cJSON_Delete(root);
        return result;
    }

    OttoController() {
        otto_.Init(LEFT_LEG_PIN, RIGHT_LEG_PIN, LEFT_FOOT_PIN, RIGHT_FOOT_PIN, LEFT_HAND_PIN,
                   RIGHT_HAND_PIN);

        has_hands_ = (LEFT_HAND_PIN != -1 && RIGHT_HAND_PIN != -1);
        ESP_LOGI(TAG, "Otto机器人初始化%s手部舵机", has_hands_ ? "带" : "不带");

        LoadTrimsFromNVS();

        action_queue_ = xQueueCreate(10, sizeof(OttoActionParams));
        sound_queue_ = xQueueCreate(1, sizeof(std::string_view));
        if (sound_queue_ != nullptr &&
            xTaskCreate(SoundTask, "otto_sound", 4096, this, 3, nullptr) != pdPASS) {
            vQueueDelete(sound_queue_);
            sound_queue_ = nullptr;
        }
        if (sound_queue_ == nullptr) {
            ESP_LOGE(TAG, "Failed to create sound playback task");
        }

        QueueAction(ACTION_HOME, 1, 1000, 1, 0);  // direction=1表示复位手部

        RegisterMcpTools();
    }

    void RegisterMcpTools() {
        auto& mcp_server = McpServer::GetInstance();

        ESP_LOGI(TAG, "开始注册MCP工具...");

        mcp_server.AddTool("self.otto.laugh",
                          "播放一次本地奶龙大笑音频，不移动舵机。听不懂用户的话，或用户要求奶龙大笑时调用。"
                          "成功仅表示开始安排播放，不要再朗读笑声或声称已播放完毕。",
                          PropertyList(), [this](const PropertyList&) -> ReturnValue {
                              if (!QueueSound(Lang::Sounds::OGG_NAILONG_LAUGH,
                                              ACTION_LAUGH)) {
                                  return std::string("音效忙或暂不可用，本次未安排播放");
                              }
                              return std::string("已安排播放奶龙大笑，无需额外语音回复");
                          });

        // 基础移动动作
        mcp_server.AddTool("self.otto.walk_forward",
                           "行走。steps: 行走步数(1-100); speed: 行走速度(500-1500，数值越小越快); "
                           "direction: 行走方向(-1=后退, 1=前进); arm_swing: 手臂摆动幅度(0-170度)",
                           PropertyList({Property("steps", kPropertyTypeInteger, 3, 1, 100),
                                         Property("speed", kPropertyTypeInteger, 1000, 500, 1500),
                                         Property("arm_swing", kPropertyTypeInteger, 50, 0, 170),
                                         Property("direction", kPropertyTypeInteger, 1, -1, 1)}),
                           [this](const PropertyList& properties) -> ReturnValue {
                               int steps = properties["steps"].value<int>();
                               int speed = properties["speed"].value<int>();
                               int arm_swing = properties["arm_swing"].value<int>();
                               int direction = properties["direction"].value<int>();
                               QueueAction(ACTION_WALK, steps, speed, direction, arm_swing);
                               return true;
                           });

        mcp_server.AddTool("self.otto.turn_left",
                           "转身。steps: 转身步数(1-100); speed: 转身速度(500-1500，数值越小越快); "
                           "direction: 转身方向(1=左转, -1=右转); arm_swing: 手臂摆动幅度(0-170度)",
                           PropertyList({Property("steps", kPropertyTypeInteger, 3, 1, 100),
                                         Property("speed", kPropertyTypeInteger, 1000, 500, 1500),
                                         Property("arm_swing", kPropertyTypeInteger, 50, 0, 170),
                                         Property("direction", kPropertyTypeInteger, 1, -1, 1)}),
                           [this](const PropertyList& properties) -> ReturnValue {
                               int steps = properties["steps"].value<int>();
                               int speed = properties["speed"].value<int>();
                               int arm_swing = properties["arm_swing"].value<int>();
                               int direction = properties["direction"].value<int>();
                               QueueAction(ACTION_TURN, steps, speed, direction, arm_swing);
                               return true;
                           });

        mcp_server.AddTool("self.otto.jump",
                           "跳跃。steps: 跳跃次数(1-100); speed: 跳跃速度(500-1500，数值越小越快)",
                           PropertyList({Property("steps", kPropertyTypeInteger, 1, 1, 100),
                                         Property("speed", kPropertyTypeInteger, 1000, 500, 1500)}),
                           [this](const PropertyList& properties) -> ReturnValue {
                               int steps = properties["steps"].value<int>();
                               int speed = properties["speed"].value<int>();
                               QueueAction(ACTION_JUMP, steps, speed, 0, 0);
                               return true;
                           });

        // 特殊动作
        mcp_server.AddTool("self.otto.swing",
                           "左右摇摆。steps: 摇摆次数(1-100); speed: "
                           "摇摆速度(500-1500，数值越小越快); amount: 摇摆幅度(0-170度)",
                           PropertyList({Property("steps", kPropertyTypeInteger, 3, 1, 100),
                                         Property("speed", kPropertyTypeInteger, 1000, 500, 1500),
                                         Property("amount", kPropertyTypeInteger, 30, 0, 170)}),
                           [this](const PropertyList& properties) -> ReturnValue {
                               int steps = properties["steps"].value<int>();
                               int speed = properties["speed"].value<int>();
                               int amount = properties["amount"].value<int>();
                               QueueAction(ACTION_SWING, steps, speed, 0, amount);
                               return true;
                           });

        mcp_server.AddTool("self.otto.moonwalk",
                           "太空步。steps: 太空步步数(1-100); speed: 速度(500-1500，数值越小越快); "
                           "direction: 方向(1=左, -1=右); amount: 幅度(0-170度)",
                           PropertyList({Property("steps", kPropertyTypeInteger, 3, 1, 100),
                                         Property("speed", kPropertyTypeInteger, 1000, 500, 1500),
                                         Property("direction", kPropertyTypeInteger, 1, -1, 1),
                                         Property("amount", kPropertyTypeInteger, 25, 0, 170)}),
                           [this](const PropertyList& properties) -> ReturnValue {
                               int steps = properties["steps"].value<int>();
                               int speed = properties["speed"].value<int>();
                               int direction = properties["direction"].value<int>();
                               int amount = properties["amount"].value<int>();
                               QueueAction(ACTION_MOONWALK, steps, speed, direction, amount);
                               return true;
                           });

        mcp_server.AddTool("self.otto.bend",
                           "弯曲身体。steps: 弯曲次数(1-100); speed: "
                           "弯曲速度(500-1500，数值越小越快); direction: 弯曲方向(1=左, -1=右)",
                           PropertyList({Property("steps", kPropertyTypeInteger, 1, 1, 100),
                                         Property("speed", kPropertyTypeInteger, 1000, 500, 1500),
                                         Property("direction", kPropertyTypeInteger, 1, -1, 1)}),
                           [this](const PropertyList& properties) -> ReturnValue {
                               int steps = properties["steps"].value<int>();
                               int speed = properties["speed"].value<int>();
                               int direction = properties["direction"].value<int>();
                               QueueAction(ACTION_BEND, steps, speed, direction, 0);
                               return true;
                           });

        mcp_server.AddTool("self.otto.shake_leg",
                           "摇腿。steps: 摇腿次数(1-100); speed: 摇腿速度(500-1500，数值越小越快); "
                           "direction: 腿部选择(1=左腿, -1=右腿)",
                           PropertyList({Property("steps", kPropertyTypeInteger, 1, 1, 100),
                                         Property("speed", kPropertyTypeInteger, 1000, 500, 1500),
                                         Property("direction", kPropertyTypeInteger, 1, -1, 1)}),
                           [this](const PropertyList& properties) -> ReturnValue {
                               int steps = properties["steps"].value<int>();
                               int speed = properties["speed"].value<int>();
                               int direction = properties["direction"].value<int>();
                               QueueAction(ACTION_SHAKE_LEG, steps, speed, direction, 0);
                               return true;
                           });

        mcp_server.AddTool("self.otto.updown",
                           "上下运动。steps: 上下运动次数(1-100); speed: "
                           "运动速度(500-1500，数值越小越快); amount: 运动幅度(0-170度)",
                           PropertyList({Property("steps", kPropertyTypeInteger, 3, 1, 100),
                                         Property("speed", kPropertyTypeInteger, 1000, 500, 1500),
                                         Property("amount", kPropertyTypeInteger, 20, 0, 170)}),
                           [this](const PropertyList& properties) -> ReturnValue {
                               int steps = properties["steps"].value<int>();
                               int speed = properties["speed"].value<int>();
                               int amount = properties["amount"].value<int>();
                               QueueAction(ACTION_UPDOWN, steps, speed, 0, amount);
                               return true;
                           });

        // 手部动作（仅在有手部舵机时可用）
        if (has_hands_) {
            mcp_server.AddTool(
                "self.otto.hands_up",
                "举手。speed: 举手速度(500-1500，数值越小越快); direction: 手部选择(1=左手, "
                "-1=右手, 0=双手)",
                PropertyList({Property("speed", kPropertyTypeInteger, 1000, 500, 1500),
                              Property("direction", kPropertyTypeInteger, 1, -1, 1)}),
                [this](const PropertyList& properties) -> ReturnValue {
                    int speed = properties["speed"].value<int>();
                    int direction = properties["direction"].value<int>();
                    QueueAction(ACTION_HANDS_UP, 1, speed, direction, 0);
                    return true;
                });

            mcp_server.AddTool(
                "self.otto.hands_down",
                "放手。speed: 放手速度(500-1500，数值越小越快); direction: 手部选择(1=左手, "
                "-1=右手, 0=双手)",
                PropertyList({Property("speed", kPropertyTypeInteger, 1000, 500, 1500),
                              Property("direction", kPropertyTypeInteger, 1, -1, 1)}),
                [this](const PropertyList& properties) -> ReturnValue {
                    int speed = properties["speed"].value<int>();
                    int direction = properties["direction"].value<int>();
                    QueueAction(ACTION_HANDS_DOWN, 1, speed, direction, 0);
                    return true;
                });

            mcp_server.AddTool(
                "self.otto.hand_wave",
                "挥手。speed: 挥手速度(500-1500，数值越小越快); direction: 手部选择(1=左手, "
                "-1=右手, 0=双手)",
                PropertyList({Property("speed", kPropertyTypeInteger, 1000, 500, 1500),
                              Property("direction", kPropertyTypeInteger, 1, -1, 1)}),
                [this](const PropertyList& properties) -> ReturnValue {
                    int speed = properties["speed"].value<int>();
                    int direction = properties["direction"].value<int>();
                    QueueAction(ACTION_HAND_WAVE, 1, speed, direction, 0);
                    return true;
                });
        }

        // 系统工具
        mcp_server.AddTool("self.otto.stop", "立即停止", PropertyList(),
                           [this](const PropertyList& properties) -> ReturnValue {
                               StopAction();
                               return true;
                           });

        mcp_server.AddTool(
            "self.otto.set_trim",
            "校准单个舵机位置。设置指定舵机的微调参数以调整Otto的初始站立姿态，设置将永久保存。"
            "servo_type: 舵机类型(left_leg/right_leg/left_foot/right_foot/left_hand/right_hand); "
            "trim_value: 微调值(-50到50度)",
            PropertyList({Property("servo_type", kPropertyTypeString, "left_leg"),
                          Property("trim_value", kPropertyTypeInteger, 0, -50, 50)}),
            [this](const PropertyList& properties) -> ReturnValue {
                std::string servo_type = properties["servo_type"].value<std::string>();
                int trim_value = properties["trim_value"].value<int>();

                ESP_LOGI(TAG, "设置舵机微调: %s = %d度", servo_type.c_str(), trim_value);

                // 获取当前所有微调值
                Settings settings("otto_trims", true);
                int left_leg = settings.GetInt("left_leg", 0);
                int right_leg = settings.GetInt("right_leg", 0);
                int left_foot = settings.GetInt("left_foot", 0);
                int right_foot = settings.GetInt("right_foot", 0);
                int left_hand = settings.GetInt("left_hand", 0);
                int right_hand = settings.GetInt("right_hand", 0);

                // 更新指定舵机的微调值
                if (servo_type == "left_leg") {
                    left_leg = trim_value;
                    settings.SetInt("left_leg", left_leg);
                } else if (servo_type == "right_leg") {
                    right_leg = trim_value;
                    settings.SetInt("right_leg", right_leg);
                } else if (servo_type == "left_foot") {
                    left_foot = trim_value;
                    settings.SetInt("left_foot", left_foot);
                } else if (servo_type == "right_foot") {
                    right_foot = trim_value;
                    settings.SetInt("right_foot", right_foot);
                } else if (servo_type == "left_hand") {
                    if (!has_hands_) {
                        return "错误：机器人没有配置手部舵机";
                    }
                    left_hand = trim_value;
                    settings.SetInt("left_hand", left_hand);
                } else if (servo_type == "right_hand") {
                    if (!has_hands_) {
                        return "错误：机器人没有配置手部舵机";
                    }
                    right_hand = trim_value;
                    settings.SetInt("right_hand", right_hand);
                } else {
                    return "错误：无效的舵机类型，请使用: left_leg, right_leg, left_foot, "
                           "right_foot, left_hand, right_hand";
                }

                otto_.SetTrims(left_leg, right_leg, left_foot, right_foot, left_hand, right_hand);

                QueueAction(ACTION_JUMP, 1, 500, 0, 0);

                return "舵机 " + servo_type + " 微调设置为 " + std::to_string(trim_value) +
                       " 度，已永久保存";
            });

        mcp_server.AddTool("self.otto.get_trims", "获取当前的舵机微调设置", PropertyList(),
                           [this](const PropertyList& properties) -> ReturnValue {
                               Settings settings("otto_trims", false);

                               int left_leg = settings.GetInt("left_leg", 0);
                               int right_leg = settings.GetInt("right_leg", 0);
                               int left_foot = settings.GetInt("left_foot", 0);
                               int right_foot = settings.GetInt("right_foot", 0);
                               int left_hand = settings.GetInt("left_hand", 0);
                               int right_hand = settings.GetInt("right_hand", 0);

                               std::string result =
                                   "{\"left_leg\":" + std::to_string(left_leg) +
                                   ",\"right_leg\":" + std::to_string(right_leg) +
                                   ",\"left_foot\":" + std::to_string(left_foot) +
                                   ",\"right_foot\":" + std::to_string(right_foot) +
                                   ",\"left_hand\":" + std::to_string(left_hand) +
                                   ",\"right_hand\":" + std::to_string(right_hand) + "}";

                               ESP_LOGI(TAG, "获取微调设置: %s", result.c_str());
                               return result;
                           });

        mcp_server.AddTool("self.otto.get_status", "获取机器人状态，返回 moving 或 idle",
                           PropertyList(), [this](const PropertyList& properties) -> ReturnValue {
                               return is_action_in_progress_.load() ? "moving" : "idle";
                           });

        mcp_server.AddTool("self.battery.get_level", "获取机器人电池电量和充电状态", PropertyList(),
                           [](const PropertyList& properties) -> ReturnValue {
                               auto& board = Board::GetInstance();
                               int level = 0;
                               bool charging = false;
                               bool discharging = false;
                               board.GetBatteryLevel(level, charging, discharging);

                               std::string status =
                                   "{\"level\":" + std::to_string(level) +
                                   ",\"charging\":" + (charging ? "true" : "false") + "}";
                               return status;
                           });

        ESP_LOGI(TAG, "MCP工具注册完成");
    }

    ~OttoController() {
        if (action_task_handle_ != nullptr) {
            vTaskDelete(action_task_handle_);
            action_task_handle_ = nullptr;
        }
        vQueueDelete(action_queue_);
    }
};

static OttoController* g_otto_controller = nullptr;

bool QueueOttoAction(const std::string& action_name, int steps, int speed, int direction,
                     int amount, std::string& error) {
    if (g_otto_controller == nullptr) {
        error = "Otto 控制器尚未初始化";
        return false;
    }
    return g_otto_controller->QueueActionByName(action_name, steps, speed, direction, amount,
                                                  error);
}

bool StopOttoAction(std::string& error) {
    if (g_otto_controller == nullptr) {
        error = "Otto 控制器尚未初始化";
        return false;
    }
    g_otto_controller->StopAction();
    return true;
}

bool IsOttoIdleForConversation() {
    if (g_otto_controller == nullptr) {
        return false;
    }
    return g_otto_controller->IsIdleForConversation();
}

std::string GetOttoAvailableActionsJson() {
    if (g_otto_controller == nullptr) {
        return R"({"actions":[]})";
    }
    return g_otto_controller->GetAvailableActionsJson();
}

std::string GetOttoRuntimeStatusJson() {
    if (g_otto_controller == nullptr) {
        return R"({"action":{"state":"uninitialized","name":"unknown"},"queue_depth":0})";
    }
    return g_otto_controller->GetRuntimeStatusJson();
}

void InitializeOttoController() {
    if (g_otto_controller == nullptr) {
        g_otto_controller = new OttoController();
        ESP_LOGI(TAG, "Otto控制器已初始化并注册MCP工具");
    }
}
