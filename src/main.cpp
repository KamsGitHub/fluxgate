#include <sys/signalfd.h>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <exception>
#include <string>
#include <string_view>
#include <system_error>

#include "fluxgate/config/config.hpp"
#include "fluxgate/log/log.hpp"
#include "fluxgate/net/file_descriptor.hpp"
#include "fluxgate/proxy/server.hpp"

namespace {

constexpr int kExitRuntimeFailure = 1;
constexpr int kExitBadConfiguration = 2;

constexpr const char* kUsage =
    "usage: fluxgate --config <file> [--check-config]\n"
    "       fluxgate --version\n";

// Turns SIGINT and SIGTERM into a readable descriptor, so shutdown is observed by the
// ordinary poll loop and no application code ever runs in signal-handler context.
fluxgate::net::FileDescriptor make_stop_descriptor() {
  sigset_t signals;
  ::sigemptyset(&signals);
  ::sigaddset(&signals, SIGINT);
  ::sigaddset(&signals, SIGTERM);
  if (::pthread_sigmask(SIG_BLOCK, &signals, nullptr) != 0) {
    throw std::system_error(errno, std::generic_category(), "pthread_sigmask");
  }
  fluxgate::net::FileDescriptor fd(::signalfd(-1, &signals, SFD_CLOEXEC | SFD_NONBLOCK));
  if (!fd) {
    throw std::system_error(errno, std::generic_category(), "signalfd");
  }
  return fd;
}

}  // namespace

int main(int argc, char** argv) {
  std::string config_path;
  bool check_only = false;
  for (int i = 1; i < argc; ++i) {
    const std::string_view argument = argv[i];
    if (argument == "--version") {
      std::printf("fluxgate %s\n", FLUXGATE_VERSION);
      return 0;
    }
    if (argument == "--check-config") {
      check_only = true;
    } else if (argument == "--config" && i + 1 < argc) {
      config_path = argv[++i];
    } else {
      std::fputs(kUsage, stderr);
      return kExitBadConfiguration;
    }
  }
  if (config_path.empty()) {
    std::fputs(kUsage, stderr);
    return kExitBadConfiguration;
  }

  fluxgate::config::Config config;
  try {
    // Fully validated before any socket is opened.
    config = fluxgate::config::load_config(config_path);
  } catch (const fluxgate::config::ConfigError& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return kExitBadConfiguration;
  }
  if (check_only) {
    std::puts("configuration is valid");
    return 0;
  }

  try {
    fluxgate::log::init(config.log_level);
    ::signal(SIGPIPE, SIG_IGN);  // Log output to a closed pipe must not kill the proxy.
    const auto stop_fd = make_stop_descriptor();
    fluxgate::proxy::Server server(config, stop_fd.get());
    server.run();
    fluxgate::log::flush();
    return 0;
  } catch (const std::exception& error) {
    fluxgate::log::error("server.fatal", {{"error", error.what()}});
    fluxgate::log::flush();
    return kExitRuntimeFailure;
  }
}
