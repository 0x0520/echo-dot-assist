# Speaker identification (voice profiles): what the Echo has, and the ways to it

Goal: tell Home Assistant *who* spoke (a sensor like "Last request whispered", for the conversation agent's prompt or
for automations), with profiles of the household's voices, nothing leaving the house. Status 2026-10-06: research.
Static findings below; the API details as they are worked out go into [re-speakerid-api.md](re-speakerid-api.md).

## Room on the Echo Dot 3

Measured with `scripts/top.sh` over Wi-Fi, 2026-10-06, idle, no Home Assistant connected yet:

| | |
|---|---|
| CPU | 4 × Cortex-A35 (ARMv8, run as armv7), max 1.6 GHz |
| busy in all | 43–56 % of all cores; `mixer` 18–23 %, hassmic 17–21 % (of which "pryon wake" 17 %), the rest under 4 % each |
| memory | 983 MB, 860 MB available, swap 490 MB; hassmic 26 MB, mixer 23 MB |
| storage | `/data` 4.8 GB free |

So about half the CPU and nearly all memory are free. A speaker check runs once per request (a few seconds of
audio), not continuously, which a model of a few million parameters fits easily. No database is needed: a profile is
one vector of a few hundred numbers; with a handful of voices, a file and a cosine or PLDA score is all there is.

## What libpryon.so has (firmware NS65741, static)

`rootfs/system/lib/libpryon.so` carries Amazon's whole on-device speaker ID stack, compiled in:

- **Enrollment** (building a voice profile): exported `PryonEnrollmentApi_AllocateResources` (`0x600ab9`),
  `_DeallocateResources`, `_BuildEnrollmentProfile`, `_SetGatherEnrollmentAudioCallback`,
  `_SetEnrollmentProfileCompletedCallback`, `_SetLoggingCallback`, `_SetMetricsCallback`. Sources named in the
  binary: `adaptation/enrollment_ivector_builder.cpp`, `enrollment_buildtime_processor.cpp`, `enrollment_profile.h`,
  `core/enrollment_profile_config_util.cpp`, `decoder/enrollment_decode_fst_construct.cpp` (an "enrollment transcript
  FST": enrollment by saying given phrases).
- **Scoring** during decoding: `PryonDecoder_NewAudioDecoder(…, PryonDataBlob, PryonDataBlob, PryonSpeakerIdInfoList,
  PryonMultichannelAudioFormat, …)` (C `0x6028f5`, C++ overload `0x602a9d`) takes a list of speaker profiles; hassmic
  today uses `PryonDecoder_NewSpotterAudioDecoder`, which has none.
- **Kinds**: i-vectors (GMM statistics, `ivector_stats_accumulator.cpp`, online stats) scored with PLDA ("Loaded
  SpeakerIdPlda file"), and "E2E speaker ID" in three flavours: on the wake word (`e2e_speakerid_wakeword_config`, also a
  "split model"), text-independent (on the whole request) and a fusion of both, each with confidence bins. A separate
  "UOM" speaker ID with speaker change detection (`pryon_uom_speakerid/scd_processor.cpp`, `SpeakerIdProfileScore`).
- **Versioning**: "Model Compatibility ID for speaker id models" / "for enrollment profiles", "Supported enrollment
  profile ECIDs": a profile only works with the model it was built for.
- **Configuration** it asks for: "Path to the E2E Speaker ID top level configuration", "Path to the Speaker ID
  configuration", "Enrollment build time / run time / speaker id configuration file", "Enrollment strategy to use".

## What is missing

- **No caller on this firmware.** No program or library of the Dot 3's image imports `PryonEnrollmentApi_*` or
  `PryonDecoder_NewAudioDecoder`; stock's voice profiles were Amazon's cloud. Whisper detection had AHE as a model to
  copy ([re-whisper.md](re-whisper.md)); here the call sequence has to be read out of the library itself.
  (`libReggaeDevice.so` names a `ReggaeSpeakerIdentifier` LIPC interface, not the Pryon API.)
- **No model and no configuration.** The image has model sets for the wake word (`keyword/en-US/ALEXA`), PMA and AED
  only. The speaker ID pipeline needs at least its configuration, the i-vector extractor / embedding network, the PLDA
  file and a confidence model, matching one compatibility ID. The only trace of such files so far: the DAVS whisper
  artifact carried a stray `pryon.manifest` for speaker-ID enrollment without its configuration
  ([re-whisper.md](re-whisper.md), artifacts).
- Whether DAVS hands this device type a speaker ID model at all is unknown, and finding out needs the Echo registered
  with Amazon for a while (the guarded `scripts/artifacts.sh` route). Not done.

## Two ways

1. **Amazon's speaker ID on the Echo** (this research). Cheap at run time (the wake word variant works on audio the
   engine processes anyway), entirely local, result as an ESPHome sensor next to "Last request whispered". Needs: the
   API reversed (milestone 1, offline, under qemu: [re-speakerid-api.md](re-speakerid-api.md)), then a model (DAVS,
   uncertain), then enrollment in hassmic (Home Assistant action "enroll speaker", a few phrases) and the profile list
   in the decoder. Effort: weeks; may end at "no model obtainable".
2. **An open model on the Home Assistant host.** The request's audio goes to Home Assistant anyway; a speaker
   embedding model (ECAPA-TDNN class) in front of speech-to-text, profiles in a file. Nothing new on the Echo.
   Effort: days, with known tools. Also possible on the Echo itself (memory is there, CPU per request should be), but
   more work than on the host for the same result.

## Next steps

- [x] Milestone 1 (offline, 2026-10-06): signatures, blobs, callbacks and config schemas, a probe tool under qemu →
      [re-speakerid-api.md](re-speakerid-api.md). In short: the API is callable from C; `PryonSpeakerIdInfoList` is
      ignored by this build, a profile comes in as one archive blob; `pryon.config` is not covered by the model set's
      checksum, so the library's own validation errors gave the JSON schemas: a "UOM" cosine scorer over a named
      network output with profiles as plain JSON embeddings (enrollment = writing a vector), and Amazon's enrollment
      with the i-vector strategy (Kaldi style: UBM, T matrix, PLDA). Dead ends: the ALEXA network has no embedding
      output (only `softmax_output_ww`, `fc_output_se`); a UOM run with real "Alexa" audio produced no speaker output
      (probably tied to the turn-taking pipeline, not confirmed). ECIDs: speaker id model 3, enrollment profile 2.
- [ ] Decide on the DAVS question with the owner (Echo online with Amazon for a few minutes, update guard on).
- [ ] If no model: way 2.
- [ ] Untested middle way: an open embedding network of our own as ONNX, plugged into the UOM scorer; needs the
      turn-taking configuration worked out first (estimated 2–4 days, outcome open).
