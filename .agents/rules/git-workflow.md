## Git Workflow and Repository Awareness

### Repository Detection Protocol

Use the session's branch, remote, and status checks; inspect their roles rather
than assuming `origin` is a fork or that an `upstream` remote exists.

### Git Operation Rules

- Integration and PR comparisons target `develop`, not `master` or remote HEAD.
- Authorization follows [human ownership](../../AGENTS.md#human-ownership).
  Before an explicitly requested push, recheck remotes and use the destination
  specified by the human. Otherwise prefer the personal-fork remote (`fork`
  when present); verify its URL rather than assuming a name establishes its role.
  Ask if the destination is unclear.
- No assistant co-author or generated-by trailers.
- When adapting/superseding another person's PR, verify the original human
  author and credit them with `Co-Authored-By`; skip bot authors.

### Branch Naming Convention

Use `<type>/<short-description>`, where type is `feat`, `fix`, `refactor`,
`docs`, `test`, `chore`, or `perf`, matching PR title prefixes.

### Commit Message Format

Use `<type>: <short summary>` (summary at most 50 characters), with an optional
body containing relevant facts and verification. Types match branch prefixes.

### When to Commit

A local commit requires explicit human approval, completed requested work,
relevant passing checks, and [firmware-handoff](../skills/firmware-handoff/SKILL.md)
for firmware/build changes. A pure refactor preserves behavior. Do not commit
experiments, failed builds, build warnings, or ignored outputs; inspect status
and `.gitignore` before staging. Exclude unrelated user work. Ask if commit
authorization is unclear.

Hardware testing is required before a PR, not before an approved local commit.
The human owns hardware results and PR prose/publication.
