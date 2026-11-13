#include "ShellStartup.h"

namespace konsoll
{

namespace
{

// Sources the user's `file` from their ZDOTDIR, with ZDOTDIR theirs while
// it runs (it may change it). At the top level, not in a function: there
// `local` and `typeset` in the user's files (an oh-my-zsh theme's
// `local ret_status=...`) would end with the function.
QByteArray sourceUserFile(const QByteArray &file)
{
    return "if [[ -r \"$_KONSOLL_USER_ZDOTDIR/" + file +
           "\" ]]; then\n"
           "  ZDOTDIR=\"$_KONSOLL_USER_ZDOTDIR\"\n"
           "  source \"$_KONSOLL_USER_ZDOTDIR/" +
           file +
           "\"\n"
           "  _KONSOLL_USER_ZDOTDIR=\"${ZDOTDIR:-$_KONSOLL_USER_ZDOTDIR}\"\n"
           "fi\n"
           "ZDOTDIR=\"$_KONSOLL_WRAPPER_ZDOTDIR\"\n";
}

} // namespace

std::array<std::pair<QString, QByteArray>, 3> zshStartupFiles()
{
    return {{
        {QStringLiteral(".zshenv"),
         "typeset -g _KONSOLL_WRAPPER_ZDOTDIR=\"$ZDOTDIR\"\n"
         "typeset -g _KONSOLL_USER_ZDOTDIR=\"${KONSOLL_USER_ZDOTDIR:-$HOME}\"\n" +
             sourceUserFile(".zshenv")},
        {QStringLiteral(".zprofile"), sourceUserFile(".zprofile")},
        {QStringLiteral(".zshrc"),
         sourceUserFile(".zshrc") + "if [[ \"$HISTFILE\" == \"$_KONSOLL_WRAPPER_ZDOTDIR/.zsh_history\" ]]; then\n"
                                    "  HISTFILE=\"$_KONSOLL_USER_ZDOTDIR/.zsh_history\"\n"
                                    "  [[ -r \"$HISTFILE\" ]] && fc -R \"$HISTFILE\"\n"
                                    "fi\n"
                                    "source \"$KONSOLL_SHELL_INTEGRATION\"\n"
                                    "ZDOTDIR=\"$_KONSOLL_USER_ZDOTDIR\"\n"},
    }};
}

QByteArray bashProfile()
{
    return "export HOME=\"$KONSOLL_ORIGINAL_HOME\"\n"
           "for _konsoll_profile in .bash_profile .bash_login .profile; do\n"
           "  if [[ -r \"$HOME/$_konsoll_profile\" ]]; then\n"
           "    source \"$HOME/$_konsoll_profile\"\n"
           "    break\n"
           "  fi\n"
           "done\n"
           "source \"$KONSOLL_SHELL_INTEGRATION\"\n";
}

} // namespace konsoll
