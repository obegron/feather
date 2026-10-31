#include "bridge.h"
#include <QClipboard>
#include <QDBusConnection>
#include <QFile>
#include <QGuiApplication>
#include <QMimeData>
#include <QTemporaryDir>
#include <QUrl>
#include <QtTest>

class ClipboardService : public QObject {
  Q_OBJECT
  Q_CLASSINFO("D-Bus Interface", "io.github.obegron.feather.Radio1")
public:
  QStringList received;
  int sends = 0;
public slots:
  void SendFiles(const QStringList &urls) {
    received = urls;
    ++sends;
  }
};
class ClipboardTest : public QObject {
  Q_OBJECT
private slots:
  void copiedFiles() {
    auto bus = QDBusConnection::sessionBus();
    ClipboardService service;
    QVERIFY(bus.registerService("io.github.obegron.feather"));
    QVERIFY(bus.registerObject("/io/github/obegron/feather", &service,
                               QDBusConnection::ExportAllSlots));
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto name =
        dir.filePath(QString::fromUtf8("photo with spaces å.txt"));
    QFile file(name);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("fixture");
    file.close();
    auto *clipboard = QGuiApplication::clipboard();
    Bridge bridge;
    auto *data = new QMimeData;
    const auto url = QUrl::fromLocalFile(name);
    data->setUrls({url});
    data->setData("application/x-kde-cutselection", "1");
    clipboard->setMimeData(data);
    QVERIFY(bridge.pasteFiles());
    QTRY_COMPARE(service.sends, 1);
    QCOMPARE(service.received, QStringList{url.toString()});
    QVERIFY(QFile::exists(name));
    QCOMPARE(clipboard->mimeData()->urls(), QList<QUrl>{url});
    QCOMPARE(clipboard->mimeData()->data("application/x-kde-cutselection"),
             QByteArray("1"));

    // A path copied as plain text is not a file-manager file selection.
    clipboard->setText(name);
    QVERIFY(!bridge.pasteFiles());
    data = new QMimeData;
    data->setUrls({QUrl::fromLocalFile(dir.path())});
    clipboard->setMimeData(data);
    QVERIFY(!bridge.pasteFiles());
    data = new QMimeData;
    data->setUrls({url, QUrl("https://example.com/file")});
    clipboard->setMimeData(data);
    QVERIFY(!bridge.pasteFiles());
    data = new QMimeData;
    data->setUrls({QUrl::fromLocalFile(dir.filePath("missing"))});
    clipboard->setMimeData(data);
    QVERIFY(!bridge.pasteFiles());
    clipboard->clear();
    QVERIFY(!bridge.pasteFiles());
    QTest::qWait(30);
    QCOMPARE(service.sends, 1);
    bus.unregisterObject("/io/github/obegron/feather");
    bus.unregisterService("io.github.obegron.feather");
  }
};
QTEST_MAIN(ClipboardTest)
#include "clipboard-test.moc"
