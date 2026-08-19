// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "main_window.hpp"
#include "map_canvas.hpp"
#include "scene_builder.hpp"
#include "scene_presentation.hpp"
#include "unfold.hpp"
#include "world_loader.hpp"

#include "aeris/geo/wgs84.hpp"
#include "aeris/projection/wgs84.hpp"

#include <QApplication>
#include <QDateTime>
#include <QImage>
#include <QMessageBox>
#include <QPainter>
#include <QTimer>

#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>

namespace {

struct Arguments final {
    std::filesystem::path snapshot =
        std::filesystem::path("dev-data") / "natural-earth-v5.1.2";
    std::filesystem::path render_path;
    std::filesystem::path render_political_path;
    std::filesystem::path render_unfold_path;
    std::filesystem::path render_audit_path;
    bool smoke = false;
    bool render = false;
    bool render_political = false;
    bool render_unfold = false;
    bool render_audit = false;
    bool help = false;
    bool valid = true;
};

struct AuditCase final {
    const char* filename;
    aeris::viewer::MapContent content;
    aeris::viewer::ViewMode mode;
    double longitude_deg;
    double latitude_deg;
    double zoom;
};

constexpr std::array<AuditCase, 12> kAuditCases{{
    {"political-globe-world-1x.png", aeris::viewer::MapContent::political, aeris::viewer::ViewMode::globe, 15.0, 20.0, 1.0},
    {"political-globe-europe-2x.png", aeris::viewer::MapContent::political, aeris::viewer::ViewMode::globe, 15.0, 50.0, 2.0},
    {"political-globe-balkans-4x.png", aeris::viewer::MapContent::political, aeris::viewer::ViewMode::globe, 21.0, 43.0, 4.0},
    {"political-globe-benelux-8x.png", aeris::viewer::MapContent::political, aeris::viewer::ViewMode::globe, 4.7, 51.2, 8.0},
    {"political-mollweide-world-1x.png", aeris::viewer::MapContent::political, aeris::viewer::ViewMode::mollweide, 15.0, 20.0, 1.0},
    {"political-mollweide-europe-2x.png", aeris::viewer::MapContent::political, aeris::viewer::ViewMode::mollweide, 15.0, 50.0, 2.0},
    {"political-mollweide-balkans-4x.png", aeris::viewer::MapContent::political, aeris::viewer::ViewMode::mollweide, 21.0, 43.0, 4.0},
    {"political-mollweide-benelux-8x.png", aeris::viewer::MapContent::political, aeris::viewer::ViewMode::mollweide, 4.7, 51.2, 8.0},
    {"political-sinusoidal-world-1x.png", aeris::viewer::MapContent::political, aeris::viewer::ViewMode::sinusoidal, 15.0, 20.0, 1.0},
    {"physical-mollweide-indonesia-4x.png", aeris::viewer::MapContent::physical, aeris::viewer::ViewMode::mollweide, 118.0, -2.0, 4.0},
    {"physical-mollweide-antimeridian-4x.png", aeris::viewer::MapContent::physical, aeris::viewer::ViewMode::mollweide, 178.0, 60.0, 4.0},
    {"physical-mollweide-antarctica-2x.png", aeris::viewer::MapContent::physical, aeris::viewer::ViewMode::mollweide, 0.0, -82.0, 2.0},
}};

[[nodiscard]] Arguments parse_arguments(const int argc, char** argv) {
    Arguments arguments{};
    for (int index = 1; index < argc; ++index) {
        const std::string value(argv[index]);
        if (value == "--snapshot") {
            if (index + 1 >= argc) {
                arguments.valid = false;
                return arguments;
            }
            arguments.snapshot = argv[++index];
        } else if (value == "--smoke") {
            arguments.smoke = true;
        } else if (value == "--render") {
            if (index + 1 >= argc) {
                arguments.valid = false;
                return arguments;
            }
            arguments.render = true;
            arguments.render_path = argv[++index];
        } else if (value == "--render-political") {
            if (index + 1 >= argc) {
                arguments.valid = false;
                return arguments;
            }
            arguments.render_political = true;
            arguments.render_political_path = argv[++index];
        } else if (value == "--render-unfold") {
            if (index + 1 >= argc) {
                arguments.valid = false;
                return arguments;
            }
            arguments.render_unfold = true;
            arguments.render_unfold_path = argv[++index];
        } else if (value == "--render-audit") {
            if (index + 1 >= argc) {
                arguments.valid = false;
                return arguments;
            }
            arguments.render_audit = true;
            arguments.render_audit_path = argv[++index];
        } else if (value == "--help" || value == "-h") {
            arguments.help = true;
        } else {
            arguments.valid = false;
            return arguments;
        }
    }
    const int exclusive_modes =
        static_cast<int>(arguments.smoke) +
        static_cast<int>(arguments.render) +
        static_cast<int>(arguments.render_political) +
        static_cast<int>(arguments.render_unfold) +
        static_cast<int>(arguments.render_audit);
    if (exclusive_modes > 1) arguments.valid = false;
    return arguments;
}

void print_usage() {
    std::cout
        << "usage: aeris_viewer [--snapshot <directory>] "
           "[--smoke | --render <png> | --render-political <png> | "
           "--render-unfold <png> | --render-audit <directory>]\n"
        << "\n"
        << "The default snapshot directory is dev-data/natural-earth-v5.1.2.\n"
        << "Fetch the exact pinned physical + political demo bytes with:\n"
        << "  cmake -DDESTINATION=dev-data/natural-earth-v5.1.2 "
           "-P scripts/fetch_demo_world.cmake\n";
}

[[nodiscard]] bool save_window_png(
    QMainWindow& window,
    QApplication& application,
    const std::filesystem::path& output_path
) {
    window.resize(1280, 820);
    window.show();
    application.processEvents();

    QImage image(window.size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    window.render(&painter);
    painter.end();
    return image.save(QString::fromStdString(output_path.string()), "PNG");
}

[[nodiscard]] bool render_verified_workbench(
    QApplication& application,
    const aeris::viewer::WorkbenchWorlds& worlds,
    const aeris::viewer::MapContent content,
    const std::filesystem::path& output_path
) {
    const auto world = worlds.select(content);
    aeris::viewer::SceneRequest request{};
    request.mode = aeris::viewer::ViewMode::globe;
    request.quality = aeris::viewer::SceneQuality::verified;
    request.camera_longitude_deg = 15.0;
    request.camera_latitude_deg = 20.0;

    aeris::viewer::SceneData scene = aeris::viewer::build_scene(*world, request);
    aeris::viewer::apply_source_presentation(scene, *world);
    if (!scene.ok || scene.canceled) {
        std::cerr << "viewer render scene failed: " << scene.diagnostic << '\n';
        return false;
    }

    aeris::viewer::MainWindow window(
        worlds.physical, worlds.political, content, false
    );
    window.present_scene(std::move(scene));
    return save_window_png(window, application, output_path);
}

[[nodiscard]] bool render_unfold_workbench(
    QApplication& application,
    const aeris::viewer::WorkbenchWorlds& worlds,
    const std::filesystem::path& output_path
) {
    auto bundle = aeris::viewer::build_unfold_bundle(
        *worlds.physical,
        15.0,
        20.0,
        aeris::viewer::ViewMode::mollweide
    );
    if (!bundle.ok || bundle.canceled) {
        std::cerr << "viewer unfold render bundle failed: " << bundle.diagnostic << '\n';
        return false;
    }
    aeris::viewer::apply_source_presentation(bundle.globe_endpoint, *worlds.physical);
    aeris::viewer::apply_source_presentation(bundle.flat_endpoint, *worlds.physical);

    aeris::viewer::MainWindow window(
        worlds.physical, worlds.political, aeris::viewer::MapContent::physical, false
    );
    window.present_unfold_frame(std::move(bundle), 0.5);
    return save_window_png(window, application, output_path);
}

[[nodiscard]] std::optional<aeris::geometry::PlanarPoint> project_audit_center(
    const AuditCase& audit
) {
    if (audit.mode == aeris::viewer::ViewMode::globe) return std::nullopt;
    const auto primitive = audit.mode == aeris::viewer::ViewMode::sinusoidal
        ? aeris::projection::EqualAreaPrimitive::sinusoidal
        : aeris::projection::EqualAreaPrimitive::mollweide;
    const auto projected = aeris::projection::project_wgs84_primitive(
        audit.longitude_deg * aeris::geo::kPi / 180.0,
        audit.latitude_deg * aeris::geo::kPi / 180.0,
        primitive,
        0.0
    );
    if (!projected.ok()) return std::nullopt;
    return projected.value;
}

[[nodiscard]] bool render_audit_case(
    QApplication& application,
    const aeris::viewer::WorkbenchWorlds& worlds,
    const AuditCase& audit,
    const std::filesystem::path& output_path
) {
    const auto world = worlds.select(audit.content);
    aeris::viewer::SceneRequest request{};
    request.mode = audit.mode;
    request.quality = aeris::viewer::SceneQuality::verified;
    request.camera_longitude_deg = audit.longitude_deg;
    request.camera_latitude_deg = audit.latitude_deg;

    aeris::viewer::SceneData scene = aeris::viewer::build_scene(*world, request);
    aeris::viewer::apply_source_presentation(scene, *world);
    if (!scene.ok || scene.canceled) {
        std::cerr << "viewer audit scene failed for " << audit.filename
                  << ": " << scene.diagnostic << '\n';
        return false;
    }

    const auto flat_center = project_audit_center(audit);
    if (audit.mode != aeris::viewer::ViewMode::globe && !flat_center.has_value()) {
        std::cerr << "viewer audit could not project center for " << audit.filename << '\n';
        return false;
    }

    aeris::viewer::MainWindow window(
        worlds.physical, worlds.political, audit.content, false
    );
    window.present_scene(std::move(scene));
    window.resize(1280, 820);
    window.show();
    application.processEvents();

    auto* canvas = static_cast<aeris::viewer::MapCanvas*>(window.centralWidget());
    canvas->set_viewport(audit.zoom, flat_center);
    application.processEvents();
    return save_window_png(window, application, output_path);
}

[[nodiscard]] bool render_visual_audit(
    QApplication& application,
    const aeris::viewer::WorkbenchWorlds& worlds,
    const std::filesystem::path& output_directory
) {
    std::error_code directory_error{};
    std::filesystem::create_directories(output_directory, directory_error);
    if (directory_error) {
        std::cerr << "unable to create visual audit directory: "
                  << directory_error.message() << '\n';
        return false;
    }

    std::ofstream manifest(output_directory / "manifest.tsv", std::ios::trunc);
    if (!manifest) {
        std::cerr << "unable to create visual audit manifest\n";
        return false;
    }
    manifest << "filename\tcontent\tmode\tzoom\tlongitude_deg\tlatitude_deg\n";

    for (const AuditCase& audit : kAuditCases) {
        const std::filesystem::path output_path = output_directory / audit.filename;
        if (!render_audit_case(application, worlds, audit, output_path)) return false;
        manifest
            << audit.filename << '\t'
            << (audit.content == aeris::viewer::MapContent::political ? "political" : "physical") << '\t'
            << aeris::viewer::view_mode_name(audit.mode) << '\t'
            << audit.zoom << '\t'
            << audit.longitude_deg << '\t'
            << audit.latitude_deg << '\n';
    }

    manifest.flush();
    if (!manifest) {
        std::cerr << "unable to finalize visual audit manifest\n";
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    const Arguments arguments = parse_arguments(argc, argv);
    if (!arguments.valid) {
        print_usage();
        return EXIT_FAILURE;
    }
    if (arguments.help) {
        print_usage();
        return EXIT_SUCCESS;
    }

    QApplication application(argc, argv);
    application.setApplicationName(QStringLiteral("AERIS"));
    application.setOrganizationName(QStringLiteral("quendoris"));

    const std::string retrieved_at =
        QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs).toStdString();
    auto worlds = aeris::viewer::load_pinned_workbench_worlds(
        arguments.snapshot, retrieved_at
    );
    if (!worlds.ok()) {
        const QString message = QStringLiteral(
            "AERIS could not verify the pinned workbench sources at:\n%1\n\n%2\n\n"
            "Run the explicit demo-data fetch command shown by --help."
        )
            .arg(QString::fromStdString(arguments.snapshot.string()))
            .arg(QString::fromStdString(worlds.diagnostic));
        if (arguments.smoke || arguments.render ||
            arguments.render_political || arguments.render_unfold ||
            arguments.render_audit) {
            std::cerr << message.toStdString() << '\n';
        } else {
            QMessageBox::critical(nullptr, QStringLiteral("AERIS source verification"), message);
        }
        return EXIT_FAILURE;
    }

    if (arguments.render) {
        if (!render_verified_workbench(
                application, worlds, aeris::viewer::MapContent::physical,
                arguments.render_path)) {
            std::cerr << "unable to render physical viewer PNG\n";
            return EXIT_FAILURE;
        }
        std::cout << "viewer_render: PASS " << arguments.render_path.string() << '\n';
        return EXIT_SUCCESS;
    }

    if (arguments.render_political) {
        if (!render_verified_workbench(
                application, worlds, aeris::viewer::MapContent::political,
                arguments.render_political_path)) {
            std::cerr << "unable to render political viewer PNG\n";
            return EXIT_FAILURE;
        }
        std::cout << "viewer_political_render: PASS "
                  << arguments.render_political_path.string() << '\n';
        return EXIT_SUCCESS;
    }

    if (arguments.render_unfold) {
        if (!render_unfold_workbench(application, worlds, arguments.render_unfold_path)) {
            std::cerr << "unable to render unfold viewer PNG\n";
            return EXIT_FAILURE;
        }
        std::cout << "viewer_unfold_render: PASS "
                  << arguments.render_unfold_path.string() << '\n';
        return EXIT_SUCCESS;
    }

    if (arguments.render_audit) {
        if (!render_visual_audit(application, worlds, arguments.render_audit_path)) {
            std::cerr << "unable to render visual audit\n";
            return EXIT_FAILURE;
        }
        std::cout << "viewer_visual_audit: PASS "
                  << arguments.render_audit_path.string() << '\n';
        return EXIT_SUCCESS;
    }

    aeris::viewer::MainWindow window(
        worlds.physical,
        worlds.political,
        aeris::viewer::MapContent::physical,
        !arguments.smoke
    );
    window.show();

    if (arguments.smoke) {
        QTimer::singleShot(0, &application, &QCoreApplication::quit);
    }
    return application.exec();
}
