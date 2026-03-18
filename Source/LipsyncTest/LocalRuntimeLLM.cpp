// Fill out your copyright notice in the Description page of Project Settings.

#include "LocalRuntimeLLM.h"

#include "Dom/JsonObject.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "JsonObjectConverter.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "TimerManager.h"
#include "Engine/Engine.h"

ULocalRuntimeLLM::ULocalRuntimeLLM()
{
	// Default project-relative paths (adjust if you use different filenames).
	ServerExePath = TEXT("Llama/llama-server.exe");
	ModelPath = TEXT("Models/qwen2.5-3b-instruct-q4_k_m.gguf");
	bServerStartedNotified = false;

	BaseSystemPrompt =
		TEXT("You are an in-game assistant. Always reply in valid JSON only.\n")
		TEXT("The JSON MUST contain a non-empty string field \"reply\".\n")
		TEXT("If you want to trigger events, include \"events\": [{\"name\":\"event_name\",\"args\":{...}}].\n")
		TEXT("If no events are needed, set \"events\" to an empty array.\n")
		TEXT("Never return empty JSON. Never include extra text outside JSON.");
}

ULocalRuntimeLLM* ULocalRuntimeLLM::CreateRuntimeLLM(UObject* WorldContextObject)
{
	ULocalRuntimeLLM* Obj = NewObject<ULocalRuntimeLLM>();
	Obj->WorldContextObject = WorldContextObject;
	return Obj;
}

void ULocalRuntimeLLM::BeginDestroy()
{
	StopServer();
	Super::BeginDestroy();
}

UWorld* ULocalRuntimeLLM::GetWorldFromContext() const
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

FString ULocalRuntimeLLM::ResolvePath(const FString& Path) const
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

FString ULocalRuntimeLLM::GetBaseUrl() const
{
	return FString::Printf(TEXT("http://%s:%d"), *Host, Port);
}

bool ULocalRuntimeLLM::StartServer()
{
	if (ServerProcess.IsValid())
	{
		return true;
	}
	bServerStartedNotified = false;

	const FString ExePath = ResolvePath(ServerExePath);
	const FString ModelFullPath = ResolvePath(ModelPath);

	if (ExePath.IsEmpty() || !FPaths::FileExists(ExePath))
	{
		EmitError(TEXT("ServerExePath is missing or invalid."));
		return false;
	}

	if (ModelFullPath.IsEmpty() || !FPaths::FileExists(ModelFullPath))
	{
		EmitError(TEXT("ModelPath is missing or invalid."));
		return false;
	}

	const FString Args = FString::Printf(
		TEXT("-m \"%s\" --host %s --port %d -c %d -ngl %d --log-disable"),
		*ModelFullPath,
		*Host,
		Port,
		ContextSize,
		GpuLayers
	);
	UE_LOG(LogTemp, Warning, TEXT("LLM SERVER EXE: %s"), *ExePath);
	UE_LOG(LogTemp, Warning, TEXT("LLM SERVER MODEL: %s"), *ModelFullPath);
	UE_LOG(LogTemp, Warning, TEXT("LLM SERVER ARGS: %s"), *Args);

	const FString WorkingDir = FPaths::GetPath(ExePath);
	ServerProcess = FPlatformProcess::CreateProc(
		*ExePath,
		*Args,
		true,
		true,
		true,
		nullptr,
		0,
		*WorkingDir,
		nullptr
	);

	if (!ServerProcess.IsValid())
	{
		EmitError(TEXT("Failed to start local LLM server process."));
		return false;
	}
	UE_LOG(LogTemp, Warning, TEXT("LLM SERVER: Started (waiting for health...)"));

	return true;
}

void ULocalRuntimeLLM::StopServer()
{
	if (!ServerProcess.IsValid())
	{
		return;
	}

	UE_LOG(LogTemp, Warning, TEXT("LLM SERVER: Stopping"));
	FPlatformProcess::TerminateProc(ServerProcess, true);
	FPlatformProcess::CloseProc(ServerProcess);
	ServerProcess.Reset();
	bServerStartedNotified = false;
}

bool ULocalRuntimeLLM::IsServerRunning()
{
	if (!ServerProcess.IsValid())
	{
		return false;
	}

	return FPlatformProcess::IsProcRunning(ServerProcess);
}

void ULocalRuntimeLLM::SendPrompt(const FString& Prompt, const FString& SystemPrompt)
{
	if (Prompt.IsEmpty())
	{
		EmitError(TEXT("Prompt is empty."));
		return;
	}

	if (bRequireMeaningfulInput && !IsMeaningfulInput(Prompt))
	{
		EmitError(TEXT("Prompt is too short or not meaningful."));
		return;
	}

	const FString Url = bUseChatCompletions
		? FString::Printf(TEXT("%s/v1/chat/completions"), *GetBaseUrl())
		: FString::Printf(TEXT("%s/v1/completions"), *GetBaseUrl());

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("model"), TEXT("local"));
	Root->SetNumberField(TEXT("temperature"), Temperature);
	if (bStreamResponses)
	{
		Root->SetBoolField(TEXT("stream"), true);
	}

	const FString BuiltSystemPrompt = BuildSystemPrompt(SystemPrompt);

	if (bUseChatCompletions)
	{
		if (bKeepContext)
		{
			if (MessageHistory.Num() == 0 && !BuiltSystemPrompt.IsEmpty())
			{
				AddMessage(TEXT("system"), BuiltSystemPrompt);
			}
			AddMessage(TEXT("user"), Prompt);
			TrimHistory();
			Root->SetArrayField(TEXT("messages"), MessageHistory);
		}
		else
		{
			TArray<TSharedPtr<FJsonValue>> Messages;
			if (!BuiltSystemPrompt.IsEmpty())
			{
				TSharedPtr<FJsonObject> SystemMsg = MakeShared<FJsonObject>();
				SystemMsg->SetStringField(TEXT("role"), TEXT("system"));
				SystemMsg->SetStringField(TEXT("content"), BuiltSystemPrompt);
				Messages.Add(MakeShared<FJsonValueObject>(SystemMsg));
			}

			TSharedPtr<FJsonObject> UserMsg = MakeShared<FJsonObject>();
			UserMsg->SetStringField(TEXT("role"), TEXT("user"));
			UserMsg->SetStringField(TEXT("content"), Prompt);
			Messages.Add(MakeShared<FJsonValueObject>(UserMsg));
			Root->SetArrayField(TEXT("messages"), Messages);
		}
	}
	else
	{
		Root->SetStringField(TEXT("prompt"), Prompt);
	}

	FString Body;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Body);
	FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);

	UE_LOG(LogTemp, Warning, TEXT("LLM REQUEST URL: %s"), *Url);
	UE_LOG(LogTemp, Warning, TEXT("LLM REQUEST BODY: %s"), *Body);

	if (bAutoStartServer && !IsServerRunning())
	{
		if (!StartServer())
		{
			return;
		}
	}

	if (bWaitForServer)
	{
		UE_LOG(LogTemp, Warning, TEXT("LLM: Waiting for server health before request..."));
		CheckServerHealthAndPost(Url, Body, Prompt, BuiltSystemPrompt, FPlatformTime::Seconds(), false);
	}
	else
	{
		PostJson(Url, Body, Prompt, BuiltSystemPrompt, 0);
	}
}

void ULocalRuntimeLLM::ClearContext()
{
	MessageHistory.Reset();
}

FString ULocalRuntimeLLM::BuildSystemPrompt(const FString& RuntimeSystemPrompt) const
{
	FString SystemPrompt = BaseSystemPrompt;
	if (!RuntimeSystemPrompt.IsEmpty())
	{
		if (!SystemPrompt.IsEmpty())
		{
			SystemPrompt += TEXT("\n\n");
		}
		SystemPrompt += RuntimeSystemPrompt;
	}

	if (bForceRussian)
	{
		if (!SystemPrompt.IsEmpty())
		{
			SystemPrompt += TEXT("\n\n");
		}
		SystemPrompt += RussianInstruction;
	}

	if (bExpectStructuredResponse)
	{
		if (!SystemPrompt.IsEmpty())
		{
			SystemPrompt += TEXT("\n\n");
		}

		SystemPrompt += TEXT("Return ONLY valid JSON with this shape:\n");
		SystemPrompt += TEXT("{\"reply\":\"string\",\"events\":[{\"name\":\"event_name\",\"args\":{...}}]}\n");
		SystemPrompt += TEXT("If no events, return an empty array. Do not include any extra text.");
	}

	if (AllowedEvents.Num() > 0)
	{
		SystemPrompt += TEXT("\n\nAllowed blueprint events you can request:\n");
		for (const FRuntimeLLMEventSpec& Spec : AllowedEvents)
		{
			SystemPrompt += FString::Printf(TEXT("- %s: %s\n"), *Spec.EventName, *Spec.Description);
		}
	}

	return SystemPrompt;
}

void ULocalRuntimeLLM::EmitError(const FString& Message) const
{
	ULocalRuntimeLLM* MutableThis = const_cast<ULocalRuntimeLLM*>(this);
	MutableThis->OnError.Broadcast(Message);
}

void ULocalRuntimeLLM::PostJson(const FString& Url, const FString& Body, const FString& Prompt, const FString& SystemPrompt, int32 Attempt)
{
	UE_LOG(LogTemp, Warning, TEXT("LLM HTTP: POST attempt=%d url=%s"), Attempt + 1, *Url);
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(Url);
	Request->SetVerb(TEXT("POST"));
	Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	if (bStreamResponses)
	{
		Request->SetHeader(TEXT("Accept"), TEXT("text/event-stream"));
	}
	Request->SetContentAsString(Body);

	struct FStreamState
	{
		int32 ProcessedLen = 0;
		FString Buffer;
		FString Accumulated;
		FString TextBuffer;
		FString PrefixBuffer;
		bool bInReply = false;
		bool bEscape = false;
	};
	TSharedRef<FStreamState, ESPMode::ThreadSafe> StreamState = MakeShared<FStreamState, ESPMode::ThreadSafe>();

	if (bStreamResponses)
	{
		Request->OnRequestProgress64().BindLambda([this, StreamState](FHttpRequestPtr Req, int32 BytesSent, int32 BytesReceived)
		{
			FHttpResponsePtr Resp = Req.IsValid() ? Req->GetResponse() : nullptr;
			if (!Resp.IsValid())
			{
				return;
			}

			const FString Full = Resp->GetContentAsString();
			if (Full.Len() <= StreamState->ProcessedLen)
			{
				return;
			}

			const FString NewChunk = Full.Mid(StreamState->ProcessedLen);
			StreamState->ProcessedLen = Full.Len();
			StreamState->Buffer += NewChunk;

			int32 LineEndIdx = INDEX_NONE;
			while (StreamState->Buffer.FindChar(TEXT('\n'), LineEndIdx))
			{
				FString Line = StreamState->Buffer.Left(LineEndIdx);
				StreamState->Buffer = StreamState->Buffer.Mid(LineEndIdx + 1);
				Line.TrimStartAndEndInline();
				if (!Line.StartsWith(TEXT("data:")))
				{
					continue;
				}

				FString Data = Line.Mid(5);
				Data.TrimStartAndEndInline();
				if (Data == TEXT("[DONE]"))
				{
					return;
				}

				const FString Delta = ExtractDeltaFromJson(Data);
				if (!Delta.IsEmpty())
				{
					StreamState->Accumulated += Delta;

					const FString Filtered = FilterReplyDelta(Delta, bStreamStripJson, StreamState->PrefixBuffer, StreamState->bInReply, StreamState->bEscape);
					if (!Filtered.IsEmpty())
					{
						EmitStreamChunk(Filtered, StreamState->TextBuffer);
					}
				}
			}
		});
	}

	Request->OnProcessRequestComplete().BindLambda([this, Prompt, SystemPrompt, StreamState, Url, Body, Attempt](FHttpRequestPtr Req, FHttpResponsePtr Resp, bool bSucceeded)
	{
		if (!bSucceeded || !Resp.IsValid())
		{
			UE_LOG(LogTemp, Warning, TEXT("LLM HTTP: request failed (no response)."));
			if (bRetryOn503 && Attempt < MaxRetries)
			{
				SchedulePostRetry(Url, Body, Prompt, SystemPrompt, Attempt + 1);
				return;
			}
			EmitError(TEXT("HTTP request failed."));
			return;
		}

		const int32 Code = Resp->GetResponseCode();
		const FString ResponseText = Resp->GetContentAsString();
		UE_LOG(LogTemp, Warning, TEXT("LLM HTTP: status=%d bytes=%d"), Code, ResponseText.Len());

		if (Code == 503 && bRetryOn503 && Attempt < MaxRetries)
		{
			UE_LOG(LogTemp, Warning, TEXT("LLM HTTP: 503 retrying in %.2fs (attempt %d/%d)"), RetryDelaySeconds, Attempt + 1, MaxRetries);
			if (bLogResponseBodyOnError)
			{
				UE_LOG(LogTemp, Warning, TEXT("LLM RESPONSE BODY (503): %s"), *ResponseText);
			}
			if (bWaitForServer)
			{
				UE_LOG(LogTemp, Warning, TEXT("LLM HTTP: 503 -> re-checking server health before retry"));
				CheckServerHealthAndPost(Url, Body, Prompt, SystemPrompt, FPlatformTime::Seconds(), false);
			}
			else
			{
				SchedulePostRetry(Url, Body, Prompt, SystemPrompt, Attempt + 1);
			}
			return;
		}

		if (Code < 200 || Code >= 300)
		{
			if (bLogResponseBodyOnError)
			{
				UE_LOG(LogTemp, Warning, TEXT("LLM RESPONSE BODY (error): %s"), *ResponseText);
			}
			EmitError(FString::Printf(TEXT("HTTP error: %d"), Code));
			return;
		}

		UE_LOG(LogTemp, Warning, TEXT("LLM RESPONSE BODY: %s"), *ResponseText);

		FString Content;
		if (bStreamResponses)
		{
			Content = StreamState->Accumulated;

			if (!StreamState->TextBuffer.IsEmpty())
			{
				const FString Remainder = StreamState->TextBuffer;
				StreamState->TextBuffer.Reset();
				EmitStreamChunk(Remainder, StreamState->TextBuffer);
			}
		}
		else
		{
			TSharedPtr<FJsonObject> Json;
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResponseText);

			if (!FJsonSerializer::Deserialize(Reader, Json) || !Json.IsValid())
			{
				EmitError(TEXT("Failed to parse JSON response."));
				return;
			}

			if (Json->HasTypedField<EJson::Array>(TEXT("choices")))
			{
				const TArray<TSharedPtr<FJsonValue>> Choices = Json->GetArrayField(TEXT("choices"));
				if (Choices.Num() > 0 && Choices[0]->Type == EJson::Object)
				{
					const TSharedPtr<FJsonObject> ChoiceObj = Choices[0]->AsObject();
					if (ChoiceObj.IsValid())
					{
						if (ChoiceObj->HasTypedField<EJson::Object>(TEXT("message")))
						{
							const TSharedPtr<FJsonObject> Message = ChoiceObj->GetObjectField(TEXT("message"));
							if (Message.IsValid())
							{
								Content = Message->GetStringField(TEXT("content"));
							}
						}
						else if (ChoiceObj->HasTypedField<EJson::String>(TEXT("text")))
						{
							Content = ChoiceObj->GetStringField(TEXT("text"));
						}
					}
				}
			}
		}

		if (Content.IsEmpty())
		{
			EmitError(TEXT("Response missing content."));
			return;
		}

		if (bExpectStructuredResponse)
		{
			FString Reply;
			TArray<FRuntimeLLMEventCall> Events;
			if (TryParseStructuredResponse(Content, Reply, Events))
			{
				if (bAutoInvokeEvents)
				{
					for (const FRuntimeLLMEventCall& EventCall : Events)
					{
						TryInvokeEvent(EventCall);
					}
				}

				OnEvents.Broadcast(Reply, Events);
				OnResponse.Broadcast(Prompt, Reply);
			}
			else
			{
				EmitError(TEXT("Structured response parsing failed."));
			}
		}
		else
		{
			OnResponse.Broadcast(Prompt, Content);
		}

		if (bUseChatCompletions && bKeepContext)
		{
			AddMessage(TEXT("assistant"), Content);
			TrimHistory();
		}
	});

	Request->ProcessRequest();
}

void ULocalRuntimeLLM::SchedulePostRetry(const FString& Url, const FString& Body, const FString& Prompt, const FString& SystemPrompt, int32 Attempt)
{
	UE_LOG(LogTemp, Warning, TEXT("LLM: Retry attempt %d/%d in %.2fs"), Attempt, MaxRetries, RetryDelaySeconds);

	if (UWorld* World = GetWorldFromContext())
	{
		FTimerHandle TimerHandle;
		World->GetTimerManager().SetTimer(TimerHandle, [this, Url, Body, Prompt, SystemPrompt, Attempt]()
		{
			PostJson(Url, Body, Prompt, SystemPrompt, Attempt);
		}, RetryDelaySeconds, false);
	}
	else
	{
		PostJson(Url, Body, Prompt, SystemPrompt, Attempt);
	}
}

void ULocalRuntimeLLM::CheckServerHealthAndPost(const FString& Url, const FString& Body, const FString& Prompt, const FString& SystemPrompt, double StartTime, bool bTriedFallback)
{
	const double Elapsed = FPlatformTime::Seconds() - StartTime;
	if (Elapsed > HealthTimeoutSeconds)
	{
		EmitError(TEXT("LLM server health timed out."));
		return;
	}

	const FString EndpointToUse = (bUseHealthFallback && bTriedFallback) ? FallbackHealthEndpoint : HealthEndpoint;
	const FString HealthUrl = FString::Printf(TEXT("%s%s"), *GetBaseUrl(), *EndpointToUse);
	UE_LOG(LogTemp, Warning, TEXT("LLM HEALTH: GET %s (elapsed=%.2fs)"), *HealthUrl, Elapsed);

	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(HealthUrl);
	Request->SetVerb(TEXT("GET"));
	Request->OnProcessRequestComplete().BindLambda([this, Url, Body, Prompt, SystemPrompt, StartTime, bTriedFallback](FHttpRequestPtr Req, FHttpResponsePtr Resp, bool bSucceeded)
	{
		if (bSucceeded && Resp.IsValid() && Resp->GetResponseCode() >= 200 && Resp->GetResponseCode() < 300)
		{
			UE_LOG(LogTemp, Warning, TEXT("LLM HEALTH: OK (%d) after %.2fs"), Resp->GetResponseCode(), FPlatformTime::Seconds() - StartTime);
			if (!bServerStartedNotified)
			{
				bServerStartedNotified = true;
				UE_LOG(LogTemp, Warning, TEXT("LLM SERVER: Ready"));
				OnServerStarted.Broadcast();
			}
			PostJson(Url, Body, Prompt, SystemPrompt, 0);
		}
		else
		{
			const int32 Code = Resp.IsValid() ? Resp->GetResponseCode() : 0;
			const double ElapsedNow = FPlatformTime::Seconds() - StartTime;

			if (bUseHealthFallback && !bTriedFallback && HealthEndpoint != FallbackHealthEndpoint)
			{
				UE_LOG(LogTemp, Warning, TEXT("LLM HEALTH: fallback to %s (status=%d, elapsed=%.2fs)"), *FallbackHealthEndpoint, Code, ElapsedNow);
				CheckServerHealthAndPost(Url, Body, Prompt, SystemPrompt, StartTime, true);
				return;
			}

			UE_LOG(LogTemp, Warning, TEXT("LLM HEALTH: not ready (status=%d, elapsed=%.2fs). Retrying..."), Code, ElapsedNow);
			if (UWorld* World = GetWorldFromContext())
			{
				FTimerHandle TimerHandle;
				World->GetTimerManager().SetTimer(TimerHandle, [this, Url, Body, Prompt, SystemPrompt, StartTime, bTriedFallback]()
				{
					CheckServerHealthAndPost(Url, Body, Prompt, SystemPrompt, StartTime, bTriedFallback);
				}, HealthPollInterval, false);
			}
			else
			{
				CheckServerHealthAndPost(Url, Body, Prompt, SystemPrompt, StartTime, bTriedFallback);
			}
		}
	});
	Request->ProcessRequest();
}

void ULocalRuntimeLLM::AddMessage(const FString& Role, const FString& Content)
{
	TSharedPtr<FJsonObject> Msg = MakeShared<FJsonObject>();
	Msg->SetStringField(TEXT("role"), Role);
	Msg->SetStringField(TEXT("content"), Content);
	MessageHistory.Add(MakeShared<FJsonValueObject>(Msg));
}

void ULocalRuntimeLLM::TrimHistory()
{
	if (MaxTurns <= 0)
	{
		return;
	}

	int32 MaxMessages = MaxTurns * 2 + 1;
	while (MessageHistory.Num() > MaxMessages)
	{
		MessageHistory.RemoveAt(1);
	}
}

bool ULocalRuntimeLLM::TryParseStructuredResponse(const FString& ResponseText, FString& OutReply, TArray<FRuntimeLLMEventCall>& OutEvents) const
{
	TSharedPtr<FJsonObject> Root;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResponseText);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		OutReply = ResponseText;
		OutEvents.Reset();
		return true;
	}

	OutReply.Reset();
	OutEvents.Reset();

	const TCHAR* ReplyKeys[] = { TEXT("reply"), TEXT("response"), TEXT("text"), TEXT("content") };
	for (const TCHAR* Key : ReplyKeys)
	{
		FString Value;
		if (Root->TryGetStringField(Key, Value) && !Value.IsEmpty())
		{
			OutReply = Value;
			break;
		}
	}

	if (Root->HasTypedField<EJson::Array>(TEXT("events")))
	{
		const TArray<TSharedPtr<FJsonValue>>& Events = Root->GetArrayField(TEXT("events"));
		for (const TSharedPtr<FJsonValue>& Val : Events)
		{
			if (!Val.IsValid() || Val->Type != EJson::Object)
			{
				continue;
			}
			const TSharedPtr<FJsonObject> Obj = Val->AsObject();
			if (!Obj.IsValid())
			{
				continue;
			}

			FRuntimeLLMEventCall EventCall;
			EventCall.Name = Obj->GetStringField(TEXT("name"));

			if (Obj->HasTypedField<EJson::Object>(TEXT("args")))
			{
				FString ArgsJson;
				TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ArgsJson);
				FJsonSerializer::Serialize(Obj->GetObjectField(TEXT("args")).ToSharedRef(), Writer);
				EventCall.ArgsJson = ArgsJson;
			}

			if (!EventCall.Name.IsEmpty())
			{
				OutEvents.Add(EventCall);
			}
		}
	}

	if (OutReply.IsEmpty() && OutEvents.Num() == 0)
	{
		OutReply = ResponseText;
	}

	return true;
}

void ULocalRuntimeLLM::TryInvokeEvent(const FRuntimeLLMEventCall& EventCall) const
{
	if (EventCall.Name.IsEmpty())
	{
		return;
	}

	if (!EventTarget)
	{
		return;
	}

	if (bRestrictToAllowedEvents && !IsEventAllowed(EventCall.Name))
	{
		EmitError(FString::Printf(TEXT("Event not allowed: %s"), *EventCall.Name));
		return;
	}

	FName EventName = *EventCall.Name;
	if (bCaseInsensitiveEventMatch)
	{
		for (UFunction* Func : TFieldRange<UFunction>(EventTarget->GetClass()))
		{
			if (Func && Func->GetFName().ToString().Equals(EventCall.Name, ESearchCase::IgnoreCase))
			{
				EventName = Func->GetFName();
				break;
			}
		}
	}

	UFunction* Func = EventTarget->FindFunction(EventName);
	if (!Func)
	{
		EmitError(FString::Printf(TEXT("Event not found: %s"), *EventCall.Name));
		return;
	}

	if (Func->NumParms != 0)
	{
		EmitError(FString::Printf(TEXT("Event has parameters (not supported): %s"), *EventCall.Name));
		return;
	}

	EventTarget->ProcessEvent(Func, nullptr);
}

bool ULocalRuntimeLLM::IsMeaningfulInput(const FString& Prompt) const
{
	if (Prompt.Len() < MinInputChars)
	{
		return false;
	}

	return CountWords(Prompt) >= MinInputWords;
}

FString ULocalRuntimeLLM::ExtractDeltaFromJson(const FString& JsonText) const
{
	TSharedPtr<FJsonObject> Root;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		return FString();
	}

	const TArray<TSharedPtr<FJsonValue>>* Choices = nullptr;
	if (!Root->TryGetArrayField(TEXT("choices"), Choices) || !Choices || Choices->Num() == 0)
	{
		return FString();
	}

	const TSharedPtr<FJsonObject> Choice = (*Choices)[0]->AsObject();
	if (!Choice.IsValid())
	{
		return FString();
	}

	const TSharedPtr<FJsonObject> DeltaObj = Choice->GetObjectField(TEXT("delta"));
	if (DeltaObj.IsValid())
	{
		FString Delta;
		if (DeltaObj->TryGetStringField(TEXT("content"), Delta))
		{
			return Delta;
		}
	}

	FString Text;
	if (Choice->TryGetStringField(TEXT("text"), Text))
	{
		return Text;
	}

	return FString();
}

FString ULocalRuntimeLLM::FilterReplyDelta(const FString& Delta, bool bStripJson, FString& PrefixBuffer, bool& bInReply, bool& bEscape) const
{
	if (!bStripJson)
	{
		return Delta;
	}

	FString Out;
	for (int32 i = 0; i < Delta.Len(); ++i)
	{
		TCHAR Ch = Delta[i];
		if (!bInReply)
		{
			PrefixBuffer += Ch;
			if (PrefixBuffer.EndsWith(TEXT("\"reply\":\"")))
			{
				bInReply = true;
				PrefixBuffer.Empty();
			}
			continue;
		}

		if (bEscape)
		{
			Out += Ch;
			bEscape = false;
			continue;
		}

		if (Ch == TEXT('\\'))
		{
			bEscape = true;
			continue;
		}

		if (Ch == TEXT('"'))
		{
			bInReply = false;
			continue;
		}

		Out += Ch;
	}

	return Out;
}

void ULocalRuntimeLLM::EmitStreamChunk(const FString& Delta, FString& Buffer) const
{
	if (!bStreamAggregateChunks)
	{
		OnPartialResponse.Broadcast(Delta);
		return;
	}

	Buffer += Delta;

	bool bShouldFlush = false;
	if (bStreamEmitOnPunctuation)
	{
		for (int32 i = 0; i < Buffer.Len(); ++i)
		{
			const TCHAR Ch = Buffer[i];
			if (Ch == TEXT('.') || Ch == TEXT('!') || Ch == TEXT('?') || Ch == TEXT('\n'))
			{
				bShouldFlush = true;
				break;
			}
		}
	}

	if (!bShouldFlush && CountWords(Buffer) >= StreamMinWords)
	{
		bShouldFlush = true;
	}

	if (bShouldFlush)
	{
		OnPartialResponse.Broadcast(Buffer);
		Buffer.Empty();
	}
}

int32 ULocalRuntimeLLM::CountWords(const FString& Text) const
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

bool ULocalRuntimeLLM::IsEventAllowed(const FString& EventName) const
{
	for (const FRuntimeLLMEventSpec& Spec : AllowedEvents)
	{
		if (bCaseInsensitiveEventMatch)
		{
			if (Spec.EventName.Equals(EventName, ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
		else if (Spec.EventName == EventName)
		{
			return true;
		}
	}
	return false;
}
