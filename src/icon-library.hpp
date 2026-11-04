#pragma once

#include <QIcon>
#include <QColor>
#include <QString>
#include <QWidget>

namespace stacks::icons {

// Render a Lucide icon tinted with color (replaces currentColor in the SVG).
// Null QIcon if name is empty or unknown. Results are cached.
QIcon icon(const QString &name, const QColor &color, int size = 16);

// Modal picker with search (name and tags) and an icon grid.
// Returns the chosen name, "" for None, or a null QString() on Cancel.
QString pick(QWidget *parent, const QString &current, const QColor &color);

} // namespace stacks::icons
