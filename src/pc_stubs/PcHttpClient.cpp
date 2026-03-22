/**
 * @file PcHttpClient.cpp
 * @brief PC IHttpClient implementation using WinHTTP (Windows) or stub (Linux/Mac)
 */

#include "crosspad/net/IHttpClient.hpp"
#include "crosspad/platform/PlatformServices.hpp"
#include <cstdio>

#ifdef _WIN32

#include <Windows.h>
#include <winhttp.h>
#include <string>
#include <vector>

#pragma comment(lib, "winhttp.lib")

namespace {

// Convert UTF-8 std::string to wide string
std::wstring to_wide(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring ws(len, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), ws.data(), len);
    return ws;
}

// Parse URL into components
struct UrlParts {
    std::wstring host;
    std::wstring path;
    INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
    bool https = true;
};

bool parse_url(const std::string& url, UrlParts& out) {
    std::wstring wurl = to_wide(url);

    URL_COMPONENTS uc = {};
    uc.dwStructSize = sizeof(uc);

    wchar_t hostBuf[256] = {};
    wchar_t pathBuf[2048] = {};
    uc.lpszHostName = hostBuf;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = pathBuf;
    uc.dwUrlPathLength = 2048;

    if (!WinHttpCrackUrl(wurl.c_str(), (DWORD)wurl.size(), 0, &uc)) {
        return false;
    }

    out.host = hostBuf;
    out.path = pathBuf;
    out.port = uc.nPort;
    out.https = (uc.nScheme == INTERNET_SCHEME_HTTPS);
    return true;
}

class PcHttpClient : public crosspad::IHttpClient {
public:
    bool isAvailable() const override { return true; }

    crosspad::HttpResponse get(const crosspad::HttpRequest& request) override {
        return perform(request, L"GET");
    }

    crosspad::HttpResponse post(const crosspad::HttpRequest& request) override {
        return perform(request, L"POST");
    }

private:
    crosspad::HttpResponse perform(const crosspad::HttpRequest& request,
                                    const wchar_t* method)
    {
        crosspad::HttpResponse resp;

        UrlParts parts;
        if (!parse_url(request.url, parts)) {
            resp.errorMessage = "Failed to parse URL";
            return resp;
        }

        HINTERNET hSession = WinHttpOpen(L"CrossPad/1.0",
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!hSession) {
            resp.errorMessage = "WinHttpOpen failed";
            return resp;
        }

        // Set timeouts
        DWORD timeout = request.timeoutMs;
        WinHttpSetTimeouts(hSession, timeout, timeout, timeout, timeout);

        HINTERNET hConnect = WinHttpConnect(hSession, parts.host.c_str(),
                                            parts.port, 0);
        if (!hConnect) {
            resp.errorMessage = "WinHttpConnect failed";
            WinHttpCloseHandle(hSession);
            return resp;
        }

        DWORD flags = parts.https ? WINHTTP_FLAG_SECURE : 0;
        HINTERNET hRequest = WinHttpOpenRequest(hConnect, method,
            parts.path.c_str(), nullptr, WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        if (!hRequest) {
            resp.errorMessage = "WinHttpOpenRequest failed";
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            return resp;
        }

        // Add Content-Type header for POST
        if (!request.contentType.empty()) {
            std::wstring header = L"Content-Type: " + to_wide(request.contentType);
            WinHttpAddRequestHeaders(hRequest, header.c_str(), (DWORD)-1,
                WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
        }

        // Add custom headers
        for (auto& h : request.headers) {
            std::wstring header = to_wide(h.first) + L": " + to_wide(h.second);
            WinHttpAddRequestHeaders(hRequest, header.c_str(), (DWORD)-1,
                WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
        }

        // Send request
        LPVOID body = request.body.empty() ? WINHTTP_NO_REQUEST_DATA
                                           : (LPVOID)request.body.data();
        DWORD bodyLen = (DWORD)request.body.size();

        BOOL ok = WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                     body, bodyLen, bodyLen, 0);
        if (!ok) {
            resp.errorMessage = "WinHttpSendRequest failed (error " +
                                std::to_string(GetLastError()) + ")";
            WinHttpCloseHandle(hRequest);
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            return resp;
        }

        ok = WinHttpReceiveResponse(hRequest, nullptr);
        if (!ok) {
            resp.errorMessage = "WinHttpReceiveResponse failed (error " +
                                std::to_string(GetLastError()) + ")";
            WinHttpCloseHandle(hRequest);
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            return resp;
        }

        // Read status code
        DWORD statusCode = 0;
        DWORD size = sizeof(statusCode);
        WinHttpQueryHeaders(hRequest,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &size,
            WINHTTP_NO_HEADER_INDEX);
        resp.statusCode = (int)statusCode;

        // Read response body
        std::string responseBody;
        DWORD bytesAvail = 0;
        do {
            bytesAvail = 0;
            WinHttpQueryDataAvailable(hRequest, &bytesAvail);
            if (bytesAvail == 0) break;

            // Cap at 64KB to avoid memory issues on embedded-like targets
            if (responseBody.size() + bytesAvail > 65536) {
                responseBody += "\n...(truncated)";
                break;
            }

            std::vector<char> buf(bytesAvail);
            DWORD bytesRead = 0;
            WinHttpReadData(hRequest, buf.data(), bytesAvail, &bytesRead);
            responseBody.append(buf.data(), bytesRead);
        } while (bytesAvail > 0);

        resp.body = std::move(responseBody);

        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);

        return resp;
    }
};

static PcHttpClient s_pcHttpClient;

} // anonymous namespace

void pc_http_client_init() {
    crosspad::getPlatformServices().setHttpClient(&s_pcHttpClient);
    printf("[PC] HTTP client initialized (WinHTTP)\n");
}

#else // Linux/Mac — no WinHTTP, use NullHttpClient default

void pc_http_client_init() {
    printf("[PC] HTTP client not available (no WinHTTP on this platform)\n");
}

#endif
