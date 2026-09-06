// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "data_job_process.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QProcess>
#include <QStringList>

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

}  // namespace

DataJobProcess::DataJobProcess(QObject* parent)
    : QObject(parent),
      process_(new QProcess(this)) {
    process_->setProcessChannelMode(QProcess::SeparateChannels);

    connect(
        process_,
        &QProcess::errorOccurred,
        this,
        [this](const QProcess::ProcessError error) {
            if (error != QProcess::FailedToStart || completed_) return;
            finish_once({
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
            const QByteArray standard_output = process_->readAllStandardOutput();
            const QByteArray standard_error = process_->readAllStandardError();
            const bool changed = standard_output.contains("changed=1");
            const bool protocol_ok =
                standard_output.contains("AERIS_DATA_JOB_OK") &&
                exit_status == QProcess::NormalExit &&
                exit_code == 0;
            if (!protocol_ok) {
                std::string diagnostic = standard_error.trimmed().toStdString();
                if (diagnostic.empty()) {
                    diagnostic = "aeris-data-worker exited without a successful result";
                }
                finish_once({false, changed, std::move(diagnostic)});
                return;
            }
            finish_once({true, changed, {}});
        }
    );
}

DataJobProcess::~DataJobProcess() {
    callback_ = {};
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

void DataJobProcess::cancel() noexcept {
    callback_ = {};
    completed_ = true;
    if (process_ != nullptr && process_->state() != QProcess::NotRunning) {
        process_->kill();
    }
}

bool DataJobProcess::running() const noexcept {
    return process_ != nullptr && process_->state() != QProcess::NotRunning;
}

void DataJobProcess::finish_once(DataJobResult result) {
    if (completed_) return;
    completed_ = true;
    CompletionCallback callback = std::move(callback_);
    callback_ = {};
    if (callback) callback(std::move(result));
}

}  // namespace aeris::desktop
