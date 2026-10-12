# Flame_LTXAlphaGen_OFX
OFX plugin for Autodesk Flame to run LTX 2.5 Alpha Gen models

## Roadmap and ToDos

### MacOS Version

Based on some research is seems feasible to create a MLX port of this plugin with the same models that
should run natively on MacOS and Mac Studio hardware.

### Feature Improvements

1. ~~Support a half-chunk process option for low VRAM cards.~~ Done: Chunk Size (decision 61).
2. ~~Configure the chunk overlap.~~ Done: tied to the chunk size (decision 61).
3. Cache conscious rendering
4. ~~Changing the overlap blend to fluid-morph~~ Done: Merge = Flow Warp (decision 62). An ML morph or
   carrying the previous chunk's matte as conditioning is still open.
5. ~~Add a matte input (or a built-in ROI feature)~~ Done: the source alpha is the ROI (decision 63).
