// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include <core/common/make_unique.h>
#include "core/session/onnxruntime_cxx_api.h"
#include "providers.h"
#include <memory>
#include <vector>
#include <iostream>
#include <fstream>
#include <sstream>
#include <atomic>
#include <gtest/gtest.h>
#include "test_allocator.h"
#include "test_fixture.h"
#include "onnx_protobuf.h"

struct Input {
  const char* name;
  std::vector<int64_t> dims;
  std::vector<float> values;
};

template <typename OutT>
void RunSession(OrtAllocator* allocator, Ort::Session& session_object,
                const std::vector<Input>& inputs,
                const char* output_name,
                const std::vector<int64_t>& dims_y,
                const std::vector<OutT>& values_y,
                Ort::Value* output_tensor) {
  std::vector<Ort::Value> ort_inputs;
  std::vector<const char*> input_names;
  for (size_t i = 0; i < inputs.size(); i++) {
    input_names.emplace_back(inputs[i].name);
    ort_inputs.emplace_back(Ort::Value::CreateTensor<float>(allocator->Info(allocator), const_cast<float*>(inputs[i].values.data()), inputs[i].values.size(), inputs[i].dims.data(), inputs[i].dims.size()));
  }

  std::vector<Ort::Value> ort_outputs;
  if (output_tensor)
    session_object.Run(Ort::RunOptions{nullptr}, input_names.data(), ort_inputs.data(), ort_inputs.size(), &output_name, output_tensor, 1);
  else {
    ort_outputs = session_object.Run(Ort::RunOptions{nullptr}, input_names.data(), ort_inputs.data(), ort_inputs.size(), &output_name, 1);
    ASSERT_EQ(ort_outputs.size(), 1);
    output_tensor = &ort_outputs[0];
  }

  auto type_info = output_tensor->GetTensorTypeAndShapeInfo();
  ASSERT_EQ(type_info.GetShape(), dims_y);
  size_t total_len = type_info.GetElementCount();
  ASSERT_EQ(values_y.size(), total_len);

  OutT* f = output_tensor->GetTensorMutableData<OutT>();
  for (size_t i = 0; i != total_len; ++i) {
    ASSERT_EQ(values_y[i], f[i]);
  }
}


template <typename T, typename OutT>
void TestInference(Ort::Env& env, T model_uri,
                   const std::vector<Input>& inputs,
                   const char* output_name,
                   const std::vector<int64_t>& expected_dims_y,
                   const std::vector<OutT>& expected_values_y,
                   int provider_type, 
                   OrtCustomOpDomain* custom_op_domain_ptr,
                   const char* custom_op_library_filename) {
  Ort::SessionOptions session_options;

  if (provider_type == 1) {
#ifdef USE_CUDA
    Ort::ThrowOnError(OrtSessionOptionsAppendExecutionProvider_CUDA(session_options, 0));
    std::cout << "Running simple inference with cuda provider" << std::endl;
#else
    return;
#endif
  } else if (provider_type == 2) {
#ifdef USE_MKLDNN
    Ort::ThrowOnError(OrtSessionOptionsAppendExecutionProvider_Mkldnn(session_options, 1));
    std::cout << "Running simple inference with mkldnn provider" << std::endl;
#else
    return;
#endif
  } else if (provider_type == 3) {
#ifdef USE_NUPHAR
    Ort::ThrowOnError(OrtSessionOptionsAppendExecutionProvider_Nuphar(session_options, /*allow_unaligned_buffers*/ 1, ""));
    std::cout << "Running simple inference with nuphar provider" << std::endl;
#else
    return;
#endif
  } else {
    std::cout << "Running simple inference with default provider" << std::endl;
  }
  if (custom_op_domain_ptr) {
    session_options.Add(custom_op_domain_ptr);
  }

  if (custom_op_library_filename){
    void* library_handle = nullptr; // leak this, no harm. 
    Ort::GetApi().RegisterCustomOpsLibrary((OrtSessionOptions*)session_options, custom_op_library_filename, &library_handle);
  }

  Ort::Session session(env, model_uri, session_options);
  auto default_allocator = onnxruntime::make_unique<MockedOrtAllocator>();
  // Now run
  //without preallocated output tensor
  RunSession<OutT>(default_allocator.get(),
             session,
             inputs,
             output_name,
             expected_dims_y,
             expected_values_y,
             nullptr);
  //with preallocated output tensor
  Ort::Value value_y = Ort::Value::CreateTensor<float>(default_allocator.get(), expected_dims_y.data(), expected_dims_y.size());

  //test it twice
  for (int i = 0; i != 2; ++i)
    RunSession<OutT>(default_allocator.get(),
               session,
               inputs,
               output_name,
               expected_dims_y,
               expected_values_y,
               &value_y);
}

static constexpr PATH_TYPE MODEL_URI = TSTR("testdata/mul_1.onnx");
static constexpr PATH_TYPE CUSTOM_OP_MODEL_URI = TSTR("testdata/foo_1.onnx");
static constexpr PATH_TYPE CUSTOM_OP_LIBRARY_TEST_MODEL_URI = TSTR("testdata/custom_op_library/custom_op_test.onnx");
static constexpr PATH_TYPE OVERRIDABLE_INITIALIZER_MODEL_URI = TSTR("testdata/overridable_initializer.onnx");
static constexpr PATH_TYPE NAMED_AND_ANON_DIM_PARAM_URI = TSTR("testdata/capi_symbolic_dims.onnx");

#ifdef ENABLE_LANGUAGE_INTEROP_OPS
static constexpr PATH_TYPE PYOP_FLOAT_MODEL_URI = TSTR("testdata/pyop_1.onnx");
#endif

class CApiTestWithProvider : public CApiTest,
                             public ::testing::WithParamInterface<int> {
};

// Tests that the Foo::Bar() method does Abc.
TEST_P(CApiTestWithProvider, simple) {
  // simple inference test
  // prepare inputs
  std::vector<Input> inputs(1);
  Input& input = inputs.back();
  input.name = "X";
  input.dims = {3, 2};
  input.values = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f};

  // prepare expected inputs and outputs
  std::vector<int64_t> expected_dims_y = {3, 2};
  std::vector<float> expected_values_y = {1.0f, 4.0f, 9.0f, 16.0f, 25.0f, 36.0f};

  TestInference<PATH_TYPE, float>(env_, MODEL_URI, inputs, "Y", expected_dims_y, expected_values_y, GetParam(), nullptr, nullptr);
}

TEST_F(CApiTest, dim_param) {
  Ort::SessionOptions session_options;
  Ort::Session session(env_, NAMED_AND_ANON_DIM_PARAM_URI, session_options);

  auto in0 = session.GetInputTypeInfo(0);
  auto in0_ttsi = in0.GetTensorTypeAndShapeInfo();

  auto num_input_dims = in0_ttsi.GetDimensionsCount();
  ASSERT_GE(num_input_dims, 1);
  // reading 1st dimension only so don't need to malloc int64_t* or const char** values for the Get*Dimensions calls
  int64_t dim_value = 0;
  const char* dim_param = nullptr;
  in0_ttsi.GetDimensions(&dim_value, 1);
  in0_ttsi.GetSymbolicDimensions(&dim_param, 1);
  ASSERT_EQ(dim_value, -1) << "symbolic dimension should be -1";
  ASSERT_EQ(strcmp(dim_param, "n"), 0) << "Expected 'n'. Got: " << dim_param;

  auto out0 = session.GetOutputTypeInfo(0);
  auto out0_ttsi = out0.GetTensorTypeAndShapeInfo();
  auto num_output_dims = out0_ttsi.GetDimensionsCount();
  ASSERT_EQ(num_output_dims, 1);

  out0_ttsi.GetDimensions(&dim_value, 1);
  out0_ttsi.GetSymbolicDimensions(&dim_param, 1);
  ASSERT_EQ(dim_value, -1) << "symbolic dimension should be -1";
  ASSERT_EQ(strcmp(dim_param, ""), 0);
}

INSTANTIATE_TEST_CASE_P(CApiTestWithProviders,
                        CApiTestWithProvider,
                        ::testing::Values(0, 1, 2, 3, 4));

struct OrtTensorDimensions : std::vector<int64_t> {
  OrtTensorDimensions(Ort::CustomOpApi ort, const OrtValue* value) {
    OrtTensorTypeAndShapeInfo* info = ort.GetTensorTypeAndShape(value);
    std::vector<int64_t>::operator=(ort.GetTensorShape(info));
    ort.ReleaseTensorTypeAndShapeInfo(info);
  }
};

// Once we use C++17 this could be replaced with std::size
template <typename T, size_t N>
constexpr size_t countof(T (&)[N]) { return N; }

struct MyCustomKernel {
  MyCustomKernel(Ort::CustomOpApi ort, const OrtKernelInfo* /*info*/) : ort_(ort) {
  }

  void Compute(OrtKernelContext* context) {
    // Setup inputs
    const OrtValue* input_X = ort_.KernelContext_GetInput(context, 0);
    const OrtValue* input_Y = ort_.KernelContext_GetInput(context, 1);
    const float* X = ort_.GetTensorData<float>(input_X);
    const float* Y = ort_.GetTensorData<float>(input_Y);

    // Setup output
    OrtTensorDimensions dimensions(ort_, input_X);
    OrtValue* output = ort_.KernelContext_GetOutput(context, 0, dimensions.data(), dimensions.size());
    float* out = ort_.GetTensorMutableData<float>(output);

    OrtTensorTypeAndShapeInfo* output_info = ort_.GetTensorTypeAndShape(output);
    int64_t size = ort_.GetTensorShapeElementCount(output_info);
    ort_.ReleaseTensorTypeAndShapeInfo(output_info);

    // Do computation
    for (int64_t i = 0; i < size; i++) {
      out[i] = X[i] + Y[i];
    }
  }

 private:
  Ort::CustomOpApi ort_;
};

struct MyCustomOp : Ort::CustomOpBase<MyCustomOp, MyCustomKernel> {
  void* CreateKernel(Ort::CustomOpApi api, const OrtKernelInfo* info) { return new MyCustomKernel(api, info); };
  const char* GetName() const { return "Foo"; };

  size_t GetInputTypeCount() const { return 2; };
  ONNXTensorElementDataType GetInputType(size_t /*index*/) const { return ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT; };

  size_t GetOutputTypeCount() const { return 1; };
  ONNXTensorElementDataType GetOutputType(size_t /*index*/) const { return ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT; };
};

TEST_F(CApiTest, custom_op_handler) {
  std::cout << "Running custom op inference" << std::endl;

  std::vector<Input> inputs(1);
  Input& input = inputs[0];
  input.name = "X";
  input.dims = {3, 2};
  input.values = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f};

  // prepare expected inputs and outputs
  std::vector<int64_t> expected_dims_y = {3, 2};
  std::vector<float> expected_values_y = {2.0f, 4.0f, 6.0f, 8.0f, 10.0f, 12.0f};

  MyCustomOp custom_op;
  Ort::CustomOpDomain custom_op_domain("");
  custom_op_domain.Add(&custom_op);

  TestInference<PATH_TYPE, float>(env_, CUSTOM_OP_MODEL_URI, inputs, "Y", expected_dims_y, expected_values_y, 0, custom_op_domain, nullptr);
}

TEST_F(CApiTest, test_custom_op_library) {
  std::cout << "Running inference using custom op shared library" << std::endl;

  std::vector<Input> inputs(2);
  inputs[0].name = "input_1";
  inputs[0].dims = {3, 5};
  inputs[0].values = {1.1f,   2.2f,   3.3f,   4.4f,   5.5f, 
                      6.6f,   7.7f,   8.8f,   9.9f,   10.0f,
                      11.1f,  12.2f,  13.3f,  14.4f,  15.5f};
  inputs[1].name = "input_2";
  inputs[1].dims = {3, 5};
  inputs[1].values = {15.5f,   14.4f,   13.3f,   12.2f,   11.1f, 
                      10.0f,   9.9f,    8.8f,    7.7f,    6.6f,
                      5.5f,    4.4f,    3.3f,    2.2f,    1.1f};

  // prepare expected inputs and outputs
  std::vector<int64_t> expected_dims_y = {3, 5};
  std::vector<int32_t> expected_values_y = 
                    {17, 17, 17, 17, 17,
                     17, 18, 18, 18, 17,
                     17, 17, 17, 17, 17};

  std::string lib_name;
  #if defined(_WIN32)
    lib_name = "custom_op_library.dll";
  #elif defined(__APPLE__)
    lib_name = "libcustom_op_library.dylib";
  #else
    lib_name = "libcustom_op_library.so";
  #endif

  TestInference<PATH_TYPE, int32_t>(env_, CUSTOM_OP_LIBRARY_TEST_MODEL_URI, inputs, "output", expected_dims_y, expected_values_y, 0, nullptr, lib_name.c_str());
}



#if defined(ENABLE_LANGUAGE_INTEROP_OPS) && !defined(_WIN32)  // on windows, PYTHONHOME must be set explicitly
TEST_F(CApiTest, DISABLED_test_pyop) {
  std::cout << "Test model with pyop" << std::endl;
  std::ofstream module("mymodule.py");
  module << "class MyKernel:" << std::endl;
  module << "\t"
         << "def __init__(self,A,B,C):" << std::endl;
  module << "\t\t"
         << "self.a,self.b,self.c = A,B,C" << std::endl;
  module << "\t"
         << "def compute(self,x):" << std::endl;
  module << "\t\t"
         << "return x*2" << std::endl;
  module.close();
  std::vector<Input> inputs(1);
  Input& input = inputs[0];
  input.name = "X";
  input.dims = {2, 2};
  input.values = {1.0f, 2.0f, 3.0f, 4.0f};
  std::vector<int64_t> expected_dims_y = {2, 2};
  std::vector<float> expected_values_y = {2.0f, 4.0f, 6.0f, 8.0f};
  TestInference<PATH_TYPE, float>(env_, PYOP_FLOAT_MODEL_URI, inputs, "Y", expected_dims_y, expected_values_y, 0, nullptr, nullptr);
}
#endif

#ifdef ORT_RUN_EXTERNAL_ONNX_TESTS
TEST_F(CApiTest, create_session_without_session_option) {
  constexpr PATH_TYPE model_uri = TSTR("../models/opset8/test_squeezenet/model.onnx");
  Ort::Session ret(env_, model_uri, Ort::SessionOptions{nullptr});
  ASSERT_NE(nullptr, ret);
}
#endif

TEST_F(CApiTest, create_tensor) {
  const char* s[] = {"abc", "kmp"};
  int64_t expected_len = 2;
  auto default_allocator = onnxruntime::make_unique<MockedOrtAllocator>();

  Ort::Value tensor = Ort::Value::CreateTensor(default_allocator.get(), &expected_len, 1, ONNX_TENSOR_ELEMENT_DATA_TYPE_STRING);

  Ort::ThrowOnError(Ort::GetApi().FillStringTensor(tensor, s, expected_len));
  auto shape_info = tensor.GetTensorTypeAndShapeInfo();

  int64_t len = shape_info.GetElementCount();
  ASSERT_EQ(len, expected_len);
  std::vector<int64_t> shape_array(len);

  size_t data_len = tensor.GetStringTensorDataLength();
  std::string result(data_len, '\0');
  std::vector<size_t> offsets(len);
  tensor.GetStringTensorContent((void*)result.data(), data_len, offsets.data(), offsets.size());
}

TEST_F(CApiTest, create_tensor_with_data) {
  float values[] = {3.0f, 1.0f, 2.f, 0.f};
  constexpr size_t values_length = sizeof(values) / sizeof(values[0]);

  Ort::MemoryInfo info("Cpu", OrtDeviceAllocator, 0, OrtMemTypeDefault);

  std::vector<int64_t> dims = {4};
  Ort::Value tensor = Ort::Value::CreateTensor<float>(info, values, values_length, dims.data(), dims.size());

  float* new_pointer = tensor.GetTensorMutableData<float>();
  ASSERT_EQ(new_pointer, values);

  auto type_info = tensor.GetTypeInfo();
  auto tensor_info = type_info.GetTensorTypeAndShapeInfo();

  ASSERT_NE(tensor_info, nullptr);
  ASSERT_EQ(1, tensor_info.GetDimensionsCount());
}

TEST_F(CApiTest, override_initializer) {
  Ort::MemoryInfo info("Cpu", OrtDeviceAllocator, 0, OrtMemTypeDefault);
  auto allocator = onnxruntime::make_unique<MockedOrtAllocator>();
  // CreateTensor which is not owning this ptr
  bool Label_input[] = {true};
  std::vector<int64_t> dims = {1, 1};
  Ort::Value label_input_tensor = Ort::Value::CreateTensor<bool>(info, Label_input, 1U, dims.data(), dims.size());

  std::string f2_data{"f2_string"};
  // Place a string into Tensor OrtValue and assign to the
  Ort::Value f2_input_tensor = Ort::Value::CreateTensor(allocator.get(), dims.data(), dims.size(), ONNX_TENSOR_ELEMENT_DATA_TYPE_STRING);
  // No C++ Api to either create a string Tensor or to fill one with string, so we use C
  const char* const input_char_string[] = {f2_data.c_str()};
  Ort::ThrowOnError(Ort::GetApi().FillStringTensor(static_cast<OrtValue*>(f2_input_tensor), input_char_string, 1U));

  Ort::SessionOptions session_options;
  Ort::Session session(env_, OVERRIDABLE_INITIALIZER_MODEL_URI, session_options);

  // Get Overrideable initializers
  size_t init_count = session.GetOverridableInitializerCount();
  ASSERT_EQ(init_count, 1U);

  char* f1_init_name = session.GetOverridableInitializerName(0, allocator.get());
  ASSERT_TRUE(strcmp("F1", f1_init_name) == 0);
  allocator->Free(f1_init_name);

  Ort::TypeInfo init_type_info = session.GetOverridableInitializerTypeInfo(0);
  ASSERT_EQ(ONNX_TYPE_TENSOR, init_type_info.GetONNXType());

  // Let's override the initializer
  float f11_input_data[] = {2.0f};
  Ort::Value f11_input_tensor = Ort::Value::CreateTensor<float>(info, f11_input_data, 1U, dims.data(), dims.size());

  std::vector<Ort::Value> ort_inputs;
  ort_inputs.push_back(std::move(label_input_tensor));
  ort_inputs.push_back(std::move(f2_input_tensor));
  ort_inputs.push_back(std::move(f11_input_tensor));

  std::vector<const char*> input_names = {"Label", "F2", "F1"};

  const char* const output_names[] = {"Label0", "F20", "F11"};
  std::vector<Ort::Value> ort_outputs = session.Run(Ort::RunOptions{nullptr}, input_names.data(),
                                                    ort_inputs.data(), ort_inputs.size(),
                                                    output_names, countof(output_names));

  ASSERT_EQ(ort_outputs.size(), 3U);
  // Expecting the last output would be the overridden value of the initializer
  auto type_info = ort_outputs[2].GetTensorTypeAndShapeInfo();
  ASSERT_EQ(type_info.GetShape(), dims);
  ASSERT_EQ(type_info.GetElementType(), ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT);
  ASSERT_EQ(type_info.GetElementCount(), 1U);
  float* output_data = ort_outputs[2].GetTensorMutableData<float>();
  ASSERT_EQ(*output_data, f11_input_data[0]);
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  int ret = RUN_ALL_TESTS();
  //TODO: Linker on Mac OS X is kind of strange. The next line of code will trigger a crash
#ifndef __APPLE__
  ::google::protobuf::ShutdownProtobufLibrary();
#endif
  return ret;
}
