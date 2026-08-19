// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "map_view.hpp"
#include "project_model.hpp"
#include "scene_controller.hpp"

#include "aeris/storage/project.hpp"
#include "aeris/view/scene.hpp"

#include <QApplication>
#include <QImage>
#include <QPainter>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <utility>

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: aeris-desktop-render-probe <project.aeris> <output.png>\n";
        return EXIT_FAILURE;
    }

    QApplication application(argc, argv);
    auto opened = aeris::storage::ProjectStore::open(std::filesystem::path(argv[1]));
    if (!opened.ok()) {
        std::cerr << "project open failed: " << opened.status.diagnostic << '\n';
        return EXIT_FAILURE;
    }

    auto model_result = aeris::desktop::load_project_model(*opened.store);
    if (!model_result.ok()) {
        std::cerr << "project model failed: " << model_result.diagnostic << '\n';
        return EXIT_FAILURE;
    }

    aeris::desktop::RenderFrame frame{};
    frame.request.mode = aeris::view::SurfaceMode::globe;
    frame.request.quality = aeris::view::SceneQuality::verified;
    frame.request.camera_longitude_deg = 15.0;
    frame.request.camera_latitude_deg = 20.0;
    for (const auto& entry : model_result.model->sources) {
        auto scene = aeris::view::build_scene_geometry(*entry.second, frame.request);
        if (!scene.ok) {
            std::cerr << "scene failed for " << entry.first << ": " << scene.diagnostic << '\n';
            return EXIT_FAILURE;
        }
        frame.source_scenes.emplace(entry.first, std::move(scene));
    }

    aeris::desktop::MapView view;
    view.resize(1280, 820);
    view.set_project(
        model_result.model,
        opened.store->metadata().project_uuid,
        opened.store->metadata().revision
    );
    view.set_frame(std::move(frame));
    view.show();
    application.processEvents();

    QImage image(view.size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    view.render(&painter);
    painter.end();

    if (!image.save(QString::fromLocal8Bit(argv[2]), "PNG")) {
        std::cerr << "unable to save output PNG\n";
        return EXIT_FAILURE;
    }

    std::cout << "aeris_desktop_render_probe: PASS " << argv[2] << '\n';
    return EXIT_SUCCESS;
}
