#include "radioservice.h"
#include "transfermanager.h"
#include <KConfigGroup>
#include <QDBusContext>
#include <QDBusMessage>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QListWidget>
#include <QPushButton>
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
  int acquisitions = 0, renewals = 0, releases = 0;
  uint renewedDuration = 0;
  QString state() const { return current; }
  qulonglong lease() const { return id; }
public slots:
  qulonglong Acquire(uint, QVariantMap &link) {
    ++acquisitions;
    id = 42;
    current = "up";
    link = {{"interface", "fake0"}};
    return id;
  }
  void Renew(qulonglong value, uint seconds) {
    if (value != id) {
      sendErrorReply("io.github.t4t5.Owly.Error.StaleLease", "Wrong lease");
      return;
    }
    ++renewals;
    renewedDuration = seconds;
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
    radio.SetDuration(300);
    radio.ConfirmEnable(false);
    QTRY_COMPARE(radio.Snapshot()["state"].toString(), QString("on"));
    QTRY_VERIFY(radio.Snapshot()["receiving"].toBool());
    QTRY_VERIFY(radio.Snapshot()["canRenew"].toBool());
    const auto deadline = radio.Snapshot()["deadline"];
    const auto process = manager.m_process;
    QFile fixture(destination.filePath("fixture"));
    QVERIFY(fixture.open(QIODevice::WriteOnly));
    fixture.write("fixture");
    fixture.close();
    // Dolphin's Purpose plugin uses this same entry point. Opening the
    // recipient picker must reuse the timed session without any radio mutation.
    radio.SendFiles({QUrl::fromLocalFile(fixture.fileName()).toString()});
    QTRY_VERIFY(manager.m_peerPicker);
    QCOMPARE(backend.acquisitions, 1);
    QCOMPARE(backend.renewals, 0);
    QCOMPARE(backend.releases, 0);
    QCOMPARE(manager.m_process, process);
    QCOMPARE(radio.Snapshot()["deadline"], deadline);
    auto *picker = qobject_cast<QDialog *>(manager.m_peerPicker.data());
    QVERIFY(picker);
    auto *list = picker->findChild<QListWidget *>();
    auto *buttons = picker->findChild<QDialogButtonBox *>();
    QVERIFY(list);
    QVERIFY(buttons);
    QTRY_COMPARE(list->count(), 1);
    list->setCurrentRow(0);
    QVERIFY(buttons->button(QDialogButtonBox::Ok)->isEnabled());
    buttons->button(QDialogButtonBox::Ok)->click();
    QTRY_VERIFY(!manager.m_entries.isEmpty() &&
                manager.m_entries.cbegin()->job);
    QCOMPARE(radio.Snapshot()["deadline"], deadline);
    QCOMPARE(backend.acquisitions, 1);
    QCOMPARE(manager.m_process, process);
    QVERIFY(backend.renewedDuration > 0); // Finite transfer protection only.
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
    QCOMPARE(radio.Snapshot()["deadline"], deadline);
    const auto renewals = backend.renewals;
    manager.receiveFiles();
    QVERIFY(!manager.m_sharePending);
    QCOMPARE(backend.renewals, renewals);
    QCOMPARE(radio.Snapshot()["deadline"], deadline);
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
