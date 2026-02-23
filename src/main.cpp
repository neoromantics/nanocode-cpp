#include "agent.hpp"
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>
#include <cstdlib>
#include <iostream>

int main(int argc, char **argv) {
  (void)argc;
  (void)argv;

  LLMConfig config;

  if (const char *gemini = std::getenv("GEMINI_API_KEY")) {
    config.api_key = gemini;
    config.model =
        std::getenv("MODEL") ? std::getenv("MODEL") : "gemini-2.5-flash";
    // Convert gemini endpoint. The v1beta/openai is an OpenAI compatible
    // endpoint provided by Google! We will build payload using our OpenAI
    // payload builder.
    config.api_url = "https://generativelanguage.googleapis.com/v1beta/openai/"
                     "chat/completions";
    config.is_anthropic_format = false;
    config.is_openai_format = true;
  } else if (const char *openrouter = std::getenv("OPENROUTER_API_KEY")) {
    config.api_key = openrouter;
    config.model = std::getenv("MODEL") ? std::getenv("MODEL")
                                        : "anthropic/claude-3-7-sonnet";
    config.api_url = "https://openrouter.ai/api/v1/messages";
    config.is_anthropic_format = true;
    config.is_openai_format = false;
  } else if (const char *anthropic = std::getenv("ANTHROPIC_API_KEY")) {
    config.api_key = anthropic;
    config.model = std::getenv("MODEL") ? std::getenv("MODEL")
                                        : "claude-3-7-sonnet-20250219";
    config.api_url = "https://api.anthropic.com/v1/messages";
    config.is_anthropic_format = true;
    config.is_openai_format = false;
  } else {
    std::cerr << "Error: Must set GEMINI_API_KEY, OPENROUTER_API_KEY, or "
                 "ANTHROPIC_API_KEY in environment.\n";
    return EXIT_FAILURE;
  }

  try {
    boost::asio::io_context ioc;

    // Catch signals to gracefully exit
    boost::asio::signal_set signals(ioc, SIGINT, SIGTERM);
    signals.async_wait([&](auto, auto) { ioc.stop(); });

    agent::Agent agent(config);

    // Spawn the agent coroutine
    boost::asio::co_spawn(ioc, agent.run(), boost::asio::detached);

    // Run the I/O context to execute the coroutines
    ioc.run();

  } catch (const std::exception &e) {
    std::cerr << "\nException: " << e.what() << "\n";
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
