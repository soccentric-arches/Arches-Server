---
description: Static analysis — but know that make lint exits 0 when the tools are missing
allowed-tools: Bash(make lint), Bash(make cppcheck), Bash(./.claude/validate.sh:*)
---

!`./.claude/validate.sh quick`

Prefer the gate over calling make lint directly: make lint here prints
"not installed; skipping" and exits 0 when the analyser is absent — a green
light that analysed nothing. The gate requires the tool and reports 77
(GATE-BLOCKED) when it's missing. Never report a 77 as a pass.
