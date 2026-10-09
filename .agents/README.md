# CrossPoint Reader agent instructions

The root `AGENTS.md` is the always-loaded policy and router. Detailed repository facts live in `rules/`; reusable procedures and review axes live in `skills/`.

Load all task-matched rules and skills. Firmware handoff is the only fixed
multi-skill bundle; other tasks require every skill matched by the root routing
table or its description.

`CLAUDE.md` links to the root guide. Each skill lives in a directory containing
`SKILL.md`, with a matching frontmatter `name`. Descriptions identify when an
agent should load a skill; contributors can also name it explicitly. Skills
provide decision procedures and self-review checklists; those checklists also
serve as review rubrics. Keep them short, anchor them on durable API/type/file
names, and avoid line-number references that drift. Edit the skill at its new
home rather than restoring `.skills/` or duplicating the root policy.

## Rules

- `environment.md`: host detection, PlatformIO, and local configuration
- `hardware-resources.md`: target hardware and resource protocol
- `architecture-hal.md`: build flags, repository structure, and HAL boundaries
- `coding-standards.md`: C/C++ conventions and platform pitfalls
- `ui-activities.md`: orientation, input, UI, activities, tasks, and fonts
- `testing-debugging.md`: build, formatting, CI, serial, crash, and verification procedures
- `git-workflow.md`: repository context, branches, commits, and publication controls
- `generated-files-cache.md`: generated sources, i18n, local artifacts, and cache formats

## Mandatory firmware review

`firmware-handoff` dispatches four independent read-only axes:

- `review-correctness`
- `review-architecture`
- `review-embedded`
- `review-i18n-docs`

The main agent verifies and fixes their findings before asking the human to review the diff and architecture.
