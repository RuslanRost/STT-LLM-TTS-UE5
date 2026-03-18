#pragma once

#include "CoreMinimal.h"
#include "Interfaces/IHttpRequest.h"
#include "RuntimeAudioImporterTypes.h"
#include "UObject/Object.h"
#include "HttpRuntimeTTSClient.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_FourParams(FOnRuntimeTTSRequestSuccess, const FString&, Method, const FString&, Url, int32, StatusCode, const FString&, ResponseBody);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FOnRuntimeTTSRequestFailure, const FString&, Method, const FString&, Url, const FString&, ErrorMessage);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_FiveParams(FOnRuntimeTTSStreamStarted, const FString&, Text, int32, SampleRate, int32, NumChannels, const FString&, RequestId, const FString&, ClientId);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_EightParams(FOnRuntimeTTSStreamChunk, const FString&, Text, const TArray<uint8>&, AudioChunk, int32, SampleRate, int32, NumChannels, ERuntimeRAWAudioFormat, RawFormat, int32, ChunkIndex, const FString&, RequestId, const FString&, ClientId);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_SixParams(FOnRuntimeTTSStreamCompleted, const FString&, Text, const TArray<uint8>&, AudioData, int32, SampleRate, int32, NumChannels, const FString&, RequestId, const FString&, ClientId);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_FiveParams(FOnRuntimeTTSSpeechResultHttp, const FString&, Text, const TArray<uint8>&, AudioData, int32, SampleRate, int32, NumChannels, ERuntimeRAWAudioFormat, RawFormat);

UCLASS(BlueprintType)
class LIPSYNCTEST_API UHttpRuntimeTTSClient : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime TTS")
	FString BaseUrl = TEXT("http://127.0.0.1:8082");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime TTS")
	FString ClientId;

	UPROPERTY(BlueprintAssignable, Category = "Runtime TTS")
	FOnRuntimeTTSRequestSuccess OnRequestSucceeded;

	UPROPERTY(BlueprintAssignable, Category = "Runtime TTS")
	FOnRuntimeTTSRequestFailure OnRequestFailed;

	UPROPERTY(BlueprintAssignable, Category = "Runtime TTS")
	FOnRuntimeTTSStreamStarted OnTTSStreamStarted;

	UPROPERTY(BlueprintAssignable, Category = "Runtime TTS")
	FOnRuntimeTTSStreamChunk OnTTSStreamChunk;

	UPROPERTY(BlueprintAssignable, Category = "Runtime TTS")
	FOnRuntimeTTSStreamCompleted OnTTSStreamCompleted;

	UPROPERTY(BlueprintAssignable, Category = "Runtime TTS")
	FOnRuntimeTTSSpeechResultHttp OnTTSSpeechResult;

	UPROPERTY(BlueprintAssignable, Category = "Runtime TTS", meta = (DisplayName = "OnSpeechResult"))
	FOnRuntimeTTSSpeechResultHttp OnSpeechResult;

	UFUNCTION(BlueprintCallable, meta = (DisplayName = "Create Runtime TTS Client"), Category = "Runtime TTS")
	static UHttpRuntimeTTSClient* CreateRuntimeTTSClient();

	UFUNCTION(BlueprintCallable, Category = "Runtime TTS")
	void SendPing();

	UFUNCTION(BlueprintCallable, Category = "Runtime TTS")
	void GetTTSStatus();

	UFUNCTION(BlueprintCallable, Category = "Runtime TTS")
	void SendTTSStream(const FString& Text, bool bUseCuda = false, int32 SpeakerId = -1, int32 ChunkSize = 4096);

private:
	FString BuildUrl(const FString& Route) const;
	void SendJsonRequest(const FString& Method, const FString& Route, const FString& JsonBody);
	void HandleResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful, FString Method, FString Url);
};
