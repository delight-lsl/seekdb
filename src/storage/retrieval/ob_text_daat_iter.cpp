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

#define USING_LOG_PREFIX STORAGE

#include "ob_text_daat_iter.h"

namespace oceanbase
{
namespace storage
{
int ObTextDaaTIter::init(const ObTextDaaTParam &param)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(param.base_param_) || OB_ISNULL(param.dim_iters_) || OB_ISNULL(param.allocator_)
      || OB_ISNULL(param.relevance_collector_)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(ObSRDaaTIterImpl::init(*param.base_param_, *param.dim_iters_,
                                            *param.allocator_, *param.relevance_collector_))) {
  } else if (!param.count_match_only_ && OB_FAIL(bm25_param_estimator_.init(param.bm25_param_est_ctx_))) {
  } else {
    mode_flag_ = param.mode_flag_;
    function_lookup_mode_ = param.function_lookup_mode_;
    count_match_only_ = param.count_match_only_;
    count_disjunction_ = param.count_disjunction_;
  }
  return ret;
}

void ObTextDaaTIter::reset()
{
  bm25_param_estimator_.reset();
  // this class can know the type of dim_iters_, but ObSRDaaTIterImpl maybe not
  if (OB_NOT_NULL(dim_iters_)) {
    for (int64_t i = 0; i < dim_iters_->count(); ++i) {
      static_cast<ObTextRetrievalDaaTTokenIter *>(dim_iters_->at(i))->reset();
    }
  }
  ObSRDaaTIterImpl::reset();
}

void ObTextDaaTIter::reuse(const bool switch_tablet)
{
  if (count_match_only_ || (OB_NOT_NULL(dim_iters_) && 0 == dim_iters_->count())) {
    // do nothing
  } else {
    bm25_param_estimator_.reuse(switch_tablet);
  }
  if (OB_NOT_NULL(dim_iters_)) {
    for (int64_t i = 0; i < dim_iters_->count(); ++i) {
      static_cast<ObTextRetrievalDaaTTokenIter *>(dim_iters_->at(i))->reuse();
    }
  }
  ObSRDaaTIterImpl::reuse(switch_tablet);
}

int ObTextDaaTIter::get_next_rows(const int64_t capacity, int64_t &count)
{
  if (IS_INIT && count_match_only_ && count_disjunction_ && !function_lookup_mode_
      && dim_iters_->count() > 0 && dim_iters_->count() <= MAX_COUNT_STREAMS
      && iter_param_->eval_ctx_->is_vectorized() && !iter_param_->limit_param_->is_valid()
      && !iter_param_->need_filter() && OB_ISNULL(iter_param_->dim_weights_)
      && common::ObVarcharType == iter_param_->id_proj_expr_->datum_meta_.type_
      && common::CS_TYPE_BINARY == iter_param_->id_proj_expr_->datum_meta_.cs_type_
      && capacity > 0) {
    return get_next_count_rows(capacity, count);
  }
  return ObSRDaaTIterImpl::get_next_rows(capacity, count);
}

static int compare_count_doc_ids(const ObDatum &left, const ObDatum &right,
                                 const ObCharsetInfo &charset)
{
  if (OB_UNLIKELY(left.is_null() || right.is_null())) {
    return static_cast<int>(right.is_null()) - static_cast<int>(left.is_null());
  }
  const ObString lhs = left.get_string();
  const ObString rhs = right.get_string();
  // Equal lengths use the same unsigned byte order as the binary collation.
  if (OB_LIKELY(lhs.length() == rhs.length())) {
    return lhs.empty() ? 0 : MEMCMP(lhs.ptr(), rhs.ptr(), lhs.length());
  }
  return charset.coll->strnncollsp(&charset,
      reinterpret_cast<const unsigned char *>(lhs.ptr()), lhs.length(),
      reinterpret_cast<const unsigned char *>(rhs.ptr()), rhs.length(), false);
}

int ObTextDaaTIter::get_next_count_rows(const int64_t capacity, int64_t &count)
{
  struct Cursor {
    ObTextRetrievalDaaTTokenIter *iter_ = nullptr;
    const ObDocIdExt *start_ = nullptr;
    const ObDocIdExt *head_ = nullptr;
    const ObDocIdExt *end_ = nullptr;
    bool exhausted_ = false;
  };
  int ret = OB_SUCCESS;
  count = 0;
  const int64_t stream_count = dim_iters_->count();
  const int64_t output_capacity = MIN(capacity, iter_param_->max_batch_size_);
  const ObCharsetInfo &charset = *ObCharset::get_charset(common::CS_TYPE_BINARY);
  Cursor cursors[MAX_COUNT_STREAMS];
  for (int64_t i = 0; OB_SUCC(ret) && i < stream_count; ++i) {
    cursors[i].iter_ = static_cast<ObTextRetrievalDaaTTokenIter *>(dim_iters_->at(i));
    if (OB_ISNULL(cursors[i].iter_)) {
      ret = OB_ERR_UNEXPECTED;
    }
  }

  // Merge the sorted term batches. A view stays valid until its own stream refills.
  while (OB_SUCC(ret) && count < output_capacity) {
    const ObDatum *first = nullptr;
    unsigned int matching = 0;
    for (int64_t i = 0; OB_SUCC(ret) && i < stream_count; ++i) {
      Cursor &cursor = cursors[i];
      if (!cursor.exhausted_ && cursor.head_ == cursor.end_) {
        if (OB_NOT_NULL(cursor.start_)) {
          cursor.iter_->consume_doc_ids(cursor.head_ - cursor.start_);
        }
        if (OB_ITER_END == (ret = cursor.iter_->peek_doc_ids(cursor.head_, cursor.end_))) {
          ret = OB_SUCCESS;
          cursor.exhausted_ = true;
        }
        cursor.start_ = cursor.head_;
      }
      if (OB_SUCC(ret) && !cursor.exhausted_) {
        const ObDatum &id = cursor.head_->get_datum();
        const int cmp = OB_ISNULL(first) ? -1 : compare_count_doc_ids(id, *first, charset);
        if (cmp < 0) {
          first = &id;
          matching = 1U << i;
        } else if (cmp == 0) {
          matching |= 1U << i;
        }
      }
    }
    if (OB_SUCC(ret)) {
      if (OB_ISNULL(first)) {
        ret = OB_ITER_END;
      } else if (OB_FAIL(cache_result(count, *first, 1.0))) {
        LOG_WARN("failed to cache fulltext count match", K(ret));
      } else {
        for (int64_t i = 0; i < stream_count; ++i) {
          if ((matching & (1U << i)) != 0) {
            ++cursors[i].head_;
          }
        }
      }
    }
  }
  for (int64_t i = 0; i < stream_count; ++i) {
    Cursor &cursor = cursors[i];
    if (OB_NOT_NULL(cursor.start_)) {
      cursor.iter_->consume_doc_ids(cursor.head_ - cursor.start_);
    }
  }
  if ((OB_SUCC(ret) || OB_ITER_END == ret) && count > 0 && OB_FAIL(project_results(count))) {
  }
  return ret;
}

int ObTextDaaTIter::pre_process()
{
  int ret = OB_SUCCESS;
  if (dim_iters_->count() == 0) {
    ret = OB_ITER_END;
  } else if (!count_match_only_ && iter_param_->need_project_relevance()) {
    if (OB_FAIL(bm25_param_estimator_.do_estimation(*iter_param_->eval_ctx_))) {
    }
  }
  return ret;
}

ObTextBMWIter::ObTextBMWIter()
  : ObSRBMWIterImpl(),
    bm25_param_estimator_() {}

void ObTextBMWIter::reuse(const bool switch_tablet)
{
  bm25_param_estimator_.reuse(switch_tablet);
  if (OB_NOT_NULL(dim_iters_)) {
    for (int64_t i = 0; i < dim_iters_->count(); ++i) {
      static_cast<ObTextRetrievalBlockMaxIter *>(dim_iters_->at(i))->reuse();
    }
  }
  ObSRBMWIterImpl::reuse(switch_tablet);
}

void ObTextBMWIter::reset()
{
  bm25_param_estimator_.reset();
  if (OB_NOT_NULL(dim_iters_)) {
    for (int64_t i = 0; i < dim_iters_->count(); ++i) {
      static_cast<ObTextRetrievalBlockMaxIter *>(dim_iters_->at(i))->reset();
    }
  }
  ObSRBMWIterImpl::reset();
}

int ObTextBMWIter::init(const ObTextDaaTParam &param)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(param.base_param_) || OB_ISNULL(param.dim_iters_) || OB_ISNULL(param.allocator_)
      || OB_ISNULL(param.relevance_collector_)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(ObSRBMWIterImpl::init(*param.base_param_, *param.dim_iters_,
                                           *param.allocator_, *param.relevance_collector_))) {
  } else if (OB_FAIL(bm25_param_estimator_.init(param.bm25_param_est_ctx_))) {
  }
  return ret;
}

int ObTextBMWIter::get_next_rows(const int64_t capacity, int64_t &count)
{
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (dim_iters_->count() == 0) {
    ret = OB_ITER_END;
  } else if (OB_FAIL(bm25_param_estimator_.do_estimation(*iter_param_->eval_ctx_))) {
  }

  if (FAILEDx(ObSRBMWIterImpl::get_next_rows(capacity, count))) {
    if (OB_UNLIKELY(OB_ITER_END != ret)) {
    }
  }
  return ret;
}

int ObTextBMWIter::init_before_wand_process()
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!bm25_param_estimator_.is_estimated())) {
    ret = OB_ERR_UNEXPECTED;
  }

  for (int64_t i = 0; OB_SUCC(ret) && i < dim_iters_->count(); ++i) {
    ObTextRetrievalBlockMaxIter *block_max_iter = static_cast<ObTextRetrievalBlockMaxIter *>(dim_iters_->at(i));
    if (OB_FAIL(block_max_iter->init_block_max_iter(
        bm25_param_estimator_.get_total_doc_cnt(), bm25_param_estimator_.get_avg_doc_token_cnt()))) {
    }
  }
  return ret;
}

} // namespace storage
} // namespace oceanbase
