#include "agent.hpp"
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>
#include <cstdlib>
#include <iostream>

#include <fstream>
#include <string>

void load_env_file(const std::string &path) {
  std::ifstream file(path);
  if (!file)
    return;
  std::string line;
  while (std::getline(file, line)) {
    if (line.empty() || line[0] == '#')
      continue;
    auto pos = line.find('=');
    if (pos != std::string::npos) {
      std::string key = line.substr(0, pos);
      std::string val = line.substr(pos + 1);
      if (val.size() >= 2 && val.front() == '"' && val.back() == '"') {
        val = val.substr(1, val.size() - 2);
      }
      // 0 -> don't overwrite if it already exists in the environment
      setenv(key.c_str(), val.c_str(), 0);
    }
  }
}

int main(int argc, char **argv) {
  if (const char *home = std::getenv("HOME")) {
    load_env_file(std::string(home) + "/.nanocoderc");
  }
  load_env_file(".nanocoderc");
  load_env_file(".env");

  std::string cli_model;
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--model" && i + 1 < argc) {
      cli_model = argv[++i];
    }
  }

  const char *gemini = std::getenv("GEMINI_API_KEY");
  const char *anthropic = std::getenv("ANTHROPIC_API_KEY");
  const char *openrouter = std::getenv("OPENROUTER_API_KEY");

  std::string initial_model = "claude-3-7-sonnet-20250219";
  if (!cli_model.empty()) {
    initial_model = cli_model;
  } else if (std::getenv("MODEL")) {
    initial_model = std::getenv("MODEL");
  } else if (openrouter) {
    initial_model = "anthropic/claude-3-7-sonnet";
  } else if (gemini && !anthropic) {
    initial_model = "gemini-2.5-flash";
  }

  if (!gemini && !anthropic && !openrouter) {
    std::cerr << "Error: Must set GEMINI_API_KEY, OPENROUTER_API_KEY, or "
                 "ANTHROPIC_API_KEY in environment.\n";
    return EXIT_FAILURE;
  }

  agent::AgentConfig config;
  if (gemini)
    config.gemini_key = gemini;
  if (anthropic)
    config.anthropic_key = anthropic;
  if (openrouter)
    config.openrouter_key = openrouter;
  config.initial_model = initial_model;

  try {
    boost::asio::io_context ioc;

    // Catch signals to gracefully exit
    boost::asio::signal_set signals(ioc, SIGINT, SIGTERM);
    signals.async_wait([&](auto, auto) { ioc.stop(); });

    agent::Agent agent(config);

    // Spawn the agent coroutine
    boost::asio::co_spawn(ioc, agent.run(), [&](std::exception_ptr e) {
      if (e) {
        try {
          std::rethrow_exception(e);
        } catch (const std::exception &ex) {
          std::cerr << "Agent crash: " << ex.what() << "\n";
        }
      }
      ioc.stop();
    });

    // Run the I/O context to execute the coroutines
    ioc.run();

  } catch (const std::exception &e) {
    std::cerr << "\nException: " << e.what() << "\n";
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
