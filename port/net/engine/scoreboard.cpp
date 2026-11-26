// port/net engine (port/docs/netcode.md, "Playing"): a client's scoreboard, over the frame while SCORES or the gamepad's
// Back is held, or as the pause menu's PLAYERS: the match on a translucent panel, then a row per player with its name in
// its HUD colour, its scores, its ping and what it is doing.
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
#include <algorithm>
#include <memory>

bool port_ScoresHeld(); // compat/src/pad.cpp

namespace
{
std::unique_ptr<net::engine::Scoreboard> gBoard;
std::vector<net::engine::PlayerRow> gStandings;
int gStandingsType;

// In the HUD's 640x480 layout, centred on wide screens as one group (PortUiCentred)
const s32 kLeft = 40, kRight = 600, kTop = 96;
const s32 kRowStep = 18;
const s32 kNameX = kLeft + 12, kNameWidth = 150;
// The columns, right aligned, laid out from the ping's right edge leftwards; the names get what is left of them
const s32 kPingRight = 588, kColumnGap = 14;
const u32 kPanelColour = 0xB0000000, kYouColour = 0x30FFFFFF;
const s32 kHalfAlpha = 16; // of 31 (CalcQuadFlags)
// The text font's '2' has blank columns on its left: drawn this much further left, it is spaced as the other digits
const s32 kTwoTrim = 4;
// A line's capitals span y + 6 to y + 17 (the font's glyphs have blank rows on top); its descenders reach y + 22
const s32 kTextMiddle = 12;
// The HUD's top left, where the original listed every other player
const s32 kStandingsX = 8, kStandingsY = 6;

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

s32 Width(const std::string& text)
{
    wchar_t w[96];
    Widen(text, w, sizeof(w) / sizeof(w[0]));
    return Width(w);
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

// A player's name colour: its HUD colour's font remap
s32 NameRemap(int slot)
{
    Player* p = net::engine::GetPlayer(slot);
    return p ? PortFontRemap(p->mHudPaletteType, p->mHudPalette) : -1;
}

const s32 kSquareGap = 6, kSquareSize = 8;

// The port's cyan look, which no font remap has
bool Cyan(int slot)
{
    Player* p = net::engine::GetPlayer(slot);
    const s16 kCyan = 9; // PortLookOf's Elvis
    return p && p->mHudPaletteType == palette_types_enum::user_remaps_7 && p->mHudPalette == kCyan;
}

// The player's name in its HUD colour, cut to <width>, half transparent while dead, then a square in its colour when no
// font remap has it; where the name's line goes on
s32 Name(const net::engine::PlayerRow& row, s32 y, s32 width)
{
    wchar_t w[24];
    Widen(row.name, w, sizeof(w) / sizeof(w[0]));
    Fit(w, width);
    Draw(w, kNameX, kNameX, y, NameRemap(row.slot), row.dead ? kHalfAlpha : 0);
    const s32 right = kNameX + Width(w);
    if (Cyan(row.slot))
    {
        const s32 x = right + kSquareGap, size = kSquareSize;
        Fill(x, y + kTextMiddle - size / 2, x + size, y + kTextMiddle + size / 2, row.dead ? 0x808CFFFF : 0xFF8CFFFF);
        return x + size;
    }
    return right;
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

std::string Ordinal(int n)
{
    const int tens = n % 100, units = n % 10;
    const char* suffix = tens >= 11 && tens <= 13 ? "th" : units == 1 ? "st" : units == 2 ? "nd" : units == 3 ? "rd" : "th";
    return std::to_string(n) + suffix;
}

std::string Clock(int seconds)
{
    char clock[16];
    snprintf(clock, sizeof(clock), "%d:%02d", seconds / 60, seconds % 60);
    return clock;
}

// The local player's score on the HUD's line: its frags or the time "it" (a points game's is the game's money)
std::string Score(int score, int gameType)
{
    return gameType == 3 ? Clock(score) + " it" : std::to_string(score) + (score == 1 ? " frag" : " frags");
}

std::string Money(int money)
{
    return "$" + std::to_string(money);
}

// A right aligned column of the board: its heading, a cell per row, its right edge
struct Column
{
    std::string heading;
    bool money; // "$" sums
    std::vector<std::string> cells;
    s32 right;
};
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
    // Without a money column, the score is the money (a points game: client.cpp)
    std::vector<Column> columns(1, Column{b.scoreHeading, !b.money, {}, 0});
    if (b.money)
    {
        columns.push_back(Column{"Money", true, {}, 0});
    }
    if (b.frags)
    {
        columns.push_back(Column{"Frags", false, {}, 0});
    }
    columns.push_back(Column{"Deaths", false, {}, 0});
    columns.push_back(Column{"Ping", false, {}, 0});
    for (const PlayerRow& row : b.rows)
    {
        size_t c = 0;
        columns[c++].cells.push_back(!b.money ? Money(row.score) : b.clock ? Clock(row.score) : std::to_string(row.score));
        if (b.money)
        {
            columns[c++].cells.push_back(Money(row.money));
        }
        if (b.frags)
        {
            columns[c++].cells.push_back(std::to_string(row.frags));
        }
        columns[c++].cells.push_back(std::to_string(row.deaths));
        columns[c++].cells.push_back(row.ping >= 0 ? std::to_string(row.ping) : std::string(row.ping == -2 ? "BOT" : "-"));
    }
    // Wide enough for $99999 and the like: a column only moves for bigger sums
    const s32 minMoney = Width(std::string("$99999"));
    s32 right = kPingRight;
    for (size_t c = columns.size(); c-- > 0;)
    {
        Column& col = columns[c];
        s32 w = Width(col.heading);
        if (col.money)
        {
            w = std::max(w, minMoney);
        }
        for (const std::string& cell : col.cells)
        {
            w = std::max(w, Width(cell));
        }
        col.right = right;
        right -= w + kColumnGap;
    }
    const s32 nameSpace = right - kNameX;
    Text("Player", kNameX, headingsY);
    for (const Column& col : columns)
    {
        RightText(col.heading, col.right, headingsY);
    }
    for (size_t i = 0; i < b.rows.size(); i++)
    {
        const PlayerRow& row = b.rows[i];
        const s32 y = rowsY + (s32)i * kRowStep;
        if (row.you)
        {
            Fill(kLeft + 4, y + kTextMiddle - kRowStep / 2, kRight - 4, y + kTextMiddle + kRowStep / 2, kYouColour);
        }
        const std::string status = Status(row);
        const s32 after = (status.empty() ? 0 : 10 + Width(status)) + (Cyan(row.slot) ? kSquareGap + kSquareSize : 0);
        const s32 nameRight = Name(row, y, std::min(kNameWidth, nameSpace - after));
        Text(status, nameRight + 10, y);
        for (const Column& col : columns)
        {
            RightText(col.cells[i], col.right, y);
        }
    }
    if (!b.hint.empty())
    {
        CentredText(b.hint, hintY);
    }
    return true;
}

void net::engine::SetStandings(const std::vector<PlayerRow>& rows, int gameType)
{
    gStandings = rows;
    gStandingsType = gameType;
}

bool net::engine::DrawStandings()
{
    size_t you = 0;
    while (you < gStandings.size() && !gStandings[you].you)
    {
        you++;
    }
    if (you == gStandings.size())
    {
        return false; // not in the game yet: the original's list
    }
    int rank = 1;
    for (const PlayerRow& row : gStandings)
    {
        rank += row.score > gStandings[you].score;
    }
    const std::string line = Ordinal(rank) + " / " + std::to_string(gStandings.size());
    // Short: a kill's notice shows in the middle of the same line. A points game's score is the game's money top right
    Text(gStandingsType == 2 ? line : line + "  " + Score(gStandings[you].score, gStandingsType), kStandingsX, kStandingsY);
    return true;
}
