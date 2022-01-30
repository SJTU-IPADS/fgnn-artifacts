# FGNN

FGNN (previously named SamGraph) is a factored system for sample-based GNN training over GPUs.

[TOC]


## Project Structure

```bash
> tree .
├── datagen                     # Dataset Preprocessing
├── example
│   ├── dgl
│   │   ├── multi_gpu           # DGL models
│   ├── pyg
│   │   ├── multi_gpu           # PyG models
│   ├── samgraph
│   │   ├── balance_switcher    # FGNN Dynamic Switch
│   │   ├── multi_gpu           # FGNN models
│   │   ├── sgnn                # SGNN models
│   │   ├── sgnn_dgl            # DGL PinSAGE models(SGNN simulated)
├── exp                         # Experiment Scripts
│   ├── figXX
│   ├── tableXX
├── samgraph                    # FGNN, SGNN source codes
└── utility                     # Useful tools for dataset preprocessing
```



## Paper's Hardware Configuration
- 8x16GB NVIDIA V100 GPUs
- 2x24 cores Intel Xeon Platinum CPUs
- 512GB RAM

**In the AE environment we provided,  each V100 GPU has 32GB memory.**



## Installation

**We have already setup a out-of-the-box environment for AE reviewers. AE reviewers don't need to perform the following steps if AE reviewers choose to run the experiments on the machine we provided.**

### Software Version

- Python v3.8
- PyTorch v1.7.1
- CUDA v10.1
- DGL V0.7.1
- PyG v2.0.1
- GCC&G++ 7
- CMake >= 3.14

### GCC-7 And CUDA10.1 Environment

1. Install CUDA 10.1. FGNN is built on CUDA 10.1. Follow the instructions in https://developer.nvidia.com/cuda-10.1-download-archive-base to install CUDA 10.1. Make sure that `/usr/local/cuda` is linked to `/usr/local/cuda-10.1`.

2. CUDA10.1 requires GCC version<=7. Make sure that 'gcc' is linked to 'gcc-7' and 'g++' is linked to 'g++-7'. 

    ```bash
    # Ubuntu
    sudo update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-7 7
    sudo update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-7 7
    ```


### FGNN, DGL and PyG Environment

We use conda to manage our python environment.

1. Install Python=3.8, cudatoolkit=10.1, and Pytorch=1.7.1 environment from conda: 

    ```bash
    conda create -n fgnn_env python==3.8 pytorch==1.7.1 torchvision==0.8.2 torchaudio==0.7.2 cudatoolkit=10.1 -c pytorch -y
    conda activate fgnn_env
    conda install cudnn
    conda install gnuplot -c conda-forge # install gnuplot for experiments
    
    ```


2. Download the FGNN source code, install DGL(See [`3rdparty/readme.md`](3rdparty/readme.md)) and fastgraph in the source. FGNN uses DGL as the training backend. The package "fastgraph" is used to load dataset for DGL and PyG in experiments.

    ```bash
    # Download FGNN source code
    git clone --recursive https://github.com/SJTU-IPADS/fgnn-artifacts.git
    
    # Install DGL
    conda install cmake #(Optinal) Sometimes the system cmake is too old to build DGL
    
    pushd fgnn-artifacts/3rdparty/dgl
    
    git apply ../dgl.patch # patching for dataset loading
    
    export CUDNN_LIBRARY=$CONDA_PREFIX/lib
    export CUDNN_LIBRARY_PATH=$CONDA_PREFIX/lib
    export CUDNN_ROOT=$CONDA_PREFIX
    export CUDNN_INCLUDE_DIR=$CONDA_PREFIX/include
    export CUDNN_INCLUDE_PATH=$CONDA_PREFIX/include
    cmake -S . -B build -DUSE_CUDA=ON -DBUILD_TORCH=ON -DCMAKE_BUILD_TYPE=Release
    pushd build
    make -j
    popd build
    
    pushd python
    python setup.py install
    popd
    popd
    
    
    # Install fastgraph
    pushd fgnn-artifacts/utility/fastgraph
    python setup.py install
    popd
    ```

    

3. Install FGNN(Samgraph):
   
    ```bash
    cd fgnn-artifacts
    ./build.sh
    ```



4. Install PyG for experiments

    ```bash
    FORCE_CUDA=1 pip install --no-cache-dir --verbose torch-scatter==2.0.8 \
    && pip install torch-sparse==0.6.12 -f https://data.pyg.org/whl/torch-1.7.0+cu101.html \
    && pip install torch-geometric==2.0.1 \
    && pip install torch-cluster==1.5.9 -f https://data.pyg.org/whl/torch-1.7.0+cu101.html \
    && pip install torch-spline-conv==1.2.1 -f https://data.pyg.org/whl/torch-1.7.0+cu101.html
    ```

### Setting ulimit
DGL CPU sampling requires cro-processing communications.FGNN global queue requires memlock to enable fast memcpy between host memory and GPU memory. So we have to set the user limit.


Add the following content to `/etc/security/limits.conf` and then `reboot`:

    ```bash
    * soft nofile 65535         # for DGL CPU sampling
    * hard nofile 65535         # for DGL CPU sampling
    * soft memlock 200000000    # for FGNN global queue
    * hard memlock 200000000    # for FGNN global queue
    ```

After reboot you can see:

    ```bash
    > ulimit -n
    65535
    
    > ulimit -l
    200000000
    ```



## Dataset Preprocessing

**AE reviewers don't need to perform the following steps if AE reviewers choose to run the experiments on the machine we provided. We have already downloaded and processed the dataset(`/graph-learning/samgraph`)**.

See [`datagen/README.md`](datagen/README.md).




## Experiments

See [`exp/README.md`](exp/README.md).
