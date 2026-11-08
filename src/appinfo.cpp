#include "appinfo.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>

namespace AppInfo {

QString service(const QString &what) {
    return QString("%1-%2.service").arg(Name, what);
}

bool migrateLegacySettings(const QString &configRoot) {
    const QString root = configRoot.isEmpty()
        ? QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) : configRoot;
    const QString target = QString("%1/%2/%2.conf").arg(root, Name);
    const QString legacy = QString("%1/%2/%2.conf").arg(root, LegacyName);
    if (QFile::exists(target) || !QFile::exists(legacy))
        return false;
    QDir().mkpath(QFileInfo(target).absolutePath());
    return QFile::copy(legacy, target);
}

} // namespace AppInfo
