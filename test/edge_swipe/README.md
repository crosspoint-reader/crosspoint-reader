Run the allocation-free recognizer checks from the repository root:

```sh
g++ -std=c++20 -Wall -Wextra -Werror -Isrc test/edge_swipe/EdgeSwipeTest.cpp -o /tmp/edge-swipe-test
/tmp/edge-swipe-test
```

Run the indicator and renderer refresh checks:

```sh
python3 test/edge_swipe/run_indicator.py
```

This compiles the production indicator and renderer display methods against a
recording display. It checks cleanup, repaint replacement, grayscale suppression,
async baseline ordering, refresh limits, and framebuffer loans. Hardware tests
remain required for waveform behavior and ghosting.
