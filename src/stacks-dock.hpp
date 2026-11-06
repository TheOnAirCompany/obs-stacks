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

#pragma once

#include <QAction>
#include <QJsonArray>
#include <QLabel>
#include <QJsonObject>
#include <QLineEdit>
#include <QPointer>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QWidget>

#include <obs-frontend-api.h>
#include <obs.h>

namespace stacks {

enum class Kind : int { Scene = 1, Stack = 2, Separator = 3, Unsorted = 4, Favorites = 5 };

enum Role : int {
	KindRole = Qt::UserRole + 1,
	UuidRole,
	ColorRole,
	FavoriteRole,
	MissingRole,
	ProgramRole,
	PreviewRole,
	IconRole,
	MissingSinceRole,
	SeparatorStyleRole,
	CountRole,
	ContainsProgramRole,
	ContainsPreviewRole,
};

class StacksTree : public QTreeWidget {
	Q_OBJECT

public:
	explicit StacksTree(QWidget *parent = nullptr);

signals:
	void structureChanged();

protected:
	void dropEvent(QDropEvent *event) override;
	void dragMoveEvent(QDragMoveEvent *event) override;

private:
	bool dropAllowed(QDropEvent *event) const;
};

class StacksDelegate : public QStyledItemDelegate {
	Q_OBJECT

public:
	using QStyledItemDelegate::QStyledItemDelegate;
	void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
	QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;

	bool compact = false;
	int colorMode = 0; /* 0 strip + tint, 1 strip, 2 tint, 3 dot */
	bool showIcons = true;
	bool showBadges = true;
	bool showCounts = false;
};

class StacksDock : public QWidget {
	Q_OBJECT

public:
	explicit StacksDock(QWidget *parent = nullptr);
	~StacksDock() override;

	/* Called from the OBS save callback (UI thread). */
	void saveTo(obs_data_t *moduleData);
	void loadFrom(obs_data_t *moduleData);

	void handleFrontendEvent(enum obs_frontend_event event);

	/* Public API for other plugins (docs/INTEGRATION.md). UI thread only. */
	QString organizationJson() const;
	bool placeScene(const QString &uuid, const QString &stackPath);
	/* Emits the global "stacks_changed" signal once, later, if the organization changed. */
	void publishChanged();

public slots:
	void reconcile();
	void updateIndicators();

private slots:
	void onItemClicked(QTreeWidgetItem *item, int column);
	void onItemDoubleClicked(QTreeWidgetItem *item, int column);
	void onContextMenu(const QPoint &pos);
	void applyFilter();
	void onStructureChanged();

private:
	/* Tree <-> JSON */
	QJsonArray serializeChildren(QTreeWidgetItem *parent) const;
	QJsonObject serializeItem(QTreeWidgetItem *item) const;
	void buildFromJson(const QJsonArray &nodes, QTreeWidgetItem *parent);

	/* Items */
	QTreeWidgetItem *makeScene(const QString &uuid, const QString &name);
	QTreeWidgetItem *makeStack(const QString &name);
	QTreeWidgetItem *makeSeparator(const QString &label);
	QTreeWidgetItem *unsortedItem();
	void moveUnsortedLast();
	void rebuildFavorites();
	QTreeWidgetItem *original(QTreeWidgetItem *item) const;
	void collectScenes(QTreeWidgetItem *parent, QList<QTreeWidgetItem *> &out) const;
	QTreeWidgetItem *insertionParent(int *row) const;

	/* Actions */
	void activate(QTreeWidgetItem *item, bool doubleClick);
	void newStack();
	void newSeparator();
	void addScene();
	void renameItem(QTreeWidgetItem *item);
	void deleteScene(QTreeWidgetItem *item);
	bool deleteScenes(const QList<QTreeWidgetItem *> &items);
	void deleteSelection();
	void dissolveStack(QTreeWidgetItem *item);
	void deleteOrgItem(QTreeWidgetItem *item);
	void setColor(const QList<QTreeWidgetItem *> &items, const QString &color);
	void chooseIcon(const QList<QTreeWidgetItem *> &items, QTreeWidgetItem *item);
	void toggleFavorite(const QList<QTreeWidgetItem *> &items);
	void moveToStack(const QList<QTreeWidgetItem *> &items, QTreeWidgetItem *stack);
	void setNativeDockHidden(bool hidden);
	void firstRun();
	void markDirty(bool undoable = true);
	QString currentJson() const;
	void restoreSnapshot(const QString &json);
	static void undoRedoCallback(const char *data);
	static bool saveUndoSource(obs_scene_t *scene, obs_sceneitem_t *item, void *p);
	static void pruneScenes();
	static void undoDeleteScene(const char *json);
	static void redoDeleteScenes(const char *json);
	void exportJson();
	void importJson();
	void applyDisplayMode();
	void newSpacer();
	void sendToProgram(QTreeWidgetItem *item);
	static bool transitionRunning();
	void cleanupMissing(bool ask);
	void enforceTombstoneCaps();
	void onItemEdited(QTreeWidgetItem *item);
	void moveItemBy(int delta);
	void focusFirstMatch();
	void restoreBackup();
#ifdef STACKS_SELFTEST
	void runSelfTest();
#endif
	bool eventFilter(QObject *watched, QEvent *event) override;
	void resizeEvent(QResizeEvent *event) override;

	QTreeWidgetItem *editingItem = nullptr;
	QTreeWidgetItem *lastRevealed = nullptr; /* compared only, never dereferenced */
	QString editingOld;
	bool showMissing = false;
	bool revealProgram = false;
	QLabel *missingBanner = nullptr;

	static StacksDock *instance;
	StacksDelegate *delegate = nullptr;
	QString snapshot;
	bool compact = false;

	static obs_source_t *sceneByUuid(const QString &uuid);

	StacksTree *tree = nullptr;
	QLineEdit *search = nullptr;
	QToolButton *favoritesButton = nullptr;
	QToolButton *menuButton = nullptr;
	QToolButton *addButton = nullptr;
	QPointer<QAction> hideNativeAction;
	QTimer saveTimer;

	bool loaded = false;
	bool readOnly = false; /* newer or unreadable data: never overwritten */
	int busy = 0;          /* > 0 while a menu or dialog holds item pointers */
	bool pendingReconcile = false;
	bool safeClick = false;
	bool hideNative = false;
	QString lastJson;
	QString publishedJson;
	bool publishPosted = false;
};

} // namespace stacks
