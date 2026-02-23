#include "agent.hpp"
#include "tools.hpp"
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/json/src.hpp> // Include this once in the project if needed, or link
#include <format>
#include <iostream>

namespace agent {

const std::string RESET = "\033[0m";
const std::string BOLD = "\033[1m";
const std::string DIM = "\033[2m";
const std::string BLUE = "\033[34m";
const std::string CYAN = "\033[36m";
const std::string GREEN = "\033[32m";
const std::string YELLOW = "\033[33m";
const std::string RED = "\033[31m";

std::string render_markdown(const std::string &text) {
  // A simple regex replacement for **bold** could be done here.
  // To avoid complex regex in C++, we do a simple string find/replace
  std::string res = text;
  size_t pos = 0;
  while ((pos = res.find("**", pos)) != std::string::npos) {
    size_t end = res.find("**", pos + 2);
    if (end == std::string::npos)
      break;
    std::string inner = res.substr(pos + 2, end - pos - 2);
    res.replace(pos, end - pos + 2, BOLD + inner + RESET);
    pos += inner.length() + BOLD.length() + RESET.length();
  }
  return res;
}

std::string separator() {
  std::string s = DIM;
  for (int i = 0; i < 80; ++i)
    s += "─";
  s += RESET;
  return s;
}

boost::json::object Agent::build_anthropic_payload() {
  boost::json::object payload;
  payload["model"] = config_.model;
  payload["max_tokens"] = 8192;
  payload["system"] = system_prompt_;
  payload["tools"] = tools::get_tools_schema();
  boost::json::array msgs;
  for (const auto &m : messages_) {
    msgs.push_back(m);
  }
  payload["messages"] = msgs;
  return payload;
}

boost::json::object Agent::build_openai_payload() {
  boost::json::object payload;
  payload["model"] = config_.model;

  // Tools schema translation
  boost::json::array openai_tools;
  for (const auto &anthropic_tool_val : tools::get_tools_schema()) {
    const auto &anthropic_tool = anthropic_tool_val.as_object();
    boost::json::object func;
    func["name"] = anthropic_tool.at("name");
    func["description"] = anthropic_tool.at("description");
    func["parameters"] =
        anthropic_tool.at("input_schema"); // close enough for OpenAI/Gemini

    boost::json::object tool;
    tool["type"] = "function";
    tool["function"] = func;
    openai_tools.push_back(tool);
  }
  payload["tools"] = openai_tools;

  // Messages translation
  boost::json::array msgs;
  msgs.push_back({{"role", "system"}, {"content", system_prompt_}});

  for (const auto &m_val : messages_) {
    const auto &m = m_val.as_object();
    std::string role = m.at("role").as_string().c_str();

    if (role == "user") {
      const auto &content = m.at("content");
      if (content.is_string()) {
        msgs.push_back({{"role", "user"}, {"content", content.as_string()}});
      } else if (content.is_array()) {
        // Tool results
        for (const auto &item_val : content.as_array()) {
          const auto &item = item_val.as_object();
          if (item.at("type").as_string() == "tool_result") {
            msgs.push_back({{"role", "tool"},
                            {"tool_call_id", item.at("tool_use_id")},
                            {"content", item.at("content")}});
          }
        }
      }
    } else if (role == "assistant") {
      const auto &content = m.at("content");
      if (content.is_array()) {
        std::string text_content;
        boost::json::array tool_calls;
        for (const auto &block_val : content.as_array()) {
          const auto &block = block_val.as_object();
          std::string type = block.at("type").as_string().c_str();
          if (type == "text") {
            text_content += block.at("text").as_string().c_str();
          } else if (type == "tool_use") {
            boost::json::object func;
            func["name"] = block.at("name");
            func["arguments"] = boost::json::serialize(block.at("input"));
            boost::json::object tc;
            tc["id"] = block.at("id");
            tc["type"] = "function";
            tc["function"] = func;
            tool_calls.push_back(tc);
          }
        }
        boost::json::object asst_msg;
        asst_msg["role"] = "assistant";
        if (!text_content.empty())
          asst_msg["content"] = text_content;
        if (!tool_calls.empty())
          asst_msg["tool_calls"] = tool_calls;
        msgs.push_back(asst_msg);
      } else if (content.is_string()) {
        msgs.push_back(
            {{"role", "assistant"}, {"content", content.as_string()}});
      }
    }
  }
  payload["messages"] = msgs;
  return payload;
}

boost::json::object
Agent::normalize_openai_response(const boost::json::object &raw_resp) {
  // Translate OpenAI response to Anthropic structure
  boost::json::object anthropic_content;
  boost::json::array content_blocks;

  if (raw_resp.contains("choices") && raw_resp.at("choices").is_array() &&
      !raw_resp.at("choices").as_array().empty()) {
    const auto &message = raw_resp.at("choices")
                              .as_array()[0]
                              .as_object()
                              .at("message")
                              .as_object();

    if (message.contains("content") && message.at("content").is_string()) {
      boost::json::object text_block;
      text_block["type"] = "text";
      text_block["text"] = message.at("content").as_string();
      content_blocks.push_back(text_block);
    }

    if (message.contains("tool_calls") && message.at("tool_calls").is_array()) {
      for (const auto &tc_val : message.at("tool_calls").as_array()) {
        const auto &tc = tc_val.as_object();
        if (tc.at("type").as_string() == "function") {
          const auto &func = tc.at("function").as_object();
          boost::json::object tool_use_block;
          tool_use_block["type"] = "tool_use";
          tool_use_block["id"] = tc.at("id");
          tool_use_block["name"] = func.at("name");
          // parse arguments string back to json object
          tool_use_block["input"] =
              boost::json::parse(func.at("arguments").as_string().c_str())
                  .as_object();
          content_blocks.push_back(tool_use_block);
        }
      }
    }
  }

  anthropic_content["content"] = content_blocks;
  return anthropic_content;
}

Agent::Agent(LLMConfig config) : config_(std::move(config)) {}

boost::asio::awaitable<void> Agent::run() {
  std::cout << BOLD << "nanocode-cpp" << RESET << " | " << DIM << config_.model
            << " | C++23 Rewrite" << RESET << "\n\n";

  system_prompt_ = "Concise coding assistant.";

  while (true) {
    std::cout << separator() << "\n";
    std::cout << BOLD << BLUE << "❯ " << RESET;

    std::string user_input;
    if (!std::getline(std::cin, user_input))
      break;

    std::cout << separator() << "\n";

    if (user_input.empty())
      continue;
    if (user_input == "/q" || user_input == "exit")
      break;
    if (user_input == "/c") {
      messages_.clear();
      std::cout << GREEN << "⏺ Cleared conversation" << RESET << "\n";
      continue;
    }

    messages_.push_back({{"role", "user"}, {"content", user_input}});

    co_await run_agentic_loop();
    std::cout << "\n";
  }
}

boost::asio::awaitable<void> Agent::run_agentic_loop() {
  while (true) {
    boost::json::object payload;
    if (config_.is_anthropic_format) {
      payload = build_anthropic_payload();
    } else {
      payload = build_openai_payload();
    }

    auto result_expected = co_await llm::send_request(config_, payload);
    if (!result_expected.has_value()) {
      std::cout << RED << "⏺ Error: " << result_expected.error() << RESET
                << "\n";
      break;
    }

    boost::json::object raw_resp = result_expected.value().raw_json;

    if (raw_resp.contains("error")) {
      std::cout << RED << "⏺ API Error: "
                << boost::json::serialize(raw_resp.at("error")) << RESET
                << "\n";
      break;
    }

    boost::json::object content_data;
    if (config_.is_anthropic_format) {
      content_data = raw_resp;
    } else {
      content_data = normalize_openai_response(raw_resp);
    }

    const auto &content_blocks = content_data.at("content").as_array();
    boost::json::array tool_results;

    for (const auto &block_val : content_blocks) {
      const auto &block = block_val.as_object();
      std::string type = block.at("type").as_string().c_str();

      if (type == "text") {
        std::cout << "\n"
                  << CYAN << "⏺" << RESET << " "
                  << render_markdown(block.at("text").as_string().c_str())
                  << "\n";
      }

      if (type == "tool_use") {
        std::string tool_name = block.at("name").as_string().c_str();
        const auto &tool_args = block.at("input").as_object();

        std::string arg_preview;
        if (!tool_args.empty()) {
          arg_preview =
              boost::json::serialize(tool_args.begin()->value()).substr(0, 50);
        }

        std::cout << "\n"
                  << GREEN << "⏺ " << tool_name << RESET << "(" << DIM
                  << arg_preview << RESET << ")\n";

        tools::ToolResult res;
        if (tool_name == "read")
          res = tools::execute_read(tool_args);
        else if (tool_name == "write")
          res = tools::execute_write(tool_args);
        else if (tool_name == "edit")
          res = tools::execute_edit(tool_args);
        else if (tool_name == "glob")
          res = tools::execute_glob(tool_args);
        else if (tool_name == "grep")
          res = tools::execute_grep(tool_args);
        else if (tool_name == "bash")
          res = tools::execute_bash(tool_args);
        else
          res = std::unexpected("error: unknown tool " + tool_name);

        std::string res_str = res.has_value() ? res.value() : res.error();

        // Truncate preview
        std::string preview;
        auto newline_pos = res_str.find('\n');
        if (newline_pos != std::string::npos) {
          preview = res_str.substr(0, std::min<size_t>(60, newline_pos)) +
                    " ... + lines";
        } else {
          preview = res_str.substr(0, 60);
          if (res_str.length() > 60)
            preview += "...";
        }

        std::cout << "  " << DIM << "⎿  " << preview << RESET << "\n";

        tool_results.push_back({{"type", "tool_result"},
                                {"tool_use_id", block.at("id")},
                                {"content", res_str}});
      }
    }

    messages_.push_back({{"role", "assistant"}, {"content", content_blocks}});

    if (tool_results.empty())
      break;
    messages_.push_back({{"role", "user"}, {"content", tool_results}});
  }
}

} // namespace agent
