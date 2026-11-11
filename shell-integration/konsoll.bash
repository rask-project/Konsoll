_KONSOLL_COMMAND_ACTIVE=0
_KONSOLL_CAPTURED_STATUS=0
_KONSOLL_AT_PROMPT=0
_KONSOLL_HISTORY_CHECK=0
_KONSOLL_LAST_HISTORY=''
_KONSOLL_PROMPT_MARKER='\[\e]133;B\a\]'

# Meta-. inserts the previous command's last argument; repeated presses walk
# backward through command history, matching the terminal's Alt+. sequence.
bind '"\e.": yank-last-arg'

# Functions defined in Konsoll (Background Commands > Functions): loaded
# now, and again at a prompt after Konsoll rewrote them.
_KONSOLL_FUNCTIONS_DIR=${KONSOLL_SHELL_INTEGRATION%/*}
_KONSOLL_FUNCTIONS_LOADED=''

_konsoll_load_functions() {
    local serial_file="$_KONSOLL_FUNCTIONS_DIR/functions.serial" serial=''
    [[ -r "$serial_file" ]] || return 0
    read -r serial < "$serial_file"
    [[ "$serial" == "$_KONSOLL_FUNCTIONS_LOADED" ]] && return 0
    _KONSOLL_FUNCTIONS_LOADED=$serial
    [[ -r "$_KONSOLL_FUNCTIONS_DIR/functions.bash" ]] && source "$_KONSOLL_FUNCTIONS_DIR/functions.bash"
    return 0
}

_konsoll_load_functions

_konsoll_prompt_command() {
    _konsoll_load_functions
    local encoded_path=${PWD//%/%25}
    encoded_path=${encoded_path// /%20}
    encoded_path=${encoded_path//#/%23}
    encoded_path=${encoded_path//\?/%3F}
    if (( _KONSOLL_HISTORY_CHECK )); then
        # The DEBUG trap misses a command line that is only a subshell,
        # "( ... )"; a new history entry still shows that one ran.
        local history_number
        history_number=$(_konsoll_history_number)
        [[ "$history_number" != "$_KONSOLL_LAST_HISTORY" ]] && _KONSOLL_COMMAND_ACTIVE=1
        _KONSOLL_LAST_HISTORY=$history_number
    fi
    if (( _KONSOLL_COMMAND_ACTIVE )); then
        printf '\e]133;D;%d\a' "$_KONSOLL_CAPTURED_STATUS"
    fi
    printf '\e]1337;RemoteHost=%s;\a' "$KONSOLL_REMOTE_HOST_TOKEN"
    printf '\e]7;file://localhost%s\a' "$encoded_path"
    printf '\e]133;A\a'
    if [[ "$PS1" != *"$_KONSOLL_PROMPT_MARKER" ]]; then
        PS1+=$_KONSOLL_PROMPT_MARKER
    fi
    _KONSOLL_COMMAND_ACTIVE=0
    _KONSOLL_AT_PROMPT=1
}

# The number of the newest history entry.
_konsoll_history_number() {
    local entry
    entry=$(builtin history 1)
    entry=${entry#"${entry%%[![:space:]]*}"}
    printf '%s' "${entry%%[![:digit:]]*}"
}

# A command line is about to run: mark the command output (OSC 133;C) and
# report its exit status at the next prompt (OSC 133;D).
_konsoll_preexec() {
    (( _KONSOLL_AT_PROMPT )) || return 0
    _KONSOLL_AT_PROMPT=0
    # An empty command line (or Ctrl+C at the prompt) runs only
    # PROMPT_COMMAND, which starts with this assignment.
    [[ "$BASH_COMMAND" == _KONSOLL_CAPTURED_STATUS=* ]] && return 0
    _KONSOLL_COMMAND_ACTIVE=1
    printf '\e]133;C\a'
}

# Remember a direct SSH destination so image and file drops can be uploaded
# through the user's SSH configuration while the remote shell is active.
if [[ "$(type -t ssh)" != function ]]; then
  ssh() {
    local port="" user="" target="" argument
    local -a original_args=("$@")
    while (($#)); do
      argument="$1"
      if [[ "$argument" == -- ]]; then
        shift
        target="$1"
        break
      elif [[ "$argument" == -p || "$argument" == -l || "$argument" == -i || "$argument" == -F || "$argument" == -J || "$argument" == -o ]]; then
        [[ "$argument" == -p ]] && port="$2"
        [[ "$argument" == -l ]] && user="$2"
        shift 2
      elif [[ "$argument" == -* ]]; then
        shift
      else
        target="$argument"
        break
      fi
    done
    if [[ -n "$target" ]]; then
      [[ -n "$user" && "$target" != *@* ]] && target="$user@$target"
      printf '\e]1337;RemoteHost=%s;%s%s\a' "$KONSOLL_REMOTE_HOST_TOKEN" "$target" "${port:+;port=$port}"
    fi
    command ssh "${original_args[@]}"
  }
fi

if (( BASH_VERSINFO[0] > 4 || (BASH_VERSINFO[0] == 4 && BASH_VERSINFO[1] >= 4) )); then
    # PS0 is expanded in the shell itself right before a command line runs;
    # the array subscript sets the flag and expands to nothing.
    PS0='${_KONSOLL_PS0_HOOK[(_KONSOLL_COMMAND_ACTIVE=1, _KONSOLL_AT_PROMPT=0)]}'"${PS0-}"$'\e]133;C\a'
elif [[ -z "$(trap -p DEBUG)" ]]; then
    # bash before 4.4 (macOS /bin/bash is 3.2) has no PS0: the DEBUG trap
    # runs before every simple command, so _konsoll_preexec acts on the
    # first one after a prompt. A DEBUG trap of the user's is left alone.
    trap '_konsoll_preexec' DEBUG
    _KONSOLL_HISTORY_CHECK=1
    _KONSOLL_LAST_HISTORY=$(_konsoll_history_number)
fi
PROMPT_COMMAND="_KONSOLL_CAPTURED_STATUS=\$?;${PROMPT_COMMAND:+${PROMPT_COMMAND};}_konsoll_prompt_command"
