package emitter

// System-library HLE added for the PPSA30552 boot path (branch m2-hlesys).
// Each name is bound as shim_<name> by plain name and, through initCanonicalNIDMap, by NID.
// Code: pkg/runtime/modules/{libSceSaveData,libSceAudioOut,libSceAmpr,libSceAjm,libkernel,libSceHlesys}. (UserService/SystemService/PlayGo/Acm: m2-hlestubs)

var hlesysShims = []string{
	// libSceSaveData (PS5 shapes)
	"sceSaveDataMount3",
	"sceSaveDataUmount2",
	"sceSaveDataPrepare",
	"sceSaveDataCommit",
	"sceSaveDataCreateTransactionResource",
	"sceSaveDataDeleteTransactionResource",

	// libSceAudioOut2
	"sceAudioOut2Initialize",
	"sceAudioOut2InitializeEx",
	"sceAudioOut2Terminate",
	"sceAudioOut2UserCreate",
	"sceAudioOut2UserCreateEx",
	"sceAudioOut2UserDestroy",
	"sceAudioOut2ContextResetParam",
	"sceAudioOut2ContextQueryMemory",
	"sceAudioOut2ContextCreate",
	"sceAudioOut2ContextDestroy",
	"sceAudioOut2ContextPush",
	"sceAudioOut2ContextAdvance",
	"sceAudioOut2ContextGetQueueLevel",
	"sceAudioOut2ContextSetAttributes",
	"sceAudioOut2PortCreate",
	"sceAudioOut2PortDestroy",
	"sceAudioOut2PortGetState",
	"sceAudioOut2PortSetAttributes",
	"sceAudioOut2GetSystemState",

	// libSceAmpr (APR command buffers)
	"sceAmprCommandBufferConstructor",
	"sceAmprCommandBufferDestructor",
	"sceAmprCommandBufferReset",
	"sceAmprCommandBufferSetBuffer",
	"sceAmprCommandBufferClearBuffer",
	"sceAmprCommandBufferGetSize",
	"sceAmprCommandBufferGetCurrentOffset",
	"sceAmprCommandBufferWriteKernelEventQueue_04_00",
	"sceAmprAprCommandBufferConstructor",
	"sceAmprAprCommandBufferDestructor",
	"sceAmprAprCommandBufferReadFile",
	"sceAmprMeasureCommandSizeWriteKernelEventQueue_04_00",
	"sceAmprMeasureCommandSizeReadFile",

	// libSceAmpr / libkernel APR additions (astro-hle2)
	"sceAmprAprCommandBufferMapBegin",
	"sceAmprAprCommandBufferMapDirectBegin",
	"sceAmprAprCommandBufferMapEnd",
	"sceAmprAprCommandBufferReadFileGather",
	"sceAmprAprCommandBufferReadFileGatherScatter",
	"sceAmprAprCommandBufferReadFileScatter",
	"sceAmprAprCommandBufferResetGatherScatterState",
	"sceAmprCommandBufferConstructMarker",
	"sceAmprCommandBufferConstructNop",
	"sceAmprCommandBufferGetBufferBaseAddress",
	"sceAmprCommandBufferGetNumCommands",
	"sceAmprCommandBufferGetType",
	"sceAmprCommandBufferNop",
	"sceAmprCommandBufferNopWithData",
	"sceAmprCommandBufferPopMarker",
	"sceAmprCommandBufferPushMarker",
	"sceAmprCommandBufferPushMarkerWithColor",
	"sceAmprCommandBufferSetMarker",
	"sceAmprCommandBufferSetMarkerWithColor",
	"sceAmprCommandBufferWaitOnAddress_04_00",
	"sceAmprCommandBufferWaitOnCounter_04_00",
	"sceAmprCommandBufferWriteAddressFromCounterPair_04_00",
	"sceAmprCommandBufferWriteAddressFromCounter_04_00",
	"sceAmprCommandBufferWriteAddressFromTimeCounter_04_00",
	"sceAmprCommandBufferWriteAddress_04_00",
	"sceAmprCommandBufferWriteCounter_04_00",
	"sceAmprMeasureCommandSizeMapBegin",
	"sceAmprMeasureCommandSizeMapDirectBegin",
	"sceAmprMeasureCommandSizeMapEnd",
	"sceAmprMeasureCommandSizeNop",
	"sceAmprMeasureCommandSizeNopWithData",
	"sceAmprMeasureCommandSizePopMarker",
	"sceAmprMeasureCommandSizePushMarker",
	"sceAmprMeasureCommandSizePushMarkerWithColor",
	"sceAmprMeasureCommandSizeReadFileGather",
	"sceAmprMeasureCommandSizeReadFileGatherScatter",
	"sceAmprMeasureCommandSizeReadFileScatter",
	"sceAmprMeasureCommandSizeResetGatherScatterState",
	"sceAmprMeasureCommandSizeSetMarker",
	"sceAmprMeasureCommandSizeSetMarkerWithColor",
	"sceAmprMeasureCommandSizeWaitOnAddress_04_00",
	"sceAmprMeasureCommandSizeWaitOnCounter_04_00",
	"sceAmprMeasureCommandSizeWriteAddressFromCounterPair_04_00",
	"sceAmprMeasureCommandSizeWriteAddressFromCounter_04_00",
	"sceAmprMeasureCommandSizeWriteAddressFromTimeCounter_04_00",
	"sceAmprMeasureCommandSizeWriteAddress_04_00",
	"sceAmprMeasureCommandSizeWriteCounter_04_00",
	"sceKernelAprGetFileStat",
	"sceKernelAprResolveFilepathsToIdsAndFileSizesForEach",
	"sceKernelAprResolveFilepathsToIdsForEach",
	"sceKernelAprResolveFilepathsWithPrefixToIds",
	"sceKernelAprResolveFilepathsWithPrefixToIdsAndFileSizes",
	"sceKernelAprResolveFilepathsWithPrefixToIdsAndFileSizesForEach",
	"sceKernelAprResolveFilepathsWithPrefixToIdsForEach",
	"sceKernelClearVirtualRangeName",
	"sceKernelGetOpenPsId",
	"scePthreadCondSignalto",

	// libSceAjm (PS5 batch builder)
	"sceAjmBatchInitialize",
	"sceAjmBatchJobInitialize",
	"sceAjmBatchJobClearContext",
	"sceAjmBatchJobDecode",
	"sceAjmBatchStart",

	// libkernel: APR file ids and submission, AMPR events, PRT aperture, mode query, coredump hooks
	"sceKernelAprResolveFilepathsToIds",
	"sceKernelAprResolveFilepathsToIdsAndFileSizes",
	"sceKernelAprGetFileSize",
	"sceKernelAprSubmitCommandBuffer",
	"sceKernelAprSubmitCommandBufferAndGetId",
	"sceKernelAprSubmitCommandBufferAndGetResult",
	"sceKernelAprWaitCommandBuffer",
	"sceKernelAddAmprEvent",
	"sceKernelSetPrtAperture",
	"sceKernelIsTrinityMode",
	"sceCoredumpWriteUserString",
	"sceCoredumpSetUserDataType",
	"pthread_attr_setsolosched_np",
	"sceKernelGetOperationMode",
	"_sceLibcInternalThreadAtexit",
	"sceNpEntitlementAccessGetSkuFlag",
	"sceNpEntitlementAccessGetGameTrialsFlag",
	"sceNpGetAccountAge",
	"sceNpGetAccountLanguage2",

	// libScePad (libSceHlesys)
	"scePadSetVibrationMode",
	"scePadGetTriggerEffectState",
	"scePadIsRemoteController",
	"scePadSetTriggerEffect",
}

func init() {
	for _, n := range hlesysShims {
		if _, dup := CanonicalShims[n]; !dup {
			CanonicalShims[n] = "shim_" + n
		}
	}
	// SDK 11 titles (PPSA07631) import the scePthread name (NID Dk6FC-TI+7Q) of the same call.
	// libScePsml Mfsr2 (PSSR 2, Pro only) by NID: Initialize, then the 8 calls that need it (KytyPS5 libPsml.cpp table).
	CanonicalShims["o+NM86gEwFE"] = "shim_psml_mfsr2_initialize"
	for _, nid := range []string{"1ic5q-kdOsc", "gMduXCLYrNg", "0GAw7SmkwII", "5z2gBlqxJ+0", "m9JLPc3wOQw", "Kyy1baXgaVU", "ZLL31lzzxr4", "lrJwpLjKXRc"} {
		CanonicalShims[nid] = "shim_psml_mfsr2_not_initialized"
	}
	if _, dup := CanonicalShims["scePthreadAttrSetsolosched"]; !dup {
		CanonicalShims["scePthreadAttrSetsolosched"] = "shim_pthread_attr_setsolosched_np"
	}
}
