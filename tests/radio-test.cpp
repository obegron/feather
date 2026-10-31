#include "radioservice.h"
#include <KConfigGroup>
#include <QDBusContext>
#include <QDBusMessage>
#include <QDBusPendingReply>
#include <QtTest>
class Fake : public QObject, protected QDBusContext {
  Q_OBJECT
  Q_CLASSINFO("D-Bus Interface", "io.github.t4t5.Owly.Radio1")
  Q_PROPERTY(QString State READ state)
  Q_PROPERTY(qulonglong Lease READ lease)
public:
  QString current = "idle";
  qulonglong id = 0;
  uint duration = 999;
  int acquisitions = 0, releases = 0;
  bool failAcquire = false, failRelease = false;
  QString state() const { return current; }
  qulonglong lease() const { return id; }
public slots:
  qulonglong Acquire(uint seconds, QVariantMap &link) {
    ++acquisitions;
    duration = seconds;
    if (failAcquire) {
      sendErrorReply("io.github.t4t5.Owly.Error.NotAuthorized",
                     "Denied by fake");
      return 0;
    }
    id = 42;
    current = "up";
    link = {{"interface", "awdl0"}};
    emit StateChanged(id, current, "");
    return id;
  }
  void Release(qulonglong value) {
    ++releases;
    if (value != id || failRelease) {
      sendErrorReply("io.github.t4t5.Owly.Error.StaleLease", "Release failed");
      return;
    }
    end();
  }
  void end() {
    const auto old = id;
    id = 0;
    current = "idle";
    emit StateChanged(old, current, "expired");
  }
signals:
  void StateChanged(qulonglong lease, QString state, QString reason);
};
class RadioTest : public QObject {
  Q_OBJECT
private slots:
  void lifecycle() {
    auto config = KSharedConfig::openConfig("featherrc");
    config->deleteGroup("Radio");
    config->sync();
    auto bus = QDBusConnection::sessionBus();
    QVERIFY(bus.isConnected());
    Fake fake;
    QVERIFY(bus.registerService("io.github.t4t5.Owly"));
    QVERIFY(bus.registerObject("/io/github/t4t5/Owly", &fake,
                               QDBusConnection::ExportAllSlots |
                                   QDBusConnection::ExportAllSignals |
                                   QDBusConnection::ExportAllProperties));
    RadioService radio(bus);
    QSignalSpy warning(&radio, &RadioService::WarningRequired);
    QTRY_COMPARE(radio.Snapshot()["state"].toString(), QString("off"));
    QCOMPARE(radio.Snapshot()["duration"].toUInt(), 600u);
    radio.Enable();
    QCOMPARE(warning.count(), 1);
    QCOMPARE(fake.acquisitions, 0);
    // Cancellation does not mutate suppression. A second request still warns.
    radio.Enable();
    QCOMPARE(warning.count(), 2);
    QVERIFY(radio.Snapshot()["showWarning"].toBool());
    radio.ConfirmEnable(true);
    radio.ConfirmEnable(true);
    QTRY_COMPARE(radio.Snapshot()["state"].toString(), QString("on"));
    QCOMPARE(fake.acquisitions, 1);
    QCOMPARE(fake.duration, 600u);
    QVERIFY(radio.Snapshot()["owned"].toBool());
    QVERIFY(radio.Snapshot()["deadline"].toLongLong() > 0);
    radio.SetDuration(1800);
    QCOMPARE(fake.duration, 600u);
    fake.failRelease = true;
    radio.Disable();
    QTRY_VERIFY(!radio.Snapshot()["busy"].toBool());
    QTRY_COMPARE(radio.Snapshot()["state"].toString(), QString("on"));
    QVERIFY(radio.Snapshot()["owned"].toBool());
    fake.failRelease = false;
    radio.Disable();
    QTRY_COMPARE(radio.Snapshot()["state"].toString(), QString("off"));
    QCOMPARE(fake.releases, 2);
    radio.Enable();
    QTRY_COMPARE(radio.Snapshot()["state"].toString(), QString("on"));
    QCOMPARE(fake.duration, 1800u);
    fake.end();
    QTRY_COMPARE(radio.Snapshot()["state"].toString(), QString("off"));
    QVERIFY(!radio.Snapshot()["owned"].toBool());
    radio.SetDuration(0);
    radio.Enable();
    QTRY_COMPARE(radio.Snapshot()["state"].toString(), QString("on"));
    QCOMPARE(fake.duration, 0u);
    QCOMPARE(radio.Snapshot()["deadline"].toLongLong(), 0);
    fake.end();
    QTRY_COMPARE(radio.Snapshot()["state"].toString(), QString("off"));
    fake.id = 97;
    fake.current = "up";
    radio.Refresh();
    QTRY_COMPARE(radio.Snapshot()["state"].toString(), QString("external"));
    const int releases = fake.releases;
    radio.Disable();
    radio.Enable();
    QTest::qWait(30);
    QCOMPARE(fake.releases, releases);
    QVERIFY(!radio.Snapshot()["owned"].toBool());
    fake.end();
    QTRY_COMPARE(radio.Snapshot()["state"].toString(), QString("off"));
    fake.failAcquire = true;
    radio.Enable();
    QTRY_VERIFY(!radio.Snapshot()["busy"].toBool());
    QVERIFY(!radio.Snapshot()["owned"].toBool());
    QVERIFY(radio.Snapshot()["detail"].toString().contains("Denied"));
    fake.failAcquire = false;
    radio.Enable();
    QTRY_COMPARE(radio.Snapshot()["state"].toString(), QString("on"));
    QVERIFY(bus.unregisterService("io.github.t4t5.Owly"));
    QTRY_COMPARE(radio.Snapshot()["state"].toString(), QString("unavailable"));
    QVERIFY(!radio.Snapshot()["owned"].toBool());
    fake.id = 0;
    fake.current = "idle";
    QVERIFY(bus.registerService("io.github.t4t5.Owly"));
    QTRY_COMPARE(radio.Snapshot()["state"].toString(), QString("off"));
    const int count = fake.acquisitions;
    QTest::qWait(50);
    QCOMPARE(fake.acquisitions, count);
    radio.ResetWarning();
    radio.Enable();
    QCOMPARE(warning.count(), 3);
    QVERIFY(radio.Snapshot()["showWarning"].toBool());
    RadioService second(bus);
    QCOMPARE(second.Snapshot()["duration"].toUInt(), 0u);
    QVERIFY(second.Snapshot()["showWarning"].toBool());
    bus.unregisterObject("/io/github/t4t5/Owly");
    bus.unregisterService("io.github.t4t5.Owly");
  }
};
QTEST_GUILESS_MAIN(RadioTest)
#include "radio-test.moc"
