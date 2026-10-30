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

#ifndef BPMFETCHDIALOG_H
#define BPMFETCHDIALOG_H

#include <QDialog>
#include <QHash>
#include <QList>
#include <QMap>
#include <QString>

#include "plantag/plantagclient.h"

class QComboBox;
class QLabel;
class QNetworkAccessManager;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

// Review screen for a BPM/ID lookup run.
//
// Rows that plantag resolved with confidence are shown pre-selected; rows it
// could not decide are marked and get a combo box of the alternatives, each
// showing album, duration and a cover thumbnail so the choice is a real one
// rather than a list of opaque ids.
//
// Nothing is written until the user accepts. On accept the dialog emits
// PicksAccepted with path -> spotify id, and the caller runs `apply --pick`,
// which applies exactly those choices instead of re-running the ranking.
class BpmFetchDialog : public QDialog {
  Q_OBJECT

 public:
  explicit BpmFetchDialog(QWidget* parent = nullptr);

  void SetResults(const QList<PlantagClient::FileResult>& results);

  // path -> spotify id for every row the user left chosen.
  QHash<QString, QString> Picks() const;

 signals:
  void PicksAccepted();

 private slots:
  void RowSelectionChanged();
  void Accept();
  void ChooseBinary();

 private:
  void Populate();
  void FillCandidateList(QComboBox* combo,
                         const QList<PlantagClient::Candidate>& candidates);
  void UpdateDetails();
  void FetchCover(const QString& url);
  void EnsureBinaryAvailable();

  QTreeWidget* tree_;
  QLabel* cover_;
  QLabel* details_;
  QLabel* status_label_;
  QPushButton* accept_button_;
  QPushButton* choose_binary_button_;

  QList<PlantagClient::FileResult> results_;
  QMap<QTreeWidgetItem*, int> item_to_result_;
  QMap<QTreeWidgetItem*, QComboBox*> item_to_combo_;

  QNetworkAccessManager* network_;
  QString pending_cover_url_;
};

#endif  // BPMFETCHDIALOG_H
