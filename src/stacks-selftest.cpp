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

/* In-OBS self-test, compiled only with -DSTACKS_SELFTEST=ON and run only
 * when the STACKS_SELFTEST environment variable is set. It drives the real
 * dock code against the real frontend API on a disposable test collection
 * and logs "[selftest] PASS|FAIL <name>". Never part of a release build. */

#include "stacks-dock.hpp"
#include "backup-store.hpp"
#include "scene-actions.hpp"

#include <QApplication>
#include <QInputDialog>
#include <QKeyEvent>
#include <QMainWindow>
#include <QMessageBox>
#include <QPushButton>
#include <QTimer>
#include <QUuid>

#include <obs-module.h>
#include <util/config-file.h>
#include <plugin-support.h>

#include <functional>
#include <memory>
#include <vector>

namespace stacks {

namespace {

struct Runner {
	std::vector<std::pair<int, std::function<void()>>> steps;
	size_t next = 0;
	int pass = 0;
	int fail = 0;
	QTimer *autoAccept = nullptr;

	void check(bool ok, const char *name, const QString &detail = QString())
	{
		(ok ? pass : fail)++;
		obs_log(ok ? LOG_INFO : LOG_WARNING, "[selftest] %s %s %s", ok ? "PASS" : "FAIL", name,
			detail.toUtf8().constData());
	}
};

QTreeWidgetItem *findItem(QTreeWidget *tree, const QString &name, Kind kind)
{
	std::function<QTreeWidgetItem *(QTreeWidgetItem *)> walk = [&](QTreeWidgetItem *it) -> QTreeWidgetItem * {
		const Kind k = static_cast<Kind>(it->data(0, KindRole).toInt());
		if (k == Kind::Favorites)
			return nullptr; /* mirrors are not the real items */
		if (k == kind && it->text(0) == name)
			return it;
		for (int i = 0; i < it->childCount(); i++) {
			if (QTreeWidgetItem *f = walk(it->child(i)))
				return f;
		}
		return nullptr;
	};
	for (int i = 0; i < tree->topLevelItemCount(); i++) {
		if (QTreeWidgetItem *f = walk(tree->topLevelItem(i)))
			return f;
	}
	return nullptr;
}

QString programName()
{
	obs_source_t *p = obs_frontend_get_current_scene();
	QString n = p ? QString::fromUtf8(obs_source_get_name(p)) : QString();
	obs_source_release(p);
	return n;
}

bool sourceExists(const char *name)
{
	obs_source_t *s = obs_get_source_by_name(name);
	obs_source_release(s);
	return s != nullptr;
}

void setProgram(const char *name)
{
	if (obs_source_t *s = obs_get_source_by_name(name)) {
		obs_frontend_set_current_scene(s);
		obs_source_release(s);
	}
}

QAction *findAction(QMenu *menu, const QString &text)
{
	for (QAction *a : menu->actions()) {
		if (a->text() == text)
			return a;
		if (a->menu()) {
			if (QAction *f = findAction(a->menu(), text))
				return f;
		}
	}
	return nullptr;
}

QString txt(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

} // namespace

void StacksDock::runSelfTest()
{
	auto r = std::make_shared<Runner>();
	const QString shots = qEnvironmentVariable("STACKS_SELFTEST_SHOTS");

	/* Answer every modal dialog the code under test opens. */
	r->autoAccept = new QTimer(this);
	r->autoAccept->setInterval(150);
	connect(r->autoAccept, &QTimer::timeout, this, [] {
		QWidget *w = QApplication::activeModalWidget();
		if (auto *box = qobject_cast<QMessageBox *>(w)) {
			QAbstractButton *b = nullptr;
			for (QAbstractButton *candidate : box->buttons()) {
				if (box->buttonRole(candidate) == QMessageBox::YesRole)
					b = candidate;
			}
			if (!b)
				b = box->button(QMessageBox::Yes);
			if (!b)
				b = box->button(QMessageBox::Ok);
			if (!b)
				b = box->defaultButton();
			if (b)
				b->click();
		} else if (auto *input = qobject_cast<QInputDialog *>(w)) {
			input->accept();
		}
	});
	r->autoAccept->start();

	auto step = [r](int delayMs, std::function<void()> fn) {
		r->steps.emplace_back(delayMs, std::move(fn));
	};

	/* 1. Badge on a collapsed stack, and Reveal. */
	step(300, [this] {
		setProgram("Cam 2");
		if (QTreeWidgetItem *p = findItem(tree, "Plateau", Kind::Stack))
			p->setExpanded(false);
	});
	step(400, [this, r] {
		QTreeWidgetItem *p = findItem(tree, "Plateau", Kind::Stack);
		r->check(p && p->data(0, ContainsProgramRole).toBool(), "collapsed-stack-shows-pgm");
		if (p)
			p->setExpanded(true);
	});
	step(200, [this, r] {
		QTreeWidgetItem *p = findItem(tree, "Plateau", Kind::Stack);
		r->check(p && !p->data(0, ContainsProgramRole).toBool(), "expanded-stack-hides-pgm");
		revealProgram = true;
		if (p)
			p->setExpanded(false);
		setProgram("Cam 1");
	});
	step(400, [this, r] {
		QTreeWidgetItem *p = findItem(tree, "Plateau", Kind::Stack);
		r->check(p && p->isExpanded(), "reveal-program-expands");
		revealProgram = false;
	});

	/* 2. Send to Program in Studio Mode. */
	step(100, [] { obs_frontend_set_preview_program_mode(true); });
	step(800, [this, r] {
		QTreeWidgetItem *it = findItem(tree, "Pause", Kind::Scene);
		r->check(it != nullptr, "find-pause");
		sendToProgram(it);
	});
	step(1500, [r] {
		r->check(programName() == "Pause", "send-to-program", programName());
		obs_frontend_set_preview_program_mode(false);
	});

	/* 6. Inline rename of a scene and of a stack. */
	auto inlineRename = [this](QTreeWidgetItem *it, const QString &name) {
		renameItem(it);
		QLineEdit *editor = nullptr;
		for (QLineEdit *e : tree->findChildren<QLineEdit *>()) {
			if (e->isVisible())
				editor = e;
		}
		if (!editor)
			return false;
		editor->setText(name);
		QKeyEvent press(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
		QCoreApplication::sendEvent(editor, &press);
		return true;
	};
	step(500, [this, r, inlineRename] {
		QTreeWidgetItem *it = findItem(tree, "Test Zoom", Kind::Scene);
		r->check(it && inlineRename(it, "Zoom Renamed"), "inline-editor-opens");
	});
	step(500, [this, r, inlineRename] {
		r->check(sourceExists("Zoom Renamed") && !sourceExists("Test Zoom"), "inline-rename-scene");
		QTreeWidgetItem *st = findItem(tree, "Habillage", Kind::Stack);
		r->check(st && inlineRename(st, "Habillage 2"), "inline-editor-opens-stack");
	});
	step(400, [this, r] {
		r->check(findItem(tree, "Habillage 2", Kind::Stack) != nullptr, "inline-rename-stack");
		QTreeWidgetItem *it = findItem(tree, "Zoom Renamed", Kind::Scene);
		if (it) {
			/* A duplicate name is refused and the old name stays. */
			renameItem(it);
			for (QLineEdit *e : tree->findChildren<QLineEdit *>()) {
				if (e->isVisible()) {
					e->setText("Cam 1");
					QKeyEvent press(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
					QCoreApplication::sendEvent(e, &press);
				}
			}
		}
	});
	step(800, [this, r] {
		r->check(sourceExists("Zoom Renamed") && findItem(tree, "Zoom Renamed", Kind::Scene),
			 "inline-rename-refuses-duplicate");
	});

	/* 7. Move and search helpers, with the same entry points as the keys. */
	step(200, [this, r] {
		QTreeWidgetItem *c = findItem(tree, "Countdown", Kind::Scene);
		tree->setCurrentItem(c);
		const int before = c && c->parent() ? c->parent()->indexOfChild(c) : -1;
		moveItemBy(1);
		c = findItem(tree, "Countdown", Kind::Scene);
		const int after = c && c->parent() ? c->parent()->indexOfChild(c) : -1;
		r->check(before == 0 && after == 1, "alt-down-moves", QString("%1->%2").arg(before).arg(after));
		moveItemBy(-1);
		search->setText("cam");
	});
	step(300, [this, r] {
		QKeyEvent down(QEvent::KeyPress, Qt::Key_Down, Qt::NoModifier);
		QCoreApplication::sendEvent(search, &down);
		QTreeWidgetItem *cur = tree->currentItem();
		r->check(cur && cur->text(0).startsWith("Cam"), "search-down-selects-first-match",
			 cur ? cur->text(0) : QString());
		r->check(programName() != (cur ? cur->text(0) : QString()) || programName() == "Cam 1",
			 "search-select-does-not-activate");
		QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
		QCoreApplication::sendEvent(search, &esc);
		r->check(search->text().isEmpty(), "search-esc-clears");
	});

	/* 3. Delete, missing entry, undo, show missing, cleanup. */
	step(300, [this] {
		setProgram("Cam 1");
		if (QTreeWidgetItem *it = findItem(tree, "Plan large", Kind::Scene))
			deleteScene(it); /* the confirmation is answered Yes */
	});
	step(800, [this, r] {
		QTreeWidgetItem *it = findItem(tree, "Plan large", Kind::Scene);
		r->check(!sourceExists("Plan large"), "delete-scene");
		r->check(it && it->data(0, MissingRole).toBool() && it->isHidden(), "deleted-scene-kept-hidden");
		showMissing = true;
		applyFilter();
		r->check(it && !it->isHidden(), "show-missing-displays");
		showMissing = false;
		applyFilter();
		auto *main = static_cast<QMainWindow *>(obs_frontend_get_main_window());
		if (QAction *undo = main ? main->findChild<QAction *>("actionMainUndo") : nullptr)
			undo->trigger();
	});
	step(1000, [this, r] {
		QTreeWidgetItem *it = findItem(tree, "Plan large", Kind::Scene);
		r->check(sourceExists("Plan large"), "undo-restores-scene");
		r->check(it && !it->data(0, MissingRole).toBool() && it->parent() && it->parent()->text(0) == "Plateau",
			 "undo-restores-stack-place");
		r->check(programName() == "Cam 1", "undo-keeps-program", programName());
		if (it)
			deleteScene(it);
	});
	step(800, [this, r] {
		cleanupMissing(false);
		QTreeWidgetItem *it = findItem(tree, "Plan large", Kind::Scene);
		r->check(it == nullptr, "cleanup-missing");
	});

	/* 3. Caps: 201 tombstones show the notice, 2,001 are trimmed to 2,000. */
	step(200, [this, r] {
		QTreeWidgetItem *host = findItem(tree, "Ouverture", Kind::Stack);
		for (int i = 0; i < 201; i++) {
			QTreeWidgetItem *t = makeScene(QUuid::createUuid().toString(QUuid::WithoutBraces),
						       QStringLiteral("ghost %1").arg(i));
			t->setData(0, MissingSinceRole, 1000 + i);
			host->addChild(t);
		}
		reconcile();
		r->check(missingBanner && !missingBanner->isHidden(), "soft-cap-notice");
		for (int i = 201; i < 2001; i++) {
			QTreeWidgetItem *t = makeScene(QUuid::createUuid().toString(QUuid::WithoutBraces),
						       QStringLiteral("ghost %1").arg(i));
			t->setData(0, MissingSinceRole, 1000 + i);
			host->addChild(t);
		}
		reconcile();
		QList<QTreeWidgetItem *> all;
		collectScenes(nullptr, all);
		int missing = 0;
		for (QTreeWidgetItem *it : all)
			missing += it->data(0, MissingRole).toBool() ? 1 : 0;
		r->check(missing == 2000 && !findItem(tree, "ghost 0", Kind::Scene), "hard-cap-trims-oldest",
			 QString::number(missing));
		cleanupMissing(false);
		r->check(missingBanner->isHidden(), "notice-cleared");
	});

	/* 4. Rolling backups and restore. */
	step(300, [this, r] {
		const QString col = backups::currentCollection();
		backups::snapshot(col, currentJson(), true);
		r->check(!backups::list(col).isEmpty(), "backup-written", QString::number(backups::list(col).size()));
		if (QTreeWidgetItem *o = findItem(tree, "Ouverture", Kind::Stack))
			dissolveStack(o);
		r->check(findItem(tree, "Ouverture", Kind::Stack) == nullptr, "dissolve-before-restore");
		restoreBackup(); /* the picker is answered with the newest backup */
	});
	step(800, [this, r] {
		QTreeWidgetItem *o = findItem(tree, "Ouverture", Kind::Stack);
		r->check(o != nullptr && o->childCount() == 2, "restore-backup");
	});

	/* 5. Native scene actions. */
	step(300, [this, r] {
		obs_source_t *cam = obs_get_source_by_name("Cam 1");
		QMenu menu;
		actions::appendSceneActions(&menu, cam, this);
		QAction *mv = findAction(&menu, txt("SceneAction.ShowInMultiview"));
		r->check(mv != nullptr, "multiview-action-present");
		if (mv)
			mv->trigger();
		obs_data_t *priv = obs_source_get_private_settings(cam);
		r->check(!obs_data_get_bool(priv, "show_in_multiview"), "multiview-toggled-off");
		obs_data_release(priv);

		obs_frontend_source_list list = {};
		obs_frontend_get_transitions(&list);
		QString tname;
		if (list.sources.num > 0)
			tname = QString::fromUtf8(obs_source_get_name(list.sources.array[0]));
		obs_frontend_source_list_free(&list);
		if (QAction *t = findAction(&menu, tname))
			t->trigger();
		priv = obs_source_get_private_settings(cam);
		r->check(QString::fromUtf8(obs_data_get_string(priv, "transition")) == tname, "transition-override",
			 tname);
		obs_data_release(priv);

		if (QAction *dup = findAction(&menu, txt("SceneAction.Duplicate")))
			dup->trigger(); /* the name prompt is answered with its default */
		r->check(sourceExists("Cam 1 2"), "duplicate-scene");
		r->check(findAction(&menu, txt("SceneAction.Filters")) &&
				 findAction(&menu, txt("SceneAction.Screenshot")) &&
				 findAction(&menu, txt("SceneAction.Projector.Windowed")),
			 "filters-screenshot-projector-present");
		obs_source_release(cam);
	});

	/* 4. Backups: 5-minute throttle and 10 kept. */
	step(200, [r] {
		const QString col = backups::currentCollection();
		const qsizetype n0 = backups::list(col).size();
		backups::snapshot(col, QStringLiteral("{\"schema_version\":1,\"tree\":[],\"probe\":0}"), false);
		r->check(backups::list(col).size() == n0, "backup-throttled-within-5-min");
	});
	for (int i = 0; i < 12; i++) {
		step(1100, [i] {
			backups::snapshot(backups::currentCollection(),
					  QStringLiteral("{\"schema_version\":1,\"tree\":[],\"probe\":%1}").arg(i + 1),
					  true);
		});
	}
	step(200, [r] {
		const qsizetype n = backups::list(backups::currentCollection()).size();
		r->check(n == 10, "backup-keeps-newest-10", QString::number(n));
	});

	/* 5. Filters, screenshot and projector really open. */
	auto topLevelCount = [] {
		int n = 0;
		for (QWidget *w : QApplication::topLevelWidgets())
			n += w->isVisible() ? 1 : 0;
		return n;
	};
	auto shared = std::make_shared<int>(0);
	step(300, [this, shared, topLevelCount] {
		*shared = topLevelCount();
		obs_source_t *cam = obs_get_source_by_name("Cam 2");
		QMenu menu;
		actions::appendSceneActions(&menu, cam, this);
		if (QAction *a = findAction(&menu, txt("SceneAction.Filters")))
			a->trigger();
		if (QAction *a = findAction(&menu, txt("SceneAction.Projector.Windowed")))
			a->trigger();
		if (QAction *a = findAction(&menu, txt("SceneAction.Screenshot")))
			a->trigger();
		obs_source_release(cam);
	});
	step(1200, [r, shared, topLevelCount] {
		const int now = topLevelCount();
		QStringList opened;
		for (QWidget *w : QApplication::topLevelWidgets()) {
			const QString cls = QString::fromLatin1(w->metaObject()->className());
			if (w->isVisible() && (cls.contains("Filters") || cls.contains("Projector"))) {
				opened << cls;
				w->close();
			}
		}
		r->check(now >= *shared + 2 && opened.size() >= 2, "filters-and-projector-open", opened.join(","));
		obs_log(LOG_INFO, "[selftest] screenshot requested (see 'Saved screenshot to' in this log)");
	});

	/* 2 and 10 through the real menus: context menu and the options menu. */
	step(300, [this, r] {
		obs_frontend_set_preview_program_mode(true);
		setProgram("Cam 1");
	});
	step(800, [this, r] {
		QTreeWidgetItem *it = findItem(tree, "BRB", Kind::Scene);
		tree->scrollToItem(it);
		const QPoint pos = tree->visualItemRect(it).center();
		QTimer::singleShot(400, this, [] {
			if (auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget())) {
				if (QAction *a = findAction(menu, txt("Action.SendToProgram")))
					a->trigger();
				menu->close();
			}
		});
		onContextMenu(pos); /* the real context menu, answered by the timer */
	});
	step(1500, [r] {
		r->check(programName() == "BRB", "context-menu-send-to-program", programName());
		obs_frontend_set_preview_program_mode(false);
	});
	step(500, [this, r] {
		QMenu *options = menuButton->menu();
		QAction *dot = options ? findAction(options, txt("Display.Color.Dot")) : nullptr;
		if (dot)
			dot->trigger();
		config_t *cfg = obs_frontend_get_user_config();
		r->check(dot && delegate->colorMode == 3 && config_get_int(cfg, "obs-stacks", "ColorMode") == 3,
			 "options-menu-color-mode");
		if (QAction *back = options ? findAction(options, txt("Display.Color.StripTint")) : nullptr)
			back->trigger();
		r->check(delegate->colorMode == 0, "options-menu-color-mode-back");
	});

	/* 9. Spacer. */
	step(300, [this, r] {
		tree->setCurrentItem(nullptr);
		newSpacer();
		r->check(currentJson().contains("\"style\":\"spacer\""), "spacer-saved");
	});

	/* 10, 11. Color modes and visibility options: render each for review. */
	for (int mode = 0; mode < 4; mode++) {
		step(250, [this, mode, shots] {
			delegate->colorMode = mode;
			delegate->showCounts = true;
			tree->viewport()->update();
			if (!shots.isEmpty())
				QTimer::singleShot(150, this, [this, mode, shots] {
					grab().save(QStringLiteral("%1/mode-%2.png").arg(shots).arg(mode));
				});
		});
	}
	step(400, [this, r, shots] {
		delegate->colorMode = 0;
		delegate->showIcons = false;
		delegate->showBadges = false;
		tree->viewport()->update();
		if (!shots.isEmpty())
			grab().save(QStringLiteral("%1/no-icons-no-badges.png").arg(shots));
		delegate->showIcons = true;
		delegate->showBadges = true;
		r->check(true, "display-modes-rendered");
	});

	step(200, [r] {
		obs_log(LOG_INFO, "[selftest] DONE pass=%d fail=%d", r->pass, r->fail);
		r->autoAccept->stop();
	});

	std::shared_ptr<std::function<void()>> runNext = std::make_shared<std::function<void()>>();
	*runNext = [this, r, runNext] {
		if (r->next >= r->steps.size())
			return;
		const int delay = r->steps[r->next].first;
		std::function<void()> fn = r->steps[r->next].second;
		r->next++;
		QTimer::singleShot(delay, this, [fn, runNext] {
			fn();
			(*runNext)();
		});
	};
	(*runNext)();
}

} // namespace stacks
