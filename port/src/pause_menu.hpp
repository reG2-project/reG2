// The in-game pause menu: Escape pauses the game and opens the settings menu (menu.hpp), with RESUME, LOAD
// GAME, QUIT GAME and STATISTICS, the lines of the original's pause screen, and RADIO in a car that has one. In a network
// game it opens over the game going on, with the players and LEAVE GAME.
#pragma once

#include "Player.hpp"

namespace PortPauseMenu
{
bool IsOpen();

// Loads what the menu shows and plays as a city starts (fstyle.sty takes a quarter of a second on the
// 3DS), and frees it as the city ends.
void Preload();
void Release();

// A key pressed in game (DIK code); true when the menu took it.
bool HandleKey(s32 action, Player* pPlayer);

// A network game's menu (port/net/session/client.cpp), over the game going on: the client's keys, with the character
// each typed (Y and N answer LEAVE GAME's question), and what was chosen in it, once (net::engine::MenuChoice).
bool HandleNetworkKey(s32 action, u16 typed);
s32 TakeNetworkChoice();

// Drawn with the HUD.
void Draw();

// While the menu draws the original's pause screen lines (Hud_PauseScreen::DrawPause) or the HUD's briefs: true, and where
// a position of the HUD layout goes on screen.
bool IsDrawingStats();
f32 PlaceUi(f32 pos, bool y);
} // namespace PortPauseMenu
