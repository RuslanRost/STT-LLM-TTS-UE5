// Fill out your copyright notice in the Description page of Project Settings.

#include "LocalLLMComponent.h"

#include "Dom/JsonObject.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "JsonObjectConverter.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "TimerManager.h"

ULocalLLMComponent::ULocalLLMComponent()
{
	PrimaryComponentTick.bCanEverTick = false;

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

FString ULocalLLMComponent::ResolvePath(const FString& Path) const
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

bool ULocalLLMComponent::StartServer()
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

void ULocalLLMComponent::StopServer()
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

bool ULocalLLMComponent::IsServerRunning()
{
	if (!ServerProcess.IsValid())
	{
		return false;
	}

	return FPlatformProcess::IsProcRunning(ServerProcess);
}

void ULocalLLMComponent::SendPrompt(const FString& Prompt, const FString& SystemPrompt)
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

void ULocalLLMComponent::ClearContext()
{
	MessageHistory.Reset();
}

void ULocalLLMComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	StopServer();
	Super::EndPlay(EndPlayReason);
}

FString ULocalLLMComponent::GetBaseUrl() const
{
	return FString::Printf(TEXT("http://%s:%d"), *Host, Port);
}

FString ULocalLLMComponent::BuildSystemPrompt(const FString& RuntimeSystemPrompt) const
{
	FString Result = BaseSystemPrompt;

	if (bForceRussian && !RussianInstruction.IsEmpty())
	{
		if (!Result.IsEmpty())
		{
			Result += TEXT("\n\n");
		}
		Result += RussianInstruction;
	}

	if (!RuntimeSystemPrompt.IsEmpty())
	{
		if (!Result.IsEmpty())
		{
			Result += TEXT("\n\n");
		}
		Result += RuntimeSystemPrompt;
	}

	if (AllowedEvents.Num() > 0)
	{
		if (!Result.IsEmpty())
		{
			Result += TEXT("\n\n");
		}

		Result += TEXT("Allowed blueprint events you can request:\n");
		for (const FLLMEventSpec& Spec : AllowedEvents)
		{
			if (Spec.EventName.IsEmpty())
			{
				continue;
			}

			if (!Spec.Description.IsEmpty())
			{
				Result += FString::Printf(TEXT("- %s: %s\n"), *Spec.EventName, *Spec.Description);
			}
			else
			{
				Result += FString::Printf(TEXT("- %s\n"), *Spec.EventName);
			}
		}
	}

	if (bExpectStructuredResponse)
	{
		if (!Result.IsEmpty())
		{
			Result += TEXT("\n\n");
		}

		Result += TEXT("Return ONLY valid JSON with this shape:\n");
		Result += TEXT("{\"reply\":\"string\",\"events\":[{\"name\":\"event_name\",\"args\":{...}}]}\n");
		Result += TEXT("If no events, return an empty array. Do not include any extra text.");
	}

	return Result;
}

void ULocalLLMComponent::EmitError(const FString& Message) const
{
	ULocalLLMComponent* MutableThis = const_cast<ULocalLLMComponent*>(this);
	MutableThis->OnError.Broadcast(Message);
}

void ULocalLLMComponent::SchedulePostRetry(const FString& Url, const FString& Body, const FString& Prompt, const FString& SystemPrompt, int32 Attempt)
{
	if (Attempt > MaxRetries)
	{
		EmitError(TEXT("LLM retry limit exceeded."));
		return;
	}

	UE_LOG(LogTemp, Warning, TEXT("LLM RETRY: scheduling attempt=%d in %.2fs"), Attempt + 1, RetryDelaySeconds);

	if (UWorld* World = GetWorld())
	{
		FTimerHandle RetryTimer;
		World->GetTimerManager().SetTimer(RetryTimer, FTimerDelegate::CreateWeakLambda(this, [this, Url, Body, Prompt, SystemPrompt, Attempt]()
		{
			PostJson(Url, Body, Prompt, SystemPrompt, Attempt);
		}), RetryDelaySeconds, false);
	}
	else
	{
		PostJson(Url, Body, Prompt, SystemPrompt, Attempt);
	}
}

void ULocalLLMComponent::CheckServerHealthAndPost(const FString& Url, const FString& Body, const FString& Prompt, const FString& SystemPrompt, double StartTime, bool bTriedFallback)
{
	const double Elapsed = FPlatformTime::Seconds() - StartTime;
	if (Elapsed > HealthTimeoutSeconds)
	{
		UE_LOG(LogTemp, Warning, TEXT("LLM HEALTH: timed out after %.2fs"), Elapsed);
		EmitError(TEXT("LLM server health check timed out."));
		return;
	}

	const FString EndpointToUse = (bUseHealthFallback && bTriedFallback) ? FallbackHealthEndpoint : HealthEndpoint;
	const FString HealthUrl = GetBaseUrl() + EndpointToUse;
	UE_LOG(LogTemp, Warning, TEXT("LLM HEALTH: GET %s (elapsed=%.2fs)"), *HealthUrl, Elapsed);

	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(HealthUrl);
	Request->SetVerb(TEXT("GET"));
	Request->OnProcessRequestComplete().BindLambda([this, Url, Body, Prompt, SystemPrompt, StartTime, bTriedFallback](FHttpRequestPtr Req, FHttpResponsePtr Resp, bool bSucceeded)
	{
		const int32 Code = Resp.IsValid() ? Resp->GetResponseCode() : 0;
		if (bSucceeded && Resp.IsValid() && Code >= 200 && Code < 300)
		{
			const double OkElapsed = FPlatformTime::Seconds() - StartTime;
			UE_LOG(LogTemp, Warning, TEXT("LLM HEALTH: OK (%d) after %.2fs"), Code, OkElapsed);
			if (!bServerStartedNotified)
			{
				bServerStartedNotified = true;
				UE_LOG(LogTemp, Warning, TEXT("LLM SERVER: Ready"));
				OnServerStarted.Broadcast();
			}
			PostJson(Url, Body, Prompt, SystemPrompt, 0);
			return;
		}

		if (bUseHealthFallback && !bTriedFallback && HealthEndpoint != FallbackHealthEndpoint)
		{
			UE_LOG(LogTemp, Warning, TEXT("LLM HEALTH: fallback to %s (status=%d, elapsed=%.2fs)"), *FallbackHealthEndpoint, Code, FPlatformTime::Seconds() - StartTime);
			CheckServerHealthAndPost(Url, Body, Prompt, SystemPrompt, StartTime, true);
			return;
		}

		UE_LOG(LogTemp, Warning, TEXT("LLM HEALTH: not ready (status=%d, elapsed=%.2fs). Retrying..."), Code, FPlatformTime::Seconds() - StartTime);
		if (UWorld* World = GetWorld())
		{
			FTimerHandle HealthTimer;
			World->GetTimerManager().SetTimer(HealthTimer, FTimerDelegate::CreateWeakLambda(this, [this, Url, Body, Prompt, SystemPrompt, StartTime]()
			{
				CheckServerHealthAndPost(Url, Body, Prompt, SystemPrompt, StartTime, true);
			}), HealthPollInterval, false);
		}
	});
	Request->ProcessRequest();
}

void ULocalLLMComponent::PostJson(const FString& Url, const FString& Body, const FString& Prompt, const FString& SystemPrompt, int32 Attempt)
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

			// Flush any remaining buffered text to partial stream, but keep word boundaries.
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
			TArray<FLLMEventCall> Events;
			if (TryParseStructuredResponse(Content, Reply, Events))
			{
				if (bAutoInvokeEvents)
				{
					for (const FLLMEventCall& EventCall : Events)
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

bool ULocalLLMComponent::TryParseStructuredResponse(const FString& ResponseText, FString& OutReply, TArray<FLLMEventCall>& OutEvents) const
{
	TSharedPtr<FJsonObject> Root;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResponseText);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		// Fallback: treat whole response as plain text.
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

			FLLMEventCall Call;
			Call.Name = Obj->GetStringField(TEXT("name"));

			if (Obj->HasTypedField<EJson::Object>(TEXT("args")))
			{
				FString ArgsJson;
				TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ArgsJson);
				FJsonSerializer::Serialize(Obj->GetObjectField(TEXT("args")).ToSharedRef(), Writer);
				Call.ArgsJson = ArgsJson;
			}

			if (!Call.Name.IsEmpty())
			{
				OutEvents.Add(Call);
			}
		}
	}

	if (OutReply.IsEmpty() && OutEvents.Num() == 0)
	{
		OutReply = ResponseText;
	}

	return true;
}

void ULocalLLMComponent::TryInvokeEvent(const FLLMEventCall& EventCall) const
{
	if (EventCall.Name.IsEmpty())
	{
		return;
	}

	if (bRestrictToAllowedEvents && !IsEventAllowed(EventCall.Name))
	{
		EmitError(FString::Printf(TEXT("Event not allowed: %s"), *EventCall.Name));
		return;
	}

	UObject* Target = EventTarget ? EventTarget.Get() : GetOwner();
	if (!Target)
	{
		return;
	}

	const FName FuncName(*EventCall.Name);
	UFunction* Func = Target->FindFunction(FuncName);
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

	Target->ProcessEvent(Func, nullptr);
}

bool ULocalLLMComponent::IsMeaningfulInput(const FString& Prompt) const
{
	FString Trimmed = Prompt;
	Trimmed.TrimStartAndEndInline();

	if (Trimmed.Len() < MinInputChars)
	{
		return false;
	}

	int32 WordCount = 0;
	bool bInWord = false;
	for (int32 i = 0; i < Trimmed.Len(); ++i)
	{
		const TCHAR Ch = Trimmed[i];
		const bool bIsSpace = FChar::IsWhitespace(Ch);
		if (!bIsSpace && !bInWord)
		{
			bInWord = true;
			++WordCount;
		}
		else if (bIsSpace)
		{
			bInWord = false;
		}
	}

	return WordCount >= MinInputWords;
}

FString ULocalLLMComponent::ExtractDeltaFromJson(const FString& JsonText) const
{
	TSharedPtr<FJsonObject> Json;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
	if (!FJsonSerializer::Deserialize(Reader, Json) || !Json.IsValid())
	{
		return FString();
	}

	if (Json->HasTypedField<EJson::Array>(TEXT("choices")))
	{
		const TArray<TSharedPtr<FJsonValue>> Choices = Json->GetArrayField(TEXT("choices"));
		if (Choices.Num() > 0 && Choices[0]->Type == EJson::Object)
		{
			const TSharedPtr<FJsonObject> ChoiceObj = Choices[0]->AsObject();
			if (ChoiceObj.IsValid())
			{
				if (ChoiceObj->HasTypedField<EJson::Object>(TEXT("delta")))
				{
					const TSharedPtr<FJsonObject> Delta = ChoiceObj->GetObjectField(TEXT("delta"));
					if (Delta.IsValid() && Delta->HasTypedField<EJson::String>(TEXT("content")))
					{
						return Delta->GetStringField(TEXT("content"));
					}
				}
				if (ChoiceObj->HasTypedField<EJson::String>(TEXT("text")))
				{
					return ChoiceObj->GetStringField(TEXT("text"));
				}
			}
		}
	}

	return FString();
}

FString ULocalLLMComponent::FilterReplyDelta(const FString& Delta, bool bStripJson, FString& PrefixBuffer, bool& bInReply, bool& bEscape) const
{
	if (!bStripJson)
	{
		return Delta;
	}

	static const FString Pattern = TEXT("\"reply\":\"");
	if (Pattern.Len() == 0)
	{
		return Delta;
	}
	FString Out;
	if (!bInReply)
	{
		PrefixBuffer += Delta;
		const int32 Found = PrefixBuffer.Find(Pattern);
		if (Found != INDEX_NONE)
		{
			const int32 Start = Found + Pattern.Len();
			const FString After = PrefixBuffer.Mid(Start);
			PrefixBuffer.Reset();
			bInReply = true;
			bEscape = false;
			if (!After.IsEmpty())
			{
				Out = FilterReplyDelta(After, true, PrefixBuffer, bInReply, bEscape);
			}
		}
		else
		{
			// Keep only the tail to match across boundaries.
			const int32 Keep = FMath::Max(0, Pattern.Len() - 1);
			if (PrefixBuffer.Len() > Keep)
			{
				PrefixBuffer = PrefixBuffer.Right(Keep);
			}
		}
		return Out;
	}

	for (int32 i = 0; i < Delta.Len(); ++i)
	{
		const TCHAR Ch = Delta[i];
		if (bEscape)
		{
			Out.AppendChar(Ch);
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
			// Stop at end of reply string.
			break;
		}
		Out.AppendChar(Ch);
	}

	return Out;
}

void ULocalLLMComponent::EmitStreamChunk(const FString& Delta, FString& Buffer) const
{
	if (!bStreamAggregateChunks)
	{
		AsyncTask(ENamedThreads::GameThread, [this, Delta]()
		{
			OnPartialResponse.Broadcast(Delta);
		});
		return;
	}

	Buffer += Delta;

	int32 EmitLen = INDEX_NONE;
	if (bStreamEmitOnPunctuation)
	{
		int32 LastIdx = INDEX_NONE;
		for (int32 i = 0; i < Buffer.Len(); ++i)
		{
			const TCHAR Ch = Buffer[i];
			if (Ch == TEXT('.') || Ch == TEXT('!') || Ch == TEXT('?') || Ch == TEXT('\n'))
			{
				LastIdx = i;
			}
		}
		if (LastIdx != INDEX_NONE)
		{
			EmitLen = LastIdx + 1;
		}
	}

	if (EmitLen == INDEX_NONE && CountWords(Buffer) >= StreamMinWords)
	{
		EmitLen = Buffer.Len();
	}

	if (EmitLen != INDEX_NONE && EmitLen > 0)
	{
		int32 SafeEmitLen = EmitLen;
		// Emit only up to a safe boundary (whitespace or punctuation).
		int32 LastBoundary = INDEX_NONE;
		for (int32 i = SafeEmitLen - 1; i >= 0; --i)
		{
			const TCHAR Ch = Buffer[i];
			if (FChar::IsWhitespace(Ch) || Ch == TEXT('.') || Ch == TEXT('!') || Ch == TEXT('?') || Ch == TEXT(',') || Ch == TEXT(';') || Ch == TEXT(':') || Ch == TEXT('\n'))
			{
				LastBoundary = i + 1;
				break;
			}
		}
		if (LastBoundary == INDEX_NONE)
		{
			return;
		}
		SafeEmitLen = LastBoundary;

		FString Out = Buffer.Left(SafeEmitLen);
		Buffer = Buffer.Mid(SafeEmitLen);
		Buffer.TrimStartInline();
		// Strip JSON reply prefix if it still leaks.
		const int32 ReplyPos = Out.Find(TEXT("\"reply\":\""));
		if (ReplyPos != INDEX_NONE)
		{
			Out = Out.Mid(ReplyPos + 9);
		}
		if (Out.StartsWith(TEXT("{\"reply\":\"")))
		{
			Out = Out.Mid(10);
		}
		Out.TrimStartAndEndInline();
		if (Out.IsEmpty())
		{
			return;
		}
		AsyncTask(ENamedThreads::GameThread, [this, Out]()
		{
			OnPartialResponse.Broadcast(Out);
		});
	}
}

int32 ULocalLLMComponent::CountWords(const FString& Text) const
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

bool ULocalLLMComponent::IsEventAllowed(const FString& EventName) const
{
	if (AllowedEvents.Num() == 0)
	{
		return false;
	}

	const FString NameToCheck = bCaseInsensitiveEventMatch ? EventName.ToLower() : EventName;
	for (const FLLMEventSpec& Spec : AllowedEvents)
	{
		if (Spec.EventName.IsEmpty())
		{
			continue;
		}
		const FString AllowedName = bCaseInsensitiveEventMatch ? Spec.EventName.ToLower() : Spec.EventName;
		if (AllowedName == NameToCheck)
		{
			return true;
		}
	}
	return false;
}

void ULocalLLMComponent::AddMessage(const FString& Role, const FString& Content)
{
	if (Content.IsEmpty())
	{
		return;
	}

	TSharedPtr<FJsonObject> Msg = MakeShared<FJsonObject>();
	Msg->SetStringField(TEXT("role"), Role);
	Msg->SetStringField(TEXT("content"), Content);
	MessageHistory.Add(MakeShared<FJsonValueObject>(Msg));
}

void ULocalLLMComponent::TrimHistory()
{
	if (MaxTurns <= 0)
	{
		return;
	}

	// Each turn is user+assistant. Keep the last MaxTurns*2 messages.
	const int32 MaxMessages = MaxTurns * 2;
	while (MessageHistory.Num() > MaxMessages)
	{
		MessageHistory.RemoveAt(0);
	}
}
