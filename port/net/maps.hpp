// port/net (port/docs/netcode.md, "Running"): the multiplayer maps (data/<name>.mmp) and a server's map lists.
#pragma once
#include "engine/engine.hpp"
#include "session/server.hpp"
#include <string>

namespace net
{
// A server's maps: the first (<map>, or the rotation's first) into sc.game, the rotation (<maps>, comma-separated) and
// what a vote chooses from (the rotation, else the multiplayer maps the game ships); sc.game.players in: --players
// (0: as many as the biggest map takes); false: an error, printed
bool SetUpMaps(const std::string& map, const std::string& maps, ServerConfig& sc);
// The name players know a map by: data/<name>.mmp's Description without its " 6player" ("Face Off" for mp5-6p), as
// the original lobby listed them; <name> without one
std::string MapTitle(const std::string& name);
} // namespace net
