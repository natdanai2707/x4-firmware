#pragma once

#include <cerrno>
#include <cstdlib>

namespace crosspoint_simulator {

inline int httpPort() {
  constexpr int DEFAULT_HTTP_PORT = 8080;
  constexpr int MIN_UNPRIVILEGED_PORT = 1024;
  constexpr int MAX_HTTP_PORT = 65534;

  const char *configured = std::getenv("CROSSPOINT_SIM_HTTP_PORT");
  if (!configured || !*configured)
    return DEFAULT_HTTP_PORT;

  errno = 0;
  char *end = nullptr;
  const long parsed = std::strtol(configured, &end, 10);
  if (errno != 0 || end == configured || *end != '\0' ||
      parsed < MIN_UNPRIVILEGED_PORT || parsed > MAX_HTTP_PORT) {
    return DEFAULT_HTTP_PORT;
  }
  return static_cast<int>(parsed);
}

inline int mapFirmwarePort(int firmwarePort) {
  if (firmwarePort == 80)
    return httpPort();
  if (firmwarePort == 81)
    return httpPort() + 1;
  return firmwarePort;
}

} // namespace crosspoint_simulator
