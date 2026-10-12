# Flame_LTXAlphaGen_OFX
OFX plugin for Autodesk Flame to run LTX 2.5 Alpha Gen models

## Roadmap and ToDos

### MacOS Version

Based on some research is seems feasible to create a MLX port of this plugin with the same models that
should run natively on MacOS and Mac Studio hardware.

### Feature Improvements

1. Support a half-chunk process option for low VRAM cards.
2. Configure the chunk overlap from the default 24 frames. Need some testing to understand quality of the cross fade.
3. Cache conscious rendering
