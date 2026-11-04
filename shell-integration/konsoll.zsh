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
    printf '\e]1337;RemoteHost=%s;\a' "$KONSOLL_REMOTE_HOST_TOKEN"
    printf '\e]7;file://localhost%s\a' "$encoded_path"
    printf '\e]133;A\a'
    if [[ "$PROMPT" != *"$_KONSOLL_PROMPT_MARKER" ]]; then
        PROMPT+=$_KONSOLL_PROMPT_MARKER
    fi
    _KONSOLL_COMMAND_ACTIVE=0
}

function _konsoll_preexec() {
    local -a command_words
    command_words=("${(z)1}")
    local command_index=1
    local ssh_port=""
    local ssh_user=""
    [[ ${command_words[1]} == command || ${command_words[1]} == sudo ]] && (( command_index++ ))
    if [[ ${command_words[command_index]##*/} == ssh ]]; then
        local target_index=$((command_index + 1))
        while (( target_index <= ${#command_words} )); do
            local argument=${command_words[target_index]}
            if [[ "$argument" == -- ]]; then
                (( target_index++ ))
                break
            elif [[ "$argument" == -p || "$argument" == -l || "$argument" == -i || "$argument" == -F || "$argument" == -J || "$argument" == -o ]]; then
                [[ "$argument" == -p ]] && ssh_port=${command_words[target_index + 1]}
                [[ "$argument" == -l ]] && ssh_user=${command_words[target_index + 1]}
                (( target_index += 2 ))
            elif [[ "$argument" == -* ]]; then
                (( target_index++ ))
            else
                break
            fi
        done
        if (( target_index <= ${#command_words} )); then
            local remote_target=${command_words[target_index]}
            if [[ -n "$ssh_user" && "$remote_target" != *@* ]]; then
                remote_target="$ssh_user@$remote_target"
            fi
            printf '\e]1337;RemoteHost=%s;%s%s\a' "$KONSOLL_REMOTE_HOST_TOKEN" "$remote_target" \
                "${ssh_port:+;port=$ssh_port}"
        fi
    fi
    printf '\e]133;C\a'
    _KONSOLL_COMMAND_ACTIVE=1
}

add-zsh-hook precmd _konsoll_precmd
add-zsh-hook preexec _konsoll_preexec
