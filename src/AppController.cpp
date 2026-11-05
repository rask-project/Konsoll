#include "AppController.h"

namespace konsoll
{

QPointer<AppController> AppController::s_instance{};

AppController::AppController(QObject *parent)
    : QObject(parent)
{
    s_instance = this;
}

void AppController::openWindow()
{
    emit windowOpenRequested();
}

void AppController::requestOpenWindow()
{
    if (s_instance)
        s_instance->openWindow();
}

} // namespace konsoll
