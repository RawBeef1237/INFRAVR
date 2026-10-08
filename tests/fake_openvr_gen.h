// generated from openvr.h: default stubs
struct Base_IVRSystem : public vr::IVRSystem {
  void GetRecommendedRenderTargetSize( uint32_t *pnWidth, uint32_t *pnHeight ) override {}
  HmdMatrix44_t GetProjectionMatrix( EVREye eEye, float fNearZ, float fFarZ ) override { return {}; }
  void GetProjectionRaw( EVREye eEye, float *pfLeft, float *pfRight, float *pfTop, float *pfBottom ) override {}
  bool ComputeDistortion( EVREye eEye, float fU, float fV, DistortionCoordinates_t *pDistortionCoordinates ) override { return {}; }
  HmdMatrix34_t GetEyeToHeadTransform( EVREye eEye ) override { return {}; }
  bool GetTimeSinceLastVsync( float *pfSecondsSinceLastVsync, uint64_t *pulFrameCounter ) override { return {}; }
  int32_t GetD3D9AdapterIndex() override { return {}; }
  void GetDXGIOutputInfo( int32_t *pnAdapterIndex ) override {}
  void GetOutputDevice( uint64_t *pnDevice, ETextureType textureType, VkInstance_T *pInstance) override {}
  bool IsDisplayOnDesktop() override { return {}; }
  bool SetDisplayVisibility( bool bIsVisibleOnDesktop ) override { return {}; }
  void GetDeviceToAbsoluteTrackingPose( ETrackingUniverseOrigin eOrigin, float fPredictedSecondsToPhotonsFromNow, VR_ARRAY_COUNT(unTrackedDevicePoseArrayCount) TrackedDevicePose_t *pTrackedDevicePoseArray, uint32_t unTrackedDevicePoseArrayCount ) override {}
  HmdMatrix34_t GetSeatedZeroPoseToStandingAbsoluteTrackingPose() override { return {}; }
  HmdMatrix34_t GetRawZeroPoseToStandingAbsoluteTrackingPose() override { return {}; }
  uint32_t GetSortedTrackedDeviceIndicesOfClass( ETrackedDeviceClass eTrackedDeviceClass, VR_ARRAY_COUNT(unTrackedDeviceIndexArrayCount) vr::TrackedDeviceIndex_t *punTrackedDeviceIndexArray, uint32_t unTrackedDeviceIndexArrayCount, vr::TrackedDeviceIndex_t unRelativeToTrackedDeviceIndex) override { return {}; }
  EDeviceActivityLevel GetTrackedDeviceActivityLevel( vr::TrackedDeviceIndex_t unDeviceId ) override { return {}; }
  void ApplyTransform( TrackedDevicePose_t *pOutputPose, const TrackedDevicePose_t *pTrackedDevicePose, const HmdMatrix34_t *pTransform ) override {}
  vr::TrackedDeviceIndex_t GetTrackedDeviceIndexForControllerRole( vr::ETrackedControllerRole unDeviceType ) override { return {}; }
  vr::ETrackedControllerRole GetControllerRoleForTrackedDeviceIndex( vr::TrackedDeviceIndex_t unDeviceIndex ) override { return {}; }
  ETrackedDeviceClass GetTrackedDeviceClass( vr::TrackedDeviceIndex_t unDeviceIndex ) override { return {}; }
  bool IsTrackedDeviceConnected( vr::TrackedDeviceIndex_t unDeviceIndex ) override { return {}; }
  bool GetBoolTrackedDeviceProperty( vr::TrackedDeviceIndex_t unDeviceIndex, ETrackedDeviceProperty prop, ETrackedPropertyError *pError) override { return {}; }
  float GetFloatTrackedDeviceProperty( vr::TrackedDeviceIndex_t unDeviceIndex, ETrackedDeviceProperty prop, ETrackedPropertyError *pError) override { return {}; }
  int32_t GetInt32TrackedDeviceProperty( vr::TrackedDeviceIndex_t unDeviceIndex, ETrackedDeviceProperty prop, ETrackedPropertyError *pError) override { return {}; }
  uint64_t GetUint64TrackedDeviceProperty( vr::TrackedDeviceIndex_t unDeviceIndex, ETrackedDeviceProperty prop, ETrackedPropertyError *pError) override { return {}; }
  HmdMatrix34_t GetMatrix34TrackedDeviceProperty( vr::TrackedDeviceIndex_t unDeviceIndex, ETrackedDeviceProperty prop, ETrackedPropertyError *pError) override { return {}; }
  uint32_t GetArrayTrackedDeviceProperty( vr::TrackedDeviceIndex_t unDeviceIndex, ETrackedDeviceProperty prop, PropertyTypeTag_t propType, void *pBuffer, uint32_t unBufferSize, ETrackedPropertyError *pError) override { return {}; }
  uint32_t GetStringTrackedDeviceProperty( vr::TrackedDeviceIndex_t unDeviceIndex, ETrackedDeviceProperty prop, VR_OUT_STRING() char *pchValue, uint32_t unBufferSize, ETrackedPropertyError *pError) override { return {}; }
  const char * GetPropErrorNameFromEnum( ETrackedPropertyError error ) override { return {}; }
  bool PollNextEvent( VREvent_t *pEvent, uint32_t uncbVREvent ) override { return {}; }
  bool PollNextEventWithPose( ETrackingUniverseOrigin eOrigin, VREvent_t *pEvent, uint32_t uncbVREvent, vr::TrackedDevicePose_t *pTrackedDevicePose ) override { return {}; }
  const char * GetEventTypeNameFromEnum( EVREventType eType ) override { return {}; }
  HiddenAreaMesh_t GetHiddenAreaMesh( EVREye eEye, EHiddenAreaMeshType type) override { return {}; }
  bool GetControllerState( vr::TrackedDeviceIndex_t unControllerDeviceIndex, vr::VRControllerState_t *pControllerState, uint32_t unControllerStateSize ) override { return {}; }
  bool GetControllerStateWithPose( ETrackingUniverseOrigin eOrigin, vr::TrackedDeviceIndex_t unControllerDeviceIndex, vr::VRControllerState_t *pControllerState, uint32_t unControllerStateSize, TrackedDevicePose_t *pTrackedDevicePose ) override { return {}; }
  void TriggerHapticPulse( vr::TrackedDeviceIndex_t unControllerDeviceIndex, uint32_t unAxisId, unsigned short usDurationMicroSec ) override {}
  const char * GetButtonIdNameFromEnum( EVRButtonId eButtonId ) override { return {}; }
  const char * GetControllerAxisTypeNameFromEnum( EVRControllerAxisType eAxisType ) override { return {}; }
  bool IsInputAvailable() override { return {}; }
  bool IsSteamVRDrawingControllers() override { return {}; }
  bool ShouldApplicationPause() override { return {}; }
  bool ShouldApplicationReduceRenderingWork() override { return {}; }
  vr::EVRFirmwareError PerformFirmwareUpdate( vr::TrackedDeviceIndex_t unDeviceIndex ) override { return {}; }
  void AcknowledgeQuit_Exiting() override {}
  uint32_t GetAppContainerFilePaths( VR_OUT_STRING() char *pchBuffer, uint32_t unBufferSize ) override { return {}; }
  const char * GetRuntimeVersion() override { return {}; }
};
struct Base_IVRCompositor : public vr::IVRCompositor {
  void SetTrackingSpace( ETrackingUniverseOrigin eOrigin ) override {}
  ETrackingUniverseOrigin GetTrackingSpace() override { return {}; }
  EVRCompositorError WaitGetPoses( VR_ARRAY_COUNT( unRenderPoseArrayCount ) TrackedDevicePose_t* pRenderPoseArray, uint32_t unRenderPoseArrayCount, VR_ARRAY_COUNT( unGamePoseArrayCount ) TrackedDevicePose_t* pGamePoseArray, uint32_t unGamePoseArrayCount ) override { return {}; }
  EVRCompositorError GetLastPoses( VR_ARRAY_COUNT( unRenderPoseArrayCount ) TrackedDevicePose_t* pRenderPoseArray, uint32_t unRenderPoseArrayCount, VR_ARRAY_COUNT( unGamePoseArrayCount ) TrackedDevicePose_t* pGamePoseArray, uint32_t unGamePoseArrayCount ) override { return {}; }
  EVRCompositorError GetLastPoseForTrackedDeviceIndex( TrackedDeviceIndex_t unDeviceIndex, TrackedDevicePose_t *pOutputPose, TrackedDevicePose_t *pOutputGamePose ) override { return {}; }
  EVRCompositorError Submit( EVREye eEye, const Texture_t *pTexture, const VRTextureBounds_t* pBounds, EVRSubmitFlags nSubmitFlags) override { return {}; }
  EVRCompositorError SubmitWithArrayIndex( EVREye eEye, const Texture_t *pTexture, uint32_t unTextureArrayIndex, const VRTextureBounds_t *pBounds, EVRSubmitFlags nSubmitFlags) override { return {}; }
  void ClearLastSubmittedFrame() override {}
  void PostPresentHandoff() override {}
  bool GetFrameTiming( Compositor_FrameTiming *pTiming, uint32_t unFramesAgo) override { return {}; }
  uint32_t GetFrameTimings( VR_ARRAY_COUNT( nFrames ) Compositor_FrameTiming *pTiming, uint32_t nFrames ) override { return {}; }
  float GetFrameTimeRemaining() override { return {}; }
  void GetCumulativeStats( Compositor_CumulativeStats *pStats, uint32_t nStatsSizeInBytes ) override {}
  void FadeToColor( float fSeconds, float fRed, float fGreen, float fBlue, float fAlpha, bool bBackground) override {}
  HmdColor_t GetCurrentFadeColor( bool bBackground) override { return {}; }
  void FadeGrid( float fSeconds, bool bFadeGridIn ) override {}
  float GetCurrentGridAlpha() override { return {}; }
  EVRCompositorError SetSkyboxOverride( VR_ARRAY_COUNT( unTextureCount ) const Texture_t *pTextures, uint32_t unTextureCount ) override { return {}; }
  void ClearSkyboxOverride() override {}
  void CompositorBringToFront() override {}
  void CompositorGoToBack() override {}
  void CompositorQuit() override {}
  bool IsFullscreen() override { return {}; }
  uint32_t GetCurrentSceneFocusProcess() override { return {}; }
  uint32_t GetLastFrameRenderer() override { return {}; }
  bool CanRenderScene() override { return {}; }
  void ShowMirrorWindow() override {}
  void HideMirrorWindow() override {}
  bool IsMirrorWindowVisible() override { return {}; }
  void CompositorDumpImages() override {}
  bool ShouldAppRenderWithLowResources() override { return {}; }
  void ForceInterleavedReprojectionOn( bool bOverride ) override {}
  void ForceReconnectProcess() override {}
  void SuspendRendering( bool bSuspend ) override {}
  vr::EVRCompositorError GetMirrorTextureD3D11( vr::EVREye eEye, void *pD3D11DeviceOrResource, void **ppD3D11ShaderResourceView ) override { return {}; }
  void ReleaseMirrorTextureD3D11( void *pD3D11ShaderResourceView ) override {}
  vr::EVRCompositorError GetMirrorTextureGL( vr::EVREye eEye, vr::glUInt_t *pglTextureId, vr::glSharedTextureHandle_t *pglSharedTextureHandle ) override { return {}; }
  bool ReleaseSharedGLTexture( vr::glUInt_t glTextureId, vr::glSharedTextureHandle_t glSharedTextureHandle ) override { return {}; }
  void LockGLSharedTextureForAccess( vr::glSharedTextureHandle_t glSharedTextureHandle ) override {}
  void UnlockGLSharedTextureForAccess( vr::glSharedTextureHandle_t glSharedTextureHandle ) override {}
  uint32_t GetVulkanInstanceExtensionsRequired( VR_OUT_STRING() char *pchValue, uint32_t unBufferSize ) override { return {}; }
  uint32_t GetVulkanDeviceExtensionsRequired( VkPhysicalDevice_T *pPhysicalDevice, VR_OUT_STRING() char *pchValue, uint32_t unBufferSize ) override { return {}; }
  void SetExplicitTimingMode( EVRCompositorTimingMode eTimingMode ) override {}
  EVRCompositorError SubmitExplicitTimingData() override { return {}; }
  bool IsMotionSmoothingEnabled() override { return {}; }
  bool IsMotionSmoothingSupported() override { return {}; }
  bool IsCurrentSceneFocusAppLoading() override { return {}; }
  EVRCompositorError SetStageOverride_Async( const char *pchRenderModelPath, const HmdMatrix34_t *pTransform, const Compositor_StageRenderSettings *pRenderSettings, uint32_t nSizeOfRenderSettings) override { return {}; }
  void ClearStageOverride() override {}
  bool GetCompositorBenchmarkResults( Compositor_BenchmarkResults *pBenchmarkResults, uint32_t nSizeOfBenchmarkResults ) override { return {}; }
  EVRCompositorError GetLastPosePredictionIDs( uint32_t *pRenderPosePredictionID, uint32_t *pGamePosePredictionID ) override { return {}; }
  EVRCompositorError GetPosesForFrame( uint32_t unPosePredictionID, VR_ARRAY_COUNT( unPoseArrayCount ) TrackedDevicePose_t* pPoseArray, uint32_t unPoseArrayCount ) override { return {}; }
};
struct Base_IVROverlay : public vr::IVROverlay {
  EVROverlayError FindOverlay( const char *pchOverlayKey, VROverlayHandle_t * pOverlayHandle ) override { return {}; }
  EVROverlayError CreateOverlay( const char *pchOverlayKey, const char *pchOverlayName, VROverlayHandle_t * pOverlayHandle ) override { return {}; }
  EVROverlayError DestroyOverlay( VROverlayHandle_t ulOverlayHandle ) override { return {}; }
  uint32_t GetOverlayKey( VROverlayHandle_t ulOverlayHandle, VR_OUT_STRING() char *pchValue, uint32_t unBufferSize, EVROverlayError *pError) override { return {}; }
  uint32_t GetOverlayName( VROverlayHandle_t ulOverlayHandle, VR_OUT_STRING() char *pchValue, uint32_t unBufferSize, EVROverlayError *pError) override { return {}; }
  EVROverlayError SetOverlayName( VROverlayHandle_t ulOverlayHandle, const char *pchName ) override { return {}; }
  EVROverlayError GetOverlayImageData( VROverlayHandle_t ulOverlayHandle, void *pvBuffer, uint32_t unBufferSize, uint32_t *punWidth, uint32_t *punHeight ) override { return {}; }
  const char * GetOverlayErrorNameFromEnum( EVROverlayError error ) override { return {}; }
  EVROverlayError SetOverlayRenderingPid( VROverlayHandle_t ulOverlayHandle, uint32_t unPID ) override { return {}; }
  uint32_t GetOverlayRenderingPid( VROverlayHandle_t ulOverlayHandle ) override { return {}; }
  EVROverlayError SetOverlayFlag( VROverlayHandle_t ulOverlayHandle, VROverlayFlags eOverlayFlag, bool bEnabled ) override { return {}; }
  EVROverlayError GetOverlayFlag( VROverlayHandle_t ulOverlayHandle, VROverlayFlags eOverlayFlag, bool *pbEnabled ) override { return {}; }
  EVROverlayError GetOverlayFlags( VROverlayHandle_t ulOverlayHandle, uint32_t *pFlags ) override { return {}; }
  EVROverlayError SetOverlayColor( VROverlayHandle_t ulOverlayHandle, float fRed, float fGreen, float fBlue ) override { return {}; }
  EVROverlayError GetOverlayColor( VROverlayHandle_t ulOverlayHandle, float *pfRed, float *pfGreen, float *pfBlue ) override { return {}; }
  EVROverlayError SetOverlayAlpha( VROverlayHandle_t ulOverlayHandle, float fAlpha ) override { return {}; }
  EVROverlayError GetOverlayAlpha( VROverlayHandle_t ulOverlayHandle, float *pfAlpha ) override { return {}; }
  EVROverlayError SetOverlayTexelAspect( VROverlayHandle_t ulOverlayHandle, float fTexelAspect ) override { return {}; }
  EVROverlayError GetOverlayTexelAspect( VROverlayHandle_t ulOverlayHandle, float *pfTexelAspect ) override { return {}; }
  EVROverlayError SetOverlaySortOrder( VROverlayHandle_t ulOverlayHandle, uint32_t unSortOrder ) override { return {}; }
  EVROverlayError GetOverlaySortOrder( VROverlayHandle_t ulOverlayHandle, uint32_t *punSortOrder ) override { return {}; }
  EVROverlayError SetOverlayWidthInMeters( VROverlayHandle_t ulOverlayHandle, float fWidthInMeters ) override { return {}; }
  EVROverlayError GetOverlayWidthInMeters( VROverlayHandle_t ulOverlayHandle, float *pfWidthInMeters ) override { return {}; }
  EVROverlayError SetOverlayCurvature( VROverlayHandle_t ulOverlayHandle, float fCurvature ) override { return {}; }
  EVROverlayError GetOverlayCurvature( VROverlayHandle_t ulOverlayHandle, float *pfCurvature ) override { return {}; }
  EVROverlayError SetOverlayPreCurvePitch( VROverlayHandle_t ulOverlayHandle, float fRadians ) override { return {}; }
  EVROverlayError GetOverlayPreCurvePitch( VROverlayHandle_t ulOverlayHandle, float *pfRadians ) override { return {}; }
  EVROverlayError SetOverlayTextureColorSpace( VROverlayHandle_t ulOverlayHandle, EColorSpace eTextureColorSpace ) override { return {}; }
  EVROverlayError GetOverlayTextureColorSpace( VROverlayHandle_t ulOverlayHandle, EColorSpace *peTextureColorSpace ) override { return {}; }
  EVROverlayError SetOverlayTextureBounds( VROverlayHandle_t ulOverlayHandle, const VRTextureBounds_t *pOverlayTextureBounds ) override { return {}; }
  EVROverlayError GetOverlayTextureBounds( VROverlayHandle_t ulOverlayHandle, VRTextureBounds_t *pOverlayTextureBounds ) override { return {}; }
  EVROverlayError GetOverlayTransformType( VROverlayHandle_t ulOverlayHandle, VROverlayTransformType *peTransformType ) override { return {}; }
  EVROverlayError SetOverlayTransformAbsolute( VROverlayHandle_t ulOverlayHandle, ETrackingUniverseOrigin eTrackingOrigin, const HmdMatrix34_t *pmatTrackingOriginToOverlayTransform ) override { return {}; }
  EVROverlayError GetOverlayTransformAbsolute( VROverlayHandle_t ulOverlayHandle, ETrackingUniverseOrigin *peTrackingOrigin, HmdMatrix34_t *pmatTrackingOriginToOverlayTransform ) override { return {}; }
  EVROverlayError SetOverlayTransformTrackedDeviceRelative( VROverlayHandle_t ulOverlayHandle, TrackedDeviceIndex_t unTrackedDevice, const HmdMatrix34_t *pmatTrackedDeviceToOverlayTransform ) override { return {}; }
  EVROverlayError GetOverlayTransformTrackedDeviceRelative( VROverlayHandle_t ulOverlayHandle, TrackedDeviceIndex_t *punTrackedDevice, HmdMatrix34_t *pmatTrackedDeviceToOverlayTransform ) override { return {}; }
  EVROverlayError SetOverlayTransformTrackedDeviceComponent( VROverlayHandle_t ulOverlayHandle, TrackedDeviceIndex_t unDeviceIndex, const char *pchComponentName ) override { return {}; }
  EVROverlayError GetOverlayTransformTrackedDeviceComponent( VROverlayHandle_t ulOverlayHandle, TrackedDeviceIndex_t *punDeviceIndex, VR_OUT_STRING() char *pchComponentName, uint32_t unComponentNameSize ) override { return {}; }
  EVROverlayError SetOverlayTransformCursor( VROverlayHandle_t ulCursorOverlayHandle, const HmdVector2_t *pvHotspot ) override { return {}; }
  vr::EVROverlayError GetOverlayTransformCursor( VROverlayHandle_t ulOverlayHandle, HmdVector2_t *pvHotspot ) override { return {}; }
  vr::EVROverlayError SetOverlayTransformProjection( VROverlayHandle_t ulOverlayHandle, ETrackingUniverseOrigin eTrackingOrigin, const HmdMatrix34_t* pmatTrackingOriginToOverlayTransform, const VROverlayProjection_t *pProjection, vr::EVREye eEye ) override { return {}; }
  EVROverlayError ShowOverlay( VROverlayHandle_t ulOverlayHandle ) override { return {}; }
  EVROverlayError HideOverlay( VROverlayHandle_t ulOverlayHandle ) override { return {}; }
  bool IsOverlayVisible( VROverlayHandle_t ulOverlayHandle ) override { return {}; }
  EVROverlayError GetTransformForOverlayCoordinates( VROverlayHandle_t ulOverlayHandle, ETrackingUniverseOrigin eTrackingOrigin, HmdVector2_t coordinatesInOverlay, HmdMatrix34_t *pmatTransform ) override { return {}; }
  EVROverlayError WaitFrameSync( uint32_t nTimeoutMs ) override { return {}; }
  bool PollNextOverlayEvent( VROverlayHandle_t ulOverlayHandle, VREvent_t *pEvent, uint32_t uncbVREvent ) override { return {}; }
  EVROverlayError GetOverlayInputMethod( VROverlayHandle_t ulOverlayHandle, VROverlayInputMethod *peInputMethod ) override { return {}; }
  EVROverlayError SetOverlayInputMethod( VROverlayHandle_t ulOverlayHandle, VROverlayInputMethod eInputMethod ) override { return {}; }
  EVROverlayError GetOverlayMouseScale( VROverlayHandle_t ulOverlayHandle, HmdVector2_t *pvecMouseScale ) override { return {}; }
  EVROverlayError SetOverlayMouseScale( VROverlayHandle_t ulOverlayHandle, const HmdVector2_t *pvecMouseScale ) override { return {}; }
  bool ComputeOverlayIntersection( VROverlayHandle_t ulOverlayHandle, const VROverlayIntersectionParams_t *pParams, VROverlayIntersectionResults_t *pResults ) override { return {}; }
  bool IsHoverTargetOverlay( VROverlayHandle_t ulOverlayHandle ) override { return {}; }
  EVROverlayError SetOverlayIntersectionMask( VROverlayHandle_t ulOverlayHandle, VROverlayIntersectionMaskPrimitive_t *pMaskPrimitives, uint32_t unNumMaskPrimitives, uint32_t unPrimitiveSize) override { return {}; }
  EVROverlayError TriggerLaserMouseHapticVibration( VROverlayHandle_t ulOverlayHandle, float fDurationSeconds, float fFrequency, float fAmplitude ) override { return {}; }
  EVROverlayError SetOverlayCursor( VROverlayHandle_t ulOverlayHandle, VROverlayHandle_t ulCursorHandle ) override { return {}; }
  EVROverlayError SetOverlayCursorPositionOverride( VROverlayHandle_t ulOverlayHandle, const HmdVector2_t *pvCursor ) override { return {}; }
  EVROverlayError ClearOverlayCursorPositionOverride( VROverlayHandle_t ulOverlayHandle ) override { return {}; }
  EVROverlayError SetOverlayTexture( VROverlayHandle_t ulOverlayHandle, const Texture_t *pTexture ) override { return {}; }
  EVROverlayError ClearOverlayTexture( VROverlayHandle_t ulOverlayHandle ) override { return {}; }
  EVROverlayError SetOverlayRaw( VROverlayHandle_t ulOverlayHandle, void *pvBuffer, uint32_t unWidth, uint32_t unHeight, uint32_t unBytesPerPixel ) override { return {}; }
  EVROverlayError SetOverlayFromFile( VROverlayHandle_t ulOverlayHandle, const char *pchFilePath ) override { return {}; }
  EVROverlayError GetOverlayTexture( VROverlayHandle_t ulOverlayHandle, void **pNativeTextureHandle, void *pNativeTextureRef, uint32_t *pWidth, uint32_t *pHeight, uint32_t *pNativeFormat, ETextureType *pAPIType, EColorSpace *pColorSpace, VRTextureBounds_t *pTextureBounds ) override { return {}; }
  EVROverlayError ReleaseNativeOverlayHandle( VROverlayHandle_t ulOverlayHandle, void *pNativeTextureHandle ) override { return {}; }
  EVROverlayError GetOverlayTextureSize( VROverlayHandle_t ulOverlayHandle, uint32_t *pWidth, uint32_t *pHeight ) override { return {}; }
  EVROverlayError CreateDashboardOverlay( const char *pchOverlayKey, const char *pchOverlayFriendlyName, VROverlayHandle_t * pMainHandle, VROverlayHandle_t *pThumbnailHandle ) override { return {}; }
  bool IsDashboardVisible() override { return {}; }
  bool IsActiveDashboardOverlay( VROverlayHandle_t ulOverlayHandle ) override { return {}; }
  EVROverlayError SetDashboardOverlaySceneProcess( VROverlayHandle_t ulOverlayHandle, uint32_t unProcessId ) override { return {}; }
  EVROverlayError GetDashboardOverlaySceneProcess( VROverlayHandle_t ulOverlayHandle, uint32_t *punProcessId ) override { return {}; }
  void ShowDashboard( const char *pchOverlayToShow ) override {}
  vr::TrackedDeviceIndex_t GetPrimaryDashboardDevice() override { return {}; }
  EVROverlayError ShowKeyboard( EGamepadTextInputMode eInputMode, EGamepadTextInputLineMode eLineInputMode, uint32_t unFlags, const char *pchDescription, uint32_t unCharMax, const char *pchExistingText, uint64_t uUserValue ) override { return {}; }
  EVROverlayError ShowKeyboardForOverlay( VROverlayHandle_t ulOverlayHandle, EGamepadTextInputMode eInputMode, EGamepadTextInputLineMode eLineInputMode, uint32_t unFlags, const char *pchDescription, uint32_t unCharMax, const char *pchExistingText, uint64_t uUserValue ) override { return {}; }
  uint32_t GetKeyboardText( VR_OUT_STRING() char *pchText, uint32_t cchText ) override { return {}; }
  void HideKeyboard() override {}
  void SetKeyboardTransformAbsolute( ETrackingUniverseOrigin eTrackingOrigin, const HmdMatrix34_t *pmatTrackingOriginToKeyboardTransform ) override {}
  void SetKeyboardPositionForOverlay( VROverlayHandle_t ulOverlayHandle, HmdRect2_t avoidRect ) override {}
  VRMessageOverlayResponse ShowMessageOverlay( const char* pchText, const char* pchCaption, const char* pchButton0Text, const char* pchButton1Text, const char* pchButton2Text, const char* pchButton3Text) override { return {}; }
  void CloseMessageOverlay() override {}
};
struct Base_IVRInput : public vr::IVRInput {
  EVRInputError SetActionManifestPath( const char *pchActionManifestPath ) override { return {}; }
  EVRInputError GetActionSetHandle( const char *pchActionSetName, VRActionSetHandle_t *pHandle ) override { return {}; }
  EVRInputError GetActionHandle( const char *pchActionName, VRActionHandle_t *pHandle ) override { return {}; }
  EVRInputError GetInputSourceHandle( const char *pchInputSourcePath, VRInputValueHandle_t *pHandle ) override { return {}; }
  EVRInputError UpdateActionState( VR_ARRAY_COUNT( unSetCount ) VRActiveActionSet_t *pSets, uint32_t unSizeOfVRSelectedActionSet_t, uint32_t unSetCount ) override { return {}; }
  EVRInputError GetDigitalActionData( VRActionHandle_t action, InputDigitalActionData_t *pActionData, uint32_t unActionDataSize, VRInputValueHandle_t ulRestrictToDevice ) override { return {}; }
  EVRInputError GetAnalogActionData( VRActionHandle_t action, InputAnalogActionData_t *pActionData, uint32_t unActionDataSize, VRInputValueHandle_t ulRestrictToDevice ) override { return {}; }
  EVRInputError GetPoseActionDataRelativeToNow( VRActionHandle_t action, ETrackingUniverseOrigin eOrigin, float fPredictedSecondsFromNow, InputPoseActionData_t *pActionData, uint32_t unActionDataSize, VRInputValueHandle_t ulRestrictToDevice ) override { return {}; }
  EVRInputError GetPoseActionDataForNextFrame( VRActionHandle_t action, ETrackingUniverseOrigin eOrigin, InputPoseActionData_t *pActionData, uint32_t unActionDataSize, VRInputValueHandle_t ulRestrictToDevice ) override { return {}; }
  EVRInputError GetSkeletalActionData( VRActionHandle_t action, InputSkeletalActionData_t *pActionData, uint32_t unActionDataSize ) override { return {}; }
  EVRInputError GetDominantHand( ETrackedControllerRole *peDominantHand ) override { return {}; }
  EVRInputError SetDominantHand( ETrackedControllerRole eDominantHand ) override { return {}; }
  EVRInputError GetBoneCount( VRActionHandle_t action, uint32_t* pBoneCount ) override { return {}; }
  EVRInputError GetBoneHierarchy( VRActionHandle_t action, VR_ARRAY_COUNT( unIndexArayCount ) BoneIndex_t* pParentIndices, uint32_t unIndexArayCount ) override { return {}; }
  EVRInputError GetBoneName( VRActionHandle_t action, BoneIndex_t nBoneIndex, VR_OUT_STRING() char* pchBoneName, uint32_t unNameBufferSize ) override { return {}; }
  EVRInputError GetSkeletalReferenceTransforms( VRActionHandle_t action, EVRSkeletalTransformSpace eTransformSpace, EVRSkeletalReferencePose eReferencePose, VR_ARRAY_COUNT( unTransformArrayCount ) VRBoneTransform_t *pTransformArray, uint32_t unTransformArrayCount ) override { return {}; }
  EVRInputError GetSkeletalTrackingLevel( VRActionHandle_t action, EVRSkeletalTrackingLevel* pSkeletalTrackingLevel ) override { return {}; }
  EVRInputError GetSkeletalBoneData( VRActionHandle_t action, EVRSkeletalTransformSpace eTransformSpace, EVRSkeletalMotionRange eMotionRange, VR_ARRAY_COUNT( unTransformArrayCount ) VRBoneTransform_t *pTransformArray, uint32_t unTransformArrayCount ) override { return {}; }
  EVRInputError GetSkeletalSummaryData( VRActionHandle_t action, EVRSummaryType eSummaryType, VRSkeletalSummaryData_t * pSkeletalSummaryData ) override { return {}; }
  EVRInputError GetSkeletalBoneDataCompressed( VRActionHandle_t action, EVRSkeletalMotionRange eMotionRange, VR_OUT_BUFFER_COUNT( unCompressedSize ) void *pvCompressedData, uint32_t unCompressedSize, uint32_t *punRequiredCompressedSize ) override { return {}; }
  EVRInputError DecompressSkeletalBoneData( const void *pvCompressedBuffer, uint32_t unCompressedBufferSize, EVRSkeletalTransformSpace eTransformSpace, VR_ARRAY_COUNT( unTransformArrayCount ) VRBoneTransform_t *pTransformArray, uint32_t unTransformArrayCount ) override { return {}; }
  EVRInputError TriggerHapticVibrationAction( VRActionHandle_t action, float fStartSecondsFromNow, float fDurationSeconds, float fFrequency, float fAmplitude, VRInputValueHandle_t ulRestrictToDevice ) override { return {}; }
  EVRInputError GetActionOrigins( VRActionSetHandle_t actionSetHandle, VRActionHandle_t digitalActionHandle, VR_ARRAY_COUNT( originOutCount ) VRInputValueHandle_t *originsOut, uint32_t originOutCount ) override { return {}; }
  EVRInputError GetOriginLocalizedName( VRInputValueHandle_t origin, VR_OUT_STRING() char *pchNameArray, uint32_t unNameArraySize, int32_t unStringSectionsToInclude ) override { return {}; }
  EVRInputError GetOriginTrackedDeviceInfo( VRInputValueHandle_t origin, InputOriginInfo_t *pOriginInfo, uint32_t unOriginInfoSize ) override { return {}; }
  EVRInputError GetActionBindingInfo( VRActionHandle_t action, VR_ARRAY_COUNT( unBindingInfoCount ) InputBindingInfo_t *pOriginInfo, uint32_t unBindingInfoSize, uint32_t unBindingInfoCount, uint32_t *punReturnedBindingInfoCount ) override { return {}; }
  EVRInputError ShowActionOrigins( VRActionSetHandle_t actionSetHandle, VRActionHandle_t ulActionHandle ) override { return {}; }
  EVRInputError ShowBindingsForActionSet( VR_ARRAY_COUNT( unSetCount ) VRActiveActionSet_t *pSets, uint32_t unSizeOfVRSelectedActionSet_t, uint32_t unSetCount, VRInputValueHandle_t originToHighlight ) override { return {}; }
  EVRInputError GetComponentStateForBinding( const char *pchRenderModelName, const char *pchComponentName, const InputBindingInfo_t *pOriginInfo, uint32_t unBindingInfoSize, uint32_t unBindingInfoCount, vr::RenderModel_ComponentState_t *pComponentState ) override { return {}; }
  bool IsUsingLegacyInput() override { return {}; }
  EVRInputError OpenBindingUI( const char* pchAppKey, VRActionSetHandle_t ulActionSetHandle, VRInputValueHandle_t ulDeviceHandle, bool bShowOnDesktop ) override { return {}; }
  EVRInputError GetBindingVariant( vr::VRInputValueHandle_t ulDevicePath, VR_OUT_STRING() char *pchVariantArray, uint32_t unVariantArraySize ) override { return {}; }
};
