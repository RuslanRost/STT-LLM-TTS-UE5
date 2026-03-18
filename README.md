# STT-LLM-TTS-UE5

Runtime speech stack for Unreal Engine 5 extracted from `Source/LipsyncTest`.

Repository contents:
- `ULocalRuntimeSTT` for microphone capture and offline speech recognition through Vosk
- `ULocalRuntimeLLM` for local LLM requests through `llama-server`
- `ULocalRuntimeTTS` for local speech synthesis through Piper
- minimal module files required to compile these classes inside an Unreal module

Legacy component-based `LocalLLMComponent` and `LocalTTSComponent` are intentionally excluded. The current API is UObject-based.

## Included files

Main runtime classes:
- `Source/LipsyncTest/LocalRuntimeSTT.*`
- `Source/LipsyncTest/LocalRuntimeLLM.*`
- `Source/LipsyncTest/LocalRuntimeTTS.*`

Module files:
- `Source/LipsyncTest/LipsyncTest.Build.cs`
- `Source/LipsyncTest/LipsyncTest.cpp`
- `Source/LipsyncTest/LipsyncTest.h`

## External dependencies

The code expects these Unreal modules:
- `HTTP`
- `Json`
- `JsonUtilities`
- `RuntimeAudioImporter`
- `AudioCapture`
- `AudioCaptureCore`

The original `Build.cs` in this repo already includes them.

The code also expects these external runtime files in your project directory:

LLM:
- `Llama/llama-server.exe`
- `Models/qwen2.5-3b-instruct-q4_k_m.gguf`

TTS:
- `Piper/piper.exe`
- `Models/tts/ru_RU-denis-medium.onnx`
- `Models/tts/ru_RU-denis-medium.onnx.json` or an explicit `ModelConfigPath`

STT:
- `Vosk/vosk.dll`
- `Models/vosk/vosk-model-ru-0.42`

All paths can be overridden in Blueprint or C++.

## What each class does

### `ULocalRuntimeSTT`

Features:
- captures microphone input through `AudioCapture`
- loads Vosk dynamically from `vosk.dll`
- supports wake word flow
- supports standby/active listening states
- emits partial and final recognition events
- supports keyword detection and verbose logging

Defaults:
- `VoskDllPath = "Vosk/vosk.dll"`
- `ModelPath = "Models/vosk/vosk-model-ru-0.42"`
- `WakeKeywords = ["робот"]`
- `bUseWakeWord = true`
- `bWakeWordFocusMode = true`

Important behavior:
- in standby mode it waits for the wake word
- after wake word detection it switches to active mode
- in wake-word focus mode it returns the phrase after the wake word as the final result
- `EnterActive()` can be used as a manual wake-word equivalent
- `NotifyTTSStarted()` puts STT into standby so TTS does not immediately feed the recognizer flow

Main events:
- `OnPartial`
- `OnFinal`
- `OnWakeWordDetected`
- `OnKeywordDetected`
- `OnError`

### `ULocalRuntimeLLM`

Features:
- starts and stops `llama-server.exe`
- waits for server health before sending requests
- supports retries on `503`
- supports chat completions
- supports structured JSON responses
- can parse event calls from the model response
- can optionally auto-invoke allowed events on a target UObject
- supports optional streaming
- supports short conversation history

Defaults:
- `ServerExePath = "Llama/llama-server.exe"`
- `ModelPath = "Models/qwen2.5-3b-instruct-q4_k_m.gguf"`
- `Host = "127.0.0.1"`
- `Port = 8080`
- `bAutoStartServer = true`
- `bWaitForServer = true`
- `bExpectStructuredResponse = true`

Default system behavior:
- expects JSON only
- expects a non-empty `reply`
- optionally accepts `events`
- can force Russian replies

Main events:
- `OnResponse`
- `OnPartialResponse`
- `OnEvents`
- `OnServerStarted`
- `OnError`

### `ULocalRuntimeTTS`

Features:
- runs Piper locally
- returns generated PCM through a Blueprint event
- supports request queueing
- supports cancellation of previous generation
- can validate PCM and detect corrupted output
- can retry on CPU after bad CUDA output
- can save generated WAV files for debugging

Defaults:
- `PiperExePath = "Piper/piper.exe"`
- `ModelPath = "Models/tts/ru_RU-denis-medium.onnx"`
- `bQueueSpeech = true`
- `bCancelPreviousOnNewRequest = true`
- `bValidatePcm = true`
- `bFallbackToCpuOnBadPcm = true`

Output:
- audio is broadcast through `OnSpeechResult`
- format is mono `Int16` PCM

Main events:
- `OnSpeechResult`
- `OnError`

## Basic usage

### Blueprint flow

1. Create the objects:
   - `Create Runtime STT`
   - `Create Runtime LLM`
   - `Create Runtime TTS`
2. Store them in variables so they are not garbage collected.
3. Bind to their delegate events.
4. Start STT with `StartListening()`.
5. On final STT text, pass the text to `SendPrompt()`.
6. On LLM response, pass `reply` to `TextToSpeech()`.
7. Call `NotifyTTSStarted()` before playback and `NotifyTTSEnded()` after playback if STT should stay in wake-word standby during speech output.

### Minimal C++ example

```cpp
ULocalRuntimeSTT* STT = ULocalRuntimeSTT::CreateRuntimeSTT(this);
ULocalRuntimeLLM* LLM = ULocalRuntimeLLM::CreateRuntimeLLM(this);
ULocalRuntimeTTS* TTS = ULocalRuntimeTTS::CreateRuntimeTTS();

STT->OnFinal.AddDynamic(this, &AMyActor::HandleFinalText);
LLM->OnResponse.AddDynamic(this, &AMyActor::HandleLLMResponse);
TTS->OnSpeechResult.AddDynamic(this, &AMyActor::HandleSpeech);

STT->StartListening();
```

Typical callback chain:

```cpp
void AMyActor::HandleFinalText(const FString& Text)
{
	LLM->SendPrompt(Text, FString());
}

void AMyActor::HandleLLMResponse(const FString& Prompt, const FString& Response)
{
	STT->NotifyTTSStarted();
	TTS->TextToSpeech(Response);
}
```

## Notes

- This repository is not a full standalone plugin. It is a clean extraction of the `Source/LipsyncTest` runtime code.
- Generated headers, module name, and API macro are still based on the original module name `LIPSYNCTEST_API`.
- If you move these files into another project or plugin, update module names, `Build.cs`, and API macros accordingly.
- STT currently uses Vosk only.
- LLM and TTS are designed for local/offline inference, not cloud APIs.

## Current structure

```text
Source/LipsyncTest/
  LipsyncTest.Build.cs
  LipsyncTest.cpp
  LipsyncTest.h
  LocalRuntimeLLM.cpp
  LocalRuntimeLLM.h
  LocalRuntimeSTT.cpp
  LocalRuntimeSTT.h
  LocalRuntimeTTS.cpp
  LocalRuntimeTTS.h
```
