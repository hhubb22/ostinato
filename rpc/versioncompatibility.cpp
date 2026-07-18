#include "versioncompatibility.h"

#include <QStringList>

VersionCompatibilityResult checkVersionCompatibility(
    const QString &clientVersion, const QString &serverVersion)
{
    const QStringList client = clientVersion.split('.');
    const QStringList server = serverVersion.split('.');

    if (client.size() < 2)
        return VersionInvalid;

    Q_ASSERT(server.size() >= 2);
    return client[0] == server[0] && client[1] == server[1]
        ? VersionCompatible : VersionIncompatible;
}
