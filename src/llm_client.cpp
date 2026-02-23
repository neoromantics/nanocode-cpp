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
send_request(const LLMConfig &config, const boost::json::object &payload) {
  auto executor = co_await net::this_coro::executor;

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

    auto ep = co_await beast::get_lowest_layer(stream).async_connect(
        results, net::use_awaitable);

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

    // This buffer is used for reading and must be persisted
    beast::flat_buffer buffer;

    // Declare a container to hold the response
    http::response<http::string_body> res;

    // Receive the HTTP response
    co_await http::async_read(stream, buffer, res, net::use_awaitable);

    // Gracefully close the stream
    boost::system::error_code ec;
    co_await stream.async_shutdown(net::redirect_error(net::use_awaitable, ec));
    if (ec == net::error::eof) {
      // Rationale:
      // http://stackoverflow.com/questions/25587403/boost-asio-ssl-async-shutdown-always-finishes-with-an-error
      ec = {};
    }

    // Parse JSON response
    boost::json::value parsed = boost::json::parse(res.body());
    co_return LLMResponse{parsed.as_object()};

  } catch (std::exception const &e) {
    co_return std::unexpected(std::string("HTTP Error: ") + e.what());
  }
}

} // namespace llm
