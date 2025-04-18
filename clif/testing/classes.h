/*
 * Copyright 2020 Google LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#ifndef THIRD_PARTY_CLIF_TESTING_CLASSES_H_
#define THIRD_PARTY_CLIF_TESTING_CLASSES_H_

// This comment intentionally includes UTF-8 characters as an IO test.
//   "Use pytype 🦆✔  - make code maintainers happy!"
namespace clif_testing {
namespace classes {

class K {
 public:
  explicit K(int i): i_(i) {}
  static const int C;
  int i1() const { return i_+1; }
  int get2() const { return i_*i_; }
  int get() const { return i_; }
  void set(int i) { i_ = i; }
  static int getCplus2() { return C+2; }
 private:
  int i_;
};

const int K::C = 1;

class Derived : public K {
 public:
  int j;
  explicit Derived(int i = 0) : K(i), j(i) {}
  Derived(int i0, int j0) : K(i0), j(j0) {}
  bool has(int k) {
    int i = get();
    if (i <= j) return i <= k && k <= j;
    return false;
  }
};

struct AddInit {
  int i = 567483;
  AddInit() = default;
  AddInit(const AddInit&) = default;
  AddInit(AddInit&&) = default;
  AddInit& operator=(const AddInit&) = default;
  AddInit& operator=(AddInit&&) = default;
};

}  // namespace classes
}  // namespace clif_testing

#endif  // THIRD_PARTY_CLIF_TESTING_CLASSES_H_
