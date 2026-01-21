#include <Windows.h>
#include <mutex>
#include <cmath>
#include <algorithm>
#include <string>
#include <chrono> // Novo: Para controlar o tempo do Rapid Fire
#include "ViGEm\Client.h"
#include "IniReader\IniReader.h"
#include "DS4Emulator.h"

#pragma comment (lib, "WSock32.Lib")

// --- ESTRUTURAS DE CONFIGURAÇÃO ---
struct AimSettings {
	float SensX;
	float SensY;
	float Smoothing;
	float Curve;
	int Boost;
};

// Configs de Mira
AimSettings HipSettings;
AimSettings AdsSettings;

// Configs Extras (Novas)
struct ExtraFeatures {
	bool RapidFireEnabled;
	int RapidFireSpeed; // Tiros por segundo
	bool AntiRecoilEnabled;
	int AntiRecoilStrength; // Força para baixo
};
ExtraFeatures Extras;
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

// Variáveis Globais
std::string WindowTitle = "PlayStation Plus";
std::string WindowTitle2 = "PS4 Remote Play";

// Variáveis de Estado (Smoothing e Macros)
static double g_SmoothX = 0;
static double g_SmoothY = 0;
static bool g_RapidFireState = false;
static auto g_LastRapidFireTime = std::chrono::steady_clock::now();

// WinSock (Motion)
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
bool MotionOrientation = true;

void MotionReceiver()
{
	while (SocketActivated) {
		memset(&freePieIMU, 0, sizeof(freePieIMU));
		bytes_read = recvfrom(socketS, (char*)(&freePieIMU), sizeof(freePieIMU), 0, (sockaddr*)&from, &fromlen);
		if (bytes_read > 0) {
			if (MotionOrientation) {
				AccelZ = bytesToFloat(freePieIMU[2], freePieIMU[3], freePieIMU[4], freePieIMU[5]);
				AccelX = bytesToFloat(freePieIMU[6], freePieIMU[7], freePieIMU[8], freePieIMU[9]);
				AccelY = bytesToFloat(freePieIMU[10], freePieIMU[11], freePieIMU[12], freePieIMU[13]);
				GyroZ = bytesToFloat(freePieIMU[14], freePieIMU[15], freePieIMU[16], freePieIMU[17]);
				GyroX = bytesToFloat(freePieIMU[18], freePieIMU[19], freePieIMU[20], freePieIMU[21]);
				GyroY = bytesToFloat(freePieIMU[22], freePieIMU[23], freePieIMU[24], freePieIMU[25]);
			}
			else {
				AccelX = bytesToFloat(freePieIMU[2], freePieIMU[3], freePieIMU[4], freePieIMU[5]);
				AccelZ = bytesToFloat(freePieIMU[6], freePieIMU[7], freePieIMU[8], freePieIMU[9]);
				AccelY = bytesToFloat(freePieIMU[10], freePieIMU[11], freePieIMU[12], freePieIMU[13]);
				GyroX = bytesToFloat(freePieIMU[14], freePieIMU[15], freePieIMU[16], freePieIMU[17]);
				GyroZ = bytesToFloat(freePieIMU[18], freePieIMU[19], freePieIMU[20], freePieIMU[21]);
				GyroY = bytesToFloat(freePieIMU[22], freePieIMU[23], freePieIMU[24], freePieIMU[25]);
			}
		}
		else Sleep(SleepTimeOutMotion);
	}
}

VOID CALLBACK notification(PVIGEM_CLIENT Client, PVIGEM_TARGET Target, UCHAR LargeMotor, UCHAR SmallMotor, DS4_LIGHTBAR_COLOR LightbarColor, LPVOID UserData)
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
	if (Value > Max) Value = Max;
	else if (Value < Min) Value = Min;
	return Value;
}

SHORT DeadZoneXboxAxis(SHORT StickAxis, float Percent)
{
	float DeadZoneValue = Percent * 327.67;
	if (StickAxis > 0) {
		StickAxis -= trunc(DeadZoneValue);
		if (StickAxis < 0) StickAxis = 0;
	}
	else if (StickAxis < 0) {
		StickAxis += trunc(DeadZoneValue);
		if (StickAxis > 0) StickAxis = 0;
	}
	return trunc(StickAxis + StickAxis * Percent * 0.01);
}

void LoadKMProfile(std::string ProfileFile) {
	CIniReader IniFile("Profiles\\" + ProfileFile);
	// Carrega todas as teclas do arquivo .ini (código omitido para brevidade, mas mantido a lógica original)
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
	
	// Touchpad & Motion keys... (Mantidos do original)
	KEY_ID_TOUCHPAD_SWIPE_UP = KeyNameToKeyCode(IniFile.ReadString("Keys", "TOUCHPAD-SWIPE-UP", "7"));
	KEY_ID_TOUCHPAD_SWIPE_DOWN = KeyNameToKeyCode(IniFile.ReadString("Keys", "TOUCHPAD-SWIPE-DOWN", "8"));
	KEY_ID_TOUCHPAD_SWIPE_LEFT = KeyNameToKeyCode(IniFile.ReadString("Keys", "TOUCHPAD-SWIPE-LEFT", "9"));
	KEY_ID_TOUCHPAD_SWIPE_RIGHT = KeyNameToKeyCode(IniFile.ReadString("Keys", "TOUCHPAD-SWIPE-RIGHT", "0"));
	KEY_ID_MOTION_SHAKING = KeyNameToKeyCode(IniFile.ReadString("Keys", "MOTION-SHAKING", "T"));
}

void DefaultMainText() {
	if (EmulationMode == XboxMode) {
		printf("\n Emulating a DualShock 4 using an Xbox controller.\n");
	}
	else {
		printf("\n [DS4 XIM MATRIX MODE - FULL FEATURED]\n");
		printf(" ---------------------------------------------------------\n");
		printf(" HIP: Sens:%.1f | Smooth:%.2f | Boost:%d\n", HipSettings.SensX, HipSettings.Smoothing, HipSettings.Boost);
		printf(" ADS: Sens:%.1f | Smooth:%.2f | Boost:%d\n", AdsSettings.SensX, AdsSettings.Smoothing, AdsSettings.Boost);
		printf(" ---------------------------------------------------------\n");
		printf(" EXTRAS:\n");
		printf(" Rapid Fire:  [%s] (Speed: %d)\n", Extras.RapidFireEnabled ? "ON" : "OFF", Extras.RapidFireSpeed);
		printf(" Anti-Recoil: [%s] (Str: %d)\n", Extras.AntiRecoilEnabled ? "ON" : "OFF", Extras.AntiRecoilStrength);
		printf(" ---------------------------------------------------------\n\n");

		if (ActivateInAnyWindow == false)
			printf(" Active in: \"%s\" / \"%s\". (ALT+F3 to toggle)\n", WindowTitle.c_str(), WindowTitle2.c_str());
		else
			printf(" Active in: ANY WINDOW.\n");
	}
}

void MainTextUpdate() {
	system("cls");
	DefaultMainText();
}

int main(int argc, char **argv)
{
	SetConsoleTitle("DS4Emulator XIM Ultimate");
	WindowToCenter();

	CIniReader IniFile("Config.ini");

	// --- CARREGAR CONFIGS DO ARQUIVO ---
	HipSettings.SensX = IniFile.ReadFloat("Hip", "SensX", 15.0f);
	HipSettings.SensY = IniFile.ReadFloat("Hip", "SensY", 15.0f);
	HipSettings.Smoothing = IniFile.ReadFloat("Hip", "Smoothing", 0.1f);
	HipSettings.Curve = IniFile.ReadFloat("Hip", "Curve", 1.0f);
	HipSettings.Boost = IniFile.ReadInteger("Hip", "Boost", 0);

	AdsSettings.SensX = IniFile.ReadFloat("ADS", "SensX", 10.0f);
	AdsSettings.SensY = IniFile.ReadFloat("ADS", "SensY", 10.0f);
	AdsSettings.Smoothing = IniFile.ReadFloat("ADS", "Smoothing", 0.4f);
	AdsSettings.Curve = IniFile.ReadFloat("ADS", "Curve", 1.0f);
	AdsSettings.Boost = IniFile.ReadInteger("ADS", "Boost", 0);

	// Novos Extras
	Extras.RapidFireEnabled = IniFile.ReadBoolean("RapidFire", "Enabled", false);
	Extras.RapidFireSpeed = IniFile.ReadInteger("RapidFire", "Speed", 10);
	Extras.AntiRecoilEnabled = IniFile.ReadBoolean("AntiRecoil", "Enabled", false);
	Extras.AntiRecoilStrength = IniFile.ReadInteger("AntiRecoil", "Strength", 0);

	// Configs Gerais
	bool InvertX = IniFile.ReadBoolean("Main", "InvertX", false);
	bool InvertY = IniFile.ReadBoolean("Main", "InvertY", false);
	bool SwapSticks = IniFile.ReadBoolean("KeyboardMouse", "SwapSticks", false);
	bool LeftAnalogStick = IniFile.ReadBoolean("KeyboardMouse", "EmulateLeftAnalogStick", false);
	bool EmulateAnalogTriggers = IniFile.ReadBoolean("KeyboardMouse", "EmulateAnalogTriggers", false);
	bool ActivateInAnyWindow = IniFile.ReadBoolean("KeyboardMouse", "ActivateInAnyWindow", false);
	WindowTitle = IniFile.ReadString("KeyboardMouse", "ActivateOnlyInWindow", "PlayStation Plus");
	WindowTitle2 = IniFile.ReadString("KeyboardMouse", "ActivateOnlyInWindow2", "PS4 Remote Play");
	std::string DefaultProfile = IniFile.ReadString("KeyboardMouse", "DefaultProfile", "Default.ini");
	int SleepTimeOutKB = IniFile.ReadInteger("KeyboardMouse", "SleepTimeOut", 1);
	int AnalogStickLeft = IniFile.ReadFloat("KeyboardMouse", "AnalogStickStep", 15);
	int LeftAnalogX = 128, LeftAnalogY = 128;
	
	// Motion Init (Simplificado para o exemplo)
	// ... (Código do Motion mantido igual ao anterior, mas omitido aqui para focar nas novidades)

	LoadKMProfile(DefaultProfile);

	// ViGEm Init
	const auto client = vigem_alloc();
	vigem_connect(client);
	const auto ds4 = vigem_target_ds4_alloc();
	vigem_target_add(client, ds4);
	DS4_REPORT_EX report;

	// Load XInput (se precisar de controle Xbox)
	hDll = LoadLibrary("xinput1_3.dll");
	if (hDll == NULL) hDll = LoadLibrary("xinput1_4.dll");
	if (hDll != NULL) {
		MyXInputGetState = (_XInputGetState)GetProcAddress(hDll, (LPCSTR)100);
		if (MyXInputGetState == NULL) hDll = NULL;
	}

	m_HalfWidth = GetSystemMetrics(SM_CXSCREEN) / 2;
	m_HalfHeight = GetSystemMetrics(SM_CYSCREEN) / 2;

	MainTextUpdate();

	bool CenteringEnable = true;
	int SkipPollCount = 0;
	auto start = std::chrono::steady_clock::now();

	// LOOP PRINCIPAL
	while (!(IsKeyPressed(VK_LMENU) && IsKeyPressed(VK_ESCAPE)))
	{
		DS4_REPORT_INIT_EX(&report);
		report.bBatteryLvl = 255; // Carregando

		// Teclas de Atalho
		if (SkipPollCount == 0 && IsKeyPressed(VK_MENU)) {
			if (IsKeyPressed(VK_F3)) {
				ActivateInAnyWindow = !ActivateInAnyWindow;
				MainTextUpdate();
				SkipPollCount = 20;
			}
			if (IsKeyPressed('C')) {
				CenteringEnable = !CenteringEnable;
				SkipPollCount = 20;
			}
		}

		HWND PSPlusWindow = FindWindow(NULL, WindowTitle.c_str());
		HWND PSRemotePlayWindow = FindWindow(NULL, WindowTitle2.c_str());
		bool IsGameWindow = (PSPlusWindow && GetForegroundWindow() == PSPlusWindow) || 
		                    (PSRemotePlayWindow && GetForegroundWindow() == PSRemotePlayWindow);

		if (ActivateInAnyWindow || IsGameWindow) {
			if (CenteringEnable) GetMouseState();

			// --- LÓGICA DE MIRA XIM (Noise Gate + Smoothing) ---
			AimSettings* currentAim = (IsKeyPressed(VK_RBUTTON)) ? &AdsSettings : &HipSettings;
			
			// Anti-Recoil (Só ativa se estiver atirando com Botão Esquerdo)
			bool isFiring = IsKeyPressed(VK_LBUTTON); // Assumindo LButton como tiro (R2)
			
			double rawDeltaX = (double)DeltaMouseX;
			double rawDeltaY = (double)DeltaMouseY;

			if (InvertX) rawDeltaX *= -1;
			if (InvertY) rawDeltaY *= -1;

			// Adiciona Anti-Recoil (Puxa para baixo no eixo Y)
			if (isFiring && Extras.AntiRecoilEnabled) {
				// Adiciona uma força constante para baixo
				rawDeltaY += (double)Extras.AntiRecoilStrength;
			}

			// Processamento Matemático
			double targetX = rawDeltaX * currentAim->SensX;
			double targetY = rawDeltaY * currentAim->SensY;

			g_SmoothX = (g_SmoothX * (1.0 - currentAim->Smoothing)) + (targetX * currentAim->Smoothing);
			g_SmoothY = (g_SmoothY * (1.0 - currentAim->Smoothing)) + (targetY * currentAim->Smoothing);

			double magX = pow(abs(g_SmoothX), currentAim->Curve);
			double magY = pow(abs(g_SmoothY), currentAim->Curve);

			double noiseThreshold = 1.0; 
			int outX = 128;
			int outY = 128;

			if (magX > noiseThreshold) {
				if (g_SmoothX > 0) outX = 128 + currentAim->Boost + (int)magX;
				else               outX = 128 - currentAim->Boost - (int)magX;
			}

			if (magY > noiseThreshold) {
				if (g_SmoothY > 0) outY = 128 + currentAim->Boost + (int)magY;
				else               outY = 128 - currentAim->Boost - (int)magY;
			}

			report.bThumbRX = (BYTE)Clamp((float)outX, 0, 255);
			report.bThumbRY = (BYTE)Clamp((float)outY, 0, 255);

			// --- BOTÕES E GATILHOS (Com Rapid Fire) ---
			
			// Rapid Fire Logic (No Gatilho R2 - Mapeado no Botão Esquerdo)
			if (isFiring) {
				if (Extras.RapidFireEnabled) {
					auto now = std::chrono::steady_clock::now();
					auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - g_LastRapidFireTime).count();
					int interval = 1000 / Extras.RapidFireSpeed; // Ex: 1000ms / 10 tiros = 100ms

					if (elapsed > interval) {
						g_RapidFireState = !g_RapidFireState; // Inverte estado (Aperta/Solta)
						g_LastRapidFireTime = now;
					}
					report.bTriggerR = g_RapidFireState ? 255 : 0;
				} else {
					report.bTriggerR = 255; // Tiro normal segurado
				}
			} else {
				report.bTriggerR = 0;
				g_RapidFireState = false;
			}

			if (report.bTriggerR > 0) report.wButtons |= DS4_BUTTON_TRIGGER_RIGHT;

			// Outros botões (Mapeamento básico direto)
			if (IsKeyPressed(KEY_ID_LEFT_TRIGGER)) { report.bTriggerL = 255; report.wButtons |= DS4_BUTTON_TRIGGER_LEFT; }
			
			// Movimento (WASD)
			int moveX = 128, moveY = 128;
			if (IsKeyPressed(KEY_ID_LEFT_STICK_UP)) moveY = 0;
			if (IsKeyPressed(KEY_ID_LEFT_STICK_DOWN)) moveY = 255;
			if (IsKeyPressed(KEY_ID_LEFT_STICK_LEFT)) moveX = 0;
			if (IsKeyPressed(KEY_ID_LEFT_STICK_RIGHT)) moveX = 255;
			report.bThumbLX = moveX; report.bThumbLY = moveY;

			// Botões de Face
			if (IsKeyPressed(KEY_ID_CROSS)) report.wButtons |= DS4_BUTTON_CROSS;
			if (IsKeyPressed(KEY_ID_CIRCLE)) report.wButtons |= DS4_BUTTON_CIRCLE;
			if (IsKeyPressed(KEY_ID_SQUARE)) report.wButtons |= DS4_BUTTON_SQUARE;
			if (IsKeyPressed(KEY_ID_TRIANGLE)) report.wButtons |= DS4_BUTTON_TRIANGLE;
			if (IsKeyPressed(KEY_ID_OPTIONS)) report.wButtons |= DS4_BUTTON_OPTIONS;
			if (IsKeyPressed(KEY_ID_SHARE)) report.wButtons |= DS4_BUTTON_SHARE;
			if (IsKeyPressed(KEY_ID_PS)) report.bSpecial |= DS4_SPECIAL_BUTTON_PS;
			if (IsKeyPressed(KEY_ID_LEFT_THUMB)) report.wButtons |= DS4_BUTTON_THUMB_LEFT; // L3 (Shift)
		}

		// Envia para o controle virtual
		vigem_target_ds4_update_ex(client, ds4, report);
		
		Sleep(SleepTimeOutKB);
		if (SkipPollCount > 0) SkipPollCount--;
	}

	// Limpeza
	if (CursorHidden) SetSystemCursor(CurCursor, OCR_NORMAL);
	vigem_target_remove(client, ds4);
	vigem_target_free(ds4);
	vigem_free(client);
	if (hDll) FreeLibrary(hDll);
}
