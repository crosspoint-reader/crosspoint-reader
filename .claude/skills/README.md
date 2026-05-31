# CrossPoint Reader: Claude Code skills

Project skills for Claude Code. Claude auto-discovers them and loads one when the
task matches its `description`; you do not invoke them by hand. They encode how
this project wants C/C++ written: the judgment calls and self-review gates that
keep the firmware small, stable, and reviewable.

These are written for capable agents, not beginners. They are principle- and
decision-focused on purpose. They deliberately avoid line-number citations,
which drift; they anchor on durable names (APIs, types, macros, files).

This is separate from `.skills/SKILL.md`, the GitHub coding-agent guide that
mirrors CLAUDE.md. CLAUDE.md stays the always-loaded rule set; these skills are
the applied decision procedures that load on demand and add the judgment layer
CLAUDE.md does not carry.

| Skill | Loads when you are... |
|---|---|
| `heap-discipline` | allocating memory: new/malloc/vector/string, buffers, caches |
| `control-flow-clarity` | writing branching logic, state flags, modes, if/else ladders |
| `hal-and-abstractions` | touching storage, input, display, settings, i18n, rendering |
| `scope-discipline` | adding a feature, activity, lib, setting, or dependency |
| `refactor-for-review` | refactoring, cleaning up, or preparing a change for PR |

Each skill ends with a self-review checklist Claude runs against its own diff
before handing it back. Reviewing a PR? Those checklists double as a fast rubric.

## Maintaining these

Edit the `SKILL.md` under each directory. Keep them tight. Do not restate
CLAUDE.md; add the judgment CLAUDE.md cannot afford to carry. Trigger quality
lives in the `description` field: it must name the situations that should pull
the skill in, in the words a contributor's task would use.
