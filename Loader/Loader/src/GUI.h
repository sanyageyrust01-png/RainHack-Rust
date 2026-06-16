#pragma once

#include <Windows.h>
#include <string>
#include <functional>

namespace gui {

struct Result {
    bool   loginRequested = false;
    bool   closed = false;
    std::string key;
};

void show(const std::string& motd);
void hide();

void log(const std::string& line);
void setStatus(const std::string& msg, int kind = 0);
void setBusy(bool busy);

void showSkipButton(bool show);
bool consumeSkipPress();

using LoginCallback = std::function<void()>;
void onLogin(LoginCallback cb);

int  runMessageLoop();
HWND window();

}
