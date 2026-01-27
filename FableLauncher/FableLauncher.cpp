#include <windows.h>
#include <tlhelp32.h> 
#include <string>
#include <filesystem>
#include <vector>
#include <sstream>

std::string GetLastErrorAsString()
{
    DWORD errorMessageID = ::GetLastError();
    if (errorMessageID == 0) return "No error message has been recorded";

    LPSTR messageBuffer = nullptr;
    size_t size = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL, errorMessageID, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPSTR)&messageBuffer, 0, NULL);

    std::string message(messageBuffer, size);
    LocalFree(messageBuffer);
    return message;
}

std::string GetCurrentDirectoryPath()
{
    char launcherPath[MAX_PATH];
    GetModuleFileNameA(NULL, launcherPath, MAX_PATH);
    return std::filesystem::path(launcherPath).parent_path().string();
}

bool InjectDLL(HANDLE hProcess, const std::string& dllPath, std::string& outError)
{
    if (dllPath.empty())
    {
        outError = "InjectDLL called with an empty path.";
        return false;
    }

    void* pRemotePath = VirtualAllocEx(hProcess, NULL, dllPath.length() + 1, MEM_COMMIT, PAGE_READWRITE);
    if (!pRemotePath)
    {
        outError = "VirtualAllocEx failed for " + dllPath + "\n" + GetLastErrorAsString();
        return false;
    }

    if (!WriteProcessMemory(hProcess, pRemotePath, dllPath.c_str(), dllPath.length() + 1, NULL))
    {
        outError = "WriteProcessMemory failed for " + dllPath + "\n" + GetLastErrorAsString();
        VirtualFreeEx(hProcess, pRemotePath, 0, MEM_RELEASE);
        return false;
    }

    HMODULE hKernel32 = GetModuleHandleA("kernel32.dll");
    FARPROC pLoadLibraryA = GetProcAddress(hKernel32, "LoadLibraryA");
    if (!pLoadLibraryA)
    {
        outError = "GetProcAddress for LoadLibraryA failed.\n" + GetLastErrorAsString();
        VirtualFreeEx(hProcess, pRemotePath, 0, MEM_RELEASE);
        return false;
    }

    HANDLE hRemoteThread = CreateRemoteThread(hProcess, NULL, 0, (LPTHREAD_START_ROUTINE)pLoadLibraryA, pRemotePath, 0, NULL);
    if (!hRemoteThread)
    {
        outError = "CreateRemoteThread failed for " + dllPath + "\n" + GetLastErrorAsString();
        VirtualFreeEx(hProcess, pRemotePath, 0, MEM_RELEASE);
        return false;
    }

    WaitForSingleObject(hRemoteThread, INFINITE);

    DWORD exitCode = 0;
    GetExitCodeThread(hRemoteThread, &exitCode);

    CloseHandle(hRemoteThread);
    VirtualFreeEx(hProcess, pRemotePath, 0, MEM_RELEASE);

    if (exitCode == 0)
    {
        outError = "LoadLibraryA failed inside the game process for: " + dllPath +
            "\nThis implies missing dependencies (like VC++ Redists) or a corrupt DLL.";
        return false;
    }

    return true;
}

std::vector<std::string> GetEnabledMods(const std::string& iniPath)
{
    std::vector<std::string> enabledMods;
    char sectionBuffer[8192];

    DWORD bytesRead = GetPrivateProfileSectionA("Mods", sectionBuffer, 8192, iniPath.c_str());

    if (bytesRead == 0) return enabledMods;

    for (const char* p = sectionBuffer; *p;)
    {
        std::string entry(p);
        size_t equalsPos = entry.find('=');
        if (equalsPos != std::string::npos)
        {
            std::string modName = entry.substr(0, equalsPos);
            std::string isEnabled = entry.substr(equalsPos + 1);
            if (isEnabled == "1") enabledMods.push_back(modName);
        }
        p += entry.length() + 1;
    }
    return enabledMods;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
    const char* g_gameExeName = "Fable.exe";
    const std::string g_scriptExtenderName = "FableScriptExtender.dll";

    std::string currentDir = GetCurrentDirectoryPath();
    std::string gamePath = currentDir + "\\" + g_gameExeName;
    std::string iniPath = currentDir + "\\Mods.ini";

    char modsDir[MAX_PATH];
    GetPrivateProfileStringA("Settings", "ModsDirectory", ".\\Mods", modsDir, MAX_PATH, iniPath.c_str());
    std::string modsPath = currentDir + "\\" + modsDir;

    std::vector<std::string> modsToInject = GetEnabledMods(iniPath);

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    ZeroMemory(&pi, sizeof(pi));
    si.cb = sizeof(si);

    if (!CreateProcessA(gamePath.c_str(), NULL, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, currentDir.c_str(), &si, &pi))
    {
        std::string err = "Could not launch " + std::string(g_gameExeName) + "!\n\nCheck that the launcher is in the same folder as the game.\n\n" + GetLastErrorAsString();
        MessageBoxA(NULL, err.c_str(), "Launcher Error", MB_ICONERROR);
        return 1;
    }

    bool allInjectionsSucceeded = true;
    std::string firstErrorMsg = "";

    if (!modsToInject.empty())
    {
        for (const auto& modName : modsToInject)
        {
            std::string fullDllPath;
            std::string errorDetails;

            if (modName == g_scriptExtenderName)
                fullDllPath = currentDir + "\\" + modName;
            else
                fullDllPath = modsPath + "\\" + modName;

            if (!std::filesystem::exists(fullDllPath))
            {
                allInjectionsSucceeded = false;
                firstErrorMsg = "Mod file not found:\n" + fullDllPath;
                break;
            }

            if (!InjectDLL(pi.hProcess, fullDllPath, errorDetails))
            {
                allInjectionsSucceeded = false;
                firstErrorMsg = "Failed to inject " + modName + ":\n" + errorDetails;
                break;
            }
        }
    }

    if (allInjectionsSucceeded)
    {
        ResumeThread(pi.hThread);
    }
    else
    {
        TerminateProcess(pi.hProcess, 1);
        MessageBoxA(NULL, firstErrorMsg.c_str(), "Injection Failed", MB_ICONERROR);
    }

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    return 0;
}