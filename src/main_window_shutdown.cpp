// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "main_window.hpp"

#include "data_job_process.hpp"
#include "map_view.hpp"

#include <QCloseEvent>

namespace aeris::desktop {

void MainWindow::closeEvent(QCloseEvent* event) {
    // Close is a control boundary, not ordinary application work. Stop the
    // isolated data writer first so a long import can never make the GUI wait
    // for its completion. The worker owns its own ProjectStore handle; killing
    // it cannot leave an in-process callback holding GUI state alive.
    if (data_job_ != nullptr) {
        data_job_->cancel();
        data_job_ = nullptr;
    }

    // Prevent any new viewport/detail work from being queued as destruction
    // begins, then invalidate the scene generation. Both background systems are
    // cooperative and cancellation-aware; doing this at close-event delivery
    // keeps QObject teardown from becoming the first place cancellation happens.
    if (map_view_ != nullptr) {
        map_view_->prepare_shutdown();
    }
    scene_controller_.cancel();

    QMainWindow::closeEvent(event);
}

}  // namespace aeris::desktop
