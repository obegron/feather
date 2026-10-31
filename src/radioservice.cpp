#include "radioservice.h"
#include <KConfigGroup>
#include <QDBusArgument>
#include <QDBusConnectionInterface>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusServiceWatcher>
#include <QDateTime>
#include <QTimer>
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
  if (m_duration != 0 && m_duration != 600 && m_duration != 1800)
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
  return {{"state", m_state},         {"detail", m_detail},
          {"owned", m_lease != 0},    {"duration", m_duration},
          {"showWarning", m_warning}, {"deadline", m_deadline},
          {"busy", m_pending}};
}
void RadioService::publish() { emit Changed(Snapshot()); }
void RadioService::ownerChanged(const QString &, const QString &,
                                const QString &owner) {
  ++m_epoch;
  m_owner = owner;
  m_lease = 0;
  m_deadline = 0;
  m_pending = false;
  m_refreshing = false;
  m_state = "unavailable";
  m_detail = "Owly service changed. Radio status is being checked.";
  publish();
  reconcile();
}
void RadioService::radioChanged(qulonglong, const QString &, const QString &) {
  reconcile();
}
void RadioService::Refresh() { reconcile(); }
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
    const auto map = reply.value();
    const auto lease = map.value("Lease").toULongLong();
    const auto state = map.value("State").toString();
    if (m_lease && lease != m_lease) {
      m_lease = 0;
      m_deadline = 0;
    }
    if (state == "idle") {
      m_state = "off";
      m_lease = 0;
      m_deadline = 0;
    } else if (m_lease && lease == m_lease)
      m_state = state == "up" ? "on" : state;
    else
      m_state = "external";
    // Preserve errors until a new user action; the state still follows the
    // daemon.
    publish();
  });
}
void RadioService::Enable() {
  if (m_pending || m_lease || (m_state != "off" && m_state != "error"))
    return;
  if (m_warning) {
    emit WarningRequired();
    return;
  }
  acquire();
}
void RadioService::ConfirmEnable(bool suppress) {
  if (m_pending || m_lease || (m_state != "off" && m_state != "error"))
    return;
  if (suppress) {
    m_warning = false;
    KConfigGroup(m_config, "Radio").writeEntry("ShowWarning", false);
    m_config->sync();
  }
  acquire();
}
void RadioService::acquire() {
  ++m_epoch;
  m_refreshing = false;
  m_pending = true;
  m_state = "starting";
  m_detail.clear();
  publish();
  const auto epoch = m_epoch;
  const auto duration = m_duration;
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
            m_lease = reply.argumentAt<0>();
            m_deadline =
                duration ? QDateTime::currentSecsSinceEpoch() + duration : 0;
            m_state = "on";
            publish();
            reconcile();
          });
}
void RadioService::Disable() {
  if (m_pending || !m_lease)
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
      m_state = "off";
    }
    publish();
    reconcile();
  });
}
void RadioService::SetDuration(uint seconds) {
  if (seconds != 0 && seconds != 600 && seconds != 1800)
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
