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

#include "ui/bpmfetchdialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPixmap>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QUrl>
#include <QVBoxLayout>

namespace {

constexpr int kCoverSize = 96;

QString FormatDuration(int milliseconds) {
  if (milliseconds <= 0) return QString();
  const int total_seconds = milliseconds / 1000;
  return QString("%1:%2").arg(total_seconds / 60).arg(
      total_seconds % 60, 2, 10, QChar('0'));
}

QString DescribeCandidate(const PlantagClient::Candidate& candidate) {
  const QString artist = candidate.artists.join(", ");
  const QString tempo = candidate.tempo > 0
                            ? QString("%1 bpm").arg(candidate.tempo, 0, 'f', 1)
                            : QObject::tr("no bpm");
  const QString duration = FormatDuration(candidate.duration_ms);
  return QString("%1 — %2 (%3, %4) [%5]")
      .arg(candidate.title.isEmpty() ? QObject::tr("(unknown)") : candidate.title,
           artist.isEmpty() ? QObject::tr("(unknown artist)") : artist,
           candidate.album.isEmpty() ? QObject::tr("no album") : candidate.album,
           tempo,
           QString::number(candidate.confidence, 'f', 2));
}

}  // namespace

BpmFetchDialog::BpmFetchDialog(QWidget* parent)
    : QDialog(parent),
      tree_(new QTreeWidget(this)),
      cover_(new QLabel(this)),
      details_(new QLabel(this)),
      status_label_(new QLabel(this)),
      accept_button_(nullptr),
      choose_binary_button_(nullptr),
      network_(new QNetworkAccessManager(this)) {
  setWindowTitle(tr("Fetch BPM and identifiers"));
  resize(900, 520);

  tree_->setColumnCount(4);
  tree_->setHeaderLabels(
      {tr("File"), tr("Status"), tr("Match"), tr("Existing BPM")});
  tree_->setRootIsDecorated(false);
  tree_->setSelectionMode(QAbstractItemView::SingleSelection);
  tree_->header()->setStretchLastSection(false);
  tree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
  tree_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
  tree_->header()->setSectionResizeMode(2, QHeaderView::Stretch);
  tree_->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);

  cover_->setFixedSize(kCoverSize, kCoverSize);
  cover_->setScaledContents(true);
  cover_->setFrameShape(QFrame::StyledPanel);

  details_->setWordWrap(true);
  details_->setAlignment(Qt::AlignTop | Qt::AlignLeft);

  QHBoxLayout* detail_layout = new QHBoxLayout;
  detail_layout->addWidget(cover_);
  detail_layout->addWidget(details_, 1);

  QDialogButtonBox* buttons =
      new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
  accept_button_ = buttons->button(QDialogButtonBox::Ok);
  accept_button_->setText(tr("Write tags"));
  choose_binary_button_ = buttons->addButton(tr("Locate plantag..."),
                                             QDialogButtonBox::ActionRole);

  status_label_->setWordWrap(true);

  QVBoxLayout* layout = new QVBoxLayout(this);
  layout->addWidget(tree_, 1);
  layout->addLayout(detail_layout);
  layout->addWidget(status_label_);
  layout->addWidget(buttons);

  connect(tree_, &QTreeWidget::itemSelectionChanged, this,
          &BpmFetchDialog::RowSelectionChanged);
  connect(accept_button_, &QPushButton::clicked, this, &BpmFetchDialog::Accept);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  connect(choose_binary_button_, &QPushButton::clicked, this,
          &BpmFetchDialog::ChooseBinary);
}

void BpmFetchDialog::SetResults(
    const QList<PlantagClient::FileResult>& results) {
  results_ = results;
  Populate();
  EnsureBinaryAvailable();
}

void BpmFetchDialog::EnsureBinaryAvailable() {
  const QString configured =
      QSettings().value("plantag/binary").toString();
  const QString binary = configured.isEmpty()
                             ? QStandardPaths::findExecutable("plantag")
                             : configured;
  if (binary.isEmpty()) {
    status_label_->setText(
        tr("plantag was not found. Use \"Locate plantag...\" to point "
           "Clementine at it, or install it so it is on your PATH."));
    accept_button_->setEnabled(false);
  } else {
    status_label_->setText(tr("Using %1").arg(binary));
  }
}

void BpmFetchDialog::Populate() {
  tree_->clear();
  item_to_result_.clear();
  item_to_combo_.clear();

  int ambiguous = 0;
  int not_found = 0;

  for (int index = 0; index < results_.size(); ++index) {
    const PlantagClient::FileResult& result = results_.at(index);

    QTreeWidgetItem* item = new QTreeWidgetItem(tree_);
    item->setText(0, QFileInfo(result.path).fileName());
    item->setToolTip(0, result.path);

    QString status_text;
    if (result.status == "matched") {
      status_text = tr("Matched");
    } else if (result.status == "ambiguous") {
      status_text = tr("Ambiguous");
      ++ambiguous;
    } else if (result.status == "already_tagged") {
      status_text = tr("Already tagged");
    } else {
      status_text = tr("Not found");
      ++not_found;
    }
    item->setText(1, status_text);

    // A combo of the alternatives, so an ambiguous row is resolvable here
    // rather than by editing ids by hand.
    if (result.candidates.size() > 1) {
      QComboBox* combo = new QComboBox(tree_);
      FillCandidateList(combo, result.candidates);
      tree_->setItemWidget(item, 2, combo);
      item_to_combo_.insert(item, combo);
    } else if (result.has_chosen) {
      item->setText(2, DescribeCandidate(result.chosen));
    } else {
      item->setText(2, tr("—"));
    }

    item->setText(3, result.existing_bpm > 0
                         ? QString::number(result.existing_bpm, 'g', 6)
                         : QString());

    item_to_result_.insert(item, index);
  }

  tree_->resizeColumnToContents(1);
  accept_button_->setEnabled(!results_.isEmpty());

  if (ambiguous > 0 || not_found > 0) {
    status_label_->setText(
        tr("%1 file(s). %2 need a decision, %3 not found.")
            .arg(results_.size())
            .arg(ambiguous)
            .arg(not_found));
  } else {
    status_label_->setText(tr("%1 file(s) matched.").arg(results_.size()));
  }

  if (tree_->topLevelItemCount() > 0) {
    tree_->setCurrentItem(tree_->topLevelItem(0));
  }
}

void BpmFetchDialog::FillCandidateList(
    QComboBox* combo, const QList<PlantagClient::Candidate>& candidates) {
  for (const PlantagClient::Candidate& candidate : candidates) {
    combo->addItem(DescribeCandidate(candidate), candidate.spotify_id);
  }
}

void BpmFetchDialog::RowSelectionChanged() { UpdateDetails(); }

void BpmFetchDialog::UpdateDetails() {
  QTreeWidgetItem* item = tree_->currentItem();
  if (!item || !item_to_result_.contains(item)) {
    details_->clear();
    cover_->clear();
    return;
  }

  const PlantagClient::FileResult& result = results_.at(item_to_result_.value(item));
  const PlantagClient::Candidate* candidate = nullptr;

  if (item_to_combo_.contains(item)) {
    QComboBox* combo = item_to_combo_.value(item);
    const int selected = combo->currentIndex();
    if (selected >= 0 && selected < result.candidates.size()) {
      candidate = &result.candidates.at(selected);
    }
  } else if (result.has_chosen) {
    candidate = &result.chosen;
  }

  if (!candidate) {
    details_->setText(tr("%1\n\n%2").arg(result.path, result.reason));
    cover_->clear();
    return;
  }

  const QString tempo = candidate->tempo > 0
                            ? QString("%1 bpm").arg(candidate->tempo, 0, 'f', 1)
                            : tr("no bpm available");
  const QString duration = FormatDuration(candidate->duration_ms);
  // Chained one placeholder at a time: the multi-argument arg() overloads vary
  // between Qt versions and are easy to get wrong.
  QString text = tr("%1\n%2\n\nAlbum: %3\nLength: %4\nTempo: %5\n"
                    "Confidence: %6\nSpotify: %7\nISRC: %8\n\n%9");
  text = text.arg(result.title.isEmpty() ? QFileInfo(result.path).fileName()
                                        : result.title);
  text = text.arg(candidate->artists.join(", "));
  text = text.arg(candidate->album.isEmpty() ? tr("(none)") : candidate->album);
  text = text.arg(duration.isEmpty() ? tr("(unknown)") : duration);
  text = text.arg(tempo);
  text = text.arg(QString::number(candidate->confidence, 'f', 2));
  text = text.arg(candidate->spotify_id.isEmpty() ? tr("(none)")
                                                  : candidate->spotify_id);
  text = text.arg(candidate->isrc.isEmpty() ? tr("(none)") : candidate->isrc);
  text = text.arg(result.reason);
  details_->setText(text);

  if (!candidate->cover_url.isEmpty()) {
    FetchCover(candidate->cover_url);
  } else {
    cover_->clear();
  }
}

void BpmFetchDialog::FetchCover(const QString& url) {
  if (url == pending_cover_url_) return;
  pending_cover_url_ = url;

  QNetworkRequest request{QUrl(url)};
  request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                       QNetworkRequest::NoLessSafeRedirectPolicy);
  QNetworkReply* reply = network_->get(request);

  connect(reply, &QNetworkReply::finished, this, [this, reply, url]() {
    reply->deleteLater();
    // The user may have moved on to another row while this was in flight.
    if (url != pending_cover_url_) return;
    if (reply->error() != QNetworkReply::NoError) {
      cover_->setText(tr("no art"));
      return;
    }
    QPixmap pixmap;
    if (pixmap.loadFromData(reply->readAll())) {
      cover_->setPixmap(pixmap.scaled(kCoverSize, kCoverSize,
                                     Qt::KeepAspectRatio,
                                     Qt::SmoothTransformation));
    } else {
      cover_->setText(tr("no art"));
    }
  });
}

QHash<QString, QString> BpmFetchDialog::Picks() const {
  QHash<QString, QString> picks;

  for (auto it = item_to_result_.constBegin(); it != item_to_result_.constEnd();
       ++it) {
    QTreeWidgetItem* item = it.key();
    const PlantagClient::FileResult& result = results_.at(it.value());
    if (result.status == "not_found") continue;

    if (item_to_combo_.contains(item)) {
      QComboBox* combo = item_to_combo_.value(item);
      const QString spotify_id = combo->currentData().toString();
      if (!spotify_id.isEmpty()) {
        picks.insert(result.path, spotify_id);
      }
    } else if (result.has_chosen && !result.chosen.spotify_id.isEmpty()) {
      picks.insert(result.path, result.chosen.spotify_id);
    }
  }
  return picks;
}

void BpmFetchDialog::Accept() {
  if (Picks().isEmpty()) {
    status_label_->setText(tr("Nothing to write — no row has a chosen match."));
    return;
  }
  emit PicksAccepted();
  accept();
}

void BpmFetchDialog::ChooseBinary() {
  const QString path = QFileDialog::getOpenFileName(
      this, tr("Locate the plantag executable"), QString(),
      tr("All files (*)"));
  if (path.isEmpty()) return;

  QSettings().setValue("plantag/binary", path);
  EnsureBinaryAvailable();
}
