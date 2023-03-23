#!/usr/bin/env python
# coding: utf-8

# Run p_error search for CIFAR-10 8-bits network.

# Steps:
# 1. Load CIFAR-10 8-bits model

# 2. Load CIFAR-10 dataset

# 3. Pre-process the dataset, in our case, that means:
#   - Reducing the dataset size (that we will can calibration data in our experiments)
#   - Computing the features maps of the model on the client side

# 4. Run the search for a given set of hyper-parameters
#   - The objective is to look for the largest `p_error = i`, with i ∈ ]0,1[ ∩ ℝ,
#       which gives a model_i that has `accuracy_i`, such that:
#       | accuracy_i - accuracy_0| <= Threshold, where:
#           - Threshold is given by the user and
#           - `accuracy_0` refers to original model with `p_error = 0`

#   - Define our objective:
#       - If the objective is matched -> update the lower bound to be the current p-error
#       - Else, update the upper bound to be the current p-error
#   - Update the current p-error with the mean of the bounds

#   - We stop the search either
#         - when the maximum number of iterations is reached, or
#         - when the update of the `p_error` is below at a given threshold

#   - The inference is performed via the FHE simulation mode (Virtual Library)

# `p_error` is bounded between 0 and 1
#       - `p_error = 0.0`, refers to the original model in clear, that gives an accuracy
#           that we note as `accuracy_0`
#       - `p_error = 1.0`, refers to the worst case scenario, where the model perfoms very badly
#       - By default, `lower = 0.0` and `uppder` bound to 1.

#   - Run the inference using the Virtual Library and this `p_error`
#   - Define our objective:
#       - If the objective is matched -> update the lower bound to be the current p-error
#       - Else, update the upper bound to be the current p-error
#   - Update the current p-error with the mean of the bounds

import argparse

import torch
from model import CNV
from torchvision import datasets, transforms

from concrete.ml.pytest.utils import (
    data_calibration_processing,
    get_torchvision_dataset,
    load_torch_model,
)
from concrete.ml.search_parameters import BinarySearch

DATASETS_ARGS = {
    "CIFAR10": {
        "dataset": datasets.CIFAR10,
        "train_transform": transforms.Compose(
            [
                transforms.ToTensor(),
                transforms.Normalize((0.5, 0.5, 0.5), (0.5, 0.5, 0.5)),
            ]
        ),
        "test_transform": transforms.Compose(
            [transforms.ToTensor(), transforms.Normalize((0.5, 0.5, 0.5), (0.5, 0.5, 0.5))]
        ),
    }
}


MODELS_ARGS = {
    "CIFAR10_8b": {
        "model_class": CNV,
        "path": "./use_case_examples/cifar_brevitas_with_model_splitting/8_bit_model.pt",
        "params": {
            "num_classes": 10,
            "weight_bit_width": 2,
            "act_bit_width": 2,
            "in_bit_width": 3,
            "in_ch": 3,
        },
    },
}


def main(args):

    if args.verbose:
        print(f"** Download `{args.dataset_name=}` dataset")
    dataset = get_torchvision_dataset(DATASETS_ARGS[args.dataset_name], train_set=True)
    x_calib, y = data_calibration_processing(dataset, n_sample=args.n_sample)

    if args.verbose:
        print(f"** Load `{args.model_name=}` network")

    checkpoint = torch.load(MODELS_ARGS[args.model_name]["path"], map_location=args.device)
    state_dict = checkpoint["model_state_dict"]

    model = load_torch_model(
        MODELS_ARGS[args.model_name]["model_class"],
        state_dict,
        MODELS_ARGS[args.model_name]["params"],
    )
    model.eval()

    with torch.no_grad():
        x_calib = model.clear_module(torch.tensor(x_calib)).numpy()

    model = model.encrypted_module

    if args.verbose:
        print("** `p_error` search")

    search = BinarySearch(estimator=model)

    p_error = search.run(x=x_calib, ground_truth=y, strategy=all)

    if args.verbose:
        print(f"Optimal p_error: {p_error}")


if __name__ == "__main__":

    parser = argparse.ArgumentParser()

    parser.add_argument("--seed", type=int, default=42, help="Seed")
    parser.add_argument("--verbose", type=bool, default=True, help="Verbose")
    parser.add_argument("--n_sample", type=int, default=500, help="n_sample")
    parser.add_argument(
        "--dataset_name",
        type=str,
        default="CIFAR10",
        choices=["CIFAR10"],
        help="The selected dataset",
    )
    parser.add_argument(
        "--model_name",
        type=str,
        default="CIFAR10_8b",
        choices=["CIFAR10_8b"],
        help="The selected model",
    )

    args = parser.parse_args()

    args.device = torch.device("cuda:0" if torch.cuda.is_available() else "cpu")

    main(args)