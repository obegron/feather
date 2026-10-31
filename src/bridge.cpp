#include "bridge.h"
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusServiceWatcher>
#include <QProcess>
#include <QTimer>
static const QString service = QStringLiteral("org.kde.Feather");
static const QString path = QStringLiteral("/org/kde/Feather");
static const QString iface = QStringLiteral("org.kde.Feather.Radio1");
Bridge::Bridge(QObject *parent) : QObject(parent) {
  auto bus = QDBusConnection::sessionBus();
  bus.connect(service, path, iface, "Changed", this, SLOT(update(QVariantMap)));
  bus.connect(service, path, iface, "WarningRequired", this,
              SLOT(handleWarningRequired()));
  auto *watch = new QDBusServiceWatcher(
      service, bus, QDBusServiceWatcher::WatchForOwnerChange, this);
  connect(watch, &QDBusServiceWatcher::serviceOwnerChanged, this,
          [this](const QString &, const QString &, const QString &owner) {
            ++m_epoch;
            if (owner.isEmpty()) {
              m_snapshot["state"] = "unavailable";
              m_snapshot["owned"] = false;
              m_snapshot["busy"] = false;
              m_snapshot["deadline"] = 0;
              m_snapshot["detail"] =
                  "Feather session service is unavailable. Install "
                  "feather-session and its D-Bus activation file.";
              emit changed();
            } else
              refresh();
          });
  QTimer::singleShot(0, this, &Bridge::refresh);
}
void Bridge::handleWarningRequired() { emit warningRequired(); }
void Bridge::update(QVariantMap snapshot) {
  m_snapshot = std::move(snapshot);
  emit changed();
}
void Bridge::refresh() {
  const auto epoch = m_epoch;
  auto msg = QDBusMessage::createMethodCall(service, path, iface, "Snapshot");
  auto *w = new QDBusPendingCallWatcher(
      QDBusConnection::sessionBus().asyncCall(msg), this);
  connect(w, &QDBusPendingCallWatcher::finished, this, [this, w, epoch] {
    QDBusPendingReply<QVariantMap> reply = *w;
    w->deleteLater();
    if (epoch != m_epoch)
      return;
    if (reply.isError()) {
      m_snapshot["state"] = "unavailable";
      m_snapshot["detail"] =
          reply.error().message() + " Install/start feather-session via its "
                                    "session D-Bus activation file.";
      emit changed();
    } else
      update(reply.value());
  });
  invoke("Refresh");
}
void Bridge::invoke(const QString &method, const QVariantList &args) {
  auto msg = QDBusMessage::createMethodCall(service, path, iface, method);
  msg.setArguments(args);
  auto *w = new QDBusPendingCallWatcher(
      QDBusConnection::sessionBus().asyncCall(msg), this);
  connect(w, &QDBusPendingCallWatcher::finished, this, [this, w] {
    QDBusPendingReply<> reply = *w;
    w->deleteLater();
    if (reply.isError()) {
      m_snapshot["detail"] = reply.error().message();
      emit changed();
    }
  });
}
void Bridge::enable() { invoke("Enable"); }
void Bridge::confirmEnable(bool suppress) {
  invoke("ConfirmEnable", {suppress});
}
void Bridge::disable() { invoke("Disable"); }
void Bridge::setDuration(uint seconds) { invoke("SetDuration", {seconds}); }
void Bridge::resetWarning() { invoke("ResetWarning"); }
void Bridge::openNetworkSettings() {
  QProcess::startDetached("systemsettings", {"kcm_networkmanagement"});
}
