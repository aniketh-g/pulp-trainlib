"""
Copyright (C) 2021-2022 ETH Zurich and University of Bologna

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.

Authors: Davide Nadalini, Leonardo Ravaglia, Calin Diaconu
"""

import argparse

import torch
import torch.nn as nn

import dump_utils as dump
from SoftmaxFastExp import SoftmaxFastExp


# Define modules that use the required activations
class ReLU(nn.Module):
    def __init__(self):
        super(ReLU, self).__init__()
        self.relu = nn.ReLU()

    def forward(self, x):
        out = self.relu(x)
        return out

class SoftMax(nn.Module):
    def __init__(self):
        super(SoftMax, self).__init__()
        self.softmax = nn.Softmax(dim=1)

    def forward(self, x):
        out = self.softmax(x)
        # out = F.softmax(x, dim=0)
        return out


class Sigmoid(nn.Module):
    def __init__(self):
        super(Sigmoid, self).__init__()
        self.sigmoid = nn.Sigmoid()

    def forward(self, x):
        out = self.sigmoid(x)
        return out


def main():
    # Parse and store the arguments
    parser = argparse.ArgumentParser("Activations tests")

    parser.add_argument('--in_h', type=int, default=16)
    parser.add_argument('--in_w', type=int, default=16)
    parser.add_argument('--in_c', type=int, default=8)
    parser.add_argument('--value', type=float, default=0.5)
    parser.add_argument('--data_type', type=str, default='FP32')

    args = parser.parse_args()

    in_h = args.in_h
    in_w = args.in_w
    in_c = args.in_c
    value = args.value
    data_type = args.data_type

    # Generate fake input tensors
    relu_input = torch.ones(in_c, in_h, in_w)
    softmax_input = torch.ones(in_h, in_w)
    sigmoid_input = torch.ones(in_c, in_h, in_w)

    with torch.no_grad():
        for i in range(in_h):
            for j in range(in_w):
                softmax_input[i, j] += (i + j) * value

                for k in range(in_c):
                    relu_input[k, i, j] += (i + j + k) * value
                    sigmoid_input[k, i, j] += (i + j + k) * value

    print("relu_input:")
    print(relu_input)
    print("softmax_input:")
    print(softmax_input)
    print("sigmoid_input:")
    print(sigmoid_input)

    # Generate fake labels
    relu_label = torch.ones(in_c, int(in_h), int(in_w))
    softmax_label = torch.ones(int(in_h), int(in_w))
    sigmoid_label = torch.ones(in_c, int(in_h), int(in_w))

    print("relu_label:")
    print(relu_label.size())
    print("softmax_label:")
    print(softmax_label.size())
    print("sigmoid_label:")
    print(sigmoid_label.size())

    relu_input.requires_grad = True
    softmax_input.requires_grad = True
    sigmoid_input.requires_grad = True

    # Define loss function
    loss_fn = nn.MSELoss()

    # Instantiate pooling functions
    relu = ReLU()
    softmax = SoftmaxFastExp
    sigmoid = Sigmoid()

    # Compute the output and the backward of both
    relu_out = relu(relu_input)
    softmax_out = softmax.apply(softmax_input[None, ...])
    sigmoid_out = sigmoid(sigmoid_input)

    relu_out.retain_grad()
    softmax_out.retain_grad()
    sigmoid_out.retain_grad()

    print("relu_out: ")
    print(relu_out.size())
    print("softmax_out: ")
    print(softmax_out.size())
    print("sigmoid_out: ")
    print(sigmoid_out.size())

    relu_loss = loss_fn(relu_out, relu_label)
    softmax_loss = loss_fn(softmax_out, softmax_label)
    sigmoid_loss = loss_fn(sigmoid_out, sigmoid_label)

    relu_loss.backward()
    softmax_loss.backward()
    sigmoid_loss.backward()

    print("\n*** RELU DATA ***")
    print("ReLU out is:")
    print(relu_out)
    print("ReLU out grad is:")
    print(relu_out.grad)
    print("ReLU in grad is:")
    print(relu_input.grad)

    print("\n*** SOFTMAX DATA ***")
    print("SoftMax out is:")
    print(softmax_out)
    print("SoftMax out grad is:")
    print(softmax_out.grad)
    print("SoftMax in grad is:")
    print(softmax_input.grad)

    print("\n*** SIGMOID DATA ***")
    print("Sigmoid out is:")
    print(sigmoid_out)
    print("Sigmoid out grad is:")
    print(sigmoid_out.grad)
    print("Sigmoid in grad is:")
    print(sigmoid_input.grad)

    # Write setup to file
    f = open("init_defines.h", "w")

    f.write("// Layer sizes\n")
    f.write("#define Tin_C " + str(in_c) + "\n")
    f.write("#define Tin_H " + str(in_h) + "\n")
    f.write("#define Tin_W " + str(in_w) + "\n")
    f.write("#define Tout_H Tin_H\n")
    f.write("#define Tout_W Tin_W\n")
    f.write("#define Tout_C Tin_C\n")

    f.close()

    if data_type == 'FP32':
        # Write data to file
        f = open("act_data.h", "w")

        f.write("#define IN_SIZE " + str(in_c * in_h * in_w) + "\n")
        f.write("#define OUT_SIZE " + str(in_c * int(in_h) * int(in_w)) + "\n")

        f.write("#define SOFTMAX_IN_SIZE " + str(in_h * in_w) + "\n")
        f.write("#define SOFTMAX_OUT_SIZE " + str(int(in_h) * int(in_w)) + "\n")

        f.write("PI_L2 float RELULOSS = {" + str(relu_loss.data.item()) + "};\n")
        f.write("PI_L2 float RELUOUTPUT[OUT_SIZE] = {" + dump.tensor_to_string(relu_out) + "};\n")
        f.write("PI_L2 float RELUOUTPUT_GRAD[OUT_SIZE] = {" + dump.tensor_to_string(relu_out.grad) + "};\n")
        f.write("PI_L1 float RELUIN[IN_SIZE] = {" + dump.tensor_to_string(relu_input) + "};\n")
        f.write("PI_L2 float RELUIN_GRAD[IN_SIZE] = {" + dump.tensor_to_string(relu_input.grad) + "};\n")
        f.write("PI_L1 float RELULABEL[OUT_SIZE] = {" + dump.tensor_to_string(relu_label) + "};\n")

        f.write("PI_L2 float SOFTMLOSS = {" + str(softmax_loss.data.item()) + "};\n")
        f.write("PI_L2 float SOFTMOUTPUT[SOFTMAX_OUT_SIZE] = {" + dump.tensor_to_string(softmax_out) + "};\n")
        f.write("PI_L2 float SOFTMOUTPUT_GRAD[SOFTMAX_OUT_SIZE] = {" + dump.tensor_to_string(softmax_out.grad) + "};\n")
        f.write("PI_L1 float SOFTMIN[SOFTMAX_IN_SIZE] = {" + dump.tensor_to_string(softmax_input) + "};\n")
        f.write("PI_L2 float SOFTMIN_GRAD[SOFTMAX_IN_SIZE] = {" + dump.tensor_to_string(softmax_input.grad) + "};\n")
        f.write("PI_L1 float SOFTMLABEL[SOFTMAX_OUT_SIZE] = {" + dump.tensor_to_string(softmax_label) + "};\n")

        f.write("PI_L2 float SIGMOIDLOSS = {" + str(sigmoid_loss.data.item()) + "};\n")
        f.write("PI_L2 float SIGMOIDOUTPUT[OUT_SIZE] = {" + dump.tensor_to_string(sigmoid_out) + "};\n")
        f.write("PI_L2 float SIGMOIDOUTPUT_GRAD[OUT_SIZE] = {" + dump.tensor_to_string(sigmoid_out.grad) + "};\n")
        f.write("PI_L1 float SIGMOIDIN[IN_SIZE] = {" + dump.tensor_to_string(sigmoid_input) + "};\n")
        f.write("PI_L2 float SIGMOIDIN_GRAD[IN_SIZE] = {" + dump.tensor_to_string(sigmoid_input.grad) + "};\n")
        f.write("PI_L1 float SIGMOIDLABEL[OUT_SIZE] = {" + dump.tensor_to_string(sigmoid_label) + "};\n")

        f.close()
    elif data_type == 'FP16':
        # Write data to file
        f = open("act_data.h", "w")

        f.write("#define IN_SIZE " + str(in_c * in_h * in_w) + "\n")
        f.write("#define OUT_SIZE " + str(in_c * int(in_h) * int(in_w)) + "\n")

        f.write("#define SOFTMAX_IN_SIZE " + str(in_h * in_w) + "\n")
        f.write("#define SOFTMAX_OUT_SIZE " + str(int(in_h) * int(in_w)) + "\n")

        f.write("PI_L2 fp16 RELULOSS = {" + str(relu_loss.data.item()) + "};\n")
        f.write("PI_L2 fp16 RELUOUTPUT[OUT_SIZE] = {" + dump.tensor_to_string(relu_out.half()) + "};\n")
        f.write("PI_L2 fp16 RELUOUTPUT_GRAD[OUT_SIZE] = {" + dump.tensor_to_string(relu_out.grad.half()) + "};\n")
        f.write("PI_L1 fp16 RELUIN[IN_SIZE] = {" + dump.tensor_to_string(relu_input.half()) + "};\n")
        f.write("PI_L2 fp16 RELUIN_GRAD[IN_SIZE] = {" + dump.tensor_to_string(relu_input.grad.half()) + "};\n")
        f.write("PI_L1 fp16 RELULABEL[OUT_SIZE] = {" + dump.tensor_to_string(relu_label.half()) + "};\n")

        f.write("PI_L2 fp16 SOFTMLOSS = {" + str(softmax_loss.data.item()) + "};\n")
        f.write("PI_L2 fp16 SOFTMOUTPUT[SOFTMAX_OUT_SIZE] = {" + dump.tensor_to_string(softmax_out.half()) + "};\n")
        f.write("PI_L2 fp16 SOFTMOUTPUT_GRAD[SOFTMAX_OUT_SIZE] = {" +
                dump.tensor_to_string(softmax_out.grad.half()) + "};\n")
        f.write("PI_L1 fp16 SOFTMIN[SOFTMAX_IN_SIZE] = {" + dump.tensor_to_string(softmax_input.half()) + "};\n")
        f.write("PI_L2 fp16 SOFTMIN_GRAD[SOFTMAX_IN_SIZE] = {" +
                dump.tensor_to_string(softmax_input.grad.half()) + "};\n")
        f.write("PI_L1 fp16 SOFTMLABEL[SOFTMAX_OUT_SIZE] = {" + dump.tensor_to_string(softmax_label.half()) + "};\n")

        f.write("PI_L2 fp16 SIGMOIDLOSS = {" + str(sigmoid_loss.data.item()) + "};\n")
        f.write("PI_L2 fp16 SIGMOIDOUTPUT[OUT_SIZE] = {" + dump.tensor_to_string(sigmoid_out.half()) + "};\n")
        f.write("PI_L2 fp16 SIGMOIDOUTPUT_GRAD[OUT_SIZE] = {" + dump.tensor_to_string(sigmoid_out.grad.half()) + "};\n")
        f.write("PI_L1 fp16 SIGMOIDIN[IN_SIZE] = {" + dump.tensor_to_string(sigmoid_input.half()) + "};\n")
        f.write("PI_L2 fp16 SIGMOIDIN_GRAD[IN_SIZE] = {" + dump.tensor_to_string(sigmoid_input.grad.half()) + "};\n")
        f.write("PI_L1 fp16 SIGMOIDLABEL[OUT_SIZE] = {" + dump.tensor_to_string(sigmoid_label.half()) + "};\n")

        f.close()


if __name__ == '__main__':
    main()
