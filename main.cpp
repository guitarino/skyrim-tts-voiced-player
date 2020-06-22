#include <string>
#include <sapi.h>
#include <comdef.h>
#include <sstream>
#include <math.h>
#include <sphelper.h>
#include <vector>

#include "skse/PluginAPI.h"
#include "skse/skse_version.h"
#include "skse/GameAPI.h"
#include "skse/GameData.h"
#include "skse/GameTypes.h"
#include "skse/PapyrusNativeFunctions.h"
#include "skse/PapyrusVM.h"
#include "skse/GameThreads.h"

#include "detour.h"
#include "thiscall.h"

using namespace std;

static SKSEPapyrusInterface* g_papyrusInterface = NULL;
static SKSETaskInterface* g_taskInterface = NULL;

IDebugLog gLog("ttsVoicedPlayerDialoguePlugin.log");
PluginHandle gPluginHandle = kPluginHandle_Invalid;

vector<ISpObjectToken*> gVoices;
ISpVoice* gVoice = NULL;

// Default settings - should match those in Papyrus

bool gModEnabled = true;
UInt32 gPlayerVoiceID = 0;
UInt32 gPlayerVoiceVolume = 50;
SInt32 gPlayerVoiceRateAdjust = 0;

/*************************
 **	Player speech state **
**************************/

enum PlayerSpeechState {
	TOPIC_NOT_SELECTED = 0,
	TOPIC_SELECTED = 1,
	TOPIC_SPOKEN = 2
};

struct PlayerSpeech {
	PlayerSpeechState state;
	bool isNPCSpeechDelayed;
	UInt32 option;
};

PlayerSpeech* gPlayerSpeech = NULL;

void initializePlayerSpeech() {
	if (gPlayerSpeech == NULL) {
		gPlayerSpeech = new PlayerSpeech();
		gPlayerSpeech->state = TOPIC_NOT_SELECTED;
		gPlayerSpeech->isNPCSpeechDelayed = false;
	}
}

/***************************************************
 **	Event handling for when TTS finished speaking **
****************************************************/

class TopicSpokenEventDelegate : public TaskDelegate {
public:
	virtual void Run() override {
		UInt32 setTopic = 0x006740E0;
		UInt32 sayTopicResponse = 0x006741B0;

		if (gPlayerSpeech->state == TOPIC_SELECTED) {
			gPlayerSpeech->state = TOPIC_SPOKEN;
			gPlayerSpeech->isNPCSpeechDelayed = false;

			// Here's the fun part: once TTS stopped speaking, we gotta set the topic again,
			// then speak it. It's already done on the first click event, but we're ignoring it
			// with our onDialogueSayHook to allow TTS to speak.
			MenuTopicManager* mtm = MenuTopicManager::GetSingleton();
			thisCall<void>(mtm, setTopic, gPlayerSpeech->option);
			thisCall<void>(mtm, sayTopicResponse, 0, 0);
		}
	}

	virtual void Dispose() override {
		delete this;
	}
};

void __stdcall executeVoiceNotifyThread() {
	CSpEvent evt;
	HANDLE voiceNotifyHandle = gVoice->GetNotifyEventHandle();

	do {
		WaitForSingleObject(voiceNotifyHandle, INFINITE);

		while (gVoice != NULL && evt.GetFrom(gVoice) == S_OK) {
			if (evt.eEventId == SPEI_END_INPUT_STREAM) {
				g_taskInterface->AddTask(new TopicSpokenEventDelegate());
			}
		}
	} while (gVoice != NULL);
};

/********************************************
 **	Initializing voices and setting up TTS **
*********************************************/

float* masterVolumeSetting = (float*)0x01271D0C;
float* voiceVolumeSetting = (float*)0x01B108F0;

vector<ISpObjectToken*> getVoices() {
	vector<ISpObjectToken*> voiceObjects;
	ULONG voiceCount = 0;
	ISpObjectToken* voiceObject;
	IEnumSpObjectTokens* enumVoiceObjects;

	SpEnumTokens(SPCAT_VOICES, NULL, NULL, &enumVoiceObjects);
	enumVoiceObjects->GetCount(&voiceCount);

	HRESULT hr = S_OK;
	ULONG i = 0;
	while (SUCCEEDED(hr) && i < voiceCount) {
		hr = enumVoiceObjects->Item(i, &voiceObject);
		voiceObjects.push_back(voiceObject);
		i++;
	}

	enumVoiceObjects->Release();
	return voiceObjects;
}

ULONG getVoicesCount() {
	ULONG voiceCount = 0;
	IEnumSpObjectTokens* enumVoiceObjects;
	SpEnumTokens(SPCAT_VOICES, NULL, NULL, &enumVoiceObjects);
	enumVoiceObjects->GetCount(&voiceCount);
	enumVoiceObjects->Release();
	return voiceCount;
}

void initializeVoices() {
	if (gVoice == NULL) {
		gVoices = getVoices();

		if (FAILED(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED))) {
			_MESSAGE("Problem: CoInitializeEx failed");
		}
		else if (FAILED(CoCreateInstance(CLSID_SpVoice, NULL, CLSCTX_ALL, IID_ISpVoice, (void **)&gVoice))) {
			_MESSAGE("Problem: CoCreateInstance failed");
		}

		CoUninitialize();

		ULONGLONG eventTypes = SPFEI(SPEI_END_INPUT_STREAM);

		if (FAILED(gVoice->SetInterest(eventTypes, eventTypes))) {
			_MESSAGE("Problem: SetInterest failed");
		}

		CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)executeVoiceNotifyThread, NULL, NULL, NULL);
	}
}

void speak(const char* message) {
	initializeVoices();
	std::wstringstream messageStream;
	messageStream << message;

	// Volume is set every time because master / voice volume might have changed
	gVoice->SetVolume(round(gPlayerVoiceVolume * (*masterVolumeSetting) * (*voiceVolumeSetting)));
	gVoice->Speak(messageStream.str().c_str(), SPF_ASYNC | SPF_IS_NOT_XML | SPF_PURGEBEFORESPEAK, NULL);
}

void stopSpeaking() {
	gVoice->Speak(L"", SPF_ASYNC | SPF_IS_NOT_XML | SPF_PURGEBEFORESPEAK, NULL);
}

/*********************
 **	ON TOPIC SETTER **
**********************/

BYTE* gOnTopicSetter = (BYTE*)0x00674113;
BYTE* gOnTopicSetterResume;

struct ObjectWithMessage {
	const char* message;
};

struct ObjectWithObjectWithMessage {
	ObjectWithMessage* object;
};

void __stdcall onTopicSetterHook(ObjectWithObjectWithMessage* object, UInt32 option) {
	if (!gModEnabled) {
		return;
	}

	initializePlayerSpeech();

	if (gPlayerSpeech->state == TOPIC_NOT_SELECTED || gPlayerSpeech->state == TOPIC_SPOKEN) {
		gPlayerSpeech->state = TOPIC_SELECTED;
		gPlayerSpeech->isNPCSpeechDelayed = false;
		gPlayerSpeech->option = option;
		speak(object->object->message);
	}
}

__declspec(naked) void onTopicSetterHooked() {
	__asm {
		push edx
		mov edx, [esp+0xC] // The selected option is an argument to the function, and is still on the stack there
		pushad
		push edx
		push esi           // `esi` register here contains a pointer to an object, containing
						   // a pointer to another object with a pointer to the string of a chosen topic
						   // (I didn't bother to figure out what this object is)
		call onTopicSetterHook
		popad
		pop edx
		jmp[gOnTopicSetterResume]
	}
}

/***********************
 **	DIALOGUE SAY HOOK **
************************/

BYTE* gOnDialogueSay = (BYTE*)0x006D397E;
BYTE* gOnDialogueSayResume;
BYTE* gOnDialogueSaySkip = (BYTE*)0x006D39C4;

bool __stdcall shouldDelayNPCSpeech() {
	if (!gModEnabled) {
		return false;
	}

	initializePlayerSpeech();


	// This is for when the user wants to skip the convo by (usually vigorously) clicking
	if (gPlayerSpeech->state == TOPIC_SELECTED && gPlayerSpeech->isNPCSpeechDelayed) {
		gPlayerSpeech->state = TOPIC_NOT_SELECTED;
		gPlayerSpeech->isNPCSpeechDelayed = false;
		stopSpeaking();
	}

	else if (gPlayerSpeech->state == TOPIC_SELECTED) {
		gPlayerSpeech->isNPCSpeechDelayed = true;
		return true;
	}

	return false;
}

__declspec(naked) void onDialogueSayHooked() {
	__asm {
		pushad
		call shouldDelayNPCSpeech
		test al, al
		jnz DELAY_NPC_SPEECH // If should delay NPC speech, go to some code after
		popad
		jmp[gOnDialogueSayResume]

DELAY_NPC_SPEECH:
		popad
		jmp[gOnDialogueSaySkip]
	}
}

/**********************************************
 **	Registered functions and their delegates **
**********************************************/

class SetVoiceDelegate : public TaskDelegate {
public:
	virtual void Run() override {
		initializeVoices();
		gVoice->SetVoice(gVoices[gPlayerVoiceID]);
	}

	virtual void Dispose() override {
		delete this;
	}
};

class SetRateAdjustDelegate : public TaskDelegate {
public:
	virtual void Run() override {
		initializeVoices();
		gVoice->SetRate(gPlayerVoiceRateAdjust);
	}

	virtual void Dispose() override {
		delete this;
	}
};

VMResultArray<BSFixedString> getAvailableTTSVoices(StaticFunctionTag*) {
	vector<ISpObjectToken*> voices = getVoices(); // We can't just use `gVoices` because it's on another thread
	VMResultArray<BSFixedString> vmVoiceList;
	WCHAR* szDesc;
	const char* szDescString;

	UInt32 size = voices.size();
	vmVoiceList.resize(size);

	for (UInt32 i = 0; i < size; i++) {
		SpGetDescription(voices[i], &szDesc);
		_bstr_t szDescCommon(szDesc);
		szDescString = szDescCommon;
		vmVoiceList[i] = BSFixedString(szDescString);
		voices[i]->Release();
	}

	voices.clear();
	return vmVoiceList;
}

SInt32 setModEnabled(StaticFunctionTag*, bool modEnabled) {
	gModEnabled = modEnabled;
	return 1; // Pretty sure it'll be successful
}

SInt32 setTTSPlayerVoiceID(StaticFunctionTag*, SInt32 id) {
	SInt32 isSuccessful = 1;

	if (id >= getVoicesCount()) {
		id = 0;
		isSuccessful = 0;
	}

	gPlayerVoiceID = id;
	g_taskInterface->AddTask(new SetVoiceDelegate());
	return isSuccessful;
}

SInt32 setTTSPlayerVoiceVolume(StaticFunctionTag*, SInt32 volume) {
	SInt32 isSuccessful = 1;

	if (volume > 100 || volume < 0) {
		volume = 50;
		isSuccessful = 0;
	}

	gPlayerVoiceVolume = volume;
	return isSuccessful;
}

SInt32 setTTSPlayerVoiceRateAdjust(StaticFunctionTag*, SInt32 rateAdjust) {
	SInt32 isSuccessful = 1;

	if (rateAdjust < -10 || rateAdjust > 10) {
		rateAdjust = 0;
		isSuccessful = 0;
	}

	gPlayerVoiceRateAdjust = rateAdjust;
	g_taskInterface->AddTask(new SetRateAdjustDelegate());
	return isSuccessful;
}

bool registerFuncs(VMClassRegistry* a_registry) {
	a_registry->RegisterFunction(new NativeFunction0<StaticFunctionTag, VMResultArray<BSFixedString>>("GetAvailableTTSVoices", "TTS_Voiced_Player_Dialogue_MCM_Script", getAvailableTTSVoices, a_registry));
	a_registry->RegisterFunction(new NativeFunction1<StaticFunctionTag, SInt32, bool>("SetTTSModEnabled", "TTS_Voiced_Player_Dialogue_MCM_Script", setModEnabled, a_registry));
	a_registry->RegisterFunction(new NativeFunction1<StaticFunctionTag, SInt32, SInt32>("SetTTSPlayerVoiceID", "TTS_Voiced_Player_Dialogue_MCM_Script", setTTSPlayerVoiceID, a_registry));
	a_registry->RegisterFunction(new NativeFunction1<StaticFunctionTag, SInt32, SInt32>("SetTTSPlayerVoiceVolume", "TTS_Voiced_Player_Dialogue_MCM_Script", setTTSPlayerVoiceVolume, a_registry));
	a_registry->RegisterFunction(new NativeFunction1<StaticFunctionTag, SInt32, SInt32>("SetTTSPlayerVoiceRateAdjust", "TTS_Voiced_Player_Dialogue_MCM_Script", setTTSPlayerVoiceRateAdjust, a_registry));
	return true;
}

/********************
**	Initialization **
*********************/

extern "C" {
	bool SKSEPlugin_Query(const SKSEInterface * skse, PluginInfo * info) {
		// populate info structure
		info->infoVersion =	PluginInfo::kInfoVersion;
		info->name = "TTS Voiced Player Dialogue";
		info->version = 1;

		// store plugin handle so we can identify ourselves later
		gPluginHandle = skse->GetPluginHandle();

		if(skse->isEditor) {
			_MESSAGE("loaded in editor, marking as incompatible");
			return false;
		}
	
		if(skse->runtimeVersion != RUNTIME_VERSION_1_9_32_0) {
			_MESSAGE("Problem: unsupported runtime version %08X", skse->runtimeVersion);
			return false;
		}

		_MESSAGE("TTS Voiced Player Dialogue initialized");
		return true;
	}

	bool SKSEPlugin_Load(const SKSEInterface * skse) {
		// These set up injection points to the game:

		// 1. When the topic is clicked, we'd like to remember the selected
		//    option (so that we can trigger same option choice later) and actually speak the TTS message
		gOnTopicSetterResume = detourWithTrampoline(gOnTopicSetter, (BYTE*)onTopicSetterHooked, 5);

		// 2. When the NPC is about to speak, we'd like prevent them initially, but still allow other dialogue events.
		//    We also check there, well, if user clicks during a convo to try to skip it, we'll also stop the TTS speaking.
		gOnDialogueSayResume = detourWithTrampoline(gOnDialogueSay, (BYTE*)onDialogueSayHooked, 6);

		g_papyrusInterface = static_cast<SKSEPapyrusInterface*>(skse->QueryInterface(kInterface_Papyrus));

		if (!g_papyrusInterface) {
			_MESSAGE("Problem: g_papyrusInterface is false");
			return false;
		}
		else if (!g_papyrusInterface->Register(registerFuncs)) {
			_MESSAGE("Problem: registration failed");
			return false;
		}

		g_taskInterface = static_cast<SKSETaskInterface*>(skse->QueryInterface(kInterface_Task));

		if (!g_taskInterface) {
			_MESSAGE("Problem: task registration failed");
			return false;
		}

		_MESSAGE("TTS Voiced Player Dialogue loaded");
		return true;
	}
};
