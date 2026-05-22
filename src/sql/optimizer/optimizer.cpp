#include "tdb/sql/optimizer.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <sstream>

namespace tdb::sql {

// ─── Constructor ───

Optimizer::Optimizer(const catalog::Catalog &catalog)
    : catalog_(catalog) {}

// ─── Cost estimation ───

double Optimizer::estimate_table_rows(const std::string &table) const {
    const auto *info = catalog_.find_table(table);
    if (!info) return 0.0;
    return static_cast<double>(info->total_row_count());
}

double Optimizer::estimate_selectivity(const ast::Expr &predicate) const {
    switch (predicate.type) {
        case ast::ExprType::BINARY_OP: {
            const auto &bin = std::get<ast::BinaryOp>(predicate.data);

            // AND: multiply child selectivities
            if (bin.op == TokenType::KW_AND) {
                double left_sel = bin.left ? estimate_selectivity(*bin.left) : 1.0;
                double right_sel = bin.right ? estimate_selectivity(*bin.right) : 1.0;
                return left_sel * right_sel;
            }

            // OR: add child selectivities (capped at 1.0)
            if (bin.op == TokenType::KW_OR) {
                double left_sel = bin.left ? estimate_selectivity(*bin.left) : 0.5;
                double right_sel = bin.right ? estimate_selectivity(*bin.right) : 0.5;
                return std::min(left_sel + right_sel, 1.0);
            }

            // EQ on column: 10% selectivity
            if (bin.op == TokenType::EQ) {
                return 0.1;
            }

            // Range predicates (LT, GT, LTE, GTE): 33%
            if (bin.op == TokenType::LT || bin.op == TokenType::GT ||
                bin.op == TokenType::LTE || bin.op == TokenType::GTE) {
                return 0.33;
            }

            // NEQ: 90% (inverse of EQ)
            if (bin.op == TokenType::NEQ) {
                return 0.9;
            }

            return 0.5;
        }

        case ast::ExprType::UNARY_OP: {
            const auto &unary = std::get<ast::UnaryOp>(predicate.data);

            // NOT: 1 - child selectivity
            if (unary.op == TokenType::KW_NOT && unary.operand) {
                return 1.0 - estimate_selectivity(*unary.operand);
            }
            return 0.5;
        }

        case ast::ExprType::LIKE_EXPR: {
            const auto &like = std::get<ast::LikeExpr>(predicate.data);

            // Check if pattern starts with %
            if (like.pattern && like.pattern->type == ast::ExprType::LITERAL) {
                const auto &lit = std::get<ast::Literal>(like.pattern->data);
                if (!lit.value.empty() && lit.value[0] == '%') {
                    // LIKE with leading %: 50% selectivity (no index use possible)
                    return like.negated ? 0.5 : 0.5;
                }
                // LIKE without leading %: 10% selectivity (prefix match)
                return like.negated ? 0.9 : 0.1;
            }
            // Unknown pattern expression, use conservative estimate
            return 0.5;
        }

        case ast::ExprType::IN_EXPR: {
            const auto &in = std::get<ast::InExpr>(predicate.data);
            // For a value list, selectivity = num_values * 0.1 (capped at 0.5)
            if (auto *vals = std::get_if<std::vector<ast::ExprPtr>>(&in.values)) {
                double sel = std::min(static_cast<double>(vals->size()) * 0.1, 0.5);
                return in.negated ? (1.0 - sel) : sel;
            }
            return 0.5;
        }

        case ast::ExprType::BETWEEN_EXPR: {
            // BETWEEN is essentially two range predicates ANDed
            const auto &between = std::get<ast::BetweenExpr>(predicate.data);
            double sel = 0.33 * 0.33; // ~11%
            return between.negated ? (1.0 - sel) : sel;
        }

        case ast::ExprType::IS_NULL_EXPR: {
            const auto &isnull = std::get<ast::IsNullExpr>(predicate.data);
            // IS NULL typically very selective, IS NOT NULL is not
            return isnull.negated ? 0.9 : 0.05;
        }

        default:
            return 0.5;
    }
}

// ─── Plan selection ───

OptimizedPlan::ScanType Optimizer::choose_scan(const std::string &table,
                                                const ast::Expr *where) const {
    if (!where) return OptimizedPlan::ScanType::SEQ_SCAN;

    double sel = estimate_selectivity(*where);

    // Only consider index scan if selectivity is good (< 0.2)
    if (sel >= 0.2) return OptimizedPlan::ScanType::SEQ_SCAN;

    // Try to extract the column being filtered on
    std::string col = extract_filtered_column(*where);
    if (col.empty()) return OptimizedPlan::ScanType::SEQ_SCAN;

    if (has_index_on(table, col)) {
        return OptimizedPlan::ScanType::INDEX_SCAN;
    }

    return OptimizedPlan::ScanType::SEQ_SCAN;
}

OptimizedPlan::JoinMethod Optimizer::choose_join(double left_rows,
                                                  double right_rows,
                                                  bool has_equi_key) const {
    // If either side is small, nested loop is fine
    if (left_rows < 100.0 || right_rows < 100.0) {
        return OptimizedPlan::JoinMethod::NESTED_LOOP;
    }

    // If there's an equi-join key, hash join is usually best
    if (has_equi_key) {
        return OptimizedPlan::JoinMethod::HASH_JOIN;
    }

    // Default: hash join (still better than nested loop for large tables)
    return OptimizedPlan::JoinMethod::HASH_JOIN;
}

// ─── Predicate analysis ───

bool Optimizer::is_equi_join_predicate(const ast::Expr &expr,
                                       std::string &left_col,
                                       std::string &right_col) const {
    if (expr.type != ast::ExprType::BINARY_OP) return false;

    const auto &bin = std::get<ast::BinaryOp>(expr.data);
    if (bin.op != TokenType::EQ) return false;

    if (!bin.left || !bin.right) return false;

    // Both sides must be column references
    if (bin.left->type != ast::ExprType::COLUMN_REF ||
        bin.right->type != ast::ExprType::COLUMN_REF) {
        return false;
    }

    const auto &left_ref = std::get<ast::ColumnRef>(bin.left->data);
    const auto &right_ref = std::get<ast::ColumnRef>(bin.right->data);

    // They must reference different tables
    if (!left_ref.table.has_value() || !right_ref.table.has_value()) {
        // Cannot determine if different tables without qualifiers
        // Try to infer: if column names differ, assume different tables
        if (left_ref.column == right_ref.column &&
            !left_ref.table.has_value() && !right_ref.table.has_value()) {
            return false;
        }
    } else if (left_ref.table.value() == right_ref.table.value()) {
        return false; // Same table, not a join predicate
    }

    // Build qualified column strings: "table.column" or just "column"
    if (left_ref.table.has_value()) {
        left_col = left_ref.table.value() + "." + left_ref.column;
    } else {
        left_col = left_ref.column;
    }

    if (right_ref.table.has_value()) {
        right_col = right_ref.table.value() + "." + right_ref.column;
    } else {
        right_col = right_ref.column;
    }

    return true;
}

bool Optimizer::has_index_on(const std::string &table,
                             const std::string &column) const {
    auto index_names = catalog_.list_indexes();
    for (const auto &idx_name : index_names) {
        const auto *idx = catalog_.find_index(idx_name);
        if (!idx) continue;
        if (idx->table != table) continue;
        // Check if the column is the leading column of the index
        if (!idx->columns.empty() && idx->columns[0] == column) {
            return true;
        }
    }
    return false;
}

// ─── Helpers ───

std::string Optimizer::find_index_name(const std::string &table,
                                       const std::string &column) const {
    auto index_names = catalog_.list_indexes();
    for (const auto &idx_name : index_names) {
        const auto *idx = catalog_.find_index(idx_name);
        if (!idx) continue;
        if (idx->table != table) continue;
        if (!idx->columns.empty() && idx->columns[0] == column) {
            return idx_name;
        }
    }
    return {};
}

void Optimizer::collect_table_names(const ast::TableRef &ref,
                                    std::vector<std::string> &out) const {
    switch (ref.type) {
        case ast::TableRefType::TABLE:
            out.push_back(ref.alias.empty() ? ref.name : ref.alias);
            break;

        case ast::TableRefType::JOIN:
            // The left table is this ref itself (it holds the table name)
            if (!ref.name.empty()) {
                out.push_back(ref.alias.empty() ? ref.name : ref.alias);
            }
            // The right side of the join
            if (ref.join.right) {
                collect_table_names(*ref.join.right, out);
            }
            break;

        case ast::TableRefType::SUBQUERY:
            // Subquery as table source: use alias
            if (!ref.alias.empty()) {
                out.push_back(ref.alias);
            }
            break;

        case ast::TableRefType::LATERAL:
            if (!ref.alias.empty()) {
                out.push_back(ref.alias);
            }
            break;

        case ast::TableRefType::FUNCTION:
            if (!ref.alias.empty()) {
                out.push_back(ref.alias);
            }
            break;
    }
}

void Optimizer::collect_join_conditions(const ast::TableRef &ref,
                                        std::vector<const ast::Expr *> &out) const {
    if (ref.type == ast::TableRefType::JOIN) {
        if (ref.join.on_condition) {
            out.push_back(ref.join.on_condition.get());
        }
        // Recurse into the right side in case of chained joins
        if (ref.join.right) {
            collect_join_conditions(*ref.join.right, out);
        }
    }
}

std::string Optimizer::extract_filtered_column(const ast::Expr &expr) const {
    switch (expr.type) {
        case ast::ExprType::BINARY_OP: {
            const auto &bin = std::get<ast::BinaryOp>(expr.data);

            // For comparison ops, look for a column ref on either side
            if (bin.op == TokenType::EQ || bin.op == TokenType::NEQ ||
                bin.op == TokenType::LT || bin.op == TokenType::GT ||
                bin.op == TokenType::LTE || bin.op == TokenType::GTE) {
                if (bin.left && bin.left->type == ast::ExprType::COLUMN_REF) {
                    return std::get<ast::ColumnRef>(bin.left->data).column;
                }
                if (bin.right && bin.right->type == ast::ExprType::COLUMN_REF) {
                    return std::get<ast::ColumnRef>(bin.right->data).column;
                }
            }

            // For AND, try the left side first, then the right
            if (bin.op == TokenType::KW_AND) {
                if (bin.left) {
                    std::string col = extract_filtered_column(*bin.left);
                    if (!col.empty()) return col;
                }
                if (bin.right) {
                    return extract_filtered_column(*bin.right);
                }
            }

            return {};
        }

        case ast::ExprType::LIKE_EXPR: {
            const auto &like = std::get<ast::LikeExpr>(expr.data);
            if (like.operand && like.operand->type == ast::ExprType::COLUMN_REF) {
                return std::get<ast::ColumnRef>(like.operand->data).column;
            }
            return {};
        }

        case ast::ExprType::IN_EXPR: {
            const auto &in = std::get<ast::InExpr>(expr.data);
            if (in.operand && in.operand->type == ast::ExprType::COLUMN_REF) {
                return std::get<ast::ColumnRef>(in.operand->data).column;
            }
            return {};
        }

        case ast::ExprType::BETWEEN_EXPR: {
            const auto &between = std::get<ast::BetweenExpr>(expr.data);
            if (between.operand && between.operand->type == ast::ExprType::COLUMN_REF) {
                return std::get<ast::ColumnRef>(between.operand->data).column;
            }
            return {};
        }

        case ast::ExprType::IS_NULL_EXPR: {
            const auto &isnull = std::get<ast::IsNullExpr>(expr.data);
            if (isnull.operand && isnull.operand->type == ast::ExprType::COLUMN_REF) {
                return std::get<ast::ColumnRef>(isnull.operand->data).column;
            }
            return {};
        }

        default:
            return {};
    }
}

double Optimizer::compute_plan_cost(const OptimizedPlan &plan) const {
    double total = 0.0;

    for (const auto &tp : plan.table_plans) {
        double rows = estimate_table_rows(tp.table_name);
        if (tp.scan == OptimizedPlan::ScanType::SEQ_SCAN) {
            // Sequential scan: cost proportional to total rows
            total += rows * 1.0;
        } else {
            // Index scan: cost proportional to selected rows + log overhead
            double selected = rows * tp.selectivity;
            total += selected * 1.0 + std::log2(std::max(rows, 1.0)) * 2.0;
        }
    }

    for (const auto &jp : plan.join_plans) {
        double left_rows = estimate_table_rows(jp.left_table);
        double right_rows = estimate_table_rows(jp.right_table);

        // Find selectivities for both sides from table_plans
        double left_sel = 1.0, right_sel = 1.0;
        for (const auto &tp : plan.table_plans) {
            if (tp.table_name == jp.left_table) left_sel = tp.selectivity;
            if (tp.table_name == jp.right_table) right_sel = tp.selectivity;
        }
        double eff_left = left_rows * left_sel;
        double eff_right = right_rows * right_sel;

        switch (jp.method) {
            case OptimizedPlan::JoinMethod::NESTED_LOOP:
                total += eff_left * eff_right;
                break;
            case OptimizedPlan::JoinMethod::HASH_JOIN:
                // Build hash table: O(right), probe: O(left)
                total += eff_right * 1.5 + eff_left * 1.2;
                break;
            case OptimizedPlan::JoinMethod::MERGE_JOIN:
                // Merge: O(left + right) assuming already sorted
                total += eff_left + eff_right;
                break;
        }
    }

    // Sort cost
    if (plan.has_sort) {
        double total_rows = plan.cost.estimated_rows;
        if (total_rows > 1.0) {
            total += total_rows * std::log2(total_rows) * 0.5;
        }
    }

    return total;
}

std::string Optimizer::build_description(const OptimizedPlan &plan) const {
    std::ostringstream ss;
    ss << "Plan: ";

    // Describe table access
    for (size_t i = 0; i < plan.table_plans.size(); ++i) {
        const auto &tp = plan.table_plans[i];
        if (i > 0) ss << " -> ";
        if (tp.scan == OptimizedPlan::ScanType::INDEX_SCAN) {
            ss << "IndexScan(" << tp.table_name
               << " using " << tp.index_name
               << ", sel=" << tp.selectivity << ")";
        } else {
            ss << "SeqScan(" << tp.table_name
               << ", sel=" << tp.selectivity << ")";
        }
    }

    // Describe joins
    for (const auto &jp : plan.join_plans) {
        ss << " -> ";
        switch (jp.method) {
            case OptimizedPlan::JoinMethod::NESTED_LOOP:
                ss << "NestedLoop";
                break;
            case OptimizedPlan::JoinMethod::HASH_JOIN:
                ss << "HashJoin";
                break;
            case OptimizedPlan::JoinMethod::MERGE_JOIN:
                ss << "MergeJoin";
                break;
        }
        ss << "(" << jp.left_table << ", " << jp.right_table;
        if (!jp.join_column.empty()) {
            ss << " on " << jp.join_column;
        }
        ss << ")";
    }

    if (plan.has_aggregation) ss << " -> Aggregate";
    if (plan.has_sort)        ss << " -> Sort";
    if (plan.has_limit)       ss << " -> Limit";

    return ss.str();
}

// ─── Main optimizer entry point ───

OptimizedPlan Optimizer::optimize(const ast::SelectStmt &stmt) {
    OptimizedPlan plan;

    // ── 1. Collect all tables referenced in FROM clause ──
    // We need both the actual table names (for catalog lookups) and aliases.
    // For table plans, use the actual name so catalog lookups work.
    struct TableEntry {
        std::string actual_name; // real table name in catalog
        std::string alias;       // alias (or actual name if no alias)
    };
    std::vector<TableEntry> tables;

    // Also collect join conditions from the FROM clause
    std::vector<const ast::Expr *> join_conditions;

    for (const auto &from_ref : stmt.from) {
        if (!from_ref) continue;

        // Handle the top-level table (or left side of join chain)
        if (from_ref->type == ast::TableRefType::TABLE ||
            from_ref->type == ast::TableRefType::JOIN) {
            if (!from_ref->name.empty()) {
                TableEntry entry;
                entry.actual_name = from_ref->name;
                entry.alias = from_ref->alias.empty()
                    ? from_ref->name : from_ref->alias;
                tables.push_back(entry);
            }
        }

        // Collect join right-hand tables and conditions
        if (from_ref->type == ast::TableRefType::JOIN) {
            collect_join_conditions(*from_ref, join_conditions);

            // Collect the right side table(s) of the join
            if (from_ref->join.right) {
                const auto &rhs = *from_ref->join.right;
                if (!rhs.name.empty()) {
                    TableEntry entry;
                    entry.actual_name = rhs.name;
                    entry.alias = rhs.alias.empty() ? rhs.name : rhs.alias;
                    tables.push_back(entry);
                }
                // If the right side is itself a join, recurse
                if (rhs.type == ast::TableRefType::JOIN) {
                    collect_join_conditions(rhs, join_conditions);
                    if (rhs.join.right && !rhs.join.right->name.empty()) {
                        TableEntry re;
                        re.actual_name = rhs.join.right->name;
                        re.alias = rhs.join.right->alias.empty()
                            ? rhs.join.right->name : rhs.join.right->alias;
                        tables.push_back(re);
                    }
                }
            }
        }
    }

    const ast::Expr *remaining_where = stmt.where_clause.get();

    // ── 2. Build table plans with scan type selection ──
    for (const auto &tbl : tables) {
        OptimizedPlan::TablePlan tp;
        tp.table_name = tbl.actual_name;

        // Determine selectivity from WHERE clause
        if (remaining_where) {
            tp.selectivity = estimate_selectivity(*remaining_where);
        } else {
            tp.selectivity = 1.0;
        }

        // Choose scan type
        tp.scan = choose_scan(tbl.actual_name, remaining_where);

        // If we chose index scan, record which index
        if (tp.scan == OptimizedPlan::ScanType::INDEX_SCAN && remaining_where) {
            std::string col = extract_filtered_column(*remaining_where);
            if (!col.empty()) {
                tp.index_name = find_index_name(tbl.actual_name, col);
            }
        }

        plan.table_plans.push_back(std::move(tp));
    }

    // ── 3. Build join plans ──
    // First, process explicit join conditions from the FROM clause
    for (const auto *join_cond : join_conditions) {
        if (!join_cond) continue;

        std::string left_col, right_col;
        bool is_equi = is_equi_join_predicate(*join_cond, left_col, right_col);

        // Determine the left and right table names from the join columns
        std::string left_table, right_table;
        std::string join_col;

        if (is_equi) {
            // Parse "table.column" format
            auto dot_pos = left_col.find('.');
            if (dot_pos != std::string::npos) {
                left_table = left_col.substr(0, dot_pos);
                join_col = left_col.substr(dot_pos + 1);
            } else {
                // Unqualified: assign to first table
                if (tables.size() >= 1) left_table = tables[0].alias;
                join_col = left_col;
            }

            dot_pos = right_col.find('.');
            if (dot_pos != std::string::npos) {
                right_table = right_col.substr(0, dot_pos);
            } else {
                if (tables.size() >= 2) right_table = tables[1].alias;
            }
        } else {
            // Non-equi join: still need table names
            if (tables.size() >= 2) {
                left_table = tables[0].alias;
                right_table = tables[1].alias;
            }
        }

        // Resolve aliases back to actual table names for row estimation
        std::string left_actual = left_table;
        std::string right_actual = right_table;
        for (const auto &te : tables) {
            if (te.alias == left_table) left_actual = te.actual_name;
            if (te.alias == right_table) right_actual = te.actual_name;
        }

        double left_rows = estimate_table_rows(left_actual);
        double right_rows = estimate_table_rows(right_actual);

        OptimizedPlan::JoinPlan jp;
        jp.left_table = left_actual;
        jp.right_table = right_actual;
        jp.join_column = join_col;
        jp.method = choose_join(left_rows, right_rows, is_equi);

        plan.join_plans.push_back(std::move(jp));
    }

    // Handle implicit joins (comma-separated tables with WHERE join predicates)
    if (tables.size() > 1 && join_conditions.empty() && remaining_where) {
        // Walk the WHERE clause looking for equi-join predicates between tables
        // For simplicity, handle AND-connected predicates at the top level
        std::vector<const ast::Expr *> predicates;
        std::function<void(const ast::Expr *)> split_and = [&](const ast::Expr *e) {
            if (!e) return;
            if (e->type == ast::ExprType::BINARY_OP) {
                const auto &bin = std::get<ast::BinaryOp>(e->data);
                if (bin.op == TokenType::KW_AND) {
                    split_and(bin.left.get());
                    split_and(bin.right.get());
                    return;
                }
            }
            predicates.push_back(e);
        };
        split_and(remaining_where);

        // Track which table pairs already have a join plan
        // (use a simple set of "left<right" strings)
        std::vector<std::string> joined_pairs;

        for (const auto *pred : predicates) {
            if (!pred) continue;
            std::string left_col, right_col;
            if (!is_equi_join_predicate(*pred, left_col, right_col)) continue;

            std::string left_table, right_table;
            std::string join_col;

            auto dot_pos = left_col.find('.');
            if (dot_pos != std::string::npos) {
                left_table = left_col.substr(0, dot_pos);
                join_col = left_col.substr(dot_pos + 1);
            } else {
                continue; // Can't determine tables for implicit join
            }

            dot_pos = right_col.find('.');
            if (dot_pos != std::string::npos) {
                right_table = right_col.substr(0, dot_pos);
            } else {
                continue;
            }

            // Normalize pair ordering
            std::string pair_key = (left_table < right_table)
                ? left_table + "<" + right_table
                : right_table + "<" + left_table;

            if (std::find(joined_pairs.begin(), joined_pairs.end(), pair_key)
                    != joined_pairs.end()) {
                continue; // Already have a join plan for this pair
            }
            joined_pairs.push_back(pair_key);

            // Resolve aliases to actual names
            std::string left_actual = left_table;
            std::string right_actual = right_table;
            for (const auto &te : tables) {
                if (te.alias == left_table) left_actual = te.actual_name;
                if (te.alias == right_table) right_actual = te.actual_name;
            }

            double left_rows = estimate_table_rows(left_actual);
            double right_rows = estimate_table_rows(right_actual);

            OptimizedPlan::JoinPlan jp;
            jp.left_table = left_actual;
            jp.right_table = right_actual;
            jp.join_column = join_col;
            jp.method = choose_join(left_rows, right_rows, true);

            plan.join_plans.push_back(std::move(jp));
        }
    }

    // ── 4. Check for aggregation ──
    // GROUP BY or aggregate functions in SELECT list
    if (!stmt.group_by.empty()) {
        plan.has_aggregation = true;
    } else {
        // Check select list for aggregate calls
        for (const auto &sel_expr : stmt.select_list) {
            if (sel_expr && sel_expr->type == ast::ExprType::AGGREGATE_CALL) {
                plan.has_aggregation = true;
                break;
            }
        }
    }

    // ── 5. Check for sort ──
    if (!stmt.order_by.empty()) {
        plan.has_sort = true;
    }

    // ── 6. Check for limit ──
    if (stmt.limit) {
        plan.has_limit = true;
    }

    // ── 7. Estimate total output rows ──
    double estimated_rows = 1.0;
    if (!tables.empty()) {
        // Start with first table
        estimated_rows = estimate_table_rows(tables[0].actual_name);
        // Apply WHERE selectivity
        if (remaining_where) {
            estimated_rows *= estimate_selectivity(*remaining_where);
        }
        // For joins, multiply by join selectivity (heuristic: 0.1 per equi-join)
        for (const auto &jp : plan.join_plans) {
            double right_rows = estimate_table_rows(jp.right_table);
            if (!jp.join_column.empty()) {
                // Equi-join: assume 10% match rate
                estimated_rows *= right_rows * 0.1;
            } else {
                // Cross join
                estimated_rows *= right_rows;
            }
        }
    } else {
        // No FROM clause: single row (e.g., SELECT 1+1)
        estimated_rows = 1.0;
    }

    // Aggregation reduces rows
    if (plan.has_aggregation) {
        if (!stmt.group_by.empty()) {
            // Estimate distinct groups: heuristic sqrt(rows)
            estimated_rows = std::max(std::sqrt(estimated_rows), 1.0);
        } else {
            // Scalar aggregation: exactly one row
            estimated_rows = 1.0;
        }
    }

    // Limit caps the rows
    if (plan.has_limit && stmt.limit &&
        stmt.limit->type == ast::ExprType::LITERAL) {
        const auto &lit = std::get<ast::Literal>(stmt.limit->data);
        if (lit.token_type == TokenType::INTEGER_LITERAL) {
            double limit_val = std::stod(lit.value);
            estimated_rows = std::min(estimated_rows, limit_val);
        }
    }

    plan.cost.estimated_rows = std::max(estimated_rows, 0.0);

    // ── 8. Compute overall cost ──
    plan.cost.estimated_cost = compute_plan_cost(plan);

    // ── 9. Build human-readable description ──
    plan.cost.description = build_description(plan);

    return plan;
}

} // namespace tdb::sql
