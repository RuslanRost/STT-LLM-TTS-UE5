// Fill out your copyright notice in the Description page of Project Settings.

#include "LocalRuntimeTTS.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformAtomics.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#if PLATFORM_WINDOWS
	#include "Windows/AllowWindowsPlatformTypes.h"
	#include <Windows.h>
	#include "Windows/HideWindowsPlatformTypes.h"
#endif

ULocalRuntimeTTS* ULocalRuntimeTTS::CreateRuntimeTTS()
{
	return NewObject<ULocalRuntimeTTS>();
}

void ULocalRuntimeTTS::TextToSpeech(const FString& Text)
{
	if (Text.IsEmpty())
	{
		EmitError(TEXT("Text is empty."));
		return;
	}

	if (bCancelPreviousOnNewRequest)
	{
		PendingTexts.Reset();
		if (bIsGenerating)
		{
			FPlatformAtomics::InterlockedIncrement(&SpeakGeneration);
			bIsGenerating = false;
		}
		StartSpeech(Text);
		return;
	}

	if (bQueueSpeech && bIsGenerating)
	{
		PendingTexts.Add(Text);
		return;
	}

	StartSpeech(Text);
}

void ULocalRuntimeTTS::StartSpeech(const FString& Text)
{
	const int32 Gen = FPlatformAtomics::InterlockedIncrement(&SpeakGeneration);
	bIsGenerating = true;

	UE_LOG(LogTemp, Warning, TEXT("TTS SPEAK: %s"), *Text);

	const FString TextCopy = Text;
	AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [this, TextCopy, Gen]()
	{
		TArray<uint8> PcmData;
		int32 SampleRate = 0;
		FString Error;
		bool bSucceeded = false;
		bool bUseCudaAttempt = bUseCuda;
		int32 Attempts = 0;

		while (Attempts <= MaxBadPcmRetries)
		{
			PcmData.Reset();
			SampleRate = 0;
			Error.Reset();

			if (!RunPiperToRawPcmWithCuda(TextCopy, bUseCudaAttempt, PcmData, SampleRate, Error))
			{
				break;
			}

			if (bValidatePcm)
			{
				if (PcmData.Num() < MinPcmBytes)
				{
					Error = FString::Printf(TEXT("PCM buffer too small (%d bytes)."), PcmData.Num());
				}
				else
				{
					float Rms = 0.0f;
					float Peak = 0.0f;
					float ClipRatio = 0.0f;
					if (ComputePcmStats(PcmData, Rms, Peak, ClipRatio))
					{
						if (bLogPcmStats)
						{
							UE_LOG(LogTemp, Warning, TEXT("TTS PCM STATS: rms=%.3f peak=%.3f clip=%.3f bytes=%d"), Rms, Peak, ClipRatio, PcmData.Num());
						}

						if (Rms > BadPcmRmsThreshold || ClipRatio > BadPcmClipRatioThreshold)
						{
							Error = FString::Printf(TEXT("PCM validation failed (rms=%.3f peak=%.3f clip=%.3f)."), Rms, Peak, ClipRatio);
						}
						else
						{
							Error.Reset();
						}
					}
					else
					{
						Error = TEXT("Failed to compute PCM stats.");
					}
				}
			}

			if (Error.IsEmpty())
			{
				bSucceeded = true;
				break;
			}

			if (bUseCudaAttempt && bFallbackToCpuOnBadPcm)
			{
				UE_LOG(LogTemp, Warning, TEXT("TTS: Detected bad PCM, retrying on CPU."));
				bUseCudaAttempt = false;
				Attempts++;
				continue;
			}

			Attempts++;
		}

		if (!bSucceeded)
		{
			AsyncTask(ENamedThreads::GameThread, [this, Error]()
			{
				bIsGenerating = false;
				EmitError(Error.IsEmpty() ? TEXT("Failed to generate valid PCM.") : Error);
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

			if (bSaveWavDebug)
			{
				const FString BaseDir = DebugWavDir.IsEmpty() ? (FPaths::ProjectSavedDir() / TEXT("TTS")) : ResolvePath(DebugWavDir);
				IFileManager::Get().MakeDirectory(*BaseDir, true);
				const int64 Ticks = FDateTime::UtcNow().GetTicks();
				const FString FileName = FString::Printf(TEXT("tts_%lld.wav"), Ticks);
				const FString FilePath = FPaths::Combine(BaseDir, FileName);
				FString WavError;
				if (!WritePcm16ToWavFile(FilePath, PcmData, SampleRate, 1, WavError))
				{
					UE_LOG(LogTemp, Warning, TEXT("TTS WAV SAVE ERROR: %s"), *WavError);
				}
				else
				{
					UE_LOG(LogTemp, Warning, TEXT("TTS WAV SAVED: %s"), *FilePath);
				}
			}

			OnSpeechResult.Broadcast(TextCopy, PcmData, SampleRate, 1, ERuntimeRAWAudioFormat::Int16);

			if (PendingTexts.Num() > 0)
			{
				const FString Next = PendingTexts[0];
				PendingTexts.RemoveAt(0);
				StartSpeech(Next);
			}
		});
	});
}

FString ULocalRuntimeTTS::ResolvePath(const FString& Path) const
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

void ULocalRuntimeTTS::EmitError(const FString& Message) const
{
	UE_LOG(LogTemp, Warning, TEXT("TTS ERROR: %s"), *Message);
	ULocalRuntimeTTS* MutableThis = const_cast<ULocalRuntimeTTS*>(this);
	MutableThis->OnError.Broadcast(Message);
}

bool ULocalRuntimeTTS::LoadSampleRate(int32& OutSampleRate, FString& OutError) const
{
	FString ConfigPath = ResolvePath(ModelConfigPath);
	const FString ModelFullPath = ResolvePath(ModelPath);
	if (ConfigPath.IsEmpty() && !ModelFullPath.IsEmpty())
	{
		ConfigPath = ModelFullPath + TEXT(".json");
	}

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

bool ULocalRuntimeTTS::RunPiperToRawPcm(const FString& Text, TArray<uint8>& OutPcm, int32& OutSampleRate, FString& OutError) const
{
	return RunPiperToRawPcmWithCuda(Text, bUseCuda, OutPcm, OutSampleRate, OutError);
}

bool ULocalRuntimeTTS::RunPiperToRawPcmWithCuda(const FString& Text, bool bUseCudaOverride, TArray<uint8>& OutPcm, int32& OutSampleRate, FString& OutError) const
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

#if PLATFORM_WINDOWS
	if (bForceWavOutput)
	{
		return RunPiperToWavPcm(Text, OutPcm, OutSampleRate, OutError);
	}

	void* StdOutRead = nullptr;
	void* StdOutWrite = nullptr;
	void* StdInRead = nullptr;
	void* StdInWrite = nullptr;
	FPlatformProcess::CreatePipe(StdOutRead, StdOutWrite, false);
	FPlatformProcess::CreatePipe(StdInRead, StdInWrite, true);

	FString Args = FString::Printf(TEXT("--model \"%s\" --output-raw"), *ModelFullPath);
	if (bUseCudaOverride)
	{
		Args += TEXT(" --cuda");
	}
	if (SpeakerId >= 0)
	{
		Args += FString::Printf(TEXT(" --speaker %d"), SpeakerId);
	}
	Args += FString::Printf(TEXT(" --length-scale %.3f --noise-scale %.3f --noise-w %.3f"),
		LengthScale, NoiseScale, NoiseW);

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

	const FTCHARToUTF8 Utf8(*Text);
	DWORD BytesWritten = 0;
	HANDLE InWriteHandle = (HANDLE)StdInWrite;
	WriteFile(InWriteHandle, Utf8.Get(), (DWORD)Utf8.Length(), &BytesWritten, nullptr);
	WriteFile(InWriteHandle, "\n", 1, &BytesWritten, nullptr);
	CloseHandle(InWriteHandle);

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
			}
		}
		else
		{
			FPlatformProcess::Sleep(0.01f);
		}
	}

	while (PeekNamedPipe(OutReadHandle, nullptr, 0, nullptr, &BytesAvailable, nullptr) && BytesAvailable > 0)
	{
		DWORD BytesRead = 0;
		const DWORD ToRead = FMath::Min((DWORD)Buffer.Num(), BytesAvailable);
		if (ReadFile(OutReadHandle, Buffer.GetData(), ToRead, &BytesRead, nullptr) && BytesRead > 0)
		{
			const int32 Offset = OutPcm.Num();
			OutPcm.AddUninitialized(BytesRead);
			FMemory::Memcpy(OutPcm.GetData() + Offset, Buffer.GetData(), BytesRead);
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

	// Ensure PCM16 buffer size is even.
	const int32 SafeBytes = (OutPcm.Num() / 2) * 2;
	if (SafeBytes <= 0)
	{
		OutError = TEXT("Audio buffer too small.");
		return false;
	}
	OutPcm.SetNum(SafeBytes, false);

	return true;
#else
	OutError = TEXT("Piper TTS is only implemented on Windows in this project.");
	return false;
#endif
}

bool ULocalRuntimeTTS::WritePcm16ToWavFile(const FString& FilePath, const TArray<uint8>& PcmData, int32 SampleRate, int32 NumChannels, FString& OutError) const
{
	if (PcmData.Num() <= 0)
	{
		OutError = TEXT("PCM data is empty.");
		return false;
	}
	if (SampleRate <= 0 || NumChannels <= 0)
	{
		OutError = TEXT("Invalid sample rate or channels.");
		return false;
	}

	const int32 BytesPerSample = 2;
	const int32 DataSize = (PcmData.Num() / BytesPerSample) * BytesPerSample;
	if (DataSize <= 0)
	{
		OutError = TEXT("PCM data size is invalid.");
		return false;
	}

	TArray<uint8> Wav;
	Wav.Reserve(44 + DataSize);

	auto WriteU32 = [&Wav](uint32 Value)
	{
		Wav.Add((uint8)(Value & 0xFF));
		Wav.Add((uint8)((Value >> 8) & 0xFF));
		Wav.Add((uint8)((Value >> 16) & 0xFF));
		Wav.Add((uint8)((Value >> 24) & 0xFF));
	};
	auto WriteU16 = [&Wav](uint16 Value)
	{
		Wav.Add((uint8)(Value & 0xFF));
		Wav.Add((uint8)((Value >> 8) & 0xFF));
	};

	const uint32 ByteRate = (uint32)(SampleRate * NumChannels * BytesPerSample);
	const uint16 BlockAlign = (uint16)(NumChannels * BytesPerSample);

	Wav.Append(reinterpret_cast<const uint8*>("RIFF"), 4);
	WriteU32((uint32)(36 + DataSize));
	Wav.Append(reinterpret_cast<const uint8*>("WAVE"), 4);
	Wav.Append(reinterpret_cast<const uint8*>("fmt "), 4);
	WriteU32(16);
	WriteU16(1);
	WriteU16((uint16)NumChannels);
	WriteU32((uint32)SampleRate);
	WriteU32(ByteRate);
	WriteU16(BlockAlign);
	WriteU16(16);
	Wav.Append(reinterpret_cast<const uint8*>("data"), 4);
	WriteU32((uint32)DataSize);

	const int32 Offset = Wav.Num();
	Wav.AddUninitialized(DataSize);
	FMemory::Memcpy(Wav.GetData() + Offset, PcmData.GetData(), DataSize);

	if (!FFileHelper::SaveArrayToFile(Wav, *FilePath))
	{
		OutError = TEXT("Failed to save WAV file.");
		return false;
	}

	return true;
}

bool ULocalRuntimeTTS::ComputePcmStats(const TArray<uint8>& PcmData, float& OutRms, float& OutPeak, float& OutClipRatio) const
{
	const int32 NumSamples = PcmData.Num() / 2;
	if (NumSamples <= 0)
	{
		return false;
	}

	const int16* Samples = reinterpret_cast<const int16*>(PcmData.GetData());
	double SumSq = 0.0;
	float Peak = 0.0f;
	int32 ClipCount = 0;
	const float ClipThreshold = 0.98f;

	for (int32 i = 0; i < NumSamples; ++i)
	{
		const float V = (float)Samples[i] / 32768.0f;
		const float AbsV = FMath::Abs(V);
		SumSq += (double)(V * V);
		if (AbsV > Peak)
		{
			Peak = AbsV;
		}
		if (AbsV >= ClipThreshold)
		{
			ClipCount++;
		}
	}

	OutRms = FMath::Sqrt((float)(SumSq / NumSamples));
	OutPeak = Peak;
	OutClipRatio = (float)ClipCount / (float)NumSamples;
	return true;
}

bool ULocalRuntimeTTS::RunPiperToWavPcm(const FString& Text, TArray<uint8>& OutPcm, int32& OutSampleRate, FString& OutError) const
{
#if PLATFORM_WINDOWS
	const FString ExePath = ResolvePath(PiperExePath);
	const FString ModelFullPath = ResolvePath(ModelPath);

	void* StdOutRead = nullptr;
	void* StdOutWrite = nullptr;
	void* StdInRead = nullptr;
	void* StdInWrite = nullptr;
	FPlatformProcess::CreatePipe(StdOutRead, StdOutWrite, false);
	FPlatformProcess::CreatePipe(StdInRead, StdInWrite, true);

	FString Args = FString::Printf(TEXT("--model \"%s\" --output_file -"), *ModelFullPath);
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

	const FTCHARToUTF8 Utf8(*Text);
	DWORD BytesWritten = 0;
	HANDLE InWriteHandle = (HANDLE)StdInWrite;
	WriteFile(InWriteHandle, Utf8.Get(), (DWORD)Utf8.Length(), &BytesWritten, nullptr);
	WriteFile(InWriteHandle, "\n", 1, &BytesWritten, nullptr);
	CloseHandle(InWriteHandle);

	HANDLE OutReadHandle = (HANDLE)StdOutRead;
	TArray<uint8> WavData;
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
				const int32 Offset = WavData.Num();
				WavData.AddUninitialized(BytesRead);
				FMemory::Memcpy(WavData.GetData() + Offset, Buffer.GetData(), BytesRead);
			}
		}
		else
		{
			FPlatformProcess::Sleep(0.01f);
		}
	}

	while (PeekNamedPipe(OutReadHandle, nullptr, 0, nullptr, &BytesAvailable, nullptr) && BytesAvailable > 0)
	{
		DWORD BytesRead = 0;
		const DWORD ToRead = FMath::Min((DWORD)Buffer.Num(), BytesAvailable);
		if (ReadFile(OutReadHandle, Buffer.GetData(), ToRead, &BytesRead, nullptr) && BytesRead > 0)
		{
			const int32 Offset = WavData.Num();
			WavData.AddUninitialized(BytesRead);
			FMemory::Memcpy(WavData.GetData() + Offset, Buffer.GetData(), BytesRead);
		}
		else
		{
			break;
		}
	}

	FPlatformProcess::CloseProc(Proc);
	FPlatformProcess::ClosePipe(StdOutRead, StdOutWrite);
	FPlatformProcess::ClosePipe(StdInRead, StdInWrite);

	if (WavData.Num() == 0)
	{
		OutError = TEXT("No audio data received from piper (wav).");
		return false;
	}

	return ParseWavToPcm16(WavData, OutPcm, OutSampleRate, OutError);
#else
	OutError = TEXT("Piper TTS is only implemented on Windows in this project.");
	return false;
#endif
}

bool ULocalRuntimeTTS::ParseWavToPcm16(const TArray<uint8>& WavData, TArray<uint8>& OutPcm, int32& OutSampleRate, FString& OutError) const
{
	if (WavData.Num() < 44)
	{
		OutError = TEXT("WAV data too small.");
		return false;
	}

	auto ReadLE32 = [&](int32 Offset) -> int32
	{
		if (Offset + 4 > WavData.Num()) return 0;
		const uint8* Ptr = WavData.GetData() + Offset;
		return Ptr[0] | (Ptr[1] << 8) | (Ptr[2] << 16) | (Ptr[3] << 24);
	};

	auto ReadLE16 = [&](int32 Offset) -> int16
	{
		if (Offset + 2 > WavData.Num()) return 0;
		const uint8* Ptr = WavData.GetData() + Offset;
		return (int16)(Ptr[0] | (Ptr[1] << 8));
	};

	int32 FmtOffset = -1;
	int32 DataOffset = -1;
	int32 DataSize = 0;

	for (int32 i = 12; i + 8 <= WavData.Num(); )
	{
		const int32 ChunkId = ReadLE32(i);
		const int32 ChunkSize = ReadLE32(i + 4);
		if (ChunkId == 0x20746D66) // "fmt "
		{
			FmtOffset = i + 8;
		}
		else if (ChunkId == 0x61746164) // "data"
		{
			DataOffset = i + 8;
			DataSize = ChunkSize;
			break;
		}
		i += 8 + ChunkSize;
	}

	if (FmtOffset < 0 || DataOffset < 0 || DataSize <= 0)
	{
		OutError = TEXT("Invalid WAV headers.");
		return false;
	}

	const int16 AudioFormat = ReadLE16(FmtOffset + 0);
	const int16 NumChannels = ReadLE16(FmtOffset + 2);
	const int32 SampleRate = ReadLE32(FmtOffset + 4);
	const int16 BitsPerSample = ReadLE16(FmtOffset + 14);

	if (AudioFormat != 1 || BitsPerSample != 16)
	{
		OutError = TEXT("WAV format unsupported (need PCM16).");
		return false;
	}

	if (SampleRate <= 0 || NumChannels <= 0)
	{
		OutError = TEXT("Invalid WAV sample rate/channels.");
		return false;
	}

	OutSampleRate = SampleRate;

	const int32 BytesPerFrame = NumChannels * 2;
	const int32 FrameCount = DataSize / BytesPerFrame;
	const int16* InSamples = reinterpret_cast<const int16*>(WavData.GetData() + DataOffset);
	TArray<uint8> Pcm16;
	Pcm16.SetNumUninitialized(FrameCount * 2);
	int16* OutSamples = reinterpret_cast<int16*>(Pcm16.GetData());

	if (NumChannels == 1)
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
	return true;
}
