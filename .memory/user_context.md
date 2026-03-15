---
name: user_context
description: User context — CrossPoint Reader project, Docker sandbox, glitchy host requiring frequent session resumption
type: user
---

User is working on the CrossPoint Reader project (ESP32-C3 e-reader firmware) from a Docker sandbox environment. Their host computer is glitchy and session interruptions are frequent. They rely on the memory system to resume interrupted sessions.

**Session continuity is critical**: At the end of any significant work block, proactively save project memories capturing current task state, what was completed, what's next, and any open decisions — so interrupted sessions can be resumed with full context.
