#pragma once

#include <QString>

// The app's name in every place it shows up: binary, settings, services,
// locks and hooks. It was called azoth while it drove only the ROG Azoth.
namespace AppInfo {

constexpr const char *Name = "omaglow";
constexpr const char *DisplayName = "Omaglow";
constexpr const char *LegacyName = "azoth";

// "<name>-<what>.service", e.g. omaglow-lights.service.
QString service(const QString &what);

// Copies ~/.config/azoth/azoth.conf to ~/.config/omaglow/omaglow.conf when
// the new file isn't there yet; the old one stays as it was. True if copied.
// configRoot defaults to the user's config directory.
bool migrateLegacySettings(const QString &configRoot = {});

} // namespace AppInfo
