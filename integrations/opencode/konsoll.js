// Konsoll: shows opencode's state (working, waiting for you, done, failed)
// on its Konsoll tab. It does nothing outside Konsoll. Installed by Konsoll
// (Settings > AI Agents) into ~/.config/opencode/plugins; delete the file
// or use Remove there to uninstall.
import { spawn } from "node:child_process"

const hook = process.env.KONSOLL_AGENT_HOOK

function report(state, message) {
  if (!hook) return
  const args = ["opencode", state]
  if (message) args.push(String(message).slice(0, 200))
  const child = spawn(hook, args, { stdio: "ignore" })
  child.on("error", () => {})
  child.unref()
}

export const KonsollAgentStatus = async () => {
  let last = ""
  let pendingIdle
  const set = (state, message) => {
    clearTimeout(pendingIdle)
    pendingIdle = undefined
    const key = `${state}\n${message ?? ""}`
    if (key === last) return
    last = key
    report(state, message)
  }
  // A failed turn ends with session.idle right before session.error: the
  // error replaces an idle that has not been reported yet.
  const setIdle = () => {
    clearTimeout(pendingIdle)
    pendingIdle = setTimeout(() => set("idle"), 250)
  }
  return {
    event: async ({ event }) => {
      const p = event.properties ?? {}
      switch (event.type) {
        case "session.status":
          if (p.status?.type === "busy" || p.status?.type === "retry") set("working")
          else if (p.status?.type === "idle") setIdle()
          break
        case "session.idle":
          setIdle()
          break
        case "session.error":
          set("error", p.error?.data?.message ?? p.error?.name)
          break
        case "permission.asked":
          set("waiting", p.permission)
          break
        case "permission.updated":
          set("waiting", p.title)
          break
        case "question.asked":
          set("waiting", p.questions?.[0]?.question)
          break
        case "permission.replied":
        case "question.replied":
        case "question.rejected":
          set("working")
          break
      }
    },
  }
}
