---
name: review-i18n-docs
description: Read-only firmware-handoff review of tr(), English source strings, generated-file workflow, and user documentation for visible changes.
---

# English strings and user documentation review

Review the pinned firmware change without editing files.

Check:

1. Every new user-facing string uses `tr()` and has an English source entry. Other languages may fall back to English and are not required for handoff.
2. Translation source YAML, generated i18n files, and generated HTML follow the repository workflow; ignored generated outputs are not intended for commit.
3. A new feature or other behavior an end user should know about updates the appropriate user-facing documentation.
4. Documentation describes the merged behavior in plain English and does not promise unverified hardware behavior.
5. Developer-only refactors, bug fixes with no user-visible effect, and internal scripts do not receive unnecessary user documentation.

Report `file:line`, the missing or misleading user-facing information, and the minimal source file to update. A clean result is normal.

**Done when:** every changed user-facing behavior has an English string path and, when useful to users, accurate documentation.
