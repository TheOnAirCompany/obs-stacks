#pragma once
#include <QMenu>
#include <QWidget>
#include <obs.h>

namespace stacks::actions {

// Appends the native-equivalent scene actions to menu for scene source `scene`
// (borrowed ref, not released). Lambdas hold weak references only.
void appendSceneActions(QMenu *menu, obs_source_t *scene, QWidget *parent);

} // namespace stacks::actions
