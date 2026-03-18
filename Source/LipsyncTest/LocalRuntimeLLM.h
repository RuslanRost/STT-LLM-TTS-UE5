// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "LocalRuntimeLLM.generated.h"

USTRUCT(BlueprintType)
struct FRuntimeLLMEventSpec
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime LLM|Events")
	FString EventName;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime LLM|Events")
	FString Description;
};

USTRUCT(BlueprintType)
struct FRuntimeLLMEventCall
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Runtime LLM|Events")
	FString Name;

	UPROPERTY(BlueprintReadOnly, Category = "Runtime LLM|Events")
	FString ArgsJson;
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnRuntimeLLMResponse, const FString&, Prompt, const FString&, Response);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnRuntimeLLMPartialResponse, const FString&, Delta);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnRuntimeLLMError, const FString&, Error);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnRuntimeLLMEvents, const FString&, Reply, const TArray<FRuntimeLLMEventCall>&, Events);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnRuntimeLLMServerStarted);

UCLASS(BlueprintType)
class LIPSYNCTEST_API ULocalRuntimeLLM : public UObject
{
	GENERATED_BODY()

public:
	ULocalRuntimeLLM();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime LLM")
	FString ServerExePath;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime LLM")
	FString ModelPath;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime LLM")
	FString Host = TEXT("127.0.0.1");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime LLM")
	int32 Port = 8080;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime LLM")
	int32 ContextSize = 2048;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime LLM")
	int32 GpuLayers = 35;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime LLM")
	float Temperature = 0.7f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime LLM")
	bool bUseChatCompletions = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime LLM|Input")
	bool bRequireMeaningfulInput = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime LLM|Input", meta = (ClampMin = "0"))
	int32 MinInputChars = 3;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime LLM|Input", meta = (ClampMin = "0"))
	int32 MinInputWords = 2;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime LLM|System")
	FString BaseSystemPrompt;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime LLM|System")
	bool bForceRussian = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime LLM|System")
	FString RussianInstruction = TEXT("Always respond in Russian. Do not use any other language.");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime LLM|Events")
	TArray<FRuntimeLLMEventSpec> AllowedEvents;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime LLM|Events")
	bool bExpectStructuredResponse = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime LLM|Streaming")
	bool bStreamResponses = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime LLM|Streaming")
	bool bStreamStripJson = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime LLM|Streaming")
	bool bStreamAggregateChunks = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime LLM|Streaming", meta = (ClampMin = "1"))
	int32 StreamMinWords = 3;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime LLM|Streaming")
	bool bStreamEmitOnPunctuation = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime LLM|Reliability")
	bool bWaitForServer = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime LLM|Reliability")
	bool bAutoStartServer = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime LLM|Reliability")
	FString HealthEndpoint = TEXT("/health");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime LLM|Reliability")
	bool bUseHealthFallback = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime LLM|Reliability")
	FString FallbackHealthEndpoint = TEXT("/v1/models");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime LLM|Reliability", meta = (ClampMin = "0.1"))
	float HealthPollInterval = 0.5f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime LLM|Reliability", meta = (ClampMin = "0.1"))
	float HealthTimeoutSeconds = 10.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime LLM|Reliability")
	bool bRetryOn503 = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime LLM|Reliability", meta = (ClampMin = "0"))
	int32 MaxRetries = 3;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime LLM|Reliability", meta = (ClampMin = "0.1"))
	float RetryDelaySeconds = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime LLM|Logging")
	bool bLogResponseBodyOnError = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime LLM|Events")
	bool bAutoInvokeEvents = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime LLM|Events")
	bool bRestrictToAllowedEvents = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime LLM|Events")
	bool bCaseInsensitiveEventMatch = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime LLM|Events")
	TObjectPtr<UObject> EventTarget;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime LLM|Memory")
	bool bKeepContext = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime LLM|Memory", meta = (ClampMin = "0"))
	int32 MaxTurns = 8;

	UPROPERTY(BlueprintAssignable, Category = "Runtime LLM")
	FOnRuntimeLLMResponse OnResponse;

	UPROPERTY(BlueprintAssignable, Category = "Runtime LLM|Streaming")
	FOnRuntimeLLMPartialResponse OnPartialResponse;

	UPROPERTY(BlueprintAssignable, Category = "Runtime LLM")
	FOnRuntimeLLMError OnError;

	UPROPERTY(BlueprintAssignable, Category = "Runtime LLM|Events")
	FOnRuntimeLLMEvents OnEvents;

	UPROPERTY(BlueprintAssignable, Category = "Runtime LLM|Server")
	FOnRuntimeLLMServerStarted OnServerStarted;

	UFUNCTION(BlueprintCallable, meta = (DisplayName = "Create Runtime LLM", WorldContext = "WorldContextObject", DefaultToSelf = "WorldContextObject"), Category = "Runtime LLM")
	static ULocalRuntimeLLM* CreateRuntimeLLM(UObject* WorldContextObject);

	UFUNCTION(BlueprintCallable, Category = "Runtime LLM")
	bool StartServer();

	UFUNCTION(BlueprintCallable, Category = "Runtime LLM")
	void StopServer();

	UFUNCTION(BlueprintCallable, Category = "Runtime LLM")
	bool IsServerRunning();

	UFUNCTION(BlueprintCallable, Category = "Runtime LLM")
	void SendPrompt(const FString& Prompt, const FString& SystemPrompt);

	UFUNCTION(BlueprintCallable, Category = "Runtime LLM|Memory")
	void ClearContext();

protected:
	virtual void BeginDestroy() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	FProcHandle ServerProcess;
	TArray<TSharedPtr<FJsonValue>> MessageHistory;
	bool bServerStartedNotified = false;

	UWorld* GetWorldFromContext() const;
	FString ResolvePath(const FString& Path) const;
	FString GetBaseUrl() const;
	FString BuildSystemPrompt(const FString& RuntimeSystemPrompt) const;
	void EmitError(const FString& Message) const;
	void PostJson(const FString& Url, const FString& Body, const FString& Prompt, const FString& SystemPrompt, int32 Attempt);
	void SchedulePostRetry(const FString& Url, const FString& Body, const FString& Prompt, const FString& SystemPrompt, int32 Attempt);
	void CheckServerHealthAndPost(const FString& Url, const FString& Body, const FString& Prompt, const FString& SystemPrompt, double StartTime, bool bTriedFallback);
	void AddMessage(const FString& Role, const FString& Content);
	void TrimHistory();
	bool TryParseStructuredResponse(const FString& ResponseText, FString& OutReply, TArray<FRuntimeLLMEventCall>& OutEvents) const;
	void TryInvokeEvent(const FRuntimeLLMEventCall& EventCall) const;
	bool IsMeaningfulInput(const FString& Prompt) const;
	FString ExtractDeltaFromJson(const FString& JsonText) const;
	FString FilterReplyDelta(const FString& Delta, bool bStripJson, FString& PrefixBuffer, bool& bInReply, bool& bEscape) const;
	void EmitStreamChunk(const FString& Delta, FString& Buffer) const;
	int32 CountWords(const FString& Text) const;
	bool IsEventAllowed(const FString& EventName) const;
};
