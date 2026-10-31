// SPDX-License-Identifier: MIT
#include <KPluginFactory>
#include <Purpose/PluginBase>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QFileInfo>
#include <QJsonArray>
#include <QTimer>
#include <QUrl>

class FeatherShareJob : public Purpose::Job {
  Q_OBJECT
public:
  using Purpose::Job::Job;
  void start() override {
    // This job launches Feather's sharing flow. The session service owns the
    // actual transfer KJobs and reports their progress and final outcomes.
    QTimer::singleShot(0, this, [this] {
      QStringList urls;
      for (const auto &value : data().value("urls").toArray()) {
        const QUrl url(value.toString());
        if (!url.isLocalFile() || !QFileInfo(url.toLocalFile()).isFile()) {
          fail(tr("Feather can share local files only."));
          return;
        }
        urls << url.toString();
      }
      if (urls.isEmpty()) {
        fail(tr("No files selected."));
        return;
      }
      auto request = QDBusMessage::createMethodCall(
          "io.github.obegron.feather", "/io/github/obegron/feather",
          "io.github.obegron.feather.Radio1", "SendFiles");
      request << urls;
      auto *watcher = new QDBusPendingCallWatcher(
          QDBusConnection::sessionBus().asyncCall(request), this);
      connect(watcher, &QDBusPendingCallWatcher::finished, this,
              [this, watcher] {
                QDBusPendingReply<> reply = *watcher;
                watcher->deleteLater();
                if (reply.isError()) {
                  fail(reply.error().message());
                  return;
                }
                setOutput({{"url", QString()}});
                emitResult();
              });
    });
  }

private:
  void fail(const QString &message) {
    setError(UserDefinedError);
    setErrorText(message);
    emitResult();
  }
};
class FeatherSharePlugin : public Purpose::PluginBase {
  Q_OBJECT
public:
  using Purpose::PluginBase::PluginBase;
  Purpose::Job *createJob() const override { return new FeatherShareJob; }
};
K_PLUGIN_CLASS_WITH_JSON(FeatherSharePlugin, "purposeplugin.json")
#include "purposeplugin.moc"
