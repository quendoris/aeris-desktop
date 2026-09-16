// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "elevation_ui.hpp"
#include "flag_ui.hpp"
#include "main_window.hpp"

#include <QApplication>

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    application.setApplicationName(QStringLiteral("AERIS Desktop"));
    application.setOrganizationName(QStringLiteral("quendoris"));

    aeris::desktop::MainWindow window;
    aeris::desktop::install_country_flag_import_action(window);
    aeris::desktop::install_elevation_import_action(window);
    window.open_startup_world();
    window.show();
    return application.exec();
}
