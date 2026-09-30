#include <falso_jni/FalsoJNI_Impl.h>

#include "audio.h"
#include "jni_bia2.h"

/*
 * JNI Methods
 *
 * Exactly the set libbia2.so asks for with GetStaticMethodID(), taken from
 * the pseudo-C of BIA2_nativeInit / GLMediaPlayer_nativeInit /
 * GLResLoader_nativeInit -- the engine never looks up anything else (no
 * GetMethodID, no FindClass). FalsoJNI resolves by name only, ignoring the
 * class, which is fine here: no name repeats across the three classes.
 */

NameToMethodID nameToMethodId[] = {
	// BIA2 (BIA2_nativeInit)
	{ 0, "sendAppToBackground", METHOD_TYPE_VOID },      // ()V
	{ 1, "Exit", METHOD_TYPE_VOID },                     // ()V
	{ 2, "IsWifiEnabled", METHOD_TYPE_INT },             // ()I
	{ 3, "launchGetGames", METHOD_TYPE_VOID },           // ()V
	{ 4, "launchIGP", METHOD_TYPE_VOID },                // (I)V
	{ 5, "getHostName", METHOD_TYPE_OBJECT },            // ()[B
	{ 6, "OpenGLive", METHOD_TYPE_VOID },                // (I)V
	{ 7, "NotifyTrophy", METHOD_TYPE_VOID },             // (I)V

	// GLMediaPlayer (GLMediaPlayer_nativeInit)
	{ 20, "update", METHOD_TYPE_VOID },                  // ()V
	{ 21, "isSoundLoaded", METHOD_TYPE_INT },            // (II)I
	{ 22, "isMusicLoaded", METHOD_TYPE_INT },            // (I)I
	{ 23, "unloadSound", METHOD_TYPE_VOID },             // (II)V
	{ 24, "unloadMusic", METHOD_TYPE_VOID },             // (I)V
	{ 25, "loadSound", METHOD_TYPE_VOID },               // (I)V
	{ 26, "loadMusic", METHOD_TYPE_VOID },               // (I)V
	{ 27, "getSoundReady", METHOD_TYPE_INT },            // (I)I
	{ 28, "registerSoundFile", METHOD_TYPE_VOID },       // (ILjava/lang/String;)V
	{ 29, "playSound", METHOD_TYPE_VOID },               // (IFF)V
	{ 30, "playSoundAsync", METHOD_TYPE_VOID },          // (IF)V
	{ 31, "playMusic", METHOD_TYPE_VOID },               // (IFI)V
	{ 32, "pauseSound", METHOD_TYPE_VOID },              // (I)V
	{ 33, "pauseMusic", METHOD_TYPE_VOID },              // (I)V
	{ 34, "resumeSound", METHOD_TYPE_VOID },             // (I)V
	{ 35, "resumeMusic", METHOD_TYPE_VOID },             // (IF)V
	{ 36, "resumeAllMusic", METHOD_TYPE_VOID },          // (F)V
	{ 37, "stopSound", METHOD_TYPE_VOID },               // (I)V
	{ 38, "stopMusic", METHOD_TYPE_VOID },               // (I)V
	{ 39, "setVolume", METHOD_TYPE_VOID },               // (F)V
	{ 40, "setVolumeMusic", METHOD_TYPE_VOID },          // (F)V
	{ 41, "resetSound", METHOD_TYPE_VOID },              // (I)V
	{ 42, "setPitch", METHOD_TYPE_VOID },                // (IIF)V
	{ 43, "stopAllSounds", METHOD_TYPE_VOID },           // ()V
	{ 44, "stopAllSFX", METHOD_TYPE_VOID },              // ()V
	{ 45, "stopAllMusic", METHOD_TYPE_VOID },            // ()V
	{ 46, "destroySoundPool", METHOD_TYPE_VOID },        // ()V
	{ 47, "initSoundPoolArray", METHOD_TYPE_VOID },      // ()V
	{ 48, "loadMovie", METHOD_TYPE_VOID },               // (Ljava/lang/String;)V
	{ 49, "isMediaPlaying", METHOD_TYPE_INT },           // (I)I
	{ 50, "isSoundPlaying", METHOD_TYPE_BOOLEAN },       // (I)Z
	{ 51, "stopVoice", METHOD_TYPE_VOID },               // ()V
	{ 52, "getVoiceDuration", METHOD_TYPE_INT },         // (I)I
	{ 53, "resume", METHOD_TYPE_VOID },                  // ()V

	// GLResLoader (GLResLoader_nativeInit)
	{ 60, "getResourceFull", METHOD_TYPE_OBJECT },       // (Ljava/lang/String;)[B
	{ 61, "getResourceBytes", METHOD_TYPE_OBJECT },      // (Ljava/lang/String;II)[B
	{ 62, "getResourceLength", METHOD_TYPE_INT },        // (Ljava/lang/String;)I
	{ 63, "getSoundRaw", METHOD_TYPE_OBJECT },           // (I)[B
	{ 64, "getResourceLengthSoundRaw", METHOD_TYPE_INT },// (I)I
};

MethodsBoolean methodsBoolean[] = {
	{ 50, GLMediaPlayer_isSoundPlaying },
};

MethodsByte methodsByte[] = {};
MethodsChar methodsChar[] = {};
MethodsDouble methodsDouble[] = {};
MethodsFloat methodsFloat[] = {};

MethodsInt methodsInt[] = {
	{ 2, BIA2_IsWifiEnabled },
	{ 21, GLMediaPlayer_isSoundLoaded },
	{ 22, GLMediaPlayer_isMusicLoaded },
	{ 27, GLMediaPlayer_getSoundReady },
	{ 49, GLMediaPlayer_isMediaPlaying },
	{ 52, GLMediaPlayer_getVoiceDuration },
	{ 62, GLResLoader_getResourceLength },
	{ 64, GLResLoader_getResourceLengthSoundRaw },
};

MethodsLong methodsLong[] = {};

MethodsObject methodsObject[] = {
	{ 5, BIA2_getHostName },
	{ 60, GLResLoader_getResourceFull },
	{ 61, GLResLoader_getResourceBytes },
	{ 63, GLResLoader_getSoundRaw },
};

MethodsShort methodsShort[] = {};

MethodsVoid methodsVoid[] = {
	{ 0, BIA2_sendAppToBackground },
	{ 1, BIA2_Exit },
	{ 3, BIA2_launchGetGames },
	{ 4, BIA2_launchIGP },
	{ 6, BIA2_OpenGLive },
	{ 7, BIA2_NotifyTrophy },

	{ 20, GLMediaPlayer_update },
	{ 23, GLMediaPlayer_unloadSound },
	{ 24, GLMediaPlayer_unloadMusic },
	{ 25, GLMediaPlayer_loadSound },
	{ 26, GLMediaPlayer_loadMusic },
	{ 28, GLMediaPlayer_registerSoundFile },
	{ 29, GLMediaPlayer_playSound },
	{ 30, GLMediaPlayer_playSoundAsync },
	{ 31, GLMediaPlayer_playMusic },
	{ 32, GLMediaPlayer_pauseSound },
	{ 33, GLMediaPlayer_pauseMusic },
	{ 34, GLMediaPlayer_resumeSound },
	{ 35, GLMediaPlayer_resumeMusic },
	{ 36, GLMediaPlayer_resumeAllMusic },
	{ 37, GLMediaPlayer_stopSound },
	{ 38, GLMediaPlayer_stopMusic },
	{ 39, GLMediaPlayer_setVolume },
	{ 40, GLMediaPlayer_setVolumeMusic },
	{ 41, GLMediaPlayer_resetSound },
	{ 42, GLMediaPlayer_setPitch },
	{ 43, GLMediaPlayer_stopAllSounds },
	{ 44, GLMediaPlayer_stopAllSFX },
	{ 45, GLMediaPlayer_stopAllMusic },
	{ 46, GLMediaPlayer_destroySoundPool },
	{ 47, GLMediaPlayer_initSoundPoolArray },
	{ 48, GLMediaPlayer_loadMovie },
	{ 51, GLMediaPlayer_stopVoice },
	{ 53, GLMediaPlayer_resume },
};

/*
 * JNI Fields -- libbia2.so never calls Get*FieldID; kept for FalsoJNI.
 */

char WINDOW_SERVICE[] = "window";
const int SDK_INT = 19; // Android 4.4 / KitKat

NameToFieldID nameToFieldId[] = {
		{ 0, "WINDOW_SERVICE", FIELD_TYPE_OBJECT },
		{ 1, "SDK_INT", FIELD_TYPE_INT },
};

FieldsBoolean fieldsBoolean[] = {};
FieldsByte fieldsByte[] = {};
FieldsChar fieldsChar[] = {};
FieldsDouble fieldsDouble[] = {};
FieldsFloat fieldsFloat[] = {};
FieldsInt fieldsInt[] = {
		{ 1, SDK_INT },
};
FieldsObject fieldsObject[] = {
		{ 0, WINDOW_SERVICE },
};
FieldsLong fieldsLong[] = {};
FieldsShort fieldsShort[] = {};

__FALSOJNI_IMPL_CONTAINER_SIZES
