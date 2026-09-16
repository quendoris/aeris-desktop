// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "etopo_acquisition.hpp"

#include "elevation_tiff.hpp"

#include <QByteArray>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSaveFile>
#include <QString>
#include <QUrl>
#include <QVariant>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <system_error>

namespace aeris::desktop {
namespace {

constexpr std::uint32_t kWidth = 21600U;
constexpr std::uint32_t kHeight = 10800U;

constexpr Etopo2022SourceDescriptor kSurface{
    Etopo2022Variant::ice_surface,
    "surface",
    "Ice Surface · land, bathymetry and Greenland/Antarctic ice surface",
    "ETOPO_2022_v1_60s_N90W180_surface.tif",
    "https://www.ngdc.noaa.gov/mgg/global/relief/ETOPO2022/data/60s/"
    "60s_surface_elev_gtif/ETOPO_2022_v1_60s_N90W180_surface.tif",
};

constexpr Etopo2022SourceDescriptor kBedrock{
    Etopo2022Variant::bedrock,
    "bed",
    "Bedrock · land, bathymetry and bedrock below major ice sheets",
    "ETOPO_2022_v1_60s_N90W180_bed.tif",
    "https://www.ngdc.noaa.gov/mgg/global/relief/ETOPO2022/data/60s/"
    "60s_bed_elev_gtif/ETOPO_2022_v1_60s_N90W180_bed.tif",
};

[[nodiscard]] QString path_to_qt(const std::filesystem::path& path) {
    const std::string utf8 = path.generic_u8string();
    return QDir::fromNativeSeparators(
        QString::fromUtf8(utf8.data(), static_cast<int>(utf8.size()))
    );
}

[[nodiscard]] bool ensure_directory(
    const std::filesystem::path& root,
    std::string& diagnostic
) {
    std::error_code error;
    std::filesystem::create_directories(root, error);
    if (error) {
        diagnostic = "could not create ETOPO acquisition cache: " + error.message();
        return false;
    }
    if (!std::filesystem::is_directory(root, error) || error) {
        diagnostic = "ETOPO acquisition cache is not a directory";
        return false;
    }
    return true;
}

[[nodiscard]] bool valid_etopo_file(
    const std::filesystem::path& path,
    std::string& diagnostic
) {
    const Float32TiffInspectResult inspected = inspect_single_band_float32_tiff(path);
    if (!inspected.ok()) {
        diagnostic = inspected.diagnostic.empty()
            ? "downloaded ETOPO file is not a supported single-band Float32 TIFF"
            : inspected.diagnostic;
        return false;
    }
    if (inspected.info.width != kWidth || inspected.info.height != kHeight) {
        diagnostic = "downloaded ETOPO file has an unexpected global grid: " +
            std::to_string(inspected.info.width) + "x" +
            std::to_string(inspected.info.height);
        return false;
    }
    return true;
}

[[nodiscard]] bool content_range_starts_at(
    const QByteArray& header,
    const qint64 expected_start
) {
    constexpr auto prefix = "bytes ";
    if (!header.startsWith(prefix)) return false;
    const qsizetype first = static_cast<qsizetype>(sizeof(prefix) - 1U);
    const qsizetype dash = header.indexOf('-', first);
    if (dash <= first) return false;
    bool ok = false;
    const qlonglong start = header.mid(first, dash - first).toLongLong(&ok);
    return ok && start == expected_start;
}

[[nodiscard]] QByteArray response_if_range_validator(QNetworkReply& reply) {
    const QByteArray etag = reply.rawHeader(QByteArrayLiteral("ETag")).trimmed();
    if (!etag.isEmpty() && !etag.startsWith(QByteArrayLiteral("W/"))) {
        return etag;
    }
    return reply.rawHeader(QByteArrayLiteral("Last-Modified")).trimmed();
}

[[nodiscard]] QByteArray load_if_range_validator(
    const std::filesystem::path& validator_path
) {
    QFile file(path_to_qt(validator_path));
    if (!file.open(QIODevice::ReadOnly)) return {};
    const QByteArray encoded = file.readAll().trimmed();
    if (encoded.isEmpty()) return {};
    return QByteArray::fromBase64(encoded);
}

void save_if_range_validator(
    QNetworkReply& reply,
    const std::filesystem::path& validator_path
) {
    const QByteArray validator = response_if_range_validator(reply);
    if (validator.isEmpty()) {
        QFile::remove(path_to_qt(validator_path));
        return;
    }

    QSaveFile file(path_to_qt(validator_path));
    if (!file.open(QIODevice::WriteOnly)) return;
    if (file.write(validator.toBase64()) < 0) {
        file.cancelWriting();
        return;
    }
    file.commit();
}

[[nodiscard]] bool publish_part(
    const std::filesystem::path& part,
    const std::filesystem::path& validator_path,
    const std::filesystem::path& target,
    std::string& diagnostic
) {
    std::string validation;
    if (!valid_etopo_file(part, validation)) {
        QFile::remove(path_to_qt(part));
        QFile::remove(path_to_qt(validator_path));
        diagnostic = "downloaded ETOPO GeoTIFF failed structural validation: " + validation;
        return false;
    }

    QFile::remove(path_to_qt(target));
    if (!QFile::rename(path_to_qt(part), path_to_qt(target))) {
        diagnostic = "could not publish verified ETOPO GeoTIFF into the acquisition cache";
        return false;
    }
    QFile::remove(path_to_qt(validator_path));
    return true;
}

}  // namespace

Etopo2022SourceDescriptor etopo2022_source_descriptor(
    const Etopo2022Variant variant
) noexcept {
    return variant == Etopo2022Variant::bedrock ? kBedrock : kSurface;
}

Etopo2022AcquisitionResult acquire_etopo2022_global_60s(
    const Etopo2022Variant variant,
    const std::filesystem::path& cache_root,
    const DataJobProgressCallback& progress
) {
    if (cache_root.empty()) {
        return {false, false, {}, "ETOPO acquisition requires a cache directory"};
    }

    std::string diagnostic;
    if (!ensure_directory(cache_root, diagnostic)) {
        return {false, false, {}, std::move(diagnostic)};
    }

    const Etopo2022SourceDescriptor source = etopo2022_source_descriptor(variant);
    const std::filesystem::path target = cache_root / std::string(source.filename);

    if (std::filesystem::exists(target)) {
        std::string validation;
        if (valid_etopo_file(target, validation)) {
            report_data_job_progress(
                progress,
                1U,
                1U,
                std::string("ETOPO ") + std::string(source.id) + " · cached"
            );
            return {true, true, target, {}};
        }
        QFile::remove(path_to_qt(target));
    }

    const std::filesystem::path part = target.string() + ".part";
    const std::filesystem::path validator_path = part.string() + ".if-range";

    std::error_code size_error;
    std::uintmax_t partial_size = 0U;
    if (std::filesystem::exists(part, size_error) && !size_error) {
        partial_size = std::filesystem::file_size(part, size_error);
        if (size_error) partial_size = 0U;
    }
    if (partial_size > static_cast<std::uintmax_t>(std::numeric_limits<qint64>::max())) {
        QFile::remove(path_to_qt(part));
        QFile::remove(path_to_qt(validator_path));
        partial_size = 0U;
    }

    QByteArray if_range;
    if (partial_size > 0U) {
        if_range = load_if_range_validator(validator_path);
        if (if_range.isEmpty()) {
            // Cross-process resume without a representation validator could mix
            // bytes from two different remote objects. Restart instead.
            QFile::remove(path_to_qt(part));
            partial_size = 0U;
        }
    }

    QFile output(path_to_qt(part));
    if (!output.open(QIODevice::ReadWrite)) {
        return {
            false,
            false,
            target,
            "could not open resumable ETOPO download file",
        };
    }
    if (partial_size == 0U) {
        if (!output.resize(0)) {
            return {false, false, target, "could not reset resumable ETOPO download file"};
        }
        QFile::remove(path_to_qt(validator_path));
    }
    if (!output.seek(static_cast<qint64>(partial_size))) {
        return {false, false, target, "could not seek resumable ETOPO download file"};
    }

    QNetworkAccessManager network;
    QNetworkRequest request(QUrl(QString::fromUtf8(source.url.data(), static_cast<int>(source.url.size()))));
    request.setAttribute(
        QNetworkRequest::RedirectPolicyAttribute,
        QNetworkRequest::NoLessSafeRedirectPolicy
    );
    if (partial_size > 0U) {
        request.setRawHeader(
            QByteArrayLiteral("Range"),
            QByteArrayLiteral("bytes=") +
                QByteArray::number(static_cast<qulonglong>(partial_size)) +
                QByteArrayLiteral("-")
        );
        request.setRawHeader(QByteArrayLiteral("If-Range"), if_range);
    }

    QNetworkReply* reply = network.get(request);
    if (reply == nullptr) {
        return {false, false, target, "could not start ETOPO download"};
    }

    qint64 base_offset = static_cast<qint64>(partial_size);
    bool response_mode_ready = false;
    bool response_writable = false;
    bool response_failed = false;
    bool write_failed = false;
    std::string response_failure;

    const auto prepare_response_mode = [&]() {
        if (response_mode_ready || response_failed) return;
        const QVariant raw_status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
        if (!raw_status.isValid()) return;
        const int status = raw_status.toInt();

        if (base_offset > 0 && status == 206) {
            const QByteArray content_range = reply->rawHeader(QByteArrayLiteral("Content-Range"));
            if (!content_range_starts_at(content_range, base_offset)) {
                response_failed = true;
                response_failure =
                    "ETOPO resume response has an unexpected Content-Range; existing partial bytes were left untouched";
                return;
            }
            response_writable = true;
            response_mode_ready = true;
            return;
        }

        if (base_offset > 0 && status == 200) {
            // If-Range failed or the server ignored Range. A complete response
            // must replace, never append to, the previous partial prefix.
            if (!output.resize(0) || !output.seek(0)) {
                write_failed = true;
                return;
            }
            base_offset = 0;
            save_if_range_validator(*reply, validator_path);
            response_writable = true;
            response_mode_ready = true;
            return;
        }

        if (base_offset == 0 && status == 200) {
            save_if_range_validator(*reply, validator_path);
            response_writable = true;
        }
        response_mode_ready = true;
    };

    const auto drain = [&]() {
        prepare_response_mode();
        if (!response_mode_ready && !response_failed && !write_failed) return;
        const QByteArray chunk = reply->readAll();
        if (response_failed || write_failed || !response_writable || chunk.isEmpty()) return;
        if (output.write(chunk) != chunk.size()) write_failed = true;
    };

    QObject::connect(reply, &QNetworkReply::readyRead, reply, drain);
    QObject::connect(
        reply,
        &QNetworkReply::downloadProgress,
        reply,
        [&](const qint64 received, const qint64 total) {
            const std::uint64_t current = static_cast<std::uint64_t>(
                std::max<qint64>(0, base_offset + received)
            );
            const std::uint64_t expected = total > 0
                ? static_cast<std::uint64_t>(std::max<qint64>(0, base_offset + total))
                : 0U;
            report_data_job_progress(
                progress,
                current,
                expected,
                std::string("Downloading ETOPO 2022 · ") + std::string(source.id)
            );
        }
    );

    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    drain();
    output.flush();
    output.close();

    const QVariant raw_status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
    const int status = raw_status.isValid() ? raw_status.toInt() : 0;
    const QNetworkReply::NetworkError network_error = reply->error();
    const QString error_text = reply->errorString();
    reply->deleteLater();

    if (write_failed) {
        return {false, false, target, "could not write ETOPO partial download"};
    }
    if (response_failed) {
        return {false, false, target, std::move(response_failure)};
    }

    if (network_error != QNetworkReply::NoError) {
        if (status == 416 && !if_range.isEmpty()) {
            std::string validation;
            if (valid_etopo_file(part, validation) &&
                publish_part(part, validator_path, target, diagnostic)) {
                return {true, false, target, {}};
            }
        }
        return {
            false,
            false,
            target,
            "ETOPO download interrupted: " + error_text.toStdString() +
                " · partial bytes retained for safe resume",
        };
    }

    if (status < 200 || status >= 300) {
        return {
            false,
            false,
            target,
            "NOAA/NCEI returned HTTP " + std::to_string(status) +
                " · partial bytes retained for safe resume",
        };
    }

    if (!publish_part(part, validator_path, target, diagnostic)) {
        return {false, false, target, std::move(diagnostic)};
    }

    report_data_job_progress(
        progress,
        1U,
        1U,
        std::string("ETOPO ") + std::string(source.id) + " · acquired and verified"
    );
    return {true, false, target, {}};
}

}  // namespace aeris::desktop
