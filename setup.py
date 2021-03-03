#!/usr/bin/env python
# -*- coding: utf-8 -*-

import io
import os

from setuptools import find_packages, setup
from torch.utils.cpp_extension import BuildExtension, CUDAExtension

# Package meta-data.
NAME = 'samgraph'
DESCRIPTION = 'A high-performance GPU-based graph sampler for deep graph learning application'
REQUIRES_PYTHON = '>=3.6.0'
VERSION = '0.0.1'

# What packages are required for this module to be executed?
REQUIRED = [
    # 'cffi>=1.4.0',
]


# The rest you shouldn't have to touch too much :)
# ------------------------------------------------
# Except, perhaps the License and Trove Classifiers!
# If you do change the License, remember to change the Trove Classifier for that!

here = os.path.abspath(os.path.dirname(__file__))

# Import the README and use it as the long-description.
# Note: this will only work if 'README.md' is present in your MANIFEST.in file!
try:
    with io.open(os.path.join(here, 'README.md'), encoding='utf-8') as f:
        long_description = '\n' + f.read()
except OSError:
    long_description = DESCRIPTION

# Load the package's __version__.py module as a dictionary.
about = {}
if not VERSION:
    with open(os.path.join(here, NAME, '__version__.py')) as f:
        exec(f.read(), about)
else:
    about['__version__'] = VERSION

def get_torch_lib():
    return CUDAExtension(
        name='samgraph.torch.c_lib',
        sources=[
            'samgraph/common/logging.cc',
        ],
        include_dirs=['3rdparty/cub'],
        extra_link_args=['-Wl,--version-script=samgraph.lds', '-fopenmp'],
        extra_compile_args= {
            'cxx': ['-std=c++11', '-fPIC', '-Ofast', '-Wall', '-fopenmp', '-march=native'],
            'nvcc': ['-std=c++11', '-arch=sm_35', '--ptxas-options=-v', '--compiler-options', "'-fPIC'"]
        }
    )

# Where the magic happens:

setup(
    name=NAME,
    version=about['__version__'],
    description=DESCRIPTION,
    long_description=long_description,
    long_description_content_type='text/markdown',
    python_requires=REQUIRES_PYTHON,
    packages=find_packages(exclude=('tests',)),
    install_requires=REQUIRED,
    include_package_data=True,
    license='Apache',
    classifiers=[
        # Trove classifiers
        # Full list: https://pypi.python.org/pypi?%3Aaction=list_classifiers
        'License :: OSI Approved :: Apache Software License',
        'Programming Language :: Python',
        'Programming Language :: Python :: 3',
        'Programming Language :: Python :: 3.6',
        'Programming Language :: Python :: Implementation :: CPython',
        'Programming Language :: Python :: Implementation :: PyPy',
        'Operating System :: POSIX :: Linux'
    ],
    ext_modules=[
        CUDAExtension(
        name='samgraph.torch.c_lib',
        sources=[
            'samgraph/common/common.cc',
            'samgraph/common/logging.cc',
            'samgraph/torch/test.cu'
        ],
        include_dirs=['3rdparty/cub'],
        extra_link_args=['-Wl,--version-script=samgraph.lds', '-fopenmp'],
        extra_compile_args= {
            'cxx': ['-std=c++11', '-fPIC', '-Ofast', '-Wall', '-fopenmp', '-march=native'],
            'nvcc': ['-std=c++11', '-arch=sm_35', '--ptxas-options=-v', '--compiler-options', "'-fPIC'"]
        })
    ],
    # $ setup.py publish support.
    cmdclass={
        'build_ext': BuildExtension
    },
    # cffi is required for PyTorch
    # If cffi is specified in setup_requires, it will need libffi to be installed on the machine,
    # which is undesirable.  Luckily, `install` action will install cffi before executing build,
    # so it's only necessary for `build*` or `bdist*` actions.
    setup_requires=REQUIRED
)
