#include "mqtt_protocol.h"
#include "board.h"
#include "application.h"
#include "boards/otto-robot/otto_controller_status.h"
#include "otto_master_link.h"
#include "settings.h"
#include "system_info.h"

#include <esp_app_desc.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <arpa/inet.h>
#include "assets/lang_config.h"

#define TAG "MQTT"

namespace {

std::string PrintJson(cJSON* root) {
    char* text = cJSON_PrintUnformatted(root);
    std::string result = text == nullptr ? "{}" : text;
    if (text != nullptr) {
        cJSON_free(text);
    }
    cJSON_Delete(root);
    return result;
}

std::string DeviceName() {
    Settings settings("otto", false);
    const std::string configured = settings.GetString("name");
    return configured.empty() ? BOARD_NAME : configured;
}

bool ParseMqttEndpoint(const std::string& endpoint, std::string& host, int& port) {
    std::string value = endpoint;
    port = 8883;
    if (value.rfind("mqtt://", 0) == 0) {
        value.erase(0, 7);
        port = 1883;
    } else if (value.rfind("mqtts://", 0) == 0) {
        value.erase(0, 8);
        port = 8883;
    }

    const size_t path = value.find('/');
    if (path != std::string::npos) {
        if (value.find_first_not_of('/', path) != std::string::npos) {
            return false;
        }
        value.erase(path);
    }
    if (value.empty()) {
        return false;
    }

    std::string port_text;
    if (value.front() == '[') {
        const size_t closing = value.find(']');
        if (closing == std::string::npos) {
            return false;
        }
        host = value.substr(1, closing - 1);
        if (closing + 1 < value.size()) {
            if (value[closing + 1] != ':') {
                return false;
            }
            port_text = value.substr(closing + 2);
        }
    } else {
        const size_t colon = value.rfind(':');
        if (colon != std::string::npos && value.find(':') == colon) {
            host = value.substr(0, colon);
            port_text = value.substr(colon + 1);
        } else {
            host = value;
        }
    }
    if (host.empty()) {
        return false;
    }
    if (!port_text.empty()) {
        char* end = nullptr;
        const long parsed = std::strtol(port_text.c_str(), &end, 10);
        if (end == port_text.c_str() || *end != '\0' || parsed < 1 || parsed > 65535) {
            return false;
        }
        port = static_cast<int>(parsed);
    }
    return true;
}

bool IsOttoMasterMdnsEndpoint(const std::string& endpoint, std::string host) {
    if (endpoint.rfind("mqtt://", 0) != 0) {
        // Do not replace the hostname of a TLS endpoint: certificate/SNI
        // validation may require the original DNS name.
        return false;
    }
    while (!host.empty() && host.back() == '.') {
        host.pop_back();
    }
    for (char& character : host) {
        character = static_cast<char>(
            std::tolower(static_cast<unsigned char>(character)));
    }
    return host == "master.local";
}

std::string BuildFleetPresence(const char* type) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", type);
    cJSON_AddStringToObject(root, "mac", SystemInfo::GetMacAddress().c_str());
    cJSON_AddStringToObject(root, "name", DeviceName().c_str());
    cJSON_AddNumberToObject(root, "uptime_ms",
                            static_cast<double>(esp_timer_get_time() / 1000));

    if (strcmp(type, "hello") == 0) {
        cJSON_AddStringToObject(root, "protocol", "otto-mqtt/1");
        cJSON_AddStringToObject(root, "firmware_version",
                                esp_app_get_description()->version);
        cJSON* capabilities = cJSON_CreateObject();
        cJSON_AddBoolToObject(capabilities, "actions", true);
        cJSON_AddBoolToObject(capabilities, "state", true);
        cJSON_AddBoolToObject(capabilities, "stop", true);
        cJSON_AddBoolToObject(capabilities, "command_dedup", true);
        cJSON_AddBoolToObject(capabilities, "conversation_control", true);
        cJSON_AddItemToObject(root, "capabilities", capabilities);
    }

    cJSON* board = cJSON_Parse(Board::GetInstance().GetBoardJson().c_str());
    if (board != nullptr) {
        cJSON* ip = cJSON_GetObjectItem(board, "ip");
        if (cJSON_IsString(ip) && ip->valuestring != nullptr) {
            cJSON_AddStringToObject(root, "ip_address", ip->valuestring);
        }
        cJSON_AddItemToObject(root, "board", board);
    }

    cJSON* runtime = cJSON_CreateObject();
    cJSON* otto = cJSON_Parse(GetOttoRuntimeStatusJson().c_str());
    if (otto != nullptr) {
        cJSON_AddItemToObject(runtime, "otto", otto);
    }
    cJSON* voice = cJSON_Parse(Application::GetInstance().GetVoiceStatusJson().c_str());
    if (voice != nullptr) {
        cJSON_AddItemToObject(runtime, "voice", voice);
    }
    cJSON_AddItemToObject(root, "runtime", runtime);
    return PrintJson(root);
}

}  // namespace

MqttProtocol::MqttProtocol() {
    event_group_handle_ = xEventGroupCreate();

    // Initialize reconnect timer
    esp_timer_create_args_t reconnect_timer_args = {
        .callback = [](void* arg) {
            MqttProtocol* protocol = (MqttProtocol*)arg;
            auto& app = Application::GetInstance();
            ESP_LOGI(TAG, "Reconnecting to MQTT server");
            app.Schedule([protocol]() {
                protocol->StartMqttClient(false);
            });
        },
        .arg = this,
    };
    esp_timer_create(&reconnect_timer_args, &reconnect_timer_);

    esp_timer_create_args_t heartbeat_timer_args = {
        .callback = [](void*) {
            Application::GetInstance().SendJsonMessage(BuildFleetPresence("heartbeat"));
        },
        .arg = this,
    };
    esp_timer_create(&heartbeat_timer_args, &fleet_heartbeat_timer_);
}

MqttProtocol::~MqttProtocol() {
    ESP_LOGI(TAG, "MqttProtocol deinit");
    if (reconnect_timer_ != nullptr) {
        esp_timer_stop(reconnect_timer_);
        esp_timer_delete(reconnect_timer_);
    }
    if (fleet_heartbeat_timer_ != nullptr) {
        esp_timer_stop(fleet_heartbeat_timer_);
        esp_timer_delete(fleet_heartbeat_timer_);
    }

    udp_.reset();
    mqtt_.reset();
    
    if (event_group_handle_ != nullptr) {
        vEventGroupDelete(event_group_handle_);
    }
}

bool MqttProtocol::Start() {
    return StartMqttClient(false);
}

void MqttProtocol::ScheduleReconnect() {
    if (reconnect_timer_ == nullptr) {
        return;
    }
    esp_timer_stop(reconnect_timer_);
    esp_timer_start_once(reconnect_timer_, MQTT_RECONNECT_INTERVAL_MS * 1000ULL);
}

bool MqttProtocol::SendFleetPresence(const char* type) {
    return SendText(BuildFleetPresence(type));
}

bool MqttProtocol::StartMqttClient(bool report_error) {
    if (mqtt_ != nullptr) {
        ESP_LOGW(TAG, "Mqtt client already started");
        mqtt_.reset();
    }

    Settings settings("mqtt", false);
    auto endpoint = settings.GetString("endpoint");
    auto client_id = settings.GetString("client_id");
    auto username = settings.GetString("username");
    auto password = settings.GetString("password");
    int keepalive_interval = settings.GetInt("keepalive", 240);
    publish_topic_ = settings.GetString("publish_topic");
    subscribe_topic_ = settings.GetString("subscribe_topic");

    if (endpoint.empty()) {
        ESP_LOGW(TAG, "MQTT endpoint is not specified");
        if (report_error) {
            SetError(Lang::Strings::SERVER_NOT_FOUND);
        }
        return false;
    }

    auto network = Board::GetInstance().GetNetwork();
    mqtt_ = network->CreateMqtt(0);
    mqtt_->SetKeepAlive(keepalive_interval);

    mqtt_->OnDisconnected([this]() {
        if (fleet_heartbeat_timer_ != nullptr) {
            esp_timer_stop(fleet_heartbeat_timer_);
        }
        if (on_disconnected_ != nullptr) {
            on_disconnected_();
        }
        ESP_LOGI(TAG, "MQTT disconnected, schedule reconnect in %d seconds", MQTT_RECONNECT_INTERVAL_MS / 1000);
        ScheduleReconnect();
    });

    mqtt_->OnConnected([this]() {
        if (on_connected_ != nullptr) {
            on_connected_();
        }
        esp_timer_stop(reconnect_timer_);
    });

    mqtt_->OnMessage([this](const std::string& topic, const std::string& payload) {
        cJSON* root = cJSON_Parse(payload.c_str());
        if (root == nullptr) {
            ESP_LOGE(TAG, "Failed to parse json message %s", payload.c_str());
            return;
        }
        cJSON* type = cJSON_GetObjectItem(root, "type");
        if (!cJSON_IsString(type)) {
            ESP_LOGE(TAG, "Message type is invalid");
            cJSON_Delete(root);
            return;
        }

        if (strcmp(type->valuestring, "hello") == 0) {
            ParseServerHello(root);
        } else if (strcmp(type->valuestring, "goodbye") == 0) {
            auto session_id = cJSON_GetObjectItem(root, "session_id");
            ESP_LOGI(TAG, "Received goodbye message, session_id: %s", session_id ? session_id->valuestring : "null");
            if (session_id == nullptr || session_id_ == session_id->valuestring) {
                Application::GetInstance().Schedule([this]() {
                    CloseAudioChannel();
                });
            }
        } else if (on_incoming_json_ != nullptr) {
            on_incoming_json_(root);
        }
        cJSON_Delete(root);
        last_incoming_time_ = std::chrono::steady_clock::now();
    });

    std::string broker_address;
    int broker_port = 0;
    if (!ParseMqttEndpoint(endpoint, broker_address, broker_port)) {
        ESP_LOGE(TAG, "Invalid MQTT endpoint");
        SetError(Lang::Strings::SERVER_NOT_FOUND);
        ScheduleReconnect();
        return false;
    }
    std::string connection_address = broker_address;
    if (IsOttoMasterMdnsEndpoint(endpoint, broker_address)) {
        if (!ResolveOttoMasterMdns(connection_address)) {
            ESP_LOGW(TAG, "Cannot resolve MQTT host %s through mDNS", broker_address.c_str());
            SetError(Lang::Strings::SERVER_NOT_FOUND);
            ScheduleReconnect();
            return false;
        }
    }
    ESP_LOGI(TAG, "Connecting to MQTT host %s (%s):%d", broker_address.c_str(),
             connection_address.c_str(), broker_port);
    if (!mqtt_->Connect(connection_address, broker_port, client_id, username, password)) {
        ESP_LOGE(TAG, "Failed to connect to endpoint");
        SetError(Lang::Strings::SERVER_NOT_CONNECTED);
        ScheduleReconnect();
        return false;
    }

    if (!subscribe_topic_.empty()) {
        if (!mqtt_->Subscribe(subscribe_topic_)) {
            ESP_LOGE(TAG, "Failed to subscribe to topic %s", subscribe_topic_.c_str());
            SetError(Lang::Strings::SERVER_NOT_CONNECTED);
            mqtt_->Disconnect();
            return false;
        }
        ESP_LOGI(TAG, "Subscribed to topic %s", subscribe_topic_.c_str());
    } else {
        ESP_LOGW(TAG, "No MQTT subscribe_topic configured; server downlink is disabled");
    }

    if (!SendFleetPresence("hello")) {
        ESP_LOGE(TAG, "Failed to publish fleet hello");
        mqtt_->Disconnect();
        return false;
    }
    if (fleet_heartbeat_timer_ != nullptr) {
        esp_timer_stop(fleet_heartbeat_timer_);
        esp_timer_start_periodic(fleet_heartbeat_timer_,
                                 OTTO_FLEET_HEARTBEAT_INTERVAL_MS * 1000ULL);
    }

    ESP_LOGI(TAG, "Connected to endpoint and published fleet hello");
    return true;
}

bool MqttProtocol::SendText(const std::string& text) {
    if (publish_topic_.empty() || mqtt_ == nullptr || !mqtt_->IsConnected()) {
        return false;
    }
    if (!mqtt_->Publish(publish_topic_, text)) {
        ESP_LOGE(TAG, "Failed to publish MQTT message (%u bytes)",
                 static_cast<unsigned>(text.size()));
        SetError(Lang::Strings::SERVER_ERROR);
        return false;
    }
    return true;
}

bool MqttProtocol::SendAudio(std::unique_ptr<AudioStreamPacket> packet) {
    std::lock_guard<std::mutex> lock(channel_mutex_);
    if (udp_ == nullptr) {
        return false;
    }

    std::string nonce(aes_nonce_);
    *(uint16_t*)&nonce[2] = htons(packet->payload.size());
    *(uint32_t*)&nonce[8] = htonl(packet->timestamp);
    *(uint32_t*)&nonce[12] = htonl(++local_sequence_);

    std::string encrypted;
    encrypted.resize(aes_nonce_.size() + packet->payload.size());
    memcpy(encrypted.data(), nonce.data(), nonce.size());

    size_t nc_off = 0;
    uint8_t stream_block[16] = {0};
    if (mbedtls_aes_crypt_ctr(&aes_ctx_, packet->payload.size(), &nc_off, (uint8_t*)nonce.c_str(), stream_block,
        (uint8_t*)packet->payload.data(), (uint8_t*)&encrypted[nonce.size()]) != 0) {
        ESP_LOGE(TAG, "Failed to encrypt audio data");
        return false;
    }

    return udp_->Send(encrypted) > 0;
}

void MqttProtocol::CloseAudioChannel() {
    {
        std::lock_guard<std::mutex> lock(channel_mutex_);
        udp_.reset();
    }

    std::string message = "{";
    message += "\"session_id\":\"" + session_id_ + "\",";
    message += "\"type\":\"goodbye\"";
    message += "}";
    SendText(message);

    if (on_audio_channel_closed_ != nullptr) {
        on_audio_channel_closed_();
    }
}

bool MqttProtocol::OpenAudioChannel() {
    if (mqtt_ == nullptr || !mqtt_->IsConnected()) {
        ESP_LOGI(TAG, "MQTT is not connected, try to connect now");
        if (!StartMqttClient(true)) {
            return false;
        }
    }

    error_occurred_ = false;
    session_id_ = "";
    xEventGroupClearBits(event_group_handle_, MQTT_PROTOCOL_SERVER_HELLO_EVENT);

    auto message = GetHelloMessage();
    if (!SendText(message)) {
        return false;
    }

    // 等待服务器响应
    EventBits_t bits = xEventGroupWaitBits(event_group_handle_, MQTT_PROTOCOL_SERVER_HELLO_EVENT, pdTRUE, pdFALSE, pdMS_TO_TICKS(10000));
    if (!(bits & MQTT_PROTOCOL_SERVER_HELLO_EVENT)) {
        ESP_LOGE(TAG, "Failed to receive server hello");
        SetError(Lang::Strings::SERVER_TIMEOUT);
        return false;
    }

    std::lock_guard<std::mutex> lock(channel_mutex_);
    auto network = Board::GetInstance().GetNetwork();
    udp_ = network->CreateUdp(2);
    udp_->OnMessage([this](const std::string& data) {
        /*
         * UDP Encrypted OPUS Packet Format:
         * |type 1u|flags 1u|payload_len 2u|ssrc 4u|timestamp 4u|sequence 4u|
         * |payload payload_len|
         */
        if (data.size() < sizeof(aes_nonce_)) {
            ESP_LOGE(TAG, "Invalid audio packet size: %u", data.size());
            return;
        }
        if (data[0] != 0x01) {
            ESP_LOGE(TAG, "Invalid audio packet type: %x", data[0]);
            return;
        }
        uint32_t timestamp = ntohl(*(uint32_t*)&data[8]);
        uint32_t sequence = ntohl(*(uint32_t*)&data[12]);
        if (sequence < remote_sequence_) {
            ESP_LOGW(TAG, "Received audio packet with old sequence: %lu, expected: %lu", sequence, remote_sequence_);
            return;
        }
        if (sequence != remote_sequence_ + 1) {
            ESP_LOGW(TAG, "Received audio packet with wrong sequence: %lu, expected: %lu", sequence, remote_sequence_ + 1);
        }

        size_t decrypted_size = data.size() - aes_nonce_.size();
        size_t nc_off = 0;
        uint8_t stream_block[16] = {0};
        auto nonce = (uint8_t*)data.data();
        auto encrypted = (uint8_t*)data.data() + aes_nonce_.size();
        auto packet = std::make_unique<AudioStreamPacket>();
        packet->sample_rate = server_sample_rate_;
        packet->frame_duration = server_frame_duration_;
        packet->timestamp = timestamp;
        packet->payload.resize(decrypted_size);
        int ret = mbedtls_aes_crypt_ctr(&aes_ctx_, decrypted_size, &nc_off, nonce, stream_block, encrypted, (uint8_t*)packet->payload.data());
        if (ret != 0) {
            ESP_LOGE(TAG, "Failed to decrypt audio data, ret: %d", ret);
            return;
        }
        if (on_incoming_audio_ != nullptr) {
            on_incoming_audio_(std::move(packet));
        }
        remote_sequence_ = sequence;
        last_incoming_time_ = std::chrono::steady_clock::now();
    });

    udp_->Connect(udp_server_, udp_port_);

    if (on_audio_channel_opened_ != nullptr) {
        on_audio_channel_opened_();
    }
    return true;
}

std::string MqttProtocol::GetHelloMessage() {
    // 发送 hello 消息申请 UDP 通道
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "hello");
    cJSON_AddNumberToObject(root, "version", 3);
    cJSON_AddStringToObject(root, "transport", "udp");
    cJSON* features = cJSON_CreateObject();
#if CONFIG_USE_SERVER_AEC
    cJSON_AddBoolToObject(features, "aec", true);
#endif
    cJSON_AddBoolToObject(features, "mcp", true);
    cJSON_AddItemToObject(root, "features", features);
    cJSON* audio_params = cJSON_CreateObject();
    cJSON_AddStringToObject(audio_params, "format", "opus");
    cJSON_AddNumberToObject(audio_params, "sample_rate", 16000);
    cJSON_AddNumberToObject(audio_params, "channels", 1);
    cJSON_AddNumberToObject(audio_params, "frame_duration", OPUS_FRAME_DURATION_MS);
    cJSON_AddItemToObject(root, "audio_params", audio_params);
    auto json_str = cJSON_PrintUnformatted(root);
    std::string message(json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);
    return message;
}

void MqttProtocol::ParseServerHello(const cJSON* root) {
    auto transport = cJSON_GetObjectItem(root, "transport");
    if (transport == nullptr || strcmp(transport->valuestring, "udp") != 0) {
        ESP_LOGE(TAG, "Unsupported transport: %s", transport->valuestring);
        return;
    }

    auto session_id = cJSON_GetObjectItem(root, "session_id");
    if (cJSON_IsString(session_id)) {
        session_id_ = session_id->valuestring;
        ESP_LOGI(TAG, "Session ID: %s", session_id_.c_str());
    }

    // Get sample rate from hello message
    auto audio_params = cJSON_GetObjectItem(root, "audio_params");
    if (cJSON_IsObject(audio_params)) {
        auto sample_rate = cJSON_GetObjectItem(audio_params, "sample_rate");
        if (cJSON_IsNumber(sample_rate)) {
            server_sample_rate_ = sample_rate->valueint;
        }
        auto frame_duration = cJSON_GetObjectItem(audio_params, "frame_duration");
        if (cJSON_IsNumber(frame_duration)) {
            server_frame_duration_ = frame_duration->valueint;
        }
    }

    auto udp = cJSON_GetObjectItem(root, "udp");
    if (!cJSON_IsObject(udp)) {
        ESP_LOGE(TAG, "UDP is not specified");
        return;
    }
    udp_server_ = cJSON_GetObjectItem(udp, "server")->valuestring;
    udp_port_ = cJSON_GetObjectItem(udp, "port")->valueint;
    auto key = cJSON_GetObjectItem(udp, "key")->valuestring;
    auto nonce = cJSON_GetObjectItem(udp, "nonce")->valuestring;

    // auto encryption = cJSON_GetObjectItem(udp, "encryption")->valuestring;
    // ESP_LOGI(TAG, "UDP server: %s, port: %d, encryption: %s", udp_server_.c_str(), udp_port_, encryption);
    aes_nonce_ = DecodeHexString(nonce);
    mbedtls_aes_init(&aes_ctx_);
    mbedtls_aes_setkey_enc(&aes_ctx_, (const unsigned char*)DecodeHexString(key).c_str(), 128);
    local_sequence_ = 0;
    remote_sequence_ = 0;
    xEventGroupSetBits(event_group_handle_, MQTT_PROTOCOL_SERVER_HELLO_EVENT);
}

static const char hex_chars[] = "0123456789ABCDEF";
// 辅助函数，将单个十六进制字符转换为对应的数值
static inline uint8_t CharToHex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return 0;  // 对于无效输入，返回0
}

std::string MqttProtocol::DecodeHexString(const std::string& hex_string) {
    std::string decoded;
    decoded.reserve(hex_string.size() / 2);
    for (size_t i = 0; i < hex_string.size(); i += 2) {
        char byte = (CharToHex(hex_string[i]) << 4) | CharToHex(hex_string[i + 1]);
        decoded.push_back(byte);
    }
    return decoded;
}

bool MqttProtocol::IsAudioChannelOpened() const {
    return udp_ != nullptr && !error_occurred_ && !IsTimeout();
}
