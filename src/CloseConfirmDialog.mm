#import <AppKit/AppKit.h>

#include "CloseConfirmDialog.h"

namespace konsoll
{

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

} // namespace konsoll
