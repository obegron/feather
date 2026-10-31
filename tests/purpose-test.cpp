#include <Purpose/AlternativesModel>
#include <Purpose/Configuration>
#include <Purpose/Job>
#include <QDBusConnection>
#include <QDBusContext>
#include <QFile>
#include <QJsonArray>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QUrl>
#include <QtTest>

class ShareService : public QObject, protected QDBusContext {
  Q_OBJECT
  Q_CLASSINFO("D-Bus Interface", "io.github.obegron.feather.Radio1")
public:
  QStringList received;
  int sends = 0;
  bool reject = false;
public slots:
  void SendFiles(const QStringList &urls) {
    ++sends;
    if (reject) {
      sendErrorReply("io.github.obegron.feather.Error.Unavailable",
                     "Test rejection");
      return;
    }
    received = urls;
  }
};
class PurposeTest : public QObject {
  Q_OBJECT
private slots:
  void sharing() {
    QCoreApplication::addLibraryPath(QStringLiteral(FEATHER_PLUGIN_ROOT));
    auto bus = QDBusConnection::sessionBus();
    ShareService service;
    QVERIFY(bus.registerService("io.github.obegron.feather"));
    QVERIFY(bus.registerObject("/io/github/obegron/feather", &service,
                               QDBusConnection::ExportAllSlots));
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto path = dir.filePath(QString::fromUtf8("share #1 å.txt"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("fixture");
    file.close();
    const auto url = QUrl::fromLocalFile(path).toString();
    Purpose::AlternativesModel model;
    model.setInputData({{"urls", QJsonArray{url}}, {"mimeType", "text/plain"}});
    model.setPluginType("Export");
    auto findFeather = [&model] {
      for (int row = 0; row < model.rowCount(); ++row)
        if (model
                .data(model.index(row, 0),
                      Purpose::AlternativesModel::PluginIdRole)
                .toString() == "io.github.obegron.feather")
          return row;
      return -1;
    };
    QTRY_VERIFY(findFeather() >= 0);
    const auto row = findFeather();
    QVERIFY(model
                .data(model.index(row, 0),
                      Purpose::AlternativesModel::ActionDisplayRole)
                .toString()
                .contains("Feather"));
    auto *configuration = model.configureJob(row);
    QVERIFY(configuration);
    QVERIFY(configuration->isReady());
    configuration->setUseSeparateProcess(false);
    auto *job = configuration->createJob();
    QVERIFY(job);
    int resultError = -1;
    connect(job, &KJob::result, this, [&resultError](KJob *finished) {
      resultError = finished->error();
    });
    QSignalSpy result(job, &KJob::result);
    job->start();
    QTRY_COMPARE(result.count(), 1);
    QCOMPARE(resultError, 0);
    QCOMPARE(service.received, QStringList{url});
    QCOMPARE(service.sends, 1);

    model.setInputData(
        {{"urls", QJsonArray{QUrl::fromLocalFile(dir.path()).toString()}},
         {"mimeType", "inode/directory"}});
    QCOMPARE(findFeather(), -1); // Not offered for folder selections.

    const auto run = [this, &model, &findFeather](const QJsonArray &urls) {
      model.setInputData(
          {{"urls", urls}, {"mimeType", "application/octet-stream"}});
      const auto row = findFeather();
      if (row < 0)
        return -1;
      auto *configuration = model.configureJob(row);
      if (!configuration || !configuration->isReady())
        return -1;
      configuration->setUseSeparateProcess(false);
      auto *job = configuration->createJob();
      if (!job)
        return -1;
      int error = -1;
      connect(job, &KJob::result, this,
              [&error](KJob *finished) { error = finished->error(); });
      QSignalSpy result(job, &KJob::result);
      job->start();
      if (!result.wait(3000))
        return -1;
      return error;
    };
    QVERIFY(run({}) > 0);
    QVERIFY(run({url, "https://example.com/file"}) > 0);
    QVERIFY(run({QUrl::fromLocalFile(dir.path()).toString()}) > 0);
    QCOMPARE(service.sends, 1);
    service.reject = true;
    QVERIFY(run({url}) > 0);
    QCOMPARE(service.sends, 2);
    bus.unregisterObject("/io/github/obegron/feather");
    bus.unregisterService("io.github.obegron.feather");
  }
};
QTEST_MAIN(PurposeTest)
#include "purpose-test.moc"
