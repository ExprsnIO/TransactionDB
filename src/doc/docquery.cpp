#include "tdb/docquery.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <stdexcept>
#include <sstream>

namespace tdb::doc {

// ============================================================================
// XmlNode helpers
// ============================================================================

std::vector<std::shared_ptr<XmlNode>>
XmlNode::child_elements(const std::string &name) const {
    std::vector<std::shared_ptr<XmlNode>> result;
    for (auto &ch : children) {
        if (ch->type == Type::ELEMENT && ch->tag == name) {
            result.push_back(ch);
        }
    }
    return result;
}

std::vector<std::shared_ptr<XmlNode>>
XmlNode::descendant_elements(const std::string &name) const {
    std::vector<std::shared_ptr<XmlNode>> result;
    for (auto &ch : children) {
        if (ch->type == Type::ELEMENT) {
            if (ch->tag == name) result.push_back(ch);
            auto desc = ch->descendant_elements(name);
            result.insert(result.end(), desc.begin(), desc.end());
        }
    }
    return result;
}

std::string XmlNode::all_text() const {
    if (type == Type::TEXT || type == Type::CDATA) return text;
    std::string result;
    for (auto &ch : children) {
        result += ch->all_text();
    }
    return result;
}

std::string XmlNode::to_xml() const {
    if (type == Type::TEXT) return text;
    if (type == Type::CDATA) return "<![CDATA[" + text + "]]>";
    if (type == Type::COMMENT) return "<!--" + text + "-->";
    if (type == Type::PI) return "<?" + text + "?>";

    std::string result = "<" + tag;
    for (auto &[k, v] : attributes) {
        result += " " + k + "=\"" + v + "\"";
    }
    if (children.empty()) {
        result += "/>";
    } else {
        result += ">";
        for (auto &ch : children) {
            result += ch->to_xml();
        }
        result += "</" + tag + ">";
    }
    return result;
}

// ============================================================================
// XML Parser
// ============================================================================

XmlParser::XmlParser(const std::string &xml) : src_(xml), pos_(0) {}

XmlNodePtr XmlParser::parse(const std::string &xml) {
    XmlParser parser(xml);
    return parser.parse_document();
}

XmlNodePtr XmlParser::parse_document() {
    skip_whitespace();
    // Skip XML prolog if present
    if (match("<?xml")) {
        parse_prolog();
    }
    skip_whitespace();

    // Create a virtual document root
    auto doc = std::make_shared<XmlNode>();
    doc->type = XmlNode::Type::ELEMENT;
    doc->tag = "#document";

    while (!at_end()) {
        skip_whitespace();
        if (at_end()) break;
        if (match("<!--")) {
            skip_comment();
        } else if (match("<?")) {
            skip_pi();
        } else if (peek() == '<') {
            auto elem = parse_element();
            if (elem) {
                elem->parent = doc;
                doc->children.push_back(elem);
            }
        } else {
            // Skip stray characters
            advance();
        }
    }
    return doc;
}

XmlNodePtr XmlParser::parse_element() {
    if (at_end() || peek() != '<') return nullptr;
    advance(); // skip '<'

    // Check for special nodes
    if (peek() == '!') {
        // Comment or CDATA
        if (match("!--")) {
            pos_ += 2; // skip "!-" (we already skipped '<', need to skip "--")
            // Actually, we already consumed '<', so we check if next is "!--"
            // Rewind: we consumed '<', now at '!'. Let's look at full:
            // We're now at '!', check for "!--" for comment or "![CDATA[" for cdata
            if (pos_ + 1 < src_.size() && src_[pos_] == '!' && src_[pos_ + 1] == '-') {
                pos_ += 2; // skip "!-"
                if (pos_ < src_.size() && src_[pos_] == '-') {
                    pos_++; // skip second '-'
                    // Parse comment content
                    std::string content;
                    while (!at_end()) {
                        if (match("-->")) {
                            pos_ += 3;
                            break;
                        }
                        content += advance();
                    }
                    auto node = std::make_shared<XmlNode>();
                    node->type = XmlNode::Type::COMMENT;
                    node->text = content;
                    return node;
                }
            }
            if (pos_ + 6 < src_.size() && src_.substr(pos_, 7) == "![CDATA[") {
                pos_ += 7;
                std::string content;
                while (!at_end()) {
                    if (match("]]>")) {
                        pos_ += 3;
                        break;
                    }
                    content += advance();
                }
                auto node = std::make_shared<XmlNode>();
                node->type = XmlNode::Type::CDATA;
                node->text = content;
                return node;
            }
        }
        // CDATA check
        if (src_.substr(pos_, 7) == "![CDATA[") {
            pos_ += 7;
            std::string content;
            while (!at_end()) {
                if (match("]]>")) {
                    pos_ += 3;
                    break;
                }
                content += advance();
            }
            auto node = std::make_shared<XmlNode>();
            node->type = XmlNode::Type::CDATA;
            node->text = content;
            return node;
        }
        // Comment
        if (src_.substr(pos_, 2) == "--") {
            pos_ += 2;
            std::string content;
            while (!at_end()) {
                if (match("-->")) {
                    pos_ += 3;
                    break;
                }
                content += advance();
            }
            auto node = std::make_shared<XmlNode>();
            node->type = XmlNode::Type::COMMENT;
            node->text = content;
            return node;
        }
        // Unknown, skip to '>'
        while (!at_end() && peek() != '>') advance();
        if (!at_end()) advance();
        return nullptr;
    }

    // Check for closing tag - shouldn't happen here, return nullptr
    if (peek() == '/') return nullptr;

    // Parse element name
    std::string name = parse_name();
    if (name.empty()) return nullptr;

    auto elem = std::make_shared<XmlNode>();
    elem->type = XmlNode::Type::ELEMENT;
    elem->tag = name;

    // Parse attributes
    while (!at_end()) {
        skip_whitespace();
        if (at_end()) break;
        if (peek() == '/' || peek() == '>') break;

        std::string attr_name = parse_name();
        if (attr_name.empty()) { advance(); continue; }

        skip_whitespace();
        if (!at_end() && peek() == '=') {
            advance(); // skip '='
            skip_whitespace();
            std::string attr_val = parse_attr_value();
            elem->attributes[attr_name] = decode_entities(attr_val);
        } else {
            elem->attributes[attr_name] = attr_name; // boolean attribute
        }
    }

    if (at_end()) return elem;

    // Self-closing tag?
    if (peek() == '/') {
        advance(); // skip '/'
        if (!at_end() && peek() == '>') advance(); // skip '>'
        return elem;
    }

    // Opening tag end
    if (peek() == '>') advance(); // skip '>'

    // Parse children
    while (!at_end()) {
        // Check for closing tag
        if (match("</")) {
            pos_ += 2;
            // Skip the closing tag name + '>'
            while (!at_end() && peek() != '>') advance();
            if (!at_end()) advance(); // skip '>'
            break;
        }
        // Comment
        if (match("<!--")) {
            pos_ += 4;
            std::string content;
            while (!at_end()) {
                if (match("-->")) { pos_ += 3; break; }
                content += advance();
            }
            auto comment = std::make_shared<XmlNode>();
            comment->type = XmlNode::Type::COMMENT;
            comment->text = content;
            comment->parent = elem;
            elem->children.push_back(comment);
            continue;
        }
        // CDATA
        if (match("<![CDATA[")) {
            pos_ += 9;
            std::string content;
            while (!at_end()) {
                if (match("]]>")) { pos_ += 3; break; }
                content += advance();
            }
            auto cdata = std::make_shared<XmlNode>();
            cdata->type = XmlNode::Type::CDATA;
            cdata->text = content;
            cdata->parent = elem;
            elem->children.push_back(cdata);
            continue;
        }
        // Child element
        if (peek() == '<') {
            auto child = parse_element();
            if (child) {
                child->parent = elem;
                elem->children.push_back(child);
            }
            continue;
        }
        // Text content
        auto text_node = parse_text();
        if (text_node && !text_node->text.empty()) {
            text_node->parent = elem;
            elem->children.push_back(text_node);
        }
    }

    return elem;
}

XmlNodePtr XmlParser::parse_text() {
    std::string content;
    while (!at_end() && peek() != '<') {
        content += advance();
    }
    if (content.empty()) return nullptr;
    auto node = std::make_shared<XmlNode>();
    node->type = XmlNode::Type::TEXT;
    node->text = decode_entities(content);
    return node;
}

void XmlParser::parse_prolog() {
    while (!at_end()) {
        if (match("?>")) {
            pos_ += 2;
            return;
        }
        advance();
    }
}

void XmlParser::skip_comment() {
    pos_ += 4; // skip "<!--"
    while (!at_end()) {
        if (match("-->")) { pos_ += 3; return; }
        advance();
    }
}

void XmlParser::skip_pi() {
    pos_ += 2; // skip "<?"
    while (!at_end()) {
        if (match("?>")) { pos_ += 2; return; }
        advance();
    }
}

char XmlParser::peek() const {
    return pos_ < src_.size() ? src_[pos_] : '\0';
}

char XmlParser::advance() {
    return pos_ < src_.size() ? src_[pos_++] : '\0';
}

bool XmlParser::at_end() const {
    return pos_ >= src_.size();
}

void XmlParser::skip_whitespace() {
    while (!at_end() && std::isspace(static_cast<unsigned char>(peek()))) advance();
}

bool XmlParser::match(const std::string &s) const {
    if (pos_ + s.size() > src_.size()) return false;
    return src_.compare(pos_, s.size(), s) == 0;
}

void XmlParser::expect(char c) {
    if (at_end() || peek() != c)
        throw std::runtime_error(std::string("XML parse error: expected '") + c + "'");
    advance();
}

std::string XmlParser::parse_name() {
    std::string name;
    // XML name: starts with letter or '_' or ':', followed by letters, digits, '.', '-', '_', ':'
    if (!at_end() && (std::isalpha(static_cast<unsigned char>(peek())) || peek() == '_' || peek() == ':')) {
        name += advance();
        while (!at_end() && (std::isalnum(static_cast<unsigned char>(peek())) || peek() == '_'
                             || peek() == ':' || peek() == '-' || peek() == '.')) {
            name += advance();
        }
    }
    return name;
}

std::string XmlParser::parse_attr_value() {
    if (at_end()) return "";
    char quote = peek();
    if (quote != '"' && quote != '\'') {
        // Unquoted attribute value (non-standard, but handle gracefully)
        std::string val;
        while (!at_end() && !std::isspace(static_cast<unsigned char>(peek())) && peek() != '>' && peek() != '/') {
            val += advance();
        }
        return val;
    }
    advance(); // skip opening quote
    std::string val;
    while (!at_end() && peek() != quote) {
        val += advance();
    }
    if (!at_end()) advance(); // skip closing quote
    return val;
}

std::string XmlParser::decode_entities(const std::string &s) {
    std::string result;
    result.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '&') {
            size_t semi = s.find(';', i);
            if (semi != std::string::npos) {
                std::string ent = s.substr(i + 1, semi - i - 1);
                if (ent == "amp") result += '&';
                else if (ent == "lt") result += '<';
                else if (ent == "gt") result += '>';
                else if (ent == "quot") result += '"';
                else if (ent == "apos") result += '\'';
                else if (!ent.empty() && ent[0] == '#') {
                    // Numeric entity
                    int code = 0;
                    if (ent.size() > 1 && ent[1] == 'x') {
                        code = static_cast<int>(std::strtol(ent.c_str() + 2, nullptr, 16));
                    } else {
                        code = std::atoi(ent.c_str() + 1);
                    }
                    if (code > 0 && code < 128) result += static_cast<char>(code);
                } else {
                    result += s.substr(i, semi - i + 1); // unknown, keep as-is
                }
                i = semi;
                continue;
            }
        }
        result += s[i];
    }
    return result;
}

// ============================================================================
// JSON Node helpers
// ============================================================================

std::shared_ptr<JsonNode> JsonNode::get(const std::string &key) const {
    if (type != Type::OBJECT) return nullptr;
    for (auto &[k, v] : object_members) {
        if (k == key) return v;
    }
    return nullptr;
}

std::shared_ptr<JsonNode> JsonNode::get(size_t index) const {
    if (type != Type::ARRAY) return nullptr;
    if (index >= array_elements.size()) return nullptr;
    return array_elements[index];
}

static void json_escape(std::ostream &os, const std::string &s) {
    for (char c : s) {
        switch (c) {
        case '"':  os << "\\\""; break;
        case '\\': os << "\\\\"; break;
        case '\b': os << "\\b"; break;
        case '\f': os << "\\f"; break;
        case '\n': os << "\\n"; break;
        case '\r': os << "\\r"; break;
        case '\t': os << "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                os << buf;
            } else {
                os << c;
            }
        }
    }
}

std::string JsonNode::to_json() const {
    std::ostringstream os;
    switch (type) {
    case Type::NUL: os << "null"; break;
    case Type::BOOL: os << (bool_val ? "true" : "false"); break;
    case Type::NUMBER: {
        // Print integer if no fractional part
        if (num_val == std::floor(num_val) && std::abs(num_val) < 1e15) {
            os << static_cast<int64_t>(num_val);
        } else {
            os << num_val;
        }
        break;
    }
    case Type::STRING:
        os << "\"";
        json_escape(os, str_val);
        os << "\"";
        break;
    case Type::ARRAY: {
        os << "[";
        for (size_t i = 0; i < array_elements.size(); ++i) {
            if (i > 0) os << ",";
            os << (array_elements[i] ? array_elements[i]->to_json() : "null");
        }
        os << "]";
        break;
    }
    case Type::OBJECT: {
        os << "{";
        for (size_t i = 0; i < object_members.size(); ++i) {
            if (i > 0) os << ",";
            os << "\"";
            json_escape(os, object_members[i].first);
            os << "\":";
            os << (object_members[i].second ? object_members[i].second->to_json() : "null");
        }
        os << "}";
        break;
    }
    }
    return os.str();
}

JsonNodePtr JsonNode::make_null() {
    auto n = std::make_shared<JsonNode>();
    n->type = Type::NUL;
    return n;
}

JsonNodePtr JsonNode::make_bool(bool v) {
    auto n = std::make_shared<JsonNode>();
    n->type = Type::BOOL;
    n->bool_val = v;
    return n;
}

JsonNodePtr JsonNode::make_number(double v) {
    auto n = std::make_shared<JsonNode>();
    n->type = Type::NUMBER;
    n->num_val = v;
    return n;
}

JsonNodePtr JsonNode::make_string(const std::string &v) {
    auto n = std::make_shared<JsonNode>();
    n->type = Type::STRING;
    n->str_val = v;
    return n;
}

JsonNodePtr JsonNode::make_array() {
    auto n = std::make_shared<JsonNode>();
    n->type = Type::ARRAY;
    return n;
}

JsonNodePtr JsonNode::make_object() {
    auto n = std::make_shared<JsonNode>();
    n->type = Type::OBJECT;
    return n;
}

// ============================================================================
// JSON Parser
// ============================================================================

JsonParser::JsonParser(const std::string &json) : src_(json), pos_(0) {}

JsonNodePtr JsonParser::parse(const std::string &json) {
    JsonParser parser(json);
    parser.skip_whitespace();
    auto result = parser.parse_value();
    return result ? result : JsonNode::make_null();
}

JsonNodePtr JsonParser::parse_value() {
    skip_whitespace();
    if (at_end()) return JsonNode::make_null();

    char c = peek();
    if (c == '{') return parse_object();
    if (c == '[') return parse_array();
    if (c == '"') return parse_string_node();
    if (c == 't' || c == 'f' || c == 'n') return parse_literal();
    if (c == '-' || (c >= '0' && c <= '9')) return parse_number();

    return JsonNode::make_null();
}

JsonNodePtr JsonParser::parse_object() {
    advance(); // skip '{'
    auto obj = JsonNode::make_object();
    skip_whitespace();
    if (!at_end() && peek() == '}') { advance(); return obj; }

    while (!at_end()) {
        skip_whitespace();
        if (peek() == '}') { advance(); break; }

        std::string key = parse_string();
        skip_whitespace();
        if (!at_end() && peek() == ':') advance();
        skip_whitespace();
        auto val = parse_value();
        obj->object_members.emplace_back(key, val);

        skip_whitespace();
        if (!at_end() && peek() == ',') { advance(); continue; }
        if (!at_end() && peek() == '}') { advance(); break; }
        break; // malformed
    }
    return obj;
}

JsonNodePtr JsonParser::parse_array() {
    advance(); // skip '['
    auto arr = JsonNode::make_array();
    skip_whitespace();
    if (!at_end() && peek() == ']') { advance(); return arr; }

    while (!at_end()) {
        skip_whitespace();
        if (peek() == ']') { advance(); break; }

        auto val = parse_value();
        arr->array_elements.push_back(val);

        skip_whitespace();
        if (!at_end() && peek() == ',') { advance(); continue; }
        if (!at_end() && peek() == ']') { advance(); break; }
        break; // malformed
    }
    return arr;
}

JsonNodePtr JsonParser::parse_string_node() {
    return JsonNode::make_string(parse_string());
}

std::string JsonParser::parse_string() {
    if (at_end() || peek() != '"') return "";
    advance(); // skip opening '"'
    std::string result;
    while (!at_end() && peek() != '"') {
        if (peek() == '\\') {
            advance(); // skip backslash
            if (at_end()) break;
            char esc = advance();
            switch (esc) {
            case '"': result += '"'; break;
            case '\\': result += '\\'; break;
            case '/': result += '/'; break;
            case 'b': result += '\b'; break;
            case 'f': result += '\f'; break;
            case 'n': result += '\n'; break;
            case 'r': result += '\r'; break;
            case 't': result += '\t'; break;
            case 'u': {
                std::string hex;
                for (int i = 0; i < 4 && !at_end(); ++i) hex += advance();
                int code = static_cast<int>(std::strtol(hex.c_str(), nullptr, 16));
                if (code > 0 && code < 128) result += static_cast<char>(code);
                break;
            }
            default: result += esc;
            }
        } else {
            result += advance();
        }
    }
    if (!at_end()) advance(); // skip closing '"'
    return result;
}

JsonNodePtr JsonParser::parse_number() {
    std::string num_str;
    if (peek() == '-') num_str += advance();
    while (!at_end() && peek() >= '0' && peek() <= '9') num_str += advance();
    if (!at_end() && peek() == '.') {
        num_str += advance();
        while (!at_end() && peek() >= '0' && peek() <= '9') num_str += advance();
    }
    if (!at_end() && (peek() == 'e' || peek() == 'E')) {
        num_str += advance();
        if (!at_end() && (peek() == '+' || peek() == '-')) num_str += advance();
        while (!at_end() && peek() >= '0' && peek() <= '9') num_str += advance();
    }
    double val = std::strtod(num_str.c_str(), nullptr);
    return JsonNode::make_number(val);
}

// Helper: match a literal string at current position without consuming
static bool match_str_at(const std::string &src, size_t pos, const std::string &s) {
    if (pos + s.size() > src.size()) return false;
    return src.compare(pos, s.size(), s) == 0;
}

JsonNodePtr JsonParser::parse_literal() {
    if (match_str_at(src_, pos_, "true")) { pos_ += 4; return JsonNode::make_bool(true); }
    if (match_str_at(src_, pos_, "false")) { pos_ += 5; return JsonNode::make_bool(false); }
    if (match_str_at(src_, pos_, "null")) { pos_ += 4; return JsonNode::make_null(); }
    advance();
    return JsonNode::make_null();
}

char JsonParser::peek() const {
    return pos_ < src_.size() ? src_[pos_] : '\0';
}

char JsonParser::advance() {
    return pos_ < src_.size() ? src_[pos_++] : '\0';
}

bool JsonParser::at_end() const {
    return pos_ >= src_.size();
}

void JsonParser::skip_whitespace() {
    while (!at_end() && std::isspace(static_cast<unsigned char>(peek()))) advance();
}

// ============================================================================
// XPath Engine
// ============================================================================

std::vector<std::string>
XPathEngine::evaluate(const std::string &xml_string,
                      const std::string &xpath_expr,
                      const Namespaces & /*namespaces*/) {
    auto doc = XmlParser::parse(xml_string);
    if (!doc) return {};

    auto steps = parse_xpath(xpath_expr);
    if (steps.empty()) return {};

    std::vector<XmlNodePtr> context;
    // If path starts with '/', use the document root's first element child
    // as the initial context (like the root element).
    // The document node itself wraps everything.
    context.push_back(doc);

    auto result_nodes = apply_steps(context, steps, 0);

    std::vector<std::string> results;
    results.reserve(result_nodes.size());
    for (auto &n : result_nodes) {
        results.push_back(serialize_node(n));
    }
    return results;
}

std::vector<XPathEngine::Step>
XPathEngine::parse_xpath(const std::string &expr) {
    std::vector<Step> steps;
    std::string e = expr;

    // Trim whitespace
    while (!e.empty() && std::isspace(static_cast<unsigned char>(e.front()))) e.erase(e.begin());
    while (!e.empty() && std::isspace(static_cast<unsigned char>(e.back()))) e.pop_back();

    if (e.empty()) return steps;

    size_t i = 0;
    bool absolute = false;

    // Handle leading '//' or '/'
    if (i < e.size() && e[i] == '/') {
        absolute = true;
        i++;
        if (i < e.size() && e[i] == '/') {
            // '//' -> descendant-or-self step
            i++;
            Step dos;
            dos.axis = Step::Axis::DESCENDANT_OR_SELF;
            dos.node_test = "node()";
            steps.push_back(dos);
        }
        // else just an absolute path from root
    }

    // If absolute and we haven't added descendant-or-self, we're at document root.
    // The context is already the document node, so the first step will navigate from there.
    (void)absolute;

    // Split remaining path on '/' while respecting predicates [...]
    std::string current;
    int bracket_depth = 0;
    for (; i < e.size(); ++i) {
        char c = e[i];
        if (c == '[') { bracket_depth++; current += c; continue; }
        if (c == ']') { bracket_depth--; current += c; continue; }
        if (c == '/' && bracket_depth == 0) {
            if (!current.empty()) {
                steps.push_back(parse_step(current));
                current.clear();
            }
            // Check for //
            if (i + 1 < e.size() && e[i + 1] == '/') {
                i++;
                Step dos;
                dos.axis = Step::Axis::DESCENDANT_OR_SELF;
                dos.node_test = "node()";
                steps.push_back(dos);
            }
            continue;
        }
        current += c;
    }
    if (!current.empty()) {
        steps.push_back(parse_step(current));
    }

    return steps;
}

XPathEngine::Step
XPathEngine::parse_step(const std::string &s) {
    Step step;
    std::string test = s;

    // Extract predicates from end
    std::string preds_str;
    if (!test.empty()) {
        size_t first_bracket = std::string::npos;
        int depth = 0;
        for (size_t i = 0; i < test.size(); ++i) {
            if (test[i] == '[' && depth == 0) {
                if (first_bracket == std::string::npos) first_bracket = i;
                depth++;
            } else if (test[i] == '[') {
                depth++;
            } else if (test[i] == ']') {
                depth--;
            }
        }
        if (first_bracket != std::string::npos) {
            preds_str = test.substr(first_bracket);
            test = test.substr(0, first_bracket);
        }
    }

    // Handle axis:: prefix
    if (test.find("::") != std::string::npos) {
        size_t cp = test.find("::");
        std::string axis_name = test.substr(0, cp);
        test = test.substr(cp + 2);

        if (axis_name == "child") step.axis = Step::Axis::CHILD;
        else if (axis_name == "descendant") step.axis = Step::Axis::DESCENDANT;
        else if (axis_name == "descendant-or-self") step.axis = Step::Axis::DESCENDANT_OR_SELF;
        else if (axis_name == "attribute") step.axis = Step::Axis::ATTRIBUTE;
        else if (axis_name == "parent") step.axis = Step::Axis::PARENT;
        else if (axis_name == "self") step.axis = Step::Axis::SELF;
    }
    // Handle @ shorthand
    else if (!test.empty() && test[0] == '@') {
        step.axis = Step::Axis::ATTRIBUTE;
        test = test.substr(1);
    }
    // Handle '..'
    else if (test == "..") {
        step.axis = Step::Axis::PARENT;
        test = "node()";
    }
    // Handle '.'
    else if (test == ".") {
        step.axis = Step::Axis::SELF;
        test = "node()";
    }

    step.node_test = test;
    step.predicates = parse_predicates(preds_str);
    return step;
}

std::vector<XPathEngine::Step::Predicate>
XPathEngine::parse_predicates(const std::string &s) {
    std::vector<Step::Predicate> preds;
    if (s.empty()) return preds;

    // Split into individual [...]
    size_t i = 0;
    while (i < s.size()) {
        if (s[i] == '[') {
            i++;
            int depth = 1;
            std::string content;
            while (i < s.size() && depth > 0) {
                if (s[i] == '[') depth++;
                else if (s[i] == ']') { depth--; if (depth == 0) { i++; break; } }
                content += s[i];
                i++;
            }

            // Trim content
            while (!content.empty() && std::isspace(static_cast<unsigned char>(content.front()))) content.erase(content.begin());
            while (!content.empty() && std::isspace(static_cast<unsigned char>(content.back()))) content.pop_back();

            Step::Predicate pred;

            // Check if purely numeric (position predicate)
            bool all_digits = !content.empty();
            for (char c : content) {
                if (!std::isdigit(static_cast<unsigned char>(c))) { all_digits = false; break; }
            }
            if (all_digits) {
                pred.type = Step::Predicate::Type::POSITION;
                pred.position = std::atoi(content.c_str());
                preds.push_back(pred);
                continue;
            }

            // Check for @attr='value' pattern
            if (content.size() > 1 && content[0] == '@') {
                size_t eq = content.find('=');
                if (eq != std::string::npos) {
                    pred.type = Step::Predicate::Type::ATTR_EQ;
                    pred.attr_name = content.substr(1, eq - 1);
                    std::string val = content.substr(eq + 1);
                    // Remove quotes
                    if (val.size() >= 2 && (val.front() == '\'' || val.front() == '"')) {
                        val = val.substr(1, val.size() - 2);
                    }
                    pred.attr_value = val;
                    preds.push_back(pred);
                    continue;
                }
                // Just @attr (existence check)
                pred.type = Step::Predicate::Type::ATTR_EXISTS;
                pred.attr_name = content.substr(1);
                preds.push_back(pred);
                continue;
            }

            // Generic expression predicate
            pred.type = Step::Predicate::Type::EXPR;
            pred.raw = content;
            preds.push_back(pred);
        } else {
            i++;
        }
    }

    return preds;
}

std::vector<XmlNodePtr>
XPathEngine::apply_steps(const std::vector<XmlNodePtr> &context,
                         const std::vector<Step> &steps,
                         size_t step_idx) {
    if (step_idx >= steps.size()) return context;

    auto &step = steps[step_idx];
    auto result = apply_step(context, step);
    result = apply_predicates(result, step.predicates);

    return apply_steps(result, steps, step_idx + 1);
}

std::vector<XmlNodePtr>
XPathEngine::apply_step(const std::vector<XmlNodePtr> &context,
                        const Step &step) {
    std::vector<XmlNodePtr> result;

    for (auto &node : context) {
        if (!node) continue;

        switch (step.axis) {
        case Step::Axis::CHILD: {
            for (auto &ch : node->children) {
                if (node_matches(ch, step.node_test)) {
                    result.push_back(ch);
                }
            }
            break;
        }
        case Step::Axis::DESCENDANT: {
            // All descendants (not self)
            std::function<void(const XmlNodePtr &)> collect = [&](const XmlNodePtr &n) {
                for (auto &ch : n->children) {
                    if (node_matches(ch, step.node_test)) {
                        result.push_back(ch);
                    }
                    collect(ch);
                }
            };
            collect(node);
            break;
        }
        case Step::Axis::DESCENDANT_OR_SELF: {
            // Self + all descendants
            std::function<void(const XmlNodePtr &)> collect = [&](const XmlNodePtr &n) {
                if (node_matches(n, step.node_test)) {
                    result.push_back(n);
                }
                for (auto &ch : n->children) {
                    collect(ch);
                }
            };
            collect(node);
            break;
        }
        case Step::Axis::ATTRIBUTE: {
            // Create a virtual text node for the attribute value
            if (node->type == XmlNode::Type::ELEMENT) {
                if (step.node_test == "*") {
                    for (auto &[k, v] : node->attributes) {
                        auto attr_node = std::make_shared<XmlNode>();
                        attr_node->type = XmlNode::Type::TEXT;
                        attr_node->tag = k;
                        attr_node->text = v;
                        result.push_back(attr_node);
                    }
                } else {
                    auto it = node->attributes.find(step.node_test);
                    if (it != node->attributes.end()) {
                        auto attr_node = std::make_shared<XmlNode>();
                        attr_node->type = XmlNode::Type::TEXT;
                        attr_node->tag = it->first;
                        attr_node->text = it->second;
                        result.push_back(attr_node);
                    }
                }
            }
            break;
        }
        case Step::Axis::PARENT: {
            auto p = node->parent.lock();
            if (p && node_matches(p, step.node_test)) {
                result.push_back(p);
            }
            break;
        }
        case Step::Axis::SELF: {
            if (node_matches(node, step.node_test)) {
                result.push_back(node);
            }
            break;
        }
        }
    }

    return result;
}

std::vector<XmlNodePtr>
XPathEngine::apply_predicates(const std::vector<XmlNodePtr> &nodes,
                              const std::vector<Step::Predicate> &preds) {
    if (preds.empty()) return nodes;

    std::vector<XmlNodePtr> current = nodes;

    for (auto &pred : preds) {
        std::vector<XmlNodePtr> filtered;

        switch (pred.type) {
        case Step::Predicate::Type::POSITION: {
            // XPath positions are 1-based
            int idx = pred.position;
            if (idx >= 1 && static_cast<size_t>(idx) <= current.size()) {
                filtered.push_back(current[static_cast<size_t>(idx - 1)]);
            }
            break;
        }
        case Step::Predicate::Type::ATTR_EQ: {
            for (auto &n : current) {
                if (n->type == XmlNode::Type::ELEMENT) {
                    auto it = n->attributes.find(pred.attr_name);
                    if (it != n->attributes.end() && it->second == pred.attr_value) {
                        filtered.push_back(n);
                    }
                }
            }
            break;
        }
        case Step::Predicate::Type::ATTR_EXISTS: {
            for (auto &n : current) {
                if (n->type == XmlNode::Type::ELEMENT) {
                    if (n->attributes.count(pred.attr_name)) {
                        filtered.push_back(n);
                    }
                }
            }
            break;
        }
        case Step::Predicate::Type::EXPR: {
            // Simple expression evaluation for common patterns:
            // "text()='value'" or "name()='value'" or "contains(text(),'val')"
            // or "position()=N" or "last()"

            // Check for "position()=N"
            if (pred.raw.find("position()") == 0) {
                size_t eq = pred.raw.find('=');
                if (eq != std::string::npos) {
                    std::string numstr = pred.raw.substr(eq + 1);
                    while (!numstr.empty() && std::isspace(static_cast<unsigned char>(numstr.front()))) numstr.erase(numstr.begin());
                    int pos = std::atoi(numstr.c_str());
                    if (pos >= 1 && static_cast<size_t>(pos) <= current.size()) {
                        filtered.push_back(current[static_cast<size_t>(pos - 1)]);
                    }
                }
                break;
            }
            // Check for "last()"
            if (pred.raw == "last()") {
                if (!current.empty()) {
                    filtered.push_back(current.back());
                }
                break;
            }
            // Check for "text()='value'"
            if (pred.raw.find("text()") == 0) {
                size_t eq = pred.raw.find('=');
                if (eq != std::string::npos) {
                    std::string val = pred.raw.substr(eq + 1);
                    while (!val.empty() && std::isspace(static_cast<unsigned char>(val.front()))) val.erase(val.begin());
                    if (val.size() >= 2 && (val.front() == '\'' || val.front() == '"')) {
                        val = val.substr(1, val.size() - 2);
                    }
                    for (auto &n : current) {
                        if (n->all_text() == val) {
                            filtered.push_back(n);
                        }
                    }
                }
                break;
            }
            // Check for "name()='value'"
            if (pred.raw.find("name()") == 0) {
                size_t eq = pred.raw.find('=');
                if (eq != std::string::npos) {
                    std::string val = pred.raw.substr(eq + 1);
                    while (!val.empty() && std::isspace(static_cast<unsigned char>(val.front()))) val.erase(val.begin());
                    if (val.size() >= 2 && (val.front() == '\'' || val.front() == '"')) {
                        val = val.substr(1, val.size() - 2);
                    }
                    for (auto &n : current) {
                        if (n->tag == val) {
                            filtered.push_back(n);
                        }
                    }
                }
                break;
            }
            // Check for contains(., 'val') or contains(text(), 'val')
            if (pred.raw.find("contains(") == 0) {
                size_t paren = pred.raw.find('(');
                size_t comma = pred.raw.find(',', paren);
                size_t close = pred.raw.rfind(')');
                if (comma != std::string::npos && close != std::string::npos) {
                    std::string val = pred.raw.substr(comma + 1, close - comma - 1);
                    while (!val.empty() && std::isspace(static_cast<unsigned char>(val.front()))) val.erase(val.begin());
                    while (!val.empty() && std::isspace(static_cast<unsigned char>(val.back()))) val.pop_back();
                    if (val.size() >= 2 && (val.front() == '\'' || val.front() == '"')) {
                        val = val.substr(1, val.size() - 2);
                    }
                    for (auto &n : current) {
                        if (n->all_text().find(val) != std::string::npos) {
                            filtered.push_back(n);
                        }
                    }
                }
                break;
            }
            // Check for starts-with(., 'val') or starts-with(text(), 'val')
            if (pred.raw.find("starts-with(") == 0) {
                size_t comma = pred.raw.find(',');
                size_t close = pred.raw.rfind(')');
                if (comma != std::string::npos && close != std::string::npos) {
                    std::string val = pred.raw.substr(comma + 1, close - comma - 1);
                    while (!val.empty() && std::isspace(static_cast<unsigned char>(val.front()))) val.erase(val.begin());
                    while (!val.empty() && std::isspace(static_cast<unsigned char>(val.back()))) val.pop_back();
                    if (val.size() >= 2 && (val.front() == '\'' || val.front() == '"')) {
                        val = val.substr(1, val.size() - 2);
                    }
                    for (auto &n : current) {
                        std::string text = n->all_text();
                        if (text.size() >= val.size() && text.substr(0, val.size()) == val) {
                            filtered.push_back(n);
                        }
                    }
                }
                break;
            }
            // Fallback: include everything (unrecognized expression)
            filtered = current;
            break;
        }
        }

        current = filtered;
    }

    return current;
}

bool XPathEngine::node_matches(const XmlNodePtr &node, const std::string &test) {
    if (!node) return false;
    if (test == "*") {
        return node->type == XmlNode::Type::ELEMENT;
    }
    if (test == "node()") {
        return true; // matches any node
    }
    if (test == "text()") {
        return node->type == XmlNode::Type::TEXT || node->type == XmlNode::Type::CDATA;
    }
    if (test == "comment()") {
        return node->type == XmlNode::Type::COMMENT;
    }
    // Element name match
    return node->type == XmlNode::Type::ELEMENT && node->tag == test;
}

std::string XPathEngine::serialize_node(const XmlNodePtr &node) {
    if (!node) return "";
    if (node->type == XmlNode::Type::TEXT || node->type == XmlNode::Type::CDATA) {
        return node->text;
    }
    if (node->type == XmlNode::Type::ELEMENT) {
        // If the element has text-only content, return just the text
        bool all_text = true;
        for (auto &ch : node->children) {
            if (ch->type != XmlNode::Type::TEXT && ch->type != XmlNode::Type::CDATA) {
                all_text = false;
                break;
            }
        }
        if (all_text && !node->children.empty()) {
            return node->all_text();
        }
        return node->to_xml();
    }
    return node->text;
}

// ============================================================================
// XQuery Engine
// ============================================================================

static std::string trim(const std::string &s) {
    size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) start++;
    size_t end = s.size();
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) end--;
    return s.substr(start, end - start);
}

static bool starts_with(const std::string &s, const std::string &prefix) {
    if (s.size() < prefix.size()) return false;
    return s.compare(0, prefix.size(), prefix) == 0;
}

static bool iequals_at(const std::string &s, size_t pos, const std::string &word) {
    if (pos + word.size() > s.size()) return false;
    for (size_t i = 0; i < word.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(s[pos + i])) !=
            std::tolower(static_cast<unsigned char>(word[i]))) return false;
    }
    // After the word, there must be whitespace or end or non-alnum
    size_t after = pos + word.size();
    if (after < s.size() && std::isalnum(static_cast<unsigned char>(s[after]))) return false;
    return true;
}

std::vector<std::string>
XQueryEngine::evaluate(const std::string &xml_string,
                       const std::string &xquery_expr,
                       const Params &params,
                       const Namespaces &namespaces) {
    auto doc = XmlParser::parse(xml_string);
    if (!doc) return {};

    // Build initial variable environment from params
    VarEnv env;
    for (auto &[name, value] : params) {
        Variable var;
        var.name = name;
        var.value.items.push_back(value);
        env.push_back(var);
    }

    auto result = eval_expr(trim(xquery_expr), doc, env, namespaces);
    return result.items;
}

XQueryEngine::XQValue
XQueryEngine::eval_expr(const std::string &expr, XmlNodePtr doc,
                        const VarEnv &env, const Namespaces &ns) {
    std::string e = trim(expr);
    if (e.empty()) return {};

    // FLWOR: starts with "for" or "let"
    if (iequals_at(e, 0, "for") || iequals_at(e, 0, "let")) {
        return eval_flwor(e, doc, env, ns);
    }

    // Conditional: if (...) then ... else ...
    if (iequals_at(e, 0, "if")) {
        return eval_if(e, doc, env, ns);
    }

    // Element constructor: <tag>...</tag>
    if (!e.empty() && e[0] == '<' && e.size() > 1 && std::isalpha(static_cast<unsigned char>(e[1]))) {
        return eval_element_constructor(e, doc, env, ns);
    }

    // String literal
    if (!e.empty() && (e.front() == '"' || e.front() == '\'')) {
        return eval_string_literal(e);
    }

    // Variable reference: $var
    if (!e.empty() && e[0] == '$') {
        std::string varname;
        size_t i = 1;
        while (i < e.size() && (std::isalnum(static_cast<unsigned char>(e[i])) || e[i] == '_' || e[i] == '-')) {
            varname += e[i++];
        }
        return eval_variable(varname, env);
    }

    // Function call: name(...)
    {
        size_t paren = e.find('(');
        if (paren != std::string::npos && paren > 0) {
            std::string fname = trim(e.substr(0, paren));
            // Make sure it's a valid function name (no operators)
            bool valid_name = true;
            for (char c : fname) {
                if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_') {
                    valid_name = false; break;
                }
            }
            if (valid_name && !fname.empty()) {
                // Find matching closing paren
                int depth = 0;
                size_t close = std::string::npos;
                for (size_t i = paren; i < e.size(); ++i) {
                    if (e[i] == '(') depth++;
                    else if (e[i] == ')') { depth--; if (depth == 0) { close = i; break; } }
                }
                if (close != std::string::npos) {
                    std::string args_str = e.substr(paren + 1, close - paren - 1);
                    auto args = split_function_args(args_str);
                    return eval_function(fname, args, doc, env, ns);
                }
            }
        }
    }

    // Comparison: look for =, !=, <, >, <=, >=
    // (but not inside strings or nested parens)
    {
        int paren_depth = 0;
        int bracket_depth = 0;
        bool in_string = false;
        char string_char = 0;
        for (size_t i = 0; i < e.size(); ++i) {
            char c = e[i];
            if (in_string) {
                if (c == string_char) in_string = false;
                continue;
            }
            if (c == '\'' || c == '"') { in_string = true; string_char = c; continue; }
            if (c == '(') { paren_depth++; continue; }
            if (c == ')') { paren_depth--; continue; }
            if (c == '[') { bracket_depth++; continue; }
            if (c == ']') { bracket_depth--; continue; }
            if (paren_depth > 0 || bracket_depth > 0) continue;

            if (c == '!' && i + 1 < e.size() && e[i + 1] == '=') {
                return eval_comparison(e, doc, env, ns);
            }
            if (c == '=' && (i == 0 || (e[i - 1] != '!' && e[i - 1] != '<' && e[i - 1] != '>'))) {
                return eval_comparison(e, doc, env, ns);
            }
            if ((c == '<' || c == '>') && !(i > 0 && e[i - 1] == '<') && !(i + 1 < e.size() && e[i + 1] == '/')) {
                // Might be XML, skip if looks like element
                if (c == '<' && i + 1 < e.size() && std::isalpha(static_cast<unsigned char>(e[i + 1]))) continue;
                return eval_comparison(e, doc, env, ns);
            }
        }
    }

    // Comma-separated sequence: "expr, expr, ..."
    {
        int depth = 0;
        bool in_str = false;
        char sc = 0;
        for (size_t i = 0; i < e.size(); ++i) {
            char c = e[i];
            if (in_str) { if (c == sc) in_str = false; continue; }
            if (c == '\'' || c == '"') { in_str = true; sc = c; continue; }
            if (c == '(' || c == '[' || c == '<') { depth++; continue; }
            if (c == ')' || c == ']' || c == '>') { depth--; continue; }
            if (c == ',' && depth == 0) {
                XQValue combined;
                std::string left = trim(e.substr(0, i));
                std::string right = trim(e.substr(i + 1));
                auto lval = eval_expr(left, doc, env, ns);
                auto rval = eval_expr(right, doc, env, ns);
                combined.items.insert(combined.items.end(), lval.items.begin(), lval.items.end());
                combined.items.insert(combined.items.end(), rval.items.begin(), rval.items.end());
                return combined;
            }
        }
    }

    // String concatenation with ||
    {
        int depth = 0;
        bool in_str = false;
        char sc = 0;
        for (size_t i = 0; i + 1 < e.size(); ++i) {
            char c = e[i];
            if (in_str) { if (c == sc) in_str = false; continue; }
            if (c == '\'' || c == '"') { in_str = true; sc = c; continue; }
            if (c == '(' || c == '[') { depth++; continue; }
            if (c == ')' || c == ']') { depth--; continue; }
            if (c == '|' && e[i + 1] == '|' && depth == 0) {
                std::string left_str = trim(e.substr(0, i));
                std::string right_str = trim(e.substr(i + 2));
                auto lv = eval_expr(left_str, doc, env, ns);
                auto rv = eval_expr(right_str, doc, env, ns);
                XQValue result;
                result.items.push_back(lv.first() + rv.first());
                return result;
            }
        }
    }

    // Path expression (XPath): anything starting with / or containing /
    if (!e.empty() && (e[0] == '/' || e[0] == '.' || std::isalpha(static_cast<unsigned char>(e[0])))) {
        return eval_path(e, doc, env, ns);
    }

    // Numeric literal
    {
        bool is_num = true;
        bool has_dot = false;
        for (size_t i = 0; i < e.size(); ++i) {
            char c = e[i];
            if (c == '.' && !has_dot) { has_dot = true; continue; }
            if (c == '-' && i == 0) continue;
            if (!std::isdigit(static_cast<unsigned char>(c))) { is_num = false; break; }
        }
        if (is_num && !e.empty()) {
            XQValue v;
            v.items.push_back(e);
            return v;
        }
    }

    // Fallback: treat as path
    return eval_path(e, doc, env, ns);
}

XQueryEngine::XQValue
XQueryEngine::eval_flwor(const std::string &expr, XmlNodePtr doc,
                         const VarEnv &env, const Namespaces &ns) {
    XQValue result;
    size_t pos = 0;
    VarEnv local_env = env;

    // Collected FLWOR clauses
    struct ForClause { std::string var; std::string in_expr; };
    struct LetClause { std::string var; std::string value_expr; };
    std::vector<ForClause> for_clauses;
    std::vector<LetClause> let_clauses;
    std::string where_expr;
    std::string order_expr;
    std::string return_expr;

    // Parse FLWOR clauses
    std::string e = trim(expr);
    pos = 0;

    while (pos < e.size()) {
        // Skip whitespace
        while (pos < e.size() && std::isspace(static_cast<unsigned char>(e[pos]))) pos++;
        if (pos >= e.size()) break;

        if (iequals_at(e, pos, "for")) {
            pos += 3;
            ForClause fc;
            fc.var = extract_for_var(e, pos);
            // skip "in"
            while (pos < e.size() && std::isspace(static_cast<unsigned char>(e[pos]))) pos++;
            if (iequals_at(e, pos, "in")) pos += 2;
            fc.in_expr = extract_for_in(e, pos);
            for_clauses.push_back(fc);
        } else if (iequals_at(e, pos, "let")) {
            pos += 3;
            LetClause lc;
            lc.var = extract_let_var(e, pos);
            lc.value_expr = extract_let_value(e, pos);
            let_clauses.push_back(lc);
        } else if (iequals_at(e, pos, "where")) {
            pos += 5;
            where_expr = extract_where(e, pos);
        } else if (iequals_at(e, pos, "order")) {
            // "order by"
            pos += 5;
            while (pos < e.size() && std::isspace(static_cast<unsigned char>(e[pos]))) pos++;
            if (iequals_at(e, pos, "by")) pos += 2;
            order_expr = extract_order_by(e, pos);
        } else if (iequals_at(e, pos, "return")) {
            pos += 6;
            return_expr = extract_return(e, pos);
            break;
        } else {
            pos++;
        }
    }

    if (return_expr.empty()) return result;

    // Execute FLWOR
    // For simplicity, handle one for-clause (can be extended).
    if (!for_clauses.empty()) {
        auto &fc = for_clauses[0];
        auto sequence = eval_expr(trim(fc.in_expr), doc, local_env, ns);

        struct Item {
            std::string value;
            std::string sort_key;
        };
        std::vector<Item> items;

        for (auto &item : sequence.items) {
            VarEnv iteration_env = local_env;

            // Bind the for variable
            Variable for_var;
            for_var.name = fc.var;
            for_var.value.items.push_back(item);
            iteration_env.push_back(for_var);

            // Process additional for clauses
            for (size_t i = 1; i < for_clauses.size(); ++i) {
                auto inner_seq = eval_expr(trim(for_clauses[i].in_expr), doc, iteration_env, ns);
                Variable v;
                v.name = for_clauses[i].var;
                v.value = inner_seq;
                iteration_env.push_back(v);
            }

            // Process let clauses
            for (auto &lc : let_clauses) {
                auto val = eval_expr(trim(lc.value_expr), doc, iteration_env, ns);
                Variable v;
                v.name = lc.var;
                v.value = val;
                iteration_env.push_back(v);
            }

            // Evaluate where clause
            if (!where_expr.empty()) {
                auto cond = eval_expr(trim(where_expr), doc, iteration_env, ns);
                std::string cv = cond.first();
                if (cv.empty() || cv == "false" || cv == "0") continue;
            }

            // Evaluate return expression
            auto ret = eval_expr(trim(return_expr), doc, iteration_env, ns);

            // Compute sort key if order by is present
            std::string sort_key;
            if (!order_expr.empty()) {
                auto sk = eval_expr(trim(order_expr), doc, iteration_env, ns);
                sort_key = sk.first();
            }

            for (auto &r : ret.items) {
                items.push_back({r, sort_key});
            }
        }

        // Apply ordering
        if (!order_expr.empty()) {
            std::stable_sort(items.begin(), items.end(),
                [](const Item &a, const Item &b) { return a.sort_key < b.sort_key; });
        }

        for (auto &item : items) {
            result.items.push_back(item.value);
        }
    } else if (!let_clauses.empty()) {
        // Let-only expression (no for)
        VarEnv let_env = local_env;
        for (auto &lc : let_clauses) {
            auto val = eval_expr(trim(lc.value_expr), doc, let_env, ns);
            Variable v;
            v.name = lc.var;
            v.value = val;
            let_env.push_back(v);
        }

        if (!where_expr.empty()) {
            auto cond = eval_expr(trim(where_expr), doc, let_env, ns);
            std::string cv = cond.first();
            if (cv.empty() || cv == "false" || cv == "0") return result;
        }

        auto ret = eval_expr(trim(return_expr), doc, let_env, ns);
        result = ret;
    }

    return result;
}

XQueryEngine::XQValue
XQueryEngine::eval_if(const std::string &expr, XmlNodePtr doc,
                      const VarEnv &env, const Namespaces &ns) {
    // if (cond) then expr else expr
    size_t pos = 2; // skip "if"
    while (pos < expr.size() && std::isspace(static_cast<unsigned char>(expr[pos]))) pos++;

    // Extract condition between parens
    std::string cond_str;
    if (pos < expr.size() && expr[pos] == '(') {
        pos++; // skip '('
        int depth = 1;
        while (pos < expr.size() && depth > 0) {
            if (expr[pos] == '(') depth++;
            else if (expr[pos] == ')') { depth--; if (depth == 0) { pos++; break; } }
            cond_str += expr[pos];
            pos++;
        }
    }

    while (pos < expr.size() && std::isspace(static_cast<unsigned char>(expr[pos]))) pos++;

    // "then"
    std::string then_expr, else_expr;
    if (iequals_at(expr, pos, "then")) {
        pos += 4;
        while (pos < expr.size() && std::isspace(static_cast<unsigned char>(expr[pos]))) pos++;

        // Find "else" at the same nesting level
        int depth = 0;
        bool in_str = false;
        char sc = 0;
        size_t then_start = pos;
        size_t else_pos = std::string::npos;
        for (size_t i = pos; i < expr.size(); ++i) {
            char c = expr[i];
            if (in_str) { if (c == sc) in_str = false; continue; }
            if (c == '\'' || c == '"') { in_str = true; sc = c; continue; }
            if (c == '(' || c == '[') { depth++; continue; }
            if (c == ')' || c == ']') { depth--; continue; }
            if (depth == 0 && iequals_at(expr, i, "else")) {
                else_pos = i;
                break;
            }
        }

        if (else_pos != std::string::npos) {
            then_expr = trim(expr.substr(then_start, else_pos - then_start));
            else_expr = trim(expr.substr(else_pos + 4));
        } else {
            then_expr = trim(expr.substr(then_start));
        }
    }

    auto cond_val = eval_expr(trim(cond_str), doc, env, ns);
    std::string cv = cond_val.first();
    bool is_true = !cv.empty() && cv != "false" && cv != "0";

    if (is_true) {
        return eval_expr(then_expr, doc, env, ns);
    } else {
        return eval_expr(else_expr, doc, env, ns);
    }
}

XQueryEngine::XQValue
XQueryEngine::eval_element_constructor(const std::string &expr, XmlNodePtr doc,
                                       const VarEnv &env, const Namespaces &ns) {
    // <tag attr="val">{content}</tag>
    // Simple parse: find the tag name, then content between > and </
    size_t pos = 1; // skip '<'
    std::string tag;
    while (pos < expr.size() && !std::isspace(static_cast<unsigned char>(expr[pos]))
           && expr[pos] != '>' && expr[pos] != '/') {
        tag += expr[pos++];
    }

    // Skip attributes (include them verbatim)
    std::string attrs;
    while (pos < expr.size() && expr[pos] != '>' && expr[pos] != '/') {
        attrs += expr[pos++];
    }

    // Self-closing?
    if (pos < expr.size() && expr[pos] == '/') {
        XQValue result;
        result.items.push_back("<" + tag + attrs + "/>");
        return result;
    }

    if (pos < expr.size() && expr[pos] == '>') pos++; // skip '>'

    // Find closing tag
    std::string closing = "</" + tag + ">";
    size_t close_pos = expr.rfind(closing);
    if (close_pos == std::string::npos || close_pos < pos) {
        XQValue result;
        result.items.push_back(expr);
        return result;
    }

    std::string content = expr.substr(pos, close_pos - pos);

    // Process enclosed expressions {expr} in the content
    std::string built_content;
    size_t ci = 0;
    while (ci < content.size()) {
        if (content[ci] == '{') {
            ci++; // skip '{'
            int depth = 1;
            std::string inner;
            while (ci < content.size() && depth > 0) {
                if (content[ci] == '{') depth++;
                else if (content[ci] == '}') { depth--; if (depth == 0) { ci++; break; } }
                inner += content[ci];
                ci++;
            }
            auto val = eval_expr(trim(inner), doc, env, ns);
            for (size_t i = 0; i < val.items.size(); ++i) {
                if (i > 0) built_content += " ";
                built_content += val.items[i];
            }
        } else {
            built_content += content[ci++];
        }
    }

    XQValue result;
    result.items.push_back("<" + tag + attrs + ">" + built_content + closing);
    return result;
}

XQueryEngine::XQValue
XQueryEngine::eval_path(const std::string &expr, XmlNodePtr doc,
                        const VarEnv &env, const Namespaces &ns) {
    // Substitute variables in path
    std::string path = substitute_vars(expr, env);

    // Use the XPath engine
    // Serialize doc back to XML for xpath evaluation
    std::string xml;
    if (doc && doc->tag == "#document" && !doc->children.empty()) {
        for (auto &ch : doc->children) {
            xml += ch->to_xml();
        }
    } else if (doc) {
        xml = doc->to_xml();
    }

    XPathEngine::Namespaces xpath_ns;
    for (auto &[prefix, uri] : ns) {
        xpath_ns.push_back({prefix, uri});
    }

    auto results = xpath_engine_.evaluate(xml, path, xpath_ns);
    XQValue v;
    v.items = results;
    return v;
}

XQueryEngine::XQValue
XQueryEngine::eval_function(const std::string &name,
                            const std::vector<std::string> &args,
                            XmlNodePtr doc, const VarEnv &env,
                            const Namespaces &ns) {
    if (name == "concat") {
        std::string result;
        for (auto &arg : args) {
            auto val = eval_expr(trim(arg), doc, env, ns);
            result += val.first();
        }
        XQValue v;
        v.items.push_back(result);
        return v;
    }
    if (name == "string") {
        if (args.empty()) return {};
        return eval_expr(trim(args[0]), doc, env, ns);
    }
    if (name == "data") {
        if (args.empty()) return {};
        return eval_expr(trim(args[0]), doc, env, ns);
    }
    if (name == "number") {
        if (args.empty()) return {};
        auto val = eval_expr(trim(args[0]), doc, env, ns);
        // Try to convert to number
        XQValue v;
        try {
            double d = std::stod(val.first());
            std::ostringstream os;
            if (d == std::floor(d) && std::abs(d) < 1e15) {
                os << static_cast<int64_t>(d);
            } else {
                os << d;
            }
            v.items.push_back(os.str());
        } catch (...) {
            v.items.push_back("NaN");
        }
        return v;
    }
    if (name == "count") {
        if (args.empty()) { XQValue v; v.items.push_back("0"); return v; }
        auto val = eval_expr(trim(args[0]), doc, env, ns);
        XQValue v;
        v.items.push_back(std::to_string(val.items.size()));
        return v;
    }
    if (name == "contains") {
        if (args.size() < 2) return {};
        auto s1 = eval_expr(trim(args[0]), doc, env, ns).first();
        auto s2 = eval_expr(trim(args[1]), doc, env, ns).first();
        XQValue v;
        v.items.push_back(s1.find(s2) != std::string::npos ? "true" : "false");
        return v;
    }
    if (name == "starts-with") {
        if (args.size() < 2) return {};
        auto s1 = eval_expr(trim(args[0]), doc, env, ns).first();
        auto s2 = eval_expr(trim(args[1]), doc, env, ns).first();
        XQValue v;
        v.items.push_back(::tdb::doc::starts_with(s1, s2) ? "true" : "false");
        return v;
    }
    if (name == "substring") {
        if (args.empty()) return {};
        auto s = eval_expr(trim(args[0]), doc, env, ns).first();
        int start = 1;
        int len = static_cast<int>(s.size());
        if (args.size() >= 2) {
            auto sv = eval_expr(trim(args[1]), doc, env, ns).first();
            start = std::atoi(sv.c_str());
        }
        if (args.size() >= 3) {
            auto lv = eval_expr(trim(args[2]), doc, env, ns).first();
            len = std::atoi(lv.c_str());
        }
        // XPath/XQuery substring is 1-based
        if (start < 1) start = 1;
        size_t idx = static_cast<size_t>(start - 1);
        size_t count = static_cast<size_t>(len);
        XQValue v;
        if (idx < s.size()) {
            v.items.push_back(s.substr(idx, count));
        } else {
            v.items.push_back("");
        }
        return v;
    }
    if (name == "string-length") {
        if (args.empty()) return {};
        auto s = eval_expr(trim(args[0]), doc, env, ns).first();
        XQValue v;
        v.items.push_back(std::to_string(s.size()));
        return v;
    }
    if (name == "upper-case") {
        if (args.empty()) return {};
        auto s = eval_expr(trim(args[0]), doc, env, ns).first();
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        XQValue v;
        v.items.push_back(s);
        return v;
    }
    if (name == "lower-case") {
        if (args.empty()) return {};
        auto s = eval_expr(trim(args[0]), doc, env, ns).first();
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        XQValue v;
        v.items.push_back(s);
        return v;
    }
    if (name == "not") {
        if (args.empty()) return {};
        auto val = eval_expr(trim(args[0]), doc, env, ns).first();
        XQValue v;
        bool is_true = !val.empty() && val != "false" && val != "0";
        v.items.push_back(is_true ? "false" : "true");
        return v;
    }
    if (name == "true") {
        XQValue v; v.items.push_back("true"); return v;
    }
    if (name == "false") {
        XQValue v; v.items.push_back("false"); return v;
    }
    if (name == "string-join") {
        if (args.empty()) return {};
        auto seq = eval_expr(trim(args[0]), doc, env, ns);
        std::string sep;
        if (args.size() >= 2) {
            sep = eval_expr(trim(args[1]), doc, env, ns).first();
        }
        std::string result;
        for (size_t i = 0; i < seq.items.size(); ++i) {
            if (i > 0) result += sep;
            result += seq.items[i];
        }
        XQValue v;
        v.items.push_back(result);
        return v;
    }
    if (name == "position") {
        // In a for loop context, position is often implicit. Return "1" as default.
        XQValue v; v.items.push_back("1"); return v;
    }
    if (name == "last") {
        XQValue v; v.items.push_back("1"); return v;
    }

    // Unknown function: try evaluating as XPath
    XQValue v;
    v.items.push_back("");
    return v;
}

XQueryEngine::XQValue
XQueryEngine::eval_comparison(const std::string &expr, XmlNodePtr doc,
                              const VarEnv &env, const Namespaces &ns) {
    // Find the comparison operator at depth 0
    int depth = 0;
    bool in_str = false;
    char sc = 0;
    size_t op_pos = std::string::npos;
    size_t op_len = 0;
    std::string op;

    for (size_t i = 0; i < expr.size(); ++i) {
        char c = expr[i];
        if (in_str) { if (c == sc) in_str = false; continue; }
        if (c == '\'' || c == '"') { in_str = true; sc = c; continue; }
        if (c == '(' || c == '[') { depth++; continue; }
        if (c == ')' || c == ']') { depth--; continue; }
        if (depth != 0) continue;

        if (c == '!' && i + 1 < expr.size() && expr[i + 1] == '=') {
            op_pos = i; op_len = 2; op = "!="; break;
        }
        if (c == '<' && i + 1 < expr.size() && expr[i + 1] == '=') {
            op_pos = i; op_len = 2; op = "<="; break;
        }
        if (c == '>' && i + 1 < expr.size() && expr[i + 1] == '=') {
            op_pos = i; op_len = 2; op = ">="; break;
        }
        if (c == '=' && (i == 0 || (expr[i - 1] != '!' && expr[i - 1] != '<' && expr[i - 1] != '>'))) {
            op_pos = i; op_len = 1; op = "="; break;
        }
        if (c == '<') {
            // Ensure not an element constructor
            if (i + 1 < expr.size() && std::isalpha(static_cast<unsigned char>(expr[i + 1]))) continue;
            op_pos = i; op_len = 1; op = "<"; break;
        }
        if (c == '>') {
            op_pos = i; op_len = 1; op = ">"; break;
        }
    }

    if (op_pos == std::string::npos) {
        // No operator found, treat as path
        return eval_path(expr, doc, env, ns);
    }

    std::string left_str = trim(expr.substr(0, op_pos));
    std::string right_str = trim(expr.substr(op_pos + op_len));

    auto left_val = eval_expr(left_str, doc, env, ns);
    auto right_val = eval_expr(right_str, doc, env, ns);

    std::string lv = left_val.first();
    std::string rv = right_val.first();

    bool result = false;

    // Try numeric comparison first
    bool numeric = true;
    double ld = 0, rd = 0;
    try { ld = std::stod(lv); } catch (...) { numeric = false; }
    try { rd = std::stod(rv); } catch (...) { numeric = false; }

    if (numeric) {
        if (op == "=") result = (ld == rd);
        else if (op == "!=") result = (ld != rd);
        else if (op == "<") result = (ld < rd);
        else if (op == ">") result = (ld > rd);
        else if (op == "<=") result = (ld <= rd);
        else if (op == ">=") result = (ld >= rd);
    } else {
        if (op == "=") result = (lv == rv);
        else if (op == "!=") result = (lv != rv);
        else if (op == "<") result = (lv < rv);
        else if (op == ">") result = (lv > rv);
        else if (op == "<=") result = (lv <= rv);
        else if (op == ">=") result = (lv >= rv);
    }

    XQValue v;
    v.items.push_back(result ? "true" : "false");
    return v;
}

XQueryEngine::XQValue
XQueryEngine::eval_string_literal(const std::string &expr) {
    XQValue v;
    if (expr.size() >= 2) {
        v.items.push_back(expr.substr(1, expr.size() - 2));
    }
    return v;
}

XQueryEngine::XQValue
XQueryEngine::eval_variable(const std::string &name, const VarEnv &env) {
    // Search backwards to find the most recent binding
    for (auto it = env.rbegin(); it != env.rend(); ++it) {
        if (it->name == name) return it->value;
    }
    return {};
}

XQueryEngine::XQValue
XQueryEngine::eval_concat(const std::vector<std::string> &parts, XmlNodePtr doc,
                          const VarEnv &env, const Namespaces &ns) {
    std::string result;
    for (auto &p : parts) {
        auto val = eval_expr(trim(p), doc, env, ns);
        result += val.first();
    }
    XQValue v;
    v.items.push_back(result);
    return v;
}

std::string XQueryEngine::substitute_vars(const std::string &s, const VarEnv &env) {
    std::string result;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '$') {
            std::string varname;
            size_t j = i + 1;
            while (j < s.size() && (std::isalnum(static_cast<unsigned char>(s[j])) || s[j] == '_' || s[j] == '-')) {
                varname += s[j++];
            }
            auto val = eval_variable(varname, env);
            if (!val.is_empty()) {
                result += val.first();
                i = j - 1;
                continue;
            }
        }
        result += s[i];
    }
    return result;
}

// ─── FLWOR parsing helpers ───

std::string XQueryEngine::extract_for_var(const std::string &expr, size_t &pos) {
    while (pos < expr.size() && std::isspace(static_cast<unsigned char>(expr[pos]))) pos++;
    std::string var;
    if (pos < expr.size() && expr[pos] == '$') {
        pos++; // skip '$'
        while (pos < expr.size() && (std::isalnum(static_cast<unsigned char>(expr[pos])) || expr[pos] == '_' || expr[pos] == '-')) {
            var += expr[pos++];
        }
    }
    return var;
}

std::string XQueryEngine::extract_for_in(const std::string &expr, size_t &pos) {
    while (pos < expr.size() && std::isspace(static_cast<unsigned char>(expr[pos]))) pos++;
    // Read until we hit "let", "where", "order", "return", or another "for" at depth 0
    std::string result;
    int depth = 0;
    bool in_str = false;
    char sc = 0;
    while (pos < expr.size()) {
        char c = expr[pos];
        if (in_str) {
            if (c == sc) in_str = false;
            result += c; pos++; continue;
        }
        if (c == '\'' || c == '"') { in_str = true; sc = c; result += c; pos++; continue; }
        if (c == '(' || c == '[') { depth++; result += c; pos++; continue; }
        if (c == ')' || c == ']') { depth--; result += c; pos++; continue; }

        if (depth == 0) {
            if (iequals_at(expr, pos, "let") || iequals_at(expr, pos, "where") ||
                iequals_at(expr, pos, "order") || iequals_at(expr, pos, "return") ||
                iequals_at(expr, pos, "for")) {
                break;
            }
        }
        result += c;
        pos++;
    }
    return trim(result);
}

std::string XQueryEngine::extract_let_var(const std::string &expr, size_t &pos) {
    return extract_for_var(expr, pos);
}

std::string XQueryEngine::extract_let_value(const std::string &expr, size_t &pos) {
    while (pos < expr.size() && std::isspace(static_cast<unsigned char>(expr[pos]))) pos++;
    // Skip ":="
    if (pos + 1 < expr.size() && expr[pos] == ':' && expr[pos + 1] == '=') pos += 2;
    return extract_for_in(expr, pos); // same logic
}

std::string XQueryEngine::extract_where(const std::string &expr, size_t &pos) {
    while (pos < expr.size() && std::isspace(static_cast<unsigned char>(expr[pos]))) pos++;
    std::string result;
    int depth = 0;
    bool in_str = false;
    char sc = 0;
    while (pos < expr.size()) {
        char c = expr[pos];
        if (in_str) { if (c == sc) in_str = false; result += c; pos++; continue; }
        if (c == '\'' || c == '"') { in_str = true; sc = c; result += c; pos++; continue; }
        if (c == '(' || c == '[') { depth++; result += c; pos++; continue; }
        if (c == ')' || c == ']') { depth--; result += c; pos++; continue; }
        if (depth == 0 && (iequals_at(expr, pos, "order") || iequals_at(expr, pos, "return"))) break;
        result += c;
        pos++;
    }
    return trim(result);
}

std::string XQueryEngine::extract_order_by(const std::string &expr, size_t &pos) {
    while (pos < expr.size() && std::isspace(static_cast<unsigned char>(expr[pos]))) pos++;
    std::string result;
    int depth = 0;
    bool in_str = false;
    char sc = 0;
    while (pos < expr.size()) {
        char c = expr[pos];
        if (in_str) { if (c == sc) in_str = false; result += c; pos++; continue; }
        if (c == '\'' || c == '"') { in_str = true; sc = c; result += c; pos++; continue; }
        if (c == '(' || c == '[') { depth++; result += c; pos++; continue; }
        if (c == ')' || c == ']') { depth--; result += c; pos++; continue; }
        if (depth == 0 && iequals_at(expr, pos, "return")) break;
        result += c;
        pos++;
    }
    return trim(result);
}

std::string XQueryEngine::extract_return(const std::string &expr, size_t &pos) {
    while (pos < expr.size() && std::isspace(static_cast<unsigned char>(expr[pos]))) pos++;
    // Return expression is everything remaining
    return trim(expr.substr(pos));
}

std::string XQueryEngine::extract_balanced(const std::string &expr, size_t &pos, char open, char close) {
    if (pos >= expr.size() || expr[pos] != open) return "";
    pos++; // skip open
    int depth = 1;
    std::string result;
    while (pos < expr.size() && depth > 0) {
        if (expr[pos] == open) depth++;
        else if (expr[pos] == close) { depth--; if (depth == 0) { pos++; break; } }
        result += expr[pos];
        pos++;
    }
    return result;
}

std::vector<std::string>
XQueryEngine::split_function_args(const std::string &args_str) {
    std::vector<std::string> args;
    if (args_str.empty()) return args;

    int depth = 0;
    bool in_str = false;
    char sc = 0;
    std::string current;

    for (size_t i = 0; i < args_str.size(); ++i) {
        char c = args_str[i];
        if (in_str) {
            current += c;
            if (c == sc) in_str = false;
            continue;
        }
        if (c == '\'' || c == '"') { in_str = true; sc = c; current += c; continue; }
        if (c == '(' || c == '[') { depth++; current += c; continue; }
        if (c == ')' || c == ']') { depth--; current += c; continue; }
        if (c == ',' && depth == 0) {
            args.push_back(trim(current));
            current.clear();
            continue;
        }
        current += c;
    }
    if (!current.empty()) args.push_back(trim(current));
    return args;
}

// ============================================================================
// GraphQL Engine
// ============================================================================

GraphQLEngine::EvalResult
GraphQLEngine::evaluate(const std::string &json_string,
                        const std::string &graphql_query,
                        const Variables &variables) {
    auto json_doc = JsonParser::parse(json_string);
    if (!json_doc) {
        return {"null", {}};
    }

    auto parsed = parse_query(graphql_query, variables);

    std::vector<JoinHint> join_hints;
    auto result = resolve_selection(json_doc, parsed.fields, parsed.fragments, join_hints);

    return {result ? result->to_json() : "null", join_hints};
}

void GraphQLEngine::skip_ws(const std::string &src, size_t &pos) {
    while (pos < src.size()) {
        char c = src[pos];
        if (std::isspace(static_cast<unsigned char>(c)) || c == ',') {
            pos++;
        } else if (pos + 1 < src.size() && c == '#') {
            // Line comment
            while (pos < src.size() && src[pos] != '\n') pos++;
        } else {
            break;
        }
    }
}

std::string GraphQLEngine::parse_gql_name(const std::string &src, size_t &pos) {
    std::string name;
    while (pos < src.size() && (std::isalnum(static_cast<unsigned char>(src[pos])) || src[pos] == '_')) {
        name += src[pos++];
    }
    return name;
}

std::string GraphQLEngine::parse_gql_value(const std::string &src, size_t &pos) {
    skip_ws(src, pos);
    if (pos >= src.size()) return "";

    char c = src[pos];

    // String value
    if (c == '"') {
        pos++; // skip opening quote
        std::string val;
        while (pos < src.size() && src[pos] != '"') {
            if (src[pos] == '\\' && pos + 1 < src.size()) {
                pos++;
                val += src[pos++];
            } else {
                val += src[pos++];
            }
        }
        if (pos < src.size()) pos++; // skip closing quote
        return val;
    }

    // Variable reference: $varName
    if (c == '$') {
        pos++; // skip '$'
        return "$" + parse_gql_name(src, pos);
    }

    // Number, boolean, enum, null
    std::string val;
    while (pos < src.size() && !std::isspace(static_cast<unsigned char>(src[pos]))
           && src[pos] != ')' && src[pos] != ',' && src[pos] != '}' && src[pos] != ']') {
        val += src[pos++];
    }
    return val;
}

std::vector<GraphQLEngine::Argument>
GraphQLEngine::parse_arguments(const std::string &src, size_t &pos) {
    std::vector<Argument> args;
    skip_ws(src, pos);
    if (pos >= src.size() || src[pos] != '(') return args;
    pos++; // skip '('

    while (pos < src.size()) {
        skip_ws(src, pos);
        if (pos < src.size() && src[pos] == ')') { pos++; break; }

        Argument arg;
        arg.name = parse_gql_name(src, pos);
        skip_ws(src, pos);
        if (pos < src.size() && src[pos] == ':') pos++; // skip ':'
        arg.value = parse_gql_value(src, pos);
        args.push_back(arg);

        skip_ws(src, pos);
        if (pos < src.size() && src[pos] == ',') pos++;
    }
    return args;
}

std::vector<GraphQLEngine::Directive>
GraphQLEngine::parse_directives(const std::string &src, size_t &pos) {
    std::vector<Directive> dirs;
    while (pos < src.size()) {
        skip_ws(src, pos);
        if (pos >= src.size() || src[pos] != '@') break;
        pos++; // skip '@'

        Directive dir;
        dir.name = parse_gql_name(src, pos);
        dir.args = parse_arguments(src, pos);
        dirs.push_back(dir);
    }
    return dirs;
}

GraphQLEngine::Field
GraphQLEngine::parse_field(const std::string &src, size_t &pos) {
    Field field;
    skip_ws(src, pos);

    // Check for fragment spread: ... on Type { } or ...FragmentName
    if (pos + 2 < src.size() && src[pos] == '.' && src[pos + 1] == '.' && src[pos + 2] == '.') {
        pos += 3;
        skip_ws(src, pos);

        // Inline fragment: ... on Type { }
        if (pos + 2 < src.size() && src[pos] == 'o' && src[pos + 1] == 'n' &&
            (pos + 2 >= src.size() || std::isspace(static_cast<unsigned char>(src[pos + 2])))) {
            pos += 2;
            skip_ws(src, pos);
            std::string type_name = parse_gql_name(src, pos);
            field.name = "__inline_fragment";
            field.alias = type_name;
            skip_ws(src, pos);
            if (pos < src.size() && src[pos] == '{') {
                field.sub_fields = parse_selection_set(src, pos);
            }
            return field;
        }

        // Named fragment spread
        field.name = "__fragment_spread";
        field.fragment_name = parse_gql_name(src, pos);
        return field;
    }

    // Parse name (could be "alias: name" or just "name")
    std::string first_name = parse_gql_name(src, pos);
    skip_ws(src, pos);

    if (pos < src.size() && src[pos] == ':') {
        pos++; // skip ':'
        skip_ws(src, pos);
        field.alias = first_name;
        field.name = parse_gql_name(src, pos);
    } else {
        field.name = first_name;
    }

    // Parse arguments
    skip_ws(src, pos);
    if (pos < src.size() && src[pos] == '(') {
        field.arguments = parse_arguments(src, pos);
    }

    // Parse directives
    field.directives = parse_directives(src, pos);

    // Parse sub-selection
    skip_ws(src, pos);
    if (pos < src.size() && src[pos] == '{') {
        field.sub_fields = parse_selection_set(src, pos);
    }

    return field;
}

std::vector<GraphQLEngine::Field>
GraphQLEngine::parse_selection_set(const std::string &src, size_t &pos) {
    std::vector<Field> fields;
    skip_ws(src, pos);
    if (pos >= src.size() || src[pos] != '{') return fields;
    pos++; // skip '{'

    while (pos < src.size()) {
        skip_ws(src, pos);
        if (pos < src.size() && src[pos] == '}') { pos++; break; }
        if (pos >= src.size()) break;

        fields.push_back(parse_field(src, pos));
    }
    return fields;
}

GraphQLEngine::Fragment
GraphQLEngine::parse_fragment(const std::string &src, size_t &pos) {
    Fragment frag;
    // "fragment Name on Type { ... }"
    skip_ws(src, pos);
    // Skip "fragment"
    if (pos + 8 < src.size() && src.compare(pos, 8, "fragment") == 0) {
        pos += 8;
    }
    skip_ws(src, pos);
    frag.name = parse_gql_name(src, pos);
    skip_ws(src, pos);
    // Skip "on"
    if (pos + 2 <= src.size() && src[pos] == 'o' && src[pos + 1] == 'n') {
        pos += 2;
    }
    skip_ws(src, pos);
    frag.on_type = parse_gql_name(src, pos);
    skip_ws(src, pos);
    frag.fields = parse_selection_set(src, pos);
    return frag;
}

GraphQLEngine::ParsedQuery
GraphQLEngine::parse_query(const std::string &query, const Variables &vars) {
    ParsedQuery result;
    size_t pos = 0;
    skip_ws(query, pos);

    // Collect fragments and the main query
    while (pos < query.size()) {
        skip_ws(query, pos);
        if (pos >= query.size()) break;

        // Check for "fragment"
        if (pos + 8 < query.size() && query.compare(pos, 8, "fragment") == 0 &&
            !std::isalnum(static_cast<unsigned char>(query[pos + 8]))) {
            result.fragments.push_back(parse_fragment(query, pos));
            continue;
        }

        // Check for "query" or "mutation" keyword (skip it)
        if (pos + 5 < query.size() && query.compare(pos, 5, "query") == 0 &&
            !std::isalnum(static_cast<unsigned char>(query[pos + 5]))) {
            pos += 5;
            skip_ws(query, pos);
            // Skip optional operation name
            if (pos < query.size() && std::isalpha(static_cast<unsigned char>(query[pos]))) {
                parse_gql_name(query, pos);
            }
            // Skip optional variable definitions
            if (pos < query.size() && query[pos] == '(') {
                int depth = 0;
                while (pos < query.size()) {
                    if (query[pos] == '(') depth++;
                    else if (query[pos] == ')') { depth--; if (depth == 0) { pos++; break; } }
                    pos++;
                }
            }
            skip_ws(query, pos);
        }

        // Parse the selection set
        if (pos < query.size() && query[pos] == '{') {
            auto fields = parse_selection_set(query, pos);
            result.fields.insert(result.fields.end(), fields.begin(), fields.end());
            continue;
        }

        pos++; // skip unrecognized
    }

    // Substitute variables in argument values
    std::function<void(std::vector<Field> &)> subst_vars = [&](std::vector<Field> &fields) {
        for (auto &f : fields) {
            for (auto &arg : f.arguments) {
                if (!arg.value.empty() && arg.value[0] == '$') {
                    std::string vname = arg.value.substr(1);
                    auto it = vars.find(vname);
                    if (it != vars.end()) {
                        arg.value = it->second;
                    }
                }
            }
            subst_vars(f.sub_fields);
        }
    };
    subst_vars(result.fields);

    return result;
}

JsonNodePtr
GraphQLEngine::resolve_field(const JsonNodePtr &node, const Field &field,
                             const std::vector<Fragment> &fragments,
                             std::vector<JoinHint> &join_hints) {
    if (!node) return JsonNode::make_null();

    // Handle fragment spread
    if (field.name == "__fragment_spread") {
        for (auto &frag : fragments) {
            if (frag.name == field.fragment_name) {
                return resolve_selection(node, frag.fields, fragments, join_hints);
            }
        }
        return JsonNode::make_null();
    }

    // Handle inline fragment (just process sub-fields)
    if (field.name == "__inline_fragment") {
        return resolve_selection(node, field.sub_fields, fragments, join_hints);
    }

    // Collect join hints from @join directive
    collect_join_hints(field, join_hints);

    // Navigate to the field in the JSON node
    JsonNodePtr value;
    if (node->type == JsonNode::Type::OBJECT) {
        value = node->get(field.name);
    }

    // Apply arguments as filters (for array elements)
    if (value && value->type == JsonNode::Type::ARRAY && !field.arguments.empty()) {
        // Filter array elements by argument criteria
        auto filtered = JsonNode::make_array();
        for (auto &elem : value->array_elements) {
            if (!elem || elem->type != JsonNode::Type::OBJECT) continue;
            bool match = true;
            for (auto &arg : field.arguments) {
                auto member = elem->get(arg.name);
                if (!member) { match = false; break; }
                std::string member_val;
                if (member->type == JsonNode::Type::STRING) member_val = member->str_val;
                else if (member->type == JsonNode::Type::NUMBER) {
                    if (member->num_val == std::floor(member->num_val) && std::abs(member->num_val) < 1e15) {
                        member_val = std::to_string(static_cast<int64_t>(member->num_val));
                    } else {
                        std::ostringstream os;
                        os << member->num_val;
                        member_val = os.str();
                    }
                } else if (member->type == JsonNode::Type::BOOL) {
                    member_val = member->bool_val ? "true" : "false";
                }
                if (member_val != arg.value) { match = false; break; }
            }
            if (match) filtered->array_elements.push_back(elem);
        }
        value = filtered;
    }

    if (!value) return JsonNode::make_null();

    // If there are sub-fields, recurse
    if (!field.sub_fields.empty()) {
        if (value->type == JsonNode::Type::ARRAY) {
            auto result_arr = JsonNode::make_array();
            for (auto &elem : value->array_elements) {
                auto resolved = resolve_selection(elem, field.sub_fields, fragments, join_hints);
                result_arr->array_elements.push_back(resolved);
            }
            return result_arr;
        }
        return resolve_selection(value, field.sub_fields, fragments, join_hints);
    }

    return value;
}

JsonNodePtr
GraphQLEngine::resolve_selection(const JsonNodePtr &node,
                                 const std::vector<Field> &fields,
                                 const std::vector<Fragment> &fragments,
                                 std::vector<JoinHint> &join_hints) {
    if (!node || fields.empty()) return node ? node : JsonNode::make_null();

    // If the node is an array, apply selection to each element
    if (node->type == JsonNode::Type::ARRAY) {
        auto result = JsonNode::make_array();
        for (auto &elem : node->array_elements) {
            auto resolved = resolve_selection(elem, fields, fragments, join_hints);
            result->array_elements.push_back(resolved);
        }
        return result;
    }

    auto result = JsonNode::make_object();

    for (auto &field : fields) {
        // Handle fragment spread: merge its fields into the result
        if (field.name == "__fragment_spread") {
            for (auto &frag : fragments) {
                if (frag.name == field.fragment_name) {
                    auto frag_result = resolve_selection(node, frag.fields, fragments, join_hints);
                    if (frag_result && frag_result->type == JsonNode::Type::OBJECT) {
                        for (auto &[k, v] : frag_result->object_members) {
                            result->object_members.emplace_back(k, v);
                        }
                    }
                    break;
                }
            }
            continue;
        }

        // Handle inline fragment: merge sub-fields into result
        if (field.name == "__inline_fragment") {
            auto inline_result = resolve_selection(node, field.sub_fields, fragments, join_hints);
            if (inline_result && inline_result->type == JsonNode::Type::OBJECT) {
                for (auto &[k, v] : inline_result->object_members) {
                    result->object_members.emplace_back(k, v);
                }
            }
            continue;
        }

        auto resolved = resolve_field(node, field, fragments, join_hints);
        std::string output_name = field.alias.empty() ? field.name : field.alias;
        result->object_members.emplace_back(output_name, resolved);
    }

    return result;
}

void GraphQLEngine::collect_join_hints(const Field &field,
                                       std::vector<JoinHint> &hints) {
    for (auto &dir : field.directives) {
        if (dir.name == "join") {
            JoinHint hint;
            hint.local_field = field.name;
            for (auto &arg : dir.args) {
                if (arg.name == "table") hint.table = arg.value;
                else if (arg.name == "column") hint.column = arg.value;
            }
            if (!hint.table.empty()) {
                hints.push_back(hint);
            }
        }
    }
}

// ============================================================================
// Document Query Executor
// ============================================================================

DocumentQueryExecutor::DocumentQueryExecutor(catalog::Catalog &catalog)
    : catalog_(catalog) {}

DocumentQueryExecutor::ColumnData
DocumentQueryExecutor::read_column(const std::string &table_name,
                                   const std::string &column_name) {
    ColumnData result;

    auto *table = catalog_.find_table(table_name);
    if (!table) return result;

    int col_idx = catalog_.find_column_index(table_name, column_name);
    if (col_idx < 0) return result;

    for (size_t i = 0; i < table->rows.size(); ++i) {
        auto &row = table->rows[i];
        if (static_cast<size_t>(col_idx) < row.size()) {
            result.values.push_back(row[static_cast<size_t>(col_idx)].to_string());
            result.row_indices.push_back(i);
        }
    }

    return result;
}

sql::ResultSet
DocumentQueryExecutor::exec_xpath(const sql::ast::XPathQueryStmt &stmt) {
    sql::ResultSet rs;
    rs.success = false;

    auto *table = catalog_.find_table(stmt.source_table);
    if (!table) {
        rs.error_message = "Table not found: " + stmt.source_table;
        return rs;
    }

    auto col_data = read_column(stmt.source_table, stmt.source_column);

    // Build namespace list
    XPathEngine::Namespaces ns;
    for (auto &[prefix, uri] : stmt.namespaces) {
        ns.push_back({prefix, uri});
    }

    // Determine output column name
    std::string col_name = stmt.alias.value_or("xpath_result");

    rs.columns.push_back({col_name, stmt.source_table});
    rs.columns.push_back({"source_row", stmt.source_table});

    for (size_t i = 0; i < col_data.values.size(); ++i) {
        auto results = xpath_engine_.evaluate(col_data.values[i], stmt.xpath_expr, ns);
        for (auto &r : results) {
            sql::Tuple tuple;
            tuple.push_back(sql::Value::make_string(r));
            tuple.push_back(sql::Value::make_int(static_cast<int64_t>(col_data.row_indices[i])));
            rs.rows.push_back(std::move(tuple));
        }
    }

    rs.rows_affected = static_cast<int64_t>(rs.rows.size());
    rs.success = true;
    return rs;
}

sql::ResultSet
DocumentQueryExecutor::exec_xquery(const sql::ast::XQueryStmt &stmt) {
    sql::ResultSet rs;
    rs.success = false;

    auto *table = catalog_.find_table(stmt.source_table);
    if (!table) {
        rs.error_message = "Table not found: " + stmt.source_table;
        return rs;
    }

    auto col_data = read_column(stmt.source_table, stmt.source_column);

    XQueryEngine::Namespaces ns;
    for (auto &[prefix, uri] : stmt.namespaces) {
        ns.push_back({prefix, uri});
    }

    // Build parameters from the PASSING clause
    // The passing_params have ExprPtr values; for simplicity we evaluate them
    // as literal strings from the expression.
    XQueryEngine::Params params;
    for (auto &[name, expr_ptr] : stmt.passing_params) {
        if (expr_ptr) {
            // Try to extract literal value
            if (expr_ptr->type == sql::ast::ExprType::LITERAL) {
                auto &lit = std::get<sql::ast::Literal>(expr_ptr->data);
                params.push_back({name, lit.value});
            } else {
                params.push_back({name, ""});
            }
        }
    }

    std::string col_name = stmt.alias.value_or("xquery_result");
    rs.columns.push_back({col_name, stmt.source_table});
    rs.columns.push_back({"source_row", stmt.source_table});

    for (size_t i = 0; i < col_data.values.size(); ++i) {
        auto results = xquery_engine_.evaluate(col_data.values[i], stmt.xquery_expr, params, ns);
        for (auto &r : results) {
            sql::Tuple tuple;
            tuple.push_back(sql::Value::make_string(r));
            tuple.push_back(sql::Value::make_int(static_cast<int64_t>(col_data.row_indices[i])));
            rs.rows.push_back(std::move(tuple));
        }
    }

    rs.rows_affected = static_cast<int64_t>(rs.rows.size());
    rs.success = true;
    return rs;
}

sql::ResultSet
DocumentQueryExecutor::exec_graphql(const sql::ast::GraphQLQueryStmt &stmt) {
    sql::ResultSet rs;
    rs.success = false;

    auto *table = catalog_.find_table(stmt.source_table);
    if (!table) {
        rs.error_message = "Table not found: " + stmt.source_table;
        return rs;
    }

    auto col_data = read_column(stmt.source_table, stmt.source_column);

    // Build variables map
    GraphQLEngine::Variables vars;
    for (auto &[name, expr_ptr] : stmt.variables) {
        if (expr_ptr && expr_ptr->type == sql::ast::ExprType::LITERAL) {
            auto &lit = std::get<sql::ast::Literal>(expr_ptr->data);
            vars[name] = lit.value;
        }
    }

    std::string col_name = stmt.alias.value_or("graphql_result");
    rs.columns.push_back({col_name, stmt.source_table});
    rs.columns.push_back({"source_row", stmt.source_table});

    for (size_t i = 0; i < col_data.values.size(); ++i) {
        auto eval_result = graphql_engine_.evaluate(col_data.values[i], stmt.graphql_query, vars);

        sql::Tuple tuple;
        tuple.push_back(sql::Value::make_string(eval_result.json));
        tuple.push_back(sql::Value::make_int(static_cast<int64_t>(col_data.row_indices[i])));
        rs.rows.push_back(std::move(tuple));

        // Process @join directives: for each join hint, look up the referenced
        // table and merge its data into the result.
        for (auto &hint : eval_result.join_hints) {
            auto *join_table = catalog_.find_table(hint.table);
            if (!join_table) continue;

            int join_col_idx = catalog_.find_column_index(hint.table, hint.column);
            if (join_col_idx < 0) continue;

            // Parse the current result to extract the local field value
            auto result_json = JsonParser::parse(eval_result.json);
            if (!result_json || result_json->type != JsonNode::Type::OBJECT) continue;

            auto local_val = result_json->get(hint.local_field);
            if (!local_val) continue;

            std::string match_value;
            if (local_val->type == JsonNode::Type::STRING) {
                match_value = local_val->str_val;
            } else if (local_val->type == JsonNode::Type::NUMBER) {
                if (local_val->num_val == std::floor(local_val->num_val) && std::abs(local_val->num_val) < 1e15) {
                    match_value = std::to_string(static_cast<int64_t>(local_val->num_val));
                } else {
                    std::ostringstream os;
                    os << local_val->num_val;
                    match_value = os.str();
                }
            } else {
                match_value = local_val->to_json();
            }

            // Scan the join table for matching rows
            for (auto &join_row : join_table->rows) {
                if (static_cast<size_t>(join_col_idx) >= join_row.size()) continue;
                std::string join_val = join_row[static_cast<size_t>(join_col_idx)].to_string();
                if (join_val == match_value) {
                    // Build a result tuple with the join table's columns appended
                    auto join_schema = catalog_.get_table_schema(hint.table);
                    // Create an augmented result row with join columns
                    for (size_t jc = 0; jc < join_row.size(); ++jc) {
                        // Add column to schema if this is the first matched row
                        std::string jcol_name = (jc < join_schema.size())
                            ? hint.table + "." + join_schema[jc].name
                            : hint.table + ".col" + std::to_string(jc);
                        // Check if column already in result schema
                        bool found = false;
                        for (auto &c : rs.columns) {
                            if (c.name == jcol_name) { found = true; break; }
                        }
                        if (!found) {
                            rs.columns.push_back({jcol_name, hint.table});
                        }
                    }

                    // Augment the last row with join columns
                    auto &last_row = rs.rows.back();
                    for (auto &jval : join_row) {
                        last_row.push_back(jval);
                    }
                    break; // Take first match only
                }
            }
        }
    }

    rs.rows_affected = static_cast<int64_t>(rs.rows.size());
    rs.success = true;
    return rs;
}

} // namespace tdb::doc
