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

#define USING_LOG_PREFIX SQL_ENG
#include "ob_expr_ai_embed.h"
#include "sql/engine/expr/ob_expr_lob_utils.h"
#include "query/ai/ob_ai_endpoint_resolver.h"
#include "share/rc/ob_server_runtime.h"

using namespace oceanbase::common;
using namespace oceanbase::sql;

namespace oceanbase 
{
namespace sql 
{
ObExprAIEmbed::ObExprAIEmbed(common::ObIAllocator &alloc)
    : ObFuncExprOperator(alloc, 
                        T_FUN_SYS_AI_EMBED, 
                        N_AI_EMBED, 
                        MORE_THAN_ZERO,
                        NOT_VALID_FOR_GENERATED_COL, 
                        NOT_ROW_DIMENSION) 
{
}

ObExprAIEmbed::~ObExprAIEmbed() 
{
}

int ObExprAIEmbed::calc_result_typeN(ObExprResType &type,
                                     ObExprResType *types_stack,
                                     int64_t param_num,
                                     common::ObExprTypeCtx &type_ctx) const 
{
  UNUSED(type_ctx);
  UNUSED(types_stack);
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(param_num > 3 || param_num < 2)) {
    ObString func_name_(get_name());
    ret = OB_ERR_PARAM_SIZE;
    LOG_USER_ERROR(OB_ERR_PARAM_SIZE, func_name_.length(), func_name_.ptr());
  } else {
    types_stack[MODEL_IDX].set_calc_type(ObVarcharType);
    types_stack[MODEL_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    if (!ob_is_text_tc(types_stack[CONTENT_IDX].get_type())) {
      types_stack[CONTENT_IDX].set_calc_type(ObVarcharType);
    }
    types_stack[CONTENT_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    if (param_num == 3) {
      if (ob_is_integer_type(types_stack[DIM_IDX].get_type())
          || ObNullType == types_stack[DIM_IDX].get_type()) {
        types_stack[DIM_IDX].set_calc_type(ObIntType);
        types_stack[DIM_IDX].set_precision(10);
        types_stack[DIM_IDX].set_scale(0);
      } else {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be an integer, not a decimal or float");
      }
    }
    type.set_varchar();
    type.set_collation_type(CS_TYPE_UTF8MB4_BIN);
    type.set_collation_level(CS_LEVEL_COERCIBLE);
  }
  return ret;
}

int ObExprAIEmbed::eval_ai_embed(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &res) 
{
  INIT_SUCC(ret);
  ObDatum *arg_model_id = nullptr;
  ObDatum *arg_content = nullptr;
  ObDatum *arg_dim = nullptr;
  if (expr.arg_cnt_ == 3 ? OB_FAIL(expr.eval_param_value(ctx, arg_model_id, arg_content, arg_dim))
                         : OB_FAIL(expr.eval_param_value(ctx, arg_model_id, arg_content))) {
  } else if (arg_model_id->is_null() || arg_content->is_null()) {
    res.set_null();
  } else {
    ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
    
    MultimodeAlloctor temp_allocator(tmp_alloc_g.get_allocator());
    lib::ObMallocHookAttrGuard malloc_guard(lib::ObMemAttr(N_AI_EMBED));
    ObString model_id = arg_model_id->get_string();
    ObString content;
    ObArray<ObString> contents;
    ObArray<ObString> results;
    int64_t dimension = 0;
    if (OB_FAIL(ObTextStringHelper::read_real_string_data(
            ctx.exec_ctx_, temp_allocator, *arg_content, expr.args_[CONTENT_IDX]->datum_meta_,
            expr.args_[CONTENT_IDX]->obj_meta_.has_lob_header(), content))) {
    } else if (model_id.empty() || content.empty()) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id or input is empty");
      res.set_null();
    } else if (OB_NOT_NULL(arg_dim)) {
      if (arg_dim->is_null() || (dimension = arg_dim->get_int()) <= 0) {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be a positive integer");
        res.set_null();
      }
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(contents.push_back(content))) {
    } else if (OB_FAIL(request_embeddings(temp_allocator, model_id, contents, dimension, results))) {
    } else if (results.count() != 1) {
      ret = OB_INVALID_DATA;
    } else if (OB_FAIL(ObAIFuncUtils::set_string_result(expr, ctx, res, results.at(0)))) {
    }
  }
  return ret;
}

int ObExprAIEmbed::request_embeddings(ObIAllocator &allocator, ObString &model_id,
                                      ObArray<ObString> &contents, const int64_t dimension,
                                      ObArray<ObString> &results)
{
  int ret = OB_SUCCESS;
  ObAIFuncExprInfo *info = nullptr;
  share::ObAiModelEndpointInfo endpoint_info;
  query::ObIAiEndpointResolver *endpoint_resolver =
      ::oceanbase::share::server_service<::oceanbase::query::ObIAiEndpointResolver>();
  ObJsonObject *config = nullptr;
  ObJsonInt *dimension_json = nullptr;
  if (dimension > 0) {
    if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(allocator, config))) {
    } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_int(allocator, dimension, dimension_json))) {
    } else if (OB_FAIL(config->add("dimensions", dimension_json))) {
    }
  }
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(allocator, model_id, info))) {
  } else if (OB_ISNULL(info) || OB_ISNULL(endpoint_resolver)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(endpoint_resolver->resolve_by_model_name(model_id, allocator, endpoint_info))) {
  } else {
    ObAIFuncModel model(allocator, *info, endpoint_info);
    ret = model.call_dense_embedding_vector_v2(contents, config, results);
  }
  return ret;
}

int ObExprAIEmbed::eval_ai_embed_batch(const ObExpr &expr, ObEvalCtx &ctx,
                                       const ObBitVector &skip, const int64_t batch_size)
{
  int ret = OB_SUCCESS;
  ObBitVector &eval_flags = expr.get_evaluated_flags(ctx);
  bool has_rows = false;
  for (int64_t i = 0; !has_rows && i < batch_size; ++i) {
    has_rows = !skip.at(i) && !eval_flags.at(i);
  }
  if (has_rows) {
    ObDatumVector models;
    ObDatumVector inputs;
    ObDatumVector dimensions;
    ObDatumVector res = expr.locate_expr_datumvector(ctx);
    ObEvalCtx::BatchInfoScopeGuard batch_guard(ctx);
    batch_guard.set_batch_size(batch_size);
    if (expr.arg_cnt_ == 3
        ? OB_FAIL(expr.eval_batch_param_value(ctx, skip, batch_size, models, inputs, dimensions))
        : OB_FAIL(expr.eval_batch_param_value(ctx, skip, batch_size, models, inputs))) {
    } else {
      ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
      MultimodeAlloctor allocator(tmp_alloc_g.get_allocator());
      lib::ObMallocHookAttrGuard malloc_guard(lib::ObMemAttr(N_AI_EMBED));
      ObArray<ObString> model_ids;
      ObArray<ObString> contents;
      ObArray<int64_t> requested_dimensions;
      ObArray<int64_t> row_indices;
      for (int64_t i = 0; OB_SUCC(ret) && i < batch_size; ++i) {
        if (skip.at(i) || eval_flags.at(i)) {
          continue;
        } else if (models.at(i)->is_null() || inputs.at(i)->is_null()) {
          res.at(i)->set_null();
          eval_flags.set(i);
          expr.get_eval_info(ctx).notnull_ = false;
        } else {
          ObString model_id = models.at(i)->get_string();
          ObString content;
          int64_t dimension = 0;
          batch_guard.set_batch_idx(i);
          if (OB_FAIL(ObTextStringHelper::read_real_string_data(
                  ctx.exec_ctx_, allocator, *inputs.at(i), expr.args_[CONTENT_IDX]->datum_meta_,
                  expr.args_[CONTENT_IDX]->obj_meta_.has_lob_header(), content))) {
          } else if (model_id.empty() || content.empty()) {
            ret = OB_INVALID_ARGUMENT;
            LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id or input is empty");
          } else if (expr.arg_cnt_ == 3
                     && (dimensions.at(i)->is_null() || (dimension = dimensions.at(i)->get_int()) <= 0)) {
            ret = OB_INVALID_ARGUMENT;
            LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be a positive integer");
          } else if (OB_FAIL(model_ids.push_back(model_id))) {
          } else if (OB_FAIL(contents.push_back(content))) {
          } else if (OB_FAIL(requested_dimensions.push_back(dimension))) {
          } else if (OB_FAIL(row_indices.push_back(i))) {
          }
        }
      }
      for (int64_t i = 0; OB_SUCC(ret) && i < row_indices.count(); ++i) {
        if (eval_flags.at(row_indices.at(i))) {
          continue;
        }
        ObArray<ObString> batch_contents;
        ObArray<ObString> results;
        ObArray<int64_t> batch_rows;
        for (int64_t j = i; OB_SUCC(ret) && j < row_indices.count(); ++j) {
          if (!eval_flags.at(row_indices.at(j)) && model_ids.at(i) == model_ids.at(j)
              && requested_dimensions.at(i) == requested_dimensions.at(j)) {
            if (OB_FAIL(batch_contents.push_back(contents.at(j)))) {
            } else if (OB_FAIL(batch_rows.push_back(row_indices.at(j)))) {
            }
          }
        }
        if (OB_FAIL(ret)) {
        } else if (OB_FAIL(request_embeddings(allocator, model_ids.at(i), batch_contents,
                                             requested_dimensions.at(i), results))) {
        } else if (results.count() != batch_rows.count()) {
          ret = OB_INVALID_DATA;
        } else {
          for (int64_t j = 0; OB_SUCC(ret) && j < batch_rows.count(); ++j) {
            const int64_t row = batch_rows.at(j);
            batch_guard.set_batch_idx(row);
            if (OB_FAIL(ObAIFuncUtils::set_string_result(expr, ctx, *res.at(row), results.at(j)))) {
            } else {
              eval_flags.set(row);
            }
          }
        }
      }
    }
  }
  return ret;
}

int ObExprAIEmbed::cg_expr(ObExprCGCtx &expr_cg_ctx, 
                           const ObRawExpr &raw_expr,
                           ObExpr &rt_expr) const 
{
  int ret = OB_SUCCESS;
  // TODO: support schema version match in plan cache for ai func
  // const ObRawExpr *model_key = raw_expr.get_param_expr(0);
  // if (OB_NOT_NULL(model_key)
  //     && (model_key->is_static_scalar_const_expr() || model_key->is_const_expr())
  //     && model_key->get_expr_type() != T_OP_GET_USER_VAR &&
  //     OB_NOT_NULL(expr_cg_ctx.schema_guard_)) {
  //   ObIAllocator *allocator = expr_cg_ctx.allocator_;
  //   ObExecContext *exec_ctx = expr_cg_ctx.session_->get_cur_exec_ctx();
  //   ObObj const_data;
  //   bool got_data = false;
  //   ObAIFuncExprInfo *info = nullptr;
  //   if (OB_ISNULL(allocator)) {
  //     ret = OB_ERR_UNEXPECTED;
  //     LOG_WARN("allocator is null", K(ret));
  //   } else if (OB_FAIL(ObSQLUtils::calc_const_or_calculable_expr(exec_ctx,
  //                                                         model_key,
  //                                                         const_data,
  //                                                         got_data,
  //                                                         *allocator))) {
  //     LOG_WARN("failed to calc offset expr", K(ret));
  //   } else if (!got_data || const_data.is_null()) {
  //   } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(*allocator, const_data.get_string(), *expr_cg_ctx.schema_guard_, info))) {
  //     LOG_WARN("failed to get ai func info", K(ret), K(const_data.get_string()));
  //   } else {
  //     rt_expr.extra_info_ = info;
  //   }
  // }

  if (OB_SUCC(ret)) {
    rt_expr.eval_func_ = ObExprAIEmbed::eval_ai_embed;
    rt_expr.eval_batch_func_ = ObExprAIEmbed::eval_ai_embed_batch;
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
