// Fill out your copyright notice in the Description page of Project Settings.

#include "LocalTTSComponent.h"

#include "Components/AudioComponent.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/PlatformAtomics.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Sound/SoundWaveProcedural.h"
#include "TimerManager.h"
#include "RuntimeAudioImporterLibrary.h"

#if PLATFORM_WINDOWS
	#include "Windows/AllowWindowsPlatformTypes.h"
	#include <Windows.h>
	#include "Windows/HideWindowsPlatformTypes.h"
#endif

ULocalTTSComponent::ULocalTTSComponent()
{
	PrimaryComponentTick.bCanEverTick = false;

	// Default project-relative paths (adjust if you use different filenames).
	PiperExePath = TEXT("Piper/piper.exe");
	ModelPath = TEXT("Models/tts/ru_RU-denis-medium.onnx");
}

FString ULocalTTSComponent::ResolvePath(const FString& Path) const
{
	if (Path.IsEmpty())
	{
		return Path;
	}

	if (FPaths::IsRelative(Path))
	{
		return FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / Path);
	}

	return Path;
}

void ULocalTTSComponent::Speak(const FString& Text)
{
	UE_LOG(LogTemp, Warning, TEXT("TTS: Speak called (len=%d)"), Text.Len());
	if (Text.IsEmpty())
	{
		EmitError(TEXT("Text is empty."));
		return;
	}

	// If buffered mode is enabled, route through buffer.
	if (bBufferedSpeech)
	{
		UE_LOG(LogTemp, Warning, TEXT("TTS: Buffered mode enabled -> AppendSpeechChunk"));
		AppendSpeechChunk(Text);
		return;
	}

	if (bQueueSpeech && (bIsGenerating || bIsPlaying))
	{
		UE_LOG(LogTemp, Warning, TEXT("TTS: Queued (generating=%d playing=%d). Queue size before=%d"), bIsGenerating, bIsPlaying, PendingTexts.Num());
		PendingTexts.Add(Text);
		return;
	}

	if (bIsGenerating || bIsPlaying)
	{
		UE_LOG(LogTemp, Warning, TEXT("TTS: Interrupt current (generating=%d playing=%d)"), bIsGenerating, bIsPlaying);
		Interrupt();
	}

	StartSpeech(Text);
}

void ULocalTTSComponent::TextToSpeech(const FString& Text)
{
	UE_LOG(LogTemp, Warning, TEXT("TTS: TextToSpeech called"));
	Speak(Text);
}


void ULocalTTSComponent::AppendSpeechChunk(const FString& Text)
{
	if (Text.IsEmpty())
	{
		return;
	}

	SpeechBuffer += Text;
	SpeechBuffer.TrimStartInline();
	UE_LOG(LogTemp, Warning, TEXT("TTS: Buffer append (buffer_len=%d, words=%d)"), SpeechBuffer.Len(), CountWords(SpeechBuffer));

	// If currently speaking or generating, just accumulate.
	if (bIsGenerating || bIsPlaying)
	{
		return;
	}

	bool bShouldFlush = false;
	if (bFlushOnPunctuation)
	{
		for (int32 i = 0; i < SpeechBuffer.Len(); ++i)
		{
			const TCHAR Ch = SpeechBuffer[i];
			if (Ch == TEXT('.') || Ch == TEXT('!') || Ch == TEXT('?') || Ch == TEXT('\n'))
			{
				bShouldFlush = true;
				break;
			}
		}
	}

	if (!bShouldFlush && CountWords(SpeechBuffer) >= BufferMinWords)
	{
		bShouldFlush = true;
	}

	if (bShouldFlush)
	{
		const FString ToSpeak = SpeechBuffer;
		SpeechBuffer.Reset();
		UE_LOG(LogTemp, Warning, TEXT("TTS: Buffer flush -> StartSpeech (len=%d)"), ToSpeak.Len());
		StartSpeech(ToSpeak);
	}
}

void ULocalTTSComponent::FlushSpeechBuffer()
{
	if (SpeechBuffer.IsEmpty())
	{
		return;
	}

	if (bIsGenerating || bIsPlaying)
	{
		return;
	}

	const FString ToSpeak = SpeechBuffer;
	SpeechBuffer.Reset();
	UE_LOG(LogTemp, Warning, TEXT("TTS: FlushSpeechBuffer -> StartSpeech (len=%d)"), ToSpeak.Len());
	StartSpeech(ToSpeak);
}

void ULocalTTSComponent::StartSpeech(const FString& Text)
{
	const int32 Gen = FPlatformAtomics::InterlockedIncrement(&SpeakGeneration);
	bIsGenerating = true;

	UE_LOG(LogTemp, Warning, TEXT("TTS SPEAK: %s"), *Text);
	UE_LOG(LogTemp, Warning, TEXT("TTS: StartSpeech gen=%d"), Gen);

	const FString TextCopy = Text;
	AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [this, TextCopy, Gen]()
	{
		const double StartTime = FPlatformTime::Seconds();
		TArray<uint8> PcmData;
		int32 SampleRate = 0;
		FString Error;
		if (!RunPiperToRawPcm(TextCopy, PcmData, SampleRate, Error))
		{
			AsyncTask(ENamedThreads::GameThread, [this, Error]()
			{
				bIsGenerating = false;
				EmitError(Error);
			});
			return;
		}

		AsyncTask(ENamedThreads::GameThread, [this, TextCopy, PcmData = MoveTemp(PcmData), SampleRate, Gen]()
		{
			bIsGenerating = false;
			if (Gen != SpeakGeneration)
			{
				UE_LOG(LogTemp, Warning, TEXT("TTS: Discarded PCM (generation mismatch)."));
				return;
			}
			UE_LOG(LogTemp, Warning, TEXT("TTS: PCM ready (bytes=%d sr=%d gen=%d)"), PcmData.Num(), SampleRate, Gen);
			CreateAndPlaySound(TextCopy, PcmData, SampleRate);
		});
		const double EndTime = FPlatformTime::Seconds();
		UE_LOG(LogTemp, Warning, TEXT("TTS: Piper+PCM time=%.2f ms"), (EndTime - StartTime) * 1000.0);
	});
}

void ULocalTTSComponent::Interrupt()
{
	FPlatformAtomics::InterlockedIncrement(&SpeakGeneration);
	ClearQueue();
	bIsGenerating = false;
	bIsPlaying = false;
	SpeechBuffer.Reset();
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(PlaybackTimer);
	}
	if (AudioComponent)
	{
		AudioComponent->Stop();
	}
}

void ULocalTTSComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (AudioComponent)
	{
		AudioComponent->Stop();
		AudioComponent = nullptr;
	}
	if (AudioImporter && ImporterResultHandle.IsValid())
	{
		AudioImporter->OnResultNative.Remove(ImporterResultHandle);
		ImporterResultHandle.Reset();
	}
	AudioImporter = nullptr;
	LastSound = nullptr;
	Super::EndPlay(EndPlayReason);
}

void ULocalTTSComponent::EmitError(const FString& Message) const
{
	UE_LOG(LogTemp, Warning, TEXT("TTS ERROR: %s"), *Message);
	ULocalTTSComponent* MutableThis = const_cast<ULocalTTSComponent*>(this);
	MutableThis->OnTTSError.Broadcast(Message);
}

bool ULocalTTSComponent::LoadSampleRate(int32& OutSampleRate, FString& OutError) const
{
	FString ConfigPath = ResolvePath(ModelConfigPath);
	const FString ModelFullPath = ResolvePath(ModelPath);
	if (ConfigPath.IsEmpty() && !ModelFullPath.IsEmpty())
	{
		ConfigPath = ModelFullPath + TEXT(".json");
	}
	UE_LOG(LogTemp, Warning, TEXT("TTS: Model config path=%s"), *ConfigPath);

	if (ConfigPath.IsEmpty() || !FPaths::FileExists(ConfigPath))
	{
		OutError = TEXT("Model config not found. Set ModelConfigPath or place <ModelPath>.json next to the model.");
		return false;
	}

	FString JsonText;
	if (!FFileHelper::LoadFileToString(JsonText, *ConfigPath))
	{
		OutError = TEXT("Failed to read model config.");
		return false;
	}

	TSharedPtr<FJsonObject> Root;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		OutError = TEXT("Failed to parse model config JSON.");
		return false;
	}

	int32 SampleRate = 0;
	if (Root->HasTypedField<EJson::Object>(TEXT("audio")))
	{
		const TSharedPtr<FJsonObject> Audio = Root->GetObjectField(TEXT("audio"));
		if (Audio.IsValid() && Audio->HasTypedField<EJson::Number>(TEXT("sample_rate")))
		{
			SampleRate = (int32)Audio->GetNumberField(TEXT("sample_rate"));
		}
	}
	if (SampleRate == 0 && Root->HasTypedField<EJson::Number>(TEXT("sample_rate")))
	{
		SampleRate = (int32)Root->GetNumberField(TEXT("sample_rate"));
	}

	if (SampleRate <= 0)
	{
		OutError = TEXT("Sample rate not found in model config.");
		return false;
	}

	OutSampleRate = SampleRate;
	UE_LOG(LogTemp, Warning, TEXT("TTS SAMPLE RATE: %d"), OutSampleRate);
	return true;
}

bool ULocalTTSComponent::RunPiperToRawPcm(const FString& Text, TArray<uint8>& OutPcm, int32& OutSampleRate, FString& OutError) const
{
	const FString ExePath = ResolvePath(PiperExePath);
	const FString ModelFullPath = ResolvePath(ModelPath);
	UE_LOG(LogTemp, Warning, TEXT("TTS: Resolved Exe=%s"), *ExePath);
	UE_LOG(LogTemp, Warning, TEXT("TTS: Resolved Model=%s"), *ModelFullPath);

	if (ExePath.IsEmpty() || !FPaths::FileExists(ExePath))
	{
		OutError = TEXT("PiperExePath is missing or invalid.");
		return false;
	}
	if (ModelFullPath.IsEmpty() || !FPaths::FileExists(ModelFullPath))
	{
		OutError = TEXT("ModelPath is missing or invalid.");
		return false;
	}
	if (!LoadSampleRate(OutSampleRate, OutError))
	{
		return false;
	}

#if PLATFORM_WINDOWS
	if (bForceWavOutput)
	{
		return RunPiperToWavPcm(Text, OutPcm, OutSampleRate, OutError);
	}

	void* StdOutRead = nullptr;
	void* StdOutWrite = nullptr;
	void* StdInRead = nullptr;
	void* StdInWrite = nullptr;
	// We want to read stdout in the parent process, so stdout READ end must be local.
	FPlatformProcess::CreatePipe(StdOutRead, StdOutWrite, false);
	FPlatformProcess::CreatePipe(StdInRead, StdInWrite, true);

	FString Args = FString::Printf(TEXT("--model \"%s\" --output-raw"), *ModelFullPath);
	if (bUseCuda)
	{
		Args += TEXT(" --cuda");
	}
	if (SpeakerId >= 0)
	{
		Args += FString::Printf(TEXT(" --speaker %d"), SpeakerId);
	}
	Args += FString::Printf(TEXT(" --length-scale %.3f --noise-scale %.3f --noise-w %.3f"),
		LengthScale, NoiseScale, NoiseW);

	UE_LOG(LogTemp, Warning, TEXT("TTS PIPER EXE: %s"), *ExePath);
	UE_LOG(LogTemp, Warning, TEXT("TTS MODEL: %s"), *ModelFullPath);
	UE_LOG(LogTemp, Warning, TEXT("TTS ARGS: %s"), *Args);

	const FString WorkingDir = FPaths::GetPath(ExePath);
	FProcHandle Proc = FPlatformProcess::CreateProc(
		*ExePath,
		*Args,
		true,
		true,
		true,
		nullptr,
		0,
		*WorkingDir,
		StdOutWrite,
		StdInRead
	);

	if (!Proc.IsValid())
	{
		OutError = TEXT("Failed to start piper process.");
		FPlatformProcess::ClosePipe(StdOutRead, StdOutWrite);
		FPlatformProcess::ClosePipe(StdInRead, StdInWrite);
		return false;
	}

	// Write UTF-8 text to stdin and close write end.
	const FTCHARToUTF8 Utf8(*Text);
	DWORD BytesWritten = 0;
	HANDLE InWriteHandle = (HANDLE)StdInWrite;
	WriteFile(InWriteHandle, Utf8.Get(), (DWORD)Utf8.Length(), &BytesWritten, nullptr);
	WriteFile(InWriteHandle, "\n", 1, &BytesWritten, nullptr);
	CloseHandle(InWriteHandle);

	// Read raw PCM from stdout.
	HANDLE OutReadHandle = (HANDLE)StdOutRead;
	TArray<uint8> Buffer;
	Buffer.SetNumUninitialized(4096);

	DWORD BytesAvailable = 0;
	while (FPlatformProcess::IsProcRunning(Proc))
	{
		if (PeekNamedPipe(OutReadHandle, nullptr, 0, nullptr, &BytesAvailable, nullptr) && BytesAvailable > 0)
		{
			DWORD BytesRead = 0;
			const DWORD ToRead = FMath::Min((DWORD)Buffer.Num(), BytesAvailable);
			if (ReadFile(OutReadHandle, Buffer.GetData(), ToRead, &BytesRead, nullptr) && BytesRead > 0)
			{
				const int32 Offset = OutPcm.Num();
				OutPcm.AddUninitialized(BytesRead);
				FMemory::Memcpy(OutPcm.GetData() + Offset, Buffer.GetData(), BytesRead);
				UE_LOG(LogTemp, Warning, TEXT("TTS: Read %u bytes (total=%d)"), BytesRead, OutPcm.Num());
			}
		}
		else
		{
			FPlatformProcess::Sleep(0.01f);
		}
	}

	// Drain remaining data.
	while (PeekNamedPipe(OutReadHandle, nullptr, 0, nullptr, &BytesAvailable, nullptr) && BytesAvailable > 0)
	{
		DWORD BytesRead = 0;
		const DWORD ToRead = FMath::Min((DWORD)Buffer.Num(), BytesAvailable);
		if (ReadFile(OutReadHandle, Buffer.GetData(), ToRead, &BytesRead, nullptr) && BytesRead > 0)
		{
			const int32 Offset = OutPcm.Num();
			OutPcm.AddUninitialized(BytesRead);
			FMemory::Memcpy(OutPcm.GetData() + Offset, Buffer.GetData(), BytesRead);
			UE_LOG(LogTemp, Warning, TEXT("TTS: Drain %u bytes (total=%d)"), BytesRead, OutPcm.Num());
		}
		else
		{
			break;
		}
	}

	FPlatformProcess::CloseProc(Proc);
	FPlatformProcess::ClosePipe(StdOutRead, StdOutWrite);
	FPlatformProcess::ClosePipe(StdInRead, StdInWrite);

	if (OutPcm.Num() == 0)
	{
		OutError = TEXT("No audio data received from piper.");
		return false;
	}
	UE_LOG(LogTemp, Warning, TEXT("TTS: PCM bytes=%d"), OutPcm.Num());

	// Heuristic: if PCM16 looks corrupted, try to interpret as float32 and convert.
	const int32 SafeBytes = (OutPcm.Num() / 2) * 2;
	if (SafeBytes <= 0)
	{
		OutError = TEXT("Audio buffer too small.");
		return false;
	}

	auto ComputeClipRatioInt16 = [](const TArray<uint8>& Data) -> float
	{
		const int32 NumSamples = (Data.Num() / 2);
		const int16* Samples = reinterpret_cast<const int16*>(Data.GetData());
		int32 Clipped = 0;
		for (int32 i = 0; i < NumSamples; ++i)
		{
			if (FMath::Abs((int32)Samples[i]) >= 32760)
			{
				++Clipped;
			}
		}
		return NumSamples > 0 ? (float)Clipped / (float)NumSamples : 1.0f;
	};

	const float ClipRatio = ComputeClipRatioInt16(OutPcm);
	if (ClipRatio > 0.40f && (OutPcm.Num() % 4) == 0)
	{
		const int32 NumFloats = OutPcm.Num() / 4;
		const float* FloatData = reinterpret_cast<const float*>(OutPcm.GetData());
		int32 Invalid = 0;
		float MaxAbs = 0.0f;
		float SumAbs = 0.0f;
		for (int32 i = 0; i < NumFloats; ++i)
		{
			const float V = FloatData[i];
			if (!FMath::IsFinite(V))
			{
				++Invalid;
				continue;
			}
			const float AbsV = FMath::Abs(V);
			MaxAbs = FMath::Max(MaxAbs, AbsV);
			SumAbs += AbsV;
		}

		const float InvalidRatio = NumFloats > 0 ? (float)Invalid / (float)NumFloats : 1.0f;
		const float AvgAbs = NumFloats > 0 ? (SumAbs / (float)NumFloats) : 0.0f;
		if (InvalidRatio < 0.01f && MaxAbs <= 2.0f && AvgAbs > 0.001f)
		{
			TArray<uint8> Converted;
			Converted.SetNumUninitialized(NumFloats * 2);
			int16* OutSamples = reinterpret_cast<int16*>(Converted.GetData());
			for (int32 i = 0; i < NumFloats; ++i)
			{
				float V = FloatData[i];
				V = FMath::Clamp(V, -1.0f, 1.0f);
				OutSamples[i] = (int16)(V * 32767.0f);
			}
			OutPcm = MoveTemp(Converted);
			UE_LOG(LogTemp, Warning, TEXT("TTS: Converted float32 raw to PCM16 (auto-detect)."));
		}
	}

	if (bFallbackToWavOnCorrupt)
	{
		const float FinalClipRatio = ComputeClipRatioInt16(OutPcm);
		if (FinalClipRatio > 0.40f)
		{
			UE_LOG(LogTemp, Warning, TEXT("TTS: PCM still corrupted, retrying via WAV output."));
			return RunPiperToWavPcm(Text, OutPcm, OutSampleRate, OutError);
		}
	}

	UE_LOG(LogTemp, Warning, TEXT("TTS PCM BYTES: %d"), OutPcm.Num());
	return true;
#else
	OutError = TEXT("Piper raw pipe reading is only implemented on Windows.");
	return false;
#endif
}

bool ULocalTTSComponent::RunPiperToWavPcm(const FString& Text, TArray<uint8>& OutPcm, int32& OutSampleRate, FString& OutError) const
{
	const FString ExePath = ResolvePath(PiperExePath);
	const FString ModelFullPath = ResolvePath(ModelPath);

	if (ExePath.IsEmpty() || !FPaths::FileExists(ExePath))
	{
		OutError = TEXT("PiperExePath is missing or invalid.");
		return false;
	}
	if (ModelFullPath.IsEmpty() || !FPaths::FileExists(ModelFullPath))
	{
		OutError = TEXT("ModelPath is missing or invalid.");
		return false;
	}
	if (!LoadSampleRate(OutSampleRate, OutError))
	{
		return false;
	}

	const FString OutDir = FPaths::ProjectSavedDir() / TEXT("TTS");
	IFileManager::Get().MakeDirectory(*OutDir, true);
	const FString WavPath = OutDir / TEXT("piper_out.wav");

	auto BuildArgs = [&](const FString& OutputFlag) -> FString
	{
		FString Args = FString::Printf(TEXT("--model \"%s\" %s \"%s\""), *ModelFullPath, *OutputFlag, *WavPath);
		if (bUseCuda)
		{
			Args += TEXT(" --cuda");
		}
		if (SpeakerId >= 0)
		{
			Args += FString::Printf(TEXT(" --speaker %d"), SpeakerId);
		}
		Args += FString::Printf(TEXT(" --length-scale %.3f --noise-scale %.3f --noise-w %.3f"),
			LengthScale, NoiseScale, NoiseW);
		return Args;
	};

	UE_LOG(LogTemp, Warning, TEXT("TTS PIPER EXE: %s"), *ExePath);
	UE_LOG(LogTemp, Warning, TEXT("TTS MODEL: %s"), *ModelFullPath);

	auto RunWithArgs = [&](const FString& Args) -> bool
	{
		void* StdInRead = nullptr;
		void* StdInWrite = nullptr;
		FPlatformProcess::CreatePipe(StdInRead, StdInWrite, true);

		FProcHandle Proc = FPlatformProcess::CreateProc(
			*ExePath,
			*Args,
			true,
			true,
			true,
			nullptr,
			0,
			*FPaths::GetPath(ExePath),
			nullptr,
			StdInRead
		);

		if (!Proc.IsValid())
		{
			FPlatformProcess::ClosePipe(StdInRead, StdInWrite);
			return false;
		}

		const FTCHARToUTF8 Utf8(*Text);
		DWORD BytesWritten = 0;
		HANDLE InWriteHandle = (HANDLE)StdInWrite;
		WriteFile(InWriteHandle, Utf8.Get(), (DWORD)Utf8.Length(), &BytesWritten, nullptr);
		WriteFile(InWriteHandle, "\n", 1, &BytesWritten, nullptr);
		CloseHandle(InWriteHandle);

		FPlatformProcess::WaitForProc(Proc);
		FPlatformProcess::CloseProc(Proc);
		FPlatformProcess::ClosePipe(StdInRead, StdInWrite);
		return true;
	};

	// Try underscore flag first, then dash flag if file not produced.
	FString Args = BuildArgs(TEXT("--output_file"));
	UE_LOG(LogTemp, Warning, TEXT("TTS ARGS: %s"), *Args);
	RunWithArgs(Args);
	if (!FPaths::FileExists(WavPath))
	{
		Args = BuildArgs(TEXT("--output-file"));
		UE_LOG(LogTemp, Warning, TEXT("TTS ARGS (fallback): %s"), *Args);
		RunWithArgs(Args);
	}

	TArray<uint8> WavData;
	if (!FFileHelper::LoadFileToArray(WavData, *WavPath))
	{
		OutError = TEXT("Failed to read wav output.");
		return false;
	}

	return ParseWavToPcm16(WavData, OutPcm, OutSampleRate, OutError);
}

bool ULocalTTSComponent::ParseWavToPcm16(const TArray<uint8>& WavData, TArray<uint8>& OutPcm, int32& OutSampleRate, FString& OutError) const
{
	if (WavData.Num() < 44)
	{
		OutError = TEXT("WAV data too small.");
		return false;
	}

	auto ReadU32 = [&WavData](int32 Offset) -> uint32
	{
		return (uint32)WavData[Offset] | ((uint32)WavData[Offset + 1] << 8) | ((uint32)WavData[Offset + 2] << 16) | ((uint32)WavData[Offset + 3] << 24);
	};
	auto ReadU16 = [&WavData](int32 Offset) -> uint16
	{
		return (uint16)WavData[Offset] | ((uint16)WavData[Offset + 1] << 8);
	};

	if (FMemory::Memcmp(WavData.GetData(), "RIFF", 4) != 0 || FMemory::Memcmp(WavData.GetData() + 8, "WAVE", 4) != 0)
	{
		OutError = TEXT("Invalid WAV header.");
		return false;
	}

	int32 Offset = 12;
	uint16 AudioFormat = 0;
	uint16 NumChannels = 0;
	uint32 SampleRate = 0;
	uint16 BitsPerSample = 0;
	int32 DataOffset = -1;
	int32 DataSize = 0;

	while (Offset + 8 <= WavData.Num())
	{
		const uint32 ChunkId = ReadU32(Offset);
		const uint32 ChunkSize = ReadU32(Offset + 4);
		Offset += 8;

		if (ChunkId == *(const uint32*)"fmt ")
		{
			AudioFormat = ReadU16(Offset);
			NumChannels = ReadU16(Offset + 2);
			SampleRate = ReadU32(Offset + 4);
			BitsPerSample = ReadU16(Offset + 14);
		}
		else if (ChunkId == *(const uint32*)"data")
		{
			DataOffset = Offset;
			DataSize = (int32)ChunkSize;
			break;
		}

		Offset += (int32)ChunkSize;
	}

	if (DataOffset < 0 || DataSize <= 0 || DataOffset + DataSize > WavData.Num())
	{
		OutError = TEXT("WAV data chunk not found.");
		return false;
	}
	if (AudioFormat != 1 || BitsPerSample != 16)
	{
		OutError = TEXT("WAV format not PCM16.");
		return false;
	}

	OutSampleRate = (int32)SampleRate;

	// Downmix to mono if needed.
	const int32 FrameCount = DataSize / (NumChannels * 2);
	const int16* InSamples = reinterpret_cast<const int16*>(WavData.GetData() + DataOffset);
	TArray<uint8> Pcm16;
	Pcm16.SetNumUninitialized(FrameCount * 2);
	int16* OutSamples = reinterpret_cast<int16*>(Pcm16.GetData());

	if (NumChannels <= 1)
	{
		FMemory::Memcpy(Pcm16.GetData(), WavData.GetData() + DataOffset, FrameCount * 2);
	}
	else
	{
		for (int32 Frame = 0; Frame < FrameCount; ++Frame)
		{
			int32 Sum = 0;
			for (int32 Ch = 0; Ch < NumChannels; ++Ch)
			{
				Sum += InSamples[Frame * NumChannels + Ch];
			}
			OutSamples[Frame] = (int16)(Sum / NumChannels);
		}
	}

	OutPcm = MoveTemp(Pcm16);
	UE_LOG(LogTemp, Warning, TEXT("TTS: WAV parsed (sr=%d, ch=%d, bytes=%d)."), OutSampleRate, (int32)NumChannels, DataSize);
	return true;
}

void ULocalTTSComponent::CreateAndPlaySound(const FString& Text, const TArray<uint8>& PcmData, int32 SampleRate)
{
	if (PcmData.Num() == 0 || SampleRate <= 0)
	{
		EmitError(TEXT("Invalid audio data."));
		return;
	}
	UE_LOG(LogTemp, Warning, TEXT("TTS PLAY: bytes=%d sr=%d"), PcmData.Num(), SampleRate);

	// USoundWaveProcedural expects PCM16, so buffer size must be even.
	if ((PcmData.Num() % 2) != 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("TTS PCM BYTES ODD (%d), trimming last byte."), PcmData.Num());
	}

	const int32 SafeBytes = (PcmData.Num() / 2) * 2;
	if (SafeBytes <= 0)
	{
		EmitError(TEXT("Audio buffer too small after trimming."));
		return;
	}

	// Validate PCM to avoid blasting noise if output is corrupted.
	const int32 TotalSamplesCheck = SafeBytes / 2;
	const int16* CheckSamples = reinterpret_cast<const int16*>(PcmData.GetData());
	int32 ClippedCount = 0;
	for (int32 i = 0; i < TotalSamplesCheck; ++i)
	{
		const int32 AbsVal = FMath::Abs((int32)CheckSamples[i]);
		if (AbsVal >= 32760)
		{
			++ClippedCount;
		}
	}
	const float ClipRatio = (float)ClippedCount / (float)TotalSamplesCheck;
	if (ClipRatio > 0.40f)
	{
		EmitError(TEXT("Audio looks corrupted (too many clipped samples). Skipping playback."));
		return;
	}

	// Preserve clean PCM (no pad/fade) for lip sync.
	TArray<uint8> CleanPcm;
	CleanPcm.Append(PcmData.GetData(), SafeBytes);
	LastPcmData = CleanPcm;
	LastPcmSampleRate = SampleRate;
	UE_LOG(LogTemp, Warning, TEXT("TTS PCM READY: bytes=%d sr=%d text_len=%d"), CleanPcm.Num(), SampleRate, Text.Len());
	OnTTSPcmReady.Broadcast(Text, SampleRate, CleanPcm);
	OnTTSSpeechResult.Broadcast(Text, CleanPcm, SampleRate, 1, ERuntimeRAWAudioFormat::Int16);
	OnSpeechResult.Broadcast(Text, CleanPcm, SampleRate, 1, ERuntimeRAWAudioFormat::Int16);
	if (bBroadcastFloatPcm)
	{
		const int32 SampleCount = CleanPcm.Num() / 2;
		const int16* PcmSamples = reinterpret_cast<const int16*>(CleanPcm.GetData());
		TArray<float> FloatPcm;
		FloatPcm.SetNumUninitialized(SampleCount);
		for (int32 i = 0; i < SampleCount; ++i)
		{
			FloatPcm[i] = FMath::Clamp((float)PcmSamples[i] / 32768.0f, -1.0f, 1.0f);
		}
		OnTTSPcmFloatReady.Broadcast(Text, SampleRate, FloatPcm);
	}

	// Apply a short fade-in/out to avoid clicks for playback.
	TArray<uint8> WorkingPcm = CleanPcm;

	if (PadMs > 0.0f)
	{
		const int32 PadSamples = FMath::Max(1, (int32)((PadMs / 1000.0f) * SampleRate));
		const int32 PadBytes = PadSamples * 2;
		TArray<uint8> Padded;
		Padded.AddZeroed(PadBytes);
		Padded.Append(WorkingPcm);
		Padded.AddZeroed(PadBytes);
		WorkingPcm = MoveTemp(Padded);
	}

	if (FadeMs > 0.0f)
	{
		const int32 TotalSamples = WorkingPcm.Num() / 2;
		const int32 FadeSamples = FMath::Clamp((int32)((FadeMs / 1000.0f) * SampleRate), 1, TotalSamples / 2);
		int16* Samples = reinterpret_cast<int16*>(WorkingPcm.GetData());

		for (int32 i = 0; i < FadeSamples; ++i)
		{
			const float Gain = (float)i / (float)FadeSamples;
			Samples[i] = (int16)FMath::Clamp((int32)((float)Samples[i] * Gain), -32768, 32767);
		}

		for (int32 i = 0; i < FadeSamples; ++i)
		{
			const float Gain = (float)(FadeSamples - i) / (float)FadeSamples;
			const int32 Idx = TotalSamples - FadeSamples + i;
			Samples[Idx] = (int16)FMath::Clamp((int32)((float)Samples[Idx] * Gain), -32768, 32767);
		}
	}

	auto PlaySound = [this, Text](USoundWave* Sound)
	{
		if (!Sound)
		{
			EmitError(TEXT("Failed to create sound wave."));
			return;
		}

		LastSound = Sound;
		OnTTSAudioReady.Broadcast(Text, Sound);

		if (bAutoPlay)
		{
			if (!AudioComponent)
			{
				UObject* Outer = GetOwner() ? (UObject*)GetOwner() : (UObject*)this;
				AudioComponent = NewObject<UAudioComponent>(Outer);
				AudioComponent->bAutoDestroy = false;
				if (UWorld* World = GetWorld())
				{
					AudioComponent->RegisterComponentWithWorld(World);
				}
				else
				{
					AudioComponent->RegisterComponent();
				}
			}
			AudioComponent->Stop();
			AudioComponent->SetSound(Sound);
			AudioComponent->SetVolumeMultiplier(Volume);
			AudioComponent->SetPitchMultiplier(Pitch);
			AudioComponent->Play();
			bIsPlaying = true;
			UE_LOG(LogTemp, Warning, TEXT("TTS PLAYBACK START"));
			OnTTSPlaybackStarted.Broadcast();

			if (UWorld* World = GetWorld())
			{
				World->GetTimerManager().ClearTimer(PlaybackTimer);
				const float Duration = Sound->Duration;
				World->GetTimerManager().SetTimer(PlaybackTimer, this, &ULocalTTSComponent::OnPlaybackFinished, Duration, false);
			}
		}
	};

	if (bUseRuntimeAudioImporter)
	{
		if (!AudioImporter)
		{
			AudioImporter = URuntimeAudioImporterLibrary::CreateRuntimeAudioImporter();
		}
		if (!AudioImporter)
		{
			EmitError(TEXT("Runtime Audio Importer is not available."));
			return;
		}

		if (ImporterResultHandle.IsValid())
		{
			AudioImporter->OnResultNative.Remove(ImporterResultHandle);
			ImporterResultHandle.Reset();
		}

		const TArray<uint8> ImportBuffer = WorkingPcm;
		ImporterResultHandle = AudioImporter->OnResultNative.AddWeakLambda(this, [this, Text, PlaySound](URuntimeAudioImporterLibrary* Importer, UImportedSoundWave* ImportedSoundWave, ERuntimeImportStatus Status)
		{
			if (Importer && ImporterResultHandle.IsValid())
			{
				Importer->OnResultNative.Remove(ImporterResultHandle);
				ImporterResultHandle.Reset();
			}

			if (!ImportedSoundWave || Status != ERuntimeImportStatus::SuccessfulImport)
			{
				EmitError(TEXT("Runtime Audio Importer failed to import PCM buffer."));
				return;
			}

			PlaySound(ImportedSoundWave);
		});

		AudioImporter->ImportAudioFromRAWBuffer(ImportBuffer, ERuntimeRAWAudioFormat::Int16, SampleRate, 1);
		return;
	}

	USoundWaveProcedural* Sound = NewObject<USoundWaveProcedural>(this);
	Sound->bProcedural = true;
	Sound->NumChannels = 1;
	Sound->SetSampleRate(SampleRate);
	Sound->Duration = (float)WorkingPcm.Num() / (float)(SampleRate * 2);
	Sound->bLooping = false;
	Sound->SoundGroup = SOUNDGROUP_Default;
	Sound->QueueAudio(WorkingPcm.GetData(), WorkingPcm.Num());

	PlaySound(Sound);
}

void ULocalTTSComponent::OnPlaybackFinished()
{
	bIsPlaying = false;
	UE_LOG(LogTemp, Warning, TEXT("TTS PLAYBACK FINISHED"));
	if (!SpeechBuffer.IsEmpty())
	{
		const FString NextBuffered = SpeechBuffer;
		SpeechBuffer.Reset();
		StartSpeech(NextBuffered);
		return;
	}
	if (PendingTexts.Num() > 0)
	{
		const FString Next = PendingTexts[0];
		PendingTexts.RemoveAt(0);
		StartSpeech(Next);
		return;
	}
	OnTTSPlaybackFinished.Broadcast();
}

void ULocalTTSComponent::ClearQueue()
{
	PendingTexts.Reset();
}

int32 ULocalTTSComponent::CountWords(const FString& Text) const
{
	int32 Count = 0;
	bool bInWord = false;
	for (int32 i = 0; i < Text.Len(); ++i)
	{
		const TCHAR Ch = Text[i];
		const bool bIsSpace = FChar::IsWhitespace(Ch);
		if (!bIsSpace && !bInWord)
		{
			bInWord = true;
			++Count;
		}
		else if (bIsSpace)
		{
			bInWord = false;
		}
	}
	return Count;
}
