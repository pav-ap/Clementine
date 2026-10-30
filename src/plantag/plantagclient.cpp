/* This file is part of Clementine.
   Copyright 2026, Pav Stacey <pav-ap@users.noreply.github.com>

   Clementine is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   Clementine is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with Clementine.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "plantag/plantagclient.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QStringList>
#include <QUrl>

#include "core/logging.h"

namespace {

// Files are passed on the command line, so a very large selection would risk
// hitting the OS argument limit. 200 paths is far below it and keeps the
// failure mode obvious rather than mysterious.
constexpr int kMaxPathsPerInvocation = 200;

QString JsonString(const QJsonObject& object, const char* key) {
  return object.value(QLatin1String(key)).toString();
}

double JsonDouble(const QJsonObject& object, const char* key) {
  return object.value(QLatin1String(key)).toDouble(0.0);
}

int JsonInt(const QJsonObject& object, const char* key) {
  return object.value(QLatin1String(key)).toInt(0);
}

PlantagClient::Candidate ParseCandidate(const QJsonObject& object) {
  PlantagClient::Candidate candidate;
  candidate.spotify_id = JsonString(object, "spotify_id");
  candidate.reccobeats_id = JsonString(object, "reccobeats_id");
  candidate.isrc = JsonString(object, "isrc");
  candidate.title = JsonString(object, "title");
  candidate.album = JsonString(object, "album");
  candidate.cover_url = JsonString(object, "cover_url");
  candidate.tempo = JsonDouble(object, "tempo");
  candidate.confidence = JsonDouble(object, "confidence");
  candidate.duration_ms = JsonInt(object, "duration_ms");

  for (const QJsonValue& artist : object.value("artists").toArray()) {
    if (artist.isString()) candidate.artists << artist.toString();
  }
  return candidate;
}

}  // namespace

PlantagClient::PlantagClient(QObject* parent) : QObject(parent), process_(nullptr) {}

void PlantagClient::SetBinaryPath(const QString& path) { binary_path_ = path; }

QString PlantagClient::BinaryPath() const {
  if (!binary_path_.isEmpty()) return binary_path_;
  return QStandardPaths::findExecutable("plantag");
}

void PlantagClient::SetSpotifyCredentials(const QString& client_id,
                                          const QString& client_secret) {
  client_id_ = client_id;
  client_secret_ = client_secret;
}

bool PlantagClient::IsAvailable() const { return !BinaryPath().isEmpty(); }

bool PlantagClient::IsRunning() const {
  return process_ != nullptr && process_->state() != QProcess::NotRunning;
}

void PlantagClient::Start(const QStringList& arguments) {
  Cancel();

  process_ = new QProcess(this);
  process_->setProcessChannelMode(QProcess::SeparateChannels);

  QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
  if (!client_id_.isEmpty()) {
    environment.insert("SPOTIFY_CLIENT_ID", client_id_);
  }
  if (!client_secret_.isEmpty()) {
    environment.insert("SPOTIFY_CLIENT_SECRET", client_secret_);
  }
  process_->setProcessEnvironment(environment);

  connect(process_, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
          this, [this](int exit_code, QProcess::ExitStatus) { ProcessFinished(exit_code); });
  connect(process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
    if (error == QProcess::FailedToStart) {
      emit Failed(tr("Could not run %1. Install plantag or set its path in "
                     "Settings.")
                      .arg(BinaryPath()));
      process_->deleteLater();
      process_ = nullptr;
    }
  });

  qLog(Info) << "plantag" << arguments;
  process_->start(BinaryPath(), arguments);
}

void PlantagClient::Cancel() {
  if (process_) {
    process_->disconnect(this);
    if (process_->state() != QProcess::NotRunning) {
      process_->kill();
      process_->waitForFinished(1000);
    }
    process_->deleteLater();
    process_ = nullptr;
  }
}

void PlantagClient::Identify(const SongList& songs) {
  applying_ = false;

  QStringList paths;
  for (const Song& song : songs) {
    const QString local = song.url().toLocalFile();
    if (!local.isEmpty()) paths << local;
  }
  if (paths.isEmpty()) {
    emit Failed(tr("No local files in the selection to look up."));
    return;
  }
  if (paths.size() > kMaxPathsPerInvocation) {
    paths = paths.mid(0, kMaxPathsPerInvocation);
  }
  Start(BuildArguments(paths));
}

QStringList PlantagClient::BuildArguments(const QStringList& paths) const {
  QStringList arguments;
  arguments << "identify" << "--json" << paths;
  return arguments;
}

void PlantagClient::ApplyPicks(const QHash<QString, QString>& picks) {
  applying_ = true;

  QStringList arguments;
  arguments << "apply" << "--json";
  QStringList paths;
  for (auto it = picks.constBegin(); it != picks.constEnd(); ++it) {
    // The tool matches picks on the exact path string it was given, so pass the
    // same path in both places.
    arguments << "--pick" << QString("%1=%2").arg(it.key(), it.value());
    paths << it.key();
  }
  if (paths.isEmpty()) {
    emit Applied(0, 0);
    return;
  }
  arguments << paths;

  Start(arguments);
}

void PlantagClient::ProcessFinished(int exit_code) {
  if (!process_) return;

  const QByteArray standard_out = process_->readAllStandardOutput();
  const QByteArray standard_error = process_->readAllStandardError();
  process_->deleteLater();
  process_ = nullptr;

  // Exit code 2 means "ran fine, but some files need attention" — a normal
  // outcome, not an error. Only 1 is a real failure.
  if (exit_code != 0 && exit_code != 2) {
    const QString message = QString::fromUtf8(standard_error).trimmed();
    emit Failed(message.isEmpty()
                    ? tr("plantag failed (exit code %1)").arg(exit_code)
                    : message);
    return;
  }

  if (!standard_error.trimmed().isEmpty()) {
    qLog(Info) << "plantag:" << QString::fromUtf8(standard_error).trimmed();
  }

  if (applying_) {
    // Count applied vs failed from the JSON rather than assuming success.
    QJsonParseError parse_error;
    const QJsonDocument document = QJsonDocument::fromJson(standard_out, &parse_error);
    int succeeded = 0;
    int failed = 0;
    if (parse_error.error == QJsonParseError::NoError && document.isObject()) {
      for (const QJsonValue& value : document.object().value("results").toArray()) {
        const QString status = JsonString(value.toObject(), "status");
        if (status == "applied") {
          ++succeeded;
        } else {
          ++failed;
        }
      }
    } else {
      failed = -1;
    }
    emit Applied(succeeded, failed);
    return;
  }

  QList<FileResult> results;
  QString error;
  if (ParseIdentifyJson(standard_out, &results, &error)) {
    emit Finished(results);
  } else {
    emit Failed(error);
  }
}

bool PlantagClient::ParseIdentifyJson(const QByteArray& json, QList<FileResult>* out,
                                      QString* error) {
  QJsonParseError parse_error;
  const QJsonDocument document = QJsonDocument::fromJson(json, &parse_error);
  if (parse_error.error != QJsonParseError::NoError) {
    if (error) {
      *error = tr("Could not parse plantag output: %1").arg(parse_error.errorString());
    }
    return false;
  }
  if (!document.isObject()) {
    if (error) *error = tr("Unexpected plantag output (not a JSON object).");
    return false;
  }

  const QJsonObject root = document.object();
  const QJsonArray results = root.value("results").toArray();
  if (results.isEmpty()) {
    if (error) *error = tr("plantag returned no results.");
    return false;
  }

  for (const QJsonValue& value : results) {
    const QJsonObject object = value.toObject();
    FileResult result;
    result.path = JsonString(object, "file");
    result.status = JsonString(object, "status");
    result.reason = JsonString(object, "reason");

    const QJsonObject existing = object.value("existing").toObject();
    result.artist = JsonString(existing, "artist");
    result.title = JsonString(existing, "title");
    result.existing_bpm = JsonDouble(existing, "bpm");

    if (object.value("chosen").isObject()) {
      result.chosen = ParseCandidate(object.value("chosen").toObject());
      result.has_chosen = true;
    }
    for (const QJsonValue& candidate : object.value("candidates").toArray()) {
      result.candidates.append(ParseCandidate(candidate.toObject()));
    }
    out->append(result);
  }
  return true;
}
