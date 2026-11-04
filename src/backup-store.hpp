#pragma once
#include <QString>
#include <QStringList>
#include <QDateTime>
#include <QList>

namespace stacks::backups {
struct Entry {
	QString path;
	QDateTime when;
};

// Write json as a rolling backup for `collection` (skips identical / too recent unless force).
void snapshot(const QString &collection, const QString &json, bool force = false);
// Newest first.
QList<Entry> list(const QString &collection);
// Read file content; empty on failure or if > 8 MiB.
QString read(const QString &path);
// Current scene collection name, empty if none.
QString currentCollection();
} // namespace stacks::backups
