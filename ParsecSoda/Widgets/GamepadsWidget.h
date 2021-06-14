#pragma once

#include "../imgui/imgui.h"
#include "../Hosting.h"
#include "../globals/AppIcons.h"
#include "../globals/AppStyle.h"
#include "ToggleIconButtonWidget.h"
#include "TooltipWidget.h"

class GamepadsWidget
{
public:
	GamepadsWidget(Hosting& hosting);
	bool render();

private:
	// Dependency injection
	Hosting& _hosting;
	
	// Attributes
	string _logBuffer;
	vector<Gamepad>& _gamepads;
};