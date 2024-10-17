(linux)=

# Installing on Linux

1. Retrieve and launch the docker container (optional).

    You can pre-install the environment using the [NVIDIA Container Toolkit](https://docs.nvidia.com/datacenter/cloud-native/container-toolkit) to avoid manual environment configuration.

    ```bash
    # Obtain and start the basic docker image environment (optional).
    docker run --rm --runtime=nvidia --gpus all --entrypoint /bin/bash -it nvidia/cuda:12.4.0-devel-ubuntu22.04
    ```

2. Install TensorRT-LLM.

    ```bash
    # Install dependencies, TensorRT-LLM requires Python 3.10
    apt-get update && apt-get -y install python3.10 python3-pip openmpi-bin libopenmpi-dev git git-lfs

    # Install the latest preview version (corresponding to the main branch) of TensorRT-LLM.
    # If you want to install the stable version (corresponding to the release branch), please
    # remove the `--pre` option.
    pip3 install tensorrt_llm -U --pre --extra-index-url https://pypi.nvidia.com

    # Check installation
    python3 -c "import tensorrt_llm"
    ```

    Please note that TensorRT-LLM depends on TensorRT. In earlier versions that include TensorRT 8,
    overwriting an upgraded to a new version may require explicitly running `pip uninstall tensorrt`
    to uninstall the old version.

Please refer to the [Quick Start Guide](../quick-start-guide.md) for more information.

Beyond the local execution, you can also use the NVIDIA Triton Inference Server to create a production-ready deployment of your LLM as described in this [Optimizing Inference on Large Language Models with NVIDIA TensorRT-LLM](https://developer.nvidia.com/blog/optimizing-inference-on-llms-with-tensorrt-llm-now-publicly-available/) blog.
