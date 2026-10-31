#include "bridge.h"
#include "radioservice.h"
#include <KConfigGroup>
#include <QDBusContext>
#include <QDBusMessage>
#include <QDBusPendingReply>
#include <QDateTime>
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
  int acquisitions = 0, releases = 0, renewals = 0;
  uint renewedDuration = 0;
  bool failRenew = false, loseRenewReply = false;
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
  void Renew(qulonglong value, uint seconds) {
    ++renewals;
    if (value != id || failRenew) {
      sendErrorReply("io.github.t4t5.Owly.Error.StaleLease", "Renewal failed");
      return;
    }
    renewedDuration = seconds;
    if (loseRenewReply)
      sendErrorReply("org.freedesktop.DBus.Error.NoReply", "Renew reply lost");
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
    QVERIFY(bus.registerService("io.github.obegron.feather"));
    QVERIFY(bus.registerObject("/io/github/obegron/feather", &radio,
                               QDBusConnection::ExportAllSlots |
                                   QDBusConnection::ExportAllSignals));
    Bridge bridge;
    QSignalSpy bridgeWarning(&bridge, &Bridge::warningRequired);
    QTRY_COMPARE(bridge.snapshot()["state"].toString(), QString("off"));
    // Exercise the actual applet-to-service path, including D-Bus signal
    // delivery.
    bridge.enable();
    QTRY_COMPARE(bridgeWarning.count(), 1);
    QCOMPARE(fake.acquisitions, 0);

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
    QCOMPARE(radio.Snapshot()["activeDuration"].toUInt(), 600u);
    radio.SetDuration(1800);
    QCOMPARE(radio.Snapshot()["activeDuration"].toUInt(), 600u);
    QCOMPARE(fake.duration, 600u);
    QTRY_VERIFY(radio.Snapshot()["canRenew"].toBool());
    const auto originalDeadline = radio.Snapshot()["deadline"].toLongLong();
    // Go through the same D-Bus bridge as the UI.
    bridge.addTime(300);
    QTRY_COMPARE(fake.renewals, 1);
    QTRY_VERIFY(!radio.Snapshot()["busy"].toBool());
    QVERIFY(radio.Snapshot()["deadline"].toLongLong() >=
            originalDeadline + 300);
    QVERIFY(fake.renewedDuration >= 895 && fake.renewedDuration <= 900);
    QCOMPARE(radio.Snapshot()["activeDuration"].toUInt(), 900u);
    QCOMPARE(fake.acquisitions, 1);
    QCOMPARE(fake.releases, 0);
    const auto extendedDeadline = radio.Snapshot()["deadline"];
    fake.failRenew = true;
    radio.AddTime(600);
    radio.AddTime(600); // Duplicate requests while pending are ignored.
    QTRY_VERIFY(!radio.Snapshot()["busy"].toBool());
    QCOMPARE(radio.Snapshot()["deadline"], extendedDeadline);
    QCOMPARE(radio.Snapshot()["activeDuration"].toUInt(), 900u);
    QVERIFY(radio.Snapshot()["detail"].toString().contains("Renewal failed"));
    fake.failRenew = false;
    radio.AddTime(0);
    QTRY_VERIFY(!radio.Snapshot()["busy"].toBool());
    QCOMPARE(fake.renewedDuration, 0u);
    QCOMPARE(radio.Snapshot()["deadline"].toLongLong(), 0);
    QCOMPARE(radio.Snapshot()["activeDuration"].toUInt(), 0u);
    const auto renewals = fake.renewals;
    radio.AddTime(300); // Unlimited sessions expose only off, not add time.
    QCOMPARE(fake.renewals, renewals);
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
    radio.SetDuration(300);
    QCOMPARE(radio.Snapshot()["duration"].toUInt(), 300u);
    radio.SetDuration(123);
    QCOMPARE(radio.Snapshot()["duration"].toUInt(), 300u);
    radio.SetDuration(0);
    fake.id = 97;
    fake.current = "up";
    radio.Refresh();
    QTRY_COMPARE(radio.Snapshot()["state"].toString(), QString("external"));
    const int releases = fake.releases;
    radio.Disable();
    radio.AddTime(300);
    QCOMPARE(fake.renewals, renewals);
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
    radio.SetDuration(300);
    radio.ConfirmEnable(false);
    QTRY_COMPARE(radio.Snapshot()["state"].toString(), QString("on"));
    QTRY_VERIFY(radio.Snapshot()["canRenew"].toBool());
    radio.setTransferCount(1);
    const auto releasesBeforeTransfer = fake.releases;
    radio.Disable();
    QCOMPARE(fake.releases, releasesBeforeTransfer);
    bool protectedTransfer = false;
    radio.protectTransfers(
        [&protectedTransfer](bool ok) { protectedTransfer = ok; });
    QTRY_VERIFY(protectedTransfer);
    radio.setTransferCount(0);
    QTRY_VERIFY(!radio.Snapshot()["busy"].toBool());
    radio.setTransferCount(1);
    radio.protectTransfers(
        [&protectedTransfer](bool ok) { protectedTransfer = ok; });
    QTRY_VERIFY(!radio.Snapshot()["busy"].toBool());
    QSignalSpy expired(&radio, &RadioService::DiscoveryExpired);
    radio.m_deadline = QDateTime::currentSecsSinceEpoch() - 1;
    radio.Refresh();
    QCOMPARE(expired.count(), 1);
    QVERIFY(radio.Snapshot()["owned"].toBool());
    const auto releaseBeforeExpiry = fake.releases;
    radio.Disable();
    QCOMPARE(fake.releases, releaseBeforeExpiry);
    radio.setTransferCount(0);
    QTRY_COMPARE(radio.Snapshot()["state"].toString(), QString("off"));
    QCOMPARE(fake.releases, releaseBeforeExpiry + 1);
    radio.SetDuration(300);
    radio.ConfirmEnable(false);
    QTRY_COMPARE(radio.Snapshot()["state"].toString(), QString("on"));
    fake.loseRenewReply = true;
    radio.AddTime(
        0); // The daemon applied unlimited mode, but its reply was lost.
    QTRY_VERIFY(!radio.Snapshot()["busy"].toBool());
    QVERIFY(!radio.Snapshot()["timerKnown"].toBool());
    QVERIFY(radio.Snapshot()["owned"].toBool());
    const auto beforeUnknown = fake.renewals;
    radio.AddTime(300);
    QCOMPARE(fake.renewals, beforeUnknown);
    fake.loseRenewReply = false;
    fake.end();
    QTRY_COMPARE(radio.Snapshot()["state"].toString(), QString("off"));
    QVERIFY(radio.Snapshot()["timerKnown"].toBool());
    radio.SetDuration(300);
    QSignalSpy shareWarning(&radio, &RadioService::ShareWarningRequired);
    const auto beforeShare = fake.acquisitions;
    QVERIFY(radio.startSharing());
    QCOMPARE(shareWarning.count(), 1);
    QCOMPARE(fake.acquisitions, beforeShare);
    QVERIFY(radio.Snapshot()["showWarning"].toBool());
    QVERIFY(radio.startSharing(true, false));
    QTRY_COMPARE(radio.Snapshot()["state"].toString(), QString("on"));
    QCOMPARE(fake.duration, 0u);
    QCOMPARE(radio.Snapshot()["duration"].toUInt(), 300u);
    radio.Disable();
    QTRY_COMPARE(radio.Snapshot()["state"].toString(), QString("off"));
    radio.SetDuration(0);
    RadioService second(bus);
    QCOMPARE(second.Snapshot()["duration"].toUInt(), 0u);
    QVERIFY(second.Snapshot()["showWarning"].toBool());
    bus.unregisterObject("/io/github/t4t5/Owly");
    bus.unregisterService("io.github.t4t5.Owly");
  }
};
QTEST_GUILESS_MAIN(RadioTest)
#include "radio-test.moc"
