#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusReply>
#include <QDir>
#include <QTextStream>
#include <QUrl>
int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  QStringList urls;
  for (const auto &arg : app.arguments().mid(1))
    urls << QUrl::fromUserInput(arg, QDir::currentPath(), QUrl::AssumeLocalFile)
                .toString();
  if (urls.isEmpty())
    return 1;
  auto request =
      QDBusMessage::createMethodCall("io.github.obegron.feather", "/io/github/obegron/feather",
                                     "io.github.obegron.feather.Radio1", "SendFiles");
  request << urls;
  QDBusReply<void> reply = QDBusConnection::sessionBus().call(request);
  if (!reply.isValid()) {
    QTextStream(stderr) << reply.error().message() << '\n';
    return 1;
  }
  return 0;
}
