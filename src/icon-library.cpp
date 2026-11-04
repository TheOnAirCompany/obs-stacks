#include "icon-library.hpp"

#include <obs-module.h>
#include <util/bmem.h>

#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QSvgRenderer>
#include <QVBoxLayout>

namespace stacks::icons {

namespace {

QString iconsDir()
{
	char *p = obs_module_file("icons/lucide");
	QString s = p ? QString::fromUtf8(p) : QString();
	bfree(p);
	return s;
}

QStringList &allNames()
{
	static QStringList names;
	static bool loaded = false;
	if (!loaded) {
		loaded = true;
		const QString dir = iconsDir();
		if (!dir.isEmpty()) {
			const QStringList files = QDir(dir).entryList({"*.svg"}, QDir::Files, QDir::Name);
			for (const QString &f : files)
				names << f.left(f.size() - 4);
		}
	}
	return names;
}

const QHash<QString, QString> &tagsMap()
{
	static QHash<QString, QString> tags;
	static bool loaded = false;
	if (!loaded) {
		loaded = true;
		const QString dir = iconsDir();
		QFile f(dir + "/tags.json");
		if (!dir.isEmpty() && f.open(QIODevice::ReadOnly)) {
			const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
			for (auto it = o.begin(); it != o.end(); ++it) {
				QStringList l;
				for (const QJsonValue v : it.value().toArray())
					l << v.toString();
				tags.insert(it.key(), l.join(' ').toLower());
			}
		}
	}
	return tags;
}

bool validName(const QString &name)
{
	return !name.isEmpty() && !name.contains('/') && !name.contains('\\') && !name.contains("..");
}

} // namespace

QIcon icon(const QString &name, const QColor &color, int size)
{
	if (!validName(name))
		return QIcon();

	static QHash<QString, QIcon> cache;
	const QString key = name + '|' + color.name(QColor::HexArgb) + '|' + QString::number(size);
	auto it = cache.constFind(key);
	if (it != cache.constEnd())
		return it.value();

	const QString dir = iconsDir();
	if (dir.isEmpty())
		return QIcon();
	QFile f(dir + "/" + name + ".svg");
	if (!f.open(QIODevice::ReadOnly))
		return QIcon();
	QByteArray svg = f.readAll();
	svg.replace("currentColor", color.name(QColor::HexRgb).toUtf8());

	QSvgRenderer renderer(svg);
	if (!renderer.isValid())
		return QIcon();

	const qreal dpr = 2.0;
	QPixmap pm(QSize(size, size) * dpr);
	pm.setDevicePixelRatio(dpr);
	pm.fill(Qt::transparent);
	QPainter p(&pm);
	p.setRenderHint(QPainter::Antialiasing);
	renderer.render(&p, QRectF(0, 0, size, size));
	p.end();

	QIcon ic(pm);
	cache.insert(key, ic);
	return ic;
}

QString pick(QWidget *parent, const QString &current, const QColor &color)
{
	QDialog dlg(parent);
	dlg.setWindowTitle(QString::fromUtf8(obs_module_text("Icon.Pick")));
	dlg.resize(520, 480);

	auto *layout = new QVBoxLayout(&dlg);
	auto *search = new QLineEdit(&dlg);
	search->setPlaceholderText(QString::fromUtf8(obs_module_text("Icon.Search")));
	search->setClearButtonEnabled(true);
	layout->addWidget(search);

	auto *list = new QListWidget(&dlg);
	list->setViewMode(QListView::IconMode);
	list->setResizeMode(QListView::Adjust);
	list->setMovement(QListView::Static);
	list->setUniformItemSizes(true);
	list->setIconSize(QSize(20, 20));
	list->setGridSize(QSize(36, 36));
	list->setSpacing(2);
	list->setSelectionMode(QAbstractItemView::SingleSelection);
	layout->addWidget(list, 1);

	QString result; // null = cancel
	QString chosen = current;
	const QColor c = color.isValid() ? color : QColor(Qt::white);

	const QStringList &names = allNames();
	const auto &tags = tagsMap();
	for (const QString &n : names) {
		auto *item = new QListWidgetItem(icon(n, c, 20), QString(), list);
		item->setToolTip(n);
		item->setData(Qt::UserRole, n);
		if (n == current) {
			list->setCurrentItem(item);
			list->scrollToItem(item);
		}
	}

	QObject::connect(search, &QLineEdit::textChanged, &dlg, [&](const QString &text) {
		const QString q = text.trimmed().toLower();
		list->setUpdatesEnabled(false);
		for (int i = 0; i < list->count(); ++i) {
			QListWidgetItem *it = list->item(i);
			const QString n = it->data(Qt::UserRole).toString();
			const bool match = q.isEmpty() || n.contains(q) || tags.value(n).contains(q);
			it->setHidden(!match);
		}
		list->setUpdatesEnabled(true);
	});

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
	auto *noneBtn =
		buttons->addButton(QString::fromUtf8(obs_module_text("Icon.None")), QDialogButtonBox::ActionRole);
	layout->addWidget(buttons);

	QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, [&]() {
		if (auto *it = list->currentItem())
			chosen = it->data(Qt::UserRole).toString();
		result = chosen;
		dlg.accept();
	});
	QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
	QObject::connect(noneBtn, &QPushButton::clicked, &dlg, [&]() {
		result = QString("");
		dlg.accept();
	});
	QObject::connect(list, &QListWidget::itemDoubleClicked, &dlg, [&](QListWidgetItem *it) {
		result = it->data(Qt::UserRole).toString();
		dlg.accept();
	});

	search->setFocus();
	if (dlg.exec() != QDialog::Accepted)
		return QString();
	return result;
}

} // namespace stacks::icons
