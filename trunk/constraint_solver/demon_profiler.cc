// Copyright 2010 Google
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <math.h>
#include <limits>
#include "base/logging.h"
#include "base/stl_util-inl.h"
#include <iostream>
#include <fstream>
#include "constraint_solver/constraint_solveri.h"
#include "constraint_solver/demon_profiler.pb.h"

namespace operations_research {
// ------------ Demon Info -----------
class DemonProfiler;


class DemonProfilerTest;

// DemonMonitor manages the profiling of demons and allows access to gathered
// data. Add this class as a parameter to Solver and access its information
// after the end of a search.
class DemonMonitor {
 public:
  DemonMonitor()
    : active_constraint_(NULL),
      active_demon_(NULL),
start_time_(WallTimer::GetTimeInMicroSeconds()) {}
  ~DemonMonitor() {
    STLDeleteContainerPairSecondPointers(constraint_map_.begin(),
                                         constraint_map_.end());
  }

  int64 CurrentTime() const {
return WallTimer::GetTimeInMicroSeconds() - start_time_;
  }

  void StartInitialPropagation(Constraint* const constraint) {
    CHECK(active_constraint_ == NULL);
    CHECK(active_demon_ == NULL);
    CHECK_NOTNULL(constraint);
    ConstraintRuns* const ct_run = new ConstraintRuns;
    ct_run->set_constraint_id(constraint->DebugString());
    ct_run->set_initial_propagation_start_time(CurrentTime());
    active_constraint_ = constraint;
    constraint_map_[constraint] = ct_run;
  }

  void EndInitialPropagation(Constraint* const constraint) {
    CHECK_NOTNULL(active_constraint_);
    CHECK(active_demon_ == NULL);
    CHECK_NOTNULL(constraint);
    CHECK_EQ(constraint, active_constraint_);
    ConstraintRuns* const ct_run = constraint_map_[constraint];
    CHECK_NOTNULL(ct_run);
    ct_run->set_initial_propagation_end_time(CurrentTime());
    ct_run->set_failures(0);
    active_constraint_ = NULL;
  }

  void RegisterDemon(Demon* const demon) {
    if (demon_map_.find(demon) == demon_map_.end()) {
      CHECK_NOTNULL(active_constraint_);
      CHECK(active_demon_ == NULL);
      CHECK_NOTNULL(demon);
      ConstraintRuns* const ct_run = constraint_map_[active_constraint_];
      DemonRuns* const demon_run = ct_run->add_demons();
      demon_run->set_demon_id(demon->DebugString());
      demon_run->set_failures(0);
      demon_map_[demon] = demon_run;
      demons_per_constraint_[active_constraint_].push_back(demon_run);
    }
  }

  void StartDemonRun(Demon* const demon) {
    CHECK(active_demon_ == NULL);
    CHECK_NOTNULL(demon);
    active_demon_ = demon;
    DemonRuns* const demon_run = demon_map_[active_demon_];
    demon_run->add_start_time(CurrentTime());
  }

  void EndDemonRun(Demon* const demon) {
    CHECK_EQ(active_demon_, demon);
    CHECK_NOTNULL(demon);
    DemonRuns* const demon_run = demon_map_[active_demon_];
    CHECK_NOTNULL(demon_run);
    demon_run->add_end_time(CurrentTime());
    active_demon_ = NULL;
  }

  void RaiseFailure() {
    if (active_constraint_ != NULL) {
      CHECK_NOTNULL(active_demon_);
      ConstraintRuns* const ct_run = constraint_map_[active_constraint_];
      CHECK_NOTNULL(ct_run);
      ct_run->set_initial_propagation_end_time(CurrentTime());
      ct_run->set_failures(1);
      active_constraint_ = NULL;
    } else if (active_demon_ != NULL) {
      DemonRuns* const demon_run = demon_map_[active_demon_];
      demon_run->add_end_time(CurrentTime());
      demon_run->set_failures(demon_run->failures() + 1);
      active_demon_ = NULL;
    }
  }

  // Useful for unit tests.
  void AddFakeRun(Demon* const demon,
                  int64 start_time,
                  int64 end_time,
                  bool is_fail) {
    CHECK_NOTNULL(demon);
    DemonRuns* const demon_run = demon_map_[demon];
    CHECK_NOTNULL(demon_run);
    demon_run->add_start_time(start_time);
    demon_run->add_end_time(end_time);
    if (is_fail) {
      demon_run->set_failures(demon_run->failures() + 1);
    }
  }

  // Exports collected data as human-readable text.
  void PrintOverview(const string& filename) {
    const char* const kConstraintFormat =
        "  - Constraint: %s\n                failures=%"
        GG_LL_FORMAT "d, initial propagation runtime=%" GG_LL_FORMAT
        "d us, demons=%d, demon invocations=%" GG_LL_FORMAT
        "d, total demon runtime=%" GG_LL_FORMAT "d us\n";
    const char* const kDemonFormat =
        "    - Demon: %s\n             invocations=%" GG_LL_FORMAT
        "d, failures=%" GG_LL_FORMAT "d, total runtime=%" GG_LL_FORMAT
        "d us, [average=%.2lf, median=%.2lf, stddev=%.2lf]\n";
    std::ofstream file(filename.c_str());
    if (!file.is_open()) {
      LG << "Failed to gain write access to file: " << filename;
    } else {

      for (hash_map<Constraint*, ConstraintRuns*>::const_iterator it =
               constraint_map_.begin();
           it != constraint_map_.end();
           ++it) {
        int64 fails = 0;
        int64 demon_invocations = 0;
        int64 initial_propagation_runtime = 0;
        int64 total_demon_runtime = 0;
        int demon_count = 0;
        ExportInformation(it->first,
                          &fails,
                          &initial_propagation_runtime,
                          &demon_invocations,
                          &total_demon_runtime,
                          &demon_count);
        const string constraint_message =
            StringPrintf(kConstraintFormat,
                         it->first->DebugString().c_str(),
                         fails,
                         initial_propagation_runtime,
                         demon_count,
                         demon_invocations,
                         total_demon_runtime);
        file << constraint_message;
        const vector<DemonRuns*>& demons = demons_per_constraint_[it->first];
        const int demon_size = demons.size();
        for (int demon_index = 0; demon_index < demon_size; ++demon_index) {
          DemonRuns* const demon_runs = demons[demon_index];
          int64 invocations = 0;
          int64 fails = 0;
          int64 runtime = 0;
          double mean_runtime = 0;
          double median_runtime = 0;
          double standard_deviation = 0.0;
          ExportInformation(demon_runs,
                            &invocations,
                            &fails,
                            &runtime,
                            &mean_runtime,
                            &median_runtime,
                            &standard_deviation);
          const string runs = StringPrintf(kDemonFormat,
                                           demon_runs->demon_id().c_str(),
                                           invocations,
                                           fails,
                                           runtime,
                                           mean_runtime,
                                           median_runtime,
                                           standard_deviation);
          file << runs;
        }
      }
    }
    file.close();
  }

  // Restarts a search and clears all previously collected information.
  void RestartSearch() {
    STLDeleteContainerPairSecondPointers(constraint_map_.begin(),
                                         constraint_map_.end());
    constraint_map_.clear();
    demon_map_.clear();
    demons_per_constraint_.clear();
  }

  // Export Information
  void ExportInformation(Constraint* const constraint,
                         int64* const fails,
                         int64* const initial_propagation_runtime,
                         int64* const demon_invocations,
                         int64* const total_demon_runtime,
                         int* demons) {
    CHECK_NOTNULL(constraint);
    ConstraintRuns* const ct_run = constraint_map_[constraint];
    CHECK_NOTNULL(ct_run);
    *demon_invocations = 0;
    *fails = ct_run->failures();
    *initial_propagation_runtime = ct_run->initial_propagation_end_time() -
        ct_run->initial_propagation_start_time();
    *total_demon_runtime = 0;

    // Gather information.
    *demons = ct_run->demons_size();
    CHECK_EQ(*demons, demons_per_constraint_[constraint].size());
    for (int demon_index = 0; demon_index < *demons; ++demon_index) {
      const DemonRuns& demon_runs = ct_run->demons(demon_index);
      *fails += demon_runs.failures();
      CHECK_EQ(demon_runs.start_time_size(), demon_runs.end_time_size());
      const int runs = demon_runs.start_time_size();
      *demon_invocations += runs;
      for (int run_index = 0; run_index < runs; ++run_index) {
        const int64 demon_time = demon_runs.end_time(run_index) -
            demon_runs.start_time(run_index);
        *total_demon_runtime += demon_time;
      }
    }
  }

  void ExportInformation(DemonRuns* const demon_runs,
                         int64* const demon_invocations,
                         int64* const fails,
                         int64* const total_demon_runtime,
                         double* const mean_demon_runtime,
                         double* const median_demon_runtime,
                         double* const stddev_demon_runtime) {
    CHECK_NOTNULL(demon_runs);
    CHECK_EQ(demon_runs->start_time_size(), demon_runs->end_time_size());

    const int runs = demon_runs->start_time_size();
    *demon_invocations = runs;
    *fails = demon_runs->failures();
    *total_demon_runtime = 0;
    *mean_demon_runtime = 0.0;
    *median_demon_runtime = 0.0;
    *stddev_demon_runtime = 0.0;
    vector<double> runtimes;
    for (int run_index = 0; run_index < runs; ++run_index) {
      const int64 demon_time = demon_runs->end_time(run_index) -
          demon_runs->start_time(run_index);
      *total_demon_runtime += demon_time;
      runtimes.push_back(demon_time);
    }
    // Compute mean.
    if (runtimes.size()) {
      *mean_demon_runtime = (1.0L * *total_demon_runtime) / runtimes.size();

      // Compute median.
      sort(runtimes.begin(), runtimes.end());
      const int pivot = runtimes.size() / 2;

      if (runtimes.size() == 1) {
        *median_demon_runtime = runtimes[0];
      } else {
        *median_demon_runtime = runtimes.size() % 2 == 1 ?
            runtimes[pivot] :
            (runtimes[pivot - 1] + runtimes[pivot]) / 2.0;
      }

      // Compute standard deviation.
      double total_deviation = 0.0f;

      for (int i = 0; i < runtimes.size(); ++i) {
        total_deviation += pow(runtimes[i] - *mean_demon_runtime, 2);
      }

      *stddev_demon_runtime = sqrt(total_deviation / runtimes.size());
    }
  }
 private:
  Constraint* active_constraint_;
  Demon* active_demon_;
  const int64 start_time_;
  hash_map<Constraint*, ConstraintRuns*> constraint_map_;
  hash_map<Demon*, DemonRuns*> demon_map_;
  hash_map<Constraint*, vector<DemonRuns*> > demons_per_constraint_;
};

// DemonProfiler is a wrapper for demons and add profiling capabilities to
// track the usage and perfomance of the demons.
class DemonProfiler : public Demon {
 public:
  DemonProfiler(Demon* const demon, DemonMonitor* const monitor)
      : demon_(demon), monitor_(monitor) {
    CHECK_NOTNULL(demon);
    CHECK_NOTNULL(monitor);
  }

  // This is the main callback of the demon.
  void Run(Solver* const s) {
    monitor_->StartDemonRun(demon_);
    demon_->Run(s);
    monitor_->EndDemonRun(demon_);
  }

  // Returns the priority of the demon.
  Solver::DemonPriority priority() const {
    return demon_->priority();
  }

  // DebugString of the contained demon.
  string DebugString() const {
    return StringPrintf("demon_profiler<%s>", demon_->DebugString().c_str());
  }
 private:
  Demon* const demon_;
  DemonMonitor* const monitor_;
};


void Solver::NotifyFailureToDemonMonitor() {
  if (parameters_.profile_level != SolverParameters::NO_PROFILING) {
    CHECK_NOTNULL(demon_monitor_);
    demon_monitor_->RaiseFailure();
  }
}

void Solver::ExportProfilingOverview(const string& filename) {
  CHECK_NE(SolverParameters::NO_PROFILING, parameters_.profile_level);
  CHECK_NOTNULL(demon_monitor_);
  demon_monitor_->PrintOverview(filename);
}

// ----- Exported Functions -----

DemonMonitor* BuildDemonMonitor(SolverParameters::ProfileLevel level) {
  switch (level) {
    case SolverParameters::NO_PROFILING:
      return NULL;
    default:
      return new DemonMonitor;
  }
}

void DeleteDemonMonitor(DemonMonitor* const monitor) {
  delete monitor;
}

void BuildDemonProfiler(Solver* const solver,
                        Demon* const demon,
                        DemonMonitor* const monitor) {
  monitor->RegisterDemon(demon);
  solver->RevAlloc(new DemonProfiler(demon, monitor));
}

Demon* Solver::RegisterDemon(Demon* const d) {
  CHECK_NOTNULL(d);
  if (parameters_.profile_level != SolverParameters::NO_PROFILING &&
      state_ != IN_SEARCH) {
    CHECK_NOTNULL(demon_monitor_);
    demon_monitor_->RegisterDemon(d);
    return RevAlloc(new DemonProfiler(d, demon_monitor_));
  } else {
    return d;
  }
}

void DemonMonitorStartInitialPropagation(DemonMonitor* const monitor,
                                         Constraint* const constraint) {
  monitor->StartInitialPropagation(constraint);
}

void DemonMonitorEndInitialPropagation(DemonMonitor* const monitor,
                                       Constraint* const constraint) {
  monitor->EndInitialPropagation(constraint);
}

void DemonMonitorRestartSearch(DemonMonitor* const monitor) {
  monitor->RestartSearch();
}

// ----- Exported Methods for Unit Tests -----

void DeleteDemonProfiler(DemonProfiler* const profiler) {
  delete profiler;
}

void DemonMonitorAddFakeRun(DemonMonitor* const monitor,
                            Demon* const demon,
                            int64 start_time,
                            int64 end_time,
                            bool is_fail) {
  monitor->AddFakeRun(demon, start_time, end_time, is_fail);
}

void DemonMonitorExportInformation(DemonMonitor* const monitor,
                                   Constraint* const constraint,
                                   int64* const fails,
                                   int64* const initial_propagation_runtime,
                                   int64* const demon_invocations,
                                   int64* const total_demon_runtime,
                                   int* const demon_count) {
  monitor->ExportInformation(constraint,
                             fails,
                             initial_propagation_runtime,
                             demon_invocations,
                             total_demon_runtime,
                             demon_count);
}
}  // namespace
