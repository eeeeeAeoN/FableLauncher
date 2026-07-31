#include <windows.h>

#include <filesystem>
#include <string>
#include <vector>
#include <algorithm>

namespace fs = std::filesystem;

constexpr DWORD DEFAULT_INIT_DELAY = 3000;
constexpr DWORD DEFAULT_PER_MOD_DELAY = 150;

const std::string SCRIPT_EXTENDER = "FableScriptExtender.dll";

fs::path GetModuleDirectory()
{
    char buffer[MAX_PATH];
    GetModuleFileNameA(nullptr, buffer, MAX_PATH);

    return fs::path(buffer).parent_path();
}

bool IsSupportedExecutable()
{
    char path[MAX_PATH]{};

    GetModuleFileNameA(nullptr, path, MAX_PATH);

    return _stricmp(
        std::filesystem::path(path).filename().string().c_str(),
        "Fable.exe") == 0;
}

std::vector<std::string> GetEnabledMods(const fs::path& iniPath)
{
    std::vector<std::string> enabledMods;

    char section[8192]{};

    DWORD bytesRead = GetPrivateProfileSectionA(
        "Mods",
        section,
        sizeof(section),
        iniPath.string().c_str());

    if (bytesRead == 0)
        return enabledMods;

    for (const char* p = section; *p;)
    {
        std::string entry = p;

        auto equals = entry.find('=');

        if (equals != std::string::npos)
        {
            std::string dll = entry.substr(0, equals);
            std::string enabled = entry.substr(equals + 1);

            if (enabled == "1")
                enabledMods.push_back(dll);
        }

        p += entry.length() + 1;
    }

    return enabledMods;
}

fs::path GetModsDirectory(const fs::path& iniPath, const fs::path& gameDir)
{
    char modsDir[MAX_PATH]{};

    GetPrivateProfileStringA(
        "Settings",
        "ModsDirectory",
        ".\\Mods",
        modsDir,
        MAX_PATH,
        iniPath.string().c_str());

    fs::path path(modsDir);

    if (path.is_relative())
        path = gameDir / path;

    return path.lexically_normal();
}

DWORD GetInitializationDelay(const fs::path& iniPath)
{
    return GetPrivateProfileIntA(
        "Settings",
        "InitializationDelay",
        DEFAULT_INIT_DELAY,
        iniPath.string().c_str());
}

DWORD GetPerModDelay(const fs::path& iniPath)
{
    return GetPrivateProfileIntA(
        "Settings",
        "PerModDelay",
        DEFAULT_PER_MOD_DELAY,
        iniPath.string().c_str());
}

bool LoadDll(const fs::path& dllPath)
{
    if (!fs::exists(dllPath))
        return false;

    HMODULE module = LoadLibraryA(dllPath.string().c_str());

    return module != nullptr;
}

DWORD WINAPI LoaderThread(LPVOID)
{
    fs::path gameDir = GetModuleDirectory();
    fs::path iniPath = gameDir / "Mods.ini";

    std::vector<std::string> enabledMods = GetEnabledMods(iniPath);

    if (enabledMods.empty())
        return 0;

    fs::path modsDirectory = GetModsDirectory(iniPath, gameDir);

    DWORD initializationDelay = GetInitializationDelay(iniPath);
    DWORD perModDelay = GetPerModDelay(iniPath);

    auto scriptExtender = std::find(
        enabledMods.begin(),
        enabledMods.end(),
        SCRIPT_EXTENDER);

    if (scriptExtender != enabledMods.end())
    {
        LoadDll(gameDir / SCRIPT_EXTENDER);

        enabledMods.erase(scriptExtender);

        if (initializationDelay > 0)
            Sleep(initializationDelay);
    }

    for (const auto& mod : enabledMods)
    {
        LoadDll(modsDirectory / mod);

        if (perModDelay > 0)
            Sleep(perModDelay);
    }

    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule,
    DWORD ul_reason_for_call,
    LPVOID)
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
    {
        DisableThreadLibraryCalls(hModule);

        if (!IsSupportedExecutable())
            break;

        HANDLE hThread = CreateThread(
            nullptr,
            0,
            LoaderThread,
            nullptr,
            0,
            nullptr);

        if (hThread)
            CloseHandle(hThread);

        break;
    }

    default:
        break;
    }

    return TRUE;
}