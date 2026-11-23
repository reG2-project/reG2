#include "settings.hpp"
#include <stdlib.h>
#include "frame_loop.hpp"
#include "Camera.hpp"
#include "Frontend.hpp"
#include "Function.hpp"
#include "Game.hpp"
#include "Globals.hpp"
#include "Hud.hpp"
#include "TextTable.hpp"
#include "debug.hpp"
#include "map_0x370.hpp"
#include "registry.hpp"
#include "root_sound.hpp"
#include "saves.hpp"
#include "winmain.hpp"
#include <math.h>
#include <stdio.h>
#include <string.h>

// No std::wstring: with -fshort-wchar its traits would call the C library's 4-byte wide functions.

void __stdcall SetStartMode(s32 startMode);
void __stdcall SetVideoModeFromSettings();
void __stdcall Init_FrameRateLightingAndExplodingScores();
void OnVideoModeChanged();
char_type IsWindowedModeAvailable();
bool port_GameRunning();
#if defined(GTA2_PORT_SDL)
void port_SetRenderOptions(bool trueColour, int bloomTenths, bool perPixelLighting, int textureFilter, int msaa, bool softShadows);
int port_MaxMsaaSamples();
unsigned port_RegistryGeneration();
void port_ApplyVsync();
int port_FullscreenModeCount();
int port_FullscreenModeIndex();
void port_FullscreenModeSize(int index, int* pW, int* pH, int* pCentiHz);
void port_SetFullscreenModeIndex(int index);
#endif

namespace PortSettings
{
// No DAY-NIGHT CYCLE without SDL: its settings would stay greyed.
const Id kGameSettings[] = {kLanguage, kTextSpeed, kExplodingScores, kBlood,
#if defined(GTA2_UPDATE_CHECK)
                            kUpdateCheck,
#endif
                            kSavesGroup, kAutosave,
                            kDifficultyGroup, kRetry, kKfRespawn, kInfiniteLives,
                            kTimeOfDayGroup, kLighting,
#if defined(GTA2_PORT_SDL)
                            kDayLength, kStartTime, kClock,
#endif
};
const s32 kNumGameSettings = sizeof(kGameSettings) / sizeof(kGameSettings[0]);
#if defined(GTA2_PORT_SDL)
const Id kGraphicsSettings[] = {kPreset,
                                kLightingGroup, kPerPixelLighting, kShadows, kAmbientOcclusion, kFireEffects, kHeadlights,
                                kLightEffects, kBloom, kImageGroup, kTrueColour, kTextureFilter, kAntiAliasing};
// On the web the page owns full screen, there are no display modes, the frames follow the browser's and
// there is no OpenAL for 3D sound.
#if defined(__EMSCRIPTEN__)
const Id kDisplaySettings[] = {kScreenGroup, kFrameLimit, kShowFps, kViewGroup, kBrightness, kZoom, kHudScale, kSafeArea};
#else
const Id kDisplaySettings[] = {kScreenGroup, kFullScreen, kDisplayMode, kVsync, kFrameLimit, kShowFps,
                               kViewGroup, kBrightness, kZoom, kHudScale, kSafeArea};
#endif
#else
const Id kGraphicsSettings[] = {kHeadlights, kLightEffects};
const Id kDisplaySettings[] = {kScreenGroup, kFullScreen, kFrameLimit, kShowFps, kViewGroup, kBrightness, kZoom, kHudScale, kSafeArea};
#endif
const s32 kNumGraphicsSettings = sizeof(kGraphicsSettings) / sizeof(kGraphicsSettings[0]);
const s32 kNumDisplaySettings = sizeof(kDisplaySettings) / sizeof(kDisplaySettings[0]);
#if defined(__EMSCRIPTEN__)
const Id kSoundSettings[] = {kSfxVolume, kMusicVolume};
#else
const Id kSoundSettings[] = {kSfxVolume, kMusicVolume, k3dSound};
#endif
const s32 kNumSoundSettings = sizeof(kSoundSettings) / sizeof(kSoundSettings[0]);

namespace
{
const wchar_t* const kNames[kCount] = {L"SOUND EFFECTS", L"MUSIC", L"3D SOUND", L"TEXT SPEED", L"LANGUAGE",
                                       L"FULL SCREEN", L"BRIGHTNESS", L"LIGHTING", L"EXPLODING SCORES", L"FRAME LIMIT",
                                       L"TRUE COLOUR", L"BLOOM", L"PIXEL LIGHTING", L"CAR HEADLIGHTS",
                                       L"VSYNC", L"DISPLAY MODE", L"CAMERA ZOOM", L"HUD SCALE", L"SAFE AREA", L"LIGHT EFFECTS", L"TEXTURE FILTERING", L"ANTI-ALIASING", L"SHADOWS", L"PRESET", L"CYCLE LENGTH", L"START TIME", L"CLOCK", L"CONTROLS", L"AIM CAMERA", L"STRAFE ANIMATION", L"VIBRATION", L"BLOOD", L"CHECK FOR UPDATES", L"SHOW FPS", L"AUTOSAVE", L"RETRY JOBS", L"FRENZY RESPAWN", L"INFINITE LIVES", L"AMBIENT OCCLUSION", L"FIRE EFFECTS", L"CROSSHAIR"};
const wchar_t* const kGroupNames[kGroupsEnd - kCount - 1] = {L"SAVES", L"DIFFICULTY", L"TIME OF DAY", L"LIGHTING", L"IMAGE", L"SCREEN", L"VIEW"};

struct Language
{
    char mCode;
    const wchar_t* mName;
};
const Language kLanguages[] = {{'e', L"ENGLISH"}, {'f', L"FRANCAIS"}, {'g', L"DEUTSCH"}, {'i', L"ITALIANO"}, {'s', L"ESPANOL"}};
const s32 kNumLanguages = sizeof(kLanguages) / sizeof(kLanguages[0]);

const wchar_t* const kTextSpeeds[] = {L"VERY FAST", L"FAST", L"NORMAL", L"SLOW", L"VERY SLOW"};
const s32 kMaxFpsValues[] = {30, 60, 120, 144, 240, 0};
const s32 kNumMaxFpsValues = sizeof(kMaxFpsValues) / sizeof(kMaxFpsValues[0]);
const wchar_t* const kTextureFilters[] = {L"BILINEAR", L"ANISOTROPIC", L"PIXELATED", L"SHARP BILINEAR"};
// The gl_texture_filter value of each; a stored 1 (trilinear) shows as anisotropic.
const s32 kTextureFilterValues[] = {0, 2, 3, 4};
const s32 kNumTextureFilters = sizeof(kTextureFilters) / sizeof(kTextureFilters[0]);
const s32 kMsaaValues[] = {0, 2, 4, 8};
const s32 kNumMsaaValues = sizeof(kMsaaValues) / sizeof(kMsaaValues[0]);

// A text of the port's menus: a key of the game's text table, or the text in French, German, Italian
// and Spanish (Latin-1 letters, uppercase as the game writes them), many from GTA2 Manager's data\bob_?.gxt.
struct Translation
{
    const wchar_t* mEnglish;
    const char* mGameKey;
    const wchar_t* mText[4];
};
const Translation kTranslations[] = {
    {L"OPTIONS", "options", {}},
    {L"BACK", "back", {}},
    {L"CANCEL", NULL, {L"ANNULER", L"ABBRECHEN", L"ANNULLA", L"CANCELAR"}},
    {L"ESC", "esc_i", {}},
    {L"UP", "up_i", {}},
    {L"DOWN", "down_i", {}},
    {L"LEFT", "left_i", {}},
    {L"RIGHT", "right_i", {}},
    {L"FAST", "psxfast", {}},
    {L"NORMAL", "psxnorm", {}},
    {L"SLOW", "psxslow", {}},
    {L"RESUME", "psxret", {}},
    {L"QUIT GAME", "psxquit", {}},
    {L"KEY %d", NULL, {L"TOUCHE %d", L"TASTE %d", L"TASTO %d", L"TECLA %d"}},
    {L"ENTER", NULL, {L"ENTREE", L"EINGABE", L"INVIO", L"INTRO"}},
    {L"SELECT", NULL, {L"CHOISIR", L"WÄHLEN", L"SCEGLI", L"ELEGIR"}},
    {L"STYLE", NULL, {L"STYLE", L"STIL", L"STILE", L"ESTILO"}},
    {L"VERY FAST", NULL, {L"TRES RAPIDE", L"SEHR SCHNELL", L"MOLTO VELOCE", L"MUY RÁPIDA"}},
    {L"VERY SLOW", NULL, {L"TRES LENTE", L"SEHR LANGSAM", L"MOLTO LENTA", L"MUY LENTA"}},
    {L"SOUND EFFECTS", NULL, {L"EFFETS SONORES", L"SOUNDEFFEKTE", L"EFFETTI SONORI", L"EFECTOS SONOROS"}},
    {L"MUSIC", NULL, {L"MUSIQUE", L"MUSIK", L"MUSICA", L"MÚSICA"}},
    {L"3D SOUND", NULL, {L"SON 3D", L"3D-SOUND", L"SUONO 3D", L"SONIDO 3D"}},
    {L"TEXT SPEED", NULL, {L"VITESSE TEXTE", L"TEXTTEMPO", L"VELOCITÀ TESTO", L"VELOCIDAD TEXTO"}},
    {L"LANGUAGE", NULL, {L"LANGUE", L"SPRACHE", L"LINGUA", L"IDIOMA"}},
    {L"DISPLAY", NULL, {L"AFFICHAGE", L"ANZEIGE", L"SCHERMO", L"PANTALLA"}},
    {L"FULL SCREEN", NULL, {L"PLEIN ECRAN", L"VOLLBILD", L"SCHERMO INTERO", L"PANTALLA COMPLETA"}},
    {L"BRIGHTNESS", NULL, {L"VALEUR GAMMA", L"GAMMA-WERT", L"VALORE GAMMA", L"VALOR GAMMA"}},
    {L"LIGHTING", NULL, {L"ECLAIRAGE", L"LICHT", L"ILLUMINAZIONE", L"LUCES"}},
    {L"DUSK", NULL, {L"CREPUSCULE", L"DÄMMERUNG", L"CREPUSCOLO", L"ATARDECER"}},
    {L"NOON", NULL, {L"MIDI", L"MITTAG", L"MEZZOGIORNO", L"MEDIODÍA"}},
    {L"DAY-NIGHT CYCLE", NULL, {L"CYCLE JOUR-NUIT", L"TAG-NACHT-ZYKLUS", L"CICLO GIORNO-NOTTE", L"CICLO DÍA-NOCHE"}},
    {L"CYCLE LENGTH", NULL, {L"DUREE DU CYCLE", L"ZYKLUSDAUER", L"DURATA CICLO", L"DURACIÓN CICLO"}},
    {L"START TIME", NULL, {L"HEURE DE DEPART", L"STARTZEIT", L"ORA INIZIALE", L"HORA INICIAL"}},
    {L"CLOCK", NULL, {L"HORLOGE", L"UHR", L"OROLOGIO", L"RELOJ"}},
    {L"EXPLODING SCORES", NULL, {L"SCORES EXPLOSIFS", L"PUNKTE-EXPLOSION", L"PUNTI ESPLOSIVI", L"PUNTOS EXPLOSIVOS"}},
    {L"BLOOD", NULL, {L"SANG", L"BLUT", L"SANGUE", L"SANGRE"}},
    {L"MULTIPLAYER", NULL, {L"MULTIJOUEUR", L"MEHRSPIELER", L"MULTIGIOCATORE", L"MULTIJUGADOR"}},
    {L"SOON", NULL, {L"BIENTOT", L"BALD", L"PRESTO", L"PRONTO"}},
    {L"CHECK FOR UPDATES", NULL, {L"MISES A JOUR", L"UPDATES SUCHEN", L"AGGIORNAMENTI", L"ACTUALIZACIONES"}},
    {L"AUTOSAVE", NULL, {L"SAUVEGARDE AUTO", L"AUTOSPEICHERN", L"SALVATAGGIO AUTO", L"GUARDADO AUTO"}},
    {L"AFTER EACH JOB OR FRENZY, AND WITH SAVE AND QUIT", NULL,
     {L"APRES CHAQUE MISSION OU FRENESIE, ET AVEC SAUVER ET QUITTER", L"NACH AUFTRAG ODER AMOKLAUF, UND MIT SPEICHERN UND BEENDEN",
      L"DOPO OGNI MISSIONE O FRENESIA, E CON SALVA ED ESCI", L"TRAS CADA MISIÓN O FRENESÍ, Y CON GUARDAR Y SALIR"}},
    {L"SAVE AND QUIT", NULL, {L"SAUVER ET QUITTER", L"SPEICHERN UND BEENDEN", L"SALVA ED ESCI", L"GUARDAR Y SALIR"}},
    {L"QUIT", NULL, {L"QUITTER", L"BEENDEN", L"ESCI", L"SALIR"}},
    {L"NOT DURING A JOB OR FRENZY", NULL,
     {L"PAS PENDANT UNE MISSION OU FRENESIE", L"NICHT WÄHREND AUFTRAG ODER AMOKLAUF", L"NON DURANTE MISSIONI O FRENESIE",
      L"NO DURANTE MISIONES O FRENESÍES"}},
    {L"NOT NOW", NULL, {L"PAS MAINTENANT", L"NICHT JETZT", L"NON ORA", L"AHORA NO"}},
    {L"NOT HERE", NULL, {L"PAS ICI", L"NICHT HIER", L"NON QUI", L"AQUÍ NO"}},
    {L"YOUR %ls AUTOSAVE WILL BE REPLACED", NULL,
     {L"VOTRE SAUVEGARDE AUTO DE %ls SERA REMPLACEE", L"DEIN AUTOSPEICHERSTAND VON %ls WIRD ERSETZT",
      L"IL TUO SALVATAGGIO AUTO DI %ls SARÀ SOSTITUITO", L"TU GUARDADO AUTO DE %ls SERÁ REEMPLAZADO"}},
    {L"SAVES", NULL, {L"SAUVEGARDES", L"SPEICHERN", L"SALVATAGGI", L"GUARDADO"}},
    {L"DIFFICULTY", NULL, {L"DIFFICULTE", L"SCHWIERIGKEIT", L"DIFFICOLTÀ", L"DIFICULTAD"}},
    {L"TIME OF DAY", NULL, {L"HEURE DU JOUR", L"TAGESZEIT", L"ORA DEL GIORNO", L"HORA DEL DÍA"}},
    {L"IMAGE", NULL, {L"IMAGE", L"BILD", L"IMMAGINE", L"IMAGEN"}},
    {L"SCREEN", NULL, {L"ECRAN", L"BILDSCHIRM", L"SCHERMO", L"PANTALLA"}},
    {L"VIEW", NULL, {L"VUE", L"ANSICHT", L"VISTA", L"VISTA"}},
    {L"POINTS BURST OUT WHERE THEY ARE SCORED", NULL,
     {L"LES POINTS EXPLOSENT LA OU ILS SONT MARQUES", L"PUNKTE ZERPLATZEN DORT, WO SIE ERZIELT WERDEN",
      L"I PUNTI ESPLODONO DOVE VENGONO FATTI", L"LOS PUNTOS ESTALLAN DONDE SE CONSIGUEN"}},
    {L"SOUNDS PLACED AROUND YOU, WITH THE ECHO OF THE PLACE", NULL,
     {L"SONS PLACES AUTOUR DE VOUS, AVEC L'ECHO DU LIEU", L"KLÄNGE RUND UM DICH, MIT DEM HALL DES ORTES",
      L"SUONI DISPOSTI INTORNO A TE, CON L'ECO DEL LUOGO", L"SONIDOS A TU ALREDEDOR, CON EL ECO DEL LUGAR"}},
    {L"BROAD DAYLIGHT, WITHOUT LIGHTS", NULL,
     {L"PLEIN JOUR, SANS LUMIERES", L"HELLER TAG, OHNE LICHTER", L"PIENO GIORNO, SENZA LUCI", L"PLENO DÍA, SIN LUCES"}},
    {L"THE ORIGINAL'S EVENING LIGHT, STREET LIGHTS ON", NULL,
     {L"LA LUMIERE DU SOIR DE L'ORIGINAL, LAMPADAIRES ALLUMES", L"DAS ABENDLICHT DES ORIGINALS, MIT STRASSENLATERNEN",
      L"LA LUCE SERALE DELL'ORIGINALE, LAMPIONI ACCESI", L"LA LUZ DE TARDE DEL ORIGINAL, FAROLAS ENCENDIDAS"}},
    {L"DAY, DUSK, NIGHT AND DAWN GO BY AS YOU PLAY", NULL,
     {L"JOUR, CREPUSCULE, NUIT ET AUBE DEFILENT EN JOUANT", L"TAG, DÄMMERUNG, NACHT UND MORGEN ZIEHEN VORBEI",
      L"GIORNO, CREPUSCOLO, NOTTE E ALBA SCORRONO GIOCANDO", L"DÍA, ATARDECER, NOCHE Y ALBA PASAN MIENTRAS JUEGAS"}},
    {L"REAL MINUTES FOR A WHOLE DAY AND NIGHT", NULL,
     {L"MINUTES REELLES POUR UN JOUR ET UNE NUIT", L"ECHTE MINUTEN FÜR EINEN TAG UND EINE NACHT",
      L"MINUTI REALI PER UN GIORNO E UNA NOTTE", L"MINUTOS REALES PARA UN DÍA Y UNA NOCHE"}},
    {L"THE HOUR A LEVEL STARTS AT", NULL,
     {L"L'HEURE A LAQUELLE UN NIVEAU COMMENCE", L"DIE UHRZEIT, ZU DER EIN LEVEL BEGINNT", L"L'ORA IN CUI INIZIA UN LIVELLO",
      L"LA HORA A LA QUE EMPIEZA UN NIVEL"}},
    {L"THE ORIGINAL'S PACE: A FRAME FOR EACH GAME STEP", NULL,
     {L"LE RYTHME DE L'ORIGINAL : UNE IMAGE PAR PAS DE JEU", L"DAS TEMPO DES ORIGINALS: EIN BILD PRO SPIELSCHRITT",
      L"IL RITMO DELL'ORIGINALE: UN FOTOGRAMMA PER PASSO DI GIOCO", L"EL RITMO DEL ORIGINAL: UNA IMAGEN POR PASO DE JUEGO"}},
    {L"FRAMES BETWEEN THE GAME'S 30 STEPS A SECOND, INTERPOLATED", NULL,
     {L"IMAGES INTERPOLEES ENTRE LES 30 PAS PAR SECONDE DU JEU", L"INTERPOLIERTE BILDER ZWISCHEN DEN 30 SCHRITTEN PRO SEKUNDE",
      L"FOTOGRAMMI INTERPOLATI TRA I 30 PASSI AL SECONDO DEL GIOCO", L"IMÁGENES INTERPOLADAS ENTRE LOS 30 PASOS POR SEGUNDO DEL JUEGO"}},
    {L"THE HUD'S DISTANCE FROM THE SCREEN'S EDGES, IN PERCENT", NULL,
     {L"MARGE DU HUD AUX BORDS DE L'ECRAN, EN POUR CENT", L"ABSTAND DES HUD ZUM BILDSCHIRMRAND, IN PROZENT",
      L"DISTANZA DELL'HUD DAI BORDI DELLO SCHERMO, IN PERCENTUALE", L"MARGEN DEL HUD CON LOS BORDES DE LA PANTALLA, EN PORCENTAJE"}},
    {L"ON FOOT: WALK, AND AIM WITH THE MOUSE OR RIGHT STICK", NULL,
     {L"A PIED : MARCHEZ, ET VISEZ A LA SOURIS OU AU STICK DROIT", L"ZU FUSS: GEHEN, UND MIT MAUS ODER RECHTEM STICK ZIELEN",
      L"A PIEDI: CAMMINA E MIRA CON IL MOUSE O LO STICK DESTRO", L"A PIE: CAMINA Y APUNTA CON EL RATÓN O EL STICK DERECHO"}},
    {L"ON FOOT: THE ORIGINAL'S TANK CONTROLS", NULL,
     {L"A PIED : LES COMMANDES TANK DE L'ORIGINAL", L"ZU FUSS: DIE PANZERSTEUERUNG DES ORIGINALS",
      L"A PIEDI: I COMANDI A CARRO ARMATO DELL'ORIGINALE", L"A PIE: LOS CONTROLES TIPO TANQUE DEL ORIGINAL"}},
    {L"THE VIEW LEADS TOWARD THE AIM. SHIFT, LT OR R3 LOOKS FURTHER", NULL,
     {L"LA VUE AVANCE VERS LA VISEE. MAJ, LT OU R3 POUR VOIR PLUS LOIN", L"DIE ANSICHT FOLGT DEM ZIEL. SHIFT, LT ODER R3: WEITER SEHEN",
      L"LA VISTA SI SPOSTA VERSO LA MIRA. SHIFT, LT O R3: PIÙ LONTANO", L"LA VISTA SE ADELANTA HACIA LA MIRA. SHIFT, LT O R3: MÁS LEJOS"}},
    {L"EXPERIMENTAL: THE LEGS FOLLOW YOUR MOVE, NOT YOUR AIM", NULL,
     {L"EXPERIMENTAL : LES JAMBES SUIVENT LE DEPLACEMENT, PAS LA VISEE", L"EXPERIMENTELL: DIE BEINE FOLGEN DER BEWEGUNG, NICHT DEM ZIEL",
      L"SPERIMENTALE: LE GAMBE SEGUONO IL MOVIMENTO, NON LA MIRA", L"EXPERIMENTAL: LAS PIERNAS SIGUEN EL MOVIMIENTO, NO LA MIRA"}},
    {L"THE ORIGINAL'S RUN, THE LEGS FACING THE AIM", NULL,
     {L"LA COURSE DE L'ORIGINAL, LES JAMBES VERS LA VISEE", L"DER LAUF DES ORIGINALS, DIE BEINE ZUM ZIEL",
      L"LA CORSA DELL'ORIGINALE, LE GAMBE VERSO LA MIRA", L"LA CARRERA DEL ORIGINAL, LAS PIERNAS HACIA LA MIRA"}},
    {L"A RING SHOWS WHERE YOU AIM", NULL,
     {L"UN ANNEAU MONTRE OU VOUS VISEZ", L"EIN RING ZEIGT, WOHIN DU ZIELST", L"UN ANELLO MOSTRA DOVE MIRI", L"UN ANILLO MUESTRA ADONDE APUNTAS"}},
    {L"NO RING: THE PLAYER FACES WHERE YOU AIM", NULL,
     {L"PAS D'ANNEAU : LE JOUEUR SE TOURNE VERS LA VISEE", L"KEIN RING: DIE FIGUR SCHAUT ZUM ZIEL", L"NESSUN ANELLO: IL GIOCATORE GUARDA VERSO LA MIRA",
      L"SIN ANILLO: EL JUGADOR MIRA HACIA DONDE APUNTAS"}},
    {L"THE VIEW STAYS ON THE PLAYER", NULL,
     {L"LA VUE RESTE SUR LE JOUEUR", L"DIE ANSICHT BLEIBT BEIM SPIELER", L"LA VISTA RESTA SUL GIOCATORE",
      L"LA VISTA SE QUEDA EN EL JUGADOR"}},
    {L"THE PAD SHAKES WHEN YOU ARE HIT, IN EXPLOSIONS AND IN CRASHES", NULL,
     {L"LA MANETTE VIBRE AUX COUPS RECUS, EXPLOSIONS ET ACCIDENTS", L"DAS GAMEPAD VIBRIERT BEI TREFFERN, EXPLOSIONEN UND UNFÄLLEN",
      L"IL GAMEPAD VIBRA SE SEI COLPITO, IN ESPLOSIONI E SCONTRI", L"EL MANDO VIBRA AL RECIBIR GOLPES, EN EXPLOSIONES Y CHOQUES"}},
    {L"BEAMS OF LIGHT AHEAD OF CARS AFTER DARK", NULL,
     {L"FAISCEAUX DEVANT LES VOITURES A LA NUIT TOMBEE", L"LICHTKEGEL VOR DEN AUTOS, WENN ES DUNKEL IST",
      L"FASCI DI LUCE DAVANTI ALLE AUTO CON IL BUIO", L"HACES DE LUZ DELANTE DE LOS COCHES AL ANOCHECER"}},
    {L"LIGHT FROM FIRES, EXPLOSIONS, GUNFIRE AND BRAKE LIGHTS", NULL,
     {L"LUMIERE DES FEUX, EXPLOSIONS, TIRS ET FEUX STOP", L"LICHT VON FEUER, EXPLOSIONEN, SCHÜSSEN UND BREMSLICHTERN",
      L"LUCE DI FUOCHI, ESPLOSIONI, SPARI E LUCI DEI FRENI", L"LUZ DE FUEGOS, EXPLOSIONES, DISPAROS Y LUCES DE FRENO"}},
    {L"HEAT HAZE, SMOKE AND EMBERS AROUND FIRES", NULL,
     {L"AIR CHAUD, FUMEE ET BRAISES AUTOUR DES FEUX", L"HITZEFLIMMERN, RAUCH UND GLUT UM FEUER",
      L"ARIA CALDA, FUMO E BRACI INTORNO AI FUOCHI", L"AIRE CALIENTE, HUMO Y BRASAS EN LOS FUEGOS"}},
    {L"SOFT SHADE IN CORNERS AND AT THE FOOT OF WALLS", NULL,
     {L"OMBRE DOUCE DANS LES COINS ET AU PIED DES MURS", L"WEICHER SCHATTEN IN ECKEN UND AM FUSS DER WÄNDE",
      L"OMBRA MORBIDA NEGLI ANGOLI E AI PIEDI DEI MURI", L"SOMBRA SUAVE EN LAS ESQUINAS Y AL PIE DE LOS MUROS"}},
    {L"THE ORIGINAL'S LOOK: EVERY OPTION BELOW OFF", NULL,
     {L"L'ASPECT DE L'ORIGINAL : TOUTES LES OPTIONS CI-DESSOUS COUPEES", L"DER LOOK DES ORIGINALS: ALLE OPTIONEN DARUNTER AUS",
      L"L'ASPETTO DELL'ORIGINALE: TUTTE LE OPZIONI SOTTO SPENTE", L"EL ASPECTO DEL ORIGINAL: TODAS LAS OPCIONES DE ABAJO DESACTIVADAS"}},
    {L"NEW LIGHTS, SOFT SHADOWS, FULL COLOURS, SMOOTHER EDGES", NULL,
     {L"NOUVELLES LUMIERES, OMBRES DOUCES, COULEURS PLEINES, BORDS LISSES", L"NEUE LICHTER, WEICHE SCHATTEN, VOLLE FARBEN, GLATTE KANTEN",
      L"NUOVE LUCI, OMBRE MORBIDE, COLORI PIENI, BORDI PIÙ LISCI", L"LUCES NUEVAS, SOMBRAS SUAVES, COLOR PLENO, BORDES MÁS LISOS"}},
    {L"YOUR OWN MIX OF THE OPTIONS BELOW", NULL,
     {L"VOTRE PROPRE CHOIX D'OPTIONS CI-DESSOUS", L"DEINE EIGENE MISCHUNG DER OPTIONEN DARUNTER", L"UN TUO MIX DELLE OPZIONI QUI SOTTO",
      L"TU PROPIA MEZCLA DE LAS OPCIONES DE ABAJO"}},
    {L"LIGHTS WORKED OUT FOR EACH PIXEL, NOT AT TILE CORNERS", NULL,
     {L"LUMIERES CALCULEES PAR PIXEL, PAS AUX COINS DES TUILES", L"LICHT PRO PIXEL BERECHNET, NICHT AN DEN KACHELECKEN",
      L"LUCI CALCOLATE PER PIXEL, NON AGLI ANGOLI DELLE TESSERE", L"LUCES CALCULADAS POR PÍXEL, NO EN LAS ESQUINAS DE LAS BALDOSAS"}},
    {L"A GLOW AROUND FIRES, EXPLOSIONS AND LIGHTS", NULL,
     {L"UN HALO AUTOUR DES FEUX, EXPLOSIONS ET LUMIERES", L"EIN LEUCHTEN UM FEUER, EXPLOSIONEN UND LICHTER",
      L"UN BAGLIORE INTORNO A FUOCHI, ESPLOSIONI E LUCI", L"UN BRILLO ALREDEDOR DE FUEGOS, EXPLOSIONES Y LUCES"}},
    {L"FULL COLOURS INSTEAD OF THE ORIGINAL'S 16-BIT TEXTURES", NULL,
     {L"COULEURS PLEINES AU LIEU DES TEXTURES 16 BITS DE L'ORIGINAL", L"VOLLE FARBEN STATT DER 16-BIT-TEXTUREN DES ORIGINALS",
      L"COLORI PIENI INVECE DELLE TEXTURE A 16 BIT DELL'ORIGINALE", L"COLOR COMPLETO EN VEZ DE LAS TEXTURAS DE 16 BITS DEL ORIGINAL"}},
    {L"FULL SCREEN IN THIS RESOLUTION AND REFRESH RATE", NULL,
     {L"PLEIN ECRAN DANS CETTE RESOLUTION ET CETTE FREQUENCE", L"VOLLBILD IN DIESER AUFLÖSUNG UND BILDWIEDERHOLRATE",
      L"SCHERMO INTERO IN QUESTA RISOLUZIONE E FREQUENZA", L"PANTALLA COMPLETA CON ESTA RESOLUCIÓN Y FRECUENCIA"}},
    {L"FULL SCREEN AT THE DESKTOP'S RESOLUTION", NULL,
     {L"PLEIN ECRAN DANS LA RESOLUTION DU BUREAU", L"VOLLBILD IN DER AUFLÖSUNG DES DESKTOPS", L"SCHERMO INTERO ALLA RISOLUZIONE DEL DESKTOP",
      L"PANTALLA COMPLETA CON LA RESOLUCIÓN DEL ESCRITORIO"}},
    {L"CONTINUE", NULL, {L"CONTINUER", L"WEITER", L"CONTINUA", L"CONTINUAR"}},
    {L"PROGRESS SINCE YOUR LAST SAVE WILL BE LOST", NULL,
     {L"LA PROGRESSION DEPUIS VOTRE DERNIERE SAUVEGARDE SERA PERDUE", L"DER FORTSCHRITT SEIT DEM LETZTEN SPEICHERN GEHT VERLOREN",
      L"I PROGRESSI DALL'ULTIMO SALVATAGGIO ANDRANNO PERSI", L"SE PERDERÁ EL PROGRESO DESDE TU ÚLTIMO GUARDADO"}},
    {L"SAVING", NULL, {L"SAUVEGARDE", L"SPEICHERN", L"SALVATAGGIO", L"GUARDANDO"}},
    {L"SAVE FAILED", NULL, {L"ECHEC DE LA SAUVEGARDE", L"SPEICHERN FEHLGESCHLAGEN", L"SALVATAGGIO FALLITO", L"ERROR AL GUARDAR"}},
    {L"RETRY JOBS", NULL, {L"REESSAYER MISSIONS", L"AUFTRÄGE WIEDERHOLEN", L"RIPROVA MISSIONI", L"REINTENTAR MISIONES"}},
    {L"RETRY A FAILED JOB OR FRENZY FROM THE PAUSE MENU", NULL,
     {L"REESSAYER UNE MISSION OU FRENESIE RATEE DEPUIS LE MENU PAUSE", L"NACH EINEM FEHLSCHLAG IM PAUSENMENÜ WIEDERHOLEN",
      L"RIPROVA UNA MISSIONE O FRENESIA FALLITA DAL MENU DI PAUSA", L"REINTENTA UNA MISIÓN O FRENESÍ FALLIDO DESDE EL MENÚ DE PAUSA"}},
    {L"FRENZY RESPAWN", NULL, {L"RETOUR DES FRENESIES", L"AMOKLÄUFE ZURÜCK", L"RITORNO FRENESIE", L"REGRESO DE FRENESÍES"}},
    {L"A FAILED KILL FRENZY COMES BACK ON THE MAP", NULL,
     {L"UNE FRENESIE RATEE REVIENT SUR LA CARTE", L"EIN GESCHEITERTER AMOKLAUF KEHRT AUF DIE KARTE ZURÜCK",
      L"UNA FRENESIA FALLITA TORNA SULLA MAPPA", L"UN FRENESÍ FALLIDO VUELVE AL MAPA"}},
    {L"INFINITE LIVES", NULL, {L"VIES INFINIES", L"UNENDLICHE LEBEN", L"VITE INFINITE", L"VIDAS INFINITAS"}},
    {L"YOU NEVER RUN OUT OF LIVES: NO GAME OVER", NULL,
     {L"VOUS NE MANQUEZ JAMAIS DE VIES : PAS DE FIN DE PARTIE", L"DIR GEHEN NIE DIE LEBEN AUS: KEIN SPIELENDE",
      L"NON RESTI MAI SENZA VITE: NIENTE FINE PARTITA", L"NUNCA TE QUEDAS SIN VIDAS: NO HAY FIN DE LA PARTIDA"}},
    {L"%ls: RETRY", NULL, {L"%ls : REESSAYER", L"%ls: WIEDERHOLEN", L"%ls: RIPROVA", L"%ls: REINTENTAR"}},
    {L"RETRY", NULL, {L"REESSAYER", L"WIEDERHOLEN", L"RIPROVA", L"REINTENTAR"}},
    {L"RETRY: %ls", NULL, {L"REESSAYER : %ls", L"WIEDERHOLEN: %ls", L"RIPROVA: %ls", L"REINTENTAR: %ls"}},
    {L"RETRY JOB", NULL, {L"REESSAYER LA MISSION", L"AUFTRAG WIEDERHOLEN", L"RIPROVA MISSIONE", L"REINTENTAR MISIÓN"}},
    {L"RETRY FRENZY", NULL, {L"REESSAYER LA FRENESIE", L"AMOKLAUF WIEDERHOLEN", L"RIPROVA FRENESIA", L"REINTENTAR FRENESÍ"}},
    {L"RESTART: %ls", NULL, {L"RECOMMENCER : %ls", L"NEU STARTEN: %ls", L"RICOMINCIA: %ls", L"REINICIAR: %ls"}},
    {L"RESTART JOB", NULL, {L"RECOMMENCER LA MISSION", L"AUFTRAG NEU STARTEN", L"RICOMINCIA MISSIONE", L"REINICIAR MISIÓN"}},
    {L"RESTART FRENZY", NULL, {L"RECOMMENCER LA FRENESIE", L"AMOKLAUF NEU STARTEN", L"RICOMINCIA FRENESIA", L"REINICIAR FRENESÍ"}},
    {L"BEFORE LAST JOB", NULL, {L"AVANT LA MISSION", L"VOR DEM AUFTRAG", L"PRIMA DELLA MISSIONE", L"ANTES DE LA MISIÓN"}},
    {L"BEFORE LAST FRENZY", NULL, {L"AVANT LA FRENESIE", L"VOR DEM AMOKLAUF", L"PRIMA DELLA FRENESIA", L"ANTES DEL FRENESÍ"}},
    {L"JUST NOW", NULL, {L"A L'INSTANT", L"GERADE EBEN", L"ORA", L"AHORA"}},
    {L"%d MIN AGO", NULL, {L"IL Y A %d MIN", L"VOR %d MIN", L"%d MIN FA", L"HACE %d MIN"}},
    {L"LOAD GAME", NULL, {L"CHARGER UNE PARTIE", L"SPIEL LADEN", L"CARICA PARTITA", L"CARGAR PARTIDA"}},
    {L"MONEY", NULL, {L"ARGENT", L"GELD", L"SOLDI", L"DINERO"}},
    {L"JOBS", NULL, {L"MISSIONS", L"AUFTRÄGE", L"MISSIONI", L"MISIONES"}},
    {L"DATE", NULL, {L"DATE", L"DATUM", L"DATA", L"FECHA"}},
    {L"CITY", NULL, {L"VILLE", L"STADT", L"CITTÀ", L"CIUDAD"}},
    {L"NO SAVE", NULL, {L"AUCUNE SAUVEGARDE", L"KEIN SPIELSTAND", L"NESSUN SALVATAGGIO", L"NINGUNA PARTIDA"}},
    {L"BEFORE: %ls", NULL, {L"AVANT : %ls", L"VOR: %ls", L"PRIMA DI: %ls", L"ANTES DE: %ls"}},
    {L"CHURCH", NULL, {L"EGLISE", L"KIRCHE", L"CHIESA", L"IGLESIA"}},
    {L"DOWNTOWN", NULL, {L"CENTRE-VILLE", L"INNENSTADT", L"CENTRO", L"CENTRO"}},
    {L"RESIDENTIAL", NULL, {L"RESIDENTIEL", L"WOHNVIERTEL", L"RESIDENZIALE", L"RESIDENCIAL"}},
    {L"INDUSTRIAL", NULL, {L"INDUSTRIEL", L"INDUSTRIEGEBIET", L"INDUSTRIALE", L"INDUSTRIAL"}},
    {L"TODAY", NULL, {L"AUJOURD'HUI", L"HEUTE", L"OGGI", L"HOY"}},
    {L"YESTERDAY", NULL, {L"HIER", L"GESTERN", L"IERI", L"AYER"}},
    {L"%d DAYS AGO", NULL, {L"IL Y A %d JOURS", L"VOR %d TAGEN", L"%d GIORNI FA", L"HACE %d DÍAS"}},
    {L"FRAME LIMIT", NULL, {L"LIMITE IMAGES", L"BILDRATENLIMIT", L"LIMITE FOTOGRAMMI", L"LÍMITE IMÁGENES"}},
    {L"VSYNC", NULL, {L"VSYNC", L"VSYNC", L"VSYNC", L"VSYNC"}},
    {L"SHOW FPS", NULL, {L"AFFICHER FPS", L"FPS ANZEIGEN", L"MOSTRA FPS", L"MOSTRAR FPS"}},
    {L"ORIGINAL", NULL, {L"ORIGINAL", L"ORIGINAL", L"ORIGINALE", L"ORIGINAL"}},
    {L"UNLIMITED", NULL, {L"ILLIMITE", L"UNBEGRENZT", L"ILLIMITATO", L"ILIMITADO"}},
    {L"HUD SCALE", NULL, {L"TAILLE HUD", L"HUD-GRÖSSE", L"SCALA HUD", L"ESCALA HUD"}},
    {L"SAFE AREA", NULL, {L"MARGE ECRAN", L"BILDRAND", L"MARGINE SCHERMO", L"MARGEN PANTALLA"}},
    {L"DISPLAY MODE", NULL, {L"MODE VIDEO", L"BILDSCHIRMMODUS", L"MODALITÀ VIDEO", L"MODO DE VÍDEO"}},
    {L"DESKTOP", NULL, {L"BUREAU", L"DESKTOP", L"DESKTOP", L"ESCRITORIO"}},
    {L"BLOOM", NULL, {L"FLOU LUMINEUX", L"BLOOM", L"BLOOM", L"RESPLANDOR"}},
    {L"TRUE COLOUR", NULL, {L"VRAIES COULEURS", L"ECHTFARBEN", L"COLORI REALI", L"COLOR REAL"}},
    {L"PIXEL LIGHTING", NULL, {L"ECLAIRAGE PIXEL", L"PIXEL-LICHT", L"LUCE PER PIXEL", L"LUZ POR PÍXEL"}},
    {L"CAR HEADLIGHTS", NULL, {L"PHARES", L"SCHEINWERFER", L"FARI", L"FAROS"}},
    {L"CAMERA ZOOM", NULL, {L"ZOOM CAMERA", L"KAMERAZOOM", L"ZOOM CAMERA", L"ZOOM CÁMARA"}},
    {L"LIGHT EFFECTS", NULL, {L"EFFETS LUMINEUX", L"LICHTEFFEKTE", L"EFFETTI LUCE", L"EFECTOS DE LUZ"}},
    {L"TEXTURE FILTERING", NULL, {L"FILTRAGE TEXTURES", L"TEXTURFILTER", L"FILTRO TEXTURE", L"FILTRADO TEXTURAS"}},
    {L"BILINEAR", NULL, {L"BILINEAIRE", L"BILINEAR", L"BILINEARE", L"BILINEAL"}},
    {L"ANISOTROPIC", NULL, {L"ANISOTROPE", L"ANISOTROP", L"ANISOTROPICO", L"ANISOTRÓPICO"}},
    {L"PIXELATED", NULL, {L"PIXELISE", L"VERPIXELT", L"PIXELLATO", L"PIXELADO"}},
    {L"SHARP BILINEAR", NULL, {L"BILINEAIRE NET", L"SCHARF BILINEAR", L"BILINEARE NITIDO", L"BILINEAL NÍTIDO"}},
    {L"ANTI-ALIASING", NULL, {L"ANTICRENELAGE", L"KANTENGLÄTTUNG", L"ANTI-ALIASING", L"ANTIALIASING"}},
    {L"SHADOWS", NULL, {L"OMBRES", L"SCHATTEN", L"OMBRE", L"SOMBRAS"}},
    {L"AMBIENT OCCLUSION", NULL, {L"OCCLUSION AMBIANTE", L"UMGEBUNGSVERDECKUNG", L"OCCLUSIONE AMBIENTALE", L"OCLUSIÓN AMBIENTAL"}},
    {L"FIRE EFFECTS", NULL, {L"EFFETS DE FEU", L"FEUEREFFEKTE", L"EFFETTI DEL FUOCO", L"EFECTOS DE FUEGO"}},
    {L"SOFT", NULL, {L"DOUCES", L"WEICH", L"MORBIDE", L"SUAVES"}},
    {L"PRESET", NULL, {L"PRESET", L"VOREINSTELLUNG", L"PRESET", L"PREAJUSTE"}},
    {L"CLASSIC", NULL, {L"CLASSIQUE", L"KLASSISCH", L"CLASSICO", L"CLÁSICO"}},
    {L"ENHANCED", NULL, {L"AMELIORE", L"VERBESSERT", L"MIGLIORATO", L"MEJORADO"}},
    {L"CUSTOM", NULL, {L"PERSO", L"EIGENE", L"PERSONAL.", L"PERSONAL"}},
    {L"CHOOSE YOUR STYLE", NULL, {L"CHOISISSEZ VOTRE STYLE", L"WÄHLE DEINEN STIL", L"SCEGLI IL TUO STILE", L"ELIGE TU ESTILO"}},
    {L"THE GAME AS IT WAS IN 1999", NULL,
     {L"LE JEU TEL QU'IL ETAIT EN 1999", L"DAS SPIEL WIE IM JAHR 1999", L"IL GIOCO COM'ERA NEL 1999", L"EL JUEGO TAL COMO ERA EN 1999"}},
    {L"REFINED LIGHTS AND SHADOWS", NULL,
     {L"LUMIERES ET OMBRES AFFINEES", L"VERFEINERTE LICHTER UND SCHATTEN", L"LUCI E OMBRE RAFFINATE", L"LUCES Y SOMBRAS REFINADAS"}},
    {L"DAY AND NIGHT CYCLE", NULL, {L"CYCLE JOUR ET NUIT", L"WECHSEL VON TAG UND NACHT", L"CICLO GIORNO E NOTTE", L"CICLO DE DÍA Y NOCHE"}},
    {L"TWIN STICK CONTROLS", NULL,
     {L"COMMANDES DOUBLE STICK", L"TWIN STICK STEUERUNG", L"COMANDI DOPPIO STICK", L"CONTROLES DE DOBLE STICK"}},
    {L"AUTOSAVE AND JOB RETRY", NULL,
     {L"SAUVEGARDE AUTO ET REESSAI", L"AUTOSPEICHERN UND NEUVERSUCH", L"SALVATAGGIO AUTO E RIPROVA", L"GUARDADO AUTO Y REINTENTOS"}},
    {L"ALWAYS INCLUDED", NULL, {L"TOUJOURS INCLUS", L"IMMER DABEI", L"SEMPRE INCLUSI", L"SIEMPRE INCLUIDO"}},
    {L"WIDESCREEN", NULL, {L"ECRAN LARGE", L"BREITBILD", L"SCHERMO LARGO", L"PANTALLA ANCHA"}},
    {L"HD RESOLUTIONS", NULL, {L"RESOLUTIONS HD", L"HD AUFLÖSUNGEN", L"RISOLUZIONI HD", L"RESOLUCIONES HD"}},
    {L"SMOOTH 60+ FPS", NULL, {L"60+ FPS FLUIDES", L"FLÜSSIGE 60+ FPS", L"60+ FPS FLUIDI", L"60+ FPS FLUIDOS"}},
    {L"YOU CAN CHANGE THIS AND MUCH MORE IN OPTIONS", NULL,
     {L"VOUS POUVEZ CHANGER CELA ET BIEN PLUS DANS LES OPTIONS", L"DIES UND VIELES MEHR KANNST DU IN DEN OPTIONEN ÄNDERN",
      L"PUOI CAMBIARLO, E MOLTO ALTRO, NELLE OPZIONI", L"PUEDES CAMBIAR ESTO Y MUCHO MÁS EN OPCIONES"}},
    {L"ON", NULL, {L"OUI", L"AN", L"SÌ", L"SÍ"}},
    {L"OFF", NULL, {L"NON", L"AUS", L"NO", L"NO"}},
    {L"GAME", NULL, {L"JEU", L"SPIEL", L"GIOCO", L"JUEGO"}},
    {L"GRAPHICS", NULL, {L"GRAPHISMES", L"GRAFIK", L"GRAFICA", L"GRÁFICOS"}},
    {L"SOUND", NULL, {L"SON", L"SOUND", L"AUDIO", L"SONIDO"}},
    {L"STATISTICS", NULL, {L"STATISTIQUES", L"STATISTIKEN", L"STATISTICHE", L"ESTADÍSTICAS"}},
    {L"MISSION TRANSCRIPT", NULL, {L"MESSAGES DE MISSION", L"AUFTRAGSNACHRICHTEN", L"MESSAGGI DI MISSIONE", L"MENSAJES DE MISIÓN"}},
    {L"NO MESSAGES YET", NULL, {L"AUCUN MESSAGE", L"NOCH KEINE NACHRICHTEN", L"NESSUN MESSAGGIO", L"AÚN NO HAY MENSAJES"}},
    {L"NEEDS", NULL, {L"NECESSITE", L"BENÖTIGT", L"RICHIEDE", L"REQUIERE"}},
    {L"PLAYERS", NULL, {L"JOUEURS", L"SPIELER", L"GIOCATORI", L"JUGADORES"}},
    {L"PLAYER", NULL, {L"JOUEUR", L"SPIELER", L"GIOCATORE", L"JUGADOR"}},
    {L"FRAGS", NULL, {L"FRAGS", L"FRAGS", L"FRAG", L"FRAGS"}},
    {L"POINTS", NULL, {L"POINTS", L"PUNKTE", L"PUNTI", L"PUNTOS"}},
    {L"IT", NULL, {L"CHAT", L"FANGER", L"CE L'HAI", L"LA LLEVAS"}},
    {L"DEATHS", NULL, {L"MORTS", L"TODE", L"MORTI", L"MUERTES"}},
    {L"PING", NULL, {L"PING", L"PING", L"PING", L"PING"}},
    {L"READY", NULL, {L"PRET", L"BEREIT", L"PRONTO", L"LISTO"}},
    {L"NOT READY", NULL, {L"PAS PRET", L"NICHT BEREIT", L"NON PRONTO", L"NO LISTO"}},
    {L"VOTE YES", NULL, {L"VOTER OUI", L"MIT JA STIMMEN", L"VOTA SÌ", L"VOTAR SÍ"}},
    {L"VOTE NO", NULL, {L"VOTER NON", L"MIT NEIN STIMMEN", L"VOTA NO", L"VOTAR NO"}},
    {L"LEAVE GAME", NULL, {L"QUITTER LA PARTIE", L"SPIEL VERLASSEN", L"ESCI DALLA PARTITA", L"SALIR DE LA PARTIDA"}},
    {L"LEAVE", NULL, {L"QUITTER", L"VERLASSEN", L"ESCI", L"SALIR"}},
    {L"NOT IN NETWORK GAMES", NULL, {L"PAS EN PARTIE RESEAU", L"NICHT IM NETZWERKSPIEL", L"NON NELLE PARTITE IN RETE", L"NO EN PARTIDAS EN RED"}},
    {L"CONTROLS", NULL, {L"COMMANDES", L"STEUERUNG", L"COMANDI", L"CONTROLES"}},
    {L"AIM CAMERA", NULL, {L"CAMERA DE VISEE", L"ZIELKAMERA", L"CAMERA DI MIRA", L"CÁMARA DE MIRA"}},
    {L"STRAFE ANIMATION", NULL, {L"ANIMATION DE STRAFE", L"STRAFE-ANIMATION", L"ANIMAZIONE STRAFE", L"ANIMACIÓN DE STRAFE"}},
    {L"CROSSHAIR", NULL, {L"VISEUR", L"FADENKREUZ", L"MIRINO", L"PUNTO DE MIRA"}},
    {L"VIBRATION", NULL, {L"VIBRATIONS", L"VIBRATION", L"VIBRAZIONE", L"VIBRACIÓN"}},
    {L"TWIN STICK", NULL, {L"DOUBLE STICK", L"TWIN-STICK", L"DOPPIO STICK", L"DOBLE STICK"}},
    {L"DEFAULT KEYS", NULL, {L"CONTROLES PAR DEFAUT", L"STANDARDSTEUERUNG", L"COMANDI PREDEFINITI", L"CONTROLES PREDETERMINADOS"}},
    {L"KEYBOARD", NULL, {L"CLAVIER", L"TASTATUR", L"TASTIERA", L"TECLADO"}},
    {L"GAMEPAD", NULL, {L"MANETTE", L"GAMEPAD", L"GAMEPAD", L"MANDO"}},
    {L"ALTERNATE", NULL, {L"SECONDAIRE", L"ALTERNATIV", L"ALTERNATIVO", L"ALTERNATIVA"}},
    {L"MOUSE", NULL, {L"SOURIS", L"MAUS", L"MOUSE", L"RATÓN"}},
    {L"WHEEL", NULL, {L"MOLETTE", L"RAD", L"ROTELLA", L"RUEDA"}},
    {L"DEL", "del_i", {}},
    {L"CLEAR", NULL, {L"EFFACER", L"LÖSCHEN", L"CANCELLA", L"BORRAR"}},
    {L"PRESS", NULL, {L"APPUYEZ", L"DRÜCKEN", L"PREMI", L"PULSA"}},
    {L"FORWARD", NULL, {L"AVANT", L"VORWÄRTS", L"AVANTI", L"ADELANTE"}},
    {L"BACKWARD", NULL, {L"ARRIERE", L"ZURÜCK", L"INDIETRO", L"ATRÁS"}},
    {L"ATTACK", NULL, {L"ATTAQUE", L"ANGRIFF", L"ATTACCA", L"ATACAR"}},
    {L"ENTER OR EXIT", NULL, {L"ENTREE/SORTIE", L"EIN-/AUSSTEIGEN", L"ENTRA/ESCI", L"ENTRAR/SALIR"}},
    {L"JUMP OR BRAKE", NULL, {L"FREIN/SAUT", L"SPRUNG/BREMSE", L"FRENA/SALTA", L"FRENO/SALTAR"}},
    {L"PREVIOUS WEAPON", NULL, {L"ARME PRECEDENTE", L"VORIGE WAFFE", L"ARMA PRECEDENTE", L"ARMA ANTERIOR"}},
    {L"NEXT WEAPON", NULL, {L"ARME SUIVANTE", L"NÄCHSTE WAFFE", L"ARMA SUCCESSIVA", L"ARMA SIGUIENTE"}},
    {L"HORN OR TAUNT", NULL, {L"KLAXON/NARGUER", L"HUPE/SPOTT", L"CLACSON/PROVOCA", L"CLÁXON/BURLA"}},
    {L"LOOK AROUND", NULL, {L"REGARDER AUTOUR", L"UMSCHAUEN", L"GUARDARSI INTORNO", L"MIRAR ALREDEDOR"}},
    {L"SCORES", NULL, {L"SCORES", L"PUNKTE", L"PUNTEGGI", L"PUNTOS"}},
};
const s32 kNumTranslations = sizeof(kTranslations) / sizeof(kTranslations[0]);

// The texts in the game's font codes, for sCacheLang.
wchar_t sCache[kNumTranslations][96];
char sCacheLang;

s32 TranslationLanguage(char code)
{
    switch (code)
    {
        case 'f':
            return 0;
        case 'g':
            return 1;
        case 'i':
            return 2;
        case 's':
            return 3;
        default:
            return -1;
    }
}

s32 Clamp(s32 v, s32 lo, s32 hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

s32 Wrap(s32 v, s32 n)
{
    return (v % n + n) % n;
}

void SetOption(const char* name, DWORD type, const void* pData, DWORD size)
{
    HKEY hKey;
    if (RegCreateKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\DMA Design Ltd\\GTA2\\Option", 0, NULL, 0, KEY_ALL_ACCESS, NULL,
                        &hKey, NULL) == ERROR_SUCCESS)
    {
        RegSetValueExA(hKey, name, 0, type, static_cast<const BYTE*>(pData), size);
        RegCloseKey(hKey);
    }
}

s32 LanguageIndex()
{
    char code[16] = "e";
    gRegistry.Get_Option("language", reinterpret_cast<BYTE*>(code), sizeof(code));
    for (s32 i = 0; i < kNumLanguages; i++)
    {
        if (kLanguages[i].mCode == code[0] && LanguageInstalled(code[0]))
        {
            return i;
        }
    }
    return 0;
}

// Volumes are shown 0 to 10, stored 0 to 127.
s32 VolumeStep(s32 volume)
{
    return (volume * 10 + 63) / 127;
}

const char* ScreenName(Id id)
{
    switch (id)
    {
        case kLighting:
            return "lighting";
        case kExplodingScores:
            return "exploding_on";
        case kFrameLimit:
            return "port_max_fps";
        case kVsync:
            return "port_vsync";
        case kTrueColour:
            return "gl_true_colour";
        case kBloom:
            return "gl_bloom";
        case kPerPixelLighting:
            return "gl_pixel_lighting";
        case kZoom:
            return "camera_zoom";
        case kHudScale:
            return "hud_scale";
        case kSafeArea:
            return "hud_margin";
        case kLightEffects:
            return "light_effects";
        case kTextureFilter:
            return "gl_texture_filter";
        case kAntiAliasing:
            return "gl_msaa";
        case kShadows:
            return "gl_soft_shadows";
        case kDayLength:
            return "day_length";
        case kStartTime:
            return "day_start";
        case kClock:
            return "day_clock";
        case kControls:
            return "port_controls";
        case kAimCamera:
            return "port_aim_camera";
        case kStrafeAnimation:
            return "port_strafe_animation";
        case kCrosshair:
            return "port_crosshair";
        case kVibration:
            return "port_vibration";
        case kBlood:
            return "blood";
        case kUpdateCheck:
            return "update_check";
        case kShowFps:
            return "port_show_fps";
        case kAutosave:
            return "port_autosave";
        case kRetry:
            return "port_retry";
        case kKfRespawn:
            return "port_kf_respawn";
        case kInfiniteLives:
            return "port_infinite_lives";
        case kAmbientOcclusion:
            return "gl_ambient_occlusion";
        case kFireEffects:
            return "gl_fire_effects";
        default:
            return "car_headlights";
    }
}

#if defined(GTA2_PORT_SDL)
s32 PresetDefault(Id id);
#endif

// The game's own settings are on by default; the port's graphics additions off.
s32 ScreenDefault(Id id)
{
#if defined(GTA2_PORT_SDL)
    const s32 preset = PresetDefault(id);
    if (preset >= 0)
    {
        return preset;
    }
#endif
    if (id == kFrameLimit)
    {
        return 30;
    }
    if (id == kHudScale)
    {
        return 100;
    }
    if (id == kDayLength)
    {
        return 24;
    }
    if (id == kStartTime)
    {
        return 12;
    }
    if (id == kVibration)
    {
        return 1;
    }
    return id == kLighting || id == kExplodingScores || id == kVsync || id == kClock || id == kAimCamera || id == kStrafeAnimation || id == kCrosshair || id == kBlood || id == kUpdateCheck ? 1 : 0;
}

s32 Screen(Id id)
{
#if defined(__EMSCRIPTEN__)
    // The page's ?nofx (web/site/app.js): the ENHANCED rendering passes off for this session, to compare frame rates.
    static const bool sNoFx = getenv("GTA2_NOFX") != NULL;
    if (sNoFx && (id == kPerPixelLighting || id == kShadows || id == kAmbientOcclusion || id == kFireEffects ||
                  id == kBloom || id == kAntiAliasing))
    {
        return 0;
    }
#endif
#if defined(GTA2_PORT_SDL)
    // Read many times a frame: from the registry again only once it changed (a value read without one writes it).
    static s32 sValue[kCount];
    static unsigned sReadAt[kCount]; // the registry's generation + 1 when read, 0 not yet
    if (id >= 0 && id < kCount)
    {
        if (sReadAt[id] != port_RegistryGeneration() + 1)
        {
            sValue[id] = gRegistry.Get_Screen_Setting(ScreenName(id), ScreenDefault(id));
            sReadAt[id] = port_RegistryGeneration() + 1;
        }
        return sValue[id];
    }
#endif
    return gRegistry.Get_Screen_Setting(ScreenName(id), ScreenDefault(id));
}

const s32 kMaxBloom = 5;

// Steps of the camera zoom: 0 is the original's, each step closer magnifies by kZoomStep.
const s32 kMinZoom = -5;
const s32 kMaxZoom = 5;
const f32 kZoomStep = 1.125f;
// Cached: the camera and the map renderer read it several times a frame.
bool sZoomRead;
s32 sZoom;
Fix16 sZoomFactor;

void SetZoom(s32 zoom)
{
    sZoom = Clamp(zoom, kMinZoom, kMaxZoom);
    sZoomFactor = Fix16(static_cast<s32>(powf(kZoomStep, static_cast<f32>(sZoom)) * 16384.0f + 0.5f), 0);
    sZoomRead = true;
}

s32 Zoom()
{
    if (!sZoomRead)
    {
        SetZoom(Screen(kZoom));
    }
    return sZoom;
}

// HUD SCALE in percent, by steps of 10; SAFE AREA in percent. Cached: each HUD element reads them.
const s32 kMinHudScale = 50;
const s32 kMaxHudScale = 150;
const s32 kMaxSafeArea = 10;
s32 sHudScale = -1;
s32 sSafeArea = -1;
s32 sTwinStick = -1;
const s32 kMinDayLength = 3;
const s32 kMaxDayLength = 60;
#if defined(GTA2_PORT_SDL)
const wchar_t* const kLightings[] = {L"NOON", L"DUSK", L"DAY-NIGHT CYCLE"};
#else
const wchar_t* const kLightings[] = {L"NOON", L"DUSK"};
#endif
const s32 kNumLightings = sizeof(kLightings) / sizeof(kLightings[0]);

s32 HudScalePercent()
{
    if (sHudScale < 0)
    {
        sHudScale = Clamp(Screen(kHudScale), kMinHudScale, kMaxHudScale) / 10 * 10;
    }
    return sHudScale;
}

s32 SafeAreaPercent()
{
    if (sSafeArea < 0)
    {
        sSafeArea = Clamp(Screen(kSafeArea), 0, kMaxSafeArea);
    }
    return sSafeArea;
}

// The HUD's scale is set with the screen's size, as the HUD lays its texts out.
void RefreshHud()
{
    if (port_GameRunning() && gViewCamera)
    {
        gGame_0x40->SetScreenSize(gViewCamera->mScreenPxWidth, gViewCamera->mScreenPxHeight);
    }
}
#if defined(GTA2_PORT_SDL)
// A sample count, lowered to one the renderer can do.
s32 MsaaIndex(s32 stored)
{
    const s32 max = port_MaxMsaaSamples();
    s32 i = kNumMsaaValues - 1;
    while (i > 0 && (kMsaaValues[i] > stored || kMsaaValues[i] > max))
    {
        i--;
    }
    return i;
}

s32 TextureFilterIndex()
{
    s32 i = 0;
    while (i < kNumTextureFilters - 1 && kTextureFilterValues[i] < Screen(kTextureFilter))
    {
        i++;
    }
    return i;
}

s32 MsaaIndex()
{
    return MsaaIndex(Screen(kAntiAliasing));
}

// PRESET: the GRAPHICS settings' values for CLASSIC, the original's look, and ENHANCED. It shows
// CUSTOM when the settings match neither.
enum Preset
{
    kClassic,
    kEnhanced,
    kCustom,
};
const wchar_t* const kPresetNames[] = {L"CLASSIC", L"ENHANCED", L"CUSTOM"};

struct PresetValue
{
    Id mId;
    s32 mValue[2];
};
const PresetValue kPresetValues[] = {{kTrueColour, {0, 1}},   {kBloom, {0, 2}},        {kPerPixelLighting, {0, 1}},
                                     {kTextureFilter, {0, 2}}, {kAntiAliasing, {0, 4}}, {kShadows, {0, 1}},
                                     {kHeadlights, {0, 1}},    {kLightEffects, {0, 1}}, {kAmbientOcclusion, {0, 1}},
                                     {kFireEffects, {0, 1}}};
const s32 kNumPresetValues = sizeof(kPresetValues) / sizeof(kPresetValues[0]);

// As the menu shows them: anti-aliasing at what the renderer can do, the others as their lines read them.
s32 PresetSetting(Id id)
{
    switch (id)
    {
        case kAntiAliasing:
            return kMsaaValues[MsaaIndex()];
        case kTextureFilter:
            return kTextureFilterValues[TextureFilterIndex()];
        case kBloom:
            return Clamp(Screen(id), 0, kMaxBloom);
        default:
            return Screen(id) != 0;
    }
}

s32 PresetTarget(const PresetValue& value, s32 preset)
{
    return value.mId == kAntiAliasing ? kMsaaValues[MsaaIndex(value.mValue[preset])] : value.mValue[preset];
}

// Leaves out the settings added to ENHANCED, or changed in it, since setups were first made.
bool OtherPresetValuesEnhanced()
{
    for (s32 i = 0; i < kNumPresetValues; i++)
    {
        if (kPresetValues[i].mId != kAmbientOcclusion && kPresetValues[i].mId != kFireEffects && kPresetValues[i].mId != kBloom && PresetSetting(kPresetValues[i].mId) != PresetTarget(kPresetValues[i], kEnhanced))
        {
            return false;
        }
    }
    return true;
}

// The preset last chosen; a setup from before it was saved is ENHANCED where its GRAPHICS settings are.
s32 LastPreset()
{
    static bool sGuessing;
    s32 preset = gRegistry.Get_Screen_Setting("gl_preset", -1);
    if (preset < 0 && !sGuessing)
    {
        sGuessing = true;
        preset = OtherPresetValuesEnhanced() ? kEnhanced : kClassic;
        sGuessing = false;
        gRegistry.Set_Screen_Setting("gl_preset", preset);
    }
    return preset;
}

// A GRAPHICS setting never saved takes the last preset's value; -1 for the others.
s32 PresetDefault(Id id)
{
    for (s32 i = 0; i < kNumPresetValues; i++)
    {
        if (kPresetValues[i].mId == id)
        {
            const s32 preset = LastPreset();
            return preset < 0 ? -1 : PresetTarget(kPresetValues[i], preset);
        }
    }
    return -1;
}

Preset CurrentPreset()
{
    for (s32 preset = kClassic; preset <= kEnhanced; preset++)
    {
        s32 i = 0;
        while (i < kNumPresetValues && PresetSetting(kPresetValues[i].mId) == PresetTarget(kPresetValues[i], preset))
        {
            i++;
        }
        if (i == kNumPresetValues)
        {
            return static_cast<Preset>(preset);
        }
    }
    return kCustom;
}
#endif

// The zone name on screen is a copy of the text table's.
void RefreshZoneName()
{
    Hud_MapZone& zone = gHud_2B00->mZoneName;
    gmp_map_zone* pZone = zone.mLocalNavZone ? zone.mLocalNavZone : zone.mNavZone;
    wchar_t* pName = zone.mTimer && pZone ? pZone->get_zone_str() : NULL;
    if (pName)
    {
        wcsncpy(zone.mWstr, gText_0x14->StrToUpper(pName), 64);
        zone.mWstr[64] = 0;
    }
}

// The frontend copies its texts into its pages: set them up again, keeping the selected options.
void RefreshFrontend()
{
    const s32 numPages = sizeof(gFrontend->mMenuPagesArray) / sizeof(gFrontend->mMenuPagesArray[0]);
    u16 selected[numPages];
    for (s32 i = 0; i < numPages; i++)
    {
        selected[i] = gFrontend->mMenuPagesArray[i].mCurrentOptionIdx;
    }
    gFrontend->SetupMenuStringsOptionsElements();
    for (s32 i = 0; i < numPages; i++)
    {
        gFrontend->mMenuPagesArray[i].mCurrentOptionIdx = selected[i];
    }
}
} // namespace

const wchar_t* Translate(const wchar_t* pEnglish)
{
    const s32 lang = gText_0x14 ? TranslationLanguage(gText_0x14->mLangCode) : -1;
    if (lang < 0)
    {
        return pEnglish;
    }
    if (sCacheLang != gText_0x14->mLangCode)
    {
        sCacheLang = gText_0x14->mLangCode;
        for (s32 i = 0; i < kNumTranslations; i++)
        {
            const wchar_t* pText = kTranslations[i].mText[lang];
            wcsncpy(sCache[i], pText ? pText : kTranslations[i].mEnglish, 95);
            sCache[i][95] = 0;
            gText_0x14->RemapExtendedChars(sCache[i]);
        }
    }
    for (s32 i = 0; i < kNumTranslations; i++)
    {
        if (!wcscmp(kTranslations[i].mEnglish, pEnglish))
        {
            const char* pKey = kTranslations[i].mGameKey;
            if (pKey)
            {
                return gText_0x14->KeyExists(pKey) ? gText_0x14->Find(pKey) : pEnglish;
            }
            return sCache[i];
        }
    }
    return pEnglish;
}

bool IsGroup(Id id)
{
    return id > kCount;
}

const wchar_t* Name(Id id)
{
    return IsGroup(id) ? kGroupNames[id - kCount - 1] : kNames[id];
}

f32 HudScale()
{
    return HudScalePercent() / 100.0f;
}

f32 SafeArea()
{
    return SafeAreaPercent() / 100.0f;
}

Id Requirement(Id id)
{
#if defined(GTA2_PORT_SDL)
    if (id == kHeadlights || id == kLightEffects)
    {
        return kPerPixelLighting;
    }
#endif
    if (id == kDayLength || id == kStartTime || id == kClock)
    {
        return kLighting;
    }
    return id == kAimCamera || id == kStrafeAnimation || id == kCrosshair ? kControls : kCount;
}

bool Enabled(Id id)
{
    const Id requirement = Requirement(id);
    return requirement == kCount || (requirement == kLighting ? DayNightCycle() : Screen(requirement) != 0);
}

bool DayNightCycle()
{
    return Clamp(Screen(kLighting), 0, kNumLightings - 1) == 2;
}

s32 DayLength()
{
    return Clamp(Screen(kDayLength), kMinDayLength, kMaxDayLength);
}

s32 StartTime()
{
    return Clamp(Screen(kStartTime), 0, 23);
}

bool Clock()
{
    return DayNightCycle() && Screen(kClock) != 0;
}

bool TwinStick()
{
#if defined(GTA2_PORT_SDL)
    if (sTwinStick < 0)
    {
        sTwinStick = Screen(kControls) != 0;
    }
    return sTwinStick != 0;
#else
    return false;
#endif
}
bool AimCamera()
{
    return TwinStick() && Screen(kAimCamera) != 0;
}
bool StrafeAnimation()
{
    return TwinStick() && Screen(kStrafeAnimation) != 0;
}
bool Crosshair()
{
    return Screen(kCrosshair) != 0;
}
bool Vibration()
{
    return Screen(kVibration) != 0;
}

bool UpdateCheck()
{
    return Screen(kUpdateCheck) != 0;
}

bool ShowFps()
{
    return Screen(kShowFps) != 0;
}

bool Autosave()
{
    return Screen(kAutosave) != 0;
}

bool Retry()
{
    return Screen(kRetry) != 0;
}

bool KfRespawn()
{
    return Screen(kKfRespawn) != 0;
}

bool InfiniteLives()
{
    return Screen(kInfiniteLives) != 0;
}

void ApplyInfiniteLives(bool demo)
{
    gDo_infinite_lives = (InfiniteLives() && !demo) || gRegistry.Debug_Setting_Exists("do_infinite_lives");
}

const wchar_t* Help(Id id)
{
    switch (id)
    {
        case kAutosave:
            return L"AFTER EACH JOB OR FRENZY, AND WITH SAVE AND QUIT";
        case kRetry:
            return L"RETRY A FAILED JOB OR FRENZY FROM THE PAUSE MENU";
        case kExplodingScores:
            return L"POINTS BURST OUT WHERE THEY ARE SCORED";
        case k3dSound:
            return L"SOUNDS PLACED AROUND YOU, WITH THE ECHO OF THE PLACE";
        case kLighting:
        {
            static const wchar_t* const kHelp[] = {L"BROAD DAYLIGHT, WITHOUT LIGHTS", L"THE ORIGINAL'S EVENING LIGHT, STREET LIGHTS ON",
                                                   L"DAY, DUSK, NIGHT AND DAWN GO BY AS YOU PLAY"};
            return kHelp[Clamp(Screen(id), 0, kNumLightings - 1)];
        }
        case kDayLength:
            return L"REAL MINUTES FOR A WHOLE DAY AND NIGHT";
        case kStartTime:
            return L"THE HOUR A LEVEL STARTS AT";
        case kFrameLimit:
            return MaxFps() == 30 ? L"THE ORIGINAL'S PACE: A FRAME FOR EACH GAME STEP"
                                  : L"FRAMES BETWEEN THE GAME'S 30 STEPS A SECOND, INTERPOLATED";
        case kSafeArea:
            return L"THE HUD'S DISTANCE FROM THE SCREEN'S EDGES, IN PERCENT";
        case kControls:
            return TwinStick() ? L"ON FOOT: WALK, AND AIM WITH THE MOUSE OR RIGHT STICK"
                               : L"ON FOOT: THE ORIGINAL'S TANK CONTROLS";
        case kAimCamera:
            return Screen(id) ? L"THE VIEW LEADS TOWARD THE AIM. SHIFT, LT OR R3 LOOKS FURTHER"
                              : L"THE VIEW STAYS ON THE PLAYER";
        case kStrafeAnimation:
            return Screen(id) ? L"EXPERIMENTAL: THE LEGS FOLLOW YOUR MOVE, NOT YOUR AIM"
                              : L"THE ORIGINAL'S RUN, THE LEGS FACING THE AIM";
        case kCrosshair:
            return Screen(id) ? L"A RING SHOWS WHERE YOU AIM" : L"NO RING: THE PLAYER FACES WHERE YOU AIM";
        case kVibration:
            return L"THE PAD SHAKES WHEN YOU ARE HIT, IN EXPLOSIONS AND IN CRASHES";
        case kHeadlights:
            return L"BEAMS OF LIGHT AHEAD OF CARS AFTER DARK";
        case kLightEffects:
            return L"LIGHT FROM FIRES, EXPLOSIONS, GUNFIRE AND BRAKE LIGHTS";
        case kAmbientOcclusion:
            return L"SOFT SHADE IN CORNERS AND AT THE FOOT OF WALLS";
        case kFireEffects:
            return L"HEAT HAZE, SMOKE AND EMBERS AROUND FIRES";
#if defined(GTA2_PORT_SDL)
        case kPreset:
        {
            static const wchar_t* const kHelp[] = {L"THE ORIGINAL'S LOOK: EVERY OPTION BELOW OFF",
                                                   L"NEW LIGHTS, SOFT SHADOWS, FULL COLOURS, SMOOTHER EDGES",
                                                   L"YOUR OWN MIX OF THE OPTIONS BELOW"};
            return kHelp[CurrentPreset()];
        }
        case kPerPixelLighting:
            return L"LIGHTS WORKED OUT FOR EACH PIXEL, NOT AT TILE CORNERS";
        case kBloom:
            return L"A GLOW AROUND FIRES, EXPLOSIONS AND LIGHTS";
        case kTrueColour:
            return L"FULL COLOURS INSTEAD OF THE ORIGINAL'S 16-BIT TEXTURES";
        case kDisplayMode:
            return port_FullscreenModeIndex() ? L"FULL SCREEN IN THIS RESOLUTION AND REFRESH RATE"
                                              : L"FULL SCREEN AT THE DESKTOP'S RESOLUTION";
#endif
        case kKfRespawn:
            return L"A FAILED KILL FRENZY COMES BACK ON THE MAP";
        case kInfiniteLives:
            return L"YOU NEVER RUN OUT OF LIVES: NO GAME OVER";
        default:
            return NULL;
    }
}
// The menus ask every frame, and an open costs milliseconds on the 3DS's SD card: looked up once per code.
bool LanguageInstalled(char code)
{
    static char sInstalled[128];
    char& installed = sInstalled[code & 127];
    if (!installed)
    {
        char path[] = "data\\?.gxt";
        path[5] = code;
        FILE* pFile = fopen(path, "rb");
        if (pFile)
        {
            fclose(pFile);
        }
        installed = pFile ? 'y' : 'n';
    }
    return installed == 'y';
}
bool Headlights()
{
    return Enabled(kHeadlights) && Screen(kHeadlights) != 0;
}

bool LightEffects()
{
    return Enabled(kLightEffects) && Screen(kLightEffects) != 0;
}

bool AmbientOcclusion()
{
    return Screen(kAmbientOcclusion) != 0;
}

bool FireEffects()
{
    return Screen(kFireEffects) != 0;
}

// The original's max_frame_rate is not read: off, it ran the game as fast as the machine.
s32 MaxFps()
{
    const s32 fps = Screen(kFrameLimit);
    for (s32 i = 0; i < kNumMaxFpsValues; i++)
    {
        if (kMaxFpsValues[i] == fps)
        {
            return fps;
        }
    }
    return 30;
}

bool Range(Id id, s32* pMin, s32* pMax, s32* pValue)
{
    switch (id)
    {
        case kSfxVolume:
        case kMusicVolume:
            *pMin = 0;
            *pMax = 10;
            *pValue = VolumeStep(gRegistry.Get_Sound_Setting(id == kSfxVolume ? "SFXVol" : "CDVol", 127));
            return true;
        case kBrightness:
            *pMin = 1;
            *pMax = 20;
            *pValue = Clamp(gRegistry.Get_Screen_Setting("gamma", 10), 1, 20);
            return true;
        case kBloom:
            *pMin = 0;
            *pMax = kMaxBloom;
            *pValue = Clamp(Screen(id), 0, kMaxBloom);
            return true;
        case kZoom:
            *pMin = kMinZoom;
            *pMax = kMaxZoom;
            *pValue = Zoom();
            return true;
        case kHudScale:
            *pMin = kMinHudScale / 10;
            *pMax = kMaxHudScale / 10;
            *pValue = HudScalePercent() / 10;
            return true;
        case kSafeArea:
            *pMin = 0;
            *pMax = kMaxSafeArea;
            *pValue = SafeAreaPercent();
            return true;
        case kDayLength:
            *pMin = kMinDayLength;
            *pMax = kMaxDayLength;
            *pValue = DayLength();
            return true;
        case kStartTime:
            *pMin = 0;
            *pMax = 23;
            *pValue = StartTime();
            return true;
        default:
            return false;
    }
}

void ApplyRenderOptions()
{
#if defined(GTA2_PORT_SDL)
    port_SetRenderOptions(Screen(kTrueColour) != 0, Clamp(Screen(kBloom), 0, kMaxBloom), Screen(kPerPixelLighting) != 0,
                          kTextureFilterValues[TextureFilterIndex()], kMsaaValues[MsaaIndex()], Screen(kShadows) != 0);
#endif
}

void ApplyStyle(Style style)
{
    const bool enhanced = style == kEnhancedStyle;
#if defined(GTA2_PORT_SDL)
    for (s32 i = 0; i < kNumPresetValues; i++)
    {
        gRegistry.Set_Screen_Setting(ScreenName(kPresetValues[i].mId), PresetTarget(kPresetValues[i], enhanced ? kEnhanced : kClassic));
    }
    gRegistry.Set_Screen_Setting("gl_preset", enhanced ? kEnhanced : kClassic);
    ApplyRenderOptions();
    // The original's dusk, or the day-night cycle as its settings are.
    gRegistry.Set_Screen_Setting(ScreenName(kLighting), enhanced ? 2 : 1);
    sTwinStick = enhanced;
    gRegistry.Set_Screen_Setting(ScreenName(kControls), sTwinStick);
    gRegistry.Set_Screen_Setting(ScreenName(kAutosave), enhanced);
    gRegistry.Set_Screen_Setting(ScreenName(kRetry), enhanced);
    gRegistry.Set_Screen_Setting(ScreenName(kKfRespawn), enhanced);
    gRegistry.Set_Screen_Setting(ScreenName(kInfiniteLives), 0);
    // Unlimited with vsync: a frame each refresh of the screen. In a browser 60: drawing at 120 or 144 Hz loads its
    // compositor for little, with the game's 30 steps a second.
#if defined(__EMSCRIPTEN__)
    gRegistry.Set_Screen_Setting(ScreenName(kFrameLimit), 60);
#else
    gRegistry.Set_Screen_Setting(ScreenName(kFrameLimit), 0);
#endif
    gRegistry.Set_Screen_Setting(ScreenName(kVsync), 1);
    port_ApplyVsync();
    PortFrameLoop::ApplySettings();
#else
    (void)enhanced;
#endif
}

void Value(Id id, wchar_t* pOut)
{
    pOut[0] = 0;
    switch (id)
    {
        case kSfxVolume:
        case kMusicVolume:
            swprintf(pOut, L"%d", VolumeStep(gRegistry.Get_Sound_Setting(id == kSfxVolume ? "SFXVol" : "CDVol", 127)));
            break;
        case k3dSound:
            wcscpy(pOut, Translate(gRegistry.Sound_Setting_Exists("do_3d_sound") ? L"ON" : L"OFF"));
            break;
        case kTextSpeed:
            wcscpy(pOut, Translate(kTextSpeeds[Clamp(gRegistry.Get_Option_Int("text_speed", 3), 1, 5) - 1]));
            break;
        case kLanguage:
            wcscpy(pOut, kLanguages[LanguageIndex()].mName);
            break;
        case kFullScreen:
            wcscpy(pOut, Translate(gRegistry.Get_Screen_Setting("start_mode", 1) ? L"ON" : L"OFF"));
            break;
        case kBrightness:
            swprintf(pOut, L"%d", gRegistry.Get_Screen_Setting("gamma", 10));
            break;
        case kLighting:
            wcscpy(pOut, Translate(kLightings[Clamp(Screen(id), 0, kNumLightings - 1)]));
            break;
        case kDayLength:
            swprintf(pOut, L"%d MIN", DayLength());
            break;
        case kStartTime:
            swprintf(pOut, L"%d:00", StartTime());
            break;
        case kZoom:
            swprintf(pOut, L"%d", Zoom());
            break;
        case kHudScale:
            swprintf(pOut, L"%d", HudScalePercent());
            break;
        case kSafeArea:
            swprintf(pOut, L"%d", SafeAreaPercent());
            break;
        case kControls:
            wcscpy(pOut, Translate(TwinStick() ? L"TWIN STICK" : L"CLASSIC"));
            break;
        case kBloom:
            if (Screen(id))
            {
                swprintf(pOut, L"%d", Screen(id));
            }
            else
            {
                wcscpy(pOut, Translate(L"OFF"));
            }
            break;
        case kFrameLimit:
            if (MaxFps() == 30)
            {
                wcscpy(pOut, L"30 ");
                wcscpy(pOut + 3, Translate(L"ORIGINAL"));
            }
            else if (MaxFps())
            {
                swprintf(pOut, L"%d", MaxFps());
            }
            else
            {
                wcscpy(pOut, Translate(L"UNLIMITED"));
            }
            break;
#if defined(GTA2_PORT_SDL)
        case kTextureFilter:
            wcscpy(pOut, Translate(kTextureFilters[TextureFilterIndex()]));
            if (TextureFilterIndex() == 0)
            {
                wcscat(pOut, L" (");
                wcscat(pOut, Translate(L"ORIGINAL"));
                wcscat(pOut, L")");
            }
            break;
        case kShadows:
            wcscpy(pOut, Translate(Screen(id) ? L"SOFT" : L"ORIGINAL"));
            break;
        case kPreset:
            wcscpy(pOut, Translate(kPresetNames[CurrentPreset()]));
            break;
        case kAntiAliasing:
            if (MsaaIndex())
            {
                swprintf(pOut, L"%dX", kMsaaValues[MsaaIndex()]);
            }
            else
            {
                wcscpy(pOut, Translate(L"OFF"));
            }
            break;
        case kDisplayMode:
        {
            const int index = port_FullscreenModeIndex();
            int w;
            int h;
            int centiHz;
            port_FullscreenModeSize(index, &w, &h, &centiHz);
            if (!index)
            {
                wcscpy(pOut, Translate(L"DESKTOP"));
            }
            else
            {
                swprintf(pOut, L"%dX%d %dHZ", w, h, (centiHz + 50) / 100);
            }
            break;
        }
#endif
        default:
            wcscpy(pOut, Translate(Screen(id) ? L"ON" : L"OFF"));
            break;
    }
}

void Change(Id id, s32 delta)
{
    const bool inGame = port_GameRunning();
    switch (id)
    {
        case kSfxVolume:
        case kMusicVolume:
        {
            const char* name = id == kSfxVolume ? "SFXVol" : "CDVol";
            const s32 volume = Clamp(VolumeStep(gRegistry.Get_Sound_Setting(name, 127)) + delta, 0, 10) * 127 / 10;
            gRegistry.Set_Sound_Setting(name, volume);
            if (id == kSfxVolume)
            {
                gRoot_sound.SetSfxVol(static_cast<u8>(volume));
            }
            else
            {
                gRoot_sound.SetCDVol(static_cast<u8>(volume));
            }
            break;
        }
        case k3dSound:
            if (inGame)
            {
                // As the game does when it starts: the setting stays on only if a 3D provider opened.
                gRoot_sound.Set3DSound(!gRegistry.Sound_Setting_Exists("do_3d_sound"));
                gRegistry.Clear_Or_Delete_Sound_Setting("do_3d_sound", gRoot_sound.Get3DSound());
            }
            else
            {
                gRegistry.Clear_Or_Delete_Sound_Setting("do_3d_sound", !gRegistry.Sound_Setting_Exists("do_3d_sound"));
            }
            break;
        case kTextSpeed:
        {
            const DWORD speed = static_cast<DWORD>(Clamp(gRegistry.Get_Option_Int("text_speed", 3) + delta, 1, 5));
            SetOption("text_speed", REG_DWORD, &speed, sizeof(speed));
            if (inGame)
            {
                gHud_2B00->GetTextSpeed();
            }
            break;
        }
        case kLanguage:
        {
            s32 index = LanguageIndex();
            do
            {
                index = Wrap(index + delta, kNumLanguages);
            } while (!LanguageInstalled(kLanguages[index].mCode));
            const char code[2] = {kLanguages[index].mCode, 0};
            SetOption("language", REG_SZ, code, sizeof(code));
            delete gText_0x14;
            gText_0x14 = new TextTable();
            gText_0x14->Load();
            if (inGame)
            {
                RefreshZoneName();
                gHud_2B00->RecalcTextLayout();
            }
            else if (gFrontend)
            {
                RefreshFrontend();
            }
            break;
        }
        case kFullScreen:
            // As Alt+Enter: start_mode 1 is full screen, 0 a window.
            if (IsWindowedModeAvailable())
            {
                SetStartMode(gRegistry.Get_Screen_Setting("start_mode", 1) ? 0 : 1);
                SetVideoModeFromSettings();
                OnVideoModeChanged();
            }
            break;
        case kBrightness:
        {
            const s32 gamma = Clamp(gRegistry.Get_Screen_Setting("gamma", 10) + delta, 1, 20);
            gRegistry.Set_Screen_Setting("gamma", gamma);
            SetGamma(gamma);
            break;
        }
        case kBloom:
            gRegistry.Set_Screen_Setting(ScreenName(id), Clamp(Screen(id) + delta, 0, kMaxBloom));
            ApplyRenderOptions();
            break;
        case kHeadlights:
        case kLightEffects:
        case kClock:
        case kAimCamera:
        case kStrafeAnimation:
        case kCrosshair:
        case kVibration:
        case kShowFps:
        case kAutosave:
        case kRetry:
        case kKfRespawn:
        case kAmbientOcclusion:
        case kFireEffects:
            gRegistry.Set_Screen_Setting(ScreenName(id), Screen(id) ? 0 : 1);
            break;
        case kInfiniteLives:
            gRegistry.Set_Screen_Setting(ScreenName(id), Screen(id) ? 0 : 1);
            // In a game at once; a network game keeps its cleared debug flags, a replay those it was recorded with.
            if (port_GameRunning() && !IsNetworkGame() && !PortSaves::Replaying())
            {
                ApplyInfiniteLives(false);
            }
            break;
        case kFrameLimit:
        {
            s32 i = 0;
            while (i < kNumMaxFpsValues - 1 && kMaxFpsValues[i] != MaxFps())
            {
                i++;
            }
            gRegistry.Set_Screen_Setting(ScreenName(id), kMaxFpsValues[Wrap(i + delta, kNumMaxFpsValues)]);
            PortFrameLoop::ApplySettings();
            break;
        }
#if defined(GTA2_PORT_SDL)
        case kVsync:
            gRegistry.Set_Screen_Setting(ScreenName(id), Screen(id) ? 0 : 1);
            port_ApplyVsync();
            break;
        case kTextureFilter:
            gRegistry.Set_Screen_Setting(ScreenName(id), kTextureFilterValues[Wrap(TextureFilterIndex() + delta, kNumTextureFilters)]);
            ApplyRenderOptions();
            break;
        case kPreset:
        {
            // CLASSIC and ENHANCED in turn; from CUSTOM, left gives CLASSIC and right ENHANCED.
            const Preset current = CurrentPreset();
            const s32 preset = current == kCustom ? (delta < 0 ? kClassic : kEnhanced) : (current == kClassic ? kEnhanced : kClassic);
            for (s32 i = 0; i < kNumPresetValues; i++)
            {
                gRegistry.Set_Screen_Setting(ScreenName(kPresetValues[i].mId), PresetTarget(kPresetValues[i], preset));
            }
            gRegistry.Set_Screen_Setting("gl_preset", preset);
            ApplyRenderOptions();
            break;
        }
        case kAntiAliasing:
        {
            const s32 max = port_MaxMsaaSamples();
            s32 i = MsaaIndex();
            do
            {
                i = Wrap(i + delta, kNumMsaaValues);
            } while (kMsaaValues[i] > max);
            gRegistry.Set_Screen_Setting(ScreenName(id), kMsaaValues[i]);
            ApplyRenderOptions();
            break;
        }
        case kDisplayMode:
            port_SetFullscreenModeIndex(Wrap(port_FullscreenModeIndex() + delta, port_FullscreenModeCount()));
            if (gRegistry.Get_Screen_Setting("start_mode", 1))
            {
                SetVideoModeFromSettings();
                OnVideoModeChanged();
            }
            break;
#endif
        case kZoom:
            SetZoom(Zoom() + delta);
            gRegistry.Set_Screen_Setting(ScreenName(id), sZoom);
            break;
        case kHudScale:
            sHudScale = Clamp(HudScalePercent() + delta * 10, kMinHudScale, kMaxHudScale);
            gRegistry.Set_Screen_Setting(ScreenName(id), sHudScale);
            RefreshHud();
            break;
        case kSafeArea:
            sSafeArea = Clamp(SafeAreaPercent() + delta, 0, kMaxSafeArea);
            gRegistry.Set_Screen_Setting(ScreenName(id), sSafeArea);
            RefreshHud();
            break;
        case kControls:
            sTwinStick = !TwinStick();
            gRegistry.Set_Screen_Setting(ScreenName(id), sTwinStick);
            break;
        case kLighting:
            gRegistry.Set_Screen_Setting(ScreenName(id), Wrap(Clamp(Screen(id), 0, kNumLightings - 1) + delta, kNumLightings));
            Init_FrameRateLightingAndExplodingScores();
            break;
        case kDayLength:
            gRegistry.Set_Screen_Setting(ScreenName(id), Clamp(DayLength() + delta, kMinDayLength, kMaxDayLength));
            break;
        case kStartTime:
            gRegistry.Set_Screen_Setting(ScreenName(id), Clamp(StartTime() + delta, 0, 23));
            break;
        default:
            gRegistry.Set_Screen_Setting(ScreenName(id), Screen(id) ? 0 : 1);
            if (id == kExplodingScores || id == kBlood)
            {
                Init_FrameRateLightingAndExplodingScores();
            }
            else if (id != kUpdateCheck)
            {
                ApplyRenderOptions();
            }
            break;
    }
}
} // namespace PortSettings

Fix16 PortCameraZoom(Fix16 zoom)
{
    if (bStartNetworkGame)
    {
        return zoom; // one view for every player (and for the server's spawning, port/docs/netcode.md)
    }
    PortSettings::Zoom();
    return zoom * PortSettings::sZoomFactor;
}
