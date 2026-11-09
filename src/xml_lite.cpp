#include "xml_lite.h"

#include <cstdlib>
#include <cstring>

namespace lt::xml {

const std::string* Element::attribute(std::string_view key) const
{
    for (const auto& [k, v] : attributes)
        if (k == key) return &v;
    return nullptr;
}

std::string Element::text(std::string_view key, std::string_view fallback) const
{
    const auto* v = attribute(key);
    return v ? *v : std::string(fallback);
}

int Element::integer(std::string_view key, int fallback) const
{
    const auto* v = attribute(key);
    if (!v || v->empty()) return fallback;
    char* end = nullptr;
    const long value = strtol(v->c_str(), &end, 10);
    return end && end != v->c_str() ? static_cast<int>(value) : fallback;
}

double Element::number(std::string_view key, double fallback) const
{
    const auto* v = attribute(key);
    if (!v || v->empty()) return fallback;
    char* end = nullptr;
    const double value = strtod(v->c_str(), &end);
    return end && end != v->c_str() ? value : fallback;
}

const Element* Element::child(std::string_view child_name) const
{
    for (const auto& c : children)
        if (c->name == child_name) return c.get();
    return nullptr;
}

std::vector<const Element*> Element::all(std::string_view child_name) const
{
    std::vector<const Element*> out;
    for (const auto& c : children)
        if (c->name == child_name) out.push_back(c.get());
    return out;
}

namespace {

class Parser {
public:
    explicit Parser(std::string_view text) : s_(text) {}

    std::unique_ptr<Element> document(std::string* error)
    {
        auto root = std::make_unique<Element>();
        root->name = "#document";
        std::vector<Element*> stack{root.get()};
        while (pos_ < s_.size()) {
            const size_t lt = s_.find('<', pos_);
            if (lt == std::string_view::npos) break;
            pos_ = lt + 1;
            if (pos_ >= s_.size()) return fail(error, "truncated tag");
            const char c = s_[pos_];
            if (c == '?') { // <?xml ... ?>
                if (!skip_past("?>")) return fail(error, "unterminated processing instruction");
                continue;
            }
            if (c == '!') {
                if (s_.substr(pos_, 3) == "!--") {
                    if (!skip_past("-->")) return fail(error, "unterminated comment");
                } else if (s_.substr(pos_, 8) == "![CDATA[") {
                    if (!skip_past("]]>")) return fail(error, "unterminated CDATA");
                } else if (!skip_past(">")) {
                    return fail(error, "unterminated declaration");
                }
                continue;
            }
            if (c == '/') { // closing tag
                ++pos_;
                const std::string_view name = read_name();
                if (!skip_past(">")) return fail(error, "unterminated closing tag");
                if (stack.size() <= 1 || stack.back()->name != name) return fail(error, "mismatched closing tag");
                stack.pop_back();
                continue;
            }
            auto element = std::make_unique<Element>();
            element->name = std::string(read_name());
            if (element->name.empty()) return fail(error, "empty tag name");
            bool self_closing = false;
            for (;;) {
                skip_space();
                if (pos_ >= s_.size()) return fail(error, "truncated attributes");
                if (s_[pos_] == '/') {
                    self_closing = true;
                    ++pos_;
                    skip_space();
                    if (pos_ >= s_.size() || s_[pos_] != '>') return fail(error, "bad self-closing tag");
                    ++pos_;
                    break;
                }
                if (s_[pos_] == '>') {
                    ++pos_;
                    break;
                }
                const std::string_view key = read_name();
                if (key.empty()) return fail(error, "bad attribute name");
                skip_space();
                if (pos_ >= s_.size() || s_[pos_] != '=') return fail(error, "attribute without value");
                ++pos_;
                skip_space();
                if (pos_ >= s_.size() || (s_[pos_] != '"' && s_[pos_] != '\'')) return fail(error, "unquoted attribute");
                const char quote = s_[pos_++];
                const size_t end = s_.find(quote, pos_);
                if (end == std::string_view::npos) return fail(error, "unterminated attribute");
                element->attributes.emplace_back(std::string(key), decode(s_.substr(pos_, end - pos_)));
                pos_ = end + 1;
            }
            Element* raw = element.get();
            stack.back()->children.push_back(std::move(element));
            if (!self_closing) {
                if (stack.size() > 256) return fail(error, "nesting too deep");
                stack.push_back(raw);
            }
        }
        if (stack.size() != 1) return fail(error, "unclosed element " + stack.back()->name);
        return root;
    }

private:
    std::string_view s_;
    size_t pos_ = 0;

    static bool name_char(char c)
    {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' ||
               c == '.' || c == ':';
    }
    std::string_view read_name()
    {
        const size_t start = pos_;
        while (pos_ < s_.size() && name_char(s_[pos_])) ++pos_;
        return s_.substr(start, pos_ - start);
    }
    void skip_space()
    {
        while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\r' || s_[pos_] == '\n')) ++pos_;
    }
    bool skip_past(std::string_view token)
    {
        const size_t end = s_.find(token, pos_);
        if (end == std::string_view::npos) return false;
        pos_ = end + token.size();
        return true;
    }
    static std::unique_ptr<Element> fail(std::string* error, const std::string& why)
    {
        if (error) *error = why;
        return nullptr;
    }
    static void utf8(std::string& out, unsigned long cp)
    {
        if (cp < 0x80) out += static_cast<char>(cp);
        else if (cp < 0x800) {
            out += static_cast<char>(0xc0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3f));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xe0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
            out += static_cast<char>(0x80 | (cp & 0x3f));
        } else {
            out += static_cast<char>(0xf0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3f));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
            out += static_cast<char>(0x80 | (cp & 0x3f));
        }
    }
    static std::string decode(std::string_view v)
    {
        std::string out;
        out.reserve(v.size());
        for (size_t i = 0; i < v.size(); ++i) {
            if (v[i] != '&') {
                out += v[i];
                continue;
            }
            const size_t semi = v.find(';', i);
            if (semi == std::string_view::npos || semi - i > 10) {
                out += v[i];
                continue;
            }
            const std::string_view entity = v.substr(i + 1, semi - i - 1);
            if (entity == "lt") out += '<';
            else if (entity == "gt") out += '>';
            else if (entity == "amp") out += '&';
            else if (entity == "quot") out += '"';
            else if (entity == "apos") out += '\'';
            else if (!entity.empty() && entity[0] == '#') {
                const std::string digits(entity.substr(1));
                const unsigned long cp =
                    (!digits.empty() && (digits[0] == 'x' || digits[0] == 'X')) ? strtoul(digits.c_str() + 1, nullptr, 16)
                                                                                 : strtoul(digits.c_str(), nullptr, 10);
                utf8(out, cp);
            } else {
                out.append(v.substr(i, semi - i + 1));
            }
            i = semi;
        }
        return out;
    }
};

} // namespace

std::unique_ptr<Element> parse(std::string_view text, std::string* error)
{
    return Parser(text).document(error);
}

} // namespace lt::xml
