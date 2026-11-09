#pragma once
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace lt::xml {

struct Element {
    std::string name;
    std::vector<std::pair<std::string, std::string>> attributes;
    std::vector<std::unique_ptr<Element>> children;

    const std::string* attribute(std::string_view key) const;
    std::string text(std::string_view key, std::string_view fallback = {}) const;
    int integer(std::string_view key, int fallback = 0) const;
    double number(std::string_view key, double fallback = 0) const;
    const Element* child(std::string_view child_name) const;
    std::vector<const Element*> all(std::string_view child_name) const;
};

// Parses a whole document. Returns null and sets error on malformed input.
std::unique_ptr<Element> parse(std::string_view text, std::string* error = nullptr);

} // namespace lt::xml
