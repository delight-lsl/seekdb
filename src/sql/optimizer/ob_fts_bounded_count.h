/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include "sql/resolver/ddl/ob_explain_stmt.h"
#include "sql/resolver/dml/ob_select_stmt.h"

namespace oceanbase
{
namespace sql
{

inline bool is_fts_bounded_count(const ObDMLStmt *root, const ObDMLStmt *scan_stmt)
{
  if (nullptr != root && root->is_explain_stmt()) {
    root = static_cast<const ObExplainStmt *>(root)->get_explain_query_stmt();
  }
  if (nullptr == root || !root->is_select_stmt()
      || nullptr == scan_stmt || !scan_stmt->is_select_stmt()) {
    return false;
  }
  const ObSelectStmt &outer = *static_cast<const ObSelectStmt *>(root);
  const ObSelectStmt &inner = *static_cast<const ObSelectStmt *>(scan_stmt);
  if (!outer.is_single_table_stmt() || 1 != outer.get_from_item_size()
      || !outer.is_scala_group_by() || 1 != outer.get_aggr_items().count()
      || 1 != outer.get_select_item_size() || 0 != outer.get_condition_size()
      || outer.has_select_into() || outer.has_having() || outer.has_order_by()
      || outer.has_limit() || outer.has_distinct() || outer.is_set_stmt()
      || outer.has_window_function() || outer.has_for_update()
      || outer.is_calc_found_rows() || !outer.get_subquery_exprs().empty()
      || nullptr == outer.get_query_ctx()
      || outer.get_query_ctx()->get_query_hint().has_hint_exclude_concurrent()) {
    return false;
  }
  const TableItem *view = outer.get_table_item(0);
  const ObAggFunRawExpr *count = outer.get_aggr_items().at(0);
  if (nullptr == view || !view->is_generated_table() || view->is_view_table_
      || view->ref_query_ != &inner || nullptr == count || count != outer.get_select_item(0).expr_
      || T_FUN_COUNT != count->get_expr_type() || 0 != count->get_real_param_count()
      || count->is_param_distinct()) {
    return false;
  }
  if (!inner.is_single_table_stmt() || 1 != inner.get_from_item_size()
      || 1 != inner.get_condition_size() || 1 != inner.get_match_exprs().count()
      || 1 != inner.get_select_item_size() || inner.has_order_by() || inner.has_group_by()
      || inner.has_select_into() || inner.has_having() || !inner.get_aggr_items().empty() || inner.has_distinct()
      || inner.is_set_stmt() || inner.has_window_function() || inner.has_for_update()
      || inner.is_calc_found_rows() || inner.is_fetch_with_ties()
      || nullptr != inner.get_limit_percent_expr() || !inner.get_subquery_exprs().empty()) {
    return false;
  }
  const TableItem *table = inner.get_table_item(0);
  const ObRawExpr *limit = inner.get_limit_expr();
  const ObRawExpr *offset = inner.get_offset_expr();
  const ObRawExpr *projection = inner.get_select_item(0).expr_;
  const ObMatchFunRawExpr *match = inner.get_match_exprs().at(0);
  const ObRawExpr *filter = inner.get_condition_expr(0);
  return nullptr != table && table->is_basic_table() && !table->is_view_table_
      && nullptr == table->snapshot_query_expr_ && nullptr == table->sample_info_
      && nullptr != limit && limit->is_const_raw_expr()
      && (nullptr == offset || offset->is_const_raw_expr())
      && nullptr != projection
      && (projection->is_const_raw_expr() ||
          (projection->is_column_ref_expr()
           && static_cast<const ObColumnRefRawExpr *>(projection)->get_table_id() == table->table_id_
           && !static_cast<const ObColumnRefRawExpr *>(projection)->is_generated_column()))
      && nullptr != match && T_FUN_MATCH_AGAINST == match->get_expr_type()
      && NATURAL_LANGUAGE_MODE == match->get_mode_flag() && match->get_columns_boosts().empty()
      && nullptr != match->get_search_key() && match->get_search_key()->is_const_raw_expr()
      && nullptr != filter && T_OP_BOOL == filter->get_expr_type()
      && 1 == filter->get_param_count() && match == filter->get_param_expr(0);
}

} // namespace sql
} // namespace oceanbase
