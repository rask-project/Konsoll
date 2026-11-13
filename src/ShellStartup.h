#pragma once

#include <QByteArray>
#include <QString>

#include <array>
#include <utility>

namespace konsoll
{

// The startup files that load Konsoll's shell integration around the
// user's own, for a shell on this Mac (PtyProcess) and one Konsoll sets up
// over ssh (SshIntegration). zsh reads them from ZDOTDIR (KONSOLL_USER_ZDOTDIR
// names the user's directory); they source $KONSOLL_SHELL_INTEGRATION.
std::array<std::pair<QString, QByteArray>, 3> zshStartupFiles();

// bash's .bash_profile in a HOME of Konsoll's: restores HOME from
// KONSOLL_ORIGINAL_HOME, runs the user's profile, then the integration.
QByteArray bashProfile();

} // namespace konsoll
