#pragma once

#include "CoreMinimal.h"
#include "AudioCapture.h"
#include "Interfaces/IHttpRequest.h"
#include "UObject/Object.h"
#include "HttpRuntimeSTTStreamClient.generated.h"

class UWorld;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_FourParams(FOnRuntimeSTTStreamRequestSuccess, const FString&, Method, const FString&, Url, int32, StatusCode, const FString&, ResponseBody);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FOnRuntimeSTTStreamRequestFailure, const FString&, Method, const FString&, Url, const FString&, ErrorMessage);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnRuntimeSTTStreamPartial, const FString&, Text);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnRuntimeSTTStreamFinal, const FString&, Text);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnRuntimeSTTStreamKeywordDetected, const FString&, Keyword);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnRuntimeSTTStreamWakeWordDetected, const FString&, WakeWord);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnRuntimeSTTStreamError, const FString&, Error);

UCLASS(BlueprintType)
class LIPSYNCTEST_API UHttpRuntimeSTTStreamClient : public UObject
{
	GENERATED_BODY()

public:
	UHttpRuntimeSTTStreamClient();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime STT")
	FString BaseUrl = TEXT("http://127.0.0.1:8083");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime STT")
	FString ClientId;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime STT")
	int32 SampleRate = 16000;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime STT")
	bool bUseGrammar = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime STT", meta = (EditCondition = "bUseGrammar"))
	TArray<FString> GrammarPhrases;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime STT|Keywords")
	TArray<FString> Keywords;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime STT|Wake Word")
	bool bUseWakeWord = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime STT|Wake Word")
	TArray<FString> WakeKeywords;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime STT|Wake Word")
	bool bWakeOnPartial = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime STT|Wake Word", meta = (ClampMin = "0.1"))
	float ActiveListenSeconds = 5.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime STT|Wake Word")
	bool bExtendActiveOnFinal = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime STT|Wake Word")
	bool bExtendActiveOnPartial = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime STT|Wake Word")
	bool bDelayActiveTimerUntilTTSFinished = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime STT|Wake Word")
	bool bWakeWordFocusMode = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime STT|Keywords")
	bool bDetectKeywordsInPartial = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime STT|Keywords", meta = (ClampMin = "0.0"))
	float KeywordCooldownSeconds = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime STT")
	bool bEnableWords = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime STT|Transport", meta = (ClampMin = "0.02"))
	float FlushIntervalSeconds = 0.12f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime STT|Logging")
	bool bLogDebug = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime STT|Logging")
	bool bLogWakeWord = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime STT|Logging")
	bool bLogKeywords = false;

	UPROPERTY(BlueprintAssignable, Category = "Runtime STT")
	FOnRuntimeSTTStreamRequestSuccess OnRequestSucceeded;

	UPROPERTY(BlueprintAssignable, Category = "Runtime STT")
	FOnRuntimeSTTStreamRequestFailure OnRequestFailed;

	UPROPERTY(BlueprintAssignable, Category = "Runtime STT")
	FOnRuntimeSTTStreamPartial OnPartial;

	UPROPERTY(BlueprintAssignable, Category = "Runtime STT")
	FOnRuntimeSTTStreamFinal OnFinal;

	UPROPERTY(BlueprintAssignable, Category = "Runtime STT")
	FOnRuntimeSTTStreamKeywordDetected OnKeywordDetected;

	UPROPERTY(BlueprintAssignable, Category = "Runtime STT")
	FOnRuntimeSTTStreamWakeWordDetected OnWakeWordDetected;

	UPROPERTY(BlueprintAssignable, Category = "Runtime STT")
	FOnRuntimeSTTStreamError OnError;

	UFUNCTION(BlueprintCallable, meta = (DisplayName = "Create Runtime STT Stream Client", WorldContext = "WorldContextObject", DefaultToSelf = "WorldContextObject"), Category = "Runtime STT")
	static UHttpRuntimeSTTStreamClient* CreateRuntimeSTTStreamClient(UObject* WorldContextObject);

	UFUNCTION(BlueprintCallable, Category = "Runtime STT")
	void SendPing();

	UFUNCTION(BlueprintCallable, Category = "Runtime STT")
	void GetSTTStatus();

	UFUNCTION(BlueprintCallable, Category = "Runtime STT")
	bool StartListening();

	UFUNCTION(BlueprintCallable, Category = "Runtime STT")
	void StopListening();

	UFUNCTION(BlueprintCallable, Category = "Runtime STT")
	bool IsListening() const;

	UFUNCTION(BlueprintCallable, Category = "Runtime STT|Wake Word")
	void EnterStandby();

	UFUNCTION(BlueprintCallable, Category = "Runtime STT|Wake Word")
	void EnterActive();

	UFUNCTION(BlueprintCallable, Category = "Runtime STT|Wake Word")
	void NotifyTTSStarted();

	UFUNCTION(BlueprintCallable, Category = "Runtime STT|Wake Word")
	void NotifyTTSEnded();

protected:
	virtual void BeginDestroy() override;

private:
	FString BuildUrl(const FString& Route) const;
	UWorld* GetWorldFromContext() const;
	void EmitError(const FString& Message);
	void ResetWakeState();
	void DrainQueuedAudio();
	TArray<FString> BuildGrammarPhraseList() const;
	FString ExtractPostWakeText(const FString& Text) const;
	void HandleRecognizedText(const FString& Text, bool bFinal);
	void CheckKeywords(const FString& Text);
	bool CheckWakeWords(const FString& Text, bool bFinal);
	void TransitionToActive(const FString& WakeWord);
	void HandleActiveTimeout();

	bool StartAudioCapture();
	void StopAudioCapture();
	void FlushAudioQueue();
	void RequestFinalFromServer();
	void SendStopSessionRequest();
	void SendJsonRequest(const FString& Method, const FString& Route, const FString& JsonBody, void (UHttpRuntimeSTTStreamClient::*Handler)(FHttpRequestPtr, FHttpResponsePtr, bool, FString));
	void HandleSimpleResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful, FString Url);
	void HandleStartSessionResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful, FString Url);
	void HandleAppendAudioResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful, FString Url);
	void HandleFinalResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful, FString Url);
	void HandleStopSessionResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful, FString Url);
	bool ProcessSessionEventResponse(FHttpResponsePtr Response, FString& OutErrorMessage);
	void ContinuePendingRequests();

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	TUniquePtr<Audio::FAudioCapture> AudioCapture;
	TQueue<TArray<uint8>, EQueueMode::Mpsc> AudioQueue;

	FString SessionId;
	int32 CaptureSampleRate = 16000;
	int32 CaptureNumChannels = 1;

	bool bListening = false;
	bool bStartRequested = false;
	bool bSessionStarting = false;
	bool bRequestInFlight = false;
	bool bPendingFinalRequest = false;
	bool bPendingStopRequest = false;
	bool bCaptureStarted = false;
	bool bActiveListening = true;
	bool bTTSActive = false;
	bool bWaitingPostWakeFinal = false;
	bool bHadSpeechSinceActive = false;

	FString LastWakeWord;
	FString PostWakeBuffer;
	TMap<FString, double> LastKeywordTime;
	FTimerHandle FlushTimer;
	FTimerHandle ActiveTimer;
};
