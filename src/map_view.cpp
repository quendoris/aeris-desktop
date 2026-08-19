// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "map_view.hpp"

#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>

#include <algorithm>
#include <utility>

namespace aeris::desktop {

MapView::MapView(QWidget* parent)
    : QWidget(parent) {
    setMinimumSize(720, 480);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
}

void MapView::set_project_summary(std::string project_uuid, const std::uint64_t revision) {
    has_project_ = true;
    project_uuid_ = std::move(project_uuid);
    revision_ = revision;
    interaction_scale_ = 1.0;
    interaction_offset_ = {};
    update();
}

void MapView::clear_project() {
    has_project_ = false;
    project_uuid_.clear();
    revision_ = 0;
    interaction_scale_ = 1.0;
    interaction_offset_ = {};
    update();
}

void MapView::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.fillRect(rect(), QColor(19, 21, 24));

    painter.setPen(QColor(224, 227, 231));
    const QRect content = rect().adjusted(48, 48, -48, -48);

    if (!has_project_) {
        painter.drawText(
            content,
            Qt::AlignCenter,
            QStringLiteral("Open an .aeris project to begin")
        );
        return;
    }

    painter.save();
    painter.translate(interaction_offset_);
    painter.scale(interaction_scale_, interaction_scale_);
    painter.setPen(QColor(66, 71, 79));
    painter.setBrush(QColor(25, 28, 32));
    const QRectF placeholder(
        static_cast<double>(width()) * 0.18,
        static_cast<double>(height()) * 0.18,
        static_cast<double>(width()) * 0.64,
        static_cast<double>(height()) * 0.64
    );
    painter.drawRoundedRect(placeholder, 18.0, 18.0);
    painter.restore();

    painter.setPen(QColor(224, 227, 231));
    painter.drawText(
        content,
        Qt::AlignCenter,
        QStringLiteral("Project accepted by aeris-core\nRenderer migration is the next slice")
    );

    painter.setPen(QColor(137, 143, 152));
    painter.drawText(
        QRect(24, height() - 42, width() - 48, 24),
        Qt::AlignLeft | Qt::AlignVCenter,
        QStringLiteral("%1 · revision %2")
            .arg(QString::fromStdString(project_uuid_))
            .arg(static_cast<qulonglong>(revision_))
    );
}

void MapView::wheelEvent(QWheelEvent* event) {
    const double factor = event->angleDelta().y() >= 0 ? 1.12 : 1.0 / 1.12;
    interaction_scale_ = std::clamp(interaction_scale_ * factor, 0.5, 8.0);
    update();
    event->accept();
}

void MapView::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    dragging_ = true;
    last_mouse_ = event->pos();
    setCursor(Qt::ClosedHandCursor);
    event->accept();
}

void MapView::mouseMoveEvent(QMouseEvent* event) {
    if (!dragging_) {
        QWidget::mouseMoveEvent(event);
        return;
    }
    interaction_offset_ += event->pos() - last_mouse_;
    last_mouse_ = event->pos();
    update();
    event->accept();
}

void MapView::mouseReleaseEvent(QMouseEvent* event) {
    if (!dragging_ || event->button() != Qt::LeftButton) {
        QWidget::mouseReleaseEvent(event);
        return;
    }
    dragging_ = false;
    unsetCursor();
    event->accept();
}

}  // namespace aeris::desktop
