#include "config.h"

#include <portaudio.h>

#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace persona {

// persona devices — enumerate PortAudio devices (T6). Prints a table of every
// device with index, channel counts, default sample rate, and default
// input/output markers. On PA init failure: clear stderr message + exit 1
// (thrown; main() turns exceptions into "persona: <msg>" + exit 1).

namespace {

void fail_pa(const char* where, PaError err) {
    throw std::runtime_error(std::string(where) + ": " + Pa_GetErrorText(err));
}

std::string pad(const std::string& s, size_t width) {
    return s.size() >= width ? s : s + std::string(width - s.size(), ' ');
}

}  // namespace

int verb_devices(const Config&, const std::vector<std::string>& args) {
    if (!args.empty()) {
        std::cerr << "usage: persona devices\n";
        return 1;
    }

    const PaError init_err = Pa_Initialize();
    if (init_err != paNoError) {
        fail_pa("portaudio: Pa_Initialize failed", init_err);
    }

    const int count = static_cast<int>(Pa_GetDeviceCount());
    const PaDeviceIndex default_input = Pa_GetDefaultInputDevice();
    const PaDeviceIndex default_output = Pa_GetDefaultOutputDevice();

    std::cout << "Audio devices (" << count << "):\n";
    std::cout << "  " << pad("idx", 4) << pad("in", 4) << pad("out", 4)
              << pad("rate", 9) << "name\n";
    for (int i = 0; i < count; ++i) {
        const PaDeviceInfo* info = Pa_GetDeviceInfo(i);
        if (info == nullptr) {
            continue;
        }
        std::string name = info->name;
        if (i == default_input) {
            name += "  [default input]";
        }
        if (i == default_output) {
            name += "  [default output]";
        }
        std::cout << "  " << pad(std::to_string(i), 4)
                  << pad(std::to_string(info->maxInputChannels), 4)
                  << pad(std::to_string(info->maxOutputChannels), 4)
                  << pad(std::to_string(static_cast<int>(info->defaultSampleRate)), 9)
                  << name << "\n";
    }

    Pa_Terminate();
    return 0;
}

}  // namespace persona