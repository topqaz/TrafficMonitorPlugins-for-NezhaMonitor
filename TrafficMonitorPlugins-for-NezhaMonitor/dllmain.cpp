#include "pch.h"
#include "PluginInterface.h"
#include <windows.h>
#include "api.h"
#include "SettingsDialog.h"
#include <iomanip>
#include <cmath>
#include <string>
#include <fstream>
#include <memory>
#include <sstream>
#include <vector>
#include <unordered_map>
#include <filesystem>
#include <thread>
#include "log.h"
namespace fs = std::filesystem;


static std::string WideToUtf8(const std::wstring& w)
{
    if (w.empty()) return std::string();
    int size = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (size <= 0) return std::string();
    std::string out((size_t)size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), out.data(), size, nullptr, nullptr);
    return out;
}

class NezhaItem : public IPluginItem {
public:
    std::wstring valueText = L"等待数据...";
    std::wstring labelText;

    NezhaItem(const std::wstring& label = L"") : labelText(label) {}

    const wchar_t* GetItemName() const override { return labelText.c_str(); }
    const wchar_t* GetItemId() const override { return labelText.c_str(); }
    const wchar_t* GetItemLableText() const override { return labelText.c_str(); }
    const wchar_t* GetItemValueText() const override { return valueText.c_str(); }
    const wchar_t* GetItemValueSampleText() const override { return L"sample"; }
};


class NezhaPlugin : public ITMPlugin {
private:
    struct ServerItems { int id; NezhaItem* cpu; NezhaItem* mem; NezhaItem* disk; NezhaItem* net; };
    std::vector<std::unique_ptr<NezhaItem>> allItems;
    std::vector<ServerItems> itemsByServer;
    NezhaItem* totalNetItem; 

    fs::path configDir;
    std::string serverUrl, username, password;
    std::vector<int> serverIds{ 1,2,3 };
    std::unique_ptr<NezhaAPI> api;
    struct NetPrev { double inBytes{ 0 }; double outBytes{ 0 }; unsigned long long lastMs{ 0 }; };
    std::unordered_map<int, NetPrev> serverNetPrev;

public:
    NezhaPlugin() {

        const int MAX_SERVERS = 6;
        const int ITEMS_PER_SERVER = 4;
        int requiredItems = MAX_SERVERS * ITEMS_PER_SERVER + 1;
        while ((int)allItems.size() < requiredItems) {
            allItems.emplace_back(std::make_unique<NezhaItem>(L""));
        }

        totalNetItem = allItems[requiredItems - 1].get();
        totalNetItem->labelText = L"总网速";
        rebuildItems();
    }


    bool TestConnection(const SettingsDialog::SettingsData& settings) {
        try {
            NezhaAPI testApi(settings.serverUrl, settings.username, settings.password);
            auto fut = testApi.getServers();
            auto result = fut.get();
            return !result.contains("error");
        }
        catch (...) {
            return false;
        }
    }

    // 设置改变回调函数
    void OnSettingsChanged(const SettingsDialog::SettingsData& settings) {
        serverUrl = settings.serverUrl;
        username = settings.username;
        password = settings.password;
        serverIds = settings.serverIds;
        saveConfig();
        api.reset();
        rebuildItems();
    }
    IPluginItem* GetItem(int index) override {
        if (index < 0 || index >= (int)allItems.size()) return nullptr;
        return allItems[index].get();
    }

    void DataRequired() override {
        try {
            if (!api && !serverUrl.empty()) {
                api = std::make_unique<NezhaAPI>(serverUrl, username, password);
            }
            
            if (api) {
                double totalUpBps = 0.0, totalDownBps = 0.0;

                for (auto& s : itemsByServer) {
                    try {
                        auto fut = api->getServer(s.id);
                        auto jsonData = fut.get();
                        if (jsonData.contains("state") && jsonData.contains("host")) {
                            auto state = jsonData["state"]; auto host = jsonData["host"];
                            auto round2 = [](double value) { return std::round(value * 100.0) / 100.0; };
                            double cpu = round2(state.value("cpu", 0.0));
                            double memUsed = state.value("mem_used", 0.0);
                            double diskUsed = state.value("disk_used", 0.0);
                            double memTotal = host.value("mem_total", 0.0);
                            double diskTotal = host.value("disk_total", 0.0);
                            double memUsage = (memTotal > 0) ? round2((memUsed / memTotal) * 100.0) : 0.0;
                            double diskUsage = (diskTotal > 0) ? round2((diskUsed / diskTotal) * 100.0) : 0.0;
                            double memGB = round2(memUsed / (1024.0 * 1024.0 * 1024.0));
                            double diskGB = round2(diskUsed / (1024.0 * 1024.0 * 1024.0));
							double diskTotalGB = round2(diskTotal / (1024.0 * 1024.0 * 1024.0));
							double memTotalGB = round2(memTotal / (1024.0 * 1024.0 * 1024.0));

                            if (s.cpu) { std::wstringstream ss; ss << cpu << L"%"; s.cpu->valueText = ss.str(); }
                            if (s.mem) { std::wstringstream ss; ss << memUsage << L"% (" << memTotalGB << L"GB)"; s.mem->valueText = ss.str(); }
                            if (s.disk) { std::wstringstream ss; ss << diskUsage << L"% (" << diskTotalGB << L"GB)"; s.disk->valueText = ss.str(); }

                            double upBps = 0.0, downBps = 0.0;
                            if (state.contains("net_out_speed") || state.contains("net_in_speed")) {
                                upBps = state.value("net_out_speed", 0.0);
                                downBps = state.value("net_in_speed", 0.0);
                            }
                            else if (state.contains("net_out") || state.contains("net_in")) {
                                double outBytes = state.value("net_out", 0.0);
                                double inBytes = state.value("net_in", 0.0);
                                unsigned long long now = GetTickCount64();
                                auto& prev = serverNetPrev[s.id];
                                if (prev.lastMs != 0) {
                                    double dt = (double)(now - prev.lastMs) / 1000.0;
                                    if (dt > 0.0) {
                                        upBps = (outBytes - prev.outBytes) / dt;
                                        downBps = (inBytes - prev.inBytes) / dt;
                                        if (upBps < 0) upBps = 0; if (downBps < 0) downBps = 0;
                                    }
                                }
                                prev.outBytes = outBytes; prev.inBytes = inBytes; prev.lastMs = now;
                            }

                            // 累加到总网速
                            totalUpBps += upBps;
                            totalDownBps += downBps;

                            auto fmtSpeed = [](double bps) -> std::wstring {
                                const double KB = 1024.0, MB = KB * 1024.0, GB = MB * 1024.0;
                                std::wstringstream ss; ss << std::fixed << std::setprecision(2);
                                if (bps >= GB) { ss << (bps / GB) << L" GB/s"; }
                                else if (bps >= MB) { ss << (bps / MB) << L" MB/s"; }
                                else if (bps >= KB) { ss << (bps / KB) << L" KB/s"; }
                                else { ss << bps << L" B/s"; }
                                return ss.str();
                                };
                            if (s.net) {
                                // 计算总速度（上行+下行）
                                double totalSpeed = upBps + downBps;
                                std::wstringstream ss;
                                ss << L"↑↓: " << fmtSpeed(totalSpeed);
                                s.net->valueText = ss.str();
                            }
                        }
                        else {
                            if (s.cpu) s.cpu->valueText = L"无state数据";
                            if (s.mem) s.mem->valueText = L"无state数据";
                            if (s.disk) s.disk->valueText = L"无state数据";
                            if (s.net) s.net->valueText = L"无state数据";
                        }
                    }
                    catch (...) {
                        if (s.cpu) s.cpu->valueText = L"获取失败";
                        if (s.mem) s.mem->valueText = L"获取失败";
                        if (s.disk) s.disk->valueText = L"获取失败";
                        if (s.net) s.net->valueText = L"获取失败";
                    }
                }

                // 更新总网速显示
                if (totalNetItem) {
                    auto fmtSpeed = [](double bps) -> std::wstring {
                        const double KB = 1024.0, MB = KB * 1024.0, GB = MB * 1024.0;
                        std::wstringstream ss; ss << std::fixed << std::setprecision(2);
                        if (bps >= GB) { ss << (bps / GB) << L" GB/s"; }
                        else if (bps >= MB) { ss << (bps / MB) << L" MB/s"; }
                        else if (bps >= KB) { ss << (bps / KB) << L" KB/s"; }
                        else { ss << bps << L" B/s"; }
                        return ss.str();
                        };

                    // 计算总速度（上行+下行）
                    double totalSpeed = totalUpBps + totalDownBps;
                    std::wstringstream ss;
                    ss << L"↑↓: " << fmtSpeed(totalSpeed);
                    totalNetItem->valueText = ss.str();
                }
            }
        }
        catch (...) {
            for (auto& it : allItems) it->valueText = L"获取失败";
        }
    }

    OptionReturn ShowOptionsDialog(void* hParent) override {
        HWND hParentWnd = reinterpret_cast<HWND>(hParent);

        // 准备设置数据
        SettingsDialog::SettingsData settings;
        settings.serverUrl = serverUrl;
        settings.username = username;
        settings.password = password;
        settings.serverIds = serverIds;

        // 创建设置对话框
        SettingsDialog dialog;
        dialog.SetTestConnectionCallback([this](const SettingsDialog::SettingsData& settings) {
            return TestConnection(settings);
            });
        dialog.SetSettingsChangedCallback([this](const SettingsDialog::SettingsData& settings) {
            OnSettingsChanged(settings);
            });

        // 显示对话框
        bool changed = dialog.ShowModal(hParentWnd, settings);

        return changed ? OR_OPTION_CHANGED : OR_OPTION_UNCHANGED;
    }

    const wchar_t* GetInfo(PluginInfoIndex index) override {
        switch (index) {
        case TMI_NAME: return L"nezha_monitor";
        case TMI_DESCRIPTION: return L"哪吒监控插件";
        case TMI_AUTHOR: return L"topqaz";
        case TMI_COPYRIGHT: return L"Copyright(C)by topqaz";
        case TMI_VERSION: return L"1.0";
        case TMI_URL: return L"https://github.com/topqaz/TrafficMonitorPlugins-for-NezhaMonitor";
        default: return L"";
        }
    }

    void OnExtenedInfo(ExtendedInfoIndex index, const wchar_t* data) override {
        if (index == EI_CONFIG_DIR) {
            configDir = fs::path(data);
            loadConfig();
            rebuildItems();
        }
    }

    const wchar_t* GetTooltipInfo() override {
        static std::wstring tooltip;
        tooltip.clear();

        // 添加服务器配置信息
        if (!serverUrl.empty()) {
            tooltip += L"服务器: " + std::wstring(serverUrl.begin(), serverUrl.end()) + L"\n";
        }
        if (!username.empty()) {
            tooltip += L"用户名: " + std::wstring(username.begin(), username.end()) + L"\n";
        }

        // 添加服务器ID列表
        if (!serverIds.empty()) {
            tooltip += L"监控服务器ID: ";
            for (size_t i = 0; i < serverIds.size(); ++i) {
                if (i > 0) tooltip += L", ";
                tooltip += std::to_wstring(serverIds[i]);
            }
            tooltip += L"\n";
        }

        // 添加当前监控状态
        tooltip += L"状态: ";
        if (api) {
            tooltip += L"已连接\n";

            // 尝试获取服务器详细信息
            try {
                for (const auto& s : itemsByServer) {
                    if (s.cpu && s.mem && s.disk && s.net) {
                        tooltip += L"\n服务器 " + std::to_wstring(s.id) + L":\n";

                        // 显示CPU信息
                        if (!s.cpu->valueText.empty() && s.cpu->valueText != L"等待数据..." && s.cpu->valueText != L"获取失败" && s.cpu->valueText != L"无state数据") {
                            tooltip += L"  CPU: " + s.cpu->valueText + L"\n";
                        }

                        // 显示内存信息
                        if (!s.mem->valueText.empty() && s.mem->valueText != L"等待数据..." && s.mem->valueText != L"获取失败" && s.mem->valueText != L"无state数据") {
                            tooltip += L"  内存: " + s.mem->valueText + L"\n";
                        }

                        // 显示磁盘信息
                        if (!s.disk->valueText.empty() && s.disk->valueText != L"等待数据..." && s.disk->valueText != L"获取失败" && s.disk->valueText != L"无state数据") {
                            tooltip += L"  磁盘: " + s.disk->valueText + L"\n";
                        }

                        // 显示网络信息
                        if (!s.net->valueText.empty() && s.net->valueText != L"等待数据..." && s.net->valueText != L"获取失败" && s.net->valueText != L"无state数据") {
                            tooltip += L"  网络: " + s.net->valueText + L"\n";
                        }
                    }
                }
            }
            catch (...) {
                tooltip += L" (获取详细信息失败)";
            }
        }
        else if (!serverUrl.empty()) {
            tooltip += L"未连接";
        }
        else {
            tooltip += L"未配置";
        }

        return tooltip.empty() ? L"" : tooltip.c_str();
    }

    void saveConfig() {
        if (configDir.empty()) return;
        try {
            fs::create_directories(configDir);
            std::ofstream ofs(configDir / "nezha_config.txt");
            std::stringstream ids;
            for (size_t i = 0; i < serverIds.size(); ++i) { if (i) ids << ","; ids << serverIds[i]; }
            ofs << serverUrl << "\n" << username << "\n" << password << "\n" << ids.str();
        }
        catch (...) {}
    }

    void loadConfig() {
        try {
            std::ifstream ifs(configDir / "nezha_config.txt");
            if (ifs) {
                std::string idsLine;
                std::getline(ifs, serverUrl);
                std::getline(ifs, username);
                std::getline(ifs, password);
                serverIds.clear();
                if (std::getline(ifs, idsLine)) {
                    bool parsedAny = false;
                    std::stringstream ss(idsLine);
                    std::string token;
                    while (std::getline(ss, token, ',')) {
                        try { int id = std::stoi(token); if (id > 0) { serverIds.push_back(id); parsedAny = true; } }
                        catch (...) {}
                    }
                    if (!parsedAny) {
                        try { int id = std::stoi(idsLine); if (id > 0) serverIds.push_back(id); }
                        catch (...) {}
                    }
                }
                if (serverIds.empty()) serverIds = { 7 };
            }
        }
        catch (...) {}
    }
private:
    void rebuildItems()
    {
        const int MAX_SERVERS = 6; 
        const int ITEMS_PER_SERVER = 4;

        itemsByServer.clear();

        int used = (int)serverIds.size();
        if (used > MAX_SERVERS) used = MAX_SERVERS;
        for (int i = 0; i < MAX_SERVERS; ++i) {
            int base = i * ITEMS_PER_SERVER;
            NezhaItem* cpu = allItems[base + 0].get();
            NezhaItem* mem = allItems[base + 1].get();
            NezhaItem* disk = allItems[base + 2].get();
            NezhaItem* net = allItems[base + 3].get();
            if (i < used) {
                int id = serverIds[i];
                std::wstringstream cpuL; cpuL << L"S" << id << L"CPU";
                std::wstringstream memL; memL << L"S" << id << L"内存";
                std::wstringstream diskL; diskL << L"S" << id << L"磁盘";
                std::wstringstream netL; netL << L"S" << id << L"网速";
                cpu->labelText = cpuL.str(); mem->labelText = memL.str(); disk->labelText = diskL.str(); net->labelText = netL.str();
                itemsByServer.push_back(ServerItems{ id, cpu, mem, disk, net });
            }
            else {
                cpu->labelText = L""; mem->labelText = L""; disk->labelText = L""; net->labelText = L"";
                cpu->valueText = mem->valueText = disk->valueText = net->valueText = L"";
            }
        }

        if (totalNetItem) {
            totalNetItem->labelText = L"总网速";
            totalNetItem->valueText = L"等待数据...";
        }
    }
};

static NezhaPlugin g_plugin;

extern "C" __declspec(dllexport) ITMPlugin* TMPluginGetInstance() {
    return &g_plugin;
}
