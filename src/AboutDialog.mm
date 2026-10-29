#import <AppKit/AppKit.h>

#include "AboutDialog.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QFile>
#include <QString>
#include <QtGlobal>

namespace
{

NSString *toNSString(QString const &value)
{
    QByteArray const utf8 = value.toUtf8();
    return [NSString stringWithUTF8String:utf8.constData()];
}

} // namespace

namespace konsoll
{

void AboutDialog::open()
{
    NSMutableDictionary<NSAboutPanelOptionKey, id> *const options = [NSMutableDictionary dictionaryWithDictionary:@{
        NSAboutPanelOptionApplicationName : toNSString(QCoreApplication::applicationName()),
        NSAboutPanelOptionApplicationVersion :
            [NSString stringWithFormat:@"Version %@", toNSString(QCoreApplication::applicationVersion())],
        @"Copyright" : toNSString(QStringLiteral("Copyright © 2026 rask-project"))
    }];

    QFile iconFile(QStringLiteral(":/assets/konsoll.png"));
    NSImage *appIcon = nil;
    if (iconFile.open(QIODevice::ReadOnly)) {
        QByteArray const iconData = iconFile.readAll();
        NSData *const imageData = [NSData dataWithBytes:iconData.constData()
                                                 length:static_cast<NSUInteger>(iconData.size())];
        appIcon = [[NSImage alloc] initWithData:imageData];
        if (appIcon != nil) {
            [appIcon setSize:NSMakeSize(64, 64)];
            [options setObject:appIcon forKey:NSAboutPanelOptionApplicationIcon];
        }
    }

    [NSApp orderFrontStandardAboutPanelWithOptions:options];
    [appIcon release];
}

void AboutDialog::openAboutQt()
{
    NSAlert *const alert = [[NSAlert alloc] init];
    [alert setAlertStyle:NSAlertStyleInformational];
    [alert setMessageText:@"About Qt"];
    [alert
        setInformativeText:
            [NSString
                stringWithFormat:
                    @"This program uses Qt version %s.\n\n"
                    @"Qt is a C++ toolkit for cross-platform application development.\n\n"
                    @"Qt provides single-source portability across major desktop, embedded, and mobile platforms.\n\n"
                    @"Qt is available under commercial and GNU (L)GPL licenses. See https://qt.io/licensing for "
                    @"details.\n\n"
                    @"Copyright © The Qt Company Ltd. and other contributors.\n"
                    @"Qt and the Qt logo are trademarks of The Qt Company Ltd.\n"
                    @"Qt is developed as an open source project. See https://qt.io for more information.",
                    QT_VERSION_STR]];

    QFile logoFile(QStringLiteral(":/assets/qtlogo-64.png"));
    if (logoFile.open(QIODevice::ReadOnly)) {
        QByteArray const logoData = logoFile.readAll();
        NSData *const imageData = [NSData dataWithBytes:logoData.constData()
                                                 length:static_cast<NSUInteger>(logoData.size())];
        NSImage *const logo = [[NSImage alloc] initWithData:imageData];
        if (logo != nil) {
            [alert setIcon:logo];
            [logo release];
        }
    }

    [alert addButtonWithTitle:@"OK"];
    [alert runModal];
    [alert release];
}

} // namespace konsoll
