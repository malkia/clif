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
"""Generates pybind11 bindings code for classes."""

from typing import Generator, Set

from clif.protos import ast_pb2
from clif.pybind11 import consts
from clif.pybind11 import enums
from clif.pybind11 import function
from clif.pybind11 import function_lib
from clif.pybind11 import utils
from clif.pybind11 import variables

I = utils.I


def generate_from(
    class_decl: ast_pb2.ClassDecl, superclass_name: str,
    trampoline_class_names: Set[str], capsule_types: Set[str],
    registered_types: Set[str]
) -> Generator[str, None, None]:
  """Generates a complete py::class_<>.

  Args:
    class_decl: Class declaration in proto format.
    superclass_name: String name of the superclass.
    trampoline_class_names: A Set of class names whose member functions
      will be overriden in Python.
    capsule_types: A set of C++ types that are defined as capsules.
    registered_types: A set of C++ types that are registered with Pybind11.

  Yields:
    pybind11 class bindings code.
  """
  yield I + '{'
  class_name = f'{class_decl.name.native}_class'
  definition = f'py::classh<{class_decl.name.cpp_name}'
  if not class_decl.suppress_upcasts:
    for base in class_decl.bases:
      if base.HasField('cpp_name') and base.cpp_name in registered_types:
        definition += f', {base.cpp_name}'
  trampoline_class_name = utils.trampoline_name(class_decl)
  if trampoline_class_name in trampoline_class_names:
    definition += f', {trampoline_class_name}'
  definition += (f'> {class_name}({superclass_name}, '
                 f'"{class_decl.name.native}"')
  if class_decl.HasField('docstring'):
    definition += f', {_as_cpp_string_literal(class_decl.docstring)}'
  if class_decl.enable_instance_dict:
    definition += ', py::dynamic_attr()'
  if class_decl.final:
    definition += ', py::is_final()'
  definition += ');'
  yield I + I + definition

  default_constructor_defined = False
  trampoline_generated = False
  for member in class_decl.members:
    if member.decltype == ast_pb2.Decl.Type.CONST:
      for s in consts.generate_from(class_name, member.const):
        yield I + I + s
    elif member.decltype == ast_pb2.Decl.Type.FUNC:
      if member.func.constructor:
        if not member.func.params:
          default_constructor_defined = True
        for s in _generate_constructor(class_name, member.func, class_decl):
          yield I + I + s
      else:
        for s in function.generate_from(
            class_name, member.func, capsule_types, class_decl):
          yield I + I + s
      if member.func.virtual:
        trampoline_generated = True
    elif member.decltype == ast_pb2.Decl.Type.VAR:
      for s in variables.generate_from(class_name, member.var, class_decl):
        yield I + I + s
    elif member.decltype == ast_pb2.Decl.Type.ENUM:
      for s in enums.generate_from(class_name, member.enum):
        yield I + I + s
    elif member.decltype == ast_pb2.Decl.Type.CLASS:
      for s in generate_from(member.class_, class_name,
                             trampoline_class_names, capsule_types,
                             registered_types):
        yield I + s

  if (not default_constructor_defined and class_decl.cpp_has_def_ctor and
      (not class_decl.cpp_abstract or trampoline_generated)):
    yield I + I + f'{class_name}.def(py::init<>());'
  yield I + '}'


def _generate_constructor(
    class_name: str, func_decl: ast_pb2.FuncDecl,
    class_decl: ast_pb2.ClassDecl) -> Generator[str, None, None]:
  """Generates pybind11 bindings code for a constructor.

  Multiple deinitions will be generated when the constructor contains unknown
  default value arguments.

  Args:
    class_name: Name of the class that defines the contructor.
    func_decl: Constructor declaration in proto format.
    class_decl: Class declaration in proto format.

  Yields:
    pybind11 function bindings code.
  """
  num_unknown = function_lib.num_unknown_default_values(func_decl)
  temp_func_decl = ast_pb2.FuncDecl()
  temp_func_decl.CopyFrom(func_decl)
  if num_unknown:
    for _ in range(num_unknown):
      yield from _generate_constructor_overload(class_name, temp_func_decl,
                                                class_decl)
      del temp_func_decl.params[-1]
  yield from _generate_constructor_overload(class_name, temp_func_decl,
                                            class_decl)


def _generate_constructor_overload(
    class_name: str, func_decl: ast_pb2.FuncDecl,
    class_decl: ast_pb2.ClassDecl) -> Generator[str, None, None]:
  """Generates pybind11 bindings code for a constructor."""
  params_with_types = ', '.join(
      [f'{p.type.cpp_type} {p.name.cpp_name}' for p in func_decl.params])
  params = ', '.join([f'{p.name.cpp_name}' for p in func_decl.params])
  cpp_types = ', '.join(
      [f'{function_lib.generate_param_type(p)}' for p in func_decl.params])
  if func_decl.name.native == '__init__' and func_decl.is_extend_method:
    yield f'{class_name}.def(py::init([]({params_with_types}) {{'
    yield I + f'return {func_decl.name.cpp_name}({params});'
    yield f'}}), {function_lib.generate_function_suffixes(func_decl)}'

  elif func_decl.name.native == '__init__':
    yield (f'{class_name}.def(py::init<{cpp_types}>(), '
           f'{function_lib.generate_function_suffixes(func_decl)}')

  elif func_decl.constructor:
    yield (f'{class_name}.def_static("{func_decl.name.native}", '
           f'[]({params_with_types}) {{')
    yield I + f'return {class_decl.name.cpp_name}({params});'
    yield f'}}, {function_lib.generate_function_suffixes(func_decl)}'


def _as_cpp_string_literal(s: str) -> str:
  return f'"{repr(s)}"'
