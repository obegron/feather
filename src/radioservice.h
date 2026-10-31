#pragma once
#include <KSharedConfig>
#include <QDBusConnection>
#include <QObject>
#include <QVariantMap>
#include <functional>
class RadioService : public QObject {
  friend class RadioTest;
  Q_OBJECT
  Q_CLASSINFO("D-Bus Interface", "org.kde.Feather.Radio1")
public:
  explicit RadioService(QDBusConnection bus, QObject *parent = nullptr);
  void setTransferCount(int count);
  void setTransferStatus(const QVariantMap &status);
  void protectTransfers(std::function<void(bool)> callback);
  bool startSharing(bool confirmed = false, bool suppressWarning = false);
public slots:
  QVariantMap Snapshot() const;
  void Enable();
  void ConfirmEnable(bool suppressWarning);
  void Disable();
  void AddTime(uint seconds);
  void SetDuration(uint seconds);
  void ResetWarning();
  void Refresh();
  void PickFiles();
  void ReceiveFiles();
  void ForgetRecipient();
  void PickFilesForPeer(const QString &peer);
  void SendFiles(const QStringList &urls);
  void Browse();
signals:
  void Changed(QVariantMap snapshot);
  void WarningRequired();
  void ShareWarningRequired();
  void ReceiveFilesRequested();
  void ForgetRecipientRequested();
  void LinkReady(QVariantMap link);
  void RadioEnded();
  void DiscoveryExpired();
  void PickFilesRequested();
  void PickFilesForPeerRequested(QString peer);
  void SendFilesRequested(QStringList urls);
  void BrowseRequested();
private slots:
  void ownerChanged(const QString &, const QString &, const QString &);
  void radioChanged(qulonglong, const QString &, const QString &);

private:
  void acquire(uint duration);
  void publish();
  void reconcile();
  void checkRenewal();
  void restoreTimer();
  QDBusConnection m_bus;
  KSharedConfig::Ptr m_config;
  QString m_state = "unavailable", m_detail;
  QString m_owner;
  qulonglong m_lease = 0;
  qint64 m_deadline = 0;
  uint m_duration = 600;
  qint64 m_activeDuration = 0;
  bool m_renewSupported = false, m_renewChecked = false;
  bool m_timerUncertain = false;
  bool m_warning = true, m_pending = false, m_refreshing = false;
  quint64 m_epoch = 0;
  int m_transferCount = 0;
  QVariantMap m_transferStatus;
  bool m_transferLeaseExtended = false, m_discoveryExpired = false;
};
