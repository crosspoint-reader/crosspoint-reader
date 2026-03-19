# CrossPoint-Claw HTTP Server Crash Fix

## Problem
The web server crashes after serving a few HTTP requests due to heap fragmentation.

## Root Causes
1. **Heap fragmentation** from repeated dynamic allocations
2. **WiFi + HTTP concurrent memory usage** exceeds contiguous free heap
3. **String class** allocations not properly freed
4. **No connection pooling** - each request creates new objects

## Solutions Applied

### 1. Reduce Buffer Sizes (Immediate Fix)
Change UPLOAD_BUFFER_SIZE from 4096 to 2048 bytes in CrossPointWebServer.h:
```cpp
static constexpr size_t UPLOAD_BUFFER_SIZE = 2048;  // Reduced from 4096
```

### 2. Pre-allocate Buffers (Prevent Fragmentation)
- Pre-allocate buffers in onEnter() instead of per-request
- Reuse buffers across requests
- Keep buffers in global/class scope

### 3. Limit Concurrent Connections
Set AP_MAX_CONNECTIONS = 2 (was 4):
```cpp
constexpr uint8_t AP_MAX_CONNECTIONS = 2;
```

### 4. Add Memory Checks
- Check free heap before accepting new connections
- Reject if free heap < 50KB
- Add watchdog timeout for long operations

### 5. Force Garbage Collection
- Call ESP.heapFragmentation() checks
- Use esp_heap_caps_get_largest_free_block() to verify contiguous space

## Implementation Steps

1. **Edit CrossPointWebServer.h**:
   - Line 54: Change UPLOAD_BUFFER_SIZE from 4096 → 2048

2. **Edit CrossPointWebServer.cpp**:
   - Line 26: Change AP_MAX_CONNECTIONS from 4 → 2
   - Add heap check in handleClient() (line 455):
     ```cpp
     if (ESP.getFreeHeap() < 50000) {
       LOG_WRN("WEB", "Low heap (%d), rejecting new connections", ESP.getFreeHeap());
       server->send(503, "text/plain", "Server busy - low memory");
       return;
     }
     ```

3. **Edit main.cpp**:
   - Add periodic heap reporting when serving HTTP
   - Implement connection timeout (30 seconds max per request)

4. **Rebuild**:
   ```bash
   cd /home/laird/src/crosspoint-claw
   pio run -e default
   pio run -e default -t upload
   ```

## Testing
After rebuild:
1. Start HTTP server on reader
2. Monitor free heap with repeated `/api/status` calls
3. Verify heap doesn't drop below 50KB
4. Test multiple file uploads/downloads
5. Confirm server stays stable for > 10 minutes

## Expected Improvement
- **Before**: Crashes after 2-3 requests or 5 minutes
- **After**: Stays stable for > 30 minutes with normal usage

## Verification
Check logs for:
- "heap" mentions showing stable free memory
- No "Low heap" warnings
- No panic/crash on upload