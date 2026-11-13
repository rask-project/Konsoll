#pragma once

#include "ShellFunctions.h"

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <expected>

namespace konsoll
{

// ssh to a host brings Konsoll's shell integration along, so the remote
// shell marks its commands and the AI agents started there report their
// state to the pane (AgentStatus.h: parseRemoteAgentReport). Nothing is
// installed on the host. The `ssh` function of the integration on this Mac
// (konsoll-ssh.sh) sends sshRemoteCommand() plus the session's
// KONSOLL_REMOTE_AGENT_TOKEN as the remote command of an interactive login;
// the bootstrap it carries unpacks the integration into a private
// temporary directory (removed when the shell exits) and starts the login
// shell (zsh or bash; any other starts as usual) with it. The agents get
// Konsoll's hooks for that run only: Claude Code with --settings, Codex
// with -c hooks=..., Qwen Code through its system defaults file.

// The bootstrap: POSIX sh, run with the connection's token as $1. It falls
// through (instead of exiting) when it cannot set the shell up.
QByteArray sshBootstrapScript();

// The remote command ssh runs, followed by a space and the token: the
// bootstrap from base64, or the plain login shell when it fails.
QByteArray sshRemoteCommand();

// Settings ▸ AI Agents: on unless turned off, and the hosts it leaves
// alone ("host" or "user@host", as typed after ssh).
bool sshIntegrationEnabled();
void setSshIntegrationEnabled(bool enabled);
QStringList sshExcludedHosts();
void setSshExcludedHosts(const QStringList &hosts);

// Writes what konsoll-ssh.sh reads into `directory`: ssh-remote-command
// (removed while the integration is off) and ssh-excluded-hosts.
std::expected<void, QString> writeSshIntegration(const QString &directory = shellIntegrationDirectory());

} // namespace konsoll
