from setuptools import setup, Extension
from Cython.Build import cythonize
import numpy as np
import os

extensions = [
    Extension(
        "cumulative_transience",
        sources=["ct_extension.pyx", "cumulative_transience.c"],
        include_dirs=[np.get_include(), "."],
        extra_compile_args=["-O3"] if os.name != "nt" else ["/O2"],
    )
]

setup(
    name="cumulative_transience",
    ext_modules=cythonize(extensions, language_level="3"),
)
