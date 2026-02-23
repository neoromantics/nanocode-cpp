#include "tools.hpp"
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <ranges>
#include <regex>
#include <sstream>
#include <stdio.h>
#include <vector>

namespace fs = std::filesystem;

namespace tools {

std::string get_string(const boost::json::object &args,
                       boost::json::string_view key,
                       const std::string &default_val = "") {
  if (auto it = args.find(key); it != args.end() && it->value().is_string()) {
    return std::string(it->value().get_string());
  }
  return default_val;
}

long long get_int(const boost::json::object &args, boost::json::string_view key,
                  long long default_val = 0) {
  if (auto it = args.find(key); it != args.end() && it->value().is_number()) {
    return it->value().to_number<long long>();
  }
  return default_val;
}

bool get_bool(const boost::json::object &args, boost::json::string_view key,
              bool default_val = false) {
  if (auto it = args.find(key); it != args.end() && it->value().is_bool()) {
    return it->value().get_bool();
  }
  return default_val;
}

ToolResult execute_read(const boost::json::object &args) {
  std::string path = get_string(args, "path");
  long long offset = get_int(args, "offset", 0);
  // Use negative number to represent all lines by default
  long long limit = get_int(args, "limit", -1);

  std::ifstream file(path);
  if (!file.is_open())
    return std::unexpected("error: could not open " + path);

  std::vector<std::string> lines;
  std::string line;
  while (std::getline(file, line)) {
    lines.push_back(line);
  }

  if (limit < 0)
    limit = lines.size() - offset;

  // Bounds checking
  if (offset < 0)
    offset = 0;
  if (offset >= (long long)lines.size())
    return "";

  std::stringstream ss;
  auto sliced = std::views::all(lines) | std::views::drop(offset) |
                std::views::take(limit);
  long long idx = offset + 1;
  for (const auto &l : sliced) {
    ss << std::format("{:4}| {}\n", idx++, l);
  }
  return ss.str();
}

ToolResult execute_write(const boost::json::object &args) {
  std::string path = get_string(args, "path");
  std::string content = get_string(args, "content");

  std::ofstream file(path);
  if (!file.is_open())
    return std::unexpected("error: could not open " + path + " for writing");

  file << content;
  return "ok";
}

ToolResult execute_edit(const boost::json::object &args) {
  std::string path = get_string(args, "path");
  std::string old_str = get_string(args, "old");
  std::string new_str = get_string(args, "new");
  bool replace_all = get_bool(args, "all", false);

  std::ifstream in(path);
  if (!in.is_open())
    return std::unexpected("error: could not open " + path);

  std::stringstream buffer;
  buffer << in.rdbuf();
  std::string text = buffer.str();
  in.close();

  size_t count = 0;
  size_t pos = 0;
  while ((pos = text.find(old_str, pos)) != std::string::npos) {
    count++;
    pos += old_str.length();
  }

  if (count == 0)
    return std::unexpected("error: old_string not found");
  if (count > 1 && !replace_all)
    return std::unexpected(std::format(
        "error: old_string appears {} times, must be unique (use all=true)",
        count));

  if (replace_all) {
    pos = 0;
    while ((pos = text.find(old_str, pos)) != std::string::npos) {
      text.replace(pos, old_str.length(), new_str);
      pos += new_str.length();
    }
  } else {
    pos = text.find(old_str);
    if (pos != std::string::npos) {
      text.replace(pos, old_str.length(), new_str);
    }
  }

  std::ofstream out(path);
  if (!out.is_open())
    return std::unexpected("error: could not open " + path + " for writing");
  out << text;

  return "ok";
}

// Simple glob-to-regex conversion (handles * and **)
std::string glob_to_regex(const std::string &globPat) {
  std::string re = "^";
  for (size_t i = 0; i < globPat.size(); ++i) {
    if (globPat[i] == '*') {
      if (i + 1 < globPat.size() && globPat[i + 1] == '*') {
        re += ".*";
        i++; // skip second *
      } else {
        re += "[^/]*";
      }
    } else if (globPat[i] == '?') {
      re += ".";
    } else if (globPat[i] == '.') {
      re += "\\.";
    } else if (globPat[i] == '\\') {
      re += "\\\\";
    } else {
      re += globPat[i];
    }
  }
  re += "$";
  return re;
}

ToolResult execute_glob(const boost::json::object &args) {
  std::string pat = get_string(args, "pat");
  std::string path = get_string(args, "path", ".");

  if (path.empty())
    path = ".";

  std::regex re(glob_to_regex(pat));
  std::vector<fs::path> matched_files;

  std::error_code ec;
  auto start_path = fs::path(path);
  if (!fs::exists(start_path, ec))
    return "none";

  for (auto it = fs::recursive_directory_iterator(
           start_path, fs::directory_options::skip_permission_denied, ec);
       it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (ec) {
      ec.clear();
      continue;
    } // ignore errors traversing
    if (fs::is_regular_file(it->status(ec))) {
      // Check if relative path or filename matches pattern depending on how
      // nanocode handled it. nanocode joined path/pat. Let's match the relative
      // part against the regex.
      std::string rel_path = fs::relative(it->path(), start_path, ec).string();
      if (std::regex_match(rel_path, re) ||
          std::regex_match(it->path().filename().string(), re)) {
        matched_files.push_back(it->path());
      } else if (pat.find('/') != std::string::npos &&
                 std::regex_match(it->path().string(), re)) {
        // Full matched relative path if pattern contains explicit slash
        matched_files.push_back(it->path());
      }
    }
  }

  // Sort by mtime descending
  std::ranges::sort(matched_files, [](const fs::path &a, const fs::path &b) {
    std::error_code ec1, ec2;
    auto mtA = fs::last_write_time(a, ec1);
    auto mtB = fs::last_write_time(b, ec2);
    return mtA > mtB;
  });

  if (matched_files.empty())
    return "none";

  std::stringstream ss;
  bool first = true;
  for (const auto &p : matched_files) {
    if (!first)
      ss << "\n";
    ss << p.string();
    first = false;
  }
  return ss.str();
}

ToolResult execute_grep(const boost::json::object &args) {
  std::string pat = get_string(args, "pat");
  std::string path = get_string(args, "path", ".");
  if (path.empty())
    path = ".";

  std::regex re;
  try {
    re = std::regex(pat);
  } catch (...) {
    return std::unexpected("error: invalid regex pattern");
  }

  std::error_code ec;
  std::vector<std::string> hits;

  auto start_path = fs::path(path);
  if (!fs::exists(start_path, ec))
    return "none";

  for (auto it = fs::recursive_directory_iterator(
           start_path, fs::directory_options::skip_permission_denied, ec);
       it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (ec) {
      ec.clear();
      continue;
    }
    if (fs::is_regular_file(it->status(ec))) {
      std::ifstream file(it->path());
      if (!file.is_open())
        continue;
      std::string line;
      long long line_num = 1;
      while (std::getline(file, line)) {
        if (std::regex_search(line, re)) {
          hits.push_back(
              std::format("{}:{}:{}", it->path().string(), line_num, line));
          if (hits.size() >= 50)
            goto done; // match Python's top 50 limit efficiently
        }
        line_num++;
      }
    }
  }

done:
  if (hits.empty())
    return "none";

  std::stringstream ss;
  bool first = true;
  for (const auto &h : hits) {
    if (!first)
      ss << "\n";
    ss << h;
    first = false;
  }
  return ss.str();
}

ToolResult execute_bash(const boost::json::object &args) {
  std::string cmd = get_string(args, "cmd");

  // Open popen capturing both stdout and stderr
  std::string full_cmd = cmd + " 2>&1";
  FILE *fp = popen(full_cmd.c_str(), "r");
  if (!fp)
    return std::unexpected("error: popen failed");

  std::stringstream out;
  char buffer[1024];
  while (fgets(buffer, sizeof(buffer), fp) != nullptr) {
    std::string line(buffer);
    // Print live to stdout to mimic python script
    std::cout << "  \033[2m"
              << "│ " << line << "\033[0m" << std::flush;
    out << line;
  }
  pclose(fp);

  std::string result = out.str();
  if (result.empty())
    return "(empty)";
  // Trim trailing newline if present, but since we concatenated lines directly,
  // a simple check works
  while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) {
    result.pop_back();
  }
  return result;
}

boost::json::array get_tools_schema() {
  return {
      {{"name", "read"},
       {"description",
        "Read file with line numbers (file path, not directory)"},
       {"input_schema",
        {{"type", "object"},
         {"properties",
          {{"path", {{"type", "string"}}},
           {"offset", {{"type", "integer"}}},
           {"limit", {{"type", "integer"}}}}},
         {"required", {"path"}}}}},
      {{"name", "write"},
       {"description", "Write content to file"},
       {"input_schema",
        {{"type", "object"},
         {"properties",
          {{"path", {{"type", "string"}}}, {"content", {{"type", "string"}}}}},
         {"required", {"path", "content"}}}}},
      {{"name", "edit"},
       {"description",
        "Replace old with new in file (old must be unique unless all=true)"},
       {"input_schema",
        {{"type", "object"},
         {"properties",
          {{"path", {{"type", "string"}}},
           {"old", {{"type", "string"}}},
           {"new", {{"type", "string"}}},
           {"all", {{"type", "boolean"}}}}},
         {"required", {"path", "old", "new"}}}}},
      {{"name", "glob"},
       {"description", "Find files by pattern, sorted by mtime"},
       {"input_schema",
        {{"type", "object"},
         {"properties",
          {{"pat", {{"type", "string"}}}, {"path", {{"type", "string"}}}}},
         {"required", {"pat"}}}}},
      {{"name", "grep"},
       {"description", "Search files for regex pattern"},
       {"input_schema",
        {{"type", "object"},
         {"properties",
          {{"pat", {{"type", "string"}}}, {"path", {{"type", "string"}}}}},
         {"required", {"pat"}}}}},
      {{"name", "bash"},
       {"description", "Run shell command"},
       {"input_schema",
        {{"type", "object"},
         {"properties", {{"cmd", {{"type", "string"}}}}},
         {"required", {"cmd"}}}}}};
}

} // namespace tools
