// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "flag_ui.hpp"

#include "main_window.hpp"

#include <QAction>
#include <QMenu>
#include <QMenuBar>

namespace aeris::desktop {

void install_country_flag_import_action(MainWindow& window) {
    QMenu* data_menu = nullptr;
    for (QAction* action : window.menuBar()->actions()) {
        if (action == nullptr || action->menu() == nullptr) continue;
        QString name = action->text();
        name.remove(QLatin1Char('&'));
        if (name.compare(QStringLiteral("Data"), Qt::CaseInsensitive) == 0) {
            data_menu = action->menu();
            break;
        }
    }
    if (data_menu == nullptr) {
        data_menu = window.menuBar()->addMenu(QStringLiteral("&Data"));
    }

    auto* action = data_menu->addAction(QStringLiteral("Import downloaded country flags…"));
    action->setObjectName(QStringLiteral("importCountryFlagsAction"));
    action->setToolTip(QStringLiteral(
        "Embed an optional ISO-3166 alpha-2 PNG flag pack into the current .aeris project"
    ));
    QObject::connect(
        action,
        &QAction::triggered,
        &window,
        [&window]() { window.import_country_flags(); }
    );
}

}  // namespace aeris::desktop
