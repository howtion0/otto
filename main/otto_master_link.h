#ifndef OTTO_MASTER_LINK_H
#define OTTO_MASTER_LINK_H

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

class Tcp;

// A deliberately small, non-audio connection used to validate:
// Wi-Fi DHCP -> mDNS(master.local) -> TCP -> bidirectional JSON messages.
class OttoMasterLink {
public:
    static OttoMasterLink& GetInstance();

    void Start();
    bool SendJson(const std::string& json);

private:
    OttoMasterLink() = default;
    ~OttoMasterLink() = default;
    OttoMasterLink(const OttoMasterLink&) = delete;
    OttoMasterLink& operator=(const OttoMasterLink&) = delete;

    void Run();
    bool ResolveMasterMdns(std::string& address);
    void HandleStream(const std::string& data);
    void HandleMessage(const std::string& message);
    void SendHello();
    void SendHeartbeat();
    std::string BuildBoardSnapshot() const;
    std::string BuildRuntimeSnapshot() const;

    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};
    TaskHandle_t task_handle_ = nullptr;
    std::mutex tcp_mutex_;
    std::unique_ptr<Tcp> tcp_;
    std::string receive_buffer_;
};

#endif  // OTTO_MASTER_LINK_H
