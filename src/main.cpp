// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "main_window.hpp"

#include <QApplication>

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    application.setApplicationName(QStringLiteral("AERIS Desktop"));
    application.setOrganizationName(QStringLiteral("quendoris"));

    aeris::desktop::MainWindow window;
    window.show();
    return application.exec();
}
