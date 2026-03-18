// Fill out your copyright notice in the Description page of Project Settings.

#include "LocalRuntimeSTT.h"

#include "AudioCapture.h"
#include "AudioCaptureCore.h"
#include "Async/Async.h"
#include "Containers/Queue.h"
#include "Dom/JsonObject.h"
#include "HAL/CriticalSection.h"
#include "HAL/Event.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "HAL/ThreadSafeBool.h"
#include "HAL/PlatformMisc.h"
#include "Misc/ScopeLock.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "TimerManager.h"
#include "Engine/Engine.h"

DEFINE_LOG_CATEGORY_STATIC(LogLocalRuntimeSTT, Log, All);

namespace
{
	static FString ToLowerCopy(const FString& In)
	{
		FString Out = In;
		Out.ToLowerInline();
		return Out;
	}
}

class ULocalRuntimeSTT::FSTTWorker : public FRunnable
{
public:
	FSTTWorker(ULocalRuntimeSTT* InOwner, int32 InSampleRate, int32 InCaptureChannels)
		: Owner(InOwner)
		, SampleRate(InSampleRate)
		, CaptureChannels(InCaptureChannels)
	{
		Event = FPlatformProcess::GetSynchEventFromPool(false);
		AudioCapture = MakeUnique<Audio::FAudioCapture>();
	}

	~FSTTWorker()
	{
		Stop();
		if (Thread)
		{
			Thread->Kill(true);
			delete Thread;
			Thread = nullptr;
		}
		if (Event)
		{
			FPlatformProcess::ReturnSynchEventToPool(Event);
			Event = nullptr;
		}
	}

	bool Start()
	{
		if (!AudioCapture.IsValid())
		{
			UE_LOG(LogLocalRuntimeSTT, Warning, TEXT("STT: AudioCapture is invalid."));
			return false;
		}

		Audio::FCaptureDeviceInfo DeviceInfo;
		if (!AudioCapture->GetCaptureDeviceInfo(DeviceInfo, Audio::DefaultDeviceIndex))
		{
			UE_LOG(LogLocalRuntimeSTT, Warning, TEXT("STT: Failed to get capture device info."));
			return false;
		}
		if (Owner && Owner->bLogDebug)
		{
			UE_LOG(LogLocalRuntimeSTT, Log, TEXT("STT: Device='%s' SR=%d Channels=%d"),
				*DeviceInfo.DeviceName, DeviceInfo.PreferredSampleRate, DeviceInfo.InputChannels);
		}

		Audio::FAudioCaptureDeviceParams Params;
		Params.DeviceIndex = Audio::DefaultDeviceIndex;
		Params.SampleRate = SampleRate;
		Params.NumInputChannels = CaptureChannels;
		Params.PCMAudioEncoding = Audio::EPCMAudioEncoding::FLOATING_POINT_32;
		if (Owner && Owner->bLogDebug)
		{
			UE_LOG(LogLocalRuntimeSTT, Log, TEXT("STT: Capture params SR=%d Channels=%d"), SampleRate, CaptureChannels);
		}

		const Audio::FOnAudioCaptureFunction OnCapture = [this](const void* AudioData, int32 NumFrames, int32 InNumChannels, int32 InSampleRate, double StreamTime, bool bOverflow)
		{
			++CaptureCallbackCount;
			const float* FloatData = static_cast<const float*>(AudioData);
			const int32 NumSamples = NumFrames;
			TArray<int16> Pcm;
			Pcm.SetNumUninitialized(NumSamples);

			if (InNumChannels <= 1)
			{
				for (int32 i = 0; i < NumSamples; ++i)
				{
					const float Clamped = FMath::Clamp(FloatData[i], -1.0f, 1.0f);
					Pcm[i] = (int16)(Clamped * 32767.0f);
				}
			}
			else
			{
				for (int32 Frame = 0; Frame < NumFrames; ++Frame)
				{
					float Sum = 0.0f;
					for (int32 Ch = 0; Ch < InNumChannels; ++Ch)
					{
						Sum += FloatData[Frame * InNumChannels + Ch];
					}
					const float Mixed = Sum / (float)InNumChannels;
					const float Clamped = FMath::Clamp(Mixed, -1.0f, 1.0f);
					Pcm[Frame] = (int16)(Clamped * 32767.0f);
				}
			}

			{
				FScopeLock Lock(&QueueMutex);
				Queue.Enqueue(MoveTemp(Pcm));
			}
			if (Owner && Owner->bLogAudio && Owner->AudioLogEveryN > 0 && (CaptureCallbackCount % Owner->AudioLogEveryN) == 0)
			{
				UE_LOG(LogLocalRuntimeSTT, Verbose, TEXT("STT: Captured frames=%d samples=%d ch=%d sr=%d overflow=%d"),
					NumFrames, NumSamples, InNumChannels, InSampleRate, bOverflow ? 1 : 0);
			}
			Event->Trigger();
		};

		const uint32 NumFramesDesired = 1024;
		if (!AudioCapture->OpenAudioCaptureStream(Params, OnCapture, NumFramesDesired))
		{
			UE_LOG(LogLocalRuntimeSTT, Warning, TEXT("STT: OpenAudioCaptureStream failed."));
			return false;
		}

		AudioCapture->StartStream();
		if (Owner && Owner->bLogDebug)
		{
			UE_LOG(LogLocalRuntimeSTT, Log, TEXT("STT: Audio capture started."));
		}

		Thread = FRunnableThread::Create(this, TEXT("LocalSTTWorker"), 0, TPri_Normal);
		if (!Thread)
		{
			UE_LOG(LogLocalRuntimeSTT, Warning, TEXT("STT: Failed to create worker thread."));
		}
		return Thread != nullptr;
	}

	void Stop() override
	{
		bStop = true;
		if (Event)
		{
			Event->Trigger();
		}
		if (AudioCapture.IsValid())
		{
			AudioCapture->StopStream();
			AudioCapture->CloseStream();
			if (Owner && Owner->bLogDebug)
			{
				UE_LOG(LogLocalRuntimeSTT, Log, TEXT("STT: Audio capture stopped."));
			}
		}
	}

	uint32 Run() override
	{
		while (!bStop)
		{
			Event->Wait();

			for (;;)
			{
				TArray<int16> Chunk;
				{
					FScopeLock Lock(&QueueMutex);
					if (!Queue.Dequeue(Chunk))
					{
						break;
					}
				}

				if (!Owner || !Owner->VoskRecognizer)
				{
					continue;
				}

				++ProcessedChunks;
				const char* Data = reinterpret_cast<const char*>(Chunk.GetData());
				const int32 NumBytes = Chunk.Num() * sizeof(int16);
				const int Accepted = Owner->VoskRecognizerAcceptWaveform(Owner->VoskRecognizer, Data, NumBytes);
				if (Accepted)
				{
					if (Owner->bLogDebug && Owner->AudioLogEveryN > 0 && (ProcessedChunks % Owner->AudioLogEveryN) == 0)
					{
						UE_LOG(LogLocalRuntimeSTT, Verbose, TEXT("STT: Vosk accepted chunk bytes=%d"), NumBytes);
					}
					const char* Result = Owner->VoskRecognizerResult(Owner->VoskRecognizer);
					if (Result)
					{
						const FString JsonText = UTF8_TO_TCHAR(Result);
						AsyncTask(ENamedThreads::GameThread, [Owner = Owner, JsonText]()
						{
							Owner->HandleResultJson(JsonText, true);
						});
					}
				}
				else
				{
					if (Owner->bLogDebug && Owner->AudioLogEveryN > 0 && (ProcessedChunks % Owner->AudioLogEveryN) == 0)
					{
						UE_LOG(LogLocalRuntimeSTT, Verbose, TEXT("STT: Vosk partial chunk bytes=%d"), NumBytes);
					}
					const char* Partial = Owner->VoskRecognizerPartialResult(Owner->VoskRecognizer);
					if (Partial)
					{
						const FString JsonText = UTF8_TO_TCHAR(Partial);
						AsyncTask(ENamedThreads::GameThread, [Owner = Owner, JsonText]()
						{
							Owner->HandleResultJson(JsonText, false);
						});
					}
				}
			}

			if (Owner && Owner->bRequestFinal)
			{
				Owner->bRequestFinal = false;
				if (Owner->VoskRecognizer)
				{
					const char* Final = Owner->VoskRecognizerFinalResult(Owner->VoskRecognizer);
					if (Final)
					{
						const FString JsonText = UTF8_TO_TCHAR(Final);
						AsyncTask(ENamedThreads::GameThread, [Owner = Owner, JsonText]()
						{
							Owner->HandleResultJson(JsonText, true);
						});
					}
				}
			}
		}

		if (Owner && Owner->VoskRecognizer)
		{
			const char* Final = Owner->VoskRecognizerFinalResult(Owner->VoskRecognizer);
			if (Final)
			{
				const FString JsonText = UTF8_TO_TCHAR(Final);
				AsyncTask(ENamedThreads::GameThread, [Owner = Owner, JsonText]()
				{
					Owner->HandleResultJson(JsonText, true);
				});
			}
		}

		return 0;
	}

private:
	ULocalRuntimeSTT* Owner = nullptr;
	int32 SampleRate = 16000;
	int32 CaptureChannels = 1;
	int32 CaptureCallbackCount = 0;
	int32 ProcessedChunks = 0;

	TUniquePtr<Audio::FAudioCapture> AudioCapture;
	FRunnableThread* Thread = nullptr;
	FEvent* Event = nullptr;
	TQueue<TArray<int16>, EQueueMode::Mpsc> Queue;
	FCriticalSection QueueMutex;
	FThreadSafeBool bStop = false;
};

ULocalRuntimeSTT::ULocalRuntimeSTT()
{
	// Default project-relative paths (adjust if you use different filenames).
	VoskDllPath = TEXT("Vosk/vosk.dll");
	ModelPath = TEXT("Models/vosk/vosk-model-ru-0.42");
	WakeKeywords = { TEXT("робот") };
}

ULocalRuntimeSTT* ULocalRuntimeSTT::CreateRuntimeSTT(UObject* WorldContextObject)
{
	ULocalRuntimeSTT* Obj = NewObject<ULocalRuntimeSTT>();
	Obj->WorldContextObject = WorldContextObject;
	return Obj;
}

void ULocalRuntimeSTT::BeginDestroy()
{
	StopListening();
	Super::BeginDestroy();
}

UWorld* ULocalRuntimeSTT::GetWorldFromContext() const
{
	if (WorldContextObject.IsValid())
	{
		if (UWorld* World = WorldContextObject->GetWorld())
		{
			return World;
		}
		if (GEngine)
		{
			return GEngine->GetWorldFromContextObject(WorldContextObject.Get(), EGetWorldErrorMode::ReturnNull);
		}
	}
	return nullptr;
}

FString ULocalRuntimeSTT::ResolvePath(const FString& Path) const
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

FString ULocalRuntimeSTT::ExtractPostWakeText(const FString& Text) const
{
	if (LastWakeWord.IsEmpty())
	{
		return Text;
	}

	const FString Lower = ToLowerCopy(Text);
	const FString KeyLower = ToLowerCopy(LastWakeWord);
	const int32 Pos = Lower.Find(KeyLower);
	if (Pos != INDEX_NONE)
	{
		FString Remainder = Text;
		Remainder.RemoveAt(Pos, LastWakeWord.Len(), false);
		return Remainder.TrimStartAndEnd();
	}

	return Text;
}

void ULocalRuntimeSTT::RequestFinalFromWorker()
{
	bRequestFinal = true;
}

bool ULocalRuntimeSTT::StartListening()
{
	if (bListening)
	{
		if (bLogDebug)
		{
			UE_LOG(LogLocalRuntimeSTT, Log, TEXT("STT: StartListening called but already listening."));
		}
		return true;
	}

	bWaitingPostWakeFinal = false;
	LastWakeWord.Empty();
	PostWakeBuffer.Empty();
	bRequestFinal = false;
	bHadSpeechSinceActive = false;

	const FString DllPath = ResolvePath(VoskDllPath);
	const FString ModelFullPath = ResolvePath(ModelPath);
	if (bLogDebug)
	{
		UE_LOG(LogLocalRuntimeSTT, Log, TEXT("STT: DllPath='%s'"), *DllPath);
		UE_LOG(LogLocalRuntimeSTT, Log, TEXT("STT: ModelPath='%s'"), *ModelFullPath);
	}

	if (DllPath.IsEmpty() || !FPaths::FileExists(DllPath))
	{
		UE_LOG(LogLocalRuntimeSTT, Warning, TEXT("STT: Vosk DLL not found: %s"), *DllPath);
		EmitError(TEXT("VoskDllPath is missing or invalid."));
		return false;
	}

	if (ModelFullPath.IsEmpty() || !FPaths::DirectoryExists(ModelFullPath))
	{
		UE_LOG(LogLocalRuntimeSTT, Warning, TEXT("STT: Model path not found: %s"), *ModelFullPath);
		EmitError(TEXT("ModelPath is missing or invalid."));
		return false;
	}

	if (!LoadVosk())
	{
		EmitError(TEXT("Failed to load Vosk DLL or functions."));
		return false;
	}

	// Query default device to avoid unsupported format errors.
	{
		Audio::FAudioCapture TempCapture;
		Audio::FCaptureDeviceInfo DeviceInfo;
		if (TempCapture.GetCaptureDeviceInfo(DeviceInfo, Audio::DefaultDeviceIndex))
		{
			if (DeviceInfo.PreferredSampleRate > 0)
			{
				SampleRate = DeviceInfo.PreferredSampleRate;
			}
			CaptureNumChannels = DeviceInfo.InputChannels > 0 ? DeviceInfo.InputChannels : 1;
			if (bLogDebug)
			{
				UE_LOG(LogLocalRuntimeSTT, Log, TEXT("STT: Using device SR=%d Channels=%d"),
					SampleRate, CaptureNumChannels);
			}
		}
		else
		{
			// Fallback to mono if device info failed.
			CaptureNumChannels = 1;
			if (bLogDebug)
			{
				UE_LOG(LogLocalRuntimeSTT, Warning, TEXT("STT: Device info failed, fallback to mono."));
			}
		}
	}

	if (!CreateRecognizer())
	{
		EmitError(TEXT("Failed to create Vosk recognizer."));
		return false;
	}

	Worker = new FSTTWorker(this, SampleRate, CaptureNumChannels);
	if (!Worker || !Worker->Start())
	{
		EmitError(TEXT("Failed to start audio capture."));
		StopListening();
		return false;
	}

	bActiveListening = !bUseWakeWord;
	if (bUseWakeWord)
	{
		EnterStandby();
	}
	else
	{
		EnterActive();
	}

	bListening = true;
	if (bLogDebug)
	{
		UE_LOG(LogLocalRuntimeSTT, Log, TEXT("STT: Listening started. WakeWord=%d Active=%d"),
			bUseWakeWord ? 1 : 0, bActiveListening ? 1 : 0);
	}
	return true;
}

void ULocalRuntimeSTT::StopListening()
{
	if (!bListening)
	{
		if (bLogDebug)
		{
			UE_LOG(LogLocalRuntimeSTT, Log, TEXT("STT: StopListening called but not listening."));
		}
		return;
	}

	if (Worker)
	{
		Worker->Stop();
		delete Worker;
		Worker = nullptr;
	}

	DestroyRecognizer();
	UnloadVosk();
	if (UWorld* World = GetWorldFromContext())
	{
		World->GetTimerManager().ClearTimer(ActiveTimer);
	}
	bListening = false;
	bWaitingPostWakeFinal = false;
	LastWakeWord.Empty();
	PostWakeBuffer.Empty();
	bRequestFinal = false;
	bHadSpeechSinceActive = false;
	if (bLogDebug)
	{
		UE_LOG(LogLocalRuntimeSTT, Log, TEXT("STT: Listening stopped."));
	}
}

bool ULocalRuntimeSTT::IsListening() const
{
	return bListening;
}

void ULocalRuntimeSTT::EnterStandby()
{
	bActiveListening = false;
	if (bWaitingPostWakeFinal || bHadSpeechSinceActive)
	{
		RequestFinalFromWorker();
	}
	if (!bWaitingPostWakeFinal)
	{
		PostWakeBuffer.Empty();
		LastWakeWord.Empty();
	}
	bHadSpeechSinceActive = false;
	if (UWorld* World = GetWorldFromContext())
	{
		World->GetTimerManager().ClearTimer(ActiveTimer);
	}
	if (bLogDebug)
	{
		UE_LOG(LogLocalRuntimeSTT, Log, TEXT("STT: Enter standby."));
	}
}

void ULocalRuntimeSTT::EnterActive()
{
	bActiveListening = true;
	bHadSpeechSinceActive = false;
	if (bUseWakeWord && bWakeWordFocusMode && !bWaitingPostWakeFinal)
	{
		// Treat manual EnterActive the same as a wake-word trigger: wait for the next final.
		bWaitingPostWakeFinal = true;
		PostWakeBuffer.Empty();
		LastWakeWord.Empty();
	}
	if (bUseWakeWord && (!bDelayActiveTimerUntilTTSFinished || !bTTSActive))
	{
		if (UWorld* World = GetWorldFromContext())
		{
			World->GetTimerManager().SetTimer(
				ActiveTimer,
				this,
				&ULocalRuntimeSTT::HandleActiveTimeout,
				ActiveListenSeconds,
				false
			);
		}
	}
	if (bLogDebug)
	{
		UE_LOG(LogLocalRuntimeSTT, Log, TEXT("STT: Enter active. Timer=%d"), bUseWakeWord ? 1 : 0);
	}
}

void ULocalRuntimeSTT::NotifyTTSStarted()
{
	bTTSActive = true;
	if (bUseWakeWord)
	{
		// While TTS is speaking, stay in standby and only listen for wake words.
		EnterStandby();
	}
	if (UWorld* World = GetWorldFromContext())
	{
		World->GetTimerManager().ClearTimer(ActiveTimer);
	}
	if (bLogDebug)
	{
		UE_LOG(LogLocalRuntimeSTT, Log, TEXT("STT: TTS started, active timer cleared."));
	}
}

void ULocalRuntimeSTT::NotifyTTSEnded()
{
	bTTSActive = false;
	if (bUseWakeWord && bActiveListening)
	{
		EnterActive();
	}
	if (bLogDebug)
	{
		UE_LOG(LogLocalRuntimeSTT, Log, TEXT("STT: TTS ended."));
	}
}

void ULocalRuntimeSTT::EmitError(const FString& Message) const
{
	ULocalRuntimeSTT* MutableThis = const_cast<ULocalRuntimeSTT*>(this);
	MutableThis->OnError.Broadcast(Message);
	UE_LOG(LogLocalRuntimeSTT, Warning, TEXT("STT: Error: %s"), *Message);
}

void ULocalRuntimeSTT::HandleResultJson(const FString& JsonText, bool bFinal)
{
	TSharedPtr<FJsonObject> Root;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		if (bLogJson)
		{
			UE_LOG(LogLocalRuntimeSTT, Warning, TEXT("STT: Failed to parse JSON: %s"), *JsonText);
		}
		return;
	}

	FString Text;
	if (bFinal)
	{
		Root->TryGetStringField(TEXT("text"), Text);
	}
	else
	{
		Root->TryGetStringField(TEXT("partial"), Text);
	}

	if (Text.IsEmpty())
	{
		if (bLogJson)
		{
			UE_LOG(LogLocalRuntimeSTT, Verbose, TEXT("STT: Empty %s result JSON."), bFinal ? TEXT("final") : TEXT("partial"));
		}
		return;
	}
	if (bLogJson)
	{
		UE_LOG(LogLocalRuntimeSTT, Log, TEXT("STT: %s text='%s'"), bFinal ? TEXT("Final") : TEXT("Partial"), *Text);
	}
	if (bActiveListening)
	{
		bHadSpeechSinceActive = true;
	}

	if (bUseWakeWord && bWakeWordFocusMode)
	{
		if (!bActiveListening)
		{
			const bool bWakeMatch = CheckWakeWords(Text, bFinal);
			if (bWakeMatch)
			{
				return;
			}
			// Standby: ignore other speech
			return;
		}

		if (bWaitingPostWakeFinal)
		{
			const FString Remainder = ExtractPostWakeText(Text);
			if (!Remainder.IsEmpty())
			{
				PostWakeBuffer = Remainder;
			}

			if (!bFinal)
			{
				return;
			}

			if (!PostWakeBuffer.IsEmpty())
			{
				OnFinal.Broadcast(PostWakeBuffer);
				CheckKeywords(PostWakeBuffer);
			}
			else if (bLogDebug)
			{
				UE_LOG(LogLocalRuntimeSTT, Verbose, TEXT("STT: Final after wake word, but remainder is empty."));
			}

			bWaitingPostWakeFinal = false;
			PostWakeBuffer.Empty();
			LastWakeWord.Empty();
			if (bUseWakeWord)
			{
				bHadSpeechSinceActive = false;
				EnterStandby();
			}
			return;
		}
	}

	if (bUseWakeWord && !bActiveListening)
	{
		const bool bWakeMatch = CheckWakeWords(Text, bFinal);
		if (bWakeMatch)
		{
			return;
		}
		// Standby: ignore other speech
		return;
	}

	if (bFinal)
	{
		OnFinal.Broadcast(Text);
		CheckKeywords(Text);
		if (bExtendActiveOnFinal && bUseWakeWord)
		{
			EnterActive();
		}
	}
	else
	{
		OnPartial.Broadcast(Text);
		if (bDetectKeywordsInPartial)
		{
			CheckKeywords(Text);
		}
		if (bExtendActiveOnPartial && bUseWakeWord)
		{
			EnterActive();
		}
	}
}

void ULocalRuntimeSTT::CheckKeywords(const FString& Text)
{
	if (Keywords.Num() == 0)
	{
		return;
	}

	const FString Lower = ToLowerCopy(Text);
	for (const FString& Keyword : Keywords)
	{
		if (Keyword.IsEmpty())
		{
			continue;
		}
		const FString KeyLower = ToLowerCopy(Keyword);
		if (Lower.Contains(KeyLower))
		{
			const double Now = FPlatformTime::Seconds();
			const double* LastTime = LastKeywordTime.Find(KeyLower);
			if (LastTime && (Now - *LastTime) < KeywordCooldownSeconds)
			{
				if (bLogKeywords)
				{
					UE_LOG(LogLocalRuntimeSTT, Verbose, TEXT("STT: Keyword '%s' ignored due to cooldown."), *Keyword);
				}
				continue;
			}
			LastKeywordTime.Add(KeyLower, Now);
			if (bLogKeywords)
			{
				UE_LOG(LogLocalRuntimeSTT, Log, TEXT("STT: Keyword detected '%s'"), *Keyword);
			}
			OnKeywordDetected.Broadcast(Keyword);
		}
	}
}

bool ULocalRuntimeSTT::CheckWakeWords(const FString& Text, bool bFinal)
{
	if (WakeKeywords.Num() == 0)
	{
		return false;
	}

	if (!bWakeOnPartial && !bFinal)
	{
		return false;
	}

	const FString Lower = ToLowerCopy(Text);
	for (const FString& Keyword : WakeKeywords)
	{
		if (Keyword.IsEmpty())
		{
			continue;
		}
		const FString KeyLower = ToLowerCopy(Keyword);
		const int32 Pos = Lower.Find(KeyLower);
		if (Pos != INDEX_NONE)
		{
			if (bLogWakeWord)
			{
				UE_LOG(LogLocalRuntimeSTT, Log, TEXT("STT: Wake word detected '%s' (final=%d)"), *Keyword, bFinal ? 1 : 0);
			}
			LastWakeWord = Keyword;
			if (bWakeWordFocusMode)
			{
				bWaitingPostWakeFinal = true;
				PostWakeBuffer = ExtractPostWakeText(Text);
			}
			TransitionToActive(Keyword);
			if (bFinal)
			{
				if (bWakeWordFocusMode)
				{
					const FString Remainder = ExtractPostWakeText(Text);
					if (!Remainder.IsEmpty())
					{
						PostWakeBuffer = Remainder;
						if (bLogWakeWord)
						{
							UE_LOG(LogLocalRuntimeSTT, Log, TEXT("STT: Wake remainder '%s'"), *Remainder);
						}
						OnFinal.Broadcast(Remainder);
						CheckKeywords(Remainder);
						bWaitingPostWakeFinal = false;
						PostWakeBuffer.Empty();
						LastWakeWord.Empty();
						if (bUseWakeWord)
						{
							bHadSpeechSinceActive = false;
							EnterStandby();
						}
					}
					else if (bLogDebug)
					{
						UE_LOG(LogLocalRuntimeSTT, Verbose, TEXT("STT: Wake word only, waiting for next utterance."));
					}
				}
				else
				{
					FString Remainder = Text;
					Remainder.RemoveAt(Pos, Keyword.Len(), false);
					Remainder = Remainder.TrimStartAndEnd();
					if (!Remainder.IsEmpty())
					{
						if (bLogWakeWord)
						{
							UE_LOG(LogLocalRuntimeSTT, Log, TEXT("STT: Wake remainder '%s'"), *Remainder);
						}
						OnFinal.Broadcast(Remainder);
						CheckKeywords(Remainder);
						if (bExtendActiveOnFinal && bUseWakeWord)
						{
							EnterActive();
						}
					}
				}
			}
			return true;
		}
	}
	return false;
}

void ULocalRuntimeSTT::TransitionToActive(const FString& WakeWord)
{
	OnWakeWordDetected.Broadcast(WakeWord);
	EnterActive();
}

void ULocalRuntimeSTT::HandleActiveTimeout()
{
	if (bWaitingPostWakeFinal || bHadSpeechSinceActive)
	{
		RequestFinalFromWorker();
	}
	EnterStandby();
	if (bLogDebug)
	{
		UE_LOG(LogLocalRuntimeSTT, Log, TEXT("STT: Active timeout, entering standby."));
	}
}

FString ULocalRuntimeSTT::BuildGrammarJson() const
{
	TArray<FString> Phrases = GrammarPhrases;
	if (Phrases.Num() == 0)
	{
		Phrases = Keywords;
	}

	FString Out;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
	Writer->WriteArrayStart();
	for (const FString& Phrase : Phrases)
	{
		if (!Phrase.IsEmpty())
		{
			Writer->WriteValue(Phrase);
		}
	}
	Writer->WriteArrayEnd();
	Writer->Close();
	return Out;
}

bool ULocalRuntimeSTT::LoadVosk()
{
	if (VoskLibHandle)
	{
		if (bLogDebug)
		{
			UE_LOG(LogLocalRuntimeSTT, Log, TEXT("STT: Vosk already loaded."));
		}
		return true;
	}

	FString DllPath = ResolvePath(VoskDllPath);
	if (DllPath.IsEmpty() || !FPaths::FileExists(DllPath))
	{
		const FString CandidateA = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("Vosk/vosk.dll"));
		const FString CandidateB = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("Vosk/libvosk.dll"));
		if (FPaths::FileExists(CandidateA))
		{
			DllPath = CandidateA;
		}
		else if (FPaths::FileExists(CandidateB))
		{
			DllPath = CandidateB;
		}
	}

	const FString DllDir = FPaths::GetPath(DllPath);
	if (!DllDir.IsEmpty())
	{
		FPlatformProcess::PushDllDirectory(*DllDir);
	}

	VoskLibHandle = FPlatformProcess::GetDllHandle(*DllPath);

	if (!DllDir.IsEmpty())
	{
		FPlatformProcess::PopDllDirectory(*DllDir);
	}

	if (!VoskLibHandle)
	{
		TCHAR ErrMsg[512];
		const uint32 Err = FPlatformMisc::GetLastError();
		FPlatformMisc::GetSystemErrorMessage(ErrMsg, UE_ARRAY_COUNT(ErrMsg), Err);
		UE_LOG(LogLocalRuntimeSTT, Warning, TEXT("STT: Failed to load Vosk DLL: %s (Err=%u)"), ErrMsg, Err);
		return false;
	}
	if (bLogDebug)
	{
		UE_LOG(LogLocalRuntimeSTT, Log, TEXT("STT: Vosk DLL loaded."));
	}

	VoskModelNew = (FnVoskModelNew)FPlatformProcess::GetDllExport(VoskLibHandle, TEXT("vosk_model_new"));
	VoskModelFree = (FnVoskModelFree)FPlatformProcess::GetDllExport(VoskLibHandle, TEXT("vosk_model_free"));
	VoskRecognizerNew = (FnVoskRecognizerNew)FPlatformProcess::GetDllExport(VoskLibHandle, TEXT("vosk_recognizer_new"));
	VoskRecognizerNewGrm = (FnVoskRecognizerNewGrm)FPlatformProcess::GetDllExport(VoskLibHandle, TEXT("vosk_recognizer_new_grm"));
	VoskRecognizerFree = (FnVoskRecognizerFree)FPlatformProcess::GetDllExport(VoskLibHandle, TEXT("vosk_recognizer_free"));
	VoskRecognizerAcceptWaveform = (FnVoskRecognizerAcceptWaveform)FPlatformProcess::GetDllExport(VoskLibHandle, TEXT("vosk_recognizer_accept_waveform"));
	VoskRecognizerResult = (FnVoskRecognizerResult)FPlatformProcess::GetDllExport(VoskLibHandle, TEXT("vosk_recognizer_result"));
	VoskRecognizerPartialResult = (FnVoskRecognizerPartialResult)FPlatformProcess::GetDllExport(VoskLibHandle, TEXT("vosk_recognizer_partial_result"));
	VoskRecognizerFinalResult = (FnVoskRecognizerFinalResult)FPlatformProcess::GetDllExport(VoskLibHandle, TEXT("vosk_recognizer_final_result"));
	VoskRecognizerSetWords = (FnVoskRecognizerSetWords)FPlatformProcess::GetDllExport(VoskLibHandle, TEXT("vosk_recognizer_set_words"));

	const bool bOk = VoskModelNew && VoskModelFree && VoskRecognizerNew && VoskRecognizerNewGrm &&
		VoskRecognizerFree && VoskRecognizerAcceptWaveform && VoskRecognizerResult &&
		VoskRecognizerPartialResult && VoskRecognizerFinalResult && VoskRecognizerSetWords;
	if (!bOk)
	{
		UE_LOG(LogLocalRuntimeSTT, Warning, TEXT("STT: Vosk DLL loaded but missing exports."));
	}
	return bOk;
}

void ULocalRuntimeSTT::UnloadVosk()
{
	if (VoskLibHandle)
	{
		FPlatformProcess::FreeDllHandle(VoskLibHandle);
		VoskLibHandle = nullptr;
		if (bLogDebug)
		{
			UE_LOG(LogLocalRuntimeSTT, Log, TEXT("STT: Vosk DLL unloaded."));
		}
	}
}

bool ULocalRuntimeSTT::CreateRecognizer()
{
	if (!VoskModelNew || !VoskRecognizerNew)
	{
		UE_LOG(LogLocalRuntimeSTT, Warning, TEXT("STT: Vosk function pointers not set."));
		return false;
	}

	const FString ModelFullPath = ResolvePath(ModelPath);
	if (ModelFullPath.IsEmpty() || !FPaths::DirectoryExists(ModelFullPath))
	{
		UE_LOG(LogLocalRuntimeSTT, Warning, TEXT("STT: Model path invalid: %s"), *ModelFullPath);
		return false;
	}

	VoskModel = VoskModelNew(TCHAR_TO_UTF8(*ModelFullPath));
	if (!VoskModel)
	{
		UE_LOG(LogLocalRuntimeSTT, Warning, TEXT("STT: Failed to create Vosk model."));
		return false;
	}

	const float SR = (float)SampleRate;
	if (bUseGrammar)
	{
		const FString GrammarJson = BuildGrammarJson();
		if (bLogDebug)
		{
			UE_LOG(LogLocalRuntimeSTT, Log, TEXT("STT: Using grammar (%d phrases)."), GrammarPhrases.Num());
		}
		VoskRecognizer = VoskRecognizerNewGrm(VoskModel, SR, TCHAR_TO_UTF8(*GrammarJson));
	}
	else
	{
		VoskRecognizer = VoskRecognizerNew(VoskModel, SR);
	}

	if (!VoskRecognizer)
	{
		UE_LOG(LogLocalRuntimeSTT, Warning, TEXT("STT: Failed to create Vosk recognizer."));
		return false;
	}

	VoskRecognizerSetWords(VoskRecognizer, bEnableWords ? 1 : 0);
	if (bLogDebug)
	{
		UE_LOG(LogLocalRuntimeSTT, Log, TEXT("STT: Recognizer created. Words=%d"), bEnableWords ? 1 : 0);
	}
	return true;
}

void ULocalRuntimeSTT::DestroyRecognizer()
{
	if (VoskRecognizer)
	{
		VoskRecognizerFree(VoskRecognizer);
		VoskRecognizer = nullptr;
	}
	if (VoskModel)
	{
		VoskModelFree(VoskModel);
		VoskModel = nullptr;
	}
	if (bLogDebug)
	{
		UE_LOG(LogLocalRuntimeSTT, Log, TEXT("STT: Recognizer destroyed."));
	}
}
