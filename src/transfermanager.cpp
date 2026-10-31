#include "transfermanager.h"
#include "radioservice.h"
#include <KConfigGroup>
#include <KIO/JobTracker>
#include <KIO/OpenUrlJob>
#include <KJobTrackerInterface>
#include <KNotification>
#include <KSharedConfig>
#include <KUiServerV2JobTracker>
#include <QApplication>
#include <QCheckBox>
#include <QDBusArgument>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QStandardPaths>
#include <QSysInfo>
#include <QTimer>
#include <QUrl>
#include <QUuid>
#include <QVBoxLayout>

TransferJob::TransferJob(QString title, QObject *parent)
    : KJob(parent), m_title(std::move(title)) {
  setCapabilities(Killable);
}
void TransferJob::start() {
  QTimer::singleShot(0, this, [this] { emit description(this, m_title); });
}
void TransferJob::progress(qulonglong done, qulonglong total) {
  if (m_finished)
    return;
  if (total)
    setTotalAmount(Bytes, total);
  setProcessedAmount(Bytes, done);
}
bool TransferJob::doKill() {
  emit cancellationRequested();
  return false;
}
void TransferJob::finish(const QString &error, bool cancelled) {
  if (m_finished)
    return;
  m_finished = true;
  if (cancelled)
    setError(KilledJobError);
  else if (!error.isEmpty()) {
    setError(UserDefinedError);
    setErrorText(error);
  }
  emitResult();
}
TransferManager::TransferManager(RadioService *radio)
    : QObject(radio), m_radio(radio) {
  KIO::setJobTracker(new KUiServerV2JobTracker(this));
  connect(radio, &RadioService::LinkReady, this, &TransferManager::start);
  connect(radio, &RadioService::RadioEnded, this, [this] {
    m_restartLink.clear();
    if (m_sharePending) {
      m_sharePending = false;
      m_pendingPaths.clear();
      notify(tr("Sharing stopped"),
             tr("The radio disconnected before sharing was ready."));
    }
    stop();
  });
  connect(radio, &RadioService::PickFilesRequested, this,
          &TransferManager::pickFiles);
  connect(radio, &RadioService::PickFilesForPeerRequested, this,
          &TransferManager::pickForPeer);
  connect(radio, &RadioService::SendFilesRequested, this,
          &TransferManager::sendFiles);
  connect(radio, &RadioService::ReceiveFilesRequested, this,
          &TransferManager::receiveFiles);
  connect(radio, &RadioService::ShareWarningRequired, this,
          &TransferManager::confirmSharing);
  connect(radio, &RadioService::Changed, this, [this] { advanceSharing(); });
  connect(radio, &RadioService::ForgetRecipientRequested, this, [this] {
    auto config = KSharedConfig::openConfig("featherrc");
    config->deleteGroup("Sharing");
    config->sync();
    status();
  });
  connect(radio, &RadioService::BrowseRequested, this,
          &TransferManager::browse);
  connect(radio, &RadioService::DiscoveryExpired, this, [this] {
    m_accepting = false;
    command({{"command", "pause"}});
    const auto ids = m_entries.keys();
    for (const auto &id : ids)
      if (m_entries.value(id).state == "approval")
        decline(id);
    status();
  });
  auto *heartbeat = new QTimer(this);
  heartbeat->setInterval(30000);
  connect(heartbeat, &QTimer::timeout, this, [this] {
    if (!m_entries.isEmpty() && !m_preparing &&
        !m_radio->Snapshot().value("busy").toBool()) {
      m_preparing = true;
      const auto process = m_process;
      m_radio->protectTransfers([this, process](bool ok) {
        if (m_process != process)
          return;
        m_preparing = false;
        if (!ok) {
          m_error = tr("Could not keep the radio available for transfers.");
          stop();
        }
      });
    }
  });
  heartbeat->start();
  status();
}
void TransferManager::notify(const QString &title, const QString &text) {
  auto *n =
      new KNotification("information", KNotification::CloseOnTimeout, this);
  n->setComponentName("feather");
  n->setTitle(title);
  n->setText(text.toHtmlEscaped());
  n->setIconName("document-send");
  n->sendEvent();
}
bool TransferManager::accepting() const {
  const auto snapshot = m_radio->Snapshot();
  const auto deadline = snapshot.value("deadline").toLongLong();
  return m_ready && !m_stopping && m_accepting &&
         snapshot.value("owned").toBool() &&
         snapshot.value("state").toString() == "on" &&
         snapshot.value("timerKnown").toBool() &&
         (!deadline || deadline > QDateTime::currentSecsSinceEpoch());
}
void TransferManager::start(const QVariantMap &link) {
  if (m_process) {
    m_restartLink = link;
    stop();
    return;
  }
  m_stopping = false;
  m_preparing = false;
  m_error.clear();
  m_accepting = true;
  const auto iface = link.value("interface").toString();
  const auto helper = qEnvironmentVariable("FEATHER_TRANSFER_HELPER",
                                           QStringLiteral(FEATHER_HELPER_PATH));
  if (iface.isEmpty() || helper.isEmpty()) {
    m_error = tr("The transfer helper is missing. Reinstall Feather.");
    status();
    return;
  }
  auto *process = new QProcess(this);
  m_process = process;
  const auto destination =
      QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
  m_destination = destination;
  if (!QDir().mkpath(destination)) {
    m_error = tr("Could not create the Downloads folder.");
    m_process = nullptr;
    process->deleteLater();
    status();
    return;
  }
  process->setProgram(helper);
  QStringList arguments{"--interface", iface,
                        "--name",      QSysInfo::machineHostName(),
                        "--directory", destination};
  if (link.value("announce").canConvert<QDBusArgument>()) {
    const auto announce = link.value("announce").value<QDBusArgument>();
    uint count = 0, interval = 0, pause = 0;
    announce.beginStructure();
    announce >> count >> interval >> pause;
    announce.endStructure();
    arguments << "--announce-count" << QString::number(count)
              << "--announce-interval-us" << QString::number(interval)
              << "--announce-pause-us" << QString::number(pause);
  }
  process->setArguments(arguments);
  process->setProcessChannelMode(QProcess::SeparateChannels);
  m_buffer.clear();
  connect(process, &QProcess::started, this, [this, process] {
    if (m_process == process && m_stopping)
      command({{"command", "shutdown"}});
  });
  connect(process, &QProcess::readyReadStandardOutput, this, [this, process] {
    if (m_process != process)
      return;
    m_buffer += process->readAllStandardOutput();
    if (m_buffer.size() > 1024 * 1024) {
      m_error = tr("Invalid transfer helper response.");
      stop();
      return;
    }
    while (m_buffer.contains('\n')) {
      const auto index = m_buffer.indexOf('\n');
      const auto line = m_buffer.left(index);
      m_buffer.remove(0, index + 1);
      QJsonParseError error;
      auto doc = QJsonDocument::fromJson(line, &error);
      if (error.error != QJsonParseError::NoError || !doc.isObject()) {
        m_error = tr("Invalid transfer helper response.");
        stop();
        return;
      }
      event(doc.object());
    }
  });
  connect(process, &QProcess::readyReadStandardError, this,
          [process] { process->readAllStandardError(); });
  connect(process, &QProcess::errorOccurred, this,
          [this, process](QProcess::ProcessError error) {
            if (m_process == process) {
              m_error = process->errorString();
              if (error == QProcess::FailedToStart)
                processEnded(process);
              else
                stop();
            }
          });
  connect(
      process, &QProcess::finished, this,
      [this, process](int, QProcess::ExitStatus) { processEnded(process); });
  process->start();
  status();
}
void TransferManager::command(const QJsonObject &message) {
  if (m_process && m_process->state() == QProcess::Running)
    m_process->write(QJsonDocument(message).toJson(QJsonDocument::Compact) +
                     '\n');
}
void TransferManager::stop() {
  m_ready = false;
  m_accepting = false;
  if (m_sharePending && !m_error.isEmpty()) {
    m_sharePending = false;
    m_pendingPaths.clear();
    notify(tr("Couldn’t start sharing"), m_error);
  }
  const auto waiting = m_entries.keys();
  for (const auto &id : waiting)
    if (m_entries.value(id).state == "approval")
      decline(id);
  if (m_process && !m_stopping) {
    m_stopping = true;
    for (auto it = m_entries.begin(); it != m_entries.end(); ++it)
      it->state = "stopping";
    auto process = m_process;
    command({{"command", "shutdown"}});
    QTimer::singleShot(35000, this, [this, process] {
      if (process && process->state() != QProcess::NotRunning &&
          m_entries.isEmpty())
        process->kill();
    });
  }
  m_peers.clear();
  status();
}
void TransferManager::processEnded(QProcess *process) {
  if (m_process != process) {
    process->deleteLater();
    return;
  }
  m_process = nullptr;
  m_ready = false;
  m_stopping = false;
  m_preparing = false;
  if (m_error.isEmpty() && !m_entries.isEmpty())
    m_error = tr("The transfer service stopped.");
  const auto ids = m_entries.keys();
  for (const auto &id : ids)
    terminal(id, m_error.isEmpty() ? tr("Radio disconnected.") : m_error);
  m_peers.clear();
  status();
  process->deleteLater();
  const auto link = m_restartLink;
  m_restartLink.clear();
  if (!link.isEmpty() && m_radio->Snapshot().value("owned").toBool())
    start(link);
}
void TransferManager::status() {
  QVariantList peers, transfers;
  for (auto it = m_peers.cbegin(); it != m_peers.cend(); ++it)
    peers.append(it.value());
  for (auto it = m_entries.cbegin(); it != m_entries.cend(); ++it)
    transfers.append(QVariantMap{{"id", it.key()},
                                 {"name", it->name},
                                 {"direction", it->direction},
                                 {"state", it->state}});
  const KConfigGroup sharing(KSharedConfig::openConfig("featherrc"), "Sharing");
  m_radio->setTransferStatus(
      {{"rememberedRecipient", sharing.readEntry("RecipientName", QString())},
       {"receiving", m_ready && m_accepting},
       {"transferStopping", m_stopping},
       {"peers", peers},
       {"transfers", transfers},
       {"transferError", m_error}});
  m_radio->setTransferCount(m_entries.size());
}
void TransferManager::terminal(const QString &id, const QString &error,
                               bool cancelled) {
  if (!m_entries.contains(id))
    return;
  const auto entry = m_entries.take(id);
  if (entry.prompt)
    entry.prompt->close();
  if (entry.job)
    entry.job->finish(error, cancelled);
  status();
}
void TransferManager::decline(const QString &id) {
  command({{"command", "decline"}, {"id", id}});
  terminal(id, {}, true);
}
void TransferManager::accept(const QString &id) {
  if (!m_entries.contains(id) || m_entries.value(id).state != "approval")
    return;
  if (!accepting()) {
    decline(id);
    return;
  }
  auto &entry = m_entries[id];
  entry.state = "incoming";
  auto prompt = entry.prompt;
  entry.prompt = nullptr;
  if (prompt)
    prompt->close();
  entry.job = new TransferJob(tr("Receiving from %1").arg(entry.name), this);
  const auto job = entry.job;
  const auto process = m_process;
  connect(job, &TransferJob::cancellationRequested, this, [this, id, process] {
    if (m_process == process)
      command({{"command", "cancel"}, {"id", id}});
  });
  KIO::getJobTracker()->registerJob(job);
  job->start();
  command({{"command", "accept"}, {"id", id}});
  status();
}
void TransferManager::event(const QJsonObject &message) {
  const auto kind = message["event"].toString(), id = message["id"].toString();
  if (kind == "ready" && !m_stopping) {
    m_ready = true;
    status();
    advanceSharing();
    return;
  }
  if (kind == "peer" && !m_stopping) {
    m_peers[id] = {{"id", id},
                   {"name", message["name"].toString()},
                   {"address", message["address"].toString()}};
    status();
    return;
  }
  if ((kind == "peer_removed" || kind == "peer-lost")) {
    m_peers.remove(id);
    status();
    return;
  }
  if (kind == "warning") {
    m_error = message["reason"].toString();
    status();
    return;
  }
  if (kind == "fatal") {
    m_error = message["reason"].toString();
    stop();
    return;
  }
  if (kind == "request") {
    if (id.isEmpty() || m_entries.contains(id) || !accepting() || m_preparing ||
        m_entries.size() >= 8) {
      command({{"command", "decline"}, {"id", id}});
      return;
    }
    const auto sender = message["sender"].toString().left(128);
    m_entries[id] = {sender, "incoming", "approval", {}, {}};
    status();
    m_preparing = true;
    const auto process = m_process;
    m_radio->protectTransfers([this, id, message, sender, process](bool ok) {
      if (m_process != process)
        return;
      m_preparing = false;
      if (!m_entries.contains(id))
        return;
      if (!ok || !accepting()) {
        decline(id);
        notify(tr("Couldn’t receive files"),
               tr("Use unlimited mode or update Owly to protect timed "
                  "transfers."));
        return;
      }
      auto *n = new KNotification("incoming", KNotification::Persistent, this);
      m_entries[id].prompt = n;
      n->setComponentName("feather");
      n->setTitle(tr("Receive files from %1?").arg(sender).toHtmlEscaped());
      QStringList names;
      for (const auto &value : message["files"].toArray())
        names << value.toString().left(160);
      n->setText(tr("%1 files: %2\nSave to %3")
                     .arg(names.size())
                     .arg(names.mid(0, 5).join(", "))
                     .arg(m_destination)
                     .toHtmlEscaped());
      n->setIconName("document-save");
      connect(n->addAction(tr("Accept")), &KNotificationAction::activated, this,
              [this, id, process] {
                if (m_process == process)
                  accept(id);
              });
      connect(n->addAction(tr("Decline")), &KNotificationAction::activated,
              this, [this, id, process] {
                if (m_process == process)
                  decline(id);
              });
      connect(n, &KNotification::closed, this, [this, id, process] {
        if (m_process == process && m_entries.contains(id) &&
            m_entries[id].state == "approval")
          decline(id);
      });
      QTimer::singleShot(25000, this, [this, id, process] {
        if (m_process == process && m_entries.contains(id) &&
            m_entries[id].state == "approval")
          decline(id);
      });
      n->sendEvent();
      status();
    });
    return;
  }
  if (kind == "withdrawn") {
    terminal(id, {}, true);
    return;
  }
  if (kind == "started" && m_entries.contains(id)) {
    m_entries[id].state = message["direction"].toString();
    status();
    return;
  }
  if (kind == "progress" && m_entries.contains(id) && m_entries[id].job) {
    m_entries[id].job->progress(message["done"].toVariant().toULongLong(),
                                message["total"].toVariant().toULongLong());
    return;
  }
  if (kind == "completed") {
    const bool incoming =
        m_entries.contains(id) && m_entries.value(id).direction == "incoming";
    terminal(id);
    if (incoming) {
      auto *n =
          new KNotification("information", KNotification::CloseOnTimeout, this);
      n->setComponentName("feather");
      n->setTitle(tr("Files received"));
      n->setText(tr("Saved to %1").arg(m_destination).toHtmlEscaped());
      n->setIconName("document-save");
      const auto destination = m_destination;
      connect(n->addAction(tr("Open folder")), &KNotificationAction::activated,
              this, [destination] {
                auto *job =
                    new KIO::OpenUrlJob(QUrl::fromLocalFile(destination));
                job->start();
              });
      n->sendEvent();
    }
    return;
  }
  if (kind == "failed") {
    terminal(id, message["reason"].toString());
    return;
  }
  if (kind == "cancelled") {
    terminal(id, {}, true);
    return;
  }
}
void TransferManager::browse() {
  if (!m_ready)
    return;
  m_peers.clear();
  command({{"command", "browse"}});
  status();
}
void TransferManager::pickFiles() { pickForPeer({}); }
void TransferManager::pickForPeer(const QString &peer) {
  auto *dialog = new QFileDialog(nullptr, tr("Send with Feather"));
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  dialog->setFileMode(QFileDialog::ExistingFiles);
  connect(
      dialog, &QFileDialog::filesSelected, this,
      [this, peer](const QStringList &paths) { beginSharing(paths, peer); });
  dialog->open();
}
void TransferManager::sendFiles(const QStringList &urls) {
  QStringList paths;
  for (const auto &value : urls) {
    const auto url =
        QUrl::fromUserInput(value, QDir::currentPath(), QUrl::AssumeLocalFile);
    if (!url.isLocalFile() || !QFileInfo(url.toLocalFile()).isFile()) {
      notify(tr("Couldn’t send files"), tr("Choose local files to send."));
      return;
    }
    paths << url.toLocalFile();
  }
  if (!paths.isEmpty())
    beginSharing(paths);
}
void TransferManager::selectPeer(const QStringList &paths) {
  if (!accepting()) {
    notify(tr("Turn on Feather first"),
           tr("Enable the radio, then try sharing again."));
    return;
  }
  browse();
  auto *dialog = new QDialog;
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  dialog->setWindowTitle(tr("Send with Feather"));
  m_peerPicker = dialog;
  connect(dialog, &QDialog::finished, this, [this] { m_peerPicker = nullptr; });
  auto *layout = new QVBoxLayout(dialog);
  auto *hint = new QLabel(
      tr("Unlock your Apple device and enable AirDrop for everyone."));
  hint->setWordWrap(true);
  layout->addWidget(hint);
  auto *selection =
      new QLabel(paths.size() == 1 ? QFileInfo(paths.first()).fileName()
                                   : tr("%1 files selected").arg(paths.size()));
  selection->setTextFormat(Qt::PlainText);
  selection->setWordWrap(true);
  layout->addWidget(selection);
  auto *list = new QListWidget;
  layout->addWidget(list);
  const KConfigGroup sharing(KSharedConfig::openConfig("featherrc"), "Sharing");
  const auto remembered = m_preselectedPeer.isEmpty()
                              ? sharing.readEntry("RecipientId", QString())
                              : m_preselectedPeer;
  const auto rememberedAddress =
      sharing.readEntry("RecipientAddress", QString());
  m_preselectedPeer.clear();
  auto *remember = new QCheckBox(tr("Remember this device"));
  remember->setChecked(sharing.readEntry("RememberRecipient", false));
  layout->addWidget(remember);
  auto *buttons =
      new QDialogButtonBox(QDialogButtonBox::Cancel | QDialogButtonBox::Ok);
  layout->addWidget(buttons);
  auto *send = buttons->button(QDialogButtonBox::Ok);
  send->setText(tr("Send"));
  send->setEnabled(false);
  connect(list, &QListWidget::currentItemChanged, dialog,
          [this, send](QListWidgetItem *item) {
            send->setEnabled(item && accepting() && !m_preparing);
          });
  auto *timer = new QTimer(dialog);
  timer->setInterval(1000);
  connect(
      timer, &QTimer::timeout, dialog,
      [this, list, send, remembered, rememberedAddress] {
        const auto selected =
            list->currentItem()
                ? list->currentItem()->data(Qt::UserRole).toString()
                : QString();
        list->clear();
        for (auto it = m_peers.cbegin(); it != m_peers.cend(); ++it) {
          auto *item =
              new QListWidgetItem(it.value().value("name").toString(), list);
          item->setData(Qt::UserRole, it.key());
          if (it.key() == selected ||
              (selected.isEmpty() &&
               (it.key() == remembered ||
                (!rememberedAddress.isEmpty() &&
                 it.value().value("address").toString() == rememberedAddress))))
            list->setCurrentItem(item);
        }
        send->setEnabled(list->currentItem() && accepting() && !m_preparing);
      });
  connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
  connect(
      buttons, &QDialogButtonBox::accepted, dialog,
      [this, dialog, list, paths, remember] {
        if (!list->currentItem())
          return;
        const auto peer = list->currentItem()->data(Qt::UserRole).toString();
        if (startSend(peer, paths)) {
          auto config = KSharedConfig::openConfig("featherrc");
          KConfigGroup sharing(config, "Sharing");
          sharing.deleteGroup();
          if (remember->isChecked()) {
            sharing.writeEntry("RememberRecipient", true);
            sharing.writeEntry("RecipientId", peer);
            sharing.writeEntry("RecipientAddress",
                               m_peers.value(peer).value("address").toString());
            sharing.writeEntry("RecipientName",
                               m_peers.value(peer).value("name").toString());
          }
          config->sync();
          status();
          dialog->accept();
        }
      });
  dialog->resize(400, 300);
  timer->start();
  dialog->open();
}
bool TransferManager::startSend(const QString &peer, const QStringList &paths) {
  if (!accepting() || m_preparing || paths.isEmpty() ||
      !m_peers.contains(peer) || m_entries.size() >= 8) {
    notify(
        tr("Couldn’t start sharing"),
        tr("The device is unavailable or Feather is busy. Please try again."));
    return false;
  }
  const auto id = "out-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
  const auto name = m_peers.value(peer).value("name").toString();
  m_entries[id] = {name, "outgoing", "preparing", {}, {}};
  status();
  m_preparing = true;
  const auto process = m_process;
  m_radio->protectTransfers([this, id, peer, paths, name, process](bool ok) {
    if (m_process != process)
      return;
    m_preparing = false;
    if (!m_entries.contains(id))
      return;
    if (!ok || !accepting()) {
      terminal(id,
               tr("The radio became unavailable before the transfer started."));
      notify(tr("Couldn’t send files"),
             tr("Use unlimited mode or update Owly."));
      return;
    }
    auto &entry = m_entries[id];
    entry.job = new TransferJob(tr("Sending to %1").arg(name), this);
    connect(entry.job, &TransferJob::cancellationRequested, this,
            [this, id, process] {
              if (m_process == process)
                command({{"command", "cancel"}, {"id", id}});
            });
    KIO::getJobTracker()->registerJob(entry.job);
    entry.job->start();
    command({{"command", "send"},
             {"id", id},
             {"peer", peer},
             {"paths", QJsonArray::fromStringList(paths)}});
    status();
  });
  return true;
}

void TransferManager::receiveFiles() { beginSharing({}); }
void TransferManager::beginSharing(const QStringList &paths,
                                   const QString &peer) {
  if (m_sharePending || m_peerPicker) {
    notify(tr("Sharing is already opening"),
           tr("Finish the current selection first."));
    return;
  }
  m_pendingPaths = paths;
  m_preselectedPeer = peer;
  m_sharePending = true;
  const auto initial = m_radio->Snapshot();
  m_shareRenewRequested = initial.value("owned").toBool() &&
                          initial.value("deadline").toLongLong() > 0 &&
                          !initial.value("busy").toBool();
  if (!m_radio->startSharing()) {
    m_sharePending = false;
    m_pendingPaths.clear();
    notify(tr("Couldn’t start sharing"),
           tr("Feather is busy or cannot switch this session to unlimited "
              "mode. Turn it off and try again."));
    return;
  }
  advanceSharing();
}
void TransferManager::advanceSharing() {
  if (!m_sharePending)
    return;
  const auto snapshot = m_radio->Snapshot();
  if (snapshot.value("state").toString() == "error" ||
      snapshot.value("state").toString() == "unavailable") {
    m_sharePending = false;
    m_pendingPaths.clear();
    notify(tr("Couldn’t start sharing"), snapshot.value("detail").toString());
    return;
  }
  if (snapshot.value("owned").toBool() &&
      snapshot.value("state").toString() == "on" &&
      snapshot.value("deadline").toLongLong() > 0 &&
      !snapshot.value("busy").toBool()) {
    if (m_shareRenewRequested) {
      m_sharePending = false;
      m_pendingPaths.clear();
      notify(tr("Couldn’t start sharing"),
             tr("Could not switch to unlimited mode. Turn Feather off and try "
                "sharing again."));
      return;
    }
    m_shareRenewRequested = true;
    if (!m_radio->startSharing()) {
      m_sharePending = false;
      m_pendingPaths.clear();
      notify(tr("Couldn’t start sharing"),
             tr("Turn Feather off, then try sharing again."));
    }
    return;
  }
  if (!accepting() || snapshot.value("deadline").toLongLong() != 0 ||
      snapshot.value("busy").toBool())
    return;
  m_sharePending = false;
  const auto paths = m_pendingPaths;
  m_pendingPaths.clear();
  if (!paths.isEmpty())
    selectPeer(paths);
}
void TransferManager::confirmSharing() {
  if (!m_sharePending || m_shareWarning)
    return;
  auto *dialog = new QMessageBox(
      QMessageBox::Question, tr("Turn on Feather?"),
      tr("Sharing uses AirDrop until you turn it off. Intel Wi-Fi disconnects "
         "while it is on and may need manual reconnection afterward."),
      QMessageBox::Yes | QMessageBox::Cancel);
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  dialog->button(QMessageBox::Yes)->setText(tr("Turn on"));
  dialog->setDefaultButton(QMessageBox::Cancel);
  auto *suppress = new QCheckBox(tr("Don’t ask again"));
  dialog->setCheckBox(suppress);
  m_shareWarning = dialog;
  connect(dialog, &QMessageBox::finished, this, [this, suppress](int result) {
    m_shareWarning = nullptr;
    if (!m_sharePending)
      return;
    if (result != QMessageBox::Yes) {
      m_sharePending = false;
      m_pendingPaths.clear();
      return;
    }
    if (!m_radio->startSharing(true, suppress->isChecked())) {
      m_sharePending = false;
      m_pendingPaths.clear();
      notify(tr("Couldn’t start sharing"),
             tr("The radio is currently unavailable."));
    }
    advanceSharing();
  });
  dialog->open();
}
