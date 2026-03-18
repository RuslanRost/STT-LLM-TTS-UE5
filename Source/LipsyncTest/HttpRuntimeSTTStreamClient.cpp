#include "HttpRuntimeSTTStreamClient.h"

#include "AudioCapture.h"
#include "AudioCaptureCore.h"
#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "HttpModule.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/Base64.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "TimerManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogHttpRuntimeSTTStream, Log, All);

namespace
{
	static FString ToLowerCopy(const FString& In)
	{
		FString Out = In;
		Out.ToLowerInline();
		return Out;
	}
}

UHttpRuntimeSTTStreamClient::UHttpRuntimeSTTStreamClient()
{
	WakeKeywords = { TEXT("робот") };
}

UHttpRuntimeSTTStreamClient* UHttpRuntimeSTTStreamClient::CreateRuntimeSTTStreamClient(UObject* WorldContextObject)
{
	UHttpRuntimeSTTStreamClient* Client = NewObject<UHttpRuntimeSTTStreamClient>();
	Client->WorldContextObject = WorldContextObject;
	Client->ClientId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens);
	return Client;
}

void UHttpRuntimeSTTStreamClient::BeginDestroy()
{
	StopListening();
	Super::BeginDestroy();
}

void UHttpRuntimeSTTStreamClient::SendPing()
{
	SendJsonRequest(TEXT("GET"), TEXT("/ping"), FString(), &UHttpRuntimeSTTStreamClient::HandleSimpleResponse);
}

void UHttpRuntimeSTTStreamClient::GetSTTStatus()
{
	SendJsonRequest(TEXT("GET"), TEXT("/status"), FString(), &UHttpRuntimeSTTStreamClient::HandleSimpleResponse);
}

bool UHttpRuntimeSTTStreamClient::StartListening()
{
	if (bListening || bSessionStarting)
	{
		return true;
	}

	if (!SessionId.IsEmpty())
	{
		EmitError(TEXT("STT session is still closing. Try again in a moment."));
		return false;
	}

	if (!GetWorldFromContext())
	{
		EmitError(TEXT("World context is not available."));
		return false;
	}

	DrainQueuedAudio();
	StopAudioCapture();
	ResetWakeState();
	bStartRequested = true;
	bPendingFinalRequest = false;
	bPendingStopRequest = false;
	bRequestInFlight = false;
	SessionId.Empty();

	if (ClientId.IsEmpty())
	{
		ClientId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens);
	}

	{
		Audio::FAudioCapture TempCapture;
		Audio::FCaptureDeviceInfo DeviceInfo;
		if (TempCapture.GetCaptureDeviceInfo(DeviceInfo, Audio::DefaultDeviceIndex))
		{
			CaptureSampleRate = DeviceInfo.PreferredSampleRate > 0 ? DeviceInfo.PreferredSampleRate : SampleRate;
			CaptureNumChannels = DeviceInfo.InputChannels > 0 ? DeviceInfo.InputChannels : 1;
		}
		else
		{
			CaptureSampleRate = SampleRate;
			CaptureNumChannels = 1;
		}
	}

	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("client_id"), ClientId);
	Payload->SetStringField(TEXT("request_id"), FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens));
	Payload->SetNumberField(TEXT("sample_rate"), CaptureSampleRate);
	Payload->SetNumberField(TEXT("num_channels"), 1);
	Payload->SetBoolField(TEXT("enable_words"), bEnableWords);

	TArray<TSharedPtr<FJsonValue>> GrammarValues;
	for (const FString& Phrase : BuildGrammarPhraseList())
	{
		GrammarValues.Add(MakeShared<FJsonValueString>(Phrase));
	}
	Payload->SetArrayField(TEXT("grammar_phrases"), GrammarValues);

	FString JsonBody;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonBody);
	FJsonSerializer::Serialize(Payload, Writer);

	bSessionStarting = true;
	UE_LOG(
		LogHttpRuntimeSTTStream,
		Log,
		TEXT("STT stream start: base_url=%s sample_rate=%d use_wake_word=%d wake_focus=%d enable_words=%d"),
		*BaseUrl,
		CaptureSampleRate,
		bUseWakeWord ? 1 : 0,
		bWakeWordFocusMode ? 1 : 0,
		bEnableWords ? 1 : 0
	);
	SendJsonRequest(TEXT("POST"), TEXT("/start_session"), JsonBody, &UHttpRuntimeSTTStreamClient::HandleStartSessionResponse);
	return true;
}

void UHttpRuntimeSTTStreamClient::StopListening()
{
	if (!bListening && !bSessionStarting && SessionId.IsEmpty())
	{
		return;
	}

	if (UWorld* World = GetWorldFromContext())
	{
		World->GetTimerManager().ClearTimer(FlushTimer);
		World->GetTimerManager().ClearTimer(ActiveTimer);
	}

	StopAudioCapture();
	bStartRequested = false;
	bListening = false;
	bSessionStarting = false;
	bPendingFinalRequest = false;

	if (!SessionId.IsEmpty())
	{
		if (bRequestInFlight)
		{
			bPendingStopRequest = true;
		}
		else
		{
			SendStopSessionRequest();
		}
	}
	else
	{
		bPendingStopRequest = false;
	}

	ResetWakeState();
}

bool UHttpRuntimeSTTStreamClient::IsListening() const
{
	return bListening;
}

void UHttpRuntimeSTTStreamClient::EnterStandby()
{
	bActiveListening = false;
	if (bWaitingPostWakeFinal || bHadSpeechSinceActive)
	{
		RequestFinalFromServer();
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
}

void UHttpRuntimeSTTStreamClient::EnterActive()
{
	bActiveListening = true;
	bHadSpeechSinceActive = false;
	if (bUseWakeWord && bWakeWordFocusMode && !bWaitingPostWakeFinal)
	{
		bWaitingPostWakeFinal = true;
		PostWakeBuffer.Empty();
		LastWakeWord.Empty();
	}
	if (bUseWakeWord && (!bDelayActiveTimerUntilTTSFinished || !bTTSActive))
	{
		if (UWorld* World = GetWorldFromContext())
		{
			World->GetTimerManager().SetTimer(ActiveTimer, this, &UHttpRuntimeSTTStreamClient::HandleActiveTimeout, ActiveListenSeconds, false);
		}
	}
}

void UHttpRuntimeSTTStreamClient::NotifyTTSStarted()
{
	bTTSActive = true;
	if (bUseWakeWord)
	{
		EnterStandby();
	}
	if (UWorld* World = GetWorldFromContext())
	{
		World->GetTimerManager().ClearTimer(ActiveTimer);
	}
}

void UHttpRuntimeSTTStreamClient::NotifyTTSEnded()
{
	bTTSActive = false;
	if (bUseWakeWord && bActiveListening)
	{
		EnterActive();
	}
}

FString UHttpRuntimeSTTStreamClient::BuildUrl(const FString& Route) const
{
	const FString TrimmedBase = BaseUrl.EndsWith(TEXT("/")) ? BaseUrl.LeftChop(1) : BaseUrl;
	if (Route.IsEmpty())
	{
		return TrimmedBase;
	}
	return Route.StartsWith(TEXT("/")) ? TrimmedBase + Route : TrimmedBase + TEXT("/") + Route;
}

UWorld* UHttpRuntimeSTTStreamClient::GetWorldFromContext() const
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

void UHttpRuntimeSTTStreamClient::EmitError(const FString& Message)
{
	OnError.Broadcast(Message);
}

void UHttpRuntimeSTTStreamClient::ResetWakeState()
{
	bActiveListening = !bUseWakeWord;
	bTTSActive = false;
	bWaitingPostWakeFinal = false;
	bHadSpeechSinceActive = false;
	LastWakeWord.Empty();
	PostWakeBuffer.Empty();
	LastKeywordTime.Reset();
}

void UHttpRuntimeSTTStreamClient::DrainQueuedAudio()
{
	TArray<uint8> Ignored;
	while (AudioQueue.Dequeue(Ignored))
	{
	}
}

TArray<FString> UHttpRuntimeSTTStreamClient::BuildGrammarPhraseList() const
{
	if (!bUseGrammar)
	{
		return TArray<FString>();
	}

	if (GrammarPhrases.Num() > 0)
	{
		return GrammarPhrases;
	}

	return Keywords;
}

FString UHttpRuntimeSTTStreamClient::ExtractPostWakeText(const FString& Text) const
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
		Remainder.RemoveAt(Pos, LastWakeWord.Len(), EAllowShrinking::No);
		return Remainder.TrimStartAndEnd();
	}

	return Text;
}

void UHttpRuntimeSTTStreamClient::HandleRecognizedText(const FString& Text, bool bFinal)
{
	if (Text.IsEmpty())
	{
		return;
	}

	if (bActiveListening)
	{
		bHadSpeechSinceActive = true;
	}

	if (bUseWakeWord && bWakeWordFocusMode)
	{
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
				UE_LOG(LogHttpRuntimeSTTStream, Log, TEXT("STT Final: %s"), *PostWakeBuffer);
				OnFinal.Broadcast(PostWakeBuffer);
				CheckKeywords(PostWakeBuffer);
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

		if (!bActiveListening)
		{
			if (CheckWakeWords(Text, bFinal))
			{
				return;
			}
			return;
		}
	}

	if (bUseWakeWord && !bActiveListening)
	{
		if (CheckWakeWords(Text, bFinal))
		{
			return;
		}
		return;
	}

	if (bFinal)
	{
		UE_LOG(LogHttpRuntimeSTTStream, Log, TEXT("STT Final: %s"), *Text);
		OnFinal.Broadcast(Text);
		CheckKeywords(Text);
		if (bExtendActiveOnFinal && bUseWakeWord)
		{
			EnterActive();
		}
	}
	else
	{
		UE_LOG(LogHttpRuntimeSTTStream, Log, TEXT("STT Partial: %s"), *Text);
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

void UHttpRuntimeSTTStreamClient::CheckKeywords(const FString& Text)
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
		if (!Lower.Contains(KeyLower))
		{
			continue;
		}

		const double Now = FPlatformTime::Seconds();
		const double* LastTime = LastKeywordTime.Find(KeyLower);
		if (LastTime && (Now - *LastTime) < KeywordCooldownSeconds)
		{
			continue;
		}

		LastKeywordTime.Add(KeyLower, Now);
		UE_LOG(LogHttpRuntimeSTTStream, Log, TEXT("STT Keyword: %s"), *Keyword);
		OnKeywordDetected.Broadcast(Keyword);
	}
}

bool UHttpRuntimeSTTStreamClient::CheckWakeWords(const FString& Text, bool bFinal)
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
		if (Pos == INDEX_NONE)
		{
			continue;
		}

		LastWakeWord = Keyword;
		UE_LOG(LogHttpRuntimeSTTStream, Log, TEXT("STT WakeWord: %s"), *Keyword);
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
			}
			else
			{
				FString Remainder = Text;
				Remainder.RemoveAt(Pos, Keyword.Len(), EAllowShrinking::No);
				Remainder = Remainder.TrimStartAndEnd();
				if (!Remainder.IsEmpty())
				{
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

	return false;
}

void UHttpRuntimeSTTStreamClient::TransitionToActive(const FString& WakeWord)
{
	OnWakeWordDetected.Broadcast(WakeWord);
	EnterActive();
}

void UHttpRuntimeSTTStreamClient::HandleActiveTimeout()
{
	EnterStandby();
}

bool UHttpRuntimeSTTStreamClient::StartAudioCapture()
{
	StopAudioCapture();

	AudioCapture = MakeUnique<Audio::FAudioCapture>();
	if (!AudioCapture.IsValid())
	{
		EmitError(TEXT("Failed to create audio capture."));
		return false;
	}

	Audio::FAudioCaptureDeviceParams Params;
	Params.DeviceIndex = Audio::DefaultDeviceIndex;
	Params.SampleRate = CaptureSampleRate;
	Params.NumInputChannels = CaptureNumChannels;
	Params.PCMAudioEncoding = Audio::EPCMAudioEncoding::FLOATING_POINT_32;

	const Audio::FOnAudioCaptureFunction OnCapture = [this](const void* AudioData, int32 NumFrames, int32 InNumChannels, int32, double, bool)
	{
		if (!bListening)
		{
			return;
		}

		const float* FloatData = static_cast<const float*>(AudioData);
		TArray<uint8> PcmBytes;
		PcmBytes.Reserve(NumFrames * sizeof(int16));

		if (InNumChannels <= 1)
		{
			for (int32 Index = 0; Index < NumFrames; ++Index)
			{
				const float Clamped = FMath::Clamp(FloatData[Index], -1.0f, 1.0f);
				const int16 PCM = static_cast<int16>(Clamped * 32767.0f);
				PcmBytes.Add(static_cast<uint8>(PCM & 0xFF));
				PcmBytes.Add(static_cast<uint8>((PCM >> 8) & 0xFF));
			}
		}
		else
		{
			for (int32 Frame = 0; Frame < NumFrames; ++Frame)
			{
				float Sum = 0.0f;
				for (int32 Channel = 0; Channel < InNumChannels; ++Channel)
				{
					Sum += FloatData[Frame * InNumChannels + Channel];
				}

				const float Mixed = Sum / static_cast<float>(InNumChannels);
				const float Clamped = FMath::Clamp(Mixed, -1.0f, 1.0f);
				const int16 PCM = static_cast<int16>(Clamped * 32767.0f);
				PcmBytes.Add(static_cast<uint8>(PCM & 0xFF));
				PcmBytes.Add(static_cast<uint8>((PCM >> 8) & 0xFF));
			}
		}

		if (PcmBytes.Num() > 0)
		{
			AudioQueue.Enqueue(MoveTemp(PcmBytes));
		}
	};

	if (!AudioCapture->OpenAudioCaptureStream(Params, OnCapture, 1024))
	{
		EmitError(TEXT("OpenAudioCaptureStream failed."));
		AudioCapture.Reset();
		return false;
	}

	AudioCapture->StartStream();

	bCaptureStarted = true;
	return true;
}

void UHttpRuntimeSTTStreamClient::StopAudioCapture()
{
	if (AudioCapture.IsValid())
	{
		AudioCapture->StopStream();
		AudioCapture->CloseStream();
		AudioCapture.Reset();
	}
	bCaptureStarted = false;
	DrainQueuedAudio();
}

void UHttpRuntimeSTTStreamClient::FlushAudioQueue()
{
	if (!bListening || SessionId.IsEmpty() || bRequestInFlight)
	{
		return;
	}

	TArray<uint8> CombinedAudio;
	TArray<uint8> Chunk;
	while (AudioQueue.Dequeue(Chunk))
	{
		CombinedAudio.Append(Chunk);
	}

	if (CombinedAudio.Num() <= 0)
	{
		return;
	}

	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("session_id"), SessionId);
	Payload->SetStringField(TEXT("audio_base64"), FBase64::Encode(CombinedAudio));

	FString JsonBody;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonBody);
	FJsonSerializer::Serialize(Payload, Writer);

	bRequestInFlight = true;
	SendJsonRequest(TEXT("POST"), TEXT("/append_audio"), JsonBody, &UHttpRuntimeSTTStreamClient::HandleAppendAudioResponse);
}

void UHttpRuntimeSTTStreamClient::RequestFinalFromServer()
{
	if (SessionId.IsEmpty())
	{
		return;
	}

	if (bRequestInFlight)
	{
		bPendingFinalRequest = true;
		return;
	}

	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("session_id"), SessionId);

	FString JsonBody;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonBody);
	FJsonSerializer::Serialize(Payload, Writer);

	bRequestInFlight = true;
	SendJsonRequest(TEXT("POST"), TEXT("/request_final"), JsonBody, &UHttpRuntimeSTTStreamClient::HandleFinalResponse);
}

void UHttpRuntimeSTTStreamClient::SendStopSessionRequest()
{
	if (SessionId.IsEmpty())
	{
		return;
	}

	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("session_id"), SessionId);

	FString JsonBody;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonBody);
	FJsonSerializer::Serialize(Payload, Writer);

	bRequestInFlight = true;
	bPendingStopRequest = false;
	SendJsonRequest(TEXT("POST"), TEXT("/stop_session"), JsonBody, &UHttpRuntimeSTTStreamClient::HandleStopSessionResponse);
}

void UHttpRuntimeSTTStreamClient::SendJsonRequest(const FString& Method, const FString& Route, const FString& JsonBody, void (UHttpRuntimeSTTStreamClient::*Handler)(FHttpRequestPtr, FHttpResponsePtr, bool, FString))
{
	const FString Url = BuildUrl(Route);
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(Url);
	Request->SetVerb(Method);
	Request->SetHeader(TEXT("Accept"), TEXT("application/json"));
	Request->SetHeader(TEXT("Content-Type"), TEXT("application/json; charset=utf-8"));
	if (!JsonBody.IsEmpty() && Method != TEXT("GET"))
	{
		Request->SetContentAsString(JsonBody);
	}
	Request->OnProcessRequestComplete().BindUObject(this, Handler, Url);
	if (!Request->ProcessRequest())
	{
		const FString ErrorMessage = FString::Printf(TEXT("Failed to process %s %s"), *Method, *Url);
		if (Route == TEXT("/start_session"))
		{
			bSessionStarting = false;
		}
		if (Route == TEXT("/append_audio") || Route == TEXT("/request_final") || Route == TEXT("/stop_session"))
		{
			bRequestInFlight = false;
		}
		OnRequestFailed.Broadcast(Method, Url, ErrorMessage);
		EmitError(ErrorMessage);
	}
}

void UHttpRuntimeSTTStreamClient::HandleSimpleResponse(FHttpRequestPtr, FHttpResponsePtr Response, bool bWasSuccessful, FString Url)
{
	const FString Method = TEXT("GET");
	if (!bWasSuccessful || !Response.IsValid())
	{
		OnRequestFailed.Broadcast(Method, Url, TEXT("Request failed or server unavailable."));
		return;
	}

	const int32 StatusCode = Response->GetResponseCode();
	const FString ResponseBody = Response->GetContentAsString();
	if (EHttpResponseCodes::IsOk(StatusCode))
	{
		OnRequestSucceeded.Broadcast(Method, Url, StatusCode, ResponseBody);
		return;
	}

	OnRequestFailed.Broadcast(Method, Url, FString::Printf(TEXT("HTTP %d: %s"), StatusCode, *ResponseBody));
}

void UHttpRuntimeSTTStreamClient::HandleStartSessionResponse(FHttpRequestPtr, FHttpResponsePtr Response, bool bWasSuccessful, FString Url)
{
	bSessionStarting = false;
	if (!bWasSuccessful || !Response.IsValid())
	{
		const FString ErrorMessage = TEXT("Failed to start STT streaming session.");
		OnRequestFailed.Broadcast(TEXT("POST"), Url, ErrorMessage);
		EmitError(ErrorMessage);
		return;
	}

	const int32 StatusCode = Response->GetResponseCode();
	const FString ResponseBody = Response->GetContentAsString();
	if (!EHttpResponseCodes::IsOk(StatusCode))
	{
		const FString ErrorMessage = FString::Printf(TEXT("HTTP %d: %s"), StatusCode, *ResponseBody);
		OnRequestFailed.Broadcast(TEXT("POST"), Url, ErrorMessage);
		EmitError(ErrorMessage);
		return;
	}

	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResponseBody);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		EmitError(TEXT("Invalid STT session response JSON."));
		return;
	}

	Root->TryGetStringField(TEXT("session_id"), SessionId);
	if (SessionId.IsEmpty())
	{
		EmitError(TEXT("STT session_id is missing."));
		return;
	}

	if (!bStartRequested)
	{
		SendStopSessionRequest();
		return;
	}

	double ServerSampleRate = static_cast<double>(CaptureSampleRate);
	if (Root->TryGetNumberField(TEXT("sample_rate"), ServerSampleRate))
	{
		CaptureSampleRate = static_cast<int32>(ServerSampleRate);
	}
	OnRequestSucceeded.Broadcast(TEXT("POST"), Url, StatusCode, ResponseBody);

	if (!StartAudioCapture())
	{
		SendStopSessionRequest();
		return;
	}

	bListening = true;
	if (bUseWakeWord)
	{
		EnterStandby();
	}
	else
	{
		EnterActive();
	}

	if (UWorld* World = GetWorldFromContext())
	{
		World->GetTimerManager().SetTimer(FlushTimer, this, &UHttpRuntimeSTTStreamClient::FlushAudioQueue, FlushIntervalSeconds, true);
	}
}

void UHttpRuntimeSTTStreamClient::HandleAppendAudioResponse(FHttpRequestPtr, FHttpResponsePtr Response, bool bWasSuccessful, FString Url)
{
	bRequestInFlight = false;
	if (!bWasSuccessful || !Response.IsValid())
	{
		const FString ErrorMessage = TEXT("STT append_audio request failed or server unavailable.");
		OnRequestFailed.Broadcast(TEXT("POST"), Url, ErrorMessage);
		EmitError(ErrorMessage);
		ContinuePendingRequests();
		return;
	}

	const int32 StatusCode = Response->GetResponseCode();
	const FString ResponseBody = Response->GetContentAsString();
	if (!EHttpResponseCodes::IsOk(StatusCode))
	{
		const FString ErrorMessage = FString::Printf(TEXT("HTTP %d: %s"), StatusCode, *ResponseBody);
		OnRequestFailed.Broadcast(TEXT("POST"), Url, ErrorMessage);
		EmitError(ErrorMessage);
		ContinuePendingRequests();
		return;
	}

	OnRequestSucceeded.Broadcast(TEXT("POST"), Url, StatusCode, ResponseBody);
	FString ErrorMessage;
	if (!ProcessSessionEventResponse(Response, ErrorMessage))
	{
		EmitError(ErrorMessage);
	}
	ContinuePendingRequests();
}

void UHttpRuntimeSTTStreamClient::HandleFinalResponse(FHttpRequestPtr, FHttpResponsePtr Response, bool bWasSuccessful, FString Url)
{
	bRequestInFlight = false;
	bPendingFinalRequest = false;

	if (!bWasSuccessful || !Response.IsValid())
	{
		const FString ErrorMessage = TEXT("STT request_final failed or server unavailable.");
		OnRequestFailed.Broadcast(TEXT("POST"), Url, ErrorMessage);
		EmitError(ErrorMessage);
		ContinuePendingRequests();
		return;
	}

	const int32 StatusCode = Response->GetResponseCode();
	const FString ResponseBody = Response->GetContentAsString();
	if (!EHttpResponseCodes::IsOk(StatusCode))
	{
		const FString ErrorMessage = FString::Printf(TEXT("HTTP %d: %s"), StatusCode, *ResponseBody);
		OnRequestFailed.Broadcast(TEXT("POST"), Url, ErrorMessage);
		EmitError(ErrorMessage);
		ContinuePendingRequests();
		return;
	}

	OnRequestSucceeded.Broadcast(TEXT("POST"), Url, StatusCode, ResponseBody);
	FString ErrorMessage;
	if (!ProcessSessionEventResponse(Response, ErrorMessage))
	{
		EmitError(ErrorMessage);
	}
	ContinuePendingRequests();
}

void UHttpRuntimeSTTStreamClient::HandleStopSessionResponse(FHttpRequestPtr, FHttpResponsePtr Response, bool bWasSuccessful, FString Url)
{
	bRequestInFlight = false;
	const FString ClosedSessionId = SessionId;
	SessionId.Empty();

	if (!bWasSuccessful || !Response.IsValid())
	{
		const FString ErrorMessage = TEXT("STT stop_session failed or server unavailable.");
		OnRequestFailed.Broadcast(TEXT("POST"), Url, ErrorMessage);
		EmitError(ErrorMessage);
		return;
	}

	const int32 StatusCode = Response->GetResponseCode();
	const FString ResponseBody = Response->GetContentAsString();
	if (!EHttpResponseCodes::IsOk(StatusCode))
	{
		const FString ErrorMessage = FString::Printf(TEXT("HTTP %d: %s"), StatusCode, *ResponseBody);
		OnRequestFailed.Broadcast(TEXT("POST"), Url, ErrorMessage);
		EmitError(ErrorMessage);
		return;
	}

	OnRequestSucceeded.Broadcast(TEXT("POST"), Url, StatusCode, ResponseBody);
	FString ErrorMessage;
	if (!ProcessSessionEventResponse(Response, ErrorMessage))
	{
		EmitError(ErrorMessage);
	}

	if (bLogDebug && !ClosedSessionId.IsEmpty())
	{
		UE_LOG(LogHttpRuntimeSTTStream, Verbose, TEXT("STT stream session closed: %s"), *ClosedSessionId);
	}
}

bool UHttpRuntimeSTTStreamClient::ProcessSessionEventResponse(FHttpResponsePtr Response, FString& OutErrorMessage)
{
	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Response->GetContentAsString());
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		OutErrorMessage = TEXT("Invalid STT event response JSON.");
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* Events = nullptr;
	if (!Root->TryGetArrayField(TEXT("events"), Events) || !Events)
	{
		return true;
	}

	for (const TSharedPtr<FJsonValue>& EventValue : *Events)
	{
		const TSharedPtr<FJsonObject> EventObject = EventValue.IsValid() ? EventValue->AsObject() : nullptr;
		if (!EventObject.IsValid())
		{
			continue;
		}

		FString Type;
		FString Text;
		EventObject->TryGetStringField(TEXT("type"), Type);
		EventObject->TryGetStringField(TEXT("text"), Text);
		HandleRecognizedText(Text, Type.Equals(TEXT("final"), ESearchCase::IgnoreCase));
	}

	return true;
}

void UHttpRuntimeSTTStreamClient::ContinuePendingRequests()
{
	if (!SessionId.IsEmpty() && bPendingStopRequest)
	{
		SendStopSessionRequest();
		return;
	}

	if (!SessionId.IsEmpty() && bPendingFinalRequest)
	{
		RequestFinalFromServer();
		return;
	}

	FlushAudioQueue();
}
