#pragma once

// The embedded HTML page served by the daemon's WebServer at GET / (T9 fills
// this in with the full capture/playback client). A raw string so the flake
// build needs no install step.

#include <string>

namespace persona {

inline const std::string kPageHtml = R"html(<!DOCTYPE html>
<html>
<head><meta charset="utf-8"><title>persona</title></head>
<body>
<p>persona web client — page under construction.</p>
</body>
</html>
)html";

}  // namespace persona
