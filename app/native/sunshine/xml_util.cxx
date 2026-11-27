// SPDX-License-Identifier: GPL-3.0-or-later
// 极简 XML 解析：解析 Sunshine 的 serverinfo / applist / launch / pair 响应。
#include "xml_util.h"

#include <cctype>
#include <cstdlib>

namespace tg {
namespace sunshine {
namespace {

bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

bool is_name_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == ':' || c == '.';
}

std::string trim(const std::string& value) {
    size_t begin = 0;
    while (begin < value.size() && is_space(value[begin])) {
        ++begin;
    }
    size_t end = value.size();
    while (end > begin && is_space(value[end - 1])) {
        --end;
    }
    return value.substr(begin, end - begin);
}

void append_utf8(std::string& out, unsigned long code_point) {
    if (code_point <= 0x7F) {
        out.push_back(static_cast<char>(code_point));
    } else if (code_point <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (code_point >> 6)));
        out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    } else if (code_point <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (code_point >> 12)));
        out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    } else if (code_point <= 0x10FFFF) {
        out.push_back(static_cast<char>(0xF0 | (code_point >> 18)));
        out.push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    } else {
        out.push_back('?');
    }
}

// 只处理 XML 预定义实体与数字字符引用；未知实体原样保留。
std::string decode_entities(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '&') {
            out.push_back(text[i]);
            continue;
        }
        size_t semicolon = text.find(';', i + 1);
        if (semicolon == std::string::npos || semicolon - i > 12) {
            out.push_back(text[i]);
            continue;
        }
        std::string entity = text.substr(i + 1, semicolon - i - 1);
        if (entity == "amp") {
            out.push_back('&');
        } else if (entity == "lt") {
            out.push_back('<');
        } else if (entity == "gt") {
            out.push_back('>');
        } else if (entity == "quot") {
            out.push_back('"');
        } else if (entity == "apos") {
            out.push_back('\'');
        } else if (entity.size() > 1 && entity[0] == '#') {
            unsigned long code_point = 0;
            bool ok = false;
            if (entity.size() > 2 && (entity[1] == 'x' || entity[1] == 'X')) {
                char* end = nullptr;
                code_point = std::strtoul(entity.c_str() + 2, &end, 16);
                ok = end != nullptr && *end == '\0';
            } else {
                char* end = nullptr;
                code_point = std::strtoul(entity.c_str() + 1, &end, 10);
                ok = end != nullptr && *end == '\0';
            }
            if (ok) {
                append_utf8(out, code_point);
            } else {
                out.append(text, i, semicolon - i + 1);
            }
        } else {
            out.append(text, i, semicolon - i + 1);
        }
        i = semicolon;
    }
    return out;
}

class Parser {
public:
    explicit Parser(const std::string& text) : text_(text) {}

    bool parse(XmlNode& out) {
        skip_misc();
        if (pos_ >= text_.size()) {
            error_ = "空文档";
            return false;
        }
        if (!parse_element(out)) {
            return false;
        }
        skip_misc();
        if (pos_ < text_.size()) {
            error_ = "根元素之后存在多余内容（偏移 " + std::to_string(pos_) + "）";
            return false;
        }
        return true;
    }

    const std::string& error() const { return error_; }

private:
    bool starts_with(const char* literal) const {
        size_t length = std::char_traits<char>::length(literal);
        return text_.compare(pos_, length, literal) == 0;
    }

    void skip_space() {
        while (pos_ < text_.size() && is_space(text_[pos_])) {
            ++pos_;
        }
    }

    bool skip_until(const char* terminator) {
        size_t end = text_.find(terminator, pos_);
        if (end == std::string::npos) {
            error_ = std::string("未找到结束标记 ") + terminator;
            return false;
        }
        pos_ = end + std::char_traits<char>::length(terminator);
        return true;
    }

    // 跳过空白、XML 声明、注释、DOCTYPE 等元素之外的内容。
    void skip_misc() {
        for (;;) {
            skip_space();
            if (starts_with("<?")) {
                if (!skip_until("?>")) {
                    return;
                }
            } else if (starts_with("<!--")) {
                if (!skip_until("-->")) {
                    return;
                }
            } else if (starts_with("<!")) {
                // DOCTYPE：跳过外层 '>'（内部子集不常见，这里不处理）。
                size_t end = text_.find('>', pos_);
                if (end == std::string::npos) {
                    error_ = "未闭合的 <! 声明";
                    pos_ = text_.size();
                    return;
                }
                pos_ = end + 1;
            } else {
                return;
            }
        }
    }

    bool parse_name(std::string& out) {
        size_t begin = pos_;
        while (pos_ < text_.size() && is_name_char(text_[pos_])) {
            ++pos_;
        }
        if (begin == pos_) {
            error_ = "期望元素/属性名（偏移 " + std::to_string(pos_) + "）";
            return false;
        }
        out = text_.substr(begin, pos_ - begin);
        return true;
    }

    bool parse_attributes(XmlNode& node, bool& self_closing) {
        self_closing = false;
        for (;;) {
            skip_space();
            if (pos_ >= text_.size()) {
                error_ = "元素 <" + node.name + "> 未闭合";
                return false;
            }
            char current = text_[pos_];
            if (current == '>') {
                ++pos_;
                return true;
            }
            if (current == '/') {
                ++pos_;
                if (pos_ >= text_.size() || text_[pos_] != '>') {
                    error_ = "元素 <" + node.name + "> 的 '/' 后缺少 '>'";
                    return false;
                }
                ++pos_;
                self_closing = true;
                return true;
            }
            std::string key;
            if (!parse_name(key)) {
                return false;
            }
            skip_space();
            if (pos_ >= text_.size() || text_[pos_] != '=') {
                error_ = "属性 " + key + " 缺少 '='";
                return false;
            }
            ++pos_;
            skip_space();
            if (pos_ >= text_.size() || (text_[pos_] != '"' && text_[pos_] != '\'')) {
                error_ = "属性 " + key + " 的值缺少引号";
                return false;
            }
            char quote = text_[pos_++];
            size_t end = text_.find(quote, pos_);
            if (end == std::string::npos) {
                error_ = "属性 " + key + " 的值缺少结束引号";
                return false;
            }
            node.attributes.emplace_back(key, decode_entities(text_.substr(pos_, end - pos_)));
            pos_ = end + 1;
        }
    }

    bool parse_element(XmlNode& node) {
        if (pos_ >= text_.size() || text_[pos_] != '<') {
            error_ = "期望 '<'（偏移 " + std::to_string(pos_) + "）";
            return false;
        }
        ++pos_;
        if (!parse_name(node.name)) {
            return false;
        }

        bool self_closing = false;
        if (!parse_attributes(node, self_closing)) {
            return false;
        }
        if (self_closing) {
            return true;
        }

        std::string text;
        for (;;) {
            if (pos_ >= text_.size()) {
                error_ = "元素 <" + node.name + "> 未闭合";
                return false;
            }
            if (text_[pos_] == '<') {
                if (starts_with("</")) {
                    pos_ += 2;
                    std::string closing;
                    if (!parse_name(closing)) {
                        return false;
                    }
                    if (closing != node.name) {
                        error_ = "结束标签 </" + closing + "> 与 <" + node.name + "> 不匹配";
                        return false;
                    }
                    skip_space();
                    if (pos_ >= text_.size() || text_[pos_] != '>') {
                        error_ = "结束标签 </" + closing + "> 缺少 '>'";
                        return false;
                    }
                    ++pos_;
                    node.text = trim(text);
                    return true;
                }
                if (starts_with("<!--")) {
                    if (!skip_until("-->")) {
                        return false;
                    }
                    continue;
                }
                if (starts_with("<![CDATA[")) {
                    size_t end = text_.find("]]>", pos_);
                    if (end == std::string::npos) {
                        error_ = "未闭合的 CDATA 段";
                        return false;
                    }
                    text.append(text_, pos_ + 9, end - (pos_ + 9));
                    pos_ = end + 3;
                    continue;
                }
                if (starts_with("<?")) {
                    if (!skip_until("?>")) {
                        return false;
                    }
                    continue;
                }
                XmlNode child;
                if (!parse_element(child)) {
                    return false;
                }
                node.children.push_back(std::move(child));
            } else {
                size_t next = text_.find('<', pos_);
                if (next == std::string::npos) {
                    next = text_.size();
                }
                text += decode_entities(text_.substr(pos_, next - pos_));
                pos_ = next;
            }
        }
    }

    const std::string& text_;
    size_t pos_ = 0;
    std::string error_;
};

void find_first_recursive(const XmlNode& node, const std::string& name, const XmlNode*& found) {
    if (found != nullptr) {
        return;
    }
    if (node.name == name) {
        found = &node;
        return;
    }
    for (const XmlNode& child : node.children) {
        find_first_recursive(child, name, found);
        if (found != nullptr) {
            return;
        }
    }
}

}  // namespace

bool XmlNode::has_attribute(const std::string& key) const {
    for (const auto& attribute_entry : attributes) {
        if (attribute_entry.first == key) {
            return true;
        }
    }
    return false;
}

std::string XmlNode::attribute(const std::string& key, const std::string& fallback) const {
    for (const auto& attribute_entry : attributes) {
        if (attribute_entry.first == key) {
            return attribute_entry.second;
        }
    }
    return fallback;
}

const XmlNode* XmlNode::child(const std::string& name) const {
    for (const XmlNode& candidate : children) {
        if (candidate.name == name) {
            return &candidate;
        }
    }
    return nullptr;
}

std::vector<const XmlNode*> XmlNode::children_named(const std::string& name) const {
    std::vector<const XmlNode*> matches;
    for (const XmlNode& candidate : children) {
        if (candidate.name == name) {
            matches.push_back(&candidate);
        }
    }
    return matches;
}

const XmlNode* XmlNode::find_first(const std::string& name) const {
    const XmlNode* found = nullptr;
    find_first_recursive(*this, name, found);
    return found;
}

const XmlNode* XmlNode::path(const std::string& dotted) const {
    if (dotted.empty()) {
        return nullptr;
    }
    std::vector<std::string> segments;
    size_t begin = 0;
    for (;;) {
        size_t dot = dotted.find('.', begin);
        if (dot == std::string::npos) {
            segments.push_back(dotted.substr(begin));
            break;
        }
        segments.push_back(dotted.substr(begin, dot - begin));
        begin = dot + 1;
    }

    const XmlNode* current = this;
    if (segments[0] != name) {
        current = find_first(segments[0]);
        if (current == nullptr) {
            return nullptr;
        }
    }
    for (size_t index = 1; index < segments.size(); ++index) {
        current = current->child(segments[index]);
        if (current == nullptr) {
            return nullptr;
        }
    }
    return current;
}

std::string XmlNode::child_text(const std::string& name, const std::string& fallback) const {
    const XmlNode* candidate = child(name);
    return candidate != nullptr ? candidate->text : fallback;
}

XmlDocument parse_xml(const std::string& text) {
    XmlDocument document;
    Parser parser(text);
    if (!parser.parse(document.root)) {
        document.ok = false;
        document.error = parser.error();
        return document;
    }
    document.ok = true;
    return document;
}

}  // namespace sunshine
}  // namespace tg
