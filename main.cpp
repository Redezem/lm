// lm.cpp
#include <curl/curl.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
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

static std::optional<std::string> extractStreamDeltaContent(const std::string& json) {
  // Typical chunk:
  // {"choices":[{"delta":{"content":"hi"},"index":0,"finish_reason":null}]}
  size_t d = json.find("\"delta\"");
  if (d == std::string::npos) return std::nullopt;

  size_t c = json.find("\"content\"", d);
  if (c == std::string::npos) return std::nullopt;

  size_t colon = json.find(':', c);
  if (colon == std::string::npos) return std::nullopt;

  size_t i = colon + 1;
  while (i < json.size() && (json[i] == ' ' || json[i] == '\n' || json[i] == '\r' || json[i] == '\t')) i++;

  if (i >= json.size() || json[i] != '"') return std::nullopt;
  return parseJsonStringAt(json, i);
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
    const std::optional<std::string>& systemPrompt,
    const std::vector<std::pair<std::string, std::string>>& history,
    const std::string& userPrompt,
    double temperature,
    bool stream
) {
  std::ostringstream oss;
  oss << "{";
  oss << "\"model\":\"" << jsonEscape(model) << "\",";
  oss << "\"messages\":[";
  bool first = true;

  auto addMsg = [&](const std::string& role, const std::string& content) {
    if (!first) oss << ",";
    first = false;
    oss << "{"
        << "\"role\":\"" << jsonEscape(role) << "\","
        << "\"content\":\"" << jsonEscape(content) << "\""
        << "}";
  };

  if (systemPrompt && !systemPrompt->empty()) {
    addMsg("system", *systemPrompt);
  }

  for (const auto& [role, content] : history) {
    if (role == "user" || role == "assistant") addMsg(role, content);
  }

  addMsg("user", userPrompt);

  oss << "],";
  oss << "\"temperature\":" << temperature << ",";
  oss << "\"stream\":" << (stream ? "true" : "false");
  oss << "}";
  return oss.str();
}

// ------------------------- curl streaming (SSE) -------------------------

struct StreamState {
  std::string sseBuf;
  std::string rawAll;          // keep for debugging / non-2xx
  std::string assistantAll;    // accumulated assistant content
  bool sawDone{false};
  StreamingMarkdownPrinter printer;
};

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

    auto delta = extractStreamDeltaContent(payload);
    if (delta && !delta->empty()) {
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

// ------------------------- main -------------------------

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "usage: lm \"prompt\" [more words...]\n"
              << "          [--file path] [--cmd \"cmd\"] [--model model_id]\n"
              << "          [--new] [--no-color] [--raw]\n\n"
              << "env:\n"
              << "  LM_BASE_URL        e.g. http://localhost:1234/v1\n"
              << "  LM_MODEL           optional model id\n"
              << "  LM_API_KEY         optional bearer token\n"
              << "  LM_SYSTEM_PROMPT   optional system prompt\n"
              << "  LM_HISTORY_FILE    optional history file path (default /tmp/lmstudio-cli.history.txt)\n";
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

  bool forceNoColor = false;
  bool raw = false;
  bool newConv = false;

  // prompt is argv[1] (as per your original spec)
  std::string prompt = argv[1];

  for (int i = 2; i < argc; i++) {
    std::string arg = argv[i];

    if (arg == "--new") { newConv = true; continue; }

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

  auto history = loadHistory(historyPath);

  curl_global_init(CURL_GLOBAL_DEFAULT);

  std::string url = baseUrl + "/chat/completions";
  std::string body = buildChatRequestJson(
      model,
      systemPrompt,
      history,
      prompt,
      /*temperature=*/0.7,
      /*stream=*/true
  );

  bool stdoutIsTty = isatty(fileno(stdout)) != 0;

  StreamState st;
  st.printer.raw = raw;
  st.printer.enableColor = stdoutIsTty && !forceNoColor && !raw;
  st.printer.ansi.enable = st.printer.enableColor;

  long httpCode = 0;
  std::string err;
  bool ok = httpPostJsonStream(url, body, (apiKey.empty() ? std::nullopt : std::optional<std::string>(apiKey)),
                               httpCode, err, st);

  curl_global_cleanup();

  st.printer.finalize();

  if (!ok) {
    std::cerr << "\nrequest failed: " << err << "\n";
    return 1;
  }

  if (httpCode < 200 || httpCode >= 300) {
    std::cerr << "\nhttp " << httpCode << " response:\n" << st.rawAll << "\n";
    return 1;
  }

  // Ensure we end with a newline (nice CLI behavior)
  if (!st.assistantAll.empty() && st.assistantAll.back() != '\n') std::cout << "\n";

  // Save turn to history (only if we got something back)
  if (!st.assistantAll.empty()) {
    if (!appendTurn(historyPath, prompt, st.assistantAll)) {
      std::cerr << "warning: failed to append to history file: " << historyPath << "\n";
    }
  }

  return 0;
}

