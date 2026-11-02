#import <AppKit/AppKit.h>

#include "CloseConfirmDialog.h"

namespace konsoll
{

namespace
{
bool s_quitApprovedByQml{};
}

bool CloseConfirmDialog::confirmClose()
{
    NSAlert *const alert = [[NSAlert alloc] init];
    [alert setAlertStyle:NSAlertStyleWarning];
    [alert setMessageText:@"Close Konsoll?"];
    [alert setInformativeText:
               @"Are you sure you want to close Konsoll? In-progress tasks will not be completed."];
    [alert addButtonWithTitle:@"Close Anyway"];
    [alert addButtonWithTitle:@"Cancel"];
    const bool confirmed = [alert runModal] == NSAlertFirstButtonReturn;
    [alert release];
    return confirmed;
}

bool CloseConfirmDialog::confirmQuit()
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

void CloseConfirmDialog::approveQuit()
{
    s_quitApprovedByQml = true;
}

bool CloseConfirmDialog::consumeQuitApproval()
{
    const bool approved = s_quitApprovedByQml;
    s_quitApprovedByQml = false;
    return approved;
}

} // namespace konsoll
