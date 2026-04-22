# Troubleshooting

This document show most common issues and possible solutions while using the device features.

- [Troubleshooting](#troubleshooting)
    - [Cannot See the Device on the Network](#cannot-see-the-device-on-the-network)
    - [Connection Drops or Times Out](#connection-drops-or-times-out)
    - [Upload Fails](#upload-fails)
    - [Saved Password Not Working](#saved-password-not-working)

### Cannot See the Device on the Network

**Problem:** Browser shows "Cannot connect" or "Site can't be reached"

**Solutions:**

1. Verify both devices are on the **same WiFi network**
   - Check your computer/phone WiFi settings
   - Confirm the CrossPoint Reader shows "Connected" status
2. Double-check the IP address
   - Make sure you typed it correctly
   - Include `http://` at the beginning
3. Try disabling VPN if you're using one
4. Some networks have "client isolation" enabled - check with your network administrator

### Connection Drops or Times Out

**Problem:** WiFi connection is unstable

**Solutions:**

1. Move closer to the WiFi router
2. Check signal strength on the device (should be at least `||` or better)
3. Avoid interference from other devices
4. Try a different WiFi network if available

### Upload Fails

**Problem:** File upload doesn't complete or shows an error

**Solutions:**

1. Ensure the file is a valid `.epub` file
2. Check that the SD card has enough free space
3. Try uploading a smaller file first to test
4. Refresh the browser page and try again

### Saved Password Not Working

**Problem:** Device fails to connect with saved credentials

**Solutions:**

1. When connection fails, you'll be prompted to "Forget Network"
2. Select **Yes** to remove the saved password
3. Reconnect and enter the password again
4. Choose to save the new password

### Smart (WPM) Auto Page Turn Flipping Too Fast or Too Slow

**Problem:** In Smart mode, pages turn faster or slower than you read.

**Solutions:**

1. Open the **Reader Menu** (press **Confirm** while reading) and select **Reset Reading Speed** to clear the learned WPM and start fresh.
2. The status bar shows the current learned speed (e.g. `Auto Turn Enabled: 230 wpm`) or `Auto Turn Enabled: Uncalibrated` if no speed has been learned yet. Use this to confirm the mode is active.
3. Speed is learned from manual page turns: pressing **Next Page** before the timer fires nudges the speed up; pressing **Previous Page** nudges it down. Turning pages consistently will calibrate the speed over several pages.
4. Very short page turns (under 2 seconds) are ignored as accidental taps and do not affect the learned speed.
5. If you walked away mid-page, the stored WPM may have drifted lower than your actual reading speed — use **Reset Reading Speed** to recover.

### Auto Page Turn Timer Does Not Advance

**Problem:** In Smart mode the page does not flip automatically.

**Solutions:**

1. Ensure **Auto Turn (Pages Per Minute)** is set to **Smart (WPM)** and not **Off** — check via **Reader Menu → Auto Turn (Pages Per Minute)**.
2. If you pressed **Previous Page** one or more times, the auto-turn timer is paused until you manually press **Next Page** enough times to return to where you left off (one forward press required per backward press). This prevents an unwanted auto-flip while you are browsing back through the book.
3. Selecting **Reset Reading Speed** from the Reader Menu will also un-pause the timer immediately.
4. As a last resort, open the **Reader Menu**, set **Auto Turn (Pages Per Minute)** to **Off**, then set it back to **Smart (WPM)** — this resets the counter and restarts the timer.
