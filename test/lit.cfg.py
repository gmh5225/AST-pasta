# -*- Python -*-

import os
import platform
import re
import subprocess
import tempfile

import lit.formats
import lit.util

from lit.llvm import llvm_config
from lit.llvm.subst import ToolSubst
from lit.llvm.subst import FindTool

# Configuration file for the 'lit' test runner.

# name: The name of this test suite.
config.name = 'PASTA'

config.test_format = lit.formats.ShTest(not llvm_config.use_lit_shell)

# suffixes: A list of file extensions to treat as test files.
config.suffixes = ['.c', '.cpp']

# test_source_root: The root path where tests are located.
config.test_source_root = os.path.dirname(__file__)

# test_exec_root: The root path where tests should be run.
config.test_exec_root = os.path.join(config.pasta_obj_root, 'test')

config.substitutions.append(('%PATH%', config.environment['PATH']))
config.substitutions.append(('%shlibext', config.llvm_shlib_ext))

llvm_config.with_system_environment(
    ['HOME', 'INCLUDE', 'LIB', 'TMP', 'TEMP'])

# excludes: A list of directories to exclude from the testsuite. The 'Inputs'
# subdirectories contain auxiliary inputs for various tests in their parent
# directories.
config.excludes = ['CMakeLists.txt', 'README.md']

# test_source_root: The root path where tests are located.
config.test_source_root = os.path.dirname(__file__)

# test_exec_root: The root path where tests should be run.
config.test_exec_root = os.path.join(config.pasta_obj_root, 'test')
config.pasta_tools_dir = os.path.join(config.pasta_obj_root, 'bin')

tools = [
    ToolSubst(
        "print-cxx-tokens",
        os.path.join(config.pasta_obj_root, 'bin', 'PrintTokens', 'print-tokens'),
        extra_args=["-x", "c++"]),

    ToolSubst(
        "print-c-tokens",
        os.path.join(config.pasta_obj_root, 'bin', 'PrintTokens', 'print-tokens'),
        extra_args=["-x", "c"]),
    
    ToolSubst(
        "FileCheck",
        config.file_check_path)
]

llvm_config.add_tool_substitutions(tools)
