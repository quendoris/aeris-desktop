// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <QObject>

#include <filesystem>
#include <functional>
#include <string>

class QProcess;

namespace aeris::desktop {

struct DataJobResult final {
    bool success{false};
    bool changed{false};
    std::string diagnostic;

    [[nodiscard]] bool ok() const noexcept { return success; }
};

// Owns one isolated aeris-data-worker subprocess. Heavy import code never runs
// in the GUI process through this bridge. Destroying the bridge kills the child,
// which lets window/application shutdown remain independent of an import job.
class DataJobProcess final : public QObject {
public:
    using CompletionCallback = std::function<void(DataJobResult)>;

    explicit DataJobProcess(QObject* parent = nullptr);
    ~DataJobProcess() override;

    DataJobProcess(const DataJobProcess&) = delete;
    DataJobProcess& operator=(const DataJobProcess&) = delete;

    [[nodiscard]] bool start(
        std::string operation,
        const std::filesystem::path& project_path,
        const std::filesystem::path& source_path,
        std::string modified_utc,
        CompletionCallback callback);

    void cancel() noexcept;
    [[nodiscard]] bool running() const noexcept;

private:
    void finish_once(DataJobResult result);

    QProcess* process_{nullptr};
    CompletionCallback callback_;
    bool completed_{false};
};

}  // namespace aeris::desktop
