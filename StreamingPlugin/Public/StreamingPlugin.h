// Some copyright should be here...

#pragma once

#include "ModuleManager.h"

#include "HttpRequestAdapter.h"
#include "HttpModule.h"
#include "IHttpResponse.h"
#include "Http.h"

DECLARE_LOG_CATEGORY_EXTERN(ModuleLog, Log, All)

class CStreamingPluginModule : public IModuleInterface
{
public:

	/** Methods **/

	/** IModuleInterface implementation */
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	void CStreamingPluginModule::SetUpVideoCapture();
	void CStreamingPluginModule::SetUpPlayer(int ControllerId);
	void CStreamingPluginModule::StreamFrameToClient();
	// Only handle 4 player split screen for current solution
	void CStreamingPluginModule::Split4Player();
	bool CStreamingPluginModule::CheckPlayers(float DeltaTime);
	void CStreamingPluginModule::OnAuthResponseComplete(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful);
	void CStreamingPluginModule::OnGetResponseComplete(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful);
	bool CStreamingPluginModule::GetToken();

	// Timer for capturing frames
	bool CStreamingPluginModule::CaptureFrame(float DeltaTime);

	/** Class variables **/

	int NumberOfPlayers;
	TArray<FILE*> VideoPipeList;
	TArray<TArray<FColor> > FrameBufferList;
	bool isEngineRunning;
	int sizeX, sizeY, halfSizeX, halfSizeY;
	TArray<int> PlayerFrameMapping; // index is frame index, value is player index
	FIntRect Screen1, Screen2, Screen3, Screen4;
	FReadSurfaceDataFlags flags;
	FString AuthToken;


};