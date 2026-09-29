#include "adb_manager.h"
#include <windows.h>
#include <iostream>
#include <sstream>
#include <vector>

namespace boulecam {

static std::string FindAdbExecutable() {
    // 1. Check local directory
    if (GetFileAttributesA("adb.exe") != INVALID_FILE_ATTRIBUTES) {
        return "adb.exe";
    }

    // 2. Check system PATH
    char pathBuffer[MAX_PATH];
    if (SearchPathA(NULL, "adb.exe", NULL, MAX_PATH, pathBuffer, NULL) > 0) {
        return pathBuffer;
    }

    // 3. Check %LOCALAPPDATA%\Android\Sdk\platform-tools\adb.exe
    char localAppData[MAX_PATH];
    if (GetEnvironmentVariableA("LOCALAPPDATA", localAppData, MAX_PATH) > 0) {
        std::string candidate = std::string(localAppData) + "\\Android\\Sdk\\platform-tools\\adb.exe";
        if (GetFileAttributesA(candidate.c_str()) != INVALID_FILE_ATTRIBUTES) {
            return candidate;
        }
    }

    // 4. Check %USERPROFILE%\AppData\Local\Android\Sdk\platform-tools\adb.exe
    char userProfile[MAX_PATH];
    if (GetEnvironmentVariableA("USERPROFILE", userProfile, MAX_PATH) > 0) {
        std::string candidate = std::string(userProfile) + "\\AppData\\Local\\Android\\Sdk\\platform-tools\\adb.exe";
        if (GetFileAttributesA(candidate.c_str()) != INVALID_FILE_ATTRIBUTES) {
            return candidate;
        }
    }

    return "adb.exe";
}

AdbManager::AdbManager()
    : m_localPort(8088)
    , m_phonePort(8088)
    , m_isRunning(false) {
}

AdbManager::~AdbManager() {
    Stop();
}

bool AdbManager::IsAdbInstalled() {
    std::string adbPath = FindAdbExecutable();
    return GetFileAttributesA(adbPath.c_str()) != INVALID_FILE_ATTRIBUTES || SearchPathA(NULL, "adb.exe", NULL, 0, NULL, NULL) > 0;
}

static std::vector<std::string> GetAttachedDeviceSerials() {
    std::string adbExe = FindAdbExecutable();
    std::string cmd = "\"" + adbExe + "\" devices";
    std::vector<std::string> serials;

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE hRead = NULL;
    HANDLE hWrite = NULL;
    if (!CreatePipe(&hRead, &hWrite, &sa, 0)) return serials;
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si{};
    PROCESS_INFORMATION pi{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdOutput = hWrite;
    si.hStdError = hWrite;
    si.wShowWindow = SW_HIDE;

    std::vector<char> cmdVec(cmd.begin(), cmd.end());
    cmdVec.push_back('\0');

    if (CreateProcessA(NULL, cmdVec.data(), NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        CloseHandle(hWrite);
        hWrite = NULL;

        std::string output;
        char buffer[256];
        DWORD bytesRead = 0;
        while (ReadFile(hRead, buffer, sizeof(buffer) - 1, &bytesRead, NULL) && bytesRead > 0) {
            buffer[bytesRead] = '\0';
            output.append(buffer, bytesRead);
        }

        WaitForSingleObject(pi.hProcess, 2000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);

        std::istringstream stream(output);
        std::string line;
        while (std::getline(stream, line)) {
            size_t tabPos = line.find('\t');
            if (tabPos != std::string::npos) {
                std::string state = line.substr(tabPos + 1);
                if (state.find("device") != std::string::npos) {
                    serials.push_back(line.substr(0, tabPos));
                }
            }
        }
    } else {
        if (hWrite) CloseHandle(hWrite);
    }
    if (hRead) CloseHandle(hRead);

    return serials;
}

bool AdbManager::ExecuteAdbReverse(uint16_t localPort, uint16_t phonePort) {
    std::string adbExe = FindAdbExecutable();
    std::vector<std::string> serials = GetAttachedDeviceSerials();

    if (serials.empty()) {
        std::ostringstream cmd;
        cmd << "\"" << adbExe << "\" reverse tcp:" << phonePort << " tcp:" << localPort;
        STARTUPINFOA si{};
        PROCESS_INFORMATION pi{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        std::string cmdStr = cmd.str();
        std::vector<char> cmdVec(cmdStr.begin(), cmdStr.end());
        cmdVec.push_back('\0');
        if (CreateProcessA(NULL, cmdVec.data(), NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
            WaitForSingleObject(pi.hProcess, 2000);
            DWORD exitCode = 1;
            GetExitCodeProcess(pi.hProcess, &exitCode);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            return exitCode == 0;
        }
        return false;
    }

    bool anyOk = false;
    for (const auto& serial : serials) {
        std::ostringstream cmd;
        cmd << "\"" << adbExe << "\" -s " << serial << " reverse tcp:" << phonePort << " tcp:" << localPort;

        STARTUPINFOA si{};
        PROCESS_INFORMATION pi{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;

        std::string cmdStr = cmd.str();
        std::vector<char> cmdVec(cmdStr.begin(), cmdStr.end());
        cmdVec.push_back('\0');

        if (CreateProcessA(NULL, cmdVec.data(), NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
            WaitForSingleObject(pi.hProcess, 2000);
            DWORD exitCode = 1;
            GetExitCodeProcess(pi.hProcess, &exitCode);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            if (exitCode == 0) anyOk = true;
        }
    }
    return anyOk;
}

bool AdbManager::StartAutoReverse(uint16_t localPort, uint16_t phonePort) {
    if (m_isRunning.load()) return true;

    m_localPort = localPort;
    m_phonePort = phonePort;
    m_isRunning.store(true);

    m_monitorThread = std::thread(&AdbManager::MonitorThreadWorker, this);
    return true;
}

void AdbManager::Stop() {
    m_isRunning.store(false);
    if (m_monitorThread.joinable()) {
        m_monitorThread.join();
    }
}

void AdbManager::MonitorThreadWorker() {
    std::string adbPath = FindAdbExecutable();
    std::cout << "[AdbManager] Starting USB ADB monitoring daemon for Android (ADB: " << adbPath << ")..." << std::endl;
    while (m_isRunning.load()) {
        // Periodically verify reverse port mapping and device connection
        bool connected = ExecuteAdbReverse(m_localPort, m_phonePort);
        m_isDeviceConnected.store(connected);
        
        // Sleep in small intervals to allow immediate shutdown (approx 1.5 seconds)
        for (int i = 0; i < 15 && m_isRunning.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    m_isDeviceConnected.store(false);
}

} // namespace boulecam
