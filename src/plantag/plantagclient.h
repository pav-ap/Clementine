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

#ifndef PLANTAGCLIENT_H
#define PLANTAGCLIENT_H

#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

#include "core/song.h"

class QProcess;

// Thin wrapper around the external `plantag` command-line tool, which does the
// actual Spotify/ReccoBeats lookups and tag writing.
//
// The lookups deliberately live outside the player:
//   - Spotify's /v1/audio-features (the old source of BPM) was closed to new
//     apps on 2024-11-27, so the fetching logic has an awkward dependency
//     situation that's easier to fix in one small Python tool than in C++.
//   - A separate tool also serves every other player (Strawberry, Navidrome...)
//     and survives Clementine being unmaintained.
//
// Communication is JSON over stdout, which is why the tool's --json contract
// matters. See plantag's README for the schema.
class PlantagClient : public QObject {
  Q_OBJECT

 public:
  // One possible identity for a file.
  struct Candidate {
    QString spotify_id;
    QString reccobeats_id;
    QString isrc;
    QString title;
    QString album;
    QString cover_url;
    QStringList artists;
    double tempo = 0.0;      // BPM from ReccoBeats; 0 means unavailable
    double confidence = 0.0;  // 0..1
    int duration_ms = 0;
  };

  // The resolution for a single file.
  struct FileResult {
    QString path;
    QString status;  // matched | ambiguous | not_found | already_tagged
    QString reason;
    QString artist;
    QString title;
    double existing_bpm = 0.0;
    Candidate chosen;            // best suggestion; valid even when ambiguous
    QList<Candidate> candidates;  // ranked, for the picker
    bool has_chosen = false;

    bool NeedsAttention() const {
      return status == "ambiguous" || status == "not_found";
    }
    bool IsResolved() const { return status == "matched"; }
  };

  explicit PlantagClient(QObject* parent = nullptr);

  // Where the executable lives. Empty means "look it up on PATH".
  void SetBinaryPath(const QString& path);
  QString BinaryPath() const;

  // Credentials are passed to the child process through its environment, never
  // through the command line (argv is visible in `ps` to every user on the box).
  void SetSpotifyCredentials(const QString& client_id, const QString& client_secret);

  bool IsAvailable() const;
  bool IsRunning() const;

  // Runs `identify --json` for the given songs. Emits Finished or Failed.
  void Identify(const SongList& songs);

  // Applies an explicit human choice per file: path -> Spotify track id.
  // Runs `apply --pick path=id` for each, so ranking is not re-run and the
  // user's decision cannot be silently overridden. Emits Applied.
  void ApplyPicks(const QHash<QString, QString>& picks);

  static bool ParseIdentifyJson(const QByteArray& json, QList<FileResult>* out,
                                QString* error);

 public slots:
  void Cancel();

 signals:
  void Finished(const QList<PlantagClient::FileResult>& results);
  void Applied(int succeeded, int failed);
  void Failed(const QString& message);

 private slots:
  void ProcessFinished(int exit_code);

 private:
  QStringList BuildArguments(const QStringList& paths) const;
  void Start(const QStringList& arguments);

  QProcess* process_;
  QString binary_path_;
  QString client_id_;
  QString client_secret_;
  bool applying_ = false;
};

Q_DECLARE_METATYPE(PlantagClient::FileResult)
Q_DECLARE_METATYPE(QList<PlantagClient::FileResult>)

#endif  // PLANTAGCLIENT_H
