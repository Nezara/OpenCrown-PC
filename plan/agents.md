# Agent process (adapted from the ACNB project, `DecompWork/port/plan/agents.md`)

Cost driver there was context re-read (~73% of tokens): files / asm read early stay in context and are re-read every
turn. So bounded work runs in a **fresh context per unit**, and the main session stays lean.

- **Coordinator = main session.** Reads PLAN, agent reports, git log; not asm or big dumps. Picks the next unit,
  spawns one agent per unit, relays reports, asks the user's questions, keeps PLAN "Current state" short.
  While a background agent runs, every reply ends with a bold "agent still working" line.
- **mapper** (`.claude/agents/mapper.md`, Sonnet, read-only): one subsystem (or one asset-format loader) per spawn.
  Writes `notes/map/<name>.md`. Sole user of `ghidra.ps1` while it runs.
- **formatter** (`.claude/agents/formatter.md`, Opus medium): one asset format per spawn, from a mapper's loader
  map: reader in `port/src/formats/`, synthetic test, `verify` over the dump, commit, short report.
- **debugger** (`.claude/agents/debugger.md`, Opus high, read-only): escalation after **2 attempts with no new
  information** on one problem. The stuck agent reports a brief (symptom, attempts + what each showed, ruled out,
  addresses, hypothesis, repro); coordinator spawns the debugger, then resumes the stuck agent with its findings.
- Spawn size: one subsystem / format per spawn; if a unit grows past ~40 functions read, commit what is done and
  report the rest.
- Stage 6 adds porter WRITER / VERIFIER split (ACNB s66: ~45% cheaper per function) - define it then.
- Usage: `python .claude/usage/usage_report.py --since <UTC> --md .claude/usage/<name>.md`.
