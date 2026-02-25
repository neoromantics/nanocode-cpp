#pragma once

#include "llm_client.hpp"
#include <boost/asio/awaitable.hpp>
#include <boost/json.hpp>
#include <string>

namespace agent {

struct AgentConfig {
  std::string gemini_key;
  std::string anthropic_key;
  std::string openrouter_key;
  std::string initial_model;
};

class Agent {
public:
  Agent(AgentConfig config);

  // Run the interactive agent loop
  boost::asio::awaitable<void> run();

private:
  AgentConfig agent_config_;
  std::string current_model_;
  std::vector<boost::json::value> messages_;
  std::string system_prompt_;

  LLMConfig get_llm_config() const;

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
