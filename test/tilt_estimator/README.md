# Tilt estimator tests and device diagnostics

The `IMUTiltEstimatorTest` suite compiles
`lib/hal/HalTiltSensor_IMUTiltEstimator.cpp` directly on the host and uses
only its public sample, pointer, and read-only observation interfaces.
`HalTiltSensorTest` additionally compiles the production `HalTiltSensor.cpp`
against a controllable IMU stub to cover event ownership and activity reporting.
Filter and HAL logic are not copied into the tests.

Parenthesized tuning names refer to `static constexpr` members in
`lib/hal/HalTiltSensor_IMUTiltEstimator.h` unless identified as a build-time
override.

## Run the suite

From the repository root:

```sh
cmake -S test -B build/test
cmake --build build/test --target IMUTiltEstimatorTest HalTiltSensorTest
ctest --test-dir build/test -R '^(IMUTiltEstimatorTest|HalTiltSensorTest)\.' --output-on-failure
```

The normal suite covers:

- sustained stationary calibration, measured QMI8658 noise, and all three gyro offsets;
- axis-aligned and arbitrary initial gravity vectors;
- positive-Z, negative-Z, upright, and vertical-crossing operation;
- pure and mixed three-axis motion compared with independent quaternion truth;
- held-tilt repeat, return to neutral, orientation, inversion, and sensitivity;
- acceleration-magnitude gating and invalid-sample containment;
- jitter, interval clamping, skipped gaps, long-gap reacquisition, and clock wrap;
- settled large-posture reacquisition without re-taring brief excursions;
- stationary auto-sleep behavior and one-shot HAL pointer events;
- clearing unconsumed page-turn events when the visible activity changes;
- state, timing, blocker, fault, and pointer diagnostic messages.

The test clock advances synthetically rather than sleeping. The general
`AttitudeTrace` propagates an independent truth quaternion, rotates its reference
gravity into the current body frame, and adds gyro bias only after calculating
physical motion. Tests compare the estimator's rotation vector to this truth,
not to copied implementation coefficients.

Host tests do not model IMU data-ready status, I2C transaction skew, real sensor
noise, physical board mounting, e-ink refresh latency, or user perception. The
SDK currently does not expose a sample timestamp or freshness flag, so repeated
register values cannot be distinguished from a stationary device at this layer.

The HAL generates and caches pointer events during its update, before the global
auto-sleep check. Only a generated logical move reports user activity; stationary
sampling does not keep the device awake. Reading the event consumes it once.
Changing interaction mode or screen orientation discards a pending pointer event,
and leaving the visible reader surface discards unconsumed page-turn events.

## Posture reacquisition

Relative rotation remains tied to the original neutral throughout ordinary
pointer motion. Once the shortest rotation vector reaches 32 degrees
(`REPOSITION_MIN_ANGLE_DEG`), pointer events are suppressed and the estimator
looks for a deliberately settled new posture. Window-average corrected rates
must remain below 1.75 degrees/second (`REPOSITION_MAX_RATE_DPS`), and acceleration
direction must remain stable for at least 250 ms (`REPOSITION_HOLD_MS`). A
successful window updates gravity and all three zero-rate offsets, then resets
relative rotation to zero.

The stricter rate and hold limits prevent the tested constant above-threshold
posture change and a brief large excursion from re-centering in motion. As with
startup calibration, no IMU-only rule can distinguish sufficiently slow constant
physical rotation from rest. The threshold is therefore outside the normal
pointer range and requires both gravity-direction and gyro stability.

## Estimator model

The scalar-first unit quaternion `q` maps the current IMU body frame into the
frame captured at tare:

```text
v_reference = q * v_body * conjugate(q)
```

Body-frame gyro rates right-multiply `q`. Each update uses an exponential-map
delta and normalizes the result. Pointer angles are the X/Y components of the
shortest quaternion rotation vector, so there is no global pitch/roll folding,
gravity-alignment matrix, or sign switch based on `az`. The representation is
finite through vertical and face-down postures. Large settled rotations are
re-centered well before the shortest rotation vector reaches its 180-degree
boundary.

During a stationary window, acceleration is averaged to define gravity in the
reference frame and all three raw gyro rates are averaged as zero-rate offsets.
Initial calibration must contain at least 10 samples (`CALIBRATION_MIN_SAMPLES`)
and span at least 250 ms (`CALIBRATION_HOLD_MS`). Reference reacquisition must
contain at least 12 samples (`STATIONARY_MIN_SAMPLES`) and span at least 500 ms
(`STATIONARY_HOLD_MS`). Both remain within 0.8-1.2 g (`ACCEL_MIN_G` and
`ACCEL_MAX_G`), stay within approximately two degrees of their initial
acceleration direction (`STATIONARY_ACCEL_DOT_MIN`), and keep each gyro sample
within 6 degrees/second of the running mean (`STATIONARY_GYRO_VARIATION_DPS`).
Calibration permits a constant offset up to 20 degrees/second on each axis
(`CALIBRATION_MAX_BIAS_DPS`). Once bias is known, reacquisition requires the
window-average corrected rates below 2.5 degrees/second
(`STATIONARY_CORRECTED_RATE_DPS`).

`HalTiltSensor` retains the three learned offsets across pointer activities for
the current boot. A later pointer session therefore captures its neutral gravity
direction from the first valid accelerometer sample and can track immediately,
without treating physical motion as a new zero-rate calibration window.

During ordinary tracking, at least 24 stable samples (`BIAS_ADAPT_MIN_SAMPLES`)
over 1000 ms (`BIAS_ADAPT_HOLD_MS`) can update gyro bias without changing the
neutral reference. The corrected window mean must remain within 1.75 degrees/second
(`BIAS_ADAPT_MAX_RATE_DPS`). An accepted window learns 15 percent of its residual
(`BIAS_ADAPT_ALPHA`), capped at 0.15 degrees/second per axis
(`BIAS_ADAPT_MAX_STEP_DPS`). It also removes accumulated rotation around gravity
by rebuilding the attitude from the stationary mean acceleration and the original
reference gravity. This preserves the observable tilt and neutral reference while
preventing an unobservable quaternion component from coupling into pointer X/Y.
Movement resets the candidate instead of being learned as bias. Applying the rate
limit to the average allows adaptation through the QMI8658's observed per-sample
gyro noise. Acceleration must remain within approximately one degree of the window
anchor (`BIAS_ADAPT_ACCEL_DOT_MIN`), and the gyro-variation check still rejects
unstable windows.

A candidate accumulated mostly at rest can finish immediately after motion
begins, before the IMU has observed enough gravity change to classify it. That
boundary case permits at most one capped update; continued motion invalidates
the next window rather than being repeatedly learned as bias.

Large-posture reacquisition uses the same fixed accumulators but requires at
least 8 samples (`REPOSITION_MIN_SAMPLES`) over 250 ms (`REPOSITION_HOLD_MS`)
with window-average corrected rates below 1.75 degrees/second
(`REPOSITION_MAX_RATE_DPS`). This keeps brief crossings from changing neutral
while allowing a completed grip reposition to resume promptly.

This rests on the unavoidable assumption that the device is not undergoing a
constant physical rotation during calibration. In particular, a constant slow
rotation about gravity cannot be distinguished from gyro bias without a second
world-frame reference such as a magnetometer.

While tracking, the accelerometer supplies proportional gravity correction only
when its magnitude is within 0.8-1.2 g (`ACCEL_MIN_G` and `ACCEL_MAX_G`). Gyro
integration continues outside that range. A lateral acceleration whose total
magnitude remains near 1 g can still mislead any gravity-only correction;
conservative correction (`ATTITUDE_KP`) and short sessions limit but cannot
eliminate that ambiguity.

## Timing policy

The estimator records the interval between delivered samples using unsigned
32-bit subtraction, including across `millis()` wrap:

| Delivered interval | Action |
| --- | --- |
| `0 ms` | Do not integrate or emit pointer movement |
| `1-50 ms` (`MAX_INTEGRATION_INTERVAL_MS`) | Integrate the full interval |
| `51-100 ms` (`MAX_INTEGRATION_INTERVAL_MS` to `SAMPLE_GAP_LOG_THRESHOLD_MS`) | Clamp integration to 50 ms (`MAX_INTEGRATION_INTERVAL_MS`) |
| `101-250 ms` (`SAMPLE_GAP_LOG_THRESHOLD_MS` to `REFERENCE_LOST_INTERVAL_MS`) | Skip the unknown interval and suppress output for that sample |
| `>250 ms` (`REFERENCE_LOST_INTERVAL_MS`) | Enter `NEEDS_TARE` and require a new stable reference |

Clamping prevents one endpoint gyro sample from being treated as representative
of a longer interval. Skipping and reacquiring acknowledge that motion during a
missing interval cannot be reconstructed.

## Read device logs

Build the `default` environment, install it, and capture serial output:

```sh
pio run -e default
python3 scripts/debugging_monitor.py
```

Enable Tilt To Select and open word selection or the keyboard. Relevant messages
use the `GYR-IMU` origin.

### State transitions

Normal startup reports:

```text
state=CALIBRATING reason=begin
state=CALIBRATING->NEEDS_TARE reason=stationary_bias_ready
state=NEEDS_TARE->TRACKING reason=tare_accepted
```

Both transitions can share a timestamp: once the stationary calibration window
provides bias and gravity, the same averaged window is safe to capture as the
initial neutral reference. The `tare` message includes averaged gravity, all
three biases, and sample count.

A later pointer session with a cached bias reports:

```text
state=NEEDS_TARE reason=begin_cached_bias
state=NEEDS_TARE->TRACKING reason=cached_bias_tare
```

After a gap longer than 250 ms (`REFERENCE_LOST_INTERVAL_MS`):

```text
state=TRACKING->NEEDS_TARE reason=sample_gap
```

Pointer output remains suppressed until another stable window is accepted.

A settled posture change reports:

```text
state=TRACKING->NEEDS_TARE reason=posture_change
state=NEEDS_TARE->TRACKING reason=tare_accepted
```

These transitions are immediate once the 250 ms reposition window
(`REPOSITION_HOLD_MS`) has completed; pointer output is already suppressed while
that window forms.

### Stationary blockers

`stationary_wait` is emitted when a blocker changes and then at most once per
second (`TARE_WAIT_LOG_INTERVAL_MS`) during continuous rejection. `blocked` is a
bit mask:

| Bit | Meaning |
| --- | --- |
| `0x01` | X gyro exceeds the applicable rate limit |
| `0x02` | Y gyro exceeds the applicable rate limit |
| `0x04` | Z gyro exceeds the applicable rate limit |
| `0x08` | Acceleration magnitude is outside 0.8-1.2 g (`ACCEL_MIN_G` and `ACCEL_MAX_G`) |
| `0x10` | A sample contains NaN or infinity |
| `0x20` | Acceleration direction moved too far during the candidate window |
| `0x40` | Gyro readings changed too far during the candidate window |
| `0x80` | A sample gap invalidated the reference |

Bits combine. The message includes acceleration magnitude and corrected rates.

### Periodic trace

At DEBUG level, three lines share a source-sample timestamp:

- `sample`: delivered interval (`dt_ms`), integrated interval (`used_ms`), raw
  acceleration in g, and raw gyro in degrees/second;
- `estimate`: state after the sample, blocker mask, finite-state result, and all
  three bias estimates;
- `attitude`: normalized quaternion and X/Y/Z relative rotation vector in degrees.

An accepted ordinary stationary window emits a `bias_adapt` DEBUG message with
the raw window mean, updated three-axis bias, and sample count. It does not report
a state transition because tracking and the current neutral remain active.

Every delivered interval over 100 ms (`SAMPLE_GAP_LOG_THRESHOLD_MS`) emits an
immediate `sample_gap` INFO line with `action=skip` or `action=reacquire`. Invalid
input emits `invalid_sample` once per episode and is discarded without
contaminating attitude. Internal nonfinite state emits `nonfinite_state` and
triggers reference reacquisition.

`pointer` INFO messages include orientation, sensitivity, both generated
directions, and the three rotation-vector components. They report generated
events; an activity can still discard an event or be at a selection boundary.

Periodic traces default to one set every 250 ms
(`TILT_POINTER_TRACE_INTERVAL_MS`, a build-time override). For short every-sample
captures, add a local environment to `platformio.local.ini`:

```ini
[env:tilt-diagnostics]
extends = env:default
build_flags =
  ${env:default.build_flags}
  -DTILT_POINTER_TRACE_INTERVAL_MS=0
```

Then build `pio run -e tilt-diagnostics`. Full-rate serial output can itself
affect delivery timing, so compare it with the normal rate-limited build.

## Device walkthrough

1. Hold the reader still while entering selection. Confirm the startup sequence
   and inspect all three bias values.
2. Test small X and Y movements from face-up, upright, and face-down starts.
3. Cross vertical slowly in both directions; verify direction remains continuous.
4. Apply a diagonal or twisting movement and return to neutral; the rotation
   vector should return near zero without entering `NEEDS_TARE`.
5. Slowly change holding posture by more than 32 degrees
   (`REPOSITION_MIN_ANGLE_DEG`), then stop. Confirm the `posture_change`
   transition after approximately 250 ms (`REPOSITION_HOLD_MS`) and no movement
   at the new neutral.
6. Make a brief excursion past 32 degrees (`REPOSITION_MIN_ANGLE_DEG`) and return
   within 250 ms (`REPOSITION_HOLD_MS`). Confirm the original neutral remains in
   effect and no `posture_change` is logged.
7. Pause the main loop or otherwise induce a sample gap. Check `skip` versus
   `reacquire` behavior and confirm no movement is emitted for the gap sample.
8. Save captures with model, screen orientation, sensitivity, inversion, posture,
   and the timestamp where visible behavior diverged from the logs.

The estimator and diagnostics use only fixed-size scalar and array fields. No
history buffers, new heap allocations, tasks, or SD writes are introduced. The
large log capture buffer exists only in the host test executable.
