#include "autostart.h"

#include <QCoreApplication>
#include <QDir>

#include <optional>
#include <string>

#include <windows.h>

namespace Autostart {

namespace {

std::wstring g_runKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
std::wstring g_approvedKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";
constexpr wchar_t kValueName[] = L"Jieying";

std::optional<std::wstring> readRunValue()
{
    DWORD size = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, g_runKey.c_str(), kValueName, RRF_RT_REG_SZ, nullptr, nullptr, &size)
        != ERROR_SUCCESS)
        return std::nullopt;
    std::wstring value(size / sizeof(wchar_t), L'\0');
    if (RegGetValueW(HKEY_CURRENT_USER, g_runKey.c_str(), kValueName, RRF_RT_REG_SZ, nullptr, value.data(), &size)
        != ERROR_SUCCESS)
        return std::nullopt;
    value.resize(wcslen(value.c_str()));
    return value;
}

bool writeRunValue()
{
    const std::wstring value = command().toStdWString();
    return RegSetKeyValueW(HKEY_CURRENT_USER, g_runKey.c_str(), kValueName, REG_SZ, value.c_str(),
                           DWORD((value.size() + 1) * sizeof(wchar_t)))
           == ERROR_SUCCESS;
}

// Task Manager stores 12 bytes; the first is 0x02 when enabled and 0x03 when disabled.
bool disabledInTaskManager()
{
    BYTE data[16] = {};
    DWORD size = sizeof(data);
    if (RegGetValueW(HKEY_CURRENT_USER, g_approvedKey.c_str(), kValueName, RRF_RT_REG_BINARY, nullptr, data, &size)
        != ERROR_SUCCESS)
        return false;
    return size > 0 && (data[0] & 1);
}

} // namespace

QString command()
{
    return QStringLiteral("\"%1\" --autostart").arg(QDir::toNativeSeparators(QCoreApplication::applicationFilePath()));
}

bool isEnabled()
{
    return readRunValue().has_value() && !disabledInTaskManager();
}

bool setEnabled(bool enabled)
{
    if (enabled) {
        if (!writeRunValue())
            return false;
        RegDeleteKeyValueW(HKEY_CURRENT_USER, g_approvedKey.c_str(), kValueName);
        return true;
    }
    const LSTATUS status = RegDeleteKeyValueW(HKEY_CURRENT_USER, g_runKey.c_str(), kValueName);
    RegDeleteKeyValueW(HKEY_CURRENT_USER, g_approvedKey.c_str(), kValueName);
    return status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND;
}

void refresh()
{
    const std::optional<std::wstring> current = readRunValue();
    if (current && QString::fromStdWString(*current) != command())
        writeRunValue();
}

void setKeysForTesting(const QString &runKey, const QString &approvedKey)
{
    g_runKey = runKey.toStdWString();
    g_approvedKey = approvedKey.toStdWString();
}

} // namespace Autostart
