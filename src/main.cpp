// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "elevation_ui.hpp"
#include "flag_ui.hpp"
#include "main_window.hpp"

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QFile>
#include <QTimer>

#include <filesystem>

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    application.setApplicationName(QStringLiteral("AERIS Desktop"));
    application.setOrganizationName(QStringLiteral("quendoris"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("AERIS durable map desktop"));
    parser.addHelpOption();
    parser.addPositionalArgument(
        QStringLiteral("project"),
        QStringLiteral("Open this .aeris project directly."),
        QStringLiteral("[project.aeris]")
    );
    const QCommandLineOption quit_after_option(
        QStringLiteral("quit-after-ms"),
        QStringLiteral(
            "Close the top-level window after the given delay. This is intended for end-to-end shutdown diagnostics."
        ),
        QStringLiteral("milliseconds")
    );
    parser.addOption(quit_after_option);
    const QCommandLineOption capture_ui_option(
        QStringLiteral("capture-ui"),
        QStringLiteral(
            "Capture the complete AERIS MainWindow into a PNG after the UI has settled, then exit. Intended for visual CI evidence."
        ),
        QStringLiteral("png")
    );
    const QCommandLineOption capture_after_option(
        QStringLiteral("capture-after-ms"),
        QStringLiteral(
            "Delay before --capture-ui grabs the MainWindow."
        ),
        QStringLiteral("milliseconds"),
        QStringLiteral("1500")
    );
    parser.addOption(capture_ui_option);
    parser.addOption(capture_after_option);
    parser.process(application);

    const QStringList positional = parser.positionalArguments();
    if (positional.size() > 1) parser.showHelp(2);

    aeris::desktop::MainWindow window;
    aeris::desktop::install_country_flag_import_action(window);
    aeris::desktop::install_elevation_import_action(window);
    window.install_layer_visibility_coordinator();

    if (positional.isEmpty()) {
        window.open_startup_world();
    } else {
        const std::filesystem::path project_path =
            QFile(positional.front()).filesystemFileName();
        if (!window.open_project_path(project_path)) return 2;
    }

    window.show();

    if (parser.isSet(capture_ui_option)) {
        if (parser.isSet(quit_after_option)) return 2;
        bool ok = false;
        const int delay_ms = parser.value(capture_after_option).toInt(&ok);
        const QString output_path = parser.value(capture_ui_option);
        if (!ok || delay_ms < 0 || output_path.isEmpty()) return 2;
        QTimer::singleShot(
            delay_ms,
            &window,
            [&application, &window, output_path]() {
                const auto pixmap = window.grab();
                if (pixmap.isNull() || !pixmap.save(output_path, "PNG")) {
                    application.exit(3);
                    return;
                }
                window.close();
            }
        );
    }

    if (parser.isSet(quit_after_option)) {
        bool ok = false;
        const int delay_ms = parser.value(quit_after_option).toInt(&ok);
        if (!ok || delay_ms < 0) return 2;
        QTimer::singleShot(delay_ms, &window, &QWidget::close);
    }

    return application.exec();
}
