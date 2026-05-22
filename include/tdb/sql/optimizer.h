#ifndef TDB_SQL_OPTIMIZER_H
#define TDB_SQL_OPTIMIZER_H

#include "tdb/sql/ast.h"
#include "tdb/catalog.h"
#include <string>
#include <vector>

namespace tdb::sql {

struct PlanCost {
    double estimated_rows = 0;
    double estimated_cost = 0; // arbitrary units
    std::string description;
};

struct OptimizedPlan {
    PlanCost cost;
    // Plan is described textually -- actual PlanNode creation is in the executor
    enum class ScanType { SEQ_SCAN, INDEX_SCAN };
    enum class JoinMethod { NESTED_LOOP, HASH_JOIN, MERGE_JOIN };

    struct TablePlan {
        std::string table_name;
        ScanType scan = ScanType::SEQ_SCAN;
        std::string index_name; // if INDEX_SCAN
        double selectivity = 1.0;
    };

    struct JoinPlan {
        JoinMethod method = JoinMethod::NESTED_LOOP;
        std::string left_table;
        std::string right_table;
        std::string join_column;
    };

    std::vector<TablePlan> table_plans;
    std::vector<JoinPlan> join_plans;
    bool has_aggregation = false;
    bool has_sort = false;
    bool has_limit = false;
};

class Optimizer {
public:
    explicit Optimizer(const catalog::Catalog &catalog);

    OptimizedPlan optimize(const ast::SelectStmt &stmt);

private:
    const catalog::Catalog &catalog_;

    // Cost estimation
    double estimate_table_rows(const std::string &table) const;
    double estimate_selectivity(const ast::Expr &predicate) const;

    // Plan selection
    OptimizedPlan::ScanType choose_scan(const std::string &table,
                                        const ast::Expr *where) const;
    OptimizedPlan::JoinMethod choose_join(double left_rows, double right_rows,
                                          bool has_equi_key) const;

    // Predicate analysis
    bool is_equi_join_predicate(const ast::Expr &expr,
                                std::string &left_col,
                                std::string &right_col) const;
    bool has_index_on(const std::string &table, const std::string &column) const;

    // Helpers
    std::string find_index_name(const std::string &table,
                                const std::string &column) const;
    void collect_table_names(const ast::TableRef &ref,
                             std::vector<std::string> &out) const;
    void collect_join_conditions(const ast::TableRef &ref,
                                 std::vector<const ast::Expr *> &out) const;
    double compute_plan_cost(const OptimizedPlan &plan) const;
    std::string build_description(const OptimizedPlan &plan) const;
    std::string extract_filtered_column(const ast::Expr &expr) const;
};

} // namespace tdb::sql

#endif // TDB_SQL_OPTIMIZER_H
