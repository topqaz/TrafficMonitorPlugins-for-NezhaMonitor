#include "log.h"
void logMessage(const std::string& message) {
    std::ofstream logFile("nz_log.txt", std::ios::app);  // 追加模式
    if (logFile.is_open()) {
        // 获取当前时间
        std::time_t now = std::time(nullptr);
        char timeStr[64];

        tm timeInfo;
        localtime_s(&timeInfo, &now);  
        std::strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", &timeInfo);


        logFile << "[" << timeStr << "] " << message << std::endl;
    }
}