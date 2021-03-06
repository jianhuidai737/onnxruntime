// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "core/graph/onnx_protobuf.h"
#include "runner.h"

#include <fstream>
#include <cmath>

#include <core/common/logging/logging.h>
#include <core/graph/constants.h>
#include <core/platform/env.h>
#include <core/framework/tensorprotoutils.h>
#include "test_allocator.h"
#include "path_lib.h"
#ifdef _WIN32
#include <Windows.h>
#else
#include <pthread.h>
#include <unsupported/Eigen/CXX11/ThreadPool>
#endif
#include <test/compare_mlvalue.h>
#include "TestCase.h"
#include "FixedCountFinishCallback.h"

using namespace onnxruntime;
using ::onnxruntime::common::Status;

void ORT_CALLBACK RunTestCase(ORT_CALLBACK_INSTANCE pci, void* context, ORT_WORK work) {
  OnnxRuntimeCloseThreadpoolWork(work);
  assert(context != nullptr);
  TestCaseTask* task((TestCaseTask*)context);
  ITestCase* info = task->env.tests[task->task_id];
  std::shared_ptr<TestCaseResult> ret;
  try {
    RunSingleTestCase(info, task->env.sf, task->concurrent_runs, task->repeat_count, task->pool, pci, [task](std::shared_ptr<TestCaseResult> result, ORT_CALLBACK_INSTANCE pci) {
      return OnTestCaseFinished(pci, task, result);
    });
    return;
  } catch (std::exception& ex) {
    LOGF_DEFAULT(ERROR, "Test %s failed:%s", info->GetTestCaseName().c_str(), ex.what());
    std::string node_name;
    (void)info->GetNodeName(&node_name);
    ret = std::make_shared<TestCaseResult>(info->GetDataCount(), EXECUTE_RESULT::WITH_EXCEPTION, node_name);
  }
  auto status = OnTestCaseFinished(pci, task, ret);
  if (!status.IsOK()) {
    LOGF_DEFAULT(ERROR, "FATAL ERROR");
    abort();
  }
}

void PTestRunner::Start(ORT_CALLBACK_INSTANCE, size_t concurrent_runs) {
  concurrent_runs = std::min<size_t>(std::max<size_t>(1, concurrent_runs), c_->GetDataCount());
  next_test_to_run = 0;
  for (size_t i = 0; i != concurrent_runs; ++i) {
    if (!ScheduleNew()) {
      throw std::runtime_error("ScheduleNew task failed");
    }
  }
}

bool PTestRunner::ScheduleNew() {
  size_t next_test = next_test_to_run++;
  if (next_test >= c_->GetDataCount()) return false;
  DataTask* t = new DataTask{this, next_test};
  Status st = CreateAndSubmitThreadpoolWork(RunSingleDataItem, t, tpool_);
  if (!st.IsOK()) {
    delete t;
    LOGF_DEFAULT(ERROR, "schedule test task failed: %s\n", st.ErrorMessage().c_str());
    return false;
  }
  return true;
}

void PTestRunner::OnTaskFinished(size_t, EXECUTE_RESULT, ORT_CALLBACK_INSTANCE pci) noexcept {
  try {
    ScheduleNew();
    if (++finished == c_->GetDataCount()) {
      //For each test case, only one DataTask can reach here
      finish(pci);
    }
  } catch (std::exception& ex) {
    LOGF_DEFAULT(ERROR, "%s:unrecoverable error:%s,exit...\n", c_->GetTestCaseName().c_str(), ex.what());
    abort();
  } catch (...) {
    LOGF_DEFAULT(ERROR, "%s:unrecoverable error,exit...\n", c_->GetTestCaseName().c_str());
    abort();
  }
}

PTestRunner::PTestRunner(OrtSession* session1,
                         ITestCase* c, PThreadPool tpool,
                         TestCaseCallBack on_finished1) : DataRunner(session1, c->GetTestCaseName(), c, on_finished1), next_test_to_run(0), finished(0), tpool_(tpool) {
}

void ORT_CALLBACK RunSingleDataItem(ORT_CALLBACK_INSTANCE instance, void* context, ORT_WORK work) {
  OnnxRuntimeCloseThreadpoolWork(work);
  DataTask* task((DataTask*)context);
  PTestRunner* env = task->env;
  const size_t task_id = task->task_id;
  delete task;
  env->RunTask(task_id, instance, true);
}

Status OnTestCaseFinished(ORT_CALLBACK_INSTANCE pci, TestCaseTask* task, std::shared_ptr<TestCaseResult> result) {
  FixedCountFinishCallback* finished = task->env.finished;
  auto task_id = task->task_id;
  bool failed = false;
  {
    std::unique_ptr<TestCaseTask> unused(task);
    TestEnv& env = task->env;
    int next_test = env.next_test_to_run++;
    if (static_cast<size_t>(next_test) < env.tests.size()) {
      //schedule the next TestCase
      std::unique_ptr<TestCaseTask> t(new TestCaseTask{env, next_test, task->concurrent_runs, task->repeat_count, task->pool});
      Status st = CreateAndSubmitThreadpoolWork(RunTestCase, t.get(), task->pool);
      if (st.IsOK()) {
        t.release();
      } else
        return st;
    }
  }
  if (failed)
    return finished->fail(pci);
  return finished->onFinished(task_id, result, pci);
}

//Do not run this function in the thread pool passed in
static Status ParallelRunTests(TestEnv& env, int p_models, size_t current_runs, size_t repeat_count, PThreadPool pool) {
  p_models = (int)std::min<size_t>(p_models, env.tests.size());
  LOGF_DEFAULT(ERROR, "Running tests in parallel: at most %d models at any time", p_models);
  env.next_test_to_run = p_models;
  for (int i = 0; i != p_models; ++i) {
    TestCaseTask* t(new TestCaseTask{env, i, current_runs, repeat_count, pool});
    try {
      auto st = CreateAndSubmitThreadpoolWork(RunTestCase, t, pool);
      if (!st.IsOK()) {
        delete t;
        return st;
      }
    } catch (std::exception&) {
      delete t;
      throw;
    }
  }
  bool ret = env.finished->wait();
  if (!ret) {
    return Status(::onnxruntime::common::ONNXRUNTIME, ::onnxruntime::common::FAIL, "ParallelRunTests failed");
  }
  LOGF_DEFAULT(ERROR, "Running tests finished. Generating report");
  return Status::OK();
}

Status RunTests(TestEnv& env, int p_models, int concurrent_runs, size_t repeat_count, PThreadPool tpool) {
  TestResultStat& stat = env.stat;
  stat.total_model_count = env.tests.size();
  stat.total_test_case_count = std::accumulate(env.tests.begin(), env.tests.end(), static_cast<size_t>(0), [](size_t v, const ITestCase* info) {
    return info->GetDataCount() + v;
  });
  std::vector<std::shared_ptr<TestCaseResult>> results;
  if (p_models > 1 && env.tests.size() > 1) {
    ORT_RETURN_IF_ERROR(ParallelRunTests(env, p_models, concurrent_runs, repeat_count, tpool));
    results = env.finished->getResults();
  } else {
    //run models one by one
    for (size_t i = 0; i != env.tests.size(); ++i) {
      const char* test_case_name = env.tests[i]->GetTestCaseName().c_str();
      ORT_EVENT ev;
      ORT_RETURN_IF_ERROR(CreateOnnxRuntimeEvent(&ev));
      try {
        RunSingleTestCase(env.tests[i], env.sf, concurrent_runs, repeat_count, tpool, nullptr, [repeat_count, &results, ev, concurrent_runs, test_case_name](std::shared_ptr<TestCaseResult> result, ORT_CALLBACK_INSTANCE pci) {
          //TODO:output this information to a xml
          if (concurrent_runs == 1) {
            TIME_SPEC ts = result->GetSpentTime();
            double spent = TimeSpecToSeconds(&ts);
            double spent2 = spent / result->GetExcutionResult().size() / repeat_count;
            LOGF_DEFAULT(ERROR, "Test %s finished in %.3g seconds, took %.3g for each input", test_case_name, spent, spent2);
          }
          results.push_back(result);
          return OnnxRuntimeSetEventWhenCallbackReturns(pci, ev);
        });
        ORT_RETURN_IF_ERROR(WaitAndCloseEvent(ev));
      } catch (std::exception& ex) {
        LOGF_DEFAULT(ERROR, "Test %s failed:%s", test_case_name, ex.what());
        std::string node_name;
        (void)env.tests[i]->GetNodeName(&node_name);
        results.push_back(
            std::make_shared<TestCaseResult>(env.tests[i]->GetDataCount(), EXECUTE_RESULT::WITH_EXCEPTION, node_name));
        OrtCloseEvent(ev);
      }
    }
  }
  for (size_t i = 0; i != env.tests.size(); ++i) {
    if (!results[i]) {
      stat.AddFailedTest(env.tests[i]->GetTestCaseName());
      continue;
    }
    const TestCaseResult& r = *results[i];
    for (const EXECUTE_RESULT res : r.GetExcutionResult()) {
      if (res != EXECUTE_RESULT::SUCCESS && res != EXECUTE_RESULT::NOT_SUPPORT) {
        stat.AddFailedTest(env.tests[i]->GetTestCaseName());
      }
      switch (res) {
        case EXECUTE_RESULT::SUCCESS:
          stat.succeeded++;
          break;
        case EXECUTE_RESULT::INVALID_ARGUMENT:
        case EXECUTE_RESULT::UNKNOWN_ERROR:
          if (!r.node_name.empty()) stat.AddFailedKernels(r.node_name);
          break;
        case EXECUTE_RESULT::INVALID_GRAPH:
          stat.invalid_graph++;
          break;
        case EXECUTE_RESULT::WITH_EXCEPTION:
          stat.throwed_exception++;
          if (!r.node_name.empty()) stat.AddFailedKernels(r.node_name);
          break;
        case EXECUTE_RESULT::RESULT_DIFFERS:
          stat.result_differs++;
          if (!r.node_name.empty()) stat.AddFailedKernels(r.node_name);
          break;
        case EXECUTE_RESULT::MODEL_SHAPE_MISMATCH:
        case EXECUTE_RESULT::SHAPE_MISMATCH:
        case EXECUTE_RESULT::MODEL_TYPE_MISMATCH:
        case EXECUTE_RESULT::TYPE_MISMATCH:
          stat.result_differs++;
          if (!r.node_name.empty()) stat.AddFailedKernels(r.node_name);
          break;
        case EXECUTE_RESULT::NOT_SUPPORT:
          stat.not_implemented++;
          if (!r.node_name.empty()) stat.AddNotImplementedKernels(r.node_name);
          break;
        case EXECUTE_RESULT::LOAD_MODEL_FAILED:
          stat.load_model_failed++;
          if (!r.node_name.empty()) stat.AddFailedKernels(r.node_name);
          break;
        default:
          return ORT_MAKE_STATUS(ONNXRUNTIME, FAIL, "unknown result");
      }
    }
  }
  return common::Status::OK();
}

std::vector<ITestCase*> LoadTests(const std::vector<std::basic_string<PATH_CHAR_TYPE>>& input_paths,
                                  const std::vector<std::basic_string<PATH_CHAR_TYPE>>& whitelisted_test_cases,
                                  OrtAllocator* env) {
  std::vector<ITestCase*> tests;
  std::vector<std::basic_string<PATH_CHAR_TYPE>> paths(input_paths);
  while (!paths.empty()) {
    std::basic_string<PATH_CHAR_TYPE> node_data_root_path = paths.back();
    paths.pop_back();
    std::basic_string<PATH_CHAR_TYPE> my_dir_name = GetLastComponent(node_data_root_path);
    LoopDir(node_data_root_path, [&](const PATH_CHAR_TYPE* filename, FileType f_type) -> bool {
      if (filename[0] == '.') return true;
      if (f_type == FileType::TYPE_DIR) {
        std::basic_string<PATH_CHAR_TYPE> p = ConcatPathComponent<PATH_CHAR_TYPE>(node_data_root_path, filename);
        paths.push_back(p);
        return true;
      }
      std::basic_string<PATH_CHAR_TYPE> filename_str = filename;
      if (!HasExtensionOf(filename_str, ORT_TSTR("onnx"))) return true;

      std::basic_string<PATH_CHAR_TYPE> test_case_name = my_dir_name;
      if (test_case_name.compare(0, 5, ORT_TSTR("test_")) == 0) test_case_name = test_case_name.substr(5);
      if (!whitelisted_test_cases.empty() && std::find(whitelisted_test_cases.begin(), whitelisted_test_cases.end(), test_case_name) == whitelisted_test_cases.end()) {
        return true;
      }
      std::basic_string<PATH_CHAR_TYPE> p = ConcatPathComponent<PATH_CHAR_TYPE>(node_data_root_path, filename_str);

      ITestCase* l = CreateOnnxTestCase(env, ToMBString(test_case_name));
      auto status = l->SetModelPath(p.c_str());
      if (!status.IsOK()) {
        LOGF_DEFAULT(ERROR, "load data from %s failed:%s\n", p.c_str(), status.ErrorMessage().c_str());
        delete l;
        return true;
      }
      tests.push_back(l);
      return true;
    });
  }
  return tests;
}

SeqTestRunner::SeqTestRunner(OrtSession* session1,
                             ITestCase* c, size_t repeat_count,
                             TestCaseCallBack on_finished1) : DataRunner(session1, c->GetTestCaseName(), c, on_finished1), repeat_count_(repeat_count) {
}

DataRunner::DataRunner(OrtSession* session1, const std::string& test_case_name1, ITestCase* c, TestCaseCallBack on_finished1) : test_case_name_(test_case_name1), c_(c), session(session1), on_finished(on_finished1), default_allocator(std::make_unique<MockedOrtAllocator>()) {
  std::string s;
  c->GetNodeName(&s);
  result = std::make_shared<TestCaseResult>(c->GetDataCount(), EXECUTE_RESULT::UNKNOWN_ERROR, s);
  SetTimeSpecToZero(&spent_time_);
}

DataRunner::~DataRunner() {
  OrtReleaseSession(session);
}

void DataRunner::RunTask(size_t task_id, ORT_CALLBACK_INSTANCE pci, bool store_result) {
  EXECUTE_RESULT res = EXECUTE_RESULT::UNKNOWN_ERROR;
  try {
    res = RunTaskImpl(task_id);
  } catch (std::exception& ex) {
    res = EXECUTE_RESULT::WITH_EXCEPTION;
    LOGF_DEFAULT(ERROR, "%s:%s", c_->GetTestCaseName().c_str(), ex.what());
  }
  if (store_result) {
    result->SetResult(task_id, res);
  }
  OnTaskFinished(task_id, res, pci);
}

std::pair<COMPARE_RESULT, std::string> CompareGenericValue(const OrtValue* o, const OrtValue* expected_mlvalue, double per_sample_tolerance, double relative_per_sample_tolerance,
                                                           bool post_processing) {
  return onnxruntime::CompareMLValue(*(MLValue*)o, *(MLValue*)expected_mlvalue, per_sample_tolerance, relative_per_sample_tolerance, post_processing);
}
EXECUTE_RESULT DataRunner::RunTaskImpl(size_t task_id) {
  std::unordered_map<std::string, OrtValue*> feeds;
  common::Status status = c_->LoadTestData(session, task_id, feeds, true);
  if (!status.IsOK()) {
    LOGF_DEFAULT(ERROR, "%s", status.ErrorMessage().c_str());
    return StatusCodeToExecuteResult(status.Code());
  }

  // Create output feed
  size_t output_count;
  ORT_THROW_ON_ERROR(OrtSessionGetOutputCount(session, &output_count));
  std::vector<std::string> output_names(output_count);
  for (size_t i = 0; i != output_count; ++i) {
    char* output_name = nullptr;
    ORT_THROW_ON_ERROR(OrtSessionGetOutputName(session, i, default_allocator.get(), &output_name));
    assert(output_name != nullptr);
    output_names[i] = output_name;
    default_allocator->Free(output_name);
  }

  TIME_SPEC start_time, end_time;
  GetMonotonicTimeCounter(&start_time);
  std::vector<const char*> input_names(feeds.size());
  std::vector<OrtValue*> input_values(feeds.size());
  size_t input_index = 0;
  for (auto& kvp : feeds) {
    input_names[input_index] = kvp.first.c_str();
    input_values[input_index] = kvp.second;
    ++input_index;
  }
  std::vector<OrtValue*> output_values(output_count);
  {
    std::vector<const char*> output_names_raw_ptr(output_count);
    for (size_t i = 0; i != output_count; ++i) {
      output_names_raw_ptr[i] = output_names[i].c_str();
    }
    auto onnx_status = OrtRun(session, nullptr, input_names.data(), input_values.data(), input_index, output_names_raw_ptr.data(), output_count, output_values.data());
    if (onnx_status != nullptr) {
      std::string onnx_runtime_error_message = OrtGetErrorMessage(onnx_status);
      OrtReleaseStatus(onnx_status);
      for (auto& kvp : feeds) {
        OrtReleaseValue(kvp.second);
      }
      throw std::runtime_error(onnx_runtime_error_message);
    }
  }
  GetMonotonicTimeCounter(&end_time);
  AccumulateTimeSpec(&spent_time_, &start_time, &end_time);
  for (auto& kvp : feeds) {
    OrtReleaseValue(kvp.second);
  }
  if (!status.IsOK()) {
    LOGF_DEFAULT(ERROR, "%s:%s\n", test_case_name_.c_str(), status.ErrorMessage().c_str());
    return StatusCodeToExecuteResult(status.Code());
  }

  double per_sample_tolerance, relative_per_sample_tolerance;
  bool post_procesing;

  if (!(status = c_->GetPerSampleTolerance(&per_sample_tolerance)).IsOK()) {
    LOGF_DEFAULT(ERROR, "%s", status.ErrorMessage().c_str());
    return StatusCodeToExecuteResult(status.Code());
  }
  if (!(status = c_->GetRelativePerSampleTolerance(&relative_per_sample_tolerance)).IsOK()) {
    LOGF_DEFAULT(ERROR, "%s", status.ErrorMessage().c_str());
    return StatusCodeToExecuteResult(status.Code());
  }
  if (!(status = c_->GetPostProcessing(&post_procesing)).IsOK()) {
    LOGF_DEFAULT(ERROR, "%s", status.ErrorMessage().c_str());
    return StatusCodeToExecuteResult(status.Code());
  }

  //TODO: if there are no output value files, just skip the validation
  std::unordered_map<std::string, OrtValue*> expected_output_values;
  status = c_->LoadTestData(session, task_id, expected_output_values, false);
  if (!status.IsOK()) {
    LOGF_DEFAULT(ERROR, "%s", status.ErrorMessage().c_str());
    return StatusCodeToExecuteResult(status.Code());
  }
  std::unordered_map<std::string, OrtValue*> name_fetch_output_map;
  std::unordered_map<std::string, const onnx::ValueInfoProto*> name_output_value_info_proto;
  int i = 0;
  for (auto& output_name : output_names) {
    // p_fetches is filled in the order of output_names.
    name_fetch_output_map[output_name] = output_values[i];
    const onnx::ValueInfoProto& infoProto = c_->GetOutputInfoFromModel(i);
    name_output_value_info_proto.insert(std::make_pair(infoProto.name(), &infoProto));
    i++;
  }

  EXECUTE_RESULT res = EXECUTE_RESULT::SUCCESS;
  for (auto& output : expected_output_values) {
    OrtValue* expected_output_value = output.second;
    const std::string& output_name = output.first;
    auto iter = name_fetch_output_map.find(output_name);
    if (iter == name_fetch_output_map.end()) {
      res = EXECUTE_RESULT::INVALID_GRAPH;
      LOGF_DEFAULT(ERROR, "cannot find %s in the outputs", output_name.c_str());
      break;
    }
    OrtValue* actual_output_value = iter->second;
    std::pair<COMPARE_RESULT, std::string> ret = CompareGenericValue(actual_output_value, expected_output_value, per_sample_tolerance, relative_per_sample_tolerance, post_procesing);
    COMPARE_RESULT compare_result = ret.first;
    if (compare_result == COMPARE_RESULT::SUCCESS) {
      const onnx::ValueInfoProto& v = *name_output_value_info_proto[output_name];
      ret = VerifyValueInfo(v, actual_output_value);
      compare_result = ret.first;
      if (compare_result != COMPARE_RESULT::SUCCESS) {
        switch (compare_result) {
          case COMPARE_RESULT::NOT_SUPPORT:
            res = EXECUTE_RESULT::NOT_SUPPORT;
            break;
          case COMPARE_RESULT::SHAPE_MISMATCH:
            res = EXECUTE_RESULT::MODEL_SHAPE_MISMATCH;
            break;
          case COMPARE_RESULT::TYPE_MISMATCH:
            res = EXECUTE_RESULT::MODEL_TYPE_MISMATCH;
            break;
          default:
            res = EXECUTE_RESULT::UNKNOWN_ERROR;
        }
      }
    } else {
      switch (compare_result) {
        case COMPARE_RESULT::NOT_SUPPORT:
          res = EXECUTE_RESULT::NOT_SUPPORT;
          break;
        case COMPARE_RESULT::RESULT_DIFFERS:
          res = EXECUTE_RESULT::RESULT_DIFFERS;
          break;
        case COMPARE_RESULT::SHAPE_MISMATCH:
          res = EXECUTE_RESULT::SHAPE_MISMATCH;
          break;
        case COMPARE_RESULT::TYPE_MISMATCH:
          res = EXECUTE_RESULT::TYPE_MISMATCH;
          break;
        default:
          res = EXECUTE_RESULT::UNKNOWN_ERROR;
      }
    }

    if (compare_result != COMPARE_RESULT::SUCCESS && !ret.second.empty()) {
      LOGS_DEFAULT(ERROR) << test_case_name_ << ":output=" << output_name << ":" << ret.second;
    }
    if (compare_result != COMPARE_RESULT::SUCCESS) {
      break;
    }
  }
  for (auto& kvp : expected_output_values) {
    OrtReleaseValue(kvp.second);
  }
  for (OrtValue* p : output_values) {
    OrtReleaseValue(p);
  }
  return res;
}

void SeqTestRunner::Start(ORT_CALLBACK_INSTANCE pci, size_t) {
  const size_t data_count = c_->GetDataCount();
  for (size_t idx_repeat = 0; idx_repeat != repeat_count_; ++idx_repeat)
    for (size_t idx_data = 0; idx_data != data_count; ++idx_data) {
      RunTask(idx_data, nullptr, idx_repeat == 0);
    }
  finish(pci);
}

void RunSingleTestCase(ITestCase* info, const onnxruntime::SessionOptionsWrapper& sf, size_t concurrent_runs, size_t repeat_count, PThreadPool tpool, ORT_CALLBACK_INSTANCE pci, TestCaseCallBack on_finished) {
  std::shared_ptr<TestCaseResult> ret;
  size_t data_count = info->GetDataCount();
  {
    DataRunner* r = nullptr;
    std::string node_name;
    Status status = info->GetNodeName(&node_name);
    if (!status.IsOK()) {
      LOGF_DEFAULT(ERROR, "load model %s failed:%s\n", info->GetTestCaseName().c_str(), status.ErrorMessage().c_str());
      ret = std::make_shared<TestCaseResult>(data_count, StatusCodeToExecuteResult(status.Code()), node_name);
      goto end;
    }
    auto sf2 = sf.clone();
    sf2.SetSessionLogId(info->GetTestCaseName().c_str());
    std::unique_ptr<OrtSession, decltype(&OrtReleaseSession)> session_object(
        sf2.OrtCreateSession(info->GetModelUrl()), OrtReleaseSession);
    LOGF_DEFAULT(INFO, "testing %s\n", info->GetTestCaseName().c_str());
    //temp hack. Because we have no resource control. We may not have enough memory to run this test in parallel
    if (info->GetTestCaseName() == "coreml_FNS-Candy_ImageNet")
      concurrent_runs = 1;
    if (concurrent_runs > 1 && data_count > 1) {
      r = new PTestRunner(session_object.get(), info, tpool, on_finished);
    } else {
      r = new SeqTestRunner(session_object.get(), info, repeat_count, on_finished);
    }
    session_object.release();
    r->Start(pci, concurrent_runs);
    return;
  }
end:
  on_finished(ret, pci);
}

EXECUTE_RESULT StatusCodeToExecuteResult(int input) {
  switch (input) {
    case common::NOT_IMPLEMENTED:
      return EXECUTE_RESULT::NOT_SUPPORT;
    case common::INVALID_GRAPH:
      return EXECUTE_RESULT::INVALID_GRAPH;
    case common::INVALID_ARGUMENT:
      return EXECUTE_RESULT::INVALID_ARGUMENT;
    default:
      return EXECUTE_RESULT::UNKNOWN_ERROR;
  }
}
