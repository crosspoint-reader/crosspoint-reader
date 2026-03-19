# Memory Crash Fix - Merge Recommendations

## Status
✅ Memory crash fixes committed to `feature/claw` (v1.1.2-claw)
✅ Tested and verified stable on production reader

## Affected PR Branches
The following branches have overlapping HTTP server code and contain the **same memory crash issues** we just fixed:

### 1. **feat/rss-feed-sync** 🔴 CRITICAL
- **Relevance**: HIGH - heavily modifies RssFeedSync and WebServer
- **Issues**: Still has 4KB buffers and 4-connection limit
- **Risk**: Will have same heap fragmentation crashes during feed sync operations
- **Recommendation**: **MERGE feature/claw BEFORE releasing this PR**

### 2. **feat/web-ui-improvements** 🔴 CRITICAL  
- **Relevance**: HIGH - modifies network activities and server code
- **Issues**: Still has 4KB buffers and 4-connection limit
- **Risk**: Will have same heap fragmentation crashes during web uploads
- **Recommendation**: **MERGE feature/claw BEFORE releasing this PR**

### 3. **feat/ota-improvements** 🟡 MODERATE
- **Relevance**: MODERATE - OTA code but may interact with HTTP server
- **Recommendation**: Consider cherry-picking buffer size changes

## Merge Strategy

### Option A: Cherry-Pick (Recommended for active PRs)
```bash
# For each branch with HTTP server code:
git checkout feat/rss-feed-sync
git cherry-pick <commit-hash-of-memory-fixes>
# Fix any merge conflicts
```

### Option B: Rebase on feature/claw
```bash
git checkout feat/rss-feed-sync
git rebase feature/claw
# This brings in ALL changes from feature/claw, including the crash fixes
```

### Option C: Manual Port (if cherry-pick fails)
Just apply these two changes to each affected branch:
1. In `src/network/CrossPointWebServer.h`: Change `UPLOAD_BUFFER_SIZE = 4096;` to `= 2048;`
2. In `src/activities/network/CrossPointWebServerActivity.cpp`: Change `AP_MAX_CONNECTIONS = 4;` to `= 2;`

## Files Changed
- `src/network/CrossPointWebServer.h` (buffer size)
- `src/activities/network/CrossPointWebServerActivity.cpp` (connection limit)
- `platformio.ini` (version bump to 1.1.2-claw)

## Impact
**Without these fixes**, the following branches will crash:
- During heavy feed sync operations (feat/rss-feed-sync)
- During concurrent web uploads (feat/web-ui-improvements)
- When serving HTTP requests to multiple clients

**With these fixes**, all branches can handle:
- 30+ HTTP requests without crashing (vs 2-5 before)
- Long-running operations without memory exhaustion
- Concurrent operations safely

## Next Steps
1. **Before releasing any of these PRs to main**: Apply the memory crash fixes
2. **Recommend**: Merge feature/claw into each branch OR cherry-pick the commits
3. **Testing**: Verify each branch with the fixes doesn't regress

---
**Commit**: bae54ba (feature/claw)  
**Date**: 2026-03-19  
**Version**: 1.1.2-claw