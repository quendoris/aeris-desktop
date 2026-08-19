// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <QPoint>
#include <QWidget>

#include <string>

namespace aeris::desktop {

class MapView final : public QWidget {
    Q_OBJECT

public:
    explicit MapView(QWidget* parent = nullptr);

    void set_project_summary(std::string project_uuid, std::uint64_t revision);
    void clear_project();

protected:
    void paintEvent(QPaintEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    bool has_project_{false};
    std::string project_uuid_;
    std::uint64_t revision_{0};
    double interaction_scale_{1.0};
    QPoint interaction_offset_{};
    QPoint last_mouse_{};
    bool dragging_{false};
};

}  // namespace aeris::desktop
