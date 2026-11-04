/*
Stacks for OBS
Copyright (C) 2026 The OnAir Company

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include "stacks-dock.hpp"

#include <QAction>
#include <QColorDialog>
#include <QDockWidget>
#include <QDropEvent>
#include <QHBoxLayout>
#include <QHash>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonDocument>
#include <QMainWindow>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QSet>
#include <QVBoxLayout>

#include <obs-module.h>
#include <util/config-file.h>

#include <algorithm>
#include <functional>

#include <plugin-support.h>

namespace stacks {

namespace {

constexpr const char *kModuleKey = "obs-stacks";
constexpr const char *kConfigSection = "obs-stacks";
constexpr int kSchemaVersion = 1;

QString T(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

struct PaletteEntry {
	const char *key;
	const char *hex;
};

/* Proposed palette: values to be confirmed on the design review page. */
const PaletteEntry kPalette[] = {
	{"Color.Red", "#E5484D"},  {"Color.Orange", "#F76B15"}, {"Color.Yellow", "#FFC53D"}, {"Color.Green", "#30A46C"},
	{"Color.Teal", "#12A594"}, {"Color.Blue", "#3E63DD"},   {"Color.Purple", "#8E4EC6"}, {"Color.Pink", "#D6409F"},
};

Kind kindOf(const QTreeWidgetItem *item)
{
	return item ? static_cast<Kind>(item->data(0, KindRole).toInt()) : Kind::Scene;
}

QString fold(const QString &s)
{
	/* Case- and accent-insensitive comparison key. */
	const QString d = s.normalized(QString::NormalizationForm_D);
	QString out;
	out.reserve(d.size());
	for (const QChar c : d) {
		if (c.category() != QChar::Mark_NonSpacing)
			out.append(c.toLower());
	}
	return out;
}

config_t *userConfig()
{
	return obs_frontend_get_user_config();
}

QDockWidget *nativeScenesDock()
{
	auto *main = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	return main ? main->findChild<QDockWidget *>(QStringLiteral("scenesDock")) : nullptr;
}

} // namespace

/* ------------------------------------------------------------------------ */
/* Tree                                                                     */
/* ------------------------------------------------------------------------ */

StacksTree::StacksTree(QWidget *parent) : QTreeWidget(parent)
{
	setHeaderHidden(true);
	setColumnCount(1);
	setSelectionMode(QAbstractItemView::ExtendedSelection);
	setDragEnabled(true);
	setAcceptDrops(true);
	setDropIndicatorShown(true);
	setDragDropMode(QAbstractItemView::InternalMove);
	setDefaultDropAction(Qt::MoveAction);
	setEditTriggers(QAbstractItemView::NoEditTriggers);
	setContextMenuPolicy(Qt::CustomContextMenu);
	setUniformRowHeights(false);
	setIndentation(14);
	setAnimated(false);
	setExpandsOnDoubleClick(false);
}

bool StacksTree::dropAllowed(QDropEvent *event) const
{
	const QList<QTreeWidgetItem *> dragged = selectedItems();
	if (dragged.isEmpty())
		return false;

	QTreeWidgetItem *target = itemAt(event->position().toPoint());
	const DropIndicatorPosition pos = dropIndicatorPosition();

	/* Resolve the parent the items would land in. */
	QTreeWidgetItem *newParent = nullptr;
	if (target) {
		if (pos == QAbstractItemView::OnItem)
			newParent = target;
		else
			newParent = target->parent();
	}

	if (newParent && kindOf(newParent) == Kind::Unsorted)
		return false; /* Nothing is filed into Unsorted by hand. */
	if (newParent && kindOf(newParent) != Kind::Stack)
		return false;

	for (QTreeWidgetItem *item : dragged) {
		const Kind k = kindOf(item);
		if (k == Kind::Unsorted)
			return false;
		if (k == Kind::Stack && newParent)
			return false; /* Stacks are one level deep in this version. */
	}
	return true;
}

void StacksTree::dragMoveEvent(QDragMoveEvent *event)
{
	QTreeWidget::dragMoveEvent(event);
	if (event->isAccepted() && !dropAllowed(event))
		event->ignore();
}

void StacksTree::dropEvent(QDropEvent *event)
{
	if (!dropAllowed(event)) {
		event->ignore();
		return;
	}
	QTreeWidget::dropEvent(event);
	emit structureChanged();
}

/* ------------------------------------------------------------------------ */
/* Delegate                                                                 */
/* ------------------------------------------------------------------------ */

QSize StacksDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const
{
	QSize s = QStyledItemDelegate::sizeHint(option, index);
	const Kind k = static_cast<Kind>(index.data(KindRole).toInt());
	if (k == Kind::Separator)
		s.setHeight(index.data(Qt::DisplayRole).toString().isEmpty() ? 10 : 20);
	else
		s.setHeight(std::max(s.height(), 26));
	return s;
}

void StacksDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
	const Kind k = static_cast<Kind>(index.data(KindRole).toInt());
	const QColor color(index.data(ColorRole).toString());
	const QPalette &pal = option.palette;

	if (k == Kind::Separator) {
		painter->save();
		if (option.state & QStyle::State_Selected)
			painter->fillRect(option.rect, pal.highlight().color().darker(150));
		const QString label = index.data(Qt::DisplayRole).toString();
		QColor line = color.isValid() ? color : pal.text().color();
		line.setAlphaF(color.isValid() ? 0.9 : 0.35);
		const int y = option.rect.center().y();
		const QRect r = option.rect.adjusted(6, 0, -6, 0);
		painter->setPen(QPen(line, 1));
		if (label.isEmpty()) {
			painter->drawLine(r.left(), y, r.right(), y);
		} else {
			QFont f = option.font;
			f.setPointSizeF(f.pointSizeF() * 0.85);
			f.setBold(true);
			f.setCapitalization(QFont::AllUppercase);
			painter->setFont(f);
			const QFontMetrics fm(f);
			const int tw = fm.horizontalAdvance(label);
			QColor tc = color.isValid() ? color : pal.text().color();
			tc.setAlphaF(0.75);
			painter->setPen(tc);
			painter->drawText(QRect(r.left(), r.top(), tw, r.height()), Qt::AlignVCenter, label);
			painter->setPen(QPen(line, 1));
			if (r.left() + tw + 8 < r.right())
				painter->drawLine(r.left() + tw + 8, y, r.right(), y);
		}
		painter->restore();
		return;
	}

	QStyleOptionViewItem opt(option);
	initStyleOption(&opt, index);

	const bool program = index.data(ProgramRole).toBool();
	const bool preview = index.data(PreviewRole).toBool();
	const bool favorite = index.data(FavoriteRole).toBool();
	const bool isGroup = (k == Kind::Stack || k == Kind::Unsorted);

	if (isGroup) {
		opt.font.setBold(true);
		if (k == Kind::Unsorted)
			opt.font.setItalic(true);
	}
	if (program)
		opt.font.setBold(true);

	/* Tinted background for colored stacks. */
	if (k == Kind::Stack && color.isValid() && !(opt.state & QStyle::State_Selected)) {
		QColor tint = color;
		tint.setAlphaF(0.16);
		painter->fillRect(option.rect, tint);
	}

	/* Leave room for the color strip and the badges. */
	const int badgeW = (program || preview) ? 42 : 0;
	const int favW = favorite ? 16 : 0;
	opt.rect.adjust(8, 0, -(badgeW + favW + 4), 0);
	QStyledItemDelegate::paint(painter, opt, index);

	painter->save();
	painter->setRenderHint(QPainter::Antialiasing, true);

	if (color.isValid()) {
		const QRect strip(option.rect.left() + 2, option.rect.top() + 4, 3, option.rect.height() - 8);
		QPainterPath path;
		path.addRoundedRect(strip, 1.5, 1.5);
		painter->fillPath(path, color);
	}

	int x = option.rect.right() - 4;
	if (program || preview) {
		const QString text = program ? QStringLiteral("LIVE") : QStringLiteral("PVW");
		const QColor bg = program ? QColor("#E5484D") : QColor("#30A46C");
		QFont f = option.font;
		f.setPointSizeF(f.pointSizeF() * 0.75);
		f.setBold(true);
		painter->setFont(f);
		const QRect badge(x - 38, option.rect.center().y() - 8, 38, 16);
		QPainterPath path;
		path.addRoundedRect(badge, 8, 8);
		painter->fillPath(path, bg);
		painter->setPen(Qt::white);
		painter->drawText(badge, Qt::AlignCenter, text);
		x -= badgeW;
	}
	if (favorite) {
		painter->setPen(QColor("#FFC53D"));
		painter->setFont(option.font);
		painter->drawText(QRect(x - 14, option.rect.top(), 14, option.rect.height()), Qt::AlignCenter,
				  QString::fromUtf8("\xE2\x98\x85"));
	}
	painter->restore();
}

/* ------------------------------------------------------------------------ */
/* Dock                                                                     */
/* ------------------------------------------------------------------------ */

StacksDock::StacksDock(QWidget *parent) : QWidget(parent)
{
	setObjectName(QStringLiteral("StacksDock"));
	setMinimumWidth(180);

	search = new QLineEdit(this);
	search->setPlaceholderText(T("Search.Placeholder"));
	search->setClearButtonEnabled(true);

	favoritesButton = new QToolButton(this);
	favoritesButton->setText(QString::fromUtf8("\xE2\x98\x85"));
	favoritesButton->setCheckable(true);
	favoritesButton->setToolTip(T("Favorites.Filter"));

	menuButton = new QToolButton(this);
	menuButton->setText(QString::fromUtf8("\xE2\x8B\xAF"));
	menuButton->setToolTip(T("Menu.Settings"));
	menuButton->setPopupMode(QToolButton::InstantPopup);
	menuButton->setStyleSheet(QStringLiteral("QToolButton::menu-indicator { image: none; width: 0; }"));

	auto *settings = new QMenu(menuButton);
	QAction *newStackAct = settings->addAction(T("Action.NewStack"));
	QAction *newSepAct = settings->addAction(T("Action.NewSeparator"));
	QAction *addSceneAct = settings->addAction(T("Action.AddScene"));
	settings->addSeparator();
	QAction *expandAct = settings->addAction(T("Action.ExpandAll"));
	QAction *collapseAct = settings->addAction(T("Action.CollapseAll"));
	settings->addSeparator();
	QAction *safeAct = settings->addAction(T("Setting.SafeClick"));
	safeAct->setCheckable(true);
	QAction *hideNativeAct = settings->addAction(T("Setting.HideNative"));
	hideNativeAct->setCheckable(true);
	hideNativeAction = hideNativeAct;
	menuButton->setMenu(settings);

	config_t *cfg = userConfig();
	if (cfg) {
		config_set_default_bool(cfg, kConfigSection, "SafeClick", false);
		config_set_default_bool(cfg, kConfigSection, "HideNativeScenes", false);
		safeClick = config_get_bool(cfg, kConfigSection, "SafeClick");
		hideNative = config_get_bool(cfg, kConfigSection, "HideNativeScenes");
	}
	safeAct->setChecked(safeClick);
	hideNativeAct->setChecked(hideNative);

	connect(newStackAct, &QAction::triggered, this, [this] { newStack(); });
	connect(newSepAct, &QAction::triggered, this, [this] { newSeparator(); });
	connect(addSceneAct, &QAction::triggered, this, [this] { addScene(); });
	connect(expandAct, &QAction::triggered, this, [this] {
		tree->expandAll();
		markDirty();
	});
	connect(collapseAct, &QAction::triggered, this, [this] {
		tree->collapseAll();
		markDirty();
	});
	connect(safeAct, &QAction::toggled, this, [this](bool on) {
		safeClick = on;
		if (config_t *c = userConfig())
			config_set_bool(c, kConfigSection, "SafeClick", on);
	});
	connect(hideNativeAct, &QAction::toggled, this, [this](bool on) {
		hideNative = on;
		if (config_t *c = userConfig())
			config_set_bool(c, kConfigSection, "HideNativeScenes", on);
		setNativeDockHidden(on);
	});

	auto *header = new QHBoxLayout();
	header->setContentsMargins(4, 4, 4, 2);
	header->setSpacing(4);
	header->addWidget(search, 1);
	header->addWidget(favoritesButton);
	header->addWidget(menuButton);

	tree = new StacksTree(this);
	tree->setItemDelegate(new StacksDelegate(tree));

	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(0);
	layout->addLayout(header);
	layout->addWidget(tree, 1);

	connect(tree, &QTreeWidget::itemClicked, this, &StacksDock::onItemClicked);
	connect(tree, &QTreeWidget::itemDoubleClicked, this, &StacksDock::onItemDoubleClicked);
	connect(tree, &QWidget::customContextMenuRequested, this, &StacksDock::onContextMenu);
	connect(tree, &StacksTree::structureChanged, this, &StacksDock::onStructureChanged);
	connect(tree, &QTreeWidget::itemExpanded, this, [this](QTreeWidgetItem *) { markDirty(); });
	connect(tree, &QTreeWidget::itemCollapsed, this, [this](QTreeWidgetItem *) { markDirty(); });
	connect(search, &QLineEdit::textChanged, this, &StacksDock::applyFilter);
	connect(favoritesButton, &QToolButton::toggled, this, &StacksDock::applyFilter);

	saveTimer.setSingleShot(true);
	saveTimer.setInterval(1500);
	connect(&saveTimer, &QTimer::timeout, this, [] { obs_frontend_save(); });
}

StacksDock::~StacksDock() = default;

/* ---------------------------- items ------------------------------------- */

QTreeWidgetItem *StacksDock::makeScene(const QString &uuid, const QString &name)
{
	auto *item = new QTreeWidgetItem();
	item->setText(0, name);
	item->setData(0, KindRole, static_cast<int>(Kind::Scene));
	item->setData(0, UuidRole, uuid);
	item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled);
	return item;
}

QTreeWidgetItem *StacksDock::makeStack(const QString &name)
{
	auto *item = new QTreeWidgetItem();
	item->setText(0, name);
	item->setData(0, KindRole, static_cast<int>(Kind::Stack));
	item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled);
	item->setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator);
	return item;
}

QTreeWidgetItem *StacksDock::makeSeparator(const QString &label)
{
	auto *item = new QTreeWidgetItem();
	item->setText(0, label);
	item->setData(0, KindRole, static_cast<int>(Kind::Separator));
	item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled);
	return item;
}

QTreeWidgetItem *StacksDock::unsortedItem()
{
	for (int i = 0; i < tree->topLevelItemCount(); i++) {
		QTreeWidgetItem *it = tree->topLevelItem(i);
		if (kindOf(it) == Kind::Unsorted)
			return it;
	}
	auto *item = new QTreeWidgetItem();
	item->setText(0, T("Unsorted"));
	item->setData(0, KindRole, static_cast<int>(Kind::Unsorted));
	item->setFlags(Qt::ItemIsEnabled);
	tree->addTopLevelItem(item);
	item->setExpanded(true);
	return item;
}

void StacksDock::moveUnsortedLast()
{
	QTreeWidgetItem *u = unsortedItem();
	const int idx = tree->indexOfTopLevelItem(u);
	if (idx != tree->topLevelItemCount() - 1) {
		const bool expanded = u->isExpanded();
		tree->takeTopLevelItem(idx);
		tree->addTopLevelItem(u);
		u->setExpanded(expanded);
	}
}

void StacksDock::collectScenes(QTreeWidgetItem *parent, QList<QTreeWidgetItem *> &out) const
{
	const int n = parent ? parent->childCount() : tree->topLevelItemCount();
	for (int i = 0; i < n; i++) {
		QTreeWidgetItem *it = parent ? parent->child(i) : tree->topLevelItem(i);
		if (kindOf(it) == Kind::Scene)
			out.append(it);
		else if (kindOf(it) == Kind::Stack || kindOf(it) == Kind::Unsorted)
			collectScenes(it, out);
	}
}

QTreeWidgetItem *StacksDock::insertionParent(int *row) const
{
	QTreeWidgetItem *cur = tree->currentItem();
	*row = -1;
	if (!cur)
		return nullptr;
	if (kindOf(cur) == Kind::Stack) {
		*row = cur->childCount();
		return cur;
	}
	QTreeWidgetItem *p = cur->parent();
	if (p && kindOf(p) == Kind::Unsorted)
		return nullptr;
	*row = (p ? p->indexOfChild(cur) : tree->indexOfTopLevelItem(cur)) + 1;
	return p;
}

obs_source_t *StacksDock::sceneByUuid(const QString &uuid)
{
	if (uuid.isEmpty())
		return nullptr;
	obs_source_t *src = obs_get_source_by_uuid(uuid.toUtf8().constData());
	if (src && !obs_source_is_scene(src)) {
		obs_source_release(src);
		return nullptr;
	}
	return src;
}

/* ---------------------------- persistence ------------------------------- */

QJsonObject StacksDock::serializeItem(QTreeWidgetItem *item) const
{
	QJsonObject o;
	const QString color = item->data(0, ColorRole).toString();
	switch (kindOf(item)) {
	case Kind::Scene:
		o["type"] = "scene";
		o["uuid"] = item->data(0, UuidRole).toString();
		o["name"] = item->text(0);
		if (item->data(0, FavoriteRole).toBool())
			o["favorite"] = true;
		break;
	case Kind::Stack:
		o["type"] = "stack";
		o["name"] = item->text(0);
		o["collapsed"] = !item->isExpanded();
		o["children"] = serializeChildren(item);
		break;
	case Kind::Separator:
		o["type"] = "separator";
		o["label"] = item->text(0);
		break;
	case Kind::Unsorted:
		break;
	}
	if (!color.isEmpty())
		o["color"] = color;
	return o;
}

QJsonArray StacksDock::serializeChildren(QTreeWidgetItem *parent) const
{
	QJsonArray arr;
	const int n = parent ? parent->childCount() : tree->topLevelItemCount();
	for (int i = 0; i < n; i++) {
		QTreeWidgetItem *it = parent ? parent->child(i) : tree->topLevelItem(i);
		if (kindOf(it) == Kind::Unsorted)
			continue; /* Unsorted is derived, never stored. */
		arr.append(serializeItem(it));
	}
	return arr;
}

void StacksDock::buildFromJson(const QJsonArray &nodes, QTreeWidgetItem *parent)
{
	for (const QJsonValue v : nodes) {
		const QJsonObject o = v.toObject();
		const QString type = o["type"].toString();
		QTreeWidgetItem *item = nullptr;
		if (type == "scene") {
			item = makeScene(o["uuid"].toString(), o["name"].toString());
			item->setData(0, FavoriteRole, o["favorite"].toBool());
		} else if (type == "stack" && !parent) {
			item = makeStack(o["name"].toString());
		} else if (type == "separator") {
			item = makeSeparator(o["label"].toString());
		} else {
			continue; /* Unknown node: skipped in view, kept in lastJson. */
		}
		if (o.contains("color"))
			item->setData(0, ColorRole, o["color"].toString());
		if (parent)
			parent->addChild(item);
		else
			tree->addTopLevelItem(item);
		if (type == "stack") {
			buildFromJson(o["children"].toArray(), item);
			item->setExpanded(!o["collapsed"].toBool());
		}
	}
}

void StacksDock::saveTo(obs_data_t *moduleData)
{
	if (loaded && !readOnly) {
		QJsonObject root;
		root["schema_version"] = kSchemaVersion;
		root["tree"] = serializeChildren(nullptr);
		lastJson = QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
	}
	if (lastJson.isEmpty())
		return; /* Nothing known yet: leave OBS's copy untouched. */

	obs_data_t *obj = obs_data_create_from_json(lastJson.toUtf8().constData());
	if (obj) {
		obs_data_set_obj(moduleData, kModuleKey, obj);
		obs_data_release(obj);
	}
}

void StacksDock::loadFrom(obs_data_t *moduleData)
{
	tree->clear();
	lastJson.clear();
	loaded = false;
	readOnly = false;

	obs_data_t *obj = obs_data_get_obj(moduleData, kModuleKey);
	if (!obj)
		return;
	const char *json = obs_data_get_json(obj);
	lastJson = QString::fromUtf8(json ? json : "");
	obs_data_release(obj);

	QJsonParseError err{};
	const QJsonDocument doc = QJsonDocument::fromJson(lastJson.toUtf8(), &err);
	const QJsonObject root = doc.object();
	if (err.error != QJsonParseError::NoError || !doc.isObject() ||
	    root["schema_version"].toInt() > kSchemaVersion) {
		/* Show a flat list and re-save the stored object untouched. */
		obs_log(LOG_WARNING, "organization data is newer or unreadable; kept read-only");
		readOnly = true;
		return;
	}
	buildFromJson(root["tree"].toArray(), nullptr);
}

void StacksDock::markDirty()
{
	if (loaded && !readOnly)
		saveTimer.start();
}

/* ---------------------------- sync -------------------------------------- */

void StacksDock::reconcile()
{
	if (!loaded)
		return;
	if (busy > 0) {
		/* A menu or dialog holds item pointers: run once it closes. */
		pendingReconcile = true;
		return;
	}
	pendingReconcile = false;

	struct SceneInfo {
		QString uuid;
		QString name;
	};
	QList<SceneInfo> scenes;
	QHash<QString, int> byUuid;
	QHash<QString, int> byName;

	obs_frontend_source_list list = {};
	obs_frontend_get_scenes(&list);
	for (size_t i = 0; i < list.sources.num; i++) {
		obs_source_t *src = list.sources.array[i];
		SceneInfo info{QString::fromUtf8(obs_source_get_uuid(src)),
			       QString::fromUtf8(obs_source_get_name(src))};
		byUuid.insert(info.uuid, static_cast<int>(scenes.size()));
		byName.insert(info.name, static_cast<int>(scenes.size()));
		scenes.append(info);
	}
	obs_frontend_source_list_free(&list);

	QList<QTreeWidgetItem *> items;
	collectScenes(nullptr, items);

	QSet<QString> claimed;
	QSet<QTreeWidgetItem *> boundItems; /* a second item on the same uuid is a duplicate */
	QList<QTreeWidgetItem *> unbound;
	for (QTreeWidgetItem *it : items) {
		const QString uuid = it->data(0, UuidRole).toString();
		if (byUuid.contains(uuid) && !claimed.contains(uuid)) {
			claimed.insert(uuid);
			boundItems.insert(it);
		} else {
			unbound.append(it);
		}
	}
	/* Name fallback (duplicated collections regenerate every UUID). */
	for (QTreeWidgetItem *it : unbound) {
		auto found = byName.constFind(it->text(0));
		if (found != byName.constEnd() && !claimed.contains(scenes[*found].uuid)) {
			it->setData(0, UuidRole, scenes[*found].uuid);
			claimed.insert(scenes[*found].uuid);
			boundItems.insert(it);
		}
	}

	QTreeWidgetItem *unsorted = unsortedItem();
	for (QTreeWidgetItem *it : items) {
		const QString uuid = it->data(0, UuidRole).toString();
		const bool present = boundItems.contains(it);
		if (present) {
			it->setText(0, scenes[byUuid[uuid]].name);
			it->setData(0, MissingRole, false);
		} else if (it->parent() == unsorted) {
			delete it; /* Unsorted keeps no history. */
		} else {
			/* Tombstone: kept hidden so undo restores its place. */
			it->setData(0, MissingRole, true);
		}
	}

	/* Scenes Stacks has never seen go to Unsorted, in native order. */
	QSet<QString> bound;
	QList<QTreeWidgetItem *> after;
	collectScenes(nullptr, after);
	for (QTreeWidgetItem *it : after)
		bound.insert(it->data(0, UuidRole).toString());
	for (const SceneInfo &s : scenes) {
		if (!bound.contains(s.uuid))
			unsorted->addChild(makeScene(s.uuid, s.name));
	}

	moveUnsortedLast();
	updateIndicators();
	applyFilter();
}

void StacksDock::updateIndicators()
{
	obs_source_t *program = obs_frontend_get_current_scene();
	obs_source_t *preview = obs_frontend_preview_program_mode_active() ? obs_frontend_get_current_preview_scene()
									   : nullptr;
	const QString programUuid = program ? QString::fromUtf8(obs_source_get_uuid(program)) : QString();
	const QString previewUuid = preview ? QString::fromUtf8(obs_source_get_uuid(preview)) : QString();
	obs_source_release(program);
	obs_source_release(preview);

	QList<QTreeWidgetItem *> items;
	collectScenes(nullptr, items);
	for (QTreeWidgetItem *it : items) {
		const QString uuid = it->data(0, UuidRole).toString();
		it->setData(0, ProgramRole, !uuid.isEmpty() && uuid == programUuid);
		it->setData(0, PreviewRole, !uuid.isEmpty() && uuid == previewUuid && uuid != programUuid);
	}
}

void StacksDock::applyFilter()
{
	const QString query = fold(search->text().trimmed());
	const bool favOnly = favoritesButton->isChecked();
	const bool filtering = !query.isEmpty() || favOnly;

	std::function<bool(QTreeWidgetItem *)> visit = [&](QTreeWidgetItem *it) -> bool {
		const Kind k = kindOf(it);
		bool visible = false;
		if (k == Kind::Scene) {
			const bool missing = it->data(0, MissingRole).toBool();
			visible = !missing && (query.isEmpty() || fold(it->text(0)).contains(query)) &&
				  (!favOnly || it->data(0, FavoriteRole).toBool());
		} else if (k == Kind::Separator) {
			visible = !filtering;
		} else {
			bool anyChild = false;
			for (int i = 0; i < it->childCount(); i++)
				anyChild |= visit(it->child(i));
			const bool nameMatch = k == Kind::Stack && !query.isEmpty() && !favOnly &&
					       fold(it->text(0)).contains(query);
			if (nameMatch) {
				for (int i = 0; i < it->childCount(); i++) {
					QTreeWidgetItem *c = it->child(i);
					c->setHidden(c->data(0, MissingRole).toBool());
				}
			}
			visible = anyChild || nameMatch || (k == Kind::Stack && !filtering);
			if (filtering && visible)
				it->setExpanded(true);
		}
		it->setHidden(!visible);
		return visible;
	};
	for (int i = 0; i < tree->topLevelItemCount(); i++)
		visit(tree->topLevelItem(i));
}

void StacksDock::handleFrontendEvent(enum obs_frontend_event event)
{
	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
		setNativeDockHidden(hideNative);
		QTimer::singleShot(0, this, [this] { firstRun(); });
		[[fallthrough]];
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
		loaded = true;
		reconcile();
		break;
	case OBS_FRONTEND_EVENT_SCENE_LIST_CHANGED:
		reconcile();
		break;
	case OBS_FRONTEND_EVENT_SCENE_CHANGED:
	case OBS_FRONTEND_EVENT_PREVIEW_SCENE_CHANGED:
	case OBS_FRONTEND_EVENT_STUDIO_MODE_ENABLED:
	case OBS_FRONTEND_EVENT_STUDIO_MODE_DISABLED:
		updateIndicators();
		tree->viewport()->update();
		break;
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP:
	case OBS_FRONTEND_EVENT_EXIT:
		/* OBS saved the outgoing collection before clearing it and blocks
		 * saves from here on. A new or missing collection file never calls
		 * the load callback, so drop the old tree or it leaks into it. */
		loaded = false;
		readOnly = false;
		saveTimer.stop();
		lastJson.clear();
		tree->clear();
		break;
	default:
		break;
	}
}

/* ---------------------------- activation -------------------------------- */

void StacksDock::activate(QTreeWidgetItem *item, bool doubleClick)
{
	if (!item || kindOf(item) != Kind::Scene || item->data(0, MissingRole).toBool())
		return;
	obs_source_t *src = sceneByUuid(item->data(0, UuidRole).toString());
	if (!src)
		return;

	if (obs_frontend_preview_program_mode_active()) {
		obs_frontend_set_current_preview_scene(src);
		if (doubleClick) {
			obs_source_t *transition = obs_frontend_get_current_transition();
			const bool running = transition && obs_transition_get_time(transition) < 1.0f &&
					     obs_transition_get_time(transition) > 0.0f;
			obs_source_release(transition);
			if (!running)
				obs_frontend_preview_program_trigger_transition();
		}
	} else if (!safeClick || doubleClick) {
		obs_frontend_set_current_scene(src);
	}
	obs_source_release(src);
}

void StacksDock::onItemClicked(QTreeWidgetItem *item, int)
{
	activate(item, false);
}

void StacksDock::onItemDoubleClicked(QTreeWidgetItem *item, int)
{
	const Kind k = kindOf(item);
	if (k == Kind::Stack || k == Kind::Unsorted) {
		item->setExpanded(!item->isExpanded());
		return;
	}
	if (k == Kind::Separator) {
		busy++;
		renameItem(item);
		busy--;
		if (busy == 0 && pendingReconcile)
			reconcile();
		return;
	}
	activate(item, true);
}

/* ---------------------------- editing ----------------------------------- */

void StacksDock::onStructureChanged()
{
	moveUnsortedLast();
	/* A stack dropped into Unsorted's place or empty Unsorted stays tidy. */
	applyFilter();
	markDirty();
}

void StacksDock::newStack()
{
	bool ok = false;
	const QString name = QInputDialog::getText(this, T("Action.NewStack"), T("Prompt.StackName"), QLineEdit::Normal,
						   T("Default.StackName"), &ok)
				     .trimmed();
	if (!ok || name.isEmpty())
		return;

	QTreeWidgetItem *stack = makeStack(name);
	QList<QTreeWidgetItem *> chosen;
	for (QTreeWidgetItem *it : tree->selectedItems()) {
		if (kindOf(it) == Kind::Scene || kindOf(it) == Kind::Separator)
			chosen.append(it);
	}

	/* Insert where the first selected item sits at the top level. */
	int row = tree->topLevelItemCount();
	if (!chosen.isEmpty()) {
		QTreeWidgetItem *top = chosen.first();
		while (top->parent())
			top = top->parent();
		if (kindOf(top) != Kind::Unsorted)
			row = tree->indexOfTopLevelItem(top);
	}
	tree->insertTopLevelItem(row, stack);
	stack->setExpanded(true);
	moveToStack(chosen, stack);
	moveUnsortedLast();
	tree->setCurrentItem(stack);
	applyFilter();
	markDirty();
}

void StacksDock::newSeparator()
{
	bool ok = false;
	const QString label = QInputDialog::getText(this, T("Action.NewSeparator"), T("Prompt.SeparatorLabel"),
						    QLineEdit::Normal, QString(), &ok)
				      .trimmed();
	if (!ok)
		return;
	int row = -1;
	QTreeWidgetItem *parent = insertionParent(&row);
	QTreeWidgetItem *sep = makeSeparator(label);
	if (parent)
		parent->insertChild(row, sep);
	else
		tree->insertTopLevelItem(row < 0 ? tree->topLevelItemCount() : row, sep);
	moveUnsortedLast();
	applyFilter();
	markDirty();
}

void StacksDock::addScene()
{
	bool ok = false;
	QString name = QInputDialog::getText(this, T("Action.AddScene"), T("Prompt.SceneName"), QLineEdit::Normal,
					     T("Default.SceneName"), &ok)
			       .trimmed();
	if (!ok || name.isEmpty())
		return;
	obs_source_t *existing = obs_get_source_by_name(name.toUtf8().constData());
	if (existing) {
		obs_source_release(existing);
		QMessageBox::warning(this, T("Action.AddScene"), T("Error.NameExists"));
		return;
	}

	int row = -1;
	QTreeWidgetItem *parent = insertionParent(&row);

	obs_scene_t *scene = obs_scene_create(name.toUtf8().constData());
	if (!scene)
		return;
	obs_source_t *src = obs_scene_get_source(scene);
	const QString uuid = QString::fromUtf8(obs_source_get_uuid(src));
	obs_scene_release(scene);

	/* Place it now so reconcile does not file it into Unsorted. */
	QTreeWidgetItem *item = makeScene(uuid, name);
	if (parent)
		parent->insertChild(row, item);
	else
		tree->insertTopLevelItem(row < 0 ? tree->topLevelItemCount() : row, item);
	moveUnsortedLast();
	tree->setCurrentItem(item);
	reconcile();
	markDirty();
}

void StacksDock::renameItem(QTreeWidgetItem *item)
{
	const Kind k = kindOf(item);
	if (k == Kind::Unsorted)
		return;
	bool ok = false;
	const QString title = k == Kind::Scene ? T("Action.RenameScene") : T("Action.Rename");
	const QString name = QInputDialog::getText(this, title, title, QLineEdit::Normal, item->text(0), &ok).trimmed();
	if (!ok || name == item->text(0))
		return;

	if (k == Kind::Scene) {
		if (name.isEmpty())
			return;
		obs_source_t *existing = obs_get_source_by_name(name.toUtf8().constData());
		if (existing) {
			obs_source_release(existing);
			QMessageBox::warning(this, title, T("Error.NameExists"));
			return;
		}
		obs_source_t *src = sceneByUuid(item->data(0, UuidRole).toString());
		if (src) {
			obs_source_set_name(src, name.toUtf8().constData());
			obs_source_release(src);
		}
		item->setText(0, name);
	} else {
		if (k == Kind::Stack && name.isEmpty())
			return;
		item->setText(0, name);
	}
	applyFilter();
	markDirty();
}

void StacksDock::deleteScene(QTreeWidgetItem *item)
{
	obs_source_t *src = sceneByUuid(item->data(0, UuidRole).toString());
	if (!src)
		return;

	obs_source_t *program = obs_frontend_get_current_scene();
	const bool onAir = program == src;
	obs_source_release(program);
	if (onAir) {
		obs_source_release(src);
		QMessageBox::information(this, T("Action.DeleteScene"), T("Error.DeleteProgram"));
		return;
	}

	const QString text = T("Confirm.DeleteScene").arg(item->text(0));
	if (QMessageBox::question(this, T("Action.DeleteScene"), text, QMessageBox::Yes | QMessageBox::Cancel,
				  QMessageBox::Cancel) == QMessageBox::Yes) {
		obs_source_remove(src);
	}
	obs_source_release(src);
}

void StacksDock::dissolveStack(QTreeWidgetItem *stack)
{
	const int row = tree->indexOfTopLevelItem(stack);
	if (row < 0)
		return;
	QList<QTreeWidgetItem *> children = stack->takeChildren();
	for (int i = 0; i < children.size(); i++)
		tree->insertTopLevelItem(row + 1 + i, children[i]);
	delete stack;
	moveUnsortedLast();
	applyFilter();
	markDirty();
}

void StacksDock::deleteOrgItem(QTreeWidgetItem *item)
{
	if (kindOf(item) == Kind::Stack)
		dissolveStack(item);
	else if (kindOf(item) == Kind::Separator) {
		delete item;
		markDirty();
	}
}

void StacksDock::setColor(const QList<QTreeWidgetItem *> &items, const QString &color)
{
	for (QTreeWidgetItem *it : items) {
		if (kindOf(it) != Kind::Unsorted)
			it->setData(0, ColorRole, color);
	}
	tree->viewport()->update();
	markDirty();
}

void StacksDock::toggleFavorite(const QList<QTreeWidgetItem *> &items)
{
	bool allFav = true;
	for (QTreeWidgetItem *it : items)
		allFav &= it->data(0, FavoriteRole).toBool();
	for (QTreeWidgetItem *it : items) {
		if (kindOf(it) == Kind::Scene)
			it->setData(0, FavoriteRole, !allFav);
	}
	applyFilter();
	markDirty();
}

void StacksDock::moveToStack(const QList<QTreeWidgetItem *> &items, QTreeWidgetItem *stack)
{
	for (QTreeWidgetItem *it : items) {
		const Kind k = kindOf(it);
		if (k != Kind::Scene && k != Kind::Separator)
			continue;
		if (it == stack || it->parent() == stack)
			continue;
		QTreeWidgetItem *p = it->parent();
		if (p)
			p->removeChild(it);
		else
			tree->takeTopLevelItem(tree->indexOfTopLevelItem(it));
		if (stack)
			stack->addChild(it);
		else
			tree->insertTopLevelItem(std::max(0, tree->topLevelItemCount() - 1), it);
	}
	if (stack)
		stack->setExpanded(true);
	moveUnsortedLast();
	applyFilter();
	markDirty();
}

void StacksDock::firstRun()
{
	config_t *cfg = userConfig();
	if (!cfg || config_get_bool(cfg, kConfigSection, "FirstRunDone"))
		return;
	config_set_bool(cfg, kConfigSection, "FirstRunDone", true);

	/* A dock added by id starts hidden and floating: place ours once. */
	auto *main = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	auto *own = qobject_cast<QDockWidget *>(parentWidget());
	if (main && own) {
		own->setFloating(false);
		QDockWidget *native = nativeScenesDock();
		if (native && main->dockWidgetArea(native) != Qt::NoDockWidgetArea)
			main->splitDockWidget(native, own, Qt::Horizontal);
		else
			main->addDockWidget(Qt::LeftDockWidgetArea, own);
		own->show();
		own->raise();
	}

	QMessageBox box(this);
	box.setWindowTitle(T("FirstRun.Title"));
	box.setText(T("FirstRun.Text"));
	box.setInformativeText(T("FirstRun.Recovery"));
	QPushButton *hideBtn = box.addButton(T("FirstRun.Hide"), QMessageBox::AcceptRole);
	box.addButton(T("FirstRun.KeepBoth"), QMessageBox::RejectRole);
	QPushButton *later = box.addButton(T("FirstRun.Later"), QMessageBox::DestructiveRole);
	box.setDefaultButton(hideBtn);
	box.setEscapeButton(later);
	box.exec();
	if (box.clickedButton() == hideBtn) {
		hideNative = true;
		config_set_bool(cfg, kConfigSection, "HideNativeScenes", true);
		setNativeDockHidden(true);
		if (QAction *a = hideNativeAction)
			a->setChecked(true);
	}
}

void StacksDock::setNativeDockHidden(bool hidden)
{
	QDockWidget *dock = nativeScenesDock();
	if (!dock) {
		if (hidden)
			obs_log(LOG_WARNING, "native Scenes dock not found; hide it from the Docks menu");
		return;
	}
	dock->setVisible(!hidden);
}

/* ---------------------------- context menu ------------------------------ */

void StacksDock::onContextMenu(const QPoint &pos)
{
	QTreeWidgetItem *item = tree->itemAt(pos);
	if (item && !item->isSelected()) {
		tree->clearSelection();
		item->setSelected(true);
	}
	tree->setCurrentItem(item, 0, QItemSelectionModel::NoUpdate);
	const QList<QTreeWidgetItem *> selection = tree->selectedItems();
	const Kind k = kindOf(item);

	QMenu menu(this);

	if (item && k == Kind::Scene) {
		menu.addAction(T("Action.RenameScene"), this, [this, item] { renameItem(item); });
		const bool fav = item->data(0, FavoriteRole).toBool();
		menu.addAction(fav ? T("Action.Unfavorite") : T("Action.Favorite"), this,
			       [this, selection] { toggleFavorite(selection); });

		QMenu *move = menu.addMenu(T("Action.MoveToStack"));
		for (int i = 0; i < tree->topLevelItemCount(); i++) {
			QTreeWidgetItem *s = tree->topLevelItem(i);
			if (kindOf(s) != Kind::Stack)
				continue;
			move->addAction(s->text(0), this, [this, selection, s] { moveToStack(selection, s); });
		}
		move->addSeparator();
		move->addAction(T("Action.MoveToTop"), this, [this, selection] { moveToStack(selection, nullptr); });
		move->addAction(T("Action.NewStackFromSelection"), this, [this] { newStack(); });
	}

	if (item && k != Kind::Unsorted) {
		QMenu *colors = menu.addMenu(T("Action.Color"));
		for (const PaletteEntry &p : kPalette) {
			QPixmap swatch(12, 12);
			swatch.fill(QColor(p.hex));
			const QString hex = QString::fromLatin1(p.hex);
			colors->addAction(QIcon(swatch), T(p.key), this,
					  [this, selection, hex] { setColor(selection, hex); });
		}
		colors->addSeparator();
		colors->addAction(T("Color.Custom"), this, [this, selection, item] {
			const QColor c = QColorDialog::getColor(QColor(item->data(0, ColorRole).toString()), this,
								T("Action.Color"));
			if (c.isValid())
				setColor(selection, c.name());
		});
		colors->addAction(T("Color.None"), this, [this, selection] { setColor(selection, QString()); });
	}

	if (item && (k == Kind::Stack || k == Kind::Separator)) {
		menu.addAction(T("Action.Rename"), this, [this, item] { renameItem(item); });
		if (k == Kind::Stack)
			menu.addAction(T("Action.DissolveStack"), this, [this, item] { dissolveStack(item); });
		else
			menu.addAction(T("Action.DeleteSeparator"), this, [this, item] { deleteOrgItem(item); });
	}

	if (!menu.isEmpty())
		menu.addSeparator();
	menu.addAction(T("Action.AddScene"), this, [this] { addScene(); });
	menu.addAction(T("Action.NewStack"), this, [this] { newStack(); });
	menu.addAction(T("Action.NewSeparator"), this, [this] { newSeparator(); });

	if (item && k == Kind::Scene) {
		menu.addSeparator();
		menu.addAction(T("Action.DeleteScene"), this, [this, item] { deleteScene(item); });
	}

	busy++;
	menu.exec(tree->viewport()->mapToGlobal(pos));
	busy--;
	if (busy == 0 && pendingReconcile)
		reconcile();
}

} // namespace stacks
