from setuptools import setup
from torch.utils.cpp_extension import BuildExtension, CppExtension
import os
import torch

kineto_root = os.environ.get("KINETO_ROOT", "/root/workspace/code/kineto")
kineto_include = os.path.join(kineto_root, "libkineto/include")
kineto_src = os.path.join(kineto_root, "libkineto/src")
kineto_build = os.path.join(kineto_root, "libkineto/build")

perfetto_sdk = os.environ.get(
    "PERFETTO_SDK", "/root/workspace/code/perfetto_sdk"
)

pytorch_root = os.path.dirname(os.path.dirname(torch.__file__))
project_dir = os.path.dirname(os.path.abspath(__file__))

setup(
    name="perfetto_logger",
    ext_modules=[
        CppExtension(
            "perfetto_logger",
            [
                "register_perfetto_logger.cpp",
                "src/PerfettoLogger.cpp",
                os.path.join(perfetto_sdk, "perfetto.cc"),
            ],
            include_dirs=[
                os.path.join(project_dir, "src"),
                kineto_include,
                kineto_src,
                perfetto_sdk,
            ],
            extra_compile_args=["-std=c++17", "-DUSE_KINETO", "-DKINETO_NAMESPACE=libkineto"],
        ),
    ],
    cmdclass={"build_ext": BuildExtension},
)
