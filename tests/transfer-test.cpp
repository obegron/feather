#include "radioservice.h"
#include "transfermanager.h"
#include <KConfigGroup>
#include <QDBusContext>
#include <QDBusMessage>
#include <QDateTime>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>
class TransferRadio : public QObject, protected QDBusContext {
  Q_OBJECT
  Q_CLASSINFO("D-Bus Interface", "io.github.t4t5.Owly.Radio1")
  Q_PROPERTY(QString State READ state)
  Q_PROPERTY(qulonglong Lease READ lease)
public:
  QString current = "idle";
  qulonglong id = 0;
  int renewals = 0, releases = 0;
  QString state() const { return current; }
  qulonglong lease() const { return id; }
public slots:
  qulonglong Acquire(uint, QVariantMap &link) {
    id = 42;
    current = "up";
    link = {{"interface", "fake0"}};
    return id;
  }
  void Renew(qulonglong value, uint) {
    if (value != id) {
      sendErrorReply("io.github.t4t5.Owly.Error.StaleLease", "Wrong lease");
      return;
    }
    ++renewals;
  }
  void Release(qulonglong value) {
    if (value == id) {
      ++releases;
      id = 0;
      current = "idle";
    }
  }
};
class TransferTest : public QObject {
  Q_OBJECT
private slots:
  void pipeline() {
    auto config = KSharedConfig::openConfig("featherrc");
    config->deleteGroup("Radio");
    config->sync();
    QTemporaryDir destination;
    QVERIFY(destination.isValid());
    QFile dirs(qEnvironmentVariable("XDG_CONFIG_HOME") + "/user-dirs.dirs");
    QDir().mkpath(qEnvironmentVariable("XDG_CONFIG_HOME"));
    QVERIFY(dirs.open(QIODevice::WriteOnly));
    dirs.write(("XDG_DOWNLOAD_DIR=\"" + destination.path() + "\"\n").toUtf8());
    dirs.close();
    auto bus = QDBusConnection::sessionBus();
    QVERIFY(bus.isConnected());
    TransferRadio backend;
    QVERIFY(bus.registerService("io.github.t4t5.Owly"));
    QVERIFY(bus.registerObject("/io/github/t4t5/Owly", &backend,
                               QDBusConnection::ExportAllSlots |
                                   QDBusConnection::ExportAllProperties));
    RadioService radio(bus);
    TransferManager manager(&radio);
    QTRY_COMPARE(radio.Snapshot()["state"].toString(), QString("off"));
    radio.ConfirmEnable(false);
    QTRY_COMPARE(radio.Snapshot()["state"].toString(), QString("on"));
    QTRY_VERIFY(radio.Snapshot()["receiving"].toBool());
    QTRY_VERIFY(radio.Snapshot()["canRenew"].toBool());
    manager.browse();
    QTRY_VERIFY(!manager.m_peers.isEmpty());
    QVERIFY(manager.startSend("test-phone", {destination.path() + "/fixture"}));
    QTRY_VERIFY(!manager.m_entries.isEmpty() &&
                manager.m_entries.cbegin()->job);
    const auto id = manager.m_entries.keys().first();
    const auto job = manager.m_entries[id].job;
    QTRY_COMPARE(job->processedAmount(KJob::Bytes), 64ULL);
    QCOMPARE(job->totalAmount(KJob::Bytes), 128ULL);
    radio.Disable();
    QCOMPARE(backend.releases,
             0); // Off is enforced in the service during work.
    QSignalSpy finished(job, &KJob::result);
    job->kill(KJob::EmitResult);
    QTRY_COMPARE(finished.count(), 1);
    QTRY_VERIFY(manager.m_entries.isEmpty());
    QTRY_VERIFY(!radio.Snapshot()["busy"].toBool());
    // QSignalSpy observes completion; the auto-deleted job must not be
    // dereferenced afterward.

    // A late/duplicate accept cannot create a job after discovery stopped.
    manager.m_entries["in-late"] = {"Phone", "incoming", "approval", {}, {}};
    manager.m_accepting = false;
    manager.accept("in-late");
    QVERIFY(!manager.m_entries.contains("in-late"));
    manager.m_accepting = true;
    QVERIFY(manager.startSend("test-phone", {destination.path() + "/fixture"}));
    QTRY_VERIFY(!manager.m_entries.isEmpty() &&
                manager.m_entries.cbegin()->job);
    // A busy timer operation must not be interpreted as transfer failure.
    radio.AddTime(300);
    QVERIFY(radio.Snapshot()["busy"].toBool());
    auto *heartbeat = manager.findChild<QTimer *>();
    QVERIFY(heartbeat);
    QMetaObject::invokeMethod(heartbeat, "timeout", Qt::DirectConnection);
    QVERIFY(manager.m_ready);
    QTRY_VERIFY(!radio.Snapshot()["busy"].toBool());
    manager.m_process->kill();
    QTRY_VERIFY(manager.m_entries.isEmpty());
    QTRY_VERIFY(!radio.Snapshot()["receiving"].toBool());
    QTRY_VERIFY(!radio.Snapshot()["busy"].toBool());
    radio.Disable();
    QTRY_COMPARE(radio.Snapshot()["state"].toString(), QString("off"));
    QVERIFY(bus.unregisterService("io.github.t4t5.Owly"));
    bus.unregisterObject("/io/github/t4t5/Owly");
  }
};
QTEST_MAIN(TransferTest)
#include "transfer-test.moc"
