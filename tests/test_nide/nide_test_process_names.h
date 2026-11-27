/*--- nide_test_process_names.h - per-tree QSettings names for the nide tests.

The nide tests pin a QSettings application name so a test process never
reads or writes the product's own registry/ini segment. One fixed name is
still shared across every configured build tree: two trees' ctest runs
concurrently then drive the same registry key, and each one's writes are
the other's unexpected state (D-12). The ctest add_test wrapper exports a
per-tree token (NLANG_TEST_TREE_TOKEN, the MD5 of the configuring binary
directory), so the pinned name becomes <legacy-name>-<token>: distinct per
tree, stable for a whole run of one tree, and never the product's name.

A process launched without the wrapper (a direct interactive run) gets no
token; the derivation then falls back to the legacy fixed name rather than
producing a nameless-registry mess, so hand-run debugging keeps working.
The token shape this relies on — exactly 32 lowercase hex characters — and
the key-segment bounds of the derived name (length, character set, no
hierarchy-separating backslash) are pinned by
test_mainwindow.cpp::applicationNameScopedByTreeToken.
---*/
#pragma once

#include <QByteArray>
#include <QString>

namespace nlang {

// <rLegacyName> + "-" + the tree token when the environment carries one in
// the wrapper's MD5 shape; rLegacyName itself otherwise.
inline QString nideTestApplicationName(const QString& rLegacyName) {
    const QByteArray token = qgetenv("NLANG_TEST_TREE_TOKEN");
    if (token.size() != 32)
        return rLegacyName;
    for (const char c : token) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return rLegacyName;
    }
    return rLegacyName + QLatin1Char('-') + QString::fromLatin1(token);
}

} // namespace nlang
