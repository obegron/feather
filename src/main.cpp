#include "radioservice.h"
#include "transfermanager.h"
#include <QApplication>
#include <QIcon>
int main(int argc, char **argv) {
  QApplication app(argc, argv);
  app.setQuitOnLastWindowClosed(false);
  app.setApplicationDisplayName("Feather");
  app.setWindowIcon(QIcon::fromTheme("document-send"));
  app.setDesktopFileName("org.kde.Feather");
  app.setApplicationName("feather-session");
  app.setOrganizationDomain("kde.org");
  auto session = QDBusConnection::sessionBus();
  if (!session.registerService("org.kde.Feather"))
    return 1;
  RadioService service(QDBusConnection::systemBus());
  TransferManager transfers(&service);
  if (!session.registerObject("/org/kde/Feather", &service,
                              QDBusConnection::ExportAllSlots |
                                  QDBusConnection::ExportAllSignals))
    return 2;
  return app.exec();
}
