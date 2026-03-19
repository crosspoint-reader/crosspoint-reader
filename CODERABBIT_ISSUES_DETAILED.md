# CodeRabbit Issues - Detailed Fix Guide

## PR #59 (feat/web-ui-improvements) - HIGH PRIORITY

### Issue 1: Uninitialized mutex
- **Type**: Thread Safety - Uninitialized Mutex
- **File**: Find `s_receivedFilesMutex`
- **Fix**: Implement lazy initialization pattern
- **Details**: The mutex is declared static but never explicitly initialized. Use Meyer's singleton or std::call_once pattern.

### Issue 2: Data race on s_receivedFiles flag
- **Type**: Data Race
- **File**: Find `s_receivedFiles`
- **Fix**: Change to `std::atomic<bool>`
- **Details**: Flag is accessed from multiple threads without synchronization. Convert to atomic for thread-safe access.

### Issue 3: Debug stubs in panic_stubs.c
- **Type**: Conditional Compilation
- **File**: `panic_stubs.c`
- **Fix**: Wrap with `#ifndef NDEBUG` or exclude from build in debug mode
- **Details**: Panic stubs should only be compiled in non-debug builds. Add preprocessor guards or CMake exclusion.

---

## PR #55 (feat/rss-feed-sync) - MEDIUM PRIORITY

### Issue 1: NULL dereference in FsHelpers.cpp
- **Type**: Null Pointer Dereference
- **File**: `FsHelpers.cpp` (lines 45-50)
- **Fix**: Add guard for `strlen(extension)`
- **Details**: Before calling strlen() on extension, verify it's not NULL. Add null check guard.

### Issue 2: Version mismatch in Section.cpp
- **Type**: Version Compatibility
- **File**: `Section.cpp` (line 12)
- **Fix**: Verify `SECTION_FILE_VERSION` sync
- **Details**: Check that SECTION_FILE_VERSION constant matches between declaration and usage. Ensure consistency across files.

---

## PR #62 (feat/link-navigation) - HIGH PRIORITY

### Issue 1: Vector invariant violation
- **Type**: Container Invariant
- **File**: `ChapterHtmlSlimParser.cpp`
- **Fix**: Add assertion to verify vector state
- **Details**: Add assertion after vector operations to ensure the vector maintains expected state/size/ordering invariants.

---

## PR #53 (feat/pulsr-theme) - HIGH PRIORITY

### Issue 1: Static initialization order problem
- **Type**: Static Initialization Order Fiasco (SIOF)
- **File**: `UITheme.cpp` (or wherever UITheme is defined)
- **Fix**: Change to Meyer's singleton pattern
- **Details**: Replace static instance with a static function that returns a reference to a static object (Meyer's singleton). This guarantees safe initialization order.
  
  Pattern:
  ```cpp
  UITheme& UITheme::getInstance() {
    static UITheme instance;
    return instance;
  }
  ```

---

## Fix Workflow

For each branch:
1. Checkout: `git checkout origin/BRANCH_NAME`
2. Locate the exact code mentioned
3. Apply the fix from the description
4. Test compilation: `pio run -e default`
5. Commit: `git commit -am "fix: [issue-name] CodeRabbit review fix"`
6. Push: `git push origin BRANCH_NAME`
7. Report: `BRANCH_NAME: FIXED (issues), PUSHING...`

Expected order of processing:
- PR #59 (HIGH) 
- PR #55 (MEDIUM)
- PR #62 (HIGH)
- PR #53 (HIGH)

All fixes should maintain backward compatibility and not change the feature's core functionality.
