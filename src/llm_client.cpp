#include "llm_client.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/error.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/version.hpp>
#include <iostream>

namespace beast = boost::beast;   // from <boost/beast.hpp>
namespace http = beast::http;     // from <boost/beast/http.hpp>
namespace net = boost::asio;      // from <boost/asio.hpp>
namespace ssl = boost::asio::ssl; // from <boost/asio/ssl.hpp>
using tcp = boost::asio::ip::tcp; // from <boost/asio/ip/tcp.hpp>

namespace llm {

boost::asio::awaitable<std::expected<LLMResponse, std::string>>
send_request(const LLMConfig &config, boost::json::object payload,
             ChunkCallback on_chunk) {
  auto executor = co_await net::this_coro::executor;

  if (on_chunk) {
    if (config.is_anthropic_format || config.is_openai_format) {
      payload["stream"] = true;
    }
  }

  // Parse URL (e.g. "https://api.anthropic.com/v1/messages")
  std::string url = config.api_url;
  std::string protocol = "https://";
  if (url.starts_with(protocol)) {
    url = url.substr(protocol.length());
  } else if (url.starts_with("http://")) {
    url = url.substr(7);
  }

  size_t slash_pos = url.find('/');
  std::string host = url.substr(0, slash_pos);
  std::string target =
      (slash_pos == std::string::npos) ? "/" : url.substr(slash_pos);
  std::string port = "443";

  // Setup SSL context
  ssl::context ctx(ssl::context::tlsv12_client);
  ctx.set_default_verify_paths();
  // In case user hasn't configured OpenSSL certs on mac:
  ctx.set_verify_mode(ssl::verify_none);

  try {
    // Look up the domain name
    tcp::resolver resolver(executor);
    auto const results =
        co_await resolver.async_resolve(host, port, net::use_awaitable);

    // Make the connection on the IP address we get from a lookup
    beast::ssl_stream<beast::tcp_stream> stream(executor, ctx);

    // Disable SNI verification just to be safe but set SNI host
    if (!SSL_set_tlsext_host_name(stream.native_handle(), host.c_str())) {
      boost::system::error_code ec{static_cast<int>(::ERR_get_error()),
                                   net::error::get_ssl_category()};
      throw boost::system::system_error{ec};
    }

    co_await beast::get_lowest_layer(stream).async_connect(results,
                                                           net::use_awaitable);

    // Perform the SSL handshake
    co_await stream.async_handshake(ssl::stream_base::client,
                                    net::use_awaitable);

    // Set up an HTTP POST request message
    http::request<http::string_body> req{http::verb::post, target, 11};
    req.set(http::field::host, host);
    req.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);
    req.set(http::field::content_type, "application/json");

    if (config.is_anthropic_format) {
      req.set("anthropic-version", "2023-06-01");
      if (host.find("openrouter") != std::string::npos) {
        req.set(http::field::authorization, "Bearer " + config.api_key);
      } else {
        req.set("x-api-key", config.api_key);
      }
    } else if (config.is_openai_format) {
      req.set(http::field::authorization, "Bearer " + config.api_key);
    }

    req.body() = boost::json::serialize(payload);
    req.prepare_payload();

    // Send the HTTP request
    co_await http::async_write(stream, req, net::use_awaitable);

    beast::flat_buffer buffer;

    if (!on_chunk) {
      http::response<http::string_body> res;
      co_await http::async_read(stream, buffer, res, net::use_awaitable);

      // Gracefully close the stream
      boost::system::error_code ec;
      co_await stream.async_shutdown(
          net::redirect_error(net::use_awaitable, ec));
      if (ec == net::error::eof) {
        ec = {};
      }

      boost::system::error_code parse_ec;
      boost::json::value parsed = boost::json::parse(res.body(), parse_ec);

      if (parse_ec) {
        co_return std::unexpected("JSON Parse Error: " + parse_ec.message() +
                                  "\nResponse body:\n" + res.body());
      }

      if (parsed.is_array() && !parsed.as_array().empty() &&
          parsed.as_array()[0].is_object()) {
        co_return LLMResponse{parsed.as_array()[0].as_object()};
      } else if (!parsed.is_object()) {
        co_return std::unexpected("API Response is not a JSON object nor an "
                                  "object array.\nResponse body:\n" +
                                  res.body());
      }
      co_return LLMResponse{parsed.as_object()};
    } else {
      http::response_parser<http::buffer_body> parser;
      parser.body_limit(1024ULL * 1024ULL * 100ULL);
      co_await http::async_read_header(stream, buffer, parser,
                                       net::use_awaitable);

      if (parser.get().result() != http::status::ok) {
        std::string err_body;
        char buf[8192];
        parser.get().body().data = buf;
        parser.get().body().size = sizeof(buf);
        while (!parser.is_done()) {
          boost::system::error_code ec;
          co_await http::async_read_some(
              stream, buffer, parser,
              net::redirect_error(net::use_awaitable, ec));
          if (ec && ec != http::error::need_buffer)
            break;
          size_t bytes = sizeof(buf) - parser.get().body().size;
          err_body.append(buf, bytes);
          parser.get().body().data = buf;
          parser.get().body().size = sizeof(buf);
        }
        co_return std::unexpected("HTTP Error " +
                                  std::to_string(parser.get().result_int()) +
                                  ": " + err_body);
      }

      char body_buf[8192];
      parser.get().body().data = body_buf;
      parser.get().body().size = sizeof(body_buf);
      std::string accumulated_body;

      std::string final_text;
      boost::json::array anthropic_content;
      boost::json::array openai_tool_calls;
      boost::json::object current_anthropic_tool;
      boost::json::object current_openai_tool;
      std::string current_tool_args;

      while (!parser.is_done()) {
        boost::system::error_code ec;
        co_await http::async_read_some(
            stream, buffer, parser,
            net::redirect_error(net::use_awaitable, ec));
        if (ec && ec != http::error::need_buffer)
          throw boost::system::system_error(ec);

        size_t bytes_transferred = sizeof(body_buf) - parser.get().body().size;
        accumulated_body.append(body_buf, bytes_transferred);
        parser.get().body().data = body_buf;
        parser.get().body().size = sizeof(body_buf);

        size_t pos;
        while ((pos = accumulated_body.find('\n')) != std::string::npos) {
          std::string line = accumulated_body.substr(0, pos);
          accumulated_body.erase(0, pos + 1);
          if (!line.empty() && line.back() == '\r')
            line.pop_back();

          if (line.starts_with("data: ")) {
            std::string data_str = line.substr(6);
            if (data_str == "[DONE]")
              continue;

            boost::system::error_code parse_ec;
            boost::json::value parsed = boost::json::parse(data_str, parse_ec);
            if (parse_ec || !parsed.is_object())
              continue;

            auto &obj = parsed.as_object();

            if (config.is_anthropic_format) {
              if (obj.contains("type")) {
                std::string type = obj.at("type").as_string().c_str();
                if (type == "content_block_start") {
                  auto &block = obj.at("content_block").as_object();
                  if (block.at("type").as_string() == "tool_use") {
                    current_anthropic_tool = {{"type", "tool_use"},
                                              {"id", block.at("id")},
                                              {"name", block.at("name")},
                                              {"input", boost::json::object{}}};
                    current_tool_args = "";
                  }
                } else if (type == "content_block_delta") {
                  auto &delta = obj.at("delta").as_object();
                  if (delta.at("type").as_string() == "text_delta") {
                    std::string text = delta.at("text").as_string().c_str();
                    final_text += text;
                    on_chunk(text);
                  } else if (delta.at("type").as_string() ==
                             "input_json_delta") {
                    current_tool_args +=
                        delta.at("partial_json").as_string().c_str();
                  }
                } else if (type == "content_block_stop") {
                  if (!current_anthropic_tool.empty()) {
                    if (!current_tool_args.empty())
                      current_anthropic_tool["input"] =
                          boost::json::parse(current_tool_args);
                    anthropic_content.push_back(current_anthropic_tool);
                    current_anthropic_tool = {};
                  }
                }
              }
            } else if (config.is_openai_format) {
              if (obj.contains("choices") && obj.at("choices").is_array() &&
                  !obj.at("choices").as_array().empty()) {
                auto &choice = obj.at("choices").as_array()[0].as_object();
                if (choice.contains("delta") &&
                    choice.at("delta").is_object()) {
                  auto &delta = choice.at("delta").as_object();
                  if (delta.contains("content") &&
                      delta.at("content").is_string()) {
                    std::string text = delta.at("content").as_string().c_str();
                    final_text += text;
                    on_chunk(text);
                  }
                  if (delta.contains("tool_calls") &&
                      delta.at("tool_calls").is_array()) {
                    for (auto &tc_val : delta.at("tool_calls").as_array()) {
                      auto &tc = tc_val.as_object();
                      if (tc.contains("id")) {
                        if (!current_openai_tool.empty()) {
                          current_openai_tool.at("function")
                              .as_object()["arguments"] = current_tool_args;
                          openai_tool_calls.push_back(current_openai_tool);
                        }
                        current_openai_tool = {
                            {"id", tc.at("id")},
                            {"type", "function"},
                            {"function",
                             {{"name",
                               tc.at("function").as_object().at("name")},
                              {"arguments", ""}}}};
                        current_tool_args = "";
                      }
                      if (tc.contains("function")) {
                        auto &func = tc.at("function").as_object();
                        if (func.contains("arguments"))
                          current_tool_args +=
                              func.at("arguments").as_string().c_str();
                      }
                    }
                  }
                }
              }
            }
          }
        }
      }

      if (config.is_openai_format && !current_openai_tool.empty()) {
        current_openai_tool.at("function").as_object()["arguments"] =
            current_tool_args;
        openai_tool_calls.push_back(current_openai_tool);
      }

      boost::json::object final_resp;
      if (config.is_anthropic_format) {
        if (!final_text.empty())
          anthropic_content.insert(anthropic_content.begin(),
                                   {{"type", "text"}, {"text", final_text}});
        final_resp["content"] = anthropic_content;
      } else {
        boost::json::object msg;
        msg["role"] = "assistant";
        if (!final_text.empty())
          msg["content"] = final_text;
        if (!openai_tool_calls.empty())
          msg["tool_calls"] = openai_tool_calls;
        final_resp["choices"] = boost::json::array{{{"message", msg}}};
      }

      boost::system::error_code ec;
      co_await stream.async_shutdown(
          net::redirect_error(net::use_awaitable, ec));
      co_return LLMResponse{final_resp};
    }
  } catch (std::exception const &e) {
    co_return std::unexpected(std::string("HTTP Error: ") + e.what());
  }
}

} // namespace llm
