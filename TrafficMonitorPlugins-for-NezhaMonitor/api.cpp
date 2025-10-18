#include "pch.h"
#include "api.h"
#include <curl/curl.h>
#include <stdexcept>
#include "log.h"
NezhaAPI::NezhaAPI(const std::string& dashboardUrl,
    const std::string& username,
    const std::string& password)
    : baseUrl(trimSlash(dashboardUrl) + "/api/v1"),
    username(username),
    password(password) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

NezhaAPI::~NezhaAPI() {
    curl_global_cleanup();
}


std::future<json> NezhaAPI::getServer(int id) {
    // 获取所有服务器数据
    return std::async(std::launch::async, [this, id]() {
        authenticate();
        std::string url = baseUrl + "/server";
        json resp = httpRequest("GET", url, "", token);
        
        // 筛选指定id的服务器
        if (resp.contains("data") && resp["data"].is_array()) {
            for (const auto& server : resp["data"]) {
                if (server.contains("id") && server["id"] == id) {
                    return server;
                }
            }
            // 未找到则返回错误信息
            //logMessage("返回: " + resp.dump());
            return json{ {"error", "未找到指定id的服务器"}, {"id", id} };

        }
        // 返回原始响应（可能是错误或格式不符）
        //logMessage("返回: " + resp.dump());
        return resp;
        });
}

std::future<json> NezhaAPI::getServers() {
    // 获取所有服务器数据
    return std::async(std::launch::async, [this]() {
        authenticate();
        std::string url = baseUrl + "/server";
        json resp = httpRequest("GET", url, "", token);

        // 筛选指定id的服务器
        if (resp.contains("data") && resp["data"].is_array()) {
            for (const auto& server : resp["data"]) {
                if (server.contains("id")) {
                    return server;
                }
            }
            // 未找到则返回错误信息
            return json{ {"error", "未找到服务器"} };
        }
        // 返回原始响应（可能是错误或格式不符）
        return resp;
        });
}

bool NezhaAPI::testConnection() {
    try {
        authenticate();
        return true;
    }
    catch (...) {
        return false;
    }
}
std::string NezhaAPI::trimSlash(const std::string& url) {
    if (!url.empty() && url.back() == '/')
        return url.substr(0, url.size() - 1);
    return url;
}

size_t NezhaAPI::writeCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    ((std::string*)userp)->append((char*)contents, size * nmemb);
    return size * nmemb;
}

json NezhaAPI::httpRequest(const std::string& method,
    const std::string& url,
    const std::string& body,
    const std::string& auth,
    bool retry /*= true*/)
{
    CURL* curl = curl_easy_init();
    if (!curl) throw std::runtime_error("Failed to init curl");

    std::string response;
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    if (!auth.empty()) {
        headers = curl_slist_append(headers, ("Authorization: Bearer " + auth).c_str());
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

    if (method == "POST") {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    }

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);

    if (res != CURLE_OK) {
        throw std::runtime_error("CURL error: " + std::string(curl_easy_strerror(res)));
    }

    json resp;
    try {
        resp = json::parse(response);
    }
    catch (...) {
        return json{ {"error", "Invalid JSON response"}, {"raw", response} };
    }

    // 统一处理 401 和 ApiErrorUnauthorized
    if (retry && ((resp.contains("status") && resp["status"] == 401) ||
        (resp.contains("error") && resp["error"] == "ApiErrorUnauthorized"))) {
        token.clear();
        authenticate();
        return httpRequest(method, url, body, token, false);
    }

    return resp;
}

void NezhaAPI::authenticate() {
    std::lock_guard<std::mutex> lock(mtx);
    if (!token.empty()) return;

    std::string url = baseUrl + "/login";
    json payload = { {"username", username}, {"password", password} };
    json resp = httpRequest("POST", url, payload.dump(), "");

    if (resp.contains("success") && resp["success"].get<bool>() &&
        resp.contains("data") && resp["data"].contains("token")) {
        token = resp["data"]["token"].get<std::string>();
    }
    else {
        throw std::runtime_error("认证失败，请检查用户名和密码");
    }
}

std::future<json> NezhaAPI::requestAsync(const std::string& method,
    const std::string& endpoint) {
    return std::async(std::launch::async, [this, method, endpoint]() {
        authenticate();
        return httpRequest(method, baseUrl + endpoint, "", token);
        });
}
