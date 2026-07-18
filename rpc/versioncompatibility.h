#ifndef VERSIONCOMPATIBILITY_H
#define VERSIONCOMPATIBILITY_H

#include <QString>

enum VersionCompatibilityResult {
    VersionInvalid,
    VersionCompatible,
    VersionIncompatible
};

// Ostinato's wire compatibility is defined by the first two textual fields.
VersionCompatibilityResult checkVersionCompatibility(
    const QString &clientVersion, const QString &serverVersion);

#endif
