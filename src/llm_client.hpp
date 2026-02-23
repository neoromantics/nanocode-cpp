#pragma once

#include <boost/asio/awaitable.hpp>
#include <boost/json.hpp>
#include <expected>
#include <string>
#include <vector>

struct LLMConfig {
  std::string api_url;
  std::string api_key;
  std::string model;
  bool is_anthropic_format = true;
  bool is_openai_format = false;
};

struct LLMResponse {
  boost::json::object raw_json;
};

namespace llm {

// Sends an asynchronous POST request using Boost.Asio coroutines.
// `host` and `target` are extracted from the config.api_url (e.g. host:
// api.anthropic.com, target: /v1/messages)
boost::asio::awaitable<std::expected<LLMResponse, std::string>>
send_request(const LLMConfig &config, const boost::json::object &payload);

} // namespace llm
