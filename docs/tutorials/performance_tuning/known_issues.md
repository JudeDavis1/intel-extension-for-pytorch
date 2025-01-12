Known Issues
============

- `omp_set_num_threads` function failed to change OpenMP threads number of oneDNN operators if it was set before.

  `omp_set_num_threads` function is provided in Intel® Extension for PyTorch\* to change number of threads used with openmp. However, it failed to change number of OpenMP threads if it was set before.

  pseudo code:

  ```
  omp_set_num_threads(6)
  model_execution()
  omp_set_num_threads(4)
  same_model_execution_again()
  ```

  **Reason:** oneDNN primitive descriptor stores the omp number of threads. Current oneDNN integration caches the primitive descriptor in IPEX. So if we use runtime extension with oneDNN based pytorch/ipex operation, the runtime extension fails to change the used omp number of threads.

- Low performance with INT8 support for dynamic shapes

  The support for dynamic shapes in Intel® Extension for PyTorch\* INT8 integration is still working in progress. For the use cases where the input shapes are dynamic, for example inputs of variable image sizes in an object detection task or of variable sequence lengths in NLP tasks, the Intel® Extension for PyTorch\* INT8 path may slow down the model inference. In this case, please utilize stock PyTorch INT8 functionality.

- Low throughtput with DLRM FP32 Train

  A 'Sparse Add' [PR](https://github.com/pytorch/pytorch/pull/23057) is pending on review. The issue will be fixed when the PR is merged.

- If inference is done with a custom function, `conv+bn` folding feature of the `ipex.optimize()` function doesn't work.

  ```
  import torch
  import intel_pytorch_extension as ipex
  
  class Module(torch.nn.Module):
      def __init__(self):
          super(Module, self).__init__()
          self.conv = torch.nn.Conv2d(1, 10, 5, 1)
          self.bn = torch.nn.BatchNorm2d(10)
          self.relu = torch.nn.ReLU()
  
      def forward(self, x):
          x = self.conv(x)
          x = self.bn(x)
          x = self.relu(x)
          return x
  
      def inference(self, x):
          return self.forward(x)
  
  if __name__ == '__main__':
      m = Module()
      m.eval()
      m = ipex.optimize(m, dtype=torch.float32, level="O0")
      d = torch.rand(1, 1, 112, 112)
      with torch.no_grad():
        m.inference(d)
  ```

  This is PyTorch FX limitation, user can avoid this error by calling `m = ipex.optimize(m, level="O0")`, which doesn't apply ipex optimization, or disable `conv+bn` folding by calling `m = ipex.optimize(m, level="O1", conv_bn_folding=False)`.
