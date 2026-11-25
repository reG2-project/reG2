// The settings both option menus show (frontend pages and in-game pause menu): their names, the
// text of their values, and changing them (stored in the registry, applied at once where possible).
#pragma once

#include "types.hpp"

namespace PortSettings
{
enum Id
{
    kSfxVolume,
    kMusicVolume,
    k3dSound,
    kTextSpeed,
    kLanguage,
    kFullScreen,
    kBrightness,
    kLighting,
    kExplodingScores,
    kFrameLimit,
    kTrueColour,
    kBloom,
    kPerPixelLighting,
    kHeadlights,
    kVsync,
    kDisplayMode,
    kZoom,
    kHudScale,
    kSafeArea,
    kLightEffects,
    kTextureFilter,
    kAntiAliasing,
    kShadows,
    kPreset,
    kDayLength,
    kStartTime,
    kClock,
    kControls,
    kAimCamera,
    kStrafeAnimation,
    kVibration,
    kBlood,
    kUpdateCheck,
    kShowFps,
    kAutosave,
    kRetry,
    kKfRespawn,
    kInfiniteLives,
    kAmbientOcclusion,
    kFireEffects,
    kCrosshair,
    kCount,
    // Group titles in the pages' lists: the settings after one, up to the next, are under it.
    kSavesGroup,
    kDifficultyGroup,
    kTimeOfDayGroup,
    kLightingGroup,
    kImageGroup,
    kScreenGroup,
    kViewGroup,
    kGroupsEnd,
};

// The settings of each options page, in their order, with their group titles.
extern const Id kGameSettings[];
extern const s32 kNumGameSettings;
extern const Id kGraphicsSettings[];
extern const s32 kNumGraphicsSettings;
extern const Id kDisplaySettings[];
extern const s32 kNumDisplaySettings;
extern const Id kSoundSettings[];
extern const s32 kNumSoundSettings;

// The port's menu texts in the language of the loaded text table, from their English text.
const wchar_t* Translate(const wchar_t* pEnglish);

// data\<code>.gxt is there: the 2004 freeware has only English and Spanish.
bool LanguageInstalled(char code);

bool IsGroup(Id id);
// In English, as the key to Translate; a group's title.
const wchar_t* Name(Id id);
void Value(Id id, wchar_t* pOut);
// The numeric settings, shown as sliders: false for the others.
bool Range(Id id, s32* pMin, s32* pMax, s32* pValue);
// The setting another one only works with, kCount if none.
Id Requirement(Id id);
// False while the setting's requirement is off: the menu shows it greyed and doesn't change it, and its
// stored value is kept.
bool Enabled(Id id);
// Changing the language reloads the text table and lays the frontend's or the HUD's texts out again.
void Change(Id id, s32 delta);

// Car headlight beams, and the other added lights (lights.hpp), with the dusk and pixel lighting.
bool Headlights();
bool LightEffects();
// AMBIENT OCCLUSION (ambient_occlusion.hpp).
bool AmbientOcclusion();
// FIRE EFFECTS: HEAT HAZE (heat_haze.hpp), smoke and embers (fire_smoke.hpp).
bool FireEffects();

// LIGHTING's DAY-NIGHT CYCLE (day_night.hpp), its CYCLE LENGTH in minutes and its START TIME, the hour a
// level starts at.
bool DayNightCycle();
s32 DayLength();
s32 StartTime();
// CLOCK: the cycle's hour on the HUD.
bool Clock();

// HUD SCALE: 1 for the original's size.
f32 HudScale();

// CONTROLS: TWIN STICK moves and aims apart on foot (twin_stick.hpp).
bool TwinStick();
// AIM CAMERA: with TWIN STICK, the view leads along the aim.
bool AimCamera();
// STRAFE ANIMATION: with TWIN STICK, the legs follow the walk rather than the aim.
bool StrafeAnimation();
bool Crosshair();
// VIBRATION: the gamepad shakes as the player is hit, in explosions and in crashes (rumble.hpp).
bool Vibration();

// CHECK FOR UPDATES: the start menu shows a newer release (options_menu.hpp).
bool UpdateCheck();

// SHOW FPS: the frames drawn per second in a corner of the HUD (frame_loop.hpp).
bool ShowFps();

// AUTOSAVE: a save after each job and on quitting, beside the church's (autosave.hpp).
bool Autosave();

// RETRY JOBS: a checkpoint before each job and kill frenzy, to go back to after a failure (retry.hpp).
bool Retry();

// FRENZY RESPAWN: a failed kill frenzy comes back on the map (retry.hpp).
bool KfRespawn();

// INFINITE LIVES: dying costs no life, so no GAME OVER, through the game's do_infinite_lives debug flag.
bool InfiniteLives();
// The flag from the setting or the registry's debug setting, from the latter alone for an attract demo.
void ApplyInfiniteLives(bool demo);

// What the setting does, or its value where that is not plain, on two lines at most under the page; NULL if
// nothing. A setting with help has it for every value.
const wchar_t* Help(Id id);

// SAFE AREA: the part of the screen's width or height kept clear at each edge, 0 to 0.1.
f32 SafeArea();

// FRAME LIMIT: frames drawn per second, 30 the original's pacing, 0 no limit.
s32 MaxFps();

// The renderer's options from the registry, at start-up.
void ApplyRenderOptions();

// The first start's styles (welcome.hpp), both drawing a frame per screen refresh. CLASSIC: its preset, original
// lighting and controls, church saves only; ENHANCED: its preset, day-night cycle, twin stick, AUTOSAVE, RETRY JOBS,
// FRENZY RESPAWN. INFINITE LIVES off in both.
enum Style
{
    kClassicStyle,
    kEnhancedStyle,
};
void ApplyStyle(Style style);
} // namespace PortSettings
