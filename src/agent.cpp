#include "agent.hpp"
#include "tools.hpp"
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/json/src.hpp> // Include this once in the project if needed, or link
#include <filesystem>
#include <fstream>
#include <iostream>

#include "replxx.hxx"

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
  payload["model"] = current_model_;
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
  payload["model"] = current_model_;

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

Agent::Agent(AgentConfig config)
    : agent_config_(std::move(config)),
      current_model_(agent_config_.initial_model) {}

LLMConfig Agent::get_llm_config() const {
  LLMConfig config;
  config.model = current_model_;

  // Determine API based on model name
  if (current_model_.find('/') != std::string::npos) {
    config.api_key = agent_config_.openrouter_key;
    config.api_url = "https://openrouter.ai/api/v1/messages";
    config.is_anthropic_format = true;
    config.is_openai_format = false;
  } else if (current_model_.find("gemini") != std::string::npos ||
             current_model_.find("learnlm") != std::string::npos) {
    config.api_key = agent_config_.gemini_key;
    config.api_url = "https://generativelanguage.googleapis.com/v1beta/openai/"
                     "chat/completions";
    config.is_anthropic_format = false;
    config.is_openai_format = true;
  } else {
    config.api_key = agent_config_.anthropic_key;
    config.api_url = "https://api.anthropic.com/v1/messages";
    config.is_anthropic_format = true;
    config.is_openai_format = false;
  }
  return config;
}

boost::asio::awaitable<void> Agent::run() {
  std::cout << BOLD << "nanocode-cpp" << RESET << " | " << current_model_
            << " | " << std::filesystem::current_path().string() << RESET
            << "\n";
  std::cout << DIM << "Available commands:" << RESET << "\n";
  std::cout << DIM << "  /model <name>  - Switch LLM model" << RESET << "\n";
  std::cout << DIM << "  /save <file>   - Save conversation to JSON" << RESET
            << "\n";
  std::cout << DIM << "  /load <file>   - Load conversation from JSON" << RESET
            << "\n";
  std::cout << DIM << "  /c             - Clear current conversation context"
            << RESET << "\n";
  std::cout << DIM << "  /q or /exit    - Quit application" << RESET << "\n\n";

  system_prompt_ = "Concise coding assistant.";

  replxx::Replxx rx;
  rx.install_window_change_handler();
  rx.set_completion_callback(
      [](std::string const &input,
         int &contextLen) -> replxx::Replxx::completions_t {
        replxx::Replxx::completions_t completions;
        if (input.empty())
          return completions;

        if (input.starts_with("/model ")) {
          std::string prefix = input.substr(7);
          const char *models[] = {"gemini-2.5-flash",
                                  "gemini-2.5-pro",
                                  "claude-3-5-sonnet-20241022",
                                  "claude-3-5-haiku-20241022",
                                  "gpt-4o",
                                  "gpt-4o-mini",
                                  "o1-preview",
                                  "o1-mini",
                                  "o3-mini"};
          for (const auto &m : models) {
            if (std::string(m).starts_with(prefix)) {
              completions.emplace_back("/model " + std::string(m));
            }
          }
          contextLen = input.length();
          return completions;
        }

        if (input[0] == '/') {
          const char *cmds[] = {"/save ", "/load ", "/c", "/q", "/exit"};
          for (const auto &cmd : cmds) {
            if (std::string(cmd).starts_with(input)) {
              completions.emplace_back(cmd);
            }
          }
          contextLen = input.length();
        }
        return completions;
      });

  while (true) {
    std::cout << separator() << "\n";

    std::string prompt = BOLD + BLUE + "❯ " + RESET;
    char const *line = rx.input(prompt);
    if (!line)
      break;

    std::string user_input(line);
    if (!user_input.empty()) {
      rx.history_add(user_input);
    }

    std::cout << separator() << "\n";

    if (user_input.empty())
      continue;
    if (user_input == "/q" || user_input == "exit" || user_input == "/exit")
      break;
    if (user_input == "/c") {
      messages_.clear();
      std::cout << GREEN << "⏺ Cleared conversation" << RESET << "\n";
      continue;
    }

    if (user_input.starts_with("/model ")) {
      std::string new_model = user_input.substr(7);
      if (!new_model.empty()) {
        current_model_ = new_model;
        std::cout << BOLD << "nanocode-cpp" << RESET << " | " << current_model_
                  << " | " << std::filesystem::current_path().string() << RESET
                  << "\n";
        std::cout << GREEN << "⏺ Switched model to: " << current_model_ << RESET
                  << "\n";
      }
      continue;
    }

    if (user_input.starts_with("/save ")) {
      std::string filename = user_input.substr(6);
      if (!filename.empty()) {
        std::ofstream out(filename);
        if (out) {
          boost::json::object save_data;
          save_data["model"] = current_model_;
          save_data["messages"] = boost::json::value_from(messages_);
          out << boost::json::serialize(save_data);
          std::cout << GREEN << "⏺ Saved conversation and model context to "
                    << filename << RESET << "\n";
        } else {
          std::cout << RED << "⏺ Failed to open " << filename << " for writing"
                    << RESET << "\n";
        }
      }
      continue;
    }

    if (user_input.starts_with("/load ")) {
      std::string filename = user_input.substr(6);
      if (!filename.empty()) {
        std::ifstream in(filename);
        if (in) {
          std::string content((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
          boost::system::error_code ec;
          auto parsed = boost::json::parse(content, ec);
          if (!ec) {
            if (parsed.is_object()) {
              auto obj = parsed.as_object();
              if (obj.contains("model") && obj.at("model").is_string()) {
                current_model_ = obj.at("model").as_string().c_str();
              }
              if (obj.contains("messages") && obj.at("messages").is_array()) {
                messages_.clear();
                for (auto &item : obj.at("messages").as_array()) {
                  messages_.push_back(item);
                }
              }
              std::cout << BOLD << "nanocode-cpp" << RESET << " | "
                        << current_model_ << " | "
                        << std::filesystem::current_path().string() << RESET
                        << "\n";
              std::cout << GREEN
                        << "⏺ Loaded conversation and restored model from "
                        << filename << RESET << "\n";
            } else if (parsed.is_array()) {
              // Backward compatibility for old raw-array saves
              messages_.clear();
              for (auto &item : parsed.as_array()) {
                messages_.push_back(item);
              }
              std::cout << GREEN << "⏺ Loaded legacy conversation from "
                        << filename << RESET << "\n";
            } else {
              std::cout << RED << "⏺ Invalid save file format in " << filename
                        << RESET << "\n";
            }
          } else {
            std::cout << RED << "⏺ Failed to parse " << filename << " ("
                      << ec.message() << ")" << RESET << "\n";
          }
        } else {
          std::cout << RED << "⏺ Failed to open " << filename << " for reading"
                    << RESET << "\n";
        }
      }
      continue;
    }

    messages_.push_back({{"role", "user"}, {"content", user_input}});

    co_await run_agentic_loop();
    std::cout << "\n";
  }
}

boost::asio::awaitable<void> Agent::run_agentic_loop() {
  while (true) {
    LLMConfig config_ = get_llm_config();
    boost::json::object payload;
    if (config_.is_anthropic_format) {
      payload = build_anthropic_payload();
    } else {
      payload = build_openai_payload();
    }

    bool printed_prefix = false;
    auto spinner_active = std::make_shared<bool>(true);

    boost::asio::co_spawn(
        co_await boost::asio::this_coro::executor,
        [spinner_active]() -> boost::asio::awaitable<void> {
          const char spinner[] = {'|', '/', '-', '\\'};
          int i = 0;
          boost::asio::steady_timer timer(
              co_await boost::asio::this_coro::executor);
          while (*spinner_active) {
            std::cout << "\r" << DIM << "⏺ Thinking " << spinner[i++ % 4]
                      << RESET << std::flush;
            timer.expires_after(std::chrono::milliseconds(100));
            boost::system::error_code ec;
            co_await timer.async_wait(
                boost::asio::redirect_error(boost::asio::use_awaitable, ec));
          }
        },
        boost::asio::detached);

    auto on_chunk = [&printed_prefix,
                     spinner_active](const std::string &chunk) {
      if (*spinner_active) {
        *spinner_active = false;
        std::cout << "\r\33[2K" << std::flush;
      }
      if (!printed_prefix) {
        std::cout << "\n" << CYAN << "⏺" << RESET << " ";
        printed_prefix = true;
      }
      std::cout << chunk << std::flush;
    };

    auto result_expected =
        co_await llm::send_request(config_, payload, on_chunk);

    if (*spinner_active) {
      *spinner_active = false;
      std::cout << "\r\33[2K" << std::flush;
    }
    if (!result_expected.has_value()) {
      std::cout << RED << "\n⏺ Error: " << result_expected.error() << RESET
                << "\n";
      break;
    }

    if (printed_prefix) {
      std::cout << "\n";
    }

    boost::json::object raw_resp = result_expected.value().raw_json;

    if (raw_resp.contains("error")) {
      std::cout << RED << "\n⏺ API Error: "
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
        // Text is already streamed to stdout, do nothing.
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
        else if (tool_name == "grep")
          res = tools::execute_grep(tool_args);
        else if (tool_name == "bash")
          res = tools::execute_bash(tool_args);
        else if (tool_name == "fetch_url")
          res = tools::execute_fetch_url(tool_args);
        else if (tool_name == "execute_python")
          res = tools::execute_python(tool_args);
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
