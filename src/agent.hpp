#pragma once

#include "llm_client.hpp"
#include <boost/asio/awaitable.hpp>
#include <boost/json.hpp>
#include <vector>

namespace agent {

class Agent {
public:
  Agent(LLMConfig config);

  // Run the interactive agent loop
  boost::asio::awaitable<void> run();

private:
  LLMConfig config_;
  std::vector<boost::json::value> messages_;
  std::string system_prompt_;

  boost::asio::awaitable<void> run_agentic_loop();

  // Translators for Gemini/OpenAI compatibility
  boost::json::object build_anthropic_payload();
  boost::json::object build_openai_payload();

  // Translates an OpenAI response back to Anthropic structure for consistent
  // internal handling
  boost::json::object
  normalize_openai_response(const boost::json::object &raw_resp);
};

} // namespace agent
