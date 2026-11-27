#include "codex_organizer/core.hpp"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <cmath>
#include <cctype>
#include <climits>
#include <cstdlib>
#include <cwctype>
#include <fstream>
#include <memory>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <variant>

namespace codex_organizer {
namespace {

std::wstring Trim(std::wstring value) {
    const auto is_space = [](wchar_t character) { return std::iswspace(character) != 0; };
    value.erase(value.begin(), std::find_if_not(value.begin(), value.end(), is_space));
    value.erase(std::find_if_not(value.rbegin(), value.rend(), is_space).base(), value.end());
    return value;
}

bool IsUuid(const std::wstring& value) {
    static const std::wregex pattern(
        LR"(^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$)",
        std::regex_constants::icase);
    return std::regex_match(value, pattern);
}

std::wstring FindUuid(const std::wstring& value) {
    static const std::wregex pattern(
        LR"([0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12})",
        std::regex_constants::icase);
    std::wsmatch match;
    return std::regex_search(value, match, pattern) ? ToLower(match.str()) : std::wstring();
}

std::wstring StripInlineStatusAnnotations(const std::wstring& segment) {
    std::wstring trimmed = Trim(segment);
    // Strip complete trailing badges, including adjacent goal/warning badges.
    // A plain count needs padding or its own segment to preserve names like "Fix 1 warning".
    static const std::wregex pattern(
        LR"((?:(?:^|\s+)(?:(?:goal\s+(?:achieved|stalled|paused)|pursuing\s+goal)(?:\s+\([^)]*\))?|\u26a0[\ufe0e\ufe0f]?\s*[0-9]+\s+warnings?)|(?:^|\s{2,})[0-9]+\s+warnings?)\s*$)",
        std::regex_constants::icase);
    std::wsmatch match;
    while (std::regex_search(trimmed, match, pattern)) {
        trimmed = Trim(trimmed.substr(0, static_cast<std::size_t>(match.position())));
    }
    return trimmed;
}

std::vector<std::wstring> SplitStatusLine(const std::wstring& line) {
    std::vector<std::wstring> segments;
    std::size_t start = 0;
    while (start <= line.size()) {
        const std::size_t separator = line.find(L'\u00b7', start);
        segments.push_back(StripInlineStatusAnnotations(line.substr(
            start, separator == std::wstring::npos ? std::wstring::npos : separator - start)));
        if (separator == std::wstring::npos) break;
        start = separator + 1;
    }
    return segments;
}

bool IsNormalStatusLine(const std::vector<std::wstring>& segments) {
    if (segments.size() < 2) return false;
    const bool has_status = std::any_of(
        segments.begin(), segments.end(), [](const std::wstring& segment) {
            const std::wstring lowered = ToLower(Trim(segment));
            return lowered.rfind(L"ready", 0) == 0 || lowered.rfind(L"working", 0) == 0 ||
                   lowered.rfind(L"thinking", 0) == 0 || lowered.rfind(L"waiting", 0) == 0;
        });
    const bool has_model = std::any_of(
        segments.begin(), segments.end(), [](const std::wstring& segment) {
            return ToLower(Trim(segment)).find(L"gpt") != std::wstring::npos;
        });
    return has_status && has_model;
}

std::vector<std::wstring> LatestStatusSegments(const std::wstring& terminal_tail) {
    std::vector<std::wstring> lines;
    std::wistringstream line_stream(terminal_tail);
    for (std::wstring line; std::getline(line_stream, line);) lines.push_back(std::move(line));
    for (auto iterator = lines.rbegin(); iterator != lines.rend(); ++iterator) {
        std::vector<std::wstring> segments = SplitStatusLine(Trim(*iterator));
        if (IsNormalStatusLine(segments)) return segments;
    }
    return {};
}

std::wstring ExtractUsedTokens(const std::wstring& segment) {
    static const std::wregex pattern(
        LR"(^\s*([0-9][0-9,]*(?:\.[0-9]+)?[kmgt]?)\s+(?:tokens?\s+)?used\b)",
        std::regex_constants::icase);
    std::wsmatch match;
    return std::regex_search(segment, match, pattern) ? match[1].str() : std::wstring();
}

bool IsStatusMetadata(const std::wstring& segment) {
    const std::wstring lowered = ToLower(Trim(segment));
    if (lowered.empty()) return true;
    if (lowered.rfind(L"ready", 0) == 0 || lowered.rfind(L"working", 0) == 0 ||
        lowered.rfind(L"thinking", 0) == 0 || lowered.rfind(L"waiting", 0) == 0) return true;
    if (lowered.find(L"gpt") != std::wstring::npos) return true;
    if (lowered == L"fast" || lowered.rfind(L"fast ", 0) == 0) return true;
    if (lowered.find(L"weekly") != std::wstring::npos) return true;
    if (!ExtractUsedTokens(segment).empty()) return true;
    static const std::wregex remaining_pattern(LR"(\b[0-9]+%\s+left\b)",
                                               std::regex_constants::icase);
    return std::regex_search(lowered, remaining_pattern);
}

bool IsIgnoredIdentitySegment(const std::wstring& segment) {
    if (IsStatusMetadata(segment)) return true;
    const std::wstring lowered = ToLower(Trim(segment));
    if (lowered.rfind(L"main [", 0) == 0) return true;
    static const std::wregex goal_pattern(
        LR"(^(?:goal\s+(?:achieved|stalled|paused)|pursuing\s+goal)\b)",
        std::regex_constants::icase);
    return std::regex_search(lowered, goal_pattern);
}

void AppendUtf8(std::string& output, std::uint32_t codepoint) {
    if (codepoint <= 0x7f) {
        output.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7ff) {
        output.push_back(static_cast<char>(0xc0 | (codepoint >> 6)));
        output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    } else if (codepoint <= 0xffff) {
        output.push_back(static_cast<char>(0xe0 | (codepoint >> 12)));
        output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    } else {
        output.push_back(static_cast<char>(0xf0 | (codepoint >> 18)));
        output.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    }
}

struct JsonValue {
    using Array = std::vector<JsonValue>;
    using Object = std::map<std::string, JsonValue>;
    using Storage =
        std::variant<std::nullptr_t, bool, std::int64_t, double, std::string, Array, Object>;

    Storage storage;

    [[nodiscard]] const Object* AsObject() const { return std::get_if<Object>(&storage); }
    [[nodiscard]] const Array* AsArray() const { return std::get_if<Array>(&storage); }
    [[nodiscard]] const std::string* AsString() const { return std::get_if<std::string>(&storage); }
    [[nodiscard]] const std::int64_t* AsInteger() const {
        return std::get_if<std::int64_t>(&storage);
    }
    [[nodiscard]] std::optional<double> AsNumber() const {
        if (const auto* integer = std::get_if<std::int64_t>(&storage)) {
            return static_cast<double>(*integer);
        }
        if (const auto* decimal = std::get_if<double>(&storage)) return *decimal;
        return std::nullopt;
    }
    [[nodiscard]] const bool* AsBool() const { return std::get_if<bool>(&storage); }
};

class JsonParser {
public:
    explicit JsonParser(std::string_view input) : input_(input) {}

    JsonValue Parse() {
        SkipWhitespace();
        JsonValue value = ParseValue();
        SkipWhitespace();
        if (position_ != input_.size()) {
            Fail("unexpected trailing data");
        }
        return value;
    }

private:
    [[noreturn]] void Fail(const std::string& message) const {
        throw std::runtime_error("JSON parse error at byte " + std::to_string(position_) +
                                 ": " + message);
    }

    void SkipWhitespace() {
        while (position_ < input_.size() &&
               (input_[position_] == ' ' || input_[position_] == '\t' ||
                input_[position_] == '\r' || input_[position_] == '\n')) {
            ++position_;
        }
    }

    bool Consume(char expected) {
        if (position_ < input_.size() && input_[position_] == expected) {
            ++position_;
            return true;
        }
        return false;
    }

    void Expect(char expected) {
        if (!Consume(expected)) {
            Fail(std::string("expected '") + expected + "'");
        }
    }

    bool ConsumeLiteral(std::string_view literal) {
        if (input_.substr(position_, literal.size()) == literal) {
            position_ += literal.size();
            return true;
        }
        return false;
    }

    JsonValue ParseValue() {
        if (position_ >= input_.size()) {
            Fail("expected a value");
        }
        switch (input_[position_]) {
            case '{': return JsonValue{ParseObject()};
            case '[': return JsonValue{ParseArray()};
            case '"': return JsonValue{ParseString()};
            case 't':
                if (ConsumeLiteral("true")) return JsonValue{true};
                break;
            case 'f':
                if (ConsumeLiteral("false")) return JsonValue{false};
                break;
            case 'n':
                if (ConsumeLiteral("null")) return JsonValue{nullptr};
                break;
            default:
                if (input_[position_] == '-' || std::isdigit(
                        static_cast<unsigned char>(input_[position_]))) {
                    return ParseNumber();
                }
                break;
        }
        Fail("invalid value");
    }

    JsonValue::Object ParseObject() {
        Expect('{');
        SkipWhitespace();
        JsonValue::Object object;
        if (Consume('}')) {
            return object;
        }
        while (true) {
            SkipWhitespace();
            if (position_ >= input_.size() || input_[position_] != '"') {
                Fail("object key must be a string");
            }
            std::string key = ParseString();
            SkipWhitespace();
            Expect(':');
            SkipWhitespace();
            object.insert_or_assign(std::move(key), ParseValue());
            SkipWhitespace();
            if (Consume('}')) {
                return object;
            }
            Expect(',');
        }
    }

    JsonValue::Array ParseArray() {
        Expect('[');
        SkipWhitespace();
        JsonValue::Array array;
        if (Consume(']')) {
            return array;
        }
        while (true) {
            SkipWhitespace();
            array.push_back(ParseValue());
            SkipWhitespace();
            if (Consume(']')) {
                return array;
            }
            Expect(',');
        }
    }

    std::uint32_t ParseHexQuad() {
        if (position_ + 4 > input_.size()) {
            Fail("truncated unicode escape");
        }
        std::uint32_t result = 0;
        for (int index = 0; index < 4; ++index) {
            const char character = input_[position_++];
            result <<= 4;
            if (character >= '0' && character <= '9') {
                result += static_cast<std::uint32_t>(character - '0');
            } else if (character >= 'a' && character <= 'f') {
                result += static_cast<std::uint32_t>(character - 'a' + 10);
            } else if (character >= 'A' && character <= 'F') {
                result += static_cast<std::uint32_t>(character - 'A' + 10);
            } else {
                Fail("invalid unicode escape");
            }
        }
        return result;
    }

    std::string ParseString() {
        Expect('"');
        std::string output;
        while (position_ < input_.size()) {
            const char character = input_[position_++];
            if (character == '"') {
                return output;
            }
            if (static_cast<unsigned char>(character) < 0x20) {
                Fail("control character in string");
            }
            if (character != '\\') {
                output.push_back(character);
                continue;
            }
            if (position_ >= input_.size()) {
                Fail("truncated escape");
            }
            const char escaped = input_[position_++];
            switch (escaped) {
                case '"': output.push_back('"'); break;
                case '\\': output.push_back('\\'); break;
                case '/': output.push_back('/'); break;
                case 'b': output.push_back('\b'); break;
                case 'f': output.push_back('\f'); break;
                case 'n': output.push_back('\n'); break;
                case 'r': output.push_back('\r'); break;
                case 't': output.push_back('\t'); break;
                case 'u': {
                    std::uint32_t codepoint = ParseHexQuad();
                    if (codepoint >= 0xd800 && codepoint <= 0xdbff) {
                        if (position_ + 2 > input_.size() || input_[position_] != '\\' ||
                            input_[position_ + 1] != 'u') {
                            Fail("high surrogate without low surrogate");
                        }
                        position_ += 2;
                        const std::uint32_t low = ParseHexQuad();
                        if (low < 0xdc00 || low > 0xdfff) {
                            Fail("invalid low surrogate");
                        }
                        codepoint = 0x10000 + ((codepoint - 0xd800) << 10) + (low - 0xdc00);
                    } else if (codepoint >= 0xdc00 && codepoint <= 0xdfff) {
                        Fail("unexpected low surrogate");
                    }
                    AppendUtf8(output, codepoint);
                    break;
                }
                default: Fail("invalid string escape");
            }
        }
        Fail("unterminated string");
    }

    JsonValue ParseNumber() {
        const std::size_t start = position_;
        Consume('-');
        if (position_ >= input_.size()) {
            Fail("truncated number");
        }
        if (input_[position_] == '0') {
            ++position_;
        } else {
            if (!std::isdigit(static_cast<unsigned char>(input_[position_]))) {
                Fail("invalid number");
            }
            while (position_ < input_.size() &&
                   std::isdigit(static_cast<unsigned char>(input_[position_]))) {
                ++position_;
            }
        }
        bool decimal = false;
        if (position_ < input_.size() && input_[position_] == '.') {
            decimal = true;
            ++position_;
            const std::size_t fraction_start = position_;
            while (position_ < input_.size() &&
                   std::isdigit(static_cast<unsigned char>(input_[position_]))) {
                ++position_;
            }
            if (position_ == fraction_start) Fail("fraction requires digits");
        }
        if (position_ < input_.size() &&
            (input_[position_] == 'e' || input_[position_] == 'E')) {
            decimal = true;
            ++position_;
            if (position_ < input_.size() &&
                (input_[position_] == '+' || input_[position_] == '-')) {
                ++position_;
            }
            const std::size_t exponent_start = position_;
            while (position_ < input_.size() &&
                   std::isdigit(static_cast<unsigned char>(input_[position_]))) {
                ++position_;
            }
            if (position_ == exponent_start) Fail("exponent requires digits");
        }
        const std::string token(input_.substr(start, position_ - start));
        try {
            if (decimal) return JsonValue{std::stod(token)};
            return JsonValue{std::stoll(token)};
        } catch (const std::exception&) {
            Fail("number is out of range");
        }
    }

    std::string_view input_;
    std::size_t position_{};
};

std::string ReadUtf8File(const std::filesystem::path& path, std::uintmax_t maximum_size) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error) {
        throw std::runtime_error("Could not inspect " + WideToUtf8(path.wstring()) + ": " +
                                 error.message());
    }
    if (size > maximum_size) {
        throw std::runtime_error("File is larger than the supported safety limit: " +
                                 WideToUtf8(path.wstring()));
    }
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw std::runtime_error("Could not open " + WideToUtf8(path.wstring()));
    }
    std::string value(static_cast<std::size_t>(size), '\0');
    stream.read(value.data(), static_cast<std::streamsize>(value.size()));
    if (!stream && !stream.eof()) {
        throw std::runtime_error("Could not read " + WideToUtf8(path.wstring()));
    }
    if (value.size() >= 3 && static_cast<unsigned char>(value[0]) == 0xef &&
        static_cast<unsigned char>(value[1]) == 0xbb &&
        static_cast<unsigned char>(value[2]) == 0xbf) {
        value.erase(0, 3);
    }
    return value;
}

const JsonValue* Member(const JsonValue::Object& object, const char* key) {
    const auto iterator = object.find(key);
    return iterator == object.end() ? nullptr : &iterator->second;
}

void ApplyPositiveInt(const JsonValue::Object& object, const char* key, int& target,
                      int maximum = INT_MAX) {
    const JsonValue* value = Member(object, key);
    if (!value) return;
    const auto* integer = value->AsInteger();
    if (!integer || *integer <= 0 || *integer > maximum) {
        throw std::runtime_error(std::string(key) + " must be a positive integer no larger than " +
                                 std::to_string(maximum));
    }
    target = static_cast<int>(*integer);
}

void ApplyBool(const JsonValue::Object& object, const char* key, bool& target) {
    const JsonValue* value = Member(object, key);
    if (!value) return;
    const auto* boolean = value->AsBool();
    if (!boolean) {
        throw std::runtime_error(std::string(key) + " must be true or false");
    }
    target = *boolean;
}

void ApplyString(const JsonValue::Object& object, const char* key, std::wstring& target) {
    const JsonValue* value = Member(object, key);
    if (!value) return;
    const auto* string = value->AsString();
    if (!string) {
        throw std::runtime_error(std::string(key) + " must be a string");
    }
    target = Utf8ToWide(*string);
}

void ApplyStringArray(const JsonValue::Object& object, const char* key,
                      std::vector<std::wstring>& target) {
    const JsonValue* value = Member(object, key);
    if (!value) return;
    const auto* array = value->AsArray();
    if (!array) {
        throw std::runtime_error(std::string(key) + " must be an array of strings");
    }
    std::vector<std::wstring> replacement;
    replacement.reserve(array->size());
    for (const JsonValue& item : *array) {
        const auto* string = item.AsString();
        if (!string) {
            throw std::runtime_error(std::string(key) + " must be an array of strings");
        }
        replacement.push_back(Utf8ToWide(*string));
    }
    target = std::move(replacement);
}

struct UsageCredentials {
    std::string access_token;
    std::string account_id;
};

struct UsageWindow {
    double used_percent{};
    std::int64_t duration_seconds{};
};

struct UsageUnauthorizedError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

std::string StringMember(const JsonValue::Object& object, const char* key,
                         const char* alternate = nullptr) {
    const JsonValue* value = Member(object, key);
    if (!value && alternate) value = Member(object, alternate);
    const auto* string = value ? value->AsString() : nullptr;
    return string ? *string : std::string();
}

UsageCredentials LoadUsageCredentials() {
    const std::filesystem::path auth_path = CodexHome() / L"auth.json";
    const JsonValue root = JsonParser(ReadUtf8File(auth_path, 2 * 1024 * 1024)).Parse();
    const auto* root_object = root.AsObject();
    if (!root_object) throw std::runtime_error("Codex auth.json must contain a JSON object");
    const JsonValue* tokens_value = Member(*root_object, "tokens");
    const auto* tokens = tokens_value ? tokens_value->AsObject() : nullptr;
    if (!tokens) {
        throw std::runtime_error(
            "Codex usage requires a logged-in ChatGPT account in auth.json");
    }
    UsageCredentials credentials{
        StringMember(*tokens, "access_token", "accessToken"),
        StringMember(*tokens, "account_id", "accountId")};
    if (credentials.access_token.empty()) {
        throw std::runtime_error(
            "Codex auth.json has no access token; open Codex and log in again");
    }
    const auto unsafe = [](const std::string& value) {
        return value.find('\r') != std::string::npos || value.find('\n') != std::string::npos;
    };
    if (unsafe(credentials.access_token) || unsafe(credentials.account_id)) {
        throw std::runtime_error("Codex auth.json contains an invalid header value");
    }
    return credentials;
}

std::wstring ResolveUsageUrl() {
    std::wstring base_url = L"https://chatgpt.com/backend-api";
    const std::filesystem::path config_path = CodexHome() / L"config.toml";
    if (std::filesystem::exists(config_path)) {
        std::istringstream stream(ReadUtf8File(config_path, 1024 * 1024));
        for (std::string line; std::getline(stream, line);) {
            const std::size_t comment = line.find('#');
            if (comment != std::string::npos) line.erase(comment);
            const std::size_t equals = line.find('=');
            if (equals == std::string::npos) continue;
            const std::wstring key = ToLower(Trim(Utf8ToWide(line.substr(0, equals))));
            if (key != L"chatgpt_base_url") continue;
            std::wstring value = Trim(Utf8ToWide(line.substr(equals + 1)));
            if (value.size() >= 2 &&
                ((value.front() == L'"' && value.back() == L'"') ||
                 (value.front() == L'\'' && value.back() == L'\''))) {
                value = Trim(value.substr(1, value.size() - 2));
            }
            if (!value.empty()) base_url = std::move(value);
            break;
        }
    }
    while (!base_url.empty() && base_url.back() == L'/') base_url.pop_back();
    const std::wstring lowered = ToLower(base_url);
    if ((lowered.rfind(L"https://chatgpt.com", 0) == 0 ||
         lowered.rfind(L"https://chat.openai.com", 0) == 0) &&
        lowered.find(L"/backend-api") == std::wstring::npos) {
        base_url += L"/backend-api";
    }
    return base_url +
           (ToLower(base_url).find(L"/backend-api") != std::wstring::npos
                ? L"/wham/usage"
                : L"/api/codex/usage");
}

[[noreturn]] void ThrowWinHttpError(const char* operation) {
    throw std::runtime_error(std::string(operation) + " failed with Windows error " +
                             std::to_string(GetLastError()));
}

std::string HttpGetUsage(const std::wstring& url, const UsageCredentials& credentials) {
    URL_COMPONENTS components{};
    components.dwStructSize = sizeof(components);
    components.dwSchemeLength = static_cast<DWORD>(-1);
    components.dwHostNameLength = static_cast<DWORD>(-1);
    components.dwUrlPathLength = static_cast<DWORD>(-1);
    components.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &components)) {
        ThrowWinHttpError("WinHttpCrackUrl");
    }
    if (components.nScheme != INTERNET_SCHEME_HTTP &&
        components.nScheme != INTERNET_SCHEME_HTTPS) {
        throw std::runtime_error("Codex usage URL must use HTTP or HTTPS");
    }
    const std::wstring host(components.lpszHostName, components.dwHostNameLength);
    std::wstring target = components.dwUrlPathLength
                              ? std::wstring(components.lpszUrlPath, components.dwUrlPathLength)
                              : L"/";
    if (components.dwExtraInfoLength) {
        target.append(components.lpszExtraInfo, components.dwExtraInfoLength);
    }

    using InternetHandle = std::unique_ptr<void, decltype(&WinHttpCloseHandle)>;
    InternetHandle session(
        WinHttpOpen(L"CodexOrganizer/0.3.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0),
        &WinHttpCloseHandle);
    if (!session) ThrowWinHttpError("WinHttpOpen");
    InternetHandle connection(
        WinHttpConnect(session.get(), host.c_str(), components.nPort, 0),
        &WinHttpCloseHandle);
    if (!connection) ThrowWinHttpError("WinHttpConnect");
    const wchar_t* accept_types[] = {L"application/json", nullptr};
    InternetHandle request(
        WinHttpOpenRequest(connection.get(), L"GET", target.c_str(), nullptr,
                           WINHTTP_NO_REFERER, accept_types,
                           components.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0),
        &WinHttpCloseHandle);
    if (!request) ThrowWinHttpError("WinHttpOpenRequest");
    if (!WinHttpSetTimeouts(request.get(), 15000, 15000, 15000, 15000)) {
        ThrowWinHttpError("WinHttpSetTimeouts");
    }

    std::wstring headers = L"Authorization: Bearer " + Utf8ToWide(credentials.access_token) +
                           L"\r\nAccept: application/json\r\n";
    if (!credentials.account_id.empty()) {
        headers += L"ChatGPT-Account-Id: " + Utf8ToWide(credentials.account_id) + L"\r\n";
    }
    if (!WinHttpSendRequest(request.get(), headers.c_str(), static_cast<DWORD>(headers.size()),
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        ThrowWinHttpError("WinHttpSendRequest");
    }
    if (!WinHttpReceiveResponse(request.get(), nullptr)) {
        ThrowWinHttpError("WinHttpReceiveResponse");
    }
    DWORD status_code = 0;
    DWORD status_size = sizeof(status_code);
    if (!WinHttpQueryHeaders(request.get(),
                             WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status_code, &status_size,
                             WINHTTP_NO_HEADER_INDEX)) {
        ThrowWinHttpError("WinHttpQueryHeaders");
    }
    if (status_code == 401 || status_code == 403) {
        throw UsageUnauthorizedError("Codex login expired; open Codex and log in again");
    }
    if (status_code < 200 || status_code >= 300) {
        throw std::runtime_error("Codex usage API returned HTTP " +
                                 std::to_string(status_code));
    }

    constexpr std::size_t maximum_response_bytes = 1024 * 1024;
    std::string body;
    while (true) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.get(), &available)) {
            ThrowWinHttpError("WinHttpQueryDataAvailable");
        }
        if (available == 0) break;
        if (body.size() + available > maximum_response_bytes) {
            throw std::runtime_error("Codex usage response exceeded the 1 MiB safety limit");
        }
        const std::size_t start = body.size();
        body.resize(start + available);
        DWORD read = 0;
        if (!WinHttpReadData(request.get(), body.data() + start, available, &read)) {
            ThrowWinHttpError("WinHttpReadData");
        }
        body.resize(start + read);
        if (read == 0) break;
    }
    return body;
}

std::optional<UsageWindow> ParseUsageWindow(const JsonValue* value) {
    const auto* object = value ? value->AsObject() : nullptr;
    if (!object) return std::nullopt;
    const JsonValue* used_value = Member(*object, "used_percent");
    const JsonValue* duration_value = Member(*object, "limit_window_seconds");
    const std::optional<double> used = used_value ? used_value->AsNumber() : std::nullopt;
    const std::optional<double> duration =
        duration_value ? duration_value->AsNumber() : std::nullopt;
    if (!used || !duration || !std::isfinite(*used) || !std::isfinite(*duration) ||
        *duration < 0) {
        return std::nullopt;
    }
    return UsageWindow{*used, static_cast<std::int64_t>(std::llround(*duration))};
}

}  // namespace

bool Rect::Contains(const Rect& other) const noexcept {
    return left <= other.left && top <= other.top && right >= other.right &&
           bottom >= other.bottom;
}

std::wstring Rect::Xywh() const {
    return std::to_wstring(left) + L"," + std::to_wstring(top) + L"," +
           std::to_wstring(Width()) + L"," + std::to_wstring(Height());
}

std::wstring MonitorInfo::Label() const {
    return device_name + (primary ? L" (Primary)" : L"") + L" - " +
           std::to_wstring(work_area.Width()) + L"x" +
           std::to_wstring(work_area.Height()) + L" at " +
           std::to_wstring(work_area.left) + L"," + std::to_wstring(work_area.top);
}

std::wstring WindowInfo::ThreadLabel() const {
    if (!thread_name.empty()) return thread_name;
    if (!thread_id.empty()) return L"(unnamed) " + thread_id.substr(0, 8);
    return L"(unknown)";
}

std::vector<WindowInfo> DetectionResult::AutoWindows() const {
    std::vector<WindowInfo> selected;
    std::copy_if(windows.begin(), windows.end(), std::back_inserter(selected),
                 [](const WindowInfo& window) { return window.auto_selected; });
    return selected;
}

std::size_t DetectionResult::NamedThreadCount() const noexcept {
    return static_cast<std::size_t>(std::count_if(
        windows.begin(), windows.end(),
        [](const WindowInfo& window) { return !window.thread_name.empty(); }));
}

std::wstring ToLower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](wchar_t character) { return static_cast<wchar_t>(std::towlower(character)); });
    return value;
}

bool MatchesAny(const std::wstring& value, const std::vector<std::wstring>& patterns) {
    const std::wstring lowered = ToLower(value);
    return std::any_of(patterns.begin(), patterns.end(), [&](const std::wstring& pattern) {
        return !pattern.empty() && lowered.find(ToLower(pattern)) != std::wstring::npos;
    });
}

Config LoadConfig(const std::filesystem::path& path) {
    Config config;
    if (!std::filesystem::exists(path)) {
        return config;
    }
    const JsonValue root = JsonParser(ReadUtf8File(path, 1024 * 1024)).Parse();
    const auto* object = root.AsObject();
    if (!object) {
        throw std::runtime_error(WideToUtf8(path.wstring()) + " must contain a JSON object");
    }

    ApplyPositiveInt(*object, "preferred_width", config.preferred_width);
    ApplyPositiveInt(*object, "preferred_height", config.preferred_height);
    ApplyPositiveInt(*object, "preferred_columns", config.preferred_columns);
    ApplyPositiveInt(*object, "minimum_width", config.minimum_width);
    ApplyPositiveInt(*object, "minimum_height", config.minimum_height);
    ApplyPositiveInt(*object, "gap", config.gap);
    ApplyPositiveInt(*object, "outer_margin", config.outer_margin);
    ApplyPositiveInt(*object, "terminal_text_tail_characters",
                     config.terminal_text_tail_characters, 65536);

    ApplyStringArray(*object, "terminal_class_names", config.terminal_class_names);
    ApplyStringArray(*object, "title_include_patterns", config.title_include_patterns);
    ApplyStringArray(*object, "title_exclude_patterns", config.title_exclude_patterns);
    ApplyStringArray(*object, "codex_process_names", config.codex_process_names);
    ApplyStringArray(*object, "codex_path_exclude_patterns",
                     config.codex_path_exclude_patterns);

    ApplyBool(*object, "organize_by_thread_name", config.organize_by_thread_name);
    ApplyBool(*object, "unnamed_threads_last", config.unnamed_threads_last);
    ApplyBool(*object, "request_account_usage", config.request_account_usage);
    ApplyBool(*object, "restore_minimized", config.restore_minimized);
    ApplyBool(*object, "bring_to_front", config.bring_to_front);
    ApplyBool(*object, "minimize_after_arrange", config.minimize_after_arrange);
    ApplyBool(*object, "confirm_before_move", config.confirm_before_move);
    ApplyString(*object, "session_index_path", config.session_index_path);

    if (const JsonValue* monitor = Member(*object, "monitor")) {
        if (const auto* string = monitor->AsString()) {
            config.monitor = Utf8ToWide(*string);
        } else if (const auto* number = monitor->AsInteger(); number && *number > 0) {
            config.monitor = std::to_wstring(*number);
        } else {
            throw std::runtime_error(
                "monitor must be 'primary', a display name, or a positive 1-based number");
        }
    }
    return config;
}

int ChooseColumnCount(int count, const Rect& work_area, const Config& config) {
    if (count <= 0) {
        throw std::invalid_argument("count must be positive");
    }
    const int preferred = std::min(count, config.preferred_columns);
    const auto cell_size = [&](int columns) {
        const int rows = (count + columns - 1) / columns;
        const int width = std::max(
            1, (work_area.Width() - 2 * config.outer_margin - config.gap * (columns - 1)) /
                   columns);
        const int height = std::max(
            1, (work_area.Height() - 2 * config.outer_margin - config.gap * (rows - 1)) /
                   rows);
        return std::pair(width, height);
    };

    for (int columns = preferred; columns <= count; ++columns) {
        const auto [width, height] = cell_size(columns);
        if (width >= config.minimum_width && height >= config.minimum_height) {
            return columns;
        }
    }

    double best_score = -std::numeric_limits<double>::infinity();
    int best_distance = INT_MAX;
    int best_columns = preferred;
    for (int columns = 1; columns <= count; ++columns) {
        const auto [width, height] = cell_size(columns);
        const double score = std::min(static_cast<double>(width) / config.minimum_width,
                                      static_cast<double>(height) / config.minimum_height);
        const int distance = std::abs(columns - preferred);
        if (score > best_score ||
            (score == best_score &&
             (distance < best_distance ||
              (distance == best_distance && columns < best_columns)))) {
            best_score = score;
            best_distance = distance;
            best_columns = columns;
        }
    }
    return best_columns;
}

LayoutPlan CalculateLayout(int count, const Rect& work_area, const Config& config) {
    if (count <= 0) {
        throw std::invalid_argument("At least one window is required");
    }
    const int columns = ChooseColumnCount(count, work_area, config);
    const int rows = (count + columns - 1) / columns;
    const int usable_width = work_area.Width() - 2 * config.outer_margin -
                             config.gap * (columns - 1);
    const int usable_height = work_area.Height() - 2 * config.outer_margin -
                              config.gap * (rows - 1);
    const int cell_width = std::max(1, usable_width / columns);
    const int cell_height = std::max(1, usable_height / rows);
    const int width = std::min(config.preferred_width, cell_width);
    const int height = std::min(config.preferred_height, cell_height);
    const int total_height = rows * height + (rows - 1) * config.gap;
    const int y_start = work_area.top + std::max(0, (work_area.Height() - total_height) / 2);

    LayoutPlan plan{columns, rows, work_area, {}};
    plan.rectangles.reserve(static_cast<std::size_t>(count));
    int remaining = count;
    for (int row = 0; row < rows; ++row) {
        const int in_row = std::min(columns, remaining);
        const int row_width = in_row * width + (in_row - 1) * config.gap;
        const int x_start = work_area.left + std::max(0, (work_area.Width() - row_width) / 2);
        const int y = y_start + row * (height + config.gap);
        for (int column = 0; column < in_row; ++column) {
            const int x = x_start + column * (width + config.gap);
            plan.rectangles.push_back({x, y, x + width, y + height});
        }
        remaining -= in_row;
    }
    return plan;
}

AssociationResult AssociateCodexWindows(
    std::vector<WindowInfo> candidates,
    std::size_t standalone_process_count,
    const std::vector<std::wstring>& include_patterns,
    const std::vector<std::wstring>& exclude_patterns) {
    std::size_t status_matches = 0;
    std::size_t title_matches = 0;
    for (WindowInfo& window : candidates) {
        const bool excluded = MatchesAny(window.title, exclude_patterns);
        const bool status_match = !window.codex_status.empty() && !window.codex_model.empty();
        const bool title_match = MatchesAny(window.title, include_patterns);
        window.auto_selected = !excluded && (status_match || title_match);
        if (excluded) {
            window.selection_reason = L"excluded by title";
        } else if (status_match) {
            ++status_matches;
            window.selection_reason = L"Codex status line";
        } else if (title_match) {
            ++title_matches;
            window.selection_reason = L"title match";
        } else {
            window.selection_reason = L"no Codex evidence";
        }
    }
    const std::wstring mode = status_matches ? L"status-line" : title_matches ? L"title-match" : L"manual";
    std::wstring warning;
    if (standalone_process_count > 0 && status_matches + title_matches == 0) {
        warning = L"No Codex terminal could be identified from its status line or title. "
                  L"Refresh with the Codex tab visible, or use --all-terminals to inspect candidates.";
    }
    return {std::move(candidates), std::move(mode), std::move(warning)};
}

std::filesystem::path CodexHome() {
    wchar_t* configured = nullptr;
    std::size_t length = 0;
    if (_wdupenv_s(&configured, &length, L"CODEX_HOME") == 0 && configured) {
        const std::wstring value = Trim(configured);
        std::free(configured);
        if (!value.empty()) return std::filesystem::path(value);
    }
    configured = nullptr;
    length = 0;
    if (_wdupenv_s(&configured, &length, L"USERPROFILE") == 0 && configured) {
        const std::filesystem::path value = std::filesystem::path(configured) / L".codex";
        std::free(configured);
        return value;
    }
    return std::filesystem::current_path() / L".codex";
}

std::map<std::wstring, std::wstring> LoadThreadNameIndex(
    const std::optional<std::filesystem::path>& path) {
    const std::filesystem::path index_path = path.value_or(CodexHome() / L"session_index.jsonl");
    if (!std::filesystem::exists(index_path)) return {};
    std::ifstream stream(index_path, std::ios::binary);
    if (!stream) {
        throw std::runtime_error("Could not read Codex session index: " +
                                 WideToUtf8(index_path.wstring()));
    }
    std::map<std::wstring, std::wstring> names;
    std::string line;
    while (std::getline(stream, line)) {
        if (line.size() > 1024 * 1024) continue;
        try {
            const JsonValue row = JsonParser(line).Parse();
            const auto* object = row.AsObject();
            if (!object) continue;
            const JsonValue* id_value = Member(*object, "id");
            const JsonValue* name_value = Member(*object, "thread_name");
            if (!id_value || !name_value) continue;
            const auto* id_string = id_value->AsString();
            const auto* name_string = name_value->AsString();
            if (!id_string || !name_string) continue;
            const std::wstring id = ToLower(Trim(Utf8ToWide(*id_string)));
            const std::wstring name = Trim(Utf8ToWide(*name_string));
            if (IsUuid(id) && !name.empty()) names[id] = name;
        } catch (const std::exception&) {
            continue;
        }
    }
    return names;
}

ThreadIdentity ParseThreadIdentity(
    const std::wstring& terminal_tail,
    const std::map<std::wstring, std::wstring>& names_by_id) {
    std::map<std::wstring, std::wstring> normalized_names;
    std::map<std::wstring, std::vector<std::wstring>> ids_by_name;
    for (const auto& [thread_id, name] : names_by_id) {
        const std::wstring normalized_id = ToLower(thread_id);
        normalized_names[normalized_id] = name;
        ids_by_name[ToLower(name)].push_back(normalized_id);
    }

    std::vector<std::wstring> segments = LatestStatusSegments(terminal_tail);
    for (std::size_t index = 0; index < segments.size(); ++index) {
        const std::wstring footer_id = FindUuid(segments[index]);
        if (!footer_id.empty()) {
            const std::wstring thread_id = footer_id;
            const auto name = normalized_names.find(thread_id);
            return {name == normalized_names.end() ? L"" : name->second,
                    thread_id,
                    L"footer-id"};
        }
    }
    for (std::size_t index = 0; index < segments.size(); ++index) {
        const std::wstring& label = segments[index];
        if (IsIgnoredIdentitySegment(label)) continue;
        const auto ids = ids_by_name.find(ToLower(label));
        if (ids != ids_by_name.end() && ids->second.size() == 1) {
            const std::wstring& thread_id = ids->second.front();
            return {normalized_names.at(thread_id), thread_id, L"footer-name"};
        }
    }
    for (std::size_t index = 0; index < segments.size(); ++index) {
        const std::wstring& label = segments[index];
        if (IsIgnoredIdentitySegment(label)) continue;
        return {label, L"", L"footer-name"};
    }

    std::wstring normalized;
    bool prior_space = true;
    for (wchar_t character : terminal_tail) {
        if (std::iswspace(character)) {
            if (!prior_space) normalized.push_back(L' ');
            prior_space = true;
        } else {
            normalized.push_back(character);
            prior_space = false;
        }
    }
    normalized = Trim(normalized);
    const std::wstring lowered = ToLower(normalized);
    const std::wstring prefix = L"session renamed to ";
    const std::wstring suffix = L". to resume this session run codex resume";
    const std::size_t start = lowered.rfind(prefix);
    if (start != std::wstring::npos) {
        const std::size_t name_start = start + prefix.size();
        const std::size_t end = lowered.find(suffix, name_start);
        if (end != std::wstring::npos && end > name_start) {
            const std::wstring name = Trim(normalized.substr(name_start, end - name_start));
            const auto ids = ids_by_name.find(ToLower(name));
            return {name,
                    ids != ids_by_name.end() && ids->second.size() == 1 ? ids->second.front()
                                                                          : L"",
                    L"rename-message"};
        }
    }
    return {};
}

CodexStatusLine ParseCodexStatusLine(const std::wstring& terminal_tail) {
    const std::vector<std::wstring> segments = LatestStatusSegments(terminal_tail);
    if (segments.empty()) return {};
    CodexStatusLine result;
    for (std::size_t index = 0; index < segments.size(); ++index) {
        const std::wstring lowered = ToLower(Trim(segments[index]));
        if (result.status.empty() &&
            (lowered.rfind(L"ready", 0) == 0 || lowered.rfind(L"working", 0) == 0 ||
             lowered.rfind(L"thinking", 0) == 0 || lowered.rfind(L"waiting", 0) == 0)) {
            result.status = segments[index];
        }
        if (result.model.empty() && lowered.find(L"gpt") != std::wstring::npos) {
            result.model = segments[index];
        }
        if (result.used_tokens.empty()) {
            result.used_tokens = ExtractUsedTokens(segments[index]);
        }
    }
    return result;
}

std::wstring ParseCodexWeeklyUsageResponse(const std::string& response_body) {
    const JsonValue root = JsonParser(response_body).Parse();
    const auto* root_object = root.AsObject();
    const JsonValue* rate_limit_value = root_object ? Member(*root_object, "rate_limit") : nullptr;
    const auto* rate_limit = rate_limit_value ? rate_limit_value->AsObject() : nullptr;
    if (!rate_limit) {
        throw std::runtime_error("Codex usage response has no rate_limit object");
    }
    std::vector<UsageWindow> windows;
    if (const auto primary = ParseUsageWindow(Member(*rate_limit, "primary_window"))) {
        windows.push_back(*primary);
    }
    if (const auto secondary = ParseUsageWindow(Member(*rate_limit, "secondary_window"))) {
        windows.push_back(*secondary);
    }
    if (windows.empty()) {
        throw std::runtime_error("Codex usage response has no usable rate-limit windows");
    }
    constexpr std::int64_t weekly_minimum_seconds = 6 * 24 * 60 * 60;
    const auto weekly = std::find_if(windows.begin(), windows.end(), [](const UsageWindow& item) {
        return item.duration_seconds >= weekly_minimum_seconds;
    });
    const UsageWindow* selected = weekly != windows.end()
                                      ? &*weekly
                                      : (windows.size() > 1 ? &windows[1] : nullptr);
    if (!selected) {
        throw std::runtime_error("Codex usage response has no weekly rate-limit window");
    }
    const int used_percent = static_cast<int>(
        std::lround(std::clamp(selected->used_percent, 0.0, 100.0)));
    return L"weekly " + std::to_wstring(std::max(0, 100 - used_percent)) + L"% left";
}

std::wstring FetchCodexWeeklyUsage() {
    UsageCredentials credentials = LoadUsageCredentials();
    const std::wstring url = ResolveUsageUrl();
    try {
        return ParseCodexWeeklyUsageResponse(HttpGetUsage(url, credentials));
    } catch (const UsageUnauthorizedError&) {
        UsageCredentials reloaded = LoadUsageCredentials();
        if (reloaded.access_token == credentials.access_token) throw;
        return ParseCodexWeeklyUsageResponse(HttpGetUsage(url, reloaded));
    }
}

void ApplyCodexProcessUptimes(std::vector<WindowInfo>& windows,
    const std::vector<ProcessInfo>& processes,
    const std::map<std::uint32_t, std::uintptr_t>& console_windows,
    std::uint64_t now_100ns) {
    for (auto& window : windows) {
        window.process_uptime = L"\u2014";
        window.codex_process_id = 0;
        const ProcessInfo* match = nullptr;
        bool ambiguous = false;
        for (const auto& process : processes) {
            const auto mapped = console_windows.find(process.process_id);
            if (mapped == console_windows.end() || mapped->second != window.hwnd) continue;
            if (match) { ambiguous = true; break; }
            match = &process;
        }
        if (!match || ambiguous || !match->creation_time_100ns ||
            now_100ns < match->creation_time_100ns) continue;
        window.codex_process_id = match->process_id;
        window.process_uptime = FormatUptime((now_100ns - match->creation_time_100ns) / 10000000ULL);
    }
}

std::wstring FormatUptime(std::uint64_t elapsed_seconds) {
    const std::uint64_t total_minutes = elapsed_seconds / 60;
    if (total_minutes == 0) return L"<1m";
    if (total_minutes < 60) return std::to_wstring(total_minutes) + L"m";
    const std::uint64_t total_hours = total_minutes / 60;
    const std::uint64_t minutes = total_minutes % 60;
    if (total_hours < 24) {
        return std::to_wstring(total_hours) + L"h" +
               (minutes == 0 ? std::wstring() : L" " + std::to_wstring(minutes) + L"m");
    }
    const std::uint64_t days = total_hours / 24;
    const std::uint64_t hours = total_hours % 24;
    return std::to_wstring(days) + L"d" +
           (hours == 0 ? std::wstring() : L" " + std::to_wstring(hours) + L"h");
}

bool ThreadLess(const WindowInfo& first, const WindowInfo& second, bool unnamed_last) {
    const std::wstring first_name = Trim(first.thread_name);
    const std::wstring second_name = Trim(second.thread_name);
    const int first_rank = unnamed_last && first_name.empty() ? 1 : 0;
    const int second_rank = unnamed_last && second_name.empty() ? 1 : 0;
    if (first_rank != second_rank) return first_rank < second_rank;
    const std::wstring first_identity = ToLower(first_name.empty() ? first.thread_id : first_name);
    const std::wstring second_identity = ToLower(second_name.empty() ? second.thread_id : second_name);
    if (first_identity != second_identity) return first_identity < second_identity;
    return first.z_order < second.z_order;
}

void SortWindowsByThread(std::vector<WindowInfo>& windows, bool unnamed_last) {
    std::stable_sort(windows.begin(), windows.end(),
                     [unnamed_last](const WindowInfo& first, const WindowInfo& second) {
                         return ThreadLess(first, second, unnamed_last);
                     });
}

std::wstring FormatPlan(const std::vector<WindowInfo>& windows,
                        const MonitorInfo& monitor,
                        const LayoutPlan& plan) {
    std::wostringstream output;
    output << L"Monitor: " << monitor.Label() << L"\nLayout: " << windows.size()
           << L" windows, " << plan.columns << L" columns x " << plan.rows << L" rows";
    for (std::size_t index = 0; index < windows.size() && index < plan.rectangles.size(); ++index) {
        output << L"\n" << (index + 1) << L". HWND " << windows[index].hwnd << L"  "
               << plan.rectangles[index].Xywh() << L"  " << windows[index].ThreadLabel();
    }
    return output.str();
}

std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                          static_cast<int>(value.size()), nullptr, 0);
    if (count <= 0) throw std::runtime_error("Invalid UTF-8 text");
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                        static_cast<int>(value.size()), result.data(), count);
    return result;
}

std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                                          static_cast<int>(value.size()), nullptr, 0, nullptr,
                                          nullptr);
    if (count <= 0) throw std::runtime_error("Could not encode UTF-8 text");
    std::string result(static_cast<std::size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                        static_cast<int>(value.size()), result.data(), count, nullptr, nullptr);
    return result;
}

}  // namespace codex_organizer
