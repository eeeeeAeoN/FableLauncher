#include <windows.h>
#include <tlhelp32.h> 
#include <iostream>
#include <string>
#include <filesystem>
#include <vector>

std::string GetCurrentDirectory()
{
    char launcherPath[MAX_PATH];
    GetModuleFileNameA(NULL, launcherPath, MAX_PATH);
    return std::filesystem::path(launcherPath).parent_path().string();
}

bool InjectDLL(HANDLE hProcess, const std::string& dllPath)
{

    if (dllPath.empty())
    {
        std::cerr << "Error: InjectDLL called with an empty path." << std::endl;
        return false;
    }

    void* pRemotePath = VirtualAllocEx(hProcess, NULL, dllPath.length() + 1, MEM_COMMIT, PAGE_READWRITE);
    if (!pRemotePath)
    {
        std::cerr << "Error: VirtualAllocEx failed for " << dllPath << "! GetLastError = " << GetLastError() << std::endl;
        return false;
    }

    if (!WriteProcessMemory(hProcess, pRemotePath, dllPath.c_str(), dllPath.length() + 1, NULL))
    {
        std::cerr << "Error: WriteProcessMemory failed for " << dllPath << "! GetLastError = " << GetLastError() << std::endl;
        VirtualFreeEx(hProcess, pRemotePath, 0, MEM_RELEASE);
        return false;
    }

    HMODULE hKernel32 = GetModuleHandleA("kernel32.dll");
    FARPROC pLoadLibraryA = GetProcAddress(hKernel32, "LoadLibraryA");
    if (!pLoadLibraryA)
    {
        std::cerr << "Error: GetProcAddress for LoadLibraryA failed! GetLastError = " << GetLastError() << std::endl;
        VirtualFreeEx(hProcess, pRemotePath, 0, MEM_RELEASE);
        return false;
    }

    HANDLE hRemoteThread = CreateRemoteThread(hProcess, NULL, 0, (LPTHREAD_START_ROUTINE)pLoadLibraryA, pRemotePath, 0, NULL);
    if (!hRemoteThread)
    {
        std::cerr << "Error: CreateRemoteThread failed for " << dllPath << "! GetLastError = " << GetLastError() << std::endl;
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
        std::cerr << "Error: LoadLibraryA failed inside the game process." << std::endl;
        std::cerr << "  This could mean the DLL is missing, corrupt, or is missing dependencies." << std::endl;
        return false;
    }

    return true;
}

std::vector<std::string> GetEnabledMods(const std::string& iniPath)
{
    std::vector<std::string> enabledMods;
    char sectionBuffer[8192];

    DWORD bytesRead = GetPrivateProfileSectionA(
        "Mods",
        sectionBuffer,
        8192,
        iniPath.c_str()
    );

    if (bytesRead == 0)
    {
        std::cerr << "Warning: Could not read [Mods] section from INI or section is empty.\n";
        return enabledMods;
    }

    for (const char* p = sectionBuffer; *p;)
    {
        std::string entry(p);
        std::string modName;
        std::string isEnabled;

        size_t equalsPos = entry.find('=');
        if (equalsPos != std::string::npos)
        {
            modName = entry.substr(0, equalsPos);
            isEnabled = entry.substr(equalsPos + 1);

            if (isEnabled == "1")
            {
                enabledMods.push_back(modName);
            }
        }

        p += entry.length() + 1;
    }

    return enabledMods;
}


int main()
{
    const char* g_gameExeName = "Fable.exe";
    const std::string g_scriptExtenderName = "FableScriptExtender.dll";

    std::cout << "Fable Script Extender Launcher\n";
    std::cout << "--------------------------------\n";

    std::string currentDir = GetCurrentDirectory();
    std::string gamePath = currentDir + "\\" + g_gameExeName;
    std::string iniPath = currentDir + "\\Mods.ini";

    char modsDir[MAX_PATH];
    GetPrivateProfileStringA(
        "Settings",
        "ModsDirectory",
        ".\\Mods",
        modsDir,
        MAX_PATH,
        iniPath.c_str()
    );
    std::string modsPath = currentDir + "\\" + modsDir;

    std::cout << "Launcher Path: " << currentDir << "\n";
    std::cout << "Target EXE:    " << gamePath << "\n";
    std::cout << "Config File:   " << iniPath << "\n";
    std::cout << "Mods Path:     " << modsPath << "\n\n";

    std::vector<std::string> modsToInject = GetEnabledMods(iniPath);
    if (modsToInject.empty())
    {
        std::cout << "No enabled mods found in Mods.ini. Launching game without mods...\n";
    }
    else
    {
        std::cout << "Found " << modsToInject.size() << " mod(s) to inject:\n";
        for (const auto& mod : modsToInject)
        {
            std::cout << "  - " << mod << "\n";
        }
    }
    std::cout << "\n";


    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    ZeroMemory(&pi, sizeof(pi));
    si.cb = sizeof(si);

    std::cout << "Launching " << g_gameExeName << " in suspended mode...\n";
    if (!CreateProcessA(
        gamePath.c_str(),
        NULL,
        NULL,
        NULL,
        FALSE,
        CREATE_SUSPENDED,
        NULL,
        currentDir.c_str(),
        &si,
        &pi
    ))
    {
        std::cerr << "Error: CreateProcessA failed! GetLastError = " << GetLastError() << std::endl;
        std::cerr << "Is Fable.exe in the same folder as the launcher?\n";
        system("pause");
        return 1;
    }

    bool allInjectionsSucceeded = true;
    if (!modsToInject.empty())
    {
        std::cout << "Injecting mods...\n";
        for (const auto& modName : modsToInject)
        {
            std::string fullDllPath;

            if (modName == g_scriptExtenderName)
            {
                fullDllPath = currentDir + "\\" + modName;
                std::cout << "  Injecting (root): " << modName << "... ";
            }
            else
            {
                fullDllPath = modsPath + "\\" + modName;
                std::cout << "  Injecting (mods): " << modName << "... ";
            }

            if (!std::filesystem::exists(fullDllPath))
            {
                std::cout << "FAILED.\n";
                std::cerr << "  Error: File not found at path: " << fullDllPath << "\n";
                allInjectionsSucceeded = false;
                continue;
            }


            if (InjectDLL(pi.hProcess, fullDllPath))
            {
                std::cout << "Success.\n";
            }
            else
            {
                std::cout << "FAILED.\aws\n";
                allInjectionsSucceeded = false;
            }
        }
    }

    if (allInjectionsSucceeded)
    {
        std::cout << "All injections successful.\n";
        std::cout << "Resuming game thread...\n";
        ResumeThread(pi.hThread);
    }
    else
    {
        std::cerr << "!!! At least one injection FAILED. Terminating game. !!!\n";
        TerminateProcess(pi.hProcess, 1);
        system("pause");
    }

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    std::cout << "Launcher finished. The game is now running.\n";
    return 0;
}