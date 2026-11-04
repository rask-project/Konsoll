#import <AppKit/AppKit.h>

#include "NativeDialogs.h"

#include <QByteArray>

namespace konsoll
{

namespace
{
struct QuitApproval
{
    void approve() const
    {
        approved = true;
    }
    bool consume() const
    {
        const bool wasApproved = approved;
        approved = false;
        return wasApproved;
    }
    mutable bool approved{};
};
const QuitApproval s_quitApproval{};

NSString *toNSString(const QString &value)
{
    const QByteArray utf8 = value.toUtf8();
    return [NSString stringWithUTF8String:utf8.constData()];
}

QString toQString(NSString *value)
{
    return QString::fromUtf8([value UTF8String]);
}
} // namespace

bool NativeDialogs::confirmClose()
{
    NSAlert *const alert = [[NSAlert alloc] init];
    [alert setAlertStyle:NSAlertStyleWarning];
    [alert setMessageText:@"Close Konsoll?"];
    [alert setInformativeText:@"Are you sure you want to close Konsoll? In-progress tasks will not be completed."];
    [alert addButtonWithTitle:@"Close Anyway"];
    [alert addButtonWithTitle:@"Cancel"];
    const bool confirmed = [alert runModal] == NSAlertFirstButtonReturn;
    [alert release];
    return confirmed;
}

bool NativeDialogs::confirmQuit()
{
    NSAlert *const alert = [[NSAlert alloc] init];
    [alert setAlertStyle:NSAlertStyleWarning];
    [alert setMessageText:@"Quit Konsoll?"];
    [alert setInformativeText:@"One or more sessions have active processes that will be terminated if you quit."];
    [alert addButtonWithTitle:@"Quit Anyway"];
    [alert addButtonWithTitle:@"Cancel"];
    const bool confirmed = [alert runModal] == NSAlertFirstButtonReturn;
    [alert release];
    return confirmed;
}

QVariantMap NativeDialogs::promptWorkspaceRename(const QString &currentTitle)
{
    NSAlert *const alert = [[NSAlert alloc] init];
    [alert setAlertStyle:NSAlertStyleInformational];
    [alert setMessageText:@"Rename Workspace"];
    [alert setInformativeText:@"Enter a name for this workspace."];
    [alert addButtonWithTitle:@"Rename"];
    [alert addButtonWithTitle:@"Cancel"];

    NSTextField *const nameField = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 260, 24)];
    [nameField setStringValue:toNSString(currentTitle)];
    [nameField setPlaceholderString:@"Workspace name"];
    [nameField setAutoresizingMask:NSViewWidthSizable];
    [alert setAccessoryView:nameField];
    [[alert window] setInitialFirstResponder:nameField];

    const bool accepted = [alert runModal] == NSAlertFirstButtonReturn;
    const QString title = accepted ? toQString([nameField stringValue]) : QString{};
    [nameField release];
    [alert release];
    return {{QStringLiteral("accepted"), accepted}, {QStringLiteral("title"), title}};
}

void NativeDialogs::approveQuit()
{
    s_quitApproval.approve();
}

void NativeDialogs::terminateApplication()
{
    [NSApp terminate:nil];
}

bool NativeDialogs::consumeQuitApproval()
{
    return s_quitApproval.consume();
}

} // namespace konsoll
