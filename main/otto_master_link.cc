#include "otto_master_link.h"

#include "application.h"
#include "board.h"
#include "boards/otto-robot/otto_controller_status.h"
#include "settings.h"
#include "system_info.h"
#include "tcp.h"

#include <cJSON.h>
#include <esp_app_desc.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <sdkconfig.h>

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstring>

#define TAG "OttoMaster"

namespace {

constexpr const char* kMasterHostname = "master.local";
constexpr const char* kProtocolName = "otto-master/1";
constexpr uint32_t kMdnsTimeoutMs = 1600;
constexpr uint32_t kReconnectDelayMs = 3000;
constexpr uint32_t kHeartbeatIntervalMs = 5000;

std::string DeviceName() {
    Settings settings("otto", false);
    const std::string configured = settings.GetString("name");
    return configured.empty() ? BOARD_NAME : configured;
}

std::string NormalizeMac(const std::string& value) {
    std::string normalized;
    normalized.reserve(12);
    for (unsigned char character : value) {
        if (character == ':' || character == '-') {
            continue;
        }
        if (!std::isxdigit(character)) {
            return {};
        }
        normalized.push_back(static_cast<char>(std::tolower(character)));
    }
    return normalized.size() == 12 ? normalized : std::string();
}

bool ConstantTimeEqual(const std::string& left, const std::string& right) {
    size_t different = left.size() ^ right.size();
    const size_t length = std::max(left.size(), right.size());
    for (size_t index = 0; index < length; ++index) {
        const unsigned char left_value =
            index < left.size() ? static_cast<unsigned char>(left[index]) : 0;
        const unsigned char right_value =
            index < right.size() ? static_cast<unsigned char>(right[index]) : 0;
        different |= left_value ^ right_value;
    }
    return different == 0;
}

bool ReadBoundedString(const cJSON* object, const char* key, size_t maximum,
                       std::string& value) {
    const cJSON* item = cJSON_GetObjectItem(object, key);
    if (!cJSON_IsString(item) || item->valuestring == nullptr) {
        return false;
    }
    value = item->valuestring;
    return !value.empty() && value.size() <= maximum;
}

bool ApplyLocalProvisioning(const cJSON* root, std::string& applied_name,
                            std::string& applied_transport, std::string& error) {
    std::string target_mac;
    if (!ReadBoundedString(root, "target_mac", 17, target_mac)) {
        error = "target_mac is required";
        return false;
    }
    const std::string device_id = NormalizeMac(SystemInfo::GetMacAddress());
    if (device_id.empty() || NormalizeMac(target_mac) != device_id) {
        error = "target_mac does not match this device";
        return false;
    }

    const cJSON* mqtt = cJSON_GetObjectItem(root, "mqtt");
    if (!cJSON_IsObject(mqtt)) {
        error = "mqtt object is required";
        return false;
    }
    std::string endpoint;
    std::string client_id;
    std::string username;
    std::string password;
    std::string publish_topic;
    std::string subscribe_topic;
    if (!ReadBoundedString(mqtt, "endpoint", 255, endpoint) ||
        !ReadBoundedString(mqtt, "client_id", 128, client_id) ||
        !ReadBoundedString(mqtt, "username", 128, username) ||
        !ReadBoundedString(mqtt, "password", 256, password) ||
        !ReadBoundedString(mqtt, "publish_topic", 255, publish_topic) ||
        !ReadBoundedString(mqtt, "subscribe_topic", 255, subscribe_topic)) {
        error = "mqtt provisioning fields are missing or too long";
        return false;
    }
    if (client_id != "otto-" + device_id || username != "device-" + device_id ||
        publish_topic != "otto/v1/devices/" + device_id + "/up" ||
        subscribe_topic != "otto/v1/devices/" + device_id + "/down") {
        error = "mqtt identity or topic does not match this device";
        return false;
    }

    Settings current_identity("otto", false);
    applied_transport = current_identity.GetString("transport", "mqtt");
    const cJSON* preferred_transport = cJSON_GetObjectItem(root, "preferred_transport");
    if (preferred_transport != nullptr) {
        if (!cJSON_IsString(preferred_transport) || preferred_transport->valuestring == nullptr ||
            (strcmp(preferred_transport->valuestring, "mqtt") != 0 &&
             strcmp(preferred_transport->valuestring, "websocket") != 0)) {
            error = "preferred_transport must be mqtt or websocket";
            return false;
        }
        applied_transport = preferred_transport->valuestring;
    }

    const cJSON* websocket = cJSON_GetObjectItem(root, "websocket");
    const bool has_websocket = cJSON_IsObject(websocket);
    std::string websocket_url;
    std::string websocket_client_id;
    std::string websocket_token;
    int websocket_version = 0;
    if (has_websocket) {
        const cJSON* version = cJSON_GetObjectItem(websocket, "protocol_version");
        if (!ReadBoundedString(websocket, "endpoint", 512, websocket_url) ||
            !ReadBoundedString(websocket, "client_id", 128, websocket_client_id) ||
            !ReadBoundedString(websocket, "token", 256, websocket_token) ||
            !cJSON_IsNumber(version)) {
            error = "websocket provisioning fields are missing or too long";
            return false;
        }
        websocket_version = version->valueint;
        if ((websocket_url.rfind("ws://", 0) != 0 &&
             websocket_url.rfind("wss://", 0) != 0) ||
            websocket_client_id != client_id || websocket_token != password ||
            websocket_version != 1) {
            error = "websocket profile does not match this device";
            return false;
        }
    }
    Settings current_websocket("websocket", false);
    const bool stored_websocket_ready =
        !current_websocket.GetString("url").empty() &&
        !current_websocket.GetString("client_id").empty() &&
        !current_websocket.GetString("token").empty() &&
        current_websocket.GetInt("version", 0) == 1;
    if (applied_transport == "websocket" && !has_websocket &&
        !stored_websocket_ready) {
        error = "preferred websocket transport requires a complete profile";
        return false;
    }

    Settings current_mqtt("mqtt", false);
    const std::string current_password = current_mqtt.GetString("password");
    const bool locked = current_identity.GetBool("mqtt_locked", false) &&
                        !current_password.empty();
    if (locked) {
        std::string current_token;
        if (!ReadBoundedString(root, "current_token", 256, current_token) ||
            !ConstantTimeEqual(current_token, current_password)) {
            error = "provisioning is locked";
            return false;
        }
    }

    applied_name = DeviceName();
    const cJSON* name = cJSON_GetObjectItem(root, "name");
    if (name != nullptr) {
        if (!cJSON_IsString(name) || name->valuestring == nullptr ||
            strlen(name->valuestring) == 0 || strlen(name->valuestring) > 80) {
            error = "name must be a non-empty bounded string";
            return false;
        }
        applied_name = name->valuestring;
    }

    {
        Settings settings("mqtt", true);
        settings.SetString("endpoint", endpoint);
        settings.SetString("client_id", client_id);
        settings.SetString("username", username);
        settings.SetString("password", password);
        settings.SetString("publish_topic", publish_topic);
        settings.SetString("subscribe_topic", subscribe_topic);
        settings.SetInt("keepalive", 30);
    }
    {
        if (has_websocket) {
            Settings settings("websocket", true);
            settings.SetString("url", websocket_url);
            settings.SetString("client_id", websocket_client_id);
            settings.SetString("token", websocket_token);
            settings.SetInt("version", websocket_version);
        }
    }
    {
        Settings settings("otto", true);
        settings.SetString("name", applied_name);
        settings.SetString("transport", applied_transport);
        settings.SetBool("mqtt_locked", true);
    }
    return true;
}

uint16_t ReadU16(const uint8_t* data) {
    return static_cast<uint16_t>((static_cast<uint16_t>(data[0]) << 8) | data[1]);
}

std::string NormalizeDnsName(std::string name) {
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    while (!name.empty() && name.back() == '.') {
        name.pop_back();
    }
    return name;
}

// Read a DNS name, including compressed names in a DNS/mDNS answer.
bool ReadDnsName(const uint8_t* packet, size_t packet_size, size_t& cursor, std::string& name) {
    size_t position = cursor;
    size_t next_cursor = cursor;
    bool jumped = false;
    int jumps = 0;
    name.clear();

    while (position < packet_size && jumps++ < 32) {
        uint8_t label_size = packet[position];
        if (label_size == 0) {
            if (!jumped) {
                next_cursor = position + 1;
            }
            cursor = next_cursor;
            return true;
        }

        if ((label_size & 0xC0) == 0xC0) {
            if (position + 1 >= packet_size) {
                return false;
            }
            uint16_t pointer = static_cast<uint16_t>((label_size & 0x3F) << 8) |
                               packet[position + 1];
            if (pointer >= packet_size) {
                return false;
            }
            if (!jumped) {
                next_cursor = position + 2;
                jumped = true;
            }
            position = pointer;
            continue;
        }

        if (label_size > 63 || position + 1 + label_size > packet_size) {
            return false;
        }
        if (!name.empty()) {
            name.push_back('.');
        }
        name.append(reinterpret_cast<const char*>(packet + position + 1), label_size);
        position += 1 + label_size;
    }

    return false;
}

bool BuildMdnsQuery(uint8_t* query, size_t query_size, size_t& query_length) {
    if (query_size < 12) {
        return false;
    }
    memset(query, 0, query_size);
    // mDNS uses transaction ID 0 and a standard query with one question.
    query[4] = 0;
    query[5] = 1;

    size_t cursor = 12;
    const char* labels[] = {"master", "local"};
    for (const char* label : labels) {
        const size_t length = strlen(label);
        if (cursor + 1 + length >= query_size) {
            return false;
        }
        query[cursor++] = static_cast<uint8_t>(length);
        memcpy(query + cursor, label, length);
        cursor += length;
    }
    if (cursor + 5 > query_size) {
        return false;
    }
    query[cursor++] = 0;
    query[cursor++] = 0;
    query[cursor++] = 1;  // QTYPE=A
    query[cursor++] = 0;
    query[cursor++] = 1;  // QCLASS=IN
    query_length = cursor;
    return true;
}

std::string JsonToString(cJSON* root) {
    char* text = cJSON_PrintUnformatted(root);
    std::string result = text == nullptr ? "{}" : text;
    if (text != nullptr) {
        cJSON_free(text);
    }
    cJSON_Delete(root);
    return result;
}

void CopyId(cJSON* target, const cJSON* id) {
    if (cJSON_IsString(id)) {
        cJSON_AddStringToObject(target, "id", id->valuestring);
    } else if (cJSON_IsNumber(id)) {
        cJSON_AddNumberToObject(target, "id", id->valuedouble);
    }
}

}  // namespace

OttoMasterLink& OttoMasterLink::GetInstance() {
    static OttoMasterLink instance;
    return instance;
}

void OttoMasterLink::Start() {
    if (running_.exchange(true)) {
        return;
    }

    if (xTaskCreate([](void* arg) {
            auto* link = static_cast<OttoMasterLink*>(arg);
            link->Run();
        }, "otto_master", 6144, this, 4, &task_handle_) != pdPASS) {
        running_.store(false);
        task_handle_ = nullptr;
        ESP_LOGE(TAG, "Failed to create master link task");
    }
}

bool OttoMasterLink::SendJson(const std::string& json) {
    if (json.empty()) {
        return false;
    }

    std::lock_guard<std::mutex> lock(tcp_mutex_);
    if (!connected_.load() || tcp_ == nullptr) {
        return false;
    }

    std::string line = json;
    line.push_back('\n');
    const int sent = tcp_->Send(line);
    if (sent != static_cast<int>(line.size())) {
        connected_.store(false);
        return false;
    }
    return true;
}

bool ResolveOttoMasterMdns(std::string& address) {
    address.clear();

    int socket_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_fd < 0) {
        ESP_LOGE(TAG, "mDNS socket failed: errno=%d", errno);
        return false;
    }

    int reuse = 1;
    setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in local_address = {};
    local_address.sin_family = AF_INET;
    local_address.sin_port = htons(5353);
    local_address.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(socket_fd, reinterpret_cast<sockaddr*>(&local_address), sizeof(local_address)) < 0) {
        ESP_LOGW(TAG, "mDNS bind failed: errno=%d", errno);
        close(socket_fd);
        return false;
    }

    ip_mreq multicast_request = {};
    inet_aton("224.0.0.251", &multicast_request.imr_multiaddr);
    multicast_request.imr_interface.s_addr = htonl(INADDR_ANY);
    if (setsockopt(socket_fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &multicast_request,
                   sizeof(multicast_request)) < 0) {
        ESP_LOGW(TAG, "mDNS multicast join failed: errno=%d", errno);
        close(socket_fd);
        return false;
    }

    timeval timeout = {};
    timeout.tv_sec = 0;
    timeout.tv_usec = 400 * 1000;
    setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    uint8_t query[128];
    size_t query_length = 0;
    if (!BuildMdnsQuery(query, sizeof(query), query_length)) {
        close(socket_fd);
        return false;
    }

    sockaddr_in mdns_address = {};
    mdns_address.sin_family = AF_INET;
    mdns_address.sin_port = htons(5353);
    inet_aton("224.0.0.251", &mdns_address.sin_addr);
    if (sendto(socket_fd, query, query_length, 0, reinterpret_cast<sockaddr*>(&mdns_address),
               sizeof(mdns_address)) < 0) {
        ESP_LOGW(TAG, "mDNS query failed: errno=%d", errno);
        close(socket_fd);
        return false;
    }

    const int64_t deadline = esp_timer_get_time() + kMdnsTimeoutMs * 1000LL;
    uint8_t packet[1500];
    while (esp_timer_get_time() < deadline) {
        const int received = recvfrom(socket_fd, packet, sizeof(packet), 0, nullptr, nullptr);
        if (received < 0) {
            continue;
        }
        const size_t packet_size = static_cast<size_t>(received);
        if (packet_size < 12) {
            continue;
        }

        const uint16_t question_count = ReadU16(packet + 4);
        const uint16_t answer_count = ReadU16(packet + 6);
        const uint16_t authority_count = ReadU16(packet + 8);
        const uint16_t additional_count = ReadU16(packet + 10);
        size_t cursor = 12;
        bool valid_packet = true;

        for (uint16_t i = 0; i < question_count && valid_packet; ++i) {
            std::string ignored_name;
            valid_packet = ReadDnsName(packet, packet_size, cursor, ignored_name);
            if (valid_packet && cursor + 4 <= packet_size) {
                cursor += 4;
            } else {
                valid_packet = false;
            }
        }

        const uint32_t record_count = static_cast<uint32_t>(answer_count) + authority_count +
                                      additional_count;
        for (uint32_t i = 0; i < record_count && valid_packet; ++i) {
            std::string record_name;
            valid_packet = ReadDnsName(packet, packet_size, cursor, record_name);
            if (!valid_packet || cursor + 10 > packet_size) {
                valid_packet = false;
                break;
            }

            const uint16_t record_type = ReadU16(packet + cursor);
            const uint16_t record_class = ReadU16(packet + cursor + 2) & 0x7FFF;
            const uint16_t data_length = ReadU16(packet + cursor + 8);
            cursor += 10;
            if (cursor + data_length > packet_size) {
                valid_packet = false;
                break;
            }

            if (record_type == 1 && record_class == 1 && data_length == 4 &&
                NormalizeDnsName(record_name) == kMasterHostname) {
                char ip[INET_ADDRSTRLEN] = {};
                if (inet_ntop(AF_INET, packet + cursor, ip, sizeof(ip)) != nullptr) {
                    address = ip;
                    close(socket_fd);
                    ESP_LOGI(TAG, "mDNS resolved %s -> %s", kMasterHostname, address.c_str());
                    return true;
                }
            }
            cursor += data_length;
        }
    }

    close(socket_fd);
    return false;
}

std::string OttoMasterLink::BuildBoardSnapshot() const {
    cJSON* board_json = cJSON_Parse(Board::GetInstance().GetBoardJson().c_str());
    if (board_json == nullptr) {
        return "{}";
    }
    return JsonToString(board_json);
}

std::string OttoMasterLink::BuildRuntimeSnapshot() const {
    cJSON* runtime = cJSON_CreateObject();

    cJSON* action = cJSON_Parse(GetOttoRuntimeStatusJson().c_str());
    if (action != nullptr) {
        cJSON_AddItemToObject(runtime, "otto", action);
    }

    cJSON* voice = cJSON_Parse(Application::GetInstance().GetVoiceStatusJson().c_str());
    if (voice != nullptr) {
        cJSON_AddItemToObject(runtime, "voice", voice);
    }

    return JsonToString(runtime);
}

void OttoMasterLink::SendHello() {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "hello");
    cJSON_AddStringToObject(root, "protocol", kProtocolName);
    cJSON_AddStringToObject(root, "name", DeviceName().c_str());
    cJSON_AddStringToObject(root, "mac", SystemInfo::GetMacAddress().c_str());
    cJSON_AddStringToObject(root, "firmware_version", esp_app_get_description()->version);

    cJSON* board_json = cJSON_Parse(BuildBoardSnapshot().c_str());
    if (board_json != nullptr) {
        cJSON_AddItemToObject(root, "board", board_json);
    }
    cJSON* runtime = cJSON_Parse(BuildRuntimeSnapshot().c_str());
    if (runtime != nullptr) {
        cJSON_AddItemToObject(root, "runtime", runtime);
    }
    SendJson(JsonToString(root));
}

void OttoMasterLink::SendHeartbeat() {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "heartbeat");
    cJSON_AddStringToObject(root, "name", DeviceName().c_str());
    cJSON_AddNumberToObject(root, "uptime_ms", static_cast<double>(esp_timer_get_time() / 1000));

    cJSON* board_json = cJSON_Parse(BuildBoardSnapshot().c_str());
    if (board_json != nullptr) {
        cJSON_AddItemToObject(root, "board", board_json);
    }
    cJSON* runtime = cJSON_Parse(BuildRuntimeSnapshot().c_str());
    if (runtime != nullptr) {
        cJSON_AddItemToObject(root, "runtime", runtime);
    }
    SendJson(JsonToString(root));
}

void OttoMasterLink::HandleStream(const std::string& data) {
    receive_buffer_ += data;
    if (receive_buffer_.size() > 8192) {
        ESP_LOGW(TAG, "Master message buffer overflow, clearing it");
        receive_buffer_.clear();
        return;
    }

    size_t newline = std::string::npos;
    while ((newline = receive_buffer_.find('\n')) != std::string::npos) {
        std::string message = receive_buffer_.substr(0, newline);
        receive_buffer_.erase(0, newline + 1);
        if (!message.empty() && message.back() == '\r') {
            message.pop_back();
        }
        if (!message.empty()) {
            HandleMessage(message);
        }
    }
}

void OttoMasterLink::HandleMessage(const std::string& message) {
    cJSON* root = cJSON_Parse(message.c_str());
    if (root == nullptr) {
        ESP_LOGW(TAG, "Master sent invalid JSON: %s", message.c_str());
        return;
    }

    cJSON* type = cJSON_GetObjectItem(root, "type");
    cJSON* id = cJSON_GetObjectItem(root, "id");
    const char* type_string = cJSON_IsString(type) ? type->valuestring : "";

    cJSON* response = cJSON_CreateObject();
    CopyId(response, id);
    bool reboot_after_response = false;

    if (strcmp(type_string, "ping") == 0) {
        cJSON_AddStringToObject(response, "type", "pong");
        cJSON_AddStringToObject(response, "name", DeviceName().c_str());
        cJSON_AddNumberToObject(response, "uptime_ms", static_cast<double>(esp_timer_get_time() / 1000));
    } else if (strcmp(type_string, "echo") == 0) {
        cJSON_AddStringToObject(response, "type", "echo");
        cJSON* payload = cJSON_GetObjectItem(root, "payload");
        cJSON_AddItemToObject(response, "payload",
                              payload == nullptr ? cJSON_CreateNull() : cJSON_Duplicate(payload, 1));
    } else if (strcmp(type_string, "status") == 0 || strcmp(type_string, "state") == 0 ||
               strcmp(type_string, "query_state") == 0) {
        cJSON_AddStringToObject(response, "type", "status");
        cJSON* board_json = cJSON_Parse(BuildBoardSnapshot().c_str());
        if (board_json != nullptr) {
            cJSON_AddItemToObject(response, "board", board_json);
        }
        cJSON* runtime = cJSON_Parse(BuildRuntimeSnapshot().c_str());
        if (runtime != nullptr) {
            cJSON_AddItemToObject(response, "runtime", runtime);
        }
    } else if (strcmp(type_string, "list_actions") == 0 ||
               strcmp(type_string, "query_actions") == 0) {
        cJSON_AddStringToObject(response, "type", "actions");
        cJSON* catalog = cJSON_Parse(GetOttoAvailableActionsJson().c_str());
        if (catalog != nullptr) {
            cJSON* actions = cJSON_GetObjectItem(catalog, "actions");
            if (actions != nullptr) {
                cJSON_AddItemToObject(response, "actions", cJSON_Duplicate(actions, 1));
            }
            cJSON_Delete(catalog);
        }
        cJSON* runtime = cJSON_Parse(BuildRuntimeSnapshot().c_str());
        if (runtime != nullptr) {
            cJSON_AddItemToObject(response, "runtime", runtime);
        }
    } else if (strcmp(type_string, "otto_provision") == 0) {
        std::string applied_name;
        std::string applied_transport;
        std::string error;
        const bool applied =
            ApplyLocalProvisioning(root, applied_name, applied_transport, error);
        cJSON_AddStringToObject(response, "type", "otto_provision_ack");
        cJSON_AddBoolToObject(response, "ok", applied);
        if (applied) {
            cJSON_AddStringToObject(response, "name", applied_name.c_str());
            cJSON_AddStringToObject(response, "preferred_transport",
                                    applied_transport.c_str());
            cJSON_AddBoolToObject(response, "restart_required", true);
            const cJSON* restart = cJSON_GetObjectItem(root, "restart");
            reboot_after_response = cJSON_IsTrue(restart);
            ESP_LOGI(TAG, "Local transport provisioning stored for this device");
        } else {
            cJSON_AddStringToObject(response, "error", error.c_str());
            ESP_LOGW(TAG, "Rejected local transport provisioning: %s", error.c_str());
        }
    } else if (strcmp(type_string, "voice_wake") == 0) {
        Settings mqtt("mqtt", false);
        const std::string expected_token = mqtt.GetString("password");
        std::string current_token;
        if (expected_token.empty() ||
            !ReadBoundedString(root, "current_token", 256, current_token) ||
            !ConstantTimeEqual(current_token, expected_token)) {
            cJSON_AddStringToObject(response, "type", "error");
            cJSON_AddStringToObject(response, "error", "voice wake authentication failed");
        } else {
            std::string wake_word = "你好奶龙";
            const cJSON* requested_word = cJSON_GetObjectItem(root, "text");
            if (requested_word != nullptr &&
                (!cJSON_IsString(requested_word) || requested_word->valuestring == nullptr ||
                 strlen(requested_word->valuestring) == 0 ||
                 strlen(requested_word->valuestring) > 64)) {
                cJSON_AddStringToObject(response, "type", "error");
                cJSON_AddStringToObject(response, "error",
                                        "voice wake text must be a bounded string");
            } else {
                if (requested_word != nullptr) {
                    wake_word = requested_word->valuestring;
                }
                cJSON_AddStringToObject(response, "type", "voice_wake_accepted");
                cJSON_AddStringToObject(response, "text", wake_word.c_str());
                Application::GetInstance().Schedule([wake_word]() {
                    Application::GetInstance().WakeWordInvoke(wake_word);
                });
            }
        }
    } else if (strcmp(type_string, "action") == 0 || strcmp(type_string, "move") == 0 ||
               strcmp(type_string, "otto_action") == 0) {
        cJSON* action = cJSON_GetObjectItem(root, "action");
        if (!cJSON_IsString(action)) {
            action = cJSON_GetObjectItem(root, "name");
        }
        const int steps = cJSON_IsNumber(cJSON_GetObjectItem(root, "steps"))
                              ? cJSON_GetObjectItem(root, "steps")->valueint
                              : 3;
        const int speed = cJSON_IsNumber(cJSON_GetObjectItem(root, "speed"))
                              ? cJSON_GetObjectItem(root, "speed")->valueint
                              : 1000;
        const int direction = cJSON_IsNumber(cJSON_GetObjectItem(root, "direction"))
                                  ? cJSON_GetObjectItem(root, "direction")->valueint
                                  : 1;
        const int amount = cJSON_IsNumber(cJSON_GetObjectItem(root, "amount"))
                               ? cJSON_GetObjectItem(root, "amount")->valueint
                               : 50;
        std::string error;
        if (!cJSON_IsString(action) ||
            !QueueOttoAction(action->valuestring, steps, speed, direction, amount, error)) {
            cJSON_AddStringToObject(response, "type", "error");
            cJSON_AddStringToObject(response, "error",
                                    error.empty() ? "action 字段必须是字符串" : error.c_str());
        } else {
            cJSON_AddStringToObject(response, "type", "action_accepted");
            cJSON_AddStringToObject(response, "action", action->valuestring);
            cJSON_AddNumberToObject(response, "steps", steps);
            cJSON_AddNumberToObject(response, "speed", speed);
            cJSON* runtime = cJSON_Parse(BuildRuntimeSnapshot().c_str());
            if (runtime != nullptr) {
                cJSON_AddItemToObject(response, "runtime", runtime);
            }
        }
    } else if (strcmp(type_string, "stop") == 0 || strcmp(type_string, "otto_stop") == 0) {
        std::string error;
        if (!StopOttoAction(error)) {
            cJSON_AddStringToObject(response, "type", "error");
            cJSON_AddStringToObject(response, "error", error.c_str());
        } else {
            cJSON_AddStringToObject(response, "type", "stopped");
            cJSON* runtime = cJSON_Parse(BuildRuntimeSnapshot().c_str());
            if (runtime != nullptr) {
                cJSON_AddItemToObject(response, "runtime", runtime);
            }
        }
    } else if (strcmp(type_string, "upgrade") == 0 || strcmp(type_string, "ota") == 0) {
        cJSON* url = cJSON_GetObjectItem(root, "url");
        if (!cJSON_IsString(url) || url->valuestring == nullptr || strlen(url->valuestring) == 0 ||
            strlen(url->valuestring) > 512 ||
            (strncmp(url->valuestring, "http://", 7) != 0 &&
             strncmp(url->valuestring, "https://", 8) != 0)) {
            cJSON_AddStringToObject(response, "type", "error");
            cJSON_AddStringToObject(response, "error", "upgrade requires an http(s) url");
        } else {
            const std::string firmware_url = url->valuestring;
            cJSON_AddStringToObject(response, "type", "upgrade_started");
            cJSON_AddStringToObject(response, "url", firmware_url.c_str());
            // Run the existing OTA flow on the application's main event task,
            // so audio/display/protocol state changes stay serialized.
            Application::GetInstance().Schedule([firmware_url]() {
                Ota ota;
                Application::GetInstance().UpgradeFirmware(ota, firmware_url);
            });
        }
    } else {
        // Unknown messages are acknowledged so the WebUI can verify the
        // full round trip without involving audio, MQTT, or MCP.
        cJSON_AddStringToObject(response, "type", "ack");
        cJSON_AddStringToObject(response, "name", DeviceName().c_str());
        cJSON_AddItemToObject(response, "received", cJSON_Duplicate(root, 1));
    }

    SendJson(JsonToString(response));
    cJSON_Delete(root);
    if (reboot_after_response) {
        Application::GetInstance().Schedule([]() {
            vTaskDelay(pdMS_TO_TICKS(500));
            Application::GetInstance().Reboot();
        });
    }
}

void OttoMasterLink::Run() {
    while (running_.load()) {
        std::string master_address;
        if (!ResolveOttoMasterMdns(master_address)) {
            ESP_LOGW(TAG, "Cannot resolve %s, retry in %u ms", kMasterHostname,
                     kReconnectDelayMs);
            vTaskDelay(pdMS_TO_TICKS(kReconnectDelayMs));
            continue;
        }

        auto tcp = Board::GetInstance().GetNetwork()->CreateTcp(0);
        tcp->OnStream([this](const std::string& data) { HandleStream(data); });
        tcp->OnDisconnected([this]() {
            connected_.store(false);
            ESP_LOGW(TAG, "Master TCP disconnected");
        });

        ESP_LOGI(TAG, "Connecting to master.local (%s):%d", master_address.c_str(),
                 CONFIG_OTTO_MASTER_PORT);
        if (!tcp->Connect(master_address, CONFIG_OTTO_MASTER_PORT)) {
            ESP_LOGW(TAG, "Master TCP connection failed");
            vTaskDelay(pdMS_TO_TICKS(kReconnectDelayMs));
            continue;
        }

        receive_buffer_.clear();
        {
            std::lock_guard<std::mutex> lock(tcp_mutex_);
            tcp_ = std::move(tcp);
            connected_.store(true);
        }
        ESP_LOGI(TAG, "Connected to master.local");
        SendHello();

        int64_t next_heartbeat = esp_timer_get_time() / 1000 + kHeartbeatIntervalMs;
        while (running_.load() && connected_.load()) {
            const int64_t now = esp_timer_get_time() / 1000;
            if (now >= next_heartbeat) {
                SendHeartbeat();
                next_heartbeat = now + kHeartbeatIntervalMs;
            }
            vTaskDelay(pdMS_TO_TICKS(250));
        }

        std::unique_ptr<Tcp> old_tcp;
        {
            std::lock_guard<std::mutex> lock(tcp_mutex_);
            connected_.store(false);
            old_tcp = std::move(tcp_);
        }
        if (old_tcp != nullptr) {
            old_tcp->Disconnect();
        }
        vTaskDelay(pdMS_TO_TICKS(kReconnectDelayMs));
    }

    connected_.store(false);
    task_handle_ = nullptr;
    running_.store(false);
    vTaskDelete(nullptr);
}
