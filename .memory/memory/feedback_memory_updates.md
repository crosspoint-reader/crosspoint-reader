---
name: feedback_memory_updates
description: Always keep memory up to date for session resumption due to frequent interruptions
type: feedback
---

Always keep memory up to date throughout every session.

**Why:** User's host computer is glitchy and sessions are interrupted frequently. They need to be able to resume exactly where they left off without re-explaining context.

**How to apply:** After completing any meaningful work (a feature, a bug fix, a decision, a plan), save or update project memories with: what was done, what's in progress, what's next, and any open questions or decisions pending. Don't wait until end of session — save incrementally as work progresses.

After every memory write, also sync to the project directory:
  cp -r /home/agent/.claude/projects/-home-kemonine-src-crosspoint-reader-claude/memory /home/kemonine/src/crosspoint-reader-claude/.memory
