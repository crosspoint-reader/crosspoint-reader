# Cherry-picks TODO

Upstream commits to cherry-pick into `feature/rss-feed-sync` (in order).
Created 2026-03-01. Cherry-pick one at a time, build+test after each.

Note: `d1e786a` + `a6a078b` (stack overflow fix) being handled first separately.

## Queue

```bash
git cherry-pick de60aec  # fix: properly implement requestUpdateAndWait() (#1218)
git cherry-pick f946400  # fix: Hide unusable button hints in empty directory (#1253)
git cherry-pick b789677  # fix: add missing keyboard metrics to Lyra3CoversTheme (#1101)
git cherry-pick 8f233af  # fix: remove bookProgressBarHeight from Lyra3CoversTheme
git cherry-pick 567b589  # feat: replace picojpeg with JPEGDEC (#1136)
git cherry-pick e794a17  # feat: WIFI pill, feed log fix, JPEGDEC version string
git cherry-pick 7ed1242  # feat: Add git branch to version string on settings (#1225)
git cherry-pick a4de63d  # fix: DZ auto-connect navigate directly to QR screen
git cherry-pick 3cd5883  # perf: remove unused ConfirmationActivity member (#1234)
```

## Notes
- Build with `pio run -e default` after each cherry-pick
- Flash to reader and verify no crash before moving to next
- If conflicts arise, resolve manually — keep fork additions intact
