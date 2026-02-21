// lm.cpp
#include <curl/curl.h>

#include <cerrno>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <curses.h>
#include <deque>
#include <filesystem>
#include <functional>
#include <fstream>
#include <locale.h>
#include <map>
#include <iostream>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
  #include <io.h>
  #define isatty _isatty
  #define fileno _fileno
  #define popen _popen
  #define pclose _pclose
#else
  #include <sys/wait.h>
  #include <unistd.h>
#endif

// ------------------------- env / misc utils -------------------------

static std::string getEnv(const char* key) {
  const char* v = std::getenv(key);
  return v ? std::string(v) : std::string();
}

static std::string trimTrailingSlash(std::string s) {
  while (!s.empty() && s.back() == '/') s.pop_back();
  return s;
}

static std::string trimCopy(const std::string& s) {
  size_t start = 0;
  while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) start++;
  size_t end = s.size();
  while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) end--;
  return s.substr(start, end - start);
}

static std::string expandUserPath(const std::string& path) {
  if (path.empty() || path[0] != '~') return path;
  std::string home = getEnv("HOME");
  if (home.empty()) return path;
  if (path == "~") return home;
  if (path.size() >= 2 && path[1] == '/') return home + path.substr(1);
  return path;
}

struct ToolSpecInput {
  std::string name;
  std::string description;
};

struct ToolSpec {
  std::string name;
  std::string command;
  std::string description;
  std::vector<ToolSpecInput> inputs;
};

struct ToolCall {
  std::string id;
  std::string name;
  std::string argumentsJson;
};

struct ChatMessage {
  std::string role;
  std::string content;
  std::vector<ToolCall> toolCalls;
  std::string toolCallId;
};

static std::string readFileAll(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    std::ostringstream oss;
    oss << "[error reading file '" << path << "': " << std::strerror(errno) << "]";
    return oss.str();
  }
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

static std::string runCommandCapture(const std::string& cmd) {
  // NOTE: uses a shell. Do not pass untrusted input.
  std::string full = cmd + " 2>&1";
  FILE* pipe = popen(full.c_str(), "r");
  if (!pipe) return "[error running command: popen failed]";
  std::string out;
  char buf[4096];
  while (true) {
    size_t n = std::fread(buf, 1, sizeof(buf), pipe);
    if (n > 0) out.append(buf, buf + n);
    if (n < sizeof(buf)) break;
  }
  pclose(pipe);
  return out;
}

static bool isValidToolName(const std::string& name) {
  if (name.empty() || name.size() > 64) return false;
  for (char c : name) {
    if (std::isalnum(static_cast<unsigned char>(c))) continue;
    if (c == '_' || c == '-') continue;
    return false;
  }
  return true;
}

static std::optional<std::string> parseTomlBasicString(const std::string& raw) {
  std::string s = trimCopy(raw);
  if (s.size() < 2 || s.front() != '"' || s.back() != '"') return std::nullopt;
  std::string out;
  bool escaping = false;
  for (size_t i = 1; i + 1 < s.size(); i++) {
    char c = s[i];
    if (!escaping && c == '\\') {
      escaping = true;
      continue;
    }
    if (!escaping) {
      out.push_back(c);
      continue;
    }
    escaping = false;
    switch (c) {
      case 'n': out.push_back('\n'); break;
      case 'r': out.push_back('\r'); break;
      case 't': out.push_back('\t'); break;
      case '"': out.push_back('"'); break;
      case '\\': out.push_back('\\'); break;
      default: out.push_back(c); break;
    }
  }
  if (escaping) return std::nullopt;
  return out;
}

static std::string stripTomlComment(const std::string& line) {
  bool inQuote = false;
  bool escaping = false;
  for (size_t i = 0; i < line.size(); i++) {
    char c = line[i];
    if (inQuote) {
      if (escaping) {
        escaping = false;
      } else if (c == '\\') {
        escaping = true;
      } else if (c == '"') {
        inQuote = false;
      }
      continue;
    }
    if (c == '"') {
      inQuote = true;
      continue;
    }
    if (c == '#') return line.substr(0, i);
  }
  return line;
}

static std::vector<ToolSpec> loadToolSpecsFromToml(const std::string& path, std::vector<std::string>& warningsOut) {
  std::ifstream f(path);
  if (!f) return {};

  std::vector<ToolSpec> tools;
  ToolSpec current;
  bool inTool = false;
  bool inInputs = false;
  std::set<std::string> seenNames;
  std::string line;
  int lineNo = 0;

  auto flushTool = [&]() {
    if (!inTool) return;
    if (current.name.empty() || current.command.empty() || current.description.empty()) {
      warningsOut.push_back("skills.toml: skipped tool with missing name/command/description");
    } else if (!isValidToolName(current.name)) {
      warningsOut.push_back("skills.toml: skipped tool '" + current.name + "' (name must match [A-Za-z0-9_-]{1,64})");
    } else if (seenNames.count(current.name)) {
      warningsOut.push_back("skills.toml: skipped duplicate tool name '" + current.name + "'");
    } else {
      seenNames.insert(current.name);
      tools.push_back(current);
    }
    current = ToolSpec{};
    inTool = false;
    inInputs = false;
  };

  while (std::getline(f, line)) {
    lineNo++;
    std::string cleaned = trimCopy(stripTomlComment(line));
    if (cleaned.empty()) continue;

    if (cleaned == "[[tools]]" || cleaned == "[[tool]]") {
      flushTool();
      inTool = true;
      inInputs = false;
      continue;
    }
    if (cleaned == "[tools.inputs]" || cleaned == "[tool.inputs]") {
      if (!inTool) {
        warningsOut.push_back("skills.toml:" + std::to_string(lineNo) + ": inputs section without active tool");
      } else {
        inInputs = true;
      }
      continue;
    }
    if (!inTool) {
      warningsOut.push_back("skills.toml:" + std::to_string(lineNo) + ": ignored line outside [[tools]]");
      continue;
    }

    size_t eq = cleaned.find('=');
    if (eq == std::string::npos) {
      warningsOut.push_back("skills.toml:" + std::to_string(lineNo) + ": expected key = \"value\"");
      continue;
    }
    std::string key = trimCopy(cleaned.substr(0, eq));
    std::string rawVal = trimCopy(cleaned.substr(eq + 1));
    auto parsedVal = parseTomlBasicString(rawVal);
    if (!parsedVal) {
      warningsOut.push_back("skills.toml:" + std::to_string(lineNo) + ": only basic quoted string values are supported");
      continue;
    }

    if (inInputs) {
      bool replaced = false;
      for (auto& input : current.inputs) {
        if (input.name == key) {
          input.description = *parsedVal;
          replaced = true;
          break;
        }
      }
      if (!replaced) current.inputs.push_back({key, *parsedVal});
      continue;
    }

    if (key == "name") current.name = *parsedVal;
    else if (key == "command") current.command = *parsedVal;
    else if (key == "description") current.description = *parsedVal;
    else warningsOut.push_back("skills.toml:" + std::to_string(lineNo) + ": ignored key '" + key + "'");
  }

  flushTool();
  return tools;
}

static std::vector<std::string> extractTemplateVariables(const std::string& commandTemplate) {
  std::vector<std::string> vars;
  std::set<std::string> seen;
  for (size_t i = 0; i < commandTemplate.size(); i++) {
    if (commandTemplate[i] != '$') continue;
    if (i + 1 >= commandTemplate.size()) continue;
    char first = commandTemplate[i + 1];
    if (!(std::isalpha(static_cast<unsigned char>(first)) || first == '_')) continue;
    size_t j = i + 2;
    while (j < commandTemplate.size()) {
      char c = commandTemplate[j];
      if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') j++;
      else break;
    }
    std::string var = commandTemplate.substr(i + 1, j - (i + 1));
    if (!seen.count(var)) {
      seen.insert(var);
      vars.push_back(var);
    }
    i = j - 1;
  }
  return vars;
}

static std::string shellQuoteSingle(const std::string& s) {
  std::string out = "'";
  for (char c : s) {
    if (c == '\'') out += "'\"'\"'";
    else out.push_back(c);
  }
  out += "'";
  return out;
}

static std::string renderToolCommand(
    const std::string& commandTemplate,
    const std::map<std::string, std::string>& args
) {
  std::string out;
  for (size_t i = 0; i < commandTemplate.size(); i++) {
    char c = commandTemplate[i];
    if (c != '$' || i + 1 >= commandTemplate.size()) {
      out.push_back(c);
      continue;
    }
    char first = commandTemplate[i + 1];
    if (!(std::isalpha(static_cast<unsigned char>(first)) || first == '_')) {
      out.push_back(c);
      continue;
    }
    size_t j = i + 2;
    while (j < commandTemplate.size()) {
      char cc = commandTemplate[j];
      if (std::isalnum(static_cast<unsigned char>(cc)) || cc == '_') j++;
      else break;
    }
    std::string var = commandTemplate.substr(i + 1, j - (i + 1));
    auto it = args.find(var);
    if (it != args.end()) out += shellQuoteSingle(it->second);
    else out += "$" + var;
    i = j - 1;
  }
  return out;
}

struct CommandRunResult {
  int exitCode{0};
  std::string stdoutText;
  std::string stderrText;
  bool launchFailed{false};
  std::string launchError;
};

static CommandRunResult runCommandCaptureSplit(const std::string& cmd) {
  CommandRunResult result;
  std::filesystem::path tmpDir;
  const auto now = static_cast<unsigned long long>(std::time(nullptr));
  for (int attempt = 0; attempt < 32; attempt++) {
    tmpDir = std::filesystem::temp_directory_path() /
             ("lm_tool_" + std::to_string(now) + "_" + std::to_string(attempt));
    if (!std::filesystem::exists(tmpDir)) {
      std::error_code ec;
      if (std::filesystem::create_directory(tmpDir, ec)) break;
    }
    if (attempt == 31) {
      result.launchFailed = true;
      result.launchError = "failed to create temp directory for command output";
      return result;
    }
  }

  std::filesystem::path outPath = tmpDir / "stdout.txt";
  std::filesystem::path errPath = tmpDir / "stderr.txt";
  std::string wrapped = "sh -lc " + shellQuoteSingle(cmd) +
                        " > " + shellQuoteSingle(outPath.string()) +
                        " 2> " + shellQuoteSingle(errPath.string());

  int rc = std::system(wrapped.c_str());
  if (rc == -1) {
    result.launchFailed = true;
    result.launchError = "failed to start shell command";
  }

  if (!result.launchFailed) {
#if defined(_WIN32)
    result.exitCode = rc;
#else
    if (WIFEXITED(rc)) result.exitCode = WEXITSTATUS(rc);
    else if (WIFSIGNALED(rc)) result.exitCode = 128 + WTERMSIG(rc);
    else result.exitCode = rc;
#endif
  }

  auto readIfExists = [](const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return std::string();
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
  };
  result.stdoutText = readIfExists(outPath);
  result.stderrText = readIfExists(errPath);

  std::error_code ec;
  std::filesystem::remove_all(tmpDir, ec);
  return result;
}

static std::string truncateForDisplay(const std::string& text, size_t maxChars) {
  if (text.size() <= maxChars) return text;
  std::ostringstream oss;
  oss << text.substr(0, maxChars)
      << "\n...[truncated " << (text.size() - maxChars) << " chars]";
  return oss.str();
}

// ------------------------- JSON (minimal) -------------------------

static std::string jsonEscape(const std::string& s) {
  std::string o;
  o.reserve(s.size() + 16);
  for (unsigned char c : s) {
    switch (c) {
      case '\\': o += "\\\\"; break;
      case '"':  o += "\\\""; break;
      case '\b': o += "\\b";  break;
      case '\f': o += "\\f";  break;
      case '\n': o += "\\n";  break;
      case '\r': o += "\\r";  break;
      case '\t': o += "\\t";  break;
      default:
        if (c < 0x20) {
          char buf[7];
          std::snprintf(buf, sizeof(buf), "\\u%04x", (unsigned)c);
          o += buf;
        } else {
          o.push_back((char)c);
        }
    }
  }
  return o;
}

static std::optional<std::string> parseJsonStringAt(const std::string& s, size_t& i) {
  if (i >= s.size() || s[i] != '"') return std::nullopt;
  i++;
  std::string out;
  while (i < s.size()) {
    char c = s[i++];
    if (c == '"') return out;
    if (c != '\\') { out.push_back(c); continue; }
    if (i >= s.size()) return std::nullopt;
    char e = s[i++];
    switch (e) {
      case '"': out.push_back('"'); break;
      case '\\': out.push_back('\\'); break;
      case '/': out.push_back('/'); break;
      case 'b': out.push_back('\b'); break;
      case 'f': out.push_back('\f'); break;
      case 'n': out.push_back('\n'); break;
      case 'r': out.push_back('\r'); break;
      case 't': out.push_back('\t'); break;
      case 'u': {
        if (i + 4 > s.size()) return std::nullopt;
        unsigned code = 0;
        for (int k = 0; k < 4; k++) {
          char h = s[i++];
          code <<= 4;
          if (h >= '0' && h <= '9') code |= (h - '0');
          else if (h >= 'a' && h <= 'f') code |= (h - 'a' + 10);
          else if (h >= 'A' && h <= 'F') code |= (h - 'A' + 10);
          else return std::nullopt;
        }
        if (code <= 0x7F) out.push_back((char)code);
        else out += "?"; // keep it simple
        break;
      }
      default:
        out.push_back(e);
        break;
    }
  }
  return std::nullopt;
}

struct JsonValue {
  enum class Type { Null, Bool, Number, String, Object, Array };
  Type type{Type::Null};
  bool boolValue{false};
  std::string stringValue;
  std::vector<std::pair<std::string, JsonValue>> objectValue;
  std::vector<JsonValue> arrayValue;
};

static void skipJsonWs(const std::string& s, size_t& i) {
  while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) i++;
}

static bool parseJsonValue(const std::string& s, size_t& i, JsonValue& out);

static bool parseJsonObject(const std::string& s, size_t& i, JsonValue& out) {
  if (i >= s.size() || s[i] != '{') return false;
  i++;
  out = JsonValue{};
  out.type = JsonValue::Type::Object;
  skipJsonWs(s, i);
  if (i < s.size() && s[i] == '}') {
    i++;
    return true;
  }
  while (i < s.size()) {
    skipJsonWs(s, i);
    auto key = parseJsonStringAt(s, i);
    if (!key) return false;
    skipJsonWs(s, i);
    if (i >= s.size() || s[i] != ':') return false;
    i++;
    skipJsonWs(s, i);
    JsonValue val;
    if (!parseJsonValue(s, i, val)) return false;
    out.objectValue.push_back({*key, std::move(val)});
    skipJsonWs(s, i);
    if (i >= s.size()) return false;
    if (s[i] == '}') {
      i++;
      return true;
    }
    if (s[i] != ',') return false;
    i++;
  }
  return false;
}

static bool parseJsonArray(const std::string& s, size_t& i, JsonValue& out) {
  if (i >= s.size() || s[i] != '[') return false;
  i++;
  out = JsonValue{};
  out.type = JsonValue::Type::Array;
  skipJsonWs(s, i);
  if (i < s.size() && s[i] == ']') {
    i++;
    return true;
  }
  while (i < s.size()) {
    skipJsonWs(s, i);
    JsonValue val;
    if (!parseJsonValue(s, i, val)) return false;
    out.arrayValue.push_back(std::move(val));
    skipJsonWs(s, i);
    if (i >= s.size()) return false;
    if (s[i] == ']') {
      i++;
      return true;
    }
    if (s[i] != ',') return false;
    i++;
  }
  return false;
}

static bool parseJsonNumber(const std::string& s, size_t& i, JsonValue& out) {
  size_t start = i;
  if (i < s.size() && s[i] == '-') i++;
  if (i >= s.size()) return false;
  if (s[i] == '0') {
    i++;
  } else if (std::isdigit(static_cast<unsigned char>(s[i]))) {
    while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) i++;
  } else {
    return false;
  }

  if (i < s.size() && s[i] == '.') {
    i++;
    if (i >= s.size() || !std::isdigit(static_cast<unsigned char>(s[i]))) return false;
    while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) i++;
  }

  if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
    i++;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) i++;
    if (i >= s.size() || !std::isdigit(static_cast<unsigned char>(s[i]))) return false;
    while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) i++;
  }

  out = JsonValue{};
  out.type = JsonValue::Type::Number;
  out.stringValue = s.substr(start, i - start);
  return true;
}

static bool parseJsonValue(const std::string& s, size_t& i, JsonValue& out) {
  skipJsonWs(s, i);
  if (i >= s.size()) return false;
  if (s[i] == '"') {
    auto v = parseJsonStringAt(s, i);
    if (!v) return false;
    out = JsonValue{};
    out.type = JsonValue::Type::String;
    out.stringValue = *v;
    return true;
  }
  if (s[i] == '{') return parseJsonObject(s, i, out);
  if (s[i] == '[') return parseJsonArray(s, i, out);
  if (s.compare(i, 4, "true") == 0) {
    i += 4;
    out = JsonValue{};
    out.type = JsonValue::Type::Bool;
    out.boolValue = true;
    return true;
  }
  if (s.compare(i, 5, "false") == 0) {
    i += 5;
    out = JsonValue{};
    out.type = JsonValue::Type::Bool;
    out.boolValue = false;
    return true;
  }
  if (s.compare(i, 4, "null") == 0) {
    i += 4;
    out = JsonValue{};
    out.type = JsonValue::Type::Null;
    return true;
  }
  return parseJsonNumber(s, i, out);
}

static bool parseJsonDocument(const std::string& s, JsonValue& out) {
  size_t i = 0;
  if (!parseJsonValue(s, i, out)) return false;
  skipJsonWs(s, i);
  return i == s.size();
}

static const JsonValue* jsonObjectGet(const JsonValue* object, const std::string& key) {
  if (!object || object->type != JsonValue::Type::Object) return nullptr;
  for (const auto& [k, v] : object->objectValue) {
    if (k == key) return &v;
  }
  return nullptr;
}

static const JsonValue* jsonArrayGet(const JsonValue* array, size_t idx) {
  if (!array || array->type != JsonValue::Type::Array) return nullptr;
  if (idx >= array->arrayValue.size()) return nullptr;
  return &array->arrayValue[idx];
}

static std::optional<std::string> jsonString(const JsonValue* v) {
  if (!v || v->type != JsonValue::Type::String) return std::nullopt;
  return v->stringValue;
}

static std::optional<int> jsonInt(const JsonValue* v) {
  if (!v || v->type != JsonValue::Type::Number) return std::nullopt;
  char* end = nullptr;
  long num = std::strtol(v->stringValue.c_str(), &end, 10);
  if (!end || *end != '\0') return std::nullopt;
  return static_cast<int>(num);
}

static std::string jsonSerializeCompact(const JsonValue& v) {
  switch (v.type) {
    case JsonValue::Type::Null:
      return "null";
    case JsonValue::Type::Bool:
      return v.boolValue ? "true" : "false";
    case JsonValue::Type::Number:
      return v.stringValue;
    case JsonValue::Type::String:
      return "\"" + jsonEscape(v.stringValue) + "\"";
    case JsonValue::Type::Array: {
      std::ostringstream oss;
      oss << "[";
      for (size_t i = 0; i < v.arrayValue.size(); i++) {
        if (i > 0) oss << ",";
        oss << jsonSerializeCompact(v.arrayValue[i]);
      }
      oss << "]";
      return oss.str();
    }
    case JsonValue::Type::Object: {
      std::ostringstream oss;
      oss << "{";
      bool first = true;
      for (const auto& [k, child] : v.objectValue) {
        if (!first) oss << ",";
        first = false;
        oss << "\"" << jsonEscape(k) << "\":" << jsonSerializeCompact(child);
      }
      oss << "}";
      return oss.str();
    }
  }
  return "null";
}

static const JsonValue* extractStreamChoiceDelta(const std::string& json, JsonValue& rootOut) {
  if (!parseJsonDocument(json, rootOut)) return nullptr;
  const JsonValue* choices = jsonObjectGet(&rootOut, "choices");
  const JsonValue* first = jsonArrayGet(choices, 0);
  return jsonObjectGet(first, "delta");
}

static std::optional<std::string> extractStreamDeltaContent(const std::string& json) {
  JsonValue root;
  const JsonValue* delta = extractStreamChoiceDelta(json, root);
  return jsonString(jsonObjectGet(delta, "content"));
}

static std::optional<std::string> extractStreamDeltaReasoning(const std::string& json) {
  JsonValue root;
  const JsonValue* delta = extractStreamChoiceDelta(json, root);
  return jsonString(jsonObjectGet(delta, "reasoning"));
}

struct ToolCallDelta {
  int index{0};
  std::optional<std::string> id;
  std::optional<std::string> name;
  std::optional<std::string> argumentsChunk;
};

static std::vector<ToolCallDelta> extractStreamDeltaToolCalls(const std::string& json) {
  JsonValue root;
  const JsonValue* delta = extractStreamChoiceDelta(json, root);
  const JsonValue* toolCalls = jsonObjectGet(delta, "tool_calls");
  if (!toolCalls || toolCalls->type != JsonValue::Type::Array) return {};

  std::vector<ToolCallDelta> out;
  for (const auto& item : toolCalls->arrayValue) {
    if (item.type != JsonValue::Type::Object) continue;
    auto idx = jsonInt(jsonObjectGet(&item, "index"));
    if (!idx) continue;
    ToolCallDelta d;
    d.index = *idx;
    d.id = jsonString(jsonObjectGet(&item, "id"));
    const JsonValue* fn = jsonObjectGet(&item, "function");
    d.name = jsonString(jsonObjectGet(fn, "name"));
    d.argumentsChunk = jsonString(jsonObjectGet(fn, "arguments"));
    out.push_back(std::move(d));
  }
  return out;
}

static std::optional<std::string> extractStreamFinishReason(const std::string& json) {
  JsonValue root;
  if (!parseJsonDocument(json, root)) return std::nullopt;
  const JsonValue* choices = jsonObjectGet(&root, "choices");
  const JsonValue* first = jsonArrayGet(choices, 0);
  return jsonString(jsonObjectGet(first, "finish_reason"));
}

static std::optional<std::map<std::string, std::string>> parseToolArgumentsObject(const std::string& argumentsJson) {
  JsonValue root;
  if (!parseJsonDocument(argumentsJson, root)) return std::nullopt;
  if (root.type != JsonValue::Type::Object) return std::nullopt;
  std::map<std::string, std::string> args;
  for (const auto& [k, v] : root.objectValue) {
    if (v.type == JsonValue::Type::String || v.type == JsonValue::Type::Number) {
      args[k] = v.stringValue;
    } else if (v.type == JsonValue::Type::Bool) {
      args[k] = v.boolValue ? "true" : "false";
    } else if (v.type == JsonValue::Type::Null) {
      args[k] = "";
    } else {
      args[k] = jsonSerializeCompact(v);
    }
  }
  return args;
}

// ------------------------- ANSI + streaming markdown-ish printer -------------------------

struct Ansi {
  bool enable{true};
  std::string reset() const { return enable ? "\x1b[0m" : ""; }
  std::string bold() const { return enable ? "\x1b[1m" : ""; }
  std::string cyan() const { return enable ? "\x1b[36m" : ""; }
  std::string magenta() const { return enable ? "\x1b[35m" : ""; }
  std::string yellow() const { return enable ? "\x1b[33m" : ""; }
  std::string green() const { return enable ? "\x1b[32m" : ""; }
  std::string gray() const { return enable ? "\x1b[90m" : ""; }
};

struct ReasoningWindow {
  size_t width{20};
  size_t height{3};
  bool render{false};
  bool printed{false};
  bool allocated{false};
  std::deque<std::string> lines;
  std::string current;

  ReasoningWindow() = default;
  ReasoningWindow(size_t w, size_t h) : width(w), height(h) {}

  void append(const std::string& text) {
    for (char c : text) {
      if (c == '\r') continue;
      if (c == '\n') {
        pushLine();
        continue;
      }
      if (current.size() >= width) pushLine();
      current.push_back(c);
    }
    if (render) draw();
  }

  void reset() {
    lines.clear();
    current.clear();
    printed = false;
  }

  std::vector<std::string> snapshotLines() const {
    std::deque<std::string> all = lines;
    if (!current.empty() || all.empty()) all.push_back(current);

    std::vector<std::string> out(height, "");
    size_t start = (all.size() > height) ? (all.size() - height) : 0;
    size_t outIdx = height - (all.size() - start);
    for (size_t i = start; i < all.size() && outIdx < height; i++, outIdx++) {
      out[outIdx] = all[i];
    }
    return out;
  }

  void draw() {
    if (!render) return;
    ensureAllocated();
    std::vector<std::string> view = snapshotLines();
    if (printed) moveCursorUp(height - 1);
    for (size_t i = 0; i < height; i++) {
      clearLine();
      std::string line = view[i];
      if (line.size() > width) line.resize(width);
      if (line.size() < width) line.append(width - line.size(), ' ');
      std::cout << line;
      if (i + 1 < height) moveDown();
    }
    std::cout.flush();
    printed = true;
  }

  void clearDisplay() {
    if (!render || !printed) return;
    moveCursorUp(height - 1);
    for (size_t i = 0; i < height; i++) {
      clearLine();
      if (i + 1 < height) moveDown();
    }
    moveCursorUp(height - 1);
    std::cout.flush();
    printed = false;
  }

private:
  void pushLine() {
    lines.push_back(current);
    current.clear();
    while (lines.size() > height) lines.pop_front();
  }

  void ensureAllocated() {
    if (allocated) return;
    for (size_t i = 0; i < height; i++) {
      std::cout << std::string(width, ' ');
      if (i + 1 < height) std::cout << "\n";
    }
    std::cout.flush();
    moveCursorUp(height - 1);
    allocated = true;
  }

  void moveCursorUp(size_t n) const {
    if (n == 0) return;
    std::cout << "\x1b[" << n << "F";
  }

  void moveDown() const {
    std::cout << "\x1b[1E";
  }

  void clearLine() const {
    std::cout << "\x1b[2K";
  }
};

struct StreamingMarkdownPrinter {
  Ansi ansi;
  bool raw{false};
  bool enableColor{true};

  // markdown-ish state
  bool inFence{false};       // between ``` fences
  bool inInlineCode{false};  // between `...`
  bool inBold{false};        // between **...**
  bool atLineStart{true};

  // line handling
  enum class LineMode { Unknown, Normal, Heading, Quote, FenceLine, FenceContent };
  LineMode lineMode{LineMode::Unknown};

  // we buffer up to 3 backticks at the start of a line to detect ``` fences
  std::string startBuf;

  // for detecting ** across chunk boundaries
  bool pendingStar{false};

  void writeRaw(const std::string& s) {
    std::cout << s;
    std::cout.flush();
  }

  void applyNonCodeStyle() {
    if (!enableColor) return;
    std::cout << ansi.reset();
    // base style for the line:
    if (lineMode == LineMode::Quote) std::cout << ansi.gray();
    if (inBold) std::cout << ansi.bold();
  }

  void beginLineMode(LineMode m) {
    lineMode = m;
    if (!enableColor) return;

    switch (m) {
      case LineMode::Heading:
        std::cout << ansi.cyan() << ansi.bold();
        break;
      case LineMode::Quote:
        std::cout << ansi.gray();
        if (inBold) std::cout << ansi.bold();
        break;
      case LineMode::FenceLine:
        std::cout << ansi.magenta();
        break;
      case LineMode::FenceContent:
        std::cout << ansi.green();
        break;
      case LineMode::Normal:
      case LineMode::Unknown:
        applyNonCodeStyle();
        break;
    }
  }

  void endLine() {
    if (!enableColor) return;
    std::cout << ansi.reset();
  }

  void handleInlineChar(char c) {
    // Called only for Normal/Quote lines (not inFence, not heading/fence line)
    if (!enableColor) {
      std::cout << c;
      return;
    }

    // pending single '*'
    if (pendingStar) {
      if (!inInlineCode && c == '*') {
        // ** toggle bold
        pendingStar = false;
        inBold = !inBold;
        applyNonCodeStyle();
        return;
      } else {
        // it was just a single '*'
        std::cout << '*';
        pendingStar = false;
        // continue processing current char normally
      }
    }

    if (!inInlineCode && c == '*') {
      pendingStar = true;
      return;
    }

    if (c == '`') {
      inInlineCode = !inInlineCode;
      if (inInlineCode) {
        std::cout << ansi.yellow();
      } else {
        applyNonCodeStyle();
      }
      return;
    }

    std::cout << c;
  }

  void flushStartBufAsInline() {
    for (char b : startBuf) handleInlineChar(b);
    startBuf.clear();
  }

  void handleNewline() {
    // flush pending star as literal
    if (pendingStar) {
      std::cout << '*';
      pendingStar = false;
    }
    endLine();
    std::cout << "\n";
    std::cout.flush();

    atLineStart = true;
    lineMode = LineMode::Unknown;
    startBuf.clear();

    // inline state (bold/code) can technically span lines; we keep it
    // fence state persists naturally
  }

  void write(const std::string& s) {
    if (raw || !enableColor) {
      // Still handle newlines so output flushes frequently
      for (char c : s) {
        std::cout << c;
        if (c == '\n') std::cout.flush();
      }
      std::cout.flush();
      return;
    }

    for (size_t idx = 0; idx < s.size(); idx++) {
      char c = s[idx];

      if (c == '\n') {
        // if we were buffering backticks at line start and never decided, treat them as normal
        if (!startBuf.empty() && lineMode == LineMode::Unknown) {
          if (inFence) beginLineMode(LineMode::FenceContent);
          else beginLineMode(LineMode::Normal);
          atLineStart = false;
          flushStartBufAsInline();
        }
        // if we’re on a fence delimiter line, toggle fence at newline
        if (lineMode == LineMode::FenceLine) {
          // endLine() happens in handleNewline()
          inFence = !inFence;
        }
        handleNewline();
        continue;
      }

      if (atLineStart) {
        // We decide line mode based on first chars.
        if (lineMode == LineMode::Unknown) {
          // fence detection: buffer leading backticks
          if (startBuf.empty()) {
            if (c == '`') {
              startBuf.push_back(c);
              continue;
            }
            if (!inFence && c == '#') {
              atLineStart = false;
              beginLineMode(LineMode::Heading);
              std::cout << c;
              std::cout.flush();
              continue;
            }
            if (!inFence && c == '>') {
              atLineStart = false;
              beginLineMode(LineMode::Quote);
              handleInlineChar(c);
              std::cout.flush();
              continue;
            }
            // otherwise: normal or fence content
            atLineStart = false;
            beginLineMode(inFence ? LineMode::FenceContent : LineMode::Normal);
            if (lineMode == LineMode::FenceContent) {
              std::cout << c;
            } else {
              handleInlineChar(c);
            }
            std::cout.flush();
            continue;
          } else {
            // we already saw 1-2 leading backticks
            if (c == '`' && startBuf.size() < 3) {
              startBuf.push_back('`');
              if (startBuf.size() == 3) {
                atLineStart = false;
                beginLineMode(LineMode::FenceLine);
                std::cout << "```";
                startBuf.clear();
                std::cout.flush();
              }
              continue;
            } else {
              // not a fence delimiter; decide normal/fence content and flush buffered backticks as inline
              atLineStart = false;
              beginLineMode(inFence ? LineMode::FenceContent : LineMode::Normal);
              if (lineMode == LineMode::FenceContent) {
                // in fence content, backticks are literal
                std::cout << startBuf;
                startBuf.clear();
                std::cout << c;
              } else {
                flushStartBufAsInline();
                handleInlineChar(c);
              }
              std::cout.flush();
              continue;
            }
          }
        }
      }

      // not line start (or line mode already decided)
      switch (lineMode) {
        case LineMode::FenceLine:
          std::cout << c; // magenta
          break;
        case LineMode::FenceContent:
          std::cout << c; // green
          break;
        case LineMode::Heading:
          std::cout << c; // cyan bold
          break;
        case LineMode::Quote:
        case LineMode::Normal:
        case LineMode::Unknown:
          handleInlineChar(c);
          break;
      }
      std::cout.flush();
    }
  }

  void finalize() {
    // If output ends mid-line, flush pending star and reset styles.
    if (!raw && enableColor) {
      if (pendingStar) {
        std::cout << '*';
        pendingStar = false;
      }
      endLine();
      std::cout.flush();
    }
  }
};

// ------------------------- conversation history (text file in /tmp) -------------------------

static const char* kUserMarker = "<<<USER>>>";
static const char* kAssistantMarker = "<<<ASSISTANT>>>";

static std::vector<std::pair<std::string, std::string>> loadHistory(const std::string& path) {
  std::ifstream f(path);
  if (!f) return {};

  std::vector<std::pair<std::string, std::string>> msgs;
  std::string line;
  std::string curRole;
  std::ostringstream cur;

  auto flush = [&]() {
    if (!curRole.empty()) {
      std::string content = cur.str();
      // trim a single trailing newline for neatness
      if (!content.empty() && content.back() == '\n') content.pop_back();
      msgs.push_back({curRole, content});
    }
    curRole.clear();
    cur.str("");
    cur.clear();
  };

  while (std::getline(f, line)) {
    if (line == kUserMarker) {
      flush();
      curRole = "user";
      continue;
    }
    if (line == kAssistantMarker) {
      flush();
      curRole = "assistant";
      continue;
    }
    if (!curRole.empty()) {
      cur << line << "\n";
    }
  }
  flush();
  return msgs;
}

static bool wipeHistory(const std::string& path) {
  std::ofstream f(path, std::ios::trunc);
  return (bool)f;
}

static bool appendTurn(const std::string& path, const std::string& userMsg, const std::string& assistantMsg) {
  std::ofstream f(path, std::ios::app);
  if (!f) return false;
  f << kUserMarker << "\n" << userMsg << "\n";
  f << kAssistantMarker << "\n" << assistantMsg << "\n";
  return true;
}

// ------------------------- build request -------------------------

static std::string buildChatRequestJson(
    const std::string& model,
    const std::vector<ChatMessage>& messages,
    double temperature,
    bool stream,
    const std::vector<ToolSpec>* tools
) {
  auto findInputDesc = [](const ToolSpec& tool, const std::string& inputName) -> std::optional<std::string> {
    for (const auto& input : tool.inputs) {
      if (input.name == inputName) return input.description;
    }
    return std::nullopt;
  };

  std::ostringstream oss;
  oss << "{";
  oss << "\"model\":\"" << jsonEscape(model) << "\",";
  oss << "\"messages\":[";
  bool firstMsg = true;

  auto addMessage = [&](const ChatMessage& msg) {
    if (!firstMsg) oss << ",";
    firstMsg = false;
    oss << "{";
    oss << "\"role\":\"" << jsonEscape(msg.role) << "\"";
    if (msg.role == "assistant" && !msg.toolCalls.empty()) {
      oss << ",\"content\":\"" << jsonEscape(msg.content) << "\"";
      oss << ",\"tool_calls\":[";
      bool firstCall = true;
      for (const auto& call : msg.toolCalls) {
        if (!firstCall) oss << ",";
        firstCall = false;
        oss << "{";
        oss << "\"id\":\"" << jsonEscape(call.id) << "\",";
        oss << "\"type\":\"function\",";
        oss << "\"function\":{";
        oss << "\"name\":\"" << jsonEscape(call.name) << "\",";
        oss << "\"arguments\":\"" << jsonEscape(call.argumentsJson) << "\"";
        oss << "}";
        oss << "}";
      }
      oss << "]";
    } else if (msg.role == "tool") {
      oss << ",\"tool_call_id\":\"" << jsonEscape(msg.toolCallId) << "\"";
      oss << ",\"content\":\"" << jsonEscape(msg.content) << "\"";
    } else {
      oss << ",\"content\":\"" << jsonEscape(msg.content) << "\"";
    }
    oss << "}";
  };

  for (const auto& msg : messages) addMessage(msg);

  oss << "],";
  oss << "\"temperature\":" << temperature << ",";
  oss << "\"stream\":" << (stream ? "true" : "false");

  if (tools && !tools->empty()) {
    oss << ",\"tools\":[";
    bool firstTool = true;
    for (const auto& tool : *tools) {
      if (!firstTool) oss << ",";
      firstTool = false;
      std::vector<std::string> vars = extractTemplateVariables(tool.command);
      std::set<std::string> seen(vars.begin(), vars.end());
      for (const auto& input : tool.inputs) {
        if (seen.count(input.name)) continue;
        vars.push_back(input.name);
        seen.insert(input.name);
      }

      oss << "{";
      oss << "\"type\":\"function\",";
      oss << "\"function\":{";
      oss << "\"name\":\"" << jsonEscape(tool.name) << "\",";
      oss << "\"description\":\"" << jsonEscape(tool.description) << "\",";
      oss << "\"parameters\":{";
      oss << "\"type\":\"object\",";
      oss << "\"properties\":{";
      bool firstProp = true;
      for (const auto& var : vars) {
        if (!firstProp) oss << ",";
        firstProp = false;
        oss << "\"" << jsonEscape(var) << "\":{";
        oss << "\"type\":\"string\"";
        auto desc = findInputDesc(tool, var);
        if (desc && !desc->empty()) {
          oss << ",\"description\":\"" << jsonEscape(*desc) << "\"";
        }
        oss << "}";
      }
      oss << "},";
      oss << "\"required\":[";
      bool firstRequired = true;
      std::vector<std::string> required = extractTemplateVariables(tool.command);
      for (const auto& var : required) {
        if (!firstRequired) oss << ",";
        firstRequired = false;
        oss << "\"" << jsonEscape(var) << "\"";
      }
      oss << "],";
      oss << "\"additionalProperties\":false";
      oss << "}";
      oss << "}";
      oss << "}";
    }
    oss << "]";
    oss << ",\"tool_choice\":\"auto\"";
  }

  oss << "}";
  return oss.str();
}

static std::string buildChatRequestJson(
    const std::string& model,
    const std::optional<std::string>& systemPrompt,
    const std::vector<std::pair<std::string, std::string>>& history,
    const std::string& userPrompt,
    double temperature,
    bool stream,
    const std::vector<ToolSpec>* tools = nullptr
) {
  std::vector<ChatMessage> messages;
  if (systemPrompt && !systemPrompt->empty()) {
    messages.push_back({"system", *systemPrompt, {}, ""});
  }
  for (const auto& [role, content] : history) {
    if (role == "user" || role == "assistant") {
      messages.push_back({role, content, {}, ""});
    }
  }
  messages.push_back({"user", userPrompt, {}, ""});
  return buildChatRequestJson(model, messages, temperature, stream, tools);
}

// ------------------------- curl streaming (SSE) -------------------------

struct StreamState {
  struct ToolCallAccum {
    bool used{false};
    std::string id;
    std::string name;
    std::string arguments;
  };

  std::string sseBuf;
  std::string rawAll;          // keep for debugging / non-2xx
  std::string assistantAll;    // accumulated assistant content
  std::string finishReason;
  std::vector<ToolCallAccum> toolCallAccums;
  bool sawDone{false};
  bool responseStarted{false};
  StreamingMarkdownPrinter printer;
  ReasoningWindow reasoningWindow;
  bool useCallbacks{false};
  std::function<void(const std::string&)> onReasoning;
  std::function<void()> onContentStart;
  std::function<void(const std::string&)> onContent;
};

static std::vector<ToolCall> collectToolCallsFromStream(const StreamState& st) {
  std::vector<ToolCall> out;
  for (const auto& accum : st.toolCallAccums) {
    if (!accum.used || accum.name.empty()) continue;
    ToolCall call;
    call.id = accum.id.empty() ? ("call_" + accum.name) : accum.id;
    call.name = accum.name;
    call.argumentsJson = accum.arguments.empty() ? "{}" : accum.arguments;
    out.push_back(std::move(call));
  }
  return out;
}

static size_t sseWriteCb(char* ptr, size_t size, size_t nmemb, void* userdata) {
  auto* st = reinterpret_cast<StreamState*>(userdata);
  const size_t n = size * nmemb;
  st->rawAll.append(ptr, ptr + n);
  st->sseBuf.append(ptr, ptr + n);

  // Process complete lines. SSE uses lines separated by \n.
  while (true) {
    size_t nl = st->sseBuf.find('\n');
    if (nl == std::string::npos) break;
    std::string line = st->sseBuf.substr(0, nl);
    st->sseBuf.erase(0, nl + 1);

    // strip trailing \r
    if (!line.empty() && line.back() == '\r') line.pop_back();

    // ignore empty lines / comments
    if (line.empty()) continue;

    const std::string prefix = "data: ";
    if (line.rfind(prefix, 0) != 0) {
      // ignore e.g. "event: ..." lines
      continue;
    }

    std::string payload = line.substr(prefix.size());
    if (payload == "[DONE]") {
      st->sawDone = true;
      continue;
    }

    auto finishReason = extractStreamFinishReason(payload);
    if (finishReason) st->finishReason = *finishReason;

    auto toolCallDeltas = extractStreamDeltaToolCalls(payload);
    for (const auto& tc : toolCallDeltas) {
      if (tc.index < 0) continue;
      if (st->toolCallAccums.size() <= static_cast<size_t>(tc.index)) {
        st->toolCallAccums.resize(static_cast<size_t>(tc.index) + 1);
      }
      auto& accum = st->toolCallAccums[static_cast<size_t>(tc.index)];
      accum.used = true;
      if (tc.id && !tc.id->empty()) accum.id = *tc.id;
      if (tc.name && !tc.name->empty()) accum.name = *tc.name;
      if (tc.argumentsChunk) accum.arguments += *tc.argumentsChunk;
    }

    auto delta = extractStreamDeltaContent(payload);
    auto reasoning = extractStreamDeltaReasoning(payload);

    if (st->useCallbacks) {
      if (reasoning && !st->responseStarted && st->onReasoning) {
        st->onReasoning(*reasoning);
      }

      if (delta && !delta->empty()) {
        if (!st->responseStarted) {
          if (st->onContentStart) st->onContentStart();
          st->responseStarted = true;
        }
        st->assistantAll += *delta;
        if (st->onContent) st->onContent(*delta);
      }
      continue;
    }

    if (reasoning && !st->responseStarted) {
      st->reasoningWindow.append(*reasoning);
    }

    if (delta && !delta->empty()) {
      if (!st->responseStarted) {
        st->reasoningWindow.clearDisplay();
        st->reasoningWindow.reset();
        st->responseStarted = true;
      }
      st->assistantAll += *delta;
      st->printer.write(*delta);
    }
  }

  return n;
}

static bool httpPostJsonStream(
    const std::string& url,
    const std::string& body,
    const std::optional<std::string>& bearerToken,
    long& httpCodeOut,
    std::string& curlErrOut,
    StreamState& st
) {
  CURL* curl = curl_easy_init();
  if (!curl) { curlErrOut = "curl_easy_init failed"; return false; }

  struct curl_slist* headers = nullptr;
  headers = curl_slist_append(headers, "Content-Type: application/json");
  headers = curl_slist_append(headers, "Accept: text/event-stream");
  if (bearerToken && !bearerToken->empty()) {
    std::string auth = "Authorization: Bearer " + *bearerToken;
    headers = curl_slist_append(headers, auth.c_str());
  }

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body.size());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, sseWriteCb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &st);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "lmstudio-cli-cpp/2.0");
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L);         // streaming: no overall timeout
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
  curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);

  // Reduce buffering latency
  curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 1024L);

  CURLcode rc = curl_easy_perform(curl);
  if (rc != CURLE_OK) {
    curlErrOut = curl_easy_strerror(rc);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return false;
  }

  long httpCode = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
  httpCodeOut = httpCode;

  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  return true;
}

struct ToolRunRecord {
  std::string toolName;
  std::string toolCallId;
  std::string inputJson;
  std::string renderedCommand;
  CommandRunResult commandResult;
  bool toolFound{true};
  std::string errorText;
};

static const ToolSpec* findToolByName(const std::vector<ToolSpec>& tools, const std::string& name) {
  for (const auto& tool : tools) {
    if (tool.name == name) return &tool;
  }
  return nullptr;
}

static std::string buildToolResultContent(const ToolRunRecord& run) {
  std::ostringstream oss;
  if (!run.toolFound) {
    oss << "{"
        << "\"error\":\"" << jsonEscape(run.errorText) << "\""
        << "}";
    return oss.str();
  }
  if (run.commandResult.launchFailed) {
    oss << "{"
        << "\"error\":\"" << jsonEscape(run.commandResult.launchError) << "\","
        << "\"command\":\"" << jsonEscape(run.renderedCommand) << "\""
        << "}";
    return oss.str();
  }
  oss << "{";
  oss << "\"command\":\"" << jsonEscape(run.renderedCommand) << "\",";
  oss << "\"exit_code\":" << run.commandResult.exitCode << ",";
  oss << "\"stdout\":\"" << jsonEscape(run.commandResult.stdoutText) << "\",";
  oss << "\"stderr\":\"" << jsonEscape(run.commandResult.stderrText) << "\"";
  oss << "}";
  return oss.str();
}

static std::string buildToolInteractionDisplay(const ToolRunRecord& run, size_t sectionLimit = 600) {
  std::ostringstream oss;
  oss << "tool: " << run.toolName << "\n";
  oss << "input:\n" << truncateForDisplay(run.inputJson, sectionLimit) << "\n";
  if (!run.toolFound) {
    oss << "error:\n" << run.errorText << "\n";
    return oss.str();
  }
  oss << "command:\n" << truncateForDisplay(run.renderedCommand, sectionLimit) << "\n";
  if (run.commandResult.launchFailed) {
    oss << "error:\n" << run.commandResult.launchError << "\n";
    return oss.str();
  }
  oss << "exit_code: " << run.commandResult.exitCode << "\n";
  oss << "stdout:\n" << truncateForDisplay(run.commandResult.stdoutText, sectionLimit) << "\n";
  oss << "stderr:\n" << truncateForDisplay(run.commandResult.stderrText, sectionLimit) << "\n";
  return oss.str();
}

static ToolRunRecord executeToolCall(
    const ToolCall& call,
    const std::vector<ToolSpec>& toolSpecs
) {
  ToolRunRecord run;
  run.toolName = call.name;
  run.toolCallId = call.id;
  run.inputJson = call.argumentsJson;

  const ToolSpec* spec = findToolByName(toolSpecs, call.name);
  if (!spec) {
    run.toolFound = false;
    run.errorText = "unknown tool '" + call.name + "'";
    return run;
  }

  auto args = parseToolArgumentsObject(call.argumentsJson);
  if (!args) {
    run.toolFound = false;
    run.errorText = "invalid tool arguments JSON";
    return run;
  }

  run.renderedCommand = renderToolCommand(spec->command, *args);
  run.commandResult = runCommandCaptureSplit(run.renderedCommand);
  return run;
}

static std::vector<std::string> wrapTextChars(const std::string& text, int width);

static std::vector<std::string> splitLines(const std::string& text) {
  std::vector<std::string> lines;
  std::string cur;
  for (char c : text) {
    if (c == '\n') {
      lines.push_back(cur);
      cur.clear();
      continue;
    }
    if (c != '\r') cur.push_back(c);
  }
  lines.push_back(cur);
  return lines;
}

static std::vector<std::string> buildBorderedLines(const std::string& text, int width) {
  int innerWidth = std::max(1, width - 2);
  std::vector<std::string> out;
  out.push_back("+" + std::string(innerWidth, '-') + "+");
  auto rawLines = splitLines(text);
  for (const auto& raw : rawLines) {
    auto wrapped = wrapTextChars(raw, innerWidth);
    for (const auto& line : wrapped) {
      std::string l = line;
      if ((int)l.size() < innerWidth) l.append(innerWidth - (int)l.size(), ' ');
      out.push_back("|" + l + "|");
    }
  }
  out.push_back("+" + std::string(innerWidth, '-') + "+");
  return out;
}

static int terminalWidthGuess() {
  std::string cols = getEnv("COLUMNS");
  if (!cols.empty()) {
    int parsed = std::atoi(cols.c_str());
    if (parsed > 10) return parsed;
  }
  return 100;
}

static void printBorderedBlockStdout(const std::string& text, int width) {
  auto lines = buildBorderedLines(text, width);
  for (const auto& line : lines) {
    std::cout << line << "\n";
  }
  std::cout.flush();
}

// ------------------------- TUI helpers -------------------------

static std::vector<std::string> wrapTextWords(const std::string& text, int width) {
  std::vector<std::string> lines;
  if (width <= 0) return lines;

  std::string line;
  std::string word;

  auto flushWord = [&]() {
    if (word.empty()) return;
    if ((int)line.size() == 0) {
      if ((int)word.size() <= width) {
        line = word;
      } else {
        size_t i = 0;
        while (i < word.size()) {
          size_t take = std::min<size_t>(width, word.size() - i);
          lines.push_back(word.substr(i, take));
          i += take;
        }
      }
    } else {
      if ((int)(line.size() + 1 + word.size()) <= width) {
        line += " " + word;
      } else {
        lines.push_back(line);
        line.clear();
        if ((int)word.size() <= width) {
          line = word;
        } else {
          size_t i = 0;
          while (i < word.size()) {
            size_t take = std::min<size_t>(width, word.size() - i);
            lines.push_back(word.substr(i, take));
            i += take;
          }
        }
      }
    }
    word.clear();
  };

  for (char c : text) {
    if (c == '\n') {
      flushWord();
      lines.push_back(line);
      line.clear();
      continue;
    }
    if (std::isspace(static_cast<unsigned char>(c))) {
      flushWord();
    } else {
      word.push_back(c);
    }
  }
  flushWord();
  if (!line.empty() || lines.empty()) lines.push_back(line);
  return lines;
}

static std::vector<std::string> wrapTextChars(const std::string& text, int width) {
  std::vector<std::string> lines;
  if (width <= 0) return lines;
  std::string line;
  for (char c : text) {
    if (c == '\n') {
      lines.push_back(line);
      line.clear();
      continue;
    }
    line.push_back(c);
    if ((int)line.size() >= width) {
      lines.push_back(line);
      line.clear();
    }
  }
  if (!line.empty() || lines.empty()) lines.push_back(line);
  return lines;
}

struct TuiMessage {
  std::string role;
  std::string content;
};

struct TuiRenderLine {
  std::string text;
  int colorPair{0};
};

class TuiApp {
public:
  TuiApp(std::string baseUrl,
         std::string model,
         std::string apiKey,
         std::optional<std::string> systemPrompt,
         std::vector<ToolSpec> toolSpecs)
      : baseUrl_(std::move(baseUrl)),
        model_(std::move(model)),
        apiKey_(std::move(apiKey)),
        systemPrompt_(std::move(systemPrompt)),
        toolSpecs_(std::move(toolSpecs)) {
    if (systemPrompt_ && !systemPrompt_->empty()) {
      historyMessages_.push_back({"system", *systemPrompt_, {}, ""});
    }
  }

  int run() {
    setlocale(LC_ALL, "");
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(1);
    if (has_colors()) {
      start_color();
      use_default_colors();
      init_pair(kColorAssistant, COLOR_RED, -1);
      init_pair(kColorUser, COLOR_GREEN, -1);
      init_pair(kColorReasoning, COLOR_BLUE, -1);
      init_pair(kColorTool, COLOR_CYAN, -1);
    }

    bool running = true;
    while (running) {
      render();
      int ch = getch();
      if (ch == 3) { // Ctrl+C
        running = false;
        continue;
      }
      if (ch == KEY_PPAGE) {
        scrollBy(-lastChatHeight_);
        continue;
      }
      if (ch == KEY_NPAGE) {
        scrollBy(lastChatHeight_);
        continue;
      }
      if (ch == KEY_RESIZE) {
        continue;
      }
      if (ch == '\n' || ch == KEY_ENTER) {
        if (!input_.empty()) {
          std::string prompt = input_;
          input_.clear();
          sendPrompt(prompt);
        }
        continue;
      }
      if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
        if (!input_.empty()) input_.pop_back();
        continue;
      }
      if (std::isprint(ch)) {
        input_.push_back(static_cast<char>(ch));
      }
    }

    endwin();
    return 0;
  }

  void appendReasoning(const std::string& chunk) {
    reasoningText_ += chunk;
    showReasoning_ = true;
    render();
  }

  void clearReasoning() {
    reasoningText_.clear();
    showReasoning_ = false;
  }

  void appendAssistantDelta(const std::string& chunk) {
    if (currentAssistantIndex_ < messages_.size()) {
      messages_[currentAssistantIndex_].content += chunk;
    }
    stickToBottom_ = true;
    render();
  }

private:
  static constexpr int kColorAssistant = 1;
  static constexpr int kColorUser = 2;
  static constexpr int kColorReasoning = 3;
  static constexpr int kColorTool = 4;
  static constexpr const char* kHelpText =
      "PgUp/PgDn scroll | Enter send | Ctrl+C quit | /clear clears";

  std::string baseUrl_;
  std::string model_;
  std::string apiKey_;
  std::optional<std::string> systemPrompt_;
  std::vector<ToolSpec> toolSpecs_;
  std::vector<TuiMessage> messages_;
  std::vector<ChatMessage> historyMessages_;
  std::string input_;
  std::string reasoningText_;
  bool showReasoning_{false};
  bool responseStarted_{false};
  size_t currentAssistantIndex_{0};
  int scrollTop_{0};
  int lastChatHeight_{1};
  bool stickToBottom_{true};

  void scrollBy(int delta) {
    stickToBottom_ = false;
    scrollTop_ += delta;
    if (scrollTop_ < 0) scrollTop_ = 0;
  }

  std::vector<TuiRenderLine> buildChatLines(
      int width,
      const std::vector<std::string>& reasoningLines,
      bool showReasoning
  ) const {
    std::vector<TuiRenderLine> lines;
    int wrapWidth = std::max(1, width - 2);
    for (const auto& msg : messages_) {
      if (msg.role == "tool") {
        auto boxed = buildBorderedLines(msg.content, width);
        for (const auto& line : boxed) {
          lines.push_back({line, kColorTool});
        }
        lines.push_back({"", 0});
        continue;
      }
      auto wrapped = wrapTextWords(msg.content, wrapWidth);
      for (const auto& line : wrapped) {
        TuiRenderLine out;
        out.colorPair = (msg.role == "user") ? kColorUser : kColorAssistant;
        if (msg.role == "user") {
          int indent = std::max(0, width - (int)line.size());
          out.text = std::string(indent, ' ') + line;
        } else {
          out.text = line;
        }
        lines.push_back(std::move(out));
      }
      lines.push_back({"", 0});
    }
    if (showReasoning) {
      for (const auto& line : reasoningLines) {
        lines.push_back({line, kColorReasoning});
      }
    }
    if (!lines.empty() && lines.back().text.empty()) lines.pop_back();
    return lines;
  }

  void render() {
    int h = 0;
    int w = 0;
    getmaxyx(stdscr, h, w);
    if (h <= 0 || w <= 0) return;

    std::string inputDisplay = "> " + input_;
    auto inputLines = wrapTextChars(inputDisplay, w);
    int inputHeight = std::max(1, (int)inputLines.size());
    if (inputHeight >= h) {
      inputHeight = std::max(1, h - 1);
    }
    lastChatHeight_ = std::max(1, h - inputHeight);

    erase();

    std::vector<std::string> reasoningLines(3, "");
    if (showReasoning_) {
      int reasoningWidth = std::max(1, w / 2);
      int contentWidth = std::max(1, reasoningWidth - 2);
      auto rlines = wrapTextChars(reasoningText_, contentWidth);
      size_t start = (rlines.size() > 2) ? (rlines.size() - 2) : 0;
      size_t outIdx = 1 + (2 - (rlines.size() - start));
      for (size_t i = start; i < rlines.size() && outIdx < 3; i++, outIdx++) {
        reasoningLines[outIdx] = rlines[i];
      }
      std::string label = "thinking...";
      if ((int)label.size() > contentWidth) label.resize(contentWidth);
      std::string top = label;
      if ((int)top.size() < contentWidth) top.append(contentWidth - top.size(), ' ');
      reasoningLines[0] = top;
      for (int i = 0; i < 3; i++) {
        std::string line = reasoningLines[i];
        if ((int)line.size() < contentWidth) line.append(contentWidth - line.size(), ' ');
        reasoningLines[i] = "|" + line + "|";
      }
    }

    auto chatLines = buildChatLines(w, reasoningLines, showReasoning_);
    int chatStartRow = 0;
    int chatHeight = lastChatHeight_;
    if (lastChatHeight_ >= 2) {
      chatStartRow = 1;
      chatHeight = lastChatHeight_ - 1;
      mvaddnstr(0, 0, kHelpText, w);
    }

    int maxTop = std::max(0, (int)chatLines.size() - chatHeight);
    if (stickToBottom_) scrollTop_ = maxTop;
    if (scrollTop_ > maxTop) scrollTop_ = maxTop;

    for (int row = 0; row < chatHeight; row++) {
      int idx = scrollTop_ + row;
      if (idx >= (int)chatLines.size()) break;
      const auto& line = chatLines[idx];
      if (line.colorPair > 0 && has_colors()) attron(COLOR_PAIR(line.colorPair));
      mvaddnstr(chatStartRow + row, 0, line.text.c_str(), w);
      if (line.colorPair > 0 && has_colors()) attroff(COLOR_PAIR(line.colorPair));
    }

    int inputStart = lastChatHeight_;
    if ((int)inputLines.size() > inputHeight) {
      int skip = (int)inputLines.size() - inputHeight;
      inputLines.erase(inputLines.begin(), inputLines.begin() + skip);
    }
    for (int i = 0; i < inputHeight && i < (int)inputLines.size(); i++) {
      mvaddnstr(inputStart + i, 0, inputLines[i].c_str(), w);
    }

    int cursorRow = inputStart + std::max(0, (int)inputLines.size() - 1);
    int cursorCol = inputLines.empty() ? 0 : (int)inputLines.back().size();
    if (cursorRow >= h) cursorRow = h - 1;
    if (cursorCol >= w) cursorCol = w - 1;
    move(cursorRow, cursorCol);

    refresh();
  }

  void sendPrompt(const std::string& prompt) {
    if (prompt == "/clear") {
      messages_.clear();
      historyMessages_.clear();
      if (systemPrompt_ && !systemPrompt_->empty()) {
        historyMessages_.push_back({"system", *systemPrompt_, {}, ""});
      }
      reasoningText_.clear();
      showReasoning_ = false;
      scrollTop_ = 0;
      stickToBottom_ = true;
      render();
      return;
    }

    messages_.push_back({"user", prompt});
    historyMessages_.push_back({"user", prompt, {}, ""});
    stickToBottom_ = true;

    std::string url = baseUrl_ + "/chat/completions";
    const int kMaxToolRounds = 800;
    for (int round = 0; round < kMaxToolRounds; round++) {
      messages_.push_back({"assistant", ""});
      currentAssistantIndex_ = messages_.size() - 1;
      reasoningText_.clear();
      showReasoning_ = false;
      responseStarted_ = false;
      render();

      StreamState st;
      st.useCallbacks = true;
      st.onReasoning = [&](const std::string& chunk) { appendReasoning(chunk); };
      st.onContentStart = [&]() {
        responseStarted_ = true;
        clearReasoning();
        render();
      };
      st.onContent = [&](const std::string& chunk) { appendAssistantDelta(chunk); };

      std::string body = buildChatRequestJson(
          model_,
          historyMessages_,
          /*temperature=*/0.7,
          /*stream=*/true,
          toolSpecs_.empty() ? nullptr : &toolSpecs_
      );

      long httpCode = 0;
      std::string err;
      bool ok = httpPostJsonStream(url, body,
                                   (apiKey_.empty() ? std::nullopt : std::optional<std::string>(apiKey_)),
                                   httpCode, err, st);

      clearReasoning();

      if (!ok) {
        messages_[currentAssistantIndex_].content = "[request failed: " + err + "]";
        render();
        return;
      }

      if (httpCode < 200 || httpCode >= 300) {
        messages_[currentAssistantIndex_].content = "[http " + std::to_string(httpCode) + "]";
        render();
        return;
      }

      auto toolCalls = collectToolCallsFromStream(st);
      historyMessages_.push_back({"assistant", st.assistantAll, toolCalls, ""});
      if (st.assistantAll.empty() && !toolCalls.empty() && currentAssistantIndex_ < messages_.size()) {
        auto eraseAt = messages_.begin() + static_cast<std::vector<TuiMessage>::difference_type>(currentAssistantIndex_);
        messages_.erase(eraseAt);
        currentAssistantIndex_ = messages_.size();
      }

      if (toolCalls.empty()) {
        render();
        return;
      }

      for (const auto& call : toolCalls) {
        ToolRunRecord run = executeToolCall(call, toolSpecs_);
        messages_.push_back({"tool", buildToolInteractionDisplay(run)});
        historyMessages_.push_back({"tool", buildToolResultContent(run), {}, run.toolCallId});
      }
      render();
    }

    messages_.push_back({"assistant", "[aborted: exceeded tool call round limit]"});
    render();
  }
};

// ------------------------- main -------------------------

static int runCli(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "usage: lm \"prompt\" [more words...]\n"
              << "          [--file path] [--cmd \"cmd\"] [--model model_id] [--skills path]\n"
              << "          [--new] [--no-color] [--raw]\n"
              << "          [--tui]\n\n"
              << "env:\n"
              << "  LM_BASE_URL        e.g. http://localhost:1234/v1\n"
              << "  LM_MODEL           optional model id\n"
              << "  LM_API_KEY         optional bearer token\n"
              << "  LM_SYSTEM_PROMPT   optional system prompt\n"
              << "  LM_HISTORY_FILE    optional history file path (default /tmp/lmstudio-cli.history.txt)\n"
              << "  LM_SKILLS_FILE     optional tools TOML file (default ~/.local/share/lm/skills.toml)\n";
    return 2;
  }

  std::string baseUrl = getEnv("LM_BASE_URL");
  if (baseUrl.empty()) {
    std::cerr << "error: LM_BASE_URL not set (e.g. export LM_BASE_URL=\"http://localhost:1234/v1\")\n";
    return 2;
  }
  baseUrl = trimTrailingSlash(baseUrl);

  std::string model = getEnv("LM_MODEL");
  std::string apiKey = getEnv("LM_API_KEY");
  std::string systemPromptEnv = getEnv("LM_SYSTEM_PROMPT");
  std::optional<std::string> systemPrompt = systemPromptEnv.empty() ? std::nullopt
                                                                    : std::optional<std::string>(systemPromptEnv);

  std::string historyPath = getEnv("LM_HISTORY_FILE");
  if (historyPath.empty()) historyPath = "/tmp/lmstudio-cli.history.txt";
  std::string skillsPath = getEnv("LM_SKILLS_FILE");
  if (skillsPath.empty()) skillsPath = "~/.local/share/lm/skills.toml";
  skillsPath = expandUserPath(skillsPath);

  bool forceNoColor = false;
  bool raw = false;
  bool newConv = false;

  // prompt is argv[1] (as per your original spec)
  std::string prompt = argv[1];

  for (int i = 2; i < argc; i++) {
    std::string arg = argv[i];

    if (arg == "--new") { newConv = true; continue; }
    if (arg == "--tui") { continue; }

    if (arg == "--file") {
      if (i + 1 >= argc) { std::cerr << "error: --file expects a path\n"; return 2; }
      std::string path = argv[++i];
      std::filesystem::path p(path);
      std::string name = p.filename().string();
      std::string contents = readFileAll(path);

      prompt += "\n\n=== " + name + "\n";
      prompt += contents;
      if (!prompt.empty() && prompt.back() != '\n') prompt.push_back('\n');
      continue;
    }

    if (arg == "--cmd") {
      if (i + 1 >= argc) { std::cerr << "error: --cmd expects a command string\n"; return 2; }
      std::string cmd = argv[++i];
      std::string out = runCommandCapture(cmd);

      prompt += "\n\n=== output of command " + cmd + "\n";
      prompt += out;
      if (!prompt.empty() && prompt.back() != '\n') prompt.push_back('\n');
      continue;
    }

    if (arg == "--model") {
      if (i + 1 >= argc) { std::cerr << "error: --model expects a model id\n"; return 2; }
      model = argv[++i];
      continue;
    }

    if (arg == "--skills") {
      if (i + 1 >= argc) { std::cerr << "error: --skills expects a path\n"; return 2; }
      skillsPath = expandUserPath(argv[++i]);
      continue;
    }

    if (arg == "--no-color") { forceNoColor = true; continue; }
    if (arg == "--raw") { raw = true; continue; }

    // additional input token
    prompt += " ";
    prompt += arg;
  }

  if (model.empty()) model = "local-model";

  if (newConv) {
    if (!wipeHistory(historyPath)) {
      std::cerr << "warning: failed to wipe history file: " << historyPath << "\n";
    }
  }

  std::vector<std::string> skillWarnings;
  std::vector<ToolSpec> toolSpecs = loadToolSpecsFromToml(skillsPath, skillWarnings);
  for (const auto& warning : skillWarnings) {
    std::cerr << "warning: " << warning << "\n";
  }

  auto history = loadHistory(historyPath);
  std::vector<ChatMessage> chatMessages;
  if (systemPrompt && !systemPrompt->empty()) {
    chatMessages.push_back({"system", *systemPrompt, {}, ""});
  }
  for (const auto& [role, content] : history) {
    if (role == "user" || role == "assistant") {
      chatMessages.push_back({role, content, {}, ""});
    }
  }
  chatMessages.push_back({"user", prompt, {}, ""});

  curl_global_init(CURL_GLOBAL_DEFAULT);
  auto finish = [&](int rc) {
    curl_global_cleanup();
    return rc;
  };

  std::string url = baseUrl + "/chat/completions";
  bool stdoutIsTty = isatty(fileno(stdout)) != 0;
  const std::vector<ToolSpec>* toolsPtr = toolSpecs.empty() ? nullptr : &toolSpecs;
  const int kMaxToolRounds = 8;
  bool gotFinalAssistant = false;
  std::string finalAssistantText;
  int toolBoxWidth = terminalWidthGuess();

  for (int round = 0; round < kMaxToolRounds; round++) {
    std::string body = buildChatRequestJson(
        model,
        chatMessages,
        /*temperature=*/0.7,
        /*stream=*/true,
        toolsPtr
    );

    StreamState st;
    st.printer.raw = raw;
    st.printer.enableColor = stdoutIsTty && !forceNoColor && !raw;
    st.printer.ansi.enable = st.printer.enableColor;
    st.reasoningWindow.render = stdoutIsTty && !raw;

    long httpCode = 0;
    std::string err;
    bool ok = httpPostJsonStream(
        url,
        body,
        (apiKey.empty() ? std::nullopt : std::optional<std::string>(apiKey)),
        httpCode,
        err,
        st
    );

    st.reasoningWindow.clearDisplay();
    st.reasoningWindow.reset();
    st.printer.finalize();

    if (!ok) {
      std::cerr << "\nrequest failed: " << err << "\n";
      return finish(1);
    }
    if (httpCode < 200 || httpCode >= 300) {
      std::cerr << "\nhttp " << httpCode << " response:\n" << st.rawAll << "\n";
      return finish(1);
    }

    std::vector<ToolCall> toolCalls = collectToolCallsFromStream(st);
    chatMessages.push_back({"assistant", st.assistantAll, toolCalls, ""});

    if (toolCalls.empty()) {
      gotFinalAssistant = true;
      finalAssistantText = st.assistantAll;
      break;
    }

    if (!st.assistantAll.empty() && st.assistantAll.back() != '\n') std::cout << "\n";
    for (const auto& call : toolCalls) {
      ToolRunRecord run = executeToolCall(call, toolSpecs);
      printBorderedBlockStdout(buildToolInteractionDisplay(run), toolBoxWidth);
      chatMessages.push_back({"tool", buildToolResultContent(run), {}, run.toolCallId});
    }
  }

  if (!gotFinalAssistant) {
    std::cerr << "\nrequest failed: exceeded tool call round limit\n";
    return finish(1);
  }

  if (!finalAssistantText.empty() && finalAssistantText.back() != '\n') std::cout << "\n";
  if (!finalAssistantText.empty()) {
    if (!appendTurn(historyPath, prompt, finalAssistantText)) {
      std::cerr << "warning: failed to append to history file: " << historyPath << "\n";
    }
  }
  return finish(0);
}

static int runTui(int argc, char** argv) {
  std::string baseUrl = getEnv("LM_BASE_URL");
  if (baseUrl.empty()) {
    std::cerr << "error: LM_BASE_URL not set (e.g. export LM_BASE_URL=\"http://localhost:1234/v1\")\n";
    return 2;
  }
  baseUrl = trimTrailingSlash(baseUrl);

  std::string model = getEnv("LM_MODEL");
  std::string apiKey = getEnv("LM_API_KEY");
  std::string systemPromptEnv = getEnv("LM_SYSTEM_PROMPT");
  std::optional<std::string> systemPrompt = systemPromptEnv.empty() ? std::nullopt
                                                                    : std::optional<std::string>(systemPromptEnv);
  std::string skillsPath = getEnv("LM_SKILLS_FILE");
  if (skillsPath.empty()) skillsPath = "~/.local/share/lm/skills.toml";
  skillsPath = expandUserPath(skillsPath);

  for (int i = 1; i < argc; i++) {
    std::string arg = argv[i];
    if (arg == "--tui") continue;
    if (arg == "--model") {
      if (i + 1 >= argc) { std::cerr << "error: --model expects a model id\n"; return 2; }
      model = argv[++i];
      continue;
    }
    if (arg == "--skills") {
      if (i + 1 >= argc) { std::cerr << "error: --skills expects a path\n"; return 2; }
      skillsPath = expandUserPath(argv[++i]);
      continue;
    }
    if (arg == "--no-color" || arg == "--raw" || arg == "--file" || arg == "--cmd" || arg == "--new") {
      std::cerr << "error: " << arg << " is not supported in --tui mode\n";
      return 2;
    }
    if (arg.rfind("--", 0) == 0) {
      std::cerr << "error: unknown option " << arg << "\n";
      return 2;
    }
    std::cerr << "error: prompt text is not supported in --tui mode\n";
    return 2;
  }

  if (model.empty()) model = "local-model";

  std::vector<std::string> skillWarnings;
  std::vector<ToolSpec> toolSpecs = loadToolSpecsFromToml(skillsPath, skillWarnings);
  for (const auto& warning : skillWarnings) {
    std::cerr << "warning: " << warning << "\n";
  }

  curl_global_init(CURL_GLOBAL_DEFAULT);
  TuiApp app(baseUrl, model, apiKey, systemPrompt, toolSpecs);
  int rc = app.run();
  curl_global_cleanup();
  return rc;
}

#ifndef LM_TESTING
int main(int argc, char** argv) {
  bool useTui = (argc == 1);
  for (int i = 1; i < argc; i++) {
    if (std::string(argv[i]) == "--tui") {
      useTui = true;
      break;
    }
  }
  if (useTui) return runTui(argc, argv);
  return runCli(argc, argv);
}
#endif
