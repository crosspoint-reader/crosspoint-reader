# W100 final-state pruning A/B

Date: 2026-08-16

## Candidate

```text
knietty-W100-DIFF-ab5d5784-20MHz-EXPERIMENTAL.bin
CrossPoint ab5d5784
FreeInk 2218b6c
Adaptive 100 ms / 20 MHz experimental
```

This candidate differs from the hardware-tested `e4238425` W100 image only in
ordinary terminal rendering: mutation-dirty edge cells and rows whose final
glyph, attributes, and cursor overlay match the last presented snapshot are
pruned. Named diagnostic activations bypass the pruning.

## Physical observation

The user flashed the candidate successfully on the available XTEINK X4. The
problem remained:

- sustained input progressively made the screen grainier and grayer;
- the degradation appeared across the screen, not only at the latest glyph;
- occasional maintenance refreshes cleared some, but not all, of the residue;
- btop still appeared to lag and also accumulated gray residue.

No new quantitative diagnostic capture or fixed-camera comparison accompanied
this report. The earlier `e4238425` smoke timing remains the electrical control.

## Conclusion

Final-state pruning did not resolve the observed optical degradation or btop
cadence. Redundant clear-and-identical-repaint traffic is therefore not the
primary explanation. The optimization is still logically correct and bounded,
but it must not be credited with a hardware quality improvement.

Two effects remain deliberately separate:

1. After 250 ms of quiet, the adaptive scheduler starts a stock-waveform
   forced-target settle over the accumulated changed region. This explains the
   occasional partial cleanup and may delay the next burst.
2. The W100 waveform is still a single directional phase: twenty 5 ms target
   pulses, with unchanged-black and unchanged-white LUT entries completely idle.
   A RAM window reduces transferred bytes but does not reduce the configured 480
   gate outputs; every `MASTER_ACTIVATION` remains panel-global. Repeated common-
   electrode/gate activity can therefore perturb nominally unchanged pixels,
   while the current LUT provides no charge-balanced sustain phase to restore
   their black/white endpoint. This matches the reported whole-screen drift more
   closely than a dirty-rectangle error.

Before changing either, reproduce the same btop view with an explicit one-second update rate
and capture the on-device update/window/fallback/settle/clean counters after
disconnect. Then change only one dimension per firmware image: scheduler settle
first for cadence attribution, waveform phase shape second for optical quality.
The quality candidate must add a short balanced sustain/restore pair for
unchanged black and white as well as a final target phase for changed pixels;
conditioning only changed transitions would not directly test the whole-screen
symptom.
