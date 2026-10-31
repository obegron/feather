#include "radioservice.h"
#include <KConfigGroup>
#include <QDBusArgument>
#include <QDBusConnectionInterface>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusServiceWatcher>
#include <QDateTime>
#include <QTimer>
#include <QXmlStreamReader>
#include <limits>
static const QString name = QStringLiteral("io.github.t4t5.Owly");
static const QString path = QStringLiteral("/io/github/t4t5/Owly");
static const QString iface = QStringLiteral("io.github.t4t5.Owly.Radio1");
static QDBusMessage call(const QString &method) {
  return QDBusMessage::createMethodCall(name, path, iface, method);
}
RadioService::RadioService(QDBusConnection bus, QObject *parent)
    : QObject(parent), m_bus(bus),
      m_config(KSharedConfig::openConfig("featherrc")) {
  KConfigGroup settings(m_config, "Radio");
  m_duration = settings.readEntry("Duration", 600u);
  if (m_duration != 0 && m_duration != 300 && m_duration != 600)
    m_duration = 600;
  m_warning = settings.readEntry("ShowWarning", true);
  auto *watch = new QDBusServiceWatcher(
      name, m_bus, QDBusServiceWatcher::WatchForOwnerChange, this);
  connect(watch, &QDBusServiceWatcher::serviceOwnerChanged, this,
          &RadioService::ownerChanged);
  m_bus.connect(name, path, iface, "StateChanged", this,
                SLOT(radioChanged(qulonglong, QString, QString)));
  auto *timer = new QTimer(this);
  timer->setInterval(2000);
  connect(timer, &QTimer::timeout, this, &RadioService::Refresh);
  timer->start();
  QTimer::singleShot(0, this, &RadioService::Refresh);
}
QVariantMap RadioService::Snapshot() const {
  auto status = m_transferStatus;
  const QVariantMap radio{{"state", m_state},
                          {"detail", m_detail},
                          {"owned", m_lease != 0},
                          {"duration", m_duration},
                          {"showWarning", m_warning},
                          {"deadline", m_deadline},
                          {"activeDuration", m_activeDuration},
                          {"canRenew", m_renewSupported},
                          {"timerKnown", !m_timerUncertain},
                          {"busy", m_pending},
                          {"transferCount", m_transferCount}};
  if (status.contains("receiving"))
    status["receiving"] =
        status.value("receiving").toBool() && m_lease && m_state == "on" &&
        !m_timerUncertain &&
        (!m_deadline || m_deadline > QDateTime::currentSecsSinceEpoch());
  for (auto it = radio.cbegin(); it != radio.cend(); ++it)
    status[it.key()] = it.value();
  return status;
}
void RadioService::publish() { emit Changed(Snapshot()); }
void RadioService::ownerChanged(const QString &, const QString &,
                                const QString &owner) {
  ++m_epoch;
  m_owner = owner;
  m_renewChecked = false;
  m_renewSupported = false;
  m_lease = 0;
  m_deadline = 0;
  m_activeDuration = 0;
  m_timerUncertain = false;
  m_pending = false;
  m_refreshing = false;
  m_state = "unavailable";
  m_transferLeaseExtended = false;
  emit RadioEnded();
  m_detail = "Owly service changed. Radio status is being checked.";
  publish();
  reconcile();
}
void RadioService::radioChanged(qulonglong, const QString &, const QString &) {
  reconcile();
}
void RadioService::Refresh() {
  if (m_deadline && QDateTime::currentSecsSinceEpoch() >= m_deadline &&
      !m_discoveryExpired) {
    m_discoveryExpired = true;
    emit DiscoveryExpired();
  }
  if (!m_transferCount && m_transferLeaseExtended)
    restoreTimer();
  reconcile();
}
void RadioService::reconcile() {
  if (m_pending || m_refreshing)
    return;
  m_refreshing = true;
  const auto epoch = m_epoch;
  auto msg = QDBusMessage::createMethodCall(
      name, path, "org.freedesktop.DBus.Properties", "GetAll");
  msg << iface;
  auto *w = new QDBusPendingCallWatcher(m_bus.asyncCall(msg), this);
  connect(w, &QDBusPendingCallWatcher::finished, this, [this, w, epoch] {
    QDBusPendingReply<QVariantMap> reply = *w;
    w->deleteLater();
    if (epoch != m_epoch)
      return;
    m_refreshing = false;
    if (m_pending)
      return;
    if (reply.isError()) {
      m_state = "unavailable";
      m_detail = reply.error().message();
      publish();
      return;
    }
    checkRenewal();
    const auto map = reply.value();
    const auto lease = map.value("Lease").toULongLong();
    const auto state = map.value("State").toString();
    if (m_lease && lease != m_lease) {
      m_transferLeaseExtended = false;
      m_lease = 0;
      m_deadline = 0;
      m_activeDuration = 0;
      m_timerUncertain = false;
      emit RadioEnded();
    }
    if (state == "idle") {
      m_state = "off";
      m_lease = 0;
      m_deadline = 0;
      m_activeDuration = 0;
      m_timerUncertain = false;
    } else if (m_lease && lease == m_lease)
      m_state = state == "up" ? "on" : state;
    else
      m_state = "external";
    // Preserve errors until a new user action; the state still follows the
    // daemon.
    publish();
  });
}
void RadioService::checkRenewal() {
  if (m_renewChecked)
    return;
  m_renewChecked = true;
  const auto owner = m_owner;
  auto msg = QDBusMessage::createMethodCall(
      name, path, "org.freedesktop.DBus.Introspectable", "Introspect");
  auto *w = new QDBusPendingCallWatcher(m_bus.asyncCall(msg), this);
  connect(w, &QDBusPendingCallWatcher::finished, this, [this, w, owner] {
    QDBusPendingReply<QString> reply = *w;
    w->deleteLater();
    if (owner != m_owner)
      return;
    if (reply.isError()) {
      m_renewChecked = false;
      return;
    }
    QXmlStreamReader xml(reply.value());
    bool radioInterface = false;
    while (!xml.atEnd()) {
      xml.readNext();
      if (xml.isStartElement() && xml.name() == QLatin1String("interface"))
        radioInterface = xml.attributes().value("name") == iface;
      if (xml.isStartElement() && radioInterface &&
          xml.name() == QLatin1String("method") &&
          xml.attributes().value("name") == QLatin1String("Renew")) {
        QString inputs, outputs;
        while (xml.readNextStartElement()) {
          if (xml.name() == QLatin1String("arg")) {
            const auto type = xml.attributes().value("type").toString();
            if (xml.attributes().value("direction") == QLatin1String("out"))
              outputs += type;
            else
              inputs += type;
          }
          xml.skipCurrentElement();
        }
        m_renewSupported = inputs == QLatin1String("tu") && outputs.isEmpty();
      }
      if (xml.isEndElement() && xml.name() == QLatin1String("interface"))
        radioInterface = false;
    }
    if (xml.hasError())
      m_renewSupported = false;
    publish();
  });
}
void RadioService::AddTime(uint seconds) {
  if (seconds != 0 && seconds != 300 && seconds != 600 && seconds != 1800)
    return;
  if (m_pending || !m_lease || m_state != "on" || !m_deadline ||
      m_timerUncertain)
    return;
  if (!m_renewSupported) {
    m_detail = "Adding time requires owlyd with the Radio1 Renew method.";
    publish();
    return;
  }
  const auto sentAt = QDateTime::currentSecsSinceEpoch();
  const auto left = m_deadline - sentAt;
  if (left <= 0) {
    reconcile();
    return;
  }
  const auto next = seconds ? left + seconds : 0;
  if (next > std::numeric_limits<uint>::max())
    return;
  ++m_epoch;
  m_refreshing = false;
  m_pending = true;
  m_detail.clear();
  publish();
  const auto epoch = m_epoch;
  auto msg = call("Renew");
  msg << m_lease << static_cast<uint>(next);
  auto *w = new QDBusPendingCallWatcher(m_bus.asyncCall(msg), this);
  connect(w, &QDBusPendingCallWatcher::finished, this,
          [this, w, epoch, next, seconds, sentAt] {
            QDBusPendingReply<> reply = *w;
            w->deleteLater();
            if (epoch != m_epoch)
              return;
            m_pending = false;
            if (reply.isError()) {
              m_detail = reply.error().message();
              const auto error = reply.error().name();
              if (error == "org.freedesktop.DBus.Error.NoReply" ||
                  error == "org.freedesktop.DBus.Error.Timeout" ||
                  error == "org.freedesktop.DBus.Error.TimedOut" ||
                  error == "org.freedesktop.DBus.Error.Disconnected") {
                m_timerUncertain = true;
                m_detail =
                    "Owly did not confirm the timer change. The radio may "
                    "still be active, but its timeout is unknown. Turn it "
                    "off before starting a new timed session.";
              }
            } else {
              m_timerUncertain = false;
              m_deadline = next ? sentAt + next : 0;
              m_activeDuration = next ? m_activeDuration + seconds : 0;
            }
            publish();
            reconcile();
          });
}
void RadioService::Enable() {
  if (m_pending || m_lease || m_transferCount ||
      (m_state != "off" && m_state != "error"))
    return;
  if (m_warning) {
    emit WarningRequired();
    return;
  }
  acquire(m_duration);
}
void RadioService::ConfirmEnable(bool suppress) {
  if (m_pending || m_lease || m_transferCount ||
      (m_state != "off" && m_state != "error"))
    return;
  if (suppress) {
    m_warning = false;
    KConfigGroup(m_config, "Radio").writeEntry("ShowWarning", false);
    m_config->sync();
  }
  acquire(m_duration);
}
void RadioService::acquire(uint duration) {
  ++m_epoch;
  m_refreshing = false;
  m_pending = true;
  m_state = "starting";
  m_detail.clear();
  publish();
  const auto epoch = m_epoch;
  auto msg = call("Acquire");
  msg << duration;
  auto *w = new QDBusPendingCallWatcher(m_bus.asyncCall(msg, 120000), this);
  connect(w, &QDBusPendingCallWatcher::finished, this,
          [this, w, epoch, duration] {
            QDBusPendingReply<qulonglong, QVariantMap> reply = *w;
            w->deleteLater();
            if (epoch != m_epoch)
              return;
            m_pending = false;
            if (reply.isError()) {
              m_state = "error";
              m_detail = reply.error().message();
              publish();
              reconcile();
              return;
            }
            m_transferLeaseExtended = false;
            m_discoveryExpired = false;
            m_lease = reply.argumentAt<0>();
            m_activeDuration = duration;
            m_timerUncertain = false;
            m_deadline =
                duration ? QDateTime::currentSecsSinceEpoch() + duration : 0;
            m_state = "on";
            emit LinkReady(reply.argumentAt<1>());
            publish();
            reconcile();
          });
}
void RadioService::Disable() {
  if (m_pending || !m_lease || m_transferCount)
    return;
  ++m_epoch;
  m_refreshing = false;
  m_pending = true;
  m_state = "stopping";
  m_detail.clear();
  publish();
  const auto epoch = m_epoch;
  auto msg = call("Release");
  msg << m_lease;
  auto *w = new QDBusPendingCallWatcher(m_bus.asyncCall(msg, 120000), this);
  connect(w, &QDBusPendingCallWatcher::finished, this, [this, w, epoch] {
    QDBusPendingReply<> reply = *w;
    w->deleteLater();
    if (epoch != m_epoch)
      return;
    m_pending = false;
    if (reply.isError()) {
      m_state = "error";
      m_detail = reply.error().message();
    } else {
      m_lease = 0;
      m_deadline = 0;
      m_activeDuration = 0;
      m_timerUncertain = false;
      m_transferLeaseExtended = false;
      m_state = "off";
      emit RadioEnded();
    }
    publish();
    reconcile();
  });
}
void RadioService::SetDuration(uint seconds) {
  if (seconds != 0 && seconds != 300 && seconds != 600 && seconds != 1800)
    return;
  m_duration = seconds;
  KConfigGroup(m_config, "Radio").writeEntry("Duration", seconds);
  m_config->sync();
  publish();
}
void RadioService::ResetWarning() {
  m_warning = true;
  KConfigGroup(m_config, "Radio").writeEntry("ShowWarning", true);
  m_config->sync();
  publish();
}

void RadioService::PickFiles() { emit PickFilesRequested(); }
void RadioService::PickFilesForPeer(const QString &peer) {
  emit PickFilesForPeerRequested(peer);
}
void RadioService::SendFiles(const QStringList &urls) {
  emit SendFilesRequested(urls);
}
void RadioService::Browse() { emit BrowseRequested(); }
void RadioService::setTransferStatus(const QVariantMap &status) {
  m_transferStatus = status;
  publish();
}
void RadioService::setTransferCount(int count) {
  m_transferCount = qMax(0, count);
  publish();
  if (!m_transferCount && m_transferLeaseExtended)
    restoreTimer();
}
void RadioService::protectTransfers(std::function<void(bool)> callback) {
  if (!m_lease || m_state != "on" || m_pending || m_timerUncertain) {
    callback(false);
    return;
  }
  if (!m_deadline) {
    callback(true);
    return;
  }
  if (!m_renewSupported) {
    callback(false);
    return;
  }
  const auto seconds =
      qMax<qint64>(120, m_deadline - QDateTime::currentSecsSinceEpoch());
  const auto epoch = m_epoch;
  m_pending = true;
  publish();
  auto msg = call("Renew");
  msg << m_lease << static_cast<uint>(seconds);
  auto *w = new QDBusPendingCallWatcher(m_bus.asyncCall(msg), this);
  connect(w, &QDBusPendingCallWatcher::finished, this,
          [this, w, epoch, callback] {
            QDBusPendingReply<> reply = *w;
            w->deleteLater();
            if (epoch != m_epoch) {
              callback(false);
              return;
            }
            m_pending = false;
            const bool ok = !reply.isError();
            if (ok)
              m_transferLeaseExtended = true;
            else
              m_detail = "Could not keep the radio available for a transfer: " +
                         reply.error().message();
            callback(ok);
            publish();
            if (!m_transferCount && m_transferLeaseExtended)
              restoreTimer();
          });
}
void RadioService::restoreTimer() {
  if (m_pending || m_transferCount || !m_lease || !m_transferLeaseExtended)
    return;
  if (m_deadline && QDateTime::currentSecsSinceEpoch() >= m_deadline) {
    Disable();
    return;
  }
  const auto seconds =
      m_deadline ? m_deadline - QDateTime::currentSecsSinceEpoch() : 0;
  if (m_deadline && seconds <= 0) {
    Disable();
    return;
  }
  const auto epoch = m_epoch;
  m_pending = true;
  auto msg = call("Renew");
  msg << m_lease << static_cast<uint>(seconds);
  auto *w = new QDBusPendingCallWatcher(m_bus.asyncCall(msg), this);
  connect(w, &QDBusPendingCallWatcher::finished, this, [this, w, epoch] {
    QDBusPendingReply<> reply = *w;
    w->deleteLater();
    if (epoch != m_epoch)
      return;
    m_pending = false;
    if (!reply.isError())
      m_transferLeaseExtended = false;
    else
      m_detail =
          "Could not restore the radio timeout: " + reply.error().message();
    publish();
  });
}

bool RadioService::startSharing(bool confirmed, bool suppressWarning) {
  if (m_pending)
    return m_state == "starting";
  if (m_lease) {
    if (m_state != "on" || m_timerUncertain)
      return false;
    if (!m_deadline)
      return true;
    if (!m_renewSupported)
      return false;
    AddTime(0);
    return true;
  }
  if (m_transferCount || (m_state != "off" && m_state != "error"))
    return false;
  if (m_warning && !confirmed) {
    emit ShareWarningRequired();
    return true;
  }
  if (confirmed && suppressWarning) {
    m_warning = false;
    KConfigGroup(m_config, "Radio").writeEntry("ShowWarning", false);
    m_config->sync();
  }
  acquire(0);
  return true;
}
void RadioService::ReceiveFiles() { emit ReceiveFilesRequested(); }
void RadioService::ForgetRecipient() { emit ForgetRecipientRequested(); }
