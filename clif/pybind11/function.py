# Copyright 2020 Google LLC
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Generates pybind11 bindings code for functions."""

from typing import Generator, Optional, Set

from clif.protos import ast_pb2
from clif.pybind11 import function_lib
from clif.pybind11 import lambdas
from clif.pybind11 import operators
from clif.pybind11 import utils


I = utils.I


def generate_from(
    module_name: str, func_decl: ast_pb2.FuncDecl,
    capsule_types: Set[str],
    class_decl: Optional[ast_pb2.ClassDecl] = None,
) -> Generator[str, None, None]:
  """Generates pybind11 bindings code for functions.

  Args:
    module_name: String containing the superclass name.
    func_decl: Function declaration in proto format.
    capsule_types: A list of C++ types that are defined as capsules.
    class_decl: Outer class declaration in proto format. None if the function is
      not a member of a class.

  Yields:
    pybind11 function bindings code.
  """
  num_unknown = function_lib.num_unknown_default_values(func_decl)
  if num_unknown:
    yield from _generate_overload_for_unknown_default_function(
        num_unknown, module_name, func_decl, capsule_types, class_decl)
  else:
    yield from _generate_function(
        module_name, func_decl, capsule_types, class_decl)


def _generate_function(
    module_name: str, func_decl: ast_pb2.FuncDecl,
    capsule_types: Set[str],
    class_decl: Optional[ast_pb2.ClassDecl] = None,
) -> Generator[str, None, None]:
  """Generates pybind11 bindings code for ast_pb2.FuncDecl."""
  if lambdas.needs_lambda(func_decl, capsule_types, class_decl):
    yield from lambdas.generate_lambda(
        module_name, func_decl, capsule_types, class_decl)
  elif operators.needs_operator_overloading(func_decl):
    yield from operators.generate_operator(module_name, func_decl)
  else:
    yield from _generate_simple_function(module_name, func_decl, class_decl)


def _generate_simple_function(
    module_name: str, func_decl: ast_pb2.FuncDecl,
    class_decl: Optional[ast_pb2.ClassDecl] = None
) -> Generator[str, None, None]:
  func_name = func_decl.name.native.rstrip('#')  # @sequential
  yield f'{module_name}.{function_lib.generate_def(func_decl)}("{func_name}",'
  yield I + function_lib.generate_cpp_function_cast(func_decl, class_decl)
  yield I + f'&{func_decl.name.cpp_name},'
  yield I + function_lib.generate_function_suffixes(func_decl)


def _generate_overload_for_unknown_default_function(
    num_unknown: int, module_name: str,
    func_decl: ast_pb2.FuncDecl, capsule_types: Set[str],
    class_decl: Optional[ast_pb2.ClassDecl] = None
) -> Generator[str, None, None]:
  """Generate multiple definitions for functions with unknown default values."""
  temp_func_decl = ast_pb2.FuncDecl()
  temp_func_decl.CopyFrom(func_decl)
  for _ in range(num_unknown):
    yield from _generate_function(
        module_name, temp_func_decl, capsule_types, class_decl)
    del temp_func_decl.params[-1]
  yield from _generate_function(
      module_name, temp_func_decl, capsule_types, class_decl)
