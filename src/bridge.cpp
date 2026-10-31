#include "bridge.h"
#include <QClipboard>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusServiceWatcher>
#include <QFileInfo>
#include <QGuiApplication>
#include <QMimeData>
#include <QProcess>
#include <QTimer>
#include <QUrl>
static const QString service = QStringLiteral("io.github.obegron.feather");
static const QString path = QStringLiteral("/io/github/obegron/feather");
static const QString iface = QStringLiteral("io.github.obegron.feather.Radio1");
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
      m_snapshot["detail"] = reply.error().message() +
                             " Install/start feather-session via its "
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
void Bridge::addTime(uint seconds) { invoke("AddTime", {seconds}); }
void Bridge::setDuration(uint seconds) { invoke("SetDuration", {seconds}); }
void Bridge::resetWarning() { invoke("ResetWarning"); }
void Bridge::openNetworkSettings() {
  QProcess::startDetached("systemsettings", {"kcm_networkmanagement"});
}

void Bridge::pickFiles() { invoke("PickFiles"); }
void Bridge::sendTo(const QString &peer) { invoke("PickFilesForPeer", {peer}); }
void Bridge::browse() { invoke("Browse"); }

void Bridge::receiveFiles() { invoke("ReceiveFiles"); }
void Bridge::sendFiles(const QStringList &urls) { invoke("SendFiles", {urls}); }
void Bridge::forgetRecipient() { invoke("ForgetRecipient"); }

bool Bridge::pasteFiles() {
  // Read from the focused applet, not the background session service: on
  // Wayland clipboard access belongs to the client with keyboard focus.
  const auto *data =
      QGuiApplication::clipboard()->mimeData(QClipboard::Clipboard);
  if (!data || !data->hasUrls() || data->urls().isEmpty())
    return false;
  QStringList urls;
  for (const auto &url : data->urls()) {
    if (!url.isLocalFile() || !QFileInfo(url.toLocalFile()).isFile())
      return false;
    urls << url.toString();
  }
  sendFiles(urls);
  return true;
}
