// Copyright (c) 2019 PaddlePaddle Authors. All Rights Reserved.
//
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

#include "lite/kernels/npu/bridges/graph.h"
#include "lite/kernels/npu/bridges/registry.h"
#include "lite/kernels/npu/bridges/utility.h"

namespace paddle {
namespace lite {
namespace subgraph {
namespace npu {

int ConvTransposeConverter(void* ctx, OpLite* op, KernelBase* kernel) {
  CHECK(ctx != nullptr);
  CHECK(op != nullptr);
  auto graph = static_cast<Graph*>(ctx);
  auto op_info = op->op_info();
  auto op_type = op_info->Type();
  auto scope = op->scope();
  VLOG(3) << "[NPU] Converting " << op_type << "... ";

  // Get input, output and op attributes
  auto input_name = op_info->Input("Input").front();
  auto input_type = kernel->GetInputDeclType("Input");
  CHECK(input_type->precision() == PRECISION(kFloat));
  CHECK(input_type->layout() == DATALAYOUT(kNCHW));
  auto input = scope->FindMutableTensor(input_name);
  auto input_dims = input->dims();
  CHECK_EQ(input_dims.size(), 4);
  auto filter_name = op_info->Input("Filter").front();
  auto filter_type = kernel->GetInputDeclType("Filter");
  CHECK(filter_type->precision() == PRECISION(kFloat));
  CHECK(filter_type->layout() == DATALAYOUT(kNCHW));
  auto filter = scope->FindMutableTensor(filter_name);
  auto filter_dims = filter->dims();
  CHECK_EQ(filter_dims.size(), 4);
  auto output_name = op_info->Output("Output").front();
  auto output_type = kernel->GetOutputDeclType("Output");
  CHECK(output_type->precision() == PRECISION(kFloat));
  CHECK(output_type->layout() == DATALAYOUT(kNCHW));
  auto strides = op_info->GetAttr<std::vector<int>>("strides");
  auto paddings = op_info->GetAttr<std::vector<int>>("paddings");
  auto groups = op_info->GetAttr<int>("groups");
  auto dilations = op_info->GetAttr<std::vector<int>>("dilations");
  auto fuse_relu =
      op_info->HasAttr("fuse_relu") && op_info->GetAttr<bool>("fuse_relu");
  CHECK_EQ(strides.size(), 2L);
  CHECK_EQ(dilations.size(), 2L);

  // Input node
  std::shared_ptr<Node> input_node = nullptr;
  if (graph->Has(input_name)) {
    input_node = graph->Get(input_name);
  } else {
    input_node = graph->Add(input_name, *input);
  }

  // Create input sizes node to describe the dimensions of input tensor
  if (paddings.size() == 2L) {
    for (size_t i = 0; i < 2L; ++i) {
      int copy_pad = *(paddings.begin() + 2 * i);
      paddings.insert(paddings.begin() + 2 * i + 1, copy_pad);
    }
  }
  CHECK_EQ(paddings.size(), 4L)
      << "[NPU] Paddings size should be the same or twice as the input size.";
  std::vector<int32_t> input_sizes;
  input_sizes.push_back(input_dims[0]);
  input_sizes.push_back(filter_dims[1] * groups);
  for (int i = 0; i < strides.size(); i++) {
    int kernel_ext = dilations[i] * (filter_dims[i + 2] - 1) + 1;
    int output_size =
        (input_dims[i + 2] - 1) * strides[i] + kernel_ext - 2 * paddings[i];
    input_sizes.push_back(output_size);
  }
  auto input_sizes_node = graph->Add(output_name + "/input_sizes", input_sizes);

  // Filter node
  auto filter_node = graph->Add(filter_name, *filter);

  // Deconv node
  auto conv_transpose_node = graph->Add<ge::op::Deconvolution>(output_name);
  auto conv_transpose_op = conv_transpose_node->data<ge::op::Deconvolution>();
  conv_transpose_op->set_input_input_sizes(*input_sizes_node->data());
  conv_transpose_op->set_input_filter(*filter_node->data());
  conv_transpose_op->set_input_x(*input_node->data());
  // Set attributes
  conv_transpose_op->set_attr_format(0);    // NCHW
  conv_transpose_op->set_attr_pad_mode(0);  // NOTSET
  conv_transpose_op->set_attr_group(groups);
  conv_transpose_op->set_attr_pad(ge::AttrValue::LIST_INT(
      {paddings[0], paddings[1], paddings[2], paddings[3]}));
  conv_transpose_op->set_attr_dilation(
      ge::AttrValue::LIST_INT({dilations[0], dilations[1]}));
  conv_transpose_op->set_attr_stride(
      ge::AttrValue::LIST_INT({strides[0], strides[1]}));
  conv_transpose_op->set_attr_kernel(
      ge::AttrValue::LIST_INT({filter_dims[2], filter_dims[3]}));

  // Append add node to add bias if exists bias
  if (HasInputArg(op_info, scope, "Bias")) {
    std::shared_ptr<Node> bias_node = nullptr;
    auto bias_name = op_info->Input("Bias").front();
    if (graph->Has(bias_name)) {
      bias_node = graph->Get(bias_name);
    } else {
      auto bias_type = kernel->GetInputDeclType("Bias");
      CHECK(bias_type->precision() == PRECISION(kFloat));
      CHECK(bias_type->layout() == DATALAYOUT(kNCHW));
      auto bias = scope->FindMutableTensor(bias_name);
      auto channel_size = bias->dims().production();
      CHECK_EQ(channel_size, filter_dims[1] * groups);
      bias_node = graph->Add(bias_name, *bias, {1, channel_size, 1, 1});
    }
    // Append add node to add bias node
    auto add_node = graph->Add<ge::op::Add>(output_name);
    auto add_op = add_node->data<ge::op::Add>();
    add_op->set_input_x1(*conv_transpose_node->data());
    add_op->set_input_x2(*bias_node->data());
    conv_transpose_node = add_node;
  }

  if (fuse_relu) {
    // Append relu node if fuse_relu is true
    auto relu_node = graph->Add<ge::op::Activation>(output_name);
    auto relu_op = relu_node->data<ge::op::Activation>();
    relu_op->set_input_x(*conv_transpose_node->data());
    relu_op->set_attr_mode(CvtActMode("relu"));
  }
  return REBUILD_WHEN_SHAPE_CHANGED;
}

}  // namespace npu
}  // namespace subgraph
}  // namespace lite
}  // namespace paddle

REGISTER_SUBGRAPH_BRIDGE(conv2d_transpose,
                         kNPU,
                         paddle::lite::subgraph::npu::ConvTransposeConverter);
