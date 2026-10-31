#pragma once
#include <QObject>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>
class Bridge : public QObject {
  Q_OBJECT
  QML_ELEMENT
  Q_PROPERTY(QVariantMap snapshot READ snapshot NOTIFY changed)
public:
  explicit Bridge(QObject *parent = nullptr);
  QVariantMap snapshot() const { return m_snapshot; }
  Q_INVOKABLE void enable();
  Q_INVOKABLE void confirmEnable(bool suppress);
  Q_INVOKABLE void disable();
  Q_INVOKABLE void addTime(uint seconds);
  Q_INVOKABLE void setDuration(uint seconds);
  Q_INVOKABLE void resetWarning();
  Q_INVOKABLE void refresh();
  Q_INVOKABLE void pickFiles();
  Q_INVOKABLE void receiveFiles();
  Q_INVOKABLE void sendFiles(const QStringList &urls);
  Q_INVOKABLE void forgetRecipient();
  Q_INVOKABLE void sendTo(const QString &peer);
  Q_INVOKABLE void browse();
  Q_INVOKABLE void openNetworkSettings();
signals:
  void changed();
  void warningRequired();
private slots:
  void handleWarningRequired();
  void update(QVariantMap snapshot);

private:
  void invoke(const QString &method, const QVariantList &arguments = {});
  QVariantMap m_snapshot{{"state", "unavailable"},
                         {"detail", "Install feather-session and its D-Bus "
                                    "activation file, then reopen Feather."},
                         {"busy", false},
                         {"owned", false},
                         {"duration", 600},
                         {"showWarning", true},
                         {"deadline", 0}};
  quint64 m_epoch = 0;
};
