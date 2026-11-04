function _konsoll_prompt --on-event fish_prompt
    set -l encoded_path (string escape --style=url -- "$PWD")
    printf '\e]7;file://localhost%s\a' "$encoded_path"
    printf '\e]1337;RemoteHost=%s;\a' "$KONSOLL_REMOTE_HOST_TOKEN"
    printf '\e]133;A\a'
    printf '\e]133;B\a'
end

function _konsoll_preexec --on-event fish_preexec
    set -l command_words (string split ' ' -- $argv[1])
    if test "$command_words[1]" = ssh; and set -q command_words[2]
        if string match -q -- '-*' $command_words[2]
            if test "$command_words[2]" = -p; and set -q command_words[4]
                printf '\e]1337;RemoteHost=%s;%s;port=%s\a' "$KONSOLL_REMOTE_HOST_TOKEN" $command_words[4] $command_words[3]
            end
        else
            printf '\e]1337;RemoteHost=%s;%s\a' "$KONSOLL_REMOTE_HOST_TOKEN" $command_words[2]
        end
    end
    printf '\e]133;C\a'
end

function _konsoll_postexec --on-event fish_postexec
    printf '\e]133;D;%d\a' $status
end

function _konsoll_insert_last_argument
    set -l current (commandline -ct)
    if test "$current" != "$_KONSOLL_LAST_ARG_INSERTED"
        set -g _KONSOLL_LAST_ARG_INDEX 1
    end

    set -l entries (history --max 100)
    set -l index $_KONSOLL_LAST_ARG_INDEX
    while test $index -le (count $entries)
        set -l argument (string match -r '[^[:space:]]+$' -- "$entries[$index]")
        if test -n "$argument"
            commandline -rt -- "$argument"
            set -g _KONSOLL_LAST_ARG_INSERTED "$argument"
            set -g _KONSOLL_LAST_ARG_INDEX (math $index + 1)
            return
        end
        set index (math $index + 1)
    end
end

set -g _KONSOLL_LAST_ARG_INDEX 1
set -g _KONSOLL_LAST_ARG_INSERTED ''
bind '\e.' _konsoll_insert_last_argument
