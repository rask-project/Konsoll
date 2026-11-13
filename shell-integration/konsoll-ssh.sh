# Sourced by konsoll.zsh and konsoll.bash on this Mac (not in a shell
# Konsoll set up over ssh): an interactive "ssh host" brings Konsoll's
# shell integration along, so the remote shell marks its commands (OSC 133)
# and AI agents started there report their state to the pane.
#
# Nothing is installed on the host: ssh runs a bootstrap as the remote
# command (ssh-remote-command, written by Konsoll) that unpacks the
# integration into a private temporary directory, removed when the shell
# exits, and starts the login shell with it. Anything else -- a remote
# command, -N, -W, a RemoteCommand in ssh_config, a host excluded in
# Settings, no terminal -- runs ssh untouched. Works in zsh and bash.

_KONSOLL_SSH_DIR=${KONSOLL_SHELL_INTEGRATION%/*}

# Sets _KONSOLL_SSH_TARGET when `ssh ARGS` opens a login shell on a host:
# the arguments end with the destination (no remote command) and no option
# asks for something else (-N no shell, -W forwarding, -f background, -G /
# -V / -Q / -O queries, -s subsystem, -T no terminal).
_konsoll_ssh_interactive() {
    _KONSOLL_SSH_TARGET=''
    local argument rest flag
    while [ $# -gt 0 ]; do
        argument=$1
        shift
        case $argument in
        --)
            [ $# -eq 1 ] || return 1
            _KONSOLL_SSH_TARGET=$1
            return 0
            ;;
        -?*)
            rest=${argument#-}
            while [ -n "$rest" ]; do
                flag=${rest%"${rest#?}"}
                rest=${rest#?}
                case $flag in
                N | T | W | f | G | O | Q | s | V) return 1 ;;
                B | b | c | D | E | e | F | I | i | J | L | l | m | o | P | p | R | S | w)
                    # Takes a value: the rest of this word or the next one.
                    if [ -z "$rest" ]; then
                        [ $# -gt 0 ] || return 1
                        shift
                    fi
                    break
                    ;;
                esac
            done
            ;;
        *)
            [ $# -eq 0 ] || return 1
            _KONSOLL_SSH_TARGET=$argument
            return 0
            ;;
        esac
    done
    return 1
}

# True when Settings excludes `target` ("user@host" or "host" as typed).
_konsoll_ssh_excluded() {
    local file="$_KONSOLL_SSH_DIR/ssh-excluded-hosts" line
    [ -r "$file" ] || return 1
    while IFS= read -r line || [ -n "$line" ]; do
        if [ "$line" = "$1" ] || [ "$line" = "${1#*@}" ]; then
            return 0
        fi
    done <"$file"
    return 1
}

# True unless ssh_config gives the host a command or session of its own,
# which the bootstrap would replace (or conflict with).
_konsoll_ssh_plain_config() {
    local config
    config=$(command ssh -G "$@" 2>/dev/null) || return 1
    case $config in
    *"
remotecommand "* | *"
sessiontype none"* | *"
sessiontype subsystem"* | *"
requesttty no"*) return 1 ;;
    esac
    return 0
}

_konsoll_ssh() {
    local remote_command="$_KONSOLL_SSH_DIR/ssh-remote-command"
    if [ -r "$remote_command" ] && [ -n "${KONSOLL_REMOTE_AGENT_TOKEN-}" ] && [ -t 0 ] && [ -t 1 ] &&
        _konsoll_ssh_interactive "$@" && ! _konsoll_ssh_excluded "$_KONSOLL_SSH_TARGET" &&
        _konsoll_ssh_plain_config "$@"; then
        command ssh -t "$@" "$(cat "$remote_command") $KONSOLL_REMOTE_AGENT_TOKEN"
    else
        command ssh "$@"
    fi
}
