autoload -Uz add-zsh-hook

# Match the standard readline Meta-. behavior in both zsh line-editing modes.
bindkey -M emacs '^[.' insert-last-word
bindkey -M viins '^[.' insert-last-word

typeset -g _KONSOLL_PROMPT_MARKER=$'%{\e]133;B\a%}'
typeset -g _KONSOLL_COMMAND_ACTIVE=0

function _konsoll_precmd() {
    local command_status=$?
    local encoded_path=${PWD//\%/%25}
    encoded_path=${encoded_path// /%20}
    encoded_path=${encoded_path//#/%23}
    encoded_path=${encoded_path//\?/%3F}
    if (( _KONSOLL_COMMAND_ACTIVE )); then
        printf '\e]133;D;%d\a' "$command_status"
    fi
    printf '\e]7;file://%s%s\a' "$HOST" "$encoded_path"
    printf '\e]133;A\a'
    if [[ "$PROMPT" != *"$_KONSOLL_PROMPT_MARKER" ]]; then
        PROMPT+=$_KONSOLL_PROMPT_MARKER
    fi
    _KONSOLL_COMMAND_ACTIVE=0
}

function _konsoll_preexec() {
    printf '\e]133;C\a'
    _KONSOLL_COMMAND_ACTIVE=1
}

add-zsh-hook precmd _konsoll_precmd
add-zsh-hook preexec _konsoll_preexec
