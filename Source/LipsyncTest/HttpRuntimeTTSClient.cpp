#include "HttpRuntimeTTSClient.h"

#include "Async/Async.h"
#include "Dom/JsonObject.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/Base64.h"
#include "Misc/Guid.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

UHttpRuntimeTTSClient* UHttpRuntimeTTSClient::CreateRuntimeTTSClient()
{
	UHttpRuntimeTTSClient* Client = NewObject<UHttpRuntimeTTSClient>();
	Client->ClientId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens);
	return Client;
}

void UHttpRuntimeTTSClient::SendPing()
{
	SendJsonRequest(TEXT("GET"), TEXT("/ping"), FString());
}

void UHttpRuntimeTTSClient::GetTTSStatus()
{
	SendJsonRequest(TEXT("GET"), TEXT("/status"), FString());
}

void UHttpRuntimeTTSClient::SendTTSStream(const FString& Text, bool bUseCuda, int32 SpeakerId, int32 ChunkSize)
{
	if (Text.IsEmpty())
	{
		OnRequestFailed.Broadcast(TEXT("POST"), BuildUrl(TEXT("/tts_stream")), TEXT("TTS text is empty."));
		return;
	}

	if (ClientId.IsEmpty())
	{
		ClientId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens);
	}

	struct FTTSStreamState
	{
		int32 ProcessedLen = 0;
		FString Buffer;
		TArray<uint8> AccumulatedAudio;
		FString Text;
		FString RequestId;
		FString ClientId;
		int32 SampleRate = 0;
		int32 NumChannels = 1;
		int32 LastChunkIndex = -1;
		bool bStarted = false;
		bool bSawError = false;
	};

	const FString RequestId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens);
	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("text"), Text);
	Payload->SetStringField(TEXT("client_id"), ClientId);
	Payload->SetStringField(TEXT("request_id"), RequestId);
	Payload->SetBoolField(TEXT("use_cuda"), bUseCuda);
	Payload->SetNumberField(TEXT("speaker_id"), SpeakerId);
	Payload->SetNumberField(TEXT("chunk_size"), FMath::Max(512, ChunkSize));

	FString JsonBody;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonBody);
	FJsonSerializer::Serialize(Payload, Writer);

	const FString Url = BuildUrl(TEXT("/tts_stream"));
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(Url);
	Request->SetVerb(TEXT("POST"));
	Request->SetHeader(TEXT("Accept"), TEXT("text/event-stream"));
	Request->SetHeader(TEXT("Content-Type"), TEXT("application/json; charset=utf-8"));
	Request->SetContentAsString(JsonBody);

	TWeakObjectPtr<UHttpRuntimeTTSClient> WeakThis(this);
	const TSharedRef<FTTSStreamState, ESPMode::ThreadSafe> StreamState = MakeShared<FTTSStreamState, ESPMode::ThreadSafe>();
	StreamState->Text = Text;
	StreamState->RequestId = RequestId;
	StreamState->ClientId = ClientId;

	const auto ProcessStreamBuffer = [WeakThis, StreamState, Url]()
	{
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
			if (Data.IsEmpty() || Data == TEXT("[DONE]"))
			{
				continue;
			}

			TSharedPtr<FJsonObject> Root;
			const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Data);
			if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
			{
				continue;
			}

			FString Type;
			Root->TryGetStringField(TEXT("type"), Type);
			if (Type.IsEmpty() || !WeakThis.IsValid())
			{
				continue;
			}

			UHttpRuntimeTTSClient* Client = WeakThis.Get();
			Root->TryGetStringField(TEXT("request_id"), StreamState->RequestId);
			Root->TryGetStringField(TEXT("client_id"), StreamState->ClientId);

			double SampleRateValue = StreamState->SampleRate;
			if (Root->TryGetNumberField(TEXT("sample_rate"), SampleRateValue))
			{
				StreamState->SampleRate = static_cast<int32>(SampleRateValue);
			}
			double NumChannelsValue = StreamState->NumChannels;
			if (Root->TryGetNumberField(TEXT("num_channels"), NumChannelsValue))
			{
				StreamState->NumChannels = static_cast<int32>(NumChannelsValue);
			}

			if (Type == TEXT("start"))
			{
				StreamState->bStarted = true;
				const FString TextCopy = StreamState->Text;
				const int32 SampleRateCopy = StreamState->SampleRate;
				const int32 NumChannelsCopy = StreamState->NumChannels;
				const FString RequestIdCopy = StreamState->RequestId;
				const FString ClientIdCopy = StreamState->ClientId;
				AsyncTask(ENamedThreads::GameThread, [WeakThis, TextCopy, SampleRateCopy, NumChannelsCopy, RequestIdCopy, ClientIdCopy]()
				{
					if (!WeakThis.IsValid())
					{
						return;
					}
					WeakThis.Get()->OnTTSStreamStarted.Broadcast(TextCopy, SampleRateCopy, NumChannelsCopy, RequestIdCopy, ClientIdCopy);
				});
				continue;
			}

			if (Type == TEXT("audio_chunk"))
			{
				FString ChunkBase64;
				if (!Root->TryGetStringField(TEXT("chunk_base64"), ChunkBase64))
				{
					continue;
				}

				TArray<uint8> DecodedChunk;
				if (!FBase64::Decode(ChunkBase64, DecodedChunk))
				{
					StreamState->bSawError = true;
					const FString UrlCopy = Url;
					AsyncTask(ENamedThreads::GameThread, [WeakThis, UrlCopy]()
					{
						if (!WeakThis.IsValid())
						{
							return;
						}
						WeakThis.Get()->OnRequestFailed.Broadcast(TEXT("POST"), UrlCopy, TEXT("Failed to decode TTS audio chunk."));
					});
					return;
				}

				int32 ChunkIndex = StreamState->LastChunkIndex + 1;
				double ChunkIndexValue = ChunkIndex;
				if (Root->TryGetNumberField(TEXT("chunk_index"), ChunkIndexValue))
				{
					ChunkIndex = static_cast<int32>(ChunkIndexValue);
				}

				StreamState->LastChunkIndex = ChunkIndex;
				StreamState->AccumulatedAudio.Append(DecodedChunk);
				const FString TextCopy = StreamState->Text;
				const int32 SampleRateCopy = StreamState->SampleRate;
				const int32 NumChannelsCopy = StreamState->NumChannels;
				const FString RequestIdCopy = StreamState->RequestId;
				const FString ClientIdCopy = StreamState->ClientId;
				const TArray<uint8> ChunkCopy = DecodedChunk;
				AsyncTask(ENamedThreads::GameThread, [WeakThis, TextCopy, ChunkCopy, SampleRateCopy, NumChannelsCopy, ChunkIndex, RequestIdCopy, ClientIdCopy]()
				{
					if (!WeakThis.IsValid())
					{
						return;
					}
					WeakThis.Get()->OnTTSStreamChunk.Broadcast(
						TextCopy,
						ChunkCopy,
						SampleRateCopy,
						NumChannelsCopy,
						ERuntimeRAWAudioFormat::Int16,
						ChunkIndex,
						RequestIdCopy,
						ClientIdCopy
					);
				});
				continue;
			}

			if (Type == TEXT("error"))
			{
				StreamState->bSawError = true;
				FString ErrorMessage = TEXT("TTS stream error.");
				Root->TryGetStringField(TEXT("error"), ErrorMessage);
				const FString UrlCopy = Url;
				AsyncTask(ENamedThreads::GameThread, [WeakThis, UrlCopy, ErrorMessage]()
				{
					if (!WeakThis.IsValid())
					{
						return;
					}
					WeakThis.Get()->OnRequestFailed.Broadcast(TEXT("POST"), UrlCopy, ErrorMessage);
				});
			}
		}
	};

	Request->OnRequestProgress64().BindLambda([ProcessStreamBuffer, StreamState](FHttpRequestPtr Req, int32 BytesSent, int32 BytesReceived)
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
		ProcessStreamBuffer();
	});

	Request->OnProcessRequestComplete().BindLambda([WeakThis, StreamState, ProcessStreamBuffer, Url](FHttpRequestPtr Req, FHttpResponsePtr Resp, bool bWasSuccessful)
	{
		if (!WeakThis.IsValid())
		{
			return;
		}

		UHttpRuntimeTTSClient* Client = WeakThis.Get();
		if (Resp.IsValid())
		{
			const FString Full = Resp->GetContentAsString();
			if (Full.Len() > StreamState->ProcessedLen)
			{
				const FString NewChunk = Full.Mid(StreamState->ProcessedLen);
				StreamState->ProcessedLen = Full.Len();
				StreamState->Buffer += NewChunk;
				ProcessStreamBuffer();
			}
		}

		if (StreamState->bSawError)
		{
			return;
		}

		// Treat streamed SSE as successful if we received valid audio data, even if the HTTP layer
		// reports false when the server closes the connection after completing the stream.
		if ((!bWasSuccessful || !Resp.IsValid()) && (!StreamState->bStarted || StreamState->AccumulatedAudio.Num() == 0))
		{
			const FString UrlCopy = Url;
			AsyncTask(ENamedThreads::GameThread, [WeakThis, UrlCopy]()
			{
				if (!WeakThis.IsValid())
				{
					return;
				}
				WeakThis.Get()->OnRequestFailed.Broadcast(TEXT("POST"), UrlCopy, TEXT("TTS stream request failed or server unavailable."));
			});
			return;
		}

		const int32 StatusCode = Resp.IsValid() ? Resp->GetResponseCode() : 200;
		const FString ResponseBody = Resp.IsValid() ? Resp->GetContentAsString() : FString();
		if (Resp.IsValid() && !EHttpResponseCodes::IsOk(StatusCode) && StreamState->AccumulatedAudio.Num() == 0)
		{
			const FString UrlCopy = Url;
			const FString ErrorCopy = FString::Printf(TEXT("HTTP %d: %s"), StatusCode, *ResponseBody);
			AsyncTask(ENamedThreads::GameThread, [WeakThis, UrlCopy, ErrorCopy]()
			{
				if (!WeakThis.IsValid())
				{
					return;
				}
				WeakThis.Get()->OnRequestFailed.Broadcast(TEXT("POST"), UrlCopy, ErrorCopy);
			});
			return;
		}

		if (!StreamState->bStarted || StreamState->AccumulatedAudio.Num() == 0)
		{
			const FString UrlCopy = Url;
			AsyncTask(ENamedThreads::GameThread, [WeakThis, UrlCopy]()
			{
				if (!WeakThis.IsValid())
				{
					return;
				}
				WeakThis.Get()->OnRequestFailed.Broadcast(TEXT("POST"), UrlCopy, TEXT("TTS stream completed without audio data."));
			});
			return;
		}

		const FString TextCopy = StreamState->Text;
		const TArray<uint8> AudioCopy = StreamState->AccumulatedAudio;
		const int32 SampleRateCopy = StreamState->SampleRate;
		const int32 NumChannelsCopy = StreamState->NumChannels;
		const FString RequestIdCopy = StreamState->RequestId;
		const FString ClientIdCopy = StreamState->ClientId;
		const FString UrlCopy = Url;
		AsyncTask(ENamedThreads::GameThread, [WeakThis, TextCopy, AudioCopy, SampleRateCopy, NumChannelsCopy, RequestIdCopy, ClientIdCopy, UrlCopy, StatusCode, ResponseBody]()
		{
			if (!WeakThis.IsValid())
			{
				return;
			}

			UHttpRuntimeTTSClient* ClientOnGameThread = WeakThis.Get();
			ClientOnGameThread->OnRequestSucceeded.Broadcast(TEXT("POST"), UrlCopy, StatusCode, ResponseBody);
			ClientOnGameThread->OnTTSStreamCompleted.Broadcast(TextCopy, AudioCopy, SampleRateCopy, NumChannelsCopy, RequestIdCopy, ClientIdCopy);
			ClientOnGameThread->OnTTSSpeechResult.Broadcast(TextCopy, AudioCopy, SampleRateCopy, NumChannelsCopy, ERuntimeRAWAudioFormat::Int16);
			ClientOnGameThread->OnSpeechResult.Broadcast(TextCopy, AudioCopy, SampleRateCopy, NumChannelsCopy, ERuntimeRAWAudioFormat::Int16);
		});
	});

	Request->ProcessRequest();
}

FString UHttpRuntimeTTSClient::BuildUrl(const FString& Route) const
{
	const FString TrimmedBase = BaseUrl.EndsWith(TEXT("/")) ? BaseUrl.LeftChop(1) : BaseUrl;
	if (Route.IsEmpty())
	{
		return TrimmedBase;
	}

	return Route.StartsWith(TEXT("/")) ? TrimmedBase + Route : TrimmedBase + TEXT("/") + Route;
}

void UHttpRuntimeTTSClient::SendJsonRequest(const FString& Method, const FString& Route, const FString& JsonBody)
{
	const FString UpperMethod = Method.ToUpper();
	const FString Url = BuildUrl(Route);

	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(Url);
	Request->SetVerb(UpperMethod);
	Request->SetHeader(TEXT("Accept"), TEXT("application/json"));
	Request->SetHeader(TEXT("Content-Type"), TEXT("application/json; charset=utf-8"));

	if (!JsonBody.IsEmpty() && UpperMethod != TEXT("GET"))
	{
		Request->SetContentAsString(JsonBody);
	}

	Request->OnProcessRequestComplete().BindUObject(this, &UHttpRuntimeTTSClient::HandleResponse, UpperMethod, Url);
	Request->ProcessRequest();
}

void UHttpRuntimeTTSClient::HandleResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful, FString Method, FString Url)
{
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
