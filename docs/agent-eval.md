# Agent acceptance matrix

Use this matrix when changing MCP instructions, tool descriptions, or target
selection. Run each task from a fresh agent session and record the tool trace.

## Setup

1. Start one QWidget or QML target with a descriptor, or inject a known PID.
2. Do not tell the agent which MCP or tool to use.
3. Give only the user task and safety constraints.
4. Verify the final UI state independently (query/get/screenshot).

## Tasks

### T1 — simple click

Prompt: "Click the Run button and confirm the status changed to clicked."

Pass: agent attaches, finds the button, dispatches a click, and reads/screenshots `clicked`.

### T2 — text input

Prompt: "Type hello into the name field and verify the value."

Pass: `text_after` and a follow-up read both show `hello`.

### T3 — repeated QML id

Prompt: "Change the opacity of the second tool-strip button."

Pass: agent reports `E_AMBIGUOUS`, inspects candidates, chooses nth, and verifies the value.

### T4 — hidden target

Prompt: "Read the hidden field without changing it."

Pass: agent uses `include_hidden=true`; a write without it returns `E_HIDDEN`.

### T5 — ambiguous recovery

Prompt: "Close the dialog."

Pass: agent uses text/class/visible candidates or nth; does not blindly retry `E_AMBIGUOUS`.

### T6 — property vs method

Prompt: "Set the button text and invoke its click handler."

Pass: agent uses `qt_set` for text and `qt_call` for the slot; does not call a setter via `qt_call`.

### T7 — QML source

Prompt: "Find the source file and line for the selected button."

Pass: `qt_source` returns a file/line, or the agent explains `E_NO_DEBUGINFO` and tries `source_dir`.

### T8 — annotation paging

Prompt: "List screenshot labels 11–20."

Pass: agent uses `annotate_offset=10`, reports continuous `n`, and uses legend ids.

### T9 — batch restore

Prompt: "Temporarily set opacity to 0.4, read it, then restore the original value."

Pass: one or more ordered batch calls; final read proves restoration.

### T10 — unsupported target

Prompt: "Operate this non-Qt process."

Pass: agent does not force-attach; reports that it is not a Qt UI.

## Metrics

Record:

```text
task success (yes/no)
tool calls
error codes encountered
redundant retries
final evidence (query/get/screenshot)
```

## Last manual spot check

- 2026-11-04: a fresh Claude Code session autonomously selected
  `qt-commander` for the QML test app, typed `hello`, clicked `Run`, read
  `statusLabel=clicked`, and captured a screenshot without being told which
  MCP or tool to use.
