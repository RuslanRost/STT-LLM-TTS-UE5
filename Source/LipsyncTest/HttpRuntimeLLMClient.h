#pragma once

#include "CoreMinimal.h"
#include "Interfaces/IHttpRequest.h"
#include "UObject/Object.h"
#include "HttpRuntimeLLMClient.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_FourParams(FOnRuntimeLLMRequestSuccess, const FString&, Method, const FString&, Url, int32, StatusCode, const FString&, ResponseBody);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FOnRuntimeLLMRequestFailure, const FString&, Method, const FString&, Url, const FString&, ErrorMessage);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnRuntimeLLMPromptResponse, const FString&, Prompt, const FString&, Reply);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_FourParams(FOnRuntimeLLMPromptResponseDetailed, const FString&, Prompt, const FString&, Reply, const FString&, RequestId, const FString&, ClientId);

UCLASS(BlueprintType)
class LIPSYNCTEST_API UHttpRuntimeLLMClient : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime LLM")
	FString BaseUrl = TEXT("http://127.0.0.1:8081");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime LLM")
	FString ClientId;

	UPROPERTY(BlueprintAssignable, Category = "Runtime LLM")
	FOnRuntimeLLMRequestSuccess OnRequestSucceeded;

	UPROPERTY(BlueprintAssignable, Category = "Runtime LLM")
	FOnRuntimeLLMRequestFailure OnRequestFailed;

	UPROPERTY(BlueprintAssignable, Category = "Runtime LLM")
	FOnRuntimeLLMPromptResponse OnPromptResponse;

	UPROPERTY(BlueprintAssignable, Category = "Runtime LLM")
	FOnRuntimeLLMPromptResponseDetailed OnPromptResponseDetailed;

	UFUNCTION(BlueprintCallable, meta = (DisplayName = "Create Runtime LLM Client"), Category = "Runtime LLM")
	static UHttpRuntimeLLMClient* CreateRuntimeLLMClient();

	UFUNCTION(BlueprintCallable, Category = "Runtime LLM")
	void SendPing();

	UFUNCTION(BlueprintCallable, Category = "Runtime LLM")
	void StartLLMServer();

	UFUNCTION(BlueprintCallable, Category = "Runtime LLM")
	void StopLLMServer();

	UFUNCTION(BlueprintCallable, Category = "Runtime LLM")
	void GetLLMStatus();

	UFUNCTION(BlueprintCallable, Category = "Runtime LLM")
	void SendPrompt(const FString& Prompt, const FString& SystemPrompt, float Temperature = 0.7f);

private:
	FString BuildUrl(const FString& Route) const;
	void SendJsonRequest(const FString& Method, const FString& Route, const FString& JsonBody);
	void HandleResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful, FString Method, FString Url);
	void HandlePromptResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful, FString Prompt, FString Url, FString RequestId);
};
