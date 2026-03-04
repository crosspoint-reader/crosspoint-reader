# Modular Architecture Guide

## Overview

CrossPoint Reader now has a fully modular feature flag system that allows users to pick and choose exactly which features they want in their firmware. This document describes the architecture, testing strategy, and how to add new features.

## Architecture Principles

### 1. Feature Independence

Each feature is independently toggleable:
```cpp
#ifndef ENABLE_MARKDOWN
#define ENABLE_MARKDOWN 1
#endif

#if ENABLE_MARKDOWN
// Feature implementation
#endif
```

**Key principles:**
- Features default to ENABLED (1) for backward compatibility
- Disabling a feature removes it entirely at compile-time (no runtime cost)
- Features have defined dependencies and conflicts
- Unimplemented features are prevented from being enabled

### 2. Dependency Management

Dependencies are declared in `scripts/generate_build_config.py`:

```python
FEATURE_METADATA = {
    'markdown': FeatureMetadata(
        implemented=True,       # Is the code actually written?
        stable=True,           # Is it production-ready?
        requires=[],           # Must have these features enabled
        conflicts=[],          # Cannot coexist with these
        recommends=[]          # Works best with these
    ),
}
```

The build system validates dependencies before generating configuration.

### 3. Graceful Degradation

When disabled features are accessed, the system shows user-friendly errors:

```cpp
#if ENABLE_MARKDOWN
  return loadMarkdown(path);
#else
  showFeatureDisabledError("Markdown support\nnot available\nin this build");
#endif
```

**Never crash or show cryptic errors when features are disabled.**

### 4. Runtime Observability

The core feature API (`core::FeatureCatalog`) provides runtime observability and
dependency validation. `FeatureManifest` remains as a compatibility wrapper for
existing call sites.

```cpp
#include "core/features/FeatureCatalog.h"

if (core::FeatureCatalog::isEnabled("markdown")) {
  // Safe to use markdown-specific runtime paths
}

// Validate dependency graph at startup
String error;
bool ok = core::FeatureCatalog::validate(&error);

// Expose via web API (/api/plugins)
String json = core::FeatureCatalog::toJson();
```

### 5. Feature Startup Lifecycle

Feature-specific startup behavior (user fonts, dark mode, integration credential
stores) is centralized behind explicit lifecycle hooks rather than scattered as
`#if ENABLE_X` blocks in `main.cpp`.

```cpp
#include "core/features/FeatureLifecycle.h"

// After SD card mounted — features scan storage resources (font families, etc.)
core::FeatureLifecycle::onStorageReady();

// After settings loaded — features apply config to hardware/runtime state
core::FeatureLifecycle::onSettingsLoaded(renderer);

// During font setup — feature-provided font families are registered
core::FeatureLifecycle::onFontSetup(renderer);
```

`main.cpp` calls each stage once in order; each hook internally gates its work
behind compile-time feature flags so disabled features cost nothing.

Internally, `FeatureLifecycle` uses a registry table keyed by feature id,
making startup wiring additive (add one registry entry) instead of modifying
central boot flow logic.

**Adding startup behavior for a new feature:**

1. Add the feature's startup code inside the appropriate hook in
   `src/core/features/FeatureLifecycle.cpp`, guarded by `#if ENABLE_MY_FEATURE`.
2. No changes to `main.cpp` are required.

### 6. External Sleep App Hook

Sleep rendering has an extension point for future integrations (for example, TRMNL-like dashboards during sleep).

Implement this weak symbol in any compiled module:

```cpp
#include "activities/boot_sleep/SleepExtensionHooks.h"

namespace SleepExtensionHooks {
bool renderExternalSleepScreen(GfxRenderer& renderer, MappedInputManager& mappedInput) {
  // Render your external sleep UI.
  // Return true when handled, false to fall back to built-in behavior.
  return false;
}
}  // namespace SleepExtensionHooks
```

Behavior:
- If implemented and returns `true`, `SleepActivity` skips built-in sleep rendering.
- If missing or returns `false`, standard CrossPoint sleep rendering is used.

## Current Features

| Feature | Flag | Size | Status | Dependencies |
|---------|------|------|--------|--------------|
| Extended Fonts | `ENABLE_EXTENDED_FONTS` | ~3.5MB | ✅ Stable | None |
| PNG/JPEG Sleep | `ENABLE_IMAGE_SLEEP` | ~33KB | ✅ Stable | None |
| Book Images | `ENABLE_BOOK_IMAGES` | ~0KB | ✅ Stable | None |
| Markdown/Obsidian | `ENABLE_MARKDOWN` | ~158KB | ✅ Stable | None |
| Integrations Base | `ENABLE_INTEGRATIONS` | ~0KB | ✅ Stable | None |
| KOReader Sync | `ENABLE_KOREADER_SYNC` | ~2KB | ✅ Stable | `ENABLE_INTEGRATIONS` |
| Calibre Sync | `ENABLE_CALIBRE_SYNC` | ~17KB | ✅ Stable | `ENABLE_INTEGRATIONS` |
| Background Server | `ENABLE_BACKGROUND_SERVER` | ~4KB | ✅ Stable | None |
| Web Pokedex Plugin | `ENABLE_WEB_POKEDEX_PLUGIN` | ~34KB | ✅ Stable | None |
| Pokemon Party | `ENABLE_POKEMON_PARTY` | ~4KB | ✅ Stable | `ENABLE_WEB_POKEDEX_PLUGIN` |

## Testing Strategy

### Automated Testing (CI)

**Feature Matrix Tests** (`.github/workflows/feature-matrix-test.yml`):
- Tests 11 critical combinations on every PR
- Validates firmware sizes
- Catches build failures early
- Runs in ~15 minutes

**Tested Combinations:**
1. All 3 profiles (lean, standard, full)
2. Each individual feature
3. Important combinations (e.g., "all except markdown")
4. Edge cases

**Nightly Tests:**
- Full 256-combination test (all possible feature sets)
- Size regression detection
- Non-linear interaction detection

### Manual Testing

Before release, test on physical hardware:
1. Lean profile - verify core functionality
2. Standard profile - verify common usage
3. Full profile - verify all features work together

### Size Validation

Run `scripts/measure_feature_sizes.py` monthly:
```bash
python scripts/measure_feature_sizes.py
```

This measures actual firmware sizes and detects:
- Size estimate drift
- Non-linear interactions between features
- Flash capacity violations

## Adding a New Feature

### Step 1: Implement the Feature

Create your feature code normally, without feature flags initially.

### Step 2: Add Feature Flag

Add the flag definition in the appropriate header:

```cpp
// In MyFeature.h or main.cpp
#ifndef ENABLE_MY_FEATURE
#define ENABLE_MY_FEATURE 1
#endif
```

### Step 3: Guard the Code

Wrap feature code with conditional compilation:

```cpp
#if ENABLE_MY_FEATURE
// Implementation
class MyFeature {
  // ...
};
#endif
```

Guard includes:
```cpp
#if ENABLE_MY_FEATURE
#include "MyFeature.h"
#endif
```

Guard instantiation:
```cpp
#if ENABLE_MY_FEATURE
  auto feature = std::make_unique<MyFeature>();
#else
  showFeatureDisabledError("MyFeature not available");
#endif
```

### Step 4: Add to Build System

Update `scripts/generate_build_config.py`:

```python
FEATURES = {
    # ... existing features ...
    'my_feature': Feature(
        name='My Feature Name',
        flag='ENABLE_MY_FEATURE',
        size_kb=100,  # Estimate, then measure
        description='Brief description of feature'
    ),
}

FEATURE_METADATA = {
    # ... existing metadata ...
    'my_feature': FeatureMetadata(
        implemented=True,
        stable=True,
        requires=['other_feature'],  # If dependent
        conflicts=[],
        recommends=[]
    ),
}
```

### Step 5: Update Feature Manifest

Add to `src/FeatureManifest.h`:

```cpp
#ifndef ENABLE_MY_FEATURE
#define ENABLE_MY_FEATURE 1
#endif

class FeatureManifest {
  // ... existing methods ...
  static constexpr bool hasMyFeature() { return ENABLE_MY_FEATURE != 0; }

  // Update toJson():
  static String toJson() {
    // ... existing fields ...
    json += ",\"my_feature\":" + String(hasMyFeature() ? "true" : "false");
    // ...
  }

  // Update printToSerial():
  static void printToSerial() {
    // ... existing output ...
    Serial.printf("  My Feature:        %s\n", hasMyFeature() ? "ENABLED " : "DISABLED");
  }
};
```

### Step 6: Add Tests

Update `.github/workflows/feature-matrix-test.yml`:

```yaml
- name: "My Feature only"
  config: "--enable my_feature"
  expected_max_size_mb: 5.7
```

### Step 7: Measure Actual Size

```bash
python scripts/measure_feature_sizes.py
# Update size_kb in generate_build_config.py with actual measurement
```

### Step 8: Update Documentation

- Add to `docs/BUILD_CONFIGURATION.md` feature reference
- Update web UI (`docs/configurator/index.html`)
- Document any user-facing changes

## Best Practices

### Do's ✅

- **Guard entire features** - If disabling, remove completely
- **Default to enabled** - Backward compatibility
- **Show user-friendly errors** - No crashes or cryptic messages
- **Validate dependencies** - Prevent invalid combinations
- **Measure actual sizes** - Don't guess
- **Test all combinations** - Use automated CI

### Don'ts ❌

- **Don't use runtime checks** - Compile-time only (except error messages)
- **Don't guess dependencies** - Declare them explicitly
- **Don't break default builds** - All flags default to 1
- **Don't expose unimplemented features** - Set `implemented=False`
- **Don't forget documentation** - Update all relevant docs

## Troubleshooting

### Build Fails with "undefined reference"

**Cause:** Feature code is referenced but not guarded.

**Fix:** Find the reference and wrap it:
```cpp
#if ENABLE_FEATURE
  useFeature();
#endif
```

### Configuration Validation Fails

**Cause:** Trying to enable unimplemented feature or violate dependencies.

**Fix:** Check error message:
```bash
python scripts/generate_build_config.py --enable my_feature
# ❌ Configuration errors:
#   • My Feature is not yet implemented...
```

### Firmware Size Exceeds Estimate

**Cause:** Size estimates are outdated or features interact non-linearly.

**Fix:** Re-measure sizes:
```bash
python scripts/measure_feature_sizes.py
# Update estimates in generate_build_config.py
```

### Feature Doesn't Work When Enabled

**Cause:** Missing initialization or incomplete guards.

**Debug:**
1. Check if feature is actually enabled:
   ```cpp
   Serial.printf("Feature enabled: %d\n", ENABLE_MY_FEATURE);
   ```
2. Verify no guards are missing
3. Check dependencies are also enabled

## CI/CD Integration

### On Every PR

- Feature matrix tests run automatically
- Validates 11 critical combinations
- Fails if any combination doesn't build
- Reports firmware sizes

### Nightly

- Full 256-combination test
- Size regression detection
- Results archived for trending

### On Release

- Manually test lean, standard, full on hardware
- Run `measure_feature_sizes.py` to update estimates
- Update documentation if features changed

## Performance Considerations

### Compile Time

- Full build: ~5 minutes
- Custom build: ~5 minutes (same)
- Configuration generation: <1 second

**Feature flags have zero compile-time overhead** - same build time regardless of which features enabled.

### Flash Size

- Lean: ~2.6MB (~3.8MB savings vs full)
- Standard: ~6.2MB (recommended)
- Full: ~6.4MB (tight headroom)

**Feature flags enable significant flash savings** for users who don't need all features.

### Runtime Performance

- Feature detection: zero cost (constexpr)
- Disabled code: completely removed (zero cost)
- Error messages: only when accessing disabled features

**Feature flags have zero runtime overhead** - no performance difference vs always-on code.

## Future Enhancements

### Planned Features

- OPDS browser (toggleable)
- Advanced hyphenation (toggleable)
- Experimental formats (toggleable)
- Custom font pipeline in web picker (user uploads TTF/OTF, conversion runs locally, generated bitmap pack is baked into custom build) - roadmap only, pending value/performance tradeoff review

### System Improvements

- Pre-built firmware assets (common configs as release artifacts)
- Web service for one-click builds
- Feature usage telemetry
- A/B firmware partitions for safe rollback

## References

- **Fork & Branch Strategy:** `docs/fork-strategy.md`
- **Build Configuration:** `docs/BUILD_CONFIGURATION.md`
- **Test Plan:** `docs/FEATURE_PICKER_TEST_PLAN.md`
- **SRE Analysis:** Result from sre-code-reviewer agent

## Questions?

- Check troubleshooting section above
- Review test failures in CI logs
- Open an issue on GitHub
- Consult main README.md

---

**Last Updated:** 2026-02-05
**Status:** Production-Ready (P0+P1 items complete)
**Next Review:** Monthly (measure sizes, update docs)
