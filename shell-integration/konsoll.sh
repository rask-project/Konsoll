# Sourced as $ENV by a POSIX sh on the host (dash, BusyBox ash, a bash
# started as sh) in a shell Konsoll set up over ssh (konsoll-ssh.sh): the
# prompt reports the host's directory, for file transfers and links to its
# files. Such a shell has no prompt hook, but expands parameters in PS1
# each time it shows it, so the ${PWD} in the report follows cd. Nothing
# else of the zsh/bash integration (command marks, agents) is set up here.

# The user's own $ENV, which the bootstrap replaced with this file
# (parameters in it expand, as the shell would).
if [ -n "${KONSOLL_USER_ENV-}" ]; then
    eval "_konsoll_env=\"$KONSOLL_USER_ENV\""
    [ -r "$_konsoll_env" ] && . "$_konsoll_env"
    unset _konsoll_env
fi

# A line editor counts the prompt's bytes as its width: BusyBox ash's and
# bash's skip what \[ \] enclose; dash usually has no line editor.
_konsoll_report="$(printf '\033]1337;KonsollRemoteDir=%s;' "$KONSOLL_REMOTE_AGENT_TOKEN")"'${PWD}'"$(printf '\007')"
case $(readlink "/proc/$$/exe" 2>/dev/null) in
*/busybox*) _konsoll_report="\\[$_konsoll_report\\]" ;;
*) [ -n "${BASH_VERSION-}" ] && _konsoll_report="\\[$_konsoll_report\\]" ;;
esac
case ${PS1-} in
*KonsollRemoteDir=*) ;;
*) PS1=$_konsoll_report${PS1-'$ '} ;;
esac
unset _konsoll_report

# The directory is this shell's alone (its pid is in it): removed when the
# shell exits. The next bootstrap removes what a killed shell left.
_konsoll_remote_cleanup() {
    [ -n "${KONSOLL_REMOTE_DIR-}" ] && [ "$(cat "$KONSOLL_REMOTE_DIR/pid" 2>/dev/null)" = "$$" ] &&
        rm -rf "$KONSOLL_REMOTE_DIR"
}
case $(trap) in
*EXIT*) ;;
*) trap _konsoll_remote_cleanup EXIT ;;
esac
