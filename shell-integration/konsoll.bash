_KONSOLL_COMMAND_ACTIVE=0
_KONSOLL_CAPTURED_STATUS=0
_KONSOLL_PROMPT_MARKER='\[\e]133;B\a\]'

# Meta-. inserts the previous command's last argument; repeated presses walk
# backward through command history, matching the terminal's Alt+. sequence.
bind '"\e.": yank-last-arg'

_konsoll_prompt_command() {
    if (( _KONSOLL_COMMAND_ACTIVE )); then
        printf '\e]133;D;%d\a' "$_KONSOLL_CAPTURED_STATUS"
    fi
    printf '\e]133;A\a'
    if [[ "$PS1" != *"$_KONSOLL_PROMPT_MARKER" ]]; then
        PS1+=$_KONSOLL_PROMPT_MARKER
    fi
    _KONSOLL_COMMAND_ACTIVE=0
}

PS0="${PS0-}\[\e]133;C\a\]"
PROMPT_COMMAND="_KONSOLL_CAPTURED_STATUS=\$?;${PROMPT_COMMAND:+${PROMPT_COMMAND};}_konsoll_prompt_command"
