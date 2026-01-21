#include <Windows.h>
#include <mutex>
#include <cmath> // Necessário para pow() e abs()
#include <algorithm> // Necessário para clamp
#include "ViGEm\Client.h"
#include "IniReader\IniReader.h"
#include "DS4Emulator.h"

//#include <winsock2.h>
#pragma comment (lib, "WSock32.Lib")

// --- ESTRUTURAS PARA LOGICA XIM ---
struct AimSettings {
	float SensX;
	float SensY;
	float Smoothing; // 0.0 a 1.0
	float Curve;     // Exponente (1.0 = linear)
	int Boost;       // Anti-Deadzone
};

AimSettings HipSettings;
AimSettings AdsSettings;
// ----------------------------------

_XInputGetState MyXInputGetState;
_XInputSetState MyXInputSetState;
_XINPUT_STATE myPState;
HMODULE hDll = NULL;
DWORD XboxUserIndex = 0;

static std::mutex m;

int m_HalfWidth = 1920 / 2;
int m_HalfHeight = 1080 / 2;
bool firstCP = true;
int DeltaMouseX, DeltaMouseY;
HWND PSPlusWindow = 0;
HWND PSRemotePlayWindow = 0;

// Variáveis para o algoritmo XIM (Smoothing Globais)
static double g_SmoothX = 0;
static double g_SmoothY = 0;

// WinSock
SOCKET socketS;
int bytes_read;
struct sockaddr_in from;
int fromlen;
bool SocketActivated = false;
std::thread *pSocketThread = NULL;
unsigned char freePieIMU[50];
float AccelX = 0, AccelY = 0, AccelZ = 0, GyroX = 0, GyroY = 0, GyroZ = 0;

float bytesToFloat(unsigned char b3, unsigned char b2, unsigned char b1, unsigned char b0)
{
	unsigned char byte_array[] = { b3, b2, b1, b0 };
	float result;
	std::copy(reinterpret_cast<const char*>(&byte_array[0]),
		reinterpret_cast<const char*>(&byte_array[4]),
		reinterpret_cast<char*>(&result));
	return result;
}

int SleepTimeOutMotion = 1;
bool MotionOrientation = true; // true = landscape
void MotionReceiver()
{
	while (SocketActivated) {
		memset(&freePieIMU, 0, sizeof(freePieIMU));
		bytes_read = recvfrom(socketS, (char*)(&freePieIMU), sizeof(freePieIMU), 0, (sockaddr*)&from, &fromlen);
		if (bytes_read > 0) {
			if (MotionOrientation) { // landscape mapping
				AccelZ = bytesToFloat(freePieIMU[2], freePieIMU[3], freePieIMU[4], freePieIMU[5]);
				AccelX = bytesToFloat(freePieIMU[6], freePieIMU[7], freePieIMU[8], freePieIMU[9]);
				AccelY = bytesToFloat(freePieIMU[10], freePieIMU[11], freePieIMU[12], freePieIMU[13]);

				GyroZ = bytesToFloat(freePieIMU[14], freePieIMU[15], freePieIMU[16], freePieIMU[17]);
				GyroX = bytesToFloat(freePieIMU[18], freePieIMU[19], freePieIMU[20], freePieIMU[21]);
				GyroY = bytesToFloat(freePieIMU[22], freePieIMU[23], freePieIMU[24], freePieIMU[25]);
			}
			else { // portrait mapping
				AccelX = bytesToFloat(freePieIMU[2], freePieIMU[3], freePieIMU[4], freePieIMU[5]);
				AccelZ = bytesToFloat(freePieIMU[6], freePieIMU[7], freePieIMU[8], freePieIMU[9]);
				AccelY = bytesToFloat(freePieIMU[10], freePieIMU[11], freePieIMU[12], freePieIMU[13]);

				GyroX = bytesToFloat(freePieIMU[14], freePieIMU[15], freePieIMU[16], freePieIMU[17]);
				GyroZ = bytesToFloat(freePieIMU[18], freePieIMU[19], freePieIMU[20], freePieIMU[21]);
				GyroY = bytesToFloat(freePieIMU[22], freePieIMU[23], freePieIMU[24], freePieIMU[25]);
			}
		}
		else Sleep(SleepTimeOutMotion); // Don't overload the CPU with reading
	}
}

VOID CALLBACK notification(
	PVIGEM_CLIENT Client,
	PVIGEM_TARGET Target,
	UCHAR LargeMotor,
	UCHAR SmallMotor,
	DS4_LIGHTBAR_COLOR LightbarColor,
	LPVOID UserData)
{
	m.lock();

	if (MyXInputSetState != NULL) {
		XINPUT_VIBRATION myVibration;
		myVibration.wLeftMotorSpeed = LargeMotor * 257;
		myVibration.wRightMotorSpeed = SmallMotor * 257;
		MyXInputSetState(XboxUserIndex, &myVibration);
	}

	m.unlock();
}

void GetMouseState()
{
	POINT mousePos;
	if (firstCP) { SetCursorPos(m_HalfWidth, m_HalfHeight); firstCP = false; }
	GetCursorPos(&mousePos);
	DeltaMouseX = mousePos.x - m_HalfWidth;
	DeltaMouseY = mousePos.y - m_HalfHeight;
	SetCursorPos(m_HalfWidth, m_HalfHeight);
}

float Clamp(float Value, float Min, float Max)
{
	if (Value > Max)
		Value = Max;
	else if (Value < Min)
		Value = Min;
	return Value;
}

SHORT DeadZoneXboxAxis(SHORT StickAxis, float Percent)
{
	float DeadZoneValue = Percent * 327.67;
	if (StickAxis > 0)
	{
		StickAxis -= trunc(DeadZoneValue);
		if (StickAxis < 0)
			StickAxis = 0;
	}
	else if (StickAxis < 0) {
		StickAxis += trunc(DeadZoneValue);
		if (StickAxis > 0)
			StickAxis = 0;
	}

	return trunc(StickAxis + StickAxis * Percent * 0.01);
}

void LoadKMProfile(std::string ProfileFile) {
	CIniReader IniFile("Profiles\\" + ProfileFile);

	KEY_ID_LEFT_STICK_UP = KeyNameToKeyCode(IniFile.ReadString("Keys", "LEFT-STICK-UP", "W"));
	KEY_ID_LEFT_STICK_LEFT = KeyNameToKeyCode(IniFile.ReadString("Keys", "LEFT-STICK-LEFT", "A"));
	KEY_ID_LEFT_STICK_RIGHT = KeyNameToKeyCode(IniFile.ReadString("Keys", "LEFT-STICK-RIGHT", "D"));
	KEY_ID_LEFT_STICK_DOWN = KeyNameToKeyCode(IniFile.ReadString("Keys", "LEFT-STICK-DOWN", "S"));
	KEY_ID_LEFT_TRIGGER = KeyNameToKeyCode(IniFile.ReadString("Keys", "L2", "MOUSE-RIGHT-BTN"));
	KEY_ID_RIGHT_TRIGGER = KeyNameToKeyCode(IniFile.ReadString("Keys", "R2", "MOUSE-LEFT-BTN"));
	KEY_ID_LEFT_SHOULDER = KeyNameToKeyCode(IniFile.ReadString("Keys", "L1", "ALT"));
	KEY_ID_RIGHT_SHOULDER = KeyNameToKeyCode(IniFile.ReadString("Keys", "R1", "CTRL"));
	KEY_ID_DPAD_UP = KeyNameToKeyCode(IniFile.ReadString("Keys", "DPAD-UP", "1"));
	KEY_ID_DPAD_LEFT = KeyNameToKeyCode(IniFile.ReadString("Keys", "DPAD-LEFT", "2"));
	KEY_ID_DPAD_RIGHT = KeyNameToKeyCode(IniFile.ReadString("Keys", "DPAD-RIGHT", "3"));
	KEY_ID_DPAD_DOWN = KeyNameToKeyCode(IniFile.ReadString("Keys", "DPAD-DOWN", "4"));
	KEY_ID_LEFT_THUMB = KeyNameToKeyCode(IniFile.ReadString("Keys", "L3", "SHIFT"));
	KEY_ID_RIGHT_THUMB = KeyNameToKeyCode(IniFile.ReadString("Keys", "R3", "MOUSE-MIDDLE-BTN"));
	KEY_ID_TRIANGLE = KeyNameToKeyCode(IniFile.ReadString("Keys", "TRIANGLE", "E"));
	KEY_ID_SQUARE = KeyNameToKeyCode(IniFile.ReadString("Keys", "SQUARE", "R"));
	KEY_ID_CIRCLE = KeyNameToKeyCode(IniFile.ReadString("Keys", "CIRCLE", "Q"));
	KEY_ID_CROSS = KeyNameToKeyCode(IniFile.ReadString("Keys", "CROSS", "SPACE"));
	KEY_ID_SHARE = KeyNameToKeyCode(IniFile.ReadString("Keys", "SHARE", "F12"));
	KEY_ID_TOUCHPAD = KeyNameToKeyCode(IniFile.ReadString("Keys", "TOUCHPAD", "ENTER"));
	KEY_ID_OPTIONS = KeyNameToKeyCode(IniFile.ReadString("Keys", "OPTIONS", "TAB"));
	KEY_ID_PS = KeyNameToKeyCode(IniFile.ReadString("Keys", "PS", "F2"));

	KEY_ID_MOTION_SHAKING_NAME = IniFile.ReadString("Keys", "MOTION-SHAKING", "T");
	KEY_ID_MOTION_X_ADD_NAME = IniFile.ReadString("Keys", "MOTION-X-ADD", "NUMPAD6"); // X+
	KEY_ID_MOTION_X_SUB_NAME = IniFile.ReadString("Keys", "MOTION-X-SUB", "NUMPAD4"); // X−
	KEY_ID_MOTION_Y_ADD_NAME = IniFile.ReadString("Keys", "MOTION-Y-ADD", "NUMPAD8"); // Y+
	KEY_ID_MOTION_Y_SUB_NAME = IniFile.ReadString("Keys", "MOTION-Y-SUB", "NUMPAD2"); // Y−
	KEY_ID_MOTION_Z_ADD_NAME = IniFile.ReadString("Keys", "MOTION-Z-ADD", "NUMPAD9"); // Z+
	KEY_ID_MOTION_Z_SUB_NAME = IniFile.ReadString("Keys", "MOTION-Z-SUB", "NUMPAD7"); // Z−

	KEY_ID_MOTION_SHAKING = KeyNameToKeyCode(KEY_ID_MOTION_SHAKING_NAME.c_str());
	KEY_ID_MOTION_X_ADD = KeyNameToKeyCode(KEY_ID_MOTION_X_ADD_NAME.c_str()); // X+
	KEY_ID_MOTION_X_SUB = KeyNameToKeyCode(KEY_ID_MOTION_X_SUB_NAME.c_str()); // X−
	KEY_ID_MOTION_Y_ADD = KeyNameToKeyCode(KEY_ID_MOTION_Y_ADD_NAME.c_str()); // Y+
	KEY_ID_MOTION_Y_SUB = KeyNameToKeyCode(KEY_ID_MOTION_Y_SUB_NAME.c_str()); // Y−
	KEY_ID_MOTION_Z_ADD = KeyNameToKeyCode(KEY_ID_MOTION_Z_ADD_NAME.c_str()); // Z+
	KEY_ID_MOTION_Z_SUB = KeyNameToKeyCode(KEY_ID_MOTION_Z_SUB_NAME.c_str()); // Z−

	KEY_ID_TOUCHPAD_SWIPE_UP_NAME = IniFile.ReadString("Keys", "TOUCHPAD-SWIPE-UP", "7");
	KEY_ID_TOUCHPAD_SWIPE_DOWN_NAME = IniFile.ReadString("Keys", "TOUCHPAD-SWIPE-DOWN", "8");
	KEY_ID_TOUCHPAD_SWIPE_LEFT_NAME = IniFile.ReadString("Keys", "TOUCHPAD-SWIPE-LEFT", "9");
	KEY_ID_TOUCHPAD_SWIPE_RIGHT_NAME = IniFile.ReadString("Keys", "TOUCHPAD-SWIPE-RIGHT", "0");

	KEY_ID_TOUCHPAD_FIRST_UP_NAME = IniFile.ReadString("Keys", "TOUCHPAD-FIRST-UP", "U");
	KEY_ID_TOUCHPAD_FIRST_DOWN_NAME = IniFile.ReadString("Keys", "TOUCHPAD-FIRST-DOWN", "J");
	KEY_ID_TOUCHPAD_FIRST_LEFT_NAME = IniFile.ReadString("Keys", "TOUCHPAD-FIRST-LEFT", "H");
	KEY_ID_TOUCHPAD_FIRST_RIGHT_NAME = IniFile.ReadString("Keys", "TOUCHPAD-FIRST-RIGHT", "K");

	KEY_ID_TOUCHPAD_SECOND_UP_NAME = IniFile.ReadString("Keys", "TOUCHPAD-SECOND-UP", "HOME");
	KEY_ID_TOUCHPAD_SECOND_DOWN_NAME = IniFile.ReadString("Keys", "TOUCHPAD-SECOND-DOWN", "END");
	KEY_ID_TOUCHPAD_SECOND_LEFT_NAME = IniFile.ReadString("Keys", "TOUCHPAD-SECOND-LEFT", "DELETE");
	KEY_ID_TOUCHPAD_SECOND_RIGHT_NAME = IniFile.ReadString("Keys", "TOUCHPAD-SECOND-RIGHT", "PAGE-DOWN");

	KEY_ID_TOUCHPAD_SWIPE_UP = KeyNameToKeyCode(KEY_ID_TOUCHPAD_SWIPE_UP_NAME.c_str());
	KEY_ID_TOUCHPAD_SWIPE_DOWN = KeyNameToKeyCode(KEY_ID_TOUCHPAD_SWIPE_DOWN_NAME.c_str());
	KEY_ID_TOUCHPAD_SWIPE_LEFT = KeyNameToKeyCode(KEY_ID_TOUCHPAD_SWIPE_LEFT_NAME.c_str());
	KEY_ID_TOUCHPAD_SWIPE_RIGHT = KeyNameToKeyCode(KEY_ID_TOUCHPAD_SWIPE_RIGHT_NAME.c_str());

	KEY_ID_TOUCHPAD_FIRST_UP = KeyNameToKeyCode(KEY_ID_TOUCHPAD_FIRST_UP_NAME.c_str());
	KEY_ID_TOUCHPAD_FIRST_DOWN = KeyNameToKeyCode(KEY_ID_TOUCHPAD_FIRST_DOWN_NAME.c_str());
	KEY_ID_TOUCHPAD_FIRST_LEFT = KeyNameToKeyCode(KEY_ID_TOUCHPAD_FIRST_LEFT_NAME.c_str());
	KEY_ID_TOUCHPAD_FIRST_RIGHT = KeyNameToKeyCode(KEY_ID_TOUCHPAD_FIRST_RIGHT_NAME.c_str());

	KEY_ID_TOUCHPAD_SECOND_UP = KeyNameToKeyCode(KEY_ID_TOUCHPAD_SECOND_UP_NAME.c_str());
	KEY_ID_TOUCHPAD_SECOND_DOWN = KeyNameToKeyCode(KEY_ID_TOUCHPAD_SECOND_DOWN_NAME.c_str());
	KEY_ID_TOUCHPAD_SECOND_LEFT = KeyNameToKeyCode(KEY_ID_TOUCHPAD_SECOND_LEFT_NAME.c_str());
	KEY_ID_TOUCHPAD_SECOND_RIGHT = KeyNameToKeyCode(KEY_ID_TOUCHPAD_SECOND_RIGHT_NAME.c_str());
}

// --- ATUALIZADO: FUNÇÃO DE TEXTO PARA MOSTRAR CONFIGS XIM ---
void DefaultMainText() {
	if (EmulationMode == XboxMode) {
		printf("\n Emulating a DualShock 4 using an Xbox controller.\n");
		// ... (código xbox omitido para economizar espaço, mantenha se usar)
	}
	else {
		printf("\n [DS4 XIM MATRIX MODE ACTIVATED]\n");
		printf(" ---------------------------------------------------------\n");
		printf(" HIP SETTINGS (Default):\n");
		printf(" Sens: %.1f | Smooth: %.2f | Curve: %.2f | Boost: %d\n", HipSettings.SensX, HipSettings.Smoothing, HipSettings.Curve, HipSettings.Boost);
		printf(" ---------------------------------------------------------\n");
		printf(" ADS SETTINGS (Hold Right Click):\n");
		printf(" Sens: %.1f | Smooth: %.2f | Curve: %.2f | Boost: %d\n", AdsSettings.SensX, AdsSettings.Smoothing, AdsSettings.Curve, AdsSettings.Boost);
		printf(" ---------------------------------------------------------\n\n");

		if (ActivateInAnyWindow == false)
			printf(" Only active in: \"%s\" and \"%s\".\n Switch mode with \"ALT + F3\".\n", WindowTitle.c_str(), WindowTitle2.c_str());
		else
			printf(" Active in ANY WINDOW. Switch mode with \"ALT + F3\".\n");
		
		printf_s(" Keyboard Profile: \"%s\". Change with \"ALT + Up/Down\".\n", KMProfiles[ProfileIndex].substr(0, KMProfiles[ProfileIndex].size() - 4).c_str());

		if (LeftAnalogStick == false) printf(" Left Stick: DIGITAL (WASD). Toggle: \"ALT + 1\".\n");
		else printf(" Left Stick: ANALOG (Gradual). Toggle: \"ALT + 1\".\n");

		if (EmulateAnalogTriggers == false) printf(" Triggers: DIGITAL (Instant). Toggle: \"ALT + 2\".\n");
		else printf(" Triggers: ANALOG (Gradual). Toggle: \"ALT + 2\".\n");

		if (SwapSticks == false) printf(" Aiming with MOUSE (Right Stick).\n");
		else printf(" Aiming with KEYBOARD (Left Stick).\n");
	}
	
	if (EmulationMode == KBMode) {
		printf("\n [SHORTCUTS]\n");
		printf(" ALT + F10: Fullscreen PS Plus\n");
		printf(" ALT + F2:  Hide/Show Cursor\n");
		printf(" ALT + C:   Re-center Mouse\n");
		printf(" ALT + ESC: EXIT\n");
	}
}

void RussianMainText() {
	// ... (Versão Russa pode ficar igual ou ser atualizada se você usar)
	// Para garantir que funcione, vamos usar a DefaultMainText se a língua for inglês.
	if (EmulationMode == XboxMode) {
		printf("\n Эмуляция DualShock 4 с помощью контроллера Xbox.\n");
	}
	else {
		printf("\n [РЕЖИМ DS4 XIM MATRIX АКТИВИРОВАН]\n");
		printf(" ---------------------------------------------------------\n");
		printf(" HIP (ОТ БЕДРА):\n");
		printf(" Чувств: %.1f | Сглаживание: %.2f | Кривая: %.2f | Буст: %d\n", HipSettings.SensX, HipSettings.Smoothing, HipSettings.Curve, HipSettings.Boost);
		printf(" ---------------------------------------------------------\n");
		printf(" ADS (ПРИЦЕЛИВАНИЕ - ПКМ):\n");
		printf(" Чувств: %.1f | Сглаживание: %.2f | Кривая: %.2f | Буст: %d\n", AdsSettings.SensX, AdsSettings.Smoothing, AdsSettings.Curve, AdsSettings.Boost);
		printf(" ---------------------------------------------------------\n\n");
		
		// ... resto do código russo original ...
		printf(" Нажмите \"ALT + Escape\", чтобы выйти.\n");
	}
}

void MainTextUpdate() {
	system("cls");
	if (Lang == LANG_RUSSIAN)
		RussianMainText();
	else
		DefaultMainText();
}

int main(int argc, char **argv)
{
	SetConsoleTitle("DS4Emulator 2.2 - XIM Enhanced");
	WindowToCenter();

	if (PRIMARYLANGID(GetUserDefaultLangID()) == LANG_RUSSIAN) { // Resave cpp file with UTF8 BOM
		Lang = LANG_RUSSIAN;
		setlocale(LC_ALL, ""); // Output locale
		setlocale(LC_NUMERIC, "C"); // Numbers with a dot
		system("chcp 65001 > nul"); // Console UTF8 output
	}

	CIniReader IniFile("Config.ini"); // Config

	if (IniFile.ReadBoolean("Motion", "Activate", true)) {
		WSADATA wsaData;
		int iResult;
		iResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
		if (iResult == 0) {
			struct sockaddr_in local;
			fromlen = sizeof(from);
			local.sin_family = AF_INET;
			local.sin_port = htons(IniFile.ReadInteger("Motion", "Port", 5555));
			local.sin_addr.s_addr = INADDR_ANY;

			socketS = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

			u_long nonblocking_enabled = true;
			ioctlsocket(socketS, FIONBIO, &nonblocking_enabled);

			if (socketS != INVALID_SOCKET) {
				iResult = bind(socketS, (sockaddr*)&local, sizeof(local));
				if (iResult != SOCKET_ERROR) {
					SocketActivated = true;
					pSocketThread = new std::thread(MotionReceiver);
				}
				else {
					WSACleanup();
					SocketActivated = false;
				}
			}
			else {
				WSACleanup();
				SocketActivated = false;
			}
		}
		else {
			WSACleanup();
			SocketActivated = false;
		}
	}
	int MotionSens = IniFile.ReadInteger("Motion", "Sens", 100);
	float GyroSens = 35 - IniFile.ReadInteger("Motion", "GyroSens", 100) / 100 * 33;
	float AccelSens = 160 - IniFile.ReadInteger("Motion", "AccelSens", 100) / 100 * 140;
	int InverseXStatus = IniFile.ReadBoolean("Motion", "InverseX", false) == false ? 1 : -1;
	int InverseYStatus = IniFile.ReadBoolean("Motion", "InverseY", false) == false ? 1 : -1;
	int InverseZStatus = IniFile.ReadBoolean("Motion", "InverseZ", false) == false ? 1 : -1;
	MotionOrientation = IniFile.ReadBoolean("Motion", "Orientation", true);

	SleepTimeOutMotion = IniFile.ReadInteger("Motion", "SleepTimeOut", 1);

#define OCR_NORMAL 32512
	HCURSOR CurCursor = CopyCursor(LoadCursor(0, IDC_ARROW));
	HCURSOR CursorEmpty = LoadCursorFromFile("EmptyCursor.cur");
	CursorHidden = IniFile.ReadBoolean("KeyboardMouse", "HideCursorAfterStart", false);
	if (CursorHidden) { SetSystemCursor(CursorEmpty, OCR_NORMAL); CursorHidden = true; }

	// Config parameters
	bool InvertX = IniFile.ReadBoolean("Main", "InvertX", false);
	bool InvertY = IniFile.ReadBoolean("Main", "InvertY", false);

	int SleepTimeOutXbox = IniFile.ReadInteger("Xbox", "SleepTimeOut", 1);
	SwapTriggersShoulders = IniFile.ReadBoolean("Xbox", "SwapTriggersShoulders", false);
	SwapShareTouchPad = IniFile.ReadBoolean("Xbox", "SwapShareTouchPad", false);
	TouchPadPressedWhenSwiping = IniFile.ReadBoolean("Xbox", "TouchPadPressedWhenSwiping", false);
	bool EnableXboxButton = IniFile.ReadBoolean("Xbox", "EnableXboxButton", true);

	KEY_ID_XBOX_ACTIVATE_MULTI_NAME = IniFile.ReadString("Xbox", "MultiActivateKey", "BACK");
	int KEY_ID_XBOX_ACTIVATE_MULTI = XboxKeyNameToXboxKeyCode(KEY_ID_XBOX_ACTIVATE_MULTI_NAME);
	KEY_ID_XBOX_MOTION_SHAKING_NAME = IniFile.ReadString("Xbox", "MotionShakingKey", "RIGHT-SHOULDER");
	int KEY_ID_XBOX_MOTION_SHAKING = XboxKeyNameToXboxKeyCode(KEY_ID_XBOX_MOTION_SHAKING_NAME);

	bool DisableButtonOnMotion = IniFile.ReadBoolean("Xbox", "DisableButtonOnMotion", false);
	KEY_ID_XBOX_MOTION_X_ADD_NAME = IniFile.ReadString("Xbox", "MotionXAdd", "DPAD-UP");
	KEY_ID_XBOX_MOTION_X_SUB_NAME = IniFile.ReadString("Xbox", "MotionXSub", "DPAD-DOWN");
	KEY_ID_XBOX_MOTION_Y_ADD_NAME = IniFile.ReadString("Xbox", "MotionYAdd", "NONE");
	KEY_ID_XBOX_MOTION_Y_SUB_NAME = IniFile.ReadString("Xbox", "MotionYSub", "NONE");
	KEY_ID_XBOX_MOTION_Z_ADD_NAME = IniFile.ReadString("Xbox", "MotionZAdd", "DPAD-LEFT");
	KEY_ID_XBOX_MOTION_Z_SUB_NAME = IniFile.ReadString("Xbox", "MotionZSub", "DPAD-RIGHT");
	int KEY_ID_XBOX_MOTION_X_ADD = XboxKeyNameToXboxKeyCode(KEY_ID_XBOX_MOTION_X_ADD_NAME);
	int KEY_ID_XBOX_MOTION_X_SUB = XboxKeyNameToXboxKeyCode(KEY_ID_XBOX_MOTION_X_SUB_NAME);
	int KEY_ID_XBOX_MOTION_Y_ADD = XboxKeyNameToXboxKeyCode(KEY_ID_XBOX_MOTION_Y_ADD_NAME);
	int KEY_ID_XBOX_MOTION_Y_SUB = XboxKeyNameToXboxKeyCode(KEY_ID_XBOX_MOTION_Y_SUB_NAME);
	int KEY_ID_XBOX_MOTION_Z_ADD = XboxKeyNameToXboxKeyCode(KEY_ID_XBOX_MOTION_Z_ADD_NAME);
	int KEY_ID_XBOX_MOTION_Z_SUB = XboxKeyNameToXboxKeyCode(KEY_ID_XBOX_MOTION_Z_SUB_NAME);

	float DeadZoneLeftStickX = IniFile.ReadFloat("Xbox", "DeadZoneLeftStickX", 0);
	float DeadZoneLeftStickY = IniFile.ReadFloat("Xbox", "DeadZoneLeftStickY", 0);
	float DeadZoneRightStickX = IniFile.ReadFloat("Xbox", "DeadZoneRightStickX", 0);
	float DeadZoneRightStickY = IniFile.ReadFloat("Xbox", "DeadZoneRightStickY", 0);

	bool KMActivateAlways = IniFile.ReadBoolean("KeyboardMouse", "ActivateAlways", false);
	KEY_ID_STOP_CENTERING_NAME = IniFile.ReadString("KeyboardMouse", "StopCenteringKey", "C");
	int KEY_ID_STOP_CENTERING = KeyNameToKeyCode(KEY_ID_STOP_CENTERING_NAME);
	bool CenteringEnable = IniFile.ReadBoolean("KeyboardMouse", "EnableCentering", true);
	std::string DefaultProfile = IniFile.ReadString("KeyboardMouse", "DefaultProfile", "Default.ini");

	int SleepTimeOutKB = IniFile.ReadInteger("KeyboardMouse", "SleepTimeOut", 1);
	std::string WindowTitle = IniFile.ReadString("KeyboardMouse", "ActivateOnlyInWindow", "PlayStation Plus");
	std::string WindowTitle2 = IniFile.ReadString("KeyboardMouse", "ActivateOnlyInWindow2", "PS4 Remote Play");
	int FullScreenTopOffset = IniFile.ReadInteger("KeyboardMouse", "FullScreenTopOffset", -50);
	bool HideTaskBar = IniFile.ReadBoolean("KeyboardMouse", "HideTaskBarInFullScreen", true);
	bool FullScreenMode = false;
	ActivateInAnyWindow = IniFile.ReadBoolean("KeyboardMouse", "ActivateInAnyWindow", false);
	EmulateAnalogTriggers = IniFile.ReadBoolean("KeyboardMouse", "EmulateAnalogTriggers", false);
	float LeftTriggerValue = 0;
	float RightTriggerValue = 0;
	float StepTriggerValue = IniFile.ReadFloat("KeyboardMouse", "AnalogTriggerStep", 15);

	// --- CARREGAR CONFIGURAÇÕES HIP (PADRÃO) ---
	HipSettings.SensX = IniFile.ReadFloat("Hip", "SensX", 15.0f);
	HipSettings.SensY = IniFile.ReadFloat("Hip", "SensY", 15.0f);
	HipSettings.Smoothing = IniFile.ReadFloat("Hip", "Smoothing", 0.1f);
	HipSettings.Curve = IniFile.ReadFloat("Hip", "Curve", 1.0f);
	HipSettings.Boost = IniFile.ReadInteger("Hip", "Boost", 0);

	// --- CARREGAR CONFIGURAÇÕES ADS (MIRA) ---
	AdsSettings.SensX = IniFile.ReadFloat("ADS", "SensX", 10.0f);
	AdsSettings.SensY = IniFile.ReadFloat("ADS", "SensY", 10.0f);
	AdsSettings.Smoothing = IniFile.ReadFloat("ADS", "Smoothing", 0.4f);
	AdsSettings.Curve = IniFile.ReadFloat("ADS", "Curve", 1.0f);
	AdsSettings.Boost = IniFile.ReadInteger("ADS", "Boost", 0);

	SwapSticks = IniFile.ReadBoolean("KeyboardMouse", "SwapSticks", false);
	LeftAnalogStick = IniFile.ReadBoolean("KeyboardMouse", "EmulateLeftAnalogStick", false);
	int LeftAnalogX = 128, LeftAnalogY = 128;
	int AnalogStickLeft = IniFile.ReadFloat("KeyboardMouse", "AnalogStickStep", 15);
	int DeadZoneDS4 = IniFile.ReadInteger("KeyboardMouse", "DeadZone", 0);

	static auto start = std::chrono::steady_clock::now();

	// Search keyboard and mouses profiles
	WIN32_FIND_DATA ffd;
	HANDLE hFind = INVALID_HANDLE_VALUE;
	hFind = FindFirstFile("Profiles\\*.ini", &ffd);
	KMProfiles.push_back("Default.ini");
	int ProfileCount = 0;
	if (hFind != INVALID_HANDLE_VALUE) {
		do {
			if (strcmp(ffd.cFileName, "Default.ini")) {
				KMProfiles.push_back(ffd.cFileName);
				ProfileCount++;
				if (strcmp(ffd.cFileName, DefaultProfile.c_str()) == 0)
					ProfileIndex = ProfileCount;
			}
		} while (FindNextFile(hFind, &ffd) != 0);
		FindClose(hFind);
	}

	LoadKMProfile(KMProfiles[ProfileIndex]);

	const auto client = vigem_alloc();
	auto ret = vigem_connect(client);
	const auto ds4 = vigem_target_ds4_alloc();
	ret = vigem_target_add(client, ds4);
	ret = vigem_target_ds4_register_notification(client, ds4, &notification, nullptr);
	DS4_REPORT_EX report;
	bool TouchpadSwipeUp = false, TouchpadSwipeDown = false;
	bool TouchpadSwipeLeft = false, TouchpadSwipeRight = false;
	bool MotionShakingSwap = false;

	int SkipPollCount = 0;

	// Load library and scan Xbox gamepads
	hDll = LoadLibrary("xinput1_3.dll");
	if (hDll == NULL) hDll = LoadLibrary("xinput1_4.dll");
	if (hDll != NULL) {
		MyXInputGetState = (_XInputGetState)GetProcAddress(hDll, (LPCSTR)100);
		MyXInputSetState = (_XInputSetState)GetProcAddress(hDll, "XInputSetState");
		if (MyXInputGetState == NULL || MyXInputSetState == NULL)
			hDll = NULL;
	}

	if (KMActivateAlways == false && hDll != NULL)
		for (int i = 0; i < XUSER_MAX_COUNT; ++i)
			if (MyXInputGetState(i, &myPState) == ERROR_SUCCESS)
			{
				XboxUserIndex = i;
				EmulationMode = XboxMode;
				break;
			}

	if (EmulationMode == KBMode) {
		m_HalfWidth = GetSystemMetrics(SM_CXSCREEN) / 2;
		m_HalfHeight = GetSystemMetrics(SM_CYSCREEN) / 2;
	}

	MainTextUpdate();

	DS4_TOUCH BuffPreviousTouch[2] = { 0, 0 };
	BuffPreviousTouch[0].bIsUpTrackingNum1 = 0x80;
	BuffPreviousTouch[1].bIsUpTrackingNum1 = 0x80;
	unsigned char TouchIndex = 0;
	bool AllowIncTouchIndex = false;
	bool DeadZoneMode = false;

	Touch2.LeftMode = 2;
	static uint8_t TouchPacket = 0;

	while (!(IsKeyPressed(VK_LMENU) && IsKeyPressed(VK_ESCAPE)))
	{
		DS4_REPORT_INIT_EX(&report);

		ResetTouchData(Touch1);
		ResetTouchData(Touch2);

		report.bTouchPacketsN = 0;
		report.sCurrentTouch = { 0 };
		report.sPreviousTouch[0] = { 0 };
		report.sPreviousTouch[1] = { 0 };

		static decltype(report.sCurrentTouch) LastTouch = { 0 };
		static bool LastTouchValid = false;

		report.sCurrentTouch.bIsUpTrackingNum1 = uint8_t(0x80 | (Touch1.ID & 0x7F));
		report.sCurrentTouch.bIsUpTrackingNum2 = uint8_t(0x80 | (Touch2.ID & 0x7F));

		report.bBatteryLvl = 11;

		bool MotionShaking = false, MotionXAdd = false, MotionXSub = false, MotionYAdd = false, MotionYSub = false, MotionZAdd = false, MotionZSub = false;

		// Xbox mode
		if (EmulationMode == XboxMode) {
			DWORD myStatus = ERROR_DEVICE_NOT_CONNECTED;
			if (hDll != NULL)
				myStatus = MyXInputGetState(XboxUserIndex, &myPState);

			if (myStatus == ERROR_SUCCESS) {

				if (SkipPollCount == 0 && IsKeyPressed(VK_MENU)) {
					if (IsKeyPressed('1'))
					{
						SwapTriggersShoulders = !SwapTriggersShoulders;
						MainTextUpdate();
						SkipPollCount = SkipPollTimeOut;
					}

					if (IsKeyPressed('2'))
					{
						SwapShareTouchPad = !SwapShareTouchPad;
						MainTextUpdate();
						SkipPollCount = SkipPollTimeOut;
					}

					if (IsKeyPressed('3'))
					{
						TouchPadPressedWhenSwiping = !TouchPadPressedWhenSwiping;
						MainTextUpdate();
						SkipPollCount = SkipPollTimeOut;
					}

					if (IsKeyPressed(VK_F9))
					{
						DeadZoneMode = !DeadZoneMode;
						if (DeadZoneMode == false)
							MainTextUpdate();
						SkipPollCount = SkipPollTimeOut;
					}
				}

				if (DeadZoneMode) {
					printf(" Left Stick X=%.2f, ", abs(myPState.Gamepad.sThumbLX / (32767 / 100.0f)));
					printf("Y=%3.2f | ", abs(myPState.Gamepad.sThumbLY / (32767 / 100.0f)));
					printf("Right Stick X=%.2f ", abs(myPState.Gamepad.sThumbRX / (32767 / 100.0f)));
					printf("Y=%3.2f\n", abs(myPState.Gamepad.sThumbRY / (32767 / 100.0f)));
				}

				myPState.Gamepad.sThumbLX = DeadZoneXboxAxis(myPState.Gamepad.sThumbLX, DeadZoneLeftStickX);
				myPState.Gamepad.sThumbLY = DeadZoneXboxAxis(myPState.Gamepad.sThumbLY, DeadZoneLeftStickY);
				myPState.Gamepad.sThumbRX = DeadZoneXboxAxis(myPState.Gamepad.sThumbRX, DeadZoneRightStickX);
				myPState.Gamepad.sThumbRY = DeadZoneXboxAxis(myPState.Gamepad.sThumbRY, DeadZoneRightStickY);

				report.bThumbLX = ((myPState.Gamepad.sThumbLX + ((USHRT_MAX / 2) + 1)) / 257);
				report.bThumbLY = (-(myPState.Gamepad.sThumbLY + ((USHRT_MAX / 2) - 1)) / 257);
				report.bThumbLY = (report.bThumbLY == 0) ? 0xFF : report.bThumbLY;

				if (InvertX == false)
					report.bThumbRX = ((myPState.Gamepad.sThumbRX + ((USHRT_MAX / 2) + 1)) / 257);
				else
					report.bThumbRX = ((-myPState.Gamepad.sThumbRX + ((USHRT_MAX / 2) + 1)) / 257);

				if (InvertY == false)
					report.bThumbRY = (-(myPState.Gamepad.sThumbRY + ((USHRT_MAX / 2) + 1)) / 257);
				else
					report.bThumbRY = (-(-myPState.Gamepad.sThumbRY + ((USHRT_MAX / 2) + 1)) / 257);

				report.bThumbRY = (report.bThumbRY == 0) ? 0xFF : report.bThumbRY;

				if (myPState.Gamepad.wButtons & XINPUT_GAMEPAD_BACK && myPState.Gamepad.wButtons & XINPUT_GAMEPAD_START) {
					myPState.Gamepad.wButtons &= ~XINPUT_GAMEPAD_BACK; myPState.Gamepad.wButtons &= ~XINPUT_GAMEPAD_START;
					if (SwapShareTouchPad)
						report.bSpecial |= DS4_SPECIAL_BUTTON_TOUCHPAD;
					else
						report.wButtons |= DS4_BUTTON_SHARE;
				}

				if (myPState.Gamepad.wButtons & XINPUT_GAMEPAD_START)
					report.wButtons |= DS4_BUTTON_OPTIONS;

				if (myPState.Gamepad.wButtons & XINPUT_GAMEPAD_BACK && myPState.Gamepad.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER) {
					myPState.Gamepad.wButtons &= ~XINPUT_GAMEPAD_BACK; myPState.Gamepad.wButtons &= ~XINPUT_GAMEPAD_LEFT_SHOULDER;
					report.bSpecial |= DS4_SPECIAL_BUTTON_PS;
				}

				if (EnableXboxButton && myPState.Gamepad.wButtons & XINPUT_GAMEPAD_GUIDE) report.bSpecial |= DS4_SPECIAL_BUTTON_PS;

				if (myPState.Gamepad.wButtons & KEY_ID_XBOX_ACTIVATE_MULTI && myPState.Gamepad.wButtons & KEY_ID_XBOX_MOTION_SHAKING) {
					myPState.Gamepad.wButtons &= ~KEY_ID_XBOX_ACTIVATE_MULTI; if (DisableButtonOnMotion) myPState.Gamepad.wButtons &= ~KEY_ID_XBOX_MOTION_SHAKING;
					MotionShaking = true;
				};

				if (myPState.Gamepad.wButtons & KEY_ID_XBOX_ACTIVATE_MULTI && (myPState.Gamepad.wButtons & KEY_ID_XBOX_MOTION_X_ADD)) {
					myPState.Gamepad.wButtons &= ~KEY_ID_XBOX_ACTIVATE_MULTI; if (DisableButtonOnMotion) myPState.Gamepad.wButtons &= ~KEY_ID_XBOX_MOTION_X_ADD;
					MotionXAdd = true;
				}

				if (myPState.Gamepad.wButtons & KEY_ID_XBOX_ACTIVATE_MULTI && (myPState.Gamepad.wButtons & KEY_ID_XBOX_MOTION_X_SUB)) {
					myPState.Gamepad.wButtons &= ~KEY_ID_XBOX_ACTIVATE_MULTI; if (DisableButtonOnMotion) myPState.Gamepad.wButtons &= ~KEY_ID_XBOX_MOTION_X_SUB;
					MotionXSub = true;
				}

				if (myPState.Gamepad.wButtons & KEY_ID_XBOX_ACTIVATE_MULTI && (myPState.Gamepad.wButtons & KEY_ID_XBOX_MOTION_Y_ADD)) {
					myPState.Gamepad.wButtons &= ~KEY_ID_XBOX_ACTIVATE_MULTI; if (DisableButtonOnMotion) myPState.Gamepad.wButtons &= ~KEY_ID_XBOX_MOTION_Y_ADD;
					MotionYAdd = true;
				}

				if (myPState.Gamepad.wButtons & KEY_ID_XBOX_ACTIVATE_MULTI && (myPState.Gamepad.wButtons & KEY_ID_XBOX_MOTION_Y_SUB)) {
					myPState.Gamepad.wButtons &= ~KEY_ID_XBOX_ACTIVATE_MULTI; if (DisableButtonOnMotion) myPState.Gamepad.wButtons &= ~KEY_ID_XBOX_MOTION_Y_SUB;
					MotionYSub = true;
				}

				if (myPState.Gamepad.wButtons & KEY_ID_XBOX_ACTIVATE_MULTI && (myPState.Gamepad.wButtons & KEY_ID_XBOX_MOTION_Z_ADD)) {
					myPState.Gamepad.wButtons &= ~KEY_ID_XBOX_ACTIVATE_MULTI; if (DisableButtonOnMotion) myPState.Gamepad.wButtons &= ~KEY_ID_XBOX_MOTION_Z_ADD;
					MotionZAdd = true;
				}

				if (myPState.Gamepad.wButtons & KEY_ID_XBOX_ACTIVATE_MULTI && (myPState.Gamepad.wButtons & KEY_ID_XBOX_MOTION_Z_SUB)) {
					myPState.Gamepad.wButtons &= ~KEY_ID_XBOX_ACTIVATE_MULTI; if (DisableButtonOnMotion) myPState.Gamepad.wButtons &= ~KEY_ID_XBOX_MOTION_Z_SUB;
					MotionZSub = true;
				}

				bool XboxActivateMotionPressed = myPState.Gamepad.wButtons & KEY_ID_XBOX_ACTIVATE_MULTI ? true : false;

				if (SwapShareTouchPad == false) {
					if (IsKeyPressed(KEY_ID_SHARE))
						report.wButtons |= DS4_BUTTON_SHARE;
					if (myPState.Gamepad.wButtons & XINPUT_GAMEPAD_BACK)
						report.bSpecial |= DS4_SPECIAL_BUTTON_TOUCHPAD;
				}
				else {
					if (IsKeyPressed(KEY_ID_SHARE))
						report.bSpecial |= DS4_SPECIAL_BUTTON_TOUCHPAD;
					if (myPState.Gamepad.wButtons & XINPUT_GAMEPAD_BACK)
						report.wButtons |= DS4_BUTTON_SHARE;
				}

				if (XboxActivateMotionPressed == false) {
					if (myPState.Gamepad.wButtons & XINPUT_GAMEPAD_Y)
						report.wButtons |= DS4_BUTTON_TRIANGLE;
					if (myPState.Gamepad.wButtons & XINPUT_GAMEPAD_X)
						report.wButtons |= DS4_BUTTON_SQUARE;
					if (myPState.Gamepad.wButtons & XINPUT_GAMEPAD_B)
						report.wButtons |= DS4_BUTTON_CIRCLE;
					if (myPState.Gamepad.wButtons & XINPUT_GAMEPAD_A)
						report.wButtons |= DS4_BUTTON_CROSS;

					if (myPState.Gamepad.wButtons & XINPUT_GAMEPAD_LEFT_THUMB)
						report.wButtons |= DS4_BUTTON_THUMB_LEFT;
					if (myPState.Gamepad.wButtons & XINPUT_GAMEPAD_RIGHT_THUMB)
						report.wButtons |= DS4_BUTTON_THUMB_RIGHT;

					if (SwapTriggersShoulders == false) {
						if (myPState.Gamepad.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER)
							report.wButtons |= DS4_BUTTON_SHOULDER_LEFT;
						if (myPState.Gamepad.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER)
							report.wButtons |= DS4_BUTTON_SHOULDER_RIGHT;

						report.bTriggerL = myPState.Gamepad.bLeftTrigger;
						report.bTriggerR = myPState.Gamepad.bRightTrigger;
					}
					else {
						if (myPState.Gamepad.bLeftTrigger > 0)
							report.wButtons |= DS4_BUTTON_SHOULDER_LEFT;
						if (myPState.Gamepad.bRightTrigger > 0)
							report.wButtons |= DS4_BUTTON_SHOULDER_RIGHT;
						if (myPState.Gamepad.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER)
							report.bTriggerL = 255;
						if (myPState.Gamepad.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER)
							report.bTriggerR = 255;
					}

					if (report.bTriggerL > 0)
						report.wButtons |= DS4_BUTTON_TRIGGER_LEFT;
					if (report.bTriggerR > 0)
						report.wButtons |= DS4_BUTTON_TRIGGER_RIGHT;

					if (myPState.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_UP)
						DS4_SET_DPAD_EX(&report, DS4_BUTTON_DPAD_NORTH);
					if (myPState.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_DOWN)
						DS4_SET_DPAD_EX(&report, DS4_BUTTON_DPAD_SOUTH);
					if (myPState.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_LEFT)
						DS4_SET_DPAD_EX(&report, DS4_BUTTON_DPAD_WEST);
					if (myPState.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_RIGHT)
						DS4_SET_DPAD_EX(&report, DS4_BUTTON_DPAD_EAST);

					if (myPState.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_UP && myPState.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_RIGHT)
						DS4_SET_DPAD_EX(&report, DS4_BUTTON_DPAD_NORTHEAST);
					if (myPState.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_DOWN && myPState.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_LEFT)
						DS4_SET_DPAD_EX(&report, DS4_BUTTON_DPAD_SOUTHWEST);
					if (myPState.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_LEFT && myPState.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_UP)
						DS4_SET_DPAD_EX(&report, DS4_BUTTON_DPAD_NORTHWEST);
					if (myPState.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_RIGHT && myPState.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_DOWN)
						DS4_SET_DPAD_EX(&report, DS4_BUTTON_DPAD_SOUTHEAST);
				}

				if (report.bSpecial & DS4_SPECIAL_BUTTON_TOUCHPAD) {
					if (!TouchPadPressedWhenSwiping && (report.bThumbRX != 127 || report.bThumbRY != 129)) {
						report.bSpecial &= ~DS4_SPECIAL_BUTTON_TOUCHPAD;
						if (myPState.Gamepad.wButtons & XINPUT_GAMEPAD_RIGHT_THUMB) { report.wButtons &= ~DS4_BUTTON_THUMB_RIGHT; report.bSpecial |= DS4_SPECIAL_BUTTON_TOUCHPAD; }
					}
					Touch1.X = 960; Touch1.Y = 471;

					double LeftStickValue = StickDeviationPercent(report.bThumbLX, report.bThumbLY);
					double RightStickValue = StickDeviationPercent(report.bThumbRX, report.bThumbRY);

					if (report.bThumbRX > 127)
						Touch1.X = 200 + trunc(1519 * RightStickValue);
					if (report.bThumbRX < 127)
						Touch1.X = 1719 - trunc(1519 * RightStickValue);

					if (report.bThumbRY > 129)
						Touch1.Y = 100 + trunc(741 * RightStickValue);
					if (report.bThumbRY < 129)
						Touch1.Y = 743 - trunc(741 * RightStickValue);

					if (report.bThumbLX > 127)
						Touch2.X = 200 + trunc(1519 * LeftStickValue);
					if (report.bThumbLX < 127)
						Touch2.X = 1719 - trunc(1519 * LeftStickValue);

					if (report.bThumbLY > 129)
						Touch2.Y = 100 + trunc(741 * LeftStickValue);
					if (report.bThumbLY < 129)
						Touch2.Y = 743 - trunc(741 * LeftStickValue);

				}

				if (XboxActivateMotionPressed) {
					report.bThumbLX = 128; report.bThumbLY = 128;
					report.bThumbRX = 128; report.bThumbRY = 128;
				}

			}
		}
		// Mouse and keyboard mode
		else if (EmulationMode == KBMode) {

			PSPlusWindow = FindWindow(NULL, WindowTitle.c_str());
			bool PSNowFound = (PSPlusWindow != 0) && (IsWindowVisible(PSPlusWindow)) && (PSPlusWindow == GetForegroundWindow());

			PSRemotePlayWindow = FindWindow(NULL, WindowTitle2.c_str());
			bool PSRemotePlayFound = (PSRemotePlayWindow != 0) && (IsWindowVisible(PSRemotePlayWindow)) && (PSRemotePlayWindow == GetForegroundWindow());

			if (SkipPollCount == 0 && IsKeyPressed(VK_MENU)) {
				if (IsKeyPressed(VK_UP) || IsKeyPressed(VK_DOWN)) {
					SkipPollCount = SkipPollTimeOut;
					if (IsKeyPressed(VK_UP)) if (ProfileIndex > 0) ProfileIndex--; else ProfileIndex = KMProfiles.size() - 1;
					if (IsKeyPressed(VK_DOWN)) if (ProfileIndex < KMProfiles.size() - 1) ProfileIndex++; else ProfileIndex = 0;
					MainTextUpdate();
					LoadKMProfile(KMProfiles[ProfileIndex]);
				}

				if (IsKeyPressed('1'))
				{
					LeftAnalogStick = !LeftAnalogStick;
					MainTextUpdate();
					SkipPollCount = SkipPollTimeOut;
				}

				if (IsKeyPressed('2'))
				{
					EmulateAnalogTriggers = !EmulateAnalogTriggers;
					MainTextUpdate();
					SkipPollCount = SkipPollTimeOut;
				}

				if (IsKeyPressed('3'))
				{
					SwapSticks = !SwapSticks;
					MainTextUpdate();
					SkipPollCount = SkipPollTimeOut;
				}

				if (IsKeyPressed(VK_F10)) {
					if (PSPlusWindow != 0)
						if (FullScreenMode) {
							if (HideTaskBar) ShowWindow(FindWindow("Shell_TrayWnd", NULL), SW_SHOW);
							SetWindowPos(PSPlusWindow, HWND_TOP, m_HalfWidth - 640, m_HalfHeight - 360, 1280, 720, SWP_FRAMECHANGED);
						}
						else {
							SetForegroundWindow(PSPlusWindow);
							SetActiveWindow(PSPlusWindow);
							if (HideTaskBar) ShowWindow(FindWindow("Shell_TrayWnd", NULL), SW_HIDE);
							SetWindowPos(PSPlusWindow, HWND_TOP, 0, FullScreenTopOffset, m_HalfWidth * 2, m_HalfHeight * 2 + (-FullScreenTopOffset), SWP_FRAMECHANGED);
						}
					FullScreenMode = !FullScreenMode;
					SkipPollCount = SkipPollTimeOut;
				}

				if (CursorHidden == false && IsKeyPressed(VK_F2)) {
					SetSystemCursor(CursorEmpty, OCR_NORMAL); CursorHidden = true;
					printf(" The cursor is hidden. To display the cursor, press \"ALT\" + \"Escape\".\n");
					SkipPollCount = SkipPollTimeOut;
				}

				if (IsKeyPressed(VK_F3)) {
					ActivateInAnyWindow = !ActivateInAnyWindow;
					MainTextUpdate();
					SkipPollCount = SkipPollTimeOut;
				}
			}

			if (ActivateInAnyWindow || PSNowFound || PSRemotePlayFound) {
				if (IsKeyPressed(VK_MENU) && IsKeyPressed(KEY_ID_STOP_CENTERING) && SkipPollCount == 0) { CenteringEnable = !CenteringEnable;  SkipPollCount = SkipPollTimeOut; }
				if (CenteringEnable) GetMouseState();

				// ----------- INÍCIO DA LÓGICA XIM MATRIX (CORRIGIDA) -----------

				// 1. Detectar se é HIP ou ADS (Botão Direito)
				AimSettings* currentAim = (IsKeyPressed(VK_RBUTTON)) ? &AdsSettings : &HipSettings;

				// 2. Aplicar Inversão ANTES de processar
				double rawDeltaX = (double)DeltaMouseX;
				double rawDeltaY = (double)DeltaMouseY;

				if (InvertX) rawDeltaX *= -1;
				if (InvertY) rawDeltaY *= -1;

				// 3. Input Base (Sensibilidade do perfil atual)
				double targetX = rawDeltaX * currentAim->SensX;
				double targetY = rawDeltaY * currentAim->SensY;

				// 4. Suavização (Smoothing do perfil atual)
				// Interpolação Linear simples (Lerp) para filtrar jitter
				g_SmoothX = (g_SmoothX * (1.0 - currentAim->Smoothing)) + (targetX * currentAim->Smoothing);
				g_SmoothY = (g_SmoothY * (1.0 - currentAim->Smoothing)) + (targetY * currentAim->Smoothing);

				// 5. Curva Balística (Exponencial do perfil atual)
				double magX = pow(abs(g_SmoothX), currentAim->Curve);
				double magY = pow(abs(g_SmoothY), currentAim->Curve);

				// 6. Aplicar Boost (Anti-Deadzone do perfil atual)
				// Preservar sinal e aplicar Boost a partir do centro (128)
				int outX = 128;
				int outY = 128;

				if (magX > 0.1) {
					if (g_SmoothX > 0) outX = 128 + currentAim->Boost + (int)magX;
					else               outX = 128 - currentAim->Boost - (int)magX;
				}

				if (magY > 0.1) {
					if (g_SmoothY > 0) outY = 128 + currentAim->Boost + (int)magY;
					else               outY = 128 - currentAim->Boost - (int)magY;
				}

				// Clamp final para BYTE (0-255)
				report.bThumbRX = (BYTE)Clamp((float)outX, 0, 255);
				report.bThumbRY = (BYTE)Clamp((float)outY, 0, 255);

				// ----------- FIM DA LÓGICA XIM MATRIX -----------

				if (LeftAnalogStick == false) {
					if (IsKeyPressed(KEY_ID_LEFT_STICK_UP)) report.bThumbLY = 0;
					if (IsKeyPressed(KEY_ID_LEFT_STICK_DOWN)) report.bThumbLY = 255;
					if (IsKeyPressed(KEY_ID_LEFT_STICK_LEFT)) report.bThumbLX = 0;
					if (IsKeyPressed(KEY_ID_LEFT_STICK_RIGHT)) report.bThumbLX = 255;
				}
				else {
					if (IsKeyPressed(KEY_ID_LEFT_STICK_UP)) {
						if (LeftAnalogY > 0) LeftAnalogY -= AnalogStickLeft;
					}
					else if (IsKeyPressed(KEY_ID_LEFT_STICK_DOWN)) {
						if (LeftAnalogY < 255) LeftAnalogY += AnalogStickLeft;
					}
					else {
						if (LeftAnalogY > 128 + AnalogStickLeft) LeftAnalogY -= AnalogStickLeft;
						else if (LeftAnalogY < 128 - AnalogStickLeft) LeftAnalogY += AnalogStickLeft;
						else LeftAnalogY = 128;
					}

					if (IsKeyPressed(KEY_ID_LEFT_STICK_LEFT)) {
						if (LeftAnalogX > 0) LeftAnalogX -= AnalogStickLeft;
					}
					else if (IsKeyPressed(KEY_ID_LEFT_STICK_RIGHT)) {
						if (LeftAnalogX < 255) LeftAnalogX += AnalogStickLeft;
					}
					else {
						if (LeftAnalogX > 128 + AnalogStickLeft) LeftAnalogX -= AnalogStickLeft;
						else if (LeftAnalogX < 128 - AnalogStickLeft) LeftAnalogX += AnalogStickLeft;
						else LeftAnalogX = 128;
					}
					if (LeftAnalogX > 255) LeftAnalogX = 255;
					if (LeftAnalogX < 0) LeftAnalogX = 0;
					if (LeftAnalogY > 255) LeftAnalogY = 255;
					if (LeftAnalogY < 0) LeftAnalogY = 0;
					report.bThumbLY = LeftAnalogY;
					report.bThumbLX = LeftAnalogX;
				}

				if (SwapSticks) {
					std::swap(report.bThumbLX, report.bThumbRX);
					std::swap(report.bThumbLY, report.bThumbRY);
				}

				if (EmulateAnalogTriggers == false) {

					if (IsKeyPressed(KEY_ID_LEFT_TRIGGER))
						report.bTriggerL = 255;
					if (IsKeyPressed(KEY_ID_RIGHT_TRIGGER))
						report.bTriggerR = 255;
				}
				else {

					if (IsKeyPressed(KEY_ID_LEFT_TRIGGER)) {
						if (LeftTriggerValue < 255)
							LeftTriggerValue += StepTriggerValue;
					}
					else {
						if (LeftTriggerValue > 0)
							LeftTriggerValue -= StepTriggerValue;
					}

					report.bTriggerL = trunc(Clamp(LeftTriggerValue, 0, 255));

					if (IsKeyPressed(KEY_ID_RIGHT_TRIGGER)) {
						if (RightTriggerValue < 255)
							RightTriggerValue += StepTriggerValue;
					}
					else {
						if (RightTriggerValue > 0)
							RightTriggerValue -= StepTriggerValue;
					}

					report.bTriggerR = trunc(Clamp(RightTriggerValue, 0, 255));
				}

				if (report.bTriggerL > 0)
					report.wButtons |= DS4_BUTTON_TRIGGER_LEFT;
				if (report.bTriggerR > 0)
					report.wButtons |= DS4_BUTTON_TRIGGER_RIGHT;

				if (IsKeyPressed(KEY_ID_SHARE))
					report.wButtons |= DS4_BUTTON_SHARE;
				if (IsKeyPressed(KEY_ID_TOUCHPAD))
					report.bSpecial |= DS4_SPECIAL_BUTTON_TOUCHPAD;
				if (IsKeyPressed(KEY_ID_OPTIONS))
					report.wButtons |= DS4_BUTTON_OPTIONS;

				if (IsKeyPressed(KEY_ID_TRIANGLE))
					report.wButtons |= DS4_BUTTON_TRIANGLE;
				if (IsKeyPressed(KEY_ID_SQUARE))
					report.wButtons |= DS4_BUTTON_SQUARE;
				if (IsKeyPressed(KEY_ID_CIRCLE))
					report.wButtons |= DS4_BUTTON_CIRCLE;
				if (IsKeyPressed(KEY_ID_CROSS))
					report.wButtons |= DS4_BUTTON_CROSS;

				if (IsKeyPressed(KEY_ID_LEFT_THUMB))
					report.wButtons |= DS4_BUTTON_THUMB_LEFT;
				if (IsKeyPressed(KEY_ID_RIGHT_THUMB))
					report.wButtons |= DS4_BUTTON_THUMB_RIGHT;

				if (IsKeyPressed(KEY_ID_LEFT_SHOULDER))
					report.wButtons |= DS4_BUTTON_SHOULDER_LEFT;
				if (IsKeyPressed(KEY_ID_RIGHT_SHOULDER))
					report.wButtons |= DS4_BUTTON_SHOULDER_RIGHT;

				if (IsKeyPressed(KEY_ID_DPAD_UP))
					DS4_SET_DPAD_EX(&report, DS4_BUTTON_DPAD_NORTH);
				if (IsKeyPressed(KEY_ID_DPAD_DOWN))
					DS4_SET_DPAD_EX(&report, DS4_BUTTON_DPAD_SOUTH);
				if (IsKeyPressed(KEY_ID_DPAD_LEFT))
					DS4_SET_DPAD_EX(&report, DS4_BUTTON_DPAD_WEST);
				if (IsKeyPressed(KEY_ID_DPAD_RIGHT))
					DS4_SET_DPAD_EX(&report, DS4_BUTTON_DPAD_EAST);
			}
		}

		// Any mode
		if (IsKeyPressed(KEY_ID_TOUCHPAD_FIRST_LEFT)) {
			Touch1.X = TouchLeftMode(Touch1.LeftMode);
			Touch1.Y = TouchTopMode(Touch1.TopMode);
			if (Touch1.SkipPollLeft == 0) { if (Touch1.LeftMode > 0) --Touch1.LeftMode; Touch1.SkipPollLeft = 10; }
		}
		if (IsKeyPressed(KEY_ID_TOUCHPAD_FIRST_RIGHT)) {
			Touch1.X = TouchLeftMode(Touch1.LeftMode);
			Touch1.Y = TouchTopMode(Touch1.TopMode);
			if (Touch1.SkipPollLeft == 0) { if (Touch1.LeftMode < TOUCH_LEFT_MODE_MAX) ++Touch1.LeftMode; Touch1.SkipPollLeft = 10; }
		}
		if (IsKeyPressed(KEY_ID_TOUCHPAD_FIRST_UP)) {
			Touch1.X = TouchLeftMode(Touch1.LeftMode);
			Touch1.Y = TouchTopMode(Touch1.TopMode);
			if (Touch1.SkipPollTop == 0) { if (Touch1.TopMode > 0) --Touch1.TopMode; Touch1.SkipPollTop = SkipPollTimeOut; }
		}
		if (IsKeyPressed(KEY_ID_TOUCHPAD_FIRST_DOWN)) {
			Touch1.X = TouchLeftMode(Touch1.LeftMode);
			Touch1.Y = TouchTopMode(Touch1.TopMode);
			if (Touch1.SkipPollTop == 0) { if (Touch1.TopMode < TOUCH_TOP_MODE_MAX) ++Touch1.TopMode; Touch1.SkipPollTop = SkipPollTimeOut; }
		}

		if (IsKeyPressed(KEY_ID_TOUCHPAD_SECOND_LEFT)) {
			Touch2.X = TouchLeftMode(Touch2.LeftMode);
			Touch2.Y = TouchTopMode(Touch2.TopMode);
			if (Touch2.SkipPollLeft == 0) { if (Touch2.LeftMode > 0) --Touch2.LeftMode; Touch2.SkipPollLeft = 10; }
		}
		if (IsKeyPressed(KEY_ID_TOUCHPAD_SECOND_RIGHT)) {
			Touch2.X = TouchLeftMode(Touch2.LeftMode);
			Touch2.Y = TouchTopMode(Touch2.TopMode);
			if (Touch2.SkipPollLeft == 0) { if (Touch2.LeftMode < TOUCH_LEFT_MODE_MAX) ++Touch2.LeftMode; Touch2.SkipPollLeft = 10; }
		}
		if (IsKeyPressed(KEY_ID_TOUCHPAD_SECOND_UP)) {
			Touch2.X = TouchLeftMode(Touch2.LeftMode);
			Touch2.Y = TouchTopMode(Touch2.TopMode);
			if (Touch2.SkipPollTop == 0) { if (Touch2.TopMode > 0) --Touch2.TopMode; Touch2.SkipPollTop = SkipPollTimeOut; }
		}
		if (IsKeyPressed(KEY_ID_TOUCHPAD_SECOND_DOWN)) {
			Touch2.X = TouchLeftMode(Touch2.LeftMode);
			Touch2.Y = TouchTopMode(Touch2.TopMode);
			if (Touch2.SkipPollTop == 0) { if (Touch2.TopMode < TOUCH_TOP_MODE_MAX) ++Touch2.TopMode; Touch2.SkipPollTop = SkipPollTimeOut; }
		}

		if (TouchpadSwipeUp) { TouchpadSwipeUp = false; Touch1.X = 960; Touch1.Y = 100; }
		if (TouchpadSwipeDown) { TouchpadSwipeDown = false; Touch1.X = 960; Touch1.Y = 841; }
		if (TouchpadSwipeLeft) { TouchpadSwipeLeft = false; Touch1.X = 200; Touch1.Y = 471; }
		if (TouchpadSwipeRight) { TouchpadSwipeRight = false; Touch1.X = 1719; Touch1.Y = 471; }

		if (TouchpadSwipeUp == false && IsKeyPressed(KEY_ID_TOUCHPAD_SWIPE_UP)) { Touch1.X = 960; Touch1.Y = 841; TouchpadSwipeUp = true; }
		if (TouchpadSwipeDown == false && IsKeyPressed(KEY_ID_TOUCHPAD_SWIPE_DOWN)) { Touch1.X = 960; Touch1.Y = 100; TouchpadSwipeDown = true; }
		if (TouchpadSwipeLeft == false && IsKeyPressed(KEY_ID_TOUCHPAD_SWIPE_LEFT)) { Touch1.X = 1719; Touch1.Y = 471; TouchpadSwipeLeft = true; }
		if (TouchpadSwipeRight == false && IsKeyPressed(KEY_ID_TOUCHPAD_SWIPE_RIGHT)) { Touch1.X = 200; Touch1.Y = 471; TouchpadSwipeRight = true; }

		if (IsKeyPressed(KEY_ID_MOTION_SHAKING)) MotionShaking = true;
		if (IsKeyPressed(KEY_ID_MOTION_Y_ADD)) MotionYAdd = true;
		if (IsKeyPressed(KEY_ID_MOTION_Y_SUB)) MotionYSub = true;
		if (IsKeyPressed(KEY_ID_MOTION_X_SUB)) MotionXAdd = true;
		if (IsKeyPressed(KEY_ID_MOTION_X_ADD)) MotionXSub = true;
		if (IsKeyPressed(KEY_ID_MOTION_Z_ADD)) MotionZAdd = true;
		if (IsKeyPressed(KEY_ID_MOTION_Z_SUB)) MotionZSub = true;

		Touch1.IsChanged = (Touch1.X | Touch1.Y) != 0;
		Touch2.IsChanged = (Touch2.X | Touch2.Y) != 0;

		if (Touch1.IsChanged && !Touch1.PrevTouched) Touch1.ID = uint8_t((Touch1.ID + 1) & 0x7F);
		if (Touch2.IsChanged && !Touch2.PrevTouched) Touch2.ID = uint8_t((Touch2.ID + 1) & 0x7F);

		report.bTouchPacketsN = LastTouchValid ? 2 : 1;
		if (LastTouchValid) report.sPreviousTouch[0] = LastTouch;

		if (Touch1.IsChanged) {
			report.sCurrentTouch.bIsUpTrackingNum1 = uint8_t(Touch1.ID & 0x7F);
			report.sCurrentTouch.bTouchData1[0] = uint8_t(Touch1.X & 0xFF);
			report.sCurrentTouch.bTouchData1[1] = uint8_t(((Touch1.X >> 8) & 0x0F) | ((Touch1.Y & 0x0F) << 4));
			report.sCurrentTouch.bTouchData1[2] = uint8_t((Touch1.Y >> 4) & 0xFF);
		}

		if (Touch2.IsChanged) {
			report.sCurrentTouch.bIsUpTrackingNum2 = uint8_t(Touch2.ID & 0x7F);
			report.sCurrentTouch.bTouchData2[0] = uint8_t(Touch2.X & 0xFF);
			report.sCurrentTouch.bTouchData2[1] = uint8_t(((Touch2.X >> 8) & 0x0F) | ((Touch2.Y & 0x0F) << 4));
			report.sCurrentTouch.bTouchData2[2] = uint8_t((Touch2.Y >> 4) & 0xFF);
		}

		report.sCurrentTouch.bPacketCounter = ++TouchPacket;

		LastTouch = report.sCurrentTouch;
		LastTouchValid = true;

		Touch1.PrevTouched = Touch1.IsChanged;
		Touch2.PrevTouched = Touch2.IsChanged;

		report.wAccelX = trunc(Clamp(AccelX / AccelSens * 32767, -32767, 32767)) * InverseXStatus;
		report.wAccelY = trunc(Clamp(AccelY / AccelSens * 32767, -32767, 32767)) * InverseYStatus;
		report.wAccelZ = trunc(Clamp(AccelZ / AccelSens * 32767, -32767, 32767)) * InverseZStatus;
		report.wGyroX = trunc(Clamp(GyroX / GyroSens * 32767, -32767, 32767)) * InverseXStatus;
		report.wGyroY = trunc(Clamp(GyroY / GyroSens * 32767, -32767, 32767)) * InverseYStatus;
		report.wGyroZ = trunc(Clamp(GyroZ / GyroSens * 32767, -32767, 32767)) * InverseZStatus;

		if (MotionShaking) {
			MotionShakingSwap = !MotionShakingSwap;
			if (MotionShakingSwap) {
				report.wAccelX = -6530;		report.wAccelY = 6950;		report.wAccelZ = -710;
				report.wGyroX = 2300;		report.wGyroY = 5000;		report.wGyroZ = 10;
			}
			else {
				report.wAccelX = 6830;		report.wAccelY = 7910;		report.wAccelZ = 1360;
				report.wGyroX = 2700;		report.wGyroY = -5000;		report.wGyroZ = 140;
			}
		}
		else if (MotionXAdd) {
			report.wAccelX = 32767; report.wAccelY = 0;       report.wAccelZ = 0;
			report.wGyroX = 32767; report.wGyroY = 0;       report.wGyroZ = 0;
		}
		else if (MotionXSub) {
			report.wAccelX = -32767; report.wAccelY = 0;      report.wAccelZ = 0;
			report.wGyroX = -32767; report.wGyroY = 0;      report.wGyroZ = 0;
		}
		else if (MotionYAdd) {
			report.wAccelX = 0;       report.wAccelY = 32767; report.wAccelZ = 0;
			report.wGyroX = 0;       report.wGyroY = 32767; report.wGyroZ = 0;
		}
		else if (MotionYSub) {
			report.wAccelX = 0;       report.wAccelY = -32767; report.wAccelZ = 0;
			report.wGyroX = 0;       report.wGyroY = -32767; report.wGyroZ = 0;
		}
		else if (MotionZAdd) {
			report.wAccelX = 0;       report.wAccelY = 0;       report.wAccelZ = 32767;
			report.wGyroX = 0;       report.wGyroY = 0;       report.wGyroZ = 32767;
		}
		else if (MotionZSub) {
			report.wAccelX = 0;       report.wAccelY = 0;       report.wAccelZ = -32767;
			report.wGyroX = 0;       report.wGyroY = 0;       report.wGyroZ = -32767;
		}

		if (IsKeyPressed(KEY_ID_PS))
			report.bSpecial |= DS4_SPECIAL_BUTTON_PS;

		auto now = std::chrono::steady_clock::now();
		auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();
		report.wTimestamp = (USHORT)(ms & 0xFFFF);

		ret = vigem_target_ds4_update_ex(client, ds4, report);

		if (EmulationMode == XboxMode)
			Sleep(SleepTimeOutXbox);
		else
			Sleep(SleepTimeOutKB);

		if (Touch1.SkipPollLeft > 0) Touch1.SkipPollLeft--;
		if (Touch1.SkipPollTop > 0) Touch1.SkipPollTop--;
		if (Touch2.SkipPollLeft > 0) Touch2.SkipPollLeft--;
		if (Touch2.SkipPollTop > 0) Touch2.SkipPollTop--;
		if (SkipPollCount > 0) SkipPollCount--;
	}

	if (CursorHidden) SetSystemCursor(CurCursor, OCR_NORMAL);

	vigem_target_remove(client, ds4);
	vigem_target_free(ds4);
	vigem_free(client);
	FreeLibrary(hDll);
	hDll = nullptr;

	if (SocketActivated) {
		SocketActivated = false;
		if (pSocketThread) {
			pSocketThread->join();
			delete pSocketThread;
			pSocketThread = nullptr;
		}
		closesocket(socketS);
		WSACleanup();
	}
}
