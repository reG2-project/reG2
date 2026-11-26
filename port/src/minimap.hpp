// MINIMAP: a small map of the streets around the player in the HUD's bottom left corner, in frame E of the design study
// (port/docs/minimap), built from the level's blocks and lid tiles, so any level has one. On it the church, the garages,
// the cranes and the train stations, the gangs' phones while off a job, the job's targets above or below the player, and
// each gang's icon on the frame toward its information phone. The pause menu's MAP (M) shows the whole city.
#pragma once

#include "types.hpp"

namespace PortMinimap
{
// A level loads (Hud::Init): nothing of the last one's stays, the map is built again when next shown.
void Reset();

// With the HUD, under the briefs and the pause menu.
void Draw();
// With the game's arrows (MINIMAP ONLY off, or without the minimap), an arrow by the player toward the MAP's waypoint, as
// the game's own arrows point to their targets.
void DrawWaypointArrow();

// The pause menu's MAP: the whole city, opened fitted to the land. Moved while the arrows, WASD, the left stick or the
// d-pad are held or the mouse drags it, zoomed by the wheel and the right stick; ZoomCityMap by steps (in above 0),
// CentreCityMap back on the player; a click (not a drag) sets the waypoint, moves it, or clears it on it.
void OpenCityMap();
void CentreCityMap();
void ZoomCityMap(s32 steps);
// The waypoint at the MAP's middle, moved there, or cleared if it is there; both maps show it until the player reaches it.
void ToggleCityWaypoint();
// The map in that screen rectangle with icons at the HUD scale ui, and the input; then the pause menu writes the
// districts' names (CityMapLabel), then DrawCityMarkers draws the pins, phones, targets, waypoint and player over them,
// and the buttons zoom in, zoom out and back to the player (pButtons: their row's left, top, right, bottom, for touch).
// True when the right button asks to go back.
bool DrawCityBase(f32 left, f32 top, f32 right, f32 bottom, f32 ui, const f32* pButtons);
void DrawCityMarkers();
// The districts' names DrawCityBase placed, centred on (x, y): NULL past the last.
s32 CityMapLabels();
const wchar_t* CityMapLabel(s32 i, f32* pX, f32* pY);
// The MAP's key, what the level has: entry i's name in English, NULL past the last, and its icon centred at (x, y).
const wchar_t* LegendName(s32 i);
void DrawLegendIcon(s32 i, f32 x, f32 y, f32 ui);
// A dark see-through panel over the map, for the page's texts.
void DrawShade(f32 left, f32 top, f32 right, f32 bottom, f32 alpha);
} // namespace PortMinimap
