# WriterDeck X4 Development Flow

Fork upstream CrossPoint at the start of Stage 0.

Use these remotes:

- `upstream`: `https://github.com/crosspoint-reader/crosspoint-reader.git`
- `origin`: the user fork

Keep the upstream-tracking branch clean. Do not commit writerdeck work directly to it.

Use named topic branches for local work:

- `stage-0-simulator`
- `stage-1-writer-activity`
- `stage-1-editor-core`
- `stage-1-storage-recovery`

To see local work relative to upstream:

```bash
git fetch upstream
git log --oneline upstream/master..HEAD
git diff upstream/master...HEAD
```

Use `git range-diff` after rebasing a branch to check that local commits still express the same changes:

```bash
git range-diff upstream/master old-branch new-branch
```

Prefer rebasing while the fork-local work is small. If conflicts become regular or the writer mode grows across many CrossPoint modules, switch to a documented integration branch and merge upstream into it regularly.

Record fork-local decisions in `docs/writerdeck/` so future sessions can tell what is intentional writerdeck behavior and what is inherited CrossPoint behavior.
