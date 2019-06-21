//*****************************************************************************
// Copyright 2017-2019 Intel Corporation
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
//*****************************************************************************

#include "ngraph/op/sequence_push_front.hpp"

using namespace std;
using namespace ngraph;

const string op::SequencePushFront::type_name("SequencePushFront");

op::SequencePushFront::SequencePushFront(const Output<Node>& value, const Output<Node>& sequence)
    : Op({value, sequence})
{
}

void op::SequencePushFront::validate_and_infer_types()
{
}

shared_ptr<Node> op::SequencePushFront::copy_with_new_args(const NodeVector& new_args) const
{
    check_new_args_count(this, new_args);
    return make_shared<SequencePushFront>(new_args.at(0), new_args.at(1));
}
