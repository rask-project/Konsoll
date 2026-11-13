# Sourced by konsoll.zsh and konsoll.bash in a shell Konsoll set up over ssh
# (konsoll-ssh.sh). The AI agents started here get Konsoll's hooks for this
# run only -- nothing in their settings files changes -- and say they
# started, so the pane is an agent for Send to Agent before any hook ran.
# Works in zsh and bash.

# The agent `$1` started: an idle state (no notification) the end of the
# command clears.
_konsoll_agent_started() {
    printf '\033]1337;KonsollAgent=%s;%s;idle\007' "$KONSOLL_REMOTE_AGENT_TOKEN" "$1"
}

# Defines `$1` as a function running `$2`, unless an alias has the name.
_konsoll_wrap_agent() {
    alias "$1" >/dev/null 2>&1 && return 0
    eval "$1() { $2; }"
}

# Claude Code adds --settings to the user's settings, Codex merges -c hooks
# with its own (and runs them once trusted with /hooks), Qwen Code reads
# system defaults below the user's settings. opencode needs a plugin file:
# it is only marked as an agent.
_konsoll_wrap_agent claude \
    '_konsoll_agent_started claude; command claude --settings "$KONSOLL_REMOTE_DIR/claude-settings.json" "$@"'
_konsoll_wrap_agent codex \
    '_konsoll_agent_started codex; command codex -c "hooks=$(cat "$KONSOLL_REMOTE_DIR/codex-hooks.toml")" "$@"'
_konsoll_wrap_agent qwen \
    '_konsoll_agent_started qwen; QWEN_CODE_SYSTEM_DEFAULTS_PATH="${QWEN_CODE_SYSTEM_DEFAULTS_PATH:-$KONSOLL_REMOTE_DIR/qwen-settings.json}" command qwen "$@"'
_konsoll_wrap_agent opencode '_konsoll_agent_started opencode; command opencode "$@"'

# The directory is this shell's alone (its pid is in it): removed when the
# shell exits. The next bootstrap removes what a killed shell left.
_konsoll_remote_cleanup() {
    [ -n "${KONSOLL_REMOTE_DIR-}" ] && [ "$(cat "$KONSOLL_REMOTE_DIR/pid" 2>/dev/null)" = "$$" ] &&
        rm -rf "$KONSOLL_REMOTE_DIR"
}
if [ -n "${ZSH_VERSION-}" ]; then
    add-zsh-hook zshexit _konsoll_remote_cleanup
elif [ -z "$(trap -p EXIT)" ]; then
    trap _konsoll_remote_cleanup EXIT
fi
