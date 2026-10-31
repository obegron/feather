#include "radioservice.h"
#include <QCoreApplication>
int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  app.setApplicationName("feather-session");
  app.setOrganizationDomain("kde.org");
  auto session = QDBusConnection::sessionBus();
  if (!session.registerService("org.kde.Feather"))
    return 1;
  RadioService service(QDBusConnection::systemBus());
  if (!session.registerObject("/org/kde/Feather", &service,
                              QDBusConnection::ExportAllSlots |
                                  QDBusConnection::ExportAllSignals))
    return 2;
  return app.exec();
}
