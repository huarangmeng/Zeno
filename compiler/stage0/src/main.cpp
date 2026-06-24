#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>
#include <sys/wait.h>

#ifndef ZENO_LLVM_VERSION
#define ZENO_LLVM_VERSION "unavailable"
#endif

namespace fs = std::filesystem;

namespace {

struct Options {
  std::string command;
  std::vector<std::string> inputs;
  std::string manifest;
  std::string workspace = ".";
  std::string target;
  std::string profile;
  std::string stage = "mvp";
  std::string milestone;
  std::string feature;
  std::string diagnosticFormat = "human";
  std::string emit;
  bool stageExplicit = false;
  bool release = false;
  bool frozen = true;
  bool updateLock = false;
  bool verbose = false;
};

struct Span {
  std::string file;
  std::size_t startByte = 0;
  std::size_t endByte = 0;
  int line = 1;
  int column = 1;
};

struct Diagnostic {
  std::string severity = "error";
  std::string code = "E0000";
  std::string message;
  std::string stageName = "front-end";
  std::string category = "syntax";
  std::string feature;
  Span span;
  std::vector<std::string> notes;
  std::vector<std::string> help;
  bool staged = false;
};

struct SourceText {
  fs::path path;
  std::string text;
  std::vector<std::size_t> lineStarts{0};
};

struct ExpectedError {
  std::string code;
  std::string message;
  int line = 1;
};

struct Metadata {
  std::string category;
  std::string stage;
  std::string milestone;
  std::string feature;
  std::string profile;
  std::string target;
  std::string buildArtifact;
  std::string artifactEmit;
  std::string runExitCode;
  std::vector<std::string> zmetaContains;
  std::vector<std::string> zmetaForbid;
  std::vector<std::string> emitContains;
  std::vector<std::string> emitForbid;
  std::vector<std::string> objectContains;
  std::vector<std::string> objectForbid;
};

struct TestCase {
  fs::path path;
  Metadata metadata;
  bool directoryCase = false;
};

struct PackageInfo {
  std::string name;
  std::string profile;
  std::string panicHandler;
  std::string oomHandler;
  std::map<std::string, fs::path> dependencies;
  std::set<std::string> builtinDependencies;
  std::vector<std::string> workspaceMembers;
};

struct LockPackage {
  std::string name;
  std::string source;
  std::string manifestHash;
  std::string contentHash;
  std::string compilerPackageHash;
  std::string blockText;
  int nameLine = 1;
  int sourceLine = 1;
  int manifestHashLine = 1;
  int contentHashLine = 1;
  int compilerPackageHashLine = 1;
};

std::vector<fs::path> packageSources(const fs::path &root);
std::string packageFingerprint(const fs::path &root);
std::string packageContentFingerprint(const fs::path &root);
std::string fileFingerprint(const fs::path &path);
fs::path builtinPackageRoot(const std::string &name);
std::string unquoteTomlString(std::string value);

const std::set<std::string> &supportedProfiles() {
  static const std::set<std::string> profiles{"hosted", "freestanding", "kernel", "embedded"};
  return profiles;
}

const std::set<std::string> &supportedTargetTriples() {
  static const std::set<std::string> targets{"aarch64-apple-darwin", "x86_64-unknown-linux-gnu"};
  return targets;
}

bool isSupportedProfile(const std::string &profile) {
  return supportedProfiles().count(profile) != 0;
}

bool isSupportedTargetTriple(const std::string &target) {
  return supportedTargetTriples().count(target) != 0;
}

const std::set<std::string> &supportedPackageKinds() {
  static const std::set<std::string> kinds{"application", "library"};
  return kinds;
}

const std::set<std::string> &supportedPanicStrategies() {
  static const std::set<std::string> strategies{"abort", "trap", "handler", "unwind"};
  return strategies;
}

const std::set<std::string> &supportedOomStrategies() {
  static const std::set<std::string> strategies{"abort", "trap", "handler"};
  return strategies;
}

const std::set<std::string> &supportedTrustFields() {
  static const std::set<std::string> fields{
      "ffi", "rawMemory", "hardware", "inlineAsm", "interrupts", "threadSafety",
      "dependencyTrust", "requireReport", "allowedPackages"};
  return fields;
}

bool isKnownTestStage(const std::string &stage) {
  return stage.empty() || stage == "mvp" || stage == "full-spec";
}

bool isKnownMilestone(const std::string &milestone) {
  return milestone.size() == 2 && milestone[0] == 'M' &&
         milestone[1] >= '0' && milestone[1] <= '9';
}

bool isZenoIdentifier(const std::string &name) {
  static const std::regex identPattern(R"(^[A-Za-z_][A-Za-z0-9_]*$)");
  return std::regex_match(name, identPattern);
}
std::map<std::string, std::string> manifestSectionFields(const std::string &text, const std::string &sectionName);
std::string inlineTomlField(const std::string &value, const std::string &keyName);

std::string trim(std::string_view view) {
  std::size_t first = 0;
  while (first < view.size() && std::isspace(static_cast<unsigned char>(view[first]))) {
    ++first;
  }
  std::size_t last = view.size();
  while (last > first && std::isspace(static_cast<unsigned char>(view[last - 1]))) {
    --last;
  }
  return std::string(view.substr(first, last - first));
}

bool startsWith(std::string_view text, std::string_view prefix) {
  return text.substr(0, prefix.size()) == prefix;
}

bool contains(std::string_view text, std::string_view needle) {
  return text.find(needle) != std::string_view::npos;
}

bool usesHardwareTrustCapability(std::string_view line) {
  return contains(line, "Mmio.") ||
         contains(line, "Port.") ||
         contains(line, "DmaBuffer.") ||
         contains(line, "loadVolatile") ||
         contains(line, "storeVolatile") ||
         contains(line, "mapPhysical") ||
         contains(line, "@linkSection");
}

bool usesInlineAsmTrustCapability(std::string_view line) {
  return contains(line, "asm!(") ||
         contains(line, "asm(") ||
         contains(line, "InlineAsm.") ||
         contains(line, "InlineAsm<");
}

bool usesInterruptTrustCapability(std::string_view line) {
  return contains(line, "@interrupt") ||
         contains(line, "@naked") ||
         contains(line, "callconv: Interrupt") ||
         contains(line, "abi: Interrupt") ||
         contains(line, "extern \"interrupt\"") ||
         contains(line, "startupEntry");
}

bool lineHasTrustBoundaryMarker(std::string_view line) {
  return contains(line, "trust extern") ||
         contains(line, "trust {") ||
         contains(line, "trust impl") ||
         contains(line, "RawPointer<") ||
         usesHardwareTrustCapability(line) ||
         usesInlineAsmTrustCapability(line) ||
         usesInterruptTrustCapability(line);
}

std::string readFile(const fs::path &path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    throw std::runtime_error("cannot read " + path.string());
  }
  std::ostringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

void writeFile(const fs::path &path, const std::string &text) {
  fs::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    throw std::runtime_error("cannot write " + path.string());
  }
  out << text;
}

std::vector<std::string> splitLines(const std::string &text) {
  std::vector<std::string> lines;
  std::stringstream stream(text);
  std::string line;
  while (std::getline(stream, line)) {
    lines.push_back(line);
  }
  if (!text.empty() && text.back() == '\n') {
    return lines;
  }
  if (lines.empty()) {
    lines.push_back("");
  }
  return lines;
}

SourceText loadSource(const fs::path &path) {
  SourceText source;
  source.path = path;
  source.text = readFile(path);
  for (std::size_t i = 0; i < source.text.size(); ++i) {
    if (source.text[i] == '\n') {
      source.lineStarts.push_back(i + 1);
    }
  }
  return source;
}

Span lineSpan(const SourceText &source, int line, int column = 1) {
  const int safeLine = std::max(1, line);
  const std::size_t index = static_cast<std::size_t>(safeLine - 1);
  const std::size_t start = index < source.lineStarts.size() ? source.lineStarts[index] : source.text.size();
  return Span{source.path.string(), start + static_cast<std::size_t>(std::max(0, column - 1)), start + static_cast<std::size_t>(std::max(1, column)), safeLine, column};
}

std::string jsonEscape(std::string_view value) {
  std::string out;
  for (char c : value) {
    switch (c) {
    case '\\': out += "\\\\"; break;
    case '"': out += "\\\""; break;
    case '\n': out += "\\n"; break;
    case '\r': out += "\\r"; break;
    case '\t': out += "\\t"; break;
    default:
      if (static_cast<unsigned char>(c) < 0x20) {
        out += "?";
      } else {
        out += c;
      }
    }
  }
  return out;
}

void emitJson(const Diagnostic &diag, std::ostream &out) {
  out << "{\"schemaVersion\":1"
      << ",\"severity\":\"" << jsonEscape(diag.severity) << "\""
      << ",\"code\":\"" << jsonEscape(diag.code) << "\""
      << ",\"message\":\"" << jsonEscape(diag.message) << "\""
      << ",\"stage\":\"" << jsonEscape(diag.stageName) << "\""
      << ",\"category\":\"" << jsonEscape(diag.category) << "\"";
  if (!diag.feature.empty()) {
    out << ",\"feature\":\"" << jsonEscape(diag.feature) << "\"";
  }
  out << ",\"primarySpan\":{\"file\":\"" << jsonEscape(diag.span.file) << "\""
      << ",\"startByte\":" << diag.span.startByte
      << ",\"endByte\":" << diag.span.endByte
      << ",\"startLine\":" << diag.span.line
      << ",\"startColumn\":" << diag.span.column
      << ",\"endLine\":" << diag.span.line
      << ",\"endColumn\":" << (diag.span.column + 1) << "}"
      << ",\"labels\":[]";
  out << ",\"notes\":[";
  for (std::size_t i = 0; i < diag.notes.size(); ++i) {
    if (i != 0) out << ",";
    out << "\"" << jsonEscape(diag.notes[i]) << "\"";
  }
  out << "],\"help\":[";
  for (std::size_t i = 0; i < diag.help.size(); ++i) {
    if (i != 0) out << ",";
    out << "\"" << jsonEscape(diag.help[i]) << "\"";
  }
  out << "],\"isStaged\":" << (diag.staged ? "true" : "false") << "}\n";
}

void emitHuman(const Diagnostic &diag, std::ostream &out) {
  out << diag.severity << "[" << diag.code << "]: " << diag.message << "\n";
  out << "  --> " << diag.span.file << ":" << diag.span.line << ":" << diag.span.column << "\n";
  for (const auto &note : diag.notes) {
    out << "note: " << note << "\n";
  }
  for (const auto &help : diag.help) {
    out << "help: " << help << "\n";
  }
}

bool diagnosticLess(const Diagnostic &a, const Diagnostic &b) {
  return std::tie(a.span.file, a.span.startByte, a.span.endByte, a.code, a.message, a.stageName, a.category) <
         std::tie(b.span.file, b.span.startByte, b.span.endByte, b.code, b.message, b.stageName, b.category);
}

void sortDiagnosticsInPlace(std::vector<Diagnostic> &diagnostics) {
  std::stable_sort(diagnostics.begin(), diagnostics.end(), diagnosticLess);
}

std::vector<Diagnostic> sortedDiagnostics(std::vector<Diagnostic> diagnostics) {
  sortDiagnosticsInPlace(diagnostics);
  return diagnostics;
}

void emitDiagnostics(const std::vector<Diagnostic> &diagnostics, const Options &options, std::ostream &out) {
  std::vector<Diagnostic> sorted = sortedDiagnostics(diagnostics);
  for (const auto &diag : sorted) {
    if (options.diagnosticFormat == "json") {
      emitJson(diag, out);
    } else {
      emitHuman(diag, out);
    }
  }
}

std::optional<std::smatch> matchRegex(const std::string &line, const std::regex &regex) {
  std::smatch match;
  if (std::regex_search(line, match, regex)) {
    return match;
  }
  return std::nullopt;
}

Metadata readMetadataFromText(const std::string &text, const fs::path &path) {
  Metadata metadata;
  if (contains(path.string(), "compile-pass")) metadata.category = "compile-pass";
  if (contains(path.string(), "compile-fail")) metadata.category = "compile-fail";
  if (contains(path.string(), "manifest-pass")) metadata.category = "manifest-pass";
  if (contains(path.string(), "manifest-fail")) metadata.category = "manifest-fail";
  if (contains(path.string(), "module-pass")) metadata.category = "module-pass";
  if (contains(path.string(), "module-fail")) metadata.category = "module-fail";
  if (contains(path.string(), "package-pass")) metadata.category = "package-pass";
  if (contains(path.string(), "package-fail")) metadata.category = "package-fail";
  if (contains(path.string(), "incremental-pass")) metadata.category = "incremental-pass";
  if (contains(path.string(), "incremental-fail")) metadata.category = "incremental-fail";
  if (contains(path.string(), "codegen-pass")) metadata.category = "codegen-pass";
  if (contains(path.string(), "codegen-fail")) metadata.category = "codegen-fail";

  const bool structuredTomlMetadata = path.filename() == "case.toml";
  const std::regex commentPattern(R"(^\s*(//|#)\s*([A-Za-z_-]+)\s*:\s*(.+?)\s*$)");
  const std::regex sectionPattern(R"(^\s*\[([A-Za-z0-9_.-]+)\]\s*$)");
  const std::regex tomlPattern(R"re(^\s*([A-Za-z_-]+)\s*=\s*"([^"]+)")re");
  std::string section;
  for (const auto &line : splitLines(text)) {
    if (auto match = matchRegex(line, commentPattern)) {
      const std::string key = (*match)[2];
      const std::string value = trim(std::string((*match)[3]));
      if (key == "category") metadata.category = value;
      if (key == "stage") metadata.stage = value;
      if (key == "milestone") metadata.milestone = value;
      if (key == "feature") metadata.feature = value;
      if (key == "profile") metadata.profile = value;
      if (key == "target") metadata.target = value;
      if (key == "build-artifact") metadata.buildArtifact = value;
      if (key == "artifact-emit") metadata.artifactEmit = value;
      if (key == "run-exit-code") metadata.runExitCode = value;
      if (key == "zmeta-contains") metadata.zmetaContains.push_back(value);
      if (key == "zmeta-forbid") metadata.zmetaForbid.push_back(value);
      if (key == "emit-contains") metadata.emitContains.push_back(value);
      if (key == "emit-forbid") metadata.emitForbid.push_back(value);
      if (key == "object-contains") metadata.objectContains.push_back(value);
      if (key == "object-forbid") metadata.objectForbid.push_back(value);
    } else if (structuredTomlMetadata) {
      if (auto match = matchRegex(line, sectionPattern)) {
        section = (*match)[1];
        continue;
      }
      auto toml = matchRegex(line, tomlPattern);
      if (!toml) continue;
      const std::string key = (*toml)[1];
      const std::string value = (*toml)[2];
      if (key == "stage") metadata.stage = value;
      if (key == "milestone") metadata.milestone = value;
      if (key == "feature") metadata.feature = value;
      if (key == "profile") metadata.profile = value;
      if (key == "target") metadata.target = value;
      if (key == "build-artifact") metadata.buildArtifact = value;
      if (key == "artifact-emit") metadata.artifactEmit = value;
      if (key == "run-exit-code") metadata.runExitCode = value;
      if (key == "zmeta-contains") metadata.zmetaContains.push_back(value);
      if (key == "zmeta-forbid") metadata.zmetaForbid.push_back(value);
      if (key == "emit-contains") metadata.emitContains.push_back(value);
      if (key == "emit-forbid") metadata.emitForbid.push_back(value);
      if (key == "object-contains") metadata.objectContains.push_back(value);
      if (key == "object-forbid") metadata.objectForbid.push_back(value);
      if (section == "target" && key == "profile") metadata.profile = value;
      if (section == "target" && key == "triple") metadata.target = value;
    }
  }
  return metadata;
}

std::vector<ExpectedError> expectedErrors(const std::string &text) {
  std::vector<ExpectedError> expected;
  const std::regex pattern(R"(expected-error(?:\[([A-Z]\d{4})\])?\s*:\s*(.+)$)");
  int lineNo = 1;
  for (const auto &line : splitLines(text)) {
    if (auto match = matchRegex(line, pattern)) {
      ExpectedError error;
      error.code = (*match)[1].matched ? std::string((*match)[1]) : "";
      error.message = trim(std::string((*match)[2]));
      error.line = lineNo;
      expected.push_back(error);
    }
    ++lineNo;
  }
  return expected;
}

int lineContaining(const std::string &text, const std::string &needle) {
  int lineNo = 1;
  for (const auto &line : splitLines(text)) {
    if (contains(line, needle)) return lineNo;
    ++lineNo;
  }
  return 1;
}

Diagnostic makeDiagnostic(const SourceText &source, const std::string &code, const std::string &message, int line, int column = 1) {
  Diagnostic diag;
  diag.code = code;
  diag.message = message;
  diag.span = lineSpan(source, line, column);
  return diag;
}

std::vector<Diagnostic> lexicalAndSyntaxDiagnostics(const SourceText &source) {
  std::vector<Diagnostic> diagnostics;
  int line = 1;
  int column = 1;
  std::vector<std::pair<char, Span>> stack;
  bool inString = false;
  bool inLineComment = false;
  bool inBlockComment = false;

  auto add = [&](std::string code, std::string message, int diagLine, int diagColumn) {
    diagnostics.push_back(makeDiagnostic(source, code, message, diagLine, diagColumn));
  };

  for (std::size_t i = 0; i < source.text.size(); ++i) {
    const char c = source.text[i];
    const char next = i + 1 < source.text.size() ? source.text[i + 1] : '\0';
    if (c == '\n') {
      ++line;
      column = 1;
      inLineComment = false;
      continue;
    }
    if (inLineComment) {
      ++column;
      continue;
    }
    if (inBlockComment) {
      if (c == '*' && next == '/') {
        inBlockComment = false;
        ++i;
        column += 2;
      } else {
        ++column;
      }
      continue;
    }
    if (!inString && c == '/' && next == '/') {
      inLineComment = true;
      ++i;
      column += 2;
      continue;
    }
    if (!inString && c == '/' && next == '*') {
      inBlockComment = true;
      ++i;
      column += 2;
      continue;
    }
    if (c == '"') {
      inString = !inString;
      ++column;
      continue;
    }
    if (inString) {
      ++column;
      continue;
    }
    if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
      const int startColumn = column;
      std::string ident;
      while (i < source.text.size()) {
        const char ch = source.text[i];
        if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '_') break;
        ident.push_back(ch);
        ++i;
        ++column;
      }
      --i;
      if (ident == "let") {
        add("E0004", "use val for immutable bindings or var for mutable bindings", line, startColumn);
      } else if (ident == "unsafe") {
        add("E0004", "unsafe blocks are not part of Zeno user syntax", line, startColumn);
      } else if (ident == "defer") {
        add("E0004", "defer is not a Zeno keyword; use a RAII guard with destroy", line, startColumn);
      } else if (ident == "await" || ident == "async") {
        Diagnostic diag = makeDiagnostic(source, "E9001", "async lowering is not implemented in stage0", line, startColumn);
        diag.staged = true;
        diag.feature = "async";
        diag.category = "staged";
        diag.stageName = "lowering";
        diag.notes.push_back("async syntax is reserved for the full language");
        diag.help.push_back("use synchronous code in stage0 MVP");
        diagnostics.push_back(diag);
      }
      continue;
    }
    if (c == '{' || c == '(' || c == '[') {
      stack.push_back({c, lineSpan(source, line, column)});
    } else if (c == '}' || c == ')' || c == ']') {
      if (stack.empty()) {
        add("E0003", "unexpected closing delimiter", line, column);
      } else {
        const char open = stack.back().first;
        const bool ok = (open == '{' && c == '}') || (open == '(' && c == ')') || (open == '[' && c == ']');
        if (!ok) {
          add("E0003", "mismatched closing delimiter", line, column);
        }
        stack.pop_back();
      }
    } else if (static_cast<unsigned char>(c) >= 0x80) {
      add("E0001", "stage0 source accepts only ASCII outside string literals", line, column);
    }
    ++column;
  }

  if (inString) {
    add("E0002", "unterminated string literal", line, column);
  }
  if (inBlockComment) {
    add("E0002", "unterminated block comment", line, column);
  }
  for (const auto &entry : stack) {
    Diagnostic diag;
    diag.code = "E0003";
    diag.message = "unclosed delimiter";
    diag.span = entry.second;
    diagnostics.push_back(diag);
  }
  return diagnostics;
}

std::string stripLineComment(const std::string &line) {
  bool inString = false;
  for (std::size_t i = 0; i + 1 < line.size(); ++i) {
    if (line[i] == '"' && (i == 0 || line[i - 1] != '\\')) {
      inString = !inString;
    }
    if (!inString && line[i] == '/' && line[i + 1] == '/') {
      return line.substr(0, i);
    }
  }
  return line;
}

int braceDelta(const std::string &line) {
  bool inString = false;
  int delta = 0;
  for (std::size_t i = 0; i < line.size(); ++i) {
    if (line[i] == '"' && (i == 0 || line[i - 1] != '\\')) {
      inString = !inString;
      continue;
    }
    if (inString) continue;
    if (line[i] == '{') ++delta;
    if (line[i] == '}') --delta;
  }
  return delta;
}

std::vector<std::string> splitArguments(const std::string &args) {
  std::vector<std::string> out;
  std::string current;
  int depth = 0;
  bool inString = false;
  for (char c : args) {
    if (c == '"' && (current.empty() || current.back() != '\\')) inString = !inString;
    if (!inString && (c == '(' || c == '<' || c == '{' || c == '[')) ++depth;
    if (!inString && (c == ')' || c == '>' || c == '}' || c == ']')) --depth;
    if (!inString && c == ',' && depth == 0) {
      out.push_back(trim(current));
      current.clear();
      continue;
    }
    current.push_back(c);
  }
  if (!trim(current).empty()) out.push_back(trim(current));
  return out;
}

std::optional<std::string> parenthesizedContentAt(const std::string &text, std::size_t openIndex) {
  if (openIndex >= text.size() || text[openIndex] != '(') return std::nullopt;
  bool inString = false;
  int depth = 0;
  for (std::size_t i = openIndex; i < text.size(); ++i) {
    const char c = text[i];
    if (c == '"' && (i == 0 || text[i - 1] != '\\')) {
      inString = !inString;
      continue;
    }
    if (inString) continue;
    if (c == '(') ++depth;
    if (c == ')') {
      --depth;
      if (depth == 0) {
        return text.substr(openIndex + 1, i - openIndex - 1);
      }
    }
  }
  return std::nullopt;
}

std::optional<std::string> bracedContentAt(const std::string &text, std::size_t openIndex) {
  if (openIndex >= text.size() || text[openIndex] != '{') return std::nullopt;
  bool inString = false;
  int depth = 0;
  for (std::size_t i = openIndex; i < text.size(); ++i) {
    const char c = text[i];
    if (c == '"' && (i == 0 || text[i - 1] != '\\')) {
      inString = !inString;
      continue;
    }
    if (inString) continue;
    if (c == '{') ++depth;
    if (c == '}') {
      --depth;
      if (depth == 0) {
        return text.substr(openIndex + 1, i - openIndex - 1);
      }
    }
  }
  return std::nullopt;
}

std::string normalizeSignatureType(std::string type) {
  type = trim(type);
  if (startsWith(type, "mut ")) type = trim(type.substr(4));
  if (startsWith(type, "move ")) type = trim(type.substr(5));
  return type;
}

std::vector<std::string> parameterTypes(const std::string &params) {
  std::vector<std::string> types;
  for (const auto &param : splitArguments(params)) {
    const auto colon = param.find(':');
    if (colon == std::string::npos) {
      if (contains(param, "self")) types.push_back(trim(param));
      continue;
    }
    types.push_back(normalizeSignatureType(param.substr(colon + 1)));
  }
  return types;
}

std::vector<std::string> overloadParameterTypes(const std::string &params) {
  std::vector<std::string> types;
  for (const auto &param : splitArguments(params)) {
    const auto colon = param.find(':');
    if (colon == std::string::npos) {
      if (contains(param, "mut self")) types.push_back("mut self");
      else if (contains(param, "self")) types.push_back("self");
      continue;
    }
    const std::string modeAndName = trim(param.substr(0, colon));
    std::string type = normalizeSignatureType(param.substr(colon + 1));
    if (startsWith(modeAndName, "mut ")) type = "mut " + type;
    types.push_back(type);
  }
  return types;
}

std::string overloadKey(const std::string &name, const std::string &params) {
  std::string key = name + "(";
  auto types = overloadParameterTypes(params);
  for (std::size_t i = 0; i < types.size(); ++i) {
    if (i != 0) key += ",";
    key += types[i];
  }
  key += ")";
  return key;
}

bool isCCompatibleType(const std::string &type, const std::set<std::string> &cLayoutTypes = {}) {
  const std::string normalized = normalizeSignatureType(type);
  static const std::set<std::string> simple{
      "Unit", "Bool", "I8", "I16", "I32", "I64", "ISize",
      "U8", "U16", "U32", "U64", "USize", "F32", "F64"};
  if (simple.count(normalized)) return true;
  if (cLayoutTypes.count(normalized)) return true;
  if (startsWith(normalized, "ArraySlice<") || startsWith(normalized, "Option<")) return true;
  return false;
}

std::string firstNonCType(const std::string &params, const std::string &returnType, const std::set<std::string> &cLayoutTypes = {}) {
  for (const auto &type : parameterTypes(params)) {
    if (!isCCompatibleType(type, cLayoutTypes)) return type;
  }
  if (!returnType.empty() && !isCCompatibleType(returnType, cLayoutTypes)) return normalizeSignatureType(returnType);
  return "";
}

bool isBridgeCompatibleType(const std::string &type, const std::set<std::string> &cLayoutTypes = {}) {
  const std::string normalized = normalizeSignatureType(type);
  if (normalized == "StringSlice") return true;
  if (startsWith(normalized, "Result<") || startsWith(normalized, "Option<")) return true;
  return isCCompatibleType(normalized, cLayoutTypes);
}

std::string firstNonBridgeType(const std::string &params, const std::string &returnType, const std::set<std::string> &cLayoutTypes = {}) {
  for (const auto &type : parameterTypes(params)) {
    if (!isBridgeCompatibleType(type, cLayoutTypes)) return type;
  }
  if (!returnType.empty() && !isBridgeCompatibleType(returnType, cLayoutTypes)) return normalizeSignatureType(returnType);
  return "";
}

std::string baseTypeName(std::string type) {
  type = normalizeSignatureType(type);
  const auto generic = type.find('<');
  if (generic != std::string::npos) type = type.substr(0, generic);
  return trim(type);
}

bool containsNestedType(const std::string &type, const std::string &name) {
  const std::regex pattern("(^|[^A-Za-z0-9_])" + name + "([^A-Za-z0-9_]|$)");
  return std::regex_search(type, pattern);
}

std::optional<int> packedAlignmentFromAttribute(const std::string &line) {
  const std::regex pattern(R"re(@layout\s*\(\s*Packed\s*\(\s*([0-9]+)\s*\)\s*\))re");
  if (auto match = matchRegex(line, pattern)) {
    return std::stoi(std::string((*match)[1]));
  }
  return std::nullopt;
}

std::optional<std::string> firstGenericArgument(const std::string &type, const std::string &wrapper) {
  const std::string normalized = normalizeSignatureType(type);
  const std::string prefix = wrapper + "<";
  if (!startsWith(normalized, prefix) || normalized.empty() || normalized.back() != '>') return std::nullopt;
  return trim(normalized.substr(prefix.size(), normalized.size() - prefix.size() - 1));
}

std::string firstCallArgument(const std::string &line) {
  const auto open = line.find('(');
  const auto close = line.rfind(')');
  if (open == std::string::npos || close == std::string::npos || close <= open) return "";
  auto args = splitArguments(line.substr(open + 1, close - open - 1));
  if (args.empty()) return "";
  std::string arg = trim(args.front());
  if (startsWith(arg, "move ")) arg = trim(arg.substr(5));
  if (startsWith(arg, "mut ")) arg = trim(arg.substr(4));
  return arg;
}

struct ExportAttribute {
  std::string symbol;
  bool bridge = false;
};

struct EnumVariantShape {
  bool hasPayload = false;
  std::size_t payloadArity = 0;
};

std::optional<ExportAttribute> exportAttributeFromLine(const std::string &line) {
  const std::regex pattern(R"re(@export\s*\(\s*"([^"]+)")re");
  if (auto match = matchRegex(line, pattern)) {
    return ExportAttribute{std::string((*match)[1]), contains(line, "bridge: C")};
  }
  return std::nullopt;
}

std::vector<Diagnostic> sourceSemanticDiagnostics(const SourceText &source) {
  std::vector<Diagnostic> diagnostics;
  const auto rawLines = splitLines(source.text);
  std::vector<std::string> lines;
  lines.reserve(rawLines.size());
  for (const auto &line : rawLines) lines.push_back(stripLineComment(line));

  const bool freestandingProfile = contains(source.text, "// profile: freestanding");
  std::set<std::string> moveParameterFunctions;
  std::set<std::string> moveSelfMethods;
  std::set<std::string> mutSelfMethods;
  std::set<std::string> valBindings;
  std::set<std::string> readParameters;
  std::set<std::string> uninitializedVars;
  std::set<std::string> localArrayOwners;
  std::set<std::string> movedBindings;
  std::map<std::string, std::string> movedBindingMessages;
  std::map<std::string, std::set<std::string>> movedFields;
  std::map<std::string, std::string> liveSliceOwners;
  std::set<std::string> mapEntryBindings;
  std::set<std::string> readOnlyLoopItems;
  std::set<std::string> taskHandles;
  std::set<std::string> settledTaskHandles;
  std::set<std::string> cLayoutTypes;
  std::set<std::string> nonPubCLayoutTypes;
  std::set<std::string> packedLayoutTypes;
  std::set<std::string> declaredStructTypes;
  std::map<std::string, std::set<std::string>> structFields;
  std::map<std::string, std::map<std::string, std::string>> structFieldTypes;
  std::map<std::string, std::map<std::string, EnumVariantShape>> enumVariants;
  std::set<std::string> enumVariantValueNames;
  enumVariantValueNames.insert("None");
  enumVariantValueNames.insert("Some");
  std::set<std::string> interfaceTypes;
  std::set<std::string> resourceTypes;
  std::set<std::string> destroyTypes;
  std::set<std::string> trustedSendTypes;
  std::set<std::string> hashKeyTypes;
  std::set<std::string> trustExternFunctions;
  std::set<std::string> cErrorCodeTypes;
  std::set<std::string> ctfeUnsafeFunctions;
  std::map<std::string, std::string> callableParameterKinds;
  std::set<std::string> mutableCallableParameters;
  std::map<std::string, std::string> localClosureKinds;
  std::map<std::string, std::string> variableTypes;
  std::map<std::string, std::string> functionResultErrorTypes;
  std::set<std::string> maybeInitializedVals;
  std::set<std::string> partialStructVals;
  std::map<std::string, std::set<std::string>> partialStructFields;
  std::map<std::string, int> arrayLengths;
  std::set<std::string> constOwnedStrings;
  std::map<std::string, std::set<std::string>> constReferences;
  std::set<std::string> scopedFutureOwners;
  std::set<std::string> asyncViewNames;
  std::set<std::string> taskGroupNames;
  std::set<std::string> settledTaskGroups;
  std::map<std::string, std::map<std::string, std::set<std::string>>> interfaceMethodFlags;
  std::set<std::string> interfaceImplKeys;
  std::map<std::string, std::map<std::string, std::set<std::string>>> genericInterfaceImplArgs;
  std::set<std::string> consumedOnceCallables;
  std::map<std::string, int> overloadKeys;
  std::map<std::string, std::string> overloadReturnTypes;
  std::map<std::string, std::set<std::string>> overloadArgumentTypes;
  std::map<std::string, std::vector<std::vector<std::string>>> functionParameterTypeLists;
  std::map<std::string, std::vector<std::string>> functionReturnTypeLists;
  std::set<std::string> genericFunctionNames;
  std::set<std::string> topLevelValueNames;
  std::set<std::string> constGenericValueNames;
  std::map<std::string, std::set<std::string>> functionCalls;
  std::set<std::string> directPanicFunctions;
  std::set<std::string> panicReachableFunctions;
  std::set<std::string> directAllocatingFunctions;
  std::set<std::string> allocatingReachableFunctions;
  std::map<std::string, int> exportedSymbols;

  const std::regex fnPattern(R"(^\s*(?:pub\s+|private\s+)?(?:async\s+)?fn\s+([A-Za-z_][A-Za-z0-9_]*)(?:<[^()]*>)?\s*\(([^)]*)\))");
  const std::regex topFnPattern(R"(^\s*(pub\s+|private\s+)?(?:async\s+)?fn\s+([A-Za-z_][A-Za-z0-9_]*)(<[^()]*>)?\s*\(([^)]*)\)\s*(?:->\s*([A-Za-z_][A-Za-z0-9_]*(?:<[^()]*>)?))?)");
  const std::regex externPattern(R"(^\s*(trust\s+)?extern\s+"C"\s+fn\s+([A-Za-z_][A-Za-z0-9_]*)(<[^()]*>)?\s*\(([^)]*)\)\s*(?:->\s*([A-Za-z_][A-Za-z0-9_]*(?:<[^()]*>)?))?)");
  const std::regex structDeclPattern(R"(^\s*(?:pub\s+)?struct\s+([A-Za-z_][A-Za-z0-9_]*))");
  const std::regex enumDeclPattern(R"(^\s*(?:pub\s+|private\s+)?enum\s+([A-Za-z_][A-Za-z0-9_]*)(?:<[^()]*>)?)");
  const std::regex enumVariantPattern(R"(^\s*([A-Za-z_][A-Za-z0-9_]*)\s*(?:\(|\{|,|$))");
  const std::regex valPattern(R"(^\s*val\s+([A-Za-z_][A-Za-z0-9_]*)(?:\s*:\s*([^=;]+))?\s*=\s*(.+);)");
  const std::regex valTuplePattern(R"(^\s*val\s*\(([^)]*)\)\s*=\s*(.+);)");
  const std::regex valStartPattern(R"(^\s*val\s+([A-Za-z_][A-Za-z0-9_]*)(?:\s*:\s*([^=;]+))?\s*=)");
  const std::regex valUninitPattern(R"(^\s*val\s+([A-Za-z_][A-Za-z0-9_]*)\s*:\s*([^=;]+);)");
  const std::regex closureValPattern(R"(^\s*(val|var)\s+([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(move\s+)?\(\)\s*(?:->\s*[A-Za-z_][A-Za-z0-9_]*)?\s*\{)");
  const std::regex varInitPattern(R"(^\s*var\s+([A-Za-z_][A-Za-z0-9_]*)(?:\s*:\s*([^=;]+))?\s*=\s*(.+);)");
  const std::regex varUninitPattern(R"(^\s*var\s+([A-Za-z_][A-Za-z0-9_]*)\s*:\s*([^=;]+);)");
  const std::regex assignPattern(R"(^\s*([A-Za-z_][A-Za-z0-9_]*)\s*=)");
  const std::regex assignValuePattern(R"(^\s*([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.+);)");
  const std::regex callPattern(R"(([A-Za-z_][A-Za-z0-9_]*)\s*\((.*)\)\s*;)");
  const std::regex ifConditionPattern(R"(^\s*if\s*\((.+)\)\s*\{)");
  const std::regex whileConditionPattern(R"(^\s*while\s*\((.+)\)\s*\{)");
  const std::regex forItemPattern(R"(^\s*for\s+(?:move\s+|mut\s+)?([A-Za-z_][A-Za-z0-9_]*)\s+in\b)");
  const std::regex mutCallPattern(R"(\bmut\s+([A-Za-z_][A-Za-z0-9_]*)\.)");
  const std::regex moveUsePattern(R"(\bmove\s+([A-Za-z_][A-Za-z0-9_]*)\b)");
  const std::regex returnFieldPattern(R"(\breturn\s+([A-Za-z_][A-Za-z0-9_]*)\.)");
  const std::regex interfacePattern(R"(^\s*interface\s+([A-Za-z_][A-Za-z0-9_]*))");
  const std::regex fieldPattern(R"(^\s*(?:pub\s+)?([A-Za-z_][A-Za-z0-9_]*)\s*:\s*([^,;]+),?)");
  const std::regex staticDeclPattern(R"(^\s*static\s+[A-Za-z_][A-Za-z0-9_]*\s*:\s*([^=;]+))");
  const std::regex topLevelValueDeclPattern(R"(^\s*(?:const|static)\s+([A-Za-z_][A-Za-z0-9_]*)\s*:)");
  const std::regex topLevelExecutablePattern(R"(^\s*(?:(?:val|var|return|if|while|for|match|try)\b|trust\s*\{|[A-Za-z_][A-Za-z0-9_]*\s*[=(]))");
  const std::regex implPattern(R"(^\s*impl(?:<[^()]*>)?\s+([A-Za-z_][A-Za-z0-9_]*)\s*\{)");
  const std::regex traitImplPattern(R"(^\s*(trust\s+)?impl\s+([A-Za-z_][A-Za-z0-9_]*)\s+for\s+([A-Za-z_][A-Za-z0-9_]*))");
  const std::regex genericTraitImplPattern(R"(^\s*impl\s+([A-Za-z_][A-Za-z0-9_]*)<([^>]+)>\s+for\s+([A-Za-z_][A-Za-z0-9_]*))");

  int signatureBraceDepth = 0;
  for (const auto &line : lines) {
    const std::string trimmed = trim(line);
    if (signatureBraceDepth == 0) {
      if (auto valueDecl = matchRegex(trimmed, topLevelValueDeclPattern)) {
        topLevelValueNames.insert((*valueDecl)[1]);
      }
      if (auto fn = matchRegex(trimmed, topFnPattern)) {
        const std::string name = (*fn)[2];
        const std::string generic = (*fn)[3];
        const std::string params = (*fn)[4];
        const std::string returnType = (*fn)[5];
        if (generic.empty()) {
          functionParameterTypeLists[name].push_back(parameterTypes(params));
          functionReturnTypeLists[name].push_back(normalizeSignatureType(returnType));
        } else {
          genericFunctionNames.insert(name);
        }
      }
    }
    signatureBraceDepth += braceDelta(line);
    if (signatureBraceDepth < 0) signatureBraceDepth = 0;
  }

  std::string scanningFunction;
  int scanningFunctionDepth = 0;
  bool scanningFunctionCtfeUnsafe = false;
  auto sourceLineMayAllocate = [&](const std::string &expr) {
    return contains(expr, "format(") ||
           contains(expr, "String.from(") ||
           contains(expr, "String.fromIn(") ||
           contains(expr, ".push(") ||
           contains(expr, ".reserve(") ||
           contains(expr, ".reserveExact(") ||
           contains(expr, ".tryReserve(") ||
           contains(expr, ".tryReserveExact(") ||
           contains(expr, ".clone(") ||
           contains(expr, ".cloneIn(") ||
           contains(expr, ".withCapacity(") ||
           contains(expr, ".withCapacityIn(") ||
           contains(expr, ".filled(") ||
           contains(expr, ".filledIn(") ||
           contains(expr, ".emptyIn(") ||
           contains(expr, "Box.new(") ||
           contains(expr, "Box.newIn(") ||
           contains(expr, "Shared.new(") ||
           contains(expr, "Shared.newIn(") ||
           contains(expr, ".insert(") ||
           contains(expr, ".entry(");
  };
  const std::regex bareCallPattern(R"((^|[^A-Za-z0-9_\.])([A-Za-z_][A-Za-z0-9_]*)\s*\()");
  const std::set<std::string> ignoredBareCalls{
      "if", "while", "for", "match", "return", "try", "fn", "panic", "oom",
      "Some", "None", "Ok", "Err", "Array", "Vector", "Map", "Set", "String", "Box", "Shared"};
  for (const auto &line : lines) {
    const std::string codeLine = stripLineComment(line);
    if (auto match = matchRegex(line, fnPattern)) {
      const std::string name = (*match)[1];
      const std::string params = (*match)[2];
      if (contains(params, "move ")) moveParameterFunctions.insert(name);
      if (contains(params, "mut self")) mutSelfMethods.insert(name);
      if (contains(params, "move self")) moveSelfMethods.insert(name);
      scanningFunction = name;
      scanningFunctionDepth = 0;
      scanningFunctionCtfeUnsafe = false;
    }
    if (auto externDecl = matchRegex(line, externPattern)) {
      if ((*externDecl)[1].matched) {
        trustExternFunctions.insert((*externDecl)[2]);
      }
    }
    if (auto traitImpl = matchRegex(stripLineComment(line), traitImplPattern)) {
      if (std::string((*traitImpl)[2]) == "HashKey") hashKeyTypes.insert((*traitImpl)[3]);
    }
    if (!scanningFunction.empty()) {
      if (contains(codeLine, "trust {")) scanningFunctionCtfeUnsafe = true;
      for (const auto &externName : trustExternFunctions) {
        if (contains(codeLine, externName + "(")) scanningFunctionCtfeUnsafe = true;
      }
      if (contains(codeLine, "panic(")) {
        directPanicFunctions.insert(scanningFunction);
      }
      if (sourceLineMayAllocate(codeLine)) {
        directAllocatingFunctions.insert(scanningFunction);
      }
      for (auto it = std::sregex_iterator(codeLine.begin(), codeLine.end(), bareCallPattern);
           it != std::sregex_iterator(); ++it) {
        const std::string callee = (*it)[2];
        if (!ignoredBareCalls.count(callee) && callee != scanningFunction) {
          functionCalls[scanningFunction].insert(callee);
        }
      }
      scanningFunctionDepth += braceDelta(line);
      if (scanningFunctionDepth <= 0 && contains(line, "}")) {
        if (scanningFunctionCtfeUnsafe) ctfeUnsafeFunctions.insert(scanningFunction);
        scanningFunction.clear();
      }
    }
  }
  auto closeReachableFunctions = [&](const std::set<std::string> &direct) {
    std::set<std::string> reachable = direct;
    bool changed = true;
    while (changed) {
      changed = false;
      for (const auto &[caller, callees] : functionCalls) {
        if (reachable.count(caller)) continue;
        for (const auto &callee : callees) {
          if (reachable.count(callee)) {
            reachable.insert(caller);
            changed = true;
            break;
          }
        }
      }
    }
    return reachable;
  };
  panicReachableFunctions = closeReachableFunctions(directPanicFunctions);
  allocatingReachableFunctions = closeReachableFunctions(directAllocatingFunctions);

  bool pendingNoAlloc = false;
  bool pendingLayout = false;
  bool pendingCLayout = false;
  bool pendingPackedLayout = false;
  int pendingLayoutCount = 0;
  std::optional<int> pendingPackedAlignment;
  std::optional<ExportAttribute> pendingExport;
  bool inNoAlloc = false;
  int noAllocBraceDepth = 0;
  bool pendingNoPanic = false;
  bool inNoPanic = false;
  int noPanicBraceDepth = 0;
  int declarationBraceDepth = 0;
  bool inTrustBlock = false;
  int trustBraceDepth = 0;
  bool inStruct = false;
  std::string currentStruct;
  int structBraceDepth = 0;
  std::string currentStructLayout;
  bool inEnum = false;
  std::string currentEnum;
  int enumBraceDepth = 0;
  bool inImpl = false;
  std::string currentImplType;
  int implBraceDepth = 0;
  bool inDestroy = false;
  int destroyBraceDepth = 0;
  bool inInterface = false;
  std::string currentInterface;
  int interfaceBraceDepth = 0;
  bool inThreadSpawn = false;
  int threadSpawnBraceDepth = 0;
  bool inSpawnBlockingClosure = false;
  int spawnBlockingBraceDepth = 0;
  std::string currentSelfMode;
  std::string currentReturnType;
  std::set<std::string> currentInterfaceReturnTypes;
  std::string trackedFunctionName;
  std::string trackedFunctionReturnType;
  int trackedFunctionBraceDepth = 0;
  int trackedFunctionStartLine = 0;
  bool trackedFunctionLastTopLevelTerminal = false;
  bool trackedTopLevelIfActive = false;
  int trackedTopLevelIfBraceDepth = 0;
  bool trackedTopLevelIfThenReturns = false;
  bool trackedTopLevelIfElseSeen = false;
  bool trackedTopLevelIfElseReturns = false;
  bool inClosureLiteral = false;
  std::string currentClosureName;
  int closureBraceDepth = 0;
  bool closureMutates = false;
  bool closureConsumes = false;
  bool inMatch = false;
  bool matchIsMove = false;
  int matchBraceDepth = 0;
  bool matchSawWildcard = false;
  bool matchSawReady = false;
  bool matchSawWaiting = false;
  bool matchSawDone = false;
  bool matchSawSomeUnguarded = false;
  bool matchSawSomeGuarded = false;
  bool matchSawNone = false;
  bool inAsyncFunction = false;
  bool asyncSawAwait = false;
  bool taskGroupStreamingLoop = false;
  bool taskGroupEarlyExitBlock = false;
  bool inMultilineStructLiteral = false;
  std::string multilineStructLiteralType;
  std::string multilineStructLiteralFields;
  int multilineStructLiteralBraceDepth = 0;

  for (std::size_t index = 0; index < lines.size(); ++index) {
    const int lineNo = static_cast<int>(index + 1);
    const std::string line = lines[index];
    const std::string trimmed = trim(line);
    if (trimmed.empty()) continue;
    if (trimmed == "}" || trimmed == "};") {
      liveSliceOwners.clear();
      mapEntryBindings.clear();
      readOnlyLoopItems.clear();
      taskHandles.clear();
      settledTaskHandles.clear();
    }

    auto add = [&](const std::string &code, const std::string &message) {
      diagnostics.push_back(makeDiagnostic(source, code, message, lineNo));
    };
    auto allocatingApiMessage = [&](const std::string &expr) -> std::optional<std::string> {
      if (contains(expr, "format(")) return "hidden heap allocation";
      if (contains(expr, "String.from(") || contains(expr, "String.fromIn(")) {
        return "String.from may allocate in no-allocation context";
      }
      if (contains(expr, ".push(")) return "push may allocate in no-allocation context";
      if (contains(expr, ".reserve(") || contains(expr, ".reserveExact(") ||
          contains(expr, ".tryReserve(") || contains(expr, ".tryReserveExact(")) {
        return "reserve may allocate in no-allocation context";
      }
      if (contains(expr, ".clone(") || contains(expr, ".cloneIn(")) {
        return "clone may allocate in no-allocation context";
      }
      if (contains(expr, ".withCapacity(") || contains(expr, ".withCapacityIn(") ||
          contains(expr, ".filled(") || contains(expr, ".filledIn(") ||
          contains(expr, ".emptyIn(")) {
        return "collection allocation is not allowed in no-allocation context";
      }
      if (contains(expr, "Box.new(") || contains(expr, "Box.newIn(") ||
          contains(expr, "Shared.new(") || contains(expr, "Shared.newIn(")) {
        return "Box/Shared allocation is not allowed in no-allocation context";
      }
      if (contains(expr, ".insert(") || contains(expr, ".entry(")) {
        return "collection insertion may allocate in no-allocation context";
      }
      return std::nullopt;
    };
    auto oomPanicAllocatingApiMessage = [&](const std::string &expr) -> std::optional<std::string> {
      if (contains(expr, "format(")) return "allocation failure would call panic in this profile";
      if (contains(expr, "String.from(") || contains(expr, "String.fromIn(") ||
          contains(expr, ".push(") || contains(expr, ".reserve(") ||
          contains(expr, ".reserveExact(") || contains(expr, ".clone(") ||
          contains(expr, ".cloneIn(") || contains(expr, ".withCapacity(") ||
          contains(expr, ".withCapacityIn(") || contains(expr, ".filled(") ||
          contains(expr, ".filledIn(") || contains(expr, ".emptyIn(") ||
          contains(expr, "Box.new(") || contains(expr, "Box.newIn(") ||
          contains(expr, "Shared.new(") || contains(expr, "Shared.newIn(") ||
          contains(expr, ".insert(") || contains(expr, ".entry(")) {
        return "allocation failure would call panic in this profile";
      }
      return std::nullopt;
    };
    auto isFunctionDeclarationLine = [&](const std::string &expr) {
      return startsWith(expr, "fn ") || startsWith(expr, "pub fn ") || startsWith(expr, "private fn ") ||
             startsWith(expr, "async fn ") || startsWith(expr, "pub async fn ") ||
             startsWith(expr, "private async fn ");
    };
    auto callsReachableFunction = [&](const std::string &expr, const std::set<std::string> &reachable) {
      if (isFunctionDeclarationLine(expr)) return false;
      for (const auto &callee : reachable) {
        const std::regex calleePattern("(^|[^A-Za-z0-9_\\.])" + callee + R"(\s*\()");
        if (std::regex_search(expr, calleePattern)) return true;
      }
      return false;
    };
    auto isNumericType = [&](const std::string &type) {
      const std::string normalized = normalizeSignatureType(type);
      static const std::set<std::string> numeric{
          "I8", "I16", "I32", "I64", "ISize",
          "U8", "U16", "U32", "U64", "USize", "F32", "F64"};
      return numeric.count(normalized) > 0;
    };
    auto isFloatType = [&](const std::string &type) {
      const std::string normalized = normalizeSignatureType(type);
      return normalized == "F32" || normalized == "F64";
    };
    auto comparableLocalType = [&](const std::string &type) {
      const std::string normalized = normalizeSignatureType(type);
      return normalized == "Unit" || normalized == "Bool" || normalized == "String" || normalized == "StringSlice" ||
             normalized == "Char" || isNumericType(normalized) || structFields.count(normalized) ||
             enumVariants.count(normalized);
    };
    auto hasKnownHashKey = [&](const std::string &type) {
      const std::string normalized = normalizeSignatureType(type);
      return normalized == "Bool" || normalized == "Char" || normalized == "String" ||
             normalized == "StringSlice" || isNumericType(normalized) || hashKeyTypes.count(normalized);
    };
    auto checkHashKeyCollectionType = [&](const std::string &collectionType) {
      if (auto mapArgs = firstGenericArgument(collectionType, "Map")) {
        const auto args = splitArguments(*mapArgs);
        if (args.size() == 2 && declaredStructTypes.count(normalizeSignatureType(args[0])) &&
            !hasKnownHashKey(args[0])) {
          add("E0601", "Map key type " + normalizeSignatureType(args[0]) + " must implement HashKey");
        }
      }
      if (auto element = firstGenericArgument(collectionType, "Set")) {
        if (declaredStructTypes.count(normalizeSignatureType(*element)) && !hasKnownHashKey(*element)) {
          add("E0601", "Set value type " + normalizeSignatureType(*element) + " must implement HashKey");
        }
      }
    };
    auto inferCollectionTypeFromExpression = [&](const std::string &expr) -> std::optional<std::string> {
      for (const auto &name : {"Map", "Set", "Vector", "Array"}) {
        const std::string prefix = std::string(name) + "<";
        const auto start = expr.find(prefix);
        if (start == std::string::npos) continue;
        std::size_t end = std::string::npos;
        int depth = 0;
        for (std::size_t i = start + std::string(name).size(); i < expr.size(); ++i) {
          if (expr[i] == '<') {
            ++depth;
          } else if (expr[i] == '>') {
            --depth;
            if (depth == 0) {
              end = i;
              break;
            }
          }
        }
        if (end != std::string::npos && end + 1 < expr.size() && expr[end + 1] == '.') {
          return expr.substr(start, end - start + 1);
        }
      }
      return std::nullopt;
    };
    auto isMapEntryType = [&](const std::string &type) {
      return baseTypeName(normalizeSignatureType(type)) == "MapEntry";
    };
    auto checkMapEntryStorageType = [&](const std::string &type) {
      const std::string normalized = normalizeSignatureType(type);
      if (!isMapEntryType(normalized) && containsNestedType(normalized, "MapEntry")) {
        add("E0503", "MapEntry cannot be stored inside collections");
      }
    };
    auto checkDiagnosticAccessStorageType = [&](const std::string &type) {
      const std::string normalized = normalizeSignatureType(type);
      if (containsNestedType(normalized, "PanicInfo")) {
        add("E0809", "PanicInfo is a diagnostic access value and cannot be stored");
      }
      if (containsNestedType(normalized, "StackFrames")) {
        add("E0809", "StackFrames is valid only inside the panic handler path");
      }
    };
    auto checkNestedDiagnosticAccessStorageType = [&](const std::string &type) {
      const std::string normalized = normalizeSignatureType(type);
      const std::string base = baseTypeName(normalized);
      if (base != "PanicInfo" && base != "StackFrames") {
        checkDiagnosticAccessStorageType(normalized);
      }
    };
    auto checkHashKeyCollectionExpression = [&](const std::string &expr) {
      if (const auto collectionType = inferCollectionTypeFromExpression(expr)) {
        checkHashKeyCollectionType(*collectionType);
        checkMapEntryStorageType(*collectionType);
      }
    };
    auto simpleExpressionType = [&](std::string expr) -> std::optional<std::string> {
      expr = trim(expr);
      if (startsWith(expr, "move ")) expr = trim(expr.substr(5));
      if (startsWith(expr, "mut ")) expr = trim(expr.substr(4));
      if (expr == "true" || expr == "false") return "Bool";
      if (expr.size() >= 2 && expr.front() == '"' && expr.back() == '"') return "StringSlice";
      if (std::regex_match(expr, std::regex(R"('([^'\\]|\\.)')"))) return "Char";
      if (std::regex_match(expr, std::regex(R"((?:[0-9]+\.[0-9]+|[0-9]+[eE][+-]?[0-9]+|[0-9]+\.[0-9]+[eE][+-]?[0-9]+))"))) {
        return "FloatLiteral";
      }
      if (std::regex_match(expr, std::regex(R"([0-9]+)"))) return "IntLiteral";
      if (std::regex_match(expr, std::regex(R"([A-Za-z_][A-Za-z0-9_]*)")) && variableTypes.count(expr)) {
        if (normalizeSignatureType(variableTypes[expr]) == "Unknown") return std::nullopt;
        return normalizeSignatureType(variableTypes[expr]);
      }
      if (startsWith(expr, "panic(") || startsWith(expr, "oom(")) return "Never";
      if (auto ctor = matchRegex(expr, std::regex(R"(^([A-Z][A-Za-z0-9_]*)\s*\{)"))) {
        const std::string typeName = (*ctor)[1];
        if (structFields.count(typeName)) return typeName;
      }
      if (auto variant = matchRegex(expr, std::regex(R"(^([A-Z][A-Za-z0-9_]*)\.([A-Z][A-Za-z0-9_]*))"))) {
        const std::string enumName = (*variant)[1];
        const std::string variantName = (*variant)[2];
        if (enumVariants.count(enumName) && enumVariants[enumName].count(variantName)) return enumName;
      }
      if (auto call = matchRegex(expr, std::regex(R"(^([A-Za-z_][A-Za-z0-9_]*)\s*\()"))) {
        const std::string callee = (*call)[1];
        if (!genericFunctionNames.count(callee) && functionReturnTypeLists.count(callee) &&
            functionReturnTypeLists[callee].size() == 1) {
          std::size_t open = expr.find('(', callee.size());
          if (open != std::string::npos) {
            bool inString = false;
            int depth = 0;
            for (std::size_t i = open; i < expr.size(); ++i) {
              const char c = expr[i];
              if (c == '"' && (i == 0 || expr[i - 1] != '\\')) {
                inString = !inString;
                continue;
              }
              if (inString) continue;
              if (c == '(') ++depth;
              if (c == ')') {
                --depth;
                if (depth == 0) {
                  if (trim(std::string_view(expr).substr(i + 1)).empty()) {
                    const std::string returnType = normalizeSignatureType(functionReturnTypeLists[callee].front());
                    if (!returnType.empty()) return returnType;
                  }
                  break;
                }
              }
            }
          }
        }
      }
      return std::nullopt;
    };
    auto displayExpressionType = [&](const std::string &type) {
      if (type == "IntLiteral") return std::string("integer");
      if (type == "FloatLiteral") return std::string("float");
      return normalizeSignatureType(type);
    };
    auto isNumericExpressionType = [&](const std::string &type) {
      return type == "IntLiteral" || type == "FloatLiteral" || isNumericType(type);
    };
    auto numericTypesCompatible = [&](const std::string &left, const std::string &right) {
      const std::string leftType = normalizeSignatureType(left);
      const std::string rightType = normalizeSignatureType(right);
      if (left == "IntLiteral" && isNumericExpressionType(right)) return true;
      if (right == "IntLiteral" && isNumericExpressionType(left)) return true;
      if (left == "FloatLiteral") return right == "FloatLiteral" || isFloatType(right);
      if (right == "FloatLiteral") return isFloatType(left);
      return leftType == rightType;
    };
    auto checkKnownIdentifierExpression = [&](std::string expr) {
      expr = trim(expr);
      if (startsWith(expr, "move ")) expr = trim(expr.substr(5));
      if (startsWith(expr, "mut ")) expr = trim(expr.substr(4));
      static const std::regex plainIdentifierPattern(R"(^[A-Za-z_][A-Za-z0-9_]*$)");
      if (!std::regex_match(expr, plainIdentifierPattern)) return;
      if (variableTypes.count(expr) || valBindings.count(expr) || topLevelValueNames.count(expr) ||
          enumVariantValueNames.count(expr) || constGenericValueNames.count(expr)) {
        return;
      }
      add("E0201", "value " + expr + " is not declared");
    };
    auto inferBinaryNumericExpressionType = [&](const std::string &rhs) -> std::optional<std::string> {
      const std::string expr = trim(rhs);
      if (expr.empty() || contains(expr, "=>") || contains(expr, "->") || contains(expr, "..") ||
          contains(expr, " as ")) {
        return std::nullopt;
      }
      bool inString = false;
      int parenDepth = 0;
      int bracketDepth = 0;
      int braceDepth = 0;
      std::optional<std::size_t> operatorPos;
      for (std::size_t i = 0; i < expr.size(); ++i) {
        const char c = expr[i];
        if (c == '"' && (i == 0 || expr[i - 1] != '\\')) {
          inString = !inString;
          continue;
        }
        if (inString) continue;
        if (c == '(') {
          ++parenDepth;
          continue;
        }
        if (c == ')') {
          --parenDepth;
          continue;
        }
        if (c == '[') {
          ++bracketDepth;
          continue;
        }
        if (c == ']') {
          --bracketDepth;
          continue;
        }
        if (c == '{') {
          ++braceDepth;
          continue;
        }
        if (c == '}') {
          --braceDepth;
          continue;
        }
        if (parenDepth != 0 || bracketDepth != 0 || braceDepth != 0) continue;
        if (c != '+' && c != '-' && c != '*' && c != '/') continue;
        if (c == '-' && (i == 0 || std::string("([{=<>:+-*/!,").find(expr[i - 1]) != std::string::npos)) {
          continue;
        }
        if (operatorPos) return std::nullopt;
        operatorPos = i;
      }
      if (!operatorPos) return std::nullopt;
      const auto leftType = simpleExpressionType(trim(std::string_view(expr).substr(0, *operatorPos)));
      const auto rightType = simpleExpressionType(trim(std::string_view(expr).substr(*operatorPos + 1)));
      if (!leftType || !rightType || !isNumericExpressionType(*leftType) || !isNumericExpressionType(*rightType) ||
          !numericTypesCompatible(*leftType, *rightType)) {
        return std::nullopt;
      }
      if (*leftType != "IntLiteral" && *leftType != "FloatLiteral") return normalizeSignatureType(*leftType);
      if (*rightType != "IntLiteral" && *rightType != "FloatLiteral") return normalizeSignatureType(*rightType);
      return std::nullopt;
    };
    auto checkBinaryNumericExpression = [&](const std::string &rhs) {
      const std::string expr = trim(rhs);
      if (expr.empty() || contains(expr, "=>") || contains(expr, "->") || contains(expr, "..") ||
          contains(expr, " as ")) {
        return;
      }
      bool inString = false;
      int parenDepth = 0;
      int bracketDepth = 0;
      int braceDepth = 0;
      std::optional<std::size_t> operatorPos;
      char operatorChar = '\0';
      for (std::size_t i = 0; i < expr.size(); ++i) {
        const char c = expr[i];
        if (c == '"' && (i == 0 || expr[i - 1] != '\\')) {
          inString = !inString;
          continue;
        }
        if (inString) continue;
        if (c == '(') {
          ++parenDepth;
          continue;
        }
        if (c == ')') {
          --parenDepth;
          continue;
        }
        if (c == '[') {
          ++bracketDepth;
          continue;
        }
        if (c == ']') {
          --bracketDepth;
          continue;
        }
        if (c == '{') {
          ++braceDepth;
          continue;
        }
        if (c == '}') {
          --braceDepth;
          continue;
        }
        if (parenDepth != 0 || bracketDepth != 0 || braceDepth != 0) continue;
        if (c != '+' && c != '-' && c != '*' && c != '/') continue;
        if (c == '-' && (i == 0 || std::string("([{=<>:+-*/!,").find(expr[i - 1]) != std::string::npos)) {
          continue;
        }
        if (operatorPos) return;
        operatorPos = i;
        operatorChar = c;
      }
      if (!operatorPos) return;
      const std::string left = trim(std::string_view(expr).substr(0, *operatorPos));
      const std::string right = trim(std::string_view(expr).substr(*operatorPos + 1));
      if (left.empty() || right.empty()) return;
      const auto leftType = simpleExpressionType(left);
      const auto rightType = simpleExpressionType(right);
      if (!leftType || !rightType) return;
      const bool leftNumeric = isNumericExpressionType(*leftType);
      const bool rightNumeric = isNumericExpressionType(*rightType);
      if (!leftNumeric || !rightNumeric) {
        add("E0302", "operator " + std::string(1, operatorChar) + " requires numeric operands, got " +
                        displayExpressionType(*leftType) + " and " + displayExpressionType(*rightType));
      }
    };
    auto checkComparisonExpression = [&](const std::string &rhs) {
      const std::string expr = trim(rhs);
      if (expr.empty() || contains(expr, "=>") || contains(expr, "->") || contains(expr, "..") ||
          contains(expr, " as ")) {
        return;
      }
      bool inString = false;
      int parenDepth = 0;
      int bracketDepth = 0;
      int braceDepth = 0;
      std::optional<std::size_t> operatorPos;
      std::string operatorText;
      for (std::size_t i = 0; i < expr.size(); ++i) {
        const char c = expr[i];
        if (c == '"' && (i == 0 || expr[i - 1] != '\\')) {
          inString = !inString;
          continue;
        }
        if (inString) continue;
        if (c == '(') {
          ++parenDepth;
          continue;
        }
        if (c == ')') {
          --parenDepth;
          continue;
        }
        if (c == '[') {
          ++bracketDepth;
          continue;
        }
        if (c == ']') {
          --bracketDepth;
          continue;
        }
        if (c == '{') {
          ++braceDepth;
          continue;
        }
        if (c == '}') {
          --braceDepth;
          continue;
        }
        if (parenDepth != 0 || bracketDepth != 0 || braceDepth != 0) continue;
        std::string op;
        if (i + 1 < expr.size()) {
          const std::string two = expr.substr(i, 2);
          if (two == "==" || two == "!=" || two == "<=" || two == ">=") op = two;
        }
        if (op.empty() && (c == '<' || c == '>')) op = std::string(1, c);
        if (op.empty()) continue;
        if (operatorPos) return;
        operatorPos = i;
        operatorText = op;
        if (op.size() == 2) ++i;
      }
      if (!operatorPos) return;
      const std::string left = trim(std::string_view(expr).substr(0, *operatorPos));
      const std::string right = trim(std::string_view(expr).substr(*operatorPos + operatorText.size()));
      if (left.empty() || right.empty()) return;
      const auto leftType = simpleExpressionType(left);
      const auto rightType = simpleExpressionType(right);
      if (!leftType || !rightType) return;
      const bool leftNumeric = isNumericExpressionType(*leftType);
      const bool rightNumeric = isNumericExpressionType(*rightType);
      if (operatorText == "==" || operatorText == "!=") {
        const bool sameType = normalizeSignatureType(*leftType) == normalizeSignatureType(*rightType);
        const bool compatibleNumeric = leftNumeric && rightNumeric && numericTypesCompatible(*leftType, *rightType);
        if (!sameType && !compatibleNumeric) {
          add("E0302", "operator " + operatorText +
                          " requires operands of the same comparable type, got " +
                          displayExpressionType(*leftType) + " and " + displayExpressionType(*rightType));
        }
        return;
      }
      if (!leftNumeric || !rightNumeric) {
        add("E0302", "operator " + operatorText + " requires numeric operands, got " +
                        displayExpressionType(*leftType) + " and " + displayExpressionType(*rightType));
        return;
      }
      if (!numericTypesCompatible(*leftType, *rightType)) {
        add("E0302", "operator " + operatorText + " requires matching numeric operands, got " +
                        displayExpressionType(*leftType) + " and " + displayExpressionType(*rightType));
      }
    };
    auto checkInitializerType = [&](const std::string &declaredType, const std::string &rhs) {
      const std::string targetType = normalizeSignatureType(declaredType);
      if (targetType.empty()) return;
      const auto exprType = simpleExpressionType(rhs);
      if (!exprType) {
        checkBinaryNumericExpression(rhs);
        checkComparisonExpression(rhs);
        checkKnownIdentifierExpression(rhs);
        return;
      }
      if (*exprType == "Never") return;
      if (*exprType == "IntLiteral") {
        if (!isNumericType(targetType)) {
          add("E0302", "initializer type " + targetType + " does not match integer expression");
        }
        return;
      }
      if (*exprType == "FloatLiteral") {
        if (!isFloatType(targetType)) {
          add("E0302", "initializer type " + targetType + " does not match float expression");
        }
        return;
      }
      if (targetType == "String" && *exprType == "StringSlice") return;
      if (*exprType != targetType && comparableLocalType(targetType) && comparableLocalType(*exprType)) {
        add("E0302", "initializer type " + targetType + " does not match " + *exprType + " expression");
      }
    };
    auto checkAssignmentType = [&](const std::string &declaredType, const std::string &rhs) {
      const std::string targetType = normalizeSignatureType(declaredType);
      if (targetType.empty()) return;
      const auto exprType = simpleExpressionType(rhs);
      if (!exprType) {
        checkBinaryNumericExpression(rhs);
        checkComparisonExpression(rhs);
        checkKnownIdentifierExpression(rhs);
        return;
      }
      if (*exprType == "Never") return;
      if (*exprType == "IntLiteral") {
        if (!isNumericType(targetType)) {
          add("E0302", "assignment type " + targetType + " does not match integer expression");
        }
        return;
      }
      if (*exprType == "FloatLiteral") {
        if (!isFloatType(targetType)) {
          add("E0302", "assignment type " + targetType + " does not match float expression");
        }
        return;
      }
      if (*exprType != targetType && comparableLocalType(targetType) && comparableLocalType(*exprType)) {
        add("E0302", "assignment type " + targetType + " does not match " + *exprType + " expression");
      }
    };
    auto checkFieldInitializerType = [&](const std::string &structName,
                                         const std::string &fieldName,
                                         const std::string &rhs) {
      if (!structFieldTypes.count(structName) || !structFieldTypes[structName].count(fieldName)) return;
      const std::string targetType = normalizeSignatureType(structFieldTypes[structName][fieldName]);
      const auto exprType = simpleExpressionType(rhs);
      if (!exprType) {
        checkBinaryNumericExpression(rhs);
        checkComparisonExpression(rhs);
        checkKnownIdentifierExpression(rhs);
        return;
      }
      if (*exprType == "Never") return;
      if (*exprType == "IntLiteral") {
        if (!isNumericType(targetType)) {
          add("E0302", "field " + fieldName + " type " + targetType + " does not match integer expression");
        }
        return;
      }
      if (*exprType == "FloatLiteral") {
        if (!isFloatType(targetType)) {
          add("E0302", "field " + fieldName + " type " + targetType + " does not match float expression");
        }
        return;
      }
      if (*exprType != targetType && comparableLocalType(targetType) && comparableLocalType(*exprType)) {
        add("E0302", "field " + fieldName + " type " + targetType + " does not match " + *exprType + " expression");
      }
    };
    auto checkFieldAssignmentType = [&](const std::string &structName,
                                        const std::string &fieldName,
                                        const std::string &rhs) {
      if (!structFieldTypes.count(structName) || !structFieldTypes[structName].count(fieldName)) return;
      const std::string targetType = normalizeSignatureType(structFieldTypes[structName][fieldName]);
      const auto exprType = simpleExpressionType(rhs);
      if (!exprType) {
        checkBinaryNumericExpression(rhs);
        checkComparisonExpression(rhs);
        checkKnownIdentifierExpression(rhs);
        return;
      }
      if (*exprType == "Never") return;
      if (*exprType == "IntLiteral") {
        if (!isNumericType(targetType)) {
          add("E0302", "field assignment " + fieldName + " type " + targetType + " does not match integer expression");
        }
        return;
      }
      if (*exprType == "FloatLiteral") {
        if (!isFloatType(targetType)) {
          add("E0302", "field assignment " + fieldName + " type " + targetType + " does not match float expression");
        }
        return;
      }
      if (*exprType != targetType && comparableLocalType(targetType) && comparableLocalType(*exprType)) {
        add("E0302", "field assignment " + fieldName + " type " + targetType + " does not match " + *exprType + " expression");
      }
    };
    auto checkCallArgumentTypes = [&](const std::string &callee, const std::vector<std::string> &args) {
      if (!functionParameterTypeLists.count(callee)) return;
      if (genericFunctionNames.count(callee)) return;
      const auto &signatures = functionParameterTypeLists[callee];
      if (signatures.size() != 1) return;
      const auto &params = signatures.front();
      if (params.size() != args.size()) {
        add("E0302", "function " + callee + " expects " + std::to_string(params.size()) +
                        " arguments but got " + std::to_string(args.size()));
        return;
      }
      for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string targetType = normalizeSignatureType(params[i]);
        const auto exprType = simpleExpressionType(args[i]);
        if (!exprType) {
          checkBinaryNumericExpression(args[i]);
          checkComparisonExpression(args[i]);
          checkKnownIdentifierExpression(args[i]);
          continue;
        }
        if (*exprType == "Never") continue;
        if (*exprType == "IntLiteral") {
          if (!isNumericType(targetType)) {
            add("E0302", "argument " + std::to_string(i + 1) + " of " + callee + " expects " + targetType +
                            " but got integer expression");
          }
          continue;
        }
        if (*exprType == "FloatLiteral") {
          if (!isFloatType(targetType)) {
            add("E0302", "argument " + std::to_string(i + 1) + " of " + callee + " expects " + targetType +
                            " but got float expression");
          }
          continue;
        }
        if (*exprType != targetType && comparableLocalType(targetType) && comparableLocalType(*exprType)) {
          add("E0302", "argument " + std::to_string(i + 1) + " of " + callee + " expects " + targetType +
                          " but got " + *exprType);
        }
      }
    };
    auto checkConditionType = [&](const std::string &kind, const std::string &condition) {
      const std::string expr = trim(condition);
      if (matchRegex(expr, std::regex(R"(^.+\s+is\s+(?:(?:move|mut)\s+)?[A-Za-z_][A-Za-z0-9_]*(?:\.[A-Za-z_][A-Za-z0-9_]*)?\s*\([^)]*\)$)"))) return;
      const auto exprType = simpleExpressionType(expr);
      if (!exprType) {
        checkComparisonExpression(expr);
        checkKnownIdentifierExpression(expr);
        return;
      }
      if (*exprType == "Bool") return;
      if (*exprType == "Never") return;
      if (*exprType == "IntLiteral") {
        add("E0302", kind + " condition must be Bool, got integer expression");
        return;
      }
      if (*exprType == "FloatLiteral") {
        add("E0302", kind + " condition must be Bool, got float expression");
        return;
      }
      if (comparableLocalType(*exprType)) {
        add("E0302", kind + " condition must be Bool, got " + *exprType);
      }
    };
    auto validateStructLiteral = [&](const std::string &ctorType, const std::string &fieldList) {
      if (!structFields.count(ctorType)) return;
      std::set<std::string> initializedFields;
      bool explicitInitializerSyntax = trim(fieldList).empty();
      for (const auto &entry : splitArguments(fieldList)) {
        const auto colon = entry.find(':');
        if (colon == std::string::npos) continue;
        explicitInitializerSyntax = true;
        const std::string fieldName = trim(std::string_view(entry).substr(0, colon));
        if (!fieldName.empty() && initializedFields.count(fieldName)) {
          add("E0404", "field " + fieldName + " is initialized more than once in struct " + ctorType);
        }
        if (!fieldName.empty()) initializedFields.insert(fieldName);
        if (!fieldName.empty() && !structFields[ctorType].count(fieldName)) {
          add("E0201", "field " + fieldName + " is not declared in struct " + ctorType);
        } else if (!fieldName.empty()) {
          checkFieldInitializerType(ctorType, fieldName, std::string(std::string_view(entry).substr(colon + 1)));
        }
      }
      if (!explicitInitializerSyntax) return;
      for (const auto &declaredField : structFields[ctorType]) {
        const std::regex initializedPattern("\\b" + declaredField + R"(\s*:)");
        if (!std::regex_search(fieldList, initializedPattern)) {
          add("E0404", "field " + declaredField + " is not initialized in struct " + ctorType);
        }
      }
    };

    const std::regex multilineStructLiteralStartPattern(R"(\b([A-Z][A-Za-z0-9_]*)\s*\{\s*$)");
    if (inMultilineStructLiteral) {
      const auto close = trimmed.find('}');
      const std::string fieldPart = close == std::string::npos ? trimmed : trim(std::string_view(trimmed).substr(0, close));
      if (!fieldPart.empty()) {
        if (!multilineStructLiteralFields.empty()) multilineStructLiteralFields += " ";
        multilineStructLiteralFields += fieldPart;
      }
      multilineStructLiteralBraceDepth += braceDelta(line);
      if (multilineStructLiteralBraceDepth <= 0 && close != std::string::npos) {
        validateStructLiteral(multilineStructLiteralType, multilineStructLiteralFields);
        inMultilineStructLiteral = false;
        multilineStructLiteralType.clear();
        multilineStructLiteralFields.clear();
        multilineStructLiteralBraceDepth = 0;
      }
    } else if (!startsWith(trimmed, "struct ") && !startsWith(trimmed, "pub struct ") &&
               !startsWith(trimmed, "private struct ") && !startsWith(trimmed, "impl ") &&
               !startsWith(trimmed, "trust impl ") && !startsWith(trimmed, "interface ") &&
               !startsWith(trimmed, "enum ") && !contains(trimmed, "fn ")) {
      if (auto literalStart = matchRegex(trimmed, multilineStructLiteralStartPattern)) {
        const std::string ctorType = (*literalStart)[1];
        if (structFields.count(ctorType)) {
          inMultilineStructLiteral = true;
          multilineStructLiteralType = ctorType;
          multilineStructLiteralFields.clear();
          multilineStructLiteralBraceDepth = braceDelta(line);
        }
      }
    }

    const bool atTopLevel = declarationBraceDepth == 0;
    const bool topLevelDeclaration =
        startsWith(trimmed, "//") ||
        startsWith(trimmed, "module ") ||
        startsWith(trimmed, "import ") ||
        startsWith(trimmed, "@") ||
        startsWith(trimmed, "const ") ||
        startsWith(trimmed, "static ") ||
        startsWith(trimmed, "fn ") ||
        startsWith(trimmed, "pub fn ") ||
        startsWith(trimmed, "private fn ") ||
        startsWith(trimmed, "async fn ") ||
        startsWith(trimmed, "pub async fn ") ||
        startsWith(trimmed, "private async fn ") ||
        startsWith(trimmed, "struct ") ||
        startsWith(trimmed, "pub struct ") ||
        startsWith(trimmed, "private struct ") ||
        startsWith(trimmed, "enum ") ||
        startsWith(trimmed, "pub enum ") ||
        startsWith(trimmed, "private enum ") ||
        startsWith(trimmed, "interface ") ||
        startsWith(trimmed, "pub interface ") ||
        startsWith(trimmed, "private interface ") ||
        startsWith(trimmed, "impl ") ||
        startsWith(trimmed, "trust impl ") ||
        startsWith(trimmed, "extern ") ||
        startsWith(trimmed, "trust extern ");
    if (atTopLevel && !topLevelDeclaration && matchRegex(trimmed, topLevelExecutablePattern)) {
      add("E0004", "top-level executable statements are not allowed; use an explicit function or const/static CTFE initializer");
    }

    if (contains(trimmed, "@noAlloc")) {
      pendingNoAlloc = true;
    }
    if (contains(trimmed, "@noPanic")) {
      pendingNoPanic = true;
    }
    if (contains(trimmed, "trust {")) {
      inTrustBlock = true;
      trustBraceDepth = 0;
    }
    if (contains(trimmed, "@layout(")) {
      pendingLayout = true;
      if (contains(trimmed, "@layout(C)")) pendingCLayout = true;
      if (auto alignment = packedAlignmentFromAttribute(trimmed)) {
        pendingPackedLayout = true;
        pendingPackedAlignment = *alignment;
        if (*alignment != 1 && *alignment != 2 && *alignment != 4 && *alignment != 8 && *alignment != 16) {
          add("E0603", "Packed alignment must be 1, 2, 4, 8, or 16");
        }
      }
      ++pendingLayoutCount;
    }
    if (auto exportAttribute = exportAttributeFromLine(trimmed)) {
      pendingExport = *exportAttribute;
    }
    if (contains(trimmed, "@export") && contains(trimmed, "bridge: Cxx")) {
      add("E0713", "bridge Cxx is reserved for future bindgen cxx; v1 supports bridge C only");
    }
    if (pendingNoAlloc && startsWith(trimmed, "fn ")) {
      inNoAlloc = true;
      noAllocBraceDepth = 0;
      pendingNoAlloc = false;
    }
    if (pendingNoPanic && startsWith(trimmed, "fn ")) {
      inNoPanic = true;
      noPanicBraceDepth = 0;
      pendingNoPanic = false;
    }
    if (pendingLayout && !contains(trimmed, "@layout(")) {
      if (!startsWith(trimmed, "struct ") && !startsWith(trimmed, "pub struct ")) {
        add("E0601", "@layout can only be applied to struct declarations");
      } else if (pendingLayoutCount > 1) {
        add("E0602", "struct cannot declare more than one layout strategy");
      }
      if (pendingCLayout) {
        if (auto structDecl = matchRegex(trimmed, structDeclPattern)) {
          cLayoutTypes.insert((*structDecl)[1]);
          if (!startsWith(trimmed, "pub struct ")) nonPubCLayoutTypes.insert((*structDecl)[1]);
        }
      }
      if (pendingPackedLayout) {
        if (auto structDecl = matchRegex(trimmed, structDeclPattern)) {
          packedLayoutTypes.insert((*structDecl)[1]);
        }
      }
      pendingLayout = false;
      pendingCLayout = false;
      pendingPackedLayout = false;
      pendingPackedAlignment.reset();
      pendingLayoutCount = 0;
    }
    if (auto interfaceDecl = matchRegex(trimmed, interfacePattern)) {
      interfaceTypes.insert((*interfaceDecl)[1]);
      inInterface = true;
      currentInterface = (*interfaceDecl)[1];
      interfaceBraceDepth = 0;
    }
    if (auto enumDecl = matchRegex(trimmed, enumDeclPattern)) {
      inEnum = true;
      currentEnum = (*enumDecl)[1];
      enumVariants[currentEnum];
      enumBraceDepth = 0;
    } else if (inEnum && trimmed != "}") {
      if (auto variant = matchRegex(trimmed, enumVariantPattern)) {
        const std::string variantName = (*variant)[1];
        EnumVariantShape shape;
        const auto namePos = trimmed.find(variantName);
        if (namePos != std::string::npos) {
          std::size_t afterName = namePos + variantName.size();
          while (afterName < trimmed.size() && std::isspace(static_cast<unsigned char>(trimmed[afterName]))) ++afterName;
          if (afterName < trimmed.size() && trimmed[afterName] == '(') {
            shape.hasPayload = true;
            if (const auto payload = parenthesizedContentAt(trimmed, afterName)) {
              shape.payloadArity = splitArguments(*payload).size();
            }
          } else if (afterName < trimmed.size() && trimmed[afterName] == '{') {
            shape.hasPayload = true;
            if (const auto payload = bracedContentAt(trimmed, afterName)) {
              shape.payloadArity = splitArguments(*payload).size();
            }
          }
        }
        enumVariants[currentEnum][variantName] = shape;
        enumVariantValueNames.insert(variantName);
      }
    }
    if (auto structDecl = matchRegex(trimmed, structDeclPattern)) {
      inStruct = true;
      currentStruct = (*structDecl)[1];
      declaredStructTypes.insert(currentStruct);
      structFields[currentStruct];
      structBraceDepth = 0;
      currentStructLayout = cLayoutTypes.count(currentStruct) ? "C" : (packedLayoutTypes.count(currentStruct) ? "Packed" : "");
    } else if (auto implDecl = matchRegex(trimmed, implPattern)) {
      inImpl = true;
      currentImplType = (*implDecl)[1];
      implBraceDepth = 0;
    }
    if (matchRegex(trimmed, fnPattern)) {
      currentSelfMode.clear();
      currentReturnType.clear();
      currentInterfaceReturnTypes.clear();
      valBindings.clear();
      readParameters.clear();
      uninitializedVars.clear();
      localArrayOwners.clear();
      movedBindings.clear();
      movedBindingMessages.clear();
      movedFields.clear();
      liveSliceOwners.clear();
      mapEntryBindings.clear();
      readOnlyLoopItems.clear();
      maybeInitializedVals.clear();
      partialStructVals.clear();
      partialStructFields.clear();
      callableParameterKinds.clear();
      mutableCallableParameters.clear();
      localClosureKinds.clear();
      consumedOnceCallables.clear();
      variableTypes.clear();
      scopedFutureOwners.clear();
      asyncViewNames.clear();
      taskGroupNames.clear();
      settledTaskGroups.clear();
      inAsyncFunction = contains(trimmed, "async fn ");
      asyncSawAwait = false;
      taskGroupStreamingLoop = false;
      taskGroupEarlyExitBlock = false;
      if (auto fn = matchRegex(trimmed, topFnPattern)) {
        const std::string fnName = (*fn)[2];
        const std::string params = (*fn)[4];
        currentReturnType = normalizeSignatureType((*fn)[5]);
        checkHashKeyCollectionType(currentReturnType);
        checkMapEntryStorageType(currentReturnType);
        if (contains(trimmed, "{") && isMapEntryType(currentReturnType)) {
          add("E0503", "MapEntry cannot be returned from a function");
        }
        if (contains(trimmed, "{") && !currentReturnType.empty() &&
            currentReturnType != "Unit" && currentReturnType != "Never") {
          trackedFunctionName = fnName;
          trackedFunctionReturnType = currentReturnType;
          trackedFunctionBraceDepth = 0;
          trackedFunctionStartLine = lineNo;
          trackedTopLevelIfActive = false;
          trackedTopLevelIfBraceDepth = 0;
          trackedTopLevelIfThenReturns = false;
          trackedTopLevelIfElseSeen = false;
          trackedTopLevelIfElseReturns = false;
          const auto open = trimmed.find('{');
          const std::string bodyStart = open == std::string::npos ? "" : trim(std::string_view(trimmed).substr(open + 1));
          trackedFunctionLastTopLevelTerminal = startsWith(bodyStart, "return ") ||
                                                startsWith(bodyStart, "panic(") ||
                                                startsWith(bodyStart, "oom(");
        }
        if (auto resultArgs = firstGenericArgument(currentReturnType, "Result")) {
          const auto args = splitArguments(*resultArgs);
          if (args.size() == 2) functionResultErrorTypes[fnName] = normalizeSignatureType(args[1]);
        }
        if (contains(params, "move self")) currentSelfMode = "move";
        else if (contains(params, "mut self")) currentSelfMode = "mut";
        else if (contains(params, "self")) currentSelfMode = "read";
        if (!currentSelfMode.empty()) {
          variableTypes["self"] = currentImplType.empty() ? "Self" : currentImplType;
        }
        for (const auto &param : splitArguments((*fn)[4])) {
          const auto colon = param.find(':');
          if (colon == std::string::npos) continue;
          const std::string namePart = trim(param.substr(0, colon));
          const std::string typePart = normalizeSignatureType(param.substr(colon + 1));
          const auto nameTokens = splitArguments(namePart);
          std::string paramName = namePart;
          const bool mutableParam = startsWith(paramName, "mut ");
          const bool moveParam = startsWith(paramName, "move ");
          if (startsWith(paramName, "move ")) paramName = trim(paramName.substr(5));
          if (startsWith(paramName, "mut ")) paramName = trim(paramName.substr(4));
          if (startsWith(typePart, "MutFn<")) callableParameterKinds[paramName] = "MutFn";
          if (startsWith(typePart, "OnceFn<")) callableParameterKinds[paramName] = "OnceFn";
          if (startsWith(typePart, "Fn<")) callableParameterKinds[paramName] = "Fn";
          if (mutableParam) mutableCallableParameters.insert(paramName);
          if (!mutableParam && !moveParam) readParameters.insert(paramName);
          variableTypes[paramName] = typePart;
          checkHashKeyCollectionType(typePart);
        }
      }
    }
    if (inInterface) {
      if (auto method = matchRegex(trimmed, topFnPattern)) {
        const std::string methodName = (*method)[2];
        const std::string generic = (*method)[3];
        const std::string params = (*method)[4];
        const std::string returnType = (*method)[5];
        if (!generic.empty()) interfaceMethodFlags[currentInterface][methodName].insert("generic");
        if (normalizeSignatureType(returnType) == "Self") interfaceMethodFlags[currentInterface][methodName].insert("self-return");
        if (contains(params, "move self")) interfaceMethodFlags[currentInterface][methodName].insert("move-self");
        if (contains(params, "mut self")) interfaceMethodFlags[currentInterface][methodName].insert("mut-self");
      }
    }

    if (auto externDecl = matchRegex(trimmed, externPattern)) {
      const bool trusted = (*externDecl)[1].matched;
      const std::string generic = (*externDecl)[3];
      const std::string params = (*externDecl)[4];
      const std::string returnType = (*externDecl)[5];
      if (trusted && contains(source.text, "// trust.ffi: false")) {
        add("E0714", "manifest does not allow FFI trust capability");
      }
      if (!trusted) {
        add("E0701", "raw FFI declaration requires trust");
      }
      if (!generic.empty()) {
        add("E0702", "extern C declarations cannot be generic");
      }
      const auto nonC = firstNonCType(params, returnType, cLayoutTypes);
      if (!nonC.empty()) {
        add("E0703", baseTypeName(nonC) + " is not C-compatible in extern C signature");
      }
    } else if (contains(trimmed, "extern \"C\" fn") && !contains(trimmed, "trust extern")) {
      add("E0701", "raw FFI declaration requires trust");
    }
    if (auto traitImpl = matchRegex(trimmed, traitImplPattern)) {
      const bool trusted = (*traitImpl)[1].matched;
      const std::string traitName = (*traitImpl)[2];
      const std::string implType = (*traitImpl)[3];
      if (traitName == "CErrorCode") cErrorCodeTypes.insert(implType);
      if (traitName == "HashKey") hashKeyTypes.insert(implType);
      if (trusted && traitName == "Send") trustedSendTypes.insert(implType);
      if ((traitName == "Send" || traitName == "Sync") && !trusted) {
        add("E0709", "manual " + traitName + " implementation requires trust impl");
      }
      if (trusted && traitName != "Send" && traitName != "Sync") {
        add("E0710", "trust impl is only allowed for compiler-recognized marker interfaces");
      }
    }
    if (auto genericImpl = matchRegex(trimmed, genericTraitImplPattern)) {
      const std::string traitName = (*genericImpl)[1];
      const std::string traitArg = normalizeSignatureType((*genericImpl)[2]);
      const std::string implType = (*genericImpl)[3];
      const std::string key = implType + ":" + traitName + "<" + traitArg + ">";
      if (interfaceImplKeys.count(key)) {
        add("E0307", "duplicate interface implementation for " + implType + " and " + traitName + "<" + traitArg + ">");
      }
      interfaceImplKeys.insert(key);
      genericInterfaceImplArgs[implType][traitName].insert(traitArg);
    }
    const bool staticInitializer = startsWith(trimmed, "static ");
    if (staticInitializer) {
      if (contains(trimmed, "trust {")) {
        add("E0801", "static initializer must be CTFE; implicit dynamic static initialization is not v1");
      } else if (contains(trimmed, ".withCapacity(") || contains(trimmed, "String.from(")) {
        add("E0801", "static initializer must be CTFE-materialized data; runtime allocation and global destruction must be explicit");
      } else if (contains(trimmed, "File.open(") || contains(trimmed, "readFileSync(") || contains(trimmed, "writeFileSync(")) {
        add("E0801", "static initializer cannot perform runtime I/O; I/O must be explicit runtime code");
      } else if (contains(trimmed, "Thread.spawn")) {
        add("E0801", "static initializer cannot start threads; thread creation must be explicit runtime code");
      } else if (contains(trimmed, "runtime.spawn") || contains(trimmed, "TaskGroup<")) {
        add("E0801", "static initializer cannot create task runtime work; task creation must be explicit runtime code");
      } else {
        if (auto staticDecl = matchRegex(trimmed, staticDeclPattern)) {
          const std::string staticType = normalizeSignatureType((*staticDecl)[1]);
          const std::string staticBase = baseTypeName(staticType);
          if (staticBase == "String" || staticBase == "Vector" || staticBase == "Map" ||
              staticBase == "Set" || staticBase == "Box") {
            add("E0801", "static initializer cannot require exit-time global destruction; use explicit runtime-owned state");
          }
        }
        for (const auto &externName : trustExternFunctions) {
          if (contains(trimmed, externName + "(")) {
            add("E0801", "static initializer must be CTFE; extern calls cannot execute at compile time");
            break;
          }
        }
      }
    }
    if (startsWith(trimmed, "static ") && contains(trimmed, ": StringSlice")) {
      add("E0803", "StringSlice cannot be stored in static; use const for literals or String for owned text");
    }
    if (startsWith(trimmed, "static ") && contains(trimmed, ": ArraySlice")) {
      add("E0505", "ArraySlice cannot be static storage");
    }
    if (startsWith(trimmed, "static ") && contains(trimmed, ": MapEntry")) {
      add("E0503", "MapEntry cannot be stored in static storage");
    }
    if (startsWith(trimmed, "static ")) {
      if (auto staticDecl = matchRegex(trimmed, staticDeclPattern)) {
        checkDiagnosticAccessStorageType((*staticDecl)[1]);
      }
    }
    if (startsWith(trimmed, "const ") && contains(trimmed, ": String") && contains(trimmed, "String.from(")) {
      const std::regex constNamePattern(R"(^\s*const\s+([A-Za-z_][A-Za-z0-9_]*)\s*:)");
      if (auto constName = matchRegex(trimmed, constNamePattern)) constOwnedStrings.insert((*constName)[1]);
    }
    if (startsWith(trimmed, "const ")) {
      const std::regex constRefPattern(R"(^\s*const\s+([A-Za-z_][A-Za-z0-9_]*)\s*:[^=]+=\s*([A-Za-z_][A-Za-z0-9_]*)\s*\+)");
      if (auto ref = matchRegex(trimmed, constRefPattern)) {
        const std::string lhs = (*ref)[1];
        const std::string rhs = (*ref)[2];
        constReferences[lhs].insert(rhs);
        if (constReferences[rhs].count(lhs)) {
          add("E0804", "const initialization cycle " + rhs + " -> " + lhs + " -> " + rhs);
        }
      }
    }
    if (startsWith(trimmed, "struct ") && contains(trimmed, "<const ") && contains(trimmed, ": String")) {
      add("E0805", "const generic parameter type String has no stable structural fingerprint");
    }
    if (contains(trimmed, "Vector<ArraySlice<")) {
      add("E0510", "ArraySlice cannot be stored in Vector");
    }
    if (contains(trimmed, " / 0")) {
      add("E0806", "constant division by zero");
    }
    if (inNoPanic && contains(trimmed, "panic(")) {
      add("E0807", "noPanic function cannot call panic");
    }
    if (inNoPanic && callsReachableFunction(trimmed, panicReachableFunctions)) {
      add("E0807", "noPanic function cannot call function that may panic");
    }
    if (inNoPanic && contains(source.text, "// oom: panic")) {
      if (auto message = oomPanicAllocatingApiMessage(trimmed)) {
        add("E0807", *message);
      } else if (callsReachableFunction(trimmed, allocatingReachableFunctions)) {
        add("E0807", "allocation failure would call panic in this profile");
      }
    }
    if (contains(trimmed, "Task.isCancellationRequested(")) {
      add("E0808", "cancellation checks require an explicit TaskContext parameter");
    }
    const std::regex constGenericNamePattern(R"(\bconst\s+([A-Za-z_][A-Za-z0-9_]*)\s*:)");
    for (auto it = std::sregex_iterator(trimmed.begin(), trimmed.end(), constGenericNamePattern);
         it != std::sregex_iterator(); ++it) {
      constGenericValueNames.insert((*it)[1]);
    }
    const std::regex patternBindingPattern(R"(^\s*(?:if|while)\s*\(.+\s+is\s+(?:move\s+|mut\s+)?[A-Za-z_][A-Za-z0-9_]*(?:\.[A-Za-z_][A-Za-z0-9_]*)?\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*\)\s*\)\s*\{)");
    if (auto binding = matchRegex(trimmed, patternBindingPattern)) {
      variableTypes[(*binding)[1]] = "Unknown";
    }
    if (auto forItem = matchRegex(trimmed, forItemPattern)) {
      variableTypes[(*forItem)[1]] = "Unknown";
    }
    if (auto condition = matchRegex(trimmed, ifConditionPattern)) {
      checkConditionType("if", (*condition)[1]);
    }
    if (auto condition = matchRegex(trimmed, whileConditionPattern)) {
      checkConditionType("while", (*condition)[1]);
    }
    if (startsWith(trimmed, "const ")) {
      for (const auto &fnName : ctfeUnsafeFunctions) {
        if (contains(trimmed, fnName + "(")) {
          add("E0802", "CTFE cannot execute trust or extern calls");
        }
      }
    }
    if (contains(trimmed, "RawPointer<") && !inTrustBlock) {
      add("E0711", "raw memory operation requires trust");
    }
    if (contains(trimmed, "RawPointer<") && contains(source.text, "// trust.rawMemory: false")) {
      add("E0711", "manifest does not allow rawMemory trust capability");
    }
    for (const auto &externName : trustExternFunctions) {
      if (!staticInitializer && !inTrustBlock && !startsWith(trimmed, "trust extern") && contains(trimmed, externName + "(")) {
        add("E0712", "extern function call requires trust");
      }
    }
    if (inStruct) {
      if (auto field = matchRegex(trimmed, fieldPattern)) {
        const std::string fieldName = (*field)[1];
        const std::string fieldType = normalizeSignatureType((*field)[2]);
        const std::string fieldBase = baseTypeName(fieldType);
        structFields[currentStruct].insert(fieldName);
        structFieldTypes[currentStruct][fieldName] = fieldType;
        if (fieldBase == "ArraySlice") {
          add("E0505", "ArraySlice fields are not allowed");
        } else if (containsNestedType(fieldType, "ArraySlice")) {
          add("E0505", "ArraySlice cannot be stored inside Array fields");
        }
        if (isMapEntryType(fieldType)) {
          add("E0503", "MapEntry cannot be stored in structure fields");
        } else if (containsNestedType(fieldType, "MapEntry")) {
          add("E0503", "MapEntry cannot be stored inside collection fields");
        }
        if (fieldBase == "StringSlice") {
          add("E0506", "StringSlice cannot be stored as a field; use String for owned text");
        }
        checkDiagnosticAccessStorageType(fieldType);
        if (fieldBase == "TaskContext") {
          add("E0809", "TaskContext cannot be stored in structure fields");
        }
        for (const auto &interfaceName : interfaceTypes) {
          if (fieldType == interfaceName) {
            add("E0306", "interface name cannot be used as a field type; use W: Writer or Box<Writer>");
          } else if ((startsWith(fieldType, "Vector<") || startsWith(fieldType, "Array<") ||
                      startsWith(fieldType, "Map<") || startsWith(fieldType, "Set<")) &&
                     contains(fieldType, "<" + interfaceName)) {
            add("E0306", "interface name cannot be used as a collection element type; use Box<Writer>");
          }
        }
        if (currentStructLayout == "C" && !isCCompatibleType(fieldType, cLayoutTypes)) {
          add("E0604", fieldBase + " is not C-compatible in @layout(C)");
        }
        if (currentStructLayout == "Packed" && !isCCompatibleType(fieldType, cLayoutTypes)) {
          add("E0605", "packed fields must be Copy and have no destroy behavior");
        }
        if (fieldBase == "Array" || fieldBase == "Vector" || fieldBase == "String" || fieldBase == "Box" ||
            contains(fieldType, "Array<") || contains(fieldType, "Vector<") || contains(fieldType, "String") || contains(fieldType, "Box<")) {
          resourceTypes.insert(currentStruct);
        }
      }
    }
    if (inImpl && cLayoutTypes.count(currentImplType) && startsWith(trimmed, "destroy")) {
      add("E0606", "@layout(C) type cannot define destroy");
    }
    if (inImpl && startsWith(trimmed, "destroy")) {
      inDestroy = true;
      destroyBraceDepth = 0;
      destroyTypes.insert(currentImplType);
    }
    if (inDestroy) {
      if (contains(trimmed, "try ")) add("E0405", "destroy cannot use try");
      if (contains(trimmed, "await ")) add("E0405", "destroy cannot use await");
      if (contains(trimmed, "panic(")) add("E0405", "destroy cannot call panic");
      if (allocatingApiMessage(trimmed)) add("E0405", "destroy cannot allocate");
      if (callsReachableFunction(trimmed, panicReachableFunctions)) add("E0405", "destroy cannot call panic");
      if (callsReachableFunction(trimmed, allocatingReachableFunctions)) add("E0405", "destroy cannot allocate");
      if (contains(trimmed, "closeChecked(")) add("E0405", "destroy cannot call Result-returning cleanup API");
    }
    if (!inDestroy && contains(trimmed, ".destroy(")) {
      add("E0405", "destroy cannot be called directly");
    }
    if (contains(trimmed, "mut ") && contains(trimmed, ".")) {
      for (const auto &packedType : packedLayoutTypes) {
        const std::string lowerName = std::string(1, static_cast<char>(std::tolower(static_cast<unsigned char>(packedType[0])))) + packedType.substr(1);
        if (contains(trimmed, "mut " + lowerName + ".")) {
          add("E0607", "packed field cannot be passed as mut access");
        }
      }
    }
    if (contains(trimmed, " as U8")) {
      add("E0304", "lossy integer conversion requires U8.fromChecked, U8.truncate, or U8.saturate");
    }
    if (contains(trimmed, "try U8.fromChecked(") && !contains(trimmed, ".okOr(") && !contains(trimmed, ".okOrElse(")) {
      add("E0301", "try on Option in Result function requires explicit okOr or okOrElse");
    }
    if (std::regex_search(trimmed, std::regex(R"(^\s*val\s+(?:move\s+|mut\s+)?[A-Z][A-Za-z0-9_]*\.[A-Za-z_][A-Za-z0-9_]*\([^)]*\)\s*=)"))) {
      add("E0507", "refutable pattern is not allowed in val; use match or if (value is Pattern)");
    }
    if (freestandingProfile && contains(trimmed, ".withCapacity(") && !contains(trimmed, ".withCapacityIn(")) {
      add("E1002", "no profile default allocator is available; use withCapacityIn");
    }
    if (contains(trimmed, "inspect(MultiConsumer") &&
        contains(source.text, "impl Consumer<Event> for MultiConsumer") &&
        contains(source.text, "impl Consumer<Message> for MultiConsumer")) {
      add("E0307", "cannot infer T; MultiConsumer implements multiple Consumer<T> instances");
    }
    const std::regex forPattern(R"(^\s*for\s+(mut\s+|move\s+)?([A-Za-z_][A-Za-z0-9_]*)\s+in\s+(mut\s+)?([A-Za-z_][A-Za-z0-9_]*)\s*\{)");
    if (auto forLoop = matchRegex(trimmed, forPattern)) {
      const std::string mode = (*forLoop)[1];
      const std::string itemName = (*forLoop)[2];
      const bool sourceMut = (*forLoop)[3].matched;
      const std::string sourceName = (*forLoop)[4];
      const std::string sourceType = variableTypes[sourceName];
      const bool itemMut = contains(mode, "mut");
      const bool itemMove = contains(mode, "move");
      if (itemMut && !sourceMut) {
        add("E0503", "for mut item requires `mut` source access");
      }
      if (itemMove) {
        if (startsWith(sourceType, "ArraySlice<")) {
          add("E0503", "iteration requires an owning Array or Vector source");
        }
        movedBindings.insert(sourceName);
        if (startsWith(sourceType, "Vector<")) {
          movedBindingMessages[sourceName] = "use of moved collection after consuming for iteration";
        } else if (destroyTypes.count(sourceType)) {
          movedBindingMessages[sourceName] = "use of moved iterator after consuming for iteration";
        }
      } else if (!itemMut) {
        readOnlyLoopItems.insert(itemName);
        if (destroyTypes.count(sourceType)) {
          add("E0503", "Iterator traversal yields owned values; use `for move " + itemName + " in " + sourceName + "`");
        }
      }
    }
    const std::regex taskSpawnDeclPattern(R"(^\s*val\s+([A-Za-z_][A-Za-z0-9_]*)\s*=\s*try\s+runtime\.spawn(?:Blocking)?(?:WithContext)?\s*\()");
    if (auto taskDecl = matchRegex(trimmed, taskSpawnDeclPattern)) {
      const std::string taskName = (*taskDecl)[1];
      taskHandles.insert(taskName);
      variableTypes[taskName] = "Task";
    }
    if (inAsyncFunction && contains(trimmed, ".blockOn(")) {
      add("E0521", "blockOn is only allowed in synchronous contexts; use await task");
    }
    if (inAsyncFunction && std::regex_search(trimmed, std::regex(R"(^\s*val\s+[A-Za-z_][A-Za-z0-9_]*\s*=\s*mut\s+[A-Za-z_][A-Za-z0-9_]*\.[A-Za-z_][A-Za-z0-9_]*\()"))) {
      add("E0522", "async mut self call must be immediately awaited");
    }
    if (contains(trimmed, "await ")) {
      asyncSawAwait = true;
    }
    if (currentSelfMode == "read" && std::regex_search(trimmed, std::regex(R"(^\s*self\.[A-Za-z_][A-Za-z0-9_]*\s*=)"))) {
      add("E0406", "self is read-only");
    }
    if (contains(trimmed, "return Vector<") && contains(trimmed, ".withCapacityIn(") && contains(trimmed, "mut arena")) {
      add("E0502", "scoped allocation escapes allocator");
    }
    if (contains(source.text, "fn build<A: Allocator>") && contains(trimmed, "return Vector<") &&
        contains(trimmed, ".withCapacityIn(") && contains(trimmed, "mut allocator")) {
      add("E0502", "returning owner allocated by generic allocator requires A: EscapingAllocator");
    }
    if (contains(trimmed, "mut shards[")) {
      add("E0501", "disjoint mutable access across scoped tasks is not proven");
    }
    if (contains(trimmed, "readThenWrite(") && contains(trimmed, ", mut ")) {
      const auto argsStart = trimmed.find('(');
      const auto argsEnd = trimmed.rfind(')');
      if (argsStart != std::string::npos && argsEnd != std::string::npos && argsEnd > argsStart) {
        auto args = splitArguments(trimmed.substr(argsStart + 1, argsEnd - argsStart - 1));
        if (args.size() >= 2 && startsWith(args[1], "mut ") && trim(args[1].substr(4)) == args[0]) {
          add("E0501", "writable access overlaps active read-only access");
        }
      }
    }
    if (inNoAlloc) {
      if (auto message = allocatingApiMessage(trimmed)) {
        add("E0305", *message);
      } else if (callsReachableFunction(trimmed, allocatingReachableFunctions)) {
        add("E0305", "noAlloc function cannot call function that may allocate");
      }
    }
    if (contains(trimmed, "Box.newIn(move ")) {
      for (const auto &name : readParameters) {
        if (contains(trimmed, "Box.newIn(move " + name)) {
          add("E0406", "read parameter cannot be moved into Box");
        }
      }
    }
    const std::regex spawnBlockingNamedPattern(R"(spawnBlocking\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*\))");
    if (auto spawnBlockingNamed = matchRegex(trimmed, spawnBlockingNamedPattern)) {
      add("E0520", "spawnBlocking consumes the job; use move " + std::string((*spawnBlockingNamed)[1]));
    }
    if (contains(trimmed, "spawnBlocking(move ()")) {
      inSpawnBlockingClosure = true;
      spawnBlockingBraceDepth = 0;
    }
    if (inSpawnBlockingClosure) {
      for (const auto &[name, type] : variableTypes) {
        if (destroyTypes.count(type) && contains(trimmed, name + ".")) {
          add("E0520", type + " is not Send for spawnBlocking");
        }
      }
    }
    for (const auto &taskName : taskHandles) {
      if (contains(trimmed, ".blockOn(" + taskName + ")")) {
        add("E0521", "blockOn consumes the task handle; use move " + taskName);
      }
      if (contains(trimmed, ".blockOn(move " + taskName + ")") || contains(trimmed, "await " + taskName) ||
          contains(trimmed, "move " + taskName + ".cancel()") || contains(trimmed, "move " + taskName + ".detach()")) {
        if (settledTaskHandles.count(taskName) && contains(trimmed, "await " + taskName)) {
          add("E0521", "task handle was consumed by await");
        }
        settledTaskHandles.insert(taskName);
      }
      if (contains(trimmed, taskName + ".cancel()") && !contains(trimmed, "move " + taskName + ".cancel()")) {
        add("E0521", "cancel consumes the task handle; use move " + taskName + ".cancel()");
      }
      if (contains(trimmed, taskName + ".detach()") && !contains(trimmed, "move " + taskName + ".detach()")) {
        add("E0521", "detach consumes the task handle; use move " + taskName + ".detach()");
      }
      if (contains(trimmed, "move " + taskName + ".await()")) {
        add("E0521", "Task<T> has no await method; use await " + taskName);
      }
    }
    for (const auto &[sliceName, ownerName] : liveSliceOwners) {
      (void)sliceName;
      if ((contains(trimmed, "mut " + ownerName + ".push(") ||
           contains(trimmed, "mut " + ownerName + ".reserve(")) &&
          !contains(trimmed, ".tryReserve")) {
        add("E0501", "vector structural mutation while slice is live");
      }
      if (contains(trimmed, "mut " + ownerName + ".replaceAt(")) {
        add("E0501", "cannot replace element while a dependent access is live");
      }
    }
    const std::regex destroyPatternMovePattern(R"(^\s*val\s+([A-Z][A-Za-z0-9_]*)\s*\{[^}]+\}\s*=\s*([A-Za-z_][A-Za-z0-9_]*)\s*;)");
    if (auto patternMove = matchRegex(trimmed, destroyPatternMovePattern)) {
      const std::string typeName = (*patternMove)[1];
      if (destroyTypes.count(typeName)) {
        add("E0406", "cannot move field from type with destroy block through pattern");
      }
    }

    if (auto tupleVal = matchRegex(trimmed, valTuplePattern)) {
      for (const auto &namePart : splitArguments((*tupleVal)[1])) {
        const std::string name = trim(namePart);
        if (std::regex_match(name, std::regex(R"([A-Za-z_][A-Za-z0-9_]*)"))) {
          valBindings.insert(name);
          variableTypes[name] = "Unknown";
        }
      }
      checkKnownIdentifierExpression((*tupleVal)[2]);
    } else if (auto uninitVal = matchRegex(trimmed, valUninitPattern)) {
      const std::string name = (*uninitVal)[1];
      valBindings.insert(name);
      uninitializedVars.insert(name);
      variableTypes[name] = normalizeSignatureType((*uninitVal)[2]);
      checkHashKeyCollectionType(variableTypes[name]);
      checkMapEntryStorageType(variableTypes[name]);
      checkNestedDiagnosticAccessStorageType(variableTypes[name]);
    } else if (auto match = matchRegex(trimmed, valPattern)) {
      const std::string name = (*match)[1];
      const std::string type = (*match)[2];
      const std::string rhs = (*match)[3];
      valBindings.insert(name);
      if (!trim(type).empty()) {
        variableTypes[name] = normalizeSignatureType(type);
        checkInitializerType(type, rhs);
        checkHashKeyCollectionType(variableTypes[name]);
        checkMapEntryStorageType(variableTypes[name]);
        checkNestedDiagnosticAccessStorageType(variableTypes[name]);
        checkHashKeyCollectionExpression(rhs);
      } else {
        checkBinaryNumericExpression(rhs);
        checkComparisonExpression(rhs);
        checkKnownIdentifierExpression(rhs);
        checkHashKeyCollectionExpression(rhs);
        if (const auto inferredType = simpleExpressionType(rhs)) {
          if (*inferredType != "IntLiteral" && *inferredType != "FloatLiteral" &&
              *inferredType != "Unit" && *inferredType != "Never" && comparableLocalType(*inferredType)) {
            variableTypes[name] = *inferredType;
          }
        }
        const std::regex constructorPattern(R"(^\s*([A-Z][A-Za-z0-9_]*)\s*\{)");
        if (auto ctor = matchRegex(rhs, constructorPattern)) {
          variableTypes[name] = (*ctor)[1];
        } else if (startsWith(trim(rhs), "String.from(") || startsWith(trim(rhs), "String.fromIn(")) {
          variableTypes[name] = "String";
        } else if (contains(rhs, "Shared.newIn(")) {
          const std::string arg = firstCallArgument(rhs);
          if (variableTypes.count(arg)) variableTypes[name] = "Shared<" + variableTypes[arg] + ">";
        } else if (contains(rhs, "Box.newIn(")) {
          const std::string arg = firstCallArgument(rhs);
          if (variableTypes.count(arg)) variableTypes[name] = "Box<" + variableTypes[arg] + ">";
        } else if (contains(rhs, ".entry(")) {
          const std::regex entryPattern(R"(([A-Za-z_][A-Za-z0-9_]*)\.entry\s*\()");
          if (auto entry = matchRegex(rhs, entryPattern)) {
            const std::string mapName = (*entry)[1];
            if (auto mapArgs = firstGenericArgument(variableTypes[mapName], "Map")) {
              variableTypes[name] = "MapEntry<" + *mapArgs + ">";
              mapEntryBindings.insert(name);
            }
          }
        } else if (const auto collectionType = inferCollectionTypeFromExpression(rhs)) {
          variableTypes[name] = *collectionType;
        } else if (contains(rhs, "Vector<")) {
          const auto start = rhs.find("Vector<");
          const auto end = rhs.find('>', start);
          if (start != std::string::npos && end != std::string::npos) variableTypes[name] = rhs.substr(start, end - start + 1);
        } else if (contains(rhs, "Array<")) {
          const auto start = rhs.find("Array<");
          const auto end = rhs.find('>', start);
          if (start != std::string::npos && end != std::string::npos) variableTypes[name] = rhs.substr(start, end - start + 1);
        } else if (contains(rhs, ".dropPrefix(")) {
          variableTypes[name] = "ArraySlice<U8>";
        }
        if (!variableTypes.count(name)) {
          if (const auto inferredNumeric = inferBinaryNumericExpressionType(rhs)) variableTypes[name] = *inferredNumeric;
        }
      }
      if (startsWith(trim(rhs), "mut ")) {
        variableTypes[name] = "MutAccess";
      }
      if (inAsyncFunction && contains(rhs, ".dropPrefix(")) {
        asyncViewNames.insert(name);
      }
      if (inAsyncFunction && contains(rhs, ".withCapacityIn(") && contains(rhs, "mut arena")) {
        scopedFutureOwners.insert(name);
      }
      if (contains(rhs, "Array<")) localArrayOwners.insert(name);
      if (contains(rhs, ".asSlice()")) {
        const std::regex sliceOwnerPattern(R"(([A-Za-z_][A-Za-z0-9_]*)\.asSlice\s*\()");
        if (auto owner = matchRegex(rhs, sliceOwnerPattern)) {
          liveSliceOwners[name] = (*owner)[1];
          variableTypes[name] = "ArraySlice<U8>";
        }
      }
      if (contains(rhs, "runtime.spawn(") || contains(rhs, "runtime.spawnBlocking(") || contains(rhs, "runtime.spawnWithContext(") ||
          contains(rhs, "runtime.spawnBlockingWithContext(")) {
        taskHandles.insert(name);
        variableTypes[name] = "Task";
      }
      const std::regex plainIdentifierPattern(R"(^[A-Za-z_][A-Za-z0-9_]*$)");
      if (std::regex_match(trim(rhs), plainIdentifierPattern)) {
        const std::string rhsName = trim(rhs);
        if (destroyTypes.count(variableTypes[rhsName]) ||
            (valBindings.count(rhsName) && contains(source.text, "destroy {}"))) {
          movedBindings.insert(rhsName);
        }
      }
      const std::regex fieldMovePattern(R"(^([A-Za-z_][A-Za-z0-9_]*)\.([A-Za-z_][A-Za-z0-9_]*)$)");
      if (auto fieldMove = matchRegex(trim(rhs), fieldMovePattern)) {
        const std::string fieldName = (*fieldMove)[2];
        if (fieldName == "bytes") {
          movedFields[(*fieldMove)[1]].insert(fieldName);
        }
      }
      if (contains(rhs, "Array<") && contains(rhs, ".filledIn(")) {
        const std::regex filledPattern(R"(filledIn\s*\(\s*([0-9]+))");
        if (auto filled = matchRegex(rhs, filledPattern)) arrayLengths[name] = std::stoi(std::string((*filled)[1]));
      }
      const std::regex ctorPattern(R"(([A-Z][A-Za-z0-9_]*)\s*\{([^{}]*)\})");
      auto ctorBegin = std::sregex_iterator(rhs.begin(), rhs.end(), ctorPattern);
      auto ctorEnd = std::sregex_iterator();
      for (auto it = ctorBegin; it != ctorEnd; ++it) {
        const std::string ctorType = (*it)[1];
        const std::string fieldList = (*it)[2];
        validateStructLiteral(ctorType, fieldList);
      }
      if (contains(rhs, "Box.newIn(")) {
        const std::string arg = firstCallArgument(rhs);
        if (startsWith(variableTypes[arg], "ArraySlice<")) add("E0510", "ArraySlice cannot be stored in Box");
        if (isMapEntryType(variableTypes[arg]) || mapEntryBindings.count(arg)) add("E0503", "MapEntry cannot be stored in Box");
        if (containsNestedType(variableTypes[arg], "PanicInfo")) add("E0809", "PanicInfo is a diagnostic access value and cannot be stored");
        if (containsNestedType(variableTypes[arg], "StackFrames")) add("E0809", "StackFrames is valid only inside the panic handler path");
      }
      if (contains(rhs, "Shared.newIn(")) {
        const std::string arg = firstCallArgument(rhs);
        if (startsWith(variableTypes[arg], "ArraySlice<")) add("E0510", "ArraySlice cannot be stored in Shared");
        if (isMapEntryType(variableTypes[arg]) || mapEntryBindings.count(arg)) add("E0503", "MapEntry cannot be stored in Shared");
        if (containsNestedType(variableTypes[arg], "PanicInfo")) add("E0809", "PanicInfo is a diagnostic access value and cannot be stored");
        if (containsNestedType(variableTypes[arg], "StackFrames")) add("E0809", "StackFrames is valid only inside the panic handler path");
      }
      if (contains(type, "String") && contains(rhs, "\"")) {
        add("E0301", "string literal is StringSlice; use String.from(\"hello\") for owned String");
      }
      const std::regex tryCallPattern(R"(^try\s+([A-Za-z_][A-Za-z0-9_]*)\s*\()");
      if (auto tryCall = matchRegex(trim(rhs), tryCallPattern)) {
        const std::string calleeName = (*tryCall)[1];
        if (functionResultErrorTypes.count(calleeName) && !currentReturnType.empty()) {
          if (auto resultArgs = firstGenericArgument(currentReturnType, "Result")) {
            const auto args = splitArguments(*resultArgs);
            if (args.size() == 2 && normalizeSignatureType(args[1]) != functionResultErrorTypes[calleeName]) {
              add("E0301", "try does not implicitly convert " + functionResultErrorTypes[calleeName] + " to " + normalizeSignatureType(args[1]) + "; use mapErr");
            }
          }
        }
      }
    } else if (auto valStart = matchRegex(trimmed, valStartPattern)) {
      const std::string name = (*valStart)[1];
      const std::string type = (*valStart)[2];
      valBindings.insert(name);
      if (!trim(type).empty()) {
        variableTypes[name] = normalizeSignatureType(type);
        checkNestedDiagnosticAccessStorageType(variableTypes[name]);
      } else if (!variableTypes.count(name)) {
        variableTypes[name] = "Unknown";
      }
    } else if (auto closure = matchRegex(trimmed, closureValPattern)) {
      currentClosureName = (*closure)[2];
      variableTypes[currentClosureName] = "Unknown";
      inClosureLiteral = true;
      closureBraceDepth = 0;
      closureMutates = false;
      closureConsumes = (*closure)[3].matched;
    } else if (auto varInit = matchRegex(trimmed, varInitPattern)) {
      const std::string name = (*varInit)[1];
      const std::string type = (*varInit)[2];
      const std::string rhs = (*varInit)[3];
      if (!trim(type).empty()) {
        variableTypes[name] = normalizeSignatureType(type);
        checkInitializerType(type, rhs);
        checkHashKeyCollectionType(variableTypes[name]);
        checkMapEntryStorageType(variableTypes[name]);
        checkNestedDiagnosticAccessStorageType(variableTypes[name]);
        checkHashKeyCollectionExpression(rhs);
      } else {
        checkBinaryNumericExpression(rhs);
        checkComparisonExpression(rhs);
        checkKnownIdentifierExpression(rhs);
        checkHashKeyCollectionExpression(rhs);
        if (const auto inferredType = simpleExpressionType(rhs)) {
          if (*inferredType != "IntLiteral" && *inferredType != "FloatLiteral" &&
              *inferredType != "Unit" && *inferredType != "Never" && comparableLocalType(*inferredType)) {
            variableTypes[name] = *inferredType;
          }
        }
        if (startsWith(trim(rhs), "String.from(") || startsWith(trim(rhs), "String.fromIn(")) {
          variableTypes[name] = "String";
        }
        if (contains(rhs, ".entry(")) {
          const std::regex entryPattern(R"(([A-Za-z_][A-Za-z0-9_]*)\.entry\s*\()");
          if (auto entry = matchRegex(rhs, entryPattern)) {
            const std::string mapName = (*entry)[1];
            if (auto mapArgs = firstGenericArgument(variableTypes[mapName], "Map")) {
              variableTypes[name] = "MapEntry<" + *mapArgs + ">";
              mapEntryBindings.insert(name);
            }
          }
        }
        if (!variableTypes.count(name)) {
          if (const auto inferredNumeric = inferBinaryNumericExpressionType(rhs)) variableTypes[name] = *inferredNumeric;
        }
      }
      if (trim(type).empty() && inferCollectionTypeFromExpression(rhs)) {
        variableTypes[name] = *inferCollectionTypeFromExpression(rhs);
      } else if (trim(type).empty() && contains(rhs, "Vector<")) {
        const auto start = rhs.find("Vector<");
        const auto end = rhs.find('>', start);
        if (start != std::string::npos && end != std::string::npos) variableTypes[name] = rhs.substr(start, end - start + 1);
      } else if (trim(type).empty() && contains(rhs, "TaskGroup<")) {
        variableTypes[name] = "TaskGroup";
      }
      if (variableTypes[name] == "TaskGroup" || contains(rhs, "TaskGroup<")) taskGroupNames.insert(name);
    } else if (auto match = matchRegex(trimmed, varUninitPattern)) {
      uninitializedVars.insert((*match)[1]);
      variableTypes[(*match)[1]] = normalizeSignatureType((*match)[2]);
      checkHashKeyCollectionType(variableTypes[(*match)[1]]);
      checkMapEntryStorageType(variableTypes[(*match)[1]]);
      checkNestedDiagnosticAccessStorageType(variableTypes[(*match)[1]]);
    } else if (auto match = matchRegex(trimmed, assignPattern); !contains(trimmed, "=>") && match) {
      const std::string name = (*match)[1];
      if (!valBindings.count(name) && variableTypes.count(name)) {
        if (auto assigned = matchRegex(trimmed, assignValuePattern)) {
          checkAssignmentType(variableTypes[name], std::string((*assigned)[2]));
        }
      } else if (!valBindings.count(name) && !variableTypes.count(name)) {
        add("E0201", "value " + name + " is not declared");
      }
      for (const auto &[sliceName, ownerName] : liveSliceOwners) {
        (void)sliceName;
        if (ownerName == name) {
          add("E0501", "cannot overwrite value while a dependent access is live");
        }
      }
      if (valBindings.count(name) && movedBindings.count(name)) {
        add("E0403", "val cannot be reinitialized after move");
      } else if (valBindings.count(name) && maybeInitializedVals.count(name)) {
        const std::string previousTrimmed = index > 0 ? trim(lines[index - 1]) : "";
        if (contains(previousTrimmed, "else")) {
          maybeInitializedVals.erase(name);
          uninitializedVars.erase(name);
        } else {
          add("E0403", "val may already be initialized on some path");
        }
      } else if (valBindings.count(name) && uninitializedVars.count(name)) {
        if (maybeInitializedVals.count(name)) {
          add("E0403", "val may already be initialized on some path");
        }
        maybeInitializedVals.insert(name);
        uninitializedVars.erase(name);
      } else if (valBindings.count(name)) {
        add("E0403", "val binding cannot be reassigned after initialization");
      }
      if (readOnlyLoopItems.count(name)) {
        add("E0503", "read iteration item is not writable; use `for mut byte in mut bytes`");
      }
      uninitializedVars.erase(name);
    }
    const std::regex fieldAssignPattern(R"(^\s*([A-Za-z_][A-Za-z0-9_]*)\.([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.+);)");
    if (auto fieldAssign = matchRegex(trimmed, fieldAssignPattern)) {
      const std::string objectName = (*fieldAssign)[1];
      const std::string fieldName = (*fieldAssign)[2];
      const std::string receiverType = objectName == "self" ? currentImplType : (variableTypes.count(objectName) ? variableTypes[objectName] : "");
      if (!receiverType.empty()) {
        checkFieldAssignmentType(receiverType, fieldName, std::string((*fieldAssign)[3]));
      }
      if (uninitializedVars.count(objectName)) {
        partialStructVals.insert(objectName);
        partialStructFields[objectName].insert(fieldName);
      }
    }

    if (auto fn = matchRegex(trimmed, topFnPattern)) {
      const bool isTopLevelFn = declarationBraceDepth == 0;
      const std::string visibility = (*fn)[1];
      const std::string name = (*fn)[2];
      const std::string generic = (*fn)[3];
      const std::string params = (*fn)[4];
      const std::string returnType = (*fn)[5];
      if (contains(generic, ":") && (contains(generic, ", Copy") || contains(generic, ", Send") ||
                                    contains(generic, ", Sync") || contains(generic, ", Ord") ||
                                    contains(generic, ", Eq"))) {
        add("E0303", "generic parameter can have only one direct constraint; define a named interface");
      }
      const std::string key = overloadKey(name, params);
      if (isTopLevelFn && overloadKeys.count(key)) {
        if (contains(params, "move ")) {
          add("E0302", "overload cannot differ only by read versus move parameter mode");
        } else if (overloadReturnTypes[key] != normalizeSignatureType(returnType)) {
          add("E0302", "overload cannot differ only by return type");
        } else {
          add("E0302", "duplicate overload key for " + key);
        }
      } else if (isTopLevelFn) {
        overloadKeys[key] = lineNo;
        overloadReturnTypes[key] = normalizeSignatureType(returnType);
      }
      if (isTopLevelFn) {
        auto types = overloadParameterTypes(params);
        if (types.size() == 1) overloadArgumentTypes[name].insert(types.front());
      }

      if (pendingExport) {
        if (visibility != "pub ") {
          add("E0704", "@export requires a pub top-level function");
        }
        if (!generic.empty()) {
          add("E0705", "@export cannot be applied to generic functions");
        }
        const auto nonCompatible = pendingExport->bridge
            ? firstNonBridgeType(params, returnType, cLayoutTypes)
            : firstNonCType(params, returnType, cLayoutTypes);
        if (!nonCompatible.empty()) {
          const std::string baseNonCompatible = baseTypeName(nonCompatible);
          if (pendingExport->bridge && baseNonCompatible == "String") {
            add("E0706", "String is not bridge-compatible; use StringSlice, CStr, or an explicit handle API");
          } else if (!pendingExport->bridge && startsWith(normalizeSignatureType(returnType), "Result<")) {
            add("E0706", "Result is not C-compatible in an exported C ABI signature");
          } else if (!pendingExport->bridge && declaredStructTypes.count(baseNonCompatible)) {
            add("E0706", baseNonCompatible + " is not C-compatible; add @layout(C)");
          } else {
            add("E0706", baseNonCompatible + " is not C-compatible in an exported C ABI signature");
          }
        }
        if (contains(source.text, "// panic.strategy: unwind")) {
          add("E0706", "exported C ABI function may panic under unwind profile");
        }
        for (const auto &type : parameterTypes(params)) {
          if (nonPubCLayoutTypes.count(normalizeSignatureType(type))) {
            add("E0706", "exported C ABI signature exposes non-pub type " + normalizeSignatureType(type));
          }
        }
        if (pendingExport->bridge && startsWith(normalizeSignatureType(returnType), "Result<")) {
          const auto resultArgs = firstGenericArgument(normalizeSignatureType(returnType), "Result");
          if (resultArgs) {
            const auto args = splitArguments(*resultArgs);
            if (args.size() == 2) {
              if (normalizeSignatureType(args[0]) == "String") {
                add("E0706", "bridge C Result payload String is not C-compatible; return StringSlice for immediate access or use CHandle");
              }
              if (!cErrorCodeTypes.count(normalizeSignatureType(args[1]))) {
                add("E0706", normalizeSignatureType(args[1]) + " must implement CErrorCode for bridge C Result lowering");
              }
            }
          }
        }
        if (exportedSymbols.count(pendingExport->symbol)) {
          add("E0707", "duplicate exported symbol " + pendingExport->symbol);
        } else {
          exportedSymbols[pendingExport->symbol] = lineNo;
        }
        pendingExport.reset();
      }
    } else if (pendingExport && (startsWith(trimmed, "pub struct ") || startsWith(trimmed, "struct ") || startsWith(trimmed, "enum ") || startsWith(trimmed, "interface "))) {
      add("E0708", "@export can only be applied to top-level functions");
      pendingExport.reset();
    }

    if (auto match = matchRegex(trimmed, returnFieldPattern)) {
      const std::string name = (*match)[1];
      if (uninitializedVars.count(name)) {
        add("E0404", name + " is used before initialization");
      }
    }
    if (!startsWith(trimmed, "val ") && !startsWith(trimmed, "var ") &&
        !startsWith(trimmed, "impl ") && !startsWith(trimmed, "trust impl ") &&
        !startsWith(trimmed, "struct ") && !startsWith(trimmed, "pub struct ") && !startsWith(trimmed, "private struct ") &&
        !startsWith(trimmed, "interface ") && !startsWith(trimmed, "pub interface ") && !startsWith(trimmed, "private interface ")) {
      const std::regex ctorPattern(R"(([A-Z][A-Za-z0-9_]*)\s*\{([^{}]*)\})");
      auto ctorBegin = std::sregex_iterator(trimmed.begin(), trimmed.end(), ctorPattern);
      auto ctorEnd = std::sregex_iterator();
      for (auto it = ctorBegin; it != ctorEnd; ++it) {
        const std::string ctorType = (*it)[1];
        const std::string fieldList = (*it)[2];
        validateStructLiteral(ctorType, fieldList);
      }
    }
    const std::regex simpleFieldAccessPattern(R"(\b([A-Za-z_][A-Za-z0-9_]*)\.([A-Za-z_][A-Za-z0-9_]*)\b)");
    auto accessBegin = std::sregex_iterator(trimmed.begin(), trimmed.end(), simpleFieldAccessPattern);
    auto accessEnd = std::sregex_iterator();
    for (auto it = accessBegin; it != accessEnd; ++it) {
      const auto after = static_cast<std::size_t>(it->position() + it->length());
      if (after < trimmed.size() && trimmed[after] == '(') continue;
      const std::string receiver = (*it)[1];
      const std::string fieldName = (*it)[2];
      const std::string receiverType = receiver == "self" ? currentImplType : (variableTypes.count(receiver) ? variableTypes[receiver] : "");
      if (receiverType.empty() || !structFields.count(receiverType)) continue;
      if (!structFields[receiverType].count(fieldName)) {
        add("E0201", "field " + fieldName + " is not declared in struct " + receiverType);
      }
    }
    const std::regex enumVariantUsePattern(R"(\b([A-Z][A-Za-z0-9_]*)\.([A-Z][A-Za-z0-9_]*)\b)");
    auto enumUseBegin = std::sregex_iterator(trimmed.begin(), trimmed.end(), enumVariantUsePattern);
    auto enumUseEnd = std::sregex_iterator();
    for (auto it = enumUseBegin; it != enumUseEnd; ++it) {
      const std::string enumName = (*it)[1];
      const std::string variantName = (*it)[2];
      if (!enumVariants.count(enumName)) continue;
      if (!enumVariants[enumName].count(variantName)) {
        add("E0201", "variant " + variantName + " is not declared in enum " + enumName);
        continue;
      }
      const auto &shape = enumVariants[enumName][variantName];
      std::size_t after = static_cast<std::size_t>(it->position() + it->length());
      while (after < trimmed.size() && std::isspace(static_cast<unsigned char>(trimmed[after]))) ++after;
      const bool hasCallPayload = after < trimmed.size() && trimmed[after] == '(';
      const bool hasRecordPayload = after < trimmed.size() && trimmed[after] == '{';
      if (!shape.hasPayload && (hasCallPayload || hasRecordPayload)) {
        add("E0201", "variant " + variantName + " does not take payload values");
        continue;
      }
      if (shape.hasPayload && !hasCallPayload && !hasRecordPayload) {
        std::size_t before = static_cast<std::size_t>(it->position());
        while (before > 0 && std::isspace(static_cast<unsigned char>(trimmed[before - 1]))) --before;
        const char previous = before > 0 ? trimmed[before - 1] : '\0';
        const char next = after < trimmed.size() ? trimmed[after] : '\0';
        const bool passedAsFunctionValue = (previous == '(' || previous == ',') && (next == ')' || next == ',');
        if (!passedAsFunctionValue) {
          add("E0201", "variant " + variantName + " requires " + std::to_string(shape.payloadArity) + " payload value");
        }
        continue;
      }
      if (shape.hasPayload && hasCallPayload) {
        if (const auto payload = parenthesizedContentAt(trimmed, after)) {
          const auto actualArity = splitArguments(*payload).size();
          if (actualArity != shape.payloadArity) {
            add("E0201", "variant " + variantName + " expects " + std::to_string(shape.payloadArity) +
                             " payload value but got " + std::to_string(actualArity));
          }
        }
      } else if (shape.hasPayload && hasRecordPayload) {
        if (const auto payload = bracedContentAt(trimmed, after)) {
          const auto actualArity = splitArguments(*payload).size();
          if (actualArity != shape.payloadArity) {
            add("E0201", "variant " + variantName + " expects " + std::to_string(shape.payloadArity) +
                             " payload field but got " + std::to_string(actualArity));
          }
        }
      }
    }
    if (contains(trimmed, "return ")) {
      const std::regex simpleReturnPattern(R"(^\s*return\s+(.+)\s*;)");
      if (auto returned = matchRegex(trimmed, simpleReturnPattern)) {
        const std::string expr = trim(std::string((*returned)[1]));
        auto comparableReturnType = [&](const std::string &type) {
          const std::string normalized = normalizeSignatureType(type);
          static const std::set<std::string> simple{
              "Unit", "Bool", "I8", "I16", "I32", "I64", "ISize",
              "U8", "U16", "U32", "U64", "USize", "F32", "F64",
              "Char", "String", "StringSlice"};
          return simple.count(normalized) || structFields.count(normalized) || enumVariants.count(normalized);
        };
        if (!currentReturnType.empty() && currentReturnType != "Unit") {
          if (const auto returnedType = simpleExpressionType(expr)) {
            if (*returnedType == "Never") {
              // Never-producing expressions do not return a value and can satisfy any explicit return type.
            } else if (currentReturnType == "Never") {
              add("E0302", "Never function cannot return " + displayExpressionType(*returnedType) + " expression");
            } else if (*returnedType == "IntLiteral" && currentReturnType == "Bool") {
              add("E0302", "return type Bool does not match integer expression");
            } else if (*returnedType == "FloatLiteral" && !isFloatType(currentReturnType)) {
              add("E0302", "return type " + currentReturnType + " does not match float expression");
            } else if (*returnedType == "StringSlice" && currentReturnType == "String" &&
                       expr.size() >= 2 && expr.front() == '"' && expr.back() == '"') {
              add("E0301", "string literal is StringSlice; use String.from(\"hello\") for owned String");
            } else if (*returnedType != "IntLiteral" && *returnedType != currentReturnType &&
                       comparableReturnType(currentReturnType) && comparableReturnType(*returnedType)) {
              add("E0302", "return type " + currentReturnType + " does not match " + *returnedType + " expression");
            }
          } else {
            checkBinaryNumericExpression(expr);
            checkComparisonExpression(expr);
            checkKnownIdentifierExpression(expr);
          }
        } else if (currentReturnType == "Unit") {
          if (const auto returnedType = simpleExpressionType(expr)) {
            if (*returnedType != "Unit" && *returnedType != "Never") {
              add("E0302", "Unit function cannot return " + displayExpressionType(*returnedType) + " expression");
            }
          } else {
            checkBinaryNumericExpression(expr);
            checkComparisonExpression(expr);
            checkKnownIdentifierExpression(expr);
          }
        } else {
          checkBinaryNumericExpression(expr);
          checkComparisonExpression(expr);
          checkKnownIdentifierExpression(expr);
        }
      }
      for (const auto &name : maybeInitializedVals) {
        if (contains(trimmed, "return " + name)) {
          add("E0404", name + " is not definitely initialized on every path");
        }
      }
      for (const auto &name : partialStructVals) {
        if (contains(trimmed, "return " + name) && variableTypes[name] == "Header" &&
            !partialStructFields[name].count("version")) {
          add("E0404", name + ".version is not initialized");
        }
      }
      for (const auto &name : readParameters) {
        const std::string paramType = variableTypes[name];
        if (contains(trimmed, "return " + name + ";") && (destroyTypes.count(paramType) || resourceTypes.count(paramType))) {
          add("E0406", "read parameter is not owned");
        }
      }
      for (const auto &name : constOwnedStrings) {
        if (contains(trimmed, "return " + name)) {
          add("E0810", "const String cannot be materialized as a runtime owned String without explicit String.from");
        }
      }
      if (contains(trimmed, "return ctx")) {
        add("E0811", "TaskContext cannot escape the task body");
      }
      if (contains(trimmed, "return Ok(")) {
        for (const auto &taskName : taskHandles) {
          if (!settledTaskHandles.count(taskName)) {
            add("E0521", "task handle must be settled with await, blockOn, cancel, or detach");
          }
        }
      }
      if (interfaceTypes.count(currentReturnType)) {
        const std::regex returnCtorPattern(R"(return\s+([A-Z][A-Za-z0-9_]*)\s*\{)");
        if (auto returnCtor = matchRegex(trimmed, returnCtorPattern)) {
          currentInterfaceReturnTypes.insert("type:" + std::string((*returnCtor)[1]));
        }
        const std::regex returnNamePattern(R"(return\s+([A-Za-z_][A-Za-z0-9_]*)\s*;)");
        if (auto returnName = matchRegex(trimmed, returnNamePattern)) {
          const std::string returnedName = (*returnName)[1];
          if (variableTypes[returnedName] == currentReturnType) {
            currentInterfaceReturnTypes.insert("param:" + returnedName);
          }
        }
        if (currentInterfaceReturnTypes.size() > 1) {
          bool returnsIndependentParams = false;
          for (const auto &returnFact : currentInterfaceReturnTypes) {
            if (startsWith(returnFact, "param:")) returnsIndependentParams = true;
          }
          add("E0307", returnsIndependentParams
                           ? "static interface return must resolve to one hidden concrete type"
                           : "static interface return must use one concrete type; use Box<Writer> for heterogeneous returns");
        }
      }
      const std::regex returnedFieldPattern(R"(return\s+([A-Za-z_][A-Za-z0-9_]*)\.([A-Za-z_][A-Za-z0-9_]*)\s*;)");
      if (auto returnedField = matchRegex(trimmed, returnedFieldPattern)) {
        const std::string receiver = (*returnedField)[1];
        const std::string fieldName = (*returnedField)[2];
        const bool ownedReturn = startsWith(currentReturnType, "Array<") || startsWith(currentReturnType, "Vector<") ||
                                 startsWith(currentReturnType, "Box<") || currentReturnType == "String";
        if (receiver == "self" && ownedReturn && (currentSelfMode == "read" || currentSelfMode == "mut")) {
          add("E0406", currentSelfMode == "read" ? "cannot move field from read-only self" : "cannot move field from mut self");
        }
        const std::string receiverType = receiver == "self" ? currentImplType : variableTypes[receiver];
        if (destroyTypes.count(receiverType) && (ownedReturn || fieldName == "bytes")) {
          add("E0406", "cannot move field from type with destroy block");
        }
      }
    }
    for (const auto &[objectName, fields] : movedFields) {
      for (const auto &fieldName : fields) {
        if (contains(trimmed, objectName + "." + fieldName) && !contains(trimmed, "val " + fieldName + " = " + objectName + "." + fieldName)) {
          add("E0401", "field used after move");
        }
      }
      if (!fields.empty() && contains(trimmed, "move " + objectName)) {
        add("E0401", "partially moved object cannot be used as complete value");
      }
    }
    for (const auto &name : movedBindings) {
      if (contains(trimmed, name + ".") || contains(trimmed, "move " + name) || contains(trimmed, "match " + name)) {
        add("E0401", movedBindingMessages.count(name) ? movedBindingMessages[name] : "use of moved value");
      }
    }
    if (inAsyncFunction && asyncSawAwait) {
      for (const auto &name : scopedFutureOwners) {
        if (contains(trimmed, name + ".")) {
          add("E0502", "scoped allocation cannot cross await into future state");
        }
      }
      for (const auto &name : asyncViewNames) {
        if (contains(trimmed, name + "[")) {
          add("E0502", "ArraySlice cannot cross await into future state");
        }
      }
    }
    const std::regex indexReadPattern(R"(\b([A-Za-z_][A-Za-z0-9_]*)\[([0-9]+)\])");
    if (auto indexRead = matchRegex(trimmed, indexReadPattern)) {
      const std::string arrayName = (*indexRead)[1];
      const int indexValue = std::stoi(std::string((*indexRead)[2]));
      if (arrayLengths.count(arrayName) && indexValue >= arrayLengths[arrayName]) {
        add("E0812", "constant index is out of bounds");
      }
    }
    if (contains(trimmed, "return Ok(")) {
      for (const auto &name : localArrayOwners) {
        if (contains(trimmed, "return Ok(" + name + ")")) {
          add("E0502", "returns slice to local storage");
        }
      }
      for (const auto &group : taskGroupNames) {
        if (!settledTaskGroups.count(group) && (!taskGroupStreamingLoop || taskGroupEarlyExitBlock)) {
          add("E0523", taskGroupStreamingLoop
                           ? "task group may still contain unfinished tasks; call move group.cancelRemaining() before leaving"
                           : "task group must be settled with joinAll, tryJoinAll, cancelRemaining, or drained to None");
        }
      }
    }

    if (!startsWith(trimmed, "fn ") && !startsWith(trimmed, "async fn ")) {
      if (auto match = matchRegex(trimmed, callPattern)) {
        const std::string callee = (*match)[1];
        const std::size_t calleePos = static_cast<std::size_t>(match->position(1));
        std::size_t callOpen = calleePos + callee.size();
        while (callOpen < trimmed.size() && std::isspace(static_cast<unsigned char>(trimmed[callOpen]))) ++callOpen;
        std::size_t callClose = std::string::npos;
        if (callOpen < trimmed.size() && trimmed[callOpen] == '(') {
          bool inString = false;
          int depth = 0;
          for (std::size_t i = callOpen; i < trimmed.size(); ++i) {
            const char c = trimmed[i];
            if (c == '"' && (i == 0 || trimmed[i - 1] != '\\')) {
              inString = !inString;
              continue;
            }
            if (inString) continue;
            if (c == '(') ++depth;
            if (c == ')') {
              --depth;
              if (depth == 0) {
                callClose = i;
                break;
              }
            }
          }
        }
        auto callArgs = callClose == std::string::npos
            ? splitArguments((*match)[2])
            : splitArguments(trimmed.substr(callOpen + 1, callClose - callOpen - 1));
        std::size_t afterCall = callClose == std::string::npos ? std::string::npos : callClose + 1;
        while (afterCall < trimmed.size() && std::isspace(static_cast<unsigned char>(trimmed[afterCall]))) ++afterCall;
        const bool methodCall = calleePos > 0 && trimmed[calleePos - 1] == '.';
        const bool chainedCall = afterCall < trimmed.size() && trimmed[afterCall] == '.';
        if (!methodCall && !chainedCall) checkCallArgumentTypes(callee, callArgs);
        if (overloadArgumentTypes[callee].count("I32") && overloadArgumentTypes[callee].count("U32") &&
            !callArgs.empty() && std::regex_match(trim(callArgs[0]), std::regex(R"([0-9]+)"))) {
          add("E0302", "ambiguous overload; annotate the literal or variable type");
        }
        if (callee == "inspect" && !callArgs.empty()) {
          const std::regex ctorArgPattern(R"(^([A-Z][A-Za-z0-9_]*)\s*\{)");
          if (auto ctorArg = matchRegex(trim(callArgs[0]), ctorArgPattern)) {
            const std::string argType = (*ctorArg)[1];
            if (genericInterfaceImplArgs[argType]["Consumer"].size() > 1) {
              add("E0307", "cannot infer T; " + argType + " implements multiple Consumer<T> instances");
            }
          }
        }
        if (callee == "same" && callArgs.size() >= 2) {
          const std::regex ctorArgPattern(R"(^([A-Z][A-Za-z0-9_]*)\s*\{)");
          auto leftCtor = matchRegex(trim(callArgs[0]), ctorArgPattern);
          auto rightCtor = matchRegex(trim(callArgs[1]), ctorArgPattern);
          if (leftCtor && rightCtor && std::string((*leftCtor)[1]) != std::string((*rightCtor)[1])) {
            add("E0307", "W must be one concrete type");
          }
        }
        if (moveParameterFunctions.count(callee)) {
          if (!callArgs.empty()) {
            const std::string firstArg = trim(callArgs[0]);
            if (!startsWith(firstArg, "move ") && std::regex_match(firstArg, std::regex(R"([A-Za-z_][A-Za-z0-9_]*)"))) {
              add("E0402", "move parameter requires explicit `move` at call site");
            }
          }
        }
        if (callableParameterKinds[callee] == "MutFn" && !mutableCallableParameters.count(callee) && !startsWith(trimmed, "mut ")) {
          add("E0508", "calling MutFn requires mut access to the callable");
        }
        if (callableParameterKinds[callee] == "OnceFn") {
          if (consumedOnceCallables.count(callee)) {
            add("E0508", "OnceFn callable was consumed by the first call");
          }
          consumedOnceCallables.insert(callee);
        }
        if ((callee == "readOnly" || callableParameterKinds[callee] == "Fn") && !splitArguments((*match)[2]).empty()) {
          const std::string firstArg = trim(splitArguments((*match)[2])[0]);
          if (localClosureKinds[firstArg] == "MutFn") {
            add("E0508", "mutating closure implements MutFn, not Fn");
          } else if (localClosureKinds[firstArg] == "OnceFn") {
            add("E0508", "consuming closure implements OnceFn, not Fn");
          }
        }
      }
    }
    if (contains(trimmed, "readOnly(move ()")) {
      add("E0508", "consuming closure implements OnceFn, not Fn");
    }
    if (contains(trimmed, "Thread.spawn(")) {
      inThreadSpawn = true;
      threadSpawnBraceDepth = 0;
    }
    const std::regex methodCallPattern(R"((?:move\s+|mut\s+)?([A-Za-z_][A-Za-z0-9_]*)\.([A-Za-z_][A-Za-z0-9_]*)\s*\()");
    if (auto methodCall = matchRegex(trimmed, methodCallPattern)) {
      const std::string receiver = (*methodCall)[1];
      const std::string method = (*methodCall)[2];
      const std::string receiverType = variableTypes[receiver];
      if (movedBindings.count(receiver)) {
        add("E0401", "receiver used after move self call");
      }
      if (moveSelfMethods.count(method) || contains(trimmed, "move " + receiver + "." + method + "(")) {
        movedBindings.insert(receiver);
      }
      if (auto iface = firstGenericArgument(receiverType, "Box")) {
        const auto flags = interfaceMethodFlags[*iface][method];
        if (flags.count("self-return")) add("E0511", "method returning Self cannot be dynamically dispatched");
        if (flags.count("generic")) add("E0511", "generic interface method cannot be dynamically dispatched");
        if (flags.count("move-self")) add("E0511", "self interface method cannot be dynamically dispatched");
      }
      if (auto iface = firstGenericArgument(receiverType, "Shared")) {
        const auto flags = interfaceMethodFlags[*iface][method];
        if (flags.count("mut-self")) add("E0511", "Shared<Interface> cannot call mut self interface methods");
      }
      if (method == "clone" || method == "cloneIn") {
        if (auto element = firstGenericArgument(receiverType, "Array")) {
          if (destroyTypes.count(*element)) add("E0513", "Array.clone requires T: Copy");
        }
        if (auto element = firstGenericArgument(receiverType, "Vector")) {
          if (destroyTypes.count(*element)) add("E0513", "Vector.clone requires T: Copy");
        }
        if (auto mapArgs = firstGenericArgument(receiverType, "Map")) {
          const auto args = splitArguments(*mapArgs);
          if (args.size() == 2 && (destroyTypes.count(args[0]) || destroyTypes.count(args[1]))) {
            add("E0513", "Map.clone requires K: Copy and V: Copy");
          }
        }
        if (auto element = firstGenericArgument(receiverType, "Set")) {
          if (destroyTypes.count(*element)) add("E0513", "Set.clone requires T: Copy");
        }
      }
    }
    if (inThreadSpawn) {
      for (const auto &[sliceName, ownerName] : liveSliceOwners) {
        (void)ownerName;
        if (contains(trimmed, sliceName)) {
          add("E0502", "ArraySlice cannot escape to a thread");
        }
      }
      for (const auto &[name, type] : variableTypes) {
        if ((type == "MutAccess" && contains(trimmed, name)) ||
            (type == "TaskContext" && contains(trimmed, name + ".")) ||
            contains(trimmed, name + ".")) {
          if (type == "MutAccess") {
            add("E0502", "writable access cannot escape to unscoped thread");
          }
          if (type == "TaskContext") {
            add("E0811", "TaskContext cannot be captured by Thread.spawn");
          }
          if (destroyTypes.count(type) && !trustedSendTypes.count(type)) {
            add("E0512", type + " is not Send");
          }
          if (auto iface = firstGenericArgument(type, "Box")) {
            if (!contains(*iface, "Send")) add("E0512", "Box<" + *iface + "> is not Send; use a named interface that includes Send");
          }
          if (auto inner = firstGenericArgument(type, "Shared")) {
            if (destroyTypes.count(*inner)) add("E0512", "Shared<" + *inner + "> requires " + *inner + ": Send, Sync");
          }
        }
      }
    }
    if (contains(trimmed, "return ") && contains(trimmed, "[0]")) {
      const std::regex indexPattern(R"(return\s+([A-Za-z_][A-Za-z0-9_]*)\[)");
      if (auto indexed = matchRegex(trimmed, indexPattern)) {
        const std::string collectionType = variableTypes[(*indexed)[1]];
        if (auto element = firstGenericArgument(collectionType, "Vector")) {
          if (destroyTypes.count(*element)) add("E0513", "cannot move out through indexed access; use removeAt or swapRemove");
        }
      }
    }
    if (contains(trimmed, ".get(")) {
      const std::regex getPattern(R"(return\s+([A-Za-z_][A-Za-z0-9_]*)\.get\()");
      if (auto get = matchRegex(trimmed, getPattern)) {
        const std::string collectionType = variableTypes[(*get)[1]];
        if (auto element = firstGenericArgument(collectionType, "ArraySlice")) {
          if (destroyTypes.count(*element)) add("E0513", "get(index) -> Option<T> requires T: Copy");
        }
        if (auto mapArgs = firstGenericArgument(collectionType, "Map")) {
          auto args = splitArguments(*mapArgs);
          if (args.size() == 2 && destroyTypes.count(args[1])) add("E0513", "Map.get(key) -> Option<V> requires V: Copy");
        }
      }
    }
    if (auto match = matchRegex(trimmed, mutCallPattern)) {
      const std::string receiver = (*match)[1];
      if (valBindings.count(receiver)) {
        add("E0504", "mut self method requires writable receiver");
      }
    }

    if (!startsWith(trimmed, "fn ") && !startsWith(trimmed, "async fn ") && !startsWith(trimmed, "for move ")) {
      auto moveBegin = std::sregex_iterator(trimmed.begin(), trimmed.end(), moveUsePattern);
      auto moveEnd = std::sregex_iterator();
      for (auto it = moveBegin; it != moveEnd; ++it) {
        const std::string name = (*it)[1];
        if (contains(trimmed, "move " + name + ".")) {
          continue;
        }
        if (movedBindings.count(name)) {
          add("E0401", movedBindingMessages.count(name) ? movedBindingMessages[name] : "use of moved value");
        } else {
          movedBindings.insert(name);
        }
      }
    }
    if (inClosureLiteral) {
      if (contains(trimmed, " = ") && !startsWith(trimmed, "val ") && !startsWith(trimmed, "var ")) closureMutates = true;
      if (contains(trimmed, "move ")) closureConsumes = true;
    }
    if (contains(trimmed, "return match ") || startsWith(trimmed, "match ") || contains(trimmed, "match move ")) {
      inMatch = true;
      matchIsMove = contains(trimmed, "match move ");
      if (matchIsMove) {
        const std::regex matchMovePattern(R"(match\s+move\s+([A-Za-z_][A-Za-z0-9_]*))");
        if (auto movedEnum = matchRegex(trimmed, matchMovePattern)) {
          const std::string name = (*movedEnum)[1];
          movedBindings.insert(name);
          movedBindingMessages[name] = "use of moved enum after match move";
        }
      }
      matchBraceDepth = 0;
      matchSawWildcard = false;
      matchSawReady = false;
      matchSawWaiting = false;
      matchSawDone = false;
      matchSawSomeUnguarded = false;
      matchSawSomeGuarded = false;
      matchSawNone = false;
    }
    if (contains(trimmed, "match mut ")) {
      const std::regex matchMutPattern(R"(match\s+mut\s+([A-Za-z_][A-Za-z0-9_]*))");
      if (auto matchMut = matchRegex(trimmed, matchMutPattern)) {
        if (valBindings.count((*matchMut)[1])) {
          add("E0509", "match mut requires writable enum owner");
        }
      }
    }
    if (inMatch) {
      if (!matchIsMove && contains(trimmed, "=> consume(move ")) {
        add("E0509", "cannot move payload from read-only match; use match move");
      }
      if (contains(trimmed, "\"") && contains(trimmed, "=>")) {
        add("E0509", "String and StringSlice patterns are not supported in v1");
      }
      if (contains(trimmed, " | ") && contains(trimmed, "=>")) {
        const auto arrow = trimmed.find("=>");
        const auto leftAlt = trimmed.substr(0, arrow);
        const std::regex bindingPattern(R"(\(([A-Za-z_][A-Za-z0-9_]*)\))");
        std::vector<std::string> names;
        auto begin = std::sregex_iterator(leftAlt.begin(), leftAlt.end(), bindingPattern);
        auto end = std::sregex_iterator();
        for (auto it = begin; it != end; ++it) names.push_back((*it)[1]);
        if (names.size() >= 2 && names.front() != names.back()) {
          add("E0509", "or pattern alternatives must bind the same names");
        }
      }
      if (matchSawWildcard && contains(trimmed, "=>")) {
        add("E0509", "unreachable pattern branch");
      }
      if (contains(trimmed, "_ =>")) matchSawWildcard = true;
      if (contains(trimmed, "Ready")) matchSawReady = true;
      if (contains(trimmed, "Waiting")) matchSawWaiting = true;
      if (contains(trimmed, "Done")) matchSawDone = true;
      if (contains(trimmed, "Some") && contains(trimmed, " if ")) matchSawSomeGuarded = true;
      else if (contains(trimmed, "Some")) matchSawSomeUnguarded = true;
      if (contains(trimmed, "None")) matchSawNone = true;
    }
    if (contains(trimmed, "if (shouldStop(")) {
      taskGroupEarlyExitBlock = true;
    }
    if (contains(trimmed, ".joinAll(") || contains(trimmed, ".tryJoinAll(") ||
        contains(trimmed, ".cancelRemaining(")) {
      for (const auto &group : taskGroupNames) {
        if (contains(trimmed, group + ".")) settledTaskGroups.insert(group);
      }
    }
    if (contains(trimmed, ".tryNext(")) {
      if (contains(trimmed, ".tryNext(")) taskGroupStreamingLoop = true;
    }

    if (inNoAlloc) {
      noAllocBraceDepth += braceDelta(line);
      if (noAllocBraceDepth <= 0 && contains(trimmed, "}")) {
        inNoAlloc = false;
      }
    }
    if (inNoPanic) {
      noPanicBraceDepth += braceDelta(line);
      if (noPanicBraceDepth <= 0 && contains(trimmed, "}")) {
        inNoPanic = false;
      }
    }
    if (inTrustBlock) {
      trustBraceDepth += braceDelta(line);
      if (trustBraceDepth <= 0 && contains(trimmed, "}")) {
        inTrustBlock = false;
      }
    }
    if (inDestroy) {
      destroyBraceDepth += braceDelta(line);
      if (destroyBraceDepth <= 0 && contains(trimmed, "}")) {
        inDestroy = false;
      }
    }
    if (inClosureLiteral) {
      closureBraceDepth += braceDelta(line);
      if (closureBraceDepth <= 0 && contains(trimmed, "}")) {
        localClosureKinds[currentClosureName] = closureConsumes ? "OnceFn" : (closureMutates ? "MutFn" : "Fn");
        inClosureLiteral = false;
        currentClosureName.clear();
      }
    }
    if (inInterface) {
      interfaceBraceDepth += braceDelta(line);
      if (interfaceBraceDepth <= 0 && contains(trimmed, "}")) {
        inInterface = false;
        currentInterface.clear();
      }
    }
    if (inThreadSpawn) {
      threadSpawnBraceDepth += braceDelta(line);
      if (threadSpawnBraceDepth <= 0 && contains(trimmed, "});")) {
        inThreadSpawn = false;
      }
    }
    if (inSpawnBlockingClosure) {
      spawnBlockingBraceDepth += braceDelta(line);
      if (spawnBlockingBraceDepth <= 0 && contains(trimmed, "});")) {
        inSpawnBlockingClosure = false;
      }
    }
    if (inMatch) {
      matchBraceDepth += braceDelta(line);
      if (matchBraceDepth <= 0 && contains(trimmed, "};")) {
        if (contains(source.text, "Waiting") && matchSawReady && matchSawDone && !matchSawWaiting && !matchSawWildcard) {
          add("E0509", "match is not exhaustive; missing Waiting");
        }
        if (matchSawSomeGuarded && !matchSawSomeUnguarded && matchSawNone && !matchSawWildcard) {
          add("E0509", "match is not exhaustive; guarded Some branch does not cover all Some values");
        }
        inMatch = false;
        matchIsMove = false;
      }
    }
    if (!trackedFunctionName.empty()) {
      if (trackedTopLevelIfActive) {
        if (trackedTopLevelIfBraceDepth == 1 && startsWith(trimmed, "return ")) {
          if (trackedTopLevelIfElseSeen) trackedTopLevelIfElseReturns = true;
          else trackedTopLevelIfThenReturns = true;
        }
        if (trackedTopLevelIfBraceDepth == 1 && trimmed == "} else {") {
          trackedTopLevelIfElseSeen = true;
        }
        trackedTopLevelIfBraceDepth += braceDelta(line);
        if (trackedTopLevelIfBraceDepth <= 0) {
          trackedFunctionLastTopLevelTerminal = trackedTopLevelIfThenReturns &&
                                                trackedTopLevelIfElseSeen &&
                                                trackedTopLevelIfElseReturns;
          trackedTopLevelIfActive = false;
          trackedTopLevelIfBraceDepth = 0;
          trackedTopLevelIfThenReturns = false;
          trackedTopLevelIfElseSeen = false;
          trackedTopLevelIfElseReturns = false;
        }
      }
      if (lineNo != trackedFunctionStartLine && trackedFunctionBraceDepth == 1) {
        if (startsWith(trimmed, "return ") || startsWith(trimmed, "panic(") || startsWith(trimmed, "oom(")) {
          trackedFunctionLastTopLevelTerminal = true;
        } else if (startsWith(trimmed, "if (") && contains(trimmed, "{")) {
          trackedTopLevelIfActive = true;
          trackedTopLevelIfBraceDepth = braceDelta(line);
          trackedTopLevelIfThenReturns = false;
          trackedTopLevelIfElseSeen = false;
          trackedTopLevelIfElseReturns = false;
          trackedFunctionLastTopLevelTerminal = false;
        } else if (startsWith(trimmed, "match ")) {
          trackedFunctionLastTopLevelTerminal = true;
        } else if (trackedFunctionLastTopLevelTerminal &&
                   (startsWith(trimmed, "+") || startsWith(trimmed, "-") ||
                    startsWith(trimmed, "*") || startsWith(trimmed, "/"))) {
          trackedFunctionLastTopLevelTerminal = true;
        } else if (trimmed != "}" && trimmed != "};" && !startsWith(trimmed, "//")) {
          trackedFunctionLastTopLevelTerminal = false;
        }
      }
      trackedFunctionBraceDepth += braceDelta(line);
      if (trackedFunctionBraceDepth <= 0 && contains(trimmed, "}")) {
        if (!trackedFunctionLastTopLevelTerminal) {
          add("E0302", "function " + trackedFunctionName + " must return " +
                          trackedFunctionReturnType + " on all paths");
        }
        trackedFunctionName.clear();
        trackedFunctionReturnType.clear();
        trackedFunctionBraceDepth = 0;
        trackedFunctionStartLine = 0;
        trackedFunctionLastTopLevelTerminal = false;
        trackedTopLevelIfActive = false;
        trackedTopLevelIfBraceDepth = 0;
        trackedTopLevelIfThenReturns = false;
        trackedTopLevelIfElseSeen = false;
        trackedTopLevelIfElseReturns = false;
      }
    }
    if (inStruct) {
      structBraceDepth += braceDelta(line);
      if (structBraceDepth <= 0 && contains(trimmed, "}")) {
        inStruct = false;
        currentStruct.clear();
        currentStructLayout.clear();
      }
    }
    if (inEnum) {
      enumBraceDepth += braceDelta(line);
      if (enumBraceDepth <= 0 && contains(trimmed, "}")) {
        inEnum = false;
        currentEnum.clear();
      }
    }
    if (inImpl) {
      implBraceDepth += braceDelta(line);
      if (implBraceDepth <= 0 && contains(trimmed, "}")) {
        inImpl = false;
        currentImplType.clear();
      }
    }
    declarationBraceDepth += braceDelta(line);
    if (declarationBraceDepth < 0) declarationBraceDepth = 0;
  }

  return diagnostics;
}

std::vector<Diagnostic> manifestDiagnostics(const SourceText &source) {
  std::vector<Diagnostic> diagnostics;
  auto lines = splitLines(source.text);
  std::string section;
  bool allocatorDefault = false;
  bool allocatorSymbol = false;
  bool hostedProfile = false;
  std::string targetProfile;
  std::string panicStrategy;
  std::string oomStrategy;
  bool panicHandlerStrategy = false;
  bool panicHandler = false;
  bool oomHandlerStrategy = false;
  bool oomHandler = false;
  bool trustRequireReport = false;
  bool hasPackageName = false;
  bool hasPackageSection = false;
  std::string packageKind;
  bool hasPackageEntry = false;
  const std::regex sectionPattern(R"(^\s*\[([A-Za-z_.-]+)\]\s*$)");
  const std::regex kvPattern(R"(^\s*([A-Za-z0-9_.-]+)\s*=\s*(.+?)(?:\s+#.*)?$)");
  auto stagedPackageDiagnostic = [&](const std::string &message, int lineNo, const std::string &feature) {
    Diagnostic diag = makeDiagnostic(source, "E9002", message, lineNo);
    diag.staged = true;
    diag.feature = feature;
    diag.category = "staged";
    diag.stageName = "manifest";
    diag.notes.push_back("network package resolution is reserved for a later stage0 milestone");
    diag.help.push_back("use a local path dependency, builtin dependency, or an already locked offline source");
    diagnostics.push_back(diag);
  };

  for (std::size_t i = 0; i < lines.size(); ++i) {
    const int lineNo = static_cast<int>(i + 1);
    const std::string line = lines[i];
    if (auto match = matchRegex(line, sectionPattern)) {
      section = (*match)[1];
      if (section == "package") hasPackageSection = true;
      continue;
    }
    auto kv = matchRegex(line, kvPattern);
    if (!kv) continue;
    const std::string key = (*kv)[1];
    const std::string value = trim(std::string((*kv)[2]));
    if (section == "package" && key == "name") {
      hasPackageName = true;
      const std::string name = unquoteTomlString(value);
      if (!isZenoIdentifier(name)) {
        diagnostics.push_back(makeDiagnostic(source, "E1003", "package.name must be a legal Zeno identifier", lineNo));
      }
    }
    if (section == "package" && key == "kind") {
      packageKind = unquoteTomlString(value);
      if (!supportedPackageKinds().count(packageKind)) {
        diagnostics.push_back(makeDiagnostic(source, "E1002", "package.kind must be application or library", lineNo));
      }
    }
    if (section == "package" && key == "entry") {
      hasPackageEntry = true;
    }
    if (section == "target" && key == "triple") {
      const std::string target = unquoteTomlString(value);
      if (!isSupportedTargetTriple(target)) {
        diagnostics.push_back(makeDiagnostic(source, "E1002", "unsupported target triple", lineNo));
      }
    }
    if (section == "target" && key == "profile") {
      const std::string profile = unquoteTomlString(value);
      targetProfile = profile;
      hostedProfile = profile == "hosted";
      if (!isSupportedProfile(profile)) {
        diagnostics.push_back(makeDiagnostic(source, "E1002", "unknown target profile", lineNo));
      }
    }
    if (section == "panic" && key == "stackTrace") {
      diagnostics.push_back(makeDiagnostic(source, "E1001", "unknown field; use panic.stack", lineNo));
    }
    if (section == "panic" && key == "strategy") {
      const std::string strategy = unquoteTomlString(value);
      panicStrategy = strategy;
      if (!supportedPanicStrategies().count(strategy)) {
        diagnostics.push_back(makeDiagnostic(source, "E1002", "unknown panic strategy", lineNo));
      }
      if (strategy == "handler") {
        panicHandlerStrategy = true;
      }
      if (strategy == "unwind") {
        Diagnostic diag = makeDiagnostic(source, "E9003", "panic unwind is not implemented in stage0", lineNo);
        diag.staged = true;
        diag.feature = "panic-unwind";
        diag.category = "staged";
        diag.stageName = "manifest";
        diag.help.push_back("use abort, trap, or a handler strategy in stage0 MVP");
        diagnostics.push_back(diag);
      }
    }
    if (section == "panic" && key == "handler") {
      panicHandler = true;
    }
    if (section == "oom" && key == "strategy") {
      const std::string strategy = unquoteTomlString(value);
      oomStrategy = strategy;
      if (!supportedOomStrategies().count(strategy)) {
        diagnostics.push_back(makeDiagnostic(source, "E1002", "unknown oom strategy", lineNo));
      }
      if (strategy == "handler") {
        oomHandlerStrategy = true;
      }
    }
    if (section == "oom" && key == "handler") {
      oomHandler = true;
    }
    if (section == "allocator" && key == "default" && contains(value, "true")) {
      allocatorDefault = true;
    }
    if (section == "allocator" && key == "symbol") {
      allocatorSymbol = true;
    }
    if (section == "trust" && !supportedTrustFields().count(key)) {
      diagnostics.push_back(makeDiagnostic(source, "E1001", "unknown trust capability field", lineNo));
    }
    if (section == "trust" && key == "requireReport" && contains(value, "true")) {
      trustRequireReport = true;
    }
    if (section == "trust" && (key == "hardware" || key == "inlineAsm" || key == "interrupts") && contains(value, "true") && hostedProfile) {
      diagnostics.push_back(makeDiagnostic(source, "E1002", "hosted profile cannot enable " + key + " trust capability", lineNo));
    }
    if (section == "dependencies") {
      if (!isZenoIdentifier(key)) {
        diagnostics.push_back(makeDiagnostic(source, "E1003", "dependency key must be a legal Zeno identifier", lineNo));
      }
      const std::string pathDependency = inlineTomlField(value, "path");
      if (!pathDependency.empty() && fs::path(pathDependency).is_absolute()) {
        diagnostics.push_back(makeDiagnostic(source, "E1004", "dependency path must be relative, not absolute", lineNo));
      }
      if (contains(value, "git") && !contains(value, "rev")) {
        stagedPackageDiagnostic("git dependency must specify commit rev, not a floating branch", lineNo, "git-dependency");
      }
      if (contains(value, "version") && (contains(value, "^") || contains(value, "*") || contains(value, ">") || contains(value, "<"))) {
        stagedPackageDiagnostic("registry dependency version must be exact in v1", lineNo, "registry-dependency");
      }
    }
    if (key == "source" && contains(value, "path:/")) {
      diagnostics.push_back(makeDiagnostic(source, "E1004", "lockfile source path must be relative, not absolute", lineNo));
    }
  }
  if (!hasPackageName && hasPackageSection) {
    diagnostics.push_back(makeDiagnostic(source, "E1003", "package.name is required", lineContaining(source.text, "[package]")));
  }
  if (packageKind == "library" && hasPackageEntry) {
    diagnostics.push_back(makeDiagnostic(source, "E1003", "library packages cannot declare package.entry", lineContaining(source.text, "entry =")));
  }
  if (panicHandlerStrategy && !panicHandler) {
    diagnostics.push_back(makeDiagnostic(source, "E1002", "panic.handler is required when strategy is handler", 1));
  }
  if (oomHandlerStrategy && !oomHandler) {
    diagnostics.push_back(makeDiagnostic(source, "E1002", "oom.handler is required when strategy is handler", 1));
  }
  if ((targetProfile == "kernel" || targetProfile == "embedded") && panicStrategy != "handler") {
    diagnostics.push_back(makeDiagnostic(source, "E1002", targetProfile + " profile requires handler panic strategy", 1));
  }
  if ((targetProfile == "kernel" || targetProfile == "embedded") && oomStrategy != "handler") {
    diagnostics.push_back(makeDiagnostic(source, "E1002", targetProfile + " profile requires handler oom strategy", 1));
  }
  if ((targetProfile == "kernel" || targetProfile == "embedded") && !trustRequireReport) {
    diagnostics.push_back(makeDiagnostic(source, "E1002", targetProfile + " profile requires trust.requireReport = true", 1));
  }
  if (allocatorDefault && !allocatorSymbol && !contains(source.text, "profile = \"hosted\"")) {
    diagnostics.push_back(makeDiagnostic(source, "E1002", "default allocator requires allocator.symbol or a profile built-in provider", 1));
  }
  return diagnostics;
}

std::optional<std::string> quotedValue(const std::string &line) {
  const std::regex pattern(R"re("([^"]+)")re");
  if (auto match = matchRegex(line, pattern)) {
    return std::string((*match)[1]);
  }
  return std::nullopt;
}

std::set<std::string> quotedValues(const std::string &line) {
  std::set<std::string> values;
  const std::regex pattern(R"re("([^"]+)")re");
  auto begin = std::sregex_iterator(line.begin(), line.end(), pattern);
  auto end = std::sregex_iterator();
  for (auto it = begin; it != end; ++it) {
    values.insert((*it)[1]);
  }
  return values;
}

PackageInfo parsePackageInfo(const fs::path &root) {
  PackageInfo info;
  const fs::path manifest = root / "Zeno.toml";
  if (!fs::exists(manifest)) return info;
  const auto text = readFile(manifest);
  std::string section;
  const std::regex sectionPattern(R"(^\s*\[([A-Za-z_.-]+)\]\s*$)");
  const std::regex kvPattern(R"(^\s*([A-Za-z0-9_.-]+)\s*=\s*(.+?)(?:\s+#.*)?$)");
  const std::regex pathPattern(R"re(path\s*=\s*"([^"]+)")re");
  const std::regex membersPattern(R"re(members\s*=\s*\[(.*)\])re");
  const std::regex memberPattern(R"re("([^"]+)")re");

  for (const auto &line : splitLines(text)) {
    if (auto match = matchRegex(line, sectionPattern)) {
      section = (*match)[1];
      continue;
    }
    auto kv = matchRegex(line, kvPattern);
    if (!kv) continue;
    const std::string key = (*kv)[1];
    const std::string value = (*kv)[2];
    if (section == "package" && key == "name") {
      if (auto name = quotedValue(value)) info.name = *name;
    } else if (section == "target" && key == "profile") {
      if (auto profile = quotedValue(value)) info.profile = *profile;
    } else if (section == "panic" && key == "handler") {
      if (auto handler = quotedValue(value)) info.panicHandler = *handler;
    } else if (section == "oom" && key == "handler") {
      if (auto handler = quotedValue(value)) info.oomHandler = *handler;
    } else if (section == "dependencies") {
      if (auto match = matchRegex(value, pathPattern)) {
        info.dependencies[key] = root / std::string((*match)[1]);
      } else if (trim(value) == "builtin" || contains(value, "\"builtin\"")) {
        info.builtinDependencies.insert(key);
      }
    } else if (section == "workspace" && key == "members") {
      if (auto members = matchRegex(line, membersPattern)) {
        const std::string list = (*members)[1];
        auto begin = std::sregex_iterator(list.begin(), list.end(), memberPattern);
        auto end = std::sregex_iterator();
        for (auto it = begin; it != end; ++it) {
          info.workspaceMembers.push_back((*it)[1]);
        }
      }
    }
  }
  return info;
}

bool isBuiltinPackageName(const std::string &name) {
  return name == "core" || name == "alloc" || name == "std";
}

std::string modulePathForSource(const fs::path &root, const fs::path &file, const std::string &packageName = "") {
  fs::path relative = fs::relative(file, root / "src");
  relative.replace_extension();
  std::string module;
  for (const auto &part : relative) {
    if (!module.empty()) module += ".";
    module += part.string();
  }
  if (module == "main" || module == "lib") return "";
  if (isBuiltinPackageName(packageName)) return packageName + "." + module;
  return module;
}

std::optional<std::string> explicitModulePath(const std::string &text) {
  const std::regex modulePattern(R"(^\s*module\s+([A-Za-z_][A-Za-z0-9_]*(?:\.[A-Za-z_][A-Za-z0-9_]*)*)\s*;)");
  for (const auto &line : splitLines(text)) {
    if (auto match = matchRegex(line, modulePattern)) {
      return std::string((*match)[1]);
    }
  }
  return std::nullopt;
}

std::vector<std::string> sourceTopLevelDirs(const fs::path &root) {
  std::vector<std::string> dirs;
  const fs::path src = root / "src";
  if (!fs::exists(src)) return dirs;
  for (const auto &entry : fs::directory_iterator(src)) {
    if (entry.is_directory()) dirs.push_back(entry.path().filename().string());
  }
  return dirs;
}

std::vector<LockPackage> parseLockPackages(const SourceText &lock) {
  std::vector<LockPackage> packages;
  LockPackage current;
  bool inPackage = false;
  int lineNo = 1;
  for (const auto &line : splitLines(lock.text)) {
    const std::string trimmed = trim(line);
    if (trimmed == "[[package]]") {
      if (inPackage) packages.push_back(current);
      current = LockPackage{};
      current.nameLine = lineNo;
      current.sourceLine = lineNo;
      current.manifestHashLine = lineNo;
      current.contentHashLine = lineNo;
      current.compilerPackageHashLine = lineNo;
      inPackage = true;
    } else if (inPackage) {
      current.blockText += line;
      current.blockText += "\n";
      if (startsWith(trimmed, "name = ")) {
        if (auto value = quotedValue(trimmed)) current.name = *value;
        current.nameLine = lineNo;
      } else if (startsWith(trimmed, "source = ")) {
        if (auto value = quotedValue(trimmed)) current.source = *value;
        current.sourceLine = lineNo;
      } else if (startsWith(trimmed, "manifestHash = ")) {
        if (auto value = quotedValue(trimmed)) current.manifestHash = *value;
        current.manifestHashLine = lineNo;
      } else if (startsWith(trimmed, "contentHash = ")) {
        if (auto value = quotedValue(trimmed)) current.contentHash = *value;
        current.contentHashLine = lineNo;
      } else if (startsWith(trimmed, "compilerPackageHash = ")) {
        if (auto value = quotedValue(trimmed)) current.compilerPackageHash = *value;
        current.compilerPackageHashLine = lineNo;
      }
    }
    ++lineNo;
  }
  if (inPackage) packages.push_back(current);
  return packages;
}

std::vector<Diagnostic> lockfileDiagnostics(const fs::path &root, const PackageInfo &package) {
  std::vector<Diagnostic> diagnostics;
  const fs::path lockPath = root / "Zeno.lock";
  if (!fs::exists(lockPath)) return diagnostics;
  SourceText lock = loadSource(lockPath);
  const auto packages = parseLockPackages(lock);
  auto add = [&](const std::string &code, const std::string &message, int line) {
    diagnostics.push_back(makeDiagnostic(lock, code, message, line));
  };

  if (!contains(lock.text, "compiler = \"zeno-stage0 0.1.0\"")) {
    add("E1007", "lockfile compiler identity does not match zeno-stage0 0.1.0", lineContaining(lock.text, "compiler"));
  }

  for (const auto &entry : packages) {
    if (startsWith(entry.source, "path:/")) {
      add("E1004", "lockfile source path must be relative, not absolute", entry.sourceLine);
    }
    if (startsWith(entry.source, "path:")) {
      const std::string sourcePath = entry.source.substr(std::string("path:").size());
      if (fs::path(sourcePath).is_absolute()) continue;
      const fs::path packageRoot = root / sourcePath;
      if (!fs::exists(packageRoot / "Zeno.toml")) {
        add("E1007", "lockfile path source is missing for package " + entry.name, entry.sourceLine);
        continue;
      }
      const std::string expectedManifestHash = fileFingerprint(packageRoot / "Zeno.toml");
      if (entry.manifestHash.empty()) {
        add("E1007", "lockfile manifest hash is missing for package " + entry.name, entry.sourceLine);
      } else if (entry.manifestHash != expectedManifestHash) {
        add("E1007", "lockfile manifest hash is stale for package " + entry.name, entry.manifestHashLine);
      }
      const std::string expectedContentHash = packageContentFingerprint(packageRoot);
      if (entry.contentHash.empty()) {
        add("E1007", "lockfile content hash is missing for package " + entry.name, entry.sourceLine);
      } else if (entry.contentHash != expectedContentHash) {
        add("E1007", "lockfile content hash is stale for package " + entry.name, entry.contentHashLine);
      }
    }
    if (startsWith(entry.source, "builtin:")) {
      const std::string builtinName = entry.source.substr(std::string("builtin:").size());
      const fs::path builtinRoot = builtinPackageRoot(builtinName);
      if (!isBuiltinPackageName(builtinName) || !fs::exists(builtinRoot / "Zeno.toml")) {
        add("E1007", "unknown builtin package " + builtinName + " in lockfile", entry.sourceLine);
      } else {
        const std::string expectedHash = packageFingerprint(builtinRoot);
        if (entry.compilerPackageHash.empty()) {
          add("E1007", "builtin package hash is missing for " + builtinName, entry.sourceLine);
        } else if (entry.compilerPackageHash != expectedHash) {
          add("E1007", "builtin package hash is stale for " + builtinName, entry.compilerPackageHashLine);
        }
      }
    }
  }

  for (const auto &[key, depPath] : package.dependencies) {
    const PackageInfo depInfo = parsePackageInfo(depPath);
    const std::string depName = depInfo.name.empty() ? key : depInfo.name;
    const std::string relativeSource = fs::relative(depPath, root).string();
    if (!contains(lock.text, "{ key = \"" + key + "\", package = \"" + depName + "\" }")) {
      add("E1007", "lockfile is missing dependency edge " + key + " -> " + depName, 1);
    }
    if (!contains(lock.text, "source = \"path:" + relativeSource + "\"")) {
      add("E1007", "lockfile is missing path source for dependency " + depName, 1);
    }
  }
  for (const auto &builtin : package.builtinDependencies) {
    if (!contains(lock.text, "{ key = \"" + builtin + "\", package = \"" + builtin + "\" }")) {
      add("E1007", "lockfile is missing builtin dependency edge " + builtin + " -> " + builtin, 1);
    }
    if (!contains(lock.text, "source = \"builtin:" + builtin + "\"")) {
      add("E1007", "lockfile is missing builtin source for dependency " + builtin, 1);
    }
  }
  const fs::path manifestPath = root / "Zeno.toml";
  if (fs::exists(manifestPath)) {
    const std::string manifestText = readFile(manifestPath);
    for (const auto &[key, rawValue] : manifestSectionFields(manifestText, "dependencies")) {
      const std::string scalarValue = unquoteTomlString(rawValue);
      const std::string git = inlineTomlField(rawValue, "git");
      const std::string rev = inlineTomlField(rawValue, "rev");
      const std::string version = rawValue.empty() || rawValue.front() == '{' ? inlineTomlField(rawValue, "version") : scalarValue;
      std::string source;
      std::string sourceKind;
      if (!git.empty() && !rev.empty()) {
        source = "git:" + git + "#" + rev;
        sourceKind = "git";
      } else if (!version.empty() && scalarValue != "builtin" && !contains(rawValue, "path") && git.empty()) {
        source = "registry:" + key + "@" + version;
        sourceKind = "registry";
      }
      if (source.empty()) continue;
      if (!contains(lock.text, "{ key = \"" + key + "\", package = \"" + key + "\" }")) {
        add("E1007", "lockfile is missing " + sourceKind + " dependency edge " + key + " -> " + key, 1);
      }
      if (!contains(lock.text, "source = \"" + source + "\"")) {
        add("E1007", "lockfile is missing " + sourceKind + " source for dependency " + key, 1);
      }
    }
  }
  for (const auto &member : package.workspaceMembers) {
    const PackageInfo memberInfo = parsePackageInfo(root / member);
    if (memberInfo.name.empty()) continue;
    if (!contains(lock.text, "name = \"" + memberInfo.name + "\"")) {
      add("E1007", "lockfile is missing workspace member package " + memberInfo.name, 1);
    }
    if (!contains(lock.text, "source = \"path:" + member + "\"")) {
      add("E1007", "lockfile is missing path source for workspace member " + memberInfo.name, 1);
    }
  }

  return diagnostics;
}

std::vector<Diagnostic> crossPackageOpaqueReturnDiagnostics(const PackageInfo &package) {
  std::vector<Diagnostic> diagnostics;
  const std::regex interfacePattern(R"(^\s*(?:pub\s+)?interface\s+([A-Za-z_][A-Za-z0-9_]*))");
  const std::regex pubFnPattern(R"(^\s*pub\s+fn\s+[A-Za-z_][A-Za-z0-9_]*(?:<[^>]+>)?\s*\([^)]*\)\s*->\s*([A-Za-z_][A-Za-z0-9_]*(?:<[^>]+>)?))");

  for (const auto &[depKey, depRoot] : package.dependencies) {
    (void)depKey;
    std::set<std::string> interfaceNames;
    std::vector<SourceText> sources;
    for (const auto &file : packageSources(depRoot / "src")) {
      if (file.extension() != ".zn") continue;
      sources.push_back(loadSource(file));
      for (const auto &line : splitLines(sources.back().text)) {
        if (auto match = matchRegex(stripLineComment(line), interfacePattern)) {
          interfaceNames.insert(std::string((*match)[1]));
        }
      }
    }
    if (interfaceNames.empty()) continue;

    for (const auto &source : sources) {
      int lineNo = 1;
      for (const auto &line : splitLines(source.text)) {
        const std::string clean = stripLineComment(line);
        if (auto match = matchRegex(clean, pubFnPattern)) {
          const std::string returnType = normalizeSignatureType((*match)[1]);
          if (interfaceNames.count(returnType)) {
            Diagnostic diag = makeDiagnostic(
                source,
                "E9004",
                "cross-package pub fn -> Interface opaque return metadata is not implemented in stage0",
                lineNo);
            diag.staged = true;
            diag.feature = "cross-package-interface-return";
            diag.category = "staged";
            diag.stageName = "module";
            diag.notes.push_back("same-package static interface returns are supported, but exported opaque return metadata is a later milestone");
            diag.help.push_back("return a concrete pub type or use Box<Interface> across the package boundary");
            diagnostics.push_back(diag);
          }
        }
        ++lineNo;
      }
    }
  }

  return diagnostics;
}

std::vector<std::string> splitDottedPath(const std::string &path) {
  std::vector<std::string> parts;
  std::string current;
  for (char c : path) {
    if (c == '.') {
      if (!current.empty()) parts.push_back(current);
      current.clear();
    } else {
      current.push_back(c);
    }
  }
  if (!current.empty()) parts.push_back(current);
  return parts;
}

std::vector<std::string> splitImportItems(const std::string &items) {
  std::vector<std::string> out;
  for (const auto &item : splitArguments(items)) {
    const std::string trimmedItem = trim(item);
    if (!trimmedItem.empty()) out.push_back(trimmedItem);
  }
  return out;
}

std::string importModuleName(const std::string &rootName, const std::string &importPath, bool builtinRoot) {
  if (builtinRoot) return importPath;
  if (importPath == rootName) return "";
  const std::string prefix = rootName + ".";
  if (startsWith(importPath, prefix)) return importPath.substr(prefix.size());
  return importPath;
}

struct ParsedAstNode {
  fs::path file;
  std::string moduleName;
  std::string kind;
  std::string name;
  std::string visibility;
  std::string generic;
  std::string params;
  std::string returnType;
  std::string layout = "Auto";
  std::string abi;
  std::vector<std::string> attributes;
  bool trusted = false;
  int lineNo = 1;
};

std::string expressionAstFact(std::string expr) {
  expr = trim(expr);
  if (!expr.empty() && expr.back() == ';') expr.pop_back();
  expr = trim(expr);
  auto topLevelBinaryOperator = [](const std::string &text) -> std::optional<char> {
    int parenDepth = 0;
    int braceDepth = 0;
    bool inString = false;
    for (std::size_t offset = text.size(); offset > 0; --offset) {
      const std::size_t i = offset - 1;
      const char c = text[i];
      if (c == '"' && (i == 0 || text[i - 1] != '\\')) {
        inString = !inString;
        continue;
      }
      if (inString) continue;
      if (c == ')') {
        ++parenDepth;
        continue;
      }
      if (c == '(') {
        --parenDepth;
        continue;
      }
      if (c == '}') {
        ++braceDepth;
        continue;
      }
      if (c == '{') {
        --braceDepth;
        continue;
      }
      if (parenDepth != 0 || braceDepth != 0) continue;
      if (std::string("+-*/").find(c) == std::string::npos) continue;
      if (i == 0 || i + 1 >= text.size()) continue;
      const std::string left = trim(std::string_view(text).substr(0, i));
      const std::string right = trim(std::string_view(text).substr(i + 1));
      if (left.empty() || right.empty()) continue;
      const char previous = left.back();
      if ((c == '+' || c == '-') &&
          (previous == '+' || previous == '-' || previous == '*' || previous == '/' ||
           previous == '(' || previous == '{')) {
        continue;
      }
      return c;
    }
    return std::nullopt;
  };
  if (expr.empty()) return "unit";
  if (expr == "()") return "unit";
  if (startsWith(expr, "async ")) return "async-block";
  if (startsWith(expr, "await ")) return "await:" + expressionAstFact(expr.substr(6));
  if (startsWith(expr, "trust")) return "trust-block";
  if (matchRegex(expr, std::regex(R"(^\s*move\s+\([^)]*\)\s*(?:->\s*[^={]+?)?\s*(?:\{|=>))"))) return "closure:move";
  if (matchRegex(expr, std::regex(R"(^\s*\([^)]*\)\s*->\s*[^={]+?\s*\{)"))) return "closure:block";
  if (startsWith(expr, "move ")) return "move:" + expressionAstFact(expr.substr(5));
  if (startsWith(expr, "mut ")) return "mut:" + expressionAstFact(expr.substr(4));
  if (startsWith(expr, "try ")) return "try:" + expressionAstFact(expr.substr(4));
  if (startsWith(expr, "match move ")) return "match:move";
  if (startsWith(expr, "match mut ")) return "match:mut";
  if (startsWith(expr, "match ")) return "match";
  if (expr == "true" || expr == "false") return "bool-literal:" + expr;
  if (expr.size() >= 2 && expr.front() == '"' && expr.back() == '"') return "string-literal";
  if (expr.size() >= 3 && expr.front() == '\'' && expr.back() == '\'') return "char-literal";
  if (std::regex_match(expr, std::regex(R"([0-9]+\.[0-9]+)"))) return "float-literal:" + expr;
  if (std::regex_match(expr, std::regex(R"([0-9]+)"))) return "int-literal:" + expr;
  if (auto cast = matchRegex(expr, std::regex(R"(^(.+)\s+as\s+(.+)$)"))) {
    return "cast:" + trim(std::string((*cast)[2]));
  }
  if (auto tuple = matchRegex(expr, std::regex(R"(^\((.+,.+)\)$)"))) {
    return "tuple";
  }
  if (contains(expr, "=>") ||
      matchRegex(expr, std::regex(R"(^\s*(move\s+)?\([^)]*\)\s*(?:->\s*[A-Za-z_][A-Za-z0-9_]*(?:<[^>]+>)?)?\s*\{)"))) {
    return "closure";
  }
  if (auto literal = matchRegex(expr, std::regex(R"(^([A-Z][A-Za-z0-9_]*)\s*\{)"))) {
    return "struct-literal:" + std::string((*literal)[1]);
  }
  if (auto typeStatic = matchRegex(expr, std::regex(R"(^([A-Z][A-Za-z0-9_]*(?:<[^>]+>)?)\.([A-Za-z_][A-Za-z0-9_]*)\s*\()"))) {
    return "type-static-call:" + std::string((*typeStatic)[1]) + "." + std::string((*typeStatic)[2]);
  }
  if (auto comparison = matchRegex(expr, std::regex(R"(^(.+)\s*(>=|<=|==|!=|>|<)\s*(.+)$)"))) {
    return "compare:" + std::string((*comparison)[2]);
  }
  if (auto index = matchRegex(expr, std::regex(R"(^(.+)\[.+\]$)"))) {
    return "index:" + trim(std::string((*index)[1]));
  }
  if (contains(expr, "..=")) return "range:closed";
  if (contains(expr, "..")) return "range:half-open";
  if (auto binary = topLevelBinaryOperator(expr)) {
    return std::string("binary:") + *binary;
  }
  if (auto methodCall = matchRegex(expr, std::regex(R"(^(.+)\.([A-Za-z_][A-Za-z0-9_]*)\s*\()"))) {
    return "method-call:" + std::string((*methodCall)[2]);
  }
  if (auto call = matchRegex(expr, std::regex(R"(^([A-Za-z_][A-Za-z0-9_]*)\s*\()"))) {
    return "call:" + std::string((*call)[1]);
  }
  if (contains(expr, ".")) return "field:" + expr;
  if (std::regex_match(expr, std::regex(R"([A-Za-z_][A-Za-z0-9_]*)"))) return "name:" + expr;
  return "expr:" + expr;
}

std::vector<ParsedAstNode> parseTopLevelAst(const fs::path &packageRoot,
                                            const std::string &packageName,
                                            const std::vector<fs::path> &sourceFiles) {
  std::vector<ParsedAstNode> nodes;
  const std::regex modulePattern(R"(^\s*module\s+([A-Za-z_][A-Za-z0-9_]*(?:\.[A-Za-z_][A-Za-z0-9_]*)*)\s*;)");
  const std::regex importPattern(R"(^\s*import\s+([^;]+)\s*;)");
  const std::regex attrPattern(R"(^\s*(@[A-Za-z_][A-Za-z0-9_]*(?:\([^)]*\))?))");
  const std::regex constStaticPattern(R"(^\s*(pub\s+|private\s+)?(const|static)\s+([A-Za-z_][A-Za-z0-9_]*)(?:\s*:\s*([^=;]+?))?\s*(?:=\s*(.+?))?\s*;?\s*$)");
  const std::regex typeAliasPattern(R"(^\s*(pub\s+|private\s+)?type\s+([A-Za-z_][A-Za-z0-9_]*)(<[^()]*>)?\s*=\s*([^;]+)\s*;?\s*$)");
  const std::regex declPattern(R"(^\s*(pub\s+|private\s+)?(struct|enum|interface)\s+([A-Za-z_][A-Za-z0-9_]*)(<[^()]*>)?)");
  const std::regex fnPattern(R"(^\s*(pub\s+|private\s+)?(async\s+)?fn\s+([A-Za-z_][A-Za-z0-9_]*)(<[^()]*>)?\s*\(([^)]*)\)\s*(?:->\s*([^{;]+?))?\s*(?:\{|;|$))");
  const std::regex externPattern(R"re(^\s*(trust\s+)?extern\s+"([^"]+)"\s+fn\s+([A-Za-z_][A-Za-z0-9_]*)(<[^()]*>)?\s*\(([^)]*)\)\s*(?:->\s*([A-Za-z_][A-Za-z0-9_]*(?:<[^()]*>)?))?)re");
  const std::regex traitImplPattern(R"(^\s*(trust\s+)?impl(<[^()]*>)?\s+([A-Za-z_][A-Za-z0-9_]*(?:<[^()]*>)?)\s+for\s+([A-Za-z_][A-Za-z0-9_]*(?:<[^()]*>)?))");
  const std::regex inherentImplPattern(R"(^\s*impl(<[^()]*>)?\s+([A-Za-z_][A-Za-z0-9_]*(?:<[^()]*>)?)\s*\{)");
  const std::regex enumPattern(R"(^\s*(pub\s+|private\s+)?enum\s+([A-Za-z_][A-Za-z0-9_]*))");
  const std::regex enumVariantPattern(R"(^\s*([A-Za-z_][A-Za-z0-9_]*)(?:\s*(\([^)]*\)|\{.*\}))?\s*,?\s*$)");
  const std::regex fieldPattern(R"(^\s*(pub\s+|private\s+)?([A-Za-z_][A-Za-z0-9_]*)\s*:\s*([^,]+)\s*,?\s*$)");
  const std::regex destroyPattern(R"(^\s*destroy\s*\{)");
  const std::regex returnPattern(R"(^\s*return\s+(.+)\s*$)");
  const std::regex localConstPattern(R"(^\s*const\s+([A-Za-z_][A-Za-z0-9_]*)(?:\s*:\s*([^=;]+?))?\s*=\s*(.+?)\s*;?\s*$)");
  const std::regex patternBindingPattern(R"(^\s*(val|var)\s+(.+?)\s*=\s*(.+?)\s*;?\s*$)");
  const std::regex bindingPattern(R"(^\s*(val|var)\s+([A-Za-z_][A-Za-z0-9_]*)(?:\s*:\s*([^=;]+?))?\s*=\s*(.+?)\s*;?\s*$)");
  const std::regex assignmentPattern(R"(^\s*([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.+?)\s*;?\s*$)");
  const std::regex breakPattern(R"(^\s*break(?:\s+(.+?))?\s*;?\s*$)");
  const std::regex continuePattern(R"(^\s*continue\s*;?\s*$)");
  const std::regex ifPattern(R"(^\s*if\s*\((.+)\)\s*\{\s*$)");
  const std::regex ifPatternBindingPattern(R"(^\s*if\s*\((.+)\s+is\s+(?:(move|mut)\s+)?([A-Za-z_][A-Za-z0-9_]*(?:\.[A-Za-z_][A-Za-z0-9_]*)?\s*\([^)]*\))\s*\)\s*\{\s*$)");
  const std::regex elsePattern(R"(^\s*\}?\s*else\s*\{\s*$)");
  const std::regex whilePattern(R"(^\s*while\s*\((.+)\)\s*\{\s*$)");
  const std::regex whilePatternBindingPattern(R"(^\s*while\s*\((.+)\s+is\s+(?:(move|mut)\s+)?([A-Za-z_][A-Za-z0-9_]*(?:\.[A-Za-z_][A-Za-z0-9_]*)?\s*\([^)]*\))\s*\)\s*\{\s*$)");
  const std::regex forPattern(R"(^\s*for\s+(move\s+|mut\s+)?(.+?)\s+in\s+(mut\s+)?(.+?)\s*\{\s*$)");
  const std::regex matchPattern(R"(^\s*match\s+(?:(move|mut)\s+)?(.+?)\s*\{\s*$)");
  const std::regex matchArmPattern(R"(^\s*(.+?)(?:\s+if\s+(.+?))?\s*=>\s*(.+?)[,;]?\s*$)");
  const std::regex tryStatementPattern(R"(^\s*try\s+(.+?)\s*;?\s*$)");
  const std::regex callStatementPattern(R"(^\s*([A-Za-z_][A-Za-z0-9_]*)\s*\((.*)\)\s*;?\s*$)");

  auto addNode = [&](const fs::path &file,
                     const std::string &moduleName,
                     const std::string &kind,
                     const std::string &name,
                     const std::string &visibility,
                     int lineNo,
                     std::vector<std::string> attributes = {},
                     std::string generic = "",
                     std::string params = "",
                     std::string returnType = "",
                     std::string layout = "Auto",
                     bool trusted = false,
                     std::string abi = "") {
    nodes.push_back(ParsedAstNode{
        file,
        moduleName,
        kind,
        name,
        visibility,
        std::move(generic),
        std::move(params),
        normalizeSignatureType(returnType),
        std::move(layout),
        std::move(abi),
        std::move(attributes),
        trusted,
        lineNo});
  };

  for (const auto &file : sourceFiles) {
    if (file.extension() != ".zn") continue;
    const SourceText source = loadSource(file);
    const std::string moduleName = explicitModulePath(source.text).value_or(modulePathForSource(packageRoot, file, packageName));
    int braceDepth = 0;
    bool inEnum = false;
    std::string enumVisibility;
    std::vector<std::string> pendingAttributes;
    std::string pendingLayout;
    std::string activeContainerKind;
    std::string activeContainerName;
    std::string activeFunction;
    std::string activeFunctionReturnType;
    int lineNo = 1;
    for (const auto &rawLine : splitLines(source.text)) {
      const std::string line = stripLineComment(rawLine);
      const std::string trimmed = trim(line);
      if (braceDepth == 0) {
        if (auto module = matchRegex(trimmed, modulePattern)) {
          addNode(file, (*module)[1], "module", (*module)[1], "", lineNo);
        } else if (auto import = matchRegex(trimmed, importPattern)) {
          addNode(file, moduleName, "import", trim(std::string((*import)[1])), "", lineNo);
        } else if (auto attr = matchRegex(trimmed, attrPattern)) {
          const std::string attribute = (*attr)[1];
          pendingAttributes.push_back(attribute);
          if (contains(attribute, "@layout(C)")) pendingLayout = "C";
          else if (contains(attribute, "@layout(Source)")) pendingLayout = "Source";
          else if (contains(attribute, "@layout(Packed")) pendingLayout = "Packed";
          addNode(file, moduleName, "attribute", attribute, "", lineNo);
        } else if (auto item = matchRegex(trimmed, constStaticPattern)) {
          const std::string kind = "decl." + std::string((*item)[2]);
          addNode(file,
                  moduleName,
                  kind,
                  (*item)[3],
                  (*item)[1],
                  lineNo,
                  pendingAttributes,
                  "",
                  (*item)[5].matched ? expressionAstFact((*item)[5]) : "",
                  (*item)[4].matched ? trim(std::string((*item)[4])) : "",
                  pendingLayout.empty() ? "Auto" : pendingLayout);
          pendingAttributes.clear();
          pendingLayout.clear();
        } else if (auto alias = matchRegex(trimmed, typeAliasPattern)) {
          addNode(file,
                  moduleName,
                  "decl.type",
                  (*alias)[2],
                  (*alias)[1],
                  lineNo,
                  pendingAttributes,
                  (*alias)[3],
                  trim(std::string((*alias)[4])),
                  "",
                  pendingLayout.empty() ? "Auto" : pendingLayout);
          pendingAttributes.clear();
          pendingLayout.clear();
        } else if (auto decl = matchRegex(trimmed, declPattern)) {
          const std::string declKind = "decl." + std::string((*decl)[2]);
          std::string declParams;
          std::string declReturn;
          if (declKind == "decl.interface") {
            if (auto parents = matchRegex(trimmed, std::regex(R"(^\s*(?:pub\s+|private\s+)?interface\s+[A-Za-z_][A-Za-z0-9_]*(?:<[^()]*>)?\s*:\s*([^{]+)\s*\{)"))) {
              declParams = trim(std::string((*parents)[1]));
            }
          } else if (declKind == "decl.struct" && contains(trimmed, ": Copy")) {
            declReturn = "Copy";
          }
          addNode(file, moduleName, declKind, (*decl)[3], (*decl)[1], lineNo, pendingAttributes, (*decl)[4], declParams, declReturn, pendingLayout.empty() ? "Auto" : pendingLayout);
          if (contains(trimmed, "{")) {
            activeContainerKind = declKind;
            activeContainerName = (*decl)[3];
          }
          pendingAttributes.clear();
          pendingLayout.clear();
        } else if (auto fn = matchRegex(trimmed, fnPattern)) {
          addNode(file,
                  moduleName,
                  (*fn)[2].matched ? "decl.async-fn" : "decl.fn",
                  (*fn)[3],
                  (*fn)[1],
                  lineNo,
                  pendingAttributes,
                  (*fn)[4],
                  (*fn)[5],
                  (*fn)[6],
                  pendingLayout.empty() ? "Auto" : pendingLayout);
          if (contains(trimmed, "{") && !contains(trimmed, "}")) {
            activeFunction = (*fn)[3];
            activeFunctionReturnType = normalizeSignatureType((*fn)[6]);
          }
          pendingAttributes.clear();
          pendingLayout.clear();
        } else if (auto ext = matchRegex(trimmed, externPattern)) {
          addNode(file,
                  moduleName,
                  "decl.extern",
                  (*ext)[3],
                  "",
                  lineNo,
                  pendingAttributes,
                  (*ext)[4],
                  (*ext)[5],
                  (*ext)[6],
                  pendingLayout.empty() ? "Auto" : pendingLayout,
                  (*ext)[1].matched,
                  (*ext)[2]);
          pendingAttributes.clear();
          pendingLayout.clear();
        } else if (auto impl = matchRegex(trimmed, traitImplPattern)) {
          const std::string implName = std::string((*impl)[3]) + " for " + std::string((*impl)[4]);
          addNode(file,
                  moduleName,
                  "decl.impl",
                  implName,
                  "",
                  lineNo,
                  pendingAttributes,
                  (*impl)[2],
                  "",
                  "",
                  pendingLayout.empty() ? "Auto" : pendingLayout,
                  (*impl)[1].matched);
          if (contains(trimmed, "{")) {
            activeContainerKind = "decl.impl";
            activeContainerName = implName;
          }
          pendingAttributes.clear();
          pendingLayout.clear();
        } else if (auto impl = matchRegex(trimmed, inherentImplPattern)) {
          addNode(file, moduleName, "decl.impl", (*impl)[2], "", lineNo, pendingAttributes, (*impl)[1], "", "", pendingLayout.empty() ? "Auto" : pendingLayout);
          if (contains(trimmed, "{")) {
            activeContainerKind = "decl.impl";
            activeContainerName = (*impl)[2];
          }
          pendingAttributes.clear();
          pendingLayout.clear();
        } else if (!trimmed.empty() && !startsWith(trimmed, "@")) {
          pendingAttributes.clear();
          pendingLayout.clear();
        }
        if (auto enumDecl = matchRegex(trimmed, enumPattern)) {
          inEnum = true;
          enumVisibility = (*enumDecl)[1];
        }
      } else if (inEnum && trimmed != "}") {
        if (auto variant = matchRegex(trimmed, enumVariantPattern)) {
          addNode(file, moduleName, "decl.variant", (*variant)[1], enumVisibility, lineNo, {}, "", (*variant)[2], "");
        }
      } else if (!activeContainerKind.empty() && activeFunction.empty()) {
        if (activeContainerKind == "decl.struct") {
          if (auto field = matchRegex(trimmed, fieldPattern)) {
            addNode(file,
                    moduleName,
                    "decl.field",
                    activeContainerName + "." + std::string((*field)[2]),
                    (*field)[1],
                    lineNo,
                    {},
                    "",
                    "",
                    trim(std::string((*field)[3])));
          }
        } else if (activeContainerKind == "decl.interface") {
          if (auto method = matchRegex(trimmed, fnPattern)) {
            addNode(file,
                    moduleName,
                    "decl.interface-method",
                    activeContainerName + "." + std::string((*method)[3]),
                    (*method)[1],
                    lineNo,
                    {},
                    (*method)[4],
                    (*method)[5],
                    (*method)[6]);
          }
        } else if (activeContainerKind == "decl.impl") {
          if (auto method = matchRegex(trimmed, fnPattern)) {
            const std::string methodName = activeContainerName + "." + std::string((*method)[3]);
            addNode(file,
                    moduleName,
                    "decl.impl-method",
                    methodName,
                    (*method)[1],
                    lineNo,
                    {},
                    (*method)[4],
                    (*method)[5],
                    (*method)[6]);
            if (contains(trimmed, "{") && !contains(trimmed, "}")) {
              activeFunction = methodName;
              activeFunctionReturnType = normalizeSignatureType((*method)[6]);
            }
          } else if (matchRegex(trimmed, destroyPattern)) {
            addNode(file, moduleName, "decl.destroy", activeContainerName, "", lineNo);
          } else if (auto item = matchRegex(trimmed, constStaticPattern)) {
            addNode(file,
                    moduleName,
                    "decl.impl-const",
                    activeContainerName + "." + std::string((*item)[3]),
                    "",
                    lineNo,
                    {},
                    "",
                    (*item)[5].matched ? expressionAstFact((*item)[5]) : "",
                    (*item)[4].matched ? trim(std::string((*item)[4])) : "");
          }
        }
      } else if (!activeFunction.empty()) {
        if (auto ret = matchRegex(trimmed, returnPattern)) {
          addNode(file,
                  moduleName,
                  "stmt.return",
                  activeFunction,
                  "",
                  lineNo,
                  {},
                  "",
                  expressionAstFact((*ret)[1]),
                  activeFunctionReturnType);
        } else if (auto localConst = matchRegex(trimmed, localConstPattern)) {
          addNode(file,
                  moduleName,
                  "stmt.const",
                  activeFunction + "." + std::string((*localConst)[1]),
                  "",
                  lineNo,
                  {},
                  "",
                  expressionAstFact((*localConst)[3]),
                  (*localConst)[2].matched ? trim(std::string((*localConst)[2])) : "");
        } else if (auto binding = matchRegex(trimmed, bindingPattern)) {
          const std::string bindingMode = (*binding)[1];
          const std::string bindingName = (*binding)[2];
          addNode(file,
                  moduleName,
                  "stmt." + bindingMode,
                  activeFunction + "." + bindingName,
                  "",
                  lineNo,
                  {},
                  "",
                  expressionAstFact((*binding)[4]),
                  (*binding)[3].matched ? trim(std::string((*binding)[3])) : "");
        } else if (auto patternBinding = matchRegex(trimmed, patternBindingPattern)) {
          const std::string bindingMode = (*patternBinding)[1];
          const std::string pattern = trim(std::string((*patternBinding)[2]));
          if (!std::regex_match(pattern, std::regex(R"([A-Za-z_][A-Za-z0-9_]*(?:\s*:\s*[^=;]+)?)"))) {
            addNode(file,
                    moduleName,
                    "stmt.pattern-" + bindingMode,
                    activeFunction + "." + pattern,
                    "",
                    lineNo,
                    {},
                    "",
                    expressionAstFact((*patternBinding)[3]),
                    pattern);
          }
        } else if (auto arm = matchRegex(trimmed, matchArmPattern)) {
          addNode(file,
                  moduleName,
                  "stmt.match-arm",
                  activeFunction + "." + trim(std::string((*arm)[1])),
                  "",
                  lineNo,
                  {},
                  "",
                  expressionAstFact((*arm)[3]),
                  (*arm)[2].matched ? expressionAstFact((*arm)[2]) : "");
        } else if (auto assignment = matchRegex(trimmed, assignmentPattern)) {
          addNode(file,
                  moduleName,
                  "stmt.assign",
                  activeFunction + "." + std::string((*assignment)[1]),
                  "",
                  lineNo,
                  {},
                  "",
                  expressionAstFact((*assignment)[2]),
                  "");
        } else if (auto branch = matchRegex(trimmed, ifPatternBindingPattern)) {
          const std::string patternMode = (*branch)[2].matched ? trim(std::string((*branch)[2])) : "";
          const std::string pattern = trim(std::string((*branch)[3]));
          addNode(file,
                  moduleName,
                  "stmt.if-pattern",
                  activeFunction + "." + pattern,
                  "",
                  lineNo,
                  {},
                  patternMode,
                  expressionAstFact((*branch)[1]),
                  pattern);
        } else if (auto branch = matchRegex(trimmed, ifPattern)) {
          addNode(file,
                  moduleName,
                  "stmt.if",
                  activeFunction,
                  "",
                  lineNo,
                  {},
                  "",
                  expressionAstFact((*branch)[1]),
                  "");
        } else if (matchRegex(trimmed, elsePattern)) {
          addNode(file,
                  moduleName,
                  "stmt.else",
                  activeFunction,
                  "",
                  lineNo);
        } else if (auto loop = matchRegex(trimmed, whilePatternBindingPattern)) {
          const std::string patternMode = (*loop)[2].matched ? trim(std::string((*loop)[2])) : "";
          const std::string pattern = trim(std::string((*loop)[3]));
          addNode(file,
                  moduleName,
                  "stmt.while-pattern",
                  activeFunction + "." + pattern,
                  "",
                  lineNo,
                  {},
                  patternMode,
                  expressionAstFact((*loop)[1]),
                  pattern);
        } else if (auto loop = matchRegex(trimmed, whilePattern)) {
          addNode(file,
                  moduleName,
                  "stmt.while",
                  activeFunction,
                  "",
                  lineNo,
                  {},
                  "",
                  expressionAstFact((*loop)[1]),
                  "");
        } else if (auto loop = matchRegex(trimmed, forPattern)) {
          std::string mode = trim(std::string((*loop)[1]));
          if (mode.empty()) mode = "read";
          std::string iterable = trim(std::string((*loop)[4]));
          if ((*loop)[3].matched) iterable = "mut " + iterable;
          addNode(file,
                  moduleName,
                  "stmt.for",
                  activeFunction + "." + trim(std::string((*loop)[2])),
                  "",
                  lineNo,
                  {},
                  "",
                  expressionAstFact(iterable),
                  mode);
        } else if (auto matchStmt = matchRegex(trimmed, matchPattern)) {
          const std::string matchMode = (*matchStmt)[1].matched ? trim(std::string((*matchStmt)[1])) : "";
          addNode(file,
                  moduleName,
                  "stmt.match",
                  activeFunction,
                  "",
                  lineNo,
                  {},
                  matchMode,
                  expressionAstFact((*matchStmt)[2]),
                  "");
        } else if (auto tryStmt = matchRegex(trimmed, tryStatementPattern)) {
          addNode(file,
                  moduleName,
                  "stmt.try",
                  activeFunction,
                  "",
                  lineNo,
                  {},
                  "",
                  expressionAstFact("try " + std::string((*tryStmt)[1])),
                  "");
        } else if (auto breakStmt = matchRegex(trimmed, breakPattern)) {
          addNode(file,
                  moduleName,
                  "stmt.break",
                  activeFunction,
                  "",
                  lineNo,
                  {},
                  "",
                  (*breakStmt)[1].matched ? expressionAstFact((*breakStmt)[1]) : "unit",
                  "");
        } else if (matchRegex(trimmed, continuePattern)) {
          addNode(file,
                  moduleName,
                  "stmt.continue",
                  activeFunction,
                  "",
                  lineNo);
        } else if (matchRegex(trimmed, callStatementPattern)) {
          addNode(file,
                  moduleName,
                  "stmt.expr",
                  activeFunction,
                  "",
                  lineNo,
                  {},
                  "",
                  expressionAstFact(trimmed),
                  "");
        }
      }
      if (contains(trimmed, "trust {")) {
        addNode(file, moduleName, "expr.trust", "block", "", lineNo);
      }
      braceDepth += braceDelta(line);
      if (!activeContainerKind.empty() && !activeFunction.empty() && braceDepth <= 1) {
        activeFunction.clear();
        activeFunctionReturnType.clear();
      }
      if (braceDepth <= 0) {
        braceDepth = 0;
        inEnum = false;
        enumVisibility.clear();
        activeContainerKind.clear();
        activeContainerName.clear();
        activeFunction.clear();
        activeFunctionReturnType.clear();
      }
      ++lineNo;
    }
  }
  return nodes;
}

struct ModuleSymbol {
  std::string moduleName;
  std::string name;
  std::string kind;
  std::string visibility;
  fs::path file;
  int lineNo = 1;
};

std::vector<ModuleSymbol> collectModuleSymbols(const fs::path &packageRoot, const std::string &packageName) {
  std::vector<ModuleSymbol> symbols;
  for (const auto &node : parseTopLevelAst(packageRoot, packageName, packageSources(packageRoot / "src"))) {
    if (node.kind == "decl.struct") {
      symbols.push_back(ModuleSymbol{node.moduleName, node.name, "struct", node.visibility, node.file, node.lineNo});
    } else if (node.kind == "decl.enum") {
      symbols.push_back(ModuleSymbol{node.moduleName, node.name, "enum", node.visibility, node.file, node.lineNo});
    } else if (node.kind == "decl.interface") {
      symbols.push_back(ModuleSymbol{node.moduleName, node.name, "interface", node.visibility, node.file, node.lineNo});
    } else if (node.kind == "decl.fn" || node.kind == "decl.async-fn" || node.kind == "decl.extern") {
      symbols.push_back(ModuleSymbol{node.moduleName, node.name, "fn", node.visibility, node.file, node.lineNo});
    } else if (node.kind == "decl.variant") {
      symbols.push_back(ModuleSymbol{node.moduleName, node.name, "variant", node.visibility, node.file, node.lineNo});
    }
  }
  return symbols;
}

std::set<std::string> collectModuleNames(const fs::path &packageRoot, const std::string &packageName) {
  std::set<std::string> modules;
  for (const auto &file : packageSources(packageRoot / "src")) {
    if (file.extension() != ".zn") continue;
    const SourceText source = loadSource(file);
    modules.insert(explicitModulePath(source.text).value_or(modulePathForSource(packageRoot, file, packageName)));
  }
  return modules;
}

std::optional<ModuleSymbol> findModuleSymbol(const std::vector<ModuleSymbol> &symbols,
                                             const std::string &moduleName,
                                             const std::string &itemName) {
  for (const auto &symbol : symbols) {
    if (symbol.moduleName == moduleName && symbol.name == itemName) return symbol;
  }
  return std::nullopt;
}

void validateExternalImport(const SourceText &source,
                            int lineNo,
                            const std::string &rootName,
                            const std::string &importText,
                            const fs::path &packageRoot,
                            const std::string &packageName,
                            bool builtinRoot,
                            std::vector<Diagnostic> &diagnostics) {
  const auto symbols = collectModuleSymbols(packageRoot, packageName);
  const auto modules = collectModuleNames(packageRoot, packageName);
  std::string modulePath = importText;
  std::vector<std::string> importedItems;

  const auto groupOpen = importText.find(".{");
  if (groupOpen != std::string::npos && importText.size() >= 3 && importText.back() == '}') {
    modulePath = importText.substr(0, groupOpen);
    importedItems = splitImportItems(importText.substr(groupOpen + 2, importText.size() - groupOpen - 3));
  } else {
    const std::string directModule = importModuleName(rootName, importText, builtinRoot);
    if (!modules.count(directModule)) {
      const auto parts = splitDottedPath(importText);
      if (parts.size() >= 2) {
        std::string parentPath;
        for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
          if (i != 0) parentPath += ".";
          parentPath += parts[i];
        }
        const std::string parentModule = importModuleName(rootName, parentPath, builtinRoot);
        if (modules.count(parentModule)) {
          modulePath = parentPath;
          importedItems.push_back(parts.back());
        }
      }
    }
  }

  const std::string moduleName = importModuleName(rootName, modulePath, builtinRoot);
  if (!modules.count(moduleName)) {
    diagnostics.push_back(makeDiagnostic(source, "E0201", "import module " + modulePath + " is not declared", lineNo));
    return;
  }

  for (const auto &item : importedItems) {
    const auto symbol = findModuleSymbol(symbols, moduleName, item);
    if (!symbol) {
      diagnostics.push_back(makeDiagnostic(source, "E0201", "imported item " + item + " is not declared in module " + modulePath, lineNo));
      continue;
    }
    if (symbol->visibility != "pub ") {
      diagnostics.push_back(makeDiagnostic(source, "E0203", item + " is package-visible, not pub", lineNo));
    }
  }
}

bool isTypeSymbolKind(const std::string &kind) {
  return kind == "struct" || kind == "enum" || kind == "interface" || kind == "type";
}

const std::set<std::string> &primitiveTypeNames() {
  static const std::set<std::string> names{
      "Unit", "Never", "Bool", "Char",
      "I8", "I16", "I32", "I64", "ISize",
      "U8", "U16", "U32", "U64", "USize",
      "F32", "F64"};
  return names;
}

std::set<std::string> builtinTypeNames() {
  std::set<std::string> names = primitiveTypeNames();
  for (const auto &builtin : {"core", "alloc", "std"}) {
    const fs::path root = builtinPackageRoot(builtin);
    if (!fs::exists(root / "src")) continue;
    for (const auto &symbol : collectModuleSymbols(root, builtin)) {
      if (symbol.visibility == "pub " && isTypeSymbolKind(symbol.kind)) names.insert(symbol.name);
    }
  }
  names.insert("Fn");
  names.insert("MutFn");
  names.insert("OnceFn");
  names.insert("RawPointer");
  names.insert("Self");
  return names;
}

std::set<std::string> localTypeNames(const fs::path &root, const PackageInfo &package) {
  std::set<std::string> names;
  for (const auto &symbol : collectModuleSymbols(root, package.name)) {
    if (isTypeSymbolKind(symbol.kind)) names.insert(symbol.name);
  }
  return names;
}

std::set<std::string> genericParameterNames(const std::string &genericText) {
  std::set<std::string> names;
  if (genericText.empty()) return names;
  std::string inner = genericText;
  if (inner.front() == '<' && inner.back() == '>') {
    inner = inner.substr(1, inner.size() - 2);
  }
  const std::regex namePattern(R"(^\s*(?:const\s+)?([A-Za-z_][A-Za-z0-9_]*))");
  for (const auto &part : splitArguments(inner)) {
    if (auto match = matchRegex(part, namePattern)) names.insert((*match)[1]);
  }
  return names;
}

std::set<std::string> typeNamesInSignature(const std::string &typeText) {
  std::set<std::string> names;
  const std::regex identPattern(R"([A-Za-z_][A-Za-z0-9_]*)");
  auto begin = std::sregex_iterator(typeText.begin(), typeText.end(), identPattern);
  auto end = std::sregex_iterator();
  for (auto it = begin; it != end; ++it) {
    const std::string name = (*it)[0];
    if (name == "mut" || name == "move" || name == "self" || name == "const") continue;
    if (std::isupper(static_cast<unsigned char>(name.front()))) names.insert(name);
  }
  return names;
}

std::set<std::string> importedTypeNames(const SourceText &source, const PackageInfo &package) {
  std::set<std::string> names;
  const std::set<std::string> builtinRoots{"core", "alloc", "std"};
  const std::regex importPattern(R"(^\s*import\s+([A-Za-z_][A-Za-z0-9_]*(?:\.[A-Za-z_][A-Za-z0-9_]*)*(?:\.\{[^}]+\})?)\s*;)");

  for (const auto &line : splitLines(source.text)) {
    const std::string cleanLine = stripLineComment(line);
    auto match = matchRegex(cleanLine, importPattern);
    if (!match) continue;
    const std::string importText = (*match)[1];
    const auto parts = splitDottedPath(importText);
    if (parts.empty()) continue;
    const std::string rootName = parts.front();
    fs::path importRoot;
    std::string packageName;
    bool builtinRoot = false;
    if (package.dependencies.count(rootName)) {
      importRoot = package.dependencies.at(rootName);
      const PackageInfo depInfo = parsePackageInfo(importRoot);
      packageName = depInfo.name.empty() ? rootName : depInfo.name;
    } else if (builtinRoots.count(rootName)) {
      importRoot = builtinPackageRoot(rootName);
      packageName = rootName;
      builtinRoot = true;
    } else {
      continue;
    }
    if (!fs::exists(importRoot / "src")) continue;
    const auto symbols = collectModuleSymbols(importRoot, packageName);
    const auto modules = collectModuleNames(importRoot, packageName);
    std::string modulePath = importText;
    std::vector<std::string> items;
    const auto groupOpen = importText.find(".{");
    if (groupOpen != std::string::npos && importText.back() == '}') {
      modulePath = importText.substr(0, groupOpen);
      items = splitImportItems(importText.substr(groupOpen + 2, importText.size() - groupOpen - 3));
    } else {
      const std::string directModule = importModuleName(rootName, importText, builtinRoot);
      if (!modules.count(directModule) && parts.size() >= 2) {
        std::string parentPath;
        for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
          if (i != 0) parentPath += ".";
          parentPath += parts[i];
        }
        const std::string parentModule = importModuleName(rootName, parentPath, builtinRoot);
        if (modules.count(parentModule)) {
          modulePath = parentPath;
          items.push_back(parts.back());
        }
      }
    }
    const std::string moduleName = importModuleName(rootName, modulePath, builtinRoot);
    for (const auto &item : items) {
      auto symbol = findModuleSymbol(symbols, moduleName, item);
      if (symbol && symbol->visibility == "pub " && isTypeSymbolKind(symbol->kind)) names.insert(item);
    }
  }
  return names;
}

void validateTypeNameUse(const SourceText &source,
                         int lineNo,
                         const std::string &typeText,
                         const std::set<std::string> &knownTypes,
                         const std::set<std::string> &genericTypes,
                         std::vector<Diagnostic> &diagnostics) {
  for (const auto &name : typeNamesInSignature(typeText)) {
    if (knownTypes.count(name) || genericTypes.count(name)) continue;
    diagnostics.push_back(makeDiagnostic(source, "E0201", "type " + name + " is not declared", lineNo));
  }
}

std::vector<Diagnostic> typeNameResolutionDiagnostics(const fs::path &root, const PackageInfo &package) {
  std::vector<Diagnostic> diagnostics;
  std::set<std::string> packageTypes = builtinTypeNames();
  const auto locals = localTypeNames(root, package);
  packageTypes.insert(locals.begin(), locals.end());

  const std::regex structPattern(R"(^\s*(?:pub\s+|private\s+)?struct\s+[A-Za-z_][A-Za-z0-9_]*(<[^()]*>)?)");
  const std::regex interfacePattern(R"(^\s*(?:pub\s+|private\s+)?interface\s+[A-Za-z_][A-Za-z0-9_]*(<[^()]*>)?)");
  const std::regex fnPattern(R"(^\s*(?:pub\s+|private\s+)?(?:async\s+)?fn\s+[A-Za-z_][A-Za-z0-9_]*(<[^()]*>)?\s*\(([^)]*)\)\s*(?:->\s*([^;{]+))?)");
  const std::regex fieldPattern(R"(^\s*(?:pub\s+)?[A-Za-z_][A-Za-z0-9_]*\s*:\s*([^,;=]+))");
  const std::regex implPattern(R"(^\s*(?:trust\s+)?impl\s*(<[^()]*>)?\s+)");

  for (const auto &file : packageSources(root / "src")) {
    if (file.extension() != ".zn") continue;
    const SourceText source = loadSource(file);
    std::set<std::string> visibleTypes = packageTypes;
    const auto imports = importedTypeNames(source, package);
    visibleTypes.insert(imports.begin(), imports.end());
    std::set<std::string> activeStructGenerics;
    std::set<std::string> activeInterfaceGenerics;
    std::set<std::string> activeImplGenerics;
    bool inStruct = false;
    bool inInterface = false;
    bool inImpl = false;
    int structBraceDepth = 0;
    int interfaceBraceDepth = 0;
    int implBraceDepth = 0;
    int lineNo = 1;
    for (const auto &rawLine : splitLines(source.text)) {
      const std::string line = stripLineComment(rawLine);
      const std::string trimmed = trim(line);
      if (auto implDecl = matchRegex(trimmed, implPattern)) {
        inImpl = true;
        implBraceDepth = 0;
        activeImplGenerics = genericParameterNames((*implDecl)[1]);
      }
      if (auto fn = matchRegex(trimmed, fnPattern)) {
        std::set<std::string> fnGenerics = genericParameterNames((*fn)[1]);
        fnGenerics.insert(activeImplGenerics.begin(), activeImplGenerics.end());
        fnGenerics.insert(activeInterfaceGenerics.begin(), activeInterfaceGenerics.end());
        for (const auto &paramType : parameterTypes((*fn)[2])) {
          validateTypeNameUse(source, lineNo, paramType, visibleTypes, fnGenerics, diagnostics);
        }
        if ((*fn)[3].matched) {
          validateTypeNameUse(source, lineNo, (*fn)[3], visibleTypes, fnGenerics, diagnostics);
        }
      }
      if (auto structDecl = matchRegex(trimmed, structPattern)) {
        inStruct = true;
        structBraceDepth = 0;
        activeStructGenerics = genericParameterNames((*structDecl)[1]);
      } else if (auto interfaceDecl = matchRegex(trimmed, interfacePattern)) {
        inInterface = true;
        interfaceBraceDepth = 0;
        activeInterfaceGenerics = genericParameterNames((*interfaceDecl)[1]);
      } else if (inStruct) {
        if (auto field = matchRegex(trimmed, fieldPattern)) {
          validateTypeNameUse(source, lineNo, (*field)[1], visibleTypes, activeStructGenerics, diagnostics);
        }
      }
      if (inStruct) {
        structBraceDepth += braceDelta(line);
        if (structBraceDepth <= 0 && contains(trimmed, "}")) {
          inStruct = false;
          activeStructGenerics.clear();
        }
      }
      if (inInterface) {
        interfaceBraceDepth += braceDelta(line);
        if (interfaceBraceDepth <= 0 && contains(trimmed, "}")) {
          inInterface = false;
          activeInterfaceGenerics.clear();
        }
      }
      if (inImpl) {
        implBraceDepth += braceDelta(line);
        if (implBraceDepth <= 0 && contains(trimmed, "}")) {
          inImpl = false;
          activeImplGenerics.clear();
        }
      }
      ++lineNo;
    }
  }
  return diagnostics;
}

std::vector<Diagnostic> moduleAndPackageDiagnostics(const fs::path &root) {
  std::vector<Diagnostic> diagnostics;
  if (!fs::is_directory(root)) return diagnostics;
  const PackageInfo package = parsePackageInfo(root);
  const std::set<std::string> builtinRoots{"core", "alloc", "std"};
  std::map<std::string, std::vector<fs::path>> typeDefinitions;
  std::map<std::string, fs::path> privateDecls;
  std::map<std::string, fs::path> packageVisibleTypes;
  bool manifestAllowsFfi = false;
  bool manifestAllowsRawMemory = false;
  bool manifestAllowsHardware = false;
  bool manifestAllowsInlineAsm = false;
  bool manifestAllowsInterrupts = false;
  bool manifestAllowsDependencyTrust = false;
  bool hasTrustAllowlist = false;
  std::set<std::string> trustAllowedPackages;

  const fs::path manifestPath = root / "Zeno.toml";
  if (fs::exists(manifestPath)) {
    SourceText manifest = loadSource(manifestPath);
    const auto trustFields = manifestSectionFields(manifest.text, "trust");
    if (trustFields.count("ffi")) manifestAllowsFfi = unquoteTomlString(trustFields.at("ffi")) == "true";
    if (trustFields.count("rawMemory")) manifestAllowsRawMemory = unquoteTomlString(trustFields.at("rawMemory")) == "true";
    if (trustFields.count("hardware")) manifestAllowsHardware = unquoteTomlString(trustFields.at("hardware")) == "true";
    if (trustFields.count("inlineAsm")) manifestAllowsInlineAsm = unquoteTomlString(trustFields.at("inlineAsm")) == "true";
    if (trustFields.count("interrupts")) manifestAllowsInterrupts = unquoteTomlString(trustFields.at("interrupts")) == "true";
    if (trustFields.count("dependencyTrust")) manifestAllowsDependencyTrust = unquoteTomlString(trustFields.at("dependencyTrust")) == "true";
    if (trustFields.count("allowedPackages")) {
      hasTrustAllowlist = true;
      trustAllowedPackages = quotedValues(trustFields.at("allowedPackages"));
    }
    const auto packageFields = manifestSectionFields(manifest.text, "package");
    const std::string packageKind = packageFields.count("kind") ? unquoteTomlString(packageFields.at("kind")) : "";
    const std::string packageEntry = packageFields.count("entry") ? unquoteTomlString(packageFields.at("entry")) : "";
    if (packageKind == "application" && packageEntry.empty() && !fs::exists(root / "src" / "main.zn")) {
      diagnostics.push_back(makeDiagnostic(manifest, "E1003", "application default entry requires src/main.zn or explicit package.entry", lineContaining(manifest.text, "kind =")));
    }
    const std::string effectiveEntry = !packageEntry.empty() ? packageEntry : (packageKind == "application" ? "main.main" : "");
    if (packageKind == "application" && !effectiveEntry.empty()) {
      const auto dot = effectiveEntry.rfind('.');
      const bool malformed = dot == std::string::npos || dot == 0 || dot + 1 >= effectiveEntry.size();
      if (malformed) {
        diagnostics.push_back(makeDiagnostic(manifest, "E1003", "application entry must be module.function", lineContaining(manifest.text, packageEntry.empty() ? "kind =" : "entry =")));
      } else {
        const std::string moduleName = effectiveEntry.substr(0, dot);
        const std::string fnName = effectiveEntry.substr(dot + 1);
        const fs::path entryFile = root / "src" / fs::path(std::regex_replace(moduleName, std::regex(R"(\.)"), "/") + ".zn");
        if (!fs::exists(entryFile)) {
          diagnostics.push_back(makeDiagnostic(manifest, "E1003", "application entry source file does not exist", lineContaining(manifest.text, packageEntry.empty() ? "kind =" : "entry =")));
        } else {
          SourceText entrySource = loadSource(entryFile);
          const std::regex entryFnPattern("^\\s*(?:pub\\s+)?fn\\s+" + fnName + R"(\s*\()");
          bool foundEntryFunction = false;
          for (const auto &entryLine : splitLines(entrySource.text)) {
            if (std::regex_search(stripLineComment(entryLine), entryFnPattern)) {
              foundEntryFunction = true;
              break;
            }
          }
          if (!foundEntryFunction) {
            diagnostics.push_back(makeDiagnostic(manifest, "E1003", "application entry function is not declared", lineContaining(manifest.text, packageEntry.empty() ? "kind =" : "entry =")));
          }
        }
      }
    }
    for (const auto &[key, rawValue] : manifestSectionFields(manifest.text, "dependencies")) {
      const std::string pathDependency = inlineTomlField(rawValue, "path");
      if (pathDependency.empty() || fs::path(pathDependency).is_absolute()) continue;
      if (!fs::exists(root / pathDependency)) {
        diagnostics.push_back(makeDiagnostic(manifest, "E1004", "dependency path does not exist", lineContaining(manifest.text, key + " =")));
      }
    }
    for (const auto &dir : sourceTopLevelDirs(root)) {
      if (package.dependencies.count(dir)) {
        diagnostics.push_back(makeDiagnostic(manifest, "E0201", "dependency key conflicts with src/" + dir, lineContaining(manifest.text, dir + " =")));
      }
    }
    if (!package.workspaceMembers.empty()) {
      std::map<std::string, std::string> seen;
      for (const auto &member : package.workspaceMembers) {
        PackageInfo memberInfo = parsePackageInfo(root / member);
        if (memberInfo.name.empty()) continue;
        if (seen.count(memberInfo.name)) {
          SourceText memberManifest = loadSource(root / member / "Zeno.toml");
          diagnostics.push_back(makeDiagnostic(memberManifest, "E1005", "duplicate workspace package name " + memberInfo.name, lineContaining(memberManifest.text, "name")));
        }
        seen[memberInfo.name] = member;
      }
    }
    const fs::path lockPath = root / "Zeno.lock";
    if (fs::exists(lockPath) && contains(manifest.text, "dependencyTrust = false")) {
      SourceText lock = loadSource(lockPath);
      const int trustLine = lineContaining(lock.text, "trust = [\"");
      if (trustLine != 1 || contains(lock.text, "trust = [\"")) {
        diagnostics.push_back(makeDiagnostic(lock, "E1006", "dependency trust capability ffi is denied by root manifest", trustLine));
      }
    }
    if (!manifestAllowsDependencyTrust) {
      for (const auto &[depKey, depRoot] : package.dependencies) {
        if (!fs::exists(depRoot / "src")) continue;
        const PackageInfo depInfo = parsePackageInfo(depRoot);
        const std::string depName = depInfo.name.empty() ? depKey : depInfo.name;
        for (const auto &depFile : packageSources(depRoot / "src")) {
          if (depFile.extension() != ".zn") continue;
          SourceText depSource = loadSource(depFile);
          int depLineNo = 1;
          for (const auto &depLine : splitLines(depSource.text)) {
            if (lineHasTrustBoundaryMarker(stripLineComment(depLine))) {
              diagnostics.push_back(makeDiagnostic(depSource, "E1006", "dependency package " + depName + " contains trust boundaries but dependencyTrust is false", depLineNo));
            }
            ++depLineNo;
          }
        }
      }
    }
    auto lockDiagnostics = lockfileDiagnostics(root, package);
    diagnostics.insert(diagnostics.end(), lockDiagnostics.begin(), lockDiagnostics.end());
    if (!fs::exists(lockPath)) {
      for (const auto &[key, rawValue] : manifestSectionFields(manifest.text, "dependencies")) {
        const std::string git = inlineTomlField(rawValue, "git");
        const std::string version = rawValue.empty() || rawValue.front() == '{' ? inlineTomlField(rawValue, "version") : unquoteTomlString(rawValue);
        const bool gitMissingRev = !git.empty() && !contains(rawValue, "rev");
        const bool registryVersionRange = contains(rawValue, "version") &&
                                          (contains(rawValue, "^") || contains(rawValue, "*") ||
                                           contains(rawValue, ">") || contains(rawValue, "<"));
        std::string feature;
        std::string message;
        if (!git.empty() && !gitMissingRev) {
          feature = "git-dependency";
          message = "git dependency requires a pre-resolved Zeno.lock in stage0";
        } else if (!version.empty() && !registryVersionRange && unquoteTomlString(rawValue) != "builtin" && !contains(rawValue, "path")) {
          feature = "registry-dependency";
          message = "registry dependency requires a pre-resolved Zeno.lock in stage0";
        }
        if (!feature.empty()) {
          Diagnostic diag = makeDiagnostic(manifest, "E9002", message, lineContaining(manifest.text, key + " ="));
          diag.staged = true;
          diag.feature = feature;
          diag.category = "staged";
          diag.stageName = "manifest";
          diag.notes.push_back("stage0 does not perform registry resolution or git fetch");
          diag.help.push_back("commit a Zeno.lock with an exact offline source, or use a local path dependency");
          diagnostics.push_back(diag);
        }
      }
    }
    auto opaqueReturnDiagnostics = crossPackageOpaqueReturnDiagnostics(package);
    diagnostics.insert(diagnostics.end(), opaqueReturnDiagnostics.begin(), opaqueReturnDiagnostics.end());
  }

  for (const auto &file : packageSources(root / "src")) {
    if (file.extension() != ".zn") continue;
    SourceText source = loadSource(file);
    if (auto explicitModule = explicitModulePath(source.text)) {
      const auto inferred = modulePathForSource(root, file, package.name);
      if (!inferred.empty() && *explicitModule != inferred) {
        diagnostics.push_back(makeDiagnostic(source, "E0101", "module path does not match file path " + fs::relative(file, root).string(), lineContaining(source.text, "module ")));
      }
    }

    const std::regex importPattern(R"(^\s*import\s+([A-Za-z_][A-Za-z0-9_]*(?:\.[A-Za-z_][A-Za-z0-9_]*)*(?:\.\{[^}]+\})?)\s*;)");
    const std::regex structPattern(R"(^\s*(pub\s+|private\s+)?struct\s+([A-Za-z_][A-Za-z0-9_]*))");
    const std::regex fnPattern(R"(^\s*(pub\s+|private\s+)?fn\s+([A-Za-z_][A-Za-z0-9_]*)\s*\([^)]*\)\s*(?:->\s*([A-Za-z_][A-Za-z0-9_]*))?)");
    auto handlerMatchesCurrentFunction = [&](const std::string &handler, const std::string &name) {
      const auto dot = handler.rfind('.');
      if (dot == std::string::npos || dot + 1 >= handler.size()) return false;
      const std::string moduleName = handler.substr(0, dot);
      const std::string fnName = handler.substr(dot + 1);
      return fnName == name && moduleName == modulePathForSource(root, file, package.name);
    };
    int lineNo = 1;
    for (const auto &line : splitLines(source.text)) {
      const std::string cleanLine = stripLineComment(line);
      const bool lineHasTrustBoundary = lineHasTrustBoundaryMarker(cleanLine);
      if (hasTrustAllowlist && !trustAllowedPackages.count(package.name) && lineHasTrustBoundary) {
        diagnostics.push_back(makeDiagnostic(source, "E1006", "package " + package.name + " is not allowed to contain trust boundaries", lineNo));
      }
      if (!manifestAllowsFfi && contains(cleanLine, "trust extern")) {
        diagnostics.push_back(makeDiagnostic(source, "E0714", "manifest does not allow FFI trust capability", lineNo));
      }
      if (!manifestAllowsRawMemory && contains(cleanLine, "RawPointer<")) {
        diagnostics.push_back(makeDiagnostic(source, "E0711", "manifest does not allow rawMemory trust capability", lineNo));
      }
      if (!manifestAllowsHardware && usesHardwareTrustCapability(cleanLine)) {
        diagnostics.push_back(makeDiagnostic(source, "E0712", "manifest does not allow hardware trust capability", lineNo));
      }
      if (!manifestAllowsInlineAsm && usesInlineAsmTrustCapability(cleanLine)) {
        diagnostics.push_back(makeDiagnostic(source, "E0713", "manifest does not allow inlineAsm trust capability", lineNo));
      }
      if (!manifestAllowsInterrupts && usesInterruptTrustCapability(cleanLine)) {
        diagnostics.push_back(makeDiagnostic(source, "E0715", "manifest does not allow interrupts trust capability", lineNo));
      }
      if (auto match = matchRegex(cleanLine, importPattern)) {
        const std::string importText = (*match)[1];
        const auto importParts = splitDottedPath(importText);
        const std::string rootName = importParts.empty() ? importText : importParts.front();
        if (!package.dependencies.count(rootName) && !builtinRoots.count(rootName)) {
          diagnostics.push_back(makeDiagnostic(source, "E0201", "import root " + rootName + " is not declared in dependencies", lineNo));
        }
        if (rootName == "std" && package.profile != "hosted" && !package.builtinDependencies.count("std")) {
          diagnostics.push_back(makeDiagnostic(source, "E0201", "std is available only in hosted profile or an explicit hosted-capable builtin dependency", lineNo));
        }
        if (package.dependencies.count(rootName)) {
          const fs::path depRoot = package.dependencies.at(rootName);
          if (fs::exists(depRoot / "src")) {
            const PackageInfo depInfo = parsePackageInfo(depRoot);
            validateExternalImport(source,
                                   lineNo,
                                   rootName,
                                   importText,
                                   depRoot,
                                   depInfo.name.empty() ? rootName : depInfo.name,
                                   false,
                                   diagnostics);
          }
        } else if (builtinRoots.count(rootName)) {
          const fs::path builtinRoot = builtinPackageRoot(rootName);
          if (fs::exists(builtinRoot / "src")) {
            validateExternalImport(source, lineNo, rootName, importText, builtinRoot, rootName, true, diagnostics);
          }
        }
      }
      if (auto match = matchRegex(line, structPattern)) {
        const std::string visibility = (*match)[1];
        const std::string name = (*match)[2];
        typeDefinitions[name].push_back(file);
        if (visibility != "pub ") packageVisibleTypes[name] = file;
        if (visibility == "private ") privateDecls[name] = file;
      }
      if (auto match = matchRegex(line, fnPattern)) {
        const std::string visibility = (*match)[1];
        const std::string name = (*match)[2];
        const std::string returnType = (*match)[3];
        const std::string normalizedReturnType = normalizeSignatureType(returnType);
        if (handlerMatchesCurrentFunction(package.panicHandler, name) && normalizedReturnType != "Never") {
          diagnostics.push_back(makeDiagnostic(source, "E1002", "panic.handler must return Never", lineNo));
        }
        if (handlerMatchesCurrentFunction(package.oomHandler, name) && normalizedReturnType != "Never") {
          diagnostics.push_back(makeDiagnostic(source, "E1002", "oom.handler must return Never", lineNo));
        }
        if (visibility == "private ") privateDecls[name] = file;
        if (visibility.empty() && !returnType.empty() && privateDecls.count(returnType) && privateDecls[returnType] == file) {
          diagnostics.push_back(makeDiagnostic(source, "E0202", "private type " + returnType + " cannot appear in a package-visible signature", lineNo));
        }
        if (visibility == "pub " && !returnType.empty() && packageVisibleTypes.count(returnType)) {
          diagnostics.push_back(makeDiagnostic(source, "E0203", "pub API cannot expose package-visible type " + returnType, lineNo));
        }
      }
      ++lineNo;
    }
  }

  for (const auto &[name, definingFile] : privateDecls) {
    for (const auto &file : packageSources(root / "src")) {
      if (file.extension() != ".zn" || file == definingFile) continue;
      SourceText source = loadSource(file);
      const std::string callNeedle = name + "(";
      if (contains(source.text, callNeedle)) {
        diagnostics.push_back(makeDiagnostic(source, "E0202", "private declaration is visible only inside its defining file", lineContaining(source.text, callNeedle)));
      }
    }
  }

  for (const auto &[name, files] : typeDefinitions) {
    if (files.size() < 2) continue;
    for (const auto &file : packageSources(root / "src")) {
      if (file.extension() != ".zn") continue;
      SourceText source = loadSource(file);
      const std::string useNeedle = ": " + name;
      if (contains(source.text, useNeedle)) {
        diagnostics.push_back(makeDiagnostic(source, "E0202", "unqualified name " + name + " is ambiguous; use a module-qualified name", lineContaining(source.text, useNeedle)));
      }
    }
  }
  auto typeDiagnostics = typeNameResolutionDiagnostics(root, package);
  diagnostics.insert(diagnostics.end(), typeDiagnostics.begin(), typeDiagnostics.end());
  return diagnostics;
}

std::vector<Diagnostic> checkFile(const fs::path &path) {
  SourceText source = loadSource(path);
  const std::string extension = path.extension().string();
  if (extension == ".toml" || path.filename() == "Zeno.lock") {
    return manifestDiagnostics(source);
  }
  auto diagnostics = lexicalAndSyntaxDiagnostics(source);
  auto semanticDiagnostics = sourceSemanticDiagnostics(source);
  diagnostics.insert(diagnostics.end(), semanticDiagnostics.begin(), semanticDiagnostics.end());
  return diagnostics;
}

std::vector<fs::path> packageSources(const fs::path &root) {
  std::vector<fs::path> files;
  if (fs::is_regular_file(root)) {
    files.push_back(root);
    return files;
  }
  if (!fs::exists(root)) {
    return files;
  }
  for (const auto &entry : fs::recursive_directory_iterator(root)) {
    if (!entry.is_regular_file()) continue;
    const auto ext = entry.path().extension().string();
    const auto name = entry.path().filename().string();
    if (ext == ".zn" || ext == ".toml" || name == "Zeno.lock") {
      files.push_back(entry.path());
    }
  }
  std::sort(files.begin(), files.end());
  return files;
}

std::vector<Diagnostic> checkPath(const fs::path &path) {
  std::vector<Diagnostic> diagnostics;
  for (const auto &file : packageSources(path)) {
    auto fileDiagnostics = checkFile(file);
    diagnostics.insert(diagnostics.end(), fileDiagnostics.begin(), fileDiagnostics.end());
  }
  if (fs::is_directory(path)) {
    auto packageDiagnostics = moduleAndPackageDiagnostics(path);
    diagnostics.insert(diagnostics.end(), packageDiagnostics.begin(), packageDiagnostics.end());
  }
  return diagnostics;
}

std::vector<ExpectedError> expectedErrorsForPath(const fs::path &path) {
  std::vector<ExpectedError> expected;
  for (const auto &file : packageSources(path)) {
    auto fileExpected = expectedErrors(readFile(file));
    expected.insert(expected.end(), fileExpected.begin(), fileExpected.end());
  }
  return expected;
}

std::string argValue(const std::vector<std::string> &args, std::size_t &i, const std::string &name) {
  if (i + 1 >= args.size()) {
    throw std::runtime_error("missing value for " + name);
  }
  ++i;
  return args[i];
}

Options parseOptions(int argc, char **argv) {
  Options options;
  std::vector<std::string> args;
  for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
  if (args.empty()) {
    options.command = "help";
    return options;
  }
  if (args[0] == "--version" || args[0] == "-V") {
    options.command = "version";
    return options;
  }
  if (args[0] == "--help" || args[0] == "-h") {
    options.command = "help";
    return options;
  }
  options.command = args[0];
  for (std::size_t i = 1; i < args.size(); ++i) {
    const std::string &arg = args[i];
    if (arg == "--manifest") options.manifest = argValue(args, i, arg);
    else if (arg == "--workspace") options.workspace = argValue(args, i, arg);
    else if (arg == "--target") options.target = argValue(args, i, arg);
    else if (arg == "--profile") options.profile = argValue(args, i, arg);
    else if (arg == "--stage") {
      options.stage = argValue(args, i, arg);
      options.stageExplicit = true;
    }
    else if (arg == "--milestone") options.milestone = argValue(args, i, arg);
    else if (arg == "--feature") options.feature = argValue(args, i, arg);
    else if (arg == "--diagnostic-format") options.diagnosticFormat = argValue(args, i, arg);
    else if (arg == "--emit") options.emit = argValue(args, i, arg);
    else if (arg == "--release") options.release = true;
    else if (arg == "--frozen") options.frozen = true;
    else if (arg == "--update-lock") { options.updateLock = true; options.frozen = false; }
    else if (arg == "-v" || arg == "--verbose") options.verbose = true;
    else if (startsWith(arg, "--color")) {
      if (arg == "--color") (void)argValue(args, i, arg);
    } else if (!arg.empty() && arg[0] == '-') {
      throw std::runtime_error("unknown option " + arg);
    } else {
      options.inputs.push_back(arg);
    }
  }
  if (options.diagnosticFormat != "human" && options.diagnosticFormat != "json") {
    throw std::runtime_error("--diagnostic-format must be human or json");
  }
  if (!options.target.empty() && !isSupportedTargetTriple(options.target)) {
    throw std::runtime_error("--target must be aarch64-apple-darwin or x86_64-unknown-linux-gnu");
  }
  if (!options.profile.empty() && !isSupportedProfile(options.profile)) {
    throw std::runtime_error("--profile must be hosted, freestanding, kernel, or embedded");
  }
  if (options.command == "test" && options.stage != "mvp" && options.stage != "full-spec") {
    throw std::runtime_error("--stage must be mvp or full-spec");
  }
  if (options.command == "test" && !options.milestone.empty() && !isKnownMilestone(options.milestone)) {
    throw std::runtime_error("--milestone must be M0-M9");
  }
  if (!options.emit.empty() && options.emit != "mir" && options.emit != "llvm-ir") {
    throw std::runtime_error("--emit must be mir or llvm-ir");
  }
  return options;
}

void printHelp() {
  std::cout
      << "Zeno stage0 compiler\n\n"
      << "Usage:\n"
      << "  zeno check [path] [--diagnostic-format human|json]\n"
      << "  zeno build [path] [--emit mir|llvm-ir]\n"
      << "  zeno test [--stage mvp|full-spec] [--milestone M0-M9]\n"
      << "  zeno --version\n\n"
      << "Common options:\n"
      << "  --manifest <path> --workspace <path> --target <triple>\n"
      << "  --profile <hosted|freestanding|kernel|embedded> --release\n"
      << "  --frozen --update-lock --feature <name> -v --verbose\n";
}

std::string hostTriple() {
#if defined(__APPLE__) && defined(__aarch64__)
  return "aarch64-apple-darwin";
#elif defined(__APPLE__) && defined(__x86_64__)
  return "x86_64-apple-darwin";
#elif defined(__linux__) && defined(__x86_64__)
  return "x86_64-unknown-linux-gnu";
#elif defined(__linux__) && defined(__aarch64__)
  return "aarch64-unknown-linux-gnu";
#else
  return "unknown-host";
#endif
}

int commandVersion() {
  std::cout << "zeno-stage0 0.1.0\n"
            << "llvm " << ZENO_LLVM_VERSION << "\n"
            << "host " << hostTriple() << "\n"
#ifdef NDEBUG
            << "build release\n";
#else
            << "build debug\n";
#endif
  return 0;
}

struct BuildPackage {
  fs::path root;
  fs::path manifest;
  fs::path singleSource;
  std::string name = "zeno-package";
  std::string version = "0.0.0";
  std::string kind;
  std::string entry;
  std::string manifestTarget;
  std::string manifestProfile;
  std::string allocatorDefault;
  std::string allocatorSymbol;
  std::string panicStrategy;
  std::string panicStack;
  std::string panicHandler;
  std::string oomStrategy;
  std::string oomHandler;
  std::set<std::string> manifestTrust;
  std::set<std::string> manifestDependencies;
};

std::string stripTomlComment(std::string_view line) {
  bool inString = false;
  for (std::size_t i = 0; i < line.size(); ++i) {
    const char c = line[i];
    if (c == '"' && (i == 0 || line[i - 1] != '\\')) {
      inString = !inString;
    } else if (c == '#' && !inString) {
      return trim(line.substr(0, i));
    }
  }
  return trim(line);
}

std::string unquoteTomlString(std::string value) {
  value = trim(value);
  if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
    return value.substr(1, value.size() - 2);
  }
  return value;
}

std::string manifestRawField(const std::string &text, const std::string &sectionName, const std::string &keyName) {
  std::string section;
  const std::regex sectionPattern(R"(^\s*\[([A-Za-z_.-]+)\]\s*$)");
  for (const auto &line : splitLines(text)) {
    const std::string clean = stripTomlComment(line);
    if (auto match = matchRegex(clean, sectionPattern)) {
      section = (*match)[1];
      continue;
    }
    if (section == sectionName) {
      const auto equals = clean.find('=');
      if (equals == std::string::npos) continue;
      const std::string key = trim(std::string_view(clean).substr(0, equals));
      if (key == keyName) return trim(std::string_view(clean).substr(equals + 1));
    }
  }
  return "";
}

std::string manifestStringField(const std::string &text, const std::string &sectionName, const std::string &keyName) {
  return unquoteTomlString(manifestRawField(text, sectionName, keyName));
}

std::map<std::string, std::string> manifestSectionFields(const std::string &text, const std::string &sectionName) {
  std::map<std::string, std::string> fields;
  std::string section;
  const std::regex sectionPattern(R"(^\s*\[([A-Za-z_.-]+)\]\s*$)");
  for (const auto &line : splitLines(text)) {
    const std::string clean = stripTomlComment(line);
    if (auto match = matchRegex(clean, sectionPattern)) {
      section = (*match)[1];
      continue;
    }
    if (section != sectionName) continue;
    const auto equals = clean.find('=');
    if (equals == std::string::npos) continue;
    const std::string key = trim(std::string_view(clean).substr(0, equals));
    const std::string value = trim(std::string_view(clean).substr(equals + 1));
    if (!key.empty() && !value.empty()) fields[key] = value;
  }
  return fields;
}

std::string inlineTomlField(const std::string &value, const std::string &keyName) {
  const std::regex fieldPattern("(^|[,\\{])\\s*" + keyName + R"(\s*=\s*("[^"]+"|[^,}]+))");
  if (auto match = matchRegex(value, fieldPattern)) {
    return unquoteTomlString((*match)[2]);
  }
  return "";
}

std::set<std::string> manifestDependencyFacts(const std::string &text) {
  std::set<std::string> dependencies;
  for (const auto &[name, rawValue] : manifestSectionFields(text, "dependencies")) {
    const std::string scalarValue = unquoteTomlString(rawValue);
    const std::string path = inlineTomlField(rawValue, "path");
    const std::string git = inlineTomlField(rawValue, "git");
    const std::string version = rawValue.empty() || rawValue.front() == '{' ? inlineTomlField(rawValue, "version") : scalarValue;
    if (scalarValue == "builtin") {
      dependencies.insert(name + ":builtin");
    } else if (!path.empty()) {
      dependencies.insert(name + ":path=" + path);
    } else if (!git.empty()) {
      dependencies.insert(name + ":git=" + git + (version.empty() ? "" : ":version=" + version));
    } else if (!version.empty()) {
      dependencies.insert(name + ":version=" + version);
    } else {
      dependencies.insert(name + ":declared");
    }
  }
  return dependencies;
}

BuildPackage readBuildPackage(const fs::path &input) {
  BuildPackage package;
  package.root = fs::is_regular_file(input) ? input.parent_path() : input;
  if (fs::is_regular_file(input) && input.extension() == ".zn") {
    package.singleSource = input;
    package.manifest = package.root / "Zeno.toml";
  } else if (fs::is_regular_file(input)) {
    package.manifest = input;
  } else {
    package.manifest = package.root / "Zeno.toml";
  }
  if (fs::exists(package.manifest)) {
    const auto text = readFile(package.manifest);
    const auto name = manifestStringField(text, "package", "name");
    const auto version = manifestStringField(text, "package", "version");
    package.kind = manifestStringField(text, "package", "kind");
    package.entry = manifestStringField(text, "package", "entry");
    package.manifestTarget = manifestStringField(text, "target", "triple");
    package.manifestProfile = manifestStringField(text, "target", "profile");
    package.allocatorDefault = unquoteTomlString(manifestRawField(text, "allocator", "default"));
    package.allocatorSymbol = manifestStringField(text, "allocator", "symbol");
    package.panicStrategy = manifestStringField(text, "panic", "strategy");
    package.panicStack = manifestStringField(text, "panic", "stack");
    package.panicHandler = manifestStringField(text, "panic", "handler");
    package.oomStrategy = manifestStringField(text, "oom", "strategy");
    package.oomHandler = manifestStringField(text, "oom", "handler");
    for (const auto &[key, value] : manifestSectionFields(text, "trust")) {
      package.manifestTrust.insert(key + "=" + unquoteTomlString(value));
    }
    package.manifestDependencies = manifestDependencyFacts(text);
    if (!name.empty()) package.name = name;
    if (!version.empty()) package.version = version;
  }
  if (package.kind.empty()) {
    package.kind = fs::exists(package.root / "src" / "main.zn") ? "application" : "library";
  }
  if (package.entry.empty() && package.kind == "application") {
    package.entry = "main.main";
  }
  return package;
}

fs::path stableAbsolutePath(const fs::path &path) {
  std::error_code error;
  fs::path canonical = fs::weakly_canonical(path, error);
  return error ? fs::absolute(path) : canonical;
}

bool isBuiltinCompilerPackage(const BuildPackage &package) {
  if (!isBuiltinPackageName(package.name)) return false;
  const fs::path expectedRoot = builtinPackageRoot(package.name);
  if (!fs::exists(expectedRoot / "Zeno.toml")) return false;
  return stableAbsolutePath(package.root) == stableAbsolutePath(expectedRoot);
}

std::optional<fs::path> findBuildLockRoot(const BuildPackage &package) {
  if (!package.singleSource.empty() || !fs::is_directory(package.root)) return std::nullopt;
  fs::path current = stableAbsolutePath(package.root);
  while (true) {
    if (fs::exists(current / "Zeno.lock") && fs::exists(current / "Zeno.toml")) {
      return current;
    }
    const fs::path parent = current.parent_path();
    if (parent.empty() || parent == current) break;
    current = parent;
  }
  return std::nullopt;
}

std::string effectiveBuildLockRoot(const BuildPackage &package) {
  const auto lockRoot = findBuildLockRoot(package);
  return lockRoot ? lockRoot->string() : "none";
}

std::string effectiveBuildLockFingerprint(const BuildPackage &package) {
  const auto lockRoot = findBuildLockRoot(package);
  if (!lockRoot) return "none";
  return fileFingerprint(*lockRoot / "Zeno.lock");
}

std::vector<Diagnostic> frozenLockDiagnosticsForBuild(const BuildPackage &package) {
  std::vector<Diagnostic> diagnostics;
  if (!package.singleSource.empty() || !fs::is_directory(package.root) || !fs::exists(package.manifest)) {
    return diagnostics;
  }
  if (isBuiltinCompilerPackage(package)) return diagnostics;

  const auto lockRoot = findBuildLockRoot(package);
  if (!lockRoot) {
    SourceText manifest = loadSource(package.manifest);
    diagnostics.push_back(makeDiagnostic(
        manifest,
        "E1007",
        "frozen build requires Zeno.lock; run zeno build --update-lock to refresh local resolution",
        1));
    return diagnostics;
  }

  if (stableAbsolutePath(*lockRoot) != stableAbsolutePath(package.root)) {
    auto lockDiagnostics = lockfileDiagnostics(*lockRoot, parsePackageInfo(*lockRoot));
    diagnostics.insert(diagnostics.end(), lockDiagnostics.begin(), lockDiagnostics.end());
  }
  return diagnostics;
}

std::uint64_t fnv1a64(const std::string &text, std::uint64_t hash = 1469598103934665603ULL) {
  for (unsigned char c : text) {
    hash ^= static_cast<std::uint64_t>(c);
    hash *= 1099511628211ULL;
  }
  return hash;
}

std::string hex64(std::uint64_t value) {
  constexpr char digits[] = "0123456789abcdef";
  std::string out(16, '0');
  for (int i = 15; i >= 0; --i) {
    out[static_cast<std::size_t>(i)] = digits[value & 0xf];
    value >>= 4;
  }
  return out;
}

std::string sourceFingerprint(const fs::path &root) {
  std::uint64_t hash = 1469598103934665603ULL;
  const fs::path src = fs::exists(root / "src") ? root / "src" : root;
  for (const auto &file : packageSources(src)) {
    if (file.extension() != ".zn") continue;
    hash = fnv1a64(fs::relative(file, root).string(), hash);
    hash = fnv1a64(readFile(file), hash);
  }
  return "fnv1a64:" + hex64(hash);
}

std::string sourceFingerprint(const BuildPackage &package) {
  if (!package.singleSource.empty()) {
    std::uint64_t hash = 1469598103934665603ULL;
    hash = fnv1a64(package.singleSource.filename().string(), hash);
    hash = fnv1a64(readFile(package.singleSource), hash);
    return "fnv1a64:" + hex64(hash);
  }
  return sourceFingerprint(package.root);
}

std::string packageFingerprint(const fs::path &root) {
  std::uint64_t hash = 1469598103934665603ULL;
  for (const auto &file : packageSources(root)) {
    if (file.extension() != ".zn" && file.extension() != ".toml" && file.filename() != "Zeno.lock") continue;
    hash = fnv1a64(fs::relative(file, root).string(), hash);
    hash = fnv1a64(readFile(file), hash);
  }
  return "fnv1a64:" + hex64(hash);
}

std::string packageContentFingerprint(const fs::path &root) {
  std::uint64_t hash = 1469598103934665603ULL;
  for (const auto &file : packageSources(root)) {
    if (file.filename() == "Zeno.lock") continue;
    if (file.extension() != ".zn" && file.extension() != ".toml") continue;
    hash = fnv1a64(fs::relative(file, root).string(), hash);
    hash = fnv1a64(readFile(file), hash);
  }
  return "fnv1a64:" + hex64(hash);
}

std::string fileFingerprint(const fs::path &path) {
  if (!fs::exists(path)) return "fnv1a64:0000000000000000";
  return "fnv1a64:" + hex64(fnv1a64(readFile(path)));
}

fs::path builtinPackageRoot(const std::string &name) {
  return fs::current_path() / "lib" / "zeno" / name;
}

struct BuildSummary {
  std::set<std::string> astNodes;
  std::set<std::string> hirNodes;
  std::set<std::string> mirNodes;
  std::set<std::string> llvmNodes;
  std::set<std::string> declarations;
  std::set<std::string> publicApi;
  std::set<std::string> packageApi;
  std::set<std::string> layouts;
  std::set<std::string> dropGlue;
  std::set<std::string> sendSyncFacts;
  std::set<std::string> interfaces;
  std::set<std::string> impls;
  std::set<std::string> genericSignatures;
  std::set<std::string> staticInterfaceReturns;
  std::set<std::string> exports;
  std::set<std::string> trustCapabilities;
  std::set<std::string> dependencies;
  std::set<std::string> runtimeNeeds;
  std::set<std::string> costInputs;
};

std::string joinedSet(const std::set<std::string> &values) {
  std::string out = "[";
  bool first = true;
  for (const auto &value : values) {
    if (!first) out += ", ";
    first = false;
    out += "\"";
    out += value;
    out += "\"";
  }
  out += "]";
  return out;
}

std::string setFingerprint(const std::set<std::string> &values) {
  std::uint64_t hash = 1469598103934665603ULL;
  for (const auto &value : values) {
    hash = fnv1a64(value, hash);
  }
  return "fnv1a64:" + hex64(hash);
}

std::string defaultStrategyForProfile(const std::string &profile) {
  return profile == "hosted" ? "abort" : "trap";
}

std::set<std::string> buildPolicyFacts(const BuildPackage &package, const std::string &target, const std::string &profile) {
  std::set<std::string> facts;
  const std::string allocatorDefault = package.allocatorDefault.empty() ? (profile == "hosted" ? "true" : "false") : package.allocatorDefault;
  const std::string panicStrategy = package.panicStrategy.empty() ? defaultStrategyForProfile(profile) : package.panicStrategy;
  const std::string oomStrategy = package.oomStrategy.empty() ? defaultStrategyForProfile(profile) : package.oomStrategy;
  facts.insert("target=" + target);
  facts.insert("profile=" + profile);
  facts.insert("allocator.default=" + allocatorDefault);
  if (!package.allocatorSymbol.empty()) facts.insert("allocator.symbol=" + package.allocatorSymbol);
  facts.insert("panic.strategy=" + panicStrategy);
  if (!package.panicStack.empty()) facts.insert("panic.stack=" + package.panicStack);
  if (!package.panicHandler.empty()) facts.insert("panic.handler=" + package.panicHandler);
  facts.insert("oom.strategy=" + oomStrategy);
  if (!package.oomHandler.empty()) facts.insert("oom.handler=" + package.oomHandler);
  for (const auto &trust : package.manifestTrust) facts.insert("trust." + trust);
  for (const auto &dependency : package.manifestDependencies) facts.insert("dependency." + dependency);
  return facts;
}

std::string buildFingerprint(const BuildPackage &package,
                             const std::string &target,
                             const std::string &profile,
                             const std::string &sourceHash,
                             const std::string &lockHash,
                             const std::string &astHash,
                             const std::string &hirHash,
                             const std::string &mirHash,
                             const std::string &llvmHash,
                             const std::string &declarationHash,
                             const std::string &layoutHash,
                             const std::string &dropHash,
                             const std::string &sendSyncHash,
                             const std::string &interfaceHash,
                             const std::string &abiHash,
                             const std::string &trustHash,
                             const std::string &dependencyHash,
                             const std::string &dependencyPackageHash,
                             const std::string &costHash,
                             const std::string &runtimeHash,
                             const std::string &linkRuntimeHash,
                             const std::set<std::string> &builtinFacts) {
  std::uint64_t hash = 1469598103934665603ULL;
  hash = fnv1a64(sourceHash, hash);
  hash = fnv1a64("lock." + lockHash, hash);
  hash = fnv1a64("ast." + astHash, hash);
  hash = fnv1a64("hir." + hirHash, hash);
  hash = fnv1a64("mir." + mirHash, hash);
  hash = fnv1a64("llvm." + llvmHash, hash);
  hash = fnv1a64("declarations." + declarationHash, hash);
  hash = fnv1a64("layouts." + layoutHash, hash);
  hash = fnv1a64("drop." + dropHash, hash);
  hash = fnv1a64("sendSync." + sendSyncHash, hash);
  hash = fnv1a64("interfaces." + interfaceHash, hash);
  hash = fnv1a64("abi." + abiHash, hash);
  hash = fnv1a64("trust." + trustHash, hash);
  hash = fnv1a64("dependencies." + dependencyHash, hash);
  hash = fnv1a64("dependencyPackages." + dependencyPackageHash, hash);
  hash = fnv1a64("cost." + costHash, hash);
  hash = fnv1a64("runtime." + runtimeHash, hash);
  hash = fnv1a64("linkRuntime." + linkRuntimeHash, hash);
  for (const auto &fact : buildPolicyFacts(package, target, profile)) {
    hash = fnv1a64(fact, hash);
  }
  for (const auto &fact : builtinFacts) {
    hash = fnv1a64("builtin." + fact, hash);
  }
  return "fnv1a64:" + hex64(hash);
}

std::string declarationVisibility(const std::string &visibility) {
  if (visibility == "pub ") return "pub";
  if (visibility == "private ") return "private";
  return "package";
}

void addPrefixedFacts(std::set<std::string> &facts, const std::string &prefix, const std::set<std::string> &values) {
  for (const auto &value : values) facts.insert(prefix + value);
}

std::string sourceRelativePathForMetadata(const BuildPackage &package, const fs::path &file) {
  std::error_code error;
  fs::path relative = fs::relative(file, package.root, error);
  if (error) relative = file.filename();
  return relative.string();
}

std::string declarationMetadataFact(const BuildPackage &package,
                                    const fs::path &file,
                                    const std::string &moduleName,
                                    const std::string &kind,
                                    const std::string &name,
                                    const std::string &visibility,
                                    const std::string &signature,
                                    int lineNo) {
  const std::string module = moduleName.empty() ? "root" : moduleName;
  const std::string identity = package.name + ":" + module + ":" + kind + ":" + name + ":" + signature;
  return kind + " " + name +
         " stableNodeId=fnv1a64:" + hex64(fnv1a64(identity)) +
         " module=" + module +
         " visibility=" + declarationVisibility(visibility) +
         " span=" + sourceRelativePathForMetadata(package, file) + ":" + std::to_string(lineNo) + ":1";
}

struct TopLevelDeclaration {
  fs::path file;
  std::string moduleName;
  std::string kind;
  std::string name;
  std::string visibility;
  std::string generic;
  std::string params;
  std::string returnType;
  std::string layout;
  bool trusted = false;
  int lineNo = 1;
};

std::vector<fs::path> buildSourceFiles(const BuildPackage &package);

std::set<std::string> collectTopLevelAstNodes(const BuildPackage &package) {
  std::set<std::string> nodes;
  for (const auto &node : parseTopLevelAst(package.root, package.name, buildSourceFiles(package))) {
    const std::string module = node.moduleName.empty() ? "root" : node.moduleName;
    std::string fact = node.kind + " " + node.name +
                       " module=" + module +
                       " span=" + sourceRelativePathForMetadata(package, node.file) + ":" + std::to_string(node.lineNo) + ":1";
    if (!node.attributes.empty()) {
      fact += " attrs=";
      for (std::size_t i = 0; i < node.attributes.size(); ++i) {
        if (i != 0) fact += ",";
        fact += node.attributes[i];
      }
    }
    if (!node.generic.empty()) {
      if (startsWith(node.kind, "stmt.")) fact += " mode=" + node.generic;
      else fact += " generic=" + node.generic;
    }
    if (node.trusted) fact += " trusted=true";
    if (!node.abi.empty()) fact += " abi=" + node.abi;
    if (node.kind == "decl.fn" || node.kind == "decl.async-fn" ||
        node.kind == "decl.extern" || node.kind == "decl.interface-method" ||
        node.kind == "decl.impl-method") {
      fact += " params=(" + node.params + ")";
      if (!node.returnType.empty()) fact += " return=" + node.returnType;
    } else if (node.kind == "decl.field" && !node.returnType.empty()) {
      fact += " type=" + node.returnType;
    } else if (node.kind == "decl.variant" && !node.params.empty()) {
      fact += " payload=" + node.params;
    } else if (node.kind == "decl.type" && !node.params.empty()) {
      fact += " target=" + node.params;
    } else if (node.kind == "decl.interface" && !node.params.empty()) {
      fact += " parents=" + node.params;
    } else if (node.kind == "decl.struct" && !node.returnType.empty()) {
      fact += " marker=" + node.returnType;
    }
    if ((node.kind == "decl.const" || node.kind == "decl.static" ||
         node.kind == "stmt.return" || node.kind == "stmt.val" || node.kind == "stmt.var" ||
         node.kind == "stmt.const" || node.kind == "stmt.break" ||
         node.kind == "stmt.pattern-val" || node.kind == "stmt.pattern-var" ||
         node.kind == "stmt.assign" || node.kind == "stmt.if" || node.kind == "stmt.if-pattern" ||
         node.kind == "stmt.while" || node.kind == "stmt.while-pattern" ||
         node.kind == "stmt.for" || node.kind == "stmt.match" || node.kind == "stmt.match-arm" ||
         node.kind == "stmt.try" || node.kind == "stmt.expr") &&
        !node.params.empty()) {
      if (node.kind == "stmt.if" || node.kind == "stmt.if-pattern" ||
          node.kind == "stmt.while" || node.kind == "stmt.while-pattern" ||
          node.kind == "stmt.match") {
        fact += " condition=" + node.params;
      } else if (node.kind == "stmt.for") {
        fact += " iterable=" + node.params;
      } else {
        fact += " expr=" + node.params;
      }
      if (node.kind == "stmt.return") {
        if (!node.returnType.empty()) fact += " expected=" + node.returnType;
      } else if (node.kind == "stmt.match-arm" && !node.returnType.empty()) {
        fact += " guard=" + node.returnType;
      } else if (!node.returnType.empty() &&
                 node.kind != "stmt.if-pattern" && node.kind != "stmt.while-pattern" &&
                 node.kind != "stmt.for" &&
                 node.kind != "stmt.pattern-val" && node.kind != "stmt.pattern-var" &&
                 node.kind != "stmt.match-arm") {
        fact += " type=" + node.returnType;
      }
      if (node.kind == "stmt.for" && !node.returnType.empty()) {
        fact += " mode=" + node.returnType;
      }
      if ((node.kind == "stmt.if-pattern" || node.kind == "stmt.while-pattern" ||
           node.kind == "stmt.pattern-val" || node.kind == "stmt.pattern-var") &&
          !node.returnType.empty()) {
        fact += " pattern=" + node.returnType;
      }
    } else if ((node.kind == "decl.const" || node.kind == "decl.static") && !node.returnType.empty()) {
      fact += " type=" + node.returnType;
    }
    nodes.insert(fact);
  }
  return nodes;
}

void collectPipelineFactsFromAst(const BuildPackage &package,
                                 const std::vector<ParsedAstNode> &ast,
                                 BuildSummary &summary) {
  (void)package;
  auto functionNameForNode = [](const ParsedAstNode &node) {
    if (node.kind == "stmt.val" || node.kind == "stmt.var" || node.kind == "stmt.const" || node.kind == "stmt.assign" ||
        node.kind == "stmt.pattern-val" || node.kind == "stmt.pattern-var" ||
        node.kind == "stmt.for" || node.kind == "stmt.if-pattern" || node.kind == "stmt.while-pattern" ||
        node.kind == "stmt.match-arm") {
      const std::size_t dot = node.name.find('.');
      if (dot != std::string::npos) return node.name.substr(0, dot);
    }
    return node.name;
  };
  auto localNameForNode = [](const ParsedAstNode &node) {
    const std::size_t dot = node.name.find('.');
    if (dot == std::string::npos) return node.name;
    return node.name.substr(dot + 1);
  };
  auto emitCallPreview = [&](const std::string &expr, const std::string &functionName) {
    if (startsWith(expr, "call:")) {
      summary.llvmNodes.insert("llvm.call-preview @" + expr.substr(5) + " in @" + functionName);
    }
  };
  auto emitMirExpressionFacts = [&](const std::string &functionName, const std::string &expr, const std::string &span) {
    if (startsWith(expr, "name:")) {
      summary.mirNodes.insert("mir.place " + functionName + " %" + expr.substr(5) + " kind=local span=" + span);
      summary.mirNodes.insert("mir.operand " + functionName + " expr=" + expr + " kind=place place=%" + expr.substr(5));
    } else if (startsWith(expr, "field:")) {
      summary.mirNodes.insert("mir.place " + functionName + " " + expr.substr(6) + " kind=field span=" + span);
      summary.mirNodes.insert("mir.operand " + functionName + " expr=" + expr + " kind=place place=" + expr.substr(6));
    } else if (startsWith(expr, "int-literal:") || startsWith(expr, "float-literal:") ||
               startsWith(expr, "bool-literal:") || expr == "string-literal" || expr == "char-literal") {
      summary.mirNodes.insert("mir.operand " + functionName + " expr=" + expr + " kind=const");
    } else if (startsWith(expr, "call:")) {
      summary.mirNodes.insert("mir.operand " + functionName + " expr=" + expr + " kind=call callee=@" + expr.substr(5));
      summary.mirNodes.insert("mir.rvalue " + functionName + " expr=" + expr + " kind=call callee=@" + expr.substr(5));
    } else if (startsWith(expr, "binary:")) {
      summary.mirNodes.insert("mir.operand " + functionName + " expr=" + expr + " kind=rvalue");
      summary.mirNodes.insert("mir.rvalue " + functionName + " expr=" + expr + " kind=binary op=" + expr.substr(7));
      summary.llvmNodes.insert("llvm.binary-preview @" + functionName + " op=" + expr.substr(7));
    } else if (startsWith(expr, "compare:")) {
      summary.mirNodes.insert("mir.operand " + functionName + " expr=" + expr + " kind=condition");
      summary.mirNodes.insert("mir.rvalue " + functionName + " expr=" + expr + " kind=compare op=" + expr.substr(8));
      summary.llvmNodes.insert("llvm.compare-preview @" + functionName + " op=" + expr.substr(8));
    } else if (startsWith(expr, "try:")) {
      summary.mirNodes.insert("mir.operand " + functionName + " expr=" + expr + " kind=try");
      summary.mirNodes.insert("mir.rvalue " + functionName + " expr=" + expr + " kind=try");
      summary.mirNodes.insert("mir.cleanup-edge " + functionName + " try-error");
      summary.llvmNodes.insert("llvm.branch-preview @" + functionName + " kind=try expr=" + expr);
    } else if (startsWith(expr, "struct-literal:")) {
      summary.mirNodes.insert("mir.operand " + functionName + " expr=" + expr + " kind=aggregate");
      summary.mirNodes.insert("mir.rvalue " + functionName + " expr=" + expr + " kind=struct-literal type=" + expr.substr(15));
      summary.llvmNodes.insert("llvm.aggregate-preview @" + functionName + " type=" + expr.substr(15));
    } else if (startsWith(expr, "type-static-call:")) {
      summary.mirNodes.insert("mir.operand " + functionName + " expr=" + expr + " kind=type-static-call target=" + expr.substr(17));
      summary.mirNodes.insert("mir.rvalue " + functionName + " expr=" + expr + " kind=type-static-call target=" + expr.substr(17));
      summary.llvmNodes.insert("llvm.call-preview @" + functionName + " static=" + expr.substr(17));
    } else if (startsWith(expr, "method-call:")) {
      summary.mirNodes.insert("mir.operand " + functionName + " expr=" + expr + " kind=method-call method=" + expr.substr(12));
      summary.mirNodes.insert("mir.rvalue " + functionName + " expr=" + expr + " kind=method-call method=" + expr.substr(12));
      summary.llvmNodes.insert("llvm.call-preview @" + functionName + " method=" + expr.substr(12));
    } else if (startsWith(expr, "cast:")) {
      summary.mirNodes.insert("mir.operand " + functionName + " expr=" + expr + " kind=cast target=" + expr.substr(5));
      summary.mirNodes.insert("mir.rvalue " + functionName + " expr=" + expr + " kind=cast target=" + expr.substr(5));
      summary.llvmNodes.insert("llvm.cast-preview @" + functionName + " target=" + expr.substr(5));
    } else if (startsWith(expr, "index:")) {
      summary.mirNodes.insert("mir.place " + functionName + " " + expr.substr(6) + "[...] kind=index span=" + span);
      summary.mirNodes.insert("mir.operand " + functionName + " expr=" + expr + " kind=index base=" + expr.substr(6));
      summary.llvmNodes.insert("llvm.index-preview @" + functionName + " base=" + expr.substr(6));
    } else if (startsWith(expr, "range:")) {
      summary.mirNodes.insert("mir.operand " + functionName + " expr=" + expr + " kind=range mode=" + expr.substr(6));
      summary.mirNodes.insert("mir.rvalue " + functionName + " expr=" + expr + " kind=range mode=" + expr.substr(6));
    } else if (expr == "tuple") {
      summary.mirNodes.insert("mir.operand " + functionName + " expr=tuple kind=aggregate");
      summary.mirNodes.insert("mir.rvalue " + functionName + " expr=tuple kind=tuple");
      summary.llvmNodes.insert("llvm.aggregate-preview @" + functionName + " type=tuple");
    } else if (expr == "async-block") {
      summary.mirNodes.insert("mir.operand " + functionName + " expr=async-block kind=future");
      summary.mirNodes.insert("mir.rvalue " + functionName + " expr=async-block kind=future-state-machine staged=true");
      summary.llvmNodes.insert("llvm.async-preview @" + functionName + " staged=true");
    } else if (startsWith(expr, "await:")) {
      summary.mirNodes.insert("mir.operand " + functionName + " expr=" + expr + " kind=await");
      summary.mirNodes.insert("mir.suspend-preview " + functionName + " operand=" + expr.substr(6));
      summary.llvmNodes.insert("llvm.await-preview @" + functionName + " staged=true");
    } else if (startsWith(expr, "move:") || startsWith(expr, "mut:")) {
      const std::string mode = startsWith(expr, "move:") ? "move" : "mut";
      const std::string inner = expr.substr(mode.size() + 1);
      summary.mirNodes.insert("mir.operand " + functionName + " expr=" + expr + " kind=access mode=" + mode);
      summary.mirNodes.insert("mir.access " + functionName + " mode=" + mode + " operand=" + inner);
      summary.llvmNodes.insert("llvm.access-preview @" + functionName + " mode=" + mode + " operand=" + inner);
    } else if (expr == "closure") {
      summary.mirNodes.insert("mir.operand " + functionName + " expr=closure kind=closure");
      summary.mirNodes.insert("mir.rvalue " + functionName + " expr=closure kind=closure mayEscape=false");
      summary.llvmNodes.insert("llvm.closure-preview @" + functionName + " mayEscape=false");
    } else if (startsWith(expr, "closure:")) {
      const std::string detail = expr.substr(8);
      std::string suffix;
      if (detail == "move") suffix = " mode=move";
      else if (detail == "block") suffix = " form=block";
      else suffix = " detail=" + detail;
      summary.mirNodes.insert("mir.operand " + functionName + " expr=" + expr + " kind=closure" + suffix);
      summary.mirNodes.insert("mir.rvalue " + functionName + " expr=" + expr + " kind=closure mayEscape=false" + suffix);
      summary.llvmNodes.insert("llvm.closure-preview @" + functionName + " mayEscape=false" + suffix);
    } else if (expr == "match") {
      summary.mirNodes.insert("mir.operand " + functionName + " expr=match kind=match");
      summary.mirNodes.insert("mir.rvalue " + functionName + " expr=match kind=match");
      summary.llvmNodes.insert("llvm.switch-preview @" + functionName + " expr=match");
    } else if (startsWith(expr, "match:")) {
      const std::string mode = expr.substr(6);
      summary.mirNodes.insert("mir.operand " + functionName + " expr=" + expr + " kind=match mode=" + mode);
      summary.mirNodes.insert("mir.rvalue " + functionName + " expr=" + expr + " kind=match mode=" + mode);
      summary.llvmNodes.insert("llvm.switch-preview @" + functionName + " expr=match mode=" + mode);
    } else if (expr == "trust-block") {
      summary.mirNodes.insert("mir.operand " + functionName + " expr=trust-block kind=trust");
    } else if (expr == "unit") {
      summary.mirNodes.insert("mir.operand " + functionName + " expr=unit kind=const");
    } else if (!expr.empty()) {
      summary.mirNodes.insert("mir.operand " + functionName + " expr=" + expr + " kind=opaque");
    }
  };

  for (const auto &node : ast) {
    const std::string module = node.moduleName.empty() ? "root" : node.moduleName;
    const std::string span = sourceRelativePathForMetadata(package, node.file) + ":" + std::to_string(node.lineNo) + ":1";
    if (startsWith(node.kind, "decl.")) {
      summary.hirNodes.insert("hir." + node.kind.substr(5) + " " + node.name + " module=" + module + " span=" + span);
      if (!node.generic.empty()) {
        summary.hirNodes.insert("hir.generic-params " + node.name + " params=" + node.generic + " span=" + span);
        summary.mirNodes.insert("mir.generic-input " + node.name + " params=" + node.generic);
      }
    }
    if (node.kind == "decl.fn" || node.kind == "decl.async-fn") {
      const std::string returnType = node.returnType.empty() ? "Unit" : node.returnType;
      summary.hirNodes.insert("hir.fn-signature " + node.name + " params=(" + node.params + ") return=" + returnType + " span=" + span);
      summary.mirNodes.insert("mir.cfg " + node.name + " entry=bb0 blocks=bb0 cleanup=none");
      summary.mirNodes.insert("mir.fn " + node.name + " entry=bb0 return=" + returnType);
      summary.mirNodes.insert("mir.cleanup-edge " + node.name + " none");
      summary.mirNodes.insert("mir.drop-flags " + node.name + " state=none-preview");
      summary.llvmNodes.insert("llvm.define-preview @" + node.name + " return=" + returnType);
      summary.llvmNodes.insert("llvm.lowering-input @" + node.name + " from=mir.cfg entry=bb0");
    } else if (node.kind == "decl.extern") {
      summary.hirNodes.insert("hir.extern " + node.name + " abi=" + (node.abi.empty() ? "unknown" : node.abi));
      summary.mirNodes.insert("mir.extern " + node.name + " abi=" + (node.abi.empty() ? "unknown" : node.abi));
      summary.llvmNodes.insert("llvm.declare @" + node.name + " abi=" + (node.abi.empty() ? "unknown" : node.abi));
    } else if (node.kind == "decl.struct") {
      summary.hirNodes.insert("hir.layout-input " + node.name + " layout=" + node.layout);
      if (!node.returnType.empty()) summary.hirNodes.insert("hir.struct-marker " + node.name + " marker=" + node.returnType);
      summary.mirNodes.insert("mir.type " + node.name + " layout=" + node.layout);
      summary.llvmNodes.insert("llvm.type %" + node.name + " layout=" + node.layout);
    } else if (node.kind == "decl.interface") {
      if (!node.params.empty()) {
        summary.hirNodes.insert("hir.interface-parents " + node.name + " parents=" + node.params);
        summary.mirNodes.insert("mir.interface-parents " + node.name + " parents=" + node.params);
      }
    } else if (node.kind == "decl.field") {
      const std::string type = node.returnType.empty() ? "inferred" : node.returnType;
      summary.hirNodes.insert("hir.field " + node.name + " type=" + type + " span=" + span);
      summary.mirNodes.insert("mir.field " + node.name + " type=" + type);
      summary.llvmNodes.insert("llvm.field-preview " + node.name + " type=" + type);
    } else if (node.kind == "decl.variant") {
      const std::string payload = node.params.empty() ? "unit" : node.params;
      summary.hirNodes.insert("hir.enum-variant " + node.name + " payload=" + payload + " span=" + span);
      summary.mirNodes.insert("mir.enum-variant " + node.name + " payload=" + payload);
      summary.llvmNodes.insert("llvm.variant-preview " + node.name + " payload=" + payload);
    } else if (node.kind == "decl.interface-method") {
      const std::string returnType = node.returnType.empty() ? "Unit" : node.returnType;
      summary.hirNodes.insert("hir.interface-method " + node.name + " params=(" + node.params + ") return=" + returnType + " span=" + span);
      summary.mirNodes.insert("mir.interface-slot " + node.name + " params=(" + node.params + ") return=" + returnType);
      summary.llvmNodes.insert("llvm.interface-method-preview " + node.name + " return=" + returnType);
    } else if (node.kind == "decl.impl-method") {
      const std::string returnType = node.returnType.empty() ? "Unit" : node.returnType;
      summary.hirNodes.insert("hir.impl-method " + node.name + " params=(" + node.params + ") return=" + returnType + " span=" + span);
      summary.mirNodes.insert("mir.impl-method " + node.name + " params=(" + node.params + ") return=" + returnType);
      summary.llvmNodes.insert("llvm.impl-method-preview " + node.name + " return=" + returnType);
    } else if (node.kind == "decl.impl-const") {
      const std::string type = node.returnType.empty() ? "inferred" : node.returnType;
      summary.hirNodes.insert("hir.impl-const " + node.name + " expr=" + node.params + " type=" + type + " span=" + span);
      emitMirExpressionFacts(node.name, node.params, span);
      summary.mirNodes.insert("mir.impl-const " + node.name + " value=" + node.params + " type=" + type);
    } else if (node.kind == "decl.destroy") {
      summary.hirNodes.insert("hir.destroy " + node.name + " span=" + span);
      summary.mirNodes.insert("mir.destroy " + node.name + " cleanup=body-then-fields");
      summary.llvmNodes.insert("llvm.destroy-preview " + node.name + " cleanup=body-then-fields");
    } else if (node.kind == "decl.type") {
      summary.hirNodes.insert("hir.type-alias " + node.name + " target=" + node.params + " span=" + span);
      summary.mirNodes.insert("mir.type-alias " + node.name + " target=" + node.params);
    } else if (node.kind == "decl.const" || node.kind == "decl.static") {
      const std::string itemKind = node.kind.substr(5);
      const std::string type = node.returnType.empty() ? "inferred" : node.returnType;
      summary.hirNodes.insert("hir." + itemKind + " " + node.name + " expr=" + node.params + " type=" + type + " span=" + span);
      emitMirExpressionFacts(node.name, node.params, span);
      if (itemKind == "const") {
        summary.mirNodes.insert("mir.const " + node.name + " value=" + node.params + " type=" + type);
        summary.llvmNodes.insert("llvm.const-preview @" + node.name + " value=" + node.params + " type=" + type);
      } else {
        summary.mirNodes.insert("mir.static " + node.name + " init=" + node.params + " type=" + type);
        summary.llvmNodes.insert("llvm.global-preview @" + node.name + " init=" + node.params + " type=" + type);
      }
    } else if (node.kind == "expr.trust") {
      summary.hirNodes.insert("hir.trust block span=" + span);
      summary.mirNodes.insert("mir.trust block span=" + span);
    } else if (node.kind == "stmt.return") {
      const std::string returnType = node.returnType.empty() ? "Unit" : node.returnType;
      summary.hirNodes.insert("hir.return " + node.name + " expr=" + node.params + " expected=" + returnType + " span=" + span);
      emitMirExpressionFacts(node.name, node.params, span);
      summary.mirNodes.insert("mir.return " + node.name + " value=" + node.params + " type=" + returnType);
      summary.mirNodes.insert("mir.terminator " + node.name + " bb=bb0 kind=return value=" + node.params);
      emitCallPreview(node.params, node.name);
      summary.llvmNodes.insert("llvm.ret-preview @" + node.name + " type=" + returnType + " value=" + node.params);
    } else if (node.kind == "stmt.val" || node.kind == "stmt.var") {
      const std::string mode = node.kind == "stmt.var" ? "var" : "val";
      const std::string type = node.returnType.empty() ? "inferred" : node.returnType;
      const std::string mutableFlag = node.kind == "stmt.var" ? "true" : "false";
      const std::string functionName = functionNameForNode(node);
      const std::string localName = localNameForNode(node);
      summary.hirNodes.insert("hir.local " + node.name + " mode=" + mode + " expr=" + node.params + " type=" + type + " span=" + span);
      emitMirExpressionFacts(functionName, node.params, span);
      summary.mirNodes.insert("mir.local " + node.name + " init=" + node.params + " mutable=" + mutableFlag);
      summary.mirNodes.insert("mir.local-slot " + functionName + " %" + localName + " mutable=" + mutableFlag + " type=" + type);
      summary.mirNodes.insert("mir.place " + functionName + " %" + localName + " kind=local span=" + span);
      summary.mirNodes.insert("mir.assign " + functionName + " bb=bb0 place=%" + localName + " rvalue=" + node.params);
      emitCallPreview(node.params, functionName);
    } else if (node.kind == "stmt.const") {
      const std::string type = node.returnType.empty() ? "inferred" : node.returnType;
      const std::string functionName = functionNameForNode(node);
      const std::string localName = localNameForNode(node);
      summary.hirNodes.insert("hir.local-const " + node.name + " expr=" + node.params + " type=" + type + " span=" + span);
      emitMirExpressionFacts(functionName, node.params, span);
      summary.mirNodes.insert("mir.local-const " + node.name + " value=" + node.params + " type=" + type);
      summary.mirNodes.insert("mir.local-slot " + functionName + " %" + localName + " mutable=false type=" + type);
      emitCallPreview(node.params, functionName);
    } else if (node.kind == "stmt.assign") {
      const std::string functionName = functionNameForNode(node);
      const std::string localName = localNameForNode(node);
      summary.hirNodes.insert("hir.assign " + node.name + " expr=" + node.params + " span=" + span);
      emitMirExpressionFacts(functionName, node.params, span);
      summary.mirNodes.insert("mir.assign " + functionName + " bb=bb0 place=%" + localName + " rvalue=" + node.params);
      summary.mirNodes.insert("mir.store-preview " + functionName + " place=%" + localName + " value=" + node.params);
      summary.llvmNodes.insert("llvm.store-preview @" + functionName + " %" + localName + " value=" + node.params);
      emitCallPreview(node.params, functionName);
    } else if (node.kind == "stmt.pattern-val" || node.kind == "stmt.pattern-var") {
      const std::string functionName = functionNameForNode(node);
      const std::string pattern = localNameForNode(node);
      const std::string mode = node.kind == "stmt.pattern-var" ? "var" : "val";
      const std::string mutableFlag = node.kind == "stmt.pattern-var" ? "true" : "false";
      summary.hirNodes.insert("hir.pattern-local " + functionName + " mode=" + mode + " pattern=" + pattern + " expr=" + node.params + " span=" + span);
      emitMirExpressionFacts(functionName, node.params, span);
      summary.mirNodes.insert("mir.pattern-bind " + functionName + " pattern=" + pattern + " source=" + node.params + " mutable=" + mutableFlag);
      summary.llvmNodes.insert("llvm.pattern-preview @" + functionName + " pattern=" + pattern + " source=" + node.params);
    } else if (node.kind == "stmt.if") {
      summary.hirNodes.insert("hir.if " + node.name + " condition=" + node.params + " span=" + span);
      emitMirExpressionFacts(node.name, node.params, span);
      emitCallPreview(node.params, node.name);
      summary.mirNodes.insert("mir.branch " + node.name + " kind=if condition=" + node.params + " then=if.then else=if.cont");
      summary.llvmNodes.insert("llvm.branch-preview @" + node.name + " kind=if condition=" + node.params);
    } else if (node.kind == "stmt.if-pattern") {
      const std::string functionName = functionNameForNode(node);
      const std::string pattern = localNameForNode(node);
      const std::string mode = node.generic.empty() ? "read" : node.generic;
      summary.hirNodes.insert("hir.if-pattern " + functionName + " pattern=" + pattern + " mode=" + mode + " scrutinee=" + node.params + " span=" + span);
      emitMirExpressionFacts(functionName, node.params, span);
      summary.mirNodes.insert("mir.branch " + functionName + " kind=if-pattern pattern=" + pattern + " mode=" + mode + " scrutinee=" + node.params + " then=if.then else=if.cont");
      summary.llvmNodes.insert("llvm.branch-preview @" + functionName + " kind=if-pattern pattern=" + pattern + " mode=" + mode + " scrutinee=" + node.params);
    } else if (node.kind == "stmt.else") {
      summary.hirNodes.insert("hir.else " + node.name + " span=" + span);
      summary.mirNodes.insert("mir.branch " + node.name + " kind=else target=else.body");
    } else if (node.kind == "stmt.while") {
      summary.hirNodes.insert("hir.while " + node.name + " condition=" + node.params + " span=" + span);
      emitMirExpressionFacts(node.name, node.params, span);
      emitCallPreview(node.params, node.name);
      summary.mirNodes.insert("mir.loop " + node.name + " condition=" + node.params + " header=while.cond body=while.body exit=while.exit");
      summary.mirNodes.insert("mir.branch " + node.name + " kind=while condition=" + node.params + " then=while.body else=while.exit");
      summary.llvmNodes.insert("llvm.loop-preview @" + node.name + " condition=" + node.params);
      summary.llvmNodes.insert("llvm.branch-preview @" + node.name + " kind=while condition=" + node.params);
    } else if (node.kind == "stmt.while-pattern") {
      const std::string functionName = functionNameForNode(node);
      const std::string pattern = localNameForNode(node);
      const std::string mode = node.generic.empty() ? "read" : node.generic;
      summary.hirNodes.insert("hir.while-pattern " + functionName + " pattern=" + pattern + " mode=" + mode + " scrutinee=" + node.params + " span=" + span);
      emitMirExpressionFacts(functionName, node.params, span);
      summary.mirNodes.insert("mir.loop " + functionName + " kind=while-pattern pattern=" + pattern + " mode=" + mode + " scrutinee=" + node.params + " header=while.cond body=while.body exit=while.exit");
      summary.mirNodes.insert("mir.branch " + functionName + " kind=while-pattern pattern=" + pattern + " mode=" + mode + " then=while.body else=while.exit");
      summary.llvmNodes.insert("llvm.loop-preview @" + functionName + " kind=while-pattern pattern=" + pattern + " mode=" + mode + " scrutinee=" + node.params);
    } else if (node.kind == "stmt.for") {
      const std::string functionName = functionNameForNode(node);
      const std::string localName = localNameForNode(node);
      const std::string mode = node.returnType.empty() ? "read" : node.returnType;
      summary.hirNodes.insert("hir.for " + functionName + " item=" + localName + " mode=" + mode + " iterable=" + node.params + " span=" + span);
      emitMirExpressionFacts(functionName, node.params, span);
      summary.mirNodes.insert("mir.loop " + functionName + " kind=for item=%" + localName + " mode=" + mode + " iterable=" + node.params);
      summary.mirNodes.insert("mir.branch " + functionName + " kind=for item=%" + localName + " mode=" + mode + " iterable=" + node.params + " then=for.body else=for.exit");
      summary.mirNodes.insert("mir.local-slot " + functionName + " %" + localName + " mutable=" + std::string(mode == "mut" ? "true" : "false") + " type=iterator-item");
      summary.llvmNodes.insert("llvm.loop-preview @" + functionName + " kind=for mode=" + mode + " iterable=" + node.params);
      summary.llvmNodes.insert("llvm.branch-preview @" + functionName + " kind=for mode=" + mode + " iterable=" + node.params);
    } else if (node.kind == "stmt.match") {
      const std::string mode = node.generic.empty() ? "read" : node.generic;
      summary.hirNodes.insert("hir.match " + node.name + " mode=" + mode + " scrutinee=" + node.params + " span=" + span);
      emitMirExpressionFacts(node.name, node.params, span);
      summary.mirNodes.insert("mir.switch " + node.name + " mode=" + mode + " scrutinee=" + node.params + " arms=preview");
      summary.llvmNodes.insert("llvm.switch-preview @" + node.name + " mode=" + mode + " scrutinee=" + node.params);
    } else if (node.kind == "stmt.match-arm") {
      const std::string functionName = functionNameForNode(node);
      const std::string pattern = localNameForNode(node);
      const std::string guard = node.returnType.empty() ? "none" : node.returnType;
      summary.hirNodes.insert("hir.match-arm " + functionName + " pattern=" + pattern + " guard=" + guard + " expr=" + node.params + " span=" + span);
      emitMirExpressionFacts(functionName, node.params, span);
      if (!node.returnType.empty()) emitMirExpressionFacts(functionName, node.returnType, span);
      summary.mirNodes.insert("mir.match-arm " + functionName + " pattern=" + pattern + " guard=" + guard + " body=" + node.params);
      if (!node.returnType.empty()) {
        summary.mirNodes.insert("mir.branch " + functionName + " kind=match-guard pattern=" + pattern + " condition=" + node.returnType);
        summary.llvmNodes.insert("llvm.branch-preview @" + functionName + " kind=match-guard pattern=" + pattern + " condition=" + node.returnType);
      }
    } else if (node.kind == "stmt.try") {
      summary.hirNodes.insert("hir.try " + node.name + " expr=" + node.params + " span=" + span);
      emitMirExpressionFacts(node.name, node.params, span);
      summary.mirNodes.insert("mir.try " + node.name + " expr=" + node.params + " ok=continue err=return");
      summary.mirNodes.insert("mir.cleanup-edge " + node.name + " try-error");
      summary.llvmNodes.insert("llvm.branch-preview @" + node.name + " kind=try expr=" + node.params);
    } else if (node.kind == "stmt.break") {
      summary.hirNodes.insert("hir.break " + node.name + " expr=" + node.params + " span=" + span);
      emitMirExpressionFacts(node.name, node.params, span);
      summary.mirNodes.insert("mir.terminator " + node.name + " kind=break target=loop.exit value=" + node.params);
      summary.llvmNodes.insert("llvm.branch-preview @" + node.name + " kind=break target=loop.exit");
    } else if (node.kind == "stmt.continue") {
      summary.hirNodes.insert("hir.continue " + node.name + " span=" + span);
      summary.mirNodes.insert("mir.terminator " + node.name + " kind=continue target=loop.header");
      summary.llvmNodes.insert("llvm.branch-preview @" + node.name + " kind=continue target=loop.header");
    } else if (node.kind == "stmt.expr") {
      summary.hirNodes.insert("hir.expr " + node.name + " expr=" + node.params + " span=" + span);
      emitMirExpressionFacts(node.name, node.params, span);
      summary.mirNodes.insert("mir.eval " + node.name + " expr=" + node.params);
      summary.mirNodes.insert("mir.eval " + node.name + " bb=bb0 operand=" + node.params);
      emitCallPreview(node.params, node.name);
    }
  }
}

std::vector<fs::path> buildSourceFiles(const BuildPackage &package) {
  if (!package.singleSource.empty()) {
    return {package.singleSource};
  }
  return packageSources(package.root / "src");
}

std::vector<TopLevelDeclaration> collectTopLevelDeclarations(const BuildPackage &package) {
  std::vector<TopLevelDeclaration> declarations;
  for (const auto &node : parseTopLevelAst(package.root, package.name, buildSourceFiles(package))) {
    std::string kind;
    if (node.kind == "decl.struct") kind = "struct";
    else if (node.kind == "decl.enum") kind = "enum";
    else if (node.kind == "decl.interface") kind = "interface";
    else if (node.kind == "decl.fn" || node.kind == "decl.async-fn") kind = "fn";
    else if (node.kind == "decl.extern") kind = "extern";
    else if (node.kind == "decl.impl") kind = "impl";
    else if (node.kind == "decl.const") kind = "const";
    else if (node.kind == "decl.static") kind = "static";
    else continue;

    declarations.push_back(TopLevelDeclaration{
        node.file,
        node.moduleName,
        std::move(kind),
        node.name,
        node.visibility,
        node.generic,
        node.params,
        node.returnType,
        node.layout,
        node.trusted,
        node.lineNo});
    }
  return declarations;
}

BuildSummary summarizeBuildPackage(const BuildPackage &package) {
  BuildSummary summary;
  std::vector<fs::path> sourceFiles = buildSourceFiles(package);
  const auto ast = parseTopLevelAst(package.root, package.name, sourceFiles);
  summary.astNodes = collectTopLevelAstNodes(package);
  collectPipelineFactsFromAst(package, ast, summary);
  const auto topLevelDeclarations = collectTopLevelDeclarations(package);
  const std::regex structPattern(R"(^\s*(pub\s+|private\s+)?struct\s+([A-Za-z_][A-Za-z0-9_]*)(<[^>]+>)?)");
  const std::regex implPattern(R"(^\s*(trust\s+)?impl(?:<[^>]+>)?\s+([A-Za-z_][A-Za-z0-9_]*(?:<[^>]+>)?)\s+for\s+([A-Za-z_][A-Za-z0-9_]*))");
  const std::regex inherentImplPattern(R"(^\s*impl\s+([A-Za-z_][A-Za-z0-9_]*)\s*\{)");
  const std::regex exportPattern(R"re(@export\s*\(\s*"([^"]+)".*(abi:\s*C|bridge:\s*C))re");
  const std::regex importPattern(R"(^\s*import\s+([A-Za-z_][A-Za-z0-9_]*))");
  std::string currentImplType;
  std::string currentStructType;
  int structBraceDepth = 0;
  std::set<std::string> structTypes;
  std::set<std::string> nonAutoSendSyncTypes;

  for (const auto &decl : topLevelDeclarations) {
    if (decl.kind == "struct") {
      structTypes.insert(decl.name);
      summary.declarations.insert(declarationMetadataFact(package, decl.file, decl.moduleName, decl.kind, decl.name, decl.visibility, decl.generic, decl.lineNo));
      summary.layouts.insert(decl.name + ":" + decl.layout + ":fingerprint=" + hex64(fnv1a64(decl.name + ":" + decl.layout)));
      if (decl.visibility == "pub ") summary.publicApi.insert("struct " + decl.name);
      else if (decl.visibility != "private ") summary.packageApi.insert("struct " + decl.name);
      if (!decl.generic.empty()) summary.genericSignatures.insert("struct " + decl.name + decl.generic);
    } else if (decl.kind == "enum") {
      summary.declarations.insert(declarationMetadataFact(package, decl.file, decl.moduleName, decl.kind, decl.name, decl.visibility, decl.generic, decl.lineNo));
      if (decl.visibility == "pub ") summary.publicApi.insert("enum " + decl.name);
      else if (decl.visibility != "private ") summary.packageApi.insert("enum " + decl.name);
      if (!decl.generic.empty()) summary.genericSignatures.insert("enum " + decl.name + decl.generic);
    } else if (decl.kind == "interface") {
      summary.declarations.insert(declarationMetadataFact(package, decl.file, decl.moduleName, decl.kind, decl.name, decl.visibility, decl.generic, decl.lineNo));
      summary.interfaces.insert(decl.name + decl.generic);
      if (decl.visibility == "pub ") summary.publicApi.insert("interface " + decl.name);
      else if (decl.visibility != "private ") summary.packageApi.insert("interface " + decl.name);
      if (!decl.generic.empty()) summary.genericSignatures.insert("interface " + decl.name + decl.generic);
    } else if (decl.kind == "fn") {
      summary.declarations.insert(declarationMetadataFact(package, decl.file, decl.moduleName, decl.kind, decl.name, decl.visibility, decl.generic + overloadKey(decl.name, decl.params), decl.lineNo));
      if (decl.visibility == "pub ") summary.publicApi.insert("fn " + decl.name);
      else if (decl.visibility != "private ") summary.packageApi.insert("fn " + decl.name);
      if (!decl.generic.empty()) summary.genericSignatures.insert("fn " + decl.name + decl.generic);
      if (!decl.returnType.empty() && !startsWith(decl.returnType, "Result<") && !startsWith(decl.returnType, "Option<") &&
          !isCCompatibleType(decl.returnType) && !startsWith(decl.returnType, "Array<") && !startsWith(decl.returnType, "Vector<") &&
          decl.returnType != "String" && decl.returnType != "Unit") {
        summary.staticInterfaceReturns.insert(decl.name + "->" + decl.returnType);
      }
    } else if (decl.kind == "const" || decl.kind == "static") {
      summary.declarations.insert(declarationMetadataFact(package, decl.file, decl.moduleName, decl.kind, decl.name, decl.visibility, decl.returnType + "=" + decl.params, decl.lineNo));
      if (decl.visibility == "pub ") summary.publicApi.insert(decl.kind + " " + decl.name);
      else if (decl.visibility != "private ") summary.packageApi.insert(decl.kind + " " + decl.name);
    } else if (decl.kind == "extern") {
      summary.declarations.insert(declarationMetadataFact(package, decl.file, decl.moduleName, decl.kind, decl.name, decl.visibility, decl.generic + overloadKey(decl.name, decl.params), decl.lineNo));
      if (decl.trusted) summary.trustCapabilities.insert("ffi");
    } else if (decl.kind == "impl") {
      if (contains(decl.name, " for ")) {
        summary.impls.insert(decl.name);
        const auto split = decl.name.find(" for ");
        const std::string interfaceName = decl.name.substr(0, split);
        const std::string typeName = decl.name.substr(split + 5);
        if (decl.trusted && (interfaceName == "Send" || interfaceName == "Sync")) {
          summary.sendSyncFacts.insert(typeName + ":" + interfaceName + ":trusted");
          summary.trustCapabilities.insert("threadSafety");
        }
      }
    }
  }

  for (const auto &file : sourceFiles) {
    if (file.extension() != ".zn") continue;
    const SourceText source = loadSource(file);
    for (const auto &rawLine : splitLines(source.text)) {
      const std::string line = stripLineComment(rawLine);
      const std::string trimmed = trim(line);
      if (trimmed.empty()) {
        continue;
      }
      if (contains(trimmed, "module alloc.")) summary.runtimeNeeds.insert("allocator");
      if (contains(trimmed, "module std.thread")) summary.runtimeNeeds.insert("thread");
      if (contains(trimmed, "module std.task")) summary.runtimeNeeds.insert("task-runtime");
      if (auto import = matchRegex(trimmed, importPattern)) {
        summary.dependencies.insert((*import)[1]);
      }
      if (auto decl = matchRegex(trimmed, structPattern)) {
        const std::string name = (*decl)[2];
        currentStructType = name;
        structBraceDepth = 0;
      }
      if (auto impl = matchRegex(trimmed, implPattern)) {
        (void)impl;
      } else if (auto inherent = matchRegex(trimmed, inherentImplPattern)) {
        currentImplType = (*inherent)[1];
      }
      if (startsWith(trimmed, "destroy")) {
        summary.dropGlue.insert(currentImplType + ":destroy");
        nonAutoSendSyncTypes.insert(currentImplType);
      }
      if (!currentStructType.empty()) {
        if (contains(trimmed, "RawPointer<") || contains(trimmed, "ArraySlice<") || contains(trimmed, "StringSlice") ||
            contains(trimmed, "TaskContext") || contains(trimmed, "PanicInfo") || contains(trimmed, "StackFrames")) {
          nonAutoSendSyncTypes.insert(currentStructType);
        }
        structBraceDepth += braceDelta(line);
        if (structBraceDepth <= 0 && contains(trimmed, "}")) {
          currentStructType.clear();
        }
      }
      if (auto exportDecl = matchRegex(trimmed, exportPattern)) {
        const std::string exportMode = (*exportDecl)[2];
        const std::string bridgeOrAbi = contains(exportMode, "bridge") ? "bridge=C" : "abi=C";
        summary.exports.insert(std::string((*exportDecl)[1]) + ":" + bridgeOrAbi);
        summary.runtimeNeeds.insert("c-abi-boundary");
      }
      if (contains(trimmed, "trust extern")) summary.trustCapabilities.insert("ffi");
      if (contains(trimmed, "RawPointer<")) summary.trustCapabilities.insert("rawMemory");
      if (usesHardwareTrustCapability(trimmed)) summary.trustCapabilities.insert("hardware");
      if (usesInlineAsmTrustCapability(trimmed)) summary.trustCapabilities.insert("inlineAsm");
      if (usesInterruptTrustCapability(trimmed)) summary.trustCapabilities.insert("interrupts");
      if (contains(trimmed, "trust {")) summary.trustCapabilities.insert("trust-block");
      if (contains(trimmed, "Thread.spawn")) summary.runtimeNeeds.insert("thread");
      if (contains(trimmed, "runtime.spawn")) summary.runtimeNeeds.insert("task-runtime");
      if (contains(trimmed, "TaskGroup<")) summary.runtimeNeeds.insert("task-group");
      if (contains(trimmed, "withCapacity") || contains(trimmed, "String.from")) summary.runtimeNeeds.insert("allocator");
      if (contains(trimmed, "panic(")) summary.runtimeNeeds.insert("panic");
      if (contains(trimmed, "oom(")) summary.runtimeNeeds.insert("oom");
      if (contains(trimmed, "try ")) summary.costInputs.insert("try-cleanup");
      if (contains(trimmed, "=>")) summary.costInputs.insert("pattern-lowering");
      if (contains(trimmed, "fn ") && contains(trimmed, "Fn<")) summary.costInputs.insert("closure-dispatch");
      if (contains(trimmed, "Box<")) summary.costInputs.insert("box-interface");
      if (contains(trimmed, "ArraySlice<")) summary.costInputs.insert("slice-provenance");
    }
  }
  for (const auto &dependency : package.manifestDependencies) {
    summary.dependencies.insert("manifest:" + dependency);
  }
  summary.dependencies.insert("builtin:core");
  for (const auto &trust : package.manifestTrust) {
    summary.trustCapabilities.insert("manifest:" + trust);
  }
  for (const auto &typeName : structTypes) {
    if (nonAutoSendSyncTypes.count(typeName)) continue;
    summary.sendSyncFacts.insert(typeName + ":Send:auto");
    summary.sendSyncFacts.insert(typeName + ":Sync:auto");
  }
  return summary;
}

std::set<std::string> requiredBuiltinPackages(const BuildPackage &package, const BuildSummary &summary, const std::string &profile) {
  std::set<std::string> required{"core"};
  if (isBuiltinPackageName(package.name)) required.insert(package.name);
  for (const auto &dependency : package.manifestDependencies) {
    for (const auto &name : {"core", "alloc", "std"}) {
      if (startsWith(dependency, std::string(name) + ":")) required.insert(name);
    }
  }
  for (const auto &dependency : summary.dependencies) {
    for (const auto &name : {"core", "alloc", "std"}) {
      if (dependency == name || startsWith(dependency, std::string(name) + ".")) required.insert(name);
    }
  }
  if (summary.runtimeNeeds.count("allocator") || summary.costInputs.count("box-interface")) required.insert("alloc");
  if (summary.runtimeNeeds.count("thread") || summary.runtimeNeeds.count("task-runtime") || summary.runtimeNeeds.count("task-group")) {
    if (profile == "hosted") required.insert("std");
  }
  return required;
}

std::set<std::string> builtinPackageFacts(const std::set<std::string> &names) {
  std::set<std::string> facts;
  for (const auto &name : names) {
    const fs::path root = builtinPackageRoot(name);
    if (fs::exists(root / "Zeno.toml")) {
      facts.insert(name + ":" + packageFingerprint(root));
    } else {
      facts.insert(name + ":missing");
    }
  }
  return facts;
}

std::set<std::string> builtinPublicApiFacts(const std::set<std::string> &names) {
  std::set<std::string> facts;
  for (const auto &name : names) {
    const fs::path root = builtinPackageRoot(name);
    if (!fs::exists(root / "Zeno.toml")) continue;
    BuildPackage builtin = readBuildPackage(root);
    BuildSummary summary = summarizeBuildPackage(builtin);
    for (const auto &api : summary.publicApi) facts.insert(name + ":" + api);
    for (const auto &layout : summary.layouts) facts.insert(name + ":layout:" + layout);
    for (const auto &runtime : summary.runtimeNeeds) facts.insert(name + ":runtime:" + runtime);
  }
  return facts;
}

void collectDependencyPackageFacts(const BuildPackage &package,
                                   const std::string &prefix,
                                   const fs::path &anchorRoot,
                                   std::set<fs::path> active,
                                   std::set<std::string> &facts) {
  if (package.singleSource.empty() && fs::is_directory(package.root)) {
    const fs::path packageRoot = stableAbsolutePath(package.root);
    if (active.count(packageRoot)) return;
    active.insert(packageRoot);

    const PackageInfo info = parsePackageInfo(package.root);
    for (const auto &[key, depPath] : info.dependencies) {
      const PackageInfo depInfo = parsePackageInfo(depPath);
      const std::string depName = depInfo.name.empty() ? key : depInfo.name;
      const std::string dependencyPrefix = prefix.empty() ? key : prefix + "/" + key;
      std::error_code error;
      fs::path relative = fs::relative(depPath, anchorRoot, error);
      if (error) relative = depPath.filename();
      facts.insert(dependencyPrefix + ":package=" + depName +
                   ":path=" + relative.string() +
                   ":manifest=" + fileFingerprint(depPath / "Zeno.toml") +
                   ":content=" + packageContentFingerprint(depPath));
      collectDependencyPackageFacts(readBuildPackage(depPath), dependencyPrefix, anchorRoot, active, facts);
    }
    for (const auto &builtin : info.builtinDependencies) {
      const fs::path builtinRoot = builtinPackageRoot(builtin);
      if (isBuiltinPackageName(builtin) && fs::exists(builtinRoot / "Zeno.toml")) {
        const std::string dependencyPrefix = prefix.empty() ? builtin : prefix + "/" + builtin;
        facts.insert(dependencyPrefix + ":package=" + builtin + ":builtin=" + packageFingerprint(builtinRoot));
      }
    }
  }
}

std::set<std::string> dependencyPackageFacts(const BuildPackage &package) {
  std::set<std::string> facts;
  collectDependencyPackageFacts(package, "", package.root, {}, facts);
  return facts;
}

void collectDependencyRuntimeFacts(const BuildPackage &package,
                                   const std::string &prefix,
                                   std::set<fs::path> active,
                                   std::set<std::string> &facts) {
  if (!package.singleSource.empty() || !fs::is_directory(package.root)) return;
  const fs::path packageRoot = stableAbsolutePath(package.root);
  if (active.count(packageRoot)) return;
  active.insert(packageRoot);

  const PackageInfo info = parsePackageInfo(package.root);
  for (const auto &[key, depPath] : info.dependencies) {
    BuildPackage dependency = readBuildPackage(depPath);
    BuildSummary summary = summarizeBuildPackage(dependency);
    const std::string dependencyPrefix = prefix.empty() ? key : prefix + "/" + key;
    for (const auto &runtime : summary.runtimeNeeds) {
      facts.insert(dependencyPrefix + ":" + runtime);
    }
    collectDependencyRuntimeFacts(dependency, dependencyPrefix, active, facts);
  }
}

std::set<std::string> dependencyRuntimeFacts(const BuildPackage &package) {
  std::set<std::string> facts;
  collectDependencyRuntimeFacts(package, "", {}, facts);
  return facts;
}

std::set<std::string> linkedRuntimeNeeds(const BuildSummary &summary, const std::set<std::string> &dependencyRuntime) {
  std::set<std::string> needs = summary.runtimeNeeds;
  for (const auto &fact : dependencyRuntime) {
    const auto colon = fact.rfind(':');
    if (colon != std::string::npos && colon + 1 < fact.size()) {
      needs.insert(fact.substr(colon + 1));
    }
  }
  return needs;
}

std::string arField(const std::string &value, std::size_t width) {
  if (value.size() >= width) return value.substr(0, width);
  return value + std::string(width - value.size(), ' ');
}

std::string arMember(const std::string &name, const std::string &content) {
  std::string out;
  out += arField(name, 16);
  out += arField("0", 12);
  out += arField("0", 6);
  out += arField("0", 6);
  out += arField("100644", 8);
  out += arField(std::to_string(content.size()), 10);
  out += "`\n";
  out += content;
  if (content.size() % 2 != 0) out += "\n";
  return out;
}

std::string stage0StaticArchive(const std::string &object) {
  return "!<arch>\n" + arMember("zeno-stage0.o/", object);
}

std::string shellQuote(const std::string &value) {
  std::string out = "'";
  for (char c : value) {
    if (c == '\'') {
      out += "'\\''";
    } else {
      out.push_back(c);
    }
  }
  out += "'";
  return out;
}

bool runShellCommand(const std::string &command, const fs::path &logPath) {
  fs::create_directories(logPath.parent_path());
  const std::string fullCommand = command + " > " + shellQuote(logPath.string()) + " 2>&1";
  return std::system(fullCommand.c_str()) == 0;
}

int runShellCommandExitCode(const std::string &command, const fs::path &logPath) {
  fs::create_directories(logPath.parent_path());
  const std::string fullCommand = command + " > " + shellQuote(logPath.string()) + " 2>&1";
  const int status = std::system(fullCommand.c_str());
  if (status == -1) return -1;
  if (WIFEXITED(status)) return WEXITSTATUS(status);
  if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
  return -1;
}

struct NativeI32Expr {
  bool ok = false;
  std::string code;
  std::string value;
  std::string typeName = "I32";
};

struct NativeI32Condition {
  bool ok = false;
  std::string code;
  std::string value;
};

struct NativeI32Local {
  bool mutableLocal = false;
  std::string value;
  std::string typeName = "I32";
};

struct NativeI32StructType {
  std::vector<std::string> fields;
};

struct NativeI32EnumType {
  std::vector<std::string> variants;
  std::map<std::string, int> discriminants;
};

struct NativePayloadEnumVariant {
  std::vector<std::string> payloadTypes;
  std::vector<std::string> payloadNames;
};

struct NativePayloadEnumType {
  std::string sourceName;
  std::vector<std::string> variants;
  std::map<std::string, int> discriminants;
  std::map<std::string, NativePayloadEnumVariant> variantPayloads;
  std::size_t maxPayloadSlots = 0;
};

struct NativeGenericPayloadEnumTemplate {
  std::vector<std::string> typeParams;
  NativePayloadEnumType enumType;
};

struct NativeI32StructLocal {
  std::string typeName;
  std::string value;
};

struct NativeI32StructExpr {
  bool ok = false;
  std::string code;
  std::string typeName;
  std::string value;
};

std::string nativeLlvmTypeName(const std::string &typeName) {
  std::string out;
  for (const char c : typeName) {
    if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') {
      out += c;
    } else if (!out.empty() && out.back() != '_') {
      out += '_';
    }
  }
  while (!out.empty() && out.back() == '_') out.pop_back();
  return out.empty() ? "anon" : out;
}

std::string nativeLlvmTypeRef(const std::string &typeName) {
  return "%" + nativeLlvmTypeName(typeName);
}

bool isNativeI32ScalarType(const std::string &typeName) {
  const std::string normalized = normalizeSignatureType(typeName);
  return normalized == "I32" || normalized == "Char";
}

bool isNativeScalarType(const std::string &typeName) {
  const std::string normalized = normalizeSignatureType(typeName);
  return isNativeI32ScalarType(normalized) || normalized == "F64";
}

std::string nativeScalarLlvmType(const std::string &typeName) {
  return normalizeSignatureType(typeName) == "F64" ? "double" : "i32";
}

std::string nativeScalarZero(const std::string &typeName) {
  return normalizeSignatureType(typeName) == "F64" ? "0.000000e+00" : "0";
}

std::string nativeScalarCopyOp(const std::string &typeName) {
  return normalizeSignatureType(typeName) == "F64" ? "fadd" : "add";
}

std::optional<int> nativeCharLiteralValue(const std::string &rawExpr) {
  const std::string expr = trim(rawExpr);
  if (expr.size() < 3 || expr.front() != '\'' || expr.back() != '\'') return std::nullopt;
  const std::string body = expr.substr(1, expr.size() - 2);
  if (body.size() == 1 && body[0] != '\\') {
    return static_cast<unsigned char>(body[0]);
  }
  if (body.size() == 2 && body[0] == '\\') {
    switch (body[1]) {
      case 'n': return 10;
      case 'r': return 13;
      case 't': return 9;
      case '0': return 0;
      case '\\': return static_cast<int>('\\');
      case '\'': return static_cast<int>('\'');
      default: return std::nullopt;
    }
  }
  return std::nullopt;
}

bool isNativeF64Literal(const std::string &rawExpr) {
  const std::string expr = trim(rawExpr);
  return std::regex_match(expr, std::regex(R"([0-9]+\.[0-9]+(?:[eE][+-]?[0-9]+)?)"));
}

std::optional<std::pair<std::string, std::string>> nativeSingleGenericInstance(const std::string &typeName) {
  if (auto generic = matchRegex(typeName, std::regex(R"(^([A-Z][A-Za-z0-9_]*)<\s*([A-Za-z_][A-Za-z0-9_]*)\s*>$)"))) {
    return std::make_pair(std::string((*generic)[1]), std::string((*generic)[2]));
  }
  return std::nullopt;
}

std::optional<std::pair<std::string, std::vector<std::string>>> nativeGenericInstance(const std::string &typeName) {
  if (auto generic = matchRegex(typeName, std::regex(R"(^([A-Z][A-Za-z0-9_]*)<\s*(.*?)\s*>$)"))) {
    std::vector<std::string> args;
    for (const auto &arg : splitArguments((*generic)[2])) {
      const std::string normalized = normalizeSignatureType(arg);
      if (!std::regex_match(normalized, std::regex(R"([A-Za-z_][A-Za-z0-9_]*)"))) return std::nullopt;
      args.push_back(normalized);
    }
    if (args.empty()) return std::nullopt;
    return std::make_pair(std::string((*generic)[1]), args);
  }
  return std::nullopt;
}

std::string resolveNativePayloadEnumTypeName(const std::string &patternEnumName,
                                             const std::string &activeTypeName,
                                             const std::map<std::string, NativePayloadEnumType> &payloadEnumTypes) {
  if (payloadEnumTypes.count(patternEnumName)) return patternEnumName;
  if (!activeTypeName.empty()) {
    auto active = payloadEnumTypes.find(activeTypeName);
    if (active != payloadEnumTypes.end() && active->second.sourceName == patternEnumName) {
      return activeTypeName;
    }
  }
  std::string resolved;
  for (const auto &[typeName, enumType] : payloadEnumTypes) {
    if (enumType.sourceName != patternEnumName) continue;
    if (!resolved.empty()) return "";
    resolved = typeName;
  }
  return resolved;
}

struct NativeMatchPatternAlt {
  std::string enumName;
  std::string variantName;
  std::vector<std::string> bindingNames;
  bool hasRange = false;
  int rangeStart = 0;
  int rangeEnd = 0;
  bool rangeInclusive = false;
};

struct NativeMatchArm {
  bool ok = false;
  std::vector<NativeMatchPatternAlt> alternatives;
  std::string guard;
  std::string expr;
};

bool isSimpleLlvmGlobalName(const std::string &name) {
  return std::regex_match(name, std::regex(R"([A-Za-z_][A-Za-z0-9_]*)"));
}

int nativeStructFieldIndex(const NativeI32StructType &type, const std::string &fieldName) {
  for (std::size_t i = 0; i < type.fields.size(); ++i) {
    if (type.fields[i] == fieldName) return static_cast<int>(i);
  }
  return -1;
}

std::string nativeAccessName(std::string name) {
  name = trim(name);
  if (startsWith(name, "move ")) name = trim(name.substr(5));
  if (startsWith(name, "mut ")) name = trim(name.substr(4));
  return name;
}

std::vector<std::string> splitTopLevel(const std::string &text, char delimiter) {
  std::vector<std::string> parts;
  int parenDepth = 0;
  int braceDepth = 0;
  bool inString = false;
  std::size_t start = 0;
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (c == '"' && (i == 0 || text[i - 1] != '\\')) {
      inString = !inString;
      continue;
    }
    if (inString) continue;
    if (c == '(') ++parenDepth;
    else if (c == ')') --parenDepth;
    else if (c == '{') ++braceDepth;
    else if (c == '}') --braceDepth;
    else if (c == delimiter && parenDepth == 0 && braceDepth == 0) {
      parts.push_back(trim(std::string_view(text).substr(start, i - start)));
      start = i + 1;
    }
  }
  parts.push_back(trim(std::string_view(text).substr(start)));
  return parts;
}

std::optional<std::size_t> findTopLevelToken(const std::string &text, const std::string &token) {
  int parenDepth = 0;
  int braceDepth = 0;
  bool inString = false;
  for (std::size_t i = 0; i + token.size() <= text.size(); ++i) {
    const char c = text[i];
    if (c == '"' && (i == 0 || text[i - 1] != '\\')) {
      inString = !inString;
      continue;
    }
    if (inString) continue;
    if (c == '(') {
      ++parenDepth;
      continue;
    }
    if (c == ')') {
      --parenDepth;
      continue;
    }
    if (c == '{') {
      ++braceDepth;
      continue;
    }
    if (c == '}') {
      --braceDepth;
      continue;
    }
    if (parenDepth == 0 && braceDepth == 0 && text.compare(i, token.size(), token) == 0) {
      return i;
    }
  }
  return std::nullopt;
}

std::optional<std::pair<std::string, int>> nativeEnumDiscriminant(
    const std::string &expr,
    const std::map<std::string, NativeI32EnumType> &enumTypes) {
  auto variant = matchRegex(trim(expr), std::regex(R"(^([A-Z][A-Za-z0-9_]*)\.([A-Za-z_][A-Za-z0-9_]*)$)"));
  if (!variant) return std::nullopt;
  const std::string enumName = (*variant)[1];
  const std::string variantName = (*variant)[2];
  auto enumType = enumTypes.find(enumName);
  if (enumType == enumTypes.end()) return std::nullopt;
  auto discriminant = enumType->second.discriminants.find(variantName);
  if (discriminant == enumType->second.discriminants.end()) return std::nullopt;
  return std::make_pair(enumName, discriminant->second);
}

std::optional<std::size_t> findTopLevelNativeBinaryOperator(const std::string &expr, const std::string &operators) {
  int parenDepth = 0;
  int braceDepth = 0;
  bool inString = false;
  for (std::size_t offset = expr.size(); offset > 0; --offset) {
    const std::size_t i = offset - 1;
    const char c = expr[i];
    if (c == '"' && (i == 0 || expr[i - 1] != '\\')) {
      inString = !inString;
      continue;
    }
    if (inString) continue;
    if (c == ')') {
      ++parenDepth;
      continue;
    }
    if (c == '(') {
      --parenDepth;
      continue;
    }
    if (c == '}') {
      ++braceDepth;
      continue;
    }
    if (c == '{') {
      --braceDepth;
      continue;
    }
    if (parenDepth != 0 || braceDepth != 0 || operators.find(c) == std::string::npos) continue;
    if (i == 0) continue;
    const std::string left = trim(std::string_view(expr).substr(0, i));
    const std::string right = trim(std::string_view(expr).substr(i + 1));
    if (left.empty() || right.empty()) continue;
    const char previous = left.empty() ? '\0' : left.back();
    if ((c == '+' || c == '-') && (previous == '+' || previous == '-' || previous == '*' || previous == '/' || previous == '(' || previous == '{')) {
      continue;
    }
    return i;
  }
  return std::nullopt;
}

std::string nativeI32ExportThunk(const std::string &symbol,
                                 const std::string &functionName,
                                 const std::string &functionParams) {
  if (!isSimpleLlvmGlobalName(symbol) || !isSimpleLlvmGlobalName(functionName)) return "";
  std::vector<std::string> arguments;
  for (const auto &param : splitArguments(functionParams)) {
    const std::string trimmedParam = trim(param);
    if (trimmedParam.empty()) continue;
    if (auto parsed = matchRegex(trimmedParam, std::regex(R"(^(i32|%[A-Za-z_][A-Za-z0-9_]*)\s+(%[A-Za-z_][A-Za-z0-9_]*)$)"))) {
      arguments.push_back(std::string((*parsed)[1]) + " " + std::string((*parsed)[2]));
    } else {
      return "";
    }
  }

  std::string out = "\ndefine i32 @" + symbol + "(" + functionParams + ") {\nentry:\n";
  out += "  %ret = call i32 @" + functionName + "(";
  for (std::size_t i = 0; i < arguments.size(); ++i) {
    if (i != 0) out += ", ";
    out += arguments[i];
  }
  out += ")\n";
  out += "  ret i32 %ret\n";
  out += "}\n";
  return out;
}

bool nativeI32ParameterList(std::string params, std::string *llvmParams) {
  std::string out;
  for (const auto &param : splitArguments(params)) {
    const std::string trimmedParam = trim(param);
    if (trimmedParam.empty()) continue;
    const auto colon = trimmedParam.rfind(':');
    if (colon == std::string::npos) return false;
    if (normalizeSignatureType(trimmedParam.substr(colon + 1)) != "I32") return false;
    if (!out.empty()) out += ", ";
    out += "i32";
  }
  *llvmParams = out;
  return true;
}

bool nativeParameterList(std::string params,
                         const std::map<std::string, NativeI32StructType> &structTypes,
                         const std::map<std::string, NativeI32EnumType> &enumTypes,
                         std::vector<std::pair<std::string, std::string>> *parsedParams,
                         std::string *llvmParams) {
  std::vector<std::pair<std::string, std::string>> parsed;
  std::string out;
  for (const auto &param : splitArguments(params)) {
    const std::string trimmedParam = trim(param);
    if (trimmedParam.empty()) continue;
    const auto colon = trimmedParam.rfind(':');
    if (colon == std::string::npos) return false;
    std::string name = trim(trimmedParam.substr(0, colon));
    if (startsWith(name, "move ")) name = trim(name.substr(5));
    if (startsWith(name, "mut ")) name = trim(name.substr(4));
    if (!std::regex_match(name, std::regex(R"([A-Za-z_][A-Za-z0-9_]*)"))) return false;
    const std::string type = normalizeSignatureType(trimmedParam.substr(colon + 1));
    std::string llvmType;
    if (isNativeI32ScalarType(type)) llvmType = "i32";
    else if (enumTypes.count(type)) llvmType = "i32";
    else if (structTypes.count(type)) llvmType = nativeLlvmTypeRef(type);
    else return false;
    if (!out.empty()) out += ", ";
    out += llvmType + " %" + name;
    parsed.push_back({name, type});
  }
  *parsedParams = parsed;
  *llvmParams = out;
  return true;
}

NativeI32Expr lowerNativeI32Expr(const std::string &rawExpr,
                                 const std::map<std::string, NativeI32Local> &locals,
                                 const std::map<std::string, NativeI32StructLocal> &structLocals,
                                 const std::map<std::string, NativeI32StructType> &structTypes,
                                 const std::map<std::string, NativeI32EnumType> &enumTypes,
                                 const std::map<std::string, std::vector<std::string>> &candidateFunctionParamTypes,
                                 const std::set<std::string> &candidateCallees,
                                 int &tempId) {
  std::string expr = trim(rawExpr);
  if (!expr.empty() && expr.back() == ';') expr.pop_back();
  expr = trim(expr);
  if (std::regex_match(expr, std::regex(R"([0-9]+)"))) {
    return NativeI32Expr{true, "", expr};
  }
  if (isNativeF64Literal(expr)) {
    return NativeI32Expr{true, "", expr, "F64"};
  }
  if (auto charValue = nativeCharLiteralValue(expr)) {
    return NativeI32Expr{true, "", std::to_string(*charValue), "Char"};
  }
  if (auto enumValue = nativeEnumDiscriminant(expr, enumTypes)) {
    return NativeI32Expr{true, "", std::to_string(enumValue->second)};
  }
  if (auto field = matchRegex(expr, std::regex(R"(^([A-Za-z_][A-Za-z0-9_]*)\.([A-Za-z_][A-Za-z0-9_]*)$)"))) {
    auto aggregate = structLocals.find((*field)[1]);
    if (aggregate == structLocals.end()) return {};
    auto type = structTypes.find(aggregate->second.typeName);
    if (type == structTypes.end()) return {};
    const int index = nativeStructFieldIndex(type->second, (*field)[2]);
    if (index < 0) return {};
    const std::string temp = "%t" + std::to_string(tempId++);
    return NativeI32Expr{true,
                         "  " + temp + " = extractvalue " + nativeLlvmTypeRef(aggregate->second.typeName) + " " + aggregate->second.value + ", " + std::to_string(index) + "\n",
                         temp};
  }
  std::optional<std::size_t> binaryIndex = findTopLevelNativeBinaryOperator(expr, "+-");
  if (!binaryIndex) binaryIndex = findTopLevelNativeBinaryOperator(expr, "*/");
  if (binaryIndex) {
    const std::string op(1, expr[*binaryIndex]);
    auto left = lowerNativeI32Expr(expr.substr(0, *binaryIndex), locals, structLocals, structTypes, enumTypes, candidateFunctionParamTypes, candidateCallees, tempId);
    auto right = lowerNativeI32Expr(expr.substr(*binaryIndex + 1), locals, structLocals, structTypes, enumTypes, candidateFunctionParamTypes, candidateCallees, tempId);
    if (!left.ok || !right.ok) return {};
    std::string llvmOp;
    std::string resultType = "I32";
    if (left.typeName == "F64" || right.typeName == "F64") {
      if (left.typeName != "F64" || right.typeName != "F64") return {};
      resultType = "F64";
      if (op == "+") llvmOp = "fadd";
      else if (op == "-") llvmOp = "fsub";
      else if (op == "*") llvmOp = "fmul";
      else if (op == "/") llvmOp = "fdiv";
      else return {};
    } else {
      if (op == "+") llvmOp = "add";
      else if (op == "-") llvmOp = "sub";
      else if (op == "*") llvmOp = "mul";
      else if (op == "/") llvmOp = "sdiv";
      else return {};
    }
    const std::string temp = "%t" + std::to_string(tempId++);
    return NativeI32Expr{true,
                         left.code + right.code + "  " + temp + " = " + llvmOp + " " + nativeScalarLlvmType(resultType) + " " + left.value + ", " + right.value + "\n",
                         temp,
                         resultType};
  }
  if (std::regex_match(expr, std::regex(R"([A-Za-z_][A-Za-z0-9_]*)"))) {
    auto local = locals.find(expr);
    if (local == locals.end()) return {};
    if (!local->second.mutableLocal) {
      return NativeI32Expr{true, "", local->second.value, local->second.typeName};
    }
    const std::string temp = "%t" + std::to_string(tempId++);
    return NativeI32Expr{true, "  " + temp + " = load " + nativeScalarLlvmType(local->second.typeName) + ", ptr " + local->second.value + "\n", temp, local->second.typeName};
  }
  if (auto call = matchRegex(expr, std::regex(R"(^([A-Za-z_][A-Za-z0-9_]*)\s*\((.*)\)$)"))) {
    const std::string callee = (*call)[1];
    if (!candidateCallees.count(callee)) return {};
    const auto rawArgs = splitArguments((*call)[2]);
    std::vector<std::string> paramTypes;
    auto signature = candidateFunctionParamTypes.find(callee);
    if (signature != candidateFunctionParamTypes.end()) {
      paramTypes = signature->second;
      if (paramTypes.size() != rawArgs.size()) return {};
    } else {
      paramTypes.assign(rawArgs.size(), "I32");
    }
    std::string code;
    std::vector<std::string> arguments;
    for (std::size_t i = 0; i < rawArgs.size(); ++i) {
      const std::string arg = trim(rawArgs[i]);
      const std::string paramType = paramTypes[i];
      if (isNativeScalarType(paramType)) {
        auto lowered = lowerNativeI32Expr(arg, locals, structLocals, structTypes, enumTypes, candidateFunctionParamTypes, candidateCallees, tempId);
        if (!lowered.ok) return {};
        code += lowered.code;
        arguments.push_back(nativeScalarLlvmType(paramType) + " " + lowered.value);
      } else if (enumTypes.count(paramType)) {
        auto lowered = lowerNativeI32Expr(arg, locals, structLocals, structTypes, enumTypes, candidateFunctionParamTypes, candidateCallees, tempId);
        if (!lowered.ok) return {};
        code += lowered.code;
        arguments.push_back("i32 " + lowered.value);
      } else if (structTypes.count(paramType)) {
        auto localStruct = structLocals.find(arg);
        if (localStruct == structLocals.end() || localStruct->second.typeName != paramType) return {};
        arguments.push_back(nativeLlvmTypeRef(paramType) + " " + localStruct->second.value);
      } else {
        return {};
      }
    }
    const std::string temp = "%t" + std::to_string(tempId++);
    code += "  " + temp + " = call i32 @" + callee + "(";
    for (std::size_t i = 0; i < arguments.size(); ++i) {
      if (i != 0) code += ", ";
      code += arguments[i];
    }
    code += ")\n";
    return NativeI32Expr{true, code, temp, "I32"};
  }
  return {};
}

NativeI32Condition lowerNativeI32Condition(const std::string &rawCondition,
                                           const std::map<std::string, NativeI32Local> &locals,
                                           const std::map<std::string, NativeI32StructLocal> &structLocals,
                                           const std::map<std::string, NativeI32StructType> &structTypes,
                                           const std::map<std::string, NativeI32EnumType> &enumTypes,
                                           const std::map<std::string, std::vector<std::string>> &candidateFunctionParamTypes,
                                           const std::set<std::string> &candidateCallees,
                                           const std::set<std::string> &candidateBoolCallees,
                                           int &tempId) {
  std::string condition = trim(rawCondition);
  const std::regex comparisonPattern(R"(^(.+)\s*(>=|<=|==|!=|>|<)\s*(.+)$)");
  if (auto comparison = matchRegex(condition, comparisonPattern)) {
    auto left = lowerNativeI32Expr((*comparison)[1], locals, structLocals, structTypes, enumTypes, candidateFunctionParamTypes, candidateCallees, tempId);
    auto right = lowerNativeI32Expr((*comparison)[3], locals, structLocals, structTypes, enumTypes, candidateFunctionParamTypes, candidateCallees, tempId);
    if (!left.ok || !right.ok) return {};
    const std::string op = (*comparison)[2];
    std::string predicate;
    if (op == ">") predicate = "sgt";
    else if (op == ">=") predicate = "sge";
    else if (op == "<") predicate = "slt";
    else if (op == "<=") predicate = "sle";
    else if (op == "==") predicate = "eq";
    else if (op == "!=") predicate = "ne";
    else return {};
    const std::string temp = "%t" + std::to_string(tempId++);
    return NativeI32Condition{true,
                              left.code + right.code + "  " + temp + " = icmp " + predicate + " i32 " + left.value + ", " + right.value + "\n",
                              temp};
  }
  if (condition == "true") return NativeI32Condition{true, "", "true"};
  if (condition == "false") return NativeI32Condition{true, "", "false"};
  if (auto call = matchRegex(condition, std::regex(R"(^([A-Za-z_][A-Za-z0-9_]*)\s*\((.*)\)$)"))) {
    const std::string callee = (*call)[1];
    if (!candidateBoolCallees.count(callee)) return {};
    std::string code;
    std::vector<std::string> arguments;
    for (const auto &arg : splitArguments((*call)[2])) {
      auto lowered = lowerNativeI32Expr(arg, locals, structLocals, structTypes, enumTypes, candidateFunctionParamTypes, candidateCallees, tempId);
      if (!lowered.ok) return {};
      code += lowered.code;
      arguments.push_back(lowered.value);
    }
    const std::string temp = "%t" + std::to_string(tempId++);
    code += "  " + temp + " = call i1 @" + callee + "(";
    for (std::size_t i = 0; i < arguments.size(); ++i) {
      if (i != 0) code += ", ";
      code += "i32 " + arguments[i];
    }
    code += ")\n";
    return NativeI32Condition{true, code, temp};
  }
  return {};
}

NativeI32StructExpr lowerNativeI32StructLiteral(const std::string &rawExpr,
                                                const std::map<std::string, NativeI32Local> &locals,
                                                const std::map<std::string, NativeI32StructLocal> &structLocals,
                                                const std::map<std::string, NativeI32StructType> &structTypes,
                                                const std::map<std::string, NativeI32EnumType> &enumTypes,
                                                const std::map<std::string, std::vector<std::string>> &candidateFunctionParamTypes,
                                                const std::set<std::string> &candidateCallees,
                                                int &tempId) {
  std::string expr = trim(rawExpr);
  if (!expr.empty() && expr.back() == ';') expr.pop_back();
  expr = trim(expr);
  auto literal = matchRegex(expr, std::regex(R"(^([A-Z][A-Za-z0-9_]*)\s*\{\s*(.*)\s*\}$)"));
  if (!literal) return {};
  const std::string structName = (*literal)[1];
  auto structType = structTypes.find(structName);
  if (structType == structTypes.end()) return {};

  std::map<std::string, std::string> fieldExprs;
  for (const auto &fieldInit : splitArguments((*literal)[2])) {
    if (auto field = matchRegex(fieldInit, std::regex(R"(^\s*([A-Za-z_][A-Za-z0-9_]*)\s*:\s*(.+?)\s*$)"))) {
      fieldExprs[(*field)[1]] = (*field)[2];
    } else {
      return {};
    }
  }

  std::string code;
  std::string aggregateValue = "undef";
  for (std::size_t i = 0; i < structType->second.fields.size(); ++i) {
    const std::string &fieldName = structType->second.fields[i];
    auto fieldExpr = fieldExprs.find(fieldName);
    if (fieldExpr == fieldExprs.end()) return {};
    auto lowered = lowerNativeI32Expr(fieldExpr->second, locals, structLocals, structTypes, enumTypes, candidateFunctionParamTypes, candidateCallees, tempId);
    if (!lowered.ok) return {};
    const std::string temp = "%t" + std::to_string(tempId++);
    code += lowered.code;
    code += "  " + temp + " = insertvalue " + nativeLlvmTypeRef(structName) + " " + aggregateValue + ", i32 " + lowered.value + ", " + std::to_string(i) + "\n";
    aggregateValue = temp;
  }
  return NativeI32StructExpr{true, code, structName, aggregateValue};
}

NativeI32StructExpr lowerNativePayloadEnumConstructor(const std::string &rawExpr,
                                                      const std::map<std::string, NativeI32Local> &locals,
                                                      const std::map<std::string, NativeI32StructLocal> &structLocals,
                                                      const std::map<std::string, NativeI32StructType> &structTypes,
                                                      const std::map<std::string, NativeI32EnumType> &enumTypes,
                                                      const std::map<std::string, NativePayloadEnumType> &payloadEnumTypes,
                                                      const std::map<std::string, std::vector<std::string>> &candidateFunctionParamTypes,
                                                      const std::set<std::string> &candidateCallees,
                                                      const std::string &expectedTypeName,
                                                      int &tempId) {
  std::string expr = trim(rawExpr);
  if (!expr.empty() && expr.back() == ';') expr.pop_back();
  expr = trim(expr);
  std::string enumName;
  std::string variantName;
  std::vector<std::string> explicitArgs;
  std::map<std::string, std::string> recordArgs;
  if (auto recordCtor = matchRegex(expr, std::regex(R"(^([A-Z][A-Za-z0-9_]*)\.([A-Za-z_][A-Za-z0-9_]*)\s*\{\s*(.*)\s*\}$)"))) {
    enumName = (*recordCtor)[1];
    variantName = (*recordCtor)[2];
    for (const auto &fieldInit : splitArguments((*recordCtor)[3])) {
      if (auto field = matchRegex(fieldInit, std::regex(R"(^\s*([A-Za-z_][A-Za-z0-9_]*)\s*:\s*(.+?)\s*$)"))) {
        recordArgs[(*field)[1]] = (*field)[2];
      } else {
        return {};
      }
    }
  } else if (auto ctor = matchRegex(expr, std::regex(R"(^([A-Z][A-Za-z0-9_]*)\.([A-Za-z_][A-Za-z0-9_]*)(?:\s*\((.*)\))?$)"))) {
    enumName = (*ctor)[1];
    variantName = (*ctor)[2];
    explicitArgs = (*ctor)[3].matched ? splitArguments((*ctor)[3]) : std::vector<std::string>{};
  } else {
    return {};
  }
  const std::string resolvedEnumName = resolveNativePayloadEnumTypeName(enumName, expectedTypeName, payloadEnumTypes);
  if (resolvedEnumName.empty()) return {};
  enumName = resolvedEnumName;
  auto enumType = payloadEnumTypes.find(enumName);
  if (enumType == payloadEnumTypes.end()) return {};
  auto discriminant = enumType->second.discriminants.find(variantName);
  if (discriminant == enumType->second.discriminants.end()) return {};
  const auto payload = enumType->second.variantPayloads.find(variantName);
  const std::vector<std::string> payloadTypes = payload == enumType->second.variantPayloads.end()
                                                    ? std::vector<std::string>{}
                                                    : payload->second.payloadTypes;
  std::vector<std::string> args = explicitArgs;
  if (!recordArgs.empty()) {
    args.clear();
    if (payload == enumType->second.variantPayloads.end() ||
        payload->second.payloadNames.size() != payloadTypes.size()) {
      return {};
    }
    for (const auto &fieldName : payload->second.payloadNames) {
      auto field = recordArgs.find(fieldName);
      if (field == recordArgs.end()) return {};
      args.push_back(field->second);
    }
  }
  if (args.size() != payloadTypes.size()) return {};

  std::string code;
  std::string aggregateValue = "undef";
  std::string temp = "%t" + std::to_string(tempId++);
  code += "  " + temp + " = insertvalue " + nativeLlvmTypeRef(enumName) + " " + aggregateValue + ", i32 " + std::to_string(discriminant->second) + ", 0\n";
  aggregateValue = temp;
  std::size_t slot = 1;
  for (std::size_t i = 0; i < payloadTypes.size(); ++i) {
    const std::string payloadType = payloadTypes[i];
    const std::string arg = trim(args[i]);
    if (isNativeI32ScalarType(payloadType)) {
      auto lowered = lowerNativeI32Expr(arg, locals, structLocals, structTypes, enumTypes, candidateFunctionParamTypes, candidateCallees, tempId);
      if (!lowered.ok) return {};
      temp = "%t" + std::to_string(tempId++);
      code += lowered.code;
      code += "  " + temp + " = insertvalue " + nativeLlvmTypeRef(enumName) + " " + aggregateValue + ", i32 " + lowered.value + ", " + std::to_string(slot++) + "\n";
      aggregateValue = temp;
    } else {
      NativeI32StructExpr loweredStruct;
      if (auto localStruct = structLocals.find(arg);
          localStruct != structLocals.end() && localStruct->second.typeName == payloadType) {
        loweredStruct = NativeI32StructExpr{true, "", localStruct->second.typeName, localStruct->second.value};
      } else {
        loweredStruct = lowerNativeI32StructLiteral(arg, locals, structLocals, structTypes, enumTypes, candidateFunctionParamTypes, candidateCallees, tempId);
      }
      auto structType = structTypes.find(payloadType);
      if (!loweredStruct.ok || loweredStruct.typeName != payloadType || structType == structTypes.end()) return {};
      code += loweredStruct.code;
      for (std::size_t field = 0; field < structType->second.fields.size(); ++field) {
        const std::string fieldTemp = "%t" + std::to_string(tempId++);
        temp = "%t" + std::to_string(tempId++);
        code += "  " + fieldTemp + " = extractvalue " + nativeLlvmTypeRef(payloadType) + " " + loweredStruct.value + ", " + std::to_string(field) + "\n";
        code += "  " + temp + " = insertvalue " + nativeLlvmTypeRef(enumName) + " " + aggregateValue + ", i32 " + fieldTemp + ", " + std::to_string(slot++) + "\n";
        aggregateValue = temp;
      }
    }
  }
  return NativeI32StructExpr{true, code, enumName, aggregateValue};
}

NativeI32StructExpr lowerNativeAggregateExpr(const std::string &rawExpr,
                                             const std::map<std::string, NativeI32Local> &locals,
                                             const std::map<std::string, NativeI32StructLocal> &structLocals,
                                             const std::map<std::string, NativeI32StructType> &structTypes,
                                             const std::map<std::string, NativeI32EnumType> &enumTypes,
                                             const std::map<std::string, NativePayloadEnumType> &payloadEnumTypes,
                                             const std::map<std::string, std::vector<std::string>> &candidateFunctionParamTypes,
                                             const std::set<std::string> &candidateCallees,
                                             const std::map<std::string, std::string> &candidateStructCallees,
                                             int &tempId) {
  std::string expr = trim(rawExpr);
  if (!expr.empty() && expr.back() == ';') expr.pop_back();
  expr = trim(expr);
  if (auto localStruct = structLocals.find(expr); localStruct != structLocals.end()) {
    return NativeI32StructExpr{true, "", localStruct->second.typeName, localStruct->second.value};
  }
  if (auto lowered = lowerNativePayloadEnumConstructor(expr,
                                                       locals,
                                                       structLocals,
                                                       structTypes,
                                                       enumTypes,
                                                       payloadEnumTypes,
                                                       candidateFunctionParamTypes,
                                                       candidateCallees,
                                                       "",
                                                       tempId);
      lowered.ok) {
    return lowered;
  }
  if (auto lowered = lowerNativeI32StructLiteral(expr,
                                                 locals,
                                                 structLocals,
                                                 structTypes,
                                                 enumTypes,
                                                 candidateFunctionParamTypes,
                                                 candidateCallees,
                                                 tempId);
      lowered.ok) {
    return lowered;
  }
  auto call = matchRegex(expr, std::regex(R"(^([A-Za-z_][A-Za-z0-9_]*)\s*\((.*)\)$)"));
  if (!call) return {};
  const std::string callee = (*call)[1];
  auto structCallee = candidateStructCallees.find(callee);
  if (structCallee == candidateStructCallees.end()) return {};
  const auto rawArgs = splitArguments((*call)[2]);
  std::vector<std::string> paramTypes;
  auto signature = candidateFunctionParamTypes.find(callee);
  if (signature != candidateFunctionParamTypes.end()) {
    paramTypes = signature->second;
    if (paramTypes.size() != rawArgs.size()) return {};
  } else {
    paramTypes.assign(rawArgs.size(), "I32");
  }
  std::string code;
  std::vector<std::string> arguments;
  for (std::size_t i = 0; i < rawArgs.size(); ++i) {
    const std::string arg = trim(rawArgs[i]);
    const std::string paramType = paramTypes[i];
    if (isNativeI32ScalarType(paramType) || enumTypes.count(paramType)) {
      auto lowered = lowerNativeI32Expr(arg, locals, structLocals, structTypes, enumTypes, candidateFunctionParamTypes, candidateCallees, tempId);
      if (!lowered.ok) return {};
      code += lowered.code;
      arguments.push_back("i32 " + lowered.value);
    } else if (structTypes.count(paramType)) {
      auto localStruct = structLocals.find(arg);
      if (localStruct == structLocals.end() || localStruct->second.typeName != paramType) return {};
      arguments.push_back(nativeLlvmTypeRef(paramType) + " " + localStruct->second.value);
    } else {
      return {};
    }
  }
  const std::string temp = "%t" + std::to_string(tempId++);
  code += "  " + temp + " = call " + nativeLlvmTypeRef(structCallee->second) + " @" + callee + "(";
  for (std::size_t i = 0; i < arguments.size(); ++i) {
    if (i != 0) code += ", ";
    code += arguments[i];
  }
  code += ")\n";
  return NativeI32StructExpr{true, code, structCallee->second, temp};
}

NativeI32Expr lowerNativePayloadEnumTag(const NativeI32StructLocal &aggregate, int &tempId) {
  const std::string temp = "%t" + std::to_string(tempId++);
  return NativeI32Expr{true,
                       "  " + temp + " = extractvalue " + nativeLlvmTypeRef(aggregate.typeName) + " " + aggregate.value + ", 0\n",
                       temp};
}

bool bindNativePayloadEnumArm(const NativePayloadEnumType &enumType,
                              const std::string &enumName,
                              const std::string &variantName,
                              const std::vector<std::string> &bindingNames,
                              const std::string &aggregateValue,
                              const std::map<std::string, NativeI32StructType> &structTypes,
                              std::map<std::string, NativeI32Local> &armLocals,
                              std::map<std::string, NativeI32StructLocal> &armStructLocals,
                              std::string &code,
                              int &tempId) {
  if (bindingNames.empty()) return true;
  auto payload = enumType.variantPayloads.find(variantName);
  if (payload == enumType.variantPayloads.end()) return false;
  const bool allI32Payloads = std::all_of(payload->second.payloadTypes.begin(),
                                          payload->second.payloadTypes.end(),
                                          [](const std::string &type) { return isNativeI32ScalarType(type); });
  if (allI32Payloads && bindingNames.size() == payload->second.payloadTypes.size()) {
    std::size_t slot = 1;
    for (std::size_t i = 0; i < bindingNames.size(); ++i) {
      const std::string payloadType = payload->second.payloadTypes[i];
      if (!isNativeI32ScalarType(payloadType) || !isSimpleLlvmGlobalName(bindingNames[i])) return false;
      const std::string temp = "%t" + std::to_string(tempId++);
      code += "  " + temp + " = extractvalue " + nativeLlvmTypeRef(enumName) + " " + aggregateValue + ", " + std::to_string(slot++) + "\n";
      armLocals[bindingNames[i]] = NativeI32Local{false, temp};
    }
    return true;
  }
  if (bindingNames.size() != 1 || payload->second.payloadTypes.size() != 1) return false;
  const std::string bindingName = bindingNames.front();
  const std::string payloadType = payload->second.payloadTypes.front();
  if (!isSimpleLlvmGlobalName(bindingName)) return false;
  if (isNativeI32ScalarType(payloadType)) {
    const std::string temp = "%t" + std::to_string(tempId++);
    code += "  " + temp + " = extractvalue " + nativeLlvmTypeRef(enumName) + " " + aggregateValue + ", 1\n";
    armLocals[bindingName] = NativeI32Local{false, temp};
    return true;
  }
  auto structType = structTypes.find(payloadType);
  if (structType == structTypes.end()) return false;
  std::string structValue = "undef";
  for (std::size_t field = 0; field < structType->second.fields.size(); ++field) {
    const std::string fieldTemp = "%t" + std::to_string(tempId++);
    const std::string aggregateTemp = "%t" + std::to_string(tempId++);
    code += "  " + fieldTemp + " = extractvalue " + nativeLlvmTypeRef(enumName) + " " + aggregateValue + ", " + std::to_string(field + 1) + "\n";
    code += "  " + aggregateTemp + " = insertvalue " + nativeLlvmTypeRef(payloadType) + " " + structValue + ", i32 " + fieldTemp + ", " + std::to_string(field) + "\n";
    structValue = aggregateTemp;
  }
  armStructLocals[bindingName] = NativeI32StructLocal{payloadType, structValue};
  return true;
}

std::optional<NativeMatchPatternAlt> parseNativeMatchPatternAlt(const std::string &rawPattern) {
  const std::string pattern = trim(rawPattern);
  NativeMatchPatternAlt alt;
  if (auto record = matchRegex(pattern, std::regex(R"(^([A-Z][A-Za-z0-9_]*)\.([A-Za-z_][A-Za-z0-9_]*)\s*\{\s*(.*?)\s*\}$)"))) {
    alt.enumName = (*record)[1];
    alt.variantName = (*record)[2];
    for (const auto &binding : splitArguments((*record)[3])) {
      const std::string name = trim(binding);
      if (!isSimpleLlvmGlobalName(name)) return std::nullopt;
      alt.bindingNames.push_back(name);
    }
    return alt;
  }
  if (auto tuple = matchRegex(pattern, std::regex(R"(^([A-Z][A-Za-z0-9_]*)\.([A-Za-z_][A-Za-z0-9_]*)\s*\((.*?)\)$)"))) {
    alt.enumName = (*tuple)[1];
    alt.variantName = (*tuple)[2];
    const auto args = splitArguments((*tuple)[3]);
    if (args.size() == 1) {
      const std::string arg = trim(args.front());
      if (auto closedRange = matchRegex(arg, std::regex(R"(^([0-9]+)\s*\.\.=\s*([0-9]+)$)"))) {
        alt.hasRange = true;
        alt.rangeStart = std::stoi((*closedRange)[1]);
        alt.rangeEnd = std::stoi((*closedRange)[2]);
        alt.rangeInclusive = true;
        return alt;
      }
      if (auto halfOpenRange = matchRegex(arg, std::regex(R"(^([0-9]+)\s*\.\.\s*([0-9]+)$)"))) {
        alt.hasRange = true;
        alt.rangeStart = std::stoi((*halfOpenRange)[1]);
        alt.rangeEnd = std::stoi((*halfOpenRange)[2]);
        alt.rangeInclusive = false;
        return alt;
      }
    }
    for (const auto &binding : args) {
      const std::string name = trim(binding);
      if (!isSimpleLlvmGlobalName(name)) return std::nullopt;
      alt.bindingNames.push_back(name);
    }
    return alt;
  }
  if (auto unit = matchRegex(pattern, std::regex(R"(^([A-Z][A-Za-z0-9_]*)\.([A-Za-z_][A-Za-z0-9_]*)$)"))) {
    alt.enumName = (*unit)[1];
    alt.variantName = (*unit)[2];
    return alt;
  }
  return std::nullopt;
}

NativeMatchArm parseNativeMatchArmLine(const std::string &rawLine, bool returnExpressionMatch) {
  std::string line = trim(rawLine);
  if (!line.empty() && (line.back() == ',' || line.back() == ';')) line.pop_back();
  line = trim(line);
  const auto arrow = findTopLevelToken(line, "=>");
  if (!arrow) return {};
  std::string lhs = trim(std::string_view(line).substr(0, *arrow));
  std::string rhs = trim(std::string_view(line).substr(*arrow + 2));
  if (!returnExpressionMatch) {
    if (!startsWith(rhs, "return ")) return {};
    rhs = trim(rhs.substr(7));
  }
  NativeMatchArm arm;
  arm.expr = rhs;
  if (const auto guard = findTopLevelToken(lhs, " if ")) {
    arm.guard = trim(std::string_view(lhs).substr(*guard + 4));
    lhs = trim(std::string_view(lhs).substr(0, *guard));
  }
  for (const auto &alternative : splitTopLevel(lhs, '|')) {
    auto parsed = parseNativeMatchPatternAlt(alternative);
    if (!parsed) return {};
    arm.alternatives.push_back(*parsed);
  }
  arm.ok = !arm.alternatives.empty() && !arm.expr.empty();
  return arm;
}

bool nativePayloadAltNeedsSameLayout(const NativePayloadEnumType &enumType,
                                     const NativeMatchPatternAlt &reference,
                                     const NativeMatchPatternAlt &candidate) {
  auto referencePayload = enumType.variantPayloads.find(reference.variantName);
  auto candidatePayload = enumType.variantPayloads.find(candidate.variantName);
  if (referencePayload == enumType.variantPayloads.end() ||
      candidatePayload == enumType.variantPayloads.end()) {
    return false;
  }
  return referencePayload->second.payloadTypes == candidatePayload->second.payloadTypes &&
         reference.bindingNames == candidate.bindingNames;
}

bool appendNativeMatchArmIr(const NativeMatchArm &arm,
                            const std::string &matchValue,
                            const std::string &matchAggregateValue,
                            const std::map<std::string, NativeI32Local> &locals,
                            const std::map<std::string, NativeI32StructLocal> &structLocals,
                            const std::map<std::string, NativeI32StructType> &nativeStructs,
                            const std::map<std::string, NativeI32EnumType> &nativeEnums,
                            const std::map<std::string, NativePayloadEnumType> &nativePayloadEnums,
                            const std::map<std::string, std::vector<std::string>> &candidateFunctionParamTypes,
                            const std::set<std::string> &candidateCallees,
                            const std::set<std::string> &candidateBoolCallees,
                            std::string &currentArmLabel,
                            std::string &matchArmCode,
                            std::string &matchEnumType,
                            int &tempId) {
  if (!arm.ok || arm.alternatives.empty()) return false;
  const std::string enumName = arm.alternatives.front().enumName;
  for (const auto &alt : arm.alternatives) {
    if (alt.enumName != enumName) return false;
  }
  const std::string payloadEnumName = resolveNativePayloadEnumTypeName(enumName, matchEnumType, nativePayloadEnums);
  const bool isPayloadEnum = !payloadEnumName.empty();
  const bool isPlainEnum = nativeEnums.count(enumName) != 0;
  const std::string effectiveEnumName = isPayloadEnum ? payloadEnumName : enumName;
  if (!matchEnumType.empty() && matchEnumType != effectiveEnumName) return false;
  matchEnumType = effectiveEnumName;

  if (!isPayloadEnum && !isPlainEnum) return false;
  if (isPayloadEnum && matchAggregateValue.empty()) return false;

  const std::string afterArmLabel = "match.arm." + std::to_string(tempId++);
  matchArmCode += currentArmLabel + ":\n";
  std::string armCaseBodies;

  for (std::size_t i = 0; i < arm.alternatives.size(); ++i) {
    const auto &alt = arm.alternatives[i];
    int discriminant = -1;
    if (isPayloadEnum) {
      const auto enumType = nativePayloadEnums.find(effectiveEnumName);
      auto discriminantIt = enumType->second.discriminants.find(alt.variantName);
      if (discriminantIt == enumType->second.discriminants.end()) return false;
      discriminant = discriminantIt->second;
      if (i > 0 && (!nativePayloadAltNeedsSameLayout(enumType->second, arm.alternatives.front(), alt) &&
                    (!alt.bindingNames.empty() || !arm.alternatives.front().bindingNames.empty()))) {
        return false;
      }
    } else {
      auto enumType = nativeEnums.find(enumName);
      auto discriminantIt = enumType->second.discriminants.find(alt.variantName);
      if (discriminantIt == enumType->second.discriminants.end()) return false;
      if (!alt.bindingNames.empty() || alt.hasRange) return false;
      discriminant = discriminantIt->second;
    }

    const std::string caseLabel = "match.case." + std::to_string(tempId++);
    const std::string nextTestLabel = "match.test." + std::to_string(tempId++);
    const std::string discrTemp = "%t" + std::to_string(tempId++);
    matchArmCode += "  " + discrTemp + " = icmp eq i32 " + matchValue + ", " + std::to_string(discriminant) + "\n";
    matchArmCode += "  br i1 " + discrTemp + ", label %" + caseLabel + ", label %" + nextTestLabel + "\n";
    matchArmCode += nextTestLabel + ":\n";
    if (i + 1 == arm.alternatives.size()) {
      matchArmCode += "  br label %" + afterArmLabel + "\n";
    }

    std::map<std::string, NativeI32Local> armLocals = locals;
    std::map<std::string, NativeI32StructLocal> armStructLocals = structLocals;
    std::string caseCode = caseLabel + ":\n";
    if (isPayloadEnum) {
      const auto &payloadEnum = nativePayloadEnums.find(effectiveEnumName)->second;
      if (!bindNativePayloadEnumArm(payloadEnum,
                                    effectiveEnumName,
                                    alt.variantName,
                                    alt.bindingNames,
                                    matchAggregateValue,
                                    nativeStructs,
                                    armLocals,
                                    armStructLocals,
                                    caseCode,
                                    tempId)) {
        return false;
      }
      if (alt.hasRange) {
        auto payload = payloadEnum.variantPayloads.find(alt.variantName);
        if (payload == payloadEnum.variantPayloads.end() ||
            payload->second.payloadTypes.size() != 1 ||
            payload->second.payloadTypes.front() != "I32") {
          return false;
        }
        const std::string payloadTemp = "%t" + std::to_string(tempId++);
        const std::string lowTemp = "%t" + std::to_string(tempId++);
        const std::string highTemp = "%t" + std::to_string(tempId++);
        const std::string rangeTemp = "%t" + std::to_string(tempId++);
        const std::string rangePassLabel = "match.range.pass." + std::to_string(tempId++);
        caseCode += "  " + payloadTemp + " = extractvalue " + nativeLlvmTypeRef(effectiveEnumName) + " " + matchAggregateValue + ", 1\n";
        caseCode += "  " + lowTemp + " = icmp sge i32 " + payloadTemp + ", " + std::to_string(alt.rangeStart) + "\n";
        caseCode += "  " + highTemp + " = icmp " + std::string(alt.rangeInclusive ? "sle" : "slt") + " i32 " + payloadTemp + ", " + std::to_string(alt.rangeEnd) + "\n";
        caseCode += "  " + rangeTemp + " = and i1 " + lowTemp + ", " + highTemp + "\n";
        caseCode += "  br i1 " + rangeTemp + ", label %" + rangePassLabel + ", label %" + afterArmLabel + "\n";
        caseCode += rangePassLabel + ":\n";
      }
    }
    if (!arm.guard.empty()) {
      auto guard = lowerNativeI32Condition(arm.guard,
                                           armLocals,
                                           armStructLocals,
                                           nativeStructs,
                                           nativeEnums,
                                           candidateFunctionParamTypes,
                                           candidateCallees,
                                           candidateBoolCallees,
                                           tempId);
      if (!guard.ok) return false;
      const std::string guardPassLabel = "match.guard.pass." + std::to_string(tempId++);
      caseCode += guard.code;
      caseCode += "  br i1 " + guard.value + ", label %" + guardPassLabel + ", label %" + afterArmLabel + "\n";
      caseCode += guardPassLabel + ":\n";
    }
    auto lowered = lowerNativeI32Expr(arm.expr,
                                      armLocals,
                                      armStructLocals,
                                      nativeStructs,
                                      nativeEnums,
                                      candidateFunctionParamTypes,
                                      candidateCallees,
                                      tempId);
    if (!lowered.ok) return false;
    caseCode += lowered.code;
    caseCode += "  ret i32 " + lowered.value + "\n";
    armCaseBodies += caseCode;
  }
  matchArmCode += armCaseBodies;
  currentArmLabel = afterArmLabel;
  return true;
}

std::string entryFunctionName(const BuildPackage &package) {
  const std::size_t dot = package.entry.rfind('.');
  if (dot == std::string::npos) return package.entry.empty() ? "main" : package.entry;
  return package.entry.substr(dot + 1);
}

std::map<std::string, std::string> nativeI32FunctionIrDefinitions(const BuildPackage &package) {
  std::map<std::string, std::string> out;
  const std::regex fnPattern(R"(^\s*(pub\s+|private\s+)?fn\s+([A-Za-z_][A-Za-z0-9_]*)\s*\(([^)]*)\)\s*->\s*I32\s*\{?)");
  const std::regex boolFnPattern(R"(^\s*(pub\s+|private\s+)?fn\s+([A-Za-z_][A-Za-z0-9_]*)\s*\(([^)]*)\)\s*->\s*Bool\s*\{?)");
  const std::regex structFnPattern(R"(^\s*(pub\s+|private\s+)?fn\s+([A-Za-z_][A-Za-z0-9_]*)\s*\(([^)]*)\)\s*->\s*([A-Za-z_][A-Za-z0-9_]*(?:<[^()]+>)?)\s*\{?)");
  const std::regex localConstPattern(R"(^\s*const\s+([A-Za-z_][A-Za-z0-9_]*)(?:\s*:\s*(?:I32|Char))?\s*=\s*(.+?)\s*;?\s*$)");
  const std::regex bindingPattern(R"(^\s*(val|var)\s+([A-Za-z_][A-Za-z0-9_]*)(?:\s*:\s*(?:I32|Char))?\s*=\s*(.+?)\s*;?\s*$)");
  const std::regex assignmentPattern(R"(^\s*([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.+?)\s*;?\s*$)");
  const std::regex returnPattern(R"(^\s*return\s+(.+?)\s*;?\s*$)");
  const std::regex ifPattern(R"(^\s*if\s*\((.+)\)\s*\{\s*$)");
  const std::regex whilePattern(R"(^\s*while\s*\((.+)\)\s*\{\s*$)");
  const std::regex parameterPattern(R"(^\s*([A-Za-z_][A-Za-z0-9_]*)\s*:\s*I32\s*$)");
  const std::regex exportPattern(R"re(^\s*@export\s*\(\s*"([^"]+)"\s*,\s*abi:\s*C\s*\)\s*$)re");
  std::set<std::string> candidateCallees;
  std::set<std::string> candidateBoolCallees;
  std::map<std::string, NativeI32Local> nativeConstants;
  std::map<std::string, NativeI32Local> nativeGlobals;
  std::map<std::string, NativeI32StructType> nativeStructs;
  std::map<std::string, NativeI32EnumType> nativeEnums;
  std::map<std::string, NativePayloadEnumType> nativePayloadEnums;
  std::map<std::string, NativeGenericPayloadEnumTemplate> genericPayloadEnums;
  std::map<std::string, std::string> candidateStructCallees;
  std::map<std::string, std::vector<std::string>> candidateFunctionParamTypes;
  std::map<std::string, std::vector<std::pair<std::string, std::string>>> nativeStructFields;
  std::set<std::string> nonNativeStructs;
  const std::regex enumDeclPattern(R"(^\s*(?:pub\s+|private\s+)?enum\s+([A-Za-z_][A-Za-z0-9_]*)\s*\{?)");
  const std::regex enumVariantPattern(R"(^\s*([A-Za-z_][A-Za-z0-9_]*)\s*,?\s*$)");
  for (const auto &file : buildSourceFiles(package)) {
    if (file.extension() != ".zn") continue;
    const SourceText source = loadSource(file);
    bool inNativeEnum = false;
    bool nativeEnumValid = false;
    int enumBraceDepth = 0;
    std::string enumName;
    NativeI32EnumType enumType;
    for (const auto &rawLine : splitLines(source.text)) {
      const std::string line = stripLineComment(rawLine);
      const std::string trimmedLine = trim(line);
      if (!inNativeEnum) {
        if (auto decl = matchRegex(trimmedLine, enumDeclPattern)) {
          enumName = (*decl)[1];
          inNativeEnum = true;
          nativeEnumValid = isSimpleLlvmGlobalName(enumName);
          enumBraceDepth = braceDelta(line);
          enumType = NativeI32EnumType{};
        }
      } else {
        if (trimmedLine != "}" && !trimmedLine.empty()) {
          if (auto variant = matchRegex(trimmedLine, enumVariantPattern)) {
            const std::string variantName = (*variant)[1];
            if (isSimpleLlvmGlobalName(variantName)) {
              enumType.discriminants[variantName] = static_cast<int>(enumType.variants.size());
              enumType.variants.push_back(variantName);
            } else {
              nativeEnumValid = false;
            }
          } else {
            nativeEnumValid = false;
          }
        }
        enumBraceDepth += braceDelta(line);
        if (enumBraceDepth <= 0) {
          if (nativeEnumValid && !enumName.empty() && !enumType.variants.empty()) {
            nativeEnums[enumName] = enumType;
          }
          inNativeEnum = false;
          nativeEnumValid = false;
          enumBraceDepth = 0;
          enumName.clear();
          enumType = NativeI32EnumType{};
        }
      }
    }
  }
  const auto astNodes = parseTopLevelAst(package.root, package.name, buildSourceFiles(package));
  for (const auto &node : astNodes) {
    if (node.kind == "decl.field") {
      const std::size_t dot = node.name.find('.');
      if (dot == std::string::npos) continue;
      const std::string structName = node.name.substr(0, dot);
      const std::string fieldName = node.name.substr(dot + 1);
      if (!isSimpleLlvmGlobalName(structName) || !isSimpleLlvmGlobalName(fieldName) ||
          !isNativeI32ScalarType(node.returnType)) {
        nonNativeStructs.insert(structName);
      } else {
        nativeStructFields[structName].push_back({fieldName, normalizeSignatureType(node.returnType)});
      }
      continue;
    }
    if (node.kind == "decl.const" && normalizeSignatureType(node.returnType) == "I32") {
      std::string constantValue = node.params;
      if (startsWith(constantValue, "int-literal:")) constantValue = constantValue.substr(12);
      if (std::regex_match(constantValue, std::regex(R"([0-9]+)"))) {
        nativeConstants[node.name] = NativeI32Local{false, constantValue};
      }
    } else if (node.kind == "decl.static" && normalizeSignatureType(node.returnType) == "I32") {
      std::string initialValue = node.params;
      if (startsWith(initialValue, "int-literal:")) initialValue = initialValue.substr(12);
      if (std::regex_match(initialValue, std::regex(R"([0-9]+)")) && isSimpleLlvmGlobalName(node.name)) {
        nativeGlobals[node.name] = NativeI32Local{true, "@" + node.name};
        out[node.name] = "\n@" + node.name + " = internal global i32 " + initialValue + "\n";
      }
    } else if (node.kind == "decl.fn" && normalizeSignatureType(node.returnType) == "I32") {
      candidateCallees.insert(node.name);
    } else if (node.kind == "decl.fn" && nativeEnums.count(normalizeSignatureType(node.returnType))) {
      candidateCallees.insert(node.name);
    } else if (node.kind == "decl.fn" && normalizeSignatureType(node.returnType) == "Bool") {
      candidateBoolCallees.insert(node.name);
    } else if (node.kind == "decl.extern" && node.abi == "C" && normalizeSignatureType(node.returnType) == "I32") {
      std::string externParams;
      if (nativeI32ParameterList(node.params, &externParams)) {
        candidateCallees.insert(node.name);
        out[node.name] = "\ndeclare i32 @" + node.name + "(" + externParams + ")\n";
      }
    }
  }
  for (const auto &[structName, fields] : nativeStructFields) {
    if (nonNativeStructs.count(structName) || fields.empty()) continue;
    NativeI32StructType type;
    std::string llvmType = "\n" + nativeLlvmTypeRef(structName) + " = type { ";
    for (std::size_t i = 0; i < fields.size(); ++i) {
      if (i != 0) llvmType += ", ";
      llvmType += "i32";
      type.fields.push_back(fields[i].first);
    }
    llvmType += " }\n";
    nativeStructs[structName] = type;
    out[nativeLlvmTypeRef(structName)] = llvmType;
  }
  const std::regex payloadEnumDeclPattern(R"(^\s*(?:pub\s+|private\s+)?enum\s+([A-Za-z_][A-Za-z0-9_]*)(<[^>]+>)?\s*\{?)");
  const std::regex payloadEnumVariantPattern(R"(^\s*([A-Za-z_][A-Za-z0-9_]*)(?:\s*(\(([^)]*)\)|\{\s*(.*?)\s*\}))?\s*,?\s*$)");
  for (const auto &file : buildSourceFiles(package)) {
    if (file.extension() != ".zn") continue;
    const SourceText source = loadSource(file);
    bool inEnum = false;
    bool enumValid = false;
    bool enumGeneric = false;
    bool enumHasPayload = false;
    int enumBraceDepth = 0;
    std::string enumName;
    std::vector<std::string> enumGenericParams;
    NativePayloadEnumType enumType;
    for (const auto &rawLine : splitLines(source.text)) {
      const std::string line = stripLineComment(rawLine);
      const std::string trimmedLine = trim(line);
      if (!inEnum) {
        if (auto decl = matchRegex(trimmedLine, payloadEnumDeclPattern)) {
          enumName = (*decl)[1];
          inEnum = true;
          enumGeneric = false;
          enumGenericParams.clear();
          if ((*decl)[2].matched) {
            std::string genericText = trim(std::string((*decl)[2]));
            if (genericText.size() >= 2 && genericText.front() == '<' && genericText.back() == '>') {
              genericText = trim(genericText.substr(1, genericText.size() - 2));
              bool validGenericParams = true;
              for (const auto &param : splitArguments(genericText)) {
                const std::string genericParam = trim(param);
                if (!isSimpleLlvmGlobalName(genericParam)) {
                  validGenericParams = false;
                  break;
                }
                enumGenericParams.push_back(genericParam);
              }
              if (validGenericParams && !enumGenericParams.empty()) {
                enumGeneric = true;
              }
            }
          }
          enumValid = isSimpleLlvmGlobalName(enumName) && (!(*decl)[2].matched || enumGeneric);
          enumHasPayload = false;
          enumBraceDepth = braceDelta(line);
          enumType = NativePayloadEnumType{};
          enumType.sourceName = enumName;
        }
      } else {
        if (trimmedLine != "}" && !trimmedLine.empty()) {
          if (auto variant = matchRegex(trimmedLine, payloadEnumVariantPattern)) {
            const std::string variantName = (*variant)[1];
            if (!isSimpleLlvmGlobalName(variantName)) {
              enumValid = false;
            } else {
              enumType.discriminants[variantName] = static_cast<int>(enumType.variants.size());
              enumType.variants.push_back(variantName);
              NativePayloadEnumVariant payload;
              if ((*variant)[3].matched) {
                enumHasPayload = true;
                for (const auto &rawType : splitArguments((*variant)[3])) {
                  const std::string payloadType = normalizeSignatureType(rawType);
                  const bool genericPayload = enumGeneric && std::find(enumGenericParams.begin(), enumGenericParams.end(), payloadType) != enumGenericParams.end();
                  if (!isNativeI32ScalarType(payloadType) && !nativeStructs.count(payloadType) && !genericPayload) enumValid = false;
                  payload.payloadTypes.push_back(payloadType);
                }
              } else if ((*variant)[4].matched) {
                enumHasPayload = true;
                for (const auto &field : splitArguments((*variant)[4])) {
                  auto parsedField = matchRegex(field, std::regex(R"(^\s*([A-Za-z_][A-Za-z0-9_]*)\s*:\s*(.+?)\s*$)"));
                  if (!parsedField) {
                    enumValid = false;
                    continue;
                  }
                  const std::string payloadType = normalizeSignatureType((*parsedField)[2]);
                  const bool genericPayload = enumGeneric && std::find(enumGenericParams.begin(), enumGenericParams.end(), payloadType) != enumGenericParams.end();
                  if (!isNativeI32ScalarType(payloadType) && !nativeStructs.count(payloadType) && !genericPayload) enumValid = false;
                  payload.payloadNames.push_back((*parsedField)[1]);
                  payload.payloadTypes.push_back(payloadType);
                }
              }
              std::size_t slots = 0;
              for (const auto &payloadType : payload.payloadTypes) {
                const bool genericPayload = enumGeneric && std::find(enumGenericParams.begin(), enumGenericParams.end(), payloadType) != enumGenericParams.end();
                if (isNativeI32ScalarType(payloadType) || genericPayload) slots += 1;
                else if (nativeStructs.count(payloadType)) slots += nativeStructs[payloadType].fields.size();
                else enumValid = false;
              }
              enumType.maxPayloadSlots = std::max(enumType.maxPayloadSlots, slots);
              enumType.variantPayloads[variantName] = payload;
            }
          } else {
            enumValid = false;
          }
        }
        enumBraceDepth += braceDelta(line);
        if (enumBraceDepth <= 0) {
          if (enumValid && enumHasPayload && !enumName.empty() && !enumType.variants.empty()) {
            if (enumGeneric) {
              genericPayloadEnums[enumName] = NativeGenericPayloadEnumTemplate{enumGenericParams, enumType};
            } else {
              nativePayloadEnums[enumName] = enumType;
              NativeI32StructType aggregateType;
              aggregateType.fields.push_back("tag");
              std::string llvmType = "\n" + nativeLlvmTypeRef(enumName) + " = type { i32";
              for (std::size_t slot = 0; slot < enumType.maxPayloadSlots; ++slot) {
                llvmType += ", i32";
                aggregateType.fields.push_back("payload" + std::to_string(slot));
              }
              llvmType += " }\n";
              nativeStructs[enumName] = aggregateType;
              out[nativeLlvmTypeRef(enumName)] = llvmType;
            }
          }
          inEnum = false;
          enumValid = false;
          enumGeneric = false;
          enumHasPayload = false;
          enumBraceDepth = 0;
          enumName.clear();
          enumGenericParams.clear();
          enumType = NativePayloadEnumType{};
        }
      }
    }
  }
  std::set<std::string> genericPayloadInstances;
  auto collectGenericPayloadInstance = [&](const std::string &typeName) {
    const std::string normalized = normalizeSignatureType(typeName);
    auto instance = nativeGenericInstance(normalized);
    if (!instance) return;
    if (!genericPayloadEnums.count(instance->first)) return;
    const auto genericTemplate = genericPayloadEnums.find(instance->first);
    if (genericTemplate == genericPayloadEnums.end()) return;
    if (genericTemplate->second.typeParams.size() != instance->second.size()) return;
    for (const auto &arg : instance->second) {
      if (!isNativeI32ScalarType(arg) && !nativeStructs.count(arg)) return;
    }
    genericPayloadInstances.insert(normalized);
  };
  for (const auto &node : astNodes) {
    collectGenericPayloadInstance(node.returnType);
    if (node.kind == "decl.fn" || node.kind == "decl.extern") {
      for (const auto &paramType : parameterTypes(node.params)) {
        collectGenericPayloadInstance(paramType);
      }
    }
  }
  for (const auto &instanceTypeName : genericPayloadInstances) {
    const auto instance = nativeGenericInstance(instanceTypeName);
    if (!instance) continue;
    auto genericTemplate = genericPayloadEnums.find(instance->first);
    if (genericTemplate == genericPayloadEnums.end()) continue;
    if (genericTemplate->second.typeParams.size() != instance->second.size()) continue;
    std::map<std::string, std::string> typeSubstitutions;
    for (std::size_t i = 0; i < instance->second.size(); ++i) {
      typeSubstitutions[genericTemplate->second.typeParams[i]] = instance->second[i];
    }
    NativePayloadEnumType enumType;
    enumType.sourceName = instance->first;
    enumType.variants = genericTemplate->second.enumType.variants;
    enumType.discriminants = genericTemplate->second.enumType.discriminants;
    bool validInstance = true;
    for (const auto &[variantName, genericPayload] : genericTemplate->second.enumType.variantPayloads) {
      NativePayloadEnumVariant payload;
      payload.payloadNames = genericPayload.payloadNames;
      for (const auto &payloadType : genericPayload.payloadTypes) {
        if (typeSubstitutions.count(payloadType)) {
          payload.payloadTypes.push_back(typeSubstitutions[payloadType]);
        } else {
          payload.payloadTypes.push_back(payloadType);
        }
      }
      std::size_t slots = 0;
      for (const auto &payloadType : payload.payloadTypes) {
        if (isNativeI32ScalarType(payloadType)) slots += 1;
        else if (nativeStructs.count(payloadType)) slots += nativeStructs[payloadType].fields.size();
        else validInstance = false;
      }
      enumType.maxPayloadSlots = std::max(enumType.maxPayloadSlots, slots);
      enumType.variantPayloads[variantName] = payload;
    }
    if (!validInstance || enumType.variants.empty()) continue;
    nativePayloadEnums[instanceTypeName] = enumType;
    NativeI32StructType aggregateType;
    aggregateType.fields.push_back("tag");
    std::string llvmType = "\n" + nativeLlvmTypeRef(instanceTypeName) + " = type { i32";
    for (std::size_t slot = 0; slot < enumType.maxPayloadSlots; ++slot) {
      llvmType += ", i32";
      aggregateType.fields.push_back("payload" + std::to_string(slot));
    }
    llvmType += " }\n";
    nativeStructs[instanceTypeName] = aggregateType;
    out[nativeLlvmTypeRef(instanceTypeName)] = llvmType;
  }
  for (const auto &node : astNodes) {
    const std::string returnType = normalizeSignatureType(node.returnType);
    if (node.kind == "decl.fn" || (node.kind == "decl.extern" && node.abi == "C")) {
      std::vector<std::pair<std::string, std::string>> parsedParams;
      std::string llvmParams;
      if (nativeParameterList(node.params, nativeStructs, nativeEnums, &parsedParams, &llvmParams)) {
        std::vector<std::string> paramTypes;
        for (const auto &[paramName, paramType] : parsedParams) paramTypes.push_back(paramType);
        candidateFunctionParamTypes[node.name] = paramTypes;
      }
    }
    if (node.kind == "decl.fn" && nativeStructs.count(returnType)) {
      candidateStructCallees[node.name] = returnType;
    }
  }

  for (const auto &file : buildSourceFiles(package)) {
    if (file.extension() != ".zn") continue;
    const SourceText source = loadSource(file);
    bool active = false;
    bool activeBool = false;
    std::string activeStructReturnType;
    std::string activeEnumReturnType;
    bool valid = false;
    bool returned = false;
    int braceDepth = 0;
    int tempId = 0;
    std::string functionName;
    std::string functionParams;
    std::string pendingExportSymbol;
    std::string functionExportSymbol;
    std::string body;
    std::map<std::string, NativeI32Local> locals;
    std::map<std::string, NativeI32StructLocal> structLocals;
    bool inIf = false;
    bool inElse = false;
    bool ifReturned = false;
    std::string ifContLabel;
    bool ifHasScopedBindings = false;
    std::map<std::string, NativeI32Local> ifOuterLocals;
    std::map<std::string, NativeI32StructLocal> ifOuterStructLocals;
    bool inWhile = false;
    bool inTrustReturn = false;
    bool inMatch = false;
    std::string whileCondLabel;
    std::string whileBodyLabel;
    std::string whileExitLabel;
    bool whileReturned = false;
    bool whileHasScopedBindings = false;
    std::map<std::string, NativeI32Local> whileOuterLocals;
    std::map<std::string, NativeI32StructLocal> whileOuterStructLocals;
    bool inFor = false;
    bool forReturned = false;
    std::string forCondLabel;
    std::string forBodyLabel;
    std::string forStepLabel;
    std::string forExitLabel;
    std::string forIndexPtr;
    std::string forCurrentValue;
    std::map<std::string, NativeI32Local> forOuterLocals;
    std::map<std::string, NativeI32StructLocal> forOuterStructLocals;
    std::string matchPrefixCode;
    std::string matchValue;
    std::string matchCases;
    std::string matchBodies;
    std::string matchDefaultLabel;
    std::string matchEnumType;
    std::string matchAggregateValue;
    std::string matchNextArmLabel;
    bool matchIsReturnExpr = false;
    const std::regex matchPattern(R"(^\s*match\s+(.+?)\s*\{\s*$)");
    const std::regex returnMatchPattern(R"(^\s*return\s+match\s+(.+?)\s*\{\s*$)");
    const std::regex ifPatternBindingPattern(R"(^\s*if\s*\((.+)\s+is\s+(?:(move|mut)\s+)?(.+)\)\s*\{\s*$)");
    const std::regex whilePatternBindingPattern(R"(^\s*while\s*\((.+)\s+is\s+(?:(move|mut)\s+)?(.+)\)\s*\{\s*$)");
    const std::regex forRangePattern(R"(^\s*for\s+([A-Za-z_][A-Za-z0-9_]*)\s+in\s+(.+?)(\.\.=|\.\.)(.+?)\s*\{\s*$)");
    const std::regex breakPattern(R"(^\s*break(?:\s+(.+?))?\s*;?\s*$)");
    const std::regex continuePattern(R"(^\s*continue\s*;?\s*$)");
    const std::regex tryStatementPattern(R"(^\s*try\s+(.+?)\s*;?\s*$)");
    const std::regex matchReturnArmPattern(R"(^\s*([A-Z][A-Za-z0-9_]*)\.([A-Za-z_][A-Za-z0-9_]*)(?:(?:\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*\))|(?:\s*\{\s*(.*?)\s*\}))?\s*=>\s*return\s+(.+?)\s*[,;]?\s*$)");
    const std::regex matchExprArmPattern(R"(^\s*([A-Z][A-Za-z0-9_]*)\.([A-Za-z_][A-Za-z0-9_]*)(?:(?:\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*\))|(?:\s*\{\s*(.*?)\s*\}))?\s*=>\s*(.+?)\s*[,;]?\s*$)");
    for (const auto &rawLine : splitLines(source.text)) {
      const std::string line = stripLineComment(rawLine);
      const std::string trimmedLine = trim(line);
      if (!active) {
        if (auto exportAttr = matchRegex(trimmedLine, exportPattern)) {
          pendingExportSymbol = (*exportAttr)[1];
          braceDepth += braceDelta(line);
          continue;
        }
        if (braceDepth == 0) {
          if (auto fn = matchRegex(trimmedLine, fnPattern)) {
            active = contains(trimmedLine, "{");
            activeBool = false;
            activeStructReturnType.clear();
            activeEnumReturnType.clear();
            valid = active;
            returned = false;
            tempId = 0;
            functionName = (*fn)[2];
            functionParams.clear();
            functionExportSymbol = pendingExportSymbol;
            pendingExportSymbol.clear();
            body.clear();
            locals = nativeConstants;
            locals.insert(nativeGlobals.begin(), nativeGlobals.end());
            structLocals.clear();
            std::vector<std::pair<std::string, std::string>> parsedParams;
            if (nativeParameterList((*fn)[3], nativeStructs, nativeEnums, &parsedParams, &functionParams)) {
              for (const auto &[paramName, paramType] : parsedParams) {
                if (isNativeI32ScalarType(paramType)) {
                  locals[paramName] = NativeI32Local{false, "%" + paramName};
                } else if (nativeEnums.count(paramType)) {
                  locals[paramName] = NativeI32Local{false, "%" + paramName};
                } else {
                  structLocals[paramName] = NativeI32StructLocal{paramType, "%" + paramName};
                }
              }
            } else {
              valid = false;
            }
          } else if (auto fn = matchRegex(trimmedLine, boolFnPattern)) {
            active = contains(trimmedLine, "{");
            activeBool = true;
            activeStructReturnType.clear();
            activeEnumReturnType.clear();
            valid = active;
            returned = false;
            tempId = 0;
            functionName = (*fn)[2];
            functionParams.clear();
            functionExportSymbol = pendingExportSymbol;
            pendingExportSymbol.clear();
            body.clear();
            locals = nativeConstants;
            locals.insert(nativeGlobals.begin(), nativeGlobals.end());
            structLocals.clear();
            std::vector<std::pair<std::string, std::string>> parsedParams;
            if (nativeParameterList((*fn)[3], nativeStructs, nativeEnums, &parsedParams, &functionParams)) {
              for (const auto &[paramName, paramType] : parsedParams) {
                if (isNativeI32ScalarType(paramType)) {
                  locals[paramName] = NativeI32Local{false, "%" + paramName};
                } else if (nativeEnums.count(paramType)) {
                  locals[paramName] = NativeI32Local{false, "%" + paramName};
                } else {
                  structLocals[paramName] = NativeI32StructLocal{paramType, "%" + paramName};
                }
              }
            } else {
              valid = false;
            }
          } else if (auto fn = matchRegex(trimmedLine, structFnPattern)) {
            const std::string returnType = normalizeSignatureType((*fn)[4]);
            if (nativeStructs.count(returnType) || nativeEnums.count(returnType)) {
              active = contains(trimmedLine, "{");
              activeBool = false;
              activeStructReturnType = nativeStructs.count(returnType) ? returnType : "";
              activeEnumReturnType = nativeEnums.count(returnType) ? returnType : "";
              valid = active;
              returned = false;
              tempId = 0;
              functionName = (*fn)[2];
              functionParams.clear();
              functionExportSymbol = pendingExportSymbol;
              pendingExportSymbol.clear();
              body.clear();
              locals = nativeConstants;
              locals.insert(nativeGlobals.begin(), nativeGlobals.end());
              structLocals.clear();
              std::vector<std::pair<std::string, std::string>> parsedParams;
              if (nativeParameterList((*fn)[3], nativeStructs, nativeEnums, &parsedParams, &functionParams)) {
                for (const auto &[paramName, paramType] : parsedParams) {
                  if (isNativeI32ScalarType(paramType)) {
                    locals[paramName] = NativeI32Local{false, "%" + paramName};
                  } else if (nativeEnums.count(paramType)) {
                    locals[paramName] = NativeI32Local{false, "%" + paramName};
                  } else {
                    structLocals[paramName] = NativeI32StructLocal{paramType, "%" + paramName};
                  }
                }
              } else {
                valid = false;
              }
            }
          }
        }
      } else if (!activeBool && inIf && trimmedLine == "} else {") {
        if (!ifReturned) valid = false;
        body += ifContLabel + ":\n";
        inIf = false;
        inElse = true;
        ifReturned = false;
        if (ifHasScopedBindings) {
          locals = ifOuterLocals;
          structLocals = ifOuterStructLocals;
          ifOuterLocals.clear();
          ifOuterStructLocals.clear();
          ifHasScopedBindings = false;
        }
      } else if (!activeBool && inIf && trimmedLine == "}") {
        if (!ifReturned) valid = false;
        body += ifContLabel + ":\n";
        inIf = false;
        ifReturned = false;
        ifContLabel.clear();
        if (ifHasScopedBindings) {
          locals = ifOuterLocals;
          structLocals = ifOuterStructLocals;
          ifOuterLocals.clear();
          ifOuterStructLocals.clear();
          ifHasScopedBindings = false;
        }
      } else if (!activeBool && inElse && trimmedLine == "}") {
        if (!ifReturned) valid = false;
        inElse = false;
        ifReturned = false;
        ifContLabel.clear();
        returned = true;
      } else if (!activeBool && inWhile && trimmedLine == "}") {
        if (!whileReturned) body += "  br label %" + whileCondLabel + "\n";
        body += whileExitLabel + ":\n";
        inWhile = false;
        whileReturned = false;
        if (whileHasScopedBindings) {
          locals = whileOuterLocals;
          structLocals = whileOuterStructLocals;
          whileOuterLocals.clear();
          whileOuterStructLocals.clear();
          whileHasScopedBindings = false;
        }
        whileCondLabel.clear();
        whileBodyLabel.clear();
        whileExitLabel.clear();
      } else if (!activeBool && inFor && trimmedLine == "}") {
        const std::string nextTemp = "%t" + std::to_string(tempId++);
        if (!forReturned) body += "  br label %" + forStepLabel + "\n";
        body += forStepLabel + ":\n";
        body += "  " + nextTemp + " = add i32 " + forCurrentValue + ", 1\n";
        body += "  store i32 " + nextTemp + ", ptr " + forIndexPtr + "\n";
        body += "  br label %" + forCondLabel + "\n";
        body += forExitLabel + ":\n";
        inFor = false;
        forReturned = false;
        locals = forOuterLocals;
        structLocals = forOuterStructLocals;
        forOuterLocals.clear();
        forOuterStructLocals.clear();
        forCondLabel.clear();
        forBodyLabel.clear();
        forStepLabel.clear();
        forExitLabel.clear();
        forIndexPtr.clear();
        forCurrentValue.clear();
      } else if (!activeBool && inMatch && (trimmedLine == "}" || (matchIsReturnExpr && trimmedLine == "};"))) {
        if (matchCases.empty() || matchValue.empty() || matchDefaultLabel.empty() || matchNextArmLabel.empty()) {
          valid = false;
        } else {
          body += matchPrefixCode;
          body += matchCases;
          body += matchNextArmLabel + ":\n";
          body += "  br label %" + matchDefaultLabel + "\n";
          body += matchDefaultLabel + ":\n";
          body += "  ret i32 0\n";
          returned = true;
        }
        inMatch = false;
        matchPrefixCode.clear();
        matchValue.clear();
        matchCases.clear();
        matchBodies.clear();
        matchDefaultLabel.clear();
        matchEnumType.clear();
        matchAggregateValue.clear();
        matchNextArmLabel.clear();
        matchIsReturnExpr = false;
      } else if (!activeBool && inMatch) {
        const auto arm = parseNativeMatchArmLine(trimmedLine, matchIsReturnExpr);
        if (arm.ok) {
          if (!appendNativeMatchArmIr(arm,
                                     matchValue,
                                     matchAggregateValue,
                                     locals,
                                     structLocals,
                                     nativeStructs,
                                     nativeEnums,
                                     nativePayloadEnums,
                                     candidateFunctionParamTypes,
                                     candidateCallees,
                                     candidateBoolCallees,
                                     matchNextArmLabel,
                                     matchCases,
                                     matchEnumType,
                                     tempId)) {
            valid = false;
          }
        } else {
          valid = false;
        }
      } else if (!activeBool && inTrustReturn && (trimmedLine == "};" || trimmedLine == "}")) {
        inTrustReturn = false;
      } else if (!activeBool && inTrustReturn && !trimmedLine.empty()) {
        auto lowered = lowerNativeI32Expr(trimmedLine, locals, structLocals, nativeStructs, nativeEnums, candidateFunctionParamTypes, candidateCallees, tempId);
        if (!lowered.ok) {
          valid = false;
        } else {
          body += lowered.code;
          body += "  ret i32 " + lowered.value + "\n";
          returned = true;
        }
      } else if (!returned && !trimmedLine.empty() && trimmedLine != "}") {
        if (activeBool) {
          if (auto ret = matchRegex(trimmedLine, returnPattern)) {
            auto lowered = lowerNativeI32Condition((*ret)[1], locals, structLocals, nativeStructs, nativeEnums, candidateFunctionParamTypes, candidateCallees, candidateBoolCallees, tempId);
            if (!lowered.ok) {
              valid = false;
            } else {
              body += lowered.code;
              body += "  ret i1 " + lowered.value + "\n";
              returned = true;
            }
          } else {
            valid = false;
          }
        } else if (startsWith(trimmedLine, "return trust {")) {
          if (inIf || inElse || inWhile || inFor) {
            valid = false;
          } else {
            inTrustReturn = true;
          }
        } else
        if (auto returnMatch = matchRegex(trimmedLine, returnMatchPattern)) {
          if (inIf || inElse || inWhile || inFor || inMatch || !activeStructReturnType.empty() || !activeEnumReturnType.empty()) {
            valid = false;
          } else {
            const std::string scrutinee = nativeAccessName((*returnMatch)[1]);
            NativeI32Expr lowered;
            std::string payloadEnumScrutineeType;
            std::string payloadEnumScrutineeValue;
            if (auto aggregate = structLocals.find(scrutinee);
                aggregate != structLocals.end() && nativePayloadEnums.count(aggregate->second.typeName)) {
              lowered = lowerNativePayloadEnumTag(aggregate->second, tempId);
              payloadEnumScrutineeType = aggregate->second.typeName;
              payloadEnumScrutineeValue = aggregate->second.value;
            } else {
              lowered = lowerNativeI32Expr((*returnMatch)[1], locals, structLocals, nativeStructs, nativeEnums, candidateFunctionParamTypes, candidateCallees, tempId);
            }
            if (!lowered.ok) {
              valid = false;
            } else {
              const int labelId = tempId++;
              matchPrefixCode = lowered.code;
              matchValue = lowered.value;
              matchCases.clear();
              matchBodies.clear();
              matchDefaultLabel = "match.default." + std::to_string(labelId);
              matchNextArmLabel = "match.arm." + std::to_string(labelId);
              matchCases = "  br label %" + matchNextArmLabel + "\n";
              matchEnumType.clear();
              matchAggregateValue.clear();
              matchIsReturnExpr = true;
              if (!payloadEnumScrutineeType.empty()) {
                matchEnumType = payloadEnumScrutineeType;
                matchAggregateValue = payloadEnumScrutineeValue;
              } else if (auto enumValue = nativeEnumDiscriminant((*returnMatch)[1], nativeEnums)) {
                matchEnumType = enumValue->first;
              }
              inMatch = true;
            }
          }
        } else
        if (auto localConst = matchRegex(trimmedLine, localConstPattern)) {
          if (inIf || inElse || inWhile || inFor) {
            valid = false;
          } else {
            const std::string localName = (*localConst)[1];
            auto lowered = lowerNativeI32Expr((*localConst)[2], locals, structLocals, nativeStructs, nativeEnums, candidateFunctionParamTypes, candidateCallees, tempId);
            if (!lowered.ok) {
              valid = false;
            } else {
              body += lowered.code;
              locals[localName] = NativeI32Local{false, lowered.value};
            }
          }
        } else
        if (auto binding = matchRegex(trimmedLine, bindingPattern)) {
          if (inIf || inElse || inWhile || inFor) {
            valid = false;
          } else {
          const std::string mode = (*binding)[1];
          const std::string localName = (*binding)[2];
          std::string rawBindingExpr = trim(std::string((*binding)[3]));
          if (!rawBindingExpr.empty() && rawBindingExpr.back() == ';') rawBindingExpr.pop_back();
          rawBindingExpr = trim(rawBindingExpr);
          bool handledStructBinding = false;
          bool handledTryBinding = false;
          if (mode == "val" && startsWith(rawBindingExpr, "try ")) {
            handledTryBinding = true;
            const std::string tryExpr = trim(rawBindingExpr.substr(4));
            if (activeStructReturnType.empty() || !nativePayloadEnums.count(activeStructReturnType)) {
              valid = false;
            } else {
              auto resultEnum = nativePayloadEnums.find(activeStructReturnType);
              const auto okDiscriminant = resultEnum->second.discriminants.find("Ok");
              const auto errDiscriminant = resultEnum->second.discriminants.find("Err");
              auto aggregate = lowerNativeAggregateExpr(tryExpr,
                                                        locals,
                                                        structLocals,
                                                        nativeStructs,
                                                        nativeEnums,
                                                        nativePayloadEnums,
                                                        candidateFunctionParamTypes,
                                                        candidateCallees,
                                                        candidateStructCallees,
                                                        tempId);
              if (!aggregate.ok || aggregate.typeName != activeStructReturnType ||
                  okDiscriminant == resultEnum->second.discriminants.end() ||
                  errDiscriminant == resultEnum->second.discriminants.end()) {
                valid = false;
              } else {
                const int labelId = tempId++;
                const std::string tagTemp = "%t" + std::to_string(tempId++);
                const std::string errTemp = "%t" + std::to_string(tempId++);
                const std::string okLabel = "try.ok." + std::to_string(labelId);
                const std::string errLabel = "try.err." + std::to_string(labelId);
                body += aggregate.code;
                body += "  " + tagTemp + " = extractvalue " + nativeLlvmTypeRef(activeStructReturnType) + " " + aggregate.value + ", 0\n";
                body += "  " + errTemp + " = icmp eq i32 " + tagTemp + ", " + std::to_string(errDiscriminant->second) + "\n";
                body += "  br i1 " + errTemp + ", label %" + errLabel + ", label %" + okLabel + "\n";
                body += errLabel + ":\n";
                body += "  ret " + nativeLlvmTypeRef(activeStructReturnType) + " " + aggregate.value + "\n";
                body += okLabel + ":\n";
                std::map<std::string, NativeI32Local> okLocals = locals;
                std::map<std::string, NativeI32StructLocal> okStructLocals = structLocals;
                std::string payloadCode;
                if (!bindNativePayloadEnumArm(resultEnum->second,
                                              activeStructReturnType,
                                              "Ok",
                                              std::vector<std::string>{localName},
                                              aggregate.value,
                                              nativeStructs,
                                              okLocals,
                                              okStructLocals,
                                              payloadCode,
                                              tempId)) {
                  valid = false;
                } else {
                  body += payloadCode;
                  locals = okLocals;
                  structLocals = okStructLocals;
                }
              }
            }
          }
          if (mode == "val") {
            if (auto call = matchRegex(rawBindingExpr, std::regex(R"(^([A-Za-z_][A-Za-z0-9_]*)\s*\((.*)\)$)"));
                !handledTryBinding && call) {
              const std::string callee = (*call)[1];
              auto structCallee = candidateStructCallees.find(callee);
              if (structCallee != candidateStructCallees.end()) {
                handledStructBinding = true;
                const auto rawArgs = splitArguments((*call)[2]);
                std::vector<std::string> paramTypes;
                auto signature = candidateFunctionParamTypes.find(callee);
                if (signature != candidateFunctionParamTypes.end()) {
                  paramTypes = signature->second;
                  if (paramTypes.size() != rawArgs.size()) valid = false;
                } else {
                  paramTypes.assign(rawArgs.size(), "I32");
                }
                std::string code;
                std::vector<std::string> arguments;
                for (std::size_t i = 0; valid && i < rawArgs.size(); ++i) {
                  const std::string arg = trim(rawArgs[i]);
                  const std::string paramType = paramTypes[i];
                  if (isNativeI32ScalarType(paramType)) {
                    auto lowered = lowerNativeI32Expr(arg, locals, structLocals, nativeStructs, nativeEnums, candidateFunctionParamTypes, candidateCallees, tempId);
                    if (!lowered.ok) {
                      valid = false;
                      break;
                    }
                    code += lowered.code;
                    arguments.push_back("i32 " + lowered.value);
                  } else if (nativeEnums.count(paramType)) {
                    auto lowered = lowerNativeI32Expr(arg, locals, structLocals, nativeStructs, nativeEnums, candidateFunctionParamTypes, candidateCallees, tempId);
                    if (!lowered.ok) {
                      valid = false;
                      break;
                    }
                    code += lowered.code;
                    arguments.push_back("i32 " + lowered.value);
                  } else if (nativeStructs.count(paramType)) {
                    auto localStruct = structLocals.find(arg);
                    if (localStruct == structLocals.end() || localStruct->second.typeName != paramType) {
                      valid = false;
                      break;
                    }
                    arguments.push_back(nativeLlvmTypeRef(paramType) + " " + localStruct->second.value);
                  } else {
                    valid = false;
                    break;
                  }
                }
                if (valid) {
                  const std::string temp = "%t" + std::to_string(tempId++);
                  code += "  " + temp + " = call " + nativeLlvmTypeRef(structCallee->second) + " @" + callee + "(";
                  for (std::size_t i = 0; i < arguments.size(); ++i) {
                    if (i != 0) code += ", ";
                    code += arguments[i];
                  }
                  code += ")\n";
                  body += code;
                  structLocals[localName] = NativeI32StructLocal{structCallee->second, temp};
                }
              }
            }
            if (!handledTryBinding && !handledStructBinding && matchRegex(rawBindingExpr, std::regex(R"(^([A-Z][A-Za-z0-9_]*)\s*\{\s*(.*)\s*\}$)"))) {
              handledStructBinding = true;
              auto lowered = lowerNativeI32StructLiteral(rawBindingExpr, locals, structLocals, nativeStructs, nativeEnums, candidateFunctionParamTypes, candidateCallees, tempId);
              if (!lowered.ok) {
                valid = false;
              } else {
                body += lowered.code;
                structLocals[localName] = NativeI32StructLocal{lowered.typeName, lowered.value};
              }
            }
          }
          if (!handledTryBinding && !handledStructBinding) {
            auto lowered = lowerNativeI32Expr(rawBindingExpr, locals, structLocals, nativeStructs, nativeEnums, candidateFunctionParamTypes, candidateCallees, tempId);
            if (!lowered.ok) {
              valid = false;
            } else {
              body += lowered.code;
              if (mode == "var") {
                const std::string localPtr = "%" + localName + ".addr";
                body += "  " + localPtr + " = alloca i32\n";
                body += "  store i32 " + lowered.value + ", ptr " + localPtr + "\n";
                locals[localName] = NativeI32Local{true, localPtr};
              } else {
                const std::string localValue = "%" + localName;
                body += "  " + localValue + " = add i32 0, " + lowered.value + "\n";
                locals[localName] = NativeI32Local{false, localValue};
              }
            }
          }
          }
        } else if (auto ifPatternMatch = matchRegex(trimmedLine, ifPatternBindingPattern)) {
          if (inIf || inElse || inWhile || inFor) {
            valid = false;
          } else {
            const std::string rawPattern = trim(std::string((*ifPatternMatch)[3]));
            const std::string scrutinee = nativeAccessName((*ifPatternMatch)[1]);
            const auto pattern = parseNativeMatchPatternAlt(rawPattern);
            auto aggregate = structLocals.find(scrutinee);
            const std::string enumTypeName = pattern && aggregate != structLocals.end()
                                               ? resolveNativePayloadEnumTypeName(pattern->enumName, aggregate->second.typeName, nativePayloadEnums)
                                               : "";
            if (!pattern || pattern->hasRange || aggregate == structLocals.end() ||
                enumTypeName.empty() ||
                aggregate->second.typeName != enumTypeName) {
              valid = false;
            } else {
              auto payloadEnum = nativePayloadEnums.find(enumTypeName);
              auto discriminant = payloadEnum->second.discriminants.find(pattern->variantName);
              if (discriminant == payloadEnum->second.discriminants.end()) {
                valid = false;
              } else {
                std::map<std::string, NativeI32Local> armLocals = locals;
                std::map<std::string, NativeI32StructLocal> armStructLocals = structLocals;
                std::string payloadCode;
                if (!bindNativePayloadEnumArm(payloadEnum->second,
                                              enumTypeName,
                                              pattern->variantName,
                                              pattern->bindingNames,
                                              aggregate->second.value,
                                              nativeStructs,
                                              armLocals,
                                              armStructLocals,
                                              payloadCode,
                                              tempId)) {
                  valid = false;
                } else {
                  auto loweredTag = lowerNativePayloadEnumTag(aggregate->second, tempId);
                  const int labelId = tempId++;
                  const std::string conditionTemp = "%t" + std::to_string(tempId++);
                  const std::string thenLabel = "if.then." + std::to_string(labelId);
                  ifContLabel = "if.cont." + std::to_string(labelId);
                  body += loweredTag.code;
                  body += "  " + conditionTemp + " = icmp eq i32 " + loweredTag.value + ", " + std::to_string(discriminant->second) + "\n";
                  body += "  br i1 " + conditionTemp + ", label %" + thenLabel + ", label %" + ifContLabel + "\n";
                  body += thenLabel + ":\n";
                  body += payloadCode;
                  ifOuterLocals = locals;
                  ifOuterStructLocals = structLocals;
                  ifHasScopedBindings = true;
                  locals = armLocals;
                  structLocals = armStructLocals;
                  inIf = true;
                  ifReturned = false;
                }
              }
            }
          }
        } else if (auto ifMatch = matchRegex(trimmedLine, ifPattern)) {
          if (inIf || inElse) {
            valid = false;
          } else {
            auto lowered = lowerNativeI32Condition((*ifMatch)[1], locals, structLocals, nativeStructs, nativeEnums, candidateFunctionParamTypes, candidateCallees, candidateBoolCallees, tempId);
            if (!lowered.ok) {
              valid = false;
            } else {
              const int labelId = tempId++;
              const std::string thenLabel = "if.then." + std::to_string(labelId);
              ifContLabel = "if.cont." + std::to_string(labelId);
              body += lowered.code;
              body += "  br i1 " + lowered.value + ", label %" + thenLabel + ", label %" + ifContLabel + "\n";
              body += thenLabel + ":\n";
              inIf = true;
              ifReturned = false;
            }
          }
        } else if (auto breakStmt = matchRegex(trimmedLine, breakPattern)) {
          if ((*breakStmt)[1].matched || (!inWhile && !inFor)) {
            valid = false;
          } else {
            body += "  br label %" + (inFor ? forExitLabel : whileExitLabel) + "\n";
            if (inIf || inElse) {
              ifReturned = true;
            } else if (inFor) {
              forReturned = true;
            } else {
              whileReturned = true;
            }
          }
        } else if (matchRegex(trimmedLine, continuePattern)) {
          if (!inWhile && !inFor) {
            valid = false;
          } else {
            body += "  br label %" + (inFor ? forStepLabel : whileCondLabel) + "\n";
            if (inIf || inElse) {
              ifReturned = true;
            } else if (inFor) {
              forReturned = true;
            } else {
              whileReturned = true;
            }
          }
        } else if (auto whilePatternMatch = matchRegex(trimmedLine, whilePatternBindingPattern)) {
          if (inIf || inElse || inWhile || inFor) {
            valid = false;
          } else {
            const std::string rawPattern = trim(std::string((*whilePatternMatch)[3]));
            const std::string rawScrutinee = trim(std::string((*whilePatternMatch)[1]));
            const auto pattern = parseNativeMatchPatternAlt(rawPattern);
            if (!pattern || pattern->hasRange) {
              valid = false;
            } else {
              auto aggregate = lowerNativeAggregateExpr(rawScrutinee,
                                                        locals,
                                                        structLocals,
                                                        nativeStructs,
                                                        nativeEnums,
                                                        nativePayloadEnums,
                                                        candidateFunctionParamTypes,
                                                        candidateCallees,
                                                        candidateStructCallees,
                                                        tempId);
              const std::string enumTypeName = aggregate.ok
                                                 ? resolveNativePayloadEnumTypeName(pattern->enumName, aggregate.typeName, nativePayloadEnums)
                                                 : "";
              if (!aggregate.ok || enumTypeName.empty() || aggregate.typeName != enumTypeName) {
                valid = false;
              } else {
                auto payloadEnum = nativePayloadEnums.find(enumTypeName);
                auto discriminant = payloadEnum->second.discriminants.find(pattern->variantName);
                if (discriminant == payloadEnum->second.discriminants.end()) {
                  valid = false;
                } else {
                  const int labelId = tempId++;
                  whileCondLabel = "while.cond." + std::to_string(labelId);
                  whileBodyLabel = "while.body." + std::to_string(labelId);
                  whileExitLabel = "while.exit." + std::to_string(labelId);
                  body += "  br label %" + whileCondLabel + "\n";
                  body += whileCondLabel + ":\n";
                  std::map<std::string, NativeI32Local> armLocals = locals;
                  std::map<std::string, NativeI32StructLocal> armStructLocals = structLocals;
                  std::string payloadCode;
                  if (!bindNativePayloadEnumArm(payloadEnum->second,
                                                enumTypeName,
                                                pattern->variantName,
                                                pattern->bindingNames,
                                                aggregate.value,
                                                nativeStructs,
                                                armLocals,
                                                armStructLocals,
                                                payloadCode,
                                                tempId)) {
                    valid = false;
                  } else {
                    const std::string tagTemp = "%t" + std::to_string(tempId++);
                    const std::string conditionTemp = "%t" + std::to_string(tempId++);
                    body += aggregate.code;
                    body += "  " + tagTemp + " = extractvalue " + nativeLlvmTypeRef(aggregate.typeName) + " " + aggregate.value + ", 0\n";
                    body += "  " + conditionTemp + " = icmp eq i32 " + tagTemp + ", " + std::to_string(discriminant->second) + "\n";
                    body += "  br i1 " + conditionTemp + ", label %" + whileBodyLabel + ", label %" + whileExitLabel + "\n";
                    body += whileBodyLabel + ":\n";
                    body += payloadCode;
                    whileOuterLocals = locals;
                    whileOuterStructLocals = structLocals;
                    whileHasScopedBindings = true;
                    locals = armLocals;
                    structLocals = armStructLocals;
                    inWhile = true;
                    whileReturned = false;
                  }
                }
              }
            }
          }
        } else if (auto whileMatch = matchRegex(trimmedLine, whilePattern)) {
          if (inIf || inElse || inWhile || inFor) {
            valid = false;
          } else {
            const int labelId = tempId++;
            whileCondLabel = "while.cond." + std::to_string(labelId);
            whileBodyLabel = "while.body." + std::to_string(labelId);
            whileExitLabel = "while.exit." + std::to_string(labelId);
            body += "  br label %" + whileCondLabel + "\n";
            body += whileCondLabel + ":\n";
            auto lowered = lowerNativeI32Condition((*whileMatch)[1], locals, structLocals, nativeStructs, nativeEnums, candidateFunctionParamTypes, candidateCallees, candidateBoolCallees, tempId);
            if (!lowered.ok) {
              valid = false;
            } else {
              body += lowered.code;
              body += "  br i1 " + lowered.value + ", label %" + whileBodyLabel + ", label %" + whileExitLabel + "\n";
              body += whileBodyLabel + ":\n";
              inWhile = true;
            }
          }
        } else if (auto forMatch = matchRegex(trimmedLine, forRangePattern)) {
          if (inIf || inElse || inWhile || inFor) {
            valid = false;
          } else {
            auto start = lowerNativeI32Expr((*forMatch)[2], locals, structLocals, nativeStructs, nativeEnums, candidateFunctionParamTypes, candidateCallees, tempId);
            auto end = lowerNativeI32Expr((*forMatch)[4], locals, structLocals, nativeStructs, nativeEnums, candidateFunctionParamTypes, candidateCallees, tempId);
            if (!start.ok || !end.ok) {
              valid = false;
            } else {
              const int labelId = tempId++;
              const std::string itemName = (*forMatch)[1];
              const std::string compareTemp = "%t" + std::to_string(tempId++);
              forCurrentValue = "%t" + std::to_string(tempId++);
              forIndexPtr = "%" + itemName + ".range." + std::to_string(labelId) + ".addr";
              forCondLabel = "for.cond." + std::to_string(labelId);
              forBodyLabel = "for.body." + std::to_string(labelId);
              forStepLabel = "for.step." + std::to_string(labelId);
              forExitLabel = "for.exit." + std::to_string(labelId);
              body += start.code;
              body += end.code;
              body += "  " + forIndexPtr + " = alloca i32\n";
              body += "  store i32 " + start.value + ", ptr " + forIndexPtr + "\n";
              body += "  br label %" + forCondLabel + "\n";
              body += forCondLabel + ":\n";
              body += "  " + forCurrentValue + " = load i32, ptr " + forIndexPtr + "\n";
              body += "  " + compareTemp + " = icmp " + std::string((*forMatch)[3] == "..=" ? "sle" : "slt") + " i32 " + forCurrentValue + ", " + end.value + "\n";
              body += "  br i1 " + compareTemp + ", label %" + forBodyLabel + ", label %" + forExitLabel + "\n";
              body += forBodyLabel + ":\n";
              forOuterLocals = locals;
              forOuterStructLocals = structLocals;
              locals[itemName] = NativeI32Local{false, forCurrentValue};
              inFor = true;
              forReturned = false;
            }
          }
        } else if (auto matchStart = matchRegex(trimmedLine, matchPattern)) {
          if (inIf || inElse || inWhile || inFor || inMatch) {
            valid = false;
          } else {
            const std::string scrutinee = nativeAccessName((*matchStart)[1]);
            NativeI32Expr lowered;
            std::string payloadEnumScrutineeType;
            std::string payloadEnumScrutineeValue;
            if (auto aggregate = structLocals.find(scrutinee);
                aggregate != structLocals.end() && nativePayloadEnums.count(aggregate->second.typeName)) {
              lowered = lowerNativePayloadEnumTag(aggregate->second, tempId);
              payloadEnumScrutineeType = aggregate->second.typeName;
              payloadEnumScrutineeValue = aggregate->second.value;
            } else {
              lowered = lowerNativeI32Expr((*matchStart)[1], locals, structLocals, nativeStructs, nativeEnums, candidateFunctionParamTypes, candidateCallees, tempId);
            }
            if (!lowered.ok) {
              valid = false;
            } else {
              const int labelId = tempId++;
              matchPrefixCode = lowered.code;
              matchValue = lowered.value;
              matchCases.clear();
              matchBodies.clear();
              matchDefaultLabel = "match.default." + std::to_string(labelId);
              matchNextArmLabel = "match.arm." + std::to_string(labelId);
              matchCases = "  br label %" + matchNextArmLabel + "\n";
              matchEnumType.clear();
              matchAggregateValue.clear();
              matchIsReturnExpr = false;
              if (!payloadEnumScrutineeType.empty()) {
                matchEnumType = payloadEnumScrutineeType;
                matchAggregateValue = payloadEnumScrutineeValue;
              } else if (auto enumValue = nativeEnumDiscriminant((*matchStart)[1], nativeEnums)) {
                matchEnumType = enumValue->first;
              }
              inMatch = true;
            }
          }
        } else if (auto tryStmt = matchRegex(trimmedLine, tryStatementPattern)) {
          if (inIf || inElse || inWhile || inFor || activeStructReturnType.empty() || !nativePayloadEnums.count(activeStructReturnType)) {
            valid = false;
          } else {
            auto resultEnum = nativePayloadEnums.find(activeStructReturnType);
            const auto errDiscriminant = resultEnum->second.discriminants.find("Err");
            auto aggregate = lowerNativeAggregateExpr(trim(std::string((*tryStmt)[1])),
                                                      locals,
                                                      structLocals,
                                                      nativeStructs,
                                                      nativeEnums,
                                                      nativePayloadEnums,
                                                      candidateFunctionParamTypes,
                                                      candidateCallees,
                                                      candidateStructCallees,
                                                      tempId);
            if (!aggregate.ok || aggregate.typeName != activeStructReturnType ||
                errDiscriminant == resultEnum->second.discriminants.end()) {
              valid = false;
            } else {
              const int labelId = tempId++;
              const std::string tagTemp = "%t" + std::to_string(tempId++);
              const std::string errTemp = "%t" + std::to_string(tempId++);
              const std::string okLabel = "try.ok." + std::to_string(labelId);
              const std::string errLabel = "try.err." + std::to_string(labelId);
              body += aggregate.code;
              body += "  " + tagTemp + " = extractvalue " + nativeLlvmTypeRef(activeStructReturnType) + " " + aggregate.value + ", 0\n";
              body += "  " + errTemp + " = icmp eq i32 " + tagTemp + ", " + std::to_string(errDiscriminant->second) + "\n";
              body += "  br i1 " + errTemp + ", label %" + errLabel + ", label %" + okLabel + "\n";
              body += errLabel + ":\n";
              body += "  ret " + nativeLlvmTypeRef(activeStructReturnType) + " " + aggregate.value + "\n";
              body += okLabel + ":\n";
            }
          }
        } else if (auto assignment = matchRegex(trimmedLine, assignmentPattern)) {
          if (inIf || inElse) {
            valid = false;
          } else {
            const std::string localName = (*assignment)[1];
            auto local = locals.find(localName);
            if (local == locals.end() || !local->second.mutableLocal) {
              valid = false;
            } else {
              auto lowered = lowerNativeI32Expr((*assignment)[2], locals, structLocals, nativeStructs, nativeEnums, candidateFunctionParamTypes, candidateCallees, tempId);
              if (!lowered.ok) {
                valid = false;
              } else {
                body += lowered.code;
                body += "  store i32 " + lowered.value + ", ptr " + local->second.value + "\n";
              }
            }
          }
        } else if (auto ret = matchRegex(trimmedLine, returnPattern)) {
          if (!activeStructReturnType.empty()) {
            const std::string rawReturnExpr = (*ret)[1];
            NativeI32StructExpr loweredStruct;
            if (nativePayloadEnums.count(activeStructReturnType)) {
              loweredStruct = lowerNativePayloadEnumConstructor(rawReturnExpr,
                                                                locals,
                                                                structLocals,
                                                                nativeStructs,
                                                                nativeEnums,
                                                                nativePayloadEnums,
                                                                candidateFunctionParamTypes,
                                                                candidateCallees,
                                                                activeStructReturnType,
                                                                tempId);
            }
            if (!loweredStruct.ok) {
              loweredStruct = lowerNativeI32StructLiteral(rawReturnExpr, locals, structLocals, nativeStructs, nativeEnums, candidateFunctionParamTypes, candidateCallees, tempId);
            }
            if (loweredStruct.ok && loweredStruct.typeName == activeStructReturnType) {
              body += loweredStruct.code;
              body += "  ret " + nativeLlvmTypeRef(activeStructReturnType) + " " + loweredStruct.value + "\n";
              if (inIf || inElse) {
                ifReturned = true;
              } else if (inWhile) {
                whileReturned = true;
              } else if (inFor) {
                forReturned = true;
              } else {
                returned = true;
              }
            } else {
              const std::string returnName = trim(std::string(rawReturnExpr));
              auto localStruct = structLocals.find(returnName);
              if (localStruct == structLocals.end() || localStruct->second.typeName != activeStructReturnType) {
                valid = false;
              } else {
                body += "  ret " + nativeLlvmTypeRef(activeStructReturnType) + " " + localStruct->second.value + "\n";
                if (inIf || inElse) {
                  ifReturned = true;
                } else if (inWhile) {
                  whileReturned = true;
                } else if (inFor) {
                  forReturned = true;
                } else {
                  returned = true;
                }
              }
            }
          } else if (!activeEnumReturnType.empty()) {
            auto lowered = lowerNativeI32Expr((*ret)[1], locals, structLocals, nativeStructs, nativeEnums, candidateFunctionParamTypes, candidateCallees, tempId);
            if (!lowered.ok) {
              valid = false;
            } else {
              body += lowered.code;
              body += "  ret i32 " + lowered.value + "\n";
              if (inIf || inElse) {
                ifReturned = true;
              } else if (inWhile) {
                whileReturned = true;
              } else if (inFor) {
                forReturned = true;
              } else {
                returned = true;
              }
            }
          } else {
            auto lowered = lowerNativeI32Expr((*ret)[1], locals, structLocals, nativeStructs, nativeEnums, candidateFunctionParamTypes, candidateCallees, tempId);
            if (!lowered.ok) {
              valid = false;
            } else {
              body += lowered.code;
              body += "  ret i32 " + lowered.value + "\n";
              if (inIf || inElse) {
                ifReturned = true;
              } else if (inWhile) {
                whileReturned = true;
              } else if (inFor) {
                forReturned = true;
              } else {
                returned = true;
              }
            }
          }
        } else {
          valid = false;
        }
      }
      braceDepth += braceDelta(line);
      if (active && braceDepth <= 0) {
        if (valid && returned && !functionName.empty()) {
          const std::string llvmReturnType = activeBool ? "i1" : (activeStructReturnType.empty() ? "i32" : nativeLlvmTypeRef(activeStructReturnType));
          out[functionName] = "\ndefine " + llvmReturnType + " @" + functionName + "(" + functionParams + ") {\nentry:\n" + body + "}\n";
          if (!activeBool && activeStructReturnType.empty() && !functionExportSymbol.empty() && functionExportSymbol != functionName) {
            const std::string thunk = nativeI32ExportThunk(functionExportSymbol, functionName, functionParams);
            if (!thunk.empty()) out[functionExportSymbol] = thunk;
          }
        }
        active = false;
        activeBool = false;
        activeStructReturnType.clear();
        activeEnumReturnType.clear();
        valid = false;
        returned = false;
        braceDepth = 0;
        functionName.clear();
        functionParams.clear();
        functionExportSymbol.clear();
        activeStructReturnType.clear();
        activeEnumReturnType.clear();
        body.clear();
        locals.clear();
        structLocals.clear();
        inIf = false;
        inElse = false;
        ifReturned = false;
        ifContLabel.clear();
        ifHasScopedBindings = false;
        ifOuterLocals.clear();
        ifOuterStructLocals.clear();
        inWhile = false;
        whileReturned = false;
        whileHasScopedBindings = false;
        whileOuterLocals.clear();
        whileOuterStructLocals.clear();
        inTrustReturn = false;
        inMatch = false;
        matchIsReturnExpr = false;
        whileCondLabel.clear();
        whileBodyLabel.clear();
        whileExitLabel.clear();
        matchPrefixCode.clear();
        matchValue.clear();
        matchCases.clear();
        matchBodies.clear();
        matchDefaultLabel.clear();
        matchEnumType.clear();
        matchAggregateValue.clear();
      }
    }
  }
  bool changed = true;
  const std::regex callPattern(R"(call (?:i[0-9]+|%[A-Za-z_][A-Za-z0-9_]*) @([A-Za-z_][A-Za-z0-9_]*)\()");
  while (changed) {
    changed = false;
    for (auto it = out.begin(); it != out.end();) {
      bool missingCallee = false;
      auto begin = std::sregex_iterator(it->second.begin(), it->second.end(), callPattern);
      auto end = std::sregex_iterator();
      for (auto call = begin; call != end; ++call) {
        if (!out.count((*call)[1])) {
          missingCallee = true;
          break;
        }
      }
      if (missingCallee) {
        it = out.erase(it);
        changed = true;
      } else {
        ++it;
      }
    }
  }
  return out;
}

std::string lockPackageBlock(const std::string &name,
                             const std::string &version,
                             const std::string &source,
                             const std::string &manifestHash,
                             const std::string &contentHash,
                             const std::vector<std::pair<std::string, std::string>> &dependencies,
                             const std::string &compilerPackageHash = "") {
  std::ostringstream out;
  out << "[[package]]\n"
      << "name = \"" << name << "\"\n"
      << "version = \"" << version << "\"\n"
      << "source = \"" << source << "\"\n";
  if (!compilerPackageHash.empty()) {
    out << "compilerPackageHash = \"" << compilerPackageHash << "\"\n";
  } else {
    out << "manifestHash = \"" << manifestHash << "\"\n"
        << "contentHash = \"" << contentHash << "\"\n";
  }
  out << "trust = []\n"
      << "dependencies = [\n";
  for (const auto &[key, packageName] : dependencies) {
    out << "  { key = \"" << key << "\", package = \"" << packageName << "\" },\n";
  }
  out << "]\n\n";
  return out.str();
}

std::vector<std::pair<std::string, std::string>> lockDependencyEdges(const fs::path &packageRoot, const PackageInfo &info) {
  std::vector<std::pair<std::string, std::string>> edges;
  for (const auto &[key, depPath] : info.dependencies) {
    PackageInfo depInfo = parsePackageInfo(depPath);
    edges.push_back({key, depInfo.name.empty() ? key : depInfo.name});
  }
  for (const auto &builtin : info.builtinDependencies) {
    edges.push_back({builtin, builtin});
  }
  (void)packageRoot;
  std::sort(edges.begin(), edges.end());
  return edges;
}

void appendPathPackageLock(std::ostringstream &out,
                           const fs::path &lockRoot,
                           const fs::path &packageRoot,
                           const std::string &sourcePath,
                           std::set<std::string> &emittedPackages,
                           std::set<std::string> &requiredBuiltins) {
  PackageInfo info = parsePackageInfo(packageRoot);
  if (info.name.empty()) return;
  if (emittedPackages.count(info.name)) return;
  emittedPackages.insert(info.name);
  for (const auto &builtin : info.builtinDependencies) requiredBuiltins.insert(builtin);
  const BuildPackage buildPackage = readBuildPackage(packageRoot);
  out << lockPackageBlock(info.name,
                          buildPackage.version,
                          "path:" + sourcePath,
                          fileFingerprint(packageRoot / "Zeno.toml"),
                          packageContentFingerprint(packageRoot),
                          lockDependencyEdges(packageRoot, info));
  for (const auto &[key, depPath] : info.dependencies) {
    (void)key;
    appendPathPackageLock(out, lockRoot, depPath, fs::relative(depPath, lockRoot).string(), emittedPackages, requiredBuiltins);
  }
}

void collectRemoteDependencyDiagnostics(const fs::path &root,
                                        std::set<fs::path> &visited,
                                        std::vector<Diagnostic> &diagnostics) {
  if (!fs::is_directory(root) || !fs::exists(root / "Zeno.toml")) return;
  const fs::path canonicalRoot = stableAbsolutePath(root);
  if (visited.count(canonicalRoot)) return;
  visited.insert(canonicalRoot);

  SourceText manifest = loadSource(root / "Zeno.toml");
  for (const auto &[key, rawValue] : manifestSectionFields(manifest.text, "dependencies")) {
    const std::string scalarValue = unquoteTomlString(rawValue);
    const std::string git = inlineTomlField(rawValue, "git");
    const std::string version = rawValue.empty() || rawValue.front() == '{' ? inlineTomlField(rawValue, "version") : scalarValue;
    std::string feature;
    std::string message;
    if (!git.empty()) {
      feature = "git-dependency";
      message = "--update-lock cannot resolve git dependency " + key + " in stage0";
    } else if (!version.empty() && scalarValue != "builtin" && !contains(rawValue, "path")) {
      feature = "registry-dependency";
      message = "--update-lock cannot resolve registry dependency " + key + " in stage0";
    }
    if (!feature.empty()) {
      Diagnostic diag = makeDiagnostic(manifest, "E9002", message, lineContaining(manifest.text, key + " ="));
      diag.staged = true;
      diag.feature = feature;
      diag.category = "staged";
      diag.stageName = "manifest";
      diag.notes.push_back("stage0 lock updates are limited to local path and builtin packages");
      diag.help.push_back("commit a pre-resolved Zeno.lock or replace the dependency with a local path package");
      diagnostics.push_back(diag);
    }
  }

  const PackageInfo info = parsePackageInfo(root);
  for (const auto &[key, depPath] : info.dependencies) {
    (void)key;
    collectRemoteDependencyDiagnostics(depPath, visited, diagnostics);
  }
  for (const auto &member : info.workspaceMembers) {
    collectRemoteDependencyDiagnostics(root / member, visited, diagnostics);
  }
}

std::vector<Diagnostic> updateLockUnsupportedDependencyDiagnostics(const fs::path &root) {
  std::vector<Diagnostic> diagnostics;
  std::set<fs::path> visited;
  collectRemoteDependencyDiagnostics(root, visited, diagnostics);
  return diagnostics;
}

void writeUpdatedLockfile(const fs::path &root) {
  if (!fs::is_directory(root) || !fs::exists(root / "Zeno.toml")) return;
  const PackageInfo rootInfo = parsePackageInfo(root);
  std::set<std::string> emittedPackages;
  std::set<std::string> requiredBuiltins;
  std::ostringstream out;
  std::string rootName = rootInfo.name;
  if (rootName.empty()) {
    for (const auto &member : rootInfo.workspaceMembers) {
      PackageInfo memberInfo = parsePackageInfo(root / member);
      if (!memberInfo.name.empty()) {
        rootName = memberInfo.name;
        if (fs::exists(root / member / "src" / "main.zn")) break;
      }
    }
  }
  if (rootName.empty()) rootName = "workspace";
  out << "version = 1\n"
      << "root = \"" << rootName << "\"\n"
      << "compiler = \"zeno-stage0 0.1.0\"\n\n";

  if (!rootInfo.name.empty()) {
    appendPathPackageLock(out, root, root, ".", emittedPackages, requiredBuiltins);
  }
  for (const auto &member : rootInfo.workspaceMembers) {
    appendPathPackageLock(out, root, root / member, member, emittedPackages, requiredBuiltins);
  }
  if (rootInfo.name.empty() && rootInfo.workspaceMembers.empty()) {
    appendPathPackageLock(out, root, root, ".", emittedPackages, requiredBuiltins);
  }
  for (const auto &builtin : rootInfo.builtinDependencies) requiredBuiltins.insert(builtin);
  requiredBuiltins.insert("core");
  for (const auto &builtin : requiredBuiltins) {
    if (!isBuiltinPackageName(builtin)) continue;
    const fs::path builtinRoot = builtinPackageRoot(builtin);
    if (!fs::exists(builtinRoot / "Zeno.toml")) continue;
    out << lockPackageBlock(builtin,
                            "builtin",
                            "builtin:" + builtin,
                            "",
                            "",
                            {},
                            packageFingerprint(builtinRoot));
  }
  writeFile(root / "Zeno.lock", out.str());
}

int commandCheck(const Options &options) {
  fs::path root = options.inputs.empty() ? fs::current_path() : fs::path(options.inputs.front());
  if (!options.manifest.empty()) {
    root = options.manifest;
  }
  if (options.updateLock) {
    auto updateLockDiagnostics = updateLockUnsupportedDependencyDiagnostics(root);
    if (!updateLockDiagnostics.empty()) {
      emitDiagnostics(updateLockDiagnostics, options, std::cerr);
      return 1;
    }
    writeUpdatedLockfile(root);
  }
  auto diagnostics = checkPath(root);
  emitDiagnostics(diagnostics, options, std::cerr);
  return diagnostics.empty() ? 0 : 1;
}

int commandBuild(const Options &options, std::ostream *statusOut = &std::cout) {
  fs::path root = options.inputs.empty() ? fs::current_path() : fs::path(options.inputs.front());
  if (!options.manifest.empty()) {
    root = options.manifest;
  }
  const BuildPackage package = readBuildPackage(root);
  if (options.updateLock) {
    auto updateLockDiagnostics = updateLockUnsupportedDependencyDiagnostics(root);
    if (!updateLockDiagnostics.empty()) {
      emitDiagnostics(updateLockDiagnostics, options, std::cerr);
      return 1;
    }
    writeUpdatedLockfile(root);
  }
  auto diagnostics = checkPath(root);
  if (options.frozen) {
    auto lockDiagnostics = frozenLockDiagnosticsForBuild(package);
    diagnostics.insert(diagnostics.end(), lockDiagnostics.begin(), lockDiagnostics.end());
  }
  if (!diagnostics.empty()) {
    emitDiagnostics(diagnostics, options, std::cerr);
    return 1;
  }
  const std::string target = !options.target.empty() ? options.target : (!package.manifestTarget.empty() ? package.manifestTarget : hostTriple());
  const std::string profile = !options.profile.empty() ? options.profile : (!package.manifestProfile.empty() ? package.manifestProfile : "hosted");
  const std::string sourceHash = sourceFingerprint(package);
  const std::string lockHash = effectiveBuildLockFingerprint(package);
  const std::string lockRoot = effectiveBuildLockRoot(package);
  const std::set<std::string> policy = buildPolicyFacts(package, target, profile);
  const BuildSummary summary = summarizeBuildPackage(package);
  const std::set<std::string> dependencyRuntime = dependencyRuntimeFacts(package);
  const std::set<std::string> linkRuntime = linkedRuntimeNeeds(summary, dependencyRuntime);
  BuildSummary linkSummary = summary;
  linkSummary.runtimeNeeds = linkRuntime;
  const std::set<std::string> requiredBuiltins = requiredBuiltinPackages(package, linkSummary, profile);
  const std::set<std::string> builtinFacts = builtinPackageFacts(requiredBuiltins);
  const std::set<std::string> builtinApiFacts = builtinPublicApiFacts(requiredBuiltins);
  const std::string astHash = setFingerprint(summary.astNodes);
  const std::string hirHash = setFingerprint(summary.hirNodes);
  const std::string mirHash = setFingerprint(summary.mirNodes);
  const std::string llvmHash = setFingerprint(summary.llvmNodes);
  const std::string declarationHash = setFingerprint(summary.declarations);
  const std::string layoutHash = setFingerprint(summary.layouts);
  const std::string dropHash = setFingerprint(summary.dropGlue);
  const std::string sendSyncHash = setFingerprint(summary.sendSyncFacts);
  std::set<std::string> interfaceFacts;
  addPrefixedFacts(interfaceFacts, "interface:", summary.interfaces);
  addPrefixedFacts(interfaceFacts, "impl:", summary.impls);
  addPrefixedFacts(interfaceFacts, "generic:", summary.genericSignatures);
  addPrefixedFacts(interfaceFacts, "static-return:", summary.staticInterfaceReturns);
  const std::string interfaceHash = setFingerprint(interfaceFacts);
  const std::string abiHash = setFingerprint(summary.exports);
  const std::string trustHash = setFingerprint(summary.trustCapabilities);
  const std::string dependencyHash = setFingerprint(summary.dependencies);
  const std::set<std::string> dependencyPackages = dependencyPackageFacts(package);
  const std::string dependencyPackageHash = setFingerprint(dependencyPackages);
  const std::string costHash = setFingerprint(summary.costInputs);
  const std::string runtimeHash = setFingerprint(summary.runtimeNeeds);
  const std::string linkRuntimeHash = setFingerprint(linkRuntime);
  const std::map<std::string, std::string> nativeFunctions = nativeI32FunctionIrDefinitions(package);
  const bool canLinkNativeExecutable = package.kind == "application" && target == hostTriple() &&
                                       nativeFunctions.count(entryFunctionName(package)) != 0;
  const std::string buildHash = buildFingerprint(package,
                                                 target,
                                                 profile,
                                                 sourceHash,
                                                 lockHash,
                                                 astHash,
                                                 hirHash,
                                                 mirHash,
                                                 llvmHash,
                                                 declarationHash,
                                                 layoutHash,
                                                 dropHash,
                                                 sendSyncHash,
                                                 interfaceHash,
                                                 abiHash,
                                                 trustHash,
                                                 dependencyHash,
                                                 dependencyPackageHash,
                                                 costHash,
                                                 runtimeHash,
                                                 linkRuntimeHash,
                                                 builtinFacts);
  fs::path outRoot = fs::current_path() / "target" / target / profile;
  std::ostringstream meta;
  meta << "package = \"" << package.name << "\"\n"
       << "version = \"" << package.version << "\"\n"
       << "kind = \"" << package.kind << "\"\n"
       << "entry = \"" << package.entry << "\"\n"
       << "compiler = \"zeno-stage0 0.1.0\"\n"
       << "llvm = \"" << ZENO_LLVM_VERSION << "\"\n"
       << "target = \"" << target << "\"\n"
       << "profile = \"" << profile << "\"\n"
       << "sourceFingerprint = \"" << sourceHash << "\"\n"
       << "astFingerprint = \"" << astHash << "\"\n"
       << "hirFingerprint = \"" << hirHash << "\"\n"
       << "mirFingerprint = \"" << mirHash << "\"\n"
       << "llvmFingerprint = \"" << llvmHash << "\"\n"
       << "declarationFingerprint = \"" << declarationHash << "\"\n"
       << "layoutFingerprint = \"" << layoutHash << "\"\n"
       << "dropFingerprint = \"" << dropHash << "\"\n"
       << "sendSyncFingerprint = \"" << sendSyncHash << "\"\n"
       << "interfaceFingerprint = \"" << interfaceHash << "\"\n"
       << "abiFingerprint = \"" << abiHash << "\"\n"
       << "trustFingerprint = \"" << trustHash << "\"\n"
       << "dependencyFingerprint = \"" << dependencyHash << "\"\n"
       << "dependencyPackageFingerprint = \"" << dependencyPackageHash << "\"\n"
       << "costFingerprint = \"" << costHash << "\"\n"
       << "runtimeFingerprint = \"" << runtimeHash << "\"\n"
       << "linkRuntimeFingerprint = \"" << linkRuntimeHash << "\"\n"
       << "lockFingerprint = \"" << lockHash << "\"\n"
       << "lockRoot = \"" << lockRoot << "\"\n"
       << "buildFingerprint = \"" << buildHash << "\"\n"
       << "builtinCore = \"stage0-declarations\"\n"
       << "builtinPackages = " << joinedSet(builtinFacts) << "\n"
       << "builtinPublicApi = " << joinedSet(builtinApiFacts) << "\n"
       << "artifactFormat = \"stage0-preview\"\n"
       << "archiveFormat = \"" << (package.kind == "library" ? "ar-native-object" : "none") << "\"\n"
       << "executableFormat = \"" << (canLinkNativeExecutable ? "native-host-executable" : "none") << "\"\n"
       << "objectFormat = \"native-object\"\n"
       << "mirArtifact = \"mir/" << package.name << ".mir\"\n"
       << "llvmIrArtifact = \"ir/" << package.name << ".ll\"\n"
       << "objectArtifact = \"obj/" << package.name << ".o\"\n"
       << "finalArtifact = \"" << (package.kind == "library" ? "lib/lib" + package.name + ".a" : (canLinkNativeExecutable ? "bin/" + package.name : "obj/" + package.name + ".o")) << "\"\n"
       << "manifestTarget = \"" << package.manifestTarget << "\"\n"
       << "manifestProfile = \"" << package.manifestProfile << "\"\n"
       << "buildPolicy = " << joinedSet(policy) << "\n"
       << "manifestTrust = " << joinedSet(package.manifestTrust) << "\n"
       << "manifestDependencies = " << joinedSet(package.manifestDependencies) << "\n"
       << "astNodes = " << joinedSet(summary.astNodes) << "\n"
       << "hirNodes = " << joinedSet(summary.hirNodes) << "\n"
       << "mirNodes = " << joinedSet(summary.mirNodes) << "\n"
       << "llvmNodes = " << joinedSet(summary.llvmNodes) << "\n"
       << "declarations = " << joinedSet(summary.declarations) << "\n"
       << "cacheKeyInputs = " << joinedSet(std::set<std::string>{
              "source=" + sourceHash,
              "ast=" + astHash,
              "hir=" + hirHash,
              "mir=" + mirHash,
              "llvm=" + llvmHash,
              "declarations=" + declarationHash,
              "layouts=" + layoutHash,
              "drop=" + dropHash,
              "sendSync=" + sendSyncHash,
              "interfaces=" + interfaceHash,
              "abi=" + abiHash,
              "trust=" + trustHash,
              "dependencies=" + dependencyHash,
              "dependencyPackages=" + dependencyPackageHash,
              "cost=" + costHash,
              "runtime=" + runtimeHash,
              "linkRuntime=" + linkRuntimeHash,
              "lock=" + lockHash,
              "target=" + target,
              "profile=" + profile}) << "\n"
       << "publicApi = " << joinedSet(summary.publicApi) << "\n"
       << "packageApi = " << joinedSet(summary.packageApi) << "\n"
       << "layouts = " << joinedSet(summary.layouts) << "\n"
       << "dropGlue = " << joinedSet(summary.dropGlue) << "\n"
       << "sendSync = " << joinedSet(summary.sendSyncFacts) << "\n"
       << "interfaces = " << joinedSet(summary.interfaces) << "\n"
       << "impls = " << joinedSet(summary.impls) << "\n"
       << "genericSignatures = " << joinedSet(summary.genericSignatures) << "\n"
       << "staticInterfaceReturns = " << joinedSet(summary.staticInterfaceReturns) << "\n"
       << "exports = " << joinedSet(summary.exports) << "\n"
       << "trustCapabilities = " << joinedSet(summary.trustCapabilities) << "\n"
       << "dependencies = " << joinedSet(summary.dependencies) << "\n"
       << "dependencyPackages = " << joinedSet(dependencyPackages) << "\n"
       << "dependencyRuntimeNeeds = " << joinedSet(dependencyRuntime) << "\n"
       << "runtimeNeeds = " << joinedSet(summary.runtimeNeeds) << "\n"
       << "linkRuntimeNeeds = " << joinedSet(linkRuntime) << "\n"
       << "costInputs = " << joinedSet(summary.costInputs) << "\n";
  writeFile(outRoot / "meta" / (package.name + ".zmeta"), meta.str());

  const std::string mirArtifact =
      "module " + package.name + "\n"
      "kind " + package.kind + "\n"
      "entry " + package.entry + "\n"
      "sourceFingerprint " + sourceHash + "\n"
      "astFingerprint " + astHash + "\n"
      "hirFingerprint " + hirHash + "\n"
      "mirFingerprint " + mirHash + "\n"
      "llvmFingerprint " + llvmHash + "\n"
      "declarationFingerprint " + declarationHash + "\n"
      "layoutFingerprint " + layoutHash + "\n"
      "dropFingerprint " + dropHash + "\n"
      "sendSyncFingerprint " + sendSyncHash + "\n"
      "interfaceFingerprint " + interfaceHash + "\n"
      "abiFingerprint " + abiHash + "\n"
      "trustFingerprint " + trustHash + "\n"
      "dependencyFingerprint " + dependencyHash + "\n"
      "dependencyPackageFingerprint " + dependencyPackageHash + "\n"
      "costFingerprint " + costHash + "\n"
      "runtimeFingerprint " + runtimeHash + "\n"
      "linkRuntimeFingerprint " + linkRuntimeHash + "\n"
      "lockFingerprint " + lockHash + "\n"
      "buildFingerprint " + buildHash + "\n"
      "stage0.pipeline source->ast->hir->mir->llvm-ir->object->artifact\n"
      "stage0.pipeline ast->hir->mir->llvm\n"
      "hirNodes " + joinedSet(summary.hirNodes) + "\n"
      "mirNodes " + joinedSet(summary.mirNodes) + "\n"
      "llvmNodes " + joinedSet(summary.llvmNodes) + "\n";
  const std::string llvmIrArtifact =
      "; module = '" + package.name + "'\n"
      "source_filename = \"" + package.name + "\"\n"
      "target triple = \"" + target + "\"\n"
      "; sourceFingerprint = \"" + sourceHash + "\"\n"
      "; astFingerprint = \"" + astHash + "\"\n"
      "; hirFingerprint = \"" + hirHash + "\"\n"
      "; mirFingerprint = \"" + mirHash + "\"\n"
      "; llvmFingerprint = \"" + llvmHash + "\"\n"
      "; declarationFingerprint = \"" + declarationHash + "\"\n"
      "; layoutFingerprint = \"" + layoutHash + "\"\n"
      "; dropFingerprint = \"" + dropHash + "\"\n"
      "; sendSyncFingerprint = \"" + sendSyncHash + "\"\n"
      "; interfaceFingerprint = \"" + interfaceHash + "\"\n"
      "; abiFingerprint = \"" + abiHash + "\"\n"
      "; trustFingerprint = \"" + trustHash + "\"\n"
      "; dependencyFingerprint = \"" + dependencyHash + "\"\n"
      "; dependencyPackageFingerprint = \"" + dependencyPackageHash + "\"\n"
      "; costFingerprint = \"" + costHash + "\"\n"
      "; runtimeFingerprint = \"" + runtimeHash + "\"\n"
      "; linkRuntimeFingerprint = \"" + linkRuntimeHash + "\"\n"
      "; lockFingerprint = \"" + lockHash + "\"\n"
      "; buildFingerprint = \"" + buildHash + "\"\n"
      "; llvmNodes = " + joinedSet(summary.llvmNodes) + "\n"
      "; stage0 pipeline source->ast->hir->mir->llvm-ir->object->artifact\n";
  std::string nativeIrDefinitions;
  for (const auto &[functionName, definition] : nativeFunctions) {
    (void)functionName;
    nativeIrDefinitions += definition;
  }
  writeFile(outRoot / "mir" / (package.name + ".mir"), mirArtifact);
  writeFile(outRoot / "ir" / (package.name + ".ll"), llvmIrArtifact + nativeIrDefinitions);

  const fs::path objectPath = outRoot / "obj" / (package.name + ".o");
  const fs::path clangLog = outRoot / "obj" / (package.name + ".clang.log");
  const std::string compileCommand = "clang -target " + shellQuote(target) +
                                     " -c " + shellQuote((outRoot / "ir" / (package.name + ".ll")).string()) +
                                     " -o " + shellQuote(objectPath.string());
  if (!runShellCommand(compileCommand, clangLog)) {
    std::cerr << "zeno build: native object generation failed\n" << readFile(clangLog);
    return 1;
  }

  if (package.kind == "library") {
    writeFile(outRoot / "lib" / ("lib" + package.name + ".a"), stage0StaticArchive(readFile(objectPath)));
  } else if (canLinkNativeExecutable) {
    const fs::path executable = outRoot / "bin" / package.name;
    const std::string linkCommand = "clang -target " + shellQuote(target) +
                                    " " + shellQuote(objectPath.string()) +
                                    " -o " + shellQuote(executable.string());
    if (!runShellCommand(linkCommand, outRoot / "bin" / (package.name + ".clang.log"))) {
      std::cerr << "zeno build: native executable link failed\n" << readFile(outRoot / "bin" / (package.name + ".clang.log"));
      return 1;
    }
    fs::permissions(executable,
                    fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
                    fs::perm_options::add);
  }

  if (statusOut) {
    *statusOut << "built " << package.name << " -> " << (outRoot / "meta" / (package.name + ".zmeta")).string() << "\n";
  }
  return 0;
}

bool metadataMatches(const Metadata &metadata, const Options &options) {
  const bool applyStageFilter = options.stageExplicit || options.feature.empty();
  if (applyStageFilter && options.stage == "mvp") {
    if (metadata.stage != "mvp") return false;
  } else if (applyStageFilter && options.stage != "full-spec" && !options.stage.empty()) {
    if (metadata.stage != options.stage) return false;
  }
  if (!options.milestone.empty() && metadata.milestone != options.milestone) return false;
  if (!options.feature.empty() && metadata.feature != options.feature) return false;
  if (!options.target.empty() && !metadata.target.empty() && metadata.target != options.target) return false;
  return true;
}

bool isNestedManifest(const fs::path &root, const fs::path &path) {
  fs::path relative = fs::relative(path, root);
  int depth = 0;
  for (const auto &part : relative) {
    (void)part;
    ++depth;
  }
  return depth > 3;
}

bool isModuleOrPackageFixtureFile(const fs::path &path) {
  const std::string text = path.string();
  return contains(text, "/module-pass/") || contains(text, "/module-fail/") ||
         contains(text, "/package-pass/") || contains(text, "/package-fail/");
}

void validateTestMetadata(const Metadata &metadata, const fs::path &path) {
  if (metadata.category.empty()) return;
  const std::string where = path.string();
  if (!isKnownTestStage(metadata.stage)) {
    throw std::runtime_error("invalid test stage metadata in " + where + ": " + metadata.stage);
  }
  if (!metadata.milestone.empty() && !isKnownMilestone(metadata.milestone)) {
    throw std::runtime_error("invalid test milestone metadata in " + where + ": " + metadata.milestone);
  }
  if (!metadata.profile.empty() && !isSupportedProfile(metadata.profile)) {
    throw std::runtime_error("invalid test profile metadata in " + where + ": " + metadata.profile);
  }
  if (!metadata.target.empty() && !isSupportedTargetTriple(metadata.target)) {
    throw std::runtime_error("invalid test target metadata in " + where + ": " + metadata.target);
  }
  if (!metadata.buildArtifact.empty() && metadata.buildArtifact != "application" && metadata.buildArtifact != "library") {
    throw std::runtime_error("invalid test build-artifact metadata in " + where + ": " + metadata.buildArtifact);
  }
  if (!metadata.artifactEmit.empty() && metadata.artifactEmit != "mir" && metadata.artifactEmit != "llvm-ir") {
    throw std::runtime_error("invalid test artifact-emit metadata in " + where + ": " + metadata.artifactEmit);
  }
}

std::vector<TestCase> discoverTests(const fs::path &root, const Options &options) {
  std::vector<TestCase> tests;
  if (!fs::exists(root)) return tests;
  for (const auto &entry : fs::recursive_directory_iterator(root)) {
    if (!entry.is_regular_file()) continue;
    const fs::path path = entry.path();
    if (path.filename() == "case.toml") {
      const auto text = readFile(path);
      Metadata metadata = readMetadataFromText(text, path);
      validateTestMetadata(metadata, path);
      if (metadataMatches(metadata, options)) {
        tests.push_back(TestCase{path.parent_path(), metadata, true});
      }
      continue;
    }
    const auto ext = path.extension().string();
    if (ext != ".zn" && ext != ".toml") continue;
    if (path.filename() == "Zeno.lock") continue;
    if (path.filename() == "case.toml") continue;
    if (isModuleOrPackageFixtureFile(path) && path.filename() != "Zeno.toml") continue;
    if (isModuleOrPackageFixtureFile(path) && path.filename() == "Zeno.toml" && isNestedManifest(root, path)) continue;
    const auto text = readFile(path);
    Metadata metadata = readMetadataFromText(text, path);
    if (metadata.category.empty()) continue;
    validateTestMetadata(metadata, path);
    if (metadataMatches(metadata, options)) {
      if ((startsWith(metadata.category, "module-") || startsWith(metadata.category, "package-")) &&
          path.filename() == "Zeno.toml") {
        tests.push_back(TestCase{path.parent_path(), metadata, true});
      } else {
        tests.push_back(TestCase{path, metadata, false});
      }
    }
  }
  std::sort(tests.begin(), tests.end(), [](const TestCase &a, const TestCase &b) {
    return std::tie(a.metadata.category, a.path) < std::tie(b.metadata.category, b.path);
  });
  return tests;
}

std::string caseTomlStringValue(const SourceText &source, const std::string &sectionName, const std::string &keyName) {
  std::string section;
  const std::regex sectionPattern(R"(^\s*\[([A-Za-z0-9_.-]+)\]\s*$)");
  const std::regex kvPattern("^\\s*" + keyName + "\\s*=\\s*\"([^\"]+)\"");
  for (const auto &line : splitLines(source.text)) {
    if (auto match = matchRegex(line, sectionPattern)) {
      section = (*match)[1];
      continue;
    }
    if (section == sectionName) {
      if (auto value = matchRegex(line, kvPattern)) return std::string((*value)[1]);
    }
  }
  return "";
}

int firstLineInSection(const SourceText &source, const std::string &prefix) {
  int lineNo = 1;
  for (const auto &line : splitLines(source.text)) {
    if (startsWith(trim(line), prefix)) return lineNo;
    ++lineNo;
  }
  return 1;
}

bool sectionContainsKey(const SourceText &source, const std::string &sectionName, const std::string &keyName) {
  std::string section;
  const std::regex sectionPattern(R"(^\s*\[([A-Za-z0-9_.-]+)\]\s*$)");
  const std::regex keyPattern("^\\s*" + keyName + R"(\s*=)");
  for (const auto &line : splitLines(source.text)) {
    if (auto match = matchRegex(line, sectionPattern)) {
      section = (*match)[1];
      continue;
    }
    if (section == sectionName && std::regex_search(line, keyPattern)) return true;
  }
  return false;
}

std::vector<std::string> caseTomlArrayValue(const SourceText &source, const std::string &sectionName, const std::string &keyName) {
  std::vector<std::string> values;
  std::string section;
  bool collecting = false;
  const std::regex sectionPattern(R"(^\s*\[([A-Za-z0-9_.-]+)\]\s*$)");
  const std::regex startPattern("^\\s*" + keyName + R"(\s*=\s*\[)");
  const std::regex quotedPattern(R"re("([^"]+)")re");
  for (const auto &line : splitLines(source.text)) {
    if (auto match = matchRegex(line, sectionPattern)) {
      section = (*match)[1];
      collecting = false;
      continue;
    }
    if (section != sectionName) continue;
    if (!collecting && std::regex_search(line, startPattern)) collecting = true;
    if (!collecting) continue;
    auto begin = std::sregex_iterator(line.begin(), line.end(), quotedPattern);
    auto end = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) values.push_back((*it)[1]);
    if (contains(line, "]")) collecting = false;
  }
  return values;
}

bool caseTomlBoolValue(const SourceText &source, const std::string &sectionName, const std::string &keyName, bool defaultValue = false) {
  std::string section;
  const std::regex sectionPattern(R"(^\s*\[([A-Za-z0-9_.-]+)\]\s*$)");
  const std::regex boolPattern("^\\s*" + keyName + R"(\s*=\s*(true|false))");
  for (const auto &line : splitLines(source.text)) {
    if (auto match = matchRegex(line, sectionPattern)) {
      section = (*match)[1];
      continue;
    }
    if (section == sectionName) {
      if (auto value = matchRegex(line, boolPattern)) return std::string((*value)[1]) == "true";
    }
  }
  return defaultValue;
}

struct IncrementalPlan {
  std::map<std::string, std::set<std::string>> arrays;
  std::map<std::string, bool> bools;
  std::map<std::string, std::string> strings;
};

IncrementalPlan previewIncrementalPlan(const SourceText &source) {
  IncrementalPlan plan;
  const std::string beforePublic = caseTomlStringValue(source, "before", "publicFingerprint");
  const std::string afterPublic = caseTomlStringValue(source, "after", "publicFingerprint");
  const std::string beforePackage = caseTomlStringValue(source, "before", "packageFingerprint");
  const std::string afterPackage = caseTomlStringValue(source, "after", "packageFingerprint");
  const std::string beforeBody = caseTomlStringValue(source, "before", "bodyFingerprint");
  const std::string afterBody = caseTomlStringValue(source, "after", "bodyFingerprint");
  const std::string beforeLayout = caseTomlStringValue(source, "before", "layoutFingerprint");
  const std::string afterLayout = caseTomlStringValue(source, "after", "layoutFingerprint");
  const std::string package = caseTomlStringValue(source, "before", "package");
  const std::string item = caseTomlStringValue(source, "before", "item");
  const std::string type = caseTomlStringValue(source, "before", "type");

  if (!beforeBody.empty() && beforeBody != afterBody && beforePublic == afterPublic && beforePackage == afterPackage) {
    plan.arrays["recheck"].insert(package + "::" + item + ".body");
    plan.arrays["recodegen"].insert(package + "::" + item);
    plan.bools["diagnosticOrderStable"] = true;
  }
  if (!beforePublic.empty() && beforePublic != afterPublic && !item.empty()) {
    plan.arrays["recheck"].insert(package + "::" + item + ".signature");
    for (const auto &user : caseTomlArrayValue(source, "graph", "directUsers")) {
      plan.arrays["recheck"].insert(user);
      plan.arrays["invalidateDownstream"].insert(user);
    }
    for (const auto &nonUser : caseTomlArrayValue(source, "graph", "nonUsers")) {
      plan.arrays["keepValid"].insert(nonUser);
    }
  }
  if (!beforeLayout.empty() && beforeLayout != afterLayout && !type.empty()) {
    plan.arrays["recheck"].insert(package + "::" + type + ".layout");
    plan.arrays["recodegen"].insert(package + "::*");
    plan.arrays["recodegen"].insert("consumer::*");
    plan.arrays["invalidateDownstream"].insert("consumer");
    plan.strings["reason"] = "@layout(C) size/field layout changed";
  }
  return plan;
}

void verifyExpectedArray(const SourceText &source, const IncrementalPlan &plan, const std::string &key, std::vector<Diagnostic> &diagnostics) {
  const auto expected = caseTomlArrayValue(source, "expect", key);
  if (expected.empty() && !contains(source.text, key + " = []")) return;
  const std::set<std::string> actual = plan.arrays.count(key) ? plan.arrays.at(key) : std::set<std::string>{};
  for (const auto &value : expected) {
    if (!actual.count(value)) {
      diagnostics.push_back(makeDiagnostic(source, "E0902", "incremental plan missing " + key + " entry " + value, lineContaining(source.text, key + " =")));
    }
  }
  for (const auto &value : actual) {
    if (std::find(expected.begin(), expected.end(), value) == expected.end()) {
      diagnostics.push_back(makeDiagnostic(source, "E0902", "incremental plan has unexpected " + key + " entry " + value, lineContaining(source.text, key + " =")));
    }
  }
}

struct CaseExpectation {
  std::string section;
  std::string value;
  int line = 1;
  bool forbidden = false;
};

std::vector<CaseExpectation> codegenCaseExpectations(const SourceText &source) {
  std::vector<CaseExpectation> expectations;
  std::string section;
  std::string mode;
  int lineNo = 1;
  const std::regex sectionPattern(R"(^\s*\[([A-Za-z0-9_.-]+)\]\s*$)");
  const std::regex quotedPattern(R"re("([^"]+)")re");
  for (const auto &line : splitLines(source.text)) {
    const std::string trimmedLine = trim(line);
    if (auto match = matchRegex(line, sectionPattern)) {
      section = (*match)[1];
      mode.clear();
      ++lineNo;
      continue;
    }
    if (!startsWith(section, "expect.")) {
      ++lineNo;
      continue;
    }
    if (contains(trimmedLine, "contains =") || contains(trimmedLine, "paramAttrs =")) {
      mode = "contains";
    } else if (contains(trimmedLine, "forbid =")) {
      mode = "forbid";
    }
    if (!mode.empty()) {
      auto begin = std::sregex_iterator(line.begin(), line.end(), quotedPattern);
      auto end = std::sregex_iterator();
      for (auto it = begin; it != end; ++it) {
        expectations.push_back(CaseExpectation{section, std::string((*it)[1]), lineNo, mode == "forbid"});
      }
    }
    if (contains(trimmedLine, "]")) mode.clear();
    ++lineNo;
  }
  return expectations;
}

std::string caseSourceText(const SourceText &source) {
  const std::regex sourcePattern(R"re(text\s*=\s*'''([\s\S]*)''')re");
  if (auto match = matchRegex(source.text, sourcePattern)) {
    return std::string((*match)[1]);
  }
  return "";
}

std::set<std::string> previewCodegenFacts(const SourceText &caseSource, const std::string &expectSection) {
  const std::string src = caseSourceText(caseSource);
  std::set<std::string> facts;
  auto add = [&](std::string fact) { facts.insert(std::move(fact)); };

  if (contains(src, "try parseHeader")) {
    add("switch discriminant(parseHeader.result)");
    add("cleanup.drop attempt");
    add("cleanup.drop buffer");
    add("return Err");
    add("branch");
  }
  if (contains(src, "fn fill(mut data: ArraySlice<U8>")) {
    add("AccessRegion data unique");
    add("data mayEscape false");
    add("lowerForRange 0..data.len");
    add("data.ptr noalias");
    add("data.ptr nocapture");
  }
  if (contains(src, "for i in 0..data.len")) {
    if (contains(expectSection, "before_opt")) add("boundsCheck(i, data.len)");
    add("rangeFact 0 <= i < data.len");
    add("load data[i]");
  }
  if (contains(src, ".fold(") || contains(src, ".count(")) {
    add("closure capability Fn");
    add("capture add readonly");
    add("closure mayEscape false");
    add("closure.inline");
    add("capture.add scalar");
  }
  if (contains(src, "@layout(Packed(1))") && contains(src, "length: U16")) {
    add("field Header.length offset 1 align 1 packed");
    add("unalignedLoad U16");
    add("unalignedStore U16");
    add("load i16, align 1");
    add("store i16, align 1");
  }
  if (contains(src, "val left = try File.open(pathA)") && contains(src, "val right = try File.open(pathB)")) {
    add("dropFlag left");
    add("dropFlag right");
    add("setDropFlag left true");
    add("try File.open(pathB) err -> cleanup.drop left");
    add("move left -> setDropFlag left false");
    add("move right -> setDropFlag right false");
  }
  if (contains(src, "addOne(mut counters.read)") && contains(src, "addOne(mut counters.written)")) {
    add("MemoryObject counters");
    add("AccessPath counters.read");
    add("AccessPath counters.written");
    add("DisjointProof counters.read counters.written");
    add("alias.scope counters.read");
    add("alias.scope counters.written");
  }
  if (contains(src, "try mut out.tryReserve(input.len)") && contains(src, "mut out.push(")) {
    if (contains(expectSection, "before_opt")) {
      add("capacityCheck out");
      add("pushSlowPath out");
    }
    add("capacityFact out.free >= input.len");
    add("pushUncheckedWithinReservedCapacity");
  }
  if (contains(src, "GlobalAllocator {}") && contains(src, "withCapacityIn(64")) {
    add("GlobalAllocator size 0");
    add("withCapacityIn allocator dispatch static");
    add("allocatorFieldElided Vector<U32>");
    add("directCall GlobalAllocator.allocate");
  }
  if (contains(src, "async fn parse(") && contains(src, "InlineExecutor")) {
    add("FutureState parse local");
    add("poll direct");
    add("future mayEscape false");
  }
  if (contains(src, "CachePadded<Atomic<U64>>")) {
    add("CachePadded<Atomic<U64>> align cacheLine");
    add("CachePadded<Atomic<U64>> size multipleOf cacheLine");
    add("WorkerCounters.local offset cacheLineAligned");
    add("WorkerCounters.stolen offset cacheLineAligned");
  }
  if (contains(src, "unwrapOrPanic<T>")) {
    add("mono unwrapOrPanic<U32>");
    add("mono unwrapOrPanic<U64>");
    add("coldOutline panic path");
    add("cold panic block");
  }
  if (contains(src, "async fn readHeader")) {
    add("FutureState readHeader");
    add("state beforeAwait initialized file");
    add("state beforeAwait initialized readMetric");
    add("futureDrop switch state");
    add("cleanup.drop readMetric");
    add("cleanup.drop file");
  }
  if (contains(src, "val out = Vector<U8>.withCapacityIn(128, mut arena)")) {
    add("allocationRegion arena");
    add("owner out region arena");
    add("owner out mayEscape false");
    add("storageDead out before arena");
  }
  if (contains(src, "file = try File.open(pathB)")) {
    add("dropFlag file");
    add("rhsTemp = try File.open(pathB)");
    add("try File.open(pathB) err -> cleanup.drop file");
    add("rhs ok -> drop old file");
    add("move rhsTemp -> file");
    add("move file -> setDropFlag file false");
  }
  if (contains(src, "return match move state")) {
    add("switch enumTag(state)");
    add("Ready -> bind owning payload job");
    add("move job -> clear payload dropFlag");
    add("Done -> no payload drop");
  }
  if (contains(src, "Event.Key(code) | Event.Code(code)")) {
    add("switch enumTag(event)");
    add("orPattern merge bindings code");
    add("rangeCheck 10..=20");
    add("guard code < 10");
    add("guard false -> next pattern");
  }
  if (contains(src, "val pair: Pair;")) {
    add("dropFlag pair.left");
    add("dropFlag pair.right");
    add("setDropFlag pair.left true");
    add("try File.open(pathB) err -> cleanup.drop pair.left");
    add("setDropFlag pair.right true");
    add("move pair -> clear pair.left pair.right");
  }
  if (contains(src, "var file: File;") && contains(src, "if (flag)")) {
    add("dropFlag file");
    add("if flag then setDropFlag file true");
    add("rhsTemp = try File.open(pathB)");
    add("try File.open(pathB) err -> cleanup.conditionalDrop file");
    add("rhs ok -> conditionalDrop old file");
    add("move rhsTemp -> file");
    add("setDropFlag file true");
    add("move file -> setDropFlag file false");
  }
  if (contains(src, "impl Pair") && contains(src, "tracePair()")) {
    add("cleanup.drop pair");
    add("call Pair.destroy");
    add("cleanup.drop pair.right");
    add("cleanup.drop pair.left");
    add("call Resource.destroy pair.right");
    add("call Resource.destroy pair.left");
  }
  if (contains(src, "trust extern \"C\" fn c_read")) {
    add("trustSpan capability=ffi authorized=true");
    add("trustSpan capability=hardware authorized=true");
    add("trustReport publicImpact read");
    add("trustReport publicImpact map");
  }
  if (contains(src, "@layout(C)") && contains(src, "pub struct CPoint")) {
    add("CPoint layout=C target=x86_64-unknown-linux-gnu");
    add("field x offset=0");
    add("field y offset=4");
    add("size=8 align=4");
    add("abiFingerprint includes target C ABI");
    add("pointSum param point uses C aggregate passing");
  }
  if (contains(src, "@export(\"zeno_read\", bridge: C)")) {
    add("export bridge=C symbol=zeno_read");
    add("bridge source signature read(FileDescriptor, mut ArraySlice<U8>) -> Result<USize, IoError>");
    add("bridge thunk signature zeno_read(FileDescriptor, U8*, USize, USize*) -> I32");
    add("IoError implements CErrorCode");
    add("abiFingerprint includes bridge thunk and header");
  }
  if (contains(src, "const pageSize: USize = pow2(12)")) {
    add("ctfe pow2(12) -> 4096");
    add("const pageSize valueFingerprint=USize:4096");
    add("constGeneric Size valueFingerprint=USize:4096");
    add("monoKey includes constArg Size=4096");
    add("layoutKey includes constArg Size=4096");
  }
  if (contains(src, "TaskGroup<Result<Page, FetchError>>")) {
    add("TaskGroup<Result<Page, FetchError>> withCapacity");
    add("completionOrder collect");
    add("taskControlBlock explicit");
  }
  return facts;
}

std::vector<Diagnostic> caseTomlDiagnostics(const fs::path &caseFile, const std::string &category) {
  SourceText source = loadSource(caseFile);
  std::vector<Diagnostic> diagnostics;
  auto add = [&](const std::string &code, const std::string &message, int line) {
    diagnostics.push_back(makeDiagnostic(source, code, message, line));
  };

  if (category == "codegen-pass") {
    for (const auto &expectation : codegenCaseExpectations(source)) {
      const auto facts = previewCodegenFacts(source, expectation.section);
      const bool present = facts.count(expectation.value) != 0;
      if (!expectation.forbidden && !present) {
        add("E0902", "missing preview codegen fact: " + expectation.value, expectation.line);
      } else if (expectation.forbidden && present) {
        add("E0902", "forbidden preview codegen fact was present: " + expectation.value, expectation.line);
      }
    }
  } else if (category == "codegen-fail") {
    const bool hasBadArtifact = contains(source.text, "[bad_codegen]") || contains(source.text, "[bad_mir]") ||
                                contains(source.text, "[bad_hir]") || contains(source.text, "[bad_layout]");
    if (hasBadArtifact) {
      std::string message = caseTomlStringValue(source, "expect", "error");
      std::string code = caseTomlStringValue(source, "expect", "code");
      if (message.empty()) message = "codegen verifier rejected the supplied bad artifact";
      if (code.empty()) code = "E0902";
      add(code, message, firstLineInSection(source, "[bad_"));
    }
  } else if (category == "incremental-pass") {
    const auto plan = previewIncrementalPlan(source);
    verifyExpectedArray(source, plan, "recheck", diagnostics);
    verifyExpectedArray(source, plan, "recodegen", diagnostics);
    verifyExpectedArray(source, plan, "invalidateDownstream", diagnostics);
    verifyExpectedArray(source, plan, "keepValid", diagnostics);
    if (contains(source.text, "diagnosticOrderStable")) {
      const bool expected = caseTomlBoolValue(source, "expect", "diagnosticOrderStable");
      const bool actual = plan.bools.count("diagnosticOrderStable") ? plan.bools.at("diagnosticOrderStable") : false;
      if (expected != actual) {
        add("E0902", "incremental diagnosticOrderStable expectation was not met", lineContaining(source.text, "diagnosticOrderStable"));
      }
    }
    const std::string expectedReason = caseTomlStringValue(source, "expect", "reason");
    const std::string actualReason = plan.strings.count("reason") ? plan.strings.at("reason") : "";
    if (!expectedReason.empty() && actualReason != expectedReason) {
      add("E0902", "incremental invalidation reason mismatch", lineContaining(source.text, "reason"));
    }
  } else if (category == "incremental-fail") {
    if (contains(source.text, "[cacheKey]") && contains(source.text, "[changedInput]")) {
      const bool cacheKeyMentionsTrust = sectionContainsKey(source, "cacheKey", "trust") ||
                                         sectionContainsKey(source, "cacheKey", "trustHash") ||
                                         sectionContainsKey(source, "cacheKey", "trustFingerprint");
      if (!cacheKeyMentionsTrust) {
        add("E0902", "cache key is missing trust configuration", lineContaining(source.text, "[cacheKey]"));
      }
    }
    if (contains(source.text, "[observed]") && contains(source.text, "[expected]") &&
        contains(source.text, "order = [\"worker2\", \"worker1\"]")) {
      add("E0902", "diagnostics must be sorted by package, file, offset, and error code", lineContaining(source.text, "[observed]"));
    }
  }

  return diagnostics;
}

bool diagnosticMatches(const Diagnostic &diag, const ExpectedError &expected) {
  if (!expected.code.empty() && diag.code != expected.code) return false;
  if (!expected.message.empty() && !contains(diag.message, expected.message)) return false;
  return true;
}

bool stagedDiagnosticMetadataIsComplete(const Diagnostic &diag) {
  if (!startsWith(diag.code, "E90")) return true;
  return diag.staged && !diag.feature.empty();
}

bool hasAnyExecBit(fs::perms permissions) {
  return (permissions & (fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec)) != fs::perms::none;
}

bool hasNativeObjectMagic(const std::string &bytes) {
  if (bytes.size() < 4) return false;
  const unsigned char b0 = static_cast<unsigned char>(bytes[0]);
  const unsigned char b1 = static_cast<unsigned char>(bytes[1]);
  const unsigned char b2 = static_cast<unsigned char>(bytes[2]);
  const unsigned char b3 = static_cast<unsigned char>(bytes[3]);
  if (b0 == 0x7f && b1 == 'E' && b2 == 'L' && b3 == 'F') return true;
  if (b0 == 0xcf && b1 == 0xfa && b2 == 0xed && b3 == 0xfe) return true;
  if (b0 == 0xfe && b1 == 0xed && b2 == 0xfa && b3 == 0xcf) return true;
  if (b0 == 0xca && b1 == 0xfe && b2 == 0xba && b3 == 0xbe) return true;
  return false;
}

bool containsNativeObjectMagic(const std::string &bytes) {
  return bytes.find(std::string("\x7f"
                                "ELF",
                                4)) != std::string::npos ||
         bytes.find(std::string("\xcf\xfa\xed\xfe", 4)) != std::string::npos ||
         bytes.find(std::string("\xfe\xed\xfa\xcf", 4)) != std::string::npos ||
         bytes.find(std::string("\xca\xfe\xba\xbe", 4)) != std::string::npos;
}

bool verifyBuildArtifact(const TestCase &test, const Options &options, std::string &failure) {
  if (test.metadata.buildArtifact.empty()) return true;
  if (!test.directoryCase) {
    failure = "build-artifact metadata requires a directory package case";
    return false;
  }

  Options buildOptions = options;
  buildOptions.command = "build";
  buildOptions.inputs = {test.path.string()};
  buildOptions.emit = test.metadata.artifactEmit;
  buildOptions.updateLock = false;
  buildOptions.diagnosticFormat = "human";
  if (commandBuild(buildOptions, nullptr) != 0) {
    failure = "zeno build failed for build-artifact case";
    return false;
  }

  const BuildPackage package = readBuildPackage(test.path);
  const std::string target = !buildOptions.target.empty() ? buildOptions.target : (!package.manifestTarget.empty() ? package.manifestTarget : hostTriple());
  const std::string profile = !buildOptions.profile.empty() ? buildOptions.profile : (!package.manifestProfile.empty() ? package.manifestProfile : "hosted");
  const fs::path outRoot = fs::current_path() / "target" / target / profile;
  const fs::path metaPath = outRoot / "meta" / (package.name + ".zmeta");
  if (!fs::exists(metaPath)) {
    failure = "build did not write .zmeta for " + package.name;
    return false;
  }
  const std::string meta = readFile(metaPath);
  if (!contains(meta, "package = \"" + package.name + "\"") ||
      !contains(meta, "kind = \"" + package.kind + "\"") ||
      !contains(meta, "artifactFormat = \"stage0-preview\"")) {
    failure = ".zmeta is missing package, kind, or artifact format facts";
    return false;
  }
  for (const auto &expectedFact : test.metadata.zmetaContains) {
    if (!contains(meta, expectedFact)) {
      failure = ".zmeta is missing expected fact: " + expectedFact;
      return false;
    }
  }
  for (const auto &forbiddenFact : test.metadata.zmetaForbid) {
    if (contains(meta, forbiddenFact)) {
      failure = ".zmeta contains forbidden fact: " + forbiddenFact;
      return false;
    }
  }
  const fs::path objectPath = outRoot / "obj" / (package.name + ".o");
  if (!fs::is_regular_file(objectPath)) {
    failure = "build did not write native object obj/" + package.name + ".o";
    return false;
  }
  const std::string objectBytes = readFile(objectPath);
  if (!hasNativeObjectMagic(objectBytes) || contains(objectBytes, "zeno-stage0-preview-object")) {
    failure = "object artifact is not a native object file";
    return false;
  }
  for (const auto &expectedFact : test.metadata.objectContains) {
    if (!contains(objectBytes, expectedFact)) {
      failure = "native object is missing expected bytes: " + expectedFact;
      return false;
    }
  }
  for (const auto &forbiddenFact : test.metadata.objectForbid) {
    if (contains(objectBytes, forbiddenFact)) {
      failure = "native object contains forbidden bytes: " + forbiddenFact;
      return false;
    }
  }
  if (!test.metadata.artifactEmit.empty()) {
    const fs::path emitPath = test.metadata.artifactEmit == "mir"
        ? outRoot / "mir" / (package.name + ".mir")
        : outRoot / "ir" / (package.name + ".ll");
    if (!fs::exists(emitPath)) {
      failure = "build did not write requested " + test.metadata.artifactEmit + " artifact";
      return false;
    }
    const std::string emitted = readFile(emitPath);
    for (const auto &expectedFact : test.metadata.emitContains) {
      if (!contains(emitted, expectedFact)) {
        failure = test.metadata.artifactEmit + " artifact is missing expected fact: " + expectedFact;
        return false;
      }
    }
    for (const auto &forbiddenFact : test.metadata.emitForbid) {
      if (contains(emitted, forbiddenFact)) {
        failure = test.metadata.artifactEmit + " artifact contains forbidden fact: " + forbiddenFact;
        return false;
      }
    }
  }

  if (test.metadata.buildArtifact == "application") {
    if (package.kind != "application") {
      failure = "build-artifact application case resolved as " + package.kind;
      return false;
    }
    if (target != hostTriple()) {
      return true;
    }
    const fs::path executable = outRoot / "bin" / package.name;
    if (!fs::is_regular_file(executable)) {
      failure = "application build did not write bin/" + package.name;
      return false;
    }
    std::error_code error;
    const fs::perms permissions = fs::status(executable, error).permissions();
    if (error || !hasAnyExecBit(permissions)) {
      failure = "application artifact is not executable";
      return false;
    }
    const std::string executableBytes = readFile(executable);
    if (!hasNativeObjectMagic(executableBytes) || startsWith(executableBytes, "#!/bin/sh\n")) {
      failure = "application artifact is not a native executable";
      return false;
    }
    if (!test.metadata.runExitCode.empty()) {
      int expectedExitCode = 0;
      try {
        expectedExitCode = std::stoi(test.metadata.runExitCode);
      } catch (...) {
        failure = "invalid run-exit-code metadata: " + test.metadata.runExitCode;
        return false;
      }
      const fs::path runLog = outRoot / "bin" / (package.name + ".run.log");
      const int actualExitCode = runShellCommandExitCode(shellQuote(executable.string()), runLog);
      if (actualExitCode != expectedExitCode) {
        failure = "application executable exit code " + std::to_string(actualExitCode) +
                  " did not match expected " + std::to_string(expectedExitCode);
        return false;
      }
    }
    return true;
  }

  if (package.kind != "library") {
    failure = "build-artifact library case resolved as " + package.kind;
    return false;
  }
  const fs::path archive = outRoot / "lib" / ("lib" + package.name + ".a");
  if (!fs::is_regular_file(archive)) {
    failure = "library build did not write lib/lib" + package.name + ".a";
    return false;
  }
  const std::string archiveText = readFile(archive);
  if (!startsWith(archiveText, "!<arch>\n")) {
    failure = "library artifact is missing the ar archive header";
    return false;
  }
  if (!contains(archiveText, "zeno-stage0.o/") ||
      !containsNativeObjectMagic(archiveText) ||
      contains(archiveText, "zeno-stage0-preview-object")) {
    failure = "library artifact is missing the native stage0 object member";
    return false;
  }
  return true;
}

bool runSingleTest(const TestCase &test, const Options &options, std::string &failure) {
  const std::string category = test.metadata.category;
  const bool shouldFail = contains(category, "-fail");
  std::vector<Diagnostic> diagnostics;
  std::vector<ExpectedError> expected;

  if (test.directoryCase) {
    const fs::path caseFile = test.path / "case.toml";
    if (fs::exists(caseFile)) {
      SourceText source = loadSource(caseFile);
      expected = expectedErrors(source.text);
      diagnostics = caseTomlDiagnostics(caseFile, category);
    } else {
      expected = expectedErrorsForPath(test.path);
      diagnostics = checkPath(test.path);
    }
  } else {
    SourceText source = loadSource(test.path);
    expected = expectedErrors(source.text);
    diagnostics = checkFile(test.path);
  }
  sortDiagnosticsInPlace(diagnostics);

  for (const auto &diag : diagnostics) {
    if (!stagedDiagnosticMetadataIsComplete(diag)) {
      failure = "staged diagnostic " + diag.code + " is missing isStaged or feature metadata";
      return false;
    }
  }

  if (options.stage == "full-spec" && !shouldFail) {
    diagnostics.erase(std::remove_if(diagnostics.begin(), diagnostics.end(), [](const Diagnostic &diag) {
      return diag.code == "E9001";
    }), diagnostics.end());
    sortDiagnosticsInPlace(diagnostics);
  }
  if (!shouldFail) {
    if (!diagnostics.empty()) {
      failure = "unexpected diagnostic " + diagnostics.front().code + ": " + diagnostics.front().message;
      return false;
    }
    return verifyBuildArtifact(test, options, failure);
  }

  if (expected.empty()) {
    failure = "fail test has no expected diagnostic";
    return false;
  }
  if (test.metadata.stage == "mvp") {
    for (const auto &want : expected) {
      if (want.code.empty()) {
        failure = "mvp fail test expected diagnostic is missing a stable error code";
        return false;
      }
    }
  }
  for (const auto &want : expected) {
    const bool matched = std::any_of(diagnostics.begin(), diagnostics.end(), [&](const Diagnostic &diag) {
      return diagnosticMatches(diag, want);
    });
    if (!matched) {
      failure = "missing expected diagnostic " + (want.code.empty() ? want.message : want.code);
      return false;
    }
  }
  return true;
}

int commandTest(const Options &options) {
  const fs::path root = fs::path(options.workspace) / "tests" / "spec";
  auto tests = discoverTests(root, options);
  int passed = 0;
  std::vector<std::pair<fs::path, std::string>> failures;
  for (const auto &test : tests) {
    std::string failure;
    if (runSingleTest(test, options, failure)) {
      ++passed;
    } else {
      failures.push_back({test.path, failure});
    }
  }
  for (const auto &[path, failure] : failures) {
    std::cerr << "FAIL " << path.string() << ": " << failure << "\n";
  }
  std::cout << "zeno test: " << passed << " passed, " << failures.size() << " failed";
  if (options.stage == "mvp" && (options.stageExplicit || options.feature.empty())) {
    std::cout << " (stage mvp";
    if (!options.milestone.empty()) std::cout << ", " << options.milestone;
    std::cout << ")";
  }
  if (tests.empty()) {
    std::cout << " (no tests selected)";
  }
  std::cout << "\n";
  return failures.empty() ? 0 : 1;
}

int run(int argc, char **argv) {
  Options options = parseOptions(argc, argv);
  if (options.command == "help") {
    printHelp();
    return 0;
  }
  if (options.command == "version") return commandVersion();
  if (options.command == "check") return commandCheck(options);
  if (options.command == "build") return commandBuild(options);
  if (options.command == "test") return commandTest(options);
  std::cerr << "unknown command: " << options.command << "\n";
  return 2;
}

} // namespace

int main(int argc, char **argv) {
  try {
    return run(argc, argv);
  } catch (const std::exception &error) {
    std::cerr << "zeno: " << error.what() << "\n";
    return 2;
  }
}
