# libpryon.so — speaker ID / enrollment API notes (milestone 1, offline)

Firmware NS65741, `firmware/donut/rootfs/system/lib/libpryon.so` (Thumb-2), the same library as in
[re-pryon.md](re-pryon.md). Background and the decision this feeds: [re-speakerid.md](re-speakerid.md).
Everything here comes from the PC: static analysis, then the library run under `qemu-arm` with a small probe tool.
The real Echo was not touched, nothing went to Amazon.

Markers: **CONFIRMED (qemu)** = seen in a run; **CONFIRMED (static)** = read unambiguously from the code;
**INFERRED** = plausible reading, not exercised.

Work files (git-ignored) in `firmware/donut/re/spk/`: `tdis.py` (capstone disassembler with absolute branch targets,
symbol and PLT names and pc-relative strings), `xref.py` (finds `ldr`+`add rX,pc` string references and `bl`/`blx`/`b.w`
callers of an address across `.text` with numpy, ~3 s), `discover.py` (grows a JSON config from the library's own
vetting errors, see section 5), `spk_test.c` / `spk_test` (probe tool), `models/` (copies of the ALEXA model set with
extra `pryon.config` lines), `logs/`, dumps `inner_*.asm`, `builder.asm`, `core_newdecoder.asm`.
Build: NDK r21e `armv7a-linux-androideabi24-clang -O2 -fPIE -pie -fuse-ld=lld` against the rootfs `libpryon.so`, as the
Makefile does for `pryon_test`. Run: like `tools/qrun.sh` (new PID namespace, `qemu-arm -L rootfs`).

## 1. Summary

- The enrollment API and `PryonDecoder_NewAudioDecoder` are callable from plain C; signatures below. Both run under
  qemu with the stock ALEXA model set: `AllocateResources` returns 0, `BuildEnrollmentProfile` stops at the missing
  enrollment build-time configuration, `NewAudioDecoder` with empty blobs behaves like `NewSpotterAudioDecoder`.
- **`PryonSpeakerIdInfoList` is ignored by this build.** The C++ overload hands it to `0x6b593c`, which does not pass
  it on. A garbage list (pointer `0xdead0000`) runs without a crash. Profiles come in as one
  `PryonDataBlob enrollmentProfile` (a libarchive archive), or, for the "UOM" speaker ID, as JSON files named in the
  model's configuration.
- No model on the image has a speaker-ID part. The ALEXA ONNX has only the wake word (`softmax_output_ww`) and
  start/end (`fc_output_se`) outputs, no embedding. Every speaker-ID pipeline needs files that are not on the device.
- The model configuration is **not checksum-protected**: a copy of the ALEXA model set with extra lines in
  `pryon.config` loads (same model checksum `74a26372…`, which is the ONNX's). That made it possible to read the JSON
  schemas out of the library's validator (section 5).
- The "UOM" speaker ID is the simplest of the four pipelines: cosine similarity between a named DNN output of the
  model and profile embeddings stored as JSON (`{"type":"enrolled","speaker_id":…,"embedding":[…]}`). Its config
  schema is fully recovered. It still needs a model whose DNN has an embedding output, and apparently the NTT
  pipeline around it (a run with a stand-in output produced nothing).
- Amazon's enrollment (`PryonEnrollmentApi_*`, `enrollment.strategy = "IVECTOR"`) is Kaldi-style: GMM/UBM +
  transition model + T-matrix + LFBE→MFCC transform at build time, PLDA at run time (section 5b). The library tells
  which keys it wants; the trained files are what is missing.

## 2. Exported functions

All wrappers: `this` = the PryonApi singleton (`0x12e8560`), catch everything, return 1 on exception, else the inner
status (0 = ok). Inner methods throw `std::runtime_error` whose text goes to the logging callback (level 2) together
with the API name.

| Export | Inner | C signature (CONFIRMED static + qemu unless noted) |
|---|---|---|
| `PryonEnrollmentApi_AllocateResources` `0x600ab9` | `0x6bf350` | `int (const char *modelSetId, const char *modelId, const char *manifestPath)` |
| `PryonEnrollmentApi_DeallocateResources` `0x600b91` | `0x6bf85c` | `int (const char *modelSetId, const char *modelId)` |
| `PryonEnrollmentApi_BuildEnrollmentProfile` `0x600bf9` | `0x6bfa08` | `int (const char *builderId, const char *modelSetId, const char *modelId, int32_t type, const char *s4, const char *s5, uint32_t a6, uint32_t a7, uint32_t numStreams, const PryonEnrollmentStreamConfig *streams)` — 10 words, meaning of `s4`..`a7` see below |
| `PryonEnrollmentApi_SetGatherEnrollmentAudioCallback` `0x600c85` | `0x69e1d4` | `int (cb)`; stored at `api+0xac` |
| `PryonEnrollmentApi_SetEnrollmentProfileCompletedCallback` `0x600ced` | `0x69e21c` | `int (cb)`; stored at `api+0xb0` |
| `PryonEnrollmentApi_SetLoggingCallback` `0x600d55` | `0x69d578` | **same inner as `PryonApi_SetLoggingCallback`** (`0x601211` calls it too): one logging callback for everything |
| `PryonEnrollmentApi_SetMetricsCallback` `0x600dbd` | `0x69d708` | **same inner as `PryonApi_SetMetricsCallback`** (`0x601279`) |
| `PryonDecoder_NewAudioDecoder` (C) `0x6028f5` | via C++ `0x602a9d` | see 2.2 |
| `PryonApi_SetStructuredResultCallback` `0x601929` | `0x69deec` | `int (cb)`, raw pointer at `api+0x54`; cb see section 3 |

### 2.1 Enrollment

- **AllocateResources** builds its own model from `manifestPath` (`0x651ccc` is the manifest parser: "The file path
  handed into the manifest parser … does not end with .manifest"), keyed by `modelSetId + "-" + modelId`
  (`0x6bf700`, separator at `0x32fc9b`) in a map at `api+0x178`. Error: `Enrollment ModelId already in use: <modelId>`.
  No null checks (a NULL would crash in the `std::string` constructor). It does **not** use a model set made with
  `PryonModelSet_New`; the ids only name the allocation. qemu: returns 0 with the ALEXA manifest (loads the model again,
  ~2 s under qemu).
- **DeallocateResources**: `null or empty PryonModelSetId` / `PryonModelId`; erases the map entry. qemu: 0.
- **BuildEnrollmentProfile** (`0x6bfa08`), checks in this order: `null or empty PryonModelSetId` (arg 2),
  `null or empty PryonModelId` (3), `null or empty PryonEnrollmentProfileBuilderId` (1), `null or empty PryonString`
  for `s4` and for `s5`; `streams == NULL` only logs `Invalid stream configuration!`. Then
  `Enrollment ModelId not found <modelId>` unless allocated, and a check against builder ids already running
  (map at `api+0x1c0`).
  It constructs a `ProfileBuilder` (`0x6e350c`) and runs it **synchronously** in the caller's thread (`0x6e3790`).
  - `type` (builder `+0x1c`): compared with 1; anything else ends in `Unknown enrollment type: ` (`0x6e4afa`).
    INFERRED: 1 = the only supported kind (i-vector speaker profile).
  - `numStreams` (`+0x14`) and `streams` (`+0x18`): array of 8-byte entries, `streams[i].streamId` (a `const char *`)
    at +0, required non-null (`assertion "streamCfg[i].streamId"`); the second word is not read in the paths looked at.
    For each stream the library calls the gather callback with that stream id.
  - `s4`, `s5`, `a6`, `a7`: validated (`s4`, `s5` must be non-empty) but **not stored** by the `ProfileBuilder`
    constructor in this build. INFERRED: legacy parameters.
  - qemu, ALEXA model: returns 1 with `assertion "buildtimeConfig" failed at core/enrollment_profile_config_util.cpp:27`,
    for type 1 and 2 alike, before the type check: the model has no enrollment build-time configuration (section 4).
- Limits seen in strings: `profile.minimum_audio_msec`, `profile.maximum_audio_msec`, "enrollment building failed
  because audio threshold not reached", "Stopping enrollment build because next audio stream will exceed maximum audio
  threshold", metrics `total_audio_transcript_pairs_requested` / `total_valid_audio_transcript_pairs_received`: the
  builder asks for audio+transcript pairs, one per stream (text-dependent enrollment, "<enrollment transcript FST>").

```c
typedef struct { const char *streamId; uint32_t unknown; } PryonEnrollmentStreamConfig;   /* 8 bytes */
```

### 2.2 `PryonDecoder_NewAudioDecoder`

C export (`0x6028f5`), 13 words, **CONFIRMED (static, qemu)**:

```c
typedef struct { int32_t size; const void *data; } PryonDataBlob;       /* size FIRST */
typedef struct { uint32_t w0; const void *w1; } PryonSpeakerIdInfoList;  /* 8 bytes, layout unknown, ignored */
int PryonDecoder_NewAudioDecoder(const char *decoderId, const char *modelSetId, const char *modelId,
                                 PryonDataBlob mutableModel, PryonDataBlob enrollmentProfile,
                                 PryonSpeakerIdInfoList speakers,
                                 int32_t encoding, int32_t sampleRate, int32_t bitsPerSample, int32_t numChannels);
```

- The C version takes the four format header words as plain ints (`[r7+0x1c..0x28]`) and fills the 32 channel types
  from the same default table as `PryonDecoder_NewPryonMultichannelAudioFormat_Default` (`0x4008a0`). It then calls the
  C++ overload with empty vectors, default `RecognizerConfigurationOverrides` (`0x602a64`), u64 0 and `false`.
- C++ overload `0x602a9d` → `0x6b593c` → common decoder factory `0x6a9c50(api, kind, decoderId, modelSetId, modelId,
  mutableModel, enrollmentProfile, format, …)`. `kind` 0 here; `0x6b59c0` passes 1, `0x6b5ac0` 2. The C++ overload
  passes the config JSON `"{}"` (`0x310a1e`).
- **Blob layout**: `0x6a90d0(data, size)` is a CRC-32 over `mutableModel` (returns 0 for NULL or size < 0), and the
  enrollment profile is read as size `[r7+0x14]`, pointer `[r7+0x18]`. Errors: `null PryonMutableModel`,
  `null PryonEnrollmentProfile` (size ≠ 0 with NULL data).
- **Speaker list dropped**: `0x6b593c` copies blob words and the format to the factory's stack but not the two
  `PryonSpeakerIdInfoList` words (`[r7+0x18]`, `[r7+0x1c]` of its frame). qemu `spk_test spk` with `{3, 0xdead0000}`:
  returns 0, decodes, deletes, no fault.
- **Profile format** (qemu, 64 bytes of garbage as `enrollmentProfile`): `An error occurred while loading the enrollment
  profile: Unable to open archive for reading : archive_read_open_memory returned -30 : Unrecognized archive format :
  pointer … size 64 bytes CRC 0xa7376094`. Not fatal: the decoder is still created (returns 0) and runs without
  profile. The mutable model blob gives the same libarchive error ("using the baseline mutable model instead").
  So both blobs are archives (tar/zip-like, whatever libarchive detects); the profile's contents (boost-serialized
  `EnrollmentProfile` / `IVectorStatsTmpl` / `IVectorMatrixContainer`, per type names) are not known.
- **`NewSpotterAudioDecoder` in terms of NewAudioDecoder** (`0x6033d4`): same factory, blobs `{0, NULL}` ×2, speaker
  list `{0, NULL}`, the caller's format by value and its JSON string. So "empty" is `{0, NULL}` everywhere.
  qemu: `NewAudioDecoder` with all-empty arguments on the ALEXA model returns 0 and logs the same as the spotter.

## 3. Callbacks

- **Gather enrollment audio** (`api+0xac`, dispatcher vtable slot `0x5c` of `ApiRecognizerCallback`, `0x6e6b48`; the
  vtable is at `0x128eaf4`, type name at `0x406861`): called once per stream from `0x6e3c7a` as
  `R cb(const char *builderId, const char *streamId)` where `R` is a struct **returned by value through a hidden
  pointer** (r0). `R` word 0 is a pointer to a typed audio event that must be of type `"samples"`
  (`assertion "event->type == std::string(AudioPayloadEvent::SAMPLES)"`, payload field `"sample_count"`), the same
  kind of object as `PryonDecoder_PushAudioEvent` takes; at +0x10 sits a `std::shared_ptr` (libc++ refcount at +4)
  whose target has a sample rate or count at +0x40. **Layout not fully decoded**; it is a C++ object, so a C caller
  would have to imitate libc++ `shared_ptr`. CONFIRMED (static); not exercised (no build-time config, see 2.1).
  If the callback is unset the library asserts `PryonApi::ThePryonApi.mGatherEnrollmentAudioCallback`.
- **Enrollment profile completed** (`api+0xb0`, slot `0x60`, `0x6e6bdc`): `cb(const char *builderId, <r1 unused>,
  X)` where `X` starts in r2 (so it is 8-byte aligned: a struct with a 64-bit member) and continues over 10 stack
  words. Failure call (`0x6e46ec`, audio threshold not reached): r2 = 1, r3 = 0, eight zero words, then a `const char *`
  message, then 0. Success call (`0x6e494a`): r2 = 0, r3 = profile byte count, stack: profile data pointer, 0, an
  int64, three more words (one is a 64-bit pair), a `const char *` (JSON), 0. INFERRED struct:
  `{int32 status; PryonDataBlob profile (size, data); u32 pad; int64; …; const char *json; u32}` — 0x30 bytes. The
  `profile` bytes are what goes back into `NewAudioDecoder` as `enrollmentProfile`.
- **Metrics** (`api+0xb4`, slot `0x64`): a `boost::function` (vtable + functor at `+0xb8`), called with the builder id
  and a JSON string (`enrollment_profile_building_metrics`: `total_build_time_msec`,
  `total_audio_used_during_build_msec`, …). Set through the general metrics setter.
- **Logging**: the general Pryon logging callback, `(int level, const char *tag, const char *msg)`.
- **Structured result** (`api+0x54`, slot `0x24`, `0x6e5890`): `void cb(const char *decoderId, const char *json,
  int64_t, uint32_t)`; json = `{"decoder_id", "recognition_id", "structured_result": {…}}`. The option text for
  `speaker_id.enabled` says speaker ID results come "via the PryonStructuredResult callback". No such callback fired in
  any run here (no speaker-ID model).
- **Where results show up otherwise** (static, INFERRED): the UOM scores (`sidProfiles`, `speakerId`, `score`,
  `threshold`, `signalType`, near `0x8fed50`) are part of the NTT ("natural turn taking") fusion metadata
  (`ntt_fusion_algo_simple_and` / `window_avg`), i.e. `PryonApi_SetNttResultCallback`. `activeSpeakerScore*`,
  `personUuid`, `speakerDirections` belong to the camera ("CV") NTT client events of Echo Show devices, not to voice
  profiles. E2E speaker ID reports `"e2e_speaker_id_metrics"` in metrics.

## 4. What the pipelines expect

Library attributes (qemu, `PryonApi_GetAttributes`): `enrollment_profile_current_ecid` 2 (earliest 1),
`speaker_id_model_current_ecid` 3 (earliest 3), `mutable_model_current_ecid` 5, `wakeword_ecids` 1–37 (no 3, 18).
A model declares its own compatibility in `pryon.config`: `versioning.ep-mcid` (enrollment profiles) and
`versioning.speaker-id-mcid` (speaker ID models), both default `"0.0"`; `versioning.speaker_id_mm_mcid`
("set versioning.speaker_id_mm_mcid to 0.0"). Mismatches end in "incompatible MCID values" / "Unknown enrollment profile
ECID" / `assertion "enrollmentProfileECID <= 2"`.

`pryon.config` keys (all printed with defaults by the library once a non-default value is set; qemu):

| key | default | role |
|---|---|---|
| `speaker_id.enabled` | 0 | "engine expects Speaker ID info input when creating the decoder … results via the PryonStructuredResult callback". Set to 1 on ALEXA: loads, decodes, nothing else happens |
| `speaker_id.runtime_config`, `speaker_id.common_config` | "" | i-vector/PLDA path. With `{}` files: `Speaker common config parsing error`, model set refused |
| `pryon_speakerid.e2e_config` | "" | E2E speaker ID top-level config. `{}` or a missing file are silently ignored with the ALEXA model (also with `speaker_id.enabled = 1`): parsed only when a model needs it (INFERRED) |
| `uom_speakerid.config_filepath` | "" | UOM speaker ID, schema in section 5 |
| `scd.config_filepath` | "" | speaker change detection; root type `scd_processor`, requires `config_version` (not pursued) |
| `enrollment.op_mode` | `"enrollment_off"` | `build_profile` / `use_profile` (the option text names `off`) |
| `enrollment.strategy` | `"NONE"` | "Enrollment strategy to use"; an unknown value: `not in the set of legal choices` |
| `enrollment.build_time_configuration_file`, `.runtime_configuration_file`, `.speaker_id_configuration_file` | "" | with strategy NONE: not read even when set (qemu) |
| `scorer.dnnrt_ivector.*` | see logs | online/enrollment i-vector statistics for the DNN-RT i-vector scorer |

Config object/key names in the binary (static; which file each belongs to is mostly unknown): `enrollment_model_builder_config`,
`enrollment_model_runtime_config`, `ivector_extractor_filepath`, `ivector_mvn_transform_filepath`,
`global_ivector_mean_filepath`, `plda_filepath`, `plda_scoring_config`, `lda_matrix_transform_config`,
`lda_transform_matrix`, `ubm_gmm_model_filepath`, `ubm_gmm_feature_type`, `i_vector_stats_from_mfcc_ubm_gmm_config`,
`model_ivector_fe_config_filepath`, `speaker_id_acoustic_scoring_config`, `speaker_id_confidence_config`,
`speaker_id_common_config(_filepath)`, `speakerid_all_config`, `speaker_id_model`, `use_speaker_id_models`,
`manifest_legacy_sid_schema`. Source files: `config/e2e_speakerid_{common_config_provider, config_provider,
confidence_config, fusion_config, fusion_confidence_config, text_independent_config, text_independent_scorer_config,
text_independent_confidence_config, wakeword_config, wakeword_confidence_config, wakeword_split_model_config}.cpp`,
`config/enrollment_ivector_config.cpp`, `adaptation/speaker_id_model.cpp`, `pryon_uom_speakerid/sid_model_config.cpp`.

**Does the wake word model help?** No. Its ONNX (`runtime_stateless_standard.streaming.fixed.int16.onnx`) outputs
`softmax_output_ww` (decoding scores) and `fc_output_se` (keyword start/end), and the model set has no speaker
files. The "wakeword split model" of E2E speaker ID is a separate network config (`WakewordSplitModelConfig`), not a
mode of the keyword model. PMA and AED are watermark and sound-event models.

## 5. UOM speaker ID: schema recovered from the validator (qemu)

`discover.py` writes the JSON, loads the model set under qemu, reads the library's vetting error ("Object type 'X'
is missing a required property 'Y'", "Property 'Y' … must be 'Z'", "must be in set (…)", "must be a Array") and
adds or fixes the property. Result, accepted by `PryonModelSet_New` (model set loads, decoder runs):

```json
{ "config_version": 1,
  "sid_compute_score": "cosine_similarity",
  "sid_scaling_factor": 1.0,
  "sid_model_config": { "sid_scorer_component": { "component_name": "<DNN output name>",
                                                   "posterior_type": "decoding-scores" } },
  "sid_profiles": [ { "file_path": "prof1.json" } ] }
```

Profile file (`speaker_id_profile`): `{"config_version":1, "type":"enrolled"|"tts", "speaker_id":"<name>",
"embedding":[<at least one number>]}`. Other optional keys seen in strings: `sid_scorer_names`, `frame_embedding_history_size`,
`profiles_config`/`profile_config`, `speaker_detection_type`, `history_max_count`, `max_age_msec`,
`profile_generation_sos_offset_msec`, `profile_generation_lower_bound_frame_offset`.

So UOM speaker ID = cosine similarity between the per-frame vector that a named output of the model's DNN produces
and stored embedding vectors; enrollment is just writing the embedding to a JSON file. It needs a scorer DNN with an
embedding output.

qemu run with this config, `component_name` = `fc_output_se` (the ALEXA model's start/end output, only as a stand-in),
a profile with `"embedding":[0.5,0.5]`, and `testdata/alexa_espeak.raw` pushed through `NewAudioDecoder`: the model
set loads, ALEXA is detected as usual (Accept + NearMiss), and nothing speaker-related appears: no error, no
structured result, no extra log line. INFERRED: the UOM processor is fed only inside the NTT pipeline
(`ntt.config_filepath`, its `speaker_id_config`), which this model does not have; and a 2-dim stand-in vector says
nothing about real embeddings. The component name is not validated at load time either.

## 5b. i-vector enrollment (`enrollment.strategy = "IVECTOR"`): build-time config, partly recovered (qemu)

Legal values of `enrollment.strategy` found so far: `NONE` (default), `IVECTOR`. Rejected as "not in the set of
legal choices": `dnnrt-ivector`, `pryon-enrollment`, `pryon-e2e-speakerid`, `ivector`. With `IVECTOR`,
`enrollment.op_mode = "build_profile"` and the three enrollment files set, `PryonEnrollmentApi_AllocateResources`
(not `PryonModelSet_New`) reads `enrollment.build_time_configuration_file` and reports one missing item per run
(older JSON reader: "JSON Object does not contain item named 'X'", "expected: object_type_name=… object_type_version={1,}").
State reached:

```json
{ "object_type_name": "enrollment_model_builder_config", "object_type_version": 1,
  "i_vector_from_mfcc_and_alignments_config": {
    "object_type_name": "i_vector_build_config", "object_type_version": 1,
    "reuse_cmvn_stats": 1,
    "utterance.loglik_per_frame_acceptance_threshold": 1,
    "utterance.consecutive_phones_threshold": 1,
    "profile.minimum_audio_msec": 1000, "profile.maximum_audio_msec": 60000,
    "gmm_model_filepath": "…", "gmm_model_type": "…", "transition_filepath": "…",
    "t_matrix_filepath": "…", "lfbe_to_mfcc_transform_filepath": "…",
    "stacker_left_context_length": 1 } }
```

The next run ends in `boost::bad_get` (a value of the wrong type somewhere; not pursued). The type names come from
`0x6543a0` (`builderIVectorConfigObjectTypeGmm()` = `"i_vector_build_config"`) and `0x655b90…`: the run-time file is an
`enrollment_model_runtime_config` with an `i_vector_runtime_configuration` object of type `i_vector_runtime_config`.
So Amazon's enrollment is classic Kaldi: a GMM (UBM) acoustic model with transition model (alignments against the
enrollment transcript), MFCC from LFBE, an i-vector T-matrix, then PLDA scoring (`plda_filepath`) at run time. All
those files are trained artifacts; none exist on the image.

## 6. Open questions

1. Layout of the gather callback's return struct and of the completed callback's struct (2.1, 3). Only matters if
   Amazon's enrollment builder is used; it needs a build-time config first.
2. The enrollment profile archive format and the i-vector model files (extractor, UBM, PLDA, LDA, MVN): unknown,
   no sample.
3. Which callback carries speaker ID results in a working setup (structured result vs. NTT result vs. enumerated
   result metadata). Needs a real speaker-ID model.
4. `enrollment.strategy`: `IVECTOR` works, other spellings untested (the list of legal choices is not printed).
5. Whether DAVS serves any speaker-ID model to an Echo Dot 3. Nothing in this firmware names a DAVS artifact for it:
   no stock binary uses the enrollment API, and the wake word / AED / whisper requests do not carry speaker models.
   Hints for a request: a model set like the wake word one (`pryon.manifest`, ECIDs `speaker_id_model_current_ecid` 3,
   `enrollment_profile_current_ecid` 2), probably under the wake word artifact family or the AHE (`alexa-hybrid`) one,
   which already shipped a stray speaker-ID enrollment `pryon.manifest` in `whisper-static` (re-whisper.md). Not fetched.
6. The bad_get in 5b, and whether the run-time / speaker ID config files follow the same reader.
7. NTT config needed to make the UOM path emit anything (`ntt.config_filepath`, `speaker_id_config`).

## 7. Probe tool usage

```
spk_test attr                         # library attributes (ECIDs)
spk_test dec|prof|spk|mm <manifest>   # NewAudioDecoder: empty / dummy profile / garbage speaker list / dummy mutable model
SPK_AUDIO=<raw s16le 16 kHz> spk_test dec <manifest>   # push a clip instead of noise
spk_test enr <manifest> [model] [type]                 # AllocateResources + BuildEnrollmentProfile + Deallocate
```
