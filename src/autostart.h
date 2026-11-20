#pragma once

#include <QString>

// Start with Windows via HKCU\...\CurrentVersion\Run (no admin rights needed). Task Manager's
// Startup page can disable the entry separately (StartupApproved\Run); that counts as off.
namespace Autostart {

bool isEnabled();

// Adds or removes the Run entry. Enabling also clears a Task Manager "disabled" mark, so the
// switch in our settings always means what it says. Returns false if the registry refused.
bool setEnabled(bool enabled);

// If enabled, points the entry at the current executable (after a move or reinstall).
// Leaves a disabled entry alone.
void refresh();

// What the Run entry launches: the quoted exe path plus --autostart.
QString command();

// Tests redirect both keys (relative to HKEY_CURRENT_USER) to scratch locations.
void setKeysForTesting(const QString &runKey, const QString &approvedKey);

} // namespace Autostart
