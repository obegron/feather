#pragma once
#include <KJob>
#include <QJsonObject>
#include <QPointer>
#include <QProcess>
#include <QVariantMap>
#include <functional>
class RadioService;
class KNotification;
class TransferJob : public KJob {
  Q_OBJECT
public:
  TransferJob(QString title, QObject *parent);
  void start() override;
  void progress(qulonglong done, qulonglong total);
  void finish(const QString &error = {}, bool cancelled = false);
signals:
  void cancellationRequested();

protected:
  bool doKill() override;

private:
  QString m_title;
  bool m_finished = false;
};
class TransferManager : public QObject {
  friend class TransferTest;
  Q_OBJECT
public:
  explicit TransferManager(RadioService *radio);
  void start(const QVariantMap &link);
  void stop();
  void sendFiles(const QStringList &urls);
  void pickFiles();
  void pickForPeer(const QString &peer);
  void receiveFiles();
  void browse();

private:
  struct Entry {
    QString name, direction, state;
    QPointer<TransferJob> job;
    QPointer<KNotification> prompt;
  };
  void processEnded(QProcess *process);
  void command(const QJsonObject &message);
  void event(const QJsonObject &message);
  void status();
  void terminal(const QString &id, const QString &error = {},
                bool cancelled = false);
  void accept(const QString &id);
  void decline(const QString &id);
  bool startSend(const QString &peer, const QStringList &paths);
  void selectPeer(const QStringList &paths);
  void beginSharing(const QStringList &paths, const QString &peer = {});
  void advanceSharing();
  void confirmSharing();
  void notify(const QString &title, const QString &text);
  bool accepting() const;
  RadioService *m_radio;
  QPointer<QProcess> m_process;
  QByteArray m_buffer;
  QHash<QString, QVariantMap> m_peers;
  QHash<QString, Entry> m_entries;
  bool m_ready = false, m_accepting = true, m_preparing = false,
       m_stopping = false;
  QVariantMap m_restartLink;
  QString m_error, m_destination;
  QStringList m_pendingPaths;
  QString m_preselectedPeer;
  bool m_sharePending = false, m_shareRenewRequested = false;
  QPointer<QObject> m_shareWarning, m_peerPicker;
};
