// port/net engine (port/docs/netcode.md, "Playing"): what a client shows over the frame: the connection's status (the ping,
// an outage), the round's panel (warmup, countdown, results, votes) and announcements.
#include "internal.hpp"
#include "dev/dev.hpp"
#include "Source/Camera.hpp"
#include "Source/Draw.hpp"
#include "Source/Frontend.hpp"
#include "Source/Game.hpp"
#include "Source/Player.hpp"
#include "Source/gtx_0x106C.hpp"
#include <algorithm>
#include <ctype.h>
#include <chrono>
#include <string>
#include <vector>

namespace
{
// The corner holds the round's line, GTA2_NET_STATS's and the ping's
wchar_t gStatusCorner[256], gStatusCentre[64];
wchar_t gBanner[64];
} // namespace

namespace
{
void Widen(const std::string& text, wchar_t* out, size_t size)
{
    size_t n = 0;
    for (; n < text.size() && n + 1 < size; n++)
    {
        out[n] = (wchar_t)(unsigned char)text[n];
    }
    out[n] = 0;
}
} // namespace

namespace
{
wchar_t gAnnouncement[128];
long long gAnnouncementUntilMs;
std::vector<std::vector<wchar_t>> gPanel; // SetNetPanel's lines, widened
struct ChatLine
{
    std::vector<wchar_t> text;
    long long untilMs;
};
std::vector<ChatLine> gChatLines; // AddChatLine's, the oldest first
long long NowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
} // namespace

// Over the frame for 4 s, as the netcode's other lines: the game's own message line is in the HUD, which the rollback
// restores to before the message (it showed for a frame)
void net::engine::ShowHudMessage(const char* text)
{
    Widen(text, gAnnouncement, sizeof(gAnnouncement) / sizeof(gAnnouncement[0]));
    gAnnouncementUntilMs = NowMs() + 4000;
}

void net::engine::AddChatLine(const std::string& line)
{
    const size_t kKept = 5;
    if (gChatLines.size() == kKept)
    {
        gChatLines.erase(gChatLines.begin());
    }
    ChatLine chat{std::vector<wchar_t>(line.size() + 1, 0), NowMs() + 10000};
    Widen(line, chat.text.data(), chat.text.size());
    gChatLines.push_back(chat);
}

void net::engine::SetNetPanel(const std::vector<std::string>& lines)
{
    gPanel.resize(lines.size());
    for (size_t i = 0; i < lines.size(); i++)
    {
        gPanel[i].assign(lines[i].size() + 1, 0);
        Widen(lines[i], gPanel[i].data(), gPanel[i].size());
    }
}

void net::engine::SetNetBanner(const std::string& text)
{
    // The message font has capitals only
    std::string upper(text);
    for (char& c : upper)
    {
        c = (char)toupper((unsigned char)c);
    }
    Widen(upper, gBanner, sizeof(gBanner) / sizeof(gBanner[0]));
}

void net::engine::SetNetStatus(const std::string& corner, const std::string& centre)
{
    Widen(corner, gStatusCorner, sizeof(gStatusCorner) / sizeof(gStatusCorner[0]));
    Widen(centre, gStatusCentre, sizeof(gStatusCentre) / sizeof(gStatusCentre[0]));
}

namespace
{
// <text> centred at <y>, wrapped at spaces to fit <w> less a margin; the y under it. No std::wstring, whose
// traits assume a 4-byte wchar_t (the port has -fshort-wchar)
s32 DrawCentredWrapped(const wchar_t* text, s32 w, s32 y, s32 lineHeight)
{
    std::vector<wchar_t> line(1, 0); // 0-terminated
    size_t lineEnd = 0; // the last fitting word's end
    for (const wchar_t* c = text;; c++)
    {
        if (*c == ' ' || !*c)
        {
            if (Frontend::GetMaxTextWidth(line.data(), gStyleTextFont) > w - 32 && lineEnd)
            {
                // over: draw up to the last word that fitted, start again after it
                std::vector<wchar_t> rest(line.begin() + lineEnd + 1, line.end());
                line.resize(lineEnd);
                line.push_back(0);
                DrawStringScaled(line.data(), (w - Frontend::GetMaxTextWidth(line.data(), gStyleTextFont)) / 2, y, gStyleTextFont);
                y += lineHeight;
                line = rest;
            }
            lineEnd = line.size() - 1;
            if (!*c)
            {
                break;
            }
        }
        line.insert(line.end() - 1, *c);
    }
    if (line[0])
    {
        DrawStringScaled(line.data(), (w - Frontend::GetMaxTextWidth(line.data(), gStyleTextFont)) / 2, y, gStyleTextFont);
        y += lineHeight;
    }
    return y;
}
bool IsTableRow(const std::vector<wchar_t>& line)
{
    return std::find(line.begin(), line.end(), (wchar_t)'\t') != line.end();
}

// A row's cells, split at its tabs (each 0-terminated)
std::vector<std::vector<wchar_t>> Cells(const std::vector<wchar_t>& line)
{
    std::vector<std::vector<wchar_t>> cells(1);
    for (wchar_t c : line)
    {
        if (c == (wchar_t)'\t' || !c)
        {
            cells.back().push_back(0);
            if (!c)
            {
                break;
            }
            cells.emplace_back();
        }
        else
        {
            cells.back().push_back(c);
        }
    }
    return cells;
}

bool Numeric(const std::vector<wchar_t>& cell)
{
    return cell[0] == (wchar_t)'-' ? !cell[1] : cell[0] >= (wchar_t)'0' && cell[0] <= (wchar_t)'9';
}

// gPanel's rows [first, end) as a table centred in <w>: the columns as wide as their widest cell, a column of numbers
// ("-": none) under its heading right-aligned, the others left-aligned (the font is proportional: spaces can't line them
// up); the y under it
s32 DrawTable(size_t first, size_t end, s32 w, s32 y, s32 lineHeight)
{
    const s32 kGap = 24;
    std::vector<std::vector<std::vector<wchar_t>>> rows;
    std::vector<s32> widths;
    for (size_t i = first; i < end; i++)
    {
        rows.push_back(Cells(gPanel[i]));
        for (size_t c = 0; c < rows.back().size(); c++)
        {
            const s32 cw = Frontend::GetMaxTextWidth(rows.back()[c].data(), gStyleTextFont);
            if (c >= widths.size())
            {
                widths.push_back(0);
            }
            widths[c] = cw > widths[c] ? cw : widths[c];
        }
    }
    std::vector<bool> right(widths.size(), true); // every cell under the first row numeric (or empty)
    for (size_t r = 1; r < rows.size(); r++)
    {
        for (size_t c = 0; c < rows[r].size(); c++)
        {
            right[c] = right[c] && (!rows[r][c][0] || Numeric(rows[r][c]));
        }
    }
    s32 total = 0;
    for (size_t c = 0; c < widths.size(); c++)
    {
        total += widths[c] + (c && widths[c] ? kGap : 0);
    }
    for (auto& row : rows)
    {
        s32 x = (w - total) / 2;
        for (size_t c = 0; c < row.size(); c++)
        {
            const s32 cw = Frontend::GetMaxTextWidth(row[c].data(), gStyleTextFont);
            DrawStringScaled(row[c].data(), right[c] ? x + widths[c] - cw : x, y, gStyleTextFont);
            x += widths[c] + (widths[c] ? kGap : 0);
        }
        y += lineHeight;
    }
    return y;
}
} // namespace

void net::engine::DrawOverlay()
{
    Camera* cam = gViewCamera;
    if (!cam)
    {
        return;
    }
    // In the HUD's 640x480 layout: DrawStringScaled scales it and keeps a text near an edge at the screen's edge
    const s32 w = 640, h = 480;
    // The corner's lines, right-aligned, the last one at the bottom
    s32 cornerY = h - 24;
    wchar_t* cornerEnd = gStatusCorner;
    while (*cornerEnd)
    {
        cornerEnd++;
    }
    for (wchar_t* end = cornerEnd; end > gStatusCorner; cornerY -= 16)
    {
        wchar_t* start = end;
        while (start > gStatusCorner && start[-1] != '\n')
        {
            start--;
        }
        std::vector<wchar_t> line(start, end);
        line.push_back(0);
        DrawStringScaled(line.data(), w - 8 - Frontend::GetMaxTextWidth(line.data(), gStyleTextFont), cornerY, gStyleTextFont);
        end = start > gStatusCorner ? start - 1 : start;
    }
    // The chat lines sent, left-aligned above the lines being typed (the game's, at the bottom: Hud_ChatInput)
    const long long nowMs = NowMs();
    gChatLines.erase(std::remove_if(gChatLines.begin(), gChatLines.end(), [nowMs](const ChatLine& c) { return c.untilMs <= nowMs; }),
                     gChatLines.end());
    const s32 chatLine = GetLineSpacingFromFontType(gStyleTextFont);
    s32 chatY = h - chatLine;
    for (Player* p = gGame_0x40 ? gGame_0x40->IterateFirstPlayer() : NULL; p; p = gGame_0x40->IterateNextPlayer())
    {
        chatY -= p->mIsChatting ? chatLine : 0;
    }
    for (size_t i = gChatLines.size(); i-- > 0; chatY -= chatLine)
    {
        DrawStringScaled(gChatLines[i].text.data(), 8, chatY, gStyleTextFont);
    }
    // The scoreboard instead of the panel, the announcement and the banner, which it would cover
    const bool board = DrawScoreboard();
    // The panel near the top (the HUD's score and wanted level are above it), the announcement under it
    const s32 kLine = 16;
    s32 panelY = 56;
    for (size_t i = 0; i < gPanel.size() && !board;)
    {
        if (IsTableRow(gPanel[i]))
        {
            size_t end = i;
            while (end < gPanel.size() && IsTableRow(gPanel[end]))
            {
                end++;
            }
            panelY = DrawTable(i, end, w, panelY, kLine);
            i = end;
            continue;
        }
        panelY = gPanel[i][0] ? DrawCentredWrapped(gPanel[i].data(), w, panelY, kLine) : panelY + kLine;
        i++;
    }
    if (gAnnouncement[0] && NowMs() < gAnnouncementUntilMs && !MenuOpen() && !board) // the pause menu would hide it
    {
        const s32 ay = gPanel.empty() ? h / 4 : (panelY + 8 > h / 4 ? panelY + 8 : h / 4);
        DrawCentredWrapped(gAnnouncement, w, ay, kLine);
    }
    if (gBanner[0] && !board)
    {
        const s32 bw = Frontend::GetMaxTextWidth(gBanner, gStyleMessageFont);
        DrawStringScaled(gBanner, (w - bw) / 2, h / 2 - 24, gStyleMessageFont);
    }
    if (gStatusCentre[0])
    {
        DrawStringScaled(gStatusCentre, (w - Frontend::GetMaxTextWidth(gStatusCentre, gStyleTextFont)) / 2, h / 3, gStyleTextFont);
    }
    DebugDrawMarker(cam); // GTA2_NET_MARKER (dev/checks.cpp)
}
