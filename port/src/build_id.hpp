// The build's identity: the version and the git commit it was built from ("0.1.0-dev 1a2b3c4d5e"; the version alone
// without git). The log's first line, and what a network game's server and client compare (port/net/session/server.cpp).
#pragma once

const char* port_BuildId();
