// Copyright 2013 Google Inc. All Rights Reserved.
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
//
// Author: gee@google.com (Adam Gee)

#ifndef PAGESPEED_KERNEL_BASE_CALLBACK_H_
#define PAGESPEED_KERNEL_BASE_CALLBACK_H_



namespace net_instaweb {

// Base class for a single argument callback.  Currently we have a single
// implementation that handles single argument member functions, which are
// to be invoked at some point in the future with the parameter.
// Example Usage:
//
// class MyClass {
//  public:
//    void MyMethod(int x);
// };
//
// void Foo(MyClass* my_class) {
//   Callback1<int>* cb = NewCallback(my_class, &MyClass::MyMethod);
//   Bar(cb);
// }
//
// void Bar(Callback1<int>* cb) {
//   cb->Run(1234);
// }
//
template<class A1>
class Callback1 {
 public:
  virtual ~Callback1() {}
  virtual void Run(A1) = 0;
};

// Naming convention is:
//  (Member)?Callback_<num-pre-bound-args>_<num-runtime-args>

// TODO(gee): Fill out other useful specializations.
template<class C, class A1>
class _MemberCallback_0_1 : public Callback1<A1> {
 public:
  typedef void (C::*MemberSignature)(A1);
  typedef Callback1<A1> base;

  _MemberCallback_0_1(C* object, MemberSignature member)
      : object_(object),
        member_(member) {
  }

  void Run(A1 t) {
    (object_->*member_)(t);
    delete this;
  }

 private:
  C* object_;
  MemberSignature member_;
};

template <class T1, class T2, class A1>
typename _MemberCallback_0_1<T1, A1>::base*
NewCallback(T1* obj, void (T2::*member)(A1)) {
  return new _MemberCallback_0_1<T1, A1>(obj, member);
}

// Specified by TR1 [4.7.2] Reference modifications.
template<typename T> struct remove_reference { typedef T type; };
template<typename T> struct remove_reference<T&> { typedef T type; };

template <typename T>
struct ConstRef {
  typedef typename remove_reference<T>::type base_type;
  typedef const base_type& type;
};

template <class T, class P1, class A1>
class _MemberCallback_1_1 : public Callback1<A1> {
 public:
  typedef Callback1<A1> base;
  typedef void (T::*MemberSignature)(P1, A1);

 private:
  T* object_;
  MemberSignature member_;
  typename remove_reference<P1>::type p1_;

 public:
  _MemberCallback_1_1(T* object,
                      MemberSignature member,
                      typename ConstRef<P1>::type p1)
      : object_(object),
        member_(member),
        p1_(p1) { }

  virtual void Run(A1 a1) {
    (object_->*member_)(p1_, a1);
    delete this;
  }
};

template <class T1, class T2, class P1, class A1>
inline typename _MemberCallback_1_1<T1, P1, A1>::base*
NewCallback(T1* obj,
            void (T2::*member)(P1, A1),
            typename ConstRef<P1>::type p1) {
  return new _MemberCallback_1_1<T1, P1, A1>(obj, member, p1);
}

}  // namespace net_instaweb


#endif  // PAGESPEED_KERNEL_BASE_CALLBACK_H_
