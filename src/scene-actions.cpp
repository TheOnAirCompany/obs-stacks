#include "scene-actions.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <QAction>
#include <QGuiApplication>
#include <QInputDialog>
#include <QMessageBox>
#include <QPointer>
#include <QScreen>
#include <QString>

#include <functional>
#include <string>
#include <vector>

namespace stacks::actions {

namespace {

QString tr(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

// Runs fn with a strong ref to the scene (if it still exists).
using SceneFn = std::function<void(obs_source_t *)>;

void withScene(obs_weak_source_t *weak, const SceneFn &fn)
{
	obs_source_t *src = obs_weak_source_get_source(weak);
	if (!src)
		return;
	fn(src);
	obs_source_release(src);
}

// Shared weak reference owned by the menu (released when the menu is destroyed).
obs_weak_source_t *makeWeak(QMenu *menu, obs_source_t *scene)
{
	obs_weak_source_t *weak = obs_source_get_weak_source(scene);
	QObject::connect(menu, &QObject::destroyed, [weak]() { obs_weak_source_release(weak); });
	return weak;
}

bool sceneNameExists(const QString &name)
{
	obs_source_t *s = obs_get_source_by_name(name.toUtf8().constData());
	if (!s)
		return false;
	obs_source_release(s);
	return true;
}

void duplicateScene(obs_weak_source_t *weak, QWidget *parent)
{
	QString base;
	withScene(weak, [&](obs_source_t *s) { base = QString::fromUtf8(obs_source_get_name(s)); });
	if (base.isEmpty())
		return;

	QString name = base + QStringLiteral(" 2");
	for (int i = 3; sceneNameExists(name); ++i)
		name = base + QStringLiteral(" %1").arg(i);

	while (true) {
		bool ok = false;
		name = QInputDialog::getText(parent, tr("SceneAction.Duplicate.Title"),
					     tr("SceneAction.Duplicate.Prompt"), QLineEdit::Normal, name, &ok)
			       .trimmed();
		if (!ok || name.isEmpty())
			return;
		if (!sceneNameExists(name))
			break;
		QMessageBox::warning(parent, tr("SceneAction.Duplicate.Title"), tr("SceneAction.Duplicate.Exists"));
	}

	withScene(weak, [&](obs_source_t *s) {
		obs_scene_t *scene = obs_scene_from_source(s);
		if (!scene)
			return;
		obs_scene_t *copy = obs_scene_duplicate(scene, name.toUtf8().constData(), OBS_SCENE_DUP_REFS);
		if (copy)
			obs_scene_release(copy);
	});
}

void addProjector(QMenu *menu, obs_weak_source_t *weak)
{
	QMenu *sub = menu->addMenu(tr("SceneAction.Projector"));
	auto open = [weak](int monitor) {
		withScene(weak, [&](obs_source_t *s) {
			obs_frontend_open_projector("Scene", monitor, "", obs_source_get_name(s));
		});
	};
	QObject::connect(sub->addAction(tr("SceneAction.Projector.Windowed")), &QAction::triggered, sub,
			 [open]() { open(-1); });
	const auto screens = QGuiApplication::screens();
	for (int i = 0; i < screens.size(); ++i) {
		QScreen *sc = screens[i];
		QString label = QStringLiteral("%1: %2 (%3x%4)")
					.arg(i + 1)
					.arg(sc->name())
					.arg(sc->size().width())
					.arg(sc->size().height());
		QObject::connect(sub->addAction(label), &QAction::triggered, sub, [open, i]() { open(i); });
	}
}

void addTransitionOverride(QMenu *menu, obs_source_t *scene, obs_weak_source_t *weak, QWidget *parent)
{
	QMenu *sub = menu->addMenu(tr("SceneAction.TransitionOverride"));

	obs_data_t *data = obs_source_get_private_settings(scene);
	obs_data_set_default_int(data, "transition_duration", 300);
	std::string cur = obs_data_get_string(data, "transition");
	int curDuration = (int)obs_data_get_int(data, "transition_duration");
	obs_data_release(data);

	auto setTransition = [weak](std::string name) {
		withScene(weak, [&](obs_source_t *s) {
			obs_data_t *d = obs_source_get_private_settings(s);
			obs_data_set_string(d, "transition", name.c_str());
			obs_data_release(d);
		});
	};

	QAction *none = sub->addAction(tr("SceneAction.UseCurrentTransition"));
	none->setCheckable(true);
	none->setChecked(cur.empty());
	QObject::connect(none, &QAction::triggered, sub, [setTransition]() { setTransition(""); });

	struct obs_frontend_source_list list = {};
	obs_frontend_get_transitions(&list);
	for (size_t i = 0; i < list.sources.num; i++) {
		const char *n = obs_source_get_name(list.sources.array[i]);
		if (!n)
			continue;
		std::string name = n;
		QAction *a = sub->addAction(QString::fromUtf8(n));
		a->setCheckable(true);
		a->setChecked(name == cur);
		QObject::connect(a, &QAction::triggered, sub, [setTransition, name]() { setTransition(name); });
	}
	obs_frontend_source_list_free(&list);

	sub->addSeparator();
	QAction *dur = sub->addAction(tr("SceneAction.TransitionDuration"));
	QObject::connect(dur, &QAction::triggered, sub, [weak, parent, curDuration]() {
		bool ok = false;
		int v = QInputDialog::getInt(parent, tr("SceneAction.TransitionDuration.Title"),
					     tr("SceneAction.TransitionDuration.Prompt"), curDuration, 50, 20000, 50,
					     &ok);
		if (!ok)
			return;
		withScene(weak, [&](obs_source_t *s) {
			obs_data_t *d = obs_source_get_private_settings(s);
			obs_data_set_int(d, "transition_duration", v);
			obs_data_release(d);
		});
	});
}

} // namespace

void appendSceneActions(QMenu *menu, obs_source_t *scene, QWidget *parent)
{
	if (!menu || !scene)
		return;
	obs_weak_source_t *weak = makeWeak(menu, scene);
	QPointer<QWidget> p(parent);

	QObject::connect(menu->addAction(tr("SceneAction.Duplicate")), &QAction::triggered, menu,
			 [weak, p]() { duplicateScene(weak, p); });

	QObject::connect(menu->addAction(tr("SceneAction.Filters")), &QAction::triggered, menu,
			 [weak]() { withScene(weak, [](obs_source_t *s) { obs_frontend_open_source_filters(s); }); });

	addProjector(menu, weak);

	QObject::connect(menu->addAction(tr("SceneAction.Screenshot")), &QAction::triggered, menu, [weak]() {
		withScene(weak, [](obs_source_t *s) { obs_frontend_take_source_screenshot(s); });
	});

	addTransitionOverride(menu, scene, weak, parent);

	QAction *mv = menu->addAction(tr("SceneAction.ShowInMultiview"));
	obs_data_t *data = obs_source_get_private_settings(scene);
	obs_data_set_default_bool(data, "show_in_multiview", true);
	bool show = obs_data_get_bool(data, "show_in_multiview");
	obs_data_release(data);
	mv->setCheckable(true);
	mv->setChecked(show);
	QObject::connect(mv, &QAction::triggered, menu, [weak](bool checked) {
		withScene(weak, [&](obs_source_t *s) {
			obs_data_t *d = obs_source_get_private_settings(s);
			obs_data_set_bool(d, "show_in_multiview", checked);
			obs_data_release(d);
		});
	});
}

} // namespace stacks::actions
