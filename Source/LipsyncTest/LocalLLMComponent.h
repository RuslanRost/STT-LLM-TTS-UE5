// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "LocalLLMComponent.generated.h"

USTRUCT(BlueprintType)
struct FLLMEventSpec
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Local LLM|Events")
	FString EventName;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Local LLM|Events")
	FString Description;
};

USTRUCT(BlueprintType)
struct FLLMEventCall
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Local LLM|Events")
	FString Name;

	UPROPERTY(BlueprintReadOnly, Category = "Local LLM|Events")
	FString ArgsJson;
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnLLMResponse, const FString&, Prompt, const FString&, Response);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnLLMPartialResponse, const FString&, Delta);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnLLMError, const FString&, Error);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnLLMEvents, const FString&, Reply, const TArray<FLLMEventCall>&, Events);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnLLMServerStarted);

UCLASS(ClassGroup=(Custom), meta=(BlueprintSpawnableComponent))
class LIPSYNCTEST_API ULocalLLMComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	ULocalLLMComponent();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Local LLM")
	FString ServerExePath;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Local LLM")
	FString ModelPath;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Local LLM")
	FString Host = TEXT("127.0.0.1");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Local LLM")
	int32 Port = 8080;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Local LLM")
	int32 ContextSize = 2048;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Local LLM")
	int32 GpuLayers = 35;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Local LLM")
	float Temperature = 0.7f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Local LLM")
	bool bUseChatCompletions = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Local LLM|Input")
	bool bRequireMeaningfulInput = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Local LLM|Input", meta = (ClampMin = "0"))
	int32 MinInputChars = 3;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Local LLM|Input", meta = (ClampMin = "0"))
	int32 MinInputWords = 2;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Local LLM|System")
	FString BaseSystemPrompt;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Local LLM|System")
	bool bForceRussian = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Local LLM|System")
	FString RussianInstruction = TEXT("Always respond in Russian. Do not use any other language.");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Local LLM|Events")
	TArray<FLLMEventSpec> AllowedEvents;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Local LLM|Events")
	bool bExpectStructuredResponse = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Local LLM|Streaming")
	bool bStreamResponses = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Local LLM|Streaming")
	bool bStreamStripJson = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Local LLM|Streaming")
	bool bStreamAggregateChunks = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Local LLM|Streaming", meta = (ClampMin = "1"))
	int32 StreamMinWords = 3;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Local LLM|Streaming")
	bool bStreamEmitOnPunctuation = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Local LLM|Reliability")
	bool bWaitForServer = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Local LLM|Reliability")
	bool bAutoStartServer = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Local LLM|Reliability")
	FString HealthEndpoint = TEXT("/health");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Local LLM|Reliability")
	bool bUseHealthFallback = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Local LLM|Reliability")
	FString FallbackHealthEndpoint = TEXT("/v1/models");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Local LLM|Reliability", meta = (ClampMin = "0.1"))
	float HealthPollInterval = 0.5f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Local LLM|Reliability", meta = (ClampMin = "0.1"))
	float HealthTimeoutSeconds = 10.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Local LLM|Reliability")
	bool bRetryOn503 = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Local LLM|Reliability", meta = (ClampMin = "0"))
	int32 MaxRetries = 3;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Local LLM|Reliability", meta = (ClampMin = "0.1"))
	float RetryDelaySeconds = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Local LLM|Logging")
	bool bLogResponseBodyOnError = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Local LLM|Events")
	bool bAutoInvokeEvents = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Local LLM|Events")
	bool bRestrictToAllowedEvents = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Local LLM|Events")
	bool bCaseInsensitiveEventMatch = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Local LLM|Events")
	TObjectPtr<UObject> EventTarget;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Local LLM|Memory")
	bool bKeepContext = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Local LLM|Memory", meta = (ClampMin = "0"))
	int32 MaxTurns = 8;

	UPROPERTY(BlueprintAssignable, Category = "Local LLM")
	FOnLLMResponse OnResponse;

	UPROPERTY(BlueprintAssignable, Category = "Local LLM|Streaming")
	FOnLLMPartialResponse OnPartialResponse;

	UPROPERTY(BlueprintAssignable, Category = "Local LLM")
	FOnLLMError OnError;

	UPROPERTY(BlueprintAssignable, Category = "Local LLM|Events")
	FOnLLMEvents OnEvents;

	UPROPERTY(BlueprintAssignable, Category = "Local LLM|Server")
	FOnLLMServerStarted OnServerStarted;

	UFUNCTION(BlueprintCallable, Category = "Local LLM")
	bool StartServer();

	UFUNCTION(BlueprintCallable, Category = "Local LLM")
	void StopServer();

	UFUNCTION(BlueprintCallable, Category = "Local LLM")
	bool IsServerRunning();

	UFUNCTION(BlueprintCallable, Category = "Local LLM")
	void SendPrompt(const FString& Prompt, const FString& SystemPrompt);

	UFUNCTION(BlueprintCallable, Category = "Local LLM|Memory")
	void ClearContext();

protected:
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	FProcHandle ServerProcess;
	TArray<TSharedPtr<FJsonValue>> MessageHistory;
	bool bServerStartedNotified = false;

	FString ResolvePath(const FString& Path) const;
	FString GetBaseUrl() const;
	FString BuildSystemPrompt(const FString& RuntimeSystemPrompt) const;
	void EmitError(const FString& Message) const;
	void PostJson(const FString& Url, const FString& Body, const FString& Prompt, const FString& SystemPrompt, int32 Attempt);
	void SchedulePostRetry(const FString& Url, const FString& Body, const FString& Prompt, const FString& SystemPrompt, int32 Attempt);
	void CheckServerHealthAndPost(const FString& Url, const FString& Body, const FString& Prompt, const FString& SystemPrompt, double StartTime, bool bTriedFallback);
	void AddMessage(const FString& Role, const FString& Content);
	void TrimHistory();
	bool TryParseStructuredResponse(const FString& ResponseText, FString& OutReply, TArray<FLLMEventCall>& OutEvents) const;
	void TryInvokeEvent(const FLLMEventCall& EventCall) const;
	bool IsMeaningfulInput(const FString& Prompt) const;
	FString ExtractDeltaFromJson(const FString& JsonText) const;
	FString FilterReplyDelta(const FString& Delta, bool bStripJson, FString& PrefixBuffer, bool& bInReply, bool& bEscape) const;
	void EmitStreamChunk(const FString& Delta, FString& Buffer) const;
	int32 CountWords(const FString& Text) const;
	bool IsEventAllowed(const FString& EventName) const;
};
