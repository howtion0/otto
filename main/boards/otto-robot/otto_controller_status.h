#ifndef OTTO_CONTROLLER_STATUS_H
#define OTTO_CONTROLLER_STATUS_H

#include <string>

// Queue or stop actions from non-MCP command paths such as the local master
// link and MQTT. Errors are returned in Chinese for direct display in WebUI.
bool QueueOttoAction(const std::string& action_name, int steps, int speed, int direction,
                     int amount, std::string& error);
bool StopOttoAction(std::string& error);
bool IsOttoIdleForConversation();
std::string GetOttoAvailableActionsJson();

// Returns a compact JSON snapshot of the current and last Otto action.
std::string GetOttoRuntimeStatusJson();

#endif  // OTTO_CONTROLLER_STATUS_H
