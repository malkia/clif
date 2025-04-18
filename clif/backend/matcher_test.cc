// Copyright 2017 Google Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "clif/backend/matcher.h"
#include "clif/backend/ast.h"
#include "clif/backend/code_builder.h"
#include "clif/backend/strutil.h"
#include "clif/protos/ast.pb.h"
#include "google/protobuf/text_format.h"
#include "gtest/gtest.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"

// For bazel builds, rely on runfiles logic from @bazel_tools//... to locate test files
#ifndef CLIF_BACKEND_SOURCE_DIR
#include "tools/cpp/runfiles/runfiles.h"
#endif

#define PROTOBUF_NS google::protobuf

namespace clif {

using clif::TranslationUnitAST;

class ClifMatcherTest : public testing::Test {
 protected:
  void SetUp() override {
#ifndef CLIF_BACKEND_SOURCE_DIR
    std::string run_files_error;
    auto run_files = ::bazel::tools::cpp::runfiles::Runfiles::CreateForTest( &run_files_error );
    ASSERT_NE(run_files, nullptr) << run_files_error;

    std::vector<std::string> file_names {
        "another_file.h",
        "test.h",
        "test_clif_aux.h",
        "test_subdir/test_clif_aux.h",
        "versioned_smart_ptr_test.h",
    };

    for( const auto& file_name : file_names ) {
        std::string full_file_name = std::string("clif/clif/backend/") + file_name;
        std::string run_file_name = run_files->Rlocation( full_file_name );
        ASSERT_FALSE(run_file_name.empty()) << "Can't find " + full_file_name;

        std::string run_file_dir = run_file_name.substr(0, run_file_name.length() - file_name.size());
        if( test_src_dir_.empty() )
            test_src_dir_ = run_file_dir;

        // We assume that all test (data) files are from the same root (test_src_dir_)
        ASSERT_EQ(run_file_dir, test_src_dir_) << "All files must be from the same root";
    }
#else
    // CLIF_BACKEND_SOURCE_DIR is a preprocessor macro set in CMakeLists.txt.
    test_src_dir_ = CLIF_BACKEND_SOURCE_DIR;
#endif
  }

  // We do not do matcher preparation in the SetUp method because:
  // 1. We want to pass an argument to this function.
  // 2. Each run of TestMatch and TestNoMatch should get their own
  //    fresh matcher suitable for the proto they want to match. Hence,
  //    this method will be called repeatedly by a single test (unlike
  //    SetUp which is only called once per test).
  // 3. We want this function to return a DeclList corresponding to the
  //    protos in |proto_list|. The code builder will have made a pass
  //    over the protos in this DeclList. Hence, the C++ type names in the
  //    Decl proto messages of this DeclList will actually be keys to the
  //    corresponding qual types in the matcher's type table.

  // Add test_header_file args to support test for different header files.
  DeclList PrepareMatcher(const std::vector<std::string>& proto_list,
                          const std::string& typemaps,
                          const std::string& test_header_file,
                          std::string* built_code) {
    // We add a dummy decl which only has the cpp_file field set.
    // This cpp_file is the test header file which contains the C++
    // constructs to match.
    std::string clif_ast_proto_text;
    StrAppend(&clif_ast_proto_text,
              "decls: { decltype: UNKNOWN cpp_file: ", "'", test_src_dir_, "/",
              test_header_file, "'} ");
    for (const std::string& proto_str : proto_list) {
        StrAppend(&clif_ast_proto_text, "decls: { ", proto_str, " } ");
    }
    StrAppend(&clif_ast_proto_text, typemaps);
    EXPECT_TRUE(PROTOBUF_NS::TextFormat::ParseFromString(
        clif_ast_proto_text, &clif_ast_));
    matcher_.reset(new ClifMatcher);
    // Builds the hashmap of the typemaps from CLIF AST.
    auto& type_map = matcher_->BuildClifToClangTypeMap(clif_ast_);
    std::string code = matcher_->builder_.BuildCode(&clif_ast_, &type_map);
    if (built_code != nullptr) {
      *built_code = code;
    }
    matcher_->RunCompiler(code,
                          TranslationUnitAST::CompilerArgs(),
                          "clif_temp.cc");
    matcher_->BuildTypeTable();
    DeclList decl_list = clif_ast_.decls();
    // We take out the first decl as it was added only to specify the test
    // header file and does not correspond to the protos in |proto_list|.
    decl_list.erase(decl_list.begin());
    return decl_list;
  }

  // Add "test.h" as the default testing header file. If header file is not
  // specified by users, we are testing the declarations in test.h.
  // Parameters:
  // proto: CLIF input AST(typemaps not included) provided by the test case.
  // typemaps: CLIF typemaps provided by the test case, used for testing the
  // automatic type selector.
  // test_header_file: the file name of the test cases' C++ source code.
  // code: an output parameter to store the code generated by the code builder.
  void TestMatch(const std::string& proto, const std::string& typemaps = "",
                 const std::string& test_header_file = "test.h",
                 std::string* code = nullptr);
  void TestMatch(const std::string& proto, protos::Decl* decl,
                 const std::string& typemaps = "",
                 const std::string& test_header_file = "test.h",
                 std::string* code = nullptr);
  void TestMatch(const std::vector<std::string>& proto_list,
                 DeclList* decl_list, const std::string& typemaps = "",
                 const std::string& test_header_file = "test.h",
                 std::string* code = nullptr);
  void TestNoMatch(const std::string& proto, const std::string& typemaps = "",
                   const std::string& test_header_file = "test.h",
                   std::string* code = nullptr);
  void TestNoMatch(const std::string& proto, protos::Decl* decl,
                   const std::string& typemaps = "",
                   const std::string& test_header_file = "test.h",
                   std::string* code = nullptr);

  std::unique_ptr<ClifMatcher> matcher_;
  protos::AST clif_ast_;
  std::string test_src_dir_;
};

TEST_F(ClifMatcherTest, BuildCode) {
  // Be sure we find all the files, and that we don't crash on empty
  // or missing fields.
  std::string proto_string =
      "usertype_includes: 'foo.h'"
      "usertype_includes: 'bar.h' "
      "decls: { decltype: UNKNOWN cpp_file: 'test.h'} "
      "decls: { decltype: CONST cpp_file: '' } "
      "decls: { decltype: VAR } ";
  protos::AST ast_proto;
  EXPECT_TRUE(PROTOBUF_NS::TextFormat::ParseFromString(
      proto_string, &ast_proto));
  matcher_.reset(new ClifMatcher);
  auto type_map = matcher_->BuildClifToClangTypeMap(ast_proto);
  std::string code = matcher_->builder_.BuildCode(&ast_proto, &type_map);
  EXPECT_TRUE(llvm::StringRef(code).contains("#include \"foo.h\""));
  EXPECT_TRUE(llvm::StringRef(code).contains("#include \"bar.h\""));
  EXPECT_TRUE(llvm::StringRef(code).contains("#include \"test.h\""));
}

void ClifMatcherTest::TestMatch(const std::string& proto,
                                const std::string& typemaps,
                                const std::string& test_header_file,
                                std::string* code) {
  protos::Decl decl;
  TestMatch(proto, &decl, typemaps, test_header_file, code);
}

void ClifMatcherTest::TestNoMatch(const std::string& proto,
                                  const std::string& typemaps,
                                  const std::string& test_header_file,
                                  std::string* code) {
  protos::Decl decl;
  TestNoMatch(proto, &decl, typemaps, test_header_file, code);
}

void ClifMatcherTest::TestMatch(const std::string& proto, protos::Decl* decl,
                                const std::string& typemaps,
                                const std::string& test_header_file,
                                std::string* code) {
  std::vector<std::string> proto_list(1, proto);
  DeclList decl_list =
      PrepareMatcher(proto_list, typemaps, test_header_file, code);
  SCOPED_TRACE(proto);
  *decl = decl_list.Get(0);
  EXPECT_TRUE(matcher_->MatchAndSetOneDecl(decl));
}

void ClifMatcherTest::TestMatch(const std::vector<std::string>& proto_list,
                                DeclList* decl_list,
                                const std::string& typemaps,
                                const std::string& test_header_file,
                                std::string* code) {
  *decl_list = PrepareMatcher(proto_list, typemaps, test_header_file, code);
  EXPECT_EQ(proto_list.size(), decl_list->size());
  for (int i = 0; i < decl_list->size(); ++i) {
    SCOPED_TRACE(proto_list[i]);
    EXPECT_TRUE(matcher_->MatchAndSetOneDecl(decl_list->Mutable(i)));
  }
}

void ClifMatcherTest::TestNoMatch(const std::string& proto, protos::Decl* decl,
                                  const std::string& typemaps,
                                  const std::string& test_header_file,
                                  std::string* code) {
  std::vector<std::string> proto_list(1, proto);
  DeclList decl_list =
      PrepareMatcher(proto_list, typemaps, test_header_file, code);
  SCOPED_TRACE(proto);
  *decl = decl_list.Get(0);
  EXPECT_FALSE(matcher_->MatchAndSetOneDecl(decl));
}

TEST_F(ClifMatcherTest, TestMatchMustUseReturnValue) {
  protos::Decl decl;
  TestMatch(
      "decltype: FUNC "
      "func { "
      "  name { "
      "    cpp_name: 'FuncWithMustUseReturn' "
      "  } "
      "  returns { "
      "    type { "
      "      lang_type: 'int' "
      "      cpp_type: 'int' "
      "     } "
      "  } "
      "}",
      &decl);
  EXPECT_FALSE(decl.func().ignore_return_value());
  TestNoMatch(
      "decltype: FUNC "
      "func { "
      "  name { "
      "    cpp_name: 'FuncWithMustUseReturn' "
      "  } "
      "  ignore_return_value: true "
      "}",
      &decl);
  EXPECT_TRUE(
      llvm::StringRef(decl.not_found())
          .contains("Clif can not ignore ABSL_MUST_USE_RESULT return values."));
}

TEST_F(ClifMatcherTest, TestMatchIngoreReturnValue) {
  protos::Decl decl;
  TestMatch(
      "decltype: FUNC "
      "func { "
      "  name { "
      "    cpp_name: 'FuncReturnsFloat' "
      "  } "
      "}",
      &decl);
  EXPECT_EQ(decl.func().returns().size(), 0);
}

TEST_F(ClifMatcherTest, TestMatchAndSetFuncCplusplusReturnValue) {
  TestMatch("decltype: FUNC func { name { cpp_name: 'FuncReturnsVoid' } }");
  protos::Decl decl;
  TestMatch("decltype: FUNC func { name { cpp_name: 'FuncReturnsInt' } "
            "returns { type { lang_type: 'int' cpp_type: 'int' } } }", &decl);
  EXPECT_FALSE(decl.func().cpp_void_return());
  EXPECT_TRUE(decl.func().cpp_noexcept());
  TestMatch("decltype: FUNC func {"
            "name { cpp_name: 'FuncReturnsInt' } "
            "ignore_return_value: true "
            " } ", &decl);
  TestMatch("decltype: FUNC func { name {"
            "cpp_name: 'VoidFuncIntPointerParam' }"
            "returns { type { lang_type: 'int' cpp_type: 'int' } } }", &decl);
  EXPECT_FALSE(decl.func().returns(0).type().cpp_raw_pointer());
  EXPECT_TRUE(decl.func().cpp_void_return());
  // Type mismatch check. A function can't match a class
  TestNoMatch("decltype: FUNC func { name {"
              "cpp_name: 'aClass' }"
              "returns { type { lang_type: 'int' cpp_type: 'int' } } }");
  // Type mismatch check. A function can't return an int into a class.
  TestNoMatch("decltype: FUNC func { name {"
              "cpp_name: 'FuncReturnsInt' }"
              "returns { type { lang_type: 'aClass' cpp_type: 'aClass' } } }");
  // Check that there is no crash when with a container return value
  // mismatches a C++ plain class.
  TestNoMatch("decltype: FUNC func { name {"
              "cpp_name: 'FuncReturnsInt' }"
              "returns { type { lang_type: 'aClass' "
              "                  cpp_type: 'ComposedType' "
              "                  params { "
              "                    lang_type: 'int' "
              "                    cpp_type: 'int' "
              "} } } }");
  // Type match check. A function can return an int64 into an int.
  TestMatch("decltype: FUNC func { name {"
            "cpp_name: 'FuncReturnsInt64' }"
            "returns { type { lang_type: 'int' cpp_type: 'int' } } }");
  std::string test_proto;
  StrAppend(&test_proto,
            "cpp_file: '", test_src_dir_, "/another_file.h'",
            "decltype: FUNC func { name {cpp_name: 'FuncInAnotherFile' } }");
  TestMatch(test_proto);
  TestNoMatch("cpp_file: 'nonexistent.h' decltype: FUNC func { name {"
            "cpp_name: 'FuncInAnotherFile' } }");

  TestMatch("decltype: FUNC func { "
            "  name { cpp_name: 'FuncReturnsConstIntPtr' } "
            "  returns { "
            "    type { "
            "      lang_type: 'int' "
            "      cpp_type: 'int' "
            "    } "
            "  } "
            "}", &decl);
  EXPECT_EQ(decl.func().returns(0).type().cpp_type(), "const int *");

  std::string decl_proto =
      "decltype: FUNC func { "
      "  name { cpp_name: 'FuncReturnsConstClassPtr' } "
      "  returns { "
      "    type { "
      "      cpp_type: 'Class' "
      "    } "
      "  } "
      "}";
  TestMatch(decl_proto, &decl);
  EXPECT_EQ(decl.func().returns(0).type().cpp_type(), "const ::Class *");

  decl_proto =
      "decltype: FUNC func { "
      "  name { cpp_name: 'FuncReturnsConstInt' } "
      "  returns { "
      "    type { "
      "      cpp_type: 'int' "
      "    } "
      "  } "
      "}";
  TestMatch(decl_proto, &decl);
  EXPECT_EQ(decl.func().returns(0).type().cpp_type(), "int");

  decl_proto =
      "decltype: FUNC func { "
      "  name { cpp_name: 'FuncReturnsConstClass' } "
      "  returns { "
      "    type { "
      "      cpp_type: 'Class' "
      "    } "
      "  } "
      "}";
  TestMatch(decl_proto, &decl);
  EXPECT_EQ(decl.func().returns(0).type().cpp_type(), "::Class");

  decl_proto =
      "decltype: FUNC func {"
      "  name {"
      "    cpp_name: 'FuncReturnsSmartPtrOfConstClass'"
      "  }"
      "  returns {"
      "    type {"
      "      cpp_type: 'Class'"
      "    }"
      "  }"
      "}";
  TestMatch(decl_proto, &decl);
  EXPECT_EQ(decl.func().returns(0).type().cpp_type(),
            "::std::shared_ptr<const ::Class>");

  decl_proto =
      "decltype: FUNC func {"
      "  name {"
      "    cpp_name: 'FuncReturnsSmartPtrOfConstInt'"
      "  }"
      "  returns {"
      "    type {"
      "      cpp_type: 'int'"
      "    }"
      "  }"
      "}";
  TestMatch(decl_proto, &decl);
  EXPECT_EQ(decl.func().returns(0).type().cpp_type(),
            "::std::shared_ptr<const int>");
}

TEST_F(ClifMatcherTest, TestMatchConstOverloading) {
  protos::Decl decl;
  TestMatch(
      "decltype: CLASS class_ { "
      "  name { cpp_name: 'ConstOverloading' } "
      "  members { "
      "    decltype: FUNC func { "
      "      name { cpp_name: 'FuncConstOverloading' } "
      "      returns { "
      "        type { "
      "           lang_type: 'int' "
      "           cpp_type: 'int' "
      "        } "
      "      } "
      "    } "
      "  } "
      "}",
      &decl);
  EXPECT_EQ(decl.class_().members(0).func().returns(0).type().cpp_type(),
            "int *");
}

TEST_F(ClifMatcherTest, TestMatchAndSetUncopyableButMovableFuncReturn) {
  protos::Decl decl;
  // Returning a plain movable but uncopyable type.
  TestMatch(
      "decltype: CLASS class_ { "
      "  name { cpp_name: 'ClassMovableButUncopyable' } "
      "  members { "
      "    decltype: FUNC func { "
      "      name { cpp_name: 'Factory' } "
      "      returns { "
      "        type { "
      "           lang_type: 'ClassMovableButUncopyable' "
      "           cpp_type: 'ClassMovableButUncopyable' "
      "        } "
      "      } "
      "    } "
      "  } "
      "}",
      &decl);
  EXPECT_EQ(decl.class_().members(0).func().returns(0).type().cpp_type(),
            "::ClassMovableButUncopyable");
  EXPECT_FALSE(decl.class_().cpp_copyable());
  EXPECT_TRUE(decl.class_().cpp_movable());
  EXPECT_FALSE(
      decl.class_().members(0).func().returns(0).type().cpp_copyable());
  EXPECT_TRUE(decl.class_().members(0).func().returns(0).type().cpp_movable());

  // Returning a pointer of a movable but uncopyable type.
  TestMatch(
      "decltype: CLASS class_ { "
      "  name { cpp_name: 'ClassMovableButUncopyable' } "
      "  members { "
      "    decltype: FUNC func { "
      "      name { cpp_name: 'FactoryPointer' } "
      "      returns { "
      "        type { "
      "           lang_type: 'ClassMovableButUncopyable' "
      "           cpp_type: 'ClassMovableButUncopyable' "
      "        } "
      "      } "
      "    } "
      "  } "
      "}",
      &decl);
  EXPECT_FALSE(decl.class_().cpp_copyable());
  EXPECT_TRUE(decl.class_().cpp_movable());
  EXPECT_EQ(decl.class_().members(0).func().returns(0).type().cpp_type(),
            "::ClassMovableButUncopyable *");
  EXPECT_TRUE(
      decl.class_().members(0).func().returns(0).type().cpp_raw_pointer());
  EXPECT_FALSE(
      decl.class_().members(0).func().returns(0).type().cpp_copyable());
  EXPECT_TRUE(decl.class_().members(0).func().returns(0).type().cpp_movable());

  // Returning a reference of a movable but uncopyable type.
  TestMatch(
      "decltype: CLASS class_ { "
      "  name { cpp_name: 'ClassMovableButUncopyable' } "
      "  members { "
      "    decltype: FUNC func { "
      "      name { cpp_name: 'FactoryRef' } "
      "      returns { "
      "        type { "
      "           lang_type: 'ClassMovableButUncopyable' "
      "           cpp_type: 'ClassMovableButUncopyable' "
      "        } "
      "      } "
      "    } "
      "  } "
      "}",
      &decl);
  EXPECT_FALSE(decl.class_().cpp_copyable());
  EXPECT_TRUE(decl.class_().cpp_movable());
  EXPECT_EQ(decl.class_().members(0).func().returns(0).type().cpp_type(),
            "::ClassMovableButUncopyable &");
  EXPECT_FALSE(
      decl.class_().members(0).func().returns(0).type().cpp_copyable());
  EXPECT_TRUE(decl.class_().members(0).func().returns(0).type().cpp_movable());

  // Returning a const reference of a movable but uncopyable type.
  TestMatch(
      "decltype: CLASS class_ { "
      "  name { cpp_name: 'ClassMovableButUncopyable' } "
      "  members { "
      "    decltype: FUNC func { "
      "      name { cpp_name: 'FactoryConstRef' } "
      "      returns { "
      "        type { "
      "           lang_type: 'ClassMovableButUncopyable' "
      "           cpp_type: 'ClassMovableButUncopyable' "
      "        } "
      "      } "
      "    } "
      "  } "
      "}",
      &decl);
  EXPECT_FALSE(decl.class_().cpp_copyable());
  EXPECT_TRUE(decl.class_().cpp_movable());
  EXPECT_EQ(decl.class_().members(0).func().returns(0).type().cpp_type(),
            "const ::ClassMovableButUncopyable &");
  EXPECT_FALSE(
      decl.class_().members(0).func().returns(0).type().cpp_copyable());
  EXPECT_TRUE(decl.class_().members(0).func().returns(0).type().cpp_movable());
}

TEST_F(ClifMatcherTest, TestMatchAndSetFuncReturnOutParam) {
  // Returns in pointer or ref params is ok....
  TestMatch("decltype: FUNC func { "
            "name { cpp_name: 'FuncIntPointerParam' } "
            "returns { type { lang_type: 'int' cpp_type: 'int' } }"
            "returns { type { lang_type: 'int' cpp_type: 'int' } } }");
  TestMatch("decltype: FUNC func { "
             "name { cpp_name: 'FuncIntRefParam' } "
            "returns { type { lang_type: 'int' cpp_type: 'int' } }"
            "returns { type { lang_type: 'int' cpp_type: 'int' } } }");
  // ... as long as they are non-const.
  TestNoMatch("decltype: FUNC func { "
              "name { cpp_name: 'FuncConstIntPointerParam' } "
              "returns { type { lang_type: 'int' cpp_type: 'int' } }"
              "returns { type { lang_type: 'int' cpp_type: 'int' } } }");
  TestNoMatch("decltype: FUNC func { "
              "name { cpp_name: 'FuncConstIntRefParam' } "
              "returns { type { lang_type: 'int' cpp_type: 'int' } }"
              "returns { type { lang_type: 'int' cpp_type: 'int' } } }");
  // Type mismatch check.
  TestNoMatch("decltype: FUNC func { "
              "name { cpp_name: 'FuncConstIntRefParam' } "
              "returns { type { lang_type: 'int' cpp_type: 'int' } }"
              "returns { type { lang_type: 'aClass' cpp_type: 'aClass' } } }");
}

TEST_F(ClifMatcherTest, TestMatchAndSetFuncParamCounts) {
  protos::Decl decl;
  // Parameter count.
  TestMatch("decltype: FUNC func { "
            "name { cpp_name: 'FuncTwoParams' } "
            "params { type { lang_type: 'int' cpp_type: 'int' } } "
            "params { type { lang_type: 'int' cpp_type: 'int' } } }");
  TestMatch("decltype: FUNC func { "
            "name { cpp_name: 'FuncOneReqOneOptParams' } "
            "params { type { lang_type: 'int' cpp_type: 'int' } } "
            "params { type { lang_type: 'int' cpp_type: 'int' }"
            "         default_value: 'None' } }");
  TestMatch("decltype: FUNC func { "
            "name { cpp_name: 'FuncOneReqOneOptParams' } "
            "params { type { lang_type: 'int' cpp_type: 'int' } } }");
  TestMatch("decltype: FUNC func { "
            "name { cpp_name: 'FuncOneReqOneOptParamsReturnsInt' } "
            "params { type { lang_type: 'int' cpp_type: 'int' } } "
            "params { type { lang_type: 'int' cpp_type: 'int' }"
            "         default_value: 'None' } "
            "returns { type { lang_type: 'int' cpp_type: 'int' } } }");
  TestMatch("decltype: FUNC func { "
            "name { cpp_name: 'FuncOneReqOneOptParamsReturnsInt' } "
            "params { type { lang_type: 'int' cpp_type: 'int' } } "
            "returns { type { lang_type: 'int' cpp_type: 'int' } } }");
  TestNoMatch("decltype: FUNC func { "
              "name { cpp_name: 'FuncOneParams' } "
              "params { type { lang_type: 'int' cpp_type: 'int' } } "
              "params { type { lang_type: 'int' cpp_type: 'int' } } }");
  TestNoMatch("decltype: FUNC func { "
              "name { cpp_name: 'FuncTwoParams' } "
              "params { type { lang_type: 'int' cpp_type: 'int' } } "
              "params { type { lang_type: 'int' cpp_type: 'int' }"
              "         default_value: 'None' } }");
}

// Input parameter type-checking.  See the comment at
// "MatchAndSetInputParamType" for the different cases.
TEST_F(ClifMatcherTest, TestMatchAndSetFuncParamCase1) {
  TestNoMatch("decltype: FUNC func { "
              "name { cpp_name: 'FuncOneParam' } "
              "params { type { lang_type: 'int' cpp_type: 'int' "
              "                cpp_raw_pointer: true } } }");
}

TEST_F(ClifMatcherTest, TestMatchAndSetFuncParamCase2) {
  TestMatch("decltype: FUNC func { "
            "name { cpp_name: 'VoidFuncIntPointerParam' } "
            "params { type { lang_type: 'int' cpp_type: 'int' "
            "                cpp_raw_pointer: true } } }");
}

TEST_F(ClifMatcherTest, TestMatchAndSetFuncParamCase3) {
  TestMatch("decltype: FUNC func { "
            "name { cpp_name: 'VoidFuncIntPointerParam' } "
            "params { type { lang_type: 'int' cpp_type: 'int *' "
            "                cpp_raw_pointer: true } } }");
}

TEST_F(ClifMatcherTest, TestMatchAndSetFuncParamCase4) {
  TestNoMatch("decltype: FUNC func { "
              "name { cpp_name: 'FuncOneParam' } "
              "params { type { lang_type: 'int' cpp_type: 'int *' "
              "                cpp_raw_pointer: true } } }");
}

TEST_F(ClifMatcherTest, TestMatchAndSetFuncParamCase5) {
  TestMatch("decltype: FUNC func { "
            "name { cpp_name: 'FuncOneParam' } "
            "params { type { lang_type: 'int' cpp_type: 'int' } } } ");
}

TEST_F(ClifMatcherTest, TestMatchAndSetFuncParamCase6) {
  protos::Decl decl;
  TestMatch("decltype: FUNC func { "
            "name { cpp_name: 'VoidFuncIntPointerParam' } "
            "params { type { lang_type: 'int' cpp_type: 'int' } } }",
            &decl);
  EXPECT_TRUE(decl.func().params(0).type().cpp_raw_pointer());
}

TEST_F(ClifMatcherTest, TestMatchAndSetFuncParamCase7) {
  protos::Decl decl;
  TestNoMatch("decltype: FUNC func { "
              "name { cpp_name: 'FuncOneParam' } "
              "params { type { lang_type: 'int' cpp_type: 'int *' } } }");
}

TEST_F(ClifMatcherTest, TestMatchAndSetFuncParamCase8) {
  protos::Decl decl;
  TestMatch("decltype: FUNC func { "
            "name { cpp_name: 'VoidFuncIntPointerParam' } "
            "params { type { lang_type: 'int' cpp_type: 'int' } } }",
            &decl);
  EXPECT_TRUE(decl.func().params(0).type().cpp_raw_pointer());
}

TEST_F(ClifMatcherTest, TestMatchAndSetImplicitConversion) {
  protos::Decl decl;
  TestMatch(
      "decltype: FUNC func { "
      "  name { cpp_name: 'FuncImplicitConversion1' } "
      "  params { "
      "    type { "
      "      lang_type: 'ImplicitConvertFrom1' "
      "      cpp_type: 'ImplicitConvertFrom1' } "
      "  } }", &decl);
  EXPECT_EQ(decl.func().params(0).type().cpp_type(), "::ImplicitConvertFrom1");
  TestMatch(
      "decltype: FUNC func { "
      "  name { cpp_name: 'FuncImplicitConversion2' } "
      "  params { "
      "    type { "
      "      lang_type: 'ImplicitConvertFrom2' "
      "      cpp_type: 'ImplicitConvertFrom2' } "
      "  } }", &decl);
  EXPECT_EQ(decl.func().params(0).type().cpp_type(), "::ImplicitConvertTo");
}

TEST_F(ClifMatcherTest, TestMatchAndSetFuncParamConstRefDropped) {
  protos::Decl decl;
  TestMatch("decltype: FUNC func { "
            "name { cpp_name: 'VoidFuncConstIntRefParam' } "
            "params { type { lang_type: 'int' cpp_type: 'const int &' } } }",
            &decl);
  EXPECT_EQ(decl.func().params(0).type().cpp_type(), "int");
}

TEST_F(ClifMatcherTest, TestMatchAndSetFuncTemplateParamLValue) {
  TestMatch("decltype: FUNC func { "
            "name { cpp_name: 'FuncTemplateParamLValue' } "
            "params { type { "
            " lang_type: 'list<int>' "
            "cpp_type: 'ComposedType' "
            "params { "
            "  lang_type: 'int' "
            "  cpp_type: 'int' "
            "} } } } ");
  TestNoMatch("decltype: FUNC func { "
              "name { cpp_name: 'FuncTemplateParamLValue' } "
              "params { type { "
              " lang_type: 'list<int>' "
              "cpp_type: 'SpecializationsHaveConstructors' "
              "params { "
              "  lang_type: 'int' "
              "  cpp_type: 'int' "
              "} } } } ");
  protos::Decl decl;
  TestNoMatch("decltype: FUNC func { "
              "name { cpp_name: 'FuncTemplateParamLValue' } "
              "params { type { "
              " lang_type: 'list<int>' "
              "cpp_type: 'ComposedType' "
              "params { "
              "  lang_type: 'int' "
              "  cpp_type: 'multiparent'   "
              "} } } } ", &decl);
  EXPECT_TRUE(llvm::StringRef(decl.not_found()).contains(
      "ComposedType<int>"));
}

TEST_F(ClifMatcherTest, TestMatchAndSetFuncNamespaceParam0) {
  protos::Decl decl;
  TestMatch("decltype: FUNC func { "
            "name { cpp_name: 'VoidFuncNamespaceParam' } "
            "params { "
            "  type { lang_type: 'bClass' cpp_type: 'Namespace::bClass' } } }",
            &decl);
  EXPECT_EQ(decl.func().params(0).type().cpp_type(), "::Namespace::bClass");
}

TEST_F(ClifMatcherTest, TestMatchAndSetParamReference) {
  protos::Decl decl;
  TestMatch("decltype: FUNC func { "
            "name { cpp_name: 'const_ref_tests::PassByValue' } "
            "params { type { lang_type: 'ClassB' "
            "                cpp_type: 'const_ref_tests::ClassB' } } }");
  TestMatch("decltype: FUNC func { "
            "name { cpp_name: 'const_ref_tests::PassByConstRef' } "
            "params { type { lang_type: 'ClassB' "
            "                cpp_type: 'const_ref_tests::ClassB' } } }",
            &decl);
  EXPECT_EQ(decl.func().params(0).type().cpp_type(),
            "::const_ref_tests::ClassB");
  TestNoMatch("decltype: FUNC func { "
              "name { cpp_name: 'const_ref_tests::PassByRef' } "
              "params { type { lang_type: 'ClassB' "
              "                cpp_type: 'const_ref_tests::ClassB' } } }");
}

TEST_F(ClifMatcherTest, TestReferenceParameters) {
  TestNoMatch("decltype: FUNC func { "
              "name { cpp_name: 'VoidFuncNamespaceParam' } "
              "params { type { lang_type: 'bClass' "
              "                cpp_type: 'aClass' } } }");

  protos::Decl decl;
  TestMatch("decltype: FUNC func { "
            "name { cpp_name: 'FuncGloballyQualifiedNamePtrParam' } "
            "params { type { lang_type: 'bClass' "
            "         cpp_type: 'Globally::Qualified::ForwardDecl *' "
            "         cpp_raw_pointer: true } } }",
            &decl);
  EXPECT_EQ(decl.func().params(0).type().cpp_type(),
            "::Globally::Qualified::ForwardDecl *");
}

TEST_F(ClifMatcherTest, TestMatchUncopyableInputParamType) {
  TestNoMatch(
      "decltype: FUNC func { "
      "name { cpp_name: 'FuncUncopyableClassInputParam' } "
      "params { type { lang_type: 'UncopyableUnmovableClass' "
      "         cpp_type: 'UncopyableUnmovableClass' } } }");
  protos::Decl decl;
  // This test will pass, but the compiler will generate
  // an error because CLIF requires input parameters to be copyable.
  TestMatch(
      "decltype: FUNC func { "
      "name { cpp_name: 'FuncUncopyableClassConstRefInputParam' } "
      "params { type { lang_type: 'UncopyableUnmovableClass' "
      "         cpp_type: 'UncopyableUnmovableClass' } } }",
      &decl);
  EXPECT_EQ(decl.func().params(0).type().cpp_type(),
            "::UncopyableUnmovableClass");
  EXPECT_FALSE(decl.func().params(0).type().cpp_has_def_ctor());
  EXPECT_FALSE(decl.func().params(0).type().cpp_copyable());
  EXPECT_FALSE(decl.func().params(0).type().cpp_abstract());
}

TEST_F(ClifMatcherTest, TestMatchMovableButUncopyableOutputParamType) {
  protos::Decl decl;
  TestMatch(
      "decltype: CLASS class_ { "
      "  name { cpp_name: 'ClassMovableButUncopyable' } "
      "  members { "
      "    decltype: FUNC func { "
      "      name { cpp_name: 'FuncMovableButUncopyableOutputParam' } "
      "      returns { "
      "        type { "
      "           lang_type: 'ClassMovableButUncopyable' "
      "           cpp_type: 'ClassMovableButUncopyable' "
      "        } "
      "      } "
      "    } "
      "  } "
      "}",
      &decl);
  EXPECT_FALSE(decl.class_().cpp_copyable());
  EXPECT_TRUE(decl.class_().cpp_movable());
  EXPECT_EQ(decl.class_().members(0).func().returns(0).type().cpp_type(),
            "::ClassMovableButUncopyable");
  EXPECT_FALSE(
      decl.class_().members(0).func().returns(0).type().cpp_copyable());
  EXPECT_TRUE(decl.class_().members(0).func().returns(0).type().cpp_movable());
}

TEST_F(ClifMatcherTest, TestMatchOutputParamNonPtr) {
  protos::Decl decl;
  TestNoMatch(
      "decltype: CLASS class_ { "
      "  name { cpp_name: 'ClassMovableButUncopyable' } "
      "  members { "
      "    decltype: FUNC func { "
      "      name { cpp_name: 'FuncMovableButUncopyableOutputParamNonPtr' } "
      "      returns { "
      "        type { "
      "           lang_type: 'ClassMovableButUncopyable' "
      "           cpp_type: 'ClassMovableButUncopyable' "
      "        } "
      "      } "
      "    } "
      "  } "
      "}",
      &decl);
  EXPECT_TRUE(
      llvm::StringRef(decl.class_().members(0).not_found())
          .contains(
              "An output parameter must be either a pointer or a reference."));
}

TEST_F(ClifMatcherTest, TestMatchOutputParamConstPtr) {
  protos::Decl decl;
  TestNoMatch(
      "decltype: CLASS class_ { "
      "  name { cpp_name: 'ClassMovableButUncopyable' } "
      "  members { "
      "    decltype: FUNC func { "
      "      name { cpp_name: 'FuncMovableButUncopyableOutputParamConstPtr' } "
      "      returns { "
      "        type { "
      "           lang_type: 'ClassMovableButUncopyable' "
      "           cpp_type: 'ClassMovableButUncopyable' "
      "        } "
      "      } "
      "    } "
      "  } "
      "}",
      &decl);
  EXPECT_TRUE(llvm::StringRef(decl.class_().members(0).not_found())
                  .contains("Output parameter is constant."));
}

TEST_F(ClifMatcherTest, TestMatchUncopyableUnmovableOutputParamType) {
  protos::Decl decl;
  TestNoMatch(
      "decltype: FUNC func { "
      "name { cpp_name: 'FuncUncopyableUnmovableClassOutputParam' } "
      "returns { type { lang_type: 'UncopyableUnmovableClass' "
      "         cpp_type: 'UncopyableUnmovableClass' } } }",
      &decl);
  EXPECT_TRUE(llvm::StringRef(decl.not_found())
                  .contains("Clif expects output parameters or return types to "
                            "be copyable or movable."));
}

TEST_F(ClifMatcherTest, TestMatchFuncUncopyableUnmovableClassReturnType) {
  protos::Decl decl;
  TestNoMatch(
      "decltype: FUNC func { "
      "name { cpp_name: 'FuncUncopyableUnmovableClassReturnType' } "
      "returns { type { lang_type: 'UncopyableUnmovableClass' "
      "         cpp_type: 'UncopyableUnmovableClass' } } }",
      &decl);
  EXPECT_TRUE(llvm::StringRef(decl.not_found())
                  .contains("Clif expects output parameters or return types to "
                            "be copyable or movable."));
}

TEST_F(ClifMatcherTest, TestMatchSetDeletedOverloads) {
  protos::Decl decl;
  TestMatch(
      "decltype: CLASS class_ { "
      "  name { cpp_name: 'ClassWithDeletedCopyCtor' } "
      "    members { "
      "      decltype: FUNC func { constructor: true "
      "        name { cpp_name: 'ClassWithDeletedCopyCtor' } "
      "        params { "
      "          type { "
      "            lang_type: 'ClassWithDeletedCopyCtor' "
      "            cpp_type: 'ClassWithDeletedCopyCtor' "
      "          } "
      "        } "
      "      } "
      "    } "
      "}",
      &decl);
  EXPECT_EQ(decl.class_().members(0).func().params(0).type().cpp_type(),
            "::ClassWithDeletedCopyCtor *");
  TestNoMatch(
      "decltype: CLASS class_ { "
      "  name { cpp_name: 'ClassWithDeletedCopyCtor' } "
      "    members { "
      "      decltype: FUNC func { "
      "        name { cpp_name: 'DeletedFunc' } "
      "      } "
      "    } "
      "}",
      &decl);
  EXPECT_TRUE(llvm::StringRef(decl.class_().members(0).not_found())
                  .contains("C++ symbol \"DeletedFunc\" not found in "
                            "ClassWithDeletedCopyCtor.\n    Are you wrapping a "
                            "deleted method?"));
}

TEST_F(ClifMatcherTest, TestMatchSetTypeProperties) {
  protos::Decl decl;
  TestMatch("decltype: FUNC func { "
            "name { cpp_name: 'VoidFuncClassParamWithoutDefaultCtor' } "
            "params { type { lang_type: 'bClass' "
            "                cpp_type: 'ClassWithoutDefaultCtor' } } }",
            &decl);
  EXPECT_FALSE(decl.func().params(0).type().cpp_has_def_ctor());
  TestMatch("decltype: FUNC func { "
            "name { cpp_name: 'VoidFuncClassParamWithDefaultCtor' } "
            "params { type { lang_type: 'bClass' "
            "                cpp_type: 'ClassWithDefaultCtor' } } }",
            &decl);
  EXPECT_TRUE(decl.func().params(0).type().cpp_has_def_ctor());
  TestMatch("decltype: FUNC func { "
            "name { cpp_name: 'VoidFuncClassParamWithPrivateDefaultCtor' } "
            "params { type { lang_type: 'bClass' "
            "                cpp_type: "
            "                'ClassWithPrivateDefaultCtor' } } }",
            &decl);
  EXPECT_FALSE(decl.func().params(0).type().cpp_has_def_ctor());
  // Check for cpp_ctor flags.
  TestMatch(
      "decltype: CLASS class_ { "
      "name { cpp_name: 'ClassWithDeletedCopyCtor' } "
      "}", &decl);
  EXPECT_FALSE(decl.class_().cpp_copyable());
  EXPECT_FALSE(decl.class_().cpp_movable());
  EXPECT_FALSE(decl.class_().cpp_abstract());
  EXPECT_FALSE(decl.class_().cpp_has_trivial_defctor());
  EXPECT_TRUE(decl.class_().cpp_has_trivial_dtor());
  TestMatch(
      "decltype: CLASS class_ { "
      "name { cpp_name: 'ClassMovableButUncopyable' } "
      "}", &decl);
  EXPECT_FALSE(decl.class_().cpp_copyable());
  EXPECT_TRUE(decl.class_().cpp_movable());
  TestMatch(
      "decltype: CLASS class_ { "
      "name { cpp_name: 'ClassPureVirtual' } "
      "}", &decl);
  EXPECT_TRUE(decl.class_().cpp_has_def_ctor());
  EXPECT_TRUE(decl.class_().cpp_abstract());
  EXPECT_TRUE(decl.class_().cpp_copyable());
  EXPECT_TRUE(decl.class_().cpp_movable());
  TestMatch(
      "decltype: CLASS class_ { "
      "name { cpp_name: 'NoCopyAssign' } "
      "}", &decl);
  EXPECT_FALSE(decl.class_().cpp_copyable());
  EXPECT_FALSE(decl.class_().cpp_movable());
  EXPECT_TRUE(decl.class_().cpp_has_def_ctor());
  EXPECT_FALSE(decl.class_().cpp_has_trivial_defctor());
  EXPECT_TRUE(decl.class_().cpp_has_trivial_dtor());
  TestMatch(
      "decltype: CLASS class_ { "
      "name { cpp_name: 'AbstractClass' } "
      "}", &decl);
  EXPECT_TRUE(decl.class_().cpp_abstract());
  TestMatch(
      "decltype: CLASS class_ { "
      "name { cpp_name: 'PrivateDestructorClass' } "
      "}", &decl);
  EXPECT_FALSE(decl.class_().cpp_copyable());
  EXPECT_FALSE(decl.class_().cpp_movable());
  EXPECT_TRUE(decl.class_().cpp_has_trivial_defctor());
  EXPECT_FALSE(decl.class_().cpp_has_trivial_dtor());
  TestMatch(
      "decltype: CLASS class_ { "
      "name { cpp_name: 'ClassWithDefaultCtor' } "
      "}", &decl);
  EXPECT_TRUE(decl.class_().cpp_copyable());
  EXPECT_TRUE(decl.class_().cpp_movable());
  EXPECT_TRUE(decl.class_().cpp_has_def_ctor());
  EXPECT_FALSE(decl.class_().cpp_has_trivial_defctor());
  EXPECT_TRUE(decl.class_().cpp_has_trivial_dtor());
}

TEST_F(ClifMatcherTest, TestCppAbstract) {
  protos::Decl decl;
  TestMatch("decltype: FUNC func { "
            "name { cpp_name: 'FuncAbstractParam' } "
            "params { type { lang_type: 'ClassPureVirtual' "
            "         cpp_type: 'ClassPureVirtual' } } }", &decl);
  EXPECT_TRUE(decl.func().params(0).type().cpp_abstract());

  TestMatch("decltype: FUNC func { "
            "name { cpp_name: 'FuncAbstractParam' } "
            "params { type { lang_type: 'AbstractClass' "
            "         cpp_type: 'AbstractClass' } } }", &decl);
  EXPECT_TRUE(decl.func().params(0).type().cpp_abstract());
}

TEST_F(ClifMatcherTest, TestMatchAndSetTemplateTypes) {
  TestMatch("decltype: FUNC func { "
            "name { cpp_name: 'FuncTemplateParam' } "
            "params { type { lang_type: 'int' "
            "         cpp_type: 'ComposedType<int>' } } }");
  TestNoMatch("decltype: FUNC func { "
              "name { cpp_name: 'FuncTemplateParam' } "
              "params { type { lang_type: 'int' "
              "         cpp_type: 'ComposedType<float>' } } }");
}

TEST_F(ClifMatcherTest, TestMatchAndSetFuncMulti) {
  // More than one return type...
  TestMatch("decltype: FUNC func { "
            "name { cpp_name: 'FuncReturnsTwoInts' } "
            "returns { type { lang_type: 'int' cpp_type: 'int' } } "
            "returns { type { lang_type: 'int' cpp_type: 'int' } } }");
  TestMatch("decltype: FUNC func { "
            "name { cpp_name: 'FuncTwoParamsTwoReturns' } "
            "params { type { lang_type: 'int' cpp_type: 'int' } } "
            "params { type { lang_type: 'int' cpp_type: 'int' } } "
            "returns { type { lang_type: 'int' cpp_type: 'int'  } } "
            "returns { type { lang_type: 'int' cpp_type: 'int' } } }");
  TestNoMatch("decltype: FUNC func { "
              "name { cpp_name: 'FuncReturnsInt' }"
              "returns { type { lang_type: 'int' cpp_type: 'int' } } "
              "returns { type { lang_type: 'int' cpp_type: 'int' } } }");
  protos::Decl decl;
  TestNoMatch("decltype: FUNC func { "
              "name { cpp_name: 'UnwrappableFunction' }"
              "returns { type { lang_type: 'child' cpp_type: 'child' } } "
              "params { type { lang_type: 'int' cpp_type: 'int' } } }", &decl);
  EXPECT_TRUE(llvm::StringRef(decl.not_found()).contains(
      "Do all output parameters follow all input parameters?"));
}

TEST_F(ClifMatcherTest, TestMatchAndSetClass) {
  TestMatch("decltype: CLASS class_ { "
            "name { cpp_name: 'DerivedClass' } "
            "members { decltype: FUNC func { constructor: true "
            "   name { cpp_name: 'DerivedClass' } } } "
            "members { decltype: FUNC func { name { cpp_name: 'MemberA' } } } "
            "members { decltype: FUNC func { "
            "  name { cpp_name: 'MemberB' } "
            "  params { type { lang_type: 'int' cpp_type: 'int' } } "
            "  returns { type { lang_type: 'int' cpp_type: 'int' } } }"
            "} }");
  // First with the classmethod field set.
  TestMatch("decltype: CLASS class_ { "
            "name { cpp_name: 'aClass' } "
            "members { decltype: FUNC func { "
            "          classmethod: true "
            "          name { cpp_name: 'StaticMember' } } } }");

  // Now without the classmethod field set.
  TestNoMatch("decltype: CLASS class_ { "
              "name { cpp_name: 'aClass' } "
              "members { decltype: FUNC func { "
              "          name { cpp_name: 'StaticMember' } } } }");

  // Globally qualified-name without the classmethod field set should
  // match. (With the classmethod field set should be caught by the
  // parser.)
  TestMatch(" decltype: FUNC func { "
            "          name { cpp_name: 'aClass::StaticMember' } } ");

  // No constructor that takes an int parameter. So this shouldn't
  // match.
  TestNoMatch("decltype: CLASS class_ { "
              "name { cpp_name: 'aClass' } "
              "members { decltype: FUNC func { constructor: true "
              "   name { cpp_name: 'aClass' } "
              "   params { type { lang_type: 'int' cpp_type: 'int' } } } } }");

  // Match against a final class. Unfortunately, the negative case is
  // a compilation error of test.h, which our test harness doesn't
  // support well.
  TestMatch("decltype: CLASS class_ { "
            "name { cpp_name: 'aFinalClass' } "
            "members { decltype: FUNC func { "
            "  name { cpp_name: 'Foo' } "
            "  params { type { lang_type: 'aClass' cpp_type: 'aClass' } } } }"
            "final: true } ");
}

TEST_F(ClifMatcherTest, TestMatchAndSetClassTemplates) {
  // Default constructor lookup of non-template class.
  // If this doesn't work, then the test below it won't.
  TestMatch(
      "decltype: CLASS class_ { "
      "  name { cpp_name: 'AnotherClass' } "
      "  members { decltype: FUNC func { constructor: true "
      "        name { cpp_name: 'AnotherClass' } } } "
      "}");
  // Match a constructor of an explicit template type.
  protos::Decl decl;
  TestMatch(
      "decltype: CLASS class_ { "
      "  name { cpp_name: 'SpecializationsHaveConstructors<int>' } "
      "  members { decltype: FUNC func { constructor: true "
      "        name { cpp_name: 'SpecializationsHaveConstructors<int>' }"
      "        params { type { lang_type: 'int' cpp_type: 'int' } } } } "
      " } ", &decl);
  EXPECT_EQ(
      decl.class_().members(0).func().name().cpp_name(),
      "::SpecializationsHaveConstructors<int>::SpecializationsHaveConstructors"
  );
  // Match a constructor of an explicit template type.
  TestMatch(
      "decltype: CLASS class_ { "
      "  name { cpp_name: 'ComposedType<int>' } "
      "  members { decltype: FUNC func { constructor: true "
      "        name { cpp_name: 'ComposedType<int>' }"
      "        params { type { lang_type: 'int' cpp_type: 'int' } } "
      "} } }");
  TestMatch(
      "decltype: CLASS class_ { "
      "  name { cpp_name: 'TypedeffedTemplate' } "
      "  members { decltype: FUNC func { constructor: true "
      "        name { cpp_name: 'TypedeffedTemplate' } "
      "        params { type { lang_type: 'int' cpp_type: 'int' } } "
      "} } }");
  TestMatch(
      "decltype: CLASS "
      "  cpp_file: 'clif/backend/test.h' "
      "  class_ { "
      "  name { cpp_name: 'ClassTemplateDeclaredInImportedFile' } "
      "  members { decltype: FUNC func { "
      "        name { cpp_name: 'ClassTemplateInAnotherFile' } "
      "        constructor: true }"
      "} } ");
  TestMatch(
      "decltype: CLASS "
      "  cpp_file: 'clif/backend/test.h' "
      "  class_ { "
      "  name { cpp_name: 'ClassTemplateDeclaredInImportedFile' } "
      "  members { decltype: FUNC func { "
      "        name { cpp_name: 'SomeFunction' } "
      "        params { type { lang_type: 'int' cpp_type: 'int' } } "
      "        returns { type { lang_type: 'int' cpp_type: 'int' } } }"
      "} } ");
  TestMatch(
      "decltype: CLASS "
      "  cpp_file: 'clif/backend/test.h' "
      "  class_ { "
      "  name { cpp_name: 'ClassTemplateDeclaredInImportedFile2' } "
      "  members { decltype: FUNC func { "
      "        name { cpp_name: 'SomeFunction' } "
      "        params { type { lang_type: 'AnotherClass'  "
      "                        cpp_type: 'AnotherClass' } } "
      "        returns { type { lang_type: 'AnotherClass' "
      "                         cpp_type: 'AnotherClass' } } }"
      "} } ");
  TestNoMatch(
      "decltype: CLASS "
      "  cpp_file: 'clif/backend/test.h' "
      "  class_ { "
      "  name { cpp_name: 'ClassInAnotherFile' } "
      "  members { decltype: FUNC func { "
      "        name { cpp_name: 'SomeFunction' } "
      "        params { type { lang_type: 'int' cpp_type: 'int' } } "
      "        returns { type { lang_type: 'int' cpp_type: 'int' } } }"
      "} } ",
      &decl);
  EXPECT_TRUE(
      llvm::StringRef(decl.not_found())
          .contains(
              "Declaration was found, but not inside the required file."));
}

TEST_F(ClifMatcherTest, TestMatchAndSetConversionFunction) {
  // test case for conversion function operator bool()
  protos::Decl decl;
  TestMatch(
      "decltype: CLASS class_ { "
      "name { cpp_name: 'ConversionClass' } "
      "members { decltype: FUNC func { name { cpp_name: 'operator bool' }  "
      "          returns { type { lang_type: 'bool' cpp_type: 'bool' } } } } }",
      &decl);
  EXPECT_FALSE(decl.class_().members(0).func().cpp_opfunction());
  TestNoMatch(
      "decltype: CLASS class_ { "
      "name { cpp_name: 'ConversionClass' } "
      "members { decltype: FUNC func { name { cpp_name: 'operator double' }  "
      "          returns { type { lang_type: 'double' cpp_type: 'double' } } } "
      "} }",
      &decl);
}

TEST_F(ClifMatcherTest, TestMatchAndSetOperatorOverload) {
  // Global operator, matched outside of class, so no added implicit "this".
  TestMatch("decltype: FUNC func { name {"
            "cpp_name: 'operator==' }"
            "params { type { lang_type: 'int' cpp_type: 'grandmother' } } "
            "params { type { lang_type: 'int' cpp_type: 'grandfather' } } "
            "returns { type { lang_type: 'int' cpp_type: 'bool' } } }");

  // Add unit test for operator* in different invoking cases.
  protos::Decl decl;
  // operatorX declared outside of class in .h file
  TestMatch(
      "decltype: CLASS class_ { "
      "name { cpp_name: 'OperatorClass' } "
      "members { decltype: FUNC func { name { native: '__rmul__' cpp_name: "
      "'operator*' } "
      "    params { type { lang_type: 'int' cpp_type: 'int' } } "
      "    params { type { lang_type: 'OperatorClass' "
      "cpp_type: 'OperatorClass' } } "
      "    returns { type { lang_type: 'int' cpp_type: 'int' } } "
      "    cpp_opfunction: true } } } ",
      &decl);
  EXPECT_TRUE(decl.class_().members(0).func().cpp_opfunction());
  EXPECT_EQ(decl.class_().members(0).func().name().cpp_name(),
            "::operator*");

  TestMatch(
      "decltype: CLASS class_ { "
      "name { cpp_name: 'user::OperatorClass3' } "
      "members { decltype: FUNC func { name { native: '__rmul__' cpp_name: "
      "'operator*' } "
      "    params { type { lang_type: 'int' cpp_type: 'int' } } "
      "    params { type { lang_type: 'OperatorClass3' "
      "cpp_type: 'user::OperatorClass3' } } "
      "    returns { type { lang_type: 'int' cpp_type: 'int' } } "
      "    cpp_opfunction: true } } } ",
      &decl);
  EXPECT_TRUE(decl.class_().members(0).func().cpp_opfunction());
  EXPECT_EQ(decl.class_().members(0).func().name().cpp_name(),
            "::user::operator*");

  TestMatch(
      "decltype: CLASS class_ { "
      "name { cpp_name: 'user::OperatorClass3' } "
      "members { decltype: FUNC func { name { native: '__radd__' cpp_name: "
      "'operator+' } "
      "    params { type { lang_type: 'int' cpp_type: 'int' } } "
      "    params { type { lang_type: 'OperatorClass3' "
      "cpp_type: 'user::OperatorClass3' } } "
      "    returns { type { lang_type: 'int' cpp_type: 'int' } } "
      "    cpp_opfunction: true } } } ",
      &decl);
  EXPECT_TRUE(decl.class_().members(0).func().cpp_opfunction());
  EXPECT_EQ(decl.class_().members(0).func().name().cpp_name(),
            "::operator+");

  // operator* declared outside of class in .h file
  TestMatch(
      "decltype: CLASS class_ { "
      "name { cpp_name: 'OperatorClass' } "
      "members { decltype: FUNC func { name { native: 'Deref' cpp_name: "
      "'operator*' } "
      "          returns { type { lang_type: 'int' cpp_type: 'int' } } } } } ",
      &decl);
  EXPECT_TRUE(decl.class_().members(0).func().cpp_opfunction());

  // operator* declared inside class in .h file
  TestMatch(
      "decltype: CLASS class_ { "
      "name { cpp_name: 'OperatorClass2' } "
      "members { decltype: FUNC func { name { native: 'Deref' cpp_name: "
      "'operator*' } "
      "          returns { type { lang_type: 'int' cpp_type: 'int' } } } } } ",
      &decl);
  EXPECT_FALSE(decl.class_().members(0).func().cpp_opfunction());

  // Class operator, no added implicit this.
  TestMatch(
      "decltype: CLASS class_ { "
      "name { cpp_name: 'OperatorClass' } "
      "members { decltype: FUNC func { name { cpp_name: 'operator==' }  "
      "          returns { type { lang_type: 'int' cpp_type: 'bool' } } "
      "          params { type { lang_type: 'OperatorClass'"
      "                   cpp_type: 'OperatorClass' } } } } }", &decl);
  EXPECT_FALSE(decl.class_().members(0).func().cpp_opfunction());
  // Class operator searched outside of class, so added implicit this.
  TestMatch(
      "decltype: CLASS class_ { "
      "name { cpp_name: 'OperatorClass' } "
      "members { decltype: FUNC func { name { cpp_name: 'operator!=' }  "
      "          returns { type { lang_type: 'int' cpp_type: 'bool' } } "
      "          params { type { lang_type: 'OperatorClass'"
      "                   cpp_type: 'OperatorClass' } } }  "
      "} }", &decl);
  EXPECT_TRUE(decl.class_().members(0).func().cpp_opfunction());
  // Operator with fully-qualified name. Must match exactly.
  TestMatch("decltype: FUNC func { name {"
            "cpp_name: 'a_user::defined_namespace::operator==' }"
            "params { type { cpp_type: 'Class' } } "
            "params { type { cpp_type: 'int' } } "
            "returns { type { cpp_type: 'bool' } } }", &decl);
  // Operator with fully-qualified name inside class. Must match exactly.
  TestMatch("decltype: CLASS class_ { "
            "name { cpp_name: 'Class' } "
            "members { decltype: FUNC func { name {"
            "  cpp_name: 'a_user::defined_namespace::operator==' }"
            "  params { type { cpp_type: 'Class' } } "
            "  params { type { cpp_type: 'int' } } "
            "  returns { type { cpp_type: 'bool' } } } } }", &decl);
  // Set cpp_opfunction when the match is outside a class.
  EXPECT_TRUE(decl.class_().members(0).func().cpp_opfunction());
}

TEST_F(ClifMatcherTest, TestBaseClassSetter) {
  protos::Decl decl;
  TestMatch("decltype: CLASS class_ { name { cpp_name: 'child' } } ", &decl);
  EXPECT_EQ(decl.class_().bases(0).cpp_name(), "::parent");
  EXPECT_EQ(decl.class_().bases(1).cpp_name(), "::GrandParents::grandparent");
  EXPECT_EQ(decl.class_().bases(2).cpp_name(),
            "::GrandParents::greatgrandparent");
  EXPECT_EQ(decl.class_().cpp_bases(0).name(), "::parent");
  EXPECT_EQ(decl.class_().cpp_bases(1).name(), "::GrandParents::grandparent");
  EXPECT_EQ(decl.class_().cpp_bases(1).namespace_(), "GrandParents");
  EXPECT_EQ(decl.class_().cpp_bases(1).name(),
            decl.class_().bases(1).cpp_name());
  EXPECT_TRUE(
      llvm::StringRef(decl.class_().cpp_bases(2).filename()).endswith(
          "test.h"));

  TestMatch("decltype: CLASS class_ { name { cpp_name: 'derive1' } } ", &decl);
  EXPECT_EQ(decl.class_().bases_size(), 2);
  EXPECT_EQ(decl.class_().bases(0).cpp_name(), "::base1");
  EXPECT_EQ(decl.class_().bases(1).cpp_name(), "::base1_1");
  EXPECT_EQ(decl.class_().cpp_bases_size(), 2);
  EXPECT_EQ(decl.class_().cpp_bases(0).name(), "::base1");
  EXPECT_EQ(decl.class_().cpp_bases(1).name(), "::base1_1");
}

TEST_F(ClifMatcherTest, TestBaseClassRegularDiamondInheritance) {
  // Test for diamond inheritance. "base2_1" should only be reported once.
  protos::Decl decl;
  TestMatch("decltype: CLASS class_ { name { cpp_name: 'derive2' } } ", &decl);
  EXPECT_EQ(decl.class_().bases_size(), 3);
  EXPECT_EQ(decl.class_().bases(0).cpp_name(), "::base2");
  EXPECT_EQ(decl.class_().bases(1).cpp_name(), "::base3");
  EXPECT_EQ(decl.class_().bases(2).cpp_name(), "::base2_1");
  EXPECT_EQ(decl.class_().cpp_bases_size(), 3);
  EXPECT_EQ(decl.class_().cpp_bases(0).name(), "::base2");
  EXPECT_EQ(decl.class_().cpp_bases(1).name(), "::base3");
  EXPECT_EQ(decl.class_().cpp_bases(2).name(), "::base2_1");
}

TEST_F(ClifMatcherTest, TestBaseClassTemplateDiamondInheritance) {
  // Test for template class's diamond inheritance. "base4<int>" should only be
  // reported once.
  protos::Decl decl;
  TestMatch("decltype: CLASS class_ { name { cpp_name: 'derive3_int' } } ",
            &decl);
  EXPECT_EQ(decl.class_().bases_size(), 3);
  EXPECT_EQ(decl.class_().bases(0).cpp_name(), "::base5<int>");
  EXPECT_EQ(decl.class_().bases(1).cpp_name(), "::base6<int>");
  EXPECT_EQ(decl.class_().bases(2).cpp_name(), "::base4<int>");
  EXPECT_EQ(decl.class_().cpp_bases_size(), 3);
  EXPECT_EQ(decl.class_().cpp_bases(0).name(), "::base5<int>");
  EXPECT_EQ(decl.class_().cpp_bases(1).name(), "::base6<int>");
  EXPECT_EQ(decl.class_().cpp_bases(2).name(), "::base4<int>");
}

TEST_F(ClifMatcherTest, TestBaseClassNonVirtualDiamondInheritance) {
  protos::Decl decl;
  TestNoMatch("decltype: CLASS class_ { name { cpp_name: 'derive4' } } ",
              &decl);
  EXPECT_TRUE(llvm::StringRef(decl.not_found())
                  .contains("Non-virtual diamond inheritance."));
}

TEST_F(ClifMatcherTest, TestMatchAndSetEnum) {
  // Note that this intentionally omits enumerator 'd' from the test.h
  // declaration. The returned proto got the 'd' added.
  protos::Decl decl;
  TestMatch(
      "decltype: ENUM enum { "
      "name { cpp_name: 'anEnum' native: 'anEnum' } "
      "members { cpp_name: 'a' native: 'a' } "
      "members { cpp_name: 'b' native: 'b' } "
      "members { cpp_name: 'c' native: 'c' } "
      "} namespace_: 'Namespace'", &decl);
  EXPECT_EQ(decl.enum_().members(0).cpp_name(), "::Namespace::anEnum::a");
  EXPECT_EQ(decl.enum_().members(1).cpp_name(), "::Namespace::anEnum::b");
  EXPECT_EQ(decl.enum_().members(2).cpp_name(), "::Namespace::anEnum::c");
  EXPECT_EQ(decl.enum_().members(3).cpp_name(), "::Namespace::anEnum::d");
  EXPECT_TRUE(decl.enum_().enum_class());

  // This is a non-class enum.
  TestMatch(
      "decltype: ENUM enum { "
      "name { cpp_name: 'anotherEnum' native: 'anotherEnum' } "
      "members { cpp_name: 'e' native: 'e' } "
      "members { cpp_name: 'f' native: 'f' } "
      "members { cpp_name: 'g' native: 'g' } "
      "} namespace_: 'Namespace'", &decl);
  EXPECT_EQ(decl.enum_().members(0).cpp_name(), "::Namespace::e");
  EXPECT_EQ(decl.enum_().members(1).cpp_name(), "::Namespace::f");
  EXPECT_EQ(decl.enum_().members(2).cpp_name(), "::Namespace::g");
  EXPECT_EQ(decl.enum_().members(3).cpp_name(), "::Namespace::h");
  EXPECT_FALSE(decl.enum_().enum_class());

  // Everything should match but the 'e'.
  TestNoMatch(
      "decltype: ENUM enum { "
      "name { cpp_name: 'anEnum' native: 'anEnum' } "
      "members { cpp_name: 'a' native: 'a' } "
      "members { cpp_name: 'b' native: 'b' } "
      "members { cpp_name: 'c' native: 'c' } "
      "members { cpp_name: 'e' native: 'e' } "
      "} namespace_: 'Namespace'", &decl);
  EXPECT_TRUE(llvm::StringRef(decl.not_found())
                  .contains("Extra enumerators in Clif enum declaration "
                            "anEnum.  C++ enum Namespace::anEnum does "
                            "not contain enumerator(s): e"));

  // Type mismatch check.
  TestNoMatch("decltype: ENUM enum { "
              "name { cpp_name: 'aClass' } "
              "members { cpp_name: 'a' native: 'a' } "
              "members { cpp_name: 'b' native: 'b' } "
              "members { cpp_name: 'c' native: 'c' } "
              "members { cpp_name: 'e' native: 'e' } "
              "}", &decl);
  EXPECT_TRUE(llvm::StringRef(decl.not_found())
                  .contains("name matched \"aClass\" which is a C++ class"));

  TestMatch("decltype: CLASS class_ { "
            "name { cpp_name: 'Namespace::UsingClass' } "
            "members { "
            "decltype: ENUM enum { "
            "name { native: 'some_name' "
            "       cpp_name: 'anEnumHiddenInAUsingDeclaration' } "
            "members { cpp_name: 'a' native: 'a' } "
            "members { cpp_name: 'b' native: 'b' } "
            "members { cpp_name: 'c' native: 'c' } "
            "} } }");
}

TEST_F(ClifMatcherTest, TestMatchAndSetVar) {
  // Have to wrap this in a class because clif doesn't support
  // non-class member vars.
  TestMatch("decltype: CLASS class_ { "
            "name { cpp_name: 'Namespace::bClass' } "
            "members { "
            "  decltype: VAR var { "
            "    name { cpp_name: 'x' } "
            "    type { cpp_type: 'int' } "
            "} } }");
  // Test a not-found.
  TestNoMatch("decltype: CLASS class_ { "
              "name { cpp_name: 'Namespace::bClass' } "
              "members { "
              "  decltype: VAR var { "
              "    name { cpp_name: 'notfound' } "
              "    type { lang_type: 'float' cpp_type: 'float' } "
              "} } }");
  // // Type mismatch check
  // TestNoMatch("decltype: CLASS class_ { "
  //             "name { cpp_name: 'anEnum' } "
  //             "members { "
  //             "  decltype: VAR var { "
  //             "    name { cpp_name: 'x' } "
  //             "    type { cpp_type: 'int' } "
  //             "} } }");
}

TEST_F(ClifMatcherTest, TestMatchAndSetConst1) {
  protos::Decl decl;
  TestMatch("decltype: CONST const { "
            "  name { cpp_name: 'sample' } "
            "  type { cpp_type: 'int' } "
            "}", &decl);
  EXPECT_EQ(decl.const_().name().cpp_name(), "::sample");

  // enum constants
  // builtin type
  TestMatch("decltype: CONST const { "
            "  name { cpp_name: 'e' } "
            "  type { cpp_type: 'int' } "
            "}", &decl);
  EXPECT_EQ(decl.const_().name().cpp_name(), "::Namespace::e");
  // Non-builtin integer compatible type
  TestMatch("decltype: CONST const { "
            "  name { cpp_name: 'e' } "
            "  type { cpp_type: 'typedeffed_int' } "
            "} namespace_: 'Namespace'", &decl);
  EXPECT_EQ(decl.const_().name().cpp_name(), "::Namespace::e");
  // incompatible type
  TestNoMatch("decltype: CONST const { "
            "  name { cpp_name: 'e' } "
            "  type { cpp_type: 'string' } "
            "}");
  // class level constants
  TestMatch("decltype: CLASS class_ { "
            "name { cpp_name: 'aClass' } "
            "members { "
            " decltype: CONST const { "
            "  name { cpp_name: 'constant_int' } "
            "  type { lang_type: 'constant_int' cpp_type: 'const int' } "
            " } } "
            "members { "
            " decltype: CONST const { "
            "  name { cpp_name: 'kStringConst' } "
            "  type { lang_type: 'stringconst' cpp_type: 'const char *' } }}"
            "members { "
            " decltype: CONST const { "
            "  name { cpp_name: 'kAnotherStringConst' } "
            "  type { lang_type: 'stringconst' cpp_type: 'const char *' } "
            " } } } ", &decl);
  EXPECT_EQ(decl.class_().members(1).const_().type().cpp_type(),
            "::clif::char_ptr");
  EXPECT_EQ(decl.class_().members(2).const_().type().cpp_type(),
            "::clif::char_ptr");
  // Test a not-found.
  TestNoMatch("decltype: CONST const { "
              "  name { cpp_name: 'notfound' } "
              "  type { lang_type: 'float' cpp_type: 'float' } "
              "}");
  // Type mismatch check
  TestNoMatch("decltype: CONST const { "
              "  name { cpp_name: 'aClass' } "
              "  type { lang_type: 'float' cpp_type: 'float' } "
              "}");
  // nonconst check
  TestNoMatch("decltype: CONST const { "
              "  name { cpp_name: 'simple' } "
              "}");
}

TEST_F(ClifMatcherTest, TestFuncFieldsFilled) {
  // Ensure the cpp_names actually gets the fully-qualified name.
  std::string decl_proto = "decltype: FUNC func {"
      "name { cpp_name: 'int_id' } "
      "returns { type { lang_type: 'int' cpp_type: 'int' } } "
      "params { type { lang_type: 'int' cpp_type: 'int' } } "
      " } ";
  protos::Decl decl;
  TestMatch(decl_proto, &decl);
  EXPECT_EQ(decl.func().name().cpp_name(), "::some::int_id");
}

TEST_F(ClifMatcherTest, TestClassFieldsFilled) {
  // Ensure the cpp_names actually gets the fully-qualified name.
  protos::Decl decl;
  TestMatch("decltype: CLASS class_ {"
            "name { cpp_name: 'Namespace::bClass' } "
            " } ", &decl);
  EXPECT_EQ(decl.class_().name().cpp_name(), "::Namespace::bClass");
  EXPECT_TRUE(decl.class_().cpp_has_def_ctor());
  EXPECT_TRUE(decl.class_().cpp_has_trivial_defctor());
  EXPECT_TRUE(decl.class_().cpp_has_trivial_dtor());
  TestMatch("decltype: CLASS class_ {"
            "name { cpp_name: 'ClassWithoutDefaultCtor' } "
            "}", &decl);
  EXPECT_FALSE(decl.class_().cpp_has_def_ctor());
  EXPECT_TRUE(decl.class_().cpp_has_public_dtor());
  EXPECT_TRUE(decl.class_().cpp_has_trivial_dtor());
}

TEST_F(ClifMatcherTest, TestPrivateDestructor) {
  protos::Decl decl;
  TestMatch("decltype: CLASS class_ {"
            "name { cpp_name: 'PrivateDestructorClass' } "
            " } ", &decl);
  EXPECT_FALSE(decl.class_().cpp_has_def_ctor());
  EXPECT_FALSE(decl.class_().cpp_has_public_dtor());
}

TEST_F(ClifMatcherTest, TestTypePromotion) {
  std::string decl_proto = "decltype: FUNC func {"
      "name { cpp_name: 'UnsignedLongLongReturn' } "
      "returns { type { lang_type: 'int' cpp_type: 'int' } } "
      " } ";
  protos::Decl decl;
  TestMatch(decl_proto, &decl);
  EXPECT_EQ(decl.func().returns(0).type().cpp_type(), "unsigned long long");
  TestNoMatch("decltype: FUNC func {"
              "name { cpp_name: 'TakesBool' } "
              "params { type { lang_type: 'int' cpp_type: 'int' } } "
              " } ");
  TestNoMatch("decltype: FUNC func {"
              "name { cpp_name: 'TakesInt' } "
              "params { type { lang_type: 'bool' cpp_type: 'bool' } } "
              " } ");
  TestNoMatch("decltype: FUNC func {"
              "name { cpp_name: 'TakesFloat' } "
              "params { type { lang_type: 'int' cpp_type: 'int' } } "
              " } ");
  TestNoMatch("decltype: FUNC func {"
              "name { cpp_name: 'TakesPtr' } "
              "params { type { lang_type: 'bool' cpp_type: 'bool' } } "
              " } ");
}

TEST_F(ClifMatcherTest, TestOverloadedCallable) {
  std::string decl_proto = "decltype: FUNC func {"
      "name { cpp_name: 'OverloadedFunction' } "
      "params { type { "
      "           callable { "
      "             params { type { lang_type: 'char' cpp_type: 'child' } } "
      " } } } } ";
  protos::Decl decl;
  TestMatch(decl_proto, &decl);
  EXPECT_EQ(decl.not_found(), "");
  decl_proto = "decltype: FUNC func {"
      "name { cpp_name: 'OverloadedFunction' } "
      "params { type { "
      "           callable { "
      "             params { type { lang_type: 'char' cpp_type: 'parent' } } "
      " } } } } ";
  TestNoMatch(decl_proto, &decl);
}

TEST_F(ClifMatcherTest, TestCallableTemplateArgWithInput) {
  std::string decl_proto =
      "decltype: FUNC func {"
      "name { cpp_name: 'CallableTemplateArgFunction' } "
      "params { type { "
      "           cpp_type: '::example::Vector' "
      "             params { "
      "               callable { "
      "                 params { "
      "                   type { "
      "                     cpp_type: 'child' "
      "                   } "
      "                 } "
      "                 params { "
      "                   type { "
      "                     cpp_type: 'int' "
      "                   } "
      "                 } "
      "               } "
      "             } "
      "           } "
      "        } "
      "  } ";
  protos::Decl decl;
  TestMatch(decl_proto, &decl);
  EXPECT_EQ(decl.func().params(0).type().cpp_type(),
            "::example::Vector< ::std::function<void (child, int)>>");
}

TEST_F(ClifMatcherTest, TestCallableTemplateArgWithReturn) {
  std::string decl_proto =
      "decltype: FUNC func {"
      "name { cpp_name: 'CallableTemplateArgFunction2' } "
      "params { type { "
      "           cpp_type: '::example::Vector' "
      "             params { "
      "               callable { "
      "                 returns { "
      "                   type  { cpp_type: 'child' } "
      "                 } "
      "               } "
      "            } "
      "          } "
      "      } "
      "  }";
  protos::Decl decl;
  TestMatch(decl_proto, &decl);
  EXPECT_EQ(decl.func().params(0).type().cpp_type(),
            "::example::Vector< ::std::function<child ()>>");
}

TEST_F(ClifMatcherTest, TestCallableTemplateArgTooManyReturn) {
  // Too many returns for callable(std::function) will result in compilation
  // errors.
  std::string decl_proto =
      "decltype: FUNC func {"
      "name { cpp_name: 'CallableTemplateArgFunction2' } "
      "params { type { "
      "           cpp_type: '::example::Vector' "
      "             params { "
      "               callable { "
      "                 returns { "
      "                   type  { cpp_type: 'child' } "
      "                 } "
      "                 returns { "
      "                   type  { cpp_type: 'int' } "
      "                 } "
      "               } "
      "            } "
      "          } "
      "      } "
      "  }";
  TestNoMatch(decl_proto);
}

TEST_F(ClifMatcherTest, TestCallableTemplateArgWithBothInputAndReturn) {
  std::string decl_proto =
      "decltype: FUNC func {"
      "name { cpp_name: 'CallableTemplateArgFunction3' } "
      "params { type { "
      "           cpp_type: '::example::Vector' "
      "             params { "
      "               callable { "
      "                 params { "
      "                   type { "
      "                     cpp_type: 'child' "
      "                   } "
      "                 } "
      "                 returns { "
      "                   type  { cpp_type: 'int' } "
      "                 } "
      "               } "
      "            } "
      "          } "
      "      } "
      "  }";
  protos::Decl decl;
  TestMatch(decl_proto, &decl);
  EXPECT_EQ(decl.func().params(0).type().cpp_type(),
            "::example::Vector< ::std::function<int (child)>>");
}

TEST_F(ClifMatcherTest, TestOutputCallableTemplateArg) {
  std::string decl_proto =
      "decltype: FUNC func {"
      "name { cpp_name: 'CallableTemplateArgFunction4' } "
      "returns { type { "
      "           cpp_type: '::example::Vector' "
      "             params { "
      "               callable { "
      "                 params { "
      "                   type { "
      "                     cpp_type: 'int' "
      "                   } "
      "                 } "
      "               } "
      "            } "
      "          } "
      "      } "
      "  }";
  protos::Decl decl;
  TestMatch(decl_proto, &decl);
  EXPECT_EQ(decl.func().returns(0).type().cpp_type(),
            "::example::Vector< ::std::function<void (int)>>");
}

TEST_F(ClifMatcherTest, TestReturnCallableTemplateArg) {
  std::string decl_proto =
      "decltype: FUNC func {"
      "name { cpp_name: 'CallableTemplateArgFunction5' } "
      "returns { type { "
      "           cpp_type: '::example::Vector' "
      "             params { "
      "               callable { "
      "                 params { "
      "                   type { "
      "                     cpp_type: 'int' "
      "                   } "
      "                 } "
      "               } "
      "            } "
      "          } "
      "      } "
      "  }";
  protos::Decl decl;
  TestMatch(decl_proto, &decl);
  EXPECT_EQ(decl.func().returns(0).type().cpp_type(),
            "::example::Vector< ::std::function<void (int)>>");
}

TEST_F(ClifMatcherTest, TestConstRefCallable) {
  std::string decl_proto =
      "decltype: FUNC func {"
      "name { native: 'SimpleCallbackNonConstRef' "
      "       cpp_name: 'FunctionSimpleCallbackNonConstRef' }"
      "params { name { native: 'input'  cpp_name: 'input' }"
      "         type { lang_type: 'int' cpp_type: 'int' } }"
      "params { name { native: 'callback' cpp_name: 'callback' }"
      "         type { lang_type: '(in:int)->None' "
      "           callable { "
      "             params { "
      "               name { native: 'in' cpp_name: 'in' } "
      "               type { lang_type: 'int' cpp_type: 'int' } } "
      " } } } } ";
  protos::Decl decl;
  TestMatch(decl_proto, &decl);
  EXPECT_TRUE(decl.func().params(1).type().has_callable());
  EXPECT_EQ(decl.func().name().cpp_name(),
            "::FunctionSimpleCallbackNonConstRef");
  decl_proto =
      "decltype: FUNC func {"
      "name { native: 'SimpleCallbackConstRef' "
      "       cpp_name: 'FunctionSimpleCallbackConstRef' }"
      "params { name { native: 'input'  cpp_name: 'input' }"
      "         type { lang_type: 'int' cpp_type: 'int' } }"
      "params { name { native: 'callback' cpp_name: 'callback' }"
      "         type { lang_type: '(in:int)->None' "
      "           callable { "
      "             params { "
      "               name { native: 'in' cpp_name: 'in' } "
      "               type { lang_type: 'int' cpp_type: 'int' } } "
      " } } } } ";
  TestMatch(decl_proto, &decl);
  EXPECT_TRUE(decl.func().params(1).type().has_callable());
  EXPECT_EQ(decl.func().name().cpp_name(), "::FunctionSimpleCallbackConstRef");
}

TEST_F(ClifMatcherTest, TestNoModifyInputFQName) {
  protos::Decl decl;
  std::string decl_proto = "decltype: FUNC func {"
      "name { cpp_name: 'FunctionWithPartiallyQualifiedDecl' } "
      "params { type { "
      "           lang_type: 'char'"
      "           cpp_type: '::Globally::Qualified::ForwardDecl *' } } "
      " }";
  TestMatch(decl_proto, &decl);
  EXPECT_EQ(decl.func().params(0).type().cpp_type(),
            "::Globally::Qualified::ForwardDecl *");
}

TEST_F(ClifMatcherTest, TestConstVsNonConstFuncParams) {
  protos::Decl decl;
  std::string decl_proto = "decltype: FUNC func {"
      "name { cpp_name: 'FuncConstVsNonConst' } "
      "params { type { "
      "           lang_type: 'int'"
      "           cpp_type: 'int' } } "
      "params { type { "
      "           lang_type: 'int'"
      "           cpp_type: 'int' } } "
      " }";
  TestMatch(decl_proto, &decl);
  EXPECT_EQ(decl.func().params(0).type().cpp_type(), "int");
  // Make sure we break ties with const methods.
  TestMatch(
      "decltype: CLASS class_ { "
      "name { cpp_name: 'ClassWithDefaultCtor' } "
      "  members: { decltype: FUNC func {"
      "    name { cpp_name: 'MethodConstVsNonConst' } "
      "  } }"
      "}");
}

TEST_F(ClifMatcherTest, TestFunctionTemplate) {
  std::string decl_proto =
      "decltype: FUNC func {"
      "name { cpp_name: 'SimpleFunctionTemplate' } "
      "params { type { lang_type: 'int' cpp_type: 'int' } } "
      " } ";
  protos::Decl decl;
  TestMatch(decl_proto, &decl);
  EXPECT_EQ(decl.func().params(0).type().cpp_type(), "int");

  decl_proto = "decltype: FUNC func {"
      "name { cpp_name: 'PointerArgTemplate' } "
      "params { type { lang_type: 'int' cpp_type: 'int' } } "
      " } ";
  TestMatch(decl_proto, &decl);
  EXPECT_EQ(decl.func().params(0).type().cpp_type(), "int *");

  // Composed type passed to a template-deduction argument.
  decl_proto =
      "decltype: FUNC func {"
      "  name { cpp_name: 'SimpleFunctionTemplate'} "
      "  params { "
      "    type { "
      "       lang_type: 'list<int>' cpp_type: 'ComposedType' "
      "       params { lang_type: 'int' cpp_type: 'int' } "
      "    } } } ";
  TestMatch(decl_proto);

  decl_proto =
      "decltype: FUNC func {"
      "name { cpp_name: 'FunctionTemplateConst' } "
      "params { type { lang_type: 'int' cpp_type: 'int' } } "
      " } ";
  TestMatch(decl_proto, &decl);
  EXPECT_EQ(decl.func().params(0).type().cpp_type(), "int");
}

TEST_F(ClifMatcherTest, TestFunctionTemplateIncomplete) {
  protos::Decl decl;
  std::string decl_proto =
      "decltype: FUNC func {"
      "name { cpp_name: 'UndeducableTemplate' } "
      "params { type { lang_type: 'int' cpp_type: 'int' } } "
      " } ";
  TestNoMatch(decl_proto, &decl);
  EXPECT_TRUE(llvm::StringRef(decl.not_found())
                  .contains("Template argument deduction did not deduce a "
                            "value for every template parameter."));
}

TEST_F(ClifMatcherTest, TestFunctionTemplateTooFewArguments) {
  protos::Decl decl;
  std::string decl_proto =
      "decltype: FUNC func {"
      "name { cpp_name: 'SimpleFunctionTemplate' } "
      " } ";
  TestNoMatch(decl_proto, &decl);
  EXPECT_TRUE(
      llvm::StringRef(decl.not_found()).contains("Too few CLIF arguments"));
}

TEST_F(ClifMatcherTest, TestFunctionTemplateTooMuchguments) {
  protos::Decl decl;
  std::string decl_proto =
      "decltype: FUNC func {"
      "  name { cpp_name: 'SimpleFunctionTemplate' } "
      "  params { "
      "    type { "
      "      lang_type: 'int' "
      "      cpp_type: 'int' "
      "    } "
      "  } "
      "   params { "
      "    type { "
      "      lang_type: 'float' "
      "      cpp_type: 'float' "
      "    } "
      "  } "
      "} ";
  TestNoMatch(decl_proto, &decl);
  EXPECT_TRUE(
      llvm::StringRef(decl.not_found()).contains("Too many CLIF arguments"));
}

TEST_F(ClifMatcherTest, TestClassTemplate) {
  std::string decl_proto = "decltype: CLASS class_ {"
      "name { cpp_name: 'ComposedType<int>' } "
      " } ";
  TestMatch(decl_proto);
}

TEST_F(ClifMatcherTest, TestImplicitConversion) {
  std::string decl_proto = "decltype: FUNC func {"
      "name { cpp_name: 'FunctionWithImplicitConversion' } "
      "params { type { lang_type: 'int' cpp_type: 'Source' } } "
      " } ";
  protos::Decl decl;
  TestNoMatch(decl_proto, &decl);
}

TEST_F(ClifMatcherTest, TestToPtrConversionSet) {
  std::string decl_proto = "decltype: FUNC func {"
      "name { cpp_name: 'FunctionToPtrConversion' } "
      "params { type { lang_type: 'int' cpp_type: 'grandmother' } } "
      "params { type { lang_type: 'int' cpp_type: 'grandmother' } } "
      "params { type { lang_type: 'int' cpp_type: 'grandmother' } } "
      "params { type { lang_type: 'int' cpp_type: 'grandmother*' } } "
      " } ";
  protos::Decl decl;
  TestMatch(decl_proto, &decl);
  // Zero and one-level of indirection should have these fields set,
  // but not more.
  EXPECT_TRUE(decl.func().params(0).type().cpp_toptr_conversion());
  EXPECT_TRUE(decl.func().params(0).type().cpp_touniqptr_conversion());
  EXPECT_TRUE(decl.func().params(1).type().cpp_toptr_conversion());
  EXPECT_TRUE(decl.func().params(1).type().cpp_touniqptr_conversion());
  EXPECT_TRUE(decl.func().params(2).type().cpp_toptr_conversion());
  EXPECT_TRUE(decl.func().params(2).type().cpp_touniqptr_conversion());
  EXPECT_FALSE(decl.func().params(3).type().cpp_toptr_conversion());
  EXPECT_FALSE(decl.func().params(3).type().cpp_touniqptr_conversion());
}

TEST_F(ClifMatcherTest, TestStdSmartPointers) {
  std::string decl_proto = "decltype: FUNC func {"
      "name { cpp_name: 'FuncUniqPtrToBuiltinTypeArg' } "
      "params { type { lang_type: 'int' cpp_type: 'int' } } "
      " } ";
  protos::Decl decl1;
  TestMatch(decl_proto, &decl1);
  EXPECT_EQ(decl1.func().params(0).type().cpp_type(),
            "::std::unique_ptr<long long>");

  decl_proto = "decltype: FUNC func {"
      "name { cpp_name: 'FuncUniqPtrToBuiltinTypeReturn' } "
      "returns { type { lang_type: 'int' cpp_type: 'int' } } "
      " } ";
  protos::Decl decl2;
  TestMatch(decl_proto, &decl2);
  EXPECT_EQ(decl2.func().returns(0).type().cpp_type(),
            "::std::unique_ptr<long long>");
}

TEST_F(ClifMatcherTest, TestDeprecatedFunctions) {
  std::string decl_proto = "decltype: CLASS class_ {"
      "  name { cpp_name: 'ClassWithDeprecatedMethod' }"
      "  members { "
      "    decltype: FUNC func { "
      "      name { cpp_name: 'MethodWithDeprecatedOverload' } "
      "      params { "
      "        type { "
      "          cpp_type: 'Class'"
      "        } "
      "      } "
      "    } "
      "  } "
      "  members { "
      "    decltype: FUNC func { "
      "      name { cpp_name: 'DeprecatedMethod' } "
      "      params { "
      "        type { "
      "          cpp_type: 'Class'"
      "        } "
      "      } "
      "    } "
      "  } "
      "} ";
  TestMatch(decl_proto);

  decl_proto = "decltype: FUNC func { "
      "  name { cpp_name: 'FunctionWithDeprecatedOverload' } "
      "  params { "
      "    type { "
      "      cpp_type: 'Class'"
      "    } "
      "  } "
      "} ";
  TestMatch(decl_proto);

  decl_proto = "decltype: FUNC func { "
      "  name { cpp_name: 'DeprecatedFunction' } "
      "  params { "
      "    type { "
      "      cpp_type: 'Class'"
      "    } "
      "  } "
      "} ";
  TestMatch(decl_proto);

  decl_proto = "decltype: CLASS class_ {"
      "  name { cpp_name: 'ClassWithDeprecatedMethod' }"
      "  members { "
      "    decltype: FUNC func { "
      "      name { cpp_name: 'DeprecatedMethodWithDeprecatedOverload' } "
      "      params { "
      "        type { "
      "          cpp_type: 'Class'"
      "        } "
      "      } "
      "    } "
      "  } "
      "  members { "
      "    decltype: FUNC func { "
      "      name { cpp_name: 'DeprecatedMethod' } "
      "      params { "
      "        type { "
      "          cpp_type: 'Class'"
      "        } "
      "      } "
      "    } "
      "  } "
      "} ";
  TestNoMatch(decl_proto);
}

TEST_F(ClifMatcherTest, TestCppTypeInParamAndReturnType) {
  std::string decl_proto = "decltype: CLASS class_ {"
      "  name { cpp_name: 'ClassWithQualMethodsAndParams' }"
      "  members { "
      "    decltype: FUNC func { "
      "      name { cpp_name: 'Method1' } "
      "      params { "
      "        type { "
      "          cpp_type: 'int'"
      "        } "
      "      } "
      "    } "
      "  } "
      "  members { "
      "    decltype: FUNC func { "
      "      name { cpp_name: 'Method2' } "
      "      params { "
      "        type { "
      "          cpp_type: 'Class'"
      "        } "
      "      } "
      "    } "
      "  } "
      "  members { "
      "    decltype: FUNC func { "
      "      name { cpp_name: 'Method3' } "
      "      returns { "
      "        type { "
      "          cpp_type: 'Class'"
      "        } "
      "      } "
      "    } "
      "  } "
      "  members { "
      "    decltype: FUNC func { "
      "      name { cpp_name: 'Method4' } "
      "      params { "
      "        type { "
      "          cpp_type: 'Class'"
      "        } "
      "      } "
      "    } "
      "  } "
      "  members { "
      "    decltype: FUNC func { "
      "      name { cpp_name: 'Method5' } "
      "      params { "
      "        type { "
      "          cpp_type: 'int'"
      "        } "
      "      } "
      "      returns { "
      "        type { "
      "          cpp_type: 'Class'"
      "        } "
      "      } "
      "    } "
      "  } "
      "} ";
  protos::Decl decl;
  TestMatch(decl_proto, &decl);
  EXPECT_EQ(decl.class_().members().size(), 5);

  EXPECT_EQ(decl.class_().members(0).func().params(0).cpp_exact_type(),
            "const int");
  EXPECT_FALSE(decl.class_().members(0).func().cpp_const_method());

  EXPECT_EQ(decl.class_().members(1).func().params(0).cpp_exact_type(),
            "const ::Class &");
  EXPECT_FALSE(decl.class_().members(1).func().cpp_const_method());

  EXPECT_EQ(decl.class_().members(2).func().returns(0).cpp_exact_type(),
            "::Class");
  EXPECT_FALSE(decl.class_().members(2).func().cpp_const_method());

  EXPECT_EQ(decl.class_().members(3).func().params(0).cpp_exact_type(),
            "const ::Class &");
  EXPECT_TRUE(decl.class_().members(3).func().cpp_const_method());

  EXPECT_EQ(decl.class_().members(4).func().params(0).cpp_exact_type(),
            "const int");
  EXPECT_EQ(decl.class_().members(4).func().returns(0).cpp_exact_type(),
            "::Class *");
  EXPECT_TRUE(decl.class_().members(4).func().cpp_const_method());
}

TEST_F(ClifMatcherTest, TestDefaultArguments) {
  std::string decl_proto = "decltype: CLASS class_ {"
      " name { cpp_name: 'Class' }"
      "   members {"
      "     decltype: FUNC func {"
      "       name {"
      "         cpp_name: 'MethodWithDefaultArg'"
      "       }"
      "       params {"
      "         type {"
      "           cpp_type: 'Arg'"
      "         }"
      "         default_value: 'default'"
      "       }"
      "       returns {"
      "         type {"
      "           cpp_type: 'int'"
      "         }"
      "       }"
      "     }"
      "   }"
      "   members {"
      "     decltype: FUNC func {"
      "       name {"
      "         cpp_name: 'MethodWithDefaultFlag'"
      "       }"
      "       params {"
      "         name {"
      "           cpp_name: 'f'"
      "         }"
      "         type {"
      "           cpp_type: 'int'"
      "         }"
      "         default_value: 'default'"
      "       }"
      "       returns {"
      "         type {"
      "           cpp_type: 'int'"
      "         }"
      "       }"
      "     }"
      "   }"
      "   members {"
      "     decltype: FUNC func {"
      "       name {"
      "         cpp_name: 'MethodWithDefaultBoolArgWithoutSideEffects'"
      "       }"
      "       params {"
      "         name {"
      "           cpp_name: 'b'"
      "         }"
      "         type {"
      "           cpp_type: 'bool'"
      "         }"
      "         default_value: 'default'"
      "       }"
      "       returns {"
      "         type {"
      "           cpp_type: 'bool'"
      "         }"
      "       }"
      "     }"
      "   }"
      "   members {"
      "     decltype: FUNC func {"
      "       name {"
      "         cpp_name: 'MethodWithDefaultBoolArgWithSideEffects'"
      "       }"
      "       params {"
      "         name {"
      "           cpp_name: 'b'"
      "         }"
      "         type {"
      "           cpp_type: 'bool'"
      "         }"
      "         default_value: 'default'"
      "       }"
      "       returns {"
      "         type {"
      "           cpp_type: 'bool'"
      "         }"
      "       }"
      "     }"
      "   }"
      "   members {"
      "     decltype: FUNC func {"
      "       name {"
      "         cpp_name: 'MethodWithDefaultNullptr'"
      "       }"
      "       params {"
      "         type {"
      "           cpp_type: 'Arg'"
      "         }"
      "         default_value: 'default'"
      "       }"
      "       returns {"
      "         type {"
      "           cpp_type: 'int'"
      "         }"
      "       }"
      "     }"
      "   }"
      "   members {"
      "     decltype: FUNC func {"
      "       name {"
      "         cpp_name: 'MethodWithDefaultIntArg'"
      "       }"
      "       params {"
      "         type {"
      "           cpp_type: 'IntArg'"
      "         }"
      "         default_value: 'default'"
      "       }"
      "       returns {"
      "         type {"
      "           cpp_type: 'int'"
      "         }"
      "       }"
      "     }"
      "   }"
      " }";
  protos::Decl decl;
  TestMatch(decl_proto, &decl);

  EXPECT_EQ(decl.class_().members(0).func().params(0).default_value(),
            "default");
  EXPECT_EQ(decl.class_().members(1).func().params(0).default_value(), "3");
  EXPECT_EQ(decl.class_().members(2).func().params(0).default_value(), "false");
  EXPECT_EQ(decl.class_().members(3).func().params(0).default_value(),
            "default");
  EXPECT_EQ(decl.class_().members(4).func().params(0).default_value(),
            "nullptr");
  EXPECT_EQ(decl.class_().members(5).func().params(0).default_value(),
            "default");
}

TEST_F(ClifMatcherTest, TestDropDefaultArguments) {
  // Drop the default specifier for input parameters in clif wrapping.
  std::string decl_proto =
      "decltype: CLASS class_ {"
      " name { cpp_name: 'Class' }"
      "   members {"
      "     decltype: FUNC func {"
      "       name {"
      "         cpp_name: 'MethodWithDefaultArgs'"
      "       }"
      "       params {"
      "         type {"
      "           cpp_type: 'int'"
      "         }"
      "       }"
      "       params {"
      "         type {"
      "           cpp_type: 'int'"
      "         }"
      "       }"
      "     }"
      "   }"
      " }";
  protos::Decl decl;
  TestMatch(decl_proto, &decl);
  EXPECT_TRUE(
      decl.class_().members(0).func().params(0).default_value().empty());
  EXPECT_TRUE(
      decl.class_().members(0).func().params(1).default_value().empty());

  decl_proto =
      "decltype: CLASS class_ {"
      " name { cpp_name: 'Class' }"
      "   members {"
      "     decltype: FUNC func {"
      "       name {"
      "         cpp_name: 'MethodWithDefaultArgs'"
      "       }"
      "       params {"
      "         type {"
      "           cpp_type: 'int'"
      "         }"
      "       }"
      "       params {"
      "         type {"
      "           cpp_type: 'int'"
      "         }"
      "         default_value: 'default'"
      "       }"
      "     }"
      "   }"
      " }";
  TestMatch(decl_proto, &decl);
  EXPECT_TRUE(
      decl.class_().members(0).func().params(0).default_value().empty());
  EXPECT_FALSE(
      decl.class_().members(0).func().params(1).default_value().empty());

  decl_proto =
      "decltype: CLASS class_ {"
      " name { cpp_name: 'Class' }"
      "   members {"
      "     decltype: FUNC func {"
      "       name {"
      "         cpp_name: 'MethodWithDefaultArgs'"
      "       }"
      "       params {"
      "         type {"
      "           cpp_type: 'int'"
      "         }"
      "         default_value: 'default'"
      "       }"
      "       params {"
      "         type {"
      "           cpp_type: 'int'"
      "         }"
      "       }"
      "     }"
      "   }"
      " }";
  TestNoMatch(decl_proto, &decl);
  EXPECT_TRUE(llvm::StringRef(decl.class_().members(0).not_found())
                  .contains("Clif expects all required parameters to be placed "
                            "before default arguments."));

  // In clif wrapping, drop C++'s tailing output parameter's default specifier.
  decl_proto =
      "decltype: CLASS class_ {"
      " name { cpp_name: 'Class' }"
      "   members {"
      "     decltype: FUNC func {"
      "       name {"
      "         cpp_name: 'MethodWithDefaultArgs'"
      "       }"
      "       params {"
      "         type {"
      "           cpp_type: 'int'"
      "         }"
      "         default_value: 'default'"
      "       }"
      "       returns {"
      "         type {"
      "           cpp_type: 'int'"
      "         }"
      "       }"
      "     }"
      "   }"
      " }";
  TestMatch(decl_proto, &decl);
  EXPECT_FALSE(
      decl.class_().members(0).func().params(0).default_value().empty());

  decl_proto =
      "decltype: CLASS class_ {"
      " name { cpp_name: 'Class' }"
      "   members {"
      "     decltype: FUNC func {"
      "       name {"
      "         cpp_name: 'MethodWithDefaultArgs'"
      "       }"
      "       params {"
      "         type {"
      "           cpp_type: 'int'"
      "         }"
      "       }"
      "       returns {"
      "         type {"
      "           cpp_type: 'int'"
      "         }"
      "       }"
      "     }"
      "   }"
      " }";
  TestMatch(decl_proto, &decl);
  EXPECT_TRUE(
      decl.class_().members(0).func().params(0).default_value().empty());

  // In clif wrapping, drop C++'s tailing parameter, which contains default
  // specifier.
  decl_proto =
      "decltype: CLASS class_ {"
      " name { cpp_name: 'Class' }"
      "   members {"
      "     decltype: FUNC func {"
      "       name {"
      "         cpp_name: 'MethodWithDefaultArgs'"
      "       }"
      "       params {"
      "         type {"
      "           cpp_type: 'int'"
      "         }"
      "         default_value: 'default'"
      "       }"
      "     }"
      "   }"
      " }";
  TestMatch(decl_proto, &decl);
  EXPECT_FALSE(
      decl.class_().members(0).func().params(0).default_value().empty());

  decl_proto =
      "decltype: CLASS class_ {"
      " name { cpp_name: 'Class' }"
      "   members {"
      "     decltype: FUNC func {"
      "       name {"
      "         cpp_name: 'MethodWithDefaultArgs'"
      "       }"
      "       params {"
      "         type {"
      "           cpp_type: 'int'"
      "         }"
      "       }"
      "     }"
      "   }"
      " }";
  TestMatch(decl_proto, &decl);
  EXPECT_TRUE(
      decl.class_().members(0).func().params(0).default_value().empty());

  decl_proto =
      "decltype: CLASS class_ {"
      " name { cpp_name: 'Class' }"
      "   members {"
      "     decltype: FUNC func {"
      "       name {"
      "         cpp_name: 'MethodWithDefaultArgs'"
      "       }"
      "     }"
      "   }"
      " }";
  TestMatch(decl_proto);

  // Can't have out param after skipped input param (no place to supply the
  // default value).
  decl_proto =
      "decltype: CLASS class_ {"
      " name { cpp_name: 'Class' }"
      "   members {"
      "     decltype: FUNC func {"
      "       name {"
      "         cpp_name: 'MethodWithDefaultArgs'"
      "       }"
      "       returns {"
      "         type {"
      "           cpp_type: 'int'"
      "         }"
      "       }"
      "     }"
      "   }"
      " }";
  TestNoMatch(decl_proto, &decl);
  EXPECT_TRUE(llvm::StringRef(decl.class_().members(0).not_found())
                  .contains(" output parameter must be either a pointer or "));
}

TEST_F(ClifMatcherTest, TestUnexpectedDefaultSpecifier) {
  std::string decl_proto =
      "decltype: CLASS class_ {"
      " name { cpp_name: 'Class' }"
      "   members {"
      "     decltype: FUNC func {"
      "       name {"
      "         cpp_name: 'MethodWithoutDefaultArg'"
      "       }"
      "       params {"
      "         type {"
      "           cpp_type: 'int'"
      "         }"
      "         default_value: 'default'"
      "       }"
      "       returns {"
      "         type {"
      "           cpp_type: 'bool'"
      "         }"
      "       }"
      "     }"
      "   }"
      " }";
  protos::Decl decl;
  TestNoMatch(decl_proto, &decl);
  EXPECT_TRUE(llvm::StringRef(decl.class_().members(0).not_found())
                  .contains("Clif contains unexpected default specifiers."));
}

TEST_F(ClifMatcherTest, TestOpaqueClassCapsule) {
  std::string decl_proto = "decltype: TYPE fdecl {"
      "  name {"
      "    cpp_name: 'MyOpaqueClass'"
      "  }"
      "}";
  protos::Decl decl;
  TestMatch(decl_proto, &decl);
}

TEST_F(ClifMatcherTest, TestTypedefPtrOutputArg) {
  std::string decl_proto = "decltype: FUNC func {"
      "  name {"
      "    cpp_name: 'FuncWithPtrOutputArg'"
      "  }"
      "  returns {"
      "    type {"
      "      cpp_type: 'OpaqueClass'"
      "    }"
      "  }"
      "}";
  protos::Decl decl1;
  TestMatch(decl_proto, &decl1);
  EXPECT_EQ(decl1.func().returns(0).type().cpp_type(), "::OpaqueClass *");

  decl_proto = "decltype: FUNC func {"
      "  name {"
      "    cpp_name: 'FuncWithPtrOutputArg'"
      "  }"
      "  returns {"
      "    type {"
      "      cpp_type: 'OpaqueClass *'"
      "    }"
      "  }"
      "}";
  protos::Decl decl2;
  TestMatch(decl_proto, &decl2);
  EXPECT_EQ(decl2.func().returns(0).type().cpp_type(), "::OpaqueClass *");
}

TEST_F(ClifMatcherTest, TestTypedefWithinTemplate) {
    std::string decl_proto =
      "decltype: CLASS class_ {"
      " name { cpp_name: 'ObjectTypeHolder<Vector<float>>' }"
      "   members {"
      "     decltype: FUNC func {"
      "     name { cpp_name: 'FailTerribly'}"
      "       params {"
      "         type {"
      "           cpp_type: 'ObjectTypeHolder<Vector<float>>'"
      "         }"
      "       }"
      "     }"
      "   }"
      " } namespace_: 'example'";
  protos::Decl decl;
  TestMatch(decl_proto, &decl);
  EXPECT_EQ(decl.class_().members(0).func().params(0).type().cpp_type(),
            "::example::ObjectTypeHolder< ::example::Vector<float>> *");
}

TEST_F(ClifMatcherTest, TestFuncWithBaseClassParam) {
  // TODO: check why this test case is working. C++ side has the input
  // parameter of Class(not Class*).
  std::string decl_proto = "decltype: FUNC func {"
      "  name {"
      "    cpp_name: 'BaseFunctionValue'"
      "  }"
      "  params {"
      "    type {"
      "      cpp_type: 'DerivedClass'"
      "    }"
      "  }"
      "}";
  protos::Decl decl;
  // decl is refreshed with new contents every time TestMatch() is executed.
  TestMatch(decl_proto, &decl);
  EXPECT_EQ(decl.func().params(0).type().cpp_type(), "::DerivedClass");

  decl_proto = "decltype: FUNC func {"
      "  name {"
      "    cpp_name: 'BaseFunctionPtr'"
      "  }"
      "  params {"
      "    type {"
      "      cpp_type: 'DerivedClass'"
      "    }"
      "  }"
      "}";
  TestMatch(decl_proto, &decl);
  EXPECT_EQ(decl.func().params(0).type().cpp_type(), "::DerivedClass *");

  decl_proto = "decltype: FUNC func {"
      "  name {"
      "    cpp_name: 'BaseFunctionRef'"
      "  }"
      "  params {"
      "    type {"
      "      cpp_type: 'DerivedClass'"
      "    }"
      "  }"
      "}";
  TestMatch(decl_proto, &decl);
  EXPECT_EQ(decl.func().params(0).type().cpp_type(), "::DerivedClass");

  decl_proto = "decltype: FUNC func {"
      "  name {"
      "    cpp_name: 'BaseFunctionPtr'"
      "  }"
      "  params {"
      "    type {"
      "      cpp_type: 'DerivedClass2 *'"
      "    }"
      "  }"
      "}";
  TestMatch(decl_proto, &decl);
  EXPECT_EQ(decl.func().params(0).type().cpp_type(), "::DerivedClass2 *");

  decl_proto = "decltype: FUNC func {"
      "  name {"
      "    cpp_name: 'FuncWithUniqPtrToDynamicBaseArg'"
      "  }"
      "  params {"
      "    type {"
      "      cpp_type: 'DynamicDerived'"
      "    }"
      "  }"
      "}";
  TestMatch(decl_proto, &decl);
  EXPECT_EQ(decl.func().params(0).type().cpp_type(),
            "::std::unique_ptr<::DynamicDerived>");

  decl_proto =
      "decltype: FUNC func {"
      "  name {"
      "    cpp_name: 'FuncWithBaseReturnValue'"
      "  }"
      "  returns {"
      "    type {"
      "      cpp_type: 'DynamicDerived'"
      "    }"
      "  }"
      "}";
  TestMatch(decl_proto, &decl);
  EXPECT_EQ(decl.func().returns(0).type().cpp_type(), "::DynamicBase *");

  decl_proto =
      "decltype: FUNC func {"
      "  name {"
      "    cpp_name: 'FuncWithBaseParam'"
      "  }"
      "  params {"
      "    type {"
      "      cpp_type: 'DynamicDerived'"
      "    }"
      "  }"
      "}";
  TestMatch(decl_proto, &decl);
  EXPECT_EQ(decl.func().params(0).type().cpp_type(), "::DynamicDerived *");

  decl_proto =
      "decltype: FUNC func {"
      "  name {"
      "    cpp_name: 'FuncWithBaseParam'"
      "  }"
      "  returns {"
      "    type {"
      "      cpp_type: 'DynamicDerived'"
      "    }"
      "  }"
      "}";
  TestMatch(decl_proto, &decl);
  EXPECT_EQ(decl.func().returns(0).type().cpp_type(), "::DynamicBase");
}

TEST_F(ClifMatcherTest, TestClassWithInheritedConstructor) {
  std::string decl_proto = "decltype: CLASS class_ {"
      " name { cpp_name: 'ClassWithInheritedConstructor' }"
      "   members {"
      "     decltype: FUNC func {"
      "       name {"
      "         cpp_name: 'Method'"
      "       }"
      "     }"
      "   }"
      "   members {"
      "     decltype: FUNC func {"
      "       constructor: true"
      "       params {"
      "         type {"
      "           cpp_type: 'int'"
      "         }"
      "       }"
      "     }"
      "   }"
      " }";
  TestMatch(decl_proto);
}

TEST_F(ClifMatcherTest, TestClassWithInheritedTemplateConstructor) {
  std::string decl_proto =
      "decltype: CLASS class_ {"
      " name { cpp_name: 'ClassUsingInheritedTemplateFunctions' }"
      "   members {"
      "     decltype: FUNC func {"
      "       constructor: true"
      "       params {"
      "         type {"
      "           cpp_type: 'int'"
      "         }"
      "       }"
      "     }"
      "   }"
      " }";
  TestMatch(decl_proto);
}

TEST_F(ClifMatcherTest, TestClassWithInheritedTemplateMethod) {
  std::string decl_proto =
      "decltype: CLASS class_ {"
      " name { cpp_name: 'ClassUsingInheritedTemplateFunctions' }"
      "   members {"
      "     decltype: FUNC func {"
      "       name {"
      "         cpp_name: 'Method'"
      "       }"
      "       params {"
      "         type {"
      "           cpp_type: 'int'"
      "         }"
      "       }"
      "     }"
      "   }"
      " }";
  TestMatch(decl_proto);

  decl_proto =
      "decltype: CLASS class_ {"
      " name { cpp_name: 'ClassUsingInheritedTemplateFunctions' }"
      "   members {"
      "     decltype: FUNC func {"
      "       name {"
      "         cpp_name: 'Method'"
      "       }"
      "     }"
      "   }"
      " }";
  protos::Decl decl;
  TestNoMatch(decl_proto, &decl);
  EXPECT_TRUE(llvm::StringRef(decl.class_().members(0).not_found())
                  .contains("Function template can't be specialized"));
  EXPECT_TRUE(llvm::StringRef(decl.class_().members(0).not_found())
                  .contains("ClassWithTemplateFunctions::Method"));

  decl_proto =
      "decltype: CLASS class_ {"
      " name { cpp_name: 'ClassUsingInheritedTemplateFunctions' }"
      "   members {"
      "     decltype: FUNC func {"
      "       name {"
      "         cpp_name: 'NestClass'"
      "       }"
      "     }"
      "   }"
      " }";
  TestNoMatch(decl_proto, &decl);
  EXPECT_TRUE(
      llvm::StringRef(decl.class_().members(0).not_found())
          .contains("which is a C++ class"));
}

// Test for matching non explicit constructors. If the C++ constructor is not
// marked as explicit, matcher might do implicit type conversion in the backend,
// count copy/move constructors as valid candidates and report a multi match
// error.
TEST_F(ClifMatcherTest, TestNonExplicitConstructor) {
  std::string decl_proto =
      "decltype: CLASS class_ {"
      " name { cpp_name: 'ClassWithNonExplicitConstructor' }"
      "   members {"
      "     decltype: FUNC func {"
      "       constructor: true"
      "       params {"
      "         type {"
      "           cpp_type: 'int'"
      "         }"
      "       }"
      "     }"
      "   }"
      " }";
  protos::Decl decl;
  TestNoMatch(decl_proto, &decl);
  EXPECT_TRUE(llvm::StringRef(decl.not_found())
                  .contains("Is the keyword \"explicit\" missed in C++'s "
                            "definition of constructors?"));
}

TEST_F(ClifMatcherTest, TestTemplateAliasWithDifferentArgs) {
  protos::Decl decl;
  // The template argument type "void" is ignored by clif matcher as the type is
  // only used in the C++ template alias and does not affect the underlying
  // type.
  TestMatch(
      "decltype: FUNC func { "
      "  name { cpp_name: 'func_template_alias_set_input' } "
      "  params { "
      "    type { "
      "      cpp_type: 'clif_set' "
      "      params { "
      "        cpp_type: 'void' "
      "      } "
      "    } "
      "  } "
      "} ",
      &decl);
  EXPECT_EQ(decl.func().params(0).type().cpp_type(), "::set<>");

  TestMatch(
      "decltype: FUNC func { "
      "  name { cpp_name: 'func_template_alias_set_output' } "
      "  returns {"
      "    type { "
      "      cpp_type: 'clif_set' "
      "      params { "
      "        cpp_type: 'void' "
      "      } "
      "    } "
      "  }"
      "} ",
      &decl);
  EXPECT_EQ(decl.func().returns(0).type().cpp_type(), "::set<>");

  TestMatch(
      "decltype: FUNC func { "
      "  name { cpp_name: 'func_template_alias_set_return' } "
      "  returns {"
      "    type { "
      "      cpp_type: 'clif_set' "
      "      params { "
      "        cpp_type: 'void' "
      "      } "
      "    } "
      "  }"
      "} ",
      &decl);
  EXPECT_EQ(decl.func().returns(0).type().cpp_type(), "::set<>");

  TestMatch(
      "decltype: FUNC func { "
      "  name { cpp_name: 'func_template_alias_map' } "
      "  params { "
      "    type { "
      "      cpp_type: 'clif_map' "
      "      params { "
      "        cpp_type: 'void' "
      "      } "
      "      params { "
      "        cpp_type: 'int' "
      "      } "
      "    } "
      "  } "
      "} ",
      &decl);
  EXPECT_EQ(decl.func().params(0).type().cpp_type(), "::map<int>");
}

TEST_F(ClifMatcherTest, TestTemplateWithSmartPtr) {
  protos::Decl decl;
  TestMatch(
      "decltype: FUNC func { "
      "  name { cpp_name: 'func_template_unique_ptr' } "
      "  params { "
      "    type { "
      "      cpp_type: 'set' "
      "      params { "
      "        cpp_type: 'int' "
      "      } "
      "    } "
      "  } "
      "} ",
      &decl);
  EXPECT_EQ(decl.func().params(0).type().cpp_type(),
            "::set< ::std::unique_ptr<int>>");
}

TEST_F(ClifMatcherTest, TestMultilevelContainer) {
  protos::Decl decl;
  TestMatch("decltype: FUNC func { "
            "  name { cpp_name: 'Clone' } "
            "  params { "
            "    type { "
            "      lang_type: 'list<list<int>>' "
            "      cpp_type: 'ComposedType' "
            "      params { "
            "        lang_type: 'list<int>' "
            "        cpp_type: 'ComposedType' "
            "        params { lang_type: 'int' cpp_type: 'int' }"
            "      } "
            "    } "
            "  } "
            "  returns { "
            "    type { "
            "      lang_type: 'list<list<int>>' "
            "      cpp_type: 'ComposedType' "
            "      params { "
            "        lang_type: 'list<int>' "
            "        cpp_type: 'ComposedType' "
            "        params { lang_type: 'int' cpp_type: 'int' }"
            "      } "
            "    } "
            "  } "
            "} ", &decl);

  EXPECT_EQ(decl.func().params(0).type().cpp_type(),
            "::ComposedType< ::ComposedType<int>>");
  EXPECT_EQ(decl.func().returns(0).type().cpp_type(),
            "::ComposedType< ::ComposedType<int>>");
}

TEST_F(ClifMatcherTest, TestNestedClasses) {
  std::vector<std::string> proto_list;
  proto_list.emplace_back(
      "decltype: CLASS class_ {"
      "  name { cpp_name: 'OuterClass1' }"
      "  members {"
      "    decltype: CLASS class_ { "
      "      name { cpp_name: 'InnerClass' } "
      "      members { "
      "        decltype: VAR var { "
      "          name { cpp_name: 'a' }"
      "          type { cpp_type: 'int' } "
      "        } "
      "      } "
      "    } "
      "  } "
      "} ");
  proto_list.emplace_back(
      "decltype: CLASS class_ {"
      "  name { cpp_name: 'OuterClass2' }"
      "  members {"
      "    decltype: CLASS class_ { "
      "      name { cpp_name: 'InnerClass' } "
      "      members { "
      "        decltype: VAR var { "
      "          name { cpp_name: 'b' }"
      "          type { cpp_type: 'int' } "
      "        } "
      "      } "
      "    } "
      "  } "
      "} ");

  DeclList decl_list;
  TestMatch(proto_list, &decl_list);

  const Decl decl1 = decl_list.Get(0);
  EXPECT_EQ(decl1.class_().name().cpp_name(), "::OuterClass1");
  const ClassDecl inner_class1 = decl1.class_().members(0).class_();
  EXPECT_EQ(inner_class1.name().cpp_name(), "::OuterClass1::InnerClass");
  EXPECT_EQ(inner_class1.members(0).var().name().cpp_name(), "a");

  const Decl decl2 = decl_list.Get(1);
  EXPECT_EQ(decl2.class_().name().cpp_name(), "::OuterClass2");
  const ClassDecl inner_class2 = decl2.class_().members(0).class_();
  EXPECT_EQ(inner_class2.name().cpp_name(), "::OuterClass2::InnerClass");
  EXPECT_EQ(inner_class2.members(0).var().name().cpp_name(), "b");
}

TEST_F(ClifMatcherTest, TestTemplateFuncWithOutputArg) {
  std::string decl_proto = "decltype: FUNC func {"
      "  name {"
      "    cpp_name: 'TemplateFuncWithOutputArg1'"
      "  }"
      "  returns {"
      "    type {"
      "      cpp_type: 'int'"
      "    }"
      "  }"
      "}";
  protos::Decl decl1;
  TestMatch(decl_proto, &decl1);

  decl_proto = "decltype: FUNC func {"
      "  name {"
      "    cpp_name: 'TemplateFuncWithOutputArg2'"
      "  }"
      "  returns {"
      "    type {"
      "      cpp_type: 'float'"
      "    }"
      "  }"
      "  returns {"
      "    type {"
      "      cpp_type: 'int'"
      "    }"
      "  }"
      "}";
  protos::Decl decl2;
  TestMatch(decl_proto, &decl2);

  decl_proto = "decltype: FUNC func {"
      "  name {"
      "    cpp_name: 'TemplateFuncWithOutputArg3'"
      "  }"
      "  params {"
      "    type {"
      "      cpp_type: 'Class'"
      "    }"
      "  }"
      "  returns {"
      "    type {"
      "      cpp_type: 'int'"
      "    }"
      "  }"
      "}";
  protos::Decl decl3;
  TestMatch(decl_proto, &decl3);

  decl_proto = "decltype: FUNC func {"
      "  name {"
      "    cpp_name: 'TemplateFuncWithOutputArg4'"
      "  }"
      "  params {"
      "    type {"
      "      cpp_type: 'Class'"
      "    }"
      "  }"
      "  returns {"
      "    type {"
      "      cpp_type: 'float'"
      "    }"
      "  }"
      "  returns {"
      "    type {"
      "      cpp_type: 'int'"
      "    }"
      "  }"
      "}";
  protos::Decl decl4;
  TestMatch(decl_proto, &decl4);

  decl_proto = "decltype: FUNC func {"
      "  name {"
      "    cpp_name: 'TemplateFuncWithOutputArg5'"
      "  }"
      "  params {"
      "    type {"
      "      cpp_type: 'Class'"
      "    }"
      "  }"
      "  returns {"
      "    type {"
      "      cpp_type: 'Class'"
      "    }"
      "  }"
      "  returns {"
      "    type {"
      "      cpp_type: 'int'"
      "    }"
      "  }"
      "}";
  protos::Decl decl5;
  TestMatch(decl_proto, &decl5);
}

TEST_F(ClifMatcherTest, VariadicTemplateClass) {
  std::string decl_proto = "decltype: FUNC func {"
      "  name {"
      "    cpp_name: 'FuncWithVariadicTemplateClassInput'"
      "  }"
      "  params {"
      "    type {"
      "      cpp_type: 'VariadicTemplateClass'"
      "      params {"
      "        cpp_type: 'int'"
      "      }"
      "      params {"
      "        cpp_type: 'int'"
      "      }"
      "      params {"
      "        cpp_type: 'int'"
      "      }"
      "    }"
      "  }"
      "}";
  protos::Decl decl1;
  TestMatch(decl_proto, &decl1);

  decl_proto = "decltype: FUNC func {"
      "  name {"
      "    cpp_name: 'FuncWithVariadicTemplateClassReturn'"
      "  }"
      "  returns {"
      "    type {"
      "      cpp_type: 'VariadicTemplateClass'"
      "      params {"
      "        cpp_type: 'int'"
      "      }"
      "      params {"
      "        cpp_type: 'int'"
      "      }"
      "      params {"
      "        cpp_type: 'int'"
      "      }"
      "    }"
      "  }"
      "}";
  protos::Decl decl2;
  TestMatch(decl_proto, &decl2);
}

// Test for versioned smart pointers, which is defined in
// versioned_smart_ptr_test.h
TEST_F(ClifMatcherTest, TestMatchAndSetVersionedSmartPtr) {
  protos::Decl decl;
  TestMatch(
      "decltype: FUNC func { "
      "  name { cpp_name: 'f' }"
      "  returns { "
      "      type { lang_type: 'int' cpp_type: 'int' }"
      "    } }",
      &decl, "", "versioned_smart_ptr_test.h");
  EXPECT_EQ(decl.func().returns(0).type().cpp_type(), "::std::unique_ptr<int>");
}

// Test the automatic type selector for matching integer types.
TEST_F(ClifMatcherTest, TypeSelectorTestMatchAndSetVarInt) {
  protos::Decl decl;
  TestMatch(
      "decltype: CLASS class_ { "
      "  name { cpp_name: 'TypeSelectInt' } "
      "  members { "
      "    decltype: VAR var { "
      "      name { cpp_name: 'x_0' } "
      "      type { lang_type: 'int'} "
      "  } }"
      "  members { "
      "    decltype: VAR var { "
      "      name { cpp_name: 'x_1' } "
      "      type { lang_type: 'int'} "
      "  } }"
      "  members { "
      "    decltype: VAR var { "
      "      name { cpp_name: 'x_2' } "
      "      type { lang_type: 'int'} "
      "  } }"
      "  members { "
      "    decltype: VAR var { "
      "      name { cpp_name: 'x_3' } "
      "      type { lang_type: 'int'} "
      "  } }"
      "  members { "
      "    decltype: VAR var { "
      "      name { cpp_name: 'x_4' } "
      "      type { lang_type: 'int'} "
      "  } }"
      "  members { "
      "    decltype: VAR var { "
      "      name { cpp_name: 'x_5' } "
      "      type { lang_type: 'int'} "
      "  } }"
      "  members { "
      "    decltype: VAR var { "
      "      name { cpp_name: 'x_6' } "
      "      type { lang_type: 'int'} "
      "  } }"
      "  members { "
      "    decltype: VAR var { "
      "      name { cpp_name: 'x_7' } "
      "      type { lang_type: 'int'} "
      "  } }"
      "  members { "
      "    decltype: VAR var { "
      "      name { cpp_name: 'x_8' } "
      "      type { lang_type: 'int'} "
      "  } }"
      "  members { "
      "    decltype: VAR var { "
      "      name { cpp_name: 'x_9' } "
      "      type { lang_type: 'int'} "
      "  } }"
      "  members { "
      "    decltype: VAR var { "
      "      name { cpp_name: 'x_10' } "
      "      type { lang_type: 'int'} "
      "  } }"
      "}",
      &decl,
      "typemaps { "
      "  lang_type: 'int' "
      "  cpp_type: 'char' "
      "  cpp_type: 'signed char' "
      "  cpp_type: 'unsigned char' "
      "  cpp_type: 'int' "
      "  cpp_type: 'short' "
      "  cpp_type: 'long' "
      "  cpp_type: 'long long' "
      "  cpp_type: 'unsigned int' "
      "  cpp_type: 'unsigned short' "
      "  cpp_type: 'unsigned long' "
      "  cpp_type: 'unsigned long long' "
      "}");
  EXPECT_EQ(decl.class_().members(0).var().type().cpp_type(), "char");
  EXPECT_EQ(decl.class_().members(1).var().type().cpp_type(), "signed char");
  EXPECT_EQ(decl.class_().members(2).var().type().cpp_type(), "unsigned char");
  EXPECT_EQ(decl.class_().members(3).var().type().cpp_type(), "int");
  EXPECT_EQ(decl.class_().members(4).var().type().cpp_type(), "short");
  EXPECT_EQ(decl.class_().members(5).var().type().cpp_type(), "long");
  EXPECT_EQ(decl.class_().members(6).var().type().cpp_type(), "long long");
  EXPECT_EQ(decl.class_().members(7).var().type().cpp_type(), "unsigned int");
  EXPECT_EQ(decl.class_().members(8).var().type().cpp_type(),
            "unsigned short");
  EXPECT_EQ(decl.class_().members(9).var().type().cpp_type(), "unsigned long");
  EXPECT_EQ(decl.class_().members(10).var().type().cpp_type(),
            "unsigned long long");
}

// Test the automatic type selector for matching floating types.
TEST_F(ClifMatcherTest, TypeSelectorTestMatchAndSetVarFloat) {
  protos::Decl decl;
  TestMatch(
      "decltype: CLASS class_ { "
      "  name { cpp_name: 'TypeSelectFloat' } "
      "  members { "
      "    decltype: VAR var { "
      "      name { cpp_name: 'x_0' } "
      "      type { lang_type: 'float'} "
      "  } }"
      "  members { "
      "    decltype: VAR var { "
      "      name { cpp_name: 'x_1' } "
      "      type { lang_type: 'float'} "
      "  } }"
      "}",
      &decl,
      "typemaps { "
      "  lang_type: 'float' "
      "  cpp_type: 'float' "
      "  cpp_type: 'double' "
      "}");
  EXPECT_EQ(decl.class_().members(0).var().type().cpp_type(), "float");
  EXPECT_EQ(decl.class_().members(1).var().type().cpp_type(), "double");
}

// Test the automatic type selector for matching bytes types.
TEST_F(ClifMatcherTest, TypeSelectorTestMatchAndSetVarBytes) {
  protos::Decl decl;
  TestMatch(
      "decltype: CLASS class_ { "
      "  name { cpp_name: 'TypeSelectBytes' } "
      "  members { "
      "    decltype: VAR var { "
      "      name { cpp_name: 'x_0' } "
      "      type { lang_type: 'bytes'} "
      "  } }"
      "  members { "
      "    decltype: VAR var { "
      "      name { cpp_name: 'x_1' } "
      "      type { lang_type: 'bytes'} "
      "  } }"
      "  members { "
      "    decltype: VAR var { "
      "      name { cpp_name: 'x_2' } "
      "      type { lang_type: 'bytes'} "
      "  } }"
      "  members { "
      "    decltype: VAR var { "
      "      name { cpp_name: 'x_3' } "
      "      type { lang_type: 'bytes'} "
      "  } }"
      "}",
      &decl,
      "typemaps { "
      "  lang_type: 'bytes' "
      "  cpp_type: 'std::clif_string' "
      "  cpp_type: 'clif_string' "
      "  cpp_type: 'absl::Cord' "
      "  cpp_type: 'absl::string_view' "
      "}");
  EXPECT_EQ(decl.class_().members(0).var().type().cpp_type(),
            "::std::clif_string");
  EXPECT_EQ(decl.class_().members(1).var().type().cpp_type(), "::clif_string");
  EXPECT_EQ(decl.class_().members(2).var().type().cpp_type(), "::absl::Cord");
  EXPECT_EQ(decl.class_().members(3).var().type().cpp_type(),
            "::absl::string_view");
}

// Test the automatic type selector for matching functions' parameter/return
// types.
TEST_F(ClifMatcherTest, TypeSelectorTestMatchAndSetFuncTypes) {
  protos::Decl decl;
  TestMatch(
      "decltype: CLASS class_ { "
      "  name { cpp_name: 'TypeSelectFunctionTypes' } "
      "  members { "
      "    decltype: FUNC func { "
      "      name { cpp_name: 'Func' } "
      "      params { type { lang_type: 'float'} } "
      "      returns { type { lang_type: 'int' } } "
      "      returns { type { lang_type: 'bytes'} } "
      "  } }"
      "}",
      &decl,
      "typemaps { "
      "  lang_type: 'int' "
      "  cpp_type: 'char' "
      "  cpp_type: 'signed char' "
      "  cpp_type: 'unsigned char' "
      "  cpp_type: 'int' "
      "  cpp_type: 'short' "
      "  cpp_type: 'long' "
      "  cpp_type: 'long long' "
      "  cpp_type: 'unsigned int' "
      "  cpp_type: 'unsigned short' "
      "  cpp_type: 'unsigned long' "
      "  cpp_type: 'unsigned long long' "
      "}"
      "typemaps { "
      "  lang_type: 'float' "
      "  cpp_type: 'float' "
      "  cpp_type: 'double' "
      "}"
      "typemaps { "
      "  lang_type: 'bytes' "
      "  cpp_type: 'std::clif_string' "
      "  cpp_type: 'clif_string' "
      "  cpp_type: 'absl::Cord' "
      "  cpp_type: 'absl::string_view' "
      "}");
  EXPECT_EQ(decl.class_().members(0).func().params(0).type().cpp_type(),
            "float");
  EXPECT_EQ(decl.class_().members(0).func().returns(0).type().cpp_type(),
            "int");
  EXPECT_EQ(decl.class_().members(0).func().returns(1).type().cpp_type(),
            "::absl::Cord");
}

// Test the automatic type selector for matching pointer types.
TEST_F(ClifMatcherTest, TypeSelectorTestMatchAndSetTypePointers) {
  protos::Decl decl;
  TestMatch(
      "decltype: CLASS class_ { "
      "  name { cpp_name: 'TypeSelectTypePointers' } "
      "  members { "
      "    decltype: VAR var { "
      "      name { cpp_name: 'x_0' } "
      "      type { lang_type: 'float'} "
      "  } }"
      "  members { "
      "    decltype: FUNC func { "
      "      name { cpp_name: 'Func' } "
      "      params { type { lang_type: 'float'} } "
      "      returns { type { lang_type: 'int' } } "
      "      returns { type { lang_type: 'bytes'} } "
      "  } }"
      "}",
      &decl,
      "typemaps { "
      "  lang_type: 'int' "
      "  cpp_type: 'char' "
      "  cpp_type: 'signed char' "
      "  cpp_type: 'unsigned char' "
      "  cpp_type: 'int' "
      "  cpp_type: 'short' "
      "  cpp_type: 'long' "
      "  cpp_type: 'long long' "
      "  cpp_type: 'unsigned int' "
      "  cpp_type: 'unsigned short' "
      "  cpp_type: 'unsigned long' "
      "  cpp_type: 'unsigned long long' "
      "}"
      "typemaps { "
      "  lang_type: 'float' "
      "  cpp_type: 'float' "
      "  cpp_type: 'double' "
      "}"
      "typemaps { "
      "  lang_type: 'bytes' "
      "  cpp_type: 'std::clif_string' "
      "  cpp_type: 'clif_string' "
      "  cpp_type: 'absl::Cord' "
      "  cpp_type: 'absl::string_view' "
      "}");
  EXPECT_EQ(decl.class_().members(0).var().type().cpp_type(), "double *");
  EXPECT_EQ(decl.class_().members(1).func().params(0).type().cpp_type(),
            "float *");
  EXPECT_EQ(decl.class_().members(1).func().returns(0).type().cpp_type(),
            "int *");
  EXPECT_EQ(decl.class_().members(1).func().returns(1).type().cpp_type(),
            "::absl::Cord");
}

// Test the automatic type selector for matching const types.
TEST_F(ClifMatcherTest, TypeSelectorTestMatchAndSetConstTypes) {
  protos::Decl decl;
  std::string code;
  TestMatch(
      "decltype: CLASS class_ { "
      "  name { cpp_name: 'TypeSelectConstTypes' } "
      "  members { "
      "    decltype: VAR var { "
      "      name { cpp_name: 'x_0' } "
      "      type { lang_type: 'float'} "
      "  } }"
      "  members { "
      "    decltype: VAR var { "
      "      name { cpp_name: 'x_1' } "
      "      type { lang_type: 'float'} "
      "  } }"
      "  members { "
      "    decltype: CONST const { "
      "      name { cpp_name: 'kStringConst' } "
      "      type { lang_type: 'bytes' }"
      "  } }"
      "  members { "
      "    decltype: FUNC func { "
      "      name { cpp_name: 'FuncConstRefReturn' } "
      "      params { type { lang_type: 'float'} } "
      "      params { type { lang_type: 'float'} } "
      "      params { type { lang_type: 'bytes'} } "
      "      returns { type { lang_type: 'int' } } "
      "  } }"
      "  members { "
      "    decltype: FUNC func { "
      "      name { cpp_name: 'FuncConstPtrReturn' } "
      "      returns { type { lang_type: 'int' } } "
      "  } }"
      "}",
      &decl,
      "typemaps { "
      "  lang_type: 'int' "
      "  cpp_type: 'char' "
      "  cpp_type: 'signed char' "
      "  cpp_type: 'unsigned char' "
      "  cpp_type: 'int' "
      "  cpp_type: 'short' "
      "  cpp_type: 'long' "
      "  cpp_type: 'long long' "
      "  cpp_type: 'unsigned int' "
      "  cpp_type: 'unsigned short' "
      "  cpp_type: 'unsigned long' "
      "  cpp_type: 'unsigned long long' "
      "}"
      "typemaps { "
      "  lang_type: 'float' "
      "  cpp_type: 'float' "
      "  cpp_type: 'double' "
      "}"
      "typemaps { "
      "  lang_type: 'bytes' "
      "  cpp_type: 'std::clif_string' "
      "  cpp_type: 'clif_string' "
      "  cpp_type: 'absl::Cord' "
      "  cpp_type: 'absl::string_view' "
      "}",
      "test.h", &code);
  EXPECT_EQ(decl.class_().members(0).var().type().cpp_type(), "float");
  EXPECT_EQ(decl.class_().members(1).var().type().cpp_type(), "const double *");
  EXPECT_EQ(decl.class_().members(2).const_().type().cpp_type(),
            "::clif::char_ptr");
  EXPECT_EQ(decl.class_().members(3).func().params(0).type().cpp_type(),
            "float");
  EXPECT_EQ(decl.class_().members(3).func().params(1).type().cpp_type(),
            "float");
  EXPECT_EQ(decl.class_().members(3).func().params(2).type().cpp_type(),
            "::absl::Cord *");
  EXPECT_EQ(decl.class_().members(3).func().returns(0).type().cpp_type(),
            "int");
  EXPECT_EQ(decl.class_().members(4).func().returns(0).type().cpp_type(),
            "const int *");
  // Checks the code generated by the code builder.
  // Removes the first line(#include "...") of the built code.
  code = code.substr(code.find('\n') + 1);
  // All of the possible cpp type candidate should only be typedefed once.
  // Uses C++ raw string literals to represent the built code directely.
  std::string expected_code =
      R"(namespace clif {
} // clif
namespace clif {
typedef
TypeSelectConstTypes
clif_type_0;
template<class clif_unused_template_arg_0> class clif_class_0: public clif_type_0 { public:
typedef
float
clif_type_1;
typedef
double
clif_type_2;
typedef
std::clif_string
clif_type_3;
typedef
clif_string
clif_type_4;
typedef
absl::Cord
clif_type_5;
typedef
absl::string_view
clif_type_6;
typedef
char
clif_type_7;
typedef
signed char
clif_type_8;
typedef
unsigned char
clif_type_9;
typedef
int
clif_type_10;
typedef
short
clif_type_11;
typedef
long
clif_type_12;
typedef
long long
clif_type_13;
typedef
unsigned int
clif_type_14;
typedef
unsigned short
clif_type_15;
typedef
unsigned long
clif_type_16;
typedef
unsigned long long
clif_type_17;

 };
} // clif
)";
  EXPECT_EQ(code, expected_code);
}

// Test the automatic type selector for matching global const variables.
TEST_F(ClifMatcherTest, TypeSelectorTestMatchAndSetConstGlobalVar) {
  protos::Decl decl;
  TestMatch(
      "decltype: CONST const { "
      "  name { cpp_name: 'sample' } "
      "  type { lang_type: 'int' } "
      "}",
      &decl,
      "typemaps { "
      "  lang_type: 'int' "
      "  cpp_type: 'char' "
      "  cpp_type: 'signed char' "
      "  cpp_type: 'unsigned char' "
      "  cpp_type: 'int' "
      "  cpp_type: 'short' "
      "  cpp_type: 'long' "
      "  cpp_type: 'long long' "
      "  cpp_type: 'unsigned int' "
      "  cpp_type: 'unsigned short' "
      "  cpp_type: 'unsigned long' "
      "  cpp_type: 'unsigned long long' "
      "}");
  EXPECT_EQ(decl.const_().type().cpp_type(), "int");
}

TEST_F(ClifMatcherTest, TestIntegralTemplateParam) {
  protos::Decl decl;
  TestMatch("decltype: FUNC func { "
            "  name { cpp_name: 'FuncReturnComposedIntegralTemplate' } "
            "  returns { "
            "    type { "
            "      lang_type: 'list<ClassWithIntegralTemplateParam3>' "
            "      cpp_type: 'ComposedType' "
            "      params { "
            "        lang_type: 'ClassWithIntegralTemplateParam3' "
            "        cpp_type: 'ClassWithIntegralTemplateParam3' "
            "      } "
            "    } "
            "  } "
            "} ", &decl);
}

TEST_F(ClifMatcherTest, TestIntegralTemplateParamInFunction) {
  protos::Decl decl;
  TestMatch("decltype: FUNC func { "
            "  name { cpp_name: 'FuncWithIntegralTemplateType' } "
            "  params { "
            "    type { "
            "      lang_type: 'ClassWithIntegralTemplateParam3' "
            "      cpp_type: 'ClassWithIntegralTemplateParam<3>' "
            "    } "
            "  } "
            "} ", &decl);
}

TEST_F(ClifMatcherTest, TestIntegralTemplateParamInFunctionWithRef) {
  protos::Decl decl;
  TestMatch("decltype: FUNC func { "
            "  name { cpp_name: 'FuncWithIntegralTemplateTypeRef' } "
            "  params { "
            "    type { "
            "      lang_type: 'ClassWithIntegralTemplateParam3' "
            "      cpp_type: 'ClassWithIntegralTemplateParam<3>' "
            "    } "
            "  } "
            "} ", &decl);
}

std::string MakeTestStatusOrIntReturnTypeMaps() {
  return std::string(
      "typemaps { "
      "  lang_type: 'int' "
      "  cpp_type: 'int' "
      "}"
      "typemaps { "
      "  lang_type: 'StatusOr' "
      "  cpp_type: '::absl::StatusOr' "
      "} "
  );
}

TEST_F(ClifMatcherTest, TestStatusOrIntReturnClifStatusOrInt) {
  // In *.clif: def StatusOrIntReturn() -> StatusOr<int>
  protos::Decl decl;
  TestMatch(
      "decltype: FUNC func { "
      "  name { cpp_name: 'StatusOrIntReturn' } "
      "  returns { "
      "    type { "
      "      lang_type: 'StatusOr<int>' "
      "      cpp_type: 'absl::StatusOr' "
      "      params { "
      "        lang_type: 'int' "
      "        cpp_type: 'int' "
      "      } "
      "    } "
      "  } "
      "} ",
      &decl,
      MakeTestStatusOrIntReturnTypeMaps(),
      "test.h");
}

TEST_F(ClifMatcherTest, TestStatusOrIntReturnClifInt) {
  // In *.clif: def StatusOrIntReturn() -> int
  protos::Decl decl;
  TestNoMatch(
      "decltype: FUNC func { "
      "  name { cpp_name: 'StatusOrIntReturn' } "
      "  returns { "
      "    type { "
      "      lang_type: 'int' "
      "      cpp_type: 'int' "
      "    } "
      "  } "
      "} ",
      &decl,
      MakeTestStatusOrIntReturnTypeMaps(),
      "test.h");
}

TEST_F(ClifMatcherTest, TestClifAuxFuncInTestClifAuxH) {
  std::string test_proto;
  StrAppend(&test_proto,
            "cpp_file: '", test_src_dir_, "/test_clif_aux.h' ",
            "decltype: FUNC func { name { cpp_name: 'FuncInTestClifAuxH' } }");
  TestMatch(test_proto, "", "test_clif_aux.h");
}

TEST_F(ClifMatcherTest, TestClifAuxFuncInTestH) {
  std::string test_proto;
  StrAppend(&test_proto,
            "cpp_file: '", test_src_dir_, "/test_clif_aux.h' ",
            "decltype: FUNC func { name { cpp_name: 'FuncReturnsVoid' } }");
  TestMatch(test_proto, "", "test.h");
}

TEST_F(ClifMatcherTest, TestClifAuxTestSubdirFuncInTestH) {
  std::string test_proto;
  StrAppend(&test_proto,
            "cpp_file: '", test_src_dir_, "/test_subdir/test_clif_aux.h' ",
            "decltype: FUNC func { name { cpp_name: 'FuncReturnsVoid' } }");
  TestMatch(test_proto, "", "test.h");
}

TEST_F(ClifMatcherTest, TestNoClifAuxAnotherFile) {
  std::string test_proto;
  StrAppend(&test_proto,
            "cpp_file: '", test_src_dir_, "/test.h' ",
            "decltype: FUNC func { name { cpp_name: 'FuncInAnotherFile' } }");
  protos::Decl decl;
  TestNoMatch(test_proto, &decl, "", "another_file.h");
  llvm::StringRef msg = { decl.not_found() };
  EXPECT_TRUE(msg.contains("Clif expects it in the file "));
  EXPECT_TRUE(msg.contains("/test.h but found it at "));
  EXPECT_TRUE(msg.contains("/another_file.h:"));
}

TEST_F(ClifMatcherTest, TestClifAuxAnotherFile) {
  std::string test_proto;
  StrAppend(&test_proto,
            "cpp_file: '", test_src_dir_, "/test_subdir/test_clif_aux.h' ",
            "decltype: FUNC func { name { cpp_name: 'FuncInAnotherFile' } }");
  protos::Decl decl;
  TestNoMatch(test_proto, &decl, "", "another_file.h");
  llvm::StringRef msg = { decl.not_found() };
  EXPECT_TRUE(msg.contains("Clif expects it in one of the files {"));
  EXPECT_TRUE(msg.contains("/test_subdir/test_clif_aux.h, "));
  EXPECT_TRUE(msg.contains("/test_subdir/test.h, "));
  EXPECT_TRUE(msg.contains("/test.h} but found it at "));
  EXPECT_TRUE(msg.contains("/another_file.h:"));
}

TEST_F(ClifMatcherTest, TestPureVirtualFunction) {
  protos::Decl decl;
  std::string decl_proto =
      "decltype: CLASS class_ {"
      " name { cpp_name: 'ClassPureVirtual' }"
      "   members {"
      "     decltype: FUNC func {"
      "       name {"
      "         cpp_name: 'SomeFunction'"
      "       }"
      "     }"
      "   }"
      "   members {"
      "     decltype: FUNC func {"
      "       name {"
      "         cpp_name: 'NotPureVirtual'"
      "       }"
      "     }"
      "   }"
      " }";
  TestMatch(decl_proto, &decl);
  EXPECT_TRUE(decl.class_().members(0).func().is_pure_virtual());
  EXPECT_FALSE(decl.class_().members(1).func().is_pure_virtual());
  decl_proto =
      "decltype: CLASS class_ {"
      " name { cpp_name: 'ClassOverridesPureVirtual' }"
      "   members {"
      "     decltype: FUNC func {"
      "       name {"
      "         cpp_name: 'SomeFunction'"
      "       }"
      "     }"
      "   }"
      " }";
  TestMatch(decl_proto, &decl);
  EXPECT_FALSE(decl.class_().members(0).func().is_pure_virtual());
  decl_proto =
      "decltype: FUNC func { "
      "  name { cpp_name: 'SomeFunctionNotPureVirtual' } "
      "} ",
  TestMatch(decl_proto, &decl);
  EXPECT_FALSE(decl.func().is_pure_virtual());
}

TEST_F(ClifMatcherTest, TestFunctionMangleName) {
  protos::Decl decl;
  std::string decl_proto =
      "decltype: CLASS class_ { "
      " name { cpp_name: 'DerivedClass' } "
      "   members {"
      "     decltype: FUNC func {"
      "       constructor: true "
      "       name {"
      "         cpp_name: 'DerivedClass'"
      "       }"
      "     }"
      "  }"
      "  members {"
      "    decltype: FUNC func { "
      "      name {"
      "        cpp_name: 'MemberB'"
      "      }"
      "    params { type { lang_type: 'int' cpp_type: 'int' } } "
      "    returns { type { lang_type: 'int' cpp_type: 'int' } } }"
      "} }";
  TestMatch(decl_proto, &decl);
  auto ctor = decl.class_().members(0).func();
  EXPECT_TRUE(llvm::StringRef(ctor.mangled_name()).contains("DerivedClass"));
  auto member_func = decl.class_().members(1).func();
  EXPECT_TRUE(llvm::StringRef(member_func.mangled_name()).contains("MemberB"));
  EXPECT_TRUE(
      llvm::StringRef(member_func.mangled_name()).contains("DerivedClass"));
  decl_proto =
      "decltype: FUNC func { "
      "  name { cpp_name: 'SomeFunctionNotPureVirtual' } "
      "} ";
  TestMatch(decl_proto, &decl);
  auto free_func = decl.func();
  EXPECT_TRUE(
      llvm::StringRef(free_func.mangled_name()).contains(
          "SomeFunctionNotPureVirtual"));
}

TEST_F(ClifMatcherTest, TestOverloadedFunctions) {
  protos::Decl decl;
  // Free functions.
  std::string decl_proto =
      "decltype: FUNC func { "
      "  name { cpp_name: 'PolymorphicFunc' } "
      "  params { type { lang_type: 'int' cpp_type: 'int' } } "
      "} ";
  TestMatch(decl_proto, &decl);
  EXPECT_TRUE(decl.func().is_overloaded());
  decl_proto =
      "decltype: FUNC func { "
      "  name { cpp_name: 'SomeFunctionNotPureVirtual' } "
      "} ";
  TestMatch(decl_proto, &decl);
  EXPECT_FALSE(decl.func().is_overloaded());
  // Overloaded operators.
  decl_proto =
      "decltype: CLASS class_ { "
      "  name { cpp_name: 'OperatorClass' } "
      "  members { "
      "    decltype: FUNC func { "
      "      name { cpp_name: 'operator==' }  "
      "      returns { type { lang_type: 'int' cpp_type: 'bool' } } "
      "      params { type { lang_type: 'OperatorClass'"
      "               cpp_type: 'OperatorClass' } "
      "             } "
      "      } "
      "    } "
      "  } ";
  TestMatch(decl_proto, &decl);
  EXPECT_TRUE(decl.class_().members(0).func().is_overloaded());
  decl_proto =
      "decltype: FUNC func { "
      "  name { cpp_name: 'operator==' }"
      "  params { type { lang_type: 'int' cpp_type: 'grandmother' } } "
      "  params { type { lang_type: 'int' cpp_type: 'grandfather' } } "
      "  returns { type { lang_type: 'int' cpp_type: 'bool' } } }";
  TestMatch(decl_proto, &decl);
  EXPECT_FALSE(decl.func().is_overloaded());
}

TEST_F(ClifMatcherTest, TestPolymorphicClass) {
  protos::Decl decl;
  std::string decl_proto =
      "decltype: CLASS class_ {"
      " name { cpp_name: 'ClassPureVirtual' }"
      "   members {"
      "     decltype: FUNC func {"
      "       name {"
      "         cpp_name: 'SomeFunction'"
      "       }"
      "     }"
      "   }"
      "   members {"
      "     decltype: FUNC func {"
      "       name {"
      "         cpp_name: 'NotPureVirtual'"
      "       }"
      "     }"
      "   }"
      " }";
  TestMatch(decl_proto, &decl);
  EXPECT_TRUE(decl.class_().is_cpp_polymorphic());
  decl_proto =
      "decltype: CLASS class_ {"
      " name { cpp_name: 'ClassOverridesPureVirtual' }"
      "   members {"
      "     decltype: FUNC func {"
      "       name {"
      "         cpp_name: 'SomeFunction'"
      "       }"
      "     }"
      "   }"
      " }";
  TestMatch(decl_proto, &decl);
  EXPECT_TRUE(decl.class_().is_cpp_polymorphic());
  decl_proto =
      "decltype: CLASS class_ {"
      " name { cpp_name: 'ClassWithDefaultCtor' }"
      "   members {"
      "     decltype: FUNC func {"
      "       name {"
      "         cpp_name: 'MethodConstVsNonConst'"
      "       }"
      "     }"
      "   }"
      " }";
  TestMatch(decl_proto, &decl);
  EXPECT_FALSE(decl.class_().is_cpp_polymorphic());
}

}  // namespace clif
