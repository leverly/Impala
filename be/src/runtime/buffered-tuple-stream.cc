// Copyright 2013 Cloudera Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "runtime/buffered-tuple-stream.h"

#include <boost/bind.hpp>
#include <gutil/strings/substitute.h>

#include "runtime/descriptors.h"
#include "runtime/row-batch.h"
#include "runtime/tuple-row.h"
#include "util/bit-util.h"
#include "util/debug-util.h"

using namespace boost;
using namespace impala;
using namespace std;
using namespace strings;

string BufferedTupleStream::RowIdx::DebugString() const {
  stringstream ss;
  ss << "RowIdx block=" << block() << " offset=" << offset() << " idx=" << idx();
  return ss.str();
}

BufferedTupleStream::BufferedTupleStream(RuntimeState* state,
    const RowDescriptor& row_desc, BufferedBlockMgr* block_mgr,
    BufferedBlockMgr::Client* client, bool delete_on_read, bool read_write)
  : delete_on_read_(delete_on_read),
    read_write_(read_write),
    state_(state),
    desc_(row_desc),
    nullable_tuple_(row_desc.IsAnyTupleNullable()),
    block_mgr_(block_mgr),
    block_mgr_client_(client),
    read_ptr_(NULL),
    read_tuple_idx_(0),
    read_bytes_(0),
    rows_returned_(0),
    read_block_idx_(-1),
    write_block_(NULL),
    num_pinned_(0),
    closed_(false),
    num_rows_(0),
    pinned_(true),
    pin_timer_(NULL),
    unpin_timer_(NULL),
    get_new_block_timer_(NULL) {
  read_block_ = blocks_.end();
  fixed_tuple_row_size_ = 0;
  min_tuple_row_size_ = 0;
  for (int i = 0; i < desc_.tuple_descriptors().size(); ++i) {
    const TupleDescriptor* tuple_desc = desc_.tuple_descriptors()[i];
    const int tuple_byte_size = tuple_desc->byte_size();
    fixed_tuple_row_size_ += tuple_byte_size;
    min_tuple_row_size_ += desc_.TupleIsNullable(i) ? 0 : tuple_byte_size;
    if (tuple_desc->string_slots().empty()) continue;
    string_slots_.push_back(make_pair(i, tuple_desc->string_slots()));
  }

  if (nullable_tuple_) {
    // We assume that all rows will use their max size, so we may be underutilizing the
    // space, i.e. we may have some unused space in case of rows with NULL tuples.
    const uint32_t tuples_per_row = desc_.tuple_descriptors().size();
    const uint32_t min_row_size_in_bits = 8 * fixed_tuple_row_size_ + tuples_per_row;
    const uint32_t block_size_in_bits = 8 * block_mgr_->max_block_size();
    const uint32_t max_num_rows = block_size_in_bits / min_row_size_in_bits;
    null_indicators_per_block_ =
        BitUtil::RoundUpNumi64(max_num_rows * tuples_per_row) * 8;
  } else {
    // If there are no nullable tuples then no need to waste space for null indicators.
    null_indicators_per_block_ = 0;
  }
}

// Returns the number of pinned blocks in the list. Only called in DCHECKs to validate
// num_pinned_.
int NumPinned(const list<BufferedBlockMgr::Block*>& blocks) {
  int num_pinned = 0;
  for (list<BufferedBlockMgr::Block*>::const_iterator it = blocks.begin();
      it != blocks.end(); ++it) {
    if ((*it)->is_pinned()) ++num_pinned;
  }
  return num_pinned;
}

string BufferedTupleStream::DebugString() const {
  stringstream ss;
  ss << "BufferedTupleStream num_rows=" << num_rows_ << " rows_returned="
     << rows_returned_ << " pinned=" << (pinned_ ? "true" : "false")
     << " delete_on_read=" << (delete_on_read_ ? "true" : "false")
     << " num_pinned=" << num_pinned_
     << " write_block=" << write_block_ << " read_block_=";
  if (read_block_ == blocks_.end()) {
    ss << "<end>";
  } else {
    ss << *read_block_;
  }
  ss << " blocks=[\n";
  for (list<BufferedBlockMgr::Block*>::const_iterator it = blocks_.begin();
      it != blocks_.end(); ++it) {
    ss << "{" << (*it)->DebugString() << "}";
    if (*it != blocks_.back()) ss << ",\n";
  }
  ss << "]";
  return ss.str();
}

Status BufferedTupleStream::Init(RuntimeProfile* profile, bool pinned) {
  if (profile != NULL) {
    pin_timer_ = ADD_TIMER(profile, "PinTime");
    unpin_timer_ = ADD_TIMER(profile, "UnpinTime");
    get_new_block_timer_ = ADD_TIMER(profile, "GetNewBlockTime");
  }

  bool got_block = false;
  RETURN_IF_ERROR(NewBlockForWrite(fixed_tuple_row_size_, &got_block));
  if (!got_block) return Status("Not enough memory to initialize BufferedTupleStream.");
  DCHECK(write_block_ != NULL);
  if (read_write_) RETURN_IF_ERROR(PrepareForRead());
  if (!pinned) RETURN_IF_ERROR(UnpinStream());
  return Status::OK;
}

void BufferedTupleStream::Close() {
  for (list<BufferedBlockMgr::Block*>::iterator it = blocks_.begin();
      it != blocks_.end(); ++it) {
    (*it)->Delete();
  }
  blocks_.clear();
  num_pinned_ = 0;
  DCHECK_EQ(num_pinned_, NumPinned(blocks_));
  closed_ = true;
}

int64_t BufferedTupleStream::bytes_in_mem(bool ignore_current) const {
  int num_in_mem_blocks = num_pinned_;
  if (write_block_ != NULL && ignore_current) {
    DCHECK(write_block_->is_pinned());
    --num_in_mem_blocks;
  }
  return num_in_mem_blocks * block_mgr_->max_block_size();
}

int64_t BufferedTupleStream::bytes_unpinned() const {
  return (blocks_.size() - num_pinned_) * block_mgr_->max_block_size();
}

Status BufferedTupleStream::NewBlockForWrite(int min_size, bool* got_block) {
  DCHECK(!closed_);
  if (min_size > block_mgr_->max_block_size()) {
    return Status(Substitute("Cannot process row that is bigger than the IO size "
          "(row_size=$0). To run this query, increase the io size (--read_size option).",
          PrettyPrinter::Print(min_size, TCounterType::BYTES)));
  }

  BufferedBlockMgr::Block* unpin_block = write_block_;
  if (write_block_ != NULL) {
    DCHECK(write_block_->is_pinned());
    if (pinned_ || write_block_ == *read_block_) unpin_block = NULL;
  }

  BufferedBlockMgr::Block* new_block = NULL;
  {
    SCOPED_TIMER(get_new_block_timer_);
    RETURN_IF_ERROR(block_mgr_->GetNewBlock(block_mgr_client_, unpin_block, &new_block));
  }
  *got_block = (new_block != NULL);

  if (!*got_block) {
    DCHECK(unpin_block == NULL);
    return Status::OK;
  }

  if (!pinned_ && write_block_ != NULL) {
    if (!pinned_ && write_block_ != *read_block_) {
      DCHECK(!write_block_->is_pinned());
      --num_pinned_;
      DCHECK_EQ(num_pinned_, NumPinned(blocks_));
    }
  }
  // Allocate the block header with the null indicators
  new_block->Allocate<uint8_t>(null_indicators_per_block_);
  write_tuple_idx_ = 0;

  blocks_.push_back(new_block);
  block_start_idx_.push_back(new_block->buffer());
  write_block_ = new_block;
  DCHECK(write_block_->is_pinned());
  ++num_pinned_;
  DCHECK_EQ(num_pinned_, NumPinned(blocks_));
  return Status::OK;
}

Status BufferedTupleStream::NextBlockForRead() {
  DCHECK(!closed_);
  DCHECK(read_block_ != blocks_.end());
  DCHECK_EQ(num_pinned_, NumPinned(blocks_)) << pinned_;

  // If non-NULL, this will be the current block if we are going to free it while
  // grabbing the next block. This will stay NULL if we don't want to free the
  // current block.
  BufferedBlockMgr::Block* block_to_free =
      (!pinned_ || delete_on_read_) ? *read_block_ : NULL;
  if (delete_on_read_) {
    blocks_.pop_front();
    read_block_ = blocks_.begin();
    read_block_idx_ = 0;
  } else {
    ++read_block_;
    ++read_block_idx_;
  }

  read_ptr_ = NULL;
  read_tuple_idx_ = 0;
  read_bytes_ = 0;

  bool pinned = false;
  if (read_block_ == blocks_.end() || (*read_block_)->is_pinned()) {
    // End of the blocks or already pinned, just handle block_to_free
    if (block_to_free != NULL) {
      SCOPED_TIMER(unpin_timer_);
      if (delete_on_read_) {
        RETURN_IF_ERROR(block_to_free->Delete());
      } else {
        RETURN_IF_ERROR(block_to_free->Unpin());
      }
      --num_pinned_;
    }
  } else {
    // Call into the block mgr to atomically unpin/delete the old block and pin the
    // new block.
    SCOPED_TIMER(pin_timer_);
    RETURN_IF_ERROR((*read_block_)->Pin(&pinned, block_to_free, !delete_on_read_));
    if (!pinned) {
      DCHECK(block_to_free == NULL) << "Should have been able to pin."
          << endl << block_mgr_->DebugString(block_mgr_client_);;
    }
    if (block_to_free == NULL && pinned) ++num_pinned_;
  }

  if (read_block_ != blocks_.end() && (*read_block_)->is_pinned()) {
    read_ptr_ = (*read_block_)->buffer() + null_indicators_per_block_;
  }
  DCHECK_EQ(num_pinned_, NumPinned(blocks_)) << DebugString();
  return Status::OK;
}

Status BufferedTupleStream::PrepareForRead(bool* got_buffer) {
  DCHECK(!closed_);
  if (blocks_.empty()) return Status::OK;

  if (!read_write_ && write_block_ != NULL) {
    DCHECK(write_block_->is_pinned());
    if (!pinned_ && write_block_ != blocks_.front()) {
      SCOPED_TIMER(unpin_timer_);
      write_block_->Unpin();
      --num_pinned_;
    }
    write_block_ = NULL;
    DCHECK_EQ(num_pinned_, NumPinned(blocks_));
  }

  read_block_ = blocks_.begin();
  if (!(*read_block_)->is_pinned()) {
    SCOPED_TIMER(pin_timer_);
    bool current_pinned;
    RETURN_IF_ERROR((*read_block_)->Pin(&current_pinned));
    if (!current_pinned) {
      if (got_buffer == NULL) {
        DCHECK(current_pinned) << "Should have reserved enough blocks." << endl
            << block_mgr_->DebugString(block_mgr_client_);;
        return Status::MEM_LIMIT_EXCEEDED;
      } else {
        *got_buffer = false;
        return Status::OK;
      }
    }
    ++num_pinned_;
    DCHECK_EQ(num_pinned_, NumPinned(blocks_));
  }

  DCHECK(read_block_ != blocks_.end());
  read_ptr_ = (*read_block_)->buffer() + null_indicators_per_block_;
  read_tuple_idx_ = 0;
  read_bytes_ = 0;
  rows_returned_ = 0;
  read_block_idx_ = 0;
  if (got_buffer != NULL) *got_buffer = true;
  return Status::OK;
}

Status BufferedTupleStream::PinStream(bool already_reserved, bool* pinned) {
  DCHECK(!closed_);
  DCHECK_NOTNULL(pinned);
  if (!already_reserved) {
    // If we can't get all the blocks, don't try at all.
    if (!block_mgr_->TryAcquireTmpReservation(block_mgr_client_, blocks_unpinned())) {
      *pinned = false;
      return Status::OK;
    }
  }

  for (list<BufferedBlockMgr::Block*>::iterator it = blocks_.begin();
      it != blocks_.end(); ++it) {
    if ((*it)->is_pinned()) continue;
    {
      SCOPED_TIMER(pin_timer_);
      RETURN_IF_ERROR((*it)->Pin(pinned));
    }
    DCHECK(*pinned) << "Should have been reserved."
        << endl << block_mgr_->DebugString(block_mgr_client_);
    ++num_pinned_;
    DCHECK_EQ(num_pinned_, NumPinned(blocks_));
  }

  if (!delete_on_read_) {
    // Populate block_start_idx_ on pin.
    DCHECK_EQ(block_start_idx_.size(), blocks_.size());
    block_start_idx_.clear();
    for (list<BufferedBlockMgr::Block*>::iterator it = blocks_.begin();
        it != blocks_.end(); ++it) {
      block_start_idx_.push_back((*it)->buffer());
    }
  }
  *pinned = true;
  pinned_ = true;
  return Status::OK;
}

Status BufferedTupleStream::UnpinStream(bool all) {
  DCHECK(!closed_);
  SCOPED_TIMER(unpin_timer_);

  BOOST_FOREACH(BufferedBlockMgr::Block* block, blocks_) {
    if (!block->is_pinned()) continue;
    if (!all && (block == write_block_ || (read_write_ && block == *read_block_))) {
      continue;
    }
    RETURN_IF_ERROR(block->Unpin());
    --num_pinned_;
    DCHECK_EQ(num_pinned_, NumPinned(blocks_));
  }
  if (all) {
    read_block_ = blocks_.end();
    write_block_ = NULL;
  }
  pinned_ = false;
  return Status::OK;
}

Status BufferedTupleStream::GetRows(scoped_ptr<RowBatch>* batch, bool* got_rows) {
  RETURN_IF_ERROR(PinStream(false, got_rows));
  if (!*got_rows) return Status::OK;
  RETURN_IF_ERROR(PrepareForRead());
  batch->reset(
      new RowBatch(desc_, num_rows(), block_mgr_->get_tracker(block_mgr_client_)));
  bool eos = false;
  RETURN_IF_ERROR(GetNext(batch->get(), &eos));
  DCHECK(eos);
  return Status::OK;
}

Status BufferedTupleStream::GetNext(RowBatch* batch, bool* eos,
    vector<RowIdx>* indices) {
  if (nullable_tuple_) {
    return GetNextInternal<true>(batch, eos, indices);
  } else {
    return GetNextInternal<false>(batch, eos, indices);
  }
}

template <bool HasNullableTuple>
Status BufferedTupleStream::GetNextInternal(RowBatch* batch, bool* eos,
    vector<RowIdx>* indices) {
  DCHECK(!closed_);
  DCHECK(batch->row_desc().Equals(desc_));
  DCHECK_EQ(batch->num_rows(), 0);
  *eos = (rows_returned_ == num_rows_);
  if (*eos) return Status::OK;

  int64_t rows_left = num_rows_ - rows_returned_;
  int rows_to_fill = std::min(static_cast<int64_t>(batch->capacity()), rows_left);
  DCHECK_GE(rows_to_fill, 1);
  batch->AddRows(rows_to_fill);
  uint8_t* tuple_row_mem = reinterpret_cast<uint8_t*>(batch->GetRow(0));
  int64_t data_len = (*read_block_)->valid_data_len() - null_indicators_per_block_;
  const uint64_t tuples_per_row = desc_.tuple_descriptors().size();
  if (UNLIKELY(((data_len - read_bytes_) < min_tuple_row_size_) ||
               (HasNullableTuple &&
                ((read_tuple_idx_ + tuples_per_row) > null_indicators_per_block_ * 8)))) {
    // Get the next block in the stream. We need to do this at the beginning of
    // the GetNext() call to ensure the buffer management semantics. NextBlockForRead()
    // will recycle the memory for the rows returned from the *previous* call to
    // GetNext().
    RETURN_IF_ERROR(NextBlockForRead());
    data_len = (*read_block_)->valid_data_len() - null_indicators_per_block_;
    DCHECK_GE(data_len, min_tuple_row_size_);
  }

  DCHECK(read_block_ != blocks_.end());
  DCHECK((*read_block_)->is_pinned());
  DCHECK(read_ptr_ != NULL);

  // Produce tuple rows from the current block and the corresponding position on the
  // null tuple indicator.
  vector<RowIdx> local_indices;
  if (indices == NULL) {
    // A hack so that we do not need to check whether 'indices' is not null in the
    // tight loop.
    indices = &local_indices;
  } else {
    DCHECK(is_pinned());
    DCHECK(!delete_on_read_);
    indices->clear();
  }
  indices->reserve(rows_to_fill);

  int i = 0;
  uint8_t* null_word = NULL;
  uint32_t null_pos = 0;
  // Start reading from position read_tuple_idx_ in the block.
  uint64_t last_read_ptr = 0;
  uint64_t last_read_row = read_tuple_idx_ / tuples_per_row;
  while (i < rows_to_fill) {
    // Check if current block is done.
    if (UNLIKELY(((data_len - read_bytes_) < min_tuple_row_size_) ||
        (HasNullableTuple &&
        ((read_tuple_idx_ + tuples_per_row) > null_indicators_per_block_ * 8)))) {
      break;
    }

    // Copy the row into the output batch.
    TupleRow* row = reinterpret_cast<TupleRow*>(tuple_row_mem);
    last_read_ptr = reinterpret_cast<uint64_t>(read_ptr_);
    indices->push_back(RowIdx());
    DCHECK_EQ(indices->size(), i + 1);
    (*indices)[i].set(read_block_idx_, read_bytes_ + null_indicators_per_block_,
                      last_read_row);
    if (HasNullableTuple) {
      for (int j = 0; j < tuples_per_row; ++j) {
        // Stitch together the tuples from the block and the NULL ones.
        null_word = (*read_block_)->buffer() + (read_tuple_idx_ >> 3);
        null_pos = read_tuple_idx_ & 7;
        ++read_tuple_idx_;
        const bool is_not_null = ((*null_word & (1 << (7 - null_pos))) == 0);
        // Copy tuple and advance read_ptr_. If it it is a NULL tuple, it calls SetTuple
        // with Tuple* being 0x0. To do that we multiply the current read_ptr_ with
        // false (0x0).
        row->SetTuple(j, reinterpret_cast<Tuple*>(
            reinterpret_cast<uint64_t>(read_ptr_) * is_not_null));
        read_ptr_ += desc_.tuple_descriptors()[j]->byte_size() * is_not_null;
      }
      const uint64_t row_read_bytes =
          reinterpret_cast<uint64_t>(read_ptr_) - last_read_ptr;
      DCHECK_GE(fixed_tuple_row_size_, row_read_bytes);
      DCHECK_LE(min_tuple_row_size_, row_read_bytes);
      read_bytes_ += row_read_bytes;
      last_read_ptr = reinterpret_cast<uint64_t>(read_ptr_);
    } else {
      // When we know that there are no nullable tuples we can safely copy them without
      // checking for nullability.
      for (int j = 0; j < tuples_per_row; ++j) {
        row->SetTuple(j, reinterpret_cast<Tuple*>(read_ptr_));
        read_ptr_ += desc_.tuple_descriptors()[j]->byte_size();
      }
      read_bytes_ += fixed_tuple_row_size_;
    }
    tuple_row_mem += sizeof(Tuple*) * tuples_per_row;

    // Update string slot ptrs.
    for (int j = 0; j < string_slots_.size(); ++j) {
      Tuple* tuple = row->GetTuple(string_slots_[j].first);
      if (HasNullableTuple && tuple == NULL) continue;
      DCHECK_NOTNULL(tuple);
      for (int k = 0; k < string_slots_[j].second.size(); ++k) {
        const SlotDescriptor* slot_desc = string_slots_[j].second[k];
        if (tuple->IsNull(slot_desc->null_indicator_offset())) continue;

        StringValue* sv = tuple->GetStringSlot(slot_desc->tuple_offset());
        DCHECK_LE(sv->len, data_len - read_bytes_) << DebugString();
        sv->ptr = reinterpret_cast<char*>(read_ptr_);
        read_ptr_ += sv->len;
        read_bytes_ += sv->len;
      }
    }
    ++last_read_row;
    ++i;
  }

  batch->CommitRows(i);
  rows_returned_ += i;
  *eos = (rows_returned_ == num_rows_);
  if (!pinned_ &&
      (((data_len - read_bytes_) < min_tuple_row_size_) ||
       (HasNullableTuple &&
        ((read_tuple_idx_ + tuples_per_row) > null_indicators_per_block_ * 8)))) {
    // No more data in this block. Mark this batch as needing to return so
    // the caller can pass the rows up the operator tree.
    batch->MarkNeedToReturn();
  }
  DCHECK_EQ(batch->num_rows(), i);
  DCHECK_EQ(indices->size(), i);
  return Status::OK;
}

// TODO: Move this somewhere in general. We don't want this function inlined
// for the buffered tuple stream case though.
// TODO: In case of null-able tuples we ignore the space we could have saved from the
// null tuples of this row.
int BufferedTupleStream::ComputeRowSize(TupleRow* row) const {
  int size = fixed_tuple_row_size_;
  for (int i = 0; i < string_slots_.size(); ++i) {
    Tuple* tuple = row->GetTuple(string_slots_[i].first);
    if (nullable_tuple_ && tuple == NULL) continue;
    DCHECK_NOTNULL(tuple);
    for (int j = 0; j < string_slots_[i].second.size(); ++j) {
      const SlotDescriptor* slot_desc = string_slots_[i].second[j];
      if (tuple->IsNull(slot_desc->null_indicator_offset())) continue;
      StringValue* sv = tuple->GetStringSlot(slot_desc->tuple_offset());
      size += sv->len;
    }
  }
  return size;
}
