The reverb engines behind the Reverb effect:

  reverb.h / reverb.cpp            "Simple" reverb: a 16 line feedback delay network
  efx_reverb.h / efx_reverb.cpp    "Advanced" reverb: the OpenAL EFX / EAX reverb model
  dsp.h                            small DSP building blocks both use

FastPlay's glue (parameters, presets, the effect in the audio engine's chain) is in src/effects.cpp.
