function _konsoll_prompt --on-event fish_prompt
    printf '\e]133;A\a'
    printf '\e]133;B\a'
end

function _konsoll_preexec --on-event fish_preexec
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
