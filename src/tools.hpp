#pragma once

#include <string>
#include <expected>
#include <boost/json.hpp>

namespace tools {

// Each tool takes a boost::json::object containing its arguments
// and returns either a string (the tool's output to send back to the LLM)
// or an error string if something went wrong.
using ToolResult = std::expected<std::string, std::string>;

ToolResult execute_read(const boost::json::object& args);
ToolResult execute_write(const boost::json::object& args);
ToolResult execute_edit(const boost::json::object& args);
ToolResult execute_glob(const boost::json::object& args);
ToolResult execute_grep(const boost::json::object& args);
ToolResult execute_bash(const boost::json::object& args);

// Returns the JSON schema for all available tools
boost::json::array get_tools_schema();

} // namespace tools
