#ifndef TDB_DOCQUERY_H
#define TDB_DOCQUERY_H

#include "tdb/sql/ast.h"
#include "tdb/sql/executor.h"
#include "tdb/catalog.h"
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include <variant>
#include <optional>
#include <functional>
#include <sstream>

namespace tdb::doc {

// ============================================================================
// XML Node tree (built by the recursive descent XML parser)
// ============================================================================
struct XmlNode {
    enum class Type { ELEMENT, TEXT, COMMENT, CDATA, PI };
    Type type = Type::ELEMENT;
    std::string tag;                                          // element name
    std::string text;                                         // text/CDATA content
    std::unordered_map<std::string, std::string> attributes;  // @attr
    std::vector<std::shared_ptr<XmlNode>> children;
    std::weak_ptr<XmlNode> parent;

    // Convenience: get all direct child elements with a given tag
    std::vector<std::shared_ptr<XmlNode>> child_elements(const std::string &name) const;
    // Convenience: get all descendant elements with a given tag
    std::vector<std::shared_ptr<XmlNode>> descendant_elements(const std::string &name) const;
    // Collect all text content (recursive)
    std::string all_text() const;
    // Serialize back to XML
    std::string to_xml() const;
};

using XmlNodePtr = std::shared_ptr<XmlNode>;

// ============================================================================
// Simple recursive descent XML parser
// ============================================================================
class XmlParser {
public:
    // Parse an XML string into a tree rooted at a virtual document node
    static XmlNodePtr parse(const std::string &xml);

private:
    explicit XmlParser(const std::string &xml);
    XmlNodePtr parse_document();
    XmlNodePtr parse_element();
    XmlNodePtr parse_text();
    void parse_prolog();
    void skip_comment();
    void skip_pi();

    char peek() const;
    char advance();
    bool at_end() const;
    void skip_whitespace();
    bool match(const std::string &s) const;
    void expect(char c);
    std::string parse_name();
    std::string parse_attr_value();
    std::string decode_entities(const std::string &s);

    std::string src_;
    size_t pos_;
};

// ============================================================================
// JSON Node tree (built by the recursive descent JSON parser)
// ============================================================================
struct JsonNode {
    enum class Type { OBJECT, ARRAY, STRING, NUMBER, BOOL, NUL };
    Type type = Type::NUL;

    // OBJECT
    std::vector<std::pair<std::string, std::shared_ptr<JsonNode>>> object_members;
    // ARRAY
    std::vector<std::shared_ptr<JsonNode>> array_elements;
    // STRING
    std::string str_val;
    // NUMBER
    double num_val = 0.0;
    // BOOL
    bool bool_val = false;

    // Lookup helpers
    std::shared_ptr<JsonNode> get(const std::string &key) const;
    std::shared_ptr<JsonNode> get(size_t index) const;

    // Serialize back to JSON
    std::string to_json() const;

    static std::shared_ptr<JsonNode> make_null();
    static std::shared_ptr<JsonNode> make_bool(bool v);
    static std::shared_ptr<JsonNode> make_number(double v);
    static std::shared_ptr<JsonNode> make_string(const std::string &v);
    static std::shared_ptr<JsonNode> make_array();
    static std::shared_ptr<JsonNode> make_object();
};

using JsonNodePtr = std::shared_ptr<JsonNode>;

// ============================================================================
// Simple recursive descent JSON parser
// ============================================================================
class JsonParser {
public:
    static JsonNodePtr parse(const std::string &json);

private:
    explicit JsonParser(const std::string &json);
    JsonNodePtr parse_value();
    JsonNodePtr parse_object();
    JsonNodePtr parse_array();
    JsonNodePtr parse_string_node();
    JsonNodePtr parse_number();
    JsonNodePtr parse_literal();
    std::string parse_string();

    char peek() const;
    char advance();
    bool at_end() const;
    void skip_whitespace();

    std::string src_;
    size_t pos_;
};

// ============================================================================
// XPath Engine
// ============================================================================
// Supports a practical subset of XPath 1.0:
//   /root/child          absolute path
//   //descendant         descendant axis
//   @attribute           attribute access
//   [predicate]          numeric position or boolean predicate
//   text()               text node content
//   position()           position in context
//   name()               element name
//   *                    wildcard element match
//   .                    current node
//   ..                   parent node
//   [n]                  positional predicate
//   [@attr='value']      attribute predicate
//
class XPathEngine {
public:
    using Namespaces = std::vector<std::pair<std::string, std::string>>;

    // Evaluate an XPath expression against an XML string.
    // Returns a vector of string results (serialized nodes or text values).
    std::vector<std::string> evaluate(const std::string &xml_string,
                                      const std::string &xpath_expr,
                                      const Namespaces &namespaces = {});

private:
    // Internal XPath step representation
    struct Step {
        enum class Axis { CHILD, DESCENDANT, DESCENDANT_OR_SELF, ATTRIBUTE, PARENT, SELF };
        Axis axis = Axis::CHILD;
        std::string node_test;            // tag name, "*", "text()", "node()", "comment()"
        struct Predicate {
            enum class Type { POSITION, ATTR_EQ, ATTR_EXISTS, EXPR };
            Type type = Type::POSITION;
            int position = 0;             // for POSITION
            std::string attr_name;        // for ATTR_EQ / ATTR_EXISTS
            std::string attr_value;       // for ATTR_EQ
            std::string raw;              // for EXPR (unparsed)
        };
        std::vector<Predicate> predicates;
    };

    std::vector<Step> parse_xpath(const std::string &expr);
    Step parse_step(const std::string &s);
    std::vector<Step::Predicate> parse_predicates(const std::string &s);

    std::vector<XmlNodePtr> apply_steps(const std::vector<XmlNodePtr> &context,
                                        const std::vector<Step> &steps,
                                        size_t step_idx);
    std::vector<XmlNodePtr> apply_step(const std::vector<XmlNodePtr> &context,
                                       const Step &step);
    std::vector<XmlNodePtr> apply_predicates(const std::vector<XmlNodePtr> &nodes,
                                             const std::vector<Step::Predicate> &preds);
    bool node_matches(const XmlNodePtr &node, const std::string &test);

    std::string serialize_node(const XmlNodePtr &node);
};

// ============================================================================
// XQuery Engine
// ============================================================================
// Supports a practical subset of XQuery 1.0:
//   FLWOR: for $var in expr let $var := expr where cond order by expr return expr
//   Element constructors: <tag>{expr}</tag>
//   Path expressions (delegated to XPathEngine)
//   String concatenation via concat()
//   Basic functions: string(), number(), count(), concat(), contains(),
//                    starts-with(), substring(), string-length(), data()
//   Comparisons: =, !=, <, >, <=, >=
//   Conditional: if (cond) then expr else expr
//
class XQueryEngine {
public:
    using Namespaces = std::vector<std::pair<std::string, std::string>>;
    using Params = std::vector<std::pair<std::string, std::string>>;

    std::vector<std::string> evaluate(const std::string &xml_string,
                                      const std::string &xquery_expr,
                                      const Params &params = {},
                                      const Namespaces &namespaces = {});

private:
    // XQuery value: either a sequence of strings or a single string
    struct XQValue {
        std::vector<std::string> items;
        bool is_empty() const { return items.empty(); }
        std::string first() const { return items.empty() ? "" : items[0]; }
    };

    struct Variable {
        std::string name;
        XQValue value;
    };

    using VarEnv = std::vector<Variable>;

    XQValue eval_expr(const std::string &expr, XmlNodePtr doc,
                      const VarEnv &env, const Namespaces &ns);
    XQValue eval_flwor(const std::string &expr, XmlNodePtr doc,
                       const VarEnv &env, const Namespaces &ns);
    XQValue eval_if(const std::string &expr, XmlNodePtr doc,
                    const VarEnv &env, const Namespaces &ns);
    XQValue eval_element_constructor(const std::string &expr, XmlNodePtr doc,
                                     const VarEnv &env, const Namespaces &ns);
    XQValue eval_path(const std::string &expr, XmlNodePtr doc,
                      const VarEnv &env, const Namespaces &ns);
    XQValue eval_function(const std::string &name, const std::vector<std::string> &args,
                          XmlNodePtr doc, const VarEnv &env, const Namespaces &ns);
    XQValue eval_comparison(const std::string &expr, XmlNodePtr doc,
                            const VarEnv &env, const Namespaces &ns);
    XQValue eval_string_literal(const std::string &expr);
    XQValue eval_variable(const std::string &name, const VarEnv &env);
    XQValue eval_concat(const std::vector<std::string> &parts, XmlNodePtr doc,
                        const VarEnv &env, const Namespaces &ns);

    std::string substitute_vars(const std::string &s, const VarEnv &env);

    // Parsing helpers for XQuery sub-expressions
    std::string extract_for_var(const std::string &expr, size_t &pos);
    std::string extract_for_in(const std::string &expr, size_t &pos);
    std::string extract_let_var(const std::string &expr, size_t &pos);
    std::string extract_let_value(const std::string &expr, size_t &pos);
    std::string extract_where(const std::string &expr, size_t &pos);
    std::string extract_order_by(const std::string &expr, size_t &pos);
    std::string extract_return(const std::string &expr, size_t &pos);
    std::string extract_balanced(const std::string &expr, size_t &pos, char open, char close);
    std::vector<std::string> split_function_args(const std::string &args_str);

    XPathEngine xpath_engine_;
};

// ============================================================================
// GraphQL Engine
// ============================================================================
// Parses and evaluates GraphQL-style queries against JSON documents.
// Supports:
//   { field { subfield } }
//   field(arg: value)
//   alias: field
//   fragment ... on Type { fields }
//   @join(table: "t", column: "c") directive for columnar joins
//
class GraphQLEngine {
public:
    using Variables = std::unordered_map<std::string, std::string>;

    struct JoinHint {
        std::string table;
        std::string column;
        std::string local_field;
    };

    struct EvalResult {
        std::string json;
        std::vector<JoinHint> join_hints;
    };

    // Evaluate a GraphQL query against a JSON document string.
    // Returns a JSON result string.
    EvalResult evaluate(const std::string &json_string,
                        const std::string &graphql_query,
                        const Variables &variables = {});

private:
    // Parsed GraphQL representation
    struct Argument {
        std::string name;
        std::string value;
    };

    struct Directive {
        std::string name;
        std::vector<Argument> args;
    };

    struct Field {
        std::string alias;
        std::string name;
        std::vector<Argument> arguments;
        std::vector<Directive> directives;
        std::vector<Field> sub_fields;   // selection set
        std::string fragment_name;       // for fragment spreads
    };

    struct Fragment {
        std::string name;
        std::string on_type;
        std::vector<Field> fields;
    };

    struct ParsedQuery {
        std::vector<Field> fields;
        std::vector<Fragment> fragments;
    };

    ParsedQuery parse_query(const std::string &query, const Variables &vars);
    std::vector<Field> parse_selection_set(const std::string &src, size_t &pos);
    Field parse_field(const std::string &src, size_t &pos);
    std::vector<Argument> parse_arguments(const std::string &src, size_t &pos);
    std::vector<Directive> parse_directives(const std::string &src, size_t &pos);
    Fragment parse_fragment(const std::string &src, size_t &pos);
    std::string parse_gql_name(const std::string &src, size_t &pos);
    std::string parse_gql_value(const std::string &src, size_t &pos);
    void skip_ws(const std::string &src, size_t &pos);

    // Resolution
    JsonNodePtr resolve_field(const JsonNodePtr &node, const Field &field,
                              const std::vector<Fragment> &fragments,
                              std::vector<JoinHint> &join_hints);
    JsonNodePtr resolve_selection(const JsonNodePtr &node, const std::vector<Field> &fields,
                                  const std::vector<Fragment> &fragments,
                                  std::vector<JoinHint> &join_hints);
    void collect_join_hints(const Field &field, std::vector<JoinHint> &hints);
};

// ============================================================================
// Document Query Executor
// ============================================================================
// High-level executor that ties the document engines to the catalog/tables.
//
class DocumentQueryExecutor {
public:
    explicit DocumentQueryExecutor(catalog::Catalog &catalog);

    sql::ResultSet exec_xpath(const sql::ast::XPathQueryStmt &stmt);
    sql::ResultSet exec_xquery(const sql::ast::XQueryStmt &stmt);
    sql::ResultSet exec_graphql(const sql::ast::GraphQLQueryStmt &stmt);

private:
    // Read a column of string values from a table, applying an optional WHERE
    // filter by matching row indices.
    struct ColumnData {
        std::vector<std::string> values;
        std::vector<size_t> row_indices;
    };
    ColumnData read_column(const std::string &table_name,
                           const std::string &column_name);

    catalog::Catalog &catalog_;
    XPathEngine xpath_engine_;
    XQueryEngine xquery_engine_;
    GraphQLEngine graphql_engine_;
};

} // namespace tdb::doc

#endif // TDB_DOCQUERY_H
