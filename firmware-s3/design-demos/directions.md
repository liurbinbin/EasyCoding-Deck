# Rounded edge light directions

## Shared specification

- Target: five 128 x 128 GC9107 displays with physically rounded corners.
- Motion: begins at the top center, moves down both rounded sides, and resolves at the bottom center.
- State colors: working/speaking cyan, waiting/approval amber, done green, error red.
- Rendering constraint: preserve content-aware refresh on screens 2-5; composite after avatar frames on screen 1.
- Safe geometry: a 3 px inset rounded path with an 18 px radius.

## Six candidates

1. Dual Comet: two compact heads with short tails. Selected for full direction A.
2. Split Spark: single-pixel heads without tails. Backup; efficient but too faint at 128 x 128.
3. Energy Rail: dim persistent perimeter with two traveling packets. Selected for full direction B.
4. Scanner Gate: two wide bands sweep down the sides. Backup; obscures content near the edges.
5. Breathing Halo: directional segmented luminance wave. Selected for full direction C.
6. Corner Relay: four corner pulses hand motion off sequentially. Backup; does not express top-to-bottom flow clearly.

## Direction A: Dual Comet

- Source logic: compact embedded-device status animation; motion itself communicates progress.
- Hero material: two bright heads and short soft tails following a rounded path.
- Form: the requested top split and bottom convergence define the geometry.
- Firmware cost: lowest of the three; only narrow border segments change per frame.

## Direction B: Energy Rail

- Source logic: industrial instrument energy indicators with a persistent status rail.
- Hero material: dim full perimeter plus two bright moving packets.
- Form: persistent state visibility is prioritized over a quiet content area.
- Firmware cost: highest; the base rail and packets occupy more edge pixels.

## Direction C: Breathing Halo

- Source logic: restrained ambient device feedback.
- Hero material: segmented perimeter whose luminance travels in phases.
- Form: long-running tasks should feel calm rather than urgent.
- Firmware cost: medium; multiple edge segments change brightness.
