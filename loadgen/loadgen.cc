#include "loadgen.h"

#include <stdint.h>

#include <atomic>
#include <cassert>
#include <cstring>
#include <fstream>
#include <future>
#include <queue>
#include <random>
#include <string>
#include <thread>

#include "logging.h"
#include "query_sample.h"
#include "query_sample_library.h"
#include "system_under_test.h"
#include "test_settings.h"
#include "test_settings_internal.h"
#include "utils.h"
#include "version.h"

namespace mlperf {

struct SampleMetadata;
struct QueryMetadata;

struct ResponseDelegate {
  virtual void SampleComplete(SampleMetadata*, QuerySampleResponse*,
                              PerfClock::time_point) = 0;
  virtual void QueryComplete() = 0;
};

// SampleMetadata is used by the load generator to coordinate
// response data and completion.
struct SampleMetadata {
  QueryMetadata* query_metadata;
  uint64_t sequence_id;
  QuerySampleIndex sample_index;
};

class QueryMetadata {
 public:
  QueryMetadata(const std::vector<QuerySampleIndex>& query_sample_indicies,
                std::chrono::nanoseconds scheduled_delta,
                ResponseDelegate* response_delegate, uint64_t query_sequence_id,
                uint64_t* sample_sequence_id)
      : scheduled_delta(scheduled_delta),
        response_delegate(response_delegate),
        sequence_id(query_sequence_id),
        wait_count_(query_sample_indicies.size()) {
    samples_.reserve(query_sample_indicies.size());
    for (QuerySampleIndex qsi : query_sample_indicies) {
      samples_.push_back({this, (*sample_sequence_id)++, qsi});
    }
    query_to_send.reserve(query_sample_indicies.size());
    for (auto& s : samples_) {
      query_to_send.push_back(
          {reinterpret_cast<ResponseId>(&s), s.sample_index});
    }
  }

  QueryMetadata(QueryMetadata&& src)
      : query_to_send(std::move(src.query_to_send)),
        scheduled_delta(src.scheduled_delta),
        response_delegate(src.response_delegate),
        sequence_id(src.sequence_id),
        wait_count_(src.samples_.size()),
        samples_(std::move(src.samples_)) {
    // The move constructor should only be called while generating a
    // vector of QueryMetadata, before it's been used.
    // Assert that wait_count_ is in its initial state.
    assert(src.wait_count_.load() == samples_.size());
    // Update the "parent" of each sample to be this query; the old query
    // address will no longer be valid.
    for (size_t i = 0; i < samples_.size(); i++) {
      SampleMetadata* s = &samples_[i];
      s->query_metadata = this;
      query_to_send[i].id = reinterpret_cast<ResponseId>(s);
    }
  }

  void NotifyOneSampleCompleted() {
    size_t old_count = wait_count_.fetch_sub(1, std::memory_order_relaxed);
    if (old_count == 1) {
      all_samples_done_.set_value();
      response_delegate->QueryComplete();
    }
  }

  void WaitForAllSamplesCompleted() { all_samples_done_.get_future().wait(); }

 public:
  std::vector<QuerySample> query_to_send;
  const std::chrono::nanoseconds scheduled_delta;
  ResponseDelegate* const response_delegate;
  const uint64_t sequence_id;

  // Performance information.

  int scheduled_intervals = 0;  // Number of intervals between queries, as
                                // actually scheduled during the run.
                                // For the multi-stream scenario only.
  PerfClock::time_point scheduled_time;
  PerfClock::time_point issued_start_time;

 private:
  std::atomic<size_t> wait_count_;
  std::promise<void> all_samples_done_;
  std::vector<SampleMetadata> samples_;
};

void QuerySamplesComplete(QuerySampleResponse* responses,
                          size_t response_count) {
  PerfClock::time_point timestamp = PerfClock::now();

  auto trace = MakeScopedTracer(
      [](AsyncLog& log) { log.ScopedTrace("QuerySamplesComplete"); });

  const QuerySampleResponse* end = responses + response_count;

  // Notify first to unblock loadgen production ASAP.
  for (QuerySampleResponse* response = responses; response < end; response++) {
    SampleMetadata* sample = reinterpret_cast<SampleMetadata*>(response->id);
    QueryMetadata* query = sample->query_metadata;
    query->NotifyOneSampleCompleted();
  }

  // Log samples.
  for (QuerySampleResponse* response = responses; response < end; response++) {
    SampleMetadata* sample = reinterpret_cast<SampleMetadata*>(response->id);
    QueryMetadata* query = sample->query_metadata;
    query->response_delegate->SampleComplete(sample, response, timestamp);
  }
}

struct DurationGeneratorNs {
  const PerfClock::time_point start;
  int64_t delta(PerfClock::time_point end) const {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(end - start)
        .count();
  }
};

// Right now, this is the only implementation of ResponseDelegate,
// but more will be coming soon.
// TODO: Versions that don't copy data.
// TODO: Versions that have less detailed logs.
// TODO: Versions that do a delayed notification.
template <TestScenario scenario, TestMode mode>
struct ResponseDelegateDetailed : public ResponseDelegate {
  std::atomic<size_t> queries_completed{0};

  void SampleComplete(SampleMetadata* sample, QuerySampleResponse* response,
                      PerfClock::time_point complete_begin_time) override {
    // Using a raw pointer here should help us hit the std::function
    // small buffer optimization code path when we aren't copying data.
    // For some reason, using std::unique_ptr<std::vector> wasn't moving
    // into the lambda; even with C++14.
    uint8_t* sample_data_copy = nullptr;
    if (mode == TestMode::AccuracyOnly) {
      // TODO: Verify accuracy with the data copied here.
      auto* src_begin = reinterpret_cast<uint8_t*>(response->data);
      sample_data_copy = new uint8_t[response->size];
      std::memcpy(sample_data_copy, src_begin, response->size);
    }
    Log([sample, sample_data_copy, complete_begin_time](AsyncLog& log) {
      QueryMetadata* query = sample->query_metadata;
      DurationGeneratorNs sched{query->scheduled_time};
      QuerySampleLatency latency = sched.delta(complete_begin_time);
      log.RecordLatency(sample->sequence_id, latency);
      if (sample_data_copy != nullptr) {
        delete[] sample_data_copy;
      }
      // Disable tracing each sample in offline mode. Since thousands of
      // samples could be overlapping when visualized, it's not very useful.
      // TODO: Should we disable for cloud mode as well? Sufficiently
      // out-of-order processing could have lots of overlap too.
      if (scenario == TestScenario::Offline) {
        return;
      }
      log.TraceSample("Sample", sample->sequence_id, query->scheduled_time,
                      complete_begin_time, "sample_seq", sample->sequence_id,
                      "query_seq", query->sequence_id, "sample_idx",
                      sample->sample_index, "issue_start_ns",
                      sched.delta(query->issued_start_time), "complete_ns",
                      sched.delta(complete_begin_time));
    });
  }

  void QueryComplete() override {
    // We only need to track oustanding queries in the server scenario to
    // detect when the SUT has fallen too far behind.
    if (scenario == TestScenario::Server) {
      queries_completed.fetch_add(1, std::memory_order_relaxed);
    }
  }
};

// ScheduleDistribution templates by test scenario.
template <TestScenario scenario>
auto ScheduleDistribution(double qps) {
  return [period = std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::duration<double>(1.0 / qps))](auto& gen) {
    return period;
  };
}

template <>
auto ScheduleDistribution<TestScenario::Server>(double qps) {
  // Poisson arrival process corresponds to exponentially distributed
  // interarrival times.
  return [dist = std::exponential_distribution<>(qps)](auto& gen) mutable {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::duration<double>(dist(gen)));
  };
}

// SampleDistribution templates by test mode.
template <TestMode mode>
auto SampleDistribution(size_t sample_count) {
  return [sample_count, i = size_t(0)](auto& gen) mutable {
    return (i++) % sample_count;
  };
}

template <>
auto SampleDistribution<TestMode::PerformanceOnly>(size_t sample_count) {
  return [dist = std::uniform_int_distribution<>(0, sample_count - 1)](
             auto& gen) mutable { return dist(gen); };
}

template <TestScenario scenario, TestMode mode>
std::vector<QueryMetadata> GenerateQueries(
    const TestSettingsInternal& settings,
    const std::vector<QuerySampleIndex>& loaded_samples,
    ResponseDelegate* response_delegate) {
  auto trace = MakeScopedTracer(
      [](AsyncLog& log) { log.ScopedTrace("GenerateQueries"); });

  // Generate 2x more samples than we think we'll need given the expected
  // QPS. We should exit before issuing all queries.
  std::chrono::microseconds k2xMinDuration = 2 * settings.min_duration;
  size_t min_queries = settings.min_query_count;

  // We should not exit early in accuracy mode.
  if (mode == TestMode::AccuracyOnly) {
    k2xMinDuration = std::chrono::microseconds(0);
    // TODO: Figure out how to pad accuracy mode queries if samples_per_query
    // does not divide loaded_samples.size() evenly.
    min_queries = (loaded_samples.size() + settings.samples_per_query - 1) /
                  settings.samples_per_query;
  }

  std::vector<QueryMetadata> queries;

  assert(scenario == settings.scenario);
  assert(mode == settings.mode);

  // Using the std::mt19937 pseudo-random number generator ensures a modicum of
  // cross platform reproducibility for trace generation.
  std::mt19937 sample_rng(settings.sample_index_rng_seed);
  std::mt19937 schedule_rng(settings.schedule_rng_seed);

  auto sample_distribution = SampleDistribution<mode>(loaded_samples.size());
  auto schedule_distribution =
      ScheduleDistribution<scenario>(settings.target_qps);

  std::vector<QuerySampleIndex> samples(settings.samples_per_query);
  std::chrono::nanoseconds timestamp(0);
  uint64_t query_sequence_id = 0;
  uint64_t sample_sequence_id = 0;
  while (timestamp <= k2xMinDuration || queries.size() < min_queries) {
    for (auto& s : samples) {
      s = loaded_samples[sample_distribution(sample_rng)];
    }
    queries.emplace_back(samples, timestamp, response_delegate,
                         query_sequence_id, &sample_sequence_id);
    timestamp += schedule_distribution(schedule_rng);
    query_sequence_id++;
  }

  LogDetail([count = queries.size(), spq = settings.samples_per_query,
             duration = timestamp.count()](AsyncLog& log) {
    log.LogDetail("GeneratedQueries: ", "queries", count, "samples per query",
                  spq, "duration", duration);
  });

  return queries;
}

template <TestScenario scenario>
void RunAccuracyMode(
    SystemUnderTest* sut, QuerySampleLibrary* qsl,
    const TestSettingsInternal& settings,
    const std::vector<std::vector<QuerySampleIndex>>& loadable_sets) {
  LogDetail(
      [](AsyncLog& log) { log.LogDetail("Starting verification mode:"); });

  constexpr size_t kMaxAsyncQueries = 4;
  constexpr int64_t kNanosecondsPerSecond = 1000000000;

  ResponseDelegateDetailed<scenario, TestMode::AccuracyOnly> response_logger;

  if (settings.scenario != TestScenario::MultiStream) {
    LogError([](AsyncLog& log) {
      log.LogDetail("Unsupported scenario. Only MultiStream supported.");
    });
    return;
  }

  // Iterate through each loadable set.
  PerfClock::time_point start = PerfClock::now();
  uint64_t tick_count = 0;
  for (auto& loadable_set : loadable_sets) {
    {
      auto trace =
          MakeScopedTracer([count = loadable_set.size()](AsyncLog& log) {
            log.ScopedTrace("LoadSamples", "count", count);
          });
      qsl->LoadSamplesToRam(loadable_set);
    }
    GlobalLogger().RestartLatencyRecording();

    // Split the set up into queries.
    std::vector<QueryMetadata> queries =
        GenerateQueries<TestScenario::MultiStream, TestMode::AccuracyOnly>(
            settings, loadable_set, &response_logger);

    std::queue<QueryMetadata*> prev_queries;
    for (auto& query : queries) {
      // Sleep until the next tick, skipping ticks that have already passed.
      auto query_start_time = start;
      do {
        tick_count++;
        std::chrono::nanoseconds delta(static_cast<int64_t>(
            (tick_count * kNanosecondsPerSecond) / settings.target_qps));
        query_start_time = start + delta;
      } while (query_start_time < PerfClock::now());
      std::this_thread::sleep_until(query_start_time);

      // Limit the number of oustanding async queries by waiting for
      // old queries here.
      if (prev_queries.size() > kMaxAsyncQueries) {
        QueryMetadata* limitting_query = prev_queries.front();
        limitting_query->WaitForAllSamplesCompleted();
        prev_queries.pop();
      }
      prev_queries.push(&query);

      // Issue the query to the SUT.
      query.scheduled_time = query_start_time;
      auto trace = MakeScopedTracer(
          [](AsyncLog& log) { log.ScopedTrace("IssueQuery"); });
      query.issued_start_time = PerfClock::now();
      sut->IssueQuery(query.query_to_send);
    }

    // Wait for tail queries to complete and collect all the latencies.
    // We have to keep the synchronization primitives and loaded samples
    // alive until the SUT is done with them.
    GlobalLogger().GetLatenciesBlocking(loadable_set.size());

    auto trace = MakeScopedTracer([count = loadable_set.size()](AsyncLog& log) {
      log.ScopedTrace("UnloadSampes", "count", count);
    });
    qsl->UnloadSamplesFromRam(loadable_set);
  }
}

// Template for the QueryScheduler. This base template should never be used
// since each scenario has its own specialization.
template <TestScenario scenario>
struct QueryScheduler {
  QueryScheduler(const TestSettingsInternal& settings,
                 const PerfClock::time_point) {
    assert(false);
  }
  PerfClock::time_point Wait(QueryMetadata* next_query) {
    assert(false);
    return PerfClock::now();
  }
};

// SingleStream QueryScheduler
template <>
struct QueryScheduler<TestScenario::SingleStream> {
  QueryScheduler(const TestSettingsInternal& settings,
                 const PerfClock::time_point) {}

  PerfClock::time_point Wait(QueryMetadata* next_query) {
    auto trace =
        MakeScopedTracer([](AsyncLog& log) { log.ScopedTrace("Waiting"); });
    if (prev_query != nullptr) {
      prev_query->WaitForAllSamplesCompleted();
    }
    prev_query = next_query;

    auto now = PerfClock::now();
    next_query->scheduled_time = now;
    next_query->issued_start_time = now;
    return now;
  }

  QueryMetadata* prev_query = nullptr;
};

// MultiStream QueryScheduler
template <>
struct QueryScheduler<TestScenario::MultiStream> {
  QueryScheduler(const TestSettingsInternal& settings,
                 const PerfClock::time_point start)
      : max_async_queries(settings.max_async_queries), tick_time(start) {}

  PerfClock::time_point Wait(QueryMetadata* next_query) {
    {
      auto trace =
          MakeScopedTracer([](AsyncLog& log) { log.ScopedTrace("Waiting"); });
      if (prev_queries.size() >= max_async_queries) {
        prev_queries.front()->WaitForAllSamplesCompleted();
        prev_queries.pop();
      }
      prev_queries.push(next_query);
    }

    {
      auto trace = MakeScopedTracer(
          [](AsyncLog& log) { log.ScopedTrace("Scheduling"); });
      PerfClock::time_point now = PerfClock::now();
      auto i_period_old = i_period;
      do {
        tick_time += kPeriods[i_period++ % 3];
        Log([tick_time = tick_time](AsyncLog& log) {
          log.TraceAsyncInstant("QueryInterval", 0, tick_time);
        });
      } while (tick_time < now);
      next_query->scheduled_intervals = i_period - i_period_old;
      next_query->scheduled_time = tick_time;
      std::this_thread::sleep_until(tick_time);
    }

    auto now = PerfClock::now();
    next_query->issued_start_time = now;
    return now;
  }

  // TODO: Support frequencies other than 60Hz.
  static constexpr std::chrono::nanoseconds kPeriods[] = {
      std::chrono::nanoseconds(16666667),
      std::chrono::nanoseconds(16666666),
      std::chrono::nanoseconds(16666667),
  };
  const size_t max_async_queries;
  PerfClock::time_point tick_time;
  size_t i_period = 0;
  std::queue<QueryMetadata*> prev_queries;
};

constexpr std::chrono::nanoseconds
    QueryScheduler<TestScenario::MultiStream>::kPeriods[];

// Server QueryScheduler
template <>
struct QueryScheduler<TestScenario::Server> {
  QueryScheduler(const TestSettingsInternal& settings,
                 const PerfClock::time_point start)
      : start(start) {}

  // TODO: Coalesce all queries whose scheduled timestamps have passed.
  PerfClock::time_point Wait(QueryMetadata* next_query) {
    auto trace =
        MakeScopedTracer([](AsyncLog& log) { log.ScopedTrace("Scheduling"); });

    auto scheduled_time = start + next_query->scheduled_delta;
    next_query->scheduled_time = scheduled_time;
    std::this_thread::sleep_until(scheduled_time);

    auto now = PerfClock::now();
    next_query->issued_start_time = now;
    return now;
  }

  const PerfClock::time_point start;
};

// Offline QueryScheduler
template <>
struct QueryScheduler<TestScenario::Offline> {
  QueryScheduler(const TestSettingsInternal& settings,
                 const PerfClock::time_point start)
      : start(start) {}

  PerfClock::time_point Wait(QueryMetadata* next_query) {
    next_query->scheduled_time = start;
    auto now = PerfClock::now();
    next_query->issued_start_time = now;
    return now;
  }

  const PerfClock::time_point start;
};

struct PerformanceResult {
  std::vector<QuerySampleLatency> latencies;
  double max_latency;
  double final_query_scheduled_time;  // seconds from start.
  double final_query_issued_time;     // seconds from start.
};

template <TestScenario scenario>
PerformanceResult IssuePerformanceQueries(
    SystemUnderTest* sut, const TestSettingsInternal& settings,
    const std::vector<QuerySampleIndex>& performance_set) {
  GlobalLogger().RestartLatencyRecording();
  ResponseDelegateDetailed<scenario, TestMode::PerformanceOnly> response_logger;

  std::vector<QueryMetadata> queries =
      GenerateQueries<scenario, TestMode::PerformanceOnly>(
          settings, performance_set, &response_logger);

  size_t queries_issued = 0;
  // TODO: Replace the constant 5 below with a TestSetting.
  const double query_seconds_outstanding_threshold =
      5 * std::chrono::duration_cast<std::chrono::duration<double>>(
              settings.target_latency)
              .count();
  const size_t max_queries_outstanding =
      settings.target_qps * query_seconds_outstanding_threshold;

  const PerfClock::time_point start = PerfClock::now();
  PerfClock::time_point last_now = start;
  QueryScheduler<scenario> query_scheduler(settings, start);

  for (auto& query : queries) {
    auto trace1 =
        MakeScopedTracer([](AsyncLog& log) { log.ScopedTrace("SampleLoop"); });
    last_now = query_scheduler.Wait(&query);

    // Issue the query to the SUT.
    {
      auto trace3 = MakeScopedTracer(
          [](AsyncLog& log) { log.ScopedTrace("IssueQuery"); });
      sut->IssueQuery(query.query_to_send);
    }

    queries_issued++;
    auto duration = (last_now - start);
    if (queries_issued > settings.min_query_count &&
        duration > settings.min_duration) {
      LogDetail([](AsyncLog& log) {
        log.LogDetail(
            "Ending naturally: Minimum query count and test duration met.");
      });
      break;
    }
    if (queries_issued > settings.max_query_count) {
      LogError([queries_issued](AsyncLog& log) {
        log.LogDetail("Ending early: Max query count reached.", "query_count",
                      queries_issued);
      });
      break;
    }
    if (settings.max_duration.count() != 0 &&
        duration > settings.max_duration) {
      LogError([duration](AsyncLog& log) {
        log.LogDetail("Ending early: Max test duration reached.", "duration_ns",
                      duration.count());
      });
      break;
    }
    if (scenario == TestScenario::Server) {
      size_t queries_outstanding =
          queries_issued -
          response_logger.queries_completed.load(std::memory_order_relaxed);
      if (queries_outstanding > max_queries_outstanding) {
        LogError([queries_issued, queries_outstanding](AsyncLog& log) {
          log.LogDetail("Ending early: Too many oustanding queries.", "issued",
                        queries_issued, "outstanding", queries_outstanding);
        });
        break;
      }
    }
    // TODO: Use GetMaxLatencySoFar here if we decide to have a hard latency
    //       limit.
  }

  if (queries_issued >= queries.size()) {
    LogError([](AsyncLog& log) {
      log.LogDetail(
          "Ending early: Ran out of generated queries to issue before the "
          "minimum query count and test duration were reached.");
      log.LogDetail(
          "Please update the relevant expected latency or target qps in the "
          "TestSettings so they are more accurate.");
    });
  }

  // Wait for tail queries to complete and collect all the latencies.
  // We have to keep the synchronization primitives alive until the SUT
  // is done with them.
  auto& final_query = queries[queries_issued - 1];
  const size_t expected_latencies = queries_issued * settings.samples_per_query;
  std::vector<QuerySampleLatency> latencies(
      GlobalLogger().GetLatenciesBlocking(expected_latencies));
  double max_latency =
      QuerySampleLatencyToSeconds(GlobalLogger().GetMaxLatencySoFar());
  double final_query_scheduled_time =
      DurationToSeconds(final_query.scheduled_delta);
  double final_query_issued_time =
      DurationToSeconds(final_query.issued_start_time - start);
  return {std::move(latencies), max_latency, final_query_scheduled_time,
          final_query_issued_time};
}

// TODO: Actually do a binary search rather than copying the performance
// behavior.
template <TestScenario scenario>
void FindPeakPerformanceMode(
    SystemUnderTest* sut, QuerySampleLibrary* qsl,
    const TestSettingsInternal& settings,
    const std::vector<std::vector<QuerySampleIndex>>& loadable_sets) {
  LogDetail([](AsyncLog& log) {
    log.LogDetail("Starting FindPeakPerformance mode:");
  });

  // Use first loadable set as the performance set.
  const std::vector<QuerySampleIndex>& performance_set = loadable_sets.front();

  qsl->LoadSamplesToRam(performance_set);

  PerformanceResult pr(
      IssuePerformanceQueries<scenario>(sut, settings, performance_set));

  qsl->UnloadSamplesFromRam(performance_set);
}

template <TestScenario scenario>
void RunPerformanceMode(
    SystemUnderTest* sut, QuerySampleLibrary* qsl,
    const TestSettingsInternal& settings,
    const std::vector<std::vector<QuerySampleIndex>>& loadable_sets) {
  LogDetail([](AsyncLog& log) { log.LogDetail("Starting performance mode:"); });

  // Use first loadable set as the performance set.
  const std::vector<QuerySampleIndex>& performance_set = loadable_sets.front();
  qsl->LoadSamplesToRam(performance_set);

  PerformanceResult pr(
      IssuePerformanceQueries<scenario>(sut, settings, performance_set));

  sut->ReportLatencyResults(pr.latencies);

  // TODO: Make this re-usable by FindPeakPerformanceMode.
  Log([pr = std::move(pr), settings](AsyncLog& log) mutable {
    size_t sample_count = pr.latencies.size();
    std::sort(pr.latencies.begin(), pr.latencies.end());
    int64_t accumulated_latency = 0;
    for (auto l : pr.latencies) {
      accumulated_latency += l;
    }
    int64_t mean_latency = accumulated_latency / sample_count;
    int64_t max_latency = pr.latencies.back();
    size_t i90 = sample_count * .9;
    auto l90 = pr.latencies[i90];

    log.LogSummary("Loadgen results summary:");
    switch (scenario) {
      case TestScenario::SingleStream: {
        double qps = sample_count / pr.final_query_issued_time;
        log.LogSummary("Single Stream 90th percentile latency (ns) : ", l90);
        log.LogSummary("Single Stream QPS : ", qps);
        break;
      }
      case TestScenario::MultiStream: {
        log.LogSummary("Multi Stream N : ", settings.samples_per_query);
        // TODO: Finalize multi-stream performance targets with working group.
        bool perf_target_satisfied = l90 <= settings.target_latency.count();
        log.LogSummary("Multi Stream Performance Target Satisfied : ",
                       perf_target_satisfied ? "Yes" : "NO");
        break;
      }
      case TestScenario::Server: {
        double qps = sample_count / pr.final_query_scheduled_time;
        log.LogSummary("Server Actual QPS : ", qps);
        bool target_latency_satisfied = l90 <= settings.target_latency.count();
        log.LogSummary("Target Latency Satisfied : ",
                       target_latency_satisfied ? "Yes" : "NO");
        log.LogSummary("Server Target QPS : ", settings.target_qps);
        log.LogSummary("Server Target Latency (ns) : ",
                       settings.target_latency.count());
        break;
      }
      case TestScenario::Offline: {
        double qps = sample_count / pr.max_latency;
        log.LogSummary("\n  Offline QPS: ", qps);
        break;
      }
    }
    log.LogSummary("90th percentile latency (ns) : ", l90);
    log.LogSummary("Mean latency (ns) : ", mean_latency);
    log.LogSummary("Max latency (ns) : ", max_latency);
  });

  qsl->UnloadSamplesFromRam(performance_set);
}

// Routes runtime scenario requests to the corresponding instances of its
// mode functions.
struct RunFunctions {
  using Signature =
      void(SystemUnderTest* sut, QuerySampleLibrary* qsl,
           const TestSettingsInternal& settings,
           const std::vector<std::vector<QuerySampleIndex>>& loadable_sets);

  template <TestScenario compile_time_scenario>
  static RunFunctions GetCompileTime() {
    return {(*RunAccuracyMode<compile_time_scenario>),
            (*RunPerformanceMode<compile_time_scenario>),
            (*FindPeakPerformanceMode<compile_time_scenario>)};
  }

  static RunFunctions Get(TestScenario run_time_scenario) {
    switch (run_time_scenario) {
      case TestScenario::SingleStream:
        return GetCompileTime<TestScenario::SingleStream>();
      case TestScenario::MultiStream:
        return GetCompileTime<TestScenario::MultiStream>();
      case TestScenario::Server:
        return GetCompileTime<TestScenario::Server>();
      case TestScenario::Offline:
        return GetCompileTime<TestScenario::Offline>();
    }
    // We should not reach this point.
    assert(false);
    return GetCompileTime<TestScenario::SingleStream>();
  }

  const Signature& accuracy;
  const Signature& performance;
  const Signature& find_peak_performance;
};

// Generates random sets of samples in the QSL that we can load into RAM
// at the same time.
// Choosing samples randomly to go into a set naturally avoids biasing some
// samples to a particular set.
// TODO: Choosing bins randomly, rather than samples randomly, would avoid the
//       garbage collection logic, but we'd need to avoid later samples being
//       less likely to be in the smallest set. This may not be an important
//       requirement though.
std::vector<std::vector<QuerySampleIndex>> GenerateLoadableSets(
    QuerySampleLibrary* qsl, const TestSettingsInternal& settings) {
  constexpr float kGarbageCollectRatio = 0.5;
  constexpr size_t kUsedIndex = std::numeric_limits<size_t>::max();

  auto trace = MakeScopedTracer(
      [](AsyncLog& log) { log.ScopedTrace("GenerateLoadableSets"); });

  std::vector<std::vector<QuerySampleIndex>> result;
  std::mt19937 qsl_rng(settings.qsl_rng_seed);

  // Generate indicies for all available samples in the QSL.
  const size_t qsl_total_count = qsl->TotalSampleCount();
  std::vector<QuerySampleIndex> samples(qsl_total_count);
  for (size_t i = 0; i < qsl_total_count; i++) {
    samples[i] = static_cast<QuerySampleIndex>(i);
  }

  const size_t set_size = qsl->PerformanceSampleCount();
  std::vector<QuerySampleIndex> loadable_set;
  loadable_set.reserve(set_size);
  size_t remaining_count = samples.size();
  size_t garbage_collect_count = remaining_count * kGarbageCollectRatio;
  std::uniform_int_distribution<> dist(0, remaining_count - 1);

  while (remaining_count > 0) {
    size_t candidate_index = dist(qsl_rng);
    // Skip indicies we've already used.
    if (samples[candidate_index] == kUsedIndex) {
      continue;
    }

    // Update loadable sets and mark index as used.
    loadable_set.push_back(samples[candidate_index]);
    if (loadable_set.size() == set_size) {
      result.push_back(std::move(loadable_set));
      loadable_set.clear();
      loadable_set.reserve(set_size);
    }
    samples[candidate_index] = kUsedIndex;
    remaining_count--;

    // Garbage collect used indicies as probability of hitting one increases.
    if (garbage_collect_count != 0) {
      garbage_collect_count--;
    } else {
      RemoveValue(&samples, kUsedIndex);
      assert(remaining_count == samples.size());
      dist.param(
          std::uniform_int_distribution<>::param_type(0, remaining_count - 1));
      garbage_collect_count = remaining_count * kGarbageCollectRatio;
    }
  }

  if (!loadable_set.empty()) {
    result.push_back(std::move(loadable_set));
  }
  return result;
}

void StartTest(SystemUnderTest* sut, QuerySampleLibrary* qsl,
               const TestSettings& requested_settings) {
  GlobalLogger().StartIOThread();

  std::ofstream summary_out("mlperf_log_summary.txt");
  std::ofstream detail_out("mlperf_log_detail.txt");
  GlobalLogger().StartLogging(&summary_out, &detail_out);
  std::ofstream trace_out("mlperf_trace.json");
  GlobalLogger().StartNewTrace(&trace_out, PerfClock::now());

  LogLoadgenVersion();
  LogDetail([sut, qsl](AsyncLog& log) {
    log.LogDetail("System Under Test (SUT) name: ", sut->Name());
    log.LogDetail("Query Sample Library (QSL) name: ", qsl->Name());
    log.LogDetail("QSL total size: ", qsl->TotalSampleCount());
    log.LogDetail("QSL performance size: ", qsl->PerformanceSampleCount());
  });
  TestSettingsInternal sanitized_settings(requested_settings);
  sanitized_settings.LogSettings();

  std::vector<std::vector<QuerySampleIndex>> loadable_sets(
      GenerateLoadableSets(qsl, sanitized_settings));

  RunFunctions run_funcs = RunFunctions::Get(sanitized_settings.scenario);

  switch (sanitized_settings.mode) {
    case TestMode::SubmissionRun:
      run_funcs.accuracy(sut, qsl, sanitized_settings, loadable_sets);
      run_funcs.performance(sut, qsl, sanitized_settings, loadable_sets);
      break;
    case TestMode::AccuracyOnly:
      run_funcs.accuracy(sut, qsl, sanitized_settings, loadable_sets);
      break;
    case TestMode::PerformanceOnly:
      run_funcs.performance(sut, qsl, sanitized_settings, loadable_sets);
      break;
    case TestMode::FindPeakPerformance:
      run_funcs.find_peak_performance(sut, qsl, sanitized_settings,
                                      loadable_sets);
      break;
  }

  // Stop tracing after logging so all logs are captured in the trace.
  GlobalLogger().StopLogging();
  GlobalLogger().StopTracing();
  GlobalLogger().StopIOThread();
}

}  // namespace mlperf
