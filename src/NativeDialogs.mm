#import <AppKit/AppKit.h>

#include "NativeDialogs.h"

#include <QByteArray>

#include <algorithm>

namespace konsoll
{

namespace
{
struct QuitApproval {
    void approve() const { approved = true; }
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

// Shared "workspace name" prompt: an informational alert with a single text
// field. Returns {accepted, title}; title is empty when the user cancels.
QVariantMap
workspaceNamePrompt(const char *message, const char *informative, const char *confirmTitle, const QString &initialTitle)
{
    NSAlert *const alert = [[NSAlert alloc] init];
    [alert setAlertStyle:NSAlertStyleInformational];
    [alert setMessageText:[NSString stringWithUTF8String:message]];
    [alert setInformativeText:[NSString stringWithUTF8String:informative]];
    [alert addButtonWithTitle:[NSString stringWithUTF8String:confirmTitle]];
    [alert addButtonWithTitle:@"Cancel"];

    NSTextField *const nameField = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 260, 24)];
    [nameField setStringValue:toNSString(initialTitle)];
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

bool NativeDialogs::confirmClipboardRead(const QString &title)
{
    NSAlert *const alert = [[NSAlert alloc] init];
    [alert setAlertStyle:NSAlertStyleWarning];
    [alert setMessageText:@"Allow access to the clipboard?"];
    [alert setInformativeText:[NSString stringWithFormat:@"The program running in “%@” wants to read the clipboard. "
                                                         @"You can change this in Settings > Security.",
                                                         title.toNSString()]];
    [alert addButtonWithTitle:@"Allow"];
    [alert addButtonWithTitle:@"Deny"];
    const bool allowed = [alert runModal] == NSAlertFirstButtonReturn;
    [alert release];
    return allowed;
}

std::optional<QString> NativeDialogs::answerSshPrompt(const QString &host, const QString &prompt)
{
    // A new host key: ssh asks "Are you sure you want to continue
    // connecting (yes/no/[fingerprint])?" after showing the fingerprint.
    const bool confirmation = prompt.contains(QStringLiteral("(yes/no"));
    NSAlert *const alert = [[NSAlert alloc] init];
    [alert setAlertStyle:confirmation ? NSAlertStyleWarning : NSAlertStyleInformational];
    [alert setMessageText:confirmation
                              ? [NSString stringWithFormat:@"Connect to “%@” for file transfers?", host.toNSString()]
                              : [NSString stringWithFormat:@"Sign in to “%@” for file transfers", host.toNSString()]];
    [alert setInformativeText:confirmation ? prompt.trimmed().toNSString()
                                           : [NSString stringWithFormat:@"%@\n\nKonsoll passes it to ssh and keeps it "
                                                                        @"nowhere.",
                                                                        prompt.trimmed().toNSString()]];
    [alert addButtonWithTitle:confirmation ? @"Connect" : @"OK"];
    [alert addButtonWithTitle:@"Cancel"];
    NSSecureTextField *const field =
        confirmation ? nil : [[NSSecureTextField alloc] initWithFrame:NSMakeRect(0, 0, 260, 24)];
    if (field != nil) {
        [alert setAccessoryView:field];
        [[alert window] setInitialFirstResponder:field];
    }
    [NSApp activateIgnoringOtherApps:YES];
    const bool accepted = [alert runModal] == NSAlertFirstButtonReturn;
    std::optional<QString> answer;
    if (accepted) {
        answer = field != nil ? toQString([field stringValue]) : QStringLiteral("yes");
    }
    [field release];
    [alert release];
    return answer;
}

bool NativeDialogs::confirmQuit(int sessionCount)
{
    NSAlert *const alert = [[NSAlert alloc] init];
    [alert setAlertStyle:NSAlertStyleWarning];
    [alert setMessageText:@"Quit Konsoll?"];
    [alert
        setInformativeText:[NSString stringWithFormat:@"Quitting will close all windows and terminate %d session(s).",
                                                      sessionCount]];
    [alert addButtonWithTitle:@"Quit Anyway"];
    [alert addButtonWithTitle:@"Cancel"];
    const bool confirmed = [alert runModal] == NSAlertFirstButtonReturn;
    [alert release];
    return confirmed;
}

QVariantMap NativeDialogs::promptWorkspaceRename(const QString &currentTitle)
{
    return workspaceNamePrompt("Rename Workspace", "Enter a name for this workspace.", "Rename", currentTitle);
}

QVariantMap NativeDialogs::promptWorkspaceName()
{
    return workspaceNamePrompt("New Workspace", "Enter a name for the new workspace.", "Create", QString{});
}

QVariantMap NativeDialogs::promptAgentWorktree(const QStringList &agentNames, const QString &repository)
{
    NSAlert *const alert = [[NSAlert alloc] init];
    [alert setAlertStyle:NSAlertStyleInformational];
    [alert setMessageText:@"New Agent in Worktree"];
    [alert setInformativeText:[NSString stringWithFormat:@"Konsoll creates a git worktree of %@ on the branch "
                                                         @"(new or existing) and starts the agent there in a new tab, "
                                                         @"so it works apart from your checkout.",
                                                         toNSString(repository)]];
    [alert addButtonWithTitle:@"Start"];
    [alert addButtonWithTitle:@"Cancel"];

    NSView *const accessory = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 280, 58)];
    NSPopUpButton *const agentButton = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(0, 32, 280, 26) pullsDown:NO];
    std::ranges::for_each(agentNames, [agentButton](const QString &name) {
        [agentButton addItemWithTitle:toNSString(name)];
    });
    NSTextField *const branchField = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 280, 24)];
    [branchField setPlaceholderString:@"Branch, e.g. fix-login"];
    [accessory addSubview:agentButton];
    [accessory addSubview:branchField];
    [alert setAccessoryView:accessory];
    [[alert window] setInitialFirstResponder:branchField];

    const bool accepted = [alert runModal] == NSAlertFirstButtonReturn;
    const QVariantMap result{
        {QStringLiteral("accepted"), accepted},
        {QStringLiteral("agentIndex"), static_cast<int>([agentButton indexOfSelectedItem])},
        {QStringLiteral("branch"), toQString([branchField stringValue]).trimmed()},
    };
    [branchField release];
    [agentButton release];
    [accessory release];
    [alert release];
    return result;
}

void NativeDialogs::showWarning(const QString &message, const QString &informative)
{
    NSAlert *const alert = [[NSAlert alloc] init];
    [alert setAlertStyle:NSAlertStyleWarning];
    [alert setMessageText:toNSString(message)];
    [alert setInformativeText:toNSString(informative)];
    [alert addButtonWithTitle:@"OK"];
    [alert runModal];
    [alert release];
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
