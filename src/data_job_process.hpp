// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include "data_job_progress.hpp"

#include <QByteArray>
#include <QObject>

#include <filesystem>
#include <functional>
#include <string>

class QProcess;

namespace aeris::desktop {

struct DataJobResult final {
    bool success{false};
    bool changed{false};
    bool cancelled{false};
    std::string diagnostic;

    [[nodiscard]] bool ok() const noexcept { return success; }
};

// Owns one isolated aeris-data-worker subprocess. Heavy import/acquisition code
// never runs in the GUI process through this bridge. Destroying the bridge uses
// hard cancellation, which lets window/application shutdown remain independent
// of a data job. Interactive Cancel keeps the completion callback so the UI can
// reconcile any already-committed project transactions after the child exits.
class DataJobProcess final : public QObject {
public:
    using CompletionCallback = std::function<void(DataJobResult)>;
    using ProgressCallback = DataJobProgressCallback;

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

    void set_progress_callback(ProgressCallback callback);

    // The worker project identity remains available while the subprocess is
    // alive. Presentation-only mutations can therefore avoid contending with a
    // writer that targets the same .aeris while still committing immediately to
    // a different project opened by the user.
    [[nodiscard]] const std::filesystem::path& project_path() const noexcept {
        return project_path_;
    }

    // User-visible cancel: kill immediately, but deliver one cancelled result
    // when QProcess reports termination so callers can refresh durable state.
    void request_cancel() noexcept;

    // Shutdown/destructor cancel: kill immediately and suppress callbacks.
    void cancel() noexcept;
    [[nodiscard]] bool running() const noexcept;

private:
    void consume_stdout(bool final_drain);
    void finish_once(DataJobResult result);

    QProcess* process_{nullptr};
    CompletionCallback callback_;
    ProgressCallback progress_callback_;
    std::filesystem::path project_path_;
    QByteArray stdout_buffer_;
    QByteArray stdout_line_buffer_;
    bool completed_{false};
    bool cancel_requested_{false};
};

}  // namespace aeris::desktop
