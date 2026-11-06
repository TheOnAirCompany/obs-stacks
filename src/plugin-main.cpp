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

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>

#include <QDockWidget>
#include <QMainWindow>
#include <QMetaObject>
#include <QPointer>
#include <QThread>

#include <functional>

#include "stacks-dock.hpp"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
	return obs_module_text("Description");
}

MODULE_EXPORT const char *obs_module_name(void)
{
	return "Stacks for OBS";
}

namespace {

constexpr const char *kDockId = "obs-stacks-dock";
QPointer<stacks::StacksDock> g_dock;

void onFrontendEvent(enum obs_frontend_event event, void *)
{
	if (g_dock)
		g_dock->handleFrontendEvent(event);
}

void onSave(obs_data_t *data, bool saving, void *)
{
	if (!g_dock)
		return;
	if (saving)
		g_dock->saveTo(data);
	else
		g_dock->loadFrom(data);
}

/* Global signals fire on any thread: hop to the UI thread. */
void onSourceSignal(void *, calldata_t *cd)
{
	obs_source_t *source = static_cast<obs_source_t *>(calldata_ptr(cd, "source"));
	if (!source || !obs_source_is_scene(source))
		return;
	if (g_dock)
		QMetaObject::invokeMethod(g_dock.data(), "reconcile", Qt::QueuedConnection);
}

/* Public API on the global proc handler (docs/INTEGRATION.md). Called on the UI thread
 * by other plugins; from another thread the call waits for the UI thread. */
void runOnUi(const std::function<void()> &fn)
{
	if (!g_dock)
		return;
	if (QThread::currentThread() == g_dock->thread())
		fn();
	else
		QMetaObject::invokeMethod(g_dock.data(), fn, Qt::BlockingQueuedConnection);
}

void procGetOrganization(void *, calldata_t *cd)
{
	QByteArray json;
	runOnUi([&json] {
		if (g_dock)
			json = g_dock->organizationJson().toUtf8();
	});
	calldata_set_string(cd, "json", json.constData());
}

void procPlaceScene(void *, calldata_t *cd)
{
	const char *uuid = calldata_string(cd, "uuid");
	const char *stack = calldata_string(cd, "stack");
	bool ok = false;
	const QString u = QString::fromUtf8(uuid ? uuid : "");
	const QString st = QString::fromUtf8(stack ? stack : "");
	runOnUi([&] {
		if (g_dock)
			ok = g_dock->placeScene(u, st);
	});
	calldata_set_bool(cd, "ok", ok);
}

} // namespace

bool obs_module_load(void)
{
	auto *main = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	g_dock = new stacks::StacksDock(main);

	if (!obs_frontend_add_dock_by_id(kDockId, obs_module_text("Dock.Title"), g_dock.data())) {
		obs_log(LOG_ERROR, "could not register the Stacks dock");
		delete g_dock.data();
		return false;
	}

	/* Like OBS's own Scenes dock: no close button, so it cannot be closed by
	 * accident. OBS resets plugin dock features when docks are (un)locked. */
	if (auto *dock = qobject_cast<QDockWidget *>(g_dock->parentWidget())) {
		auto stripClose = [dock] {
			if (dock->features() & QDockWidget::DockWidgetClosable)
				dock->setFeatures(dock->features() & ~QDockWidget::DockWidgetClosable);
		};
		stripClose();
		QObject::connect(dock, &QDockWidget::featuresChanged, dock, stripClose);
	}

	obs_frontend_add_event_callback(onFrontendEvent, nullptr);
	obs_frontend_add_save_callback(onSave, nullptr);

	signal_handler_add(obs_get_signal_handler(), "void stacks_changed()");
	proc_handler_t *ph = obs_get_proc_handler();
	proc_handler_add(ph, "void stacks_get_organization(out string json)", procGetOrganization, nullptr);
	proc_handler_add(ph, "void stacks_place_scene(in string uuid, in string stack, out bool ok)", procPlaceScene,
			 nullptr);

	signal_handler_connect(obs_get_signal_handler(), "source_rename", onSourceSignal, nullptr);

	obs_log(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	signal_handler_disconnect(obs_get_signal_handler(), "source_rename", onSourceSignal, nullptr);
	obs_frontend_remove_save_callback(onSave, nullptr);
	obs_frontend_remove_event_callback(onFrontendEvent, nullptr);
	obs_log(LOG_INFO, "plugin unloaded");
}
