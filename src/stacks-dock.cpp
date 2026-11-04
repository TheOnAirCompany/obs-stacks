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
#include "icon-library.hpp"
#include "scene-actions.hpp"
#include "backup-store.hpp"

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
#include <QActionGroup>
#include <QFile>
#include <QFileDialog>
#include <QSet>
#include <QDateTime>
#include <QKeyEvent>
#include <QLabel>
#include <QShortcut>
#include <QVBoxLayout>

#include <obs-module.h>
#include <util/config-file.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>
#include <functional>

#include <plugin-support.h>

namespace stacks {

namespace {

constexpr const char *kModuleKey = "obs-stacks";
constexpr const char *kConfigSection = "obs-stacks";
constexpr int kSchemaVersion = 1;
constexpr int kMaxDepth = 4;
constexpr int kTombstoneSoftCap = 200;
constexpr int kTombstoneHardCap = 2000;

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

	if (newParent && kindOf(newParent) == Kind::Favorites)
		return false; /* The pinned section only mirrors favorites. */
	if (newParent && kindOf(newParent) == Kind::Unsorted)
		return false; /* Nothing is filed into Unsorted by hand. */
	if (newParent && kindOf(newParent) != Kind::Stack)
		return false;

	for (QTreeWidgetItem *item : dragged) {
		const Kind k = kindOf(item);
		if (k == Kind::Unsorted)
			return false;
		if (k == Kind::Stack) {
			/* A stack cannot go inside itself, and nesting stays shallow. */
			int depth = 0;
			for (QTreeWidgetItem *p = newParent; p; p = p->parent()) {
				if (p == item)
					return false;
				depth++;
			}
			if (depth >= kMaxDepth)
				return false;
		}
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
	if (k == Kind::Separator && index.data(SeparatorStyleRole).toString() == QLatin1String("spacer"))
		s.setHeight(compact ? 8 : 14);
	else if (k == Kind::Separator)
		s.setHeight(index.data(Qt::DisplayRole).toString().isEmpty() ? (compact ? 6 : 10)
									     : (compact ? 16 : 20));
	else
		s.setHeight(compact ? 20 : std::max(s.height(), 26));
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
		if (index.data(SeparatorStyleRole).toString() == QLatin1String("spacer")) {
			painter->restore();
			return; /* an empty gap */
		}
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

	const QString iconName = showIcons ? index.data(IconRole).toString() : QString();
	if (!iconName.isEmpty()) {
		const QColor tint = color.isValid() ? color : pal.text().color();
		const int px = compact ? 14 : 16;
		opt.icon = stacks::icons::icon(iconName, tint, px);
		if (!opt.icon.isNull()) {
			opt.features |= QStyleOptionViewItem::HasDecoration;
			opt.decorationSize = QSize(px, px);
		}
	}

	const bool isProgramScene = index.data(ProgramRole).toBool();
	/* A collapsed stack shows the badge of the scene it hides. */
	const bool program = showBadges && (isProgramScene || index.data(ContainsProgramRole).toBool());
	const bool preview = showBadges && !program &&
			     (index.data(PreviewRole).toBool() || index.data(ContainsPreviewRole).toBool());
	const bool favorite = index.data(FavoriteRole).toBool();
	const bool missing = index.data(MissingRole).toBool();
	const bool isGroup = (k == Kind::Stack || k == Kind::Unsorted || k == Kind::Favorites);
	const int count = (showCounts && k == Kind::Stack) ? index.data(CountRole).toInt() : -1;

	if (missing) {
		opt.font.setItalic(true);
		QColor faded = pal.text().color();
		faded.setAlphaF(0.45);
		opt.palette.setColor(QPalette::Text, faded);
	}

	if (isGroup) {
		opt.font.setBold(true);
		if (k == Kind::Unsorted)
			opt.font.setItalic(true);
	}
	if (isProgramScene)
		opt.font.setBold(true);

	/* Color display mode: strip + tinted stacks, strip, tint, or dot. */
	const bool tintStack = colorMode == 0 || colorMode == 2;
	const bool tintScene = colorMode == 2;
	if (color.isValid() && !(opt.state & QStyle::State_Selected) &&
	    ((k == Kind::Stack && tintStack) || (k == Kind::Scene && tintScene))) {
		QColor tint = color;
		tint.setAlphaF(k == Kind::Stack ? 0.16 : 0.12);
		painter->fillRect(option.rect, tint);
	}

	/* Leave room for the color mark, the badges, the count and the star. */
	const int badgeW = (program || preview) ? 42 : 0;
	const int favW = favorite ? 16 : 0;
	QString countText;
	int countW = 0;
	if (count >= 0) {
		countText = QString::number(count);
		countW = QFontMetrics(option.font).horizontalAdvance(countText) + 10;
	}
	opt.rect.adjust(colorMode == 3 ? 16 : 8, 0, -(badgeW + favW + countW + 4), 0);
	QStyledItemDelegate::paint(painter, opt, index);

	painter->save();
	painter->setRenderHint(QPainter::Antialiasing, true);

	if (color.isValid() && (colorMode == 0 || colorMode == 1)) {
		const QRect strip(option.rect.left() + 2, option.rect.top() + 4, 3, option.rect.height() - 8);
		QPainterPath path;
		path.addRoundedRect(strip, 1.5, 1.5);
		painter->fillPath(path, color);
	} else if (color.isValid() && colorMode == 3) {
		const int d = 8;
		painter->setPen(Qt::NoPen);
		painter->setBrush(color);
		painter->drawEllipse(QRect(option.rect.left() + 4, option.rect.center().y() - d / 2, d, d));
	}

	int x = option.rect.right() - 4;
	if (program || preview) {
		const QString text = program ? QStringLiteral("PGM") : QStringLiteral("PVW");
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
		x -= favW;
	}
	if (countW > 0) {
		QColor muted = pal.text().color();
		muted.setAlphaF(0.5);
		painter->setPen(muted);
		painter->setFont(option.font);
		painter->drawText(QRect(x - countW, option.rect.top(), countW - 4, option.rect.height()),
				  Qt::AlignVCenter | Qt::AlignRight, countText);
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
	QAction *newSpacerAct = settings->addAction(T("Action.NewSpacer"));
	QAction *addSceneAct = settings->addAction(T("Action.AddScene"));
	settings->addSeparator();
	QAction *expandAct = settings->addAction(T("Action.ExpandAll"));
	QAction *collapseAct = settings->addAction(T("Action.CollapseAll"));
	settings->addSeparator();
	QAction *exportAct = settings->addAction(T("Action.Export"));
	QAction *importAct = settings->addAction(T("Action.Import"));
	QAction *restoreAct = settings->addAction(T("Action.RestoreBackup"));
	settings->addSeparator();
	QAction *showMissingAct = settings->addAction(T("Setting.ShowMissing"));
	showMissingAct->setCheckable(true);
	QAction *cleanupAct = settings->addAction(T("Action.CleanupMissing"));
	settings->addSeparator();
	QMenu *display = settings->addMenu(T("Display.Mode"));
	QAction *organizedAct = display->addAction(T("Display.Organized"));
	QAction *compactAct = display->addAction(T("Display.Compact"));
	organizedAct->setCheckable(true);
	compactAct->setCheckable(true);
	auto *modeGroup = new QActionGroup(display);
	modeGroup->addAction(organizedAct);
	modeGroup->addAction(compactAct);
	display->addSeparator();
	QMenu *colorMenu = display->addMenu(T("Display.ColorMode"));
	auto *colorGroup = new QActionGroup(colorMenu);
	QList<QAction *> colorActs;
	for (const char *key :
	     {"Display.Color.StripTint", "Display.Color.Strip", "Display.Color.Tint", "Display.Color.Dot"}) {
		QAction *a = colorMenu->addAction(T(key));
		a->setCheckable(true);
		colorGroup->addAction(a);
		colorActs.append(a);
	}
	display->addSeparator();
	QAction *iconsAct = display->addAction(T("Display.ShowIcons"));
	QAction *badgesAct = display->addAction(T("Display.ShowBadges"));
	QAction *countsAct = display->addAction(T("Display.ShowCounts"));
	QAction *revealAct = display->addAction(T("Display.RevealProgram"));
	for (QAction *a : {iconsAct, badgesAct, countsAct, revealAct})
		a->setCheckable(true);
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
	if (cfg) {
		config_set_default_bool(cfg, kConfigSection, "Compact", false);
		compact = config_get_bool(cfg, kConfigSection, "Compact");
		config_set_default_int(cfg, kConfigSection, "ColorMode", 0);
		config_set_default_bool(cfg, kConfigSection, "ShowIcons", true);
		config_set_default_bool(cfg, kConfigSection, "ShowBadges", true);
		config_set_default_bool(cfg, kConfigSection, "ShowCounts", false);
		config_set_default_bool(cfg, kConfigSection, "RevealProgram", false);
		config_set_default_bool(cfg, kConfigSection, "ShowMissing", false);
		showMissing = config_get_bool(cfg, kConfigSection, "ShowMissing");
		revealProgram = config_get_bool(cfg, kConfigSection, "RevealProgram");
	}
	(compact ? compactAct : organizedAct)->setChecked(true);
	safeAct->setChecked(safeClick);
	hideNativeAct->setChecked(hideNative);

	connect(newStackAct, &QAction::triggered, this, [this] { newStack(); });
	connect(exportAct, &QAction::triggered, this, [this] { exportJson(); });
	connect(importAct, &QAction::triggered, this, [this] { importJson(); });
	connect(compactAct, &QAction::toggled, this, [this](bool on) {
		compact = on;
		if (config_t *c = userConfig())
			config_set_bool(c, kConfigSection, "Compact", on);
		applyDisplayMode();
	});
	connect(newSepAct, &QAction::triggered, this, [this] { newSeparator(); });
	connect(newSpacerAct, &QAction::triggered, this, [this] { newSpacer(); });
	connect(restoreAct, &QAction::triggered, this, [this] { restoreBackup(); });
	connect(cleanupAct, &QAction::triggered, this, [this] { cleanupMissing(true); });
	showMissingAct->setChecked(showMissing);
	connect(showMissingAct, &QAction::toggled, this, [this](bool on) {
		showMissing = on;
		if (config_t *c = userConfig())
			config_set_bool(c, kConfigSection, "ShowMissing", on);
		applyFilter();
	});
	connect(settings, &QMenu::aboutToShow, this, [this, cleanupAct] {
		QList<QTreeWidgetItem *> all;
		collectScenes(nullptr, all);
		int n = 0;
		for (QTreeWidgetItem *it : all)
			n += it->data(0, MissingRole).toBool() ? 1 : 0;
		cleanupAct->setText(T("Action.CleanupMissing") + QStringLiteral(" (%1)").arg(n));
		cleanupAct->setEnabled(n > 0 && loaded && !readOnly);
	});
	revealAct->setChecked(revealProgram);
	connect(revealAct, &QAction::toggled, this, [this](bool on) {
		revealProgram = on;
		if (config_t *c = userConfig())
			config_set_bool(c, kConfigSection, "RevealProgram", on);
	});
	connect(addSceneAct, &QAction::triggered, this, [this] { addScene(); });
	connect(expandAct, &QAction::triggered, this, [this] {
		tree->expandAll();
		markDirty(false);
	});
	connect(collapseAct, &QAction::triggered, this, [this] {
		tree->collapseAll();
		markDirty(false);
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
	header->setContentsMargins(4, 2, 4, 4);
	header->setSpacing(4);
	header->addWidget(search, 1);
	header->addWidget(favoritesButton);
	header->addWidget(menuButton);

	tree = new StacksTree(this);
	delegate = new StacksDelegate(tree);
	tree->setItemDelegate(delegate);

	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(0);
	/* Search and options sit below the list (design review DR-14). */
	layout->addWidget(tree, 1);
	layout->addLayout(header);

	connect(tree, &QTreeWidget::itemClicked, this, &StacksDock::onItemClicked);
	connect(tree, &QTreeWidget::itemDoubleClicked, this, &StacksDock::onItemDoubleClicked);
	connect(tree, &QWidget::customContextMenuRequested, this, &StacksDock::onContextMenu);
	connect(tree, &StacksTree::structureChanged, this, &StacksDock::onStructureChanged);
	connect(tree, &QTreeWidget::itemExpanded, this, [this](QTreeWidgetItem *) {
		updateIndicators();
		markDirty(false);
	});
	connect(tree, &QTreeWidget::itemCollapsed, this, [this](QTreeWidgetItem *) {
		updateIndicators();
		markDirty(false);
	});
	connect(search, &QLineEdit::textChanged, this, &StacksDock::applyFilter);
	connect(favoritesButton, &QToolButton::toggled, this, &StacksDock::applyFilter);

	/* Keyboard: F2 / Return rename (like OBS on macOS), Delete removes. */
	auto withItem = [this](auto fn) {
		return [this, fn] {
			QTreeWidgetItem *item = original(tree->currentItem());
			if (!item || kindOf(item) == Kind::Favorites)
				return;
			busy++;
			fn(item);
			busy--;
			if (busy == 0 && pendingReconcile)
				reconcile();
		};
	};
	auto *renameKey = new QShortcut(QKeySequence(Qt::Key_F2), tree, nullptr, nullptr, Qt::WidgetShortcut);
	connect(renameKey, &QShortcut::activated, this, withItem([this](QTreeWidgetItem *it) { renameItem(it); }));
	auto *returnKey = new QShortcut(QKeySequence(Qt::Key_Return), tree, nullptr, nullptr, Qt::WidgetShortcut);
	connect(returnKey, &QShortcut::activated, this, withItem([this](QTreeWidgetItem *it) { renameItem(it); }));
	auto removeItem = withItem([this](QTreeWidgetItem *it) {
		if (kindOf(it) == Kind::Scene)
			deleteScene(it);
		else
			deleteOrgItem(it);
	});
	for (const QKeySequence &seq : {QKeySequence(QKeySequence::Delete), QKeySequence(Qt::Key_Backspace)}) {
		auto *del = new QShortcut(seq, tree, nullptr, nullptr, Qt::WidgetShortcut);
		connect(del, &QShortcut::activated, this, removeItem);
	}

	instance = this;
	if (config_t *c = userConfig()) {
		delegate->colorMode = std::clamp((int)config_get_int(c, kConfigSection, "ColorMode"), 0, 3);
		delegate->showIcons = config_get_bool(c, kConfigSection, "ShowIcons");
		delegate->showBadges = config_get_bool(c, kConfigSection, "ShowBadges");
		delegate->showCounts = config_get_bool(c, kConfigSection, "ShowCounts");
	}
	colorActs[delegate->colorMode]->setChecked(true);
	for (int i = 0; i < colorActs.size(); i++) {
		connect(colorActs[i], &QAction::triggered, this, [this, i] {
			delegate->colorMode = i;
			if (config_t *c = userConfig())
				config_set_int(c, kConfigSection, "ColorMode", i);
			tree->viewport()->update();
		});
	}
	struct Toggle {
		QAction *action;
		bool *flag;
		const char *key;
	};
	for (const Toggle &t : {Toggle{iconsAct, &delegate->showIcons, "ShowIcons"},
				Toggle{badgesAct, &delegate->showBadges, "ShowBadges"},
				Toggle{countsAct, &delegate->showCounts, "ShowCounts"}}) {
		t.action->setChecked(*t.flag);
		bool *flag = t.flag;
		const char *key = t.key;
		connect(t.action, &QAction::toggled, this, [this, flag, key](bool on) {
			*flag = on;
			if (config_t *c = userConfig())
				config_set_bool(c, kConfigSection, key, on);
			tree->doItemsLayout();
			tree->viewport()->update();
		});
	}

	/* Inline rename. */
	connect(tree, &QTreeWidget::itemChanged, this, &StacksDock::onItemEdited);
	connect(delegate, &QAbstractItemDelegate::closeEditor, this, [this] {
		QTimer::singleShot(0, this, [this] {
			if (editingItem) {
				QTreeWidgetItem *it = editingItem;
				editingItem = nullptr;
				it->setFlags(it->flags() & ~Qt::ItemIsEditable);
			}
		});
	});

	/* Keyboard: search, move. */
	auto *findKey = new QShortcut(QKeySequence::Find, this, nullptr, nullptr, Qt::WidgetWithChildrenShortcut);
	connect(findKey, &QShortcut::activated, this, [this] {
		search->setFocus();
		search->selectAll();
	});
	auto *slashKey = new QShortcut(QKeySequence(Qt::Key_Slash), tree, nullptr, nullptr, Qt::WidgetShortcut);
	connect(slashKey, &QShortcut::activated, this, [this] {
		search->setFocus();
		search->selectAll();
	});
	auto *upKey = new QShortcut(QKeySequence(Qt::ALT | Qt::Key_Up), tree, nullptr, nullptr, Qt::WidgetShortcut);
	connect(upKey, &QShortcut::activated, this, [this] { moveItemBy(-1); });
	auto *downKey = new QShortcut(QKeySequence(Qt::ALT | Qt::Key_Down), tree, nullptr, nullptr, Qt::WidgetShortcut);
	connect(downKey, &QShortcut::activated, this, [this] { moveItemBy(1); });
	search->installEventFilter(this);
	connect(search, &QLineEdit::returnPressed, this, [this] { focusFirstMatch(); });

	/* Notice when many missing scenes are kept. */
	missingBanner = new QLabel(this);
	missingBanner->setWordWrap(true);
	missingBanner->setContentsMargins(6, 4, 6, 4);
	missingBanner->setTextFormat(Qt::RichText);
	missingBanner->hide();
	layout->insertWidget(1, missingBanner);
	connect(missingBanner, &QLabel::linkActivated, this, [this] { cleanupMissing(true); });

	applyDisplayMode();

	saveTimer.setSingleShot(true);
	saveTimer.setInterval(1500);
	connect(&saveTimer, &QTimer::timeout, this, [] { obs_frontend_save(); });
}

StacksDock::~StacksDock()
{
	if (instance == this)
		instance = nullptr;
}

void StacksDock::applyDisplayMode()
{
	if (!tree || !delegate)
		return;
	delegate->compact = compact;
	tree->setIndentation(compact ? 10 : 14);
	tree->doItemsLayout();
	tree->viewport()->update();
}

void StacksDock::exportJson()
{
	const QString path = QFileDialog::getSaveFileName(this, T("Action.Export"), QStringLiteral("stacks.json"),
							  QStringLiteral("JSON (*.json)"));
	if (path.isEmpty())
		return;
	QFile file(path);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		QMessageBox::warning(this, T("Action.Export"), T("Error.File"));
		return;
	}
	const QJsonDocument doc = QJsonDocument::fromJson((loaded && !readOnly ? currentJson() : lastJson).toUtf8());
	file.write(doc.toJson(QJsonDocument::Indented));
}

void StacksDock::importJson()
{
	if (!loaded || readOnly)
		return;
	const QString path =
		QFileDialog::getOpenFileName(this, T("Action.Import"), QString(), QStringLiteral("JSON (*.json)"));
	if (path.isEmpty())
		return;
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly) || file.size() > 8 * 1024 * 1024) {
		QMessageBox::warning(this, T("Action.Import"), T("Error.File"));
		return;
	}
	QJsonParseError err{};
	const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &err);
	const QJsonObject root = doc.object();
	if (err.error != QJsonParseError::NoError || !root["tree"].isArray() ||
	    root["schema_version"].toInt() > kSchemaVersion) {
		QMessageBox::warning(this, T("Action.Import"), T("Error.ImportInvalid"));
		return;
	}
	if (QMessageBox::question(this, T("Action.Import"), T("Confirm.Import"), QMessageBox::Yes | QMessageBox::Cancel,
				  QMessageBox::Cancel) != QMessageBox::Yes)
		return;
	/* Scenes are matched by UUID, then by name; nothing is created or deleted. */
	tree->clear();
	buildFromJson(root["tree"].toArray(), nullptr);
	reconcile();
	markDirty(); /* undoable: Edit > Undo restores the previous organization */
}

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
		if (item->data(0, MissingSinceRole).isValid())
			o["missing_since"] = item->data(0, MissingSinceRole).toLongLong();
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
		if (item->data(0, SeparatorStyleRole).toString() == QLatin1String("spacer"))
			o["style"] = "spacer";
		break;
	case Kind::Unsorted:
	case Kind::Favorites:
		break;
	}
	if (!color.isEmpty())
		o["color"] = color;
	const QString icon = item->data(0, IconRole).toString();
	if (!icon.isEmpty() && kindOf(item) != Kind::Separator)
		o["icon"] = icon;
	return o;
}

QJsonArray StacksDock::serializeChildren(QTreeWidgetItem *parent) const
{
	QJsonArray arr;
	const int n = parent ? parent->childCount() : tree->topLevelItemCount();
	for (int i = 0; i < n; i++) {
		QTreeWidgetItem *it = parent ? parent->child(i) : tree->topLevelItem(i);
		if (kindOf(it) == Kind::Unsorted || kindOf(it) == Kind::Favorites)
			continue; /* Derived sections are never stored. */
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
			if (o.contains("missing_since"))
				item->setData(0, MissingSinceRole, o["missing_since"].toVariant().toLongLong());
		} else if (type == "stack") {
			item = makeStack(o["name"].toString());
		} else if (type == "separator") {
			item = makeSeparator(o["label"].toString());
			if (o["style"].toString() == QLatin1String("spacer"))
				item->setData(0, SeparatorStyleRole, QStringLiteral("spacer"));
		} else {
			continue; /* Unknown node: skipped in view, kept in lastJson. */
		}
		if (o.contains("color"))
			item->setData(0, ColorRole, o["color"].toString());
		if (o.contains("icon"))
			item->setData(0, IconRole, o["icon"].toString());
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
		/* Rolling backup: at most one every 5 minutes, 10 kept. */
		backups::snapshot(backups::currentCollection(), lastJson);
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

QString StacksDock::currentJson() const
{
	QJsonObject root;
	root["schema_version"] = kSchemaVersion;
	root["tree"] = serializeChildren(nullptr);
	return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
}

StacksDock *StacksDock::instance = nullptr;

void StacksDock::undoRedoCallback(const char *data)
{
	if (instance && data)
		instance->restoreSnapshot(QString::fromUtf8(data));
}

void StacksDock::restoreSnapshot(const QString &json)
{
	if (!loaded || readOnly)
		return;
	const QJsonObject root = QJsonDocument::fromJson(json.toUtf8()).object();
	tree->clear();
	buildFromJson(root["tree"].toArray(), nullptr);
	snapshot = json;
	reconcile();
	saveTimer.start();
}

void StacksDock::markDirty(bool undoable)
{
	if (!loaded || readOnly)
		return;
	const QString now = currentJson();
	if (undoable && !snapshot.isEmpty() && now != snapshot) {
		obs_frontend_add_undo_redo_action(obs_module_text("Undo.Organize"), undoRedoCallback, undoRedoCallback,
						  snapshot.toUtf8().constData(), now.toUtf8().constData(), false);
	}
	snapshot = now;
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
			it->setData(0, MissingSinceRole, QVariant());
			it->setData(0, Qt::ToolTipRole, QVariant());
		} else if (it->parent() == unsorted) {
			delete it; /* Unsorted keeps no history. */
		} else {
			/* Tombstone: kept hidden so undo restores its place. */
			it->setData(0, MissingRole, true);
			if (!it->data(0, MissingSinceRole).isValid())
				it->setData(0, MissingSinceRole, QDateTime::currentSecsSinceEpoch());
			it->setData(0, Qt::ToolTipRole, T("Missing.Tooltip"));
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
	enforceTombstoneCaps();
	updateIndicators();
	applyFilter();
}

void StacksDock::enforceTombstoneCaps()
{
	QList<QTreeWidgetItem *> all;
	collectScenes(nullptr, all);
	QList<QTreeWidgetItem *> missing;
	for (QTreeWidgetItem *it : all) {
		if (it->data(0, MissingRole).toBool())
			missing.append(it);
	}
	if (missing.size() > kTombstoneHardCap && !readOnly) {
		std::sort(missing.begin(), missing.end(), [](QTreeWidgetItem *a, QTreeWidgetItem *b) {
			return a->data(0, MissingSinceRole).toLongLong() < b->data(0, MissingSinceRole).toLongLong();
		});
		const int excess = static_cast<int>(missing.size()) - kTombstoneHardCap;
		for (int i = 0; i < excess; i++)
			delete missing[i];
		missing = missing.mid(excess);
		obs_log(LOG_INFO, "trimmed %d oldest missing-scene entries", excess);
		markDirty(false);
	}
	if (missingBanner) {
		const bool show = missing.size() > kTombstoneSoftCap;
		if (show)
			missingBanner->setText(T("Missing.Banner").arg(missing.size()) +
					       QStringLiteral(" <a href=\"#\">") + T("Action.CleanupMissing") +
					       QStringLiteral("</a>"));
		missingBanner->setVisible(show);
	}
}

void StacksDock::cleanupMissing(bool ask)
{
	if (!loaded || readOnly)
		return;
	QList<QTreeWidgetItem *> all;
	collectScenes(nullptr, all);
	QList<QTreeWidgetItem *> missing;
	for (QTreeWidgetItem *it : all) {
		if (it->data(0, MissingRole).toBool())
			missing.append(it);
	}
	if (missing.isEmpty())
		return;
	if (ask &&
	    QMessageBox::question(this, T("Action.CleanupMissing"), T("Confirm.CleanupMissing").arg(missing.size()),
				  QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes)
		return;
	qDeleteAll(missing);
	enforceTombstoneCaps();
	applyFilter();
	markDirty(); /* undoable */
}

void StacksDock::restoreBackup()
{
	if (!loaded || readOnly)
		return;
	const QList<backups::Entry> entries = backups::list(backups::currentCollection());
	if (entries.isEmpty()) {
		QMessageBox::information(this, T("Action.RestoreBackup"), T("Backup.None"));
		return;
	}
	QStringList labels;
	for (const backups::Entry &e : entries)
		labels << QLocale().toString(e.when, QLocale::LongFormat);
	bool ok = false;
	const QString chosen =
		QInputDialog::getItem(this, T("Action.RestoreBackup"), T("Backup.Choose"), labels, 0, false, &ok);
	if (!ok)
		return;
	const QString json = backups::read(entries[labels.indexOf(chosen)].path);
	const QJsonObject root = QJsonDocument::fromJson(json.toUtf8()).object();
	if (!root["tree"].isArray() || root["schema_version"].toInt() > kSchemaVersion) {
		QMessageBox::warning(this, T("Action.RestoreBackup"), T("Error.ImportInvalid"));
		return;
	}
	tree->clear();
	buildFromJson(root["tree"].toArray(), nullptr);
	reconcile();
	markDirty(); /* undoable */
}

QTreeWidgetItem *StacksDock::original(QTreeWidgetItem *item) const
{
	if (!item || !item->parent() || kindOf(item->parent()) != Kind::Favorites)
		return item;
	const QString uuid = item->data(0, UuidRole).toString();
	QList<QTreeWidgetItem *> items;
	collectScenes(nullptr, items);
	for (QTreeWidgetItem *it : items) {
		if (it->data(0, UuidRole).toString() == uuid)
			return it;
	}
	return nullptr;
}

void StacksDock::rebuildFavorites()
{
	QTreeWidgetItem *section = nullptr;
	for (int i = 0; i < tree->topLevelItemCount(); i++) {
		if (kindOf(tree->topLevelItem(i)) == Kind::Favorites) {
			section = tree->topLevelItem(i);
			break;
		}
	}
	QList<QTreeWidgetItem *> items;
	collectScenes(nullptr, items);
	QList<QTreeWidgetItem *> favorites;
	for (QTreeWidgetItem *it : items) {
		if (it->data(0, FavoriteRole).toBool() && !it->data(0, MissingRole).toBool())
			favorites.append(it);
	}
	if (favorites.isEmpty()) {
		delete section;
		return;
	}
	bool expanded = true;
	if (!section) {
		section = new QTreeWidgetItem();
		section->setText(0, T("Favorites"));
		section->setData(0, KindRole, static_cast<int>(Kind::Favorites));
		section->setData(0, ColorRole, QStringLiteral("#FFC53D"));
		section->setData(0, IconRole, QStringLiteral("star"));
		section->setFlags(Qt::ItemIsEnabled);
		tree->insertTopLevelItem(0, section);
	} else {
		expanded = section->isExpanded();
		qDeleteAll(section->takeChildren());
		if (tree->indexOfTopLevelItem(section) != 0) {
			tree->takeTopLevelItem(tree->indexOfTopLevelItem(section));
			tree->insertTopLevelItem(0, section);
		}
	}
	for (QTreeWidgetItem *fav : favorites) {
		/* A mirror: activating it activates the scene, edits go to the original. */
		QTreeWidgetItem *mirror = makeScene(fav->data(0, UuidRole).toString(), fav->text(0));
		mirror->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
		for (int role : {int(ColorRole), int(IconRole), int(ProgramRole), int(PreviewRole)})
			mirror->setData(0, role, fav->data(0, role));
		section->addChild(mirror);
	}
	section->setExpanded(expanded);
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
	for (int i = 0; i < tree->topLevelItemCount(); i++) {
		QTreeWidgetItem *top = tree->topLevelItem(i);
		if (kindOf(top) == Kind::Favorites) {
			for (int j = 0; j < top->childCount(); j++)
				items.append(top->child(j));
		}
	}
	QTreeWidgetItem *programItem = nullptr;
	for (QTreeWidgetItem *it : items) {
		const QString uuid = it->data(0, UuidRole).toString();
		const bool isProgram = !uuid.isEmpty() && uuid == programUuid;
		it->setData(0, ProgramRole, isProgram);
		it->setData(0, PreviewRole, !uuid.isEmpty() && uuid == previewUuid && uuid != programUuid);
		if (isProgram && !programItem && !(it->parent() && kindOf(it->parent()) == Kind::Favorites))
			programItem = it;
	}

	/* Collapsed stacks carry the badge of the scene they hide. */
	std::function<std::pair<bool, bool>(QTreeWidgetItem *)> visit = [&](QTreeWidgetItem *it) {
		bool p = it->data(0, ProgramRole).toBool(), v = it->data(0, PreviewRole).toBool();
		for (int i = 0; i < it->childCount(); i++) {
			auto [cp, cv] = visit(it->child(i));
			p |= cp;
			v |= cv;
		}
		const Kind k = kindOf(it);
		if (k == Kind::Stack || k == Kind::Unsorted) {
			it->setData(0, ContainsProgramRole, p && !it->isExpanded());
			it->setData(0, ContainsPreviewRole, v && !it->isExpanded());
		}
		return std::make_pair(p, v);
	};
	for (int i = 0; i < tree->topLevelItemCount(); i++) {
		if (kindOf(tree->topLevelItem(i)) != Kind::Favorites)
			visit(tree->topLevelItem(i));
	}

	if (revealProgram && programItem && programItem != lastRevealed) {
		for (QTreeWidgetItem *p = programItem->parent(); p; p = p->parent())
			p->setExpanded(true);
		tree->scrollToItem(programItem);
	}
	lastRevealed = programItem;
}

void StacksDock::applyFilter()
{
	const QString query = fold(search->text().trimmed());
	const bool favOnly = favoritesButton->isChecked();
	const bool filtering = !query.isEmpty() || favOnly;
	rebuildFavorites();

	std::function<bool(QTreeWidgetItem *)> visit = [&](QTreeWidgetItem *it) -> bool {
		const Kind k = kindOf(it);
		bool visible = false;
		if (k == Kind::Favorites) {
			/* Hidden while filtering: the filtered tree already shows them. */
			visible = !filtering && it->childCount() > 0;
			it->setHidden(!visible);
			return visible;
		}
		if (k == Kind::Scene) {
			const bool missing = it->data(0, MissingRole).toBool();
			if (missing)
				visible = showMissing && !filtering;
			else
				visible = (query.isEmpty() || fold(it->text(0)).contains(query)) &&
					  (!favOnly || it->data(0, FavoriteRole).toBool());
		} else if (k == Kind::Separator) {
			visible = !filtering;
		} else {
			bool anyChild = false;
			for (int i = 0; i < it->childCount(); i++)
				anyChild |= visit(it->child(i));
			if (k == Kind::Stack) {
				QList<QTreeWidgetItem *> inside;
				collectScenes(it, inside);
				int n = 0;
				for (QTreeWidgetItem *c : inside)
					n += c->data(0, MissingRole).toBool() ? 0 : 1;
				it->setData(0, CountRole, n);
			}
			const bool nameMatch = k == Kind::Stack && !query.isEmpty() && !favOnly &&
					       fold(it->text(0)).contains(query);
			if (nameMatch) {
				for (int i = 0; i < it->childCount(); i++) {
					QTreeWidgetItem *c = it->child(i);
					c->setHidden(c->data(0, MissingRole).toBool() && !showMissing);
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
		snapshot = currentJson();
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
		if (doubleClick && !transitionRunning())
			obs_frontend_preview_program_trigger_transition();
	} else if (!safeClick || doubleClick) {
		obs_frontend_set_current_scene(src);
	}
	obs_source_release(src);
}

bool StacksDock::transitionRunning()
{
	obs_source_t *transition = obs_frontend_get_current_transition();
	const float t = transition ? obs_transition_get_time(transition) : 0.0f;
	obs_source_release(transition);
	return t > 0.0f && t < 1.0f;
}

void StacksDock::sendToProgram(QTreeWidgetItem *item)
{
	/* Studio Mode: set Preview, then run the transition, like the native button. */
	if (!item || kindOf(item) != Kind::Scene || item->data(0, MissingRole).toBool())
		return;
	if (!obs_frontend_preview_program_mode_active() || transitionRunning())
		return;
	obs_source_t *src = sceneByUuid(item->data(0, UuidRole).toString());
	if (!src)
		return;
	obs_frontend_set_current_preview_scene(src);
	obs_source_release(src);
	obs_frontend_preview_program_trigger_transition();
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
	QTreeWidgetItem *stack = makeStack(T("Default.StackName"));
	QList<QTreeWidgetItem *> chosen;
	for (QTreeWidgetItem *it : tree->selectedItems()) {
		if (kindOf(it) == Kind::Scene || kindOf(it) == Kind::Separator)
			chosen.append(it);
	}

	/* Insert where the first selected item sits (inside its stack). */
	QTreeWidgetItem *cur = chosen.isEmpty() ? tree->currentItem() : chosen.first();
	QTreeWidgetItem *host = nullptr;
	int row = -1;
	if (cur && kindOf(cur) == Kind::Stack && chosen.isEmpty()) {
		host = cur;
		row = cur->childCount();
	} else if (cur && cur->parent() && kindOf(cur->parent()) == Kind::Stack) {
		host = cur->parent();
		row = host->indexOfChild(cur);
	} else if (cur && !cur->parent()) {
		row = tree->indexOfTopLevelItem(cur);
	}
	int depth = 0;
	for (QTreeWidgetItem *p = host; p; p = p->parent())
		depth++;
	if (depth >= kMaxDepth)
		host = nullptr, row = -1;
	if (host)
		host->insertChild(row, stack);
	else
		tree->insertTopLevelItem(row < 0 ? tree->topLevelItemCount() : row, stack);
	stack->setExpanded(true);
	moveToStack(chosen, stack);
	moveUnsortedLast();
	tree->setCurrentItem(stack);
	applyFilter();
	markDirty();
	renameItem(stack); /* name it in place */
}

void StacksDock::newSpacer()
{
	int row = -1;
	QTreeWidgetItem *parent = insertionParent(&row);
	QTreeWidgetItem *sep = makeSeparator(QString());
	sep->setData(0, SeparatorStyleRole, QStringLiteral("spacer"));
	if (parent)
		parent->insertChild(row, sep);
	else
		tree->insertTopLevelItem(row < 0 ? tree->topLevelItemCount() : row, sep);
	moveUnsortedLast();
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
	if (k == Kind::Unsorted || k == Kind::Favorites || item->data(0, MissingRole).toBool())
		return;
	if (k == Kind::Separator && item->data(0, SeparatorStyleRole).toString() == QLatin1String("spacer"))
		return;
	/* Edit in place; onItemEdited validates and applies. */
	item->setFlags(item->flags() | Qt::ItemIsEditable);
	editingOld = item->text(0);
	editingItem = item;
	tree->scrollToItem(item);
	tree->editItem(item, 0);
}

void StacksDock::onItemEdited(QTreeWidgetItem *item)
{
	if (!item || item != editingItem)
		return;
	editingItem = nullptr;
	item->setFlags(item->flags() & ~Qt::ItemIsEditable);
	const Kind k = kindOf(item);
	const QString name = item->text(0).trimmed();
	const QString title = k == Kind::Scene ? T("Action.RenameScene") : T("Action.Rename");
	const QSignalBlocker block(tree);
	if (name == editingOld) {
		item->setText(0, editingOld);
		return;
	}
	item->setText(0, editingOld); /* applied below once valid */

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
				  QMessageBox::Cancel) != QMessageBox::Yes) {
		obs_source_release(src);
		return;
	}

	/* Same undo data as OBS's own scene removal, so Edit > Undo works.
	 * The scene and the sources only it uses are saved; the UUID is kept,
	 * so the restored scene returns to its place in its stack. */
	obs_scene_t *scene = obs_scene_from_source(src);
	obs_data_array_t *saved = obs_data_array_create();
	obs_scene_enum_items(scene, saveUndoSource, saved);
	obs_data_t *sceneData = obs_save_source(src);
	obs_data_array_push_back(saved, sceneData);
	obs_data_release(sceneData);

	obs_data_t *undoData = obs_data_create();
	obs_data_set_array(undoData, "sources_in_deleted_scene", saved);
	obs_data_array_release(saved);

	const std::string name = obs_source_get_name(src);
	const QString label = T("Undo.DeleteScene").arg(QString::fromStdString(name));
	obs_frontend_add_undo_redo_action(label.toUtf8().constData(), undoDeleteScene, redoDeleteScene,
					  obs_data_get_json(undoData), name.c_str(), false);
	obs_data_release(undoData);

	removeSceneAndPrune(src);
	obs_source_release(src);
}

bool StacksDock::saveUndoSource(obs_scene_t *, obs_sceneitem_t *item, void *p)
{
	obs_source_t *source = obs_sceneitem_get_source(item);
	if (obs_obj_is_private(source) && !obs_source_removed(source))
		return true;
	auto *array = static_cast<obs_data_array_t *>(p);
	const char *name = obs_source_get_name(source);
	for (size_t i = 0; i < obs_data_array_count(array); i++) {
		obs_data_t *d = obs_data_array_item(array, i);
		const bool same = strcmp(name, obs_data_get_string(d, "name")) == 0;
		obs_data_release(d);
		if (same)
			return true;
	}
	if (obs_source_is_group(source))
		obs_scene_enum_items(obs_group_from_source(source), saveUndoSource, p);
	obs_data_t *d = obs_save_source(source);
	obs_data_array_push_back(array, d);
	obs_data_release(d);
	return true;
}

void StacksDock::removeSceneAndPrune(obs_source_t *source)
{
	obs_source_remove(source);
	obs_enum_scenes(
		[](void *, obs_source_t *s) {
			if (strcmp(obs_source_get_id(s), "scene") == 0)
				obs_scene_prune_sources(obs_scene_from_source(s));
			return true;
		},
		nullptr);
}

void StacksDock::undoDeleteScene(const char *json)
{
	obs_data_t *base = obs_data_create_from_json(json);
	obs_data_array_t *saved = obs_data_get_array(base, "sources_in_deleted_scene");
	std::vector<obs_source_t *> created;
	for (size_t i = 0; i < obs_data_array_count(saved); i++) {
		obs_data_t *d = obs_data_array_item(saved, i);
		obs_source_t *existing = obs_get_source_by_name(obs_data_get_string(d, "name"));
		if (existing)
			obs_source_release(existing);
		else if (obs_source_t *src = obs_load_source(d))
			created.push_back(src);
		obs_data_release(d);
	}
	for (obs_source_t *src : created)
		obs_source_load2(src);
	for (obs_source_t *src : created)
		obs_source_release(src);
	obs_data_array_release(saved);
	obs_data_release(base);
	/* Program is never switched here, unlike OBS's own undo. */
	if (instance)
		QMetaObject::invokeMethod(instance, "reconcile", Qt::QueuedConnection);
}

void StacksDock::redoDeleteScene(const char *name)
{
	obs_source_t *src = obs_get_source_by_name(name);
	if (!src)
		return;
	obs_source_t *program = obs_frontend_get_current_scene();
	const bool onAir = program == src;
	obs_source_release(program);
	if (!onAir)
		removeSceneAndPrune(src);
	obs_source_release(src);
}

void StacksDock::moveItemBy(int delta)
{
	QTreeWidgetItem *item = tree->currentItem();
	if (!item || !loaded || readOnly)
		return;
	const Kind k = kindOf(item);
	if (k == Kind::Unsorted || k == Kind::Favorites)
		return;
	QTreeWidgetItem *parent = item->parent();
	if (parent && (kindOf(parent) == Kind::Unsorted || kindOf(parent) == Kind::Favorites))
		return;
	const int count = parent ? parent->childCount() : tree->topLevelItemCount();
	const int row = parent ? parent->indexOfChild(item) : tree->indexOfTopLevelItem(item);
	int target = row + delta;
	if (!parent) {
		/* Stay between the Favorites section and Unsorted. */
		const int first = (count > 0 && kindOf(tree->topLevelItem(0)) == Kind::Favorites) ? 1 : 0;
		const int last = (count > 0 && kindOf(tree->topLevelItem(count - 1)) == Kind::Unsorted) ? count - 2
													: count - 1;
		if (target < first || target > last)
			return;
	} else if (target < 0 || target >= count) {
		return;
	}
	const bool expanded = item->isExpanded();
	if (parent) {
		parent->takeChild(row);
		parent->insertChild(target, item);
	} else {
		tree->takeTopLevelItem(row);
		tree->insertTopLevelItem(target, item);
	}
	item->setExpanded(expanded);
	tree->setCurrentItem(item);
	applyFilter();
	markDirty();
}

void StacksDock::focusFirstMatch()
{
	std::function<QTreeWidgetItem *(QTreeWidgetItem *)> find = [&](QTreeWidgetItem *it) -> QTreeWidgetItem * {
		if (it->isHidden())
			return nullptr;
		if (kindOf(it) == Kind::Scene && !it->data(0, MissingRole).toBool() &&
		    !(it->parent() && kindOf(it->parent()) == Kind::Favorites))
			return it;
		for (int i = 0; i < it->childCount(); i++) {
			if (QTreeWidgetItem *f = find(it->child(i)))
				return f;
		}
		return nullptr;
	};
	for (int i = 0; i < tree->topLevelItemCount(); i++) {
		if (QTreeWidgetItem *f = find(tree->topLevelItem(i))) {
			/* Select only: activation stays an explicit click. */
			tree->setCurrentItem(f);
			tree->scrollToItem(f);
			tree->setFocus();
			return;
		}
	}
}

bool StacksDock::eventFilter(QObject *watched, QEvent *event)
{
	if (watched == search && event->type() == QEvent::KeyPress) {
		auto *key = static_cast<QKeyEvent *>(event);
		if (key->key() == Qt::Key_Down) {
			focusFirstMatch();
			return true;
		}
		if (key->key() == Qt::Key_Escape) {
			search->clear();
			tree->setFocus();
			return true;
		}
	}
	return QWidget::eventFilter(watched, event);
}

void StacksDock::dissolveStack(QTreeWidgetItem *stack)
{
	QTreeWidgetItem *parent = stack->parent();
	const int row = parent ? parent->indexOfChild(stack) : tree->indexOfTopLevelItem(stack);
	if (row < 0)
		return;
	QList<QTreeWidgetItem *> children = stack->takeChildren();
	for (int i = 0; i < children.size(); i++) {
		if (parent)
			parent->insertChild(row + 1 + i, children[i]);
		else
			tree->insertTopLevelItem(row + 1 + i, children[i]);
	}
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
	applyFilter();
	tree->viewport()->update();
	markDirty();
}

void StacksDock::chooseIcon(const QList<QTreeWidgetItem *> &items, QTreeWidgetItem *item)
{
	const QColor tint(item->data(0, ColorRole).toString());
	const QString name = stacks::icons::pick(this, item->data(0, IconRole).toString(),
						 tint.isValid() ? tint : palette().text().color());
	if (name.isNull())
		return; /* cancelled */
	for (QTreeWidgetItem *it : items) {
		const Kind k = kindOf(it);
		if (k == Kind::Scene || k == Kind::Stack)
			it->setData(0, IconRole, name);
	}
	applyFilter();
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
	if (item && kindOf(item) == Kind::Favorites)
		return;
	if (item && item->parent() && kindOf(item->parent()) == Kind::Favorites) {
		item = original(item);
		if (!item)
			return;
		tree->clearSelection();
	}
	if (item && !item->isSelected()) {
		tree->clearSelection();
		item->setSelected(true);
	}
	tree->setCurrentItem(item, 0, QItemSelectionModel::NoUpdate);
	const QList<QTreeWidgetItem *> selection = tree->selectedItems();
	const Kind k = kindOf(item);

	QMenu menu(this);

	if (item && k == Kind::Scene && item->data(0, MissingRole).toBool()) {
		/* A missing scene only keeps its place for undo; it can be forgotten. */
		menu.addAction(T("Action.ForgetMissing"), this, [this, selection] {
			for (QTreeWidgetItem *it : selection) {
				if (it->data(0, MissingRole).toBool())
					delete it;
			}
			enforceTombstoneCaps();
			applyFilter();
			markDirty();
		});
		menu.addAction(T("Action.CleanupMissing"), this, [this] { cleanupMissing(true); });
		busy++;
		menu.exec(tree->viewport()->mapToGlobal(pos));
		busy--;
		if (busy == 0 && pendingReconcile)
			reconcile();
		return;
	}

	if (item && k == Kind::Scene && obs_frontend_preview_program_mode_active()) {
		QAction *send = menu.addAction(T("Action.SendToProgram"), this, [this, item] { sendToProgram(item); });
		send->setEnabled(!transitionRunning());
		menu.addSeparator();
	}

	if (item && k == Kind::Scene) {
		menu.addAction(T("Action.RenameScene"), this, [this, item] { renameItem(item); });
		const bool fav = item->data(0, FavoriteRole).toBool();
		menu.addAction(fav ? T("Action.Unfavorite") : T("Action.Favorite"), this,
			       [this, selection] { toggleFavorite(selection); });

		QMenu *move = menu.addMenu(T("Action.MoveToStack"));
		std::function<void(QTreeWidgetItem *, int)> listStacks = [&](QTreeWidgetItem *parent, int level) {
			const int n = parent ? parent->childCount() : tree->topLevelItemCount();
			for (int i = 0; i < n; i++) {
				QTreeWidgetItem *s = parent ? parent->child(i) : tree->topLevelItem(i);
				if (kindOf(s) != Kind::Stack)
					continue;
				const QString label = QString(level * 3, QChar(' ')) + s->text(0);
				move->addAction(label, this, [this, selection, s] { moveToStack(selection, s); });
				listStacks(s, level + 1);
			}
		};
		listStacks(nullptr, 0);
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
		if (k != Kind::Separator)
			menu.addAction(T("Icon.Pick") + QStringLiteral("…"), this,
				       [this, selection, item] { chooseIcon(selection, item); });
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
	menu.addAction(T("Action.NewSpacer"), this, [this] { newSpacer(); });

	if (item && k == Kind::Scene) {
		if (obs_source_t *src = sceneByUuid(item->data(0, UuidRole).toString())) {
			menu.addSeparator();
			stacks::actions::appendSceneActions(&menu, src, this);
			obs_source_release(src);
		}
	}

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
