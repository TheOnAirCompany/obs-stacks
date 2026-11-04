#include "backup-store.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>
#include <util/base.h>
#include <util/bmem.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

namespace stacks::backups {

namespace {
constexpr int kKeep = 10;
constexpr qint64 kMinAgeSecs = 5 * 60;
constexpr qint64 kMaxRead = 8LL * 1024 * 1024;

QString sanitize(const QString &name)
{
	QString out;
	for (const QChar &c : name) {
		if (c.isLetterOrNumber() || c == ' ' || c == '-' || c == '_')
			out += c;
		else
			out += '_';
	}
	out = out.left(64);
	if (out.isEmpty())
		out = QStringLiteral("_");
	return out;
}

QString dirFor(const QString &collection)
{
	char *p = obs_module_config_path((QStringLiteral("backups/") + sanitize(collection)).toUtf8().constData());
	if (!p)
		return QString();
	QString dir = QString::fromUtf8(p);
	bfree(p);
	return dir;
}

QDateTime parseWhen(const QFileInfo &fi)
{
	QDateTime dt = QDateTime::fromString(fi.completeBaseName().mid(7), QStringLiteral("yyyyMMdd-HHmmss"));
	return dt.isValid() ? dt : fi.lastModified();
}
} // namespace

QList<Entry> list(const QString &collection)
{
	QList<Entry> out;
	try {
		const QString dir = dirFor(collection);
		if (dir.isEmpty())
			return out;
		const QFileInfoList files = QDir(dir).entryInfoList({QStringLiteral("stacks-*.json")}, QDir::Files,
								    QDir::Name | QDir::Reversed);
		for (const QFileInfo &fi : files)
			out.append({fi.absoluteFilePath(), parseWhen(fi)});
	} catch (...) {
	}
	return out;
}

QString read(const QString &path)
{
	try {
		QFile f(path);
		if (f.size() > kMaxRead || !f.open(QIODevice::ReadOnly))
			return QString();
		const QByteArray data = f.read(kMaxRead + 1);
		if (data.size() > kMaxRead)
			return QString();
		return QString::fromUtf8(data);
	} catch (...) {
		return QString();
	}
}

QString currentCollection()
{
	char *c = obs_frontend_get_current_scene_collection();
	if (!c)
		return QString();
	QString s = QString::fromUtf8(c);
	bfree(c);
	return s;
}

void snapshot(const QString &collection, const QString &json, bool force)
{
	try {
		const QString dir = dirFor(collection);
		if (dir.isEmpty()) {
			obs_log(LOG_WARNING, "backup: no config path");
			return;
		}
		if (!QDir().mkpath(dir)) {
			obs_log(LOG_WARNING, "backup: cannot create %s", dir.toUtf8().constData());
			return;
		}
		const QList<Entry> existing = list(collection);
		if (!existing.isEmpty()) {
			const Entry &newest = existing.first();
			if (read(newest.path) == json)
				return;
			const qint64 age = QFileInfo(newest.path).lastModified().secsTo(QDateTime::currentDateTime());
			if (!force && age >= 0 && age < kMinAgeSecs)
				return;
		}
		const QString name =
			QStringLiteral("stacks-%1.json")
				.arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss")));
		const QString path = QDir(dir).filePath(name);
		QSaveFile f(path);
		if (!f.open(QIODevice::WriteOnly)) {
			obs_log(LOG_WARNING, "backup: cannot open %s", path.toUtf8().constData());
			return;
		}
		f.write(json.toUtf8());
		if (!f.commit()) {
			obs_log(LOG_WARNING, "backup: cannot write %s", path.toUtf8().constData());
			return;
		}
		const QList<Entry> all = list(collection);
		for (int i = kKeep; i < all.size(); ++i)
			QFile::remove(all[i].path);
	} catch (...) {
		obs_log(LOG_WARNING, "backup: unexpected failure");
	}
}

} // namespace stacks::backups
