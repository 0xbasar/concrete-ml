<!-- markdownlint-disable -->

<a href="https://github.com/zama-ai/concrete-ml-internal/tree/main/src/concrete/ml/search_parameters/p_error_search.py#L0"><img align="right" style="float:right;" src="https://img.shields.io/badge/-source-cccccc?style=flat-square"></a>

# <kbd>module</kbd> `concrete.ml.search_parameters.p_error_search`

p_error binary search for classification and regression tasks.

Only Pytorch neural networks and Concrete built-in models are supported.

- Concrete built-in models include trees and QNN
- Quantized aware trained model are supported using Brevitas framework
- Torch models can be converted into post-trained quantized models

The `p_error` represents an essential hyper-parameter in the FHE computation at Zama. As it impacts the speed of the FHE computations and the model's performance.

In this script, we provide an approach to find out an optimal `p_error`, which would offer an interesting compromise between speed and efficiency.

The `p_error` represents the probability of a single PBS being incorrect. Know that the FHE scheme allows to perfom 2 types of operations

- Linear operations: additions and multiplications
- Non-linear operation: uni-variate activation functions

At Zama, non-linear operations are represented by table lookup (TLU), which are implemented through the Programmable Bootstrapping technology (PBS). A single PBS operation has `p_error` chances of being incorrect.

It's highly recommended to adjust the `p_error` as it is linked to the data-set.

The inference is performed via the FHE simulation mode previously known as Virtual Library (VL)

The goal is to look for the largest `p_error_i`, a float ∈ \]0,1\[, which gives a model_i that has `accuracy_i`, such that: | accuracy_i - accuracy_0| \<= Threshold, where: Threshold ∈ R, given by the user and `accuracy_0` refers to original model_0 with `p_error_0 = 0.0`.

`p_error` is bounded between 0 and 1 `p_error ~ 0.0`, refers to the original model in clear, that gives an accuracy that we note as `accuracy_0` `p_error = 1.0`, refers to the worst case scenario, where the model perfoms very badly By default, `lower = 0.0` and `uppder = 1.0`.

We assume that the condition is satisfied when we have a match A match is defined as a uni-variate function, through `strategy` argument, given by the user, it can be

`any = lambda all_matches: any(all_matches)` `all = lambda all_matches: all(all_matches)` `mean = lambda all_matches: numpy.mean(all_matches) >= 0.5` `median = lambda all_matches: numpy.median(all_matches) == 1`

To validate the results of the FHE simulation and get a stable estimation, we do several simulations If match, we update the lower bound to be the current `p_error` Else, we update the upper bound to be the current `p_error` Update the current `p_error` with the mean of the bounds

We stop the search either when the maximum number of iterations is reached or when the update of the `p_error` is below a given threshold called `delta_tolerence`

If we don't reach the convergence, a user warning is raised.

______________________________________________________________________

<a href="https://github.com/zama-ai/concrete-ml-internal/tree/main/src/concrete/ml/search_parameters/p_error_search.py#L73"><img align="right" style="float:right;" src="https://img.shields.io/badge/-source-cccccc?style=flat-square"></a>

## <kbd>function</kbd> `compile_and_simulated_fhe_inference`

```python
compile_and_simulated_fhe_inference(
    estimator: Module,
    calibration_data: ndarray,
    ground_truth: ndarray,
    p_error: float,
    n_bits: int,
    is_qat: bool,
    metric: Callable,
    predict_method: Optional[str],
    **kwargs: Dict
) → Tuple[ndarray, float]
```

Get the quantized module of a given model in FHE, simulated or not.

Supported models are:

- Built-in models, including trees and QNN,
- Quantized aware trained model are supported using Brevitas framework,
- Torch models can be converted into post-trained quantized models.

**Args:**

- <b>`estimator`</b> (torch.nn.Module):  Torch model or a built-in model
- <b>`calibration_data`</b> (numpy.ndarray):  Calibration data required for compilation
- <b>`ground_truth`</b> (numpy.ndarray):  The ground truth
- <b>`p_error`</b> (float):  Concrete-ML uses table lookup (TLU) to represent any non-linear
- <b>`n_bits`</b> (int):  Quantization bits
- <b>`is_qat`</b> (bool):  True, if the NN has been trained through QAT.  If `False` it is converted into post-trained quantized model.
- <b>`metric`</b> (Callable):  Classification or regression evaluation metric.
- <b>`predict_method`</b> (str):  The predict method to use.
- <b>`kwargs`</b> (Dict):  Metric's hyper-parameters.

**Returns:**

- <b>`Tuple[numpy.ndarray, float]`</b>:  Dequantized or quantized output model depending on `is_benchmark_test` and the score.

**Raises:**

- <b>`ValueError`</b>:  If the model is neither a built-in model nor a torch neural network.

______________________________________________________________________

<a href="https://github.com/zama-ai/concrete-ml-internal/tree/main/src/concrete/ml/search_parameters/p_error_search.py#L164"><img align="right" style="float:right;" src="https://img.shields.io/badge/-source-cccccc?style=flat-square"></a>

## <kbd>class</kbd> `BinarySearch`

Class for `p_error` hyper-parameter search for classification and regression tasks.

<a href="https://github.com/zama-ai/concrete-ml-internal/tree/main/src/concrete/ml/search_parameters/p_error_search.py#L170"><img align="right" style="float:right;" src="https://img.shields.io/badge/-source-cccccc?style=flat-square"></a>

### <kbd>method</kbd> `__init__`

```python
__init__(
    estimator,
    n_bits: int = 4,
    is_qat: bool = True,
    verbose: bool = True,
    save: bool = False,
    lower: float = 0.0,
    upper: float = 1.0,
    max_iter: int = 100,
    n_simulation: int = 10,
    strategy: Any = <built-in function all>,
    metric: Callable = <function top_k_accuracy_score at 0x7febd1dbe550>,
    max_metric_loss: float = 0.01,
    delta_tolerance: float = 1e-05,
    log_file: str = 'log_file.txt',
    directory: str = '/tmp/cml_search/p_error',
    predict_method: Optional[str] = None,
    **kwargs: dict
)
```

`p_error` binary search algorithm.

**Args:**

- <b>`estimator `</b>:  Custom model (Brevitas or Pytorch) or built-in models (trees or QNNs)
- <b>`is_qat`</b> (bool):  Flag that indicates whether the `estimator` has been  trained through QAT (quantization-aware training). Default is True
- <b>`verbose`</b> (bool):  Flag that indicates whether to print detailed information.  Default is True.
- <b>`n_bits`</b> (int):  Quantization bits
- <b>`save`</b> (bool):  Flag that indicates whether to save some meta data in  `log_file.txt`file. Default is False.
- <b>`lower`</b> (float):  The lower bound of the search space for the `p_error`.  Default is 0.0.
- <b>`upper`</b> (float):  The upper bound of the search space for the `p_error`.  Default is 1.0.
- <b>`max_iter`</b> (int):  The maximum number of iterations to run the binary search  algorithm. Default is 100.
- <b>`n_simulation`</b> (int):  The number of simulations to validate the results of  the VL. Default is 10.
- <b>`max_metric_loss`</b> (float):  The threshold to use to satisfy the condition:  | accuracy_i - accuracy_0| \<= `max_metric_loss` and stop the search algorithm.  Default is 0.01.
- <b>`strategy`</b> (Any):  A uni-variate function that defines a "match". It can be:  a built-in functions provided in python, like:  any or all or a custom function, like:
- <b>`mean = lambda all_matches`</b>:  numpy.mean(all_matches) >= 0.5
- <b>`median = lambda all_matches`</b>:  numpy.median(all_matches) == 1 Default is 'all'.
- <b>`metric`</b> (Callable):  Evaluation metric for classification or regression tasks
- <b>`log_file`</b> (str):  The kog file name. Default is 'log_file.txt'.
- <b>`delta_tolerance`</b> (float):  Tolerance's threshold of the relative difference between  |current_p_error - previous_p_error|. Default is 1e-5.
- <b>`directory`</b> (str):  The directory to save the meta data.  Default is '/tmp/cml_search/p_error'.
- <b>`predict_method`</b> (str):  The prediction method to use for built-in tree models.
- <b>`kwargs`</b>:  Parameter of the evaluation metric.

______________________________________________________________________

<a href="https://github.com/zama-ai/concrete-ml-internal/tree/main/src/concrete/ml/search_parameters/p_error_search.py#L292"><img align="right" style="float:right;" src="https://img.shields.io/badge/-source-cccccc?style=flat-square"></a>

### <kbd>method</kbd> `eval_match`

```python
eval_match(strategy: Callable, all_matches: List) → Union[bool, bool_]
```

Eval the matches.

**Args:**

- <b>`strategy`</b> (Callable):  A uni-variate function that defines a "match". It can be: a
- <b>`built-in functions provided in python, like`</b>:  any or all or a custom function, like:
- <b>`mean = lambda all_matches`</b>:  numpy.mean(all_matches) >= 0.5
- <b>`median = lambda all_matches`</b>:  numpy.median(all_matches) == 1
- <b>`all_matches`</b> (List):  List of matches.

**Returns:**

- <b>`bool`</b>:  Evaluation of the matches according to the given strategy.

**Raises:**

- <b>`TypeError`</b>:  If the `strategy` function is not valid.

______________________________________________________________________

<a href="https://github.com/zama-ai/concrete-ml-internal/tree/main/src/concrete/ml/search_parameters/p_error_search.py#L259"><img align="right" style="float:right;" src="https://img.shields.io/badge/-source-cccccc?style=flat-square"></a>

### <kbd>method</kbd> `reset_history`

```python
reset_history() → None
```

Clean history.

______________________________________________________________________

<a href="https://github.com/zama-ai/concrete-ml-internal/tree/main/src/concrete/ml/search_parameters/p_error_search.py#L398"><img align="right" style="float:right;" src="https://img.shields.io/badge/-source-cccccc?style=flat-square"></a>

### <kbd>method</kbd> `run`

```python
run(
    x: ndarray,
    ground_truth: ndarray,
    strategy: Callable = <built-in function all>,
    **kwargs: Dict
) → float
```

Get an optimal `p_error` using binary search for classification and regression tasks.

Only PyTorch models are supported. If the given model is not quantization-aware trained, it will be converted into a post-trained quantized model.

To find an optimal `p_error` that  offers a balance between speed and efficiency, we use a binary search approach. Where the goal to look for the largest `p_error_i`, a float ∈ \]0,1\[, which gives a model_i that has `accuracy_i`, such that | accuracy_i - accuracy_0| \<= max_metric_loss, where max_metric_loss ∈ R and `accuracy_0` refers to original model_0 with `p_error ~ 0.0`.

We assume that the condition is satisfied when we have a match. A match is defined as a uni-variate function, specified through `strategy` argument.

To validate the results of the FHE simulation and get a stable estimation, we perform multiple samplings. If match, we update the lower bound to be the current p_error. Else, we update the upper bound to be the current p_error. Update the current p_error with the mean of the bounds.

We stop the search either when the maximum number of iterations is reached or when the update of the `p_error` is below at a given threshold.

**Args:**

- <b>`x`</b> (numpy.ndarray):  Data-set which is used for calibration and evaluation
- <b>`ground_truth`</b> (numpy.ndarray):  The ground truth
- <b>`kwargs`</b> (Dict):  Class parameters
- <b>`strategy`</b> (Callable):  A uni-variate function that defines a "match". It can be: a
- <b>`built-in functions provided in python, like`</b>:  any or all or a custom function, like:
- <b>`mean = lambda all_matches`</b>:  numpy.mean(all_matches) >= 0.5
- <b>`median = lambda all_matches`</b>:  numpy.median(all_matches) == 1 Default is `all`.

**Returns:**

- <b>`float`</b>:  The optimal `p_error` that aims to speedup computations while maintaining good  performance.