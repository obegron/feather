#pragma once
#include <KSharedConfig>
#include <QDBusConnection>
#include <QObject>
#include <QVariantMap>
class RadioService : public QObject {
  Q_OBJECT
  Q_CLASSINFO("D-Bus Interface", "org.kde.Feather.Radio1")
public:
  explicit RadioService(QDBusConnection bus, QObject *parent = nullptr);
public slots:
  QVariantMap Snapshot() const;
  void Enable();
  void ConfirmEnable(bool suppressWarning);
  void Disable();
  void SetDuration(uint seconds);
  void ResetWarning();
  void Refresh();
signals:
  void Changed(QVariantMap snapshot);
  void WarningRequired();
private slots:
  void ownerChanged(const QString &, const QString &, const QString &);
  void radioChanged(qulonglong, const QString &, const QString &);

private:
  void acquire();
  void publish();
  void reconcile();
  QDBusConnection m_bus;
  KSharedConfig::Ptr m_config;
  QString m_state = "unavailable", m_detail;
  QString m_owner;
  qulonglong m_lease = 0;
  qint64 m_deadline = 0;
  uint m_duration = 600, m_activeDuration = 0;
  bool m_warning = true, m_pending = false, m_refreshing = false;
  quint64 m_epoch = 0;
};
