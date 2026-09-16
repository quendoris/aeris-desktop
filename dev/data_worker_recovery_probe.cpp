// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "world_data_import.hpp"

#include "aeris/storage/layer.hpp"
#include "aeris/storage/project.hpp"
#include "aeris/storage/resource.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QProcess>
#include <QThread>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

namespace {

constexpr std::string_view kTimestamp = "2026-09-16T16:00:00Z";

[[nodiscard]] QString path_to_qt(const std::filesystem::path& path) {
    const std::string utf8 = path.generic_u8string();
    return QDir::fromNativeSeparators(
        QString::fromUtf8(utf8.data(), static_cast<int>(utf8.size()))
    );
}

int fail(const int code, const std::string& diagnostic) {
    std::cerr << "aeris_desktop_data_worker_recovery_probe: FAIL "
              << diagnostic << '\n';
    return code;
}

[[nodiscard]] std::optional<std::size_t> durable_resource_count(
    const std::filesystem::path& project_path
) {
    auto opened = aeris::storage::ProjectStore::open(project_path);
    if (!opened.ok()) return std::nullopt;
    const auto listed = aeris::storage::list_project_resources(*opened.store);
    if (!listed.ok()) return std::nullopt;
    return listed.records.size();
}

[[nodiscard]] bool run_flags_worker(
    const std::filesystem::path& worker_path,
    const std::filesystem::path& project_path,
    const std::filesystem::path& flags_root,
    std::string& diagnostic
) {
    QProcess worker;
    worker.setProgram(path_to_qt(worker_path));
    worker.setArguments({
        QStringLiteral("flags"),
        path_to_qt(project_path),
        path_to_qt(flags_root),
        QString::fromLatin1(kTimestamp.data(), static_cast<int>(kTimestamp.size())),
    });
    worker.start();
    if (!worker.waitForStarted(3000)) {
        diagnostic = "recovery worker could not start: " + worker.errorString().toStdString();
        return false;
    }
    if (!worker.waitForFinished(30000)) {
        worker.kill();
        worker.waitForFinished(3000);
        diagnostic = "recovery worker did not finish within 30 seconds";
        return false;
    }
    if (worker.exitStatus() != QProcess::NormalExit || worker.exitCode() != 0) {
        diagnostic = worker.readAllStandardError().trimmed().toStdString();
        if (diagnostic.empty()) {
            diagnostic = "recovery worker exited without success";
        }
        return false;
    }
    if (!worker.readAllStandardOutput().contains("AERIS_DATA_JOB_OK")) {
        diagnostic = "recovery worker did not emit the success protocol";
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    Q_UNUSED(application);

    if (argc != 5) {
        return fail(
            2,
            "usage: <aeris-data-worker> <natural-earth-root> <flags-root> <output.aeris>"
        );
    }

    const std::filesystem::path worker_path = argv[1];
    const std::filesystem::path world_root = argv[2];
    const std::filesystem::path flags_root = argv[3];
    const std::filesystem::path project_path = argv[4];

    std::error_code remove_error;
    std::filesystem::remove(project_path, remove_error);

    aeris::storage::ProjectCreateOptions options{};
    options.timestamp_utc = std::string(kTimestamp);
    options.producer = "aeris-desktop-recovery-probe";
    options.producer_version = "0.1.0";
    auto created = aeris::storage::ProjectStore::create(project_path, options);
    if (!created.ok()) {
        return fail(3, "could not create recovery project: " + created.status.diagnostic);
    }

    const auto world = aeris::desktop::import_natural_earth_110m_world(
        *created.store,
        world_root,
        kTimestamp
    );
    if (!world.ok()) {
        return fail(4, "could not seed base world: " + world.diagnostic);
    }
    const auto before_resources = aeris::storage::list_project_resources(*created.store);
    if (!before_resources.ok() || !before_resources.records.empty()) {
        return fail(5, "base world unexpectedly owns embedded resources before flag test");
    }
    created.store.reset();

    QProcess interrupted;
    interrupted.setProgram(path_to_qt(worker_path));
    interrupted.setArguments({
        QStringLiteral("flags"),
        path_to_qt(project_path),
        path_to_qt(flags_root),
        QString::fromLatin1(kTimestamp.data(), static_cast<int>(kTimestamp.size())),
    });
    interrupted.start();
    if (!interrupted.waitForStarted(3000)) {
        return fail(6, "interrupted worker could not start");
    }

    // Do not merely kill at an arbitrary time. Wait until another connection can
    // observe at least one committed embedded flag resource while the worker is
    // still alive, proving the kill occurs after a real partial mutation.
    std::size_t partial_resources = 0U;
    bool observed_partial_commit = false;
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 5000 && interrupted.state() != QProcess::NotRunning) {
        const auto count = durable_resource_count(project_path);
        if (count.has_value() && *count > 0U) {
            partial_resources = *count;
            observed_partial_commit = true;
            break;
        }
        QThread::msleep(5);
    }
    if (!observed_partial_commit || interrupted.state() == QProcess::NotRunning) {
        interrupted.kill();
        interrupted.waitForFinished(3000);
        return fail(7, "could not observe a committed partial flag import before worker exit");
    }

    interrupted.kill();
    if (!interrupted.waitForFinished(3000)) {
        return fail(8, "killed data worker did not terminate promptly");
    }

    auto recovered = aeris::storage::ProjectStore::open(project_path);
    if (!recovered.ok()) {
        return fail(9, "project could not reopen after worker kill: " + recovered.status.diagnostic);
    }
    const auto recovered_integrity = recovered.store->verify_integrity();
    if (!recovered_integrity.ok()) {
        return fail(
            10,
            "project integrity failed after worker kill: " + recovered_integrity.diagnostic
        );
    }
    const auto interrupted_resources =
        aeris::storage::list_project_resources(*recovered.store);
    if (!interrupted_resources.ok() || interrupted_resources.records.empty()) {
        return fail(11, "partial durable resources vanished after reopening killed import");
    }
    partial_resources = interrupted_resources.records.size();
    recovered.store.reset();

    std::string retry_diagnostic;
    if (!run_flags_worker(worker_path, project_path, flags_root, retry_diagnostic)) {
        return fail(12, "idempotent retry failed: " + retry_diagnostic);
    }

    auto final_project = aeris::storage::ProjectStore::open(project_path);
    if (!final_project.ok()) {
        return fail(13, "final project could not reopen: " + final_project.status.diagnostic);
    }
    const auto final_integrity = final_project.store->verify_integrity();
    if (!final_integrity.ok()) {
        return fail(14, "final project integrity failed: " + final_integrity.diagnostic);
    }

    const auto layers = aeris::storage::list_project_layers(*final_project.store);
    const auto resources = aeris::storage::list_project_resources(*final_project.store);
    if (!layers.ok() || !resources.ok()) {
        return fail(15, "could not enumerate recovered final project");
    }
    const auto flag_layer = std::find_if(
        layers.records.begin(),
        layers.records.end(),
        [](const aeris::storage::ProjectLayerRecord& layer) {
            return layer.role_id == aeris::storage::kLayerRoleCountryFlagV1;
        }
    );
    if (flag_layer == layers.records.end() ||
        flag_layer->resources.size() < 150U ||
        resources.records.size() != flag_layer->resources.size()) {
        return fail(16, "retry did not complete one coherent country-flag layer");
    }

    std::cout
        << "aeris_desktop_data_worker_recovery_probe: PASS"
        << " partial_resources=" << partial_resources
        << " final_flags=" << flag_layer->resources.size()
        << " kill_reopen=yes integrity=yes retry=yes\n";
    return EXIT_SUCCESS;
}
