# Copyright 2021 Google LLC
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
"""Generates C++ lambda functions inside pybind11 bindings code."""

import re
from typing import Generator, List, Optional, Set

from clif.protos import ast_pb2
from clif.pybind11 import function_lib
from clif.pybind11 import utils

I = utils.I

_STATUS_PATTERNS = (r'::absl::Status', r'::absl::StatusOr<(\S)+>')


class Parameter:
  """Wraps a C++ function parameter."""
  cpp_type: str  # The actual generated cpp_type in lambda expressions
  name: str  # cpp_name of this parameter
  function_argument: str  # How to pass this parameter to functions

  def __init__(self, param: ast_pb2.ParamDecl, capsule_types: Set[str]):
    ptype = param.type
    ctype = ptype.cpp_type
    self.cpp_type = ctype
    self.name = param.name.cpp_name
    self.function_argument = self.name

    if ptype.lang_type == 'object':
      self.cpp_type = 'py::object'
      if param.cpp_exact_type == '::PyObject *':
        self.function_argument = f'{param.name.cpp_name}.ptr()'
      else:
        self.function_argument = (
            f'{param.name.cpp_name}.cast<{param.cpp_exact_type}>()')
    elif not ptype.cpp_type:  # std::function
      self.cpp_type = function_lib.generate_callback_signature(param)
    # unique_ptr<T>, shared_ptr<T>
    elif (param.cpp_exact_type.startswith('::std::unique_ptr') or
          param.cpp_exact_type.startswith('::std::shared_ptr')):
      self.function_argument = f'std::move({param.name.cpp_name})'
    # T, [const] T&
    elif not ptype.cpp_raw_pointer and (
        param.cpp_exact_type.endswith('&') and not ctype.endswith('&')):
      # CLIF matcher might set param.type.cpp_type to `T` when the function
      # being wrapped takes `T&`.
      self.cpp_type = param.cpp_exact_type

    if ptype.lang_type in capsule_types:
      self.cpp_type = f'clif::CapsuleWrapper<{self.cpp_type}>'
      self.function_argument = f'{param.name.cpp_name}.ptr'


def generate_lambda(
    module_name: str, func_decl: ast_pb2.FuncDecl,
    capsule_types: Set[str],
    class_decl: Optional[ast_pb2.ClassDecl] = None
) -> Generator[str, None, None]:
  """Entry point for generation of lambda functions in pybind11."""
  params_list = []
  for param in func_decl.params:
    params_list.append(Parameter(param, capsule_types))
  params_with_type = _generate_lambda_params_with_types(
      func_decl, params_list, class_decl)
  func_name = func_decl.name.native.rstrip('#')  # @sequential
  yield (f'{module_name}.{function_lib.generate_def(func_decl)}'
         f'("{func_name}", []({params_with_type}) {{')
  yield from _generate_lambda_body(
      func_decl, params_list, capsule_types, class_decl)
  yield f'}}, {function_lib.generate_function_suffixes(func_decl)}'


def needs_lambda(
    func_decl: ast_pb2.FuncDecl,
    capsule_types: Set[str],
    class_decl: Optional[ast_pb2.ClassDecl] = None) -> bool:
  if class_decl and _has_inherited_methods(class_decl):
    return True
  return (bool(func_decl.postproc) or
          _func_has_capsule_params(func_decl, capsule_types) or
          _func_has_status_params(func_decl) or
          _func_needs_implicit_conversion(func_decl) or
          _func_has_pointer_params(func_decl) or
          _func_has_py_object_params(func_decl) or
          _func_has_status_params(func_decl) or
          _has_bytes_return(func_decl) or
          func_decl.cpp_num_params != len(func_decl.params))


def _generate_lambda_body(
    func_decl: ast_pb2.FuncDecl,
    params: List[Parameter],
    capsule_types: Set[str],
    class_decl: Optional[ast_pb2.ClassDecl] = None
) -> Generator[str, None, None]:
  """Generates body of lambda expressions."""
  function_call = _generate_function_call(func_decl, class_decl)
  function_call_params = _generate_function_call_params(func_decl, params)
  function_call_returns = _generate_function_call_returns(
      func_decl, capsule_types)

  # Generates declarations of return values
  for i, r in enumerate(func_decl.returns):
    if r.type.lang_type == 'object':
      yield I + f'py::object ret{i}{{}};'
    elif _is_status_param(r):
      yield (I +
             f'pybind11::google::PyCLIFStatus<{r.cpp_exact_type}> ret{i}{{}};')
    else:
      yield I + f'{r.type.cpp_type} ret{i}{{}};'

  # Generates call to the wrapped function
  if not func_decl.cpp_void_return and len(func_decl.returns):
    if func_decl.returns[0].type.lang_type == 'object':
      yield I + ('ret0 = '
                 f'clif::ConvertPyObject({function_call}'
                 f'({function_call_params}));')
    else:
      yield I + f'ret0 = {function_call}({function_call_params});'
  else:
    yield I + f'{function_call}({function_call_params});'

  # Generates returns of the lambda expression
  if func_decl.postproc == '->self':
    yield I + 'return self;'
  elif func_decl.postproc:
    assert '.' in func_decl.postproc
    module_name, method_name = func_decl.postproc.rsplit('.', maxsplit=1)
    # TODO: Port or reuse `clif::ImportFQName`.
    yield I + f'auto mod = py::module_::import("{module_name}");'
    yield I + ('py::object result_ = '
               f'mod.attr("{method_name}")({function_call_returns});')
    yield I + 'return result_;'
  elif len(func_decl.returns) > 1:
    yield I + f'return std::make_tuple({function_call_returns});'
  else:
    yield I + f'return {function_call_returns};'


def _generate_function_call_params(
    func_decl: ast_pb2.FuncDecl, params: List[Parameter]) -> str:
  """Generates the parameters of function calls in lambda expressions."""
  params = ', '.join([p.function_argument for p in params])
  # Ignore the return value of the function itself when generating pointer
  # parameters.
  stard_idx = 0
  if not func_decl.cpp_void_return and len(func_decl.returns):
    stard_idx = 1
  pointer_params_str = ', '.join(
      [f'&ret{i}' for i in range(stard_idx, len(func_decl.returns))])

  if params and pointer_params_str:
    return f'{params}, {pointer_params_str}'
  elif pointer_params_str:
    return pointer_params_str
  else:
    return params


def _generate_function_call_returns(
    func_decl: ast_pb2.FuncDecl, capsule_types: Set[str]) -> str:
  """Generates return values of cpp function."""
  all_returns_list = []
  for i, r in enumerate(func_decl.returns):
    if r.type.lang_type == 'bytes':
      if r.cpp_exact_type in ('::std::string_view', '::absl::string_view'):
        all_returns_list.append(f'py::bytes(ret{i}.data())')
      else:
        all_returns_list.append(f'py::bytes(ret{i})')
    elif r.type.lang_type in capsule_types:
      all_returns_list.append(
          f'clif::CapsuleWrapper<{r.type.cpp_type}>(ret{i})')
    else:
      all_returns_list.append(f'ret{i}')
  return ', '.join(all_returns_list)


def _generate_lambda_params_with_types(
    func_decl: ast_pb2.FuncDecl,
    params: List[Parameter],
    class_decl: Optional[ast_pb2.ClassDecl] = None) -> str:
  """Generates parameters and types in the signatures of lambda expressions."""
  params_list = [f'{p.cpp_type} {p.name}' for p in params]
  if (class_decl and not func_decl.classmethod and
      not func_decl.is_extend_method):
    params_list = [f'{class_decl.name.cpp_name} &self'] + params_list
  return ', '.join(params_list)


def _generate_function_call(
    func_decl: ast_pb2.FuncDecl,
    class_decl: Optional[ast_pb2.ClassDecl] = None):
  """Generates the function call underneath the lambda expression."""
  if func_decl.classmethod or not class_decl:
    return func_decl.name.cpp_name
  elif func_decl.is_extend_method:
    return func_decl.name.cpp_name
  else:
    method_name = func_decl.name.cpp_name.split('::')[-1]
    return f'self.{method_name}'


def _func_has_pointer_params(func_decl: ast_pb2.FuncDecl) -> bool:
  num_returns = len(func_decl.returns)
  return num_returns >= 2 or (num_returns == 1 and func_decl.cpp_void_return)


def _func_has_py_object_params(func_decl: ast_pb2.FuncDecl) -> bool:
  for p in func_decl.params:
    if p.type.lang_type == 'object':
      return True
  for r in func_decl.returns:
    if r.type.lang_type == 'object':
      return True
  return False


def _is_status_param(param: ast_pb2.ParamDecl) -> bool:
  for pattern in _STATUS_PATTERNS:
    if re.match(pattern, param.cpp_exact_type):
      return True
  return False


def _func_has_status_params(func_decl: ast_pb2.FuncDecl) -> bool:
  for p in func_decl.params:
    if _is_status_param(p):
      return True
  for r in func_decl.returns:
    if _is_status_param(r):
      return True
  return False


def _func_has_capsule_params(
    func_decl: ast_pb2.FuncDecl, capsule_types: Set[str]) -> bool:
  for p in func_decl.params:
    if p.type.lang_type in capsule_types:
      return True
  for r in func_decl.returns:
    if r.type.lang_type in capsule_types:
      return True
  return False


def _has_inherited_methods(class_decl: ast_pb2.ClassDecl) -> bool:
  if class_decl.cpp_bases:
    for member in class_decl.members:
      if (member.decltype == ast_pb2.Decl.Type.FUNC and not
          member.func.is_extend_method):
        namespaces = member.func.name.cpp_name.split('::')
        if len(namespaces) > 1 and namespaces[-2] != class_decl.name.cpp_name:
          return True
  return False


def _has_bytes_return(func_decl: ast_pb2.FuncDecl) -> bool:
  for r in func_decl.returns:
    if r.type.lang_type == 'bytes':
      return True
  return False


def _func_needs_implicit_conversion(func_decl: ast_pb2.FuncDecl) -> bool:
  """Check if a function contains an implicitly converted parameter."""
  if len(func_decl.params) == 1:
    param = func_decl.params[0]
    if not utils.is_usable_cpp_exact_type(param.cpp_exact_type):
      # Stop-gap approach. This `if` condition needs to be removed after
      # resolution of b/118736768. Until then this detection function cannot
      # work correctly in this situation (but there are no corresponding unit
      # tests).
      return False
    if (_extract_bare_type(param.cpp_exact_type) !=
        _extract_bare_type(param.type.cpp_type) and
        param.type.cpp_toptr_conversion and
        param.type.cpp_touniqptr_conversion):
      return True
  return False


def _extract_bare_type(cpp_name: str) -> str:
  # This helper function is not general and only meant
  # to be used in _func_needs_implicit_conversion.
  t = cpp_name.split(' ')
  if t[0] == 'const':
    t = t[1:]
  if t[-1] in {'&', '*'}:  # Minimum viable approach. To be refined as needed.
    t = t[:-1]
  return ' '.join(t)
