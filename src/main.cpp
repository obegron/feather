#include "radioservice.h"
#include "transfermanager.h"
#include <QApplication>
#include <QIcon>
int main(int argc, char **argv) {
  QApplication app(argc, argv);
  app.setQuitOnLastWindowClosed(false);
  app.setApplicationDisplayName("Feather");
  app.setWindowIcon(QIcon::fromTheme("document-send"));
  app.setDesktopFileName("io.github.obegron.feather");
  app.setApplicationName("feather-session");
  app.setOrganizationDomain("obegron.github.io");
  auto session = QDBusConnection::sessionBus();
  if (!session.registerService("io.github.obegron.feather"))
    return 1;
  RadioService service(QDBusConnection::systemBus());
  TransferManager transfers(&service);
  if (!session.registerObject("/io/github/obegron/feather", &service,
                              QDBusConnection::ExportAllSlots |
                                  QDBusConnection::ExportAllSignals))
    return 2;
  return app.exec();
}
