// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "data_job_process.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QProcess>
#include <QStringList>

#include <cstdint>
#include <utility>

namespace aeris::desktop {
namespace {

[[nodiscard]] QString path_to_qt(const std::filesystem::path& path) {
    const std::string utf8 = path.generic_u8string();
    return QDir::fromNativeSeparators(
        QString::fromUtf8(utf8.data(), static_cast<int>(utf8.size()))
    );
}

[[nodiscard]] QString worker_program_path() {
    QString executable = QStringLiteral("aeris-data-worker");
#ifdef Q_OS_WIN
    executable += QStringLiteral(".exe");
#endif
    return QDir(QCoreApplication::applicationDirPath()).filePath(executable);
}

[[nodiscard]] bool parse_unsigned_field(
    const QByteArray& line,
    const QByteArray& key,
    std::uint64_t& value
) {
    const qsizetype position = line.indexOf(key);
    if (position < 0) return false;
    const qsizetype first = position + key.size();
    qsizetype last = line.indexOf(' ', first);
    if (last < 0) last = line.size();
    bool ok = false;
    const qulonglong parsed = line.mid(first, last - first).toULongLong(&ok);
    if (!ok) return false;
    value = static_cast<std::uint64_t>(parsed);
    return true;
}

}  // namespace

DataJobProcess::DataJobProcess(QObject* parent)
    : QObject(parent),
      process_(new QProcess(this)) {
    process_->setProcessChannelMode(QProcess::SeparateChannels);

    connect(process_, &QProcess::readyReadStandardOutput, this, [this]() {
        consume_stdout(false);
    });

    connect(
        process_,
        &QProcess::errorOccurred,
        this,
        [this](const QProcess::ProcessError error) {
            if (error != QProcess::FailedToStart || completed_) return;
            finish_once({
                false,
                false,
                false,
                "could not start aeris-data-worker: " +
                    process_->errorString().toStdString(),
            });
        }
    );

    connect(
        process_,
        qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
        this,
        [this](const int exit_code, const QProcess::ExitStatus exit_status) {
            if (completed_) return;
            consume_stdout(true);
            const QByteArray standard_error = process_->readAllStandardError();
            const bool changed = stdout_buffer_.contains("changed=1");

            if (cancel_requested_) {
                std::string diagnostic = standard_error.trimmed().toStdString();
                if (diagnostic.empty()) diagnostic = "data job cancelled";
                finish_once({false, changed, true, std::move(diagnostic)});
                return;
            }

            const bool protocol_ok =
                stdout_buffer_.contains("AERIS_DATA_JOB_OK") &&
                exit_status == QProcess::NormalExit &&
                exit_code == 0;
            if (!protocol_ok) {
                std::string diagnostic = standard_error.trimmed().toStdString();
                if (diagnostic.empty()) {
                    diagnostic = "aeris-data-worker exited without a successful result";
                }
                finish_once({false, changed, false, std::move(diagnostic)});
                return;
            }
            finish_once({true, changed, false, {}});
        }
    );
}

DataJobProcess::~DataJobProcess() {
    callback_ = {};
    progress_callback_ = {};
    completed_ = true;
    if (process_ != nullptr && process_->state() != QProcess::NotRunning) {
        // QProcess::kill maps to an unconditional process kill on supported
        // desktop platforms. No graceful worker shutdown is awaited here: the
        // GUI close contract takes precedence, while SQLite/storage recovery is
        // verified separately at the durable project boundary.
        process_->kill();
    }
}

bool DataJobProcess::start(
    std::string operation,
    const std::filesystem::path& project_path,
    const std::filesystem::path& source_path,
    std::string modified_utc,
    CompletionCallback callback
) {
    if (process_ == nullptr || process_->state() != QProcess::NotRunning || !callback) {
        return false;
    }

    completed_ = false;
    cancel_requested_ = false;
    stdout_buffer_.clear();
    stdout_line_buffer_.clear();
    callback_ = std::move(callback);
    process_->setProgram(worker_program_path());
    process_->setArguments({
        QString::fromStdString(std::move(operation)),
        path_to_qt(project_path),
        path_to_qt(source_path),
        QString::fromStdString(std::move(modified_utc)),
    });
    process_->start();
    return true;
}

void DataJobProcess::set_progress_callback(ProgressCallback callback) {
    progress_callback_ = std::move(callback);
}

void DataJobProcess::request_cancel() noexcept {
    if (completed_ || process_ == nullptr || process_->state() == QProcess::NotRunning) {
        return;
    }
    cancel_requested_ = true;
    process_->kill();
}

void DataJobProcess::cancel() noexcept {
    callback_ = {};
    progress_callback_ = {};
    completed_ = true;
    cancel_requested_ = true;
    if (process_ != nullptr && process_->state() != QProcess::NotRunning) {
        process_->kill();
    }
}

bool DataJobProcess::running() const noexcept {
    return process_ != nullptr && process_->state() != QProcess::NotRunning;
}

void DataJobProcess::consume_stdout(const bool final_drain) {
    if (process_ == nullptr) return;
    const QByteArray chunk = process_->readAllStandardOutput();
    if (!chunk.isEmpty()) {
        stdout_buffer_.append(chunk);
        stdout_line_buffer_.append(chunk);
    }

    while (true) {
        const qsizetype newline = stdout_line_buffer_.indexOf('\n');
        if (newline < 0) break;
        const QByteArray line = stdout_line_buffer_.left(newline).trimmed();
        stdout_line_buffer_.remove(0, newline + 1);
        if (!line.startsWith("AERIS_DATA_JOB_PROGRESS")) continue;

        std::uint64_t current = 0U;
        std::uint64_t total = 0U;
        const bool has_current =
            parse_unsigned_field(line, QByteArrayLiteral("current="), current);
        const bool has_total =
            parse_unsigned_field(line, QByteArrayLiteral("total="), total);
        if (!has_current || !has_total) continue;

        const qsizetype phase_position = line.indexOf(QByteArrayLiteral(" phase="));
        std::string phase;
        if (phase_position >= 0) {
            phase = line.mid(phase_position + 7).trimmed().toStdString();
        }
        if (progress_callback_) {
            progress_callback_(DataJobProgress{current, total, std::move(phase)});
        }
    }

    if (final_drain && !stdout_line_buffer_.trimmed().isEmpty()) {
        // Preserve a final non-newline protocol fragment for completion parsing.
        // Progress lines are always flushed with a newline by the worker.
        stdout_line_buffer_.clear();
    }
}

void DataJobProcess::finish_once(DataJobResult result) {
    if (completed_) return;
    completed_ = true;
    CompletionCallback callback = std::move(callback_);
    callback_ = {};
    progress_callback_ = {};
    if (callback) callback(std::move(result));
}

}  // namespace aeris::desktop
