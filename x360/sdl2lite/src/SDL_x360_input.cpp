/*
*	Xbox 360 port of Sonic 3 A.I.R.
*	SDL2-lite: events, joysticks and game controllers on top of XInput.
*
*	Each XInput controller (user index 0..3) is an SDL joystick with a fixed layout, and the game
*	controller bindings describe that layout, which is what Oxygen's InputManager queries:
*	  buttons: 0 A, 1 B, 2 X, 3 Y, 4 LB, 5 RB, 6 BACK, 7 START, 8 left stick, 9 right stick
*	  axes:    0 left X, 1 left Y, 2 right X, 3 right Y (Y positive = down, as in SDL), 4 LT, 5 RT
*	  hat 0:   the D-pad
*	SDL_PollEvent reports controllers connecting / disconnecting as SDL_JOYDEVICEADDED / _REMOVED.
*	There is no keyboard, mouse, touch or text input.
*/

#include "SDL_x360_internal.h"

#include <string.h>


struct _SDL_Joystick
{
	int mUserIndex = 0;
};

struct _SDL_GameController
{
	SDL_Joystick* mJoystick = nullptr;
};


namespace
{
	const int MAX_CONTROLLERS = 4;
	const int NUM_BUTTONS = 10;
	const int NUM_AXES = 6;
	const int NUM_HATS = 1;

	const WORD BUTTON_MASKS[NUM_BUTTONS] =
	{
		XINPUT_GAMEPAD_A, XINPUT_GAMEPAD_B, XINPUT_GAMEPAD_X, XINPUT_GAMEPAD_Y,
		XINPUT_GAMEPAD_LEFT_SHOULDER, XINPUT_GAMEPAD_RIGHT_SHOULDER,
		XINPUT_GAMEPAD_BACK, XINPUT_GAMEPAD_START,
		XINPUT_GAMEPAD_LEFT_THUMB, XINPUT_GAMEPAD_RIGHT_THUMB
	};

	struct ControllerState
	{
		bool mConnected = false;
		bool mReportedConnected = false;	// What the engine was told through device events
		XINPUT_GAMEPAD mGamepad;
		Uint32 mRumbleEndTicks = 0;
		bool mRumbling = false;
	};

	ControllerState gControllers[MAX_CONTROLLERS];
	SDL_Joystick gJoysticks[MAX_CONTROLLERS];
	SDL_GameController gGameControllers[MAX_CONTROLLERS];
	DWORD gLastPollTicks = 0;
	bool gEverPolled = false;

	void pollControllers(bool force)
	{
		const DWORD now = GetTickCount();
		if (!force && gEverPolled && now - gLastPollTicks < 2)
			return;
		gLastPollTicks = now;
		gEverPolled = true;

		for (int i = 0; i < MAX_CONTROLLERS; ++i)
		{
			ControllerState& controller = gControllers[i];
			XINPUT_STATE state;
			ZeroMemory(&state, sizeof(state));
			controller.mConnected = (XInputGetState(i, &state) == ERROR_SUCCESS);
			if (controller.mConnected)
				controller.mGamepad = state.Gamepad;
			else
				ZeroMemory(&controller.mGamepad, sizeof(controller.mGamepad));

			// Stop timed rumble
			if (controller.mRumbling && (Sint32)(SDL_GetTicks() - controller.mRumbleEndTicks) >= 0)
			{
				XINPUT_VIBRATION vibration = { 0, 0 };
				XInputSetState(i, &vibration);
				controller.mRumbling = false;
			}
		}
	}

	const ControllerState* getController(SDL_Joystick* joystick)
	{
		if (nullptr == joystick || joystick->mUserIndex < 0 || joystick->mUserIndex >= MAX_CONTROLLERS)
			return nullptr;
		pollControllers(false);
		return &gControllers[joystick->mUserIndex];
	}

	// The i-th connected controller, as SDL counts joysticks
	int getUserIndexForDeviceIndex(int deviceIndex)
	{
		pollControllers(false);
		int count = 0;
		for (int i = 0; i < MAX_CONTROLLERS; ++i)
		{
			if (gControllers[i].mConnected)
			{
				if (count == deviceIndex)
					return i;
				++count;
			}
		}
		return -1;
	}

	Sint16 convertTrigger(BYTE value)
	{
		return (Sint16)(((int)value * 32767) / 255);
	}

	Sint16 invertAxis(SHORT value)
	{
		return (Sint16)((value == -32768) ? 32767 : -value);
	}
}


extern "C"
{

/* ----- Events ----- */

void SDL_PumpEvents(void)
{
	pollControllers(true);
}

int SDL_PollEvent(SDL_Event* event)
{
	pollControllers(false);

	// Report controller connection changes, one event per call
	for (int i = 0; i < MAX_CONTROLLERS; ++i)
	{
		ControllerState& controller = gControllers[i];
		if (controller.mConnected != controller.mReportedConnected)
		{
			controller.mReportedConnected = controller.mConnected;
			if (nullptr != event)
			{
				memset(event, 0, sizeof(SDL_Event));
				event->type = controller.mConnected ? SDL_JOYDEVICEADDED : SDL_JOYDEVICEREMOVED;
				event->jdevice.timestamp = SDL_GetTicks();
				event->jdevice.which = i;
			}
			return 1;
		}
	}
	return 0;
}


/* ----- Joysticks ----- */

int SDL_NumJoysticks(void)
{
	pollControllers(false);
	int count = 0;
	for (int i = 0; i < MAX_CONTROLLERS; ++i)
	{
		if (gControllers[i].mConnected)
			++count;
	}
	return count;
}

SDL_Joystick* SDL_JoystickOpen(int device_index)
{
	const int userIndex = getUserIndexForDeviceIndex(device_index);
	if (userIndex < 0)
	{
		SDL_SetError("Joystick %d is not connected", device_index);
		return nullptr;
	}
	gJoysticks[userIndex].mUserIndex = userIndex;
	return &gJoysticks[userIndex];
}

void SDL_JoystickClose(SDL_Joystick* joystick)  {}

SDL_JoystickID SDL_JoystickInstanceID(SDL_Joystick* joystick)
{
	return (nullptr != joystick) ? (SDL_JoystickID)joystick->mUserIndex : -1;
}

const char* SDL_JoystickName(SDL_Joystick* joystick)
{
	return "Xbox 360 Controller";
}

int SDL_JoystickNumButtons(SDL_Joystick* joystick)  { return (nullptr != joystick) ? NUM_BUTTONS : 0; }
int SDL_JoystickNumAxes(SDL_Joystick* joystick)     { return (nullptr != joystick) ? NUM_AXES : 0; }
int SDL_JoystickNumHats(SDL_Joystick* joystick)     { return (nullptr != joystick) ? NUM_HATS : 0; }

Uint8 SDL_JoystickGetButton(SDL_Joystick* joystick, int button)
{
	const ControllerState* controller = getController(joystick);
	if (nullptr == controller || button < 0 || button >= NUM_BUTTONS)
		return 0;
	return (controller->mGamepad.wButtons & BUTTON_MASKS[button]) ? 1 : 0;
}

Sint16 SDL_JoystickGetAxis(SDL_Joystick* joystick, int axis)
{
	const ControllerState* controller = getController(joystick);
	if (nullptr == controller)
		return 0;
	const XINPUT_GAMEPAD& gamepad = controller->mGamepad;
	switch (axis)
	{
		case 0:  return gamepad.sThumbLX;
		case 1:  return invertAxis(gamepad.sThumbLY);
		case 2:  return gamepad.sThumbRX;
		case 3:  return invertAxis(gamepad.sThumbRY);
		case 4:  return convertTrigger(gamepad.bLeftTrigger);
		case 5:  return convertTrigger(gamepad.bRightTrigger);
		default: return 0;
	}
}

Uint8 SDL_JoystickGetHat(SDL_Joystick* joystick, int hat)
{
	const ControllerState* controller = getController(joystick);
	if (nullptr == controller || hat != 0)
		return SDL_HAT_CENTERED;
	const WORD buttons = controller->mGamepad.wButtons;
	Uint8 value = SDL_HAT_CENTERED;
	if (buttons & XINPUT_GAMEPAD_DPAD_UP)     value |= SDL_HAT_UP;
	if (buttons & XINPUT_GAMEPAD_DPAD_RIGHT)  value |= SDL_HAT_RIGHT;
	if (buttons & XINPUT_GAMEPAD_DPAD_DOWN)   value |= SDL_HAT_DOWN;
	if (buttons & XINPUT_GAMEPAD_DPAD_LEFT)   value |= SDL_HAT_LEFT;
	return value;
}

SDL_bool SDL_JoystickHasRumble(SDL_Joystick* joystick)
{
	return (nullptr != joystick) ? SDL_TRUE : SDL_FALSE;
}

int SDL_JoystickRumble(SDL_Joystick* joystick, Uint16 low_frequency_rumble, Uint16 high_frequency_rumble, Uint32 duration_ms)
{
	if (nullptr == joystick)
		return SDL_InvalidParamError("joystick");
	const int userIndex = joystick->mUserIndex;
	XINPUT_VIBRATION vibration;
	vibration.wLeftMotorSpeed = low_frequency_rumble;
	vibration.wRightMotorSpeed = high_frequency_rumble;
	XInputSetState(userIndex, &vibration);

	ControllerState& controller = gControllers[userIndex];
	controller.mRumbling = (low_frequency_rumble != 0 || high_frequency_rumble != 0);
	controller.mRumbleEndTicks = SDL_GetTicks() + duration_ms;
	return 0;
}


/* ----- Game controllers (bindings describe the fixed joystick layout above) ----- */

SDL_GameController* SDL_GameControllerOpen(int joystick_index)
{
	SDL_Joystick* joystick = SDL_JoystickOpen(joystick_index);
	if (nullptr == joystick)
		return nullptr;
	SDL_GameController& controller = gGameControllers[joystick->mUserIndex];
	controller.mJoystick = joystick;
	return &controller;
}

const char* SDL_GameControllerName(SDL_GameController* gamecontroller)
{
	return "Xbox 360 Controller";
}

SDL_GameControllerButtonBind SDL_GameControllerGetBindForButton(SDL_GameController* gamecontroller, SDL_GameControllerButton button)
{
	SDL_GameControllerButtonBind bind;
	memset(&bind, 0, sizeof(bind));
	bind.bindType = SDL_CONTROLLER_BINDTYPE_NONE;

	int joystickButton = -1;
	int hatMask = 0;
	switch (button)
	{
		case SDL_CONTROLLER_BUTTON_A:				joystickButton = 0;  break;
		case SDL_CONTROLLER_BUTTON_B:				joystickButton = 1;  break;
		case SDL_CONTROLLER_BUTTON_X:				joystickButton = 2;  break;
		case SDL_CONTROLLER_BUTTON_Y:				joystickButton = 3;  break;
		case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:	joystickButton = 4;  break;
		case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:	joystickButton = 5;  break;
		case SDL_CONTROLLER_BUTTON_BACK:			joystickButton = 6;  break;
		case SDL_CONTROLLER_BUTTON_START:			joystickButton = 7;  break;
		case SDL_CONTROLLER_BUTTON_LEFTSTICK:		joystickButton = 8;  break;
		case SDL_CONTROLLER_BUTTON_RIGHTSTICK:		joystickButton = 9;  break;
		case SDL_CONTROLLER_BUTTON_DPAD_UP:			hatMask = SDL_HAT_UP;     break;
		case SDL_CONTROLLER_BUTTON_DPAD_DOWN:		hatMask = SDL_HAT_DOWN;   break;
		case SDL_CONTROLLER_BUTTON_DPAD_LEFT:		hatMask = SDL_HAT_LEFT;   break;
		case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:		hatMask = SDL_HAT_RIGHT;  break;
		default:	break;	// GUIDE is not readable through XInputGetState
	}

	if (joystickButton >= 0)
	{
		bind.bindType = SDL_CONTROLLER_BINDTYPE_BUTTON;
		bind.value.button = joystickButton;
	}
	else if (hatMask != 0)
	{
		bind.bindType = SDL_CONTROLLER_BINDTYPE_HAT;
		bind.value.hat.hat = 0;
		bind.value.hat.hat_mask = hatMask;
	}
	return bind;
}

SDL_GameControllerButtonBind SDL_GameControllerGetBindForAxis(SDL_GameController* gamecontroller, SDL_GameControllerAxis axis)
{
	SDL_GameControllerButtonBind bind;
	memset(&bind, 0, sizeof(bind));
	bind.bindType = SDL_CONTROLLER_BINDTYPE_NONE;
	if (axis >= SDL_CONTROLLER_AXIS_LEFTX && axis <= SDL_CONTROLLER_AXIS_TRIGGERRIGHT)
	{
		// Same order as the joystick axes above
		bind.bindType = SDL_CONTROLLER_BINDTYPE_AXIS;
		bind.value.axis = (int)axis;
	}
	return bind;
}

int SDL_GameControllerSetLED(SDL_GameController* gamecontroller, Uint8 red, Uint8 green, Uint8 blue)
{
	return SDL_Unsupported();
}


/* ----- Keyboard / text input: none on the console ----- */

SDL_Keymod SDL_GetModState(void)
{
	return KMOD_NONE;
}

void SDL_StartTextInput(void)  {}
void SDL_StopTextInput(void)  {}

SDL_bool SDL_IsTextInputActive(void)
{
	return SDL_FALSE;
}


/* ----- Touch: none ----- */

int SDL_GetNumTouchDevices(void)
{
	return 0;
}

SDL_TouchID SDL_GetTouchDevice(int index)
{
	SDL_SetError("No touch devices");
	return 0;
}

int SDL_GetNumTouchFingers(SDL_TouchID touchID)
{
	return 0;
}

SDL_Finger* SDL_GetTouchFinger(SDL_TouchID touchID, int index)
{
	return nullptr;
}

}
