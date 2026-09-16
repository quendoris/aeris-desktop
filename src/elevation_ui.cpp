// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "elevation_ui.hpp"

#include "main_window.hpp"

#include <QAction>
#include <QMenu>
#include <QMenuBar>

namespace aeris::desktop {

void install_elevation_import_action(MainWindow& window) {
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

    auto* action = data_menu->addAction(
        QStringLiteral("Add NOAA ETOPO 2022 elevation…")
    );
    action->setObjectName(QStringLiteral("importEtopo2022ElevationAction"));
    action->setToolTip(QStringLiteral(
        "Download an official NOAA/NCEI ETOPO 2022 global 60 arc-second Ice Surface or Bedrock dataset, or import an already acquired GeoTIFF for offline use"
    ));
    QObject::connect(
        action,
        &QAction::triggered,
        &window,
        [&window]() { window.import_etopo_elevation(); }
    );
}

}  // namespace aeris::desktop
