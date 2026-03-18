#pragma once

#include "CoreMinimal.h"
#include "Interfaces/IHttpRequest.h"
#include "UObject/Object.h"
#include "HttpRuntimeSTTClient.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_FourParams(FOnRuntimeSTTRequestSuccess, const FString&, Method, const FString&, Url, int32, StatusCode, const FString&, ResponseBody);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FOnRuntimeSTTRequestFailure, const FString&, Method, const FString&, Url, const FString&, ErrorMessage);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnRuntimeSTTPartialHttp, const FString&, Text);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnRuntimeSTTFinalHttp, const FString&, Text);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnRuntimeSTTErrorHttp, const FString&, Error);

UCLASS(BlueprintType)
class LIPSYNCTEST_API UHttpRuntimeSTTClient : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime STT")
	FString BaseUrl = TEXT("http://127.0.0.1:8083");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime STT")
	FString ClientId;

	UPROPERTY(BlueprintAssignable, Category = "Runtime STT")
	FOnRuntimeSTTRequestSuccess OnRequestSucceeded;

	UPROPERTY(BlueprintAssignable, Category = "Runtime STT")
	FOnRuntimeSTTRequestFailure OnRequestFailed;

	UPROPERTY(BlueprintAssignable, Category = "Runtime STT")
	FOnRuntimeSTTPartialHttp OnPartial;

	UPROPERTY(BlueprintAssignable, Category = "Runtime STT")
	FOnRuntimeSTTFinalHttp OnFinal;

	UPROPERTY(BlueprintAssignable, Category = "Runtime STT")
	FOnRuntimeSTTErrorHttp OnError;

	UFUNCTION(BlueprintCallable, meta = (DisplayName = "Create Runtime STT Client"), Category = "Runtime STT")
	static UHttpRuntimeSTTClient* CreateRuntimeSTTClient();

	UFUNCTION(BlueprintCallable, Category = "Runtime STT")
	void SendPing();

	UFUNCTION(BlueprintCallable, Category = "Runtime STT")
	void GetSTTStatus();

	UFUNCTION(BlueprintCallable, Category = "Runtime STT", meta = (AutoCreateRefTerm = "GrammarPhrases"))
	void TranscribePCM16(const TArray<uint8>& AudioData, int32 SampleRate, int32 NumChannels, bool bEnableWords, const TArray<FString>& GrammarPhrases);

	UFUNCTION(BlueprintCallable, Category = "Runtime STT", meta = (AutoCreateRefTerm = "GrammarPhrases"))
	void TranscribeFloatAudio(const TArray<float>& AudioData, int32 SampleRate, int32 NumChannels, bool bEnableWords, const TArray<FString>& GrammarPhrases);

private:
	FString BuildUrl(const FString& Route) const;
	void SendJsonRequest(const FString& Method, const FString& Route, const FString& JsonBody);
	void HandleResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful, FString Method, FString Url);
	void HandleTranscribeResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful, FString Url);
};
