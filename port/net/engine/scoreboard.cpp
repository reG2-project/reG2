// port/net engine (port/docs/netcode.md, "Playing"): a client's scoreboard, over the frame while SCORES or the gamepad's
// Back is held, or as the pause menu's PLAYERS: the match on a translucent panel, then a row per player with its $ sign,
// its name in its HUD colour, its scores, the local player's ping and what it is doing.
#include "internal.hpp"
#include "Source/Camera.hpp"
#include "Source/Draw.hpp"
#include "Source/Frontend.hpp"
#include "Source/Game.hpp"
#include "Source/Player.hpp"
#include "Source/TagGame.hpp"
#include "Source/enums.hpp"
#include "Source/gbh_graphics.hpp"
#include "port/src/player_looks.hpp"
#include <memory>

bool port_ScoresHeld(); // compat/src/pad.cpp

namespace
{
std::unique_ptr<net::engine::Scoreboard> gBoard;

// In the HUD's 640x480 layout, centred on wide screens as one group (PortUiCentred)
const s32 kLeft = 40, kRight = 600, kTop = 96;
const s32 kRowStep = 18;
const s32 kHeadX = kLeft + 22, kNameX = kLeft + 34, kNameWidth = 150;
// The score's right edge, or the frags' when a points or tag game shows them after its score
const s32 kScoreRight = 450, kPointsRight = 370, kDeathsRight = 530, kPingRight = 588;
const u32 kPanelColour = 0xB0000000, kYouColour = 0x30FFFFFF;
const s32 kHalfAlpha = 16; // of 31 (CalcQuadFlags)
// The text font's '2' has blank columns on its left: drawn this much further left, it is spaced as the other digits
const s32 kTwoTrim = 4;

void Widen(const std::string& text, wchar_t* out, size_t size)
{
    size_t n = 0;
    for (; n < text.size() && n + 1 < size; n++)
    {
        out[n] = (wchar_t)(unsigned char)text[n];
    }
    out[n] = 0;
}

s32 Width(const wchar_t* text)
{
    s32 twos = 0;
    for (size_t i = 0; text[i]; i++)
    {
        twos += text[i] == L'2';
    }
    return Frontend::GetMaxTextWidth(const_cast<wchar_t*>(text), gStyleTextFont) - twos * kTwoTrim;
}

// <text> cut until it fits <w> (no wcslen: the port has -fshort-wchar)
void Fit(wchar_t* text, s32 w)
{
    size_t n = 0;
    while (text[n])
    {
        n++;
    }
    for (; n > 0 && Width(text) > w; n--)
    {
        text[n - 1] = 0;
    }
}

// <text> at x, y of the layout, placed as the layout's <anchor> x is (PortUiX: the whole line keeps one anchor), in a
// font remap (-1: none), each '2' pulled left by kTwoTrim
void Draw(const wchar_t* text, s32 anchor, s32 x, s32 y, s32 remap = -1, s32 alpha = 0)
{
    const Fix16 scale = gViewCamera->mUiScale;
    const Fix16 left = PortUiX(Fix16(anchor)) + Fix16(x - anchor) * scale;
    const Fix16 top = PortUiY(Fix16(y));
    const s32 type = remap >= 0 ? palette_types_enum::font_remaps_8 : palette_types_enum::sprites_2;
    wchar_t piece[96];
    size_t n = 0;
    s32 pieceX = 0;
    for (size_t i = 0;; i++)
    {
        if (n && (!text[i] || text[i] == L'2' || n + 1 == sizeof(piece) / sizeof(piece[0])))
        {
            piece[n] = 0;
            DrawString(piece, left + Fix16(pieceX) * scale, top, gStyleTextFont, scale, type, remap >= 0 ? (u16)remap : 0, alpha ? 1 : 0,
                       (u8)alpha);
            pieceX += Width(piece) + (piece[0] == L'2' ? kTwoTrim : 0);
            n = 0;
        }
        if (!text[i])
        {
            break;
        }
        pieceX -= n == 0 && text[i] == L'2' ? kTwoTrim : 0;
        piece[n++] = text[i];
    }
}

void Text(const std::string& text, s32 x, s32 y)
{
    wchar_t w[96];
    Widen(text, w, sizeof(w) / sizeof(w[0]));
    Draw(w, x, x, y);
}

void RightText(const std::string& text, s32 right, s32 y)
{
    wchar_t w[32];
    Widen(text, w, sizeof(w) / sizeof(w[0]));
    Draw(w, right, right - Width(w), y);
}

void CentredText(const std::string& text, s32 y)
{
    wchar_t w[96];
    Widen(text, w, sizeof(w) / sizeof(w[0]));
    Fit(w, kRight - kLeft - 16);
    const s32 x = (kLeft + kRight - Width(w)) / 2;
    Draw(w, x, x, y);
}

// A translucent rectangle of the layout
void Fill(s32 left, s32 top, s32 right, s32 bottom, u32 colour)
{
    const f32 x0 = PortUiX(Fix16(left)).ToFloat(), x1 = PortUiX(Fix16(right)).ToFloat();
    const f32 y0 = PortUiY(Fix16(top)).ToFloat(), y1 = PortUiY(Fix16(bottom)).ToFloat();
    Vert verts[8] = {};
    verts[0].x = verts[3].x = x0;
    verts[1].x = verts[2].x = x1;
    verts[0].y = verts[1].y = y0;
    verts[2].y = verts[3].y = y1;
    for (s32 i = 0; i < 4; i++)
    {
        verts[i].z = 0.0001f; // as the HUD's sprites
        verts[i].diff = colour;
    }
    const u32 kBlendAlpha = 0x100, kVertexDiffuse = 0x2000; // gbh_api.cpp's FLAG_BLEND_ALPHA, FLAG_VERTEX_DIFFUSE
    pgbh_DrawQuad(kBlendAlpha | kVertexDiffuse, NULL, verts, 255);
}

// A player's name colour: its HUD colour's font remap, but white for the purple one, too dark to read on the panel
s32 NameRemap(int slot)
{
    Player* p = net::engine::GetPlayer(slot);
    const s32 remap = p ? PortFontRemap(p->mHudPaletteType, p->mHudPalette) : -1;
    const s32 kDarkPurple = 1;
    return remap == kDarkPurple ? -1 : remap;
}

// The player's $ sign in its HUD colour and its name, half transparent while dead; the name's right end
s32 Name(const net::engine::PlayerRow& row, s32 y)
{
    wchar_t w[24];
    Widen(row.name, w, sizeof(w) / sizeof(w[0]));
    Fit(w, kNameWidth);
    Draw(w, kNameX, kNameX, y, NameRemap(row.slot), row.dead ? kHalfAlpha : 0);
    if (Player* p = net::engine::GetPlayer(row.slot))
    {
        DrawFigureScaled(sprite_types_enum::user_6, 16, kHeadX, y + 9, Ang16(0), p->mHudPaletteType, p->mHudPalette, row.dead ? 1 : 0,
                         row.dead ? kHalfAlpha : 0);
    }
    return kNameX + Width(w);
}

const char* Status(const net::engine::PlayerRow& row)
{
    Player* p = net::engine::GetPlayer(row.slot);
    if (row.chatting)
    {
        return "chat";
    }
    if (p && gTagGame.IsTagGame() && gTagGame.mFugitive == p)
    {
        return "IT";
    }
    return row.dead ? "dead" : row.ready ? "ready" : "";
}
} // namespace

void net::engine::SetScoreboard(const Scoreboard* pBoard)
{
    if (!pBoard)
    {
        gBoard.reset();
        return;
    }
    if (!gBoard)
    {
        gBoard.reset(new Scoreboard());
    }
    *gBoard = *pBoard;
}

bool net::engine::ScoresHeld()
{
    return port_ScoresHeld();
}

bool net::engine::DrawScoreboard()
{
    if (!gBoard)
    {
        return false;
    }
    const Scoreboard& b = *gBoard;
    PortUiCentred centred;
    const s32 headingsY = kTop + 60, rowsY = headingsY + kRowStep;
    const s32 hintY = rowsY + (s32)b.rows.size() * kRowStep + 4;
    const s32 bottom = b.hint.empty() ? hintY + 2 : hintY + kRowStep + 2;
    Fill(kLeft, kTop, kRight, bottom, kPanelColour);
    CentredText(b.title, kTop + 6);
    CentredText(b.lines[0], kTop + 24);
    CentredText(b.lines[1], kTop + 40);
    const s32 scoreRight = b.frags ? kPointsRight : kScoreRight;
    Text("Player", kNameX, headingsY);
    RightText(b.scoreHeading, scoreRight, headingsY);
    if (b.frags)
    {
        RightText("Frags", kScoreRight, headingsY);
    }
    RightText("Deaths", kDeathsRight, headingsY);
    RightText("Ping", kPingRight, headingsY);
    for (size_t i = 0; i < b.rows.size(); i++)
    {
        const PlayerRow& row = b.rows[i];
        const s32 y = rowsY + (s32)i * kRowStep;
        if (row.you)
        {
            Fill(kLeft + 4, y - 1, kRight - 4, y + kRowStep - 1, kYouColour);
        }
        const s32 nameRight = Name(row, y);
        Text(Status(row), nameRight + 10, y);
        RightText(std::to_string(row.score), scoreRight, y);
        if (b.frags)
        {
            RightText(std::to_string(row.frags), kScoreRight, y);
        }
        RightText(std::to_string(row.deaths), kDeathsRight, y);
        RightText(row.ping >= 0 ? std::to_string(row.ping) : std::string("-"), kPingRight, y);
    }
    if (!b.hint.empty())
    {
        CentredText(b.hint, hintY);
    }
    return true;
}
