# m3-pad: controller support

Branch `m3-pad` (ps5recomp) with `nps5-pad` (AnyPS5, `docs/nps5-controllers.md`: SDL setup, mapping tables, output
paths, keyboard/mouse mapping, tests). With `-gpu anyps5` every `scePad*` import is rebound to AnyPS5's libScePad
(`emitter.EnableAnyPS5Shims`, thunks in `pkg/runtime/modules/libSceAnyPS5`), which lives in `libnps5_gpu.dylib` next to
VideoOut and UserService and reads SDL GameController input.

## The four former stubs

`scePadSetVibrationMode`, `scePadGetTriggerEffectState`, `scePadSetTriggerEffect` and `scePadIsRemoteController` were
answered by `libSceHlesys` (`nps5_hlesys.c`), which could only log. They stay there as the answer for a build without a
pad backend (`-gpu off`: there is no controller), and are now rebound to `shim_aps5_<name>` whenever `-gpu anyps5` is on
(test `TestEnableAnyPS5Shims` covers all four by name and NID, in both modes). Behaviour behind them:

| import | AnyPS5 implementation |
|---|---|
| `scePadSetVibrationMode(handle, mode)` | validates handle/mode (1..5), stores the mode per port |
| `scePadSetTriggerEffect(handle, param)` | validates the mask and the 7 modes, stores the 11-byte effect per trigger; `PadInput` sends it to a DualSense with `SDL_GameControllerSendEffect`, or as trigger rumble on an Xbox pad |
| `scePadGetTriggerEffectState(handle, info)` | the effect/status the DualSense reports in its input report (`int32_t state[2]`) |
| `scePadIsRemoteController(handle, bool*)` | false |

The fallback `scePadGetTriggerEffectState` used to clear 2 bytes of the 8-byte `int32_t state[2]`; it clears all 8 now.

## Calculator3D end to end

`NPS5_VPAD_SCRIPT` attaches a scripted SDL virtual pad inside the guest process; with
`tools/m2_anyps5_run.sh` (new `M2_EXTRA_ENV` and `DUMP_SCALE` variables) the title's selection moved as scripted:

```
NPS5_ANYPS5_BUILD=<wt/anyps5-pad>/build-arm64 DUMP_SCALE=4 \
M2_EXTRA_ENV="NPS5_PAD_LOG=1 NPS5_VPAD_SCRIPT=f90:tap=up;f150:tap=up;f210:tap=left;f270:tap=right" \
  tools/m2_anyps5_run.sh <extract>/eboot.elf 60 300
```

Frames 80 / 120 / 180 / 240 / 299: highlighted key `0` -> `2` -> `5` -> `4` -> `5` (Up, Up, Left, Right). The `[pad]` log lines in `run.err`
show the matching Up (0x10), Left (0x80) and Right (0x20) button states.
