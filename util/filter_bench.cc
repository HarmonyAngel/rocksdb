//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#if !defined(GFLAGS) || defined(ROCKSDB_LITE)
#include <cstdio>
int main() {
  fprintf(stderr, "filter_bench requires gflags and !ROCKSDB_LITE\n");
  return 1;
}
#else

#include <cinttypes>
#include <iostream>
#include <sstream>
#include <vector>

#include "memory/arena.h"
#include "port/port.h"
#include "port/stack_trace.h"
#include "table/block_based/filter_policy_internal.h"
#include "table/block_based/full_filter_block.h"
#include "table/block_based/mock_block_based_table.h"
#include "table/plain/plain_table_bloom.h"
#include "util/gflags_compat.h"
#include "util/hash.h"
#include "util/random.h"
#include "util/stop_watch.h"

using GFLAGS_NAMESPACE::ParseCommandLineFlags;
using GFLAGS_NAMESPACE::RegisterFlagValidator;
using GFLAGS_NAMESPACE::SetUsageMessage;

DEFINE_uint32(seed, 0, "Seed for random number generators");

DEFINE_double(working_mem_size_mb, 200,
              "MB of memory to get up to among all filters");

DEFINE_uint32(average_keys_per_filter, 10000,
              "Average number of keys per filter");

DEFINE_uint32(key_size, 24, "Average number of bytes for each key");

DEFINE_bool(vary_key_alignment, true,
            "Whether to vary key alignment (default: at least 32-bit "
            "alignment)");

DEFINE_uint32(vary_key_size_log2_interval, 5,
              "Use same key size 2^n times, then change. Key size varies from "
              "-2 to +2 bytes vs. average, unless n>=30 to fix key size.");

DEFINE_uint32(batch_size, 8, "Number of keys to group in each batch");

DEFINE_uint32(bits_per_key, 10, "Bits per key setting for filters");

DEFINE_double(m_queries, 200, "Millions of queries for each test mode");

DEFINE_bool(use_full_block_reader, false,
            "Use FullFilterBlockReader interface rather than FilterBitsReader");

DEFINE_bool(use_plain_table_bloom, false,
            "Use PlainTableBloom structure and interface rather than "
            "FilterBitsReader/FullFilterBlockReader");

DEFINE_uint32(impl, 0,
              "Select filter implementation. Without -use_plain_table_bloom:"
              "0 = full filter, 1 = block-based filter. With "
              "-use_plain_table_bloom: 0 = no locality, 1 = locality.");

DEFINE_bool(net_includes_hashing, false,
            "Whether query net ns/op times should include hashing. "
            "(if not, dry run will include hashing) "
            "(build times always include hashing)");

DEFINE_bool(quick, false, "Run more limited set of tests, fewer queries");

DEFINE_bool(best_case, false, "Run limited tests only for best-case");

DEFINE_bool(allow_bad_fp_rate, false, "Continue even if FP rate is bad");

DEFINE_bool(legend, false,
            "Print more information about interpreting results instead of "
            "running tests");

void _always_assert_fail(int line, const char *file, const char *expr) {
  fprintf(stderr, "%s: %d: Assertion %s failed\n", file, line, expr);
  abort();
}

#define ALWAYS_ASSERT(cond) \
  ((cond) ? (void)0 : ::_always_assert_fail(__LINE__, __FILE__, #cond))

using rocksdb::Arena;
using rocksdb::BlockContents;
using rocksdb::BloomFilterPolicy;
using rocksdb::BloomHash;
using rocksdb::CachableEntry;
using rocksdb::EncodeFixed32;
using rocksdb::fastrange32;
using rocksdb::FilterBitsBuilder;
using rocksdb::FilterBitsReader;
using rocksdb::FilterBuildingContext;
using rocksdb::FullFilterBlockReader;
using rocksdb::GetSliceHash;
using rocksdb::GetSliceHash64;
using rocksdb::Lower32of64;
using rocksdb::ParsedFullFilterBlock;
using rocksdb::PlainTableBloomV1;
using rocksdb::Random32;
using rocksdb::Slice;
using rocksdb::mock::MockBlockBasedTableTester;

struct KeyMaker {
  KeyMaker(size_t avg_size)
      : smallest_size_(avg_size -
                       (FLAGS_vary_key_size_log2_interval >= 30 ? 2 : 0)),
        buf_size_(avg_size + 11),  // pad to vary key size and alignment
        buf_(new char[buf_size_]) {
    memset(buf_.get(), 0, buf_size_);
    assert(smallest_size_ > 8);
  }
  size_t smallest_size_;
  size_t buf_size_;
  std::unique_ptr<char[]> buf_;

  // Returns a unique(-ish) key based on the given parameter values. Each
  // call returns a Slice from the same buffer so previously returned
  // Slices should be considered invalidated.
  Slice Get(uint32_t filter_num, uint32_t val_num) {
    size_t start = FLAGS_vary_key_alignment ? val_num % 4 : 0;
    size_t len = smallest_size_;
    if (FLAGS_vary_key_size_log2_interval < 30) {
      // To get range [avg_size - 2, avg_size + 2]
      // use range [smallest_size, smallest_size + 4]
      len += fastrange32(
          (val_num >> FLAGS_vary_key_size_log2_interval) * 1234567891, 5);
    }
    char * data = buf_.get() + start;
    // Populate key data such that all data makes it into a key of at
    // least 8 bytes. We also don't want all the within-filter key
    // variance confined to a contiguous 32 bits, because then a 32 bit
    // hash function can "cheat" the false positive rate by
    // approximating a perfect hash.
    EncodeFixed32(data, val_num);
    EncodeFixed32(data + 4, filter_num + val_num);
    // ensure clearing leftovers from different alignment
    EncodeFixed32(data + 8, 0);
    return Slice(data, len);
  }
};

void PrintWarnings() {
#if defined(__GNUC__) && !defined(__OPTIMIZE__)
  fprintf(stdout,
          "WARNING: Optimization is disabled: benchmarks unnecessarily slow\n");
#endif
#ifndef NDEBUG
  fprintf(stdout,
          "WARNING: Assertions are enabled; benchmarks unnecessarily slow\n");
#endif
}

struct FilterInfo {
  uint32_t filter_id_ = 0;
  std::unique_ptr<const char[]> owner_;
  Slice filter_;
  uint32_t keys_added_ = 0;
  std::unique_ptr<FilterBitsReader> reader_;
  std::unique_ptr<FullFilterBlockReader> full_block_reader_;
  std::unique_ptr<PlainTableBloomV1> plain_table_bloom_;
  uint64_t outside_queries_ = 0;
  uint64_t false_positives_ = 0;
};

enum TestMode {
  kSingleFilter,
  kBatchPrepared,
  kBatchUnprepared,
  kFiftyOneFilter,
  kEightyTwentyFilter,
  kRandomFilter,
};

static const std::vector<TestMode> allTestModes = {
    kSingleFilter,   kBatchPrepared,      kBatchUnprepared,
    kFiftyOneFilter, kEightyTwentyFilter, kRandomFilter,
};

static const std::vector<TestMode> quickTestModes = {
    kSingleFilter,
    kRandomFilter,
};

static const std::vector<TestMode> bestCaseTestModes = {
    kSingleFilter,
};

const char *TestModeToString(TestMode tm) {
  switch (tm) {
    case kSingleFilter:
      return "Single filter";
    case kBatchPrepared:
      return "Batched, prepared";
    case kBatchUnprepared:
      return "Batched, unprepared";
    case kFiftyOneFilter:
      return "Skewed 50% in 1%";
    case kEightyTwentyFilter:
      return "Skewed 80% in 20%";
    case kRandomFilter:
      return "Random filter";
  }
  return "Bad TestMode";
}

// Do just enough to keep some data dependence for the
// compiler / CPU
static uint32_t DryRunNoHash(Slice &s) {
  uint32_t sz = static_cast<uint32_t>(s.size());
  if (sz >= 4) {
    return sz + s.data()[3];
  } else {
    return sz;
  }
}

static uint32_t DryRunHash32(Slice &s) {
  // Same perf characteristics as GetSliceHash()
  return BloomHash(s);
}

static uint32_t DryRunHash64(Slice &s) {
  return Lower32of64(GetSliceHash64(s));
}

struct FilterBench : public MockBlockBasedTableTester {
  std::vector<KeyMaker> kms_;
  std::vector<FilterInfo> infos_;
  Random32 random_;
  std::ostringstream fp_rate_report_;
  Arena arena_;

  FilterBench()
      : MockBlockBasedTableTester(new BloomFilterPolicy(
            FLAGS_bits_per_key,
            static_cast<BloomFilterPolicy::Mode>(FLAGS_impl))),
        random_(FLAGS_seed) {
    for (uint32_t i = 0; i < FLAGS_batch_size; ++i) {
      kms_.emplace_back(FLAGS_key_size < 8 ? 8 : FLAGS_key_size);
    }
  }

  void Go();

  double RandomQueryTest(uint32_t inside_threshold, bool dry_run,
                         TestMode mode);
};

void FilterBench::Go() {
  if (FLAGS_use_plain_table_bloom && FLAGS_use_full_block_reader) {
    throw std::runtime_error(
        "Can't combine -use_plain_table_bloom and -use_full_block_reader");
  }
  if (FLAGS_use_plain_table_bloom) {
    if (FLAGS_impl > 1) {
      throw std::runtime_error(
          "-impl must currently be >= 0 and <= 1 for Plain table");
    }
  } else {
    if (FLAGS_impl == 1) {
      throw std::runtime_error(
          "Block-based filter not currently supported by filter_bench");
    }
    if (FLAGS_impl > 2) {
      throw std::runtime_error(
          "-impl must currently be 0 or 2 for Block-based table");
    }
  }

  std::unique_ptr<FilterBitsBuilder> builder;
  if (!FLAGS_use_plain_table_bloom && FLAGS_impl != 1) {
    builder.reset(FilterBuildingContext(table_options_).GetBuilder());
  }

  uint32_t variance_mask = 1;
  while (variance_mask * variance_mask * 4 < FLAGS_average_keys_per_filter) {
    variance_mask = variance_mask * 2 + 1;
  }

  const std::vector<TestMode> &testModes =
      FLAGS_best_case ? bestCaseTestModes
                      : FLAGS_quick ? quickTestModes : allTestModes;
  if (FLAGS_quick) {
    FLAGS_m_queries /= 7.0;
  } else if (FLAGS_best_case) {
    FLAGS_m_queries /= 3.0;
    FLAGS_working_mem_size_mb /= 10.0;
  }

  std::cout << "Building..." << std::endl;

  size_t total_memory_used = 0;
  size_t total_keys_added = 0;

  rocksdb::StopWatchNano timer(rocksdb::Env::Default(), true);

  while (total_memory_used < 1024 * 1024 * FLAGS_working_mem_size_mb) {
    uint32_t filter_id = random_.Next();
    uint32_t keys_to_add = FLAGS_average_keys_per_filter +
                           (random_.Next() & variance_mask) -
                           (variance_mask / 2);
    infos_.emplace_back();
    FilterInfo &info = infos_.back();
    info.filter_id_ = filter_id;
    info.keys_added_ = keys_to_add;
    if (FLAGS_use_plain_table_bloom) {
      info.plain_table_bloom_.reset(new PlainTableBloomV1());
      info.plain_table_bloom_->SetTotalBits(
          &arena_, keys_to_add * FLAGS_bits_per_key, FLAGS_impl,
          0 /*huge_page*/, nullptr /*logger*/);
      for (uint32_t i = 0; i < keys_to_add; ++i) {
        uint32_t hash = GetSliceHash(kms_[0].Get(filter_id, i));
        info.plain_table_bloom_->AddHash(hash);
      }
      info.filter_ = info.plain_table_bloom_->GetRawData();
    } else {
      for (uint32_t i = 0; i < keys_to_add; ++i) {
        builder->AddKey(kms_[0].Get(filter_id, i));
      }
      info.filter_ = builder->Finish(&info.owner_);
      info.reader_.reset(
          table_options_.filter_policy->GetFilterBitsReader(info.filter_));
      CachableEntry<ParsedFullFilterBlock> block(
          new ParsedFullFilterBlock(table_options_.filter_policy.get(),
                                    BlockContents(info.filter_)),
          nullptr /* cache */, nullptr /* cache_handle */,
          true /* own_value */);
      info.full_block_reader_.reset(
          new FullFilterBlockReader(table_.get(), std::move(block)));
    }
    total_memory_used += info.filter_.size();
    total_keys_added += keys_to_add;
  }

  uint64_t elapsed_nanos = timer.ElapsedNanos();
  double ns = double(elapsed_nanos) / total_keys_added;
  std::cout << "Build avg ns/key: " << ns << std::endl;
  std::cout << "Number of filters: " << infos_.size() << std::endl;
  std::cout << "Total memory (MB): " << total_memory_used / 1024.0 / 1024.0
            << std::endl;

  double bpk = total_memory_used * 8.0 / total_keys_added;
  std::cout << "Bits/key actual: " << bpk << std::endl;
  if (!FLAGS_quick && !FLAGS_best_case) {
    double tolerable_rate = std::pow(2.0, -(bpk - 1.0) / (1.4 + bpk / 50.0));
    std::cout << "Best possible FP rate %: " << 100.0 * std::pow(2.0, -bpk)
              << std::endl;
    std::cout << "Tolerable FP rate %: " << 100.0 * tolerable_rate << std::endl;

    std::cout << "----------------------------" << std::endl;
    std::cout << "Verifying..." << std::endl;

    uint32_t outside_q_per_f =
        static_cast<uint32_t>(FLAGS_m_queries * 1000000 / infos_.size());
    uint64_t fps = 0;
    for (uint32_t i = 0; i < infos_.size(); ++i) {
      FilterInfo &info = infos_[i];
      for (uint32_t j = 0; j < info.keys_added_; ++j) {
        if (FLAGS_use_plain_table_bloom) {
          uint32_t hash = GetSliceHash(kms_[0].Get(info.filter_id_, j));
          ALWAYS_ASSERT(info.plain_table_bloom_->MayContainHash(hash));
        } else {
          ALWAYS_ASSERT(
              info.reader_->MayMatch(kms_[0].Get(info.filter_id_, j)));
        }
      }
      for (uint32_t j = 0; j < outside_q_per_f; ++j) {
        if (FLAGS_use_plain_table_bloom) {
          uint32_t hash =
              GetSliceHash(kms_[0].Get(info.filter_id_, j | 0x80000000));
          fps += info.plain_table_bloom_->MayContainHash(hash);
        } else {
          fps += info.reader_->MayMatch(
              kms_[0].Get(info.filter_id_, j | 0x80000000));
        }
      }
    }
    std::cout << " No FNs :)" << std::endl;
    double prelim_rate = double(fps) / outside_q_per_f / infos_.size();
    std::cout << " Prelim FP rate %: " << (100.0 * prelim_rate) << std::endl;

    if (!FLAGS_allow_bad_fp_rate) {
      ALWAYS_ASSERT(prelim_rate < tolerable_rate);
    }
  }

  std::cout << "----------------------------" << std::endl;
  std::cout << "Mixed inside/outside queries..." << std::endl;
  // 50% each inside and outside
  uint32_t inside_threshold = UINT32_MAX / 2;
  for (TestMode tm : testModes) {
    random_.Seed(FLAGS_seed + 1);
    double f = RandomQueryTest(inside_threshold, /*dry_run*/ false, tm);
    random_.Seed(FLAGS_seed + 1);
    double d = RandomQueryTest(inside_threshold, /*dry_run*/ true, tm);
    std::cout << "  " << TestModeToString(tm) << " net ns/op: " << (f - d)
              << std::endl;
  }

  if (!FLAGS_quick) {
    std::cout << "----------------------------" << std::endl;
    std::cout << "Inside queries (mostly)..." << std::endl;
    // Do about 95% inside queries rather than 100% so that branch predictor
    // can't give itself an artifically crazy advantage.
    inside_threshold = UINT32_MAX / 20 * 19;
    for (TestMode tm : testModes) {
      random_.Seed(FLAGS_seed + 1);
      double f = RandomQueryTest(inside_threshold, /*dry_run*/ false, tm);
      random_.Seed(FLAGS_seed + 1);
      double d = RandomQueryTest(inside_threshold, /*dry_run*/ true, tm);
      std::cout << "  " << TestModeToString(tm) << " net ns/op: " << (f - d)
                << std::endl;
    }

    std::cout << "----------------------------" << std::endl;
    std::cout << "Outside queries (mostly)..." << std::endl;
    // Do about 95% outside queries rather than 100% so that branch predictor
    // can't give itself an artifically crazy advantage.
    inside_threshold = UINT32_MAX / 20;
    for (TestMode tm : testModes) {
      random_.Seed(FLAGS_seed + 2);
      double f = RandomQueryTest(inside_threshold, /*dry_run*/ false, tm);
      random_.Seed(FLAGS_seed + 2);
      double d = RandomQueryTest(inside_threshold, /*dry_run*/ true, tm);
      std::cout << "  " << TestModeToString(tm) << " net ns/op: " << (f - d)
                << std::endl;
    }
  }
  std::cout << fp_rate_report_.str();

  std::cout << "----------------------------" << std::endl;
  std::cout << "Done. (For more info, run with -legend or -help.)" << std::endl;
}

double FilterBench::RandomQueryTest(uint32_t inside_threshold, bool dry_run,
                                    TestMode mode) {
  for (auto &info : infos_) {
    info.outside_queries_ = 0;
    info.false_positives_ = 0;
  }

  auto dry_run_hash_fn = DryRunNoHash;
  if (!FLAGS_net_includes_hashing) {
    if (FLAGS_impl < 2 || FLAGS_use_plain_table_bloom) {
      dry_run_hash_fn = DryRunHash32;
    } else {
      dry_run_hash_fn = DryRunHash64;
    }
  }

  uint32_t num_infos = static_cast<uint32_t>(infos_.size());
  uint32_t dry_run_hash = 0;
  uint64_t max_queries =
      static_cast<uint64_t>(FLAGS_m_queries * 1000000 + 0.50);
  // Some filters may be considered secondary in order to implement skewed
  // queries. num_primary_filters is the number that are to be treated as
  // equal, and any remainder will be treated as secondary.
  uint32_t num_primary_filters = num_infos;
  // The proportion (when divided by 2^32 - 1) of filter queries going to
  // the primary filters (default = all). The remainder of queries are
  // against secondary filters.
  uint32_t primary_filter_threshold = 0xffffffff;
  if (mode == kSingleFilter) {
    // 100% of queries to 1 filter
    num_primary_filters = 1;
  } else if (mode == kFiftyOneFilter) {
    // 50% of queries
    primary_filter_threshold /= 2;
    // to 1% of filters
    num_primary_filters = (num_primary_filters + 99) / 100;
  } else if (mode == kEightyTwentyFilter) {
    // 80% of queries
    primary_filter_threshold = primary_filter_threshold / 5 * 4;
    // to 20% of filters
    num_primary_filters = (num_primary_filters + 4) / 5;
  }
  uint32_t batch_size = 1;
  std::unique_ptr<Slice[]> batch_slices;
  std::unique_ptr<Slice *[]> batch_slice_ptrs;
  std::unique_ptr<bool[]> batch_results;
  if (mode == kBatchPrepared || mode == kBatchUnprepared) {
    batch_size = static_cast<uint32_t>(kms_.size());
  }

  batch_slices.reset(new Slice[batch_size]);
  batch_slice_ptrs.reset(new Slice *[batch_size]);
  batch_results.reset(new bool[batch_size]);
  for (uint32_t i = 0; i < batch_size; ++i) {
    batch_results[i] = false;
    batch_slice_ptrs[i] = &batch_slices[i];
  }

  rocksdb::StopWatchNano timer(rocksdb::Env::Default(), true);

  for (uint64_t q = 0; q < max_queries; q += batch_size) {
    bool inside_this_time = random_.Next() <= inside_threshold;

    uint32_t filter_index;
    if (random_.Next() <= primary_filter_threshold) {
      filter_index = random_.Uniformish(num_primary_filters);
    } else {
      // secondary
      filter_index = num_primary_filters +
                     random_.Uniformish(num_infos - num_primary_filters);
    }
    FilterInfo &info = infos_[filter_index];
    for (uint32_t i = 0; i < batch_size; ++i) {
      if (inside_this_time) {
        batch_slices[i] =
            kms_[i].Get(info.filter_id_, random_.Uniformish(info.keys_added_));
      } else {
        batch_slices[i] =
            kms_[i].Get(info.filter_id_, random_.Uniformish(info.keys_added_) |
                                             uint32_t{0x80000000});
        info.outside_queries_++;
      }
    }
    // TODO: implement batched interface to full block reader
    // TODO: implement batched interface to plain table bloom
    if (mode == kBatchPrepared && !FLAGS_use_full_block_reader &&
        !FLAGS_use_plain_table_bloom) {
      for (uint32_t i = 0; i < batch_size; ++i) {
        batch_results[i] = false;
      }
      if (dry_run) {
        for (uint32_t i = 0; i < batch_size; ++i) {
          batch_results[i] = true;
          dry_run_hash += dry_run_hash_fn(batch_slices[i]);
        }
      } else {
        info.reader_->MayMatch(batch_size, batch_slice_ptrs.get(),
                               batch_results.get());
      }
      for (uint32_t i = 0; i < batch_size; ++i) {
        if (inside_this_time) {
          ALWAYS_ASSERT(batch_results[i]);
        } else {
          info.false_positives_ += batch_results[i];
        }
      }
    } else {
      for (uint32_t i = 0; i < batch_size; ++i) {
        bool may_match;
        if (FLAGS_use_plain_table_bloom) {
          if (dry_run) {
            dry_run_hash += dry_run_hash_fn(batch_slices[i]);
            may_match = true;
          } else {
            uint32_t hash = GetSliceHash(batch_slices[i]);
            may_match = info.plain_table_bloom_->MayContainHash(hash);
          }
        } else if (FLAGS_use_full_block_reader) {
          if (dry_run) {
            dry_run_hash += dry_run_hash_fn(batch_slices[i]);
            may_match = true;
          } else {
            may_match = info.full_block_reader_->KeyMayMatch(
                batch_slices[i],
                /*prefix_extractor=*/nullptr,
                /*block_offset=*/rocksdb::kNotValid,
                /*no_io=*/false, /*const_ikey_ptr=*/nullptr,
                /*get_context=*/nullptr,
                /*lookup_context=*/nullptr);
          }
        } else {
          if (dry_run) {
            dry_run_hash += dry_run_hash_fn(batch_slices[i]);
            may_match = true;
          } else {
            may_match = info.reader_->MayMatch(batch_slices[i]);
          }
        }
        if (inside_this_time) {
          ALWAYS_ASSERT(may_match);
        } else {
          info.false_positives_ += may_match;
        }
      }
    }
  }

  uint64_t elapsed_nanos = timer.ElapsedNanos();
  double ns = double(elapsed_nanos) / max_queries;

  if (!FLAGS_quick) {
    if (dry_run) {
      // Printing part of hash prevents dry run components from being optimized
      // away by compiler
      std::cout << "    Dry run (" << std::hex << (dry_run_hash & 0xfffff)
                << std::dec << ") ";
    } else {
      std::cout << "    Gross filter    ";
    }
    std::cout << "ns/op: " << ns << std::endl;
  }

  if (!dry_run) {
    fp_rate_report_ = std::ostringstream();
    uint64_t q = 0;
    uint64_t fp = 0;
    double worst_fp_rate = 0.0;
    double best_fp_rate = 1.0;
    for (auto &info : infos_) {
      q += info.outside_queries_;
      fp += info.false_positives_;
      if (info.outside_queries_ > 0) {
        double fp_rate = double(info.false_positives_) / info.outside_queries_;
        worst_fp_rate = std::max(worst_fp_rate, fp_rate);
        best_fp_rate = std::min(best_fp_rate, fp_rate);
      }
    }
    fp_rate_report_ << "    Average FP rate %: " << 100.0 * fp / q << std::endl;
    if (!FLAGS_quick && !FLAGS_best_case) {
      fp_rate_report_ << "    Worst   FP rate %: " << 100.0 * worst_fp_rate
                      << std::endl;
      fp_rate_report_ << "    Best    FP rate %: " << 100.0 * best_fp_rate
                      << std::endl;
      fp_rate_report_ << "    Best possible bits/key: "
                      << -std::log(double(fp) / q) / std::log(2.0) << std::endl;
    }
  }
  return ns;
}

int main(int argc, char **argv) {
  rocksdb::port::InstallStackTraceHandler();
  SetUsageMessage(std::string("\nUSAGE:\n") + std::string(argv[0]) +
                  " [-quick] [OTHER OPTIONS]...");
  ParseCommandLineFlags(&argc, &argv, true);

  PrintWarnings();

  if (FLAGS_legend) {
    std::cout
        << "Legend:" << std::endl
        << "  \"Inside\" - key that was added to filter" << std::endl
        << "  \"Outside\" - key that was not added to filter" << std::endl
        << "  \"FN\" - false negative query (must not happen)" << std::endl
        << "  \"FP\" - false positive query (OK at low rate)" << std::endl
        << "  \"Dry run\" - cost of testing and hashing overhead." << std::endl
        << "  \"Gross filter\" - cost of filter queries including testing "
        << "\n     and hashing overhead." << std::endl
        << "  \"net\" - best estimate of time in filter operation, without "
        << "\n     testing and hashing overhead (gross filter - dry run)"
        << std::endl
        << "  \"ns/op\" - nanoseconds per operation (key query or add)"
        << std::endl
        << "  \"Single filter\" - essentially minimum cost, assuming filter"
        << "\n     fits easily in L1 CPU cache." << std::endl
        << "  \"Batched, prepared\" - several queries at once against a"
        << "\n     randomly chosen filter, using multi-query interface."
        << std::endl
        << "  \"Batched, unprepared\" - similar, but using serial calls"
        << "\n     to single query interface." << std::endl
        << "  \"Random filter\" - a filter is chosen at random as target"
        << "\n     of each query." << std::endl
        << "  \"Skewed X% in Y%\" - like \"Random filter\" except Y% of"
        << "\n      the filters are designated as \"hot\" and receive X%"
        << "\n      of queries." << std::endl;
  } else {
    FilterBench b;
    b.Go();
  }

  return 0;
}

#endif  // !defined(GFLAGS) || defined(ROCKSDB_LITE)
