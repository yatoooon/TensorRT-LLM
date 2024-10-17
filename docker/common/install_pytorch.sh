#!/bin/bash

set -ex

# Use latest stable version from https://pypi.org/project/torch/#history
# and closest to the version specified in
# https://docs.nvidia.com/deeplearning/frameworks/pytorch-release-notes/rel-24-03.html#rel-24-03
TORCH_VERSION="2.2.2"
SYSTEM_ID=$(grep -oP '(?<=^ID=).+' /etc/os-release | tr -d '"')

prepare_environment() {
    if [[ $SYSTEM_ID == *"ubuntu"* ]]; then
      apt-get update && apt-get -y install ninja-build
      apt-get clean && rm -rf /var/lib/apt/lists/*
    elif [[ $SYSTEM_ID == *"centos"* ]]; then
      yum -y update && yum install -y ninja-build && yum clean all
      if [[ "$1" -eq "1" ]]; then
          # Temporarily disable devtoolset
          mv /tmp/devtoolset_env /tmp/devtoolset_env.bak
          touch /tmp/devtoolset_env
      fi
    else
      echo "This system type cannot be supported..."
      exit 1
    fi
}

restore_environment() {
    if [[ $SYSTEM_ID == *"centos"* ]] && [[ "$1" -eq "1" ]]; then
        # Re-enable devtoolset
        rm -f /tmp/devtoolset_env
        mv /tmp/devtoolset_env.bak /tmp/devtoolset_env
    fi
}

install_from_source() {
    prepare_environment $1
    export _GLIBCXX_USE_CXX11_ABI=$1
    export TORCH_CUDA_ARCH_LIST="8.0;9.0"

    pip3 uninstall -y torch
    cd /tmp
    git clone --depth 1 --branch v$TORCH_VERSION https://github.com/pytorch/pytorch
    cd pytorch
    # Apply PyTorch patch for supporting compiling with CUDA 12.4 from source codes.
    git apply /tmp/pytorch_pr_116072.patch
    git submodule sync && git submodule update --init --recursive
    pip3 install -r requirements.txt
    python3 setup.py install
    cd /tmp && rm -rf /tmp/pytorch
    restore_environment $1
}

install_from_pypi() {
    pip3 install torch==${TORCH_VERSION}
}

case "$1" in
  "skip")
    ;;
  "pypi")
    install_from_pypi
    ;;
  "src_cxx11_abi")
    install_from_source 1
    ;;
  "src_non_cxx11_abi")
    install_from_source 0
    ;;
  *)
    echo "Incorrect input argument..."
    exit 1
    ;;
esac
